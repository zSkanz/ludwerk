#include "engine/script/data_service.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "engine/core/error.h"
#include "engine/core/i18n.h"
#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/core/log.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/instance_binding.h"
#include "engine/script/modules.h"
#include "engine/script/save_service.h"
#include "engine/script/save_store.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

// The largest shared value, written.
constexpr core::usize MaxSharedBytes = 256u * 1024u;
// How many times `UpdateAsync` tries again after another world wrote first.
constexpr core::u32 MostConflicts = 32;
// How often a record that changed is written while it is held.
constexpr core::f64 WriteBehindSeconds = 30.0;
// The largest record, written: a player's record is not a level.
constexpr core::u64 MaxRecordBytes = 4u * 1024u * 1024u;

[[nodiscard]] ServiceState& services(lua_State* L) noexcept
{
    return *context(L).services;
}

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

// **Whose record it is**: the account a ticket named; or, with no accounts,
// the key the player's machine joined with; or, for a player at the hosting
// machine itself, their number there -- that machine's key arrives only once
// it listens, and a record must not change hands when it does.
[[nodiscard]] std::string accountOf(const scene::PlayerComponent& player)
{
    if (!player.accountId.empty())
        return player.accountId;
    if (!player.local && !player.publicKey.empty())
        return "key:" + player.publicKey;
    return "local:" + std::to_string(player.userId);
}

// What a script may do with records: on a server with a database.
void requireData(lua_State* L)
{
    const scene::EngineState& state = world(L).engineState();
    if (state.networkTopology == scene::NetworkTopology::Replica || threadMakesLocal(L))
        raise(L, ENG_TR("script.err.data_client"));
    if (!state.dataAvailable)
        raise(L, ENG_TR("script.err.data_unavailable"));
}

// **Whose items or money**: a player's account, or a name the game gives
// what no player owns -- a guild's bank, an auction house. Nil is the game
// itself, where `game` allows it: money it pays comes from nowhere.
[[nodiscard]] std::string checkOwner(lua_State* L, int index, bool game)
{
    index = lua_absindex(L, index);
    if (game && lua_isnoneornil(L, index))
        return {};
    if (lua_type(L, index) == LUA_TSTRING) {
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        if (length == 0 || length > 128)
            raise(L, ENG_TR("script.err.data_owner"));
        return std::string(text, length);
    }
    const core::InstanceId player = checkInstance(L, index);
    const scene::PlayerComponent* component = world(L).players().find(player);
    if (component == nullptr)
        luaL_typeerrorL(L, index, "Player or string");
    return accountOf(*component);
}

// A whole number of at most 2^53, as an item's id or an amount is.
[[nodiscard]] core::i64 checkWhole(lua_State* L, int index, core::i64 least)
{
    const core::f64 number = luaL_checknumber(L, index);
    if (!std::isfinite(number) || std::floor(number) != number || number < static_cast<core::f64>(least) ||
        number > 9.0e15)
        raise(L, ENG_TR("script.err.data_whole_number"));
    return static_cast<core::i64>(number);
}

// What an operation's answer becomes in the thread that waits for it.
enum class DataOpKind : core::u8
{
    Transact,
    Create,
    Destroy,
    Items,
    Balance,
    Get,
    Set,
    Update,
};

// The request sent, under a new number; the number, for the waiter.
core::u32 sendOp(lua_State* L, std::string request)
{
    scene::EngineState& state = world(L).engineState();
    const core::u32 id = state.nextDataLoad++;
    state.dataOps.push_back(scene::EngineState::DataOp{id, std::move(request)});
    return id;
}

int askOp(lua_State* L, const core::JsonWriter& request, DataOpKind kind)
{
    const core::u32 id = sendOp(L, request.text());
    lua_pushthread(L);
    ServiceState::DataOpWaiter waiter;
    waiter.id = id;
    waiter.threadRef = lua_ref(L, -1);
    waiter.kind = static_cast<core::u8>(kind);
    services(L).dataOpWaiters.push_back(std::move(waiter));
    lua_pop(L, 1);
    return lua_yield(L, 0);
}

