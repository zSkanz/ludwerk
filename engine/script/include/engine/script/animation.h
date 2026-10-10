// `AnimationPlayer:LoadAnimation` and the `AnimationTrack` handle
// (api-design.md §2.2).
//
// The runtime is not here. Clip data belongs to `render`, which owns the loaded
// skeleton, and `script` may not include it -- so what this file talks to is
// `scene::AnimationHost`, the same injected-pointer arrangement the physics
// queries use for `PhysicsSync`. Null is a real state: a build with no render
// module has no host, and `LoadAnimation` then answers with a track that plays
// nothing rather than with an error.
//
// A track is a HANDLE and not an Instance, exactly as `Tween` is: nothing
// parents one and nothing finds one in the tree. Unlike `Tween` it is also not a
// value type -- `track.Looped = true` names the one track every holder of that
// handle can see -- which is why this is the first datatype in the surface with
// writable members.
#pragma once

#include <span>
#include <vector>

#include "engine/core/types.h"
#include "engine/scene/animation_host.h"
#include "engine/script/signals.h"

struct lua_State;

namespace engine::script {

using core::u32;

// What one `AnimationTrack` userdata points at. The `Ended` signal lives here
// rather than in the host, because a signal is a `script` thing and the host is
// one layer down.
struct TrackRecord
{
    scene::TrackId track = 0;
    SignalId ended;
};

// `AnimationPlayer:LoadAnimation(content)`. Bound through the service method
// table like every other instance method; declared here because that table lives
// in `services.cpp` and the implementation belongs beside the track.
int animationPlayerLoadAnimation(lua_State* L);

// Installs the `AnimationTrack` metatable and its member tables. Runs at boot
// with the other datatypes, before the sandbox.
void registerAnimationTypes(lua_State* L);

// Fires `Ended` for each track the host reported finished this tick. Called by
// the host right after `AnimationHost::sample`, so the signal is enqueued in the
// same drain as everything else that happened on that tick.
void fireAnimationEnded(lua_State* L, std::span<const scene::TrackId> ended);

// --- Animation graphs (ADR 0197) ------------------------------------------------
//
// `AnimationPlayer:SetParameter`, `:GetParameter`, `:ClearParameter` and
// `:GetState`, bound through the service method table like `LoadAnimation`.
int animationPlayerSetParameter(lua_State* L);
int animationPlayerGetParameter(lua_State* L);
int animationPlayerClearParameter(lua_State* L);
int animationPlayerGetState(lua_State* L);

// What the graphs did in the tick just sampled, as `StateChanged` and
// `EventReached` on each player: deferred, in the order the host hands them.
void fireGraphSignals(lua_State* L, std::span<const scene::GraphSignal> signals);

} // namespace engine::script
