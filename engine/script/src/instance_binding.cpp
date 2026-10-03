#include "engine/script/instance_binding.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/core/finite.h"
#include "engine/core/log.h"
#include "engine/scene/camera_view.h"
#include "engine/scene/pivot.h"
#include "engine/scene/players.h"
#include "engine/scene/ragdoll_build.h"
#include "engine/scene/scene_file.h"
#include "engine/scene/swarm.h"
#include "engine/scene/ui_pages.h"
#include "engine/scene/water.h"
#include "engine/scene/world.h"
#include "engine/script/content_provider.h"
#include "engine/script/datatypes.h"
#include "engine/script/materials.h"
#include "engine/script/modules.h"
#include "engine/script/remote.h"
#include "engine/script/save_service.h"
#include "engine/script/scenes.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

using scene::ClassId;
using scene::World;

[[nodiscard]] World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

// --- Identity ----------------------------------------------------------------

[[nodiscard]] std::string_view className(lua_State* L, core::InstanceId id)
{
    const World& w = world(L);
    const scene::ClassDescriptor* descriptor = w.classes().find(w.classOf(id));
    // A view into the atom table, which is append-only and outlives the VM, so
    // it stays good for as long as an error message needs it.
    return descriptor == nullptr ? std::string_view{"Instance"} : w.atoms().text(descriptor->name);
}

[[noreturn]] void raiseDead(lua_State* L)
{
    raise(L, ENG_TR("script.err.instance_dead"));
}

[[nodiscard]] core::InstanceId liveInstance(lua_State* L, int index)
{
    const core::InstanceId* id = toInstance(L, index);
    if (id == nullptr) {
        // `luaL_checkudatatagged` produces the argument error naming the type it
        // wanted, which reads better than anything assembled here would.
        luaL_checkudatatagged(L, index, static_cast<int>(UserdataTag::Instance));
    }
    if (id == nullptr || !world(L).alive(*id))
        raiseDead(L);
    return *id;
}

// Defined with the other methods, and answered by the dispatch below for a
// destroyed instance as well (G16).
int methodIsAncestorOf(lua_State* L);
int methodIsDescendantOf(lua_State* L);

// --- Property dispatch -------------------------------------------------------

[[noreturn]] void raiseUnknownInstanceMember(lua_State* L, core::InstanceId id, const char* member)
{
    const std::string_view name{member == nullptr ? "" : member};

    // **When the name is a CHILD, say so**: reading one reaches it (ADR 0078),
    // so the only way here with a child's name is ASSIGNING to it, and a child
    // is replaced by parenting another instance, not by assignment.
    //
    // `lookup` and never `intern`: the name comes from a script, and interning
    // it would let a loop of misspellings grow the atom table without bound.
    const World& w = world(L);
    if (!name.empty()) {
        if (const core::NameAtom atom = w.atoms().lookup(name); atom.valid()) {
            if (w.findFirstChild(id, atom).valid()) {
                const core::I18nArg childArgs[] = {
                    {"className", className(L, id)},
                    {"member", name},
                };
                raise(L, ENG_TR("scene.err.child_not_member"), childArgs);
            }
        }
    }

    const core::I18nArg args[] = {
        {"className", className(L, id)},
        {"member", name},
    };
    raise(L, ENG_TR("scene.err.unknown_member"), args);
}

[[noreturn]] void raisePropertyError(lua_State* L, core::TextKey key, core::InstanceId id,
                                     const scene::PropertyDesc& property)
{
    const World& w = world(L);
    const core::I18nArg args[] = {
        {"className", className(L, id)},
        {"property", w.atoms().text(property.name)},
    };
    raise(L, key, args);
}

int instanceIndex(lua_State* L)
{
    // **`Parent` of a destroyed instance is nil** (D206, the owner's decision
    // of 2026-09-28): the one read a dead handle still answers, because
    // `if part.Parent then` is how a game asks whether something is still in
    // the world. Everything else raises, as divergence #25 has it.
    if (const core::InstanceId* dead = toInstance(L, 1); dead != nullptr && !world(L).alive(*dead)) {
        if (const char* key = lua_tostring(L, 2); key != nullptr && std::string_view{key} == "Parent") {
            lua_pushnil(L);
            return 1;
        }
        // And its ancestry, reached as a member as well as called (G16).
        if (const char* key = lua_tostring(L, 2); key != nullptr) {
            if (std::string_view{key} == "IsDescendantOf") {
                lua_pushcfunction(L, methodIsDescendantOf, "IsDescendantOf");
                return 1;
            }
            if (std::string_view{key} == "IsAncestorOf") {
                lua_pushcfunction(L, methodIsAncestorOf, "IsAncestorOf");
                return 1;
            }
        }
    }

    const core::InstanceId id = liveInstance(L, 1);

    int atom = -1;
    const char* key = lua_tostringatom(L, 2, &atom);
    if (key == nullptr)
        raiseUnknownInstanceMember(L, id, key);

    World& w = world(L);
    const core::NameAtom name = context(L).resolve(atom, key);
    const ClassId classId = w.classOf(id);

    if (const scene::PropertyDesc* property = w.classes().findProperty(classId, name)) {
        if (property->get == nullptr)
            raisePropertyError(L, ENG_TR("script.err.not_implemented"), id, *property);
        pushValue(L, property->get(w, id));
        return 1;
    }

    // A method reached without calling it -- `local f = part.Destroy`. It
    // allocates a closure per access, which is why `__namecall` exists and why
    // `part:Destroy()` never comes through here.
    if (const scene::MethodDesc* method = w.classes().findMethod(classId, name)) {
        const auto& implementations = context(L).instanceMethods;
        const auto found = implementations.find(method);
        if (found == implementations.end()) {
            const core::I18nArg args[] = {
                {"className", className(L, id)},
                {"property", w.atoms().text(method->name)},
            };
            raise(L, ENG_TR("script.err.not_implemented"), args);
        }
        // The debug name comes from the ATOM TABLE, not from `key`. Luau stores
        // it as a raw `const char*` unless `LuauManagedDebugNames` is on
        // (`lapi.cpp:792-795`), and `key` points into an interned Luau string
        // that the collector may free while the closure outlives it. The atom
        // table is append-only and outlives the VM, so its text is the one
        // pointer here with the right lifetime.
        lua_pushcfunction(L, found->second, w.atoms().text(method->name).data());
        return 1;
    }

    if (const scene::EventDesc* event = w.classes().findEvent(classId, name)) {
        // The tree's per-member events are enqueued only for an instance
        // somebody listens on (audit E10): reaching the event is listening.
        if (event->name == w.atoms().lookup("DescendantAdded") || event->name == w.atoms().lookup("DescendantRemoving"))
            w.listenForTree(id, scene::World::TreeListen::Descendants);
        else if (event->name == w.atoms().lookup("AncestryChanged"))
            w.listenForTree(id, scene::World::TreeListen::Ancestry);
        // The same object every time, which a script that connects in one place
        // and disconnects in another depends on.
        pushInstanceEvent(L, id, event->slot);
        return 1;
    }

    // A callback is a member too (ADR 0079), and a script reads back the
    // function it assigned.
    if (remoteCallbackGet(L, id, key) || saveCallbackGet(L, id, key) || sceneMemberGet(L, id, key) ||
        contentProviderMemberGet(L, id, key))
        return 1;

    // **Then a child by that name** (ADR 0078, reversing 0061 on the owner's
    // word): `workspace.Baseplate` reaches the first child called Baseplate.
    // A member always wins over a child of the same name, so a part named
    // `Name` never hides the property. `lookup`, never `intern`: the name comes
    // from a script, and a loop of misspellings must not grow the atom table.
    if (const core::NameAtom childName = w.atoms().lookup(key); childName.valid()) {
        if (const core::InstanceId child = w.findFirstChild(id, childName); child.valid()) {
            pushInstance(L, child);
            return 1;
        }
    }

    raiseUnknownInstanceMember(L, id, key);
}

int instanceNewIndex(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);

    int atom = -1;
    const char* key = lua_tostringatom(L, 2, &atom);
    if (key == nullptr)
        raiseUnknownInstanceMember(L, id, key);

    World& w = world(L);
    const core::NameAtom name = context(L).resolve(atom, key);
    const ClassId classId = w.classOf(id);

    const scene::PropertyDesc* property = w.classes().findProperty(classId, name);
    if (property == nullptr) {
        if (remoteCallbackSet(L, id, key, 3) || saveCallbackSet(L, id, key, 3) || sceneMemberSet(L, id, key))
            return 0;
        raiseUnknownInstanceMember(L, id, key);
    }
    if (property->readOnly || property->set == nullptr)
        raisePropertyError(L, ENG_TR("scene.err.read_only_property"), id, *property);
    // **The editor's and the engine's to write** (ADR 0137 §4): a script that
    // could write another's `Source` and enable it would run text the sandbox
    // never loaded. `RunContext` likewise (ADR 0138 §2): the side was fixed
    // when the package was made, and the other side's code is not in it.
    if (property->scriptReadOnly) {
        const bool code = property->name == world(L).atoms().lookup("Source");
        raisePropertyError(
            L, code ? ENG_TR("script.err.property_not_script_writable") : ENG_TR("script.err.property_editor_only"), id,
            *property);
    }

    const std::optional<scene::Value> value = toValue(L, 3, property->type);
    if (!value.has_value())
        raisePropertyError(L, property->errKeyOnInvalidSet, id, *property);

    // An engine-made folder of `GlobalScriptService` keeps its name: the name is
    // how scripts find it, and its place decides what runs (ADR 0105).
    if (name == context(L).wellKnown.name && w.fixed(id)) {
        const core::I18nArg args[] = {{"instance", w.atoms().text(w.name(id))}};
        raise(L, ENG_TR("scene.err.fixed_instance"), args);
    }

    // `Parent` goes through `World::setParent` directly rather than through the
    // generic setter, because a cycle and a destroyed instance are two different
    // refusals with two different keys and the accessor collapses both to
    // `false` (native_accessors.cpp says so at the collapse).
    if (name == context(L).wellKnown.parent && property->type == scene::ValueType::Instance) {
        core::InstanceId target;
        if (const auto* reference = std::get_if<core::InstanceId>(&value.value()))
            target = *reference;

        const core::InstanceId previous = w.parentOf(id);
        if (const std::optional<core::TextKey> refusal = w.setParent(id, target)) {
            const core::I18nArg args[] = {{"instance", w.atoms().text(w.name(id))}};
            raise(L, *refusal, args);
        }
        // `Parent` is a property as well as structure, so a change to it is a
        // change `GetPropertyChangedSignal("Parent")` reports. `setParent` deals
        // in tree links and raises none of that, so the fact is pushed here --
        // equality-filtered like every other property write (§3.1).
        if (w.parentOf(id) != previous)
            w.changes().push(scene::Change{scene::ChangeKind::PropertyChanged, id, {}, name});
        // Who put a screen in `UIService` (D469): the game's own code, or a
        // scene's. Remembered on the screen for the host to speak of.
        if (scene::ScreenGuiComponent* screen = w.screenGuis().find(id); screen != nullptr) {
            const core::InstanceId global = w.findFirstChildOfClass(
                context(L).services->dataModel, w.classes().findId(w.atoms().lookup("GlobalScriptService")));
            const core::InstanceId asking = scriptOfThread(L);
            screen->parentedByGlobal = global.valid() && asking.valid() && w.isAncestorOf(global, asking);
        }
        flushSceneChanges(L);
        return 0;
    }

    switch (w.setProperty(id, name, *value)) {
    case World::SetResult::Changed:
        flushSceneChanges(L);
        return 0;
    case World::SetResult::Unchanged:
        return 0;
    case World::SetResult::ReadOnly:
        raisePropertyError(L, ENG_TR("scene.err.read_only_property"), id, *property);
    case World::SetResult::InvalidValue:
        raisePropertyError(L, property->errKeyOnInvalidSet, id, *property);
    case World::SetResult::UnknownProperty:
        break;
    }
    raiseUnknownInstanceMember(L, id, key);
}

int instanceNamecall(lua_State* L)
{
    // The ancestry questions are answered on a destroyed instance (G16).
    if (const core::InstanceId* dead = toInstance(L, 1); dead != nullptr && !world(L).alive(*dead)) {
        if (const char* asked = lua_namecallatom(L, nullptr); asked != nullptr) {
            const std::string_view name{asked};
            if (name == "IsDescendantOf")
                return methodIsDescendantOf(L);
            if (name == "IsAncestorOf")
                return methodIsAncestorOf(L);
        }
    }
    const core::InstanceId id = liveInstance(L, 1);

    int atom = -1;
    const char* method = lua_namecallatom(L, &atom);
    if (method == nullptr)
        raiseUnknownInstanceMember(L, id, method);

    World& w = world(L);
    const scene::MethodDesc* descriptor = w.classes().findMethod(w.classOf(id), context(L).resolve(atom, method));
    if (descriptor == nullptr)
        raiseUnknownInstanceMember(L, id, method);

    const auto& implementations = context(L).instanceMethods;
    const auto found = implementations.find(descriptor);
    if (found == implementations.end()) {
        const core::I18nArg args[] = {
            {"className", className(L, id)},
            {"property", w.atoms().text(descriptor->name)},
        };
        raise(L, ENG_TR("script.err.not_implemented"), args);
    }
    return found->second(L);
}

int instanceEq(lua_State* L)
{
    const core::InstanceId* a = toInstance(L, 1);
    const core::InstanceId* b = toInstance(L, 2);
    // The generation is what makes this safe across slot reuse: a handle to a
    // destroyed instance compares unequal to a handle to whatever moved into its
    // slot, rather than silently aliasing it.
    lua_pushboolean(L, a != nullptr && b != nullptr && *a == *b);
    return 1;
}

int instanceTostring(lua_State* L)
{
    const core::InstanceId* id = toInstance(L, 1);
    if (id == nullptr || !world(L).alive(*id)) {
        // Deliberately does NOT raise. `tostring` is what a log line and a
        // debugger call, and a print that throws where a handle happens to be
        // dead is worse than a print that says so.
        lua_pushstring(L, "Instance");
        return 1;
    }
    const std::string_view text = world(L).atoms().text(world(L).name(*id));
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// --- Instance methods --------------------------------------------------------

[[nodiscard]] core::NameAtom lookupAtom(lua_State* L, int index)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    // `lookup` rather than `intern`: a query for a name nothing in the world has
    // ever carried is a normal answer, and interning it would let a loop calling
    // `FindFirstChild` on random strings grow the table without bound.
    return world(L).atoms().lookup(std::string_view{text, length});
}

[[nodiscard]] ClassId lookupClass(lua_State* L, int index)
{
    return world(L).classes().findId(lookupAtom(L, index));
}

void pushInstanceArray(lua_State* L, const std::vector<core::InstanceId>& ids)
{
    lua_createtable(L, static_cast<int>(ids.size()), 0);
    for (usize index = 0; index < ids.size(); ++index) {
        pushInstance(L, ids[index]);
        lua_rawseti(L, -2, static_cast<int>(index) + 1);
    }
}