[[nodiscard]] std::string checkName(lua_State* L, int index, core::usize longest)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    if (length == 0 || length > longest)
        raise(L, ENG_TR("script.err.data_shared_name"));
    return std::string(text, length);
}

[[nodiscard]] std::string setRequest(std::string_view scope, std::string_view key, std::string_view value,
                                     core::i64 version)
{
    core::JsonWriter request(core::JsonLayout::Compact);
    request.beginObject();
    request.field("op", "set");
    request.field("scope", scope);
    request.field("key", key);
    request.field("value", value);
    request.field("version", version);
    request.endObject();
    return request.text();
}

// A shared value's text, pushed: nil for none.
void pushShared(lua_State* L, std::string_view text)
{
    if (text.empty())
        lua_pushnil(L);
    else
        pushSavedValue(L, text);
}

// The answer, as the waiting thread gets it; the number of values pushed,
// or -1 where it is an error, its message pushed.
int pushOpAnswer(lua_State* co, DataOpKind kind, std::string_view text)
{
    core::JsonDocument document;
    const bool parsed = static_cast<bool>(document.parse(text, "data-result"));
    const core::JsonValue root = document.root();
    const bool ok = parsed && root["ok"].asBool(false);
    switch (kind) {
    case DataOpKind::Transact:
        lua_pushboolean(co, ok ? 1 : 0);
        if (ok) {
            lua_pushnil(co);
        }
        else {
            const std::string_view reason = parsed ? root["reason"].asString("Failed") : "Failed";
            lua_pushlstring(co, reason.data(), reason.size());
        }
        return 2;
    case DataOpKind::Create:
        if (!ok) {
            const std::string said = core::formatKeyPrefixed(ENG_TR("script.err.data_item_failed"));
            lua_pushlstring(co, said.data(), said.size());
            return -1;
        }
        lua_pushnumber(co, static_cast<core::f64>(root["item"].asInteger(0)));
        return 1;
    case DataOpKind::Destroy:
        lua_pushboolean(co, ok ? 1 : 0);
        return 1;
    case DataOpKind::Items: {
        const core::JsonValue items = root["items"];
        lua_createtable(co, static_cast<int>(items.size()), 0);
        for (core::usize at = 0; at < items.size(); ++at) {
            const core::JsonValue item = items.at(at);
            lua_createtable(co, 0, 3);
            lua_pushnumber(co, static_cast<core::f64>(item["id"].asInteger(0)));
            lua_setfield(co, -2, "Id");
            const std::string_view kindName = item["kind"].asString();
            lua_pushlstring(co, kindName.data(), kindName.size());
            lua_setfield(co, -2, "Kind");
            const std::string_view fields = item["fields"].asString();
            if (fields.empty())
                lua_createtable(co, 0, 0);
            else
                pushSavedValue(co, fields);
            lua_setfield(co, -2, "Fields");
            lua_rawseti(co, -2, static_cast<int>(at + 1));
        }
        return 1;
    }
    case DataOpKind::Balance:
        lua_pushnumber(co, static_cast<core::f64>(root["balance"].asInteger(0)));
        return 1;
    case DataOpKind::Get:
        pushShared(co, root["value"].asString());
        lua_pushnumber(co, static_cast<core::f64>(root["version"].asInteger(0)));
        return 2;
    case DataOpKind::Set:
        lua_pushboolean(co, ok ? 1 : 0);
        lua_pushnumber(co, static_cast<core::f64>(root["version"].asInteger(0)));
        return 2;
    case DataOpKind::Update:
        pushShared(co, root["value"].asString());
        return 1;
    }
    return 0;
}

// `UpdateAsync`'s function, called with the value as it is, and what it
// returns as a saved value's text: empty for nil. Called protected -- it may
// raise, and must not yield.
int runUpdate(lua_State* L)
{
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1)) {
        lua_pushliteral(L, "");
        return 1;
    }
    const std::string text = encodeSavedValue(L, -1, MaxSharedBytes);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

