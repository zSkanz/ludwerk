#include "engine/script/save_service.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "engine/core/error.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/text_key.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/datatypes.h"
#include "engine/script/instance_binding.h"
#include "engine/script/save_store.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

// How deep a saved table may nest. Deep enough for any game's data and a
// wall well before the C stack: a table built by accident to nest forever
// is an error, not a crash.
constexpr int MaxDepth = 32;

[[nodiscard]] ServiceState& services(lua_State* L) noexcept
{
    return *context(L).services;
}

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

[[nodiscard]] core::f64 gameVersion(lua_State* L) noexcept
{
    return world(L).engineState().saveVersion;
}

[[nodiscard]] SaveStore& store(lua_State* L)
{
    SaveStore* saves = services(L).saves;
    if (saves == nullptr)
        raise(L, ENG_TR("script.err.save_unavailable"));
    return *saves;
}

[[nodiscard]] SaveSlotData& checkSlot(lua_State* L, int index)
{
    auto** slot = static_cast<SaveSlotData**>(luaL_checkudatatagged(L, index, static_cast<int>(UserdataTag::SaveSlot)));
    return **slot;
}

void pushSlot(lua_State* L, SaveSlotData& slot)
{
    void* memory =
        lua_newuserdatataggedwithmetatable(L, sizeof(SaveSlotData*), static_cast<int>(UserdataTag::SaveSlot));
    *static_cast<SaveSlotData**>(memory) = &slot;
}

[[nodiscard]] std::string checkKey(lua_State* L, int index)
{
    if (lua_type(L, index) != LUA_TSTRING)
        luaL_typeerrorL(L, index, "string");
    size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    return std::string(text, length);
}

[[noreturn]] void raiseValue(lua_State* L, std::string_view what)
{
    const core::I18nArg args[] = {{"valueType", what}};
    raise(L, ENG_TR("script.err.save_value"), args);
}

// A Luau value as a save holds it, or an error naming what it could not hold.
[[nodiscard]] SaveValue toSaveValue(lua_State* L, int index, int depth, std::vector<const void*>& path)
{
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TNIL:
        return SaveValue{};
    case LUA_TBOOLEAN:
        return SaveValue{scene::Value{lua_toboolean(L, index) != 0}, nullptr};
    case LUA_TNUMBER: {
        const core::f64 number = lua_tonumber(L, index);
        // JSON has no infinity and no NaN; a save that wrote one would read
        // back something else.
        if (!std::isfinite(number))
            raiseValue(L, "a number that is not finite");
        return SaveValue{scene::Value{number}, nullptr};
    }
    case LUA_TSTRING: {
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        return SaveValue{scene::Value{std::string(text, length)}, nullptr};
    }
    case LUA_TVECTOR:
        if (std::optional<scene::Value> value = toValue(L, index, scene::ValueType::Vector3))
            return SaveValue{std::move(*value), nullptr};
        break;
    case LUA_TUSERDATA:
        for (const scene::ValueType candidate :
             {scene::ValueType::CFrame, scene::ValueType::Color3, scene::ValueType::Vector2, scene::ValueType::UDim,
              scene::ValueType::UDim2, scene::ValueType::Rect, scene::ValueType::ColorSequence,
              scene::ValueType::NumberSequence}) {
            if (std::optional<scene::Value> value = toValue(L, index, candidate))
                return SaveValue{std::move(*value), nullptr};
        }
        raiseValue(L, luaL_typename(L, index));
    case LUA_TTABLE:
        break;
    default:
        raiseValue(L, luaL_typename(L, index));
    }

    if (depth >= MaxDepth)
        raiseValue(L, "a table nested too deep");
    const void* identity = lua_topointer(L, index);
    if (std::find(path.begin(), path.end(), identity) != path.end())
        raiseValue(L, "a table that holds itself");
    path.push_back(identity);
    lua_checkstack(L, 4);

    auto table = std::make_shared<SaveTable>();
    std::vector<std::pair<core::f64, SaveValue>> numbered;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        SaveValue item = toSaveValue(L, -1, depth + 1, path);
        if (lua_type(L, -2) == LUA_TSTRING) {
            size_t length = 0;
            const char* key = lua_tolstring(L, -2, &length);
            table->fields.emplace(std::string(key, length), std::move(item));
        }
        else if (lua_type(L, -2) == LUA_TNUMBER) {
            numbered.emplace_back(lua_tonumber(L, -2), std::move(item));
        }
        else {
            raiseValue(L, "a table with a key that is neither a string nor a position");
        }
        lua_pop(L, 1);
    }
    // The numbered keys are an array, 1 to n with no gaps -- anything else
    // has no one spelling a file could keep.
    std::sort(numbered.begin(), numbered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (core::usize at = 0; at < numbered.size(); ++at) {
        if (numbered[at].first != static_cast<core::f64>(at + 1))
            raiseValue(L, "a table whose numbered keys are not 1 to n");
        table->array.push_back(std::move(numbered[at].second));
    }
    path.pop_back();
    return SaveValue{scene::Value{}, std::move(table)};
}

