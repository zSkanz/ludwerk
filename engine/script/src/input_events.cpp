#include "engine/script/input_events.h"

#include <lua.h>
#include <lualib.h>

#include <array>
#include <new>

#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/datatypes.h"
#include "engine/script/instance_binding.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

// scene's generated enum and class ids, through the include directory
// `engine_scene` exports.
#include "class_descriptors.gen.h"

namespace engine::script {
namespace {

// The payload of an `InputObject` userdata: exactly what the system produced,
// minus the two fields that are the event's rather than the object's. Trivially
// copyable, like every other value type here.
struct InputObjectData
{
    i32 userInputType = 0;
    i32 keyCode = 0;
    core::Vec3 position;
    core::Vec3 delta;
    i32 touchId = 0;
    u32 gamepadId = 0;
};

static_assert(sizeof(InputObjectData) == 40, "the InputObject payload's size is an ABI decision");

[[nodiscard]] const InputObjectData& checkInputObject(lua_State* L, int index)
{
    return *static_cast<InputObjectData*>(luaL_checkudatatagged(L, index, static_cast<int>(UserdataTag::InputObject)));
}

void pushInputObject(lua_State* L, const input::RawInputEvent& event)
{
    void* memory =
        lua_newuserdatataggedwithmetatable(L, sizeof(InputObjectData), static_cast<int>(UserdataTag::InputObject));
    InputObjectData* data = new (memory) InputObjectData{};
    data->userInputType = static_cast<i32>(event.userInputType);
    data->keyCode = event.keyCode;
    data->position = event.position;
    data->delta = event.delta;
    data->touchId = event.touchId;
    data->gamepadId = event.gamepadId;
}

int inputObjectGetGamepadId(lua_State* L)
{
    lua_pushnumber(L, static_cast<double>(checkInputObject(L, 1).gamepadId));
    return 1;
}

int inputObjectGetTouchId(lua_State* L)
{
    lua_pushinteger(L, checkInputObject(L, 1).touchId);
    return 1;
}

int inputObjectGetUserInputType(lua_State* L)
{
    pushEnumItem(L, scene::EnumValue{scene::generated::UserInputTypeEnumId, checkInputObject(L, 1).userInputType});
    return 1;
}

int inputObjectGetKeyCode(lua_State* L)
{
    pushEnumItem(L, scene::EnumValue{scene::generated::KeyCodeEnumId, checkInputObject(L, 1).keyCode});
    return 1;
}

int inputObjectGetPosition(lua_State* L)
{
    pushVector3(L, checkInputObject(L, 1).position);
    return 1;
}

int inputObjectGetDelta(lua_State* L)
{
    pushVector3(L, checkInputObject(L, 1).delta);
    return 1;
}

int inputObjectToString(lua_State* L)
{
    const InputObjectData& data = checkInputObject(L, 1);
    // The two enums by name rather than by number: a `print` in a handler is the
    // first thing anybody does with one of these, and "InputObject(Keyboard,
    // Space)" answers the question a number would not.
    const scene::World& w = *context(L).world;
    const scene::EnumItemDesc* kind = w.enums().findValue(scene::generated::UserInputTypeEnumId, data.userInputType);
    const scene::EnumItemDesc* key = w.enums().findValue(scene::generated::KeyCodeEnumId, data.keyCode);
    lua_pushfstring(L, "InputObject(%s, %s)", kind == nullptr ? "?" : w.atoms().text(kind->name).data(),
                    key == nullptr ? "?" : w.atoms().text(key->name).data());
    return 1;
}

// Which of `InputService`'s events one phase fires. Resolved per drain rather
// than cached, because the descriptor lives in static storage and the lookup is
// a hash probe on a name that is already interned.
[[nodiscard]] const char* eventNameOf(input::RawInputEvent::Phase phase) noexcept
{
    switch (phase) {
    case input::RawInputEvent::Phase::Began:
        return "InputBegan";
    case input::RawInputEvent::Phase::Changed:
        return "InputChanged";
    case input::RawInputEvent::Phase::Ended:
        return "InputEnded";
    }
    return "InputBegan";
}

} // namespace

void registerInputTypes(lua_State* L)
{
    VmContext& ctx = context(L);
    core::AtomTable& atoms = ctx.world->atoms();

    MemberTable& getters = ctx.getters[static_cast<usize>(UserdataTag::InputObject)];
    addMember(getters, atoms, "UserInputType", inputObjectGetUserInputType);
    addMember(getters, atoms, "KeyCode", inputObjectGetKeyCode);
    addMember(getters, atoms, "Position", inputObjectGetPosition);
    addMember(getters, atoms, "Delta", inputObjectGetDelta);
    addMember(getters, atoms, "TouchId", inputObjectGetTouchId);
    addMember(getters, atoms, "GamepadId", inputObjectGetGamepadId);

    // No `__eq`: two snapshots of one press are two facts about one tick, and
    // the bitwise comparison a trivially-copyable payload gives is the right
    // answer for a value type anyway.
    installTagMetatable(L, UserdataTag::InputObject, nullptr, inputObjectToString);
}

void fireInputDeviceEvents(lua_State* L, std::span<const input::DeviceEvent> events)
{
    if (events.empty())
        return;
    auto& ctx = context(L);
    auto& w = *ctx.world;
    const auto serviceClass = w.classes().findId(w.atoms().lookup("InputService"));
    const auto service = w.findFirstChildOfClass(ctx.services->dataModel, serviceClass);
    if (!service.valid())
        return;
    for (const auto& event : events) {
        const char* name = "";
        int arguments = 1;
        switch (event.kind) {
        case input::DeviceEvent::Kind::InputChanged:
            name = "InputDeviceChanged";
            pushEnumItem(
                L, scene::EnumValue{scene::generated::InputDeviceTypeEnumId, static_cast<core::i32>(event.device)});
            break;
        case input::DeviceEvent::Kind::GamepadTypeChanged:
            name = "PreferredGamepadTypeChanged";
            pushEnumItem(L,
                         scene::EnumValue{scene::generated::GamepadTypeEnumId, static_cast<core::i32>(event.family)});
            break;
        case input::DeviceEvent::Kind::GamepadIdChanged:
            name = "PreferredGamepadIdChanged";
            lua_pushnumber(L, static_cast<double>(event.id));
            break;
        case input::DeviceEvent::Kind::Connected:
        case input::DeviceEvent::Kind::Disconnected:
            name = event.kind == input::DeviceEvent::Kind::Connected ? "GamepadConnected" : "GamepadDisconnected";
            lua_pushnumber(L, static_cast<double>(event.id));
            pushEnumItem(L,
                         scene::EnumValue{scene::generated::GamepadTypeEnumId, static_cast<core::i32>(event.family)});
            arguments = 2;
            break;
        }
        const auto* descriptor = w.classes().findEvent(serviceClass, w.atoms().intern(name));
        if (descriptor != nullptr)
            fireInstanceEvent(L, service, descriptor->slot, lua_gettop(L) - arguments + 1, arguments);
        lua_pop(L, arguments);
    }
}

void fireInputEvents(lua_State* L, std::span<const input::RawInputEvent> events)
{
    if (events.empty())
        return;

    VmContext& ctx = context(L);
    scene::World& w = *ctx.world;

    // `InputService` is created on first `GetService` like most services, so a
    // world whose scripts never read input has none -- and firing into nothing
    // is the common case rather than an error.
    const scene::ClassId serviceClass = w.classes().findId(w.atoms().lookup("InputService"));
    if (serviceClass == scene::InvalidClass)
        return;
    const core::InstanceId root = ctx.services->dataModel;
    if (!root.valid())
        return;
    const core::InstanceId service = w.findFirstChildOfClass(root, serviceClass);
    if (!service.valid())
        return;

    for (const input::RawInputEvent& event : events) {
        const scene::EventDesc* descriptor =
            w.classes().findEvent(serviceClass, w.atoms().intern(eventNameOf(event.phase)));
        if (descriptor == nullptr)
            continue;

        pushInputObject(L, event);
        lua_pushboolean(L, event.uiConsumed);
        fireInstanceEvent(L, service, descriptor->slot, lua_gettop(L) - 1, 2);
        lua_pop(L, 2);
    }
}

void fireGestureEvents(lua_State* L, std::span<const input::GestureEvent> events)
{
    if (events.empty())
        return;
    VmContext& ctx = context(L);
    scene::World& w = *ctx.world;
    const scene::ClassId serviceClass = w.classes().findId(w.atoms().lookup("InputService"));
    const core::InstanceId root = ctx.services->dataModel;
    if (serviceClass == scene::InvalidClass || !root.valid())
        return;
    const core::InstanceId service = w.findFirstChildOfClass(root, serviceClass);
    if (!service.valid())
        return;

    const auto fire = [&](const char* name, int count) {
        if (const scene::EventDesc* descriptor = w.classes().findEvent(serviceClass, w.atoms().intern(name));
            descriptor != nullptr)
            fireInstanceEvent(L, service, descriptor->slot, lua_gettop(L) - count + 1, count);
        lua_pop(L, count);
    };
    for (const input::GestureEvent& event : events) {
        switch (event.kind) {
        case input::GestureEvent::Kind::Swipe:
            pushEnumItem(L, scene::EnumValue{w.enums().findId(w.atoms().lookup("SwipeDirection")), event.direction});
            pushVector2(L, event.position);
            lua_pushinteger(L, event.fingers);
            fire("TouchSwiped", 3);
            break;
        case input::GestureEvent::Kind::Tap:
            pushVector2(L, event.position);
            fire("TouchTapped", 1);
            break;
        case input::GestureEvent::Kind::LongPress:
            pushVector2(L, event.position);
            fire("TouchLongPressed", 1);
            break;
        case input::GestureEvent::Kind::Pinch:
            lua_pushnumber(L, static_cast<double>(event.scale));
            pushVector2(L, event.position);
            fire("TouchPinched", 2);
            break;
        case input::GestureEvent::Kind::Pan:
            pushVector2(L, event.delta);
            lua_pushinteger(L, event.fingers);
            fire("TouchPanned", 2);
            break;
        }
    }
}

} // namespace engine::script