[[nodiscard]] ServiceState::HeldRecord& checkRecord(lua_State* L, int index)
{
    auto** record = static_cast<ServiceState::HeldRecord**>(
        luaL_checkudatatagged(L, index, static_cast<int>(UserdataTag::DataRecord)));
    return **record;
}

[[nodiscard]] ServiceState::HeldRecord& checkHeld(lua_State* L, int index)
{
    ServiceState::HeldRecord& record = checkRecord(L, index);
    if (record.released) {
        const core::I18nArg args[] = {{"account", std::string_view{record.data.name}}};
        raise(L, ENG_TR("script.err.data_released"), args);
    }
    return record;
}

void pushRecord(lua_State* L, ServiceState::HeldRecord& record)
{
    void* memory = lua_newuserdatataggedwithmetatable(L, sizeof(ServiceState::HeldRecord*),
                                                      static_cast<int>(UserdataTag::DataRecord));
    *static_cast<ServiceState::HeldRecord**>(memory) = &record;
}

[[nodiscard]] std::string checkKey(lua_State* L, int index)
{
    if (lua_type(L, index) != LUA_TSTRING)
        luaL_typeerrorL(L, index, "string");
    size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    return std::string(text, length);
}

// Stores `value` (nil removes), refusing what would make the record larger
// than it may be and leaving it as it was.
void storeValue(lua_State* L, ServiceState::HeldRecord& record, const std::string& key, SaveValue value)
{
    const bool removing = value.table == nullptr && std::holds_alternative<std::monostate>(value.scalar);
    const auto previous = record.data.values.find(key);
    if (removing) {
        if (previous == record.data.values.end())
            return;
        record.data.values.erase(previous);
        ++record.data.generation;
        return;
    }
    std::optional<SaveValue> before;
    if (previous != record.data.values.end())
        before = previous->second;
    record.data.values.insert_or_assign(key, std::move(value));
    if (SaveStore::encodedSize(record.data) > MaxRecordBytes) {
        if (before.has_value())
            record.data.values.insert_or_assign(key, std::move(*before));
        else
            record.data.values.erase(key);
        const core::I18nArg args[] = {{"account", std::string_view{record.data.name}},
                                      {"limit", static_cast<core::i64>(MaxRecordBytes)}};
        raise(L, ENG_TR("script.err.data_too_large"), args);
    }
    ++record.data.generation;
}

// **Written**, and let go with it where `release` says: to the manager's
// database, or this server's own, by the host at the end of the frame.
void writeRecord(lua_State* L, ServiceState::HeldRecord& record, bool release)
{
    const core::f64 version = record.data.version;
    world(L).engineState().dataWrites.push_back(
        scene::EngineState::DataWrite{record.data.name, SaveStore::encode(record.data, version), version, release});
    record.data.written = record.data.generation;
    record.sinceWrite = 0.0;
    if (release) {
        record.released = true;
        record.data.values.clear();
        record.player = {};
    }
}

// --- DataRecord members ---------------------------------------------------------

int recordGetReleased(lua_State* L)
{
    lua_pushboolean(L, checkRecord(L, 1).released ? 1 : 0);
    return 1;
}

int recordGet(lua_State* L)
{
    const ServiceState::HeldRecord& record = checkHeld(L, 1);
    const auto found = record.data.values.find(checkKey(L, 2));
    if (found == record.data.values.end())
        lua_pushnil(L);
    else
        pushSaveValueOf(L, found->second);
    return 1;
}

int recordSet(lua_State* L)
{
    ServiceState::HeldRecord& record = checkHeld(L, 1);
    const std::string key = checkKey(L, 2);
    storeValue(L, record, key, saveValueFrom(L, 3));
    return 0;
}

int recordUpdate(lua_State* L)
{
    ServiceState::HeldRecord& record = checkHeld(L, 1);
    const std::string key = checkKey(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 3);
    const auto found = record.data.values.find(key);
    if (found == record.data.values.end())
        lua_pushnil(L);
    else
        pushSaveValueOf(L, found->second);
    // A plain call: the function must not yield, and Luau says so if it does.
    lua_call(L, 1, 1);
    storeValue(L, record, key, saveValueFrom(L, -1));
    return 1;
}