int methodFindFirstChild(lua_State* L)
{
    pushInstance(L, world(L).findFirstChild(liveInstance(L, 1), lookupAtom(L, 2)));
    return 1;
}

int methodFindFirstChildOfClass(lua_State* L)
{
    pushInstance(L, world(L).findFirstChildOfClass(liveInstance(L, 1), lookupClass(L, 2)));
    return 1;
}

int methodFindFirstChildWhichIsA(lua_State* L)
{
    pushInstance(L, world(L).findFirstChildWhichIsA(liveInstance(L, 1), lookupClass(L, 2)));
    return 1;
}

int methodFindFirstAncestor(lua_State* L)
{
    pushInstance(L, world(L).findFirstAncestor(liveInstance(L, 1), lookupAtom(L, 2)));
    return 1;
}

int methodFindFirstAncestorOfClass(lua_State* L)
{
    pushInstance(L, world(L).findFirstAncestorOfClass(liveInstance(L, 1), lookupClass(L, 2)));
    return 1;
}

// The camera a method was called on, where it is drawn from, and the size of
// what the world is drawn into.
struct CameraView
{
    const scene::CameraComponent* camera = nullptr;
    core::CFrameD frame;
    core::Vec2 viewport;
};

[[nodiscard]] CameraView cameraView(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const scene::World& w = world(L);
    CameraView view;
    view.camera = w.cameras().find(id);
    if (view.camera == nullptr)
        raise(L, ENG_TR("script.err.not_a_camera"));
    // Where a render phase put it, when one did: the picture's own camera.
    view.frame = view.camera->presenting ? view.camera->presented : view.camera->cframe;
    view.viewport = w.engineState().viewportSize;
    return view;
}

int methodWorldToViewportPoint(lua_State* L)
{
    const CameraView view = cameraView(L);
    const scene::ViewportPoint point =
        scene::worldToViewport(*view.camera, view.frame, view.viewport, core::toDVec3(checkVector3(L, 2)));
    pushVector2(L, point.pixel);
    lua_pushnumber(L, point.depth);
    lua_pushboolean(L, point.onScreen ? 1 : 0);
    return 3;
}

int methodViewportPointToRay(lua_State* L)
{
    const CameraView view = cameraView(L);
    const scene::ViewRay ray = scene::viewportToRay(*view.camera, view.frame, view.viewport, checkVector2(L, 2));
    pushVector3(L, core::toVec3(ray.origin));
    pushVector3(L, ray.direction);
    return 2;
}

int methodViewportPointToWorld2D(lua_State* L)
{
    const CameraView view = cameraView(L);
    const std::optional<core::Vec2> place =
        scene::viewportToPlane(*view.camera, view.frame, view.viewport, checkVector2(L, 2));
    if (place.has_value())
        pushVector2(L, *place);
    else
        lua_pushnil(L);
    return 1;
}

int methodGetFullName(lua_State* L)
{
    const scene::World& authored = world(L);
    const scene::ClassId dataModel = authored.classes().findId(authored.atoms().lookup("DataModel"));
    std::vector<std::string_view> names;
    for (core::InstanceId at = liveInstance(L, 1); at.valid(); at = authored.parentOf(at)) {
        // Every path starts below the data model, which is not part of one --
        // unless it is the data model that was asked.
        if (!names.empty() && !authored.parentOf(at).valid() && authored.classOf(at) == dataModel)
            break;
        names.push_back(authored.atoms().text(authored.name(at)));
    }
    std::string path;
    for (auto name = names.rbegin(); name != names.rend(); ++name) {
        if (!path.empty())
            path += '.';
        path += *name;
    }
    lua_pushlstring(L, path.data(), path.size());
    return 1;
}

int methodGetChildren(lua_State* L)
{
    std::vector<core::InstanceId> children;
    world(L).collectChildren(liveInstance(L, 1), children);
    // A fresh array the caller owns, which is what makes destroying while
    // iterating safe (api-design.md §3.1).
    pushInstanceArray(L, children);
    return 1;
}

int methodGetDescendants(lua_State* L)
{
    std::vector<core::InstanceId> descendants;
    world(L).collectDescendants(liveInstance(L, 1), descendants);
    pushInstanceArray(L, descendants);
    return 1;
}

int methodIsA(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    // A string naming no class answers false rather than raising: the point of
    // `IsA` is to test names you do not trust.
    const ClassId base = lookupClass(L, 2);
    lua_pushboolean(L, base != scene::InvalidClass && world(L).isA(id, base));
    return 1;
}

// **Ancestry answers for a destroyed instance too** (G16): a thing the world
// let go of is the descendant and the ancestor of nothing, and "is it still
// there?" -- asked either way round -- must not need a `pcall`.
[[nodiscard]] bool related(lua_State* L, bool ancestorFirst)
{
    const core::InstanceId* self = toInstance(L, 1);
    if (self == nullptr)
        luaL_checkudatatagged(L, 1, static_cast<int>(UserdataTag::Instance));
    const core::InstanceId* other = toInstance(L, 2);
    if (other == nullptr)
        luaL_checkudatatagged(L, 2, static_cast<int>(UserdataTag::Instance));
    const World& w = world(L);
    if (self == nullptr || other == nullptr || !w.alive(*self) || !w.alive(*other))
        return false;
    return ancestorFirst ? w.isAncestorOf(*self, *other) : w.isAncestorOf(*other, *self);
}

int methodIsAncestorOf(lua_State* L)
{
    lua_pushboolean(L, related(L, true));
    return 1;
}

int methodIsDescendantOf(lua_State* L)
{
    lua_pushboolean(L, related(L, false));
    return 1;
}

int methodClone(lua_State* L)
{
    pushInstance(L, world(L).clone(liveInstance(L, 1)));
    flushSceneChanges(L);
    return 1;
}

int methodDestroy(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    World& w = world(L);
    if (w.fixed(id)) {
        const core::I18nArg args[] = {{"instance", w.atoms().text(w.name(id))}};
        raise(L, ENG_TR("scene.err.fixed_instance"), args);
    }

    // The subtree first, because `destroy` is synchronous and the descendants
    // are gone from the tree by the time it returns -- there is no walking down
    // to them afterwards.
    std::vector<core::InstanceId> subtree;
    subtree.push_back(id);
    w.collectDescendants(id, subtree);

    if (!w.destroy(id))
        return 0;

    // Enqueue `Destroying` FIRST -- which is what capturing the connection list
    // now means -- and only then close the other signals. api-design.md §3.1
    // spells the order out, and it is what makes a `ChildAdded` queued earlier
    // in the same frame invoke nothing while `Destroying` still runs.
    flushSceneChanges(L);
    for (const core::InstanceId member : subtree)
        closeInstanceSignalsExceptDestroying(L, member);
    return 0;
}

// The attribute domain (api-design.md §2.2). Inferred rather than expected,
// because `SetAttribute` takes a union and the caller's value is what names the
// member of it.
[[nodiscard]] std::optional<scene::Value> toAttributeValue(lua_State* L, int index)
{
    switch (lua_type(L, index)) {
    case LUA_TNIL:
    case LUA_TNONE:
        return scene::Value{};
    case LUA_TBOOLEAN:
        return scene::Value{lua_toboolean(L, index) != 0};
    case LUA_TNUMBER:
        return scene::Value{lua_tonumber(L, index)};
    case LUA_TSTRING: {
        size_t length = 0;
        const char* text = lua_tolstring(L, index, &length);
        return scene::Value{std::string(text, length)};
    }
    case LUA_TVECTOR:
        return toValue(L, index, scene::ValueType::Vector3);
    default:
        break;
    }

    // The userdata half of the domain, tried in turn because a tag test is a
    // pointer comparison and there is no dispatch table from a tag to a
    // `ValueType`. Every alternative `AttributeValue` names in the IDL has a
    // line here, and the conformance suite round-trips one of each -- which is
    // what says the two lists are the same list.
    for (const scene::ValueType candidate :
         {scene::ValueType::CFrame, scene::ValueType::Color3, scene::ValueType::Vector2, scene::ValueType::UDim,
          scene::ValueType::UDim2, scene::ValueType::Rect, scene::ValueType::ColorSequence,
          scene::ValueType::NumberSequence}) {
        if (std::optional<scene::Value> value = toValue(L, index, candidate))
            return value;
    }
    // A table, an Instance or a function: outside the domain, and the caller
    // raises `scene.err.attribute_type` leaving any previous value in place.
    return std::nullopt;
}

[[nodiscard]] core::NameAtom checkAttributeName(lua_State* L, int index)
{
    // `luaL_checklstring` accepts a number and coerces it, which would make
    // `AddTag(1)` a tag called "1" rather than the error api-design.md §2.2
    // says it is. The type test has to come first.
    if (lua_type(L, index) != LUA_TSTRING)
        luaL_typeerrorL(L, index, "string");

    size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    if (length == 0) {
        const core::I18nArg args[] = {{"name", std::string_view{""}}};
        raise(L, ENG_TR("scene.err.invalid_name"), args);
    }
    return world(L).atoms().intern(std::string_view{text, length});
}

int methodGetAttribute(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    pushValue(L, world(L).getAttribute(id, lookupAtom(L, 2)));
    return 1;
}

int methodSetAttribute(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::NameAtom name = checkAttributeName(L, 2);

    const std::optional<scene::Value> value = toAttributeValue(L, 3);
    if (!value.has_value() || !world(L).setAttribute(id, name, *value)) {
        const core::I18nArg args[] = {
            {"attribute", world(L).atoms().text(name)},
            {"valueType", std::string_view{luaL_typename(L, 3)}},
        };
        raise(L, ENG_TR("scene.err.attribute_type"), args);
    }
    flushSceneChanges(L);
    return 0;
}

int methodGetPropertyChangedSignal(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    World& w = world(L);
    const core::NameAtom name = lookupAtom(L, 2);

    // A name the class does not have raises, and the key says where attributes
    // are watched instead -- which is the mistake this call actually attracts.
    if (w.classes().findProperty(w.classOf(id), name) == nullptr) {
        size_t length = 0;
        const char* text = luaL_checklstring(L, 2, &length);
        const core::I18nArg args[] = {
            {"className", className(L, id)},
            {"property", std::string_view{text, length}},
        };
        raise(L, ENG_TR("scene.err.unknown_property"), args);
    }

    pushPropertyChangedSignal(L, id, name);
    return 1;
}

int methodGetAttributeChangedSignal(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    // Any name is accepted: an attribute that has never been set is a reasonable
    // thing to wait for, so this interns rather than looking up.
    pushAttributeChangedSignal(L, id, checkAttributeName(L, 2));
    return 1;
}

int methodGetAttributes(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    scene::AttributeMap attributes;
    world(L).collectAttributes(id, attributes);

    lua_createtable(L, 0, static_cast<int>(attributes.size()));
    for (const auto& [name, value] : attributes) {
        const std::string_view text = world(L).atoms().text(name);
        lua_pushlstring(L, text.data(), text.size());
        pushValue(L, value);
        lua_rawset(L, -3);
    }
    return 1;
}

[[nodiscard]] core::NameAtom checkTagName(lua_State* L, int index)
{
    return checkAttributeName(L, index);
}

int methodAddTag(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    world(L).addTag(id, checkTagName(L, 2));
    flushSceneChanges(L);
    return 0;
}

int methodRemoveTag(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    // Looked up, not interned: a tag nothing ever carried is nothing to
    // remove, and nothing for the table that never frees (audit S11).
    if (lua_type(L, 2) != LUA_TSTRING)
        luaL_typeerrorL(L, 2, "string");
    world(L).removeTag(id, lookupAtom(L, 2));
    flushSceneChanges(L);
    return 0;
}

int methodHasTag(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    lua_pushboolean(L, world(L).hasTag(id, lookupAtom(L, 2)));
    return 1;
}

int methodGetTags(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    scene::TagSet tags;
    world(L).collectTags(id, tags);

    lua_createtable(L, static_cast<int>(tags.size()), 0);
    for (usize index = 0; index < tags.size(); ++index) {
        const std::string_view text = world(L).atoms().text(tags[index]);
        lua_pushlstring(L, text.data(), text.size());
        lua_rawseti(L, -2, static_cast<int>(index) + 1);
    }
    return 1;
}

// --- PVInstance ---------------------------------------------------------------
//
// `GetPivot` and `PivotTo` live on the abstract base rather than on `Model`,
// which is where they were through M4. Three things had gone wrong with that,
// and only the third is about where the methods are declared:
//
//   * **`PivotOffset` did not exist**, so a pivot was always an object's centre
//     and `Model:PivotTo(cf)` was `PrimaryPart.CFrame = cf` -- the deprecated
//     call the pivot API exists to replace, reimplemented under the new name. It
//     passed its tests and could not hinge a door.
//   * **The fallback said one thing and did another.** The comment read "the
//     centre of the extents box" and the code averaged part positions, which is
//     a different point whenever parts differ in size. The box is computed here
//     now, by the same walk `GetExtentsSize` uses.
//   * **Generic code had to branch on class.** Anything positional can take
//     `obj:PivotTo(cf)` now, and `obj:IsA("PVInstance")` is the question that
//     asks whether it can -- which needs `PVInstance` to be a real class, and is
//     why it is one.

// The four of these moved to `engine/scene/src/pivot.cpp` so that everything
// below `script` can ask where a model's middle is -- `Model.Scale` scales about
// it, and an editor gizmo stands on it. See `engine/scene/pivot.h`.
using scene::pivotBase;
using scene::pivotOf;
using scene::pivotOffsetOf;
using scene::worldExtents;

int methodGetPivot(lua_State* L)
{
    pushCFrame(L, pivotOf(world(L), liveInstance(L, 1)));
    return 1;
}

