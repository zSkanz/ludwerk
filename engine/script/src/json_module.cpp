// `@std/json` in the game VM (api-design.md §7, ADR 0030).
//
// **Lute's surface, name for name** -- `serialize`, `deserialize`, `null`,
// `object`, `asObject`, `asArray` -- because `@std/*` exists so that utility
// and backend code runs unchanged on both runtimes: a module that talks to a
// game's own service encodes what it sends here and there with one `require`.
//
// **Written here and not carried over.** Lute's is Luau source that leans on
// `newproxy`, which a game's VM does not have, and marks an object by a key
// put in the table itself -- one a `for` over the object then meets. This one
// keeps which tables are objects in a table of its own, weak in its keys, so
// an object from `deserialize` is exactly its members.
//
// What differs, and is said in the declaration (`runtime/std/json`):
//   - an object's members are written in the order of their names, so the same
//     table is the same text on every run and every machine (R10);
//   - a number is written so that reading it back gives the same number, where
//     `tostring` keeps fourteen digits of it;
//   - what JSON cannot say is refused -- a number that is not finite, a key
//     that is not a string, a table that holds itself -- rather than written
//     as something a reader then chokes on.
#include "engine/script/json_module.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/script/binding.h"

namespace engine::script {
namespace {

using core::usize;

// What `json.null` is: a light userdata of this address, so it is one value in
// every VM of the process, never collected, and equal only to itself.
const char NullKey = 0;
// Where the VM keeps the table of which tables are objects.
const char ObjectsKey = 0;

// How deep a value may nest, either way. A table that holds itself would
// otherwise be written until the stack ran out.
constexpr int MostDeep = 200;

void pushNull(lua_State* L)
{
    lua_pushlightuserdata(L, const_cast<char*>(&NullKey));
}

[[nodiscard]] bool isNull(lua_State* L, int index)
{
    return lua_islightuserdata(L, index) && lua_tolightuserdata(L, index) == &NullKey;
}

// The table of objects, weak in its keys: made with the module.
void pushObjects(lua_State* L)
{
    lua_pushlightuserdata(L, const_cast<char*>(&ObjectsKey));
    lua_rawget(L, LUA_REGISTRYINDEX);
}

// Marks the table on top of the stack as an object.
void markObject(lua_State* L)
{
    pushObjects(L);
    lua_pushvalue(L, -2);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

[[nodiscard]] bool isObject(lua_State* L, int index)
{
    if (!lua_istable(L, index))
        return false;
    const int absolute = lua_absindex(L, index);
    pushObjects(L);
    lua_pushvalue(L, absolute);
    lua_rawget(L, -2);
    const bool marked = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    return marked;
}

// --- Writing ----------------------------------------------------------------

void writeString(std::string& out, std::string_view text)
{
    out.push_back('"');
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (byte) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(byte));
                out += escaped;
            }
            else {
                out.push_back(raw);
            }
        }
    }
    out.push_back('"');
}

// The shortest text that reads back as the same number.
void writeNumber(lua_State* L, std::string& out, double value)
{
    if (!std::isfinite(value))
        raise(L, ENG_TR("script.err.json_number"));
    if (value == std::floor(value) && std::fabs(value) < 9007199254740992.0) {
        char whole[32];
        std::snprintf(whole, sizeof(whole), "%.0f", value);
        // Minus zero is zero to JSON's readers.
        out += std::strcmp(whole, "-0") == 0 ? "0" : whole;
        return;
    }
    char text[40];
    for (const int digits : {15, 16, 17}) {
        std::snprintf(text, sizeof(text), "%.*g", digits, value);
        if (std::strtod(text, nullptr) == value)
            break;
    }
    out += text;
}

void newline(std::string& out, bool pretty, int depth)
{
    if (!pretty)
        return;
    out.push_back('\n');
    out.append(static_cast<usize>(depth) * 4u, ' ');
}

