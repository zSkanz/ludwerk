// A part pressed and prompted without code (ADR 0126): `ClickDetector` and
// `ProximityPrompt`, resolved once a tick from the pointer, the keys and the
// local character, and fired with the player. In a match the machine that
// acted fires at once and tells the authority, which checks and fires there.
#pragma once

#include <span>
#include <vector>

#include "engine/core/id.h"
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
};

// Once a tick, after input is dispatched and before scripts see its events:
// the authority's inbox checked and fired, then this machine's pointer, keys
// and taps.
void stepDetectors(lua_State* L, core::f64 dt, std::span<const input::RawInputEvent> events);

} // namespace engine::script
