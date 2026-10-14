// `ColorSequence`, `NumberSequence` and their keypoints, the Luau data types
// (ADR 0110).
//
// Values, like `Color3`: immutable, compared by value, and built from the
// module named after each (`ColorSequence.new`, `ColorSequenceKeypoint.new`).
// A sequence owns its stops, so its tag has a destructor; a keypoint is a few
// numbers and needs none.
#pragma once

#include "engine/core/sequence.h"

struct lua_State;

namespace engine::script {

void registerSequenceTypes(lua_State* L);

void pushColorSequence(lua_State* L, const core::ColorSequence& sequence);
void pushNumberSequence(lua_State* L, const core::NumberSequence& sequence);

// The sequence at `index`, or null for anything that is not one.
[[nodiscard]] const core::ColorSequence* toColorSequence(lua_State* L, int index) noexcept;
[[nodiscard]] const core::NumberSequence* toNumberSequence(lua_State* L, int index) noexcept;

} // namespace engine::script