void pushSaveValue(lua_State* L, const SaveValue& value)
{
    lua_checkstack(L, 4);
    if (value.table == nullptr) {
        pushValue(L, value.scalar);
        return;
    }
    lua_createtable(L, static_cast<int>(value.table->array.size()), static_cast<int>(value.table->fields.size()));
    int position = 1;
    for (const SaveValue& item : value.table->array) {
        pushSaveValue(L, item);
        lua_rawseti(L, -2, position++);
    }
    for (const auto& [key, item] : value.table->fields) {
        lua_pushlstring(L, key.data(), key.size());
        pushSaveValue(L, item);
        lua_rawset(L, -3);
    }
}

// The slot's `Changed`, one per slot per VM, made the first time it is asked
// for or fired.
[[nodiscard]] SignalId slotSignal(lua_State* L, const SaveSlotData& slot)
{
    auto& signals = services(L).slotSignals;
    if (const auto found = signals.find(slot.name); found != signals.end())
        return found->second;
    const SignalId made = createScriptSignal(L);
    signals.emplace(slot.name, made);
    return made;
}

void fireChanged(lua_State* L, const SaveSlotData& slot, const std::string& key)
{
    const SignalId signal = slotSignal(L, slot);
    lua_pushlstring(L, key.data(), key.size());
    fireSignal(L, signal, lua_gettop(L), 1);
    lua_pop(L, 1);
}

// Stores `value` (nil removes) and says so, refusing what would take the slot
// past the project's limit and leaving it as it was.
void storeValue(lua_State* L, SaveSlotData& slot, const std::string& key, SaveValue value)
{
    // **Measured by the entry that changes** (audit S12): every `Set` encoded
    // the whole slot to check the limit, so a game writing a few keys a frame
    // into a big slot re-serialised all of it a few times a frame.
    if (!slot.entryBytesKnown) {
        slot.entryBytes = 0;
        for (const auto& [name, held] : slot.values)
            slot.entryBytes += SaveStore::entrySize(name, held);
        slot.entryBytesKnown = true;
    }
    const auto previous = slot.values.find(key);
    const bool had = previous != slot.values.end();
    const core::u64 old = had ? SaveStore::entrySize(key, previous->second) : 0;
    const bool removing = value.table == nullptr && std::holds_alternative<std::monostate>(value.scalar);
    if (removing) {
        if (!had)
            return;
        slot.values.erase(previous);
        slot.entryBytes -= old;
    }
    else {
        const core::u64 now = SaveStore::entrySize(key, value);
        const SaveStore& saves = store(L);
        const core::u64 count = slot.values.size() + (had ? 0 : 1);
        if (SaveStore::payloadSize(slot.entryBytes - old + now, count) > saves.options().maxSlotBytes) {
            const core::I18nArg args[] = {{"slot", std::string_view{slot.name}},
                                          {"limit", static_cast<core::i64>(saves.options().maxSlotBytes)}};
            raise(L, ENG_TR("script.err.save_too_large"), args);
        }
        if (had)
            previous->second = std::move(value);
        else
            slot.values.emplace(key, std::move(value));
        slot.entryBytes = slot.entryBytes - old + now;
    }
    ++slot.generation;
    fireChanged(L, slot, key);
}

// --- SaveSlot members -----------------------------------------------------------

