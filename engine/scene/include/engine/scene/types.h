// The scalar vocabulary, imported into `engine::scene` once.
//
// Same role as `engine/rhi/types.h`: engine code spells widths rather than
// platform ints, because these structs are snapshotted, hashed and compared for
// determinism (ADR 0025), and a layout that depends on how wide `int` happens
// to be is a layout that reproduces on one machine and not another.
#pragma once

#include "engine/core/types.h"

namespace engine::scene {

using core::f32;
using core::f64;
using core::i32;
using core::i64;
using core::u16;
using core::u32;
using core::u64;
using core::u8;
using core::usize;

} // namespace engine::scene