// `GetStamp()` -- the stamp a linked copy was placed from, as its content path,
// or nil (ADR 0155 §7). Its parameters are its attributes.
int methodGetStamp(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    World& w = world(L);
    const core::NameAtom mark = w.stampOf(id);
    if (!mark.valid()) {
        lua_pushnil(L);
        return 1;
    }
    const std::string_view text = w.atoms().text(mark);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

int methodPivotTo(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    World& w = world(L);
    const core::CFrameD target = checkCFrame(L, 2);
    // One answer below the VM, which the editor's placement uses too: exact
    // enough to call every frame, and a no-op onto the pivot it already has
    // (`scene::pivotTo`, D512).
    scene::pivotTo(w, id, target);
    flushSceneChanges(L);
    return 0;
}

// --- Model -------------------------------------------------------------------

int methodGetExtentsSize(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    World& w = world(L);

    std::vector<core::InstanceId> descendants;
    w.collectDescendants(id, descendants);

    bool any = false;
    core::DVec3 minimum;
    core::DVec3 maximum;
    for (const core::InstanceId descendant : descendants) {
        const scene::PartComponent* part = w.parts().find(descendant);
        if (part == nullptr)
            continue;

        // The standard OBB-to-AABB bound: the world half-extent along axis i is
        // the sum over the part's own axes j of |R(i, j)| * half_j. `Mat3` is
        // stored column-major as `m[col][row]`, so `m[j][i]` is R(i, j).
        const core::Mat3& r = part->cframe.rotation;
        const f64 half[3] = {
            static_cast<f64>(part->size.x) * 0.5,
            static_cast<f64>(part->size.y) * 0.5,
            static_cast<f64>(part->size.z) * 0.5,
        };
        f64 extents[3] = {0.0, 0.0, 0.0};
        for (int world = 0; world < 3; ++world) {
            for (int local = 0; local < 3; ++local)
                extents[world] += std::abs(static_cast<f64>(r.m[local][world])) * half[local];
        }
        const core::DVec3 extent{extents[0], extents[1], extents[2]};

        const core::DVec3 low = part->cframe.position - extent;
        const core::DVec3 high = part->cframe.position + extent;
        if (!any) {
            minimum = low;
            maximum = high;
            any = true;
            continue;
        }
        minimum = core::DVec3{std::min(minimum.x, low.x), std::min(minimum.y, low.y), std::min(minimum.z, low.z)};
        maximum = core::DVec3{std::max(maximum.x, high.x), std::max(maximum.y, high.y), std::max(maximum.z, high.z)};
    }

    // A model with no parts has no size, and zero is the honest answer rather
    // than an inverted box.
    pushVector3(L, any ? core::toVec3(maximum - minimum) : core::Vec3{});
    return 1;
}

// --- Instance.new ------------------------------------------------------------

// `Instance.stamp(name[, linked])` -- a prefab, placed by code (ADR 0051).
//
// **The same two things the editor's menu offers**, because they are the same
// two things: a LINKED instance inherits from its file and changes with it, and
// a copy is its own from the first frame. A game that spawns forty lamp posts
// wants the first; one that spawns a starting point it is about to rebuild
// wants the second.
//
// Returns the placed instance, unparented, exactly as `Instance.new` does. The
// caller parents it, which is what makes `Instance.stamp("lantern").Parent =
// workspace` read like every other line of this API.
//
// **The stamps come from the host**, through the same source a scene load uses.
// A VM with none -- a conformance run, a test fixture -- raises rather than
// pretending: a prefab that silently arrived empty would be a bug shaped like
// content.
int instanceStamp(lua_State* L)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    // `"lantern-post"` is `stamps/lantern-post.stamp.json`, as the editor reads
    // it and as the error text says (B12).
    const std::string name = scene::normalizeStampPath(std::string_view(text, length));
    const bool linked = lua_isnoneornil(L, 2) || lua_toboolean(L, 2) != 0;
    if (!lua_isnoneornil(L, 3))
        luaL_checktype(L, 3, LUA_TTABLE);

    VmContext& ctx = context(L);
    if (!ctx.stamps) {
        const core::I18nArg args[] = {{"name", std::string_view{name}}};
        raise(L, ENG_TR("script.err.no_stamp_source"), args);
    }

    // Preloaded, or read now (ADR 0155 §10).
    const auto preloaded = ctx.preloadedStamps.find(name);
    const std::optional<std::string> source =
        preloaded != ctx.preloadedStamps.end() ? std::optional<std::string>(preloaded->second) : ctx.stamps(name);
    if (!source.has_value()) {
        const core::I18nArg args[] = {{"name", std::string_view{name}}};
        raise(L, ENG_TR("script.err.stamp_not_found"), args);
    }

    World& w = world(L);
    scene::SceneIoReport report;
    // Unparented, like `Instance.new`. A stamp's own internal references still
    // resolve, because `readStamp` resolves them against the placed root.
    // Through the host's source for every other stamp it names: the stamps it
    // holds, and a variant's base (ADR 0155).
    const core::InstanceId placed = scene::readStamp(w, *source, core::InstanceId{}, name, &report, &ctx.stamps);
    if (!placed.valid()) {
        const core::I18nArg args[] = {{"name", std::string_view{name}}};
        raise(L, ENG_TR("script.err.stamp_not_found"), args);
    }
    // **A stamp that reaches itself is refused, by name** (ADR 0155 §3).
    if (report.stampCycles > 0) {
        (void)w.destroy(placed);
        const core::I18nArg args[] = {{"chain", std::string_view{report.stampCycle}}};
        raise(L, ENG_TR("script.err.stamp_cycle"), args);
    }

    // **Its parameters, before anything else runs on it** (ADR 0155 §7):
    // each one the stamp declares, checked against its declaration and refused
    // -- never clamped -- with the copy left unbuilt.
    if (lua_istable(L, 3)) {
        const std::vector<scene::StampParameter> declared = scene::stampParametersOf(w, placed);
        lua_pushnil(L);
        while (lua_next(L, 3) != 0) {
            size_t keyLength = 0;
            const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tolstring(L, -2, &keyLength) : nullptr;
            const std::string_view parameterName =
                key != nullptr ? std::string_view(key, keyLength) : std::string_view{};
            const auto found = std::find_if(declared.begin(), declared.end(), [&](const scene::StampParameter& each) {
                return each.name == parameterName;
            });
            if (found == declared.end()) {
                (void)w.destroy(placed);
                const core::I18nArg args[] = {{"stamp", std::string_view{name}}, {"parameter", parameterName}};
                raise(L, ENG_TR("script.err.stamp_parameter_unknown"), args);
            }
            const std::optional<scene::Value> value = toAttributeValue(L, -1);
            const std::string_view refusal =
                value.has_value() ? scene::stampParameterRefusal(*found, *value) : std::string_view{"type"};
            if (!refusal.empty()) {
                (void)w.destroy(placed);
                const core::I18nArg args[] = {{"stamp", std::string_view{name}}, {"parameter", parameterName}};
                if (refusal == "range")
                    raise(L, ENG_TR("script.err.stamp_parameter_range"), args);
                if (refusal == "choice")
                    raise(L, ENG_TR("script.err.stamp_parameter_choice"), args);
                raise(L, ENG_TR("script.err.stamp_parameter_type"), args);
            }
            (void)w.setAttribute(placed, w.atoms().intern(parameterName), *value);
            lua_pop(L, 1);
        }
    }

    // **Its parts built from its parameters, before the caller has it** (ADR
    // 0155 §8).
    constructStamp(L, placed);

    if (!linked)
        w.setStamp(placed, core::NameAtom{});
    // **Which stamp, and where in it**, linked or not (ADR 0138 §6): what a
    // replica reads the stamp's client and shared scripts from its own package
    // by, since the authority never sends a script.
    (void)w.numberOrigins(placed, w.atoms().intern("stamp:" + name), 0);

    pushInstance(L, placed);
    return 1;
}

int instanceNew(lua_State* L)
{
    World& w = world(L);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    const std::string_view requested{text, length};

    const ClassId classId = w.classes().findId(w.atoms().lookup(requested));
    const scene::ClassDescriptor* descriptor = w.classes().find(classId);
    if (descriptor == nullptr) {
        const core::I18nArg args[] = {{"className", requested}};
        raise(L, ENG_TR("scene.err.unknown_class"), args);
    }

    // Abstract, service and not-creatable are three reasons and one message: the
    // tag names which, so the reader is told what is actually wrong rather than
    // "cannot create".
    const char* tag = nullptr;
    if (hasFlag(descriptor->flags, scene::ClassFlags::Abstract))
        tag = "Abstract";
    else if (hasFlag(descriptor->flags, scene::ClassFlags::Service))
        tag = "Service";
    else if (hasFlag(descriptor->flags, scene::ClassFlags::NotCreatable))
        tag = "NotCreatable";

    if (tag != nullptr) {
        const core::I18nArg args[] = {{"className", requested}, {"classTag", std::string_view{tag}}};
        raise(L, ENG_TR("scene.err.not_creatable"), args);
    }

    pushInstance(L, w.create(classId));
    return 1;
}

// --- Physics (M5) ------------------------------------------------------------

int methodApplyImpulse(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 impulse = checkVector3(L, 2);
    // Finite, or refused (audit E3): the 2D one always checked.
    if (!core::isFinite(impulse))
        raise(L, ENG_TR("script.err.not_finite"));

    // Accumulated into the component and applied by the mirror at the start of
    // the next tick. A script may run at any point in the frame and the solver
    // may not be interrupted -- and summing impulses is exactly what applying
    // them one after another would do anyway.
    if (scene::RigidBodyComponent* body = world(L).rigidBodies().find(id); body != nullptr)
        body->pendingImpulse = body->pendingImpulse + impulse;
    return 0;
}

// --- Swarm (ADR 0156) ------------------------------------------------------------

// The swarm a method was called on.
[[nodiscard]] scene::SwarmComponent& swarmOf(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    scene::SwarmComponent* swarm = world(L).swarms().find(id);
    if (swarm == nullptr)
        raise(L, ENG_TR("script.err.instance_dead"));
    return *swarm;
}

// A number argument that is finite, or an error.
[[nodiscard]] double checkFinite(lua_State* L, int index)
{
    const double value = luaL_checknumber(L, index);
    if (!std::isfinite(value))
        raise(L, ENG_TR("script.err.not_finite"));
    return value;
}

[[nodiscard]] core::Vec3 checkFiniteVector(lua_State* L, int index)
{
    const core::Vec3 value = checkVector3(L, index);
    if (!core::isFinite(value))
        raise(L, ENG_TR("script.err.not_finite"));
    return value;
}

[[nodiscard]] core::u32 checkAgent(lua_State* L, int index)
{
    const double agent = checkFinite(L, index);
    return agent >= 1.0 && agent < 4294967296.0 ? static_cast<core::u32>(agent) : 0u;
}

// `AddAgent(body, settings?)`: an agent standing where the body is.
int methodSwarmAddAgent(lua_State* L)
{
    scene::SwarmComponent& swarm = swarmOf(L);
    const core::InstanceId body = checkInstance(L, 2);
    const scene::PartComponent* part = world(L).parts().find(body);
    if (part == nullptr)
        raise(L, ENG_TR("script.err.swarm_body_not_part"));
    scene::SwarmAgentSettings settings;
    if (lua_istable(L, 3)) {
        const auto number = [L](const char* name, f32& out) {
            lua_getfield(L, 3, name);
            if (lua_isnumber(L, -1)) {
                const double value = lua_tonumber(L, -1);
                if (!std::isfinite(value))
                    raise(L, ENG_TR("script.err.not_finite"));
                out = static_cast<f32>(value);
            }
            lua_pop(L, 1);
        };
        const auto flag = [L](const char* name, bool& out) {
            lua_getfield(L, 3, name);
            if (lua_isboolean(L, -1))
                out = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        };
        number("Radius", settings.radius);
        number("Height", settings.height);
        number("Speed", settings.speed);
        number("FloatHeight", settings.floatHeight);
        flag("Floats", settings.floats);
        flag("Climbs", settings.climbs);
    }
    // It starts where its body stands. The swarm places the body by its
    // `CFrame`, so the body's origin is the agent's feet.
    lua_pushnumber(L, static_cast<double>(scene::addSwarmAgent(swarm, body, part->cframe.position, settings)));
    return 1;
}

int methodSwarmRemoveAgent(lua_State* L)
{
    scene::SwarmComponent& swarm = swarmOf(L);
    (void)scene::removeSwarmAgent(swarm, checkAgent(L, 2));
    return 0;
}

int methodSwarmSetAgentSpeed(lua_State* L)
{
    scene::SwarmComponent& swarm = swarmOf(L);
    scene::SwarmAgent* agent = scene::swarmAgent(swarm, checkAgent(L, 2));
    const double speed = checkFinite(L, 3);
    if (agent != nullptr)
        agent->speed = static_cast<f32>(std::max(speed, 0.0));
    return 0;
}

int methodSwarmSetAgentPosition(lua_State* L)
{
    scene::SwarmComponent& swarm = swarmOf(L);
    scene::SwarmAgent* agent = scene::swarmAgent(swarm, checkAgent(L, 2));
    const core::Vec3 position = checkFiniteVector(L, 3);
    if (agent != nullptr) {
        agent->position =
            core::DVec3{static_cast<f64>(position.x), static_cast<f64>(position.y), static_cast<f64>(position.z)};
        agent->verticalSpeed = 0.0f;
        agent->groundX = 1.0e30;
        swarm.gridValid = false;
    }
    return 0;
}

int methodSwarmPush(lua_State* L)
{
    scene::SwarmComponent& swarm = swarmOf(L);
    scene::SwarmAgent* agent = scene::swarmAgent(swarm, checkAgent(L, 2));
    const core::Vec3 velocity = checkFiniteVector(L, 3);
    if (agent != nullptr) {
        agent->pushX += velocity.x;
        agent->pushZ += velocity.z;
    }
    return 0;
}