int slotGetName(lua_State* L)
{
    const SaveSlotData& slot = checkSlot(L, 1);
    lua_pushlstring(L, slot.name.data(), slot.name.size());
    return 1;
}

int slotGetRecovered(lua_State* L)
{
    lua_pushboolean(L, checkSlot(L, 1).recovered ? 1 : 0);
    return 1;
}

int slotGetChanged(lua_State* L)
{
    pushSignalObject(L, slotSignal(L, checkSlot(L, 1)));
    return 1;
}

int slotGet(lua_State* L)
{
    const SaveSlotData& slot = checkSlot(L, 1);
    const std::string key = checkKey(L, 2);
    const auto found = slot.values.find(key);
    if (found == slot.values.end())
        lua_pushnil(L);
    else
        pushSaveValue(L, found->second);
    return 1;
}

int slotSet(lua_State* L)
{
    SaveSlotData& slot = checkSlot(L, 1);
    const std::string key = checkKey(L, 2);
    std::vector<const void*> path;
    storeValue(L, slot, key, toSaveValue(L, 3, 0, path));
    return 0;
}

int slotUpdate(lua_State* L)
{
    SaveSlotData& slot = checkSlot(L, 1);
    const std::string key = checkKey(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_pushvalue(L, 3);
    const auto found = slot.values.find(key);
    if (found == slot.values.end())
        lua_pushnil(L);
    else
        pushSaveValue(L, found->second);
    // A plain call: the function must not yield, and Luau says so if it does.
    lua_call(L, 1, 1);
    std::vector<const void*> path;
    storeValue(L, slot, key, toSaveValue(L, -1, 0, path));
    return 1;
}

int slotRemove(lua_State* L)
{
    SaveSlotData& slot = checkSlot(L, 1);
    storeValue(L, slot, checkKey(L, 2), SaveValue{});
    return 0;
}

int slotGetKeys(lua_State* L)
{
    const SaveSlotData& slot = checkSlot(L, 1);
    lua_createtable(L, static_cast<int>(slot.values.size()), 0);
    int position = 1;
    for (const auto& [key, value] : slot.values) {
        lua_pushlstring(L, key.data(), key.size());
        lua_rawseti(L, -2, position++);
    }
    return 1;
}

int slotSaveAsync(lua_State* L)
{
    SaveSlotData& slot = checkSlot(L, 1);
    const core::u64 ticket = store(L).write(slot, gameVersion(L));
    // A caller that cannot wait -- the main thread, or one inside a
    // metamethod (audit S7) -- gets the write queued and an answer now.
    if (!lua_pushthread(L) && lua_isyieldable(L)) {
        const int threadRef = lua_ref(L, -1);
        lua_pop(L, 1);
        services(L).saveWaiters.push_back(ServiceState::SaveWaiter{ticket, threadRef, slot.name});
        return lua_yield(L, 0);
    }
    // The main thread cannot wait; nothing runs a script there, but a host
    // calling in directly gets the write queued and an answer now.
    lua_pop(L, 1);
    return 0;
}

int slotTostring(lua_State* L)
{
    const SaveSlotData& slot = checkSlot(L, 1);
    lua_pushfstring(L, "SaveSlot(%s)", slot.name.c_str());
    return 1;
}

} // namespace

// --- SaveService -------------------------------------------------------------------