void writeValue(lua_State* L, int index, std::string& out, bool pretty, int depth)
{
    if (depth > MostDeep)
        raise(L, ENG_TR("script.err.json_depth"));
    const int at = lua_absindex(L, index);
    if (isNull(L, at)) {
        out += "null";
        return;
    }
    switch (lua_type(L, at)) {
    case LUA_TBOOLEAN:
        out += lua_toboolean(L, at) != 0 ? "true" : "false";
        return;
    case LUA_TNUMBER:
        writeNumber(L, out, lua_tonumber(L, at));
        return;
    case LUA_TSTRING: {
        size_t length = 0;
        const char* text = lua_tolstring(L, at, &length);
        writeString(out, std::string_view(text, length));
        return;
    }
    case LUA_TTABLE:
        break;
    default: {
        const core::I18nArg args[] = {{"type", std::string(luaL_typename(L, at))}};
        raise(L, ENG_TR("script.err.json_value"), args);
    }
    }

    luaL_checkstack(L, 4, "json");
    const int length = lua_objlen(L, at);
    // An object: said to be one, or a table with members and no first element.
    bool object = isObject(L, at);
    if (!object && length == 0) {
        lua_pushnil(L);
        if (lua_next(L, at) != 0) {
            object = true;
            lua_pop(L, 2);
        }
    }
    if (!object) {
        out.push_back('[');
        for (int element = 1; element <= length; ++element) {
            if (element != 1)
                out.push_back(',');
            newline(out, pretty, depth + 1);
            lua_rawgeti(L, at, element);
            // A hole in an array has nothing JSON can say for it but null.
            if (lua_isnil(L, -1))
                out += "null";
            else
                writeValue(L, -1, out, pretty, depth + 1);
            lua_pop(L, 1);
        }
        if (length != 0)
            newline(out, pretty, depth);
        out.push_back(']');
        return;
    }

    // Its members by name, in the order of their names: the same table is the
    // same text wherever it is written.
    std::vector<std::string> names;
    lua_pushnil(L);
    while (lua_next(L, at) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            const core::I18nArg args[] = {{"type", std::string(luaL_typename(L, -2))}};
            raise(L, ENG_TR("script.err.json_key"), args);
        }
        size_t size = 0;
        const char* name = lua_tolstring(L, -2, &size);
        names.emplace_back(name, size);
        lua_pop(L, 1);
    }
    std::sort(names.begin(), names.end());
    out.push_back('{');
    bool first = true;
    for (const std::string& name : names) {
        if (!first)
            out.push_back(',');
        first = false;
        newline(out, pretty, depth + 1);
        writeString(out, name);
        out += pretty ? ": " : ":";
        lua_pushlstring(L, name.data(), name.size());
        lua_rawget(L, at);
        writeValue(L, -1, out, pretty, depth + 1);
        lua_pop(L, 1);
    }
    if (!names.empty())
        newline(out, pretty, depth);
    out.push_back('}');
}

// --- Reading ----------------------------------------------------------------

struct Reader
{
    lua_State* L = nullptr;
    std::string_view text;
    usize at = 0;

    [[noreturn]] void fail() const
    {
        // One past, in the way a script counts a string's bytes.
        const core::I18nArg args[] = {{"position", static_cast<core::i64>(std::min(at, text.size()) + 1)}};
        raise(L, ENG_TR("script.err.json_parse"), args);
    }

