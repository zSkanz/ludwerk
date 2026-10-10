#include "engine/script/animation.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <string>
#include <string_view>

#include "engine/core/i18n.h"
#include "engine/core/text_key.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/datatypes.h"
#include "engine/script/instance_binding.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

// The payload of an `AnimationTrack` userdata: an index into the VM's record
// list. Four bytes, and the `scene::TrackId` is not in it -- the record holds
// that, and putting it in both is two places to disagree.
struct TrackUserdata
{
    u32 record = 0;
};

static_assert(sizeof(TrackUserdata) == 4, "the AnimationTrack payload's size is an ABI decision");

[[nodiscard]] ServiceState& services(lua_State* L)
{
    return *context(L).services;
}

[[nodiscard]] TrackRecord* findRecord(lua_State* L, int index)
{
    const TrackUserdata payload =
        *static_cast<TrackUserdata*>(luaL_checkudatatagged(L, index, static_cast<int>(UserdataTag::AnimationTrack)));
    ServiceState& state = services(L);
    if (payload.record >= state.animationTracks.size())
        return nullptr;
    return &state.animationTracks[payload.record];
}

// The host, or null in a build with no render module. Every reader checks: a
// track with no host is a track that plays nothing, which is the same answer an
// unskinned mesh gives.
[[nodiscard]] scene::AnimationHost* host(lua_State* L)
{
    return services(L).animation;
}

[[nodiscard]] scene::TrackState stateOf(lua_State* L, const TrackRecord* record)
{
    scene::AnimationHost* animation = host(L);
    if (record == nullptr || animation == nullptr)
        return {};
    return animation->state(record->track);
}

// --- Reads -------------------------------------------------------------------

int trackGetPlaying(lua_State* L)
{
    lua_pushboolean(L, stateOf(L, findRecord(L, 1)).playing);
    return 1;
}

int trackGetLooped(lua_State* L)
{
    lua_pushboolean(L, stateOf(L, findRecord(L, 1)).looped);
    return 1;
}

int trackGetSpeed(lua_State* L)
{
    lua_pushnumber(L, static_cast<double>(stateOf(L, findRecord(L, 1)).speed));
    return 1;
}

int trackGetWeight(lua_State* L)
{
    lua_pushnumber(L, static_cast<double>(stateOf(L, findRecord(L, 1)).weight));
    return 1;
}

int trackGetLength(lua_State* L)
{
    lua_pushnumber(L, static_cast<double>(stateOf(L, findRecord(L, 1)).length));
    return 1;
}

int trackGetTimePosition(lua_State* L)
{
    lua_pushnumber(L, stateOf(L, findRecord(L, 1)).timePosition);
    return 1;
}

int trackGetEnded(lua_State* L)
{
    const TrackRecord* record = findRecord(L, 1);
    if (record == nullptr)
        return 0;
    pushSignalObject(L, record->ended);
    return 1;
}

// --- Writes ------------------------------------------------------------------

int trackSetLooped(lua_State* L)
{
    luaL_checktype(L, 3, LUA_TBOOLEAN);
    const TrackRecord* record = findRecord(L, 1);
    if (scene::AnimationHost* animation = host(L); animation != nullptr && record != nullptr)
        animation->setLooped(record->track, lua_toboolean(L, 3) != 0);
    return 0;
}

int trackSetSpeed(lua_State* L)
{
    const auto speed = static_cast<f32>(luaL_checknumber(L, 3));
    const TrackRecord* record = findRecord(L, 1);
    if (scene::AnimationHost* animation = host(L); animation != nullptr && record != nullptr)
        animation->adjustSpeed(record->track, speed);
    return 0;
}

int trackSetWeight(lua_State* L)
{
    const auto weight = static_cast<f32>(luaL_checknumber(L, 3));
    const TrackRecord* record = findRecord(L, 1);
    // No fade: an assignment is immediate, and the faded form is `Play`'s and
    // `Stop`'s first argument. A property that took a second parameter would not
    // be a property.
    if (scene::AnimationHost* animation = host(L); animation != nullptr && record != nullptr)
        animation->adjustWeight(record->track, weight, 0.0f);
    return 0;
}

// --- Methods -----------------------------------------------------------------

int trackPlay(lua_State* L)
{
    const auto fadeTime = static_cast<f32>(luaL_optnumber(L, 2, 0.0));
    const TrackRecord* record = findRecord(L, 1);
    scene::AnimationHost* animation = host(L);
    if (animation == nullptr || record == nullptr)
        return 0;

    // Weight and speed come from the track's own properties rather than from
    // arguments, so that `Play(0.2)` fades in to whatever the track was set to.
    // api-design.md §2.2 gives `Play` one parameter and this is what makes that
    // enough.
    const scene::TrackState state = animation->state(record->track);
    animation->play(record->track, fadeTime, state.weight, state.speed);
    return 0;
}