int saveServiceGetSlotAsync(lua_State* L)
{
    const std::string name = checkKey(L, 2);
    if (!SaveStore::validName(name)) {
        const core::I18nArg args[] = {{"name", std::string_view{name}}};
        raise(L, ENG_TR("script.err.save_slot_name"), args);
    }
    SaveStore& saves = store(L);
    if (const std::string_view held = saves.otherSpelling(name); !held.empty()) {
        const core::I18nArg args[] = {{"name", std::string_view{name}}, {"held", held}};
        raise(L, ENG_TR("script.err.save_slot_case"), args);
    }
    SaveDamage damage = SaveDamage::None;
    SaveSlotData* slot = saves.open(name, &damage);
    if (slot == nullptr) {
        const core::I18nArg args[] = {{"limit", static_cast<core::i64>(saves.options().maxSlots)}};
        raise(L, ENG_TR("script.err.save_too_many_slots"), args);
    }
    if (damage != SaveDamage::None) {
        const core::I18nArg args[] = {{"slot", std::string_view{name}}};
        core::log(core::LogLevel::Warn,
                  damage == SaveDamage::FromBackup ? ENG_TR("script.warn.save_from_backup")
                                                   : ENG_TR("script.warn.save_lost"),
                  args);
    }

    ServiceState& state = services(L);
    ServiceState::SlotWaiter waiter;
    waiter.slot = slot;

    // **An older slot is brought up to date first** (ADR 0111 section 4),
    // once, in a thread of its own, before anybody is handed it. One that is
    // already migrating is waited for, not migrated twice.
    const core::f64 version = gameVersion(L);
    const bool migrating =
        std::any_of(state.slotWaiters.begin(), state.slotWaiters.end(), [&](const ServiceState::SlotWaiter& other) {
            return other.slot == slot && other.migrateRef != -1;
        });
    if (!migrating && slot->version < version && !slot->values.empty() && state.migrateHandler != -1) {
        lua_State* co = lua_newthread(L);
        const int coRef = lua_ref(L, -1);
        lua_pop(L, 1);
        lua_getref(co, state.migrateHandler);
        pushSlot(co, *slot);
        lua_pushnumber(co, slot->version);
        const int status = startScheduled(L, co, 2);
        if (status == LUA_YIELD || status == LUA_BREAK) {
            waiter.migrateRef = coRef;
        }
        else {
            (void)lua_unref(L, coRef);
            if (status == LUA_OK) {
                slot->version = version;
                ++slot->generation;
            }
        }
    }
    else if (!migrating && slot->values.empty()) {
        // Nothing to migrate: an empty slot is at the game's version already.
        slot->version = version;
    }

    if (lua_pushthread(L) || !lua_isyieldable(L)) {
        // The main thread, or a caller that cannot wait (audit S7): answered
        // now.
        lua_pop(L, 1);
        pushSlot(L, *slot);
        return 1;
    }
    waiter.threadRef = lua_ref(L, -1);
    lua_pop(L, 1);
    state.slotWaiters.push_back(waiter);
    return lua_yield(L, 0);
}

int saveServiceListSlots(lua_State* L)
{
    const std::vector<std::string> names = store(L).list();
    lua_createtable(L, static_cast<int>(names.size()), 0);
    int position = 1;
    for (const std::string& name : names) {
        lua_pushlstring(L, name.data(), name.size());
        lua_rawseti(L, -2, position++);
    }
    return 1;
}

int saveServiceDeleteSlot(lua_State* L)
{
    const std::string name = checkKey(L, 2);
    if (!SaveStore::validName(name)) {
        const core::I18nArg args[] = {{"name", std::string_view{name}}};
        raise(L, ENG_TR("script.err.save_slot_name"), args);
    }
    (void)store(L).remove(name);
    return 0;
}

bool saveCallbackGet(lua_State* L, core::InstanceId id, std::string_view key)
{
    scene::World& w = world(L);
    if (key != "OnMigrate" || w.atoms().text(w.classes().find(w.classOf(id))->name) != "SaveService")
        return false;
    const int handler = services(L).migrateHandler;
    if (handler != -1)
        lua_getref(L, handler);
    else
        lua_pushnil(L);
    return true;
}

bool saveCallbackSet(lua_State* L, core::InstanceId id, std::string_view key, int valueIndex)
{
    scene::World& w = world(L);
    if (key != "OnMigrate" || w.atoms().text(w.classes().find(w.classOf(id))->name) != "SaveService")
        return false;
    if (!lua_isnil(L, valueIndex) && !lua_isfunction(L, valueIndex))
        raise(L, ENG_TR("script.err.save_migrate_handler"));
    ServiceState& state = services(L);
    if (state.migrateHandler != -1)
        (void)lua_unref(L, state.migrateHandler);
    state.migrateHandler = -1;
    if (lua_isfunction(L, valueIndex)) {
        lua_pushvalue(L, valueIndex);
        state.migrateHandler = lua_ref(L, -1);
        lua_pop(L, 1);
    }
    return true;
}

