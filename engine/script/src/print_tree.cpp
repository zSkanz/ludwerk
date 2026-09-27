#include "print_tree.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string_view>
#include <vector>

namespace engine::script {
namespace {

// How much of a table a print carries. A print is a glance, and a table of a
// hundred thousand rows printed every frame must not become the frame.
constexpr int kMaxDepth = 8;
constexpr std::size_t kMaxFieldsPerTable = 200;
constexpr std::size_t kMaxRows = 2000;
constexpr std::size_t kSummaryWidth = 96;
constexpr std::size_t kValueWidth = 200;
constexpr char kSeparator = '\x1F';

// One key of a table, in the order it is listed: numbers ascending, then
// strings, then anything else by its text.
struct Key
{
    int group = 0;
    double number = 0.0;
    std::string text;
    int slot = 0;
};

[[nodiscard]] bool isIdentifier(std::string_view text)
{
    if (text.empty() || std::isdigit(static_cast<unsigned char>(text.front())) != 0)
        return false;
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; });
}

// A row never holds the characters that frame it.
void appendClean(std::string& out, std::string_view text, std::size_t width)
{
    std::size_t kept = 0;
    for (const char c : text) {
        if (kept >= width) {
            out += "...";
            return;
        }
        if (c == '\n')
            out += "\\n";
        else if (c == '\t')
            out += "\\t";
        else if (c == kSeparator || c == '\r')
            out += ' ';
        else
            out += c;
        ++kept;
    }
}

[[nodiscard]] std::string toText(lua_State* L, int index)
{
    size_t length = 0;
    const char* text = luaL_tolstring(L, index, &length);
    std::string out(text, length);
    lua_pop(L, 1);
    return out;
}

// The keys of the table at `table`, sorted, copied into a new array left on the
// stack so each can be pushed again to read its value.
[[nodiscard]] std::vector<Key> sortedKeys(lua_State* L, int table)
{
    std::vector<Key> keys;
    lua_createtable(L, 0, 0);
    const int store = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, table) != 0) {
        lua_pop(L, 1);
        Key key;
        key.slot = static_cast<int>(keys.size()) + 1;
        if (lua_type(L, -1) == LUA_TNUMBER) {
            key.group = 0;
            key.number = lua_tonumber(L, -1);
        }
        else if (lua_type(L, -1) == LUA_TSTRING) {
            key.group = 1;
            size_t length = 0;
            const char* text = lua_tolstring(L, -1, &length);
            key.text.assign(text, length);
        }
        else {
            key.group = 2;
            key.text = toText(L, -1);
        }
        lua_pushvalue(L, -1);
        lua_rawseti(L, store, key.slot);
        keys.push_back(std::move(key));
    }
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        if (a.group != b.group)
            return a.group < b.group;
        if (a.group == 0)
            return a.number < b.number;
        return a.text < b.text;
    });
    return keys;
}

[[nodiscard]] std::string keyText(const Key& key)
{
    if (key.group == 0) {
        char number[32];
        (void)std::snprintf(number, sizeof(number), "[%.14g]", key.number);
        return number;
    }
    if (key.group == 1 && isIdentifier(key.text))
        return key.text;
    return "[\"" + key.text + "\"]";
}

void appendSummary(lua_State* L, int index, std::string& out, int depth);

// A value as one short piece of text.
void appendValue(lua_State* L, int index, std::string& out, int depth)
{
    switch (lua_type(L, index)) {
    case LUA_TSTRING: {
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        out += '"';
        appendClean(out, std::string_view(text, length), kValueWidth);
        out += '"';
        return;
    }
    case LUA_TTABLE:
        if (printsAsTree(L, index)) {
            if (depth > 0)
                out += "{...}";
            else
                appendSummary(L, index, out, depth + 1);
            return;
        }
        break;
    default:
        break;
    }
    appendClean(out, toText(L, index), kValueWidth);
}

void appendSummary(lua_State* L, int index, std::string& out, int depth)
{
    if (!lua_checkstack(L, 8)) {
        out += "{...}";
        return;
    }
    const int table = lua_absindex(L, index);
    const std::vector<Key> keys = sortedKeys(L, table);
    const int store = lua_gettop(L);
    out += '{';
    const std::size_t start = out.size();
    bool first = true;
    for (const Key& key : keys) {
        if (out.size() - start > kSummaryWidth) {
            out += ", ...";
            break;
        }
        if (!first)
            out += ", ";
        first = false;
        out += keyText(key);
        out += " = ";
        lua_rawgeti(L, store, key.slot);
        lua_rawget(L, table);
        appendValue(L, -1, out, depth);
        lua_pop(L, 1);
    }
    out += '}';
    lua_pop(L, 1);
}

struct TreeWriter
{
    lua_State* L = nullptr;
    std::string& out;
    std::size_t rows = 0;
    std::vector<const void*> path;

    void row(int depth, std::string_view key, std::string_view value)
    {
        out += std::to_string(depth);
        out += kSeparator;
        appendClean(out, key, kValueWidth);
        out += kSeparator;
        out += value;
        out += '\n';
        ++rows;
    }

    void table(int index, int depth)
    {
        if (!lua_checkstack(L, 8))
            return;
        const int at = lua_absindex(L, index);
        path.push_back(lua_topointer(L, at));
        const std::vector<Key> keys = sortedKeys(L, at);
        const int store = lua_gettop(L);
        std::size_t shown = 0;
        for (const Key& key : keys) {
            if (shown == kMaxFieldsPerTable || rows >= kMaxRows) {
                row(depth, "...", std::to_string(keys.size() - shown) + " more");
                break;
            }
            ++shown;
            lua_rawgeti(L, store, key.slot);
            lua_rawget(L, at);
            std::string value;
            const bool nested = lua_type(L, -1) == LUA_TTABLE && printsAsTree(L, -1);
            const bool cycle = nested && std::find(path.begin(), path.end(), lua_topointer(L, -1)) != path.end();
            if (cycle)
                value = "<the table it is inside>";
            else
                appendValue(L, -1, value, 0);
            row(depth, keyText(key), value);
            if (nested && !cycle && depth + 1 < kMaxDepth)
                table(-1, depth + 1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        path.pop_back();
    }
};

} // namespace

bool printsAsTree(lua_State* L, int index)
{
    if (lua_type(L, index) != LUA_TTABLE)
        return false;
    if (!lua_getmetatable(L, index))
        return true;
    lua_rawgetfield(L, -1, "__tostring");
    const bool described = !lua_isnil(L, -1);
    lua_pop(L, 2);
    return !described;
}

std::string printSummary(lua_State* L, int index)
{
    std::string out;
    appendSummary(L, index, out, 0);
    return out;
}

void appendPrintTree(lua_State* L, int index, int depth, std::string& out)
{
    TreeWriter writer{L, out, 0, {}};
    writer.table(index, depth);
}

} // namespace engine::script