int trackStop(lua_State* L)
{
    const auto fadeTime = static_cast<f32>(luaL_optnumber(L, 2, 0.0));
    const TrackRecord* record = findRecord(L, 1);
    if (scene::AnimationHost* animation = host(L); animation != nullptr && record != nullptr)
        animation->stop(record->track, fadeTime);
    return 0;
}

} // namespace

// `AnimationPlayer:LoadAnimation(content)`.
//
// The argument is a Content URN, and everything after a `#` is the clip's name
// inside the file. A string with no `#` is a clip name in the player's own
// mesh, which is the common case and the one worth being short:
// `player:LoadAnimation("Walk")`.
//
// **A path before the `#` names the file the CLIP is in** (S6.8), and it need
// not be the file the player's own skeleton came from: one walk cycle authored
// once and played by every character in a game is the reason a clip is
// addressable at all. It is retargeted onto this rig by joint NAME, which is not
// new work -- `AnimationSystem` has mapped joints that way since one player had
// to drive a body and a shirt with different rigs. What was missing was any way
// to SAY which file the clip was in.
//
// A rig that shares no joint names with the clip's plays nothing rather than
// something wrong: the mapper skips a joint the target does not have, so a clip
// for a horse on a person moves the joints they have in common and no others.
int animationPlayerLoadAnimation(lua_State* L)
{
    const core::InstanceId player = checkInstance(L, 1);
    usize length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    const std::string_view content{text, length};

    std::string_view clip = content;
    std::string_view path;
    if (const usize hash = content.rfind('#'); hash != std::string_view::npos) {
        path = content.substr(0, hash);
        clip = content.substr(hash + 1);
    }

    ServiceState& state = services(L);
    TrackRecord record;
    if (scene::AnimationHost* animation = state.animation; animation != nullptr && !clip.empty()) {
        // **Interned rather than looked up**, because the library is keyed by
        // atom and a URN nothing has loaded yet is a track that finds no clip --
        // which is the same answer it gives for a name the file does not have,
        // and the state a `MeshPart` is in for the frames before its file
        // arrives.
        const core::NameAtom from = path.empty() ? core::NameAtom{} : context(L).world->atoms().intern(path);
        record.track = animation->createTrack(player, from, clip);
    }
    record.ended = createScriptSignal(L);

    state.animationTracks.push_back(record);

    void* memory =
        lua_newuserdatataggedwithmetatable(L, sizeof(TrackUserdata), static_cast<int>(UserdataTag::AnimationTrack));
    static_cast<TrackUserdata*>(memory)->record = static_cast<u32>(state.animationTracks.size() - 1);
    return 1;
}

namespace {

[[nodiscard]] std::string_view parameterName(lua_State* L, int index)
{
    usize length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    return std::string_view{text, length};
}

// A name the graph does not declare: said with the name, so a typo reads as
// one.
[[noreturn]] void raiseUnknownParameter(lua_State* L, std::string_view name)
{
    const core::I18nArg args[] = {{"name", name}};
    raise(L, ENG_TR("script.err.graph_parameter_unknown"), args);
}

} // namespace

int animationPlayerSetParameter(lua_State* L)
{
    const core::InstanceId player = checkInstance(L, 1);
    const std::string_view name = parameterName(L, 2);
    f32 value = 0.0f;
    if (lua_isboolean(L, 3)) {
        value = lua_toboolean(L, 3) != 0 ? 1.0f : 0.0f;
    }
    else {
        const double number = luaL_checknumber(L, 3);
        // Not a number, or one too many: a blend along it would be nowhere.
        if (!(number == number) || number > 3.0e38 || number < -3.0e38)
            raise(L, ENG_TR("script.err.graph_parameter_not_finite"));
        value = static_cast<f32>(number);
    }
    if (scene::AnimationHost* animation = host(L);
        animation != nullptr && animation->setGraphParameter(player, name, value) == scene::GraphWrite::Unknown)
        raiseUnknownParameter(L, name);
    return 0;
}

int animationPlayerClearParameter(lua_State* L)
{
    const core::InstanceId player = checkInstance(L, 1);
    const std::string_view name = parameterName(L, 2);
    if (scene::AnimationHost* animation = host(L);
        animation != nullptr && animation->clearGraphParameter(player, name) == scene::GraphWrite::Unknown)
        raiseUnknownParameter(L, name);
    return 0;
}

