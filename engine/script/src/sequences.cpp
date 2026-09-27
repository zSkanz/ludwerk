#include "engine/script/sequences.h"

#include <lua.h>
#include <lualib.h>

#include <cmath>
#include <cstdio>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "engine/core/text_key.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/datatypes.h"

namespace engine::script {
namespace {

using core::ColorKeypoint;
using core::ColorSequence;
using core::NumberKeypoint;
using core::NumberSequence;

template <class T>
void pushPayload(lua_State* L, UserdataTag tag, T value)
{
    void* memory = lua_newuserdatataggedwithmetatable(L, sizeof(T), static_cast<int>(tag));
    new (memory) T(std::move(value));
}

template <class T>
[[nodiscard]] T& checkPayload(lua_State* L, int index, UserdataTag tag)
{
    return *static_cast<T*>(luaL_checkudatatagged(L, index, static_cast<int>(tag)));
}

template <class T>
[[nodiscard]] T* toPayload(lua_State* L, int index, UserdataTag tag) noexcept
{
    return static_cast<T*>(lua_touserdatatagged(L, index, static_cast<int>(tag)));
}

void pushNumber(lua_State* L, core::f32 value)
{
    lua_pushnumber(L, static_cast<double>(value));
}

// A keypoint's time, checked where it is made: a stop past the ends is a
// mistake to report at the stop, not later at the sequence that holds it.
[[nodiscard]] core::f32 checkTime(lua_State* L, int index)
{
    const double time = luaL_checknumber(L, index);
    if (!(time >= 0.0 && time <= 1.0)) {
        char text[32]{};
        std::snprintf(text, sizeof(text), "%g", time);
        const core::I18nArg args[] = {{"time", std::string_view{text}}};
        raise(L, ENG_TR("script.err.keypoint_time"), args);
    }
    return static_cast<core::f32>(time);
}

[[nodiscard]] core::f32 checkFinite(lua_State* L, int index)
{
    const double value = luaL_checknumber(L, index);
    if (!std::isfinite(value))
        raise(L, ENG_TR("script.err.bad_sequence"));
    return static_cast<core::f32>(value);
}

// --- ColorSequenceKeypoint ----------------------------------------------------

int colorKeypointNew(lua_State* L)
{
    const core::f32 time = checkTime(L, 1);
    pushPayload(L, UserdataTag::ColorSequenceKeypoint, ColorKeypoint{time, checkColor3(L, 2)});
    return 1;
}

int colorKeypointGetTime(lua_State* L)
{
    pushNumber(L, checkPayload<ColorKeypoint>(L, 1, UserdataTag::ColorSequenceKeypoint).time);
    return 1;
}

int colorKeypointGetValue(lua_State* L)
{
    pushColor3(L, checkPayload<ColorKeypoint>(L, 1, UserdataTag::ColorSequenceKeypoint).value);
    return 1;
}

int colorKeypointEq(lua_State* L)
{
    const ColorKeypoint* a = toPayload<ColorKeypoint>(L, 1, UserdataTag::ColorSequenceKeypoint);
    const ColorKeypoint* b = toPayload<ColorKeypoint>(L, 2, UserdataTag::ColorSequenceKeypoint);
    lua_pushboolean(L, a != nullptr && b != nullptr && *a == *b);
    return 1;
}

int colorKeypointTostring(lua_State* L)
{
    const ColorKeypoint& self = checkPayload<ColorKeypoint>(L, 1, UserdataTag::ColorSequenceKeypoint);
    char text[96]{};
    std::snprintf(text, sizeof(text), "%g %g %g %g", static_cast<double>(self.time), static_cast<double>(self.value.r),
                  static_cast<double>(self.value.g), static_cast<double>(self.value.b));
    lua_pushstring(L, text);
    return 1;
}

// --- NumberSequenceKeypoint ---------------------------------------------------

int numberKeypointNew(lua_State* L)
{
    const core::f32 time = checkTime(L, 1);
    const core::f32 value = checkFinite(L, 2);
    const core::f32 envelope = lua_isnoneornil(L, 3) ? 0.0f : checkFinite(L, 3);
    pushPayload(L, UserdataTag::NumberSequenceKeypoint, NumberKeypoint{time, value, envelope});
    return 1;
}

int numberKeypointGetTime(lua_State* L)
{
    pushNumber(L, checkPayload<NumberKeypoint>(L, 1, UserdataTag::NumberSequenceKeypoint).time);
    return 1;
}

int numberKeypointGetValue(lua_State* L)
{
    pushNumber(L, checkPayload<NumberKeypoint>(L, 1, UserdataTag::NumberSequenceKeypoint).value);
    return 1;
}

int numberKeypointGetEnvelope(lua_State* L)
{
    pushNumber(L, checkPayload<NumberKeypoint>(L, 1, UserdataTag::NumberSequenceKeypoint).envelope);
    return 1;
}

int numberKeypointEq(lua_State* L)
{
    const NumberKeypoint* a = toPayload<NumberKeypoint>(L, 1, UserdataTag::NumberSequenceKeypoint);
    const NumberKeypoint* b = toPayload<NumberKeypoint>(L, 2, UserdataTag::NumberSequenceKeypoint);
    lua_pushboolean(L, a != nullptr && b != nullptr && *a == *b);
    return 1;
}

int numberKeypointTostring(lua_State* L)
{
    const NumberKeypoint& self = checkPayload<NumberKeypoint>(L, 1, UserdataTag::NumberSequenceKeypoint);
    char text[80]{};
    std::snprintf(text, sizeof(text), "%g %g %g", static_cast<double>(self.time), static_cast<double>(self.value),
                  static_cast<double>(self.envelope));
    lua_pushstring(L, text);
    return 1;
}

// --- The sequences -------------------------------------------------------------

// The stops of a table argument, each a keypoint of `tag`, in the table's order.
template <class Keypoint>
[[nodiscard]] std::vector<Keypoint> keypointsOf(lua_State* L, int index, UserdataTag tag)
{
    std::vector<Keypoint> stops;
    const int count = lua_objlen(L, index);
    if (count < 0 || static_cast<core::usize>(count) > core::MaxSequenceKeypoints)
        raise(L, ENG_TR("script.err.bad_sequence"));
    stops.reserve(static_cast<core::usize>(count));
    for (int at = 1; at <= count; ++at) {
        lua_rawgeti(L, index, at);
        const Keypoint* stop = toPayload<Keypoint>(L, -1, tag);
        if (stop == nullptr)
            raise(L, ENG_TR("script.err.bad_sequence"));
        stops.push_back(*stop);
        lua_pop(L, 1);
    }
    return stops;
}

int colorSequenceNew(lua_State* L)
{
    ColorSequence sequence;
    if (lua_istable(L, 1)) {
        sequence.keypoints = keypointsOf<ColorKeypoint>(L, 1, UserdataTag::ColorSequenceKeypoint);
    }
    else {
        const core::Color3 from = checkColor3(L, 1);
        const core::Color3 to = lua_isnoneornil(L, 2) ? from : checkColor3(L, 2);
        sequence.keypoints = {ColorKeypoint{0.0f, from}, ColorKeypoint{1.0f, to}};
    }
    if (!core::validSequence(sequence.keypoints))
        raise(L, ENG_TR("script.err.bad_sequence"));
    pushColorSequence(L, sequence);
    return 1;
}

int colorSequenceGetKeypoints(lua_State* L)
{
    const ColorSequence& self = checkPayload<ColorSequence>(L, 1, UserdataTag::ColorSequence);
    lua_createtable(L, static_cast<int>(self.keypoints.size()), 0);
    int at = 1;
    for (const ColorKeypoint& stop : self.keypoints) {
        pushPayload(L, UserdataTag::ColorSequenceKeypoint, stop);
        lua_rawseti(L, -2, at++);
    }
    return 1;
}

int colorSequenceEq(lua_State* L)
{
    const ColorSequence* a = toPayload<ColorSequence>(L, 1, UserdataTag::ColorSequence);
    const ColorSequence* b = toPayload<ColorSequence>(L, 2, UserdataTag::ColorSequence);
    lua_pushboolean(L, a != nullptr && b != nullptr && *a == *b);
    return 1;
}

int colorSequenceTostring(lua_State* L)
{
    const ColorSequence& self = checkPayload<ColorSequence>(L, 1, UserdataTag::ColorSequence);
    std::string text;
    for (const ColorKeypoint& stop : self.keypoints) {
        char part[96]{};
        std::snprintf(part, sizeof(part), "%s%g %g %g %g", text.empty() ? "" : " ", static_cast<double>(stop.time),
                      static_cast<double>(stop.value.r), static_cast<double>(stop.value.g),
                      static_cast<double>(stop.value.b));
        text += part;
    }
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

void colorSequenceDtor(lua_State*, void* userdata)
{
    static_cast<ColorSequence*>(userdata)->~ColorSequence();
}

int numberSequenceNew(lua_State* L)
{
    NumberSequence sequence;
    if (lua_istable(L, 1)) {
        sequence.keypoints = keypointsOf<NumberKeypoint>(L, 1, UserdataTag::NumberSequenceKeypoint);
    }
    else {
        const core::f32 from = checkFinite(L, 1);
        const core::f32 to = lua_isnoneornil(L, 2) ? from : checkFinite(L, 2);
        sequence.keypoints = {NumberKeypoint{0.0f, from, 0.0f}, NumberKeypoint{1.0f, to, 0.0f}};
    }
    if (!core::validSequence(sequence.keypoints))
        raise(L, ENG_TR("script.err.bad_sequence"));
    pushNumberSequence(L, sequence);
    return 1;
}

int numberSequenceGetKeypoints(lua_State* L)
{
    const NumberSequence& self = checkPayload<NumberSequence>(L, 1, UserdataTag::NumberSequence);
    lua_createtable(L, static_cast<int>(self.keypoints.size()), 0);
    int at = 1;
    for (const NumberKeypoint& stop : self.keypoints) {
        pushPayload(L, UserdataTag::NumberSequenceKeypoint, stop);
        lua_rawseti(L, -2, at++);
    }
    return 1;
}

int numberSequenceEq(lua_State* L)
{
    const NumberSequence* a = toPayload<NumberSequence>(L, 1, UserdataTag::NumberSequence);
    const NumberSequence* b = toPayload<NumberSequence>(L, 2, UserdataTag::NumberSequence);
    lua_pushboolean(L, a != nullptr && b != nullptr && *a == *b);
    return 1;
}

int numberSequenceTostring(lua_State* L)
{
    const NumberSequence& self = checkPayload<NumberSequence>(L, 1, UserdataTag::NumberSequence);
    std::string text;
    for (const NumberKeypoint& stop : self.keypoints) {
        char part[80]{};
        std::snprintf(part, sizeof(part), "%s%g %g %g", text.empty() ? "" : " ", static_cast<double>(stop.time),
                      static_cast<double>(stop.value), static_cast<double>(stop.envelope));
        text += part;
    }
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

void numberSequenceDtor(lua_State*, void* userdata)
{
    static_cast<NumberSequence*>(userdata)->~NumberSequence();
}

void registerModule(lua_State* L, const char* name, lua_CFunction constructor)
{
    const luaL_Reg constructors[] = {{"new", constructor}, {nullptr, nullptr}};
    luaL_register(L, name, constructors);
    lua_setreadonly(L, -1, true);
    lua_pop(L, 1);
}

} // namespace

void registerSequenceTypes(lua_State* L)
{
    VmContext& ctx = context(L);
    core::AtomTable& atoms = ctx.world->atoms();

    MemberTable& colorStop = ctx.getters[static_cast<core::usize>(UserdataTag::ColorSequenceKeypoint)];
    addMember(colorStop, atoms, "Time", colorKeypointGetTime);
    addMember(colorStop, atoms, "Value", colorKeypointGetValue);
    installTagMetatable(L, UserdataTag::ColorSequenceKeypoint, colorKeypointEq, colorKeypointTostring);
    registerModule(L, "ColorSequenceKeypoint", colorKeypointNew);

    MemberTable& numberStop = ctx.getters[static_cast<core::usize>(UserdataTag::NumberSequenceKeypoint)];
    addMember(numberStop, atoms, "Time", numberKeypointGetTime);
    addMember(numberStop, atoms, "Value", numberKeypointGetValue);
    addMember(numberStop, atoms, "Envelope", numberKeypointGetEnvelope);
    installTagMetatable(L, UserdataTag::NumberSequenceKeypoint, numberKeypointEq, numberKeypointTostring);
    registerModule(L, "NumberSequenceKeypoint", numberKeypointNew);

    // The two that own their stops, so each has a destructor -- registered per
    // tag, which is Luau's shape for it.
    addMember(ctx.getters[static_cast<core::usize>(UserdataTag::ColorSequence)], atoms, "Keypoints",
              colorSequenceGetKeypoints);
    installTagMetatable(L, UserdataTag::ColorSequence, colorSequenceEq, colorSequenceTostring);
    lua_setuserdatadtor(L, static_cast<int>(UserdataTag::ColorSequence), colorSequenceDtor);
    registerModule(L, "ColorSequence", colorSequenceNew);

    addMember(ctx.getters[static_cast<core::usize>(UserdataTag::NumberSequence)], atoms, "Keypoints",
              numberSequenceGetKeypoints);
    installTagMetatable(L, UserdataTag::NumberSequence, numberSequenceEq, numberSequenceTostring);
    lua_setuserdatadtor(L, static_cast<int>(UserdataTag::NumberSequence), numberSequenceDtor);
    registerModule(L, "NumberSequence", numberSequenceNew);
}

void pushColorSequence(lua_State* L, const core::ColorSequence& sequence)
{
    pushPayload(L, UserdataTag::ColorSequence, sequence);
}

void pushNumberSequence(lua_State* L, const core::NumberSequence& sequence)
{
    pushPayload(L, UserdataTag::NumberSequence, sequence);
}

const core::ColorSequence* toColorSequence(lua_State* L, int index) noexcept
{
    return toPayload<ColorSequence>(L, index, UserdataTag::ColorSequence);
}

const core::NumberSequence* toNumberSequence(lua_State* L, int index) noexcept
{
    return toPayload<NumberSequence>(L, index, UserdataTag::NumberSequence);
}

} // namespace engine::script
