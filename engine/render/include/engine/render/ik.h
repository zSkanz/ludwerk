// **Inverse kinematics** (ADR 0198): the solvers, and nothing of the world. A
// limb of two bones reaching a point, a chain of joints turning to look at one,
// and two feet put on the ground under them with the hips brought down to suit.
//
// **Everything is in MODEL space** -- the mesh's own, +Y up. A solver is handed
// the joints and a copy of a pose's model matrices (`Pose::model`: one matrix a
// joint, joint space to model space), and edits that array in place. Whoever
// calls it has already brought the target out of the world and cast the rays;
// a test hands it numbers.
//
// **What a solver moves, it moves rigidly, and carries what hangs from it.** A
// joint is turned about a point of model space and never rebuilt from a
// rotation, so whatever scale or skew its matrix carries is still in it
// afterwards; and every joint below one that moved keeps its transform
// relative to its parent. Joints are parents first, so that is one pass over
// the array however many joints a solver moved.
//
// **Picture, not simulation** (ADR 0198): this is run on the pose a frame is
// drawn with and nothing the tick reads. The arithmetic is the engine's own
// all the same (`core::dmath`), so one pose and one target are one answer on
// every machine.
//
// No solver allocates, and none answers with a NaN: a bone of no length, a
// target on the limb's first joint, a pole on the limb's line and a weight
// outside nought to one all have an answer below.
#pragma once

#include <span>

#include "engine/asset/model.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::render::ik {

// The most joints one look is shared between, the end joint among them: a
// head, a neck and a spine are five or six. A longer chain is taken as this
// many.
inline constexpr core::u32 MaxLookJoints = 16;

// Sets `joint`'s model matrix and carries everything below it.
//
// Both matrices are taken as affine. A joint whose matrix cannot be undone --
// one scaled to nothing -- has nothing to say about where its children were
// relative to it, and they are carried by how far it moved and no more.
// An index out of range changes nothing.
void place(std::span<const asset::Joint> joints, std::span<core::Mat4> model, core::u32 joint,
           const core::Mat4& placed) noexcept;

// A limb of two bones: `end` (a hand, a foot), its parent (the elbow, the
// knee) and that one's parent.
//
// Puts the end at `target`. With `rotation` the end takes that orientation;
// without it the end keeps the one it had in model space, whichever way the
// bone above it turned -- a foot stays flat while its shin leans.
//
// `pole` is a point the middle joint bends towards. Null keeps the plane the
// limb is already bent in, carried round with the limb as it swings. A limb
// that is perfectly straight and has no pole bends the way its rest pose
// bends, and if that is straight too, towards a fixed perpendicular -- for a
// limb that hangs straight down that is +Z, the way a glTF character faces, so
// a knee goes forward. A pole on the line from the limb's first joint to the
// target says nothing, and is taken as none.
//
// A target further than the limb is long: the limb is straight, pointing at
// it, the end at full reach. One nearer than the two bones can fold to: folded
// as far as they go, towards it.
//
// `weight` is clamped to nought to one. Nought changes nothing, bit for bit.
// Between, the end's target is that fraction of the way from where it is, its
// orientation that fraction of the way to `rotation`, and the bend that
// fraction of the way round to the pole.
//
// False, and nothing changed, when `end` has no parent and grandparent or an
// index is out of range. True otherwise, a weight of nought included.
bool solveTwoBone(std::span<const asset::Joint> joints, std::span<core::Mat4> model, core::u32 end, core::Vec3 target,
                  const core::Mat3* rotation, const core::Vec3* pole, core::f32 weight) noexcept;

// A look: turns `end` and up to `chainLength` joints above it (so nought is
// the end alone) so that `forward` -- a direction in model space, the way the
// end joint is taken to be facing now -- points at `target`.
//
// The turn is clamped to `maxAngle` radians from `forward`, scaled by
// `weight`, and shared EQUALLY between the joints, the topmost first, each
// turning about its own position and carrying what is below it; all about one
// axis, so the parts add up to the whole turn.
//
// **The end faces the target from where the turn leaves it**, not from where
// it began: a spine that turns moves the head, and a head aimed from its old
// place looks past what it was given. So the turn is the one that is right
// once it has been made, found by aiming again from where the last aim put
// the end, a fixed few times.
//
// A target at the end joint itself, a `forward` of no length, and a weight or
// a `maxAngle` of nought change nothing. A target straight behind is turned to
// about +Y.
//
// False, and nothing changed, when `end` is out of range.
bool solveLook(std::span<const asset::Joint> joints, std::span<core::Mat4> model, core::u32 end, core::u32 chainLength,
               core::Vec3 target, core::Vec3 forward, core::f32 maxAngle, core::f32 weight) noexcept;

struct Foot
{
    // The ankle.
    core::u32 joint = 0;
    // Whether a ray found ground under it.
    bool hit = false;
    // That ground's height (model-space y) under the ankle.
    core::f32 ground = 0.0f;
    // And its normal, model space.
    core::Vec3 normal{0.0f, 1.0f, 0.0f};
};

struct FeetResult
{
    // Whether each foot was put on its ground, rather than left to the clip.
    bool leftPlanted = false;
    bool rightPlanted = false;
    // How far the hips were moved, along y.
    core::f32 hipsOffset = 0.0f;
};

// Two feet and the hips. `clipGround` is the height (model-space y) the clips
// were made to stand on; a caller that counts from the sole takes the foot's
// height off both that and each `ground`, and it is never needed here.
//
// A foot with a hit whose ground is within `stepHeight` of `clipGround` is
// PLANTED: its ankle goes up or down by (ground - clipGround), keeping
// whatever height above the ground the clip gave it -- a foot the clip lifted
// stays lifted by the same amount. A foot with no hit, or one further off than
// `stepHeight`, is left to the clip: its offset is nought. So is one whose leg
// is not two bones hanging below `hips`.
//
// The hips move by the SMALLER of the two offsets, so that the lower foot's
// leg does not have to stretch and the higher one bends. Then each planted
// foot's leg is solved as a two-bone limb to its own ankle, keeping the plane
// its knee bends in, the foot keeping the orientation the clip gave it. Then,
// with `alignToSlope`, a planted foot is turned about its ankle by the
// rotation that takes +Y to its ground's normal.
//
// All of it scaled by `weight`, clamped to nought to one; at nought nothing
// is changed and nothing is planted. Nothing is changed either when `hips` is
// out of range.
FeetResult solveFeet(std::span<const asset::Joint> joints, std::span<core::Mat4> model, core::u32 hips,
                     const Foot& left, const Foot& right, core::f32 clipGround, core::f32 stepHeight, bool alignToSlope,
                     core::f32 weight) noexcept;

} // namespace engine::render::ik
