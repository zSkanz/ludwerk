#include "engine/script/data_service.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "engine/core/error.h"
#include "engine/core/i18n.h"
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

// A record's written form is a save slot's (ADR 0111), at this version until
// `DataService.Version` comes.
constexpr core::f64 RecordVersion = 1.0;
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
    world(L).engineState().dataWrites.push_back(scene::EngineState::DataWrite{
        record.data.name, SaveStore::encode(record.data, RecordVersion), RecordVersion, release});
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
    if (state.networkTopology == scene::NetworkTopology::Replica || threadMakesLocal(L))
        raise(L, ENG_TR("script.err.data_client"));
    if (!state.dataAvailable)
        raise(L, ENG_TR("script.err.data_unavailable"));
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
        (void)lua_unref(L, asked.threadRef);
        if (co == nullptr)
            continue;
        if (!answer.ok) {
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
        }
        pushRecord(co, record);
        (void)resumeScheduled(L, co, 1);
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
