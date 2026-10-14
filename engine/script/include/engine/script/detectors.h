// A part pressed without code (ADR 0126): `ClickDetector`, the engine's pointer
// picking, resolved once a tick from the pointer and the local character, and
// fired with the player. In a match the machine that clicked fires at once and
// tells the authority, which checks and fires there. A prompt and a world drag
// are the game's own code (withdrawn 2026-10-03).
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
};

// Once a tick, after input is dispatched and before scripts see its events:
// the authority's inbox checked and fired, then this machine's pointer, keys
// and taps.
void stepDetectors(lua_State* L, core::f64 dt, std::span<const input::RawInputEvent> events);

} // namespace engine::script