int methodSwarmGetAgentPosition(lua_State* L)
{
    scene::SwarmComponent& swarm = swarmOf(L);
    const scene::SwarmAgent* agent = scene::swarmAgent(swarm, checkAgent(L, 2));
    if (agent == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    pushVector3(L, core::Vec3{static_cast<f32>(agent->position.x), static_cast<f32>(agent->position.y),
                              static_cast<f32>(agent->position.z)});
    return 1;
}

int methodSwarmGetAgents(lua_State* L)
{
    const scene::SwarmComponent& swarm = swarmOf(L);
    lua_createtable(L, static_cast<int>(swarm.agents.size() - swarm.free.size()), 0);
    int written = 0;
    for (usize slot = 0; slot < swarm.agents.size(); ++slot) {
        if (!swarm.agents[slot].alive)
            continue;
        lua_pushnumber(L, static_cast<double>(slot + 1));
        lua_rawseti(L, -2, ++written);
    }
    return 1;
}

int methodSwarmGetPositions(lua_State* L)
{
    const scene::SwarmComponent& swarm = swarmOf(L);
    lua_createtable(L, static_cast<int>(swarm.agents.size() - swarm.free.size()), 0);
    int written = 0;
    for (const scene::SwarmAgent& agent : swarm.agents) {
        if (!agent.alive)
            continue;
        pushVector3(L, core::Vec3{static_cast<f32>(agent.position.x), static_cast<f32>(agent.position.y),
                                  static_cast<f32>(agent.position.z)});
        lua_rawseti(L, -2, ++written);
    }
    return 1;
}

// `QueryRadius(centre, radius, into?, flat?)`: into `into` when given --
// emptied first, so a weapon asking every tick allocates nothing -- or a new
// table.
int methodSwarmQueryRadius(lua_State* L)
{
    scene::SwarmComponent& swarm = swarmOf(L);
    const core::Vec3 centre = checkFiniteVector(L, 2);
    const double radius = checkFinite(L, 3);
    const bool reuse = lua_istable(L, 4);
    const bool flat = lua_toboolean(L, 5) != 0;
    static thread_local std::vector<core::u32> found;
    found.clear();
    scene::querySwarmRadius(
        swarm, core::DVec3{static_cast<f64>(centre.x), static_cast<f64>(centre.y), static_cast<f64>(centre.z)},
        std::max(radius, 0.0), flat, found);
    int table = 0;
    int previous = 0;
    if (reuse) {
        table = 4;
        previous = static_cast<int>(lua_objlen(L, 4));
    }
    else {
        lua_createtable(L, static_cast<int>(found.size()), 0);
        table = lua_gettop(L);
    }
    for (usize index = 0; index < found.size(); ++index) {
        lua_pushnumber(L, static_cast<double>(found[index]));
        lua_rawseti(L, table, static_cast<int>(index) + 1);
    }
    for (int index = static_cast<int>(found.size()) + 1; index <= previous; ++index) {
        lua_pushnil(L);
        lua_rawseti(L, table, index);
    }
    lua_pushvalue(L, table);
    return 1;
}

int methodSwarmAddObstacle(lua_State* L)
{
    scene::SwarmComponent& swarm = swarmOf(L);
    const core::Vec3 position = checkFiniteVector(L, 2);
    const double radius = checkFinite(L, 3);
    swarm.obstacles.push_back(scene::SwarmObstacle{static_cast<f64>(position.x), static_cast<f64>(position.z),
                                                   static_cast<f32>(std::max(radius, 0.0))});
    return 0;
}

int methodSwarmClearObstacles(lua_State* L)
{
    swarmOf(L).obstacles.clear();
    return 0;
}

// **A push at a point, and a twist** (ADR 0118): queued on the part, as
// `ApplyImpulse` is, and applied at the next tick.
int methodApplyImpulseAtPosition(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 impulse = checkVector3(L, 2);
    const core::Vec3 position = checkVector3(L, 3);
    if (!std::isfinite(impulse.x) || !std::isfinite(impulse.y) || !std::isfinite(impulse.z))
        luaL_argerror(L, 2, "a finite impulse");
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
        luaL_argerror(L, 3, "a finite position");
    scene::World& w = world(L);
    scene::RigidBodyComponent* body = w.rigidBodies().find(id);
    const scene::PartComponent* part = w.parts().find(id);
    if (body == nullptr || part == nullptr)
        return 0;
    // About the centre, where the mirror applies a push: off it, a turn too.
    const core::Vec3 arm = core::toVec3(core::toDVec3(position) - part->cframe.position);
    body->pendingImpulse = body->pendingImpulse + impulse;
    body->pendingAngularImpulse = body->pendingAngularImpulse + core::cross(arm, impulse);
    return 0;
}

int methodApplyAngularImpulse(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 impulse = checkVector3(L, 2);
    if (!std::isfinite(impulse.x) || !std::isfinite(impulse.y) || !std::isfinite(impulse.z))
        luaL_argerror(L, 2, "a finite impulse");
    if (scene::RigidBodyComponent* body = world(L).rigidBodies().find(id); body != nullptr)
        body->pendingAngularImpulse = body->pendingAngularImpulse + impulse;
    return 0;
}

// What a joint carried over the last tick (ADR 0127, N3): written into the
// component by the mirror after each step, read here.
int methodConstraintGetForce(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const scene::ConstraintComponent* constraint = world(L).constraints().find(id);
    lua_pushnumber(L, constraint != nullptr ? static_cast<double>(constraint->lastForce) : 0.0);
    return 1;
}

int methodConstraintGetTorque(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const scene::ConstraintComponent* constraint = world(L).constraints().find(id);
    lua_pushnumber(L, constraint != nullptr ? static_cast<double>(constraint->lastTorque) : 0.0);
    return 1;
}

// And what its own motor used, which is not what it bore.
int methodConstraintGetMotorForce(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const scene::ConstraintComponent* constraint = world(L).constraints().find(id);
    lua_pushnumber(L, constraint != nullptr ? static_cast<double>(constraint->lastMotorForce) : 0.0);
    return 1;
}

int methodConstraintGetMotorTorque(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const scene::ConstraintComponent* constraint = world(L).constraints().find(id);
    lua_pushnumber(L, constraint != nullptr ? static_cast<double>(constraint->lastMotorTorque) : 0.0);
    return 1;
}

// --- Where it is drawn (ADR 0136) ------------------------------------------------

// **This frame's drawn place in a render phase; the simulated one anywhere
// else**, so what a tick reads never depends on when a frame happened (R10).
int methodGetRenderCFrame(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const scene::World& w = world(L);
    ServiceState& state = *context(L).services;
    script::DrawnKind kind = script::DrawnKind::Part;
    core::CFrameD simulated;
    if (const scene::PartComponent* part = w.parts().find(id)) {
        simulated = part->cframe;
    }
    else if (const scene::AttachmentComponent* attachment = w.attachments().find(id)) {
        kind = script::DrawnKind::Attachment;
        simulated = attachment->worldCFrame;
    }
    else if (const scene::CameraComponent* camera = w.cameras().find(id)) {
        kind = script::DrawnKind::Camera;
        simulated = camera->cframe;
        // Presented this phase: drawn as written, whatever the frame's poses
        // remembered before the write.
        if (w.engineState().renderPhase && camera->presenting) {
            pushCFrame(L, camera->presented);
            return 1;
        }
    }
    else {
        return 0;
    }
    if (w.engineState().renderPhase) {
        core::CFrameD drawn;
        if (state.drawnPoses.pose != nullptr && state.drawnPoses.pose(state.drawnPoses.user, id, kind, drawn)) {
            pushCFrame(L, drawn);
            return 1;
        }
    }
    else if (state.renderPhasesRun && state.scenes.developer) {
        // With a window, a simulation phase asking where a thing is drawn has
        // almost certainly mistaken the phase: said once, for each script.
        const core::InstanceId script = scriptOfThread(L);
        if (std::find(state.warnedRenderCFrame.begin(), state.warnedRenderCFrame.end(), script) ==
            state.warnedRenderCFrame.end()) {
            state.warnedRenderCFrame.push_back(script);
            const std::string name = w.alive(script) ? std::string(w.atoms().text(w.name(script))) : std::string{};
            const core::I18nArg args[] = {{"script", std::string_view{name}}};
            core::log(core::LogLevel::Warn, ENG_TR("script.warn.render_cframe_in_simulation"), args);
        }
    }
    pushCFrame(L, simulated);
    return 1;
}

// --- Water (ADR 0118) ------------------------------------------------------------

// The surface above a column now, or nil outside this water.
int methodWaterGetHeightAt(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 at = checkVector3(L, 2);
    const scene::World& w = world(L);
    const auto x = static_cast<double>(at.x);
    const auto z = static_cast<double>(at.z);
    const scene::WaterHere here =
        std::isfinite(x) && std::isfinite(z) ? scene::waterHere(w, id, x, z) : scene::WaterHere{};
    if (!here.covered) {
        lua_pushnil(L);
        return 1;
    }
    // The waves about the still surface there -- a river's, where it descends,
    // is as high as its course is.
    const scene::WaterSurface surface = scene::surfaceOf(w, id);
    lua_pushnumber(L, surface.heightAt(x, z, w.engineState().simTime) - surface.level + here.level);
    return 1;
}

int methodWaterGetNormalAt(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 at = checkVector3(L, 2);
    const scene::World& w = world(L);
    const auto x = static_cast<double>(at.x);
    const auto z = static_cast<double>(at.z);
    if (!std::isfinite(x) || !std::isfinite(z) || !scene::waterCovers(w, id, x, z)) {
        lua_pushnil(L);
        return 1;
    }
    const scene::WaterSample sample = scene::surfaceOf(w, id).sample(x, z, w.engineState().simTime);
    pushVector3(
        L, core::normalize(core::Vec3{static_cast<float>(-sample.slopeX), 1.0f, static_cast<float>(-sample.slopeZ)}));
    return 1;
}

// Cuts this water's bed into every terrain it lies over (ADR 0146 section 4).
int methodWaterCarve(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    scene::World& w = world(L);
    std::vector<core::InstanceId> terrains;
    w.terrains().forEach([&](core::InstanceId terrain, const scene::TerrainComponent&) {
        if (!w.destroyed(terrain))
            terrains.push_back(terrain);
    });
    core::u64 lowered = 0;
    for (const core::InstanceId terrain : terrains)
        lowered += scene::carveWaterBed(w, id, terrain);
    lua_pushnumber(L, static_cast<double>(lowered));
    return 1;
}

// --- Network ownership (ADR 0099) ---------------------------------------------

int methodSetNetworkOwner(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::InstanceId player = lua_isnoneornil(L, 2) ? core::InstanceId{} : checkInstance(L, 2);
    scene::World& w = world(L);
    // Handing a part over is the authority's to decide; a replica asking would
    // be a client deciding who simulates what.
    if (w.engineState().networkTopology == scene::NetworkTopology::Replica)
        raise(L, ENG_TR("script.err.network_owner_authority"));
    scene::RigidBodyComponent* body = w.rigidBodies().find(id);
    if (body == nullptr || body->anchored)
        raise(L, ENG_TR("script.err.network_owner_anchored"));
    const scene::PlayerComponent* owner = player.valid() ? w.players().find(player) : nullptr;
    if (player.valid() && (owner == nullptr || !w.alive(player)))
        raise(L, ENG_TR("script.err.network_owner_player"));
    // The player at the authority's own machine owning it is the authority
    // simulating it: nothing to hand over.
    body->networkOwner = owner != nullptr && !owner->local ? owner->userId : 0u;
    return 0;
}

int methodGetNetworkOwner(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const scene::World& w = world(L);
    const scene::RigidBodyComponent* body = w.rigidBodies().find(id);
    const core::InstanceId owner =
        body != nullptr && body->networkOwner != 0 ? scene::playerByUserId(w, body->networkOwner) : core::InstanceId{};
    if (owner.valid())
        pushInstance(L, owner);
    else
        lua_pushnil(L);
    return 1;
}

// --- Materials (ADR 0090) -----------------------------------------------------

// The parameter a method names: a built-in field a material may declare, or --
// any other name an HLSL identifier can have -- one of its surface shader's
// (ADR 0091). A raise for a built-in field no part may change, or a name no
// parameter can have.
struct NamedParameter
{
    std::string_view name;
    std::optional<asset::MaterialField> field;
};

[[nodiscard]] NamedParameter checkParameter(lua_State* L, int index)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, index, &length);
    const std::string_view name{text, length};
    const std::optional<asset::MaterialField> field = asset::materialFieldNamed(name);
    if (field.has_value() && (asset::fieldBit(*field) & asset::DeclarableParameters) != 0)
        return {name, field};
    if (!field.has_value() && asset::isShaderParameterName(name))
        return {name, std::nullopt};
    const core::I18nArg args[] = {{"name", name}};
    raise(L, ENG_TR("script.err.material_parameter_unknown"), args);
    return {name, std::nullopt};
}

// Written through the property rather than into the component, so a `Changed`
// on `MaterialParameters` fires for a method call as it does for an assignment.
void writeParameters(lua_State* L, core::InstanceId id, const asset::MaterialOverrides& overrides)
{
    World& w = world(L);
    (void)w.setProperty(id, w.atoms().intern("MaterialParameters"), scene::Value{overrides});
}

// **Raises for a parameter the material does not declare** (ADR 0090): a
// material decides what a part may change about it, and a write it would
// silently ignore is a script that looks like it works.
void raiseUndeclared(lua_State* L, const scene::PartComponent& part, std::string_view name)
{
    World& w = world(L);
    if (!part.material.valid()) {
        const core::I18nArg args[] = {{"name", name}};
        raise(L, ENG_TR("script.err.material_parameter_undeclared_default"), args);
    }
    const core::I18nArg args[] = {{"name", name}, {"content", w.atoms().text(part.material)}};
    raise(L, ENG_TR("script.err.material_parameter_undeclared"), args);
}

// **Focus is the interface's to move** (ADR 0139): a script asks, and the
// interaction takes the request at its next frame -- the one place focus
// changes, so two fields can never both believe they have it.
int methodTextInputCaptureFocus(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    if (scene::TextInputComponent* field = world(L).textInputs().find(id); field != nullptr)
        field->focusRequest = 1;
    return 0;
}

int methodTextInputReleaseFocus(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    if (scene::TextInputComponent* field = world(L).textInputs().find(id); field != nullptr) {
        field->focusRequest = -1;
        field->releaseSubmitted = lua_toboolean(L, 2) != 0;
    }
    return 0;
}

int methodTextInputIsFocused(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const scene::TextInputComponent* field = world(L).textInputs().find(id);
    lua_pushboolean(L, field != nullptr && field->focused ? 1 : 0);
    return 1;
}

// `UIPageLayout`'s four ways to turn the page (ADR 0128). `CurrentPage` has
// changed by the time each returns.
int methodPageLayoutNext(lua_State* L)
{
    (void)scene::stepPage(world(L), liveInstance(L, 1), 1);
    return 0;
}

int methodPageLayoutPrevious(lua_State* L)
{
    (void)scene::stepPage(world(L), liveInstance(L, 1), -1);
    return 0;
}

int methodPageLayoutJumpTo(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::InstanceId page = liveInstance(L, 2);
    World& w = world(L);
    std::vector<core::InstanceId> pages;
    scene::pagesOf(w, id, pages);
    const auto found = std::find(pages.begin(), pages.end(), page);
    if (found == pages.end())
        raise(L, ENG_TR("script.err.not_a_page"));
    (void)scene::turnPage(w, id, static_cast<core::i32>(found - pages.begin()));
    return 0;
}

int methodPageLayoutJumpToIndex(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const double index = luaL_checknumber(L, 2);
    if (!std::isfinite(index))
        raise(L, ENG_TR("script.err.not_a_page"));
    (void)scene::turnPage(world(L), id, static_cast<core::i32>(std::floor(index)));
    return 0;
}

int methodSetMaterialParameter(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const NamedParameter parameter = checkParameter(L, 2);
    World& w = world(L);
    const scene::PartComponent* part = w.parts().find(id);
    if (part == nullptr)
        return 0;
    const asset::ResolvedMaterial material = w.resolveMaterial(part->material, part->materialClone);

    if (!parameter.field.has_value()) {
        // A surface shader's parameter, by name. Not a texture: what a part
        // draws is batched by material, and a texture of its own would be a
        // material of its own -- which is what a clone is for.
        if (!material.declaresShaderParameter(parameter.name))
            raiseUndeclared(L, *part, parameter.name);
        asset::ShaderParameter value;
        value.name = std::string(parameter.name);
        if (!readShaderParameterValue(L, 3, value, false)) {
            const core::I18nArg args[] = {{"name", parameter.name}};
            raise(L, ENG_TR("script.err.material_parameter_type"), args);
        }
        w.setPartShaderParameter(id, std::move(value));
        return 0;
    }

    const asset::MaterialField field = *parameter.field;
    if ((material.instanceParameters & asset::fieldBit(field)) == 0)
        raiseUndeclared(L, *part, parameter.name);

    asset::MaterialProperties values;
    if (!readMaterialParameter(L, 3, field, values)) {
        const core::I18nArg args[] = {{"name", parameter.name}};
        raise(L, ENG_TR("script.err.material_parameter_type"), args);
    }
    asset::MaterialOverrides overrides = part->materialParameters;
    (void)asset::setOverride(overrides, field, values);
    writeParameters(L, id, overrides);
    return 0;
}