    void skipSpace() noexcept
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r'))
            ++at;
    }

    [[nodiscard]] bool take(char wanted) noexcept
    {
        if (at < text.size() && text[at] == wanted) {
            ++at;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool takeWord(std::string_view word) noexcept
    {
        if (text.substr(at, word.size()) != word)
            return false;
        at += word.size();
        return true;
    }

    [[nodiscard]] unsigned hex4()
    {
        if (at + 4 > text.size())
            fail();
        unsigned value = 0;
        for (int digit = 0; digit < 4; ++digit) {
            const char c = text[at++];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<unsigned>(c - 'A' + 10);
            else
                fail();
        }
        return value;
    }

    static void appendUtf8(std::string& out, unsigned code)
    {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        }
        else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    // The string at the cursor, which is on its opening quote.
    [[nodiscard]] std::string string()
    {
        if (!take('"'))
            fail();
        std::string out;
        while (true) {
            if (at >= text.size())
                fail();
            const char c = text[at++];
            if (c == '"')
                return out;
            if (static_cast<unsigned char>(c) < 0x20)
                fail();
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (at >= text.size())
                fail();
            const char escape = text[at++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                out.push_back(escape);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                unsigned code = hex4();
                // A pair of halves is one character past the first plane.
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (!takeWord("\\u"))
                        fail();
                    const unsigned low = hex4();
                    if (low < 0xDC00 || low > 0xDFFF)
                        fail();
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                else if (code >= 0xDC00 && code <= 0xDFFF) {
                    fail();
                }
                appendUtf8(out, code);
                break;
            }
            default:
                --at;
                fail();
            }
        }
    }

    // JSON's own grammar for a number, then the system's reading of it.
    void number()
    {
        const usize from = at;
        (void)take('-');
        if (take('0')) {
        }
        else if (at < text.size() && text[at] >= '1' && text[at] <= '9') {
            while (at < text.size() && text[at] >= '0' && text[at] <= '9')
                ++at;
        }
        else {
            fail();
        }
        if (take('.')) {
            if (at >= text.size() || text[at] < '0' || text[at] > '9')
                fail();
            while (at < text.size() && text[at] >= '0' && text[at] <= '9')
                ++at;
        }
        if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
            ++at;
            if (at < text.size() && (text[at] == '+' || text[at] == '-'))
                ++at;
            if (at >= text.size() || text[at] < '0' || text[at] > '9')
                fail();
            while (at < text.size() && text[at] >= '0' && text[at] <= '9')
                ++at;
        }
        const std::string digits(text.substr(from, at - from));
        lua_pushnumber(L, std::strtod(digits.c_str(), nullptr));
    }

    // Pushes the value at the cursor.
    void value(int depth)
    {
        if (depth > MostDeep)
            fail();
        luaL_checkstack(L, 4, "json");
        skipSpace();
        if (at >= text.size())
            fail();
        const char c = text[at];
        if (c == '{') {
            ++at;
            lua_createtable(L, 0, 0);
            markObject(L);
            skipSpace();
            if (take('}'))
                return;
            while (true) {
                skipSpace();
                const std::string name = string();
                skipSpace();
                if (!take(':'))
                    fail();
                lua_pushlstring(L, name.data(), name.size());
                value(depth + 1);
                lua_rawset(L, -3);
                skipSpace();
                if (take(','))
                    continue;
                if (take('}'))
                    return;
                fail();
            }
        }
        if (c == '[') {
            ++at;
            lua_createtable(L, 0, 0);
            skipSpace();
            if (take(']'))
                return;
            for (int element = 1;; ++element) {
                value(depth + 1);
                lua_rawseti(L, -2, element);
                skipSpace();
                if (take(','))
                    continue;
                if (take(']'))
                    return;
                fail();
            }
        }
        if (c == '"') {
            const std::string read = string();
            lua_pushlstring(L, read.data(), read.size());
            return;
        }
        if (takeWord("null")) {
            pushNull(L);
            return;
        }
        if (takeWord("true")) {
            lua_pushboolean(L, 1);
            return;
        }
        if (takeWord("false")) {
            lua_pushboolean(L, 0);
            return;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            number();
            return;
        }
        fail();
    }
};

// --- The module -------------------------------------------------------------

int jsonSerialize(lua_State* L)
{
    luaL_checkany(L, 1);
    const bool pretty = lua_gettop(L) >= 2 && lua_toboolean(L, 2) != 0;
    std::string out;
    writeValue(L, 1, out, pretty, 0);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int jsonDeserialize(lua_State* L)
{
    // A string and nothing that could be read as one: a number handed in by
    // mistake is not the JSON text of that number.
    luaL_checktype(L, 1, LUA_TSTRING);
    size_t length = 0;
    const char* text = lua_tolstring(L, 1, &length);
    Reader reader{L, std::string_view(text, length), 0};
    reader.value(0);
    // One value, and nothing after it but space.
    reader.skipSpace();
    if (reader.at != reader.text.size())
        reader.fail();
    return 1;
}

int jsonObject(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_createtable(L, 0, 0);
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_rawset(L, -4);
    }
    markObject(L);
    return 1;
}

int jsonAsObject(lua_State* L)
{
    luaL_checkany(L, 1);
    if (isObject(L, 1))
        lua_pushvalue(L, 1);
    else
        lua_pushnil(L);
    return 1;
}

int jsonAsArray(lua_State* L)
{
    luaL_checkany(L, 1);
    if (lua_istable(L, 1) && !isObject(L, 1))
        lua_pushvalue(L, 1);
    else
        lua_pushnil(L);
    return 1;
}

} // namespace

int openStdJson(lua_State* L)
{
    // The table of which tables are objects: its keys held weakly, so a
    // reply read and thrown away is thrown away.
    lua_pushlightuserdata(L, const_cast<char*>(&ObjectsKey));
    lua_createtable(L, 0, 0);
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);

    lua_createtable(L, 0, 6);
    pushNull(L);
    lua_setfield(L, -2, "null");
    lua_pushcfunction(L, jsonSerialize, "serialize");
    lua_setfield(L, -2, "serialize");
    lua_pushcfunction(L, jsonDeserialize, "deserialize");
    lua_setfield(L, -2, "deserialize");
    lua_pushcfunction(L, jsonObject, "object");
    lua_setfield(L, -2, "object");
    lua_pushcfunction(L, jsonAsObject, "asObject");
    lua_setfield(L, -2, "asObject");
    lua_pushcfunction(L, jsonAsArray, "asArray");
    lua_setfield(L, -2, "asArray");
    // Frozen, as `@std/net` is: what the name means is a contract.
    lua_setreadonly(L, -1, true);
    return 1;
}

} // namespace engine::script