int recordRemove(lua_State* L)
{
    ServiceState::HeldRecord& record = checkHeld(L, 1);
    storeValue(L, record, checkKey(L, 2), SaveValue{});
    return 0;
}

int recordGetKeys(lua_State* L)
{
    const ServiceState::HeldRecord& record = checkHeld(L, 1);
    lua_createtable(L, static_cast<int>(record.data.values.size()), 0);
    int position = 1;
    for (const auto& [key, value] : record.data.values) {
        lua_pushlstring(L, key.data(), key.size());
        lua_rawseti(L, -2, position++);
    }
    return 1;
}

int recordSave(lua_State* L)
{
    writeRecord(L, checkHeld(L, 1), false);
    return 0;
}

int recordRelease(lua_State* L)
{
    ServiceState::HeldRecord& record = checkRecord(L, 1);
    if (!record.released)
        writeRecord(L, record, true);
    return 0;
}

// Two handles of one record are equal: a second `LoadAsync` returns the
// record already held.
int recordEquals(lua_State* L)
{
    const bool same = lua_userdatatag(L, 1) == static_cast<int>(UserdataTag::DataRecord) &&
                      lua_userdatatag(L, 2) == static_cast<int>(UserdataTag::DataRecord) &&
                      &checkRecord(L, 1) == &checkRecord(L, 2);
    lua_pushboolean(L, same ? 1 : 0);
    return 1;
}

int recordTostring(lua_State* L)
{
    const ServiceState::HeldRecord& record = checkRecord(L, 1);
    lua_pushfstring(L, "DataRecord(%s)", record.data.name.c_str());
    return 1;
}

} // namespace

int dataServiceLoadAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const core::InstanceId player = checkInstance(L, 2);
    requireYieldable(L, "LoadAsync");
    scene::World& w = world(L);
    scene::EngineState& state = w.engineState();
    requireData(L);
    const scene::PlayerComponent* component = w.players().find(player);
    if (component == nullptr)
        luaL_typeerrorL(L, 2, "Player");
    const std::string account = accountOf(*component);
    ServiceState& kept = services(L);
    // Held here already: the same record.
    if (const auto held = kept.records.find(account); held != kept.records.end() && !held->second->released) {
        pushRecord(L, *held->second);
        return 1;
    }
    const core::u32 id = state.nextDataLoad++;
    state.dataLoads.push_back(scene::EngineState::DataLoad{id, account});
    lua_pushthread(L);
    kept.dataWaiters.push_back(ServiceState::DataWaiter{id, lua_ref(L, -1), account, player});
    lua_pop(L, 1);
    return lua_yield(L, 0);
}

// **One step of an update**: true while it goes on, its next write sent under
// `waiter.id`; false once it is over -- written, or failed with
// `waiter.error`.
bool updateStep(lua_State* L, ServiceState::DataOpWaiter& waiter, std::string_view answer)
{
    core::JsonDocument document;
    if (!document.parse(answer, "data-result")) {
        waiter.error = core::formatKeyPrefixed(ENG_TR("script.err.data_shared_failed"));
        return false;
    }
    const core::JsonValue root = document.root();
    if (waiter.writing) {
        if (root["ok"].asBool(false))
            return false;
        // Another world wrote first: again, from what it wrote.
        if (++waiter.conflicts > MostConflicts) {
            const core::I18nArg args[] = {{"tries", static_cast<core::i64>(MostConflicts)}};
            waiter.error = core::formatKeyPrefixed(ENG_TR("script.err.data_update_conflicts"), args);
            return false;
        }
    }
    lua_pushcfunction(L, runUpdate, "UpdateAsync");
    lua_getref(L, waiter.functionRef);
    pushShared(L, root["value"].asString());
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        size_t length = 0;
        const char* said = lua_tolstring(L, -1, &length);
        waiter.error = said != nullptr ? std::string(said, length) : std::string{};
        lua_pop(L, 1);
        if (waiter.error.empty())
            waiter.error = core::formatKeyPrefixed(ENG_TR("script.err.data_shared_failed"));
        return false;
    }
    size_t length = 0;
    const char* text = lua_tolstring(L, -1, &length);
    std::string value(text != nullptr ? text : "", length);
    lua_pop(L, 1);
    waiter.id = sendOp(L, setRequest(waiter.scope, waiter.key, value, root["version"].asInteger(0)));
    waiter.writing = true;
    return true;
}

int dataServiceGetAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string scope = checkName(L, 2, 64);
    const std::string key = checkName(L, 3, 128);
    requireYieldable(L, "GetAsync");
    requireData(L);
    core::JsonWriter request(core::JsonLayout::Compact);
    request.beginObject();
    request.field("op", "get");
    request.field("scope", std::string_view{scope});
    request.field("key", std::string_view{key});
    request.endObject();
    return askOp(L, request, DataOpKind::Get);
}

int dataServiceSetAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string scope = checkName(L, 2, 64);
    const std::string key = checkName(L, 3, 128);
    const std::string value = lua_isnil(L, 4) ? std::string{} : encodeSavedValue(L, 4, MaxSharedBytes);
    const core::i64 version = checkWhole(L, 5, 0);
    requireYieldable(L, "SetAsync");
    requireData(L);
    const core::u32 id = sendOp(L, setRequest(scope, key, value, version));
    lua_pushthread(L);
    ServiceState::DataOpWaiter waiter;
    waiter.id = id;
    waiter.threadRef = lua_ref(L, -1);
    waiter.kind = static_cast<core::u8>(DataOpKind::Set);
    services(L).dataOpWaiters.push_back(std::move(waiter));
    lua_pop(L, 1);
    return lua_yield(L, 0);
}

// **Read, changed, written while nobody else wrote**: the value is read, the
// function makes the new one, and it is written only at the version read --
// where another world won, the function is called again with what it wrote.
// No lock is held across the script.
int dataServiceUpdateAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string scope = checkName(L, 2, 64);
    const std::string key = checkName(L, 3, 128);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    requireYieldable(L, "UpdateAsync");
    requireData(L);
    core::JsonWriter request(core::JsonLayout::Compact);
    request.beginObject();
    request.field("op", "get");
    request.field("scope", std::string_view{scope});
    request.field("key", std::string_view{key});
    request.endObject();
    const core::u32 id = sendOp(L, request.text());
    lua_pushvalue(L, 4);
    const int function = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_pushthread(L);
    ServiceState::DataOpWaiter waiter;
    waiter.id = id;
    waiter.threadRef = lua_ref(L, -1);
    waiter.kind = static_cast<core::u8>(DataOpKind::Update);
    waiter.functionRef = function;
    waiter.scope = scope;
    waiter.key = key;
    services(L).dataOpWaiters.push_back(std::move(waiter));
    lua_pop(L, 1);
    return lua_yield(L, 0);
}

bool dataCallbackGet(lua_State* L, core::InstanceId id, std::string_view key)
{
    scene::World& w = world(L);
    if (key != "OnMigrate" || w.atoms().text(w.classes().find(w.classOf(id))->name) != "DataService")
        return false;
    const int handler = services(L).dataMigrateHandler;
    if (handler != -1)
        lua_getref(L, handler);
    else
        lua_pushnil(L);
    return true;
}

bool dataCallbackSet(lua_State* L, core::InstanceId id, std::string_view key, int valueIndex)
{
    scene::World& w = world(L);
    if (key != "OnMigrate" || w.atoms().text(w.classes().find(w.classOf(id))->name) != "DataService")
        return false;
    if (!lua_isnil(L, valueIndex) && !lua_isfunction(L, valueIndex))
        raise(L, ENG_TR("script.err.data_migrate_handler"));
    ServiceState& state = services(L);
    if (state.dataMigrateHandler != -1)
        (void)lua_unref(L, state.dataMigrateHandler);
    state.dataMigrateHandler = -1;
    if (lua_isfunction(L, valueIndex)) {
        lua_pushvalue(L, valueIndex);
        state.dataMigrateHandler = lua_ref(L, -1);
        lua_pop(L, 1);
    }
    return true;
}

int dataServiceTransactAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    requireYieldable(L, "TransactAsync");
    requireData(L);
    core::JsonWriter request(core::JsonLayout::Compact);
    request.beginObject();
    request.field("op", "transact");
    request.key("moves");
    request.beginArray();
    const int count = lua_objlen(L, 2);
    if (count == 0)
        raise(L, ENG_TR("script.err.data_no_moves"));
    for (int at = 1; at <= count; ++at) {
        lua_rawgeti(L, 2, at);
        if (lua_type(L, -1) != LUA_TTABLE)
            raise(L, ENG_TR("script.err.data_move"));
        const int move = lua_gettop(L);
        request.beginObject();
        lua_getfield(L, move, "Move");
        lua_getfield(L, move, "Pay");
        const bool isItem = !lua_isnil(L, -2);
        if (isItem == !lua_isnil(L, -1))
            raise(L, ENG_TR("script.err.data_move"));
        if (isItem) {
            request.field("item", checkWhole(L, -2, 1));
        }
        else {
            request.field("amount", checkWhole(L, -1, 1));
            lua_getfield(L, move, "Currency");
            size_t length = 0;
            const char* currency = lua_tolstring(L, -1, &length);
            if (currency == nullptr || length == 0 || length > 32)
                raise(L, ENG_TR("script.err.data_currency"));
            request.field("currency", std::string_view{currency, length});
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
        lua_getfield(L, move, "From");
        request.field("from", std::string_view{checkOwner(L, -1, !isItem)});
        lua_pop(L, 1);
        lua_getfield(L, move, "To");
        request.field("to", std::string_view{checkOwner(L, -1, !isItem)});
        lua_pop(L, 1);
        request.endObject();
        lua_pop(L, 1);
    }
    request.endArray();
    request.endObject();
    return askOp(L, request, DataOpKind::Transact);
}

int dataServiceCreateItemAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireYieldable(L, "CreateItemAsync");
    requireData(L);
    const std::string owner = checkOwner(L, 2, false);
    size_t length = 0;
    const char* kind = luaL_checklstring(L, 3, &length);
    if (length == 0 || length > 64)
        raise(L, ENG_TR("script.err.data_item_kind"));
    // An item's fields are a saved value, of at most 64 KiB written.
    const std::string fields = lua_isnoneornil(L, 4) ? std::string{} : encodeSavedValue(L, 4, 64u * 1024u);
    core::JsonWriter request(core::JsonLayout::Compact);
    request.beginObject();
    request.field("op", "create");
    request.field("owner", std::string_view{owner});
    request.field("kind", std::string_view{kind, length});
    request.field("fields", std::string_view{fields});
    request.endObject();
    return askOp(L, request, DataOpKind::Create);
}

int dataServiceDestroyItemAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    const core::i64 item = checkWhole(L, 2, 1);
    requireYieldable(L, "DestroyItemAsync");
    requireData(L);
    core::JsonWriter request(core::JsonLayout::Compact);
    request.beginObject();
    request.field("op", "destroy");
    request.field("item", item);
    request.endObject();
    return askOp(L, request, DataOpKind::Destroy);
}

int dataServiceGetItemsAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireYieldable(L, "GetItemsAsync");
    requireData(L);
    core::JsonWriter request(core::JsonLayout::Compact);
    request.beginObject();
    request.field("op", "items");
    request.field("owner", std::string_view{checkOwner(L, 2, false)});
    request.endObject();
    return askOp(L, request, DataOpKind::Items);
}

int dataServiceGetBalanceAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    requireYieldable(L, "GetBalanceAsync");
    requireData(L);
    const std::string owner = checkOwner(L, 2, false);
    size_t length = 0;
    const char* currency = luaL_checklstring(L, 3, &length);
    if (length == 0 || length > 32)
        raise(L, ENG_TR("script.err.data_currency"));
    core::JsonWriter request(core::JsonLayout::Compact);
    request.beginObject();
    request.field("op", "balance");
    request.field("owner", std::string_view{owner});
    request.field("currency", std::string_view{currency, length});
    request.endObject();
    return askOp(L, request, DataOpKind::Balance);
}