int methodGetMaterialParameter(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const NamedParameter parameter = checkParameter(L, 2);
    World& w = world(L);
    const scene::PartComponent* part = w.parts().find(id);
    if (part == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    // What the part draws with: its override where the material declares it,
    // and the material's own value everywhere else.
    asset::ResolvedMaterial surface = w.surfaceOf(*part);
    if (!parameter.field.has_value()) {
        w.applyPartShaderParameters(id, surface);
        // Nil where neither the part nor its material says: the shader's own
        // default, which only the shader knows.
        if (const asset::ShaderParameter* value = surface.properties.shaderParameter(parameter.name); value != nullptr)
            pushShaderParameterValue(L, *value);
        else
            lua_pushnil(L);
        return 1;
    }
    pushMaterialField(L, *parameter.field, surface.properties);
    return 1;
}

int methodClearMaterialParameter(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const NamedParameter parameter = checkParameter(L, 2);
    World& w = world(L);
    if (!parameter.field.has_value()) {
        (void)w.clearPartShaderParameter(id, parameter.name);
        return 0;
    }
    const scene::PartComponent* part = w.parts().find(id);
    if (part == nullptr || !part->materialParameters.has(*parameter.field))
        return 0;
    asset::MaterialOverrides overrides = part->materialParameters;
    asset::clearOverride(overrides, *parameter.field);
    writeParameters(L, id, overrides);
    return 0;
}

// --- The 2D layer (post-v1 phase 3) -------------------------------------------

int methodPart2DApplyImpulse(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec2 impulse = checkVector2(L, 2);
    // Summed and applied at the next tick, as a `BasePart`'s is: a script may
    // run at any point in the frame and the solver may not be interrupted.
    if (!std::isfinite(impulse.x) || !std::isfinite(impulse.y))
        return 0;
    if (scene::Part2DComponent* part = world(L).parts2d().find(id); part != nullptr)
        part->pendingImpulse = part->pendingImpulse + impulse;
    return 0;
}

// A cell coordinate: a whole number. A fraction is refused rather than rounded,
// because which way it rounds is a cell somebody did not mean.
[[nodiscard]] core::i32 checkCell(lua_State* L, int index)
{
    const double number = luaL_checknumber(L, index);
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < static_cast<double>(std::numeric_limits<core::i32>::min()) ||
        number > static_cast<double>(std::numeric_limits<core::i32>::max()))
        raise(L, ENG_TR("script.err.tile_cell"), {});
    return static_cast<core::i32>(number);
}

[[nodiscard]] core::u16 checkTile(lua_State* L, int index)
{
    const double number = luaL_checknumber(L, index);
    if (!std::isfinite(number) || std::floor(number) != number || number < 0.0 || number > 65535.0)
        raise(L, ENG_TR("script.err.tile_id"), {});
    return static_cast<core::u16>(number);
}

[[nodiscard]] scene::Tilemap2DComponent* tilemapOf(lua_State* L, core::InstanceId id)
{
    return world(L).tilemaps2d().find(id);
}

int methodTilemapSetCell(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::i32 x = checkCell(L, 2);
    const core::i32 y = checkCell(L, 3);
    const core::u16 tile = checkTile(L, 4);
    if (scene::Tilemap2DComponent* tilemap = tilemapOf(L, id); tilemap != nullptr)
        (void)tilemap->setCell(x, y, tile);
    return 0;
}

int methodTilemapGetCell(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::i32 x = checkCell(L, 2);
    const core::i32 y = checkCell(L, 3);
    const scene::Tilemap2DComponent* tilemap = tilemapOf(L, id);
    lua_pushnumber(L, tilemap != nullptr ? static_cast<double>(tilemap->cell(x, y)) : 0.0);
    return 1;
}

// The most cells one `FillRect` writes: a thousand by a thousand. Past it the
// rectangle is a mistake in its corners, and filling it would stop the frame.
constexpr double MaxFillCells = 1'000'000.0;

int methodTilemapFillRect(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::i32 x0 = checkCell(L, 2);
    const core::i32 y0 = checkCell(L, 3);
    const core::i32 x1 = checkCell(L, 4);
    const core::i32 y1 = checkCell(L, 5);
    const core::u16 tile = checkTile(L, 6);
    const core::i32 lowX = std::min(x0, x1);
    const core::i32 highX = std::max(x0, x1);
    const core::i32 lowY = std::min(y0, y1);
    const core::i32 highY = std::max(y0, y1);
    const double cells = (static_cast<double>(highX) - lowX + 1.0) * (static_cast<double>(highY) - lowY + 1.0);
    if (cells > MaxFillCells)
        raise(L, ENG_TR("script.err.tile_rect_too_large"), {});
    if (scene::Tilemap2DComponent* tilemap = tilemapOf(L, id); tilemap != nullptr) {
        for (core::i32 y = lowY; y <= highY; ++y) {
            for (core::i32 x = lowX; x <= highX; ++x)
                (void)tilemap->setCell(x, y, tile);
        }
    }
    return 0;
}

int methodTilemapClear(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    if (scene::Tilemap2DComponent* tilemap = tilemapOf(L, id); tilemap != nullptr && !tilemap->chunks.empty()) {
        tilemap->chunks.clear();
        tilemap->revision += 1;
    }
    return 0;
}

int methodCharacterMove(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 direction = checkVector3(L, 2);
    if (!core::isFinite(direction))
        raise(L, ENG_TR("script.err.not_finite"));

    if (scene::CharacterBodyComponent* character = world(L).characterBodies().find(id); character != nullptr)
        character->moveDirection = direction;
    return 0;
}

int methodCharacterJump(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);

    // A request rather than an impulse: it becomes one at the NEXT TICK and
    // never inside this call, because a velocity written mid-frame is a replay
    // that diverges (R10). Whether the character was grounded is not asked --
    // that is the game's policy since M7, and `Grounded` is exposed so a game
    // that wants the old rule writes one line.
    if (scene::CharacterBodyComponent* character = world(L).characterBodies().find(id); character != nullptr)
        character->jumpRequested = true;
    return 0;
}

// --- Ragdoll (E9 step 13) -----------------------------------------------------

// Degrees in, radians out. Every angle a person types in this API is in degrees
// -- `HingeConstraint.LimitLow` is, and a profile that disagreed with the
// property it fills would be a trap.
[[nodiscard]] f32 degreesField(lua_State* L, int table, const char* key, f32 fallback)
{
    lua_getfield(L, table, key);
    const f32 value = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) * 0.017453292519943295f : fallback;
    lua_pop(L, 1);
    return value;
}

[[nodiscard]] f32 numberField(lua_State* L, int table, const char* key, f32 fallback)
{
    lua_getfield(L, table, key);
    const f32 value = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return value;
}

// `Joint` is a string or a list of them. A list because the same shoulder is
// `mixamorig:LeftArm` or `upper_arm.L` depending on who exported it, and a
// profile that named one spelling would be a profile for one exporter.
void readJointNames(lua_State* L, int table, std::vector<std::string>& out)
{
    lua_getfield(L, table, "Joint");
    if (lua_isstring(L, -1)) {
        out.emplace_back(lua_tostring(L, -1));
    }
    else if (lua_istable(L, -1)) {
        const int names = lua_gettop(L);
        for (int index = 1;; ++index) {
            lua_rawgeti(L, names, index);
            if (!lua_isstring(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            out.emplace_back(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

int methodRagdollBuild(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    scene::SkeletonHost* skeleton = context(L).services->skeleton;
    if (skeleton == nullptr) {
        // A build with no render module has no rig to read, and a ragdoll with
        // no limbs in it would be a silent success nobody could debug.
        raise(L, ENG_TR("scene.err.ragdoll_no_rig"));
    }

    // **Read into the profile in array order**, which becomes creation order,
    // which is what an instance id is (R10). A table with holes stops at the
    // first one, exactly as `ipairs` does and for the same reason: a profile
    // whose length depended on `#` over a sparse table is a profile whose
    // ragdoll depends on how Luau happened to size the array part.
    scene::RagdollProfile profile;
    for (int index = 1;; ++index) {
        lua_rawgeti(L, 2, index);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            break;
        }
        const int entry = lua_gettop(L);

        scene::RagdollLimb limb;
        readJointNames(L, entry, limb.joints);

        lua_getfield(L, entry, "Parent");
        // 1-based in Luau, 0-based here -- and absent means the root, which is
        // what -1 already means.
        limb.parent = lua_isnumber(L, -1) ? static_cast<i32>(lua_tointeger(L, -1)) - 1 : -1;
        lua_pop(L, 1);

        limb.radius = numberField(L, entry, "Radius", 0.08f);
        limb.leafLength = numberField(L, entry, "LeafLength", 0.18f);
        limb.limitLow = degreesField(L, entry, "LimitLow", 1.0f);
        limb.limitHigh = degreesField(L, entry, "LimitHigh", -1.0f);

        lua_getfield(L, entry, "Kind");
        const std::string_view kind = lua_isstring(L, -1) ? std::string_view(lua_tostring(L, -1)) : "BallSocket";
        // `physics::ConstraintType`'s values, and the mapping lives here because
        // this is where a person's word becomes one. A ball socket with limits
        // IS a swing-twist, which is what the `LimitsEnabled` setter says -- so
        // the presence of `Swing` is what decides between them rather than a
        // fourth word nobody would know to type.
        lua_pop(L, 1);
        lua_getfield(L, entry, "Swing");
        const bool limited = lua_isnumber(L, -1);
        lua_pop(L, 1);
        if (kind == "Hinge")
            limb.kind = 2;
        else if (kind == "Fixed")
            limb.kind = 0;
        else
            limb.kind = limited ? 3 : 1;

        limb.swingLimit = degreesField(L, entry, "Swing", 0.7f);
        limb.twistLimit = degreesField(L, entry, "Twist", 0.4f);

        profile.limbs.push_back(std::move(limb));
        lua_pop(L, 1);
    }

    scene::World& w = world(L);
    const scene::RagdollBuildResult result =
        scene::buildRagdoll(w, *skeleton, id, profile, scene::resolveRagdollClasses(w));
    if (result.error.has_value())
        raise(L, *result.error);

    lua_pushinteger(L, static_cast<int>(result.limbs));
    return 1;
}

// --- Registration ------------------------------------------------------------

// What `Instance` and `Model` declare. `WaitForChild` is absent on purpose: it
// parks on a tree state rather than on a value this file can produce, so it is
// implemented beside the services that make the tree move.
// --- Terrain (ADR 0082) ------------------------------------------------------
//
// **Every verb here writes the field, and the field is part of the world.** So a
// sculpt is undoable in the editor, it moves the world hash, and it saves with
// the project -- none of which needed anything special, because the field lives
// in a component like every other piece of world state.
//
// Hand-bound rather than generated for the reason the table below exists at all:
// a `MethodDesc` carries a name, whether it yields and its thread safety, and
// every argument is checked here with `luaL_check*`. A generated method would
// need the IDL to describe argument checking, which is a language nobody asked
// for.

// The count an edit changed, or `scene.err.terrain_brush_too_large` for one
// refused for its size (audit S10).
void pushTouched(lua_State* L, const asset::EditReport& report)
{
    if (report.refused) {
        const core::u64 limit = report.limit != 0 ? report.limit : asset::MaxEditVoxels;
        const core::I18nArg args[] = {{"limit", static_cast<core::i64>(limit)}};
        raise(L, ENG_TR("scene.err.terrain_brush_too_large"), args);
    }
    lua_pushinteger(L, static_cast<int>(report.touched));
}

// **The ground under an edit or a read, loaded first where it is not yet**
// (terrain audit U1, TA16): a script digging or building in a cell still on
// disk made a chunk of only its edit, which shadowed the cell's own when it
// came in -- and a read there read air. `reach` in metres round `center`,
// world space, and a margin for the ramp and a smooth's blur. **All of it or
// a refusal**: an edit reaching more ground than loads at once is refused
// before it touches anything, by the same words the editor uses.
void groundFirst(lua_State* L, const scene::TerrainComponent& terrain, core::Vec3 low, core::Vec3 high)
{
    // A square that is not one reads nothing: its edit is refused anyway.
    if (!std::isfinite(low.x) || !std::isfinite(low.z) || !std::isfinite(high.x) || !std::isfinite(high.z))
        return;
    const double margin = 8.0 * static_cast<double>(terrain.field.settings().voxelSize);
    const core::DVec3 from{static_cast<double>(std::min(low.x, high.x)) - margin, 0.0,
                           static_cast<double>(std::min(low.z, high.z)) - margin};
    if (!world(L).loadGround(from, core::DVec3{static_cast<double>(std::max(low.x, high.x)) + margin, 0.0,
                                               static_cast<double>(std::max(low.z, high.z)) + margin})) {
        const core::I18nArg args[] = {{"limit", static_cast<core::i64>(scene::World::MaxGroundCells)}};
        raise(L, ENG_TR("scene.err.terrain_ground_too_wide"), args);
    }
}

void groundFirst(lua_State* L, const scene::TerrainComponent& terrain, core::Vec3 center, double reach)
{
    if (!std::isfinite(reach) || !(reach >= 0.0))
        return;
    const auto r = static_cast<float>(std::min(reach, 1.0e7));
    groundFirst(L, terrain, core::Vec3{center.x - r, center.y, center.z - r},
                core::Vec3{center.x + r, center.y, center.z + r});
}

// **A material id, checked rather than wrapped** (terrain audit B6): through a
// `u8` cast, 256 was 0 -- which removes -- and -1 was 255.
[[nodiscard]] core::u8 checkMaterial(lua_State* L, int arg)
{
    const lua_Integer material = luaL_checkinteger(L, arg);
    if (material < 0 || material > 255)
        luaL_argerror(L, arg, "a material id from 0 to 255");
    return static_cast<core::u8>(material);
}

// A strength or an amount a brush can use: a NaN clamped to itself and turned
// a Smooth into a dig (terrain audit B6).
[[nodiscard]] float checkFinite(lua_State* L, int arg, double fallback)
{
    const double value = luaL_optnumber(L, arg, fallback);
    if (!std::isfinite(value))
        luaL_argerror(L, arg, "a finite number");
    return static_cast<float>(value);
}

// **A place and a reach an edit can use, or a refusal** (the terrain audit's
// "bad inputs fail silently"): a NaN centre or an infinite radius changed
// nothing and returned 0, which a script cannot tell from ground that already
// was what it asked for.
[[nodiscard]] core::Vec3 checkPlace(lua_State* L, int arg)
{
    const core::Vec3 place = checkVector3(L, arg);
    if (!std::isfinite(place.x) || !std::isfinite(place.y) || !std::isfinite(place.z))
        luaL_argerror(L, arg, "a finite position");
    return place;
}

[[nodiscard]] double checkReach(lua_State* L, int arg)
{
    const double reach = luaL_checknumber(L, arg);
    if (!std::isfinite(reach) || reach < 0.0)
        luaL_argerror(L, arg, "a finite distance, zero or more");
    return reach;
}

int methodTerrainFillBall(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 center = checkPlace(L, 2);
    const double radius = checkReach(L, 3);
    const core::u8 material = checkMaterial(L, 4);

    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushinteger(L, 0);
        return 1;
    }
    groundFirst(L, *terrain, center, radius);

    // A `Vector3` from a script is `f32` and a brush takes a world position,
    // which is `f64` (R9). Widened explicitly: Clang diagnoses the implicit form
    // and MSVC does not, so leaving it implicit is a Linux-only build break.
    // Into the field's own space: a terrain can be moved, and the offset is
    // applied by its consumers rather than baked into every tile.
    const core::DVec3 wide{static_cast<double>(center.x) - terrain->origin.x,
                           static_cast<double>(center.y) - terrain->origin.y,
                           static_cast<double>(center.z) - terrain->origin.z};
    const asset::EditReport report = asset::fillBall(terrain->field, wide, radius, material);
    if (report.touched > 0)
        terrain->fieldRevision += 1;
    pushTouched(L, report);
    return 1;
}

int methodTerrainRaiseBall(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 center = checkPlace(L, 2);
    const double radius = checkReach(L, 3);
    const auto amount = static_cast<float>(luaL_checknumber(L, 4));
    const lua_Integer material = luaL_optinteger(L, 5, 0);
    if (material < 0 || material > 255)
        luaL_argerror(L, 5, "a material id from 1 to 255");
    // `GrowBall`'s rule: an infinite amount is not a height (terrain audit B1).
    if (!std::isfinite(amount))
        luaL_argerror(L, 4, "a finite number of metres");

    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushinteger(L, 0);
        return 1;
    }
    groundFirst(L, *terrain, center, radius);

    // The field's own space; see `FillBall` above.
    const core::DVec3 wide{static_cast<double>(center.x) - terrain->origin.x,
                           static_cast<double>(center.y) - terrain->origin.y,
                           static_cast<double>(center.z) - terrain->origin.z};
    const asset::EditReport report =
        asset::raiseBall(terrain->field, wide, radius, amount, static_cast<core::u8>(material));
    if (report.touched > 0)
        terrain->fieldRevision += 1;
    pushTouched(L, report);
    return 1;
}

int methodTerrainGrowBall(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 center = checkPlace(L, 2);
    const double radius = checkReach(L, 3);
    const auto amount = static_cast<float>(luaL_checknumber(L, 4));
    const lua_Integer material = luaL_optinteger(L, 5, 0);
    if (material < 0 || material > 255)
        luaL_argerror(L, 5, "a material id from 1 to 255");
    if (!std::isfinite(amount))
        luaL_argerror(L, 4, "a finite number of metres");

    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushinteger(L, 0);
        return 1;
    }
    groundFirst(L, *terrain, center, radius);
    // The field's own space; see `FillBall` above.
    const core::DVec3 wide{static_cast<double>(center.x) - terrain->origin.x,
                           static_cast<double>(center.y) - terrain->origin.y,
                           static_cast<double>(center.z) - terrain->origin.z};
    const asset::EditReport report =
        asset::growBall(terrain->field, wide, radius, amount, static_cast<core::u8>(material));
    if (report.touched > 0)
        terrain->fieldRevision += 1;
    pushTouched(L, report);
    return 1;
}

int methodTerrainFillBlock(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 center = checkPlace(L, 2);
    const core::Vec3 size = checkPlace(L, 3);
    const core::u8 material = checkMaterial(L, 4);

    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushinteger(L, 0);
        return 1;
    }
    groundFirst(L, *terrain, center,
                0.5 * std::max(std::abs(static_cast<double>(size.x)), std::abs(static_cast<double>(size.z))));

    // The field's own space; see `FillBall` above.
    const core::DVec3 wide{static_cast<double>(center.x) - terrain->origin.x,
                           static_cast<double>(center.y) - terrain->origin.y,
                           static_cast<double>(center.z) - terrain->origin.z};
    const asset::EditReport report = asset::fillBlock(terrain->field, wide, size, material);
    if (report.touched > 0)
        terrain->fieldRevision += 1;
    pushTouched(L, report);
    return 1;
}

int methodTerrainWriteHeights(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 corner = checkPlace(L, 2);
    const lua_Integer columns = luaL_checkinteger(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    // One id for every column, or a table of them parallel to the heights.
    const bool perColumn = lua_istable(L, 5);
    const lua_Integer material = perColumn ? 1 : luaL_optinteger(L, 5, 1);
    const auto count = static_cast<core::usize>(lua_objlen(L, 4));
    // A 4096-column square is sixteen million heights and the largest thing
    // this is for; past it a script has a loop that did not stop.
    constexpr core::usize MaxHeights = 4096u * 4096u;
    if (columns < 1 || count == 0 || count % static_cast<core::usize>(columns) != 0 || count > MaxHeights) {
        const core::I18nArg args[] = {{"count", static_cast<core::i64>(count)},
                                      {"columns", static_cast<core::i64>(columns)}};
        raise(L, ENG_TR("scene.err.terrain_heights_shape"), args);
    }
    if (material < 1 || material > 255)
        luaL_argerror(L, 5, "a material id from 1 to 255");
    std::vector<core::u8> materials;
    if (perColumn) {
        if (static_cast<core::usize>(lua_objlen(L, 5)) != count)
            luaL_argerror(L, 5, "one material per height");
        materials.resize(count);
        for (core::usize at = 0; at < count; ++at) {
            lua_rawgeti(L, 5, static_cast<int>(at + 1));
            const lua_Integer value = lua_isnumber(L, -1) != 0 ? static_cast<lua_Integer>(lua_tointeger(L, -1)) : -1;
            lua_pop(L, 1);
            if (value < 0 || value > 255)
                luaL_argerror(L, 5, "material ids from 0 to 255");
            materials[at] = static_cast<core::u8>(value);
        }
    }

    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushinteger(L, 0);
        return 1;
    }
    std::vector<float> heights(count);
    for (core::usize at = 0; at < count; ++at) {
        lua_rawgeti(L, 4, static_cast<int>(at + 1));
        // World heights, like every other verb's, into the field's own space.
        heights[at] = lua_isnumber(L, -1) != 0 ? static_cast<float>(lua_tonumber(L, -1) - terrain->origin.y)
                                               : std::numeric_limits<float>::quiet_NaN();
        lua_pop(L, 1);
    }

    // The field's own space, snapped down to its grid: the first height is the
    // voxel column the corner falls in, which is what a heightmap's first pixel
    // means.
    // Through `voxelIndex`, which clamps before it casts (terrain audit B6).
    const core::i32 firstX = terrain->field.voxelIndex(static_cast<double>(corner.x) - terrain->origin.x);
    const core::i32 firstZ = terrain->field.voxelIndex(static_cast<double>(corner.z) - terrain->origin.z);
    {
        const double voxel = static_cast<double>(terrain->field.settings().voxelSize);
        const auto across = static_cast<float>(static_cast<double>(columns) * voxel);
        const auto deep = static_cast<float>(static_cast<double>(count / static_cast<core::usize>(columns)) * voxel);
        groundFirst(L, *terrain, corner, core::Vec3{corner.x + across, corner.y, corner.z + deep});
    }
    const asset::EditReport report =
        perColumn ? asset::writeHeights(terrain->field, firstX, firstZ, static_cast<core::u32>(columns), heights,
                                        std::span<const core::u8>(materials))
                  : asset::writeHeights(terrain->field, firstX, firstZ, static_cast<core::u32>(columns), heights,
                                        static_cast<core::u8>(material));
    if (report.touched > 0)
        terrain->fieldRevision += 1;
    pushTouched(L, report);
    return 1;
}

int methodTerrainGetLayers(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const scene::TerrainComponent* terrain = world(L).terrains().find(id);
    const std::vector<std::string> fallback = asset::defaultTerrainLayers();
    const std::vector<std::string>& layers = terrain != nullptr ? terrain->layers : fallback;
    lua_createtable(L, static_cast<int>(layers.size()), 0);
    for (core::usize index = 0; index < layers.size(); ++index) {
        lua_pushlstring(L, layers[index].data(), layers[index].size());
        lua_rawseti(L, -2, static_cast<int>(index + 1));
    }
    return 1;
}

int methodTerrainSetLayers(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const auto count = static_cast<core::usize>(lua_objlen(L, 2));
    if (count > asset::MaxTerrainLayers)
        luaL_argerror(L, 2, "at most 255 layers");
    std::vector<std::string> layers;
    layers.reserve(count);
    for (core::usize at = 0; at < count; ++at) {
        lua_rawgeti(L, 2, static_cast<int>(at + 1));
        // The type first: `lua_tolstring` turns a number into a string in place.
        if (lua_type(L, -1) != LUA_TSTRING)
            luaL_argerror(L, 2, "a list of material URNs");
        size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        layers.emplace_back(text, length);
        lua_pop(L, 1);
    }
    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain != nullptr && terrain->layers != layers) {
        terrain->layers = std::move(layers);
        terrain->layersRevision += 1;
    }
    return 0;
}