void registerSaveTypes(lua_State* L)
{
    VmContext& ctx = context(L);
    core::AtomTable& atoms = ctx.world->atoms();

    MemberTable& getters = ctx.getters[static_cast<core::usize>(UserdataTag::SaveSlot)];
    addMember(getters, atoms, "Name", slotGetName);
    addMember(getters, atoms, "Recovered", slotGetRecovered);
    addMember(getters, atoms, "Changed", slotGetChanged);

    MemberTable& methods = ctx.methods[static_cast<core::usize>(UserdataTag::SaveSlot)];
    addMember(methods, atoms, "Get", slotGet);
    addMember(methods, atoms, "Set", slotSet);
    addMember(methods, atoms, "Update", slotUpdate);
    addMember(methods, atoms, "Remove", slotRemove);
    addMember(methods, atoms, "GetKeys", slotGetKeys);
    addMember(methods, atoms, "SaveAsync", slotSaveAsync);

    installTagMetatable(L, UserdataTag::SaveSlot, nullptr, slotTostring);
}

void resumeSaveWaiters(lua_State* L, core::f64 dt)
{
    ServiceState& state = services(L);
    if (state.saves == nullptr)
        return;
    state.saves->pump(dt, gameVersion(L));

    // Collected before any is resumed: a resumed thread may ask for another
    // slot, and the vector it would push onto is the one being walked.
    std::vector<ServiceState::SlotWaiter> readySlots;
    for (auto& waiter : state.slotWaiters) {
        if (waiter.migrateRef == -1)
            continue;
        lua_getref(L, waiter.migrateRef);
        lua_State* co = lua_tothread(L, -1);
        const int status = co != nullptr ? lua_status(co) : LUA_OK;
        lua_pop(L, 1);
        if (status == LUA_YIELD || status == LUA_BREAK)
            continue;
        if (status == LUA_OK) {
            waiter.slot->version = gameVersion(L);
            ++waiter.slot->generation;
        }
        (void)lua_unref(L, waiter.migrateRef);
        waiter.migrateRef = -1;
    }
    for (auto waiter = state.slotWaiters.begin(); waiter != state.slotWaiters.end();) {
        const SaveSlotData* slot = waiter->slot;
        const bool blocked =
            std::any_of(state.slotWaiters.begin(), state.slotWaiters.end(), [&](const ServiceState::SlotWaiter& other) {
                return other.slot == slot && other.migrateRef != -1;
            });
        if (blocked) {
            ++waiter;
            continue;
        }
        readySlots.push_back(*waiter);
        waiter = state.slotWaiters.erase(waiter);
    }
    for (const ServiceState::SlotWaiter& waiter : readySlots) {
        lua_getref(L, waiter.threadRef);
        lua_State* co = lua_tothread(L, -1);
        if (co != nullptr) {
            pushSlot(co, *waiter.slot);
            (void)resumeScheduled(L, co, 1);
            lua_pop(L, 1);
            // Whatever the thread did (audit S3): one that parks again took its
            // own reference where it waits.
            (void)lua_unref(L, waiter.threadRef);
            continue;
        }
        lua_pop(L, 1);
        (void)lua_unref(L, waiter.threadRef);
    }

    std::vector<std::pair<ServiceState::SaveWaiter, bool>> readySaves;
    for (auto waiter = state.saveWaiters.begin(); waiter != state.saveWaiters.end();) {
        bool succeeded = false;
        if (!state.saves->finished(waiter->ticket, succeeded)) {
            ++waiter;
            continue;
        }
        readySaves.emplace_back(*waiter, succeeded);
        waiter = state.saveWaiters.erase(waiter);
    }
    for (const auto& [waiter, succeeded] : readySaves) {
        lua_getref(L, waiter.threadRef);
        lua_State* co = lua_tothread(L, -1);
        if (co != nullptr) {
            if (succeeded) {
                (void)resumeScheduled(L, co, 0);
            }
            else {
                const core::I18nArg args[] = {{"slot", std::string_view{waiter.slot}}};
                const std::string message = core::formatKeyPrefixed(ENG_TR("script.err.save_write_failed"), args);
                lua_pushlstring(co, message.data(), message.size());
                (void)resumeScheduledWithError(L, co);
            }
        }
        lua_pop(L, 1);
        // Whatever the thread did (audit S3): one that parks again took its
        // own reference where it waits.
        (void)lua_unref(L, waiter.threadRef);
    }
}

} // namespace engine::script
