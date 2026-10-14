// Backend constructors (ADR 0023), the same shape `rhi/backends.h` has.
//
// No plugin ABI and no self-registering static: consoles and iOS want a fully
// static, dead-strippable binary, and a registry defeats both. A backend the
// build did not compile is not declared here, so selecting it is a compile
// error rather than a null at startup.
#pragma once

#include <memory>

#include "engine/core/error.h"
#include "engine/physics/physics.h"
#include "engine/physics/physics2d.h"

namespace engine::physics {

using PhysicsResult = std::unique_ptr<IPhysics3D>;

#if ENG_PHYSICS_JOLT
// Null on failure, with `outError` filled when it is not null. Initialises
// Jolt's process-wide state (allocator, factory, type registry) on the first
// call and tears it down when the last instance is destroyed -- which is why
// this is a creator rather than a constructor a caller could invoke twice.
[[nodiscard]] PhysicsResult createJoltPhysics(core::EngineError* outError = nullptr);
#endif

using Physics2DResult = std::unique_ptr<IPhysics2D>;

#if ENG_PHYSICS_BOX2D
// The 2D backend (ADR 0008). Box2D keeps no process-wide state, so unlike
// Jolt's this could be a constructor; it is a creator for the same reason the
// rest of this file is -- no header outside the backend sees a Box2D type.
[[nodiscard]] Physics2DResult createBox2DPhysics();
#endif

} // namespace engine::physics