void registerDataTypes(lua_State* L)
{
    VmContext& ctx = context(L);
    core::AtomTable& atoms = ctx.world->atoms();

    MemberTable& getters = ctx.getters[static_cast<core::usize>(UserdataTag::DataRecord)];
    addMember(getters, atoms, "Released", recordGetReleased);

    MemberTable& methods = ctx.methods[static_cast<core::usize>(UserdataTag::DataRecord)];
    addMember(methods, atoms, "Get", recordGet);
    addMember(methods, atoms, "Set", recordSet);
    addMember(methods, atoms, "Update", recordUpdate);
    addMember(methods, atoms, "Remove", recordRemove);
    addMember(methods, atoms, "GetKeys", recordGetKeys);
    addMember(methods, atoms, "Save", recordSave);
    addMember(methods, atoms, "Release", recordRelease);

    installTagMetatable(L, UserdataTag::DataRecord, recordEquals, recordTostring);
}

void resumeDataWaiters(lua_State* L, core::f64 dt)
{
    scene::World& w = world(L);
    scene::EngineState& state = w.engineState();
    ServiceState& kept = services(L);

    // **The database's answers**, to the threads that asked.
    std::vector<scene::EngineState::DataAnswer> answers;
    answers.swap(state.dataAnswers);
    for (scene::EngineState::DataAnswer& answer : answers) {
        const auto waiter =
            std::find_if(kept.dataWaiters.begin(), kept.dataWaiters.end(),
                         [&](const ServiceState::DataWaiter& waiting) { return waiting.id == answer.id; });
        if (waiter == kept.dataWaiters.end())
            continue;
        const ServiceState::DataWaiter asked = std::move(*waiter);
        kept.dataWaiters.erase(waiter);
        lua_getref(L, asked.threadRef);
        lua_State* co = lua_tothread(L, -1);
        lua_pop(L, 1);
        if (co == nullptr) {
            (void)lua_unref(L, asked.threadRef);
            continue;
        }
        if (!answer.ok) {
            (void)lua_unref(L, asked.threadRef);
            const core::I18nArg args[] = {{"account", std::string_view{asked.account}}};
            const std::string said = core::formatKeyPrefixed(
                answer.busy ? ENG_TR("script.err.data_busy") : ENG_TR("script.err.data_failed"), args);
            lua_pushlstring(co, said.data(), said.size());
            (void)resumeScheduledWithError(L, co);
            continue;
        }
        std::unique_ptr<ServiceState::HeldRecord>& slot = kept.records[asked.account];
        if (slot == nullptr)
            slot = std::make_unique<ServiceState::HeldRecord>();
        ServiceState::HeldRecord& record = *slot;
        bool migrate = false;
        // A second load answered while the first is held is the same record.
        if (record.released) {
            record.data = SaveSlotData{};
            record.data.name = asked.account;
            if (answer.found && !SaveStore::decode(answer.body, record.data)) {
                const std::array<core::I18nArg, 1> args{core::I18nArg{"account", std::string_view{asked.account}}};
                core::log(core::LogLevel::Error, ENG_TR("script.err.data_damaged"), args);
                record.data = SaveSlotData{};
                record.data.recovered = false;
            }
            record.data.name = asked.account;
            record.data.written = record.data.generation;
            record.released = false;
            record.sinceWrite = 0.0;
            record.player = asked.player;
            // A record with nothing in it is at the game's version already.
            migrate =
                record.data.version < state.dataVersion && !record.data.values.empty() && kept.dataMigrateHandler != -1;
            if (record.data.values.empty())
                record.data.version = state.dataVersion;
        }
        // **An older record is brought up to date first** (ADR 0238 §2), once,
        // in a thread of its own, before the thread that asked is handed it;
        // then written at the game's version.
        if (migrate) {
            lua_State* migrating = lua_newthread(L);
            const int migratingRef = lua_ref(L, -1);
            lua_pop(L, 1);
            lua_getref(migrating, kept.dataMigrateHandler);
            pushRecord(migrating, record);
            lua_pushnumber(migrating, record.data.version);
            const int status = startScheduled(L, migrating, 2);
            if (status == LUA_YIELD || status == LUA_BREAK) {
                kept.dataMigrations.push_back(
                    ServiceState::DataMigration{migratingRef, asked.threadRef, asked.account});
                continue;
            }
            (void)lua_unref(L, migratingRef);
            if (status == LUA_OK) {
                record.data.version = state.dataVersion;
                ++record.data.generation;
            }
        }
        pushRecord(co, record);
        (void)resumeScheduled(L, co, 1);
        (void)lua_unref(L, asked.threadRef);
    }
    // Migrations that waited, finished: their records handed over.
    for (auto migration = kept.dataMigrations.begin(); migration != kept.dataMigrations.end();) {
        lua_getref(L, migration->coRef);
        lua_State* migrating = lua_tothread(L, -1);
        const int status = migrating != nullptr ? lua_status(migrating) : LUA_OK;
        lua_pop(L, 1);
        if (status == LUA_YIELD || status == LUA_BREAK) {
            ++migration;
            continue;
        }
        const auto held = kept.records.find(migration->account);
        if (held != kept.records.end() && status == LUA_OK) {
            held->second->data.version = state.dataVersion;
            ++held->second->data.generation;
        }
        lua_getref(L, migration->waiterRef);
        lua_State* co = lua_tothread(L, -1);
        lua_pop(L, 1);
        if (co != nullptr && held != kept.records.end()) {
            pushRecord(co, *held->second);
            (void)resumeScheduled(L, co, 1);
        }
        (void)lua_unref(L, migration->coRef);
        (void)lua_unref(L, migration->waiterRef);
        migration = kept.dataMigrations.erase(migration);
    }

    // **Items, money and transactions**: each answer to its thread.
    std::vector<scene::EngineState::DataResult> results;
    results.swap(state.dataResults);
    for (const scene::EngineState::DataResult& result : results) {
        const auto waiter =
            std::find_if(kept.dataOpWaiters.begin(), kept.dataOpWaiters.end(),
                         [&](const ServiceState::DataOpWaiter& waiting) { return waiting.id == result.id; });
        if (waiter == kept.dataOpWaiters.end())
            continue;
        ServiceState::DataOpWaiter asked = *waiter;
        kept.dataOpWaiters.erase(waiter);
        // **An update**: a value read, or a write another world beat -- the
        // function makes the next value from what is there now, and it is
        // written at that version. Its thread waits on.
        if (static_cast<DataOpKind>(asked.kind) == DataOpKind::Update && updateStep(L, asked, result.answer)) {
            kept.dataOpWaiters.push_back(std::move(asked));
            continue;
        }
        if (asked.functionRef != -1)
            (void)lua_unref(L, asked.functionRef);
        lua_getref(L, asked.threadRef);
        lua_State* co = lua_tothread(L, -1);
        lua_pop(L, 1);
        (void)lua_unref(L, asked.threadRef);
        if (co == nullptr)
            continue;
        if (!asked.error.empty()) {
            lua_pushlstring(co, asked.error.data(), asked.error.size());
            (void)resumeScheduledWithError(L, co);
            continue;
        }
        const int pushed = pushOpAnswer(co, static_cast<DataOpKind>(asked.kind), result.answer);
        if (pushed < 0)
            (void)resumeScheduledWithError(L, co);
        else
            (void)resumeScheduled(L, co, pushed);
    }

    // **Written behind**, and let go of where the player is no longer here.
    for (auto& [account, held] : kept.records) {
        ServiceState::HeldRecord& record = *held;
        if (record.released)
            continue;
        if (!w.alive(record.player) || w.players().find(record.player) == nullptr) {
            writeRecord(L, record, true);
            continue;
        }
        record.sinceWrite += dt;
        if (record.sinceWrite >= WriteBehindSeconds) {
            if (record.data.dirty())
                writeRecord(L, record, false);
            record.sinceWrite = 0.0;
        }
    }
}

void releaseDataRecords(lua_State* L)
{
    for (auto& [account, held] : services(L).records) {
        if (!held->released)
            writeRecord(L, *held, true);
    }
}

} // namespace engine::script