int animationPlayerGetParameter(lua_State* L)
{
    const core::InstanceId player = checkInstance(L, 1);
    const std::string_view name = parameterName(L, 2);
    const scene::AnimationHost* animation = host(L);
    const scene::GraphParameterValue held =
        animation != nullptr ? animation->graphParameter(player, name) : scene::GraphParameterValue{};
    switch (held.kind) {
    case scene::GraphParameterValue::Kind::Number:
        lua_pushnumber(L, static_cast<double>(held.value));
        break;
    case scene::GraphParameterValue::Kind::Boolean:
    case scene::GraphParameterValue::Kind::Trigger:
        lua_pushboolean(L, held.value != 0.0f ? 1 : 0);
        break;
    case scene::GraphParameterValue::Kind::None:
        lua_pushnil(L);
        break;
    }
    return 1;
}

int animationPlayerGetState(lua_State* L)
{
    const core::InstanceId player = checkInstance(L, 1);
    std::string_view layer;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2))
        layer = parameterName(L, 2);
    const scene::AnimationHost* animation = host(L);
    const std::string_view state = animation != nullptr ? animation->graphState(player, layer) : std::string_view{};
    lua_pushlstring(L, state.data(), state.size());
    return 1;
}

void fireGraphSignals(lua_State* L, std::span<const scene::GraphSignal> signals)
{
    if (signals.empty())
        return;
    scene::World& world = *context(L).world;
    const core::NameAtom stateChanged = world.atoms().lookup("StateChanged");
    const core::NameAtom eventReached = world.atoms().lookup("EventReached");
    for (const scene::GraphSignal& signal : signals) {
        if (!world.alive(signal.player) || world.destroyed(signal.player))
            continue;
        const scene::EventDesc* const descriptor =
            world.classes().findEvent(world.classOf(signal.player), signal.event ? eventReached : stateChanged);
        if (descriptor == nullptr || !instanceEventHeard(L, signal.player, descriptor->slot))
            continue;
        const int first = lua_gettop(L) + 1;
        if (signal.event) {
            lua_pushlstring(L, signal.to.data(), signal.to.size());
            lua_pushlstring(L, signal.layer.data(), signal.layer.size());
        }
        else {
            lua_pushlstring(L, signal.layer.data(), signal.layer.size());
            lua_pushlstring(L, signal.from.data(), signal.from.size());
            lua_pushlstring(L, signal.to.data(), signal.to.size());
        }
        const int count = lua_gettop(L) - first + 1;
        fireInstanceEvent(L, signal.player, descriptor->slot, first, count);
        lua_pop(L, count);
    }
}

void registerAnimationTypes(lua_State* L)
{
    VmContext& ctx = context(L);
    core::AtomTable& atoms = ctx.world->atoms();

    MemberTable& getters = ctx.getters[static_cast<usize>(UserdataTag::AnimationTrack)];
    addMember(getters, atoms, "Playing", trackGetPlaying);
    addMember(getters, atoms, "Looped", trackGetLooped);
    addMember(getters, atoms, "Speed", trackGetSpeed);
    addMember(getters, atoms, "Weight", trackGetWeight);
    addMember(getters, atoms, "Length", trackGetLength);
    addMember(getters, atoms, "TimePosition", trackGetTimePosition);
    addMember(getters, atoms, "Ended", trackGetEnded);

    // The first non-empty setter table in the surface. A track is a handle, so a
    // write to one is seen by every holder of it -- which is what makes these
    // properties rather than the `SetLooped`/`AdjustSpeed` pair a value type
    // would have needed.
    MemberTable& setters = ctx.setters[static_cast<usize>(UserdataTag::AnimationTrack)];
    addMember(setters, atoms, "Looped", trackSetLooped);
    addMember(setters, atoms, "Speed", trackSetSpeed);
    addMember(setters, atoms, "Weight", trackSetWeight);

    MemberTable& methods = ctx.methods[static_cast<usize>(UserdataTag::AnimationTrack)];
    addMember(methods, atoms, "Play", trackPlay);
    addMember(methods, atoms, "Stop", trackStop);

    // No `__eq`: two handles to one track compare by their bits, which a
    // trivially-copyable payload gives for free.
    installTagMetatable(L, UserdataTag::AnimationTrack, nullptr, nullptr);
}

void fireAnimationEnded(lua_State* L, std::span<const scene::TrackId> ended)
{
    if (ended.empty())
        return;

    ServiceState& state = services(L);
    // Walked in the host's order, which is track order, which is load order --
    // R10's requirement that an observable order come from something that
    // promises one.
    for (const scene::TrackId track : ended) {
        for (const TrackRecord& record : state.animationTracks) {
            // Every handle loaded for this track fires, because two
            // `LoadAnimation` calls for one clip are two handles that both
            // connected.
            if (record.track == track && track != 0)
                fireSignal(L, record.ended, 0, 0);
        }
    }
}

} // namespace engine::script