int methodTerrainGetRules(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const scene::TerrainComponent* terrain = world(L).terrains().find(id);
    const std::vector<asset::TerrainRule> fallback = asset::defaultTerrainRules();
    const std::vector<asset::TerrainRule>& rules = terrain != nullptr ? terrain->rules : fallback;
    lua_createtable(L, static_cast<int>(rules.size()), 0);
    for (core::usize index = 0; index < rules.size(); ++index) {
        const asset::TerrainRule& rule = rules[index];
        lua_createtable(L, 0, 9);
        lua_pushboolean(L, rule.enabled);
        lua_setfield(L, -2, "Enabled");
        const auto number = [&](const char* key, double value) {
            lua_pushnumber(L, value);
            lua_setfield(L, -2, key);
        };
        number("Material", static_cast<double>(rule.material));
        number("SlopeMin", static_cast<double>(rule.slopeMin));
        number("SlopeMax", static_cast<double>(rule.slopeMax));
        number("HeightMin", static_cast<double>(rule.heightMin));
        number("HeightMax", static_cast<double>(rule.heightMax));
        number("Blend", static_cast<double>(rule.blend));
        number("Noise", static_cast<double>(rule.noise));
        lua_createtable(L, static_cast<int>(rule.appliesTo.size()), 0);
        for (core::usize at = 0; at < rule.appliesTo.size(); ++at) {
            lua_pushnumber(L, static_cast<double>(rule.appliesTo[at]));
            lua_rawseti(L, -2, static_cast<int>(at + 1));
        }
        lua_setfield(L, -2, "AppliesTo");
        lua_rawseti(L, -2, static_cast<int>(index + 1));
    }
    return 1;
}

// `FoliageLayer:GetMaterials` and `SetMaterials` (ADR 0116).
int methodFoliageLayerGetMaterials(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const scene::FoliageLayerComponent* layer = world(L).foliageLayers().find(id);
    const core::usize count = layer != nullptr ? layer->materials.size() : 0;
    lua_createtable(L, static_cast<int>(count), 0);
    for (core::usize index = 0; index < count; ++index) {
        lua_createtable(L, 0, 2);
        lua_pushnumber(L, static_cast<double>(layer->materials[index].material));
        lua_setfield(L, -2, "Material");
        lua_pushnumber(L, static_cast<double>(layer->materials[index].density));
        lua_setfield(L, -2, "Density");
        lua_rawseti(L, -2, static_cast<int>(index + 1));
    }
    return 1;
}

int methodFoliageLayerSetMaterials(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const auto count = static_cast<core::usize>(lua_objlen(L, 2));
    if (count > 32)
        luaL_argerror(L, 2, "at most 32 materials");
    std::vector<scene::FoliageMaterial> materials;
    materials.reserve(count);
    for (core::usize at = 0; at < count; ++at) {
        lua_rawgeti(L, 2, static_cast<int>(at + 1));
        if (!lua_istable(L, -1))
            luaL_argerror(L, 2, "a list of { Material, Density } tables");
        scene::FoliageMaterial entry;
        lua_getfield(L, -1, "Material");
        const double material = lua_tonumber(L, -1);
        if (lua_isnumber(L, -1) == 0 || !std::isfinite(material) || material < 1.0 || material > 255.0)
            luaL_argerror(L, 2, "Material");
        entry.material = static_cast<core::u8>(material);
        lua_pop(L, 1);
        lua_getfield(L, -1, "Density");
        if (lua_isnumber(L, -1) != 0) {
            const double density = lua_tonumber(L, -1);
            if (!std::isfinite(density) || density < 0.0)
                luaL_argerror(L, 2, "Density");
            entry.density = static_cast<core::f32>(density);
        }
        else if (!lua_isnil(L, -1)) {
            luaL_argerror(L, 2, "Density");
        }
        lua_pop(L, 2);
        materials.push_back(entry);
    }
    if (scene::FoliageLayerComponent* layer = world(L).foliageLayers().find(id); layer != nullptr)
        layer->materials = std::move(materials);
    return 0;
}

int methodTerrainSetRules(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const auto count = static_cast<core::usize>(lua_objlen(L, 2));
    if (count > asset::MaxTerrainRules)
        luaL_argerror(L, 2, "at most 16 rules");
    std::vector<asset::TerrainRule> rules;
    rules.reserve(count);
    for (core::usize at = 0; at < count; ++at) {
        lua_rawgeti(L, 2, static_cast<int>(at + 1));
        if (!lua_istable(L, -1))
            luaL_argerror(L, 2, "a list of rule tables");
        asset::TerrainRule rule;
        const auto number = [&](const char* key, core::f32& slot, double low, double high) {
            lua_getfield(L, -1, key);
            if (lua_isnumber(L, -1) != 0) {
                const double value = lua_tonumber(L, -1);
                if (!std::isfinite(value) || value < low || value > high)
                    luaL_argerror(L, 2, key);
                slot = static_cast<core::f32>(value);
            }
            else if (!lua_isnil(L, -1)) {
                luaL_argerror(L, 2, key);
            }
            lua_pop(L, 1);
        };
        lua_getfield(L, -1, "Enabled");
        if (lua_isboolean(L, -1))
            rule.enabled = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        core::f32 material = static_cast<core::f32>(rule.material);
        number("Material", material, 1.0, 255.0);
        rule.material = static_cast<core::u8>(material);
        number("SlopeMin", rule.slopeMin, 0.0, 90.0);
        number("SlopeMax", rule.slopeMax, 0.0, 90.0);
        number("HeightMin", rule.heightMin, -100000.0, 100000.0);
        number("HeightMax", rule.heightMax, -100000.0, 100000.0);
        number("Blend", rule.blend, 0.0, 100000.0);
        number("Noise", rule.noise, 0.0, 10.0);
        rule.appliesTo.clear();
        lua_getfield(L, -1, "AppliesTo");
        if (lua_istable(L, -1)) {
            const auto ids = static_cast<core::usize>(lua_objlen(L, -1));
            for (core::usize index = 0; index < ids; ++index) {
                lua_rawgeti(L, -1, static_cast<int>(index + 1));
                const double value = lua_isnumber(L, -1) != 0 ? lua_tonumber(L, -1) : -1.0;
                lua_pop(L, 1);
                if (value < 1.0 || value > 255.0)
                    luaL_argerror(L, 2, "AppliesTo holds material ids from 1 to 255");
                rule.appliesTo.push_back(static_cast<core::u8>(value));
            }
        }
        lua_pop(L, 1);
        lua_pop(L, 1); // the rule
        rules.push_back(std::move(rule));
    }
    if (scene::TerrainComponent* terrain = world(L).terrains().find(id); terrain != nullptr)
        terrain->rules = std::move(rules);
    return 0;
}

