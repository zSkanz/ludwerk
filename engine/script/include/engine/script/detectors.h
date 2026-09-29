// A part pressed, prompted and dragged without code (ADR 0126):
// `ClickDetector`, `ProximityPrompt` and `DragDetector`, resolved once a tick
// from the pointer, the keys and the local character, and fired with the
// player. In a match the machine that acted fires at once and tells the
// authority, which checks and fires there.
#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

struct lua_State;

namespace engine::input {
struct RawInputEvent;
}

namespace engine::script {

// What this machine is doing with them, from one tick to the next. Never the
// world's: it is how this player is pointing and pressing.
struct DetectorState
{
    // The `ClickDetector` under the pointer, within reach.
    core::InstanceId hovered;
    // A prompt's key or finger down: which, since when, and whether it has
    // triggered yet.
    struct Hold
    {
        core::InstanceId prompt;
        core::i32 keyCode = 0;
        core::i32 touchId = 0;
        core::f64 seconds = 0.0;
        bool triggered = false;
    };
    std::vector<Hold> holds;
    // The prompts shown last tick, for `PromptShown` and `PromptHidden`.
    std::vector<core::InstanceId> shown;

    // A drag under way: what is dragged, by whom, and how it was taken hold
    // of -- everything a tick needs to work out where the part goes from the
    // pointer's ray alone, which is what lets the authority do the same from
    // the rays a replica sends it.
    struct Drag
    {
        core::InstanceId detector;
        core::InstanceId player;
        // The part or the model it moves, and the part whose frame the drag
        // is worked in: the part itself, or the model's primary part.
        core::InstanceId moved;
        core::InstanceId handle;
        // A finger's, or 0 for the mouse; and where that finger last was.
        core::i32 touchId = 0;
        core::Vec2 pointer{};
        // Where the part was grabbed, and the handle's frame then.
        core::DVec3 grab;
        core::CFrameD start;
        // The pointer's direction as it began: a view plane's normal, and a
        // trackball's.
        core::Vec3 startDirection{0.0f, 0.0f, -1.0f};
        // A turn's angle from where the part rests, as it began.
        core::f64 startAngle = 0.0;
        // The authority's, for a `Physical` drag it handed to the player.
        core::u32 previousOwner = 0;
        bool handedOver = false;
    };
    // This machine's own, and on the authority each player's.
    std::optional<Drag> drag;
    std::vector<Drag> remoteDrags;
};

// Once a tick, after input is dispatched and before scripts see its events:
// the authority's inbox checked and fired, then this machine's pointer, keys
// and taps.
void stepDetectors(lua_State* L, core::f64 dt, std::span<const input::RawInputEvent> events);

} // namespace engine::script