int methodTerrainApplyRules(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 low = checkPlace(L, 2);
    const core::Vec3 high = checkPlace(L, 3);
    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushinteger(L, 0);
        return 1;
    }
    groundFirst(L, *terrain, low, high);
    // The field's own space, like every other verb's.
    const auto local = [&](core::Vec3 at) {
        return core::DVec3{static_cast<double>(at.x) - terrain->origin.x, static_cast<double>(at.y) - terrain->origin.y,
                           static_cast<double>(at.z) - terrain->origin.z};
    };
    const asset::EditReport report =
        asset::applyRules(terrain->field, terrain->rules, local(low), local(high), terrain->origin.y);
    if (report.touched > 0)
        terrain->fieldRevision += 1;
    pushTouched(L, report);
    return 1;
}

int methodTerrainHeightAt(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const auto x = static_cast<double>(luaL_checknumber(L, 2));
    const auto z = static_cast<double>(luaL_checknumber(L, 3));
    if (!std::isfinite(x))
        luaL_argerror(L, 2, "a finite number");
    if (!std::isfinite(z))
        luaL_argerror(L, 3, "a finite number");

    const scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    // The column's cell, loaded first where it is on disk (TA16).
    groundFirst(L, *terrain, core::Vec3{static_cast<float>(x), 0.0f, static_cast<float>(z)}, 0.0);

    // **Nil where there is no ground, rather than zero.** Zero is a legitimate
    // height and "there is nothing here" is not a height at all, so a script
    // that placed a tree wherever this answered would otherwise plant a forest
    // at sea level across every unsculpted cell.
    // Asked in the field's own space and answered in the world's, so a moved
    // terrain answers about where it now is.
    const std::optional<float> height = asset::heightAt(terrain->field, x - terrain->origin.x, z - terrain->origin.z);
    if (!height.has_value()) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, static_cast<double>(*height) + terrain->origin.y);
    return 1;
}

int methodTerrainPaintBall(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 center = checkPlace(L, 2);
    const double radius = checkReach(L, 3);
    const core::u8 material = checkMaterial(L, 4);
    // `{ Mode, Strength, Falloff }` (ADR 0114), each optional.
    asset::PaintOptions options;
    if (!lua_isnoneornil(L, 5)) {
        luaL_checktype(L, 5, LUA_TTABLE);
        lua_getfield(L, 5, "Mode");
        if (!lua_isnil(L, -1)) {
            const scene::EnumValue mode = checkEnumItem(L, lua_gettop(L));
            if (mode.enumId != scene::generated::TerrainPaintModeEnumId)
                luaL_argerror(L, 5, "a Mode from Enum.TerrainPaintMode");
            options.mode = static_cast<asset::PaintMode>(mode.value);
        }
        lua_pop(L, 1);
        lua_getfield(L, 5, "Strength");
        if (!lua_isnil(L, -1)) {
            const double strength = luaL_checknumber(L, -1);
            if (!std::isfinite(strength))
                luaL_argerror(L, 5, "a finite Strength");
            options.strength = static_cast<float>(strength);
        }
        lua_pop(L, 1);
        lua_getfield(L, 5, "Falloff");
        if (!lua_isnil(L, -1)) {
            const double falloff = luaL_checknumber(L, -1);
            if (!std::isfinite(falloff))
                luaL_argerror(L, 5, "a finite Falloff");
            options.falloff = static_cast<float>(falloff);
        }
        lua_pop(L, 1);
    }
    if (material == 0 && options.mode != asset::PaintMode::Erase) {
        lua_pushinteger(L, 0);
        return 1;
    }

    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushinteger(L, 0);
        return 1;
    }
    groundFirst(L, *terrain, center, radius);

    // The field's own space; see `FillBall` above.
    const core::DVec3 wide{static_cast<double>(center.x) - terrain->origin.x,
                           static_cast<double>(center.y) - terrain->origin.y,
                           static_cast<double>(center.z) - terrain->origin.z};
    const asset::EditReport report = asset::paintBall(terrain->field, wide, radius, material, options);
    // **Only when something changed.** Painting a hillside the colour it already
    // is has to leave the revision alone, or a script calling it in a loop would
    // rebuild every collider in range every tick.
    if (report.touched > 0)
        terrain->fieldRevision += 1;
    pushTouched(L, report);
    return 1;
}

int methodTerrainClear(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    if (scene::TerrainComponent* terrain = world(L).terrains().find(id); terrain != nullptr) {
        terrain->field = asset::TerrainField(terrain->field.settings());
        // **And its cells on disk** (terrain audit TA16c), as the editor's
        // Clear does: cleared in memory only, every cell not loaded streamed
        // back in.
        terrain->cellIndex.clear();
        terrain->fieldRevision += 1;
    }
    return 0;
}

// **Nothing to do, and kept so scripts written against the hybrid still run.**
// Every write leaves the voxels compact (ADR 0082); there is no second encoding
// to convert back to.
int methodTerrainCompact(lua_State* L)
{
    (void)liveInstance(L, 1);
    lua_pushinteger(L, 0);
    return 1;
}

// A brush's centre, from a script's world `vector` into the field's own space.
[[nodiscard]] core::DVec3 intoField(const scene::TerrainComponent& terrain, core::Vec3 at) noexcept
{
    return core::DVec3{static_cast<double>(at.x) - terrain.origin.x, static_cast<double>(at.y) - terrain.origin.y,
                       static_cast<double>(at.z) - terrain.origin.z};
}

// Answers the count a brush changed, bumping the revision only when it did:
// a brush that changed nothing must not rebuild a collider.
int finishEdit(lua_State* L, scene::TerrainComponent& terrain, const asset::EditReport& report)
{
    if (report.touched > 0)
        terrain.fieldRevision += 1;
    pushTouched(L, report);
    return 1;
}

int methodTerrainFillCylinder(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 center = checkPlace(L, 2);
    const double height = checkReach(L, 3);
    const double radius = checkReach(L, 4);
    const core::u8 material = checkMaterial(L, 5);
    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushinteger(L, 0);
        return 1;
    }
    groundFirst(L, *terrain, center, radius);
    return finishEdit(L, *terrain,
                      asset::fillCylinder(terrain->field, intoField(*terrain, center), height, radius, material));
}

int methodTerrainSmoothBall(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 center = checkPlace(L, 2);
    const double radius = checkReach(L, 3);
    const float strength = checkFinite(L, 4, 0.5);
    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushinteger(L, 0);
        return 1;
    }
    groundFirst(L, *terrain, center, radius);
    return finishEdit(L, *terrain, asset::smoothBall(terrain->field, intoField(*terrain, center), radius, strength));
}

int methodTerrainFlattenBall(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 center = checkPlace(L, 2);
    const double radius = checkReach(L, 3);
    const double height = luaL_checknumber(L, 4);
    if (!std::isfinite(height))
        luaL_argerror(L, 4, "a finite height");
    const float strength = checkFinite(L, 5, 1.0);
    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushinteger(L, 0);
        return 1;
    }
    groundFirst(L, *terrain, center, radius);
    // A world height, like every other verb's, into the field's own space.
    const auto local = static_cast<float>(height - terrain->origin.y);
    return finishEdit(L, *terrain,
                      asset::flattenBall(terrain->field, intoField(*terrain, center), radius, local, strength));
}

int methodTerrainReplaceMaterial(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 low = checkPlace(L, 2);
    const core::Vec3 high = checkPlace(L, 3);
    const core::u8 from = checkMaterial(L, 4);
    const core::u8 to = checkMaterial(L, 5);
    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushinteger(L, 0);
        return 1;
    }
    groundFirst(L, *terrain, low, high);
    return finishEdit(
        L, *terrain,
        asset::replaceMaterial(terrain->field, intoField(*terrain, low), intoField(*terrain, high), from, to));
}

// The largest region `ReadVoxels` and `WriteVoxels` take, in voxels: 256 on a
// side. Past it a script wants a loop of regions, not one table of sixteen
// million entries.
constexpr core::usize MaxVoxelRegion = 256u * 256u * 256u;

// The voxel box a pair of world corners names: every voxel whose cube overlaps
// `[low, high)`, snapped outward to the grid.
struct VoxelRegion
{
    core::i32 x = 0;
    core::i32 y = 0;
    core::i32 z = 0;
    core::i32 sizeX = 0;
    core::i32 sizeY = 0;
    core::i32 sizeZ = 0;

    // **Saturating** (terrain audit TA5): each side can be 2^27 voxels, and
    // three of them multiplied as integers passed 2^64 and wrapped to a count
    // the limit let through -- after which the loops ran the true sizes. In
    // doubles the product is only inexact where it is far past any limit.
    [[nodiscard]] core::usize count() const noexcept
    {
        const double product = static_cast<double>(sizeX) * static_cast<double>(sizeY) * static_cast<double>(sizeZ);
        constexpr auto Most = std::numeric_limits<core::usize>::max();
        return product >= static_cast<double>(Most) ? Most : static_cast<core::usize>(product);
    }
};

[[nodiscard]] VoxelRegion regionOf(const scene::TerrainComponent& terrain, core::Vec3 low, core::Vec3 high)
{
    const double voxel = static_cast<double>(terrain.field.settings().voxelSize);
    const core::DVec3 a = intoField(terrain, low);
    const core::DVec3 b = intoField(terrain, high);
    // Through `voxelIndex`, which clamps before it casts, and the sizes in 64
    // bits: a region a script can name is not one whose width overflows an
    // `i32` (terrain audit B6).
    const auto first = [&terrain](double p, double q) { return terrain.field.voxelIndex(std::min(p, q)); };
    const auto past = [&terrain, voxel](double p, double q) {
        const double top = std::max(p, q);
        const core::i32 floor = terrain.field.voxelIndex(top);
        return static_cast<core::i64>(floor) + (std::floor(top / voxel) == top / voxel ? 0 : 1);
    };
    const auto size = [](core::i64 past, core::i32 first) {
        return static_cast<core::i32>(std::clamp<core::i64>(past - first, 0, std::numeric_limits<core::i32>::max()));
    };
    VoxelRegion region;
    region.x = first(a.x, b.x);
    region.y = first(a.y, b.y);
    region.z = first(a.z, b.z);
    region.sizeX = size(past(a.x, b.x), region.x);
    region.sizeY = size(past(a.y, b.y), region.y);
    region.sizeZ = size(past(a.z, b.z), region.z);
    return region;
}

// **Materials and occupancies, flat, x fastest, then y, then z**: index
// `1 + x + sizeX * (y + sizeY * z)`, with the size returned third. Flat rather
// than nested three deep because a table of tables of tables is a table per row
// for a script to allocate and the engine to walk, for no information the size
// does not already give.
int methodTerrainReadVoxels(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 low = checkPlace(L, 2);
    const core::Vec3 high = checkPlace(L, 3);
    const scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_createtable(L, 0, 0);
        lua_createtable(L, 0, 0);
        pushVector3(L, core::Vec3{});
        return 3;
    }
    const VoxelRegion region = regionOf(*terrain, low, high);
    if (region.count() <= MaxVoxelRegion)
        groundFirst(L, *terrain, low, high);
    if (region.count() > MaxVoxelRegion) {
        const core::I18nArg args[] = {{"count", static_cast<core::i64>(std::min<core::usize>(
                                                    region.count(), std::numeric_limits<core::i64>::max()))},
                                      {"limit", static_cast<core::i64>(MaxVoxelRegion)}};
        raise(L, ENG_TR("scene.err.terrain_region_too_large"), args);
    }
    const auto count = static_cast<int>(region.count());
    lua_createtable(L, count, 0);
    const int materials = lua_gettop(L);
    lua_createtable(L, count, 0);
    const int occupancies = lua_gettop(L);
    int at = 1;
    for (core::i32 z = 0; z < region.sizeZ; ++z) {
        for (core::i32 y = 0; y < region.sizeY; ++y) {
            for (core::i32 x = 0; x < region.sizeX; ++x) {
                const asset::Voxel voxel = terrain->field.voxel(region.x + x, region.y + y, region.z + z);
                lua_pushinteger(L, voxel.material);
                lua_rawseti(L, materials, at);
                lua_pushnumber(L, static_cast<double>(asset::occupancyOf(voxel)));
                lua_rawseti(L, occupancies, at);
                ++at;
            }
        }
    }
    pushVector3(L, core::Vec3{static_cast<float>(region.sizeX), static_cast<float>(region.sizeY),
                              static_cast<float>(region.sizeZ)});
    return 3;
}

// The inverse: `corner` names the first voxel (the one it falls in), `size`
// how many on each axis, and the two tables are laid out as `ReadVoxels`
// returns them. A material of zero, or an occupancy of zero, is air.
int methodTerrainWriteVoxels(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 corner = checkPlace(L, 2);
    const core::Vec3 size = checkPlace(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    luaL_checktype(L, 5, LUA_TTABLE);
    scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        lua_pushinteger(L, 0);
        return 1;
    }
    // **Each side a count before it is an integer** (terrain audit TA5): a
    // NaN, an infinity or a side past the largest region was cast to an `i32`
    // as it was -- undefined behaviour -- and is refused instead.
    for (const float side : {size.x, size.y, size.z}) {
        if (!std::isfinite(side) || side < 0.0f || side > static_cast<float>(MaxVoxelRegion)) {
            const core::I18nArg args[] = {{"x", static_cast<core::f64>(size.x)},
                                          {"y", static_cast<core::f64>(size.y)},
                                          {"z", static_cast<core::f64>(size.z)}};
            raise(L, ENG_TR("scene.err.terrain_voxels_size"), args);
        }
    }
    {
        const float voxel = terrain->field.settings().voxelSize;
        groundFirst(L, *terrain, corner, core::Vec3{corner.x + size.x * voxel, corner.y, corner.z + size.z * voxel});
    }
    VoxelRegion region;
    const core::DVec3 local = intoField(*terrain, corner);
    region.x = terrain->field.voxelIndex(local.x);
    region.y = terrain->field.voxelIndex(local.y);
    region.z = terrain->field.voxelIndex(local.z);
    region.sizeX = static_cast<core::i32>(std::floor(size.x));
    region.sizeY = static_cast<core::i32>(std::floor(size.y));
    region.sizeZ = static_cast<core::i32>(std::floor(size.z));
    const auto materialCount = static_cast<core::usize>(lua_objlen(L, 4));
    const auto occupancyCount = static_cast<core::usize>(lua_objlen(L, 5));
    if (region.count() > MaxVoxelRegion || materialCount != region.count() || occupancyCount != region.count()) {
        const core::I18nArg args[] = {{"count", static_cast<core::i64>(std::min<core::usize>(
                                                    region.count(), std::numeric_limits<core::i64>::max()))},
                                      {"materials", static_cast<core::i64>(materialCount)},
                                      {"occupancies", static_cast<core::i64>(occupancyCount)}};
        raise(L, ENG_TR("scene.err.terrain_voxels_shape"), args);
    }
    // Every occupancy a number before any voxel is written: a refusal half
    // way through would leave half an edit.
    for (int index = 1; index <= static_cast<int>(region.count()); ++index) {
        lua_rawgeti(L, 5, index);
        const bool number = lua_isnumber(L, -1) != 0;
        const double occupancy = number ? lua_tonumber(L, -1) : 0.0;
        lua_pop(L, 1);
        if (number && !std::isfinite(occupancy)) {
            const core::I18nArg args[] = {{"index", static_cast<core::i64>(index)}, {"value", occupancy}};
            raise(L, ENG_TR("scene.err.terrain_voxels_value"), args);
        }
    }
    asset::FieldWriter writer(terrain->field);
    int at = 1;
    for (core::i32 z = 0; z < region.sizeZ; ++z) {
        for (core::i32 y = 0; y < region.sizeY; ++y) {
            for (core::i32 x = 0; x < region.sizeX; ++x) {
                lua_rawgeti(L, 4, at);
                const lua_Integer material = lua_isnumber(L, -1) != 0 ? lua_tointeger(L, -1) : 0;
                lua_pop(L, 1);
                lua_rawgeti(L, 5, at);
                const double occupancy = lua_isnumber(L, -1) != 0 ? lua_tonumber(L, -1) : 0.0;
                lua_pop(L, 1);
                ++at;
                const auto id8 = static_cast<core::u8>(std::clamp<lua_Integer>(material, 0, 255));
                (void)writer.set(region.x + x, region.y + y, region.z + z,
                                 asset::Voxel{asset::quantiseOccupancy(static_cast<float>(occupancy)), id8});
            }
        }
    }
    writer.finish();
    asset::EditReport report;
    report.touched = writer.changed();
    return finishEdit(L, *terrain, report);
}

// Which voxel a world position falls in, as its index on each axis.
int methodTerrainWorldToCell(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 position = checkVector3(L, 2);
    const scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        pushVector3(L, core::Vec3{});
        return 1;
    }
    const core::DVec3 local = intoField(*terrain, position);
    pushVector3(L, core::Vec3{static_cast<float>(terrain->field.voxelIndex(local.x)),
                              static_cast<float>(terrain->field.voxelIndex(local.y)),
                              static_cast<float>(terrain->field.voxelIndex(local.z))});
    return 1;
}

// The world position of a voxel's centre, from its index on each axis.
int methodTerrainCellCenterToWorld(lua_State* L)
{
    const core::InstanceId id = liveInstance(L, 1);
    const core::Vec3 cell = checkVector3(L, 2);
    const scene::TerrainComponent* terrain = world(L).terrains().find(id);
    if (terrain == nullptr) {
        pushVector3(L, core::Vec3{});
        return 1;
    }
    const auto index = [](float value) { return static_cast<core::i32>(std::floor(value)); };
    pushVector3(L, core::Vec3{static_cast<float>(terrain->field.voxelCenter(index(cell.x)) + terrain->origin.x),
                              static_cast<float>(terrain->field.voxelCenter(index(cell.y)) + terrain->origin.y),
                              static_cast<float>(terrain->field.voxelCenter(index(cell.z)) + terrain->origin.z)});
    return 1;
}

constexpr InstanceMethodBinding InstanceMethods[] = {
    {"Instance", "FindFirstChild", methodFindFirstChild},
    {"Instance", "FindFirstChildOfClass", methodFindFirstChildOfClass},
    {"Instance", "FindFirstChildWhichIsA", methodFindFirstChildWhichIsA},
    {"Instance", "FindFirstAncestor", methodFindFirstAncestor},
    {"Instance", "GetFullName", methodGetFullName},
    {"Instance", "FindFirstAncestorOfClass", methodFindFirstAncestorOfClass},
    {"Instance", "GetChildren", methodGetChildren},
    {"Instance", "GetDescendants", methodGetDescendants},
    {"Instance", "IsA", methodIsA},
    {"Instance", "IsAncestorOf", methodIsAncestorOf},
    {"Instance", "IsDescendantOf", methodIsDescendantOf},
    {"Instance", "GetStamp", methodGetStamp},
    {"Instance", "Clone", methodClone},
    {"Instance", "Destroy", methodDestroy},
    {"Instance", "GetAttribute", methodGetAttribute},
    {"Instance", "SetAttribute", methodSetAttribute},
    {"Instance", "GetAttributes", methodGetAttributes},
    {"Instance", "GetPropertyChangedSignal", methodGetPropertyChangedSignal},
    {"Instance", "GetAttributeChangedSignal", methodGetAttributeChangedSignal},
    {"Instance", "AddTag", methodAddTag},
    {"Instance", "RemoveTag", methodRemoveTag},
    {"Instance", "HasTag", methodHasTag},
    {"Instance", "GetTags", methodGetTags},
    {"PVInstance", "GetPivot", methodGetPivot},
    {"PVInstance", "PivotTo", methodPivotTo},
    {"Model", "GetExtentsSize", methodGetExtentsSize},
    {"BasePart", "ApplyImpulse", methodApplyImpulse},
    {"Swarm", "AddAgent", methodSwarmAddAgent},
    {"Swarm", "RemoveAgent", methodSwarmRemoveAgent},
    {"Swarm", "SetAgentSpeed", methodSwarmSetAgentSpeed},
    {"Swarm", "SetAgentPosition", methodSwarmSetAgentPosition},
    {"Swarm", "Push", methodSwarmPush},
    {"Swarm", "GetAgentPosition", methodSwarmGetAgentPosition},
    {"Swarm", "GetAgents", methodSwarmGetAgents},
    {"Swarm", "GetPositions", methodSwarmGetPositions},
    {"Swarm", "QueryRadius", methodSwarmQueryRadius},
    {"Swarm", "AddObstacle", methodSwarmAddObstacle},
    {"Swarm", "ClearObstacles", methodSwarmClearObstacles},
    {"BasePart", "SetNetworkOwner", methodSetNetworkOwner},
    {"BasePart", "GetRenderCFrame", methodGetRenderCFrame},
    {"Attachment", "GetRenderCFrame", methodGetRenderCFrame},
    {"Camera", "GetRenderCFrame", methodGetRenderCFrame},
    {"Camera", "WorldToViewportPoint", methodWorldToViewportPoint},
    {"Camera", "ViewportPointToRay", methodViewportPointToRay},
    {"Camera", "ViewportPointToWorld2D", methodViewportPointToWorld2D},
    {"BasePart", "ApplyImpulseAtPosition", methodApplyImpulseAtPosition},
    {"BasePart", "ApplyAngularImpulse", methodApplyAngularImpulse},
    {"Constraint", "GetForce", methodConstraintGetForce},
    {"Constraint", "GetTorque", methodConstraintGetTorque},
    {"Constraint", "GetMotorForce", methodConstraintGetMotorForce},
    {"Constraint", "GetMotorTorque", methodConstraintGetMotorTorque},
    {"Water", "GetHeightAt", methodWaterGetHeightAt},
    {"Water", "GetNormalAt", methodWaterGetNormalAt},
    {"Water", "Carve", methodWaterCarve},
    {"BasePart", "GetNetworkOwner", methodGetNetworkOwner},
    {"BasePart", "SetMaterialParameter", methodSetMaterialParameter},
    {"TextInput", "CaptureFocus", methodTextInputCaptureFocus},
    {"TextInput", "ReleaseFocus", methodTextInputReleaseFocus},
    {"TextInput", "IsFocused", methodTextInputIsFocused},
    {"UIPageLayout", "Next", methodPageLayoutNext},
    {"UIPageLayout", "Previous", methodPageLayoutPrevious},
    {"UIPageLayout", "JumpTo", methodPageLayoutJumpTo},
    {"UIPageLayout", "JumpToIndex", methodPageLayoutJumpToIndex},
    {"BasePart", "GetMaterialParameter", methodGetMaterialParameter},
    {"BasePart", "ClearMaterialParameter", methodClearMaterialParameter},
    {"Part2D", "ApplyImpulse", methodPart2DApplyImpulse},
    {"Tilemap2D", "SetCell", methodTilemapSetCell},
    {"Tilemap2D", "GetCell", methodTilemapGetCell},
    {"Tilemap2D", "FillRect", methodTilemapFillRect},
    {"Tilemap2D", "Clear", methodTilemapClear},
    {"CharacterBody", "Move", methodCharacterMove},
    {"CharacterBody", "Jump", methodCharacterJump},
    {"Ragdoll", "Build", methodRagdollBuild},
    {"Terrain", "FillBall", methodTerrainFillBall},
    {"Terrain", "GrowBall", methodTerrainGrowBall},
    {"Terrain", "RaiseBall", methodTerrainRaiseBall},
    {"Terrain", "FillBlock", methodTerrainFillBlock},
    {"Terrain", "PaintBall", methodTerrainPaintBall},
    {"Terrain", "HeightAt", methodTerrainHeightAt},
    {"Terrain", "WriteHeights", methodTerrainWriteHeights},
    {"Terrain", "Clear", methodTerrainClear},
    {"Terrain", "GetLayers", methodTerrainGetLayers},
    {"Terrain", "SetLayers", methodTerrainSetLayers},
    {"Terrain", "GetRules", methodTerrainGetRules},
    {"FoliageLayer", "GetMaterials", methodFoliageLayerGetMaterials},
    {"FoliageLayer", "SetMaterials", methodFoliageLayerSetMaterials},
    {"Terrain", "SetRules", methodTerrainSetRules},
    {"Terrain", "ApplyRules", methodTerrainApplyRules},
    {"Terrain", "Compact", methodTerrainCompact},
    {"Terrain", "FillCylinder", methodTerrainFillCylinder},
    {"Terrain", "SmoothBall", methodTerrainSmoothBall},
    {"Terrain", "FlattenBall", methodTerrainFlattenBall},
    {"Terrain", "ReplaceMaterial", methodTerrainReplaceMaterial},
    {"Terrain", "ReadVoxels", methodTerrainReadVoxels},
    {"Terrain", "WriteVoxels", methodTerrainWriteVoxels},
    {"Terrain", "WorldToCell", methodTerrainWorldToCell},
    {"Terrain", "CellCenterToWorld", methodTerrainCellCenterToWorld},
};

} // namespace

void pushInstance(lua_State* L, core::InstanceId id)
{
    if (!id.valid()) {
        // nil rather than a dead handle: `nil` is the honest answer and the one
        // `Instance?` is typed for.
        lua_pushnil(L);
        return;
    }

    const VmContext& ctx = context(L);
    lua_getref(L, ctx.instanceCacheRef);
    const int cache = lua_gettop(L);

    // The slot index, not the whole id: a slot holds one live instance at a
    // time, so this is dense and lands in the table's array part. The generation
    // is what the hit is validated against.
    const int slot = static_cast<int>(id.index) + 1;
    lua_rawgeti(L, cache, slot);
    if (const core::InstanceId* cached = toInstance(L, -1); cached != nullptr && *cached == id) {
        lua_replace(L, cache);
        return;
    }
    lua_pop(L, 1);

    void* memory =
        lua_newuserdatataggedwithmetatable(L, sizeof(InstanceUserdata), static_cast<int>(UserdataTag::Instance));
    *static_cast<InstanceUserdata*>(memory) = InstanceUserdata{id};

    lua_pushvalue(L, -1);
    lua_rawseti(L, cache, slot);
    lua_replace(L, cache);
}

bool instanceHeld(lua_State* L, core::InstanceId id)
{
    if (!id.valid())
        return false;
    const VmContext& ctx = context(L);
    lua_getref(L, ctx.instanceCacheRef);
    lua_rawgeti(L, -1, static_cast<int>(id.index) + 1);
    const core::InstanceId* cached = toInstance(L, -1);
    const bool held = cached != nullptr && *cached == id;
    lua_pop(L, 2);
    return held;
}

const core::InstanceId* toInstance(lua_State* L, int index) noexcept
{
    const void* payload = lua_touserdatatagged(L, index, static_cast<int>(UserdataTag::Instance));
    return payload == nullptr ? nullptr : &static_cast<const InstanceUserdata*>(payload)->id;
}

core::InstanceId checkInstance(lua_State* L, int index)
{
    return liveInstance(L, index);
}

void bindInstanceMethods(lua_State* L, std::span<const InstanceMethodBinding> bindings)
{
    VmContext& ctx = context(L);
    World& w = *ctx.world;

    for (const InstanceMethodBinding& binding : bindings) {
        const ClassId classId = w.classes().findId(w.atoms().lookup(binding.className));
        const scene::MethodDesc* descriptor = w.classes().findMethod(classId, w.atoms().lookup(binding.methodName));
        if (descriptor == nullptr) {
            ++ctx.unboundDeclarations;
            continue;
        }
        ctx.instanceMethods.emplace(descriptor, binding.fn);
    }
}

MethodCoverage methodCoverage(lua_State* L)
{
    const VmContext& ctx = context(L);
    const World& w = *ctx.world;

    MethodCoverage coverage;
    coverage.bound = ctx.instanceMethods.size();
    coverage.boundWithoutDeclaration = ctx.unboundDeclarations;

    // Walks every class the registry holds and counts the declared methods with
    // no implementation. The other direction is counted as the bindings land,
    // because a stale entry has no descriptor to be found by.
    for (ClassId classId = 1; classId < static_cast<ClassId>(w.classes().classCount()); ++classId) {
        const scene::ClassDescriptor* descriptor = w.classes().find(classId);
        if (descriptor == nullptr)
            continue;
        for (const scene::MethodDesc& method : descriptor->methods) {
            ++coverage.declared;
            if (ctx.instanceMethods.find(&method) == ctx.instanceMethods.end())
                ++coverage.declaredWithoutBinding;
        }
    }
    return coverage;
}

void registerInstanceBinding(lua_State* L)
{
    VmContext& ctx = context(L);

    // Weak values, so a userdata nothing holds is collected and the cache does
    // not turn every instance a script has ever touched into a permanent one.
    // The keys are integers and are dropped with their values.
    lua_createtable(L, 0, 0);
    lua_createtable(L, 0, 1);
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setreadonly(L, -1, true);
    lua_setmetatable(L, -2);
    ctx.instanceCacheRef = lua_ref(L, -1);
    lua_pop(L, 1);

    lua_createtable(L, 0, 6);
    lua_pushstring(L, typeName(UserdataTag::Instance));
    lua_setfield(L, -2, "__type");
    lua_pushvalue(L, -1);
    lua_setuserdatametatable(L, static_cast<int>(UserdataTag::Instance));

    lua_pushcfunction(L, instanceIndex, "__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, instanceNewIndex, "__newindex");
    lua_setfield(L, -2, "__newindex");
    // Named `__namecall` on purpose: `laux.cpp:42` special-cases exactly this
    // debug name so an argument error reports the *method* rather than the
    // metamethod, which is free and is the difference between a usable message
    // and a confusing one.
    lua_pushcfunction(L, instanceNamecall, "__namecall");
    lua_setfield(L, -2, "__namecall");
    lua_pushcfunction(L, instanceEq, "__eq");
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, instanceTostring, "__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_setreadonly(L, -1, true);
    lua_pop(L, 1);

    bindInstanceMethods(L, InstanceMethods);

    const luaL_Reg constructors[] = {{"new", instanceNew}, {"stamp", instanceStamp}, {nullptr, nullptr}};
    luaL_register(L, "Instance", constructors);
    lua_setreadonly(L, -1, true);
    lua_pop(L, 1);
}

} // namespace engine::script
