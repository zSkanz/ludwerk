#include "engine/render/ik.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "engine/core/dmath.h"

namespace engine::render::ik {

namespace {

using core::f32;
using core::Mat3;
using core::Mat4;
using core::u32;
using core::u8;
using core::usize;
using core::Vec3;

// A length, a sine or an angle below this is nought: there is no direction in
// it to turn by.
constexpr f32 Tiny = 1e-6f;
constexpr f32 Pi = 3.14159265358979f;
constexpr Vec3 Up{0.0f, 1.0f, 0.0f};

// How many times a look is aimed again from where the last aim left the end.
// Each takes off what the one before missed by, and what it missed by is the
// chain's length against the target's distance -- a tenth, for a head looking
// across a room -- so this is far more than is needed there, and a bound on
// the work for a target close enough that the aims never settle.
constexpr int LookPasses = 8;

// --- Moving joints, and carrying what hangs from them -----------------------

// A motion of model space: a point `p` goes to `linear * p + offset`.
//
// **A solver's whole answer is a few of these, each said against the pose as
// it was handed in.** A joint is never rebuilt from a rotation and a place:
// its matrix is moved, so the scale and the skew an exporter left in it are
// still there, and nothing has to be taken apart to keep them.
struct Motion
{
    Mat3 linear{};
    Vec3 offset{};
};

[[nodiscard]] Vec3 originOf(const Mat4& m) noexcept
{
    return {m.m[3][0], m.m[3][1], m.m[3][2]};
}

// The motion that turns by `turn` and takes the point `from` to `to`: a joint
// at `from` ends at `to`, turned, and so does everything rigid with it.
[[nodiscard]] Motion turning(const Mat3& turn, Vec3 from, Vec3 to) noexcept
{
    return {turn, to - turn * from};
}

[[nodiscard]] bool isFinite(const Motion& by) noexcept
{
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            if (!std::isfinite(by.linear.m[c][r]))
                return false;
    return std::isfinite(by.offset.x) && std::isfinite(by.offset.y) && std::isfinite(by.offset.z);
}

// `by` then nothing else: the 4x4 product with the motion's own last row left
// out, since it is (0, 0, 0, 1) and the general product would spend a third of
// its multiplications finding that out.
[[nodiscard]] Mat4 apply(const Motion& by, const Mat4& m) noexcept
{
    Mat4 out;
    for (int c = 0; c < 4; ++c) {
        const Vec3 moved = by.linear * Vec3{m.m[c][0], m.m[c][1], m.m[c][2]} + by.offset * m.m[c][3];
        out.m[c][0] = moved.x;
        out.m[c][1] = moved.y;
        out.m[c][2] = moved.z;
        out.m[c][3] = m.m[c][3];
    }
    return out;
}

// One joint a solver moved, and the motion that takes it -- and what hangs
// from it -- from where the pose had it to where it now is.
struct Carried
{
    u32 joint = 0;
    Motion by;
};

// How many joints, counted from the first one moved, have the answer to "which
// motion carries you" kept on the stack. Four times the joints a skin can be
// drawn with; a rig past it is still carried right, by the slower way below.
constexpr usize CarryWindow = 256;
constexpr u8 NotCarried = 0xFF;

// **The one place a pose is written.** Every joint named in `moved` takes its
// motion, and every other joint takes the motion of the nearest joint above it
// that is named -- which is what "a descendant keeps its transform relative to
// its parent" comes to when the motions are all said against the pose as it
// was. Joints are parents first, so a joint's parent has its answer before the
// joint asks, and one forward pass from the first moved joint does all of it
// however many joints moved.
//
// A motion that is not finite moves nothing at all: a pose left as the clip
// had it is a frame nobody notices, and a NaN in a palette is a character
// that vanishes.
void carry(std::span<const asset::Joint> joints, std::span<Mat4> model, std::span<const Carried> moved) noexcept
{
    const usize count = std::min(joints.size(), model.size());
    if (moved.empty() || moved.size() >= NotCarried)
        return;
    usize first = count;
    for (const Carried& one : moved) {
        if (one.joint >= count || !isFinite(one.by))
            return;
        first = std::min<usize>(first, one.joint);
    }

    std::array<u8, CarryWindow> tags;
    tags.fill(NotCarried);
    const usize kept = std::min(count - first, CarryWindow);
    for (usize index = 0; index < moved.size(); ++index) {
        const usize at = moved[index].joint - first;
        if (at < kept)
            tags[at] = static_cast<u8>(index);
    }

    // For a joint past the window: up its parents until one is named, or one
    // is inside the window and knows.
    const auto walked = [&](usize joint) noexcept -> u8 {
        for (;;) {
            for (usize index = 0; index < moved.size(); ++index)
                if (moved[index].joint == joint)
                    return static_cast<u8>(index);
            const u32 parent = joints[joint].parent;
            if (parent == asset::Joint::NoParent || parent >= joint || parent < first)
                return NotCarried;
            joint = parent;
            if (joint - first < kept)
                return tags[joint - first];
        }
    };

    for (usize joint = first; joint < count; ++joint) {
        u8 tag = NotCarried;
        if (joint - first < kept) {
            tag = tags[joint - first];
            if (tag == NotCarried) {
                const u32 parent = joints[joint].parent;
                if (parent != asset::Joint::NoParent && parent >= first && parent < joint)
                    tag = tags[parent - first];
                tags[joint - first] = tag;
            }
        }
        else {
            tag = walked(joint);
        }
        if (tag != NotCarried)
            model[joint] = apply(moved[tag].by, model[joint]);
    }
}

// --- Directions and turns ----------------------------------------------------

// The part of `v` that is across `axis`, a unit vector.
[[nodiscard]] Vec3 across(Vec3 v, Vec3 axis) noexcept
{
    return v - axis * core::dot(v, axis);
}

// `v` at unit length, or nought for one too short to have a direction.
[[nodiscard]] Vec3 unit(Vec3 v) noexcept
{
    const f32 size = core::length(v);
    return size > Tiny ? v * (1.0f / size) : Vec3{};
}

// A unit vector across `direction`, the same one for the same direction.
[[nodiscard]] Vec3 anyAcross(Vec3 direction) noexcept
{
    const Vec3 other = std::abs(direction.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
    return unit(core::cross(direction, other));
}

// The rotation that turns `from` onto `to`, both unit vectors, by the shortest
// way -- or `share` of that way. Opposite vectors have no shortest way; any
// half turn about an axis across them is one, and that is what is given. A
// vector of no length turns nothing.
//
// The angle is taken from the sine and the cosine together. From the cosine
// alone a turn of a thousandth of a radian is lost in the rounding of a number
// next to one, and a foot on a gentle slope is turned by exactly such angles.
[[nodiscard]] Mat3 swing(Vec3 from, Vec3 to, f32 share = 1.0f) noexcept
{
    const Vec3 axis = core::cross(from, to);
    const f32 sine = core::length(axis);
    const f32 cosine = core::dot(from, to);
    if (!(sine > Tiny)) {
        if (!(cosine < 0.0f))
            return Mat3{};
        return core::fromAxisAngle(anyAcross(from), Pi * share);
    }
    return core::fromAxisAngle(axis * (1.0f / sine), core::dmath::atan2(sine, cosine) * share);
}

// Which way a joint is turned, out of a matrix that may carry scale: the
// columns are measured before they are trusted. A joint flattened along one
// axis still has the turn its other two say.
[[nodiscard]] Mat3 rotationOf(const Mat4& m) noexcept
{
    Vec3 x{m.m[0][0], m.m[0][1], m.m[0][2]};
    Vec3 y{m.m[1][0], m.m[1][1], m.m[1][2]};
    Vec3 z{m.m[2][0], m.m[2][1], m.m[2][2]};
    if (!(core::length(z) > Tiny))
        z = core::cross(x, y);
    if (!(core::length(y) > Tiny))
        y = core::cross(z, x);
    // `orthonormalize` builds from the last two columns and never reads the
    // first.
    Mat3 basis;
    basis.m[1][0] = y.x;
    basis.m[1][1] = y.y;
    basis.m[1][2] = y.z;
    basis.m[2][0] = z.x;
    basis.m[2][1] = z.y;
    basis.m[2][2] = z.z;
    return core::orthonormalize(basis);
}

[[nodiscard]] f32 share01(f32 weight) noexcept
{
    return std::clamp(weight, 0.0f, 1.0f);
}

// --- A limb of two bones -----------------------------------------------------

struct Limb
{
    u32 root = 0;
    u32 mid = 0;
    u32 end = 0;
};

// False for an end joint with no parent and grandparent in the first `count`
// joints.
[[nodiscard]] bool limbOf(std::span<const asset::Joint> joints, usize count, u32 end, Limb& limb) noexcept
{
    if (end >= count)
        return false;
    const u32 mid = joints[end].parent;
    if (mid == asset::Joint::NoParent || mid >= end)
        return false;
    const u32 root = joints[mid].parent;
    if (root == asset::Joint::NoParent || root >= mid)
        return false;
    limb = Limb{root, mid, end};
    return true;
}

// The axis a limb's middle joint bends about in its rest pose, in model space
// as the limb's first joint is turned NOW: a knee is a hinge fixed in its
// thigh, and it goes where the thigh goes. Nought for a limb that rests
// straight.
[[nodiscard]] Vec3 restHinge(std::span<const asset::Joint> joints, std::span<const Mat4> model,
                             const Limb& limb) noexcept
{
    const Vec3 upper = core::toVec3(joints[limb.mid].localBind.position);
    const Vec3 lower = joints[limb.mid].localBind.rotation * core::toVec3(joints[limb.end].localBind.position);
    const Vec3 hinge = core::cross(upper, lower);
    if (!(core::length(hinge) > 1e-4f * core::length(upper) * core::length(lower)))
        return Vec3{};
    return core::transformDirection(model[limb.root], hinge);
}

// A limb solved: how its upper bone turned, how its lower bone turned (the
// upper's turn in it), and where its middle and its end are.
struct Bent
{
    Mat3 upper{};
    Mat3 lower{};
    Vec3 mid{};
    Vec3 end{};
};

// The limb whose joints are at `a`, `b` and `c`, with its end put at `target`
// or as near as two bones reach.
//
// **The places are worked out first and the turns from them.** The triangle
// the three joints make is known from its sides -- two bones and the distance
// to the target -- so where the middle joint goes is one cosine and the side
// it bends to. The turns are then the shortest that take each bone from where
// it points to where it must: nothing is iterated and nothing can fail to
// arrive.
//
// The joints' places are passed rather than read, because the feet solve a
// leg whose hips have already been lowered and have not yet been written.
[[nodiscard]] Bent bend(std::span<const asset::Joint> joints, std::span<const Mat4> model, const Limb& limb, Vec3 a,
                        Vec3 b, Vec3 c, Vec3 target, const Vec3* pole, f32 poleShare) noexcept
{
    const Vec3 upperBone = b - a;
    const Vec3 lowerBone = c - b;
    const f32 upperLength = core::length(upperBone);
    const f32 lowerLength = core::length(lowerBone);
    const Vec3 toTarget = target - a;
    const f32 distance = core::length(toTarget);
    const Vec3 stretch = c - a;
    const f32 stretchLength = core::length(stretch);

    // Which way the limb points. A target on the first joint itself names no
    // direction, and the limb folds the way it already points.
    Vec3 direction = Up;
    if (distance > Tiny)
        direction = toTarget * (1.0f / distance);
    else if (stretchLength > Tiny)
        direction = stretch * (1.0f / stretchLength);
    else if (upperLength > Tiny)
        direction = upperBone * (1.0f / upperLength);

    // Which side the middle joint is on. The plane the limb is bent in now,
    // swung round with the limb to where it will point: that is what keeps a
    // knee from rolling as its foot is raised.
    Vec3 side{};
    const f32 straight = 1e-4f * (upperLength + lowerLength);
    if (stretchLength > Tiny) {
        const Vec3 along = stretch * (1.0f / stretchLength);
        const Vec3 out = across(upperBone, along);
        if (core::length(out) > straight)
            side = unit(across(swing(along, direction) * out, direction));
    }
    else {
        const Vec3 out = across(upperBone, direction);
        if (core::length(out) > straight)
            side = unit(out);
    }
    // A straight limb is in no plane. Its rest pose is asked; the middle joint
    // sits to the side of the line that the hinge, crossed with the line, says.
    if (side == Vec3{})
        side = unit(core::cross(direction, restHinge(joints, model, limb)));
    if (side == Vec3{})
        side = anyAcross(direction);
    if (pole != nullptr) {
        const Vec3 towards = across(*pole - a, direction);
        if (core::length(towards) > Tiny) {
            const Vec3 polar = unit(towards);
            const Vec3 mixed = side * (1.0f - poleShare) + polar * poleShare;
            side = core::length(mixed) > Tiny ? unit(mixed) : polar;
        }
    }

    // The triangle. Past full reach it is a line, and is made one exactly
    // rather than by a cosine that rounds to a hair under one.
    const f32 longest = upperLength + lowerLength;
    f32 reach = longest;
    f32 cosine = 1.0f;
    f32 sine = 0.0f;
    if (distance < longest) {
        reach = std::max(distance, std::abs(upperLength - lowerLength));
        const f32 twice = 2.0f * upperLength * reach;
        cosine = twice > Tiny
                     ? std::clamp((upperLength * upperLength + reach * reach - lowerLength * lowerLength) / twice,
                                  -1.0f, 1.0f)
                     : 0.0f;
        sine = std::sqrt(std::max(0.0f, 1.0f - cosine * cosine));
    }

    Bent bent;
    bent.mid = a + direction * (upperLength * cosine) + side * (upperLength * sine);
    bent.end = a + direction * reach;
    if (upperLength > Tiny)
        bent.upper = swing(upperBone * (1.0f / upperLength), unit(bent.mid - a));
    bent.lower = bent.upper;
    if (lowerLength > Tiny)
        bent.lower = swing(unit(bent.upper * lowerBone), unit(bent.end - bent.mid)) * bent.upper;
    return bent;
}

// Whether `joint` hangs from `ancestor`, at any depth, and is not it.
[[nodiscard]] bool below(std::span<const asset::Joint> joints, u32 joint, u32 ancestor) noexcept
{
    u32 at = joint;
    while (at > ancestor) {
        const u32 parent = joints[at].parent;
        if (parent == asset::Joint::NoParent || parent >= at)
            return false;
        at = parent;
        if (at == ancestor)
            return true;
    }
    return false;
}

} // namespace

void place(std::span<const asset::Joint> joints, std::span<core::Mat4> model, core::u32 joint,
           const core::Mat4& placed) noexcept
{
    const usize count = std::min(joints.size(), model.size());
    if (joint >= count)
        return;

    // What takes the joint from where it was to where it is put: `placed`
    // after the old matrix undone. The inverse of three columns is three cross
    // products over the volume they hold.
    const Mat4& old = model[joint];
    const Vec3 x{old.m[0][0], old.m[0][1], old.m[0][2]};
    const Vec3 y{old.m[1][0], old.m[1][1], old.m[1][2]};
    const Vec3 z{old.m[2][0], old.m[2][1], old.m[2][2]};
    const Vec3 rows[3] = {core::cross(y, z), core::cross(z, x), core::cross(x, y)};
    const f32 volume = core::dot(x, rows[0]);

    Motion by;
    if (std::abs(volume) > Tiny * core::length(x) * core::length(y) * core::length(z)) {
        Mat3 undone;
        Mat3 put;
        for (int r = 0; r < 3; ++r) {
            const Vec3 row = rows[r] * (1.0f / volume);
            undone.m[0][r] = row.x;
            undone.m[1][r] = row.y;
            undone.m[2][r] = row.z;
            for (int c = 0; c < 3; ++c)
                put.m[c][r] = placed.m[c][r];
        }
        by = turning(put * undone, originOf(old), originOf(placed));
    }
    else {
        by.offset = originOf(placed) - originOf(old);
    }

    const std::array<Carried, 1> moved{Carried{joint, by}};
    carry(joints, model, moved);
    // The joint itself is what it was asked to be, to the bit, and not that
    // matrix taken apart and put together again.
    model[joint] = placed;
}

bool solveTwoBone(std::span<const asset::Joint> joints, std::span<core::Mat4> model, core::u32 end, core::Vec3 target,
                  const core::Mat3* rotation, const core::Vec3* pole, core::f32 weight) noexcept
{
    const usize count = std::min(joints.size(), model.size());
    Limb limb;
    if (!limbOf(joints, count, end, limb))
        return false;
    const f32 share = share01(weight);
    if (!(share > 0.0f))
        return true;

    const Vec3 a = originOf(model[limb.root]);
    const Vec3 b = originOf(model[limb.mid]);
    const Vec3 c = originOf(model[limb.end]);
    const Bent bent = bend(joints, model, limb, a, b, c, c + (target - c) * share, pole, share);

    // The end's own turn, against the pose as it was: none keeps the way it
    // faced in model space, whatever the bone above it did.
    Mat3 turn{};
    if (rotation != nullptr) {
        const Mat3 had = rotationOf(model[limb.end]);
        const Mat3 wanted = share < 1.0f ? core::slerp(had, *rotation, share) : *rotation;
        turn = wanted * core::transpose(had);
    }

    const std::array<Carried, 3> moved{
        Carried{limb.root, turning(bent.upper, a, a)},
        Carried{limb.mid, turning(bent.lower, b, bent.mid)},
        Carried{limb.end, turning(turn, c, bent.end)},
    };
    carry(joints, model, moved);
    return true;
}

bool solveLook(std::span<const asset::Joint> joints, std::span<core::Mat4> model, core::u32 end, core::u32 chainLength,
               core::Vec3 target, core::Vec3 forward, core::f32 maxAngle, core::f32 weight) noexcept
{
    const usize count = std::min(joints.size(), model.size());
    if (end >= count)
        return false;
    const f32 share = share01(weight);
    const f32 limit = std::clamp(maxAngle, 0.0f, Pi);
    const Vec3 facing = unit(forward);
    if (!(share > 0.0f) || !(limit > 0.0f) || facing == Vec3{})
        return true;

    // The joints that share the turn, the end first, and where each is.
    std::array<u32, MaxLookJoints> chain{};
    std::array<Vec3, MaxLookJoints> at{};
    u32 links = 0;
    for (u32 joint = end; links < MaxLookJoints;) {
        chain[links] = joint;
        at[links] = originOf(model[joint]);
        ++links;
        if (links > chainLength)
            break;
        const u32 parent = joints[joint].parent;
        if (parent == asset::Joint::NoParent || parent >= joint)
            break;
        joint = parent;
    }

    // The whole turn: about which axis, and how far. Aimed from where the end
    // is, then again from where that turn would leave it, since every joint
    // above the end moves it as it turns.
    Vec3 axis{};
    f32 angle = 0.0f;
    Vec3 from = at[0];
    for (int pass = 0; pass < LookPasses; ++pass) {
        const Vec3 toTarget = target - from;
        const f32 distance = core::length(toTarget);
        if (!(distance > Tiny))
            break;
        const Vec3 direction = toTarget * (1.0f / distance);
        Vec3 about = core::cross(facing, direction);
        const f32 sine = core::length(about);
        const f32 cosine = core::dot(facing, direction);
        f32 whole = 0.0f;
        if (sine > Tiny) {
            about = about * (1.0f / sine);
            whole = core::dmath::atan2(sine, cosine);
        }
        else if (cosine < 0.0f) {
            // Straight behind: a head turns round about the way up, when
            // there is one across the way it faces.
            about = unit(across(Up, facing));
            if (about == Vec3{})
                about = anyAcross(facing);
            whole = Pi;
        }
        else {
            about = axis;
        }
        whole = std::min(whole, limit);
        const bool settled = std::abs(whole - angle) < Tiny && core::length(about - axis) < Tiny;
        axis = about;
        angle = whole;
        if (links == 1 || settled)
            break;

        const Mat3 part = core::fromAxisAngle(axis, angle / static_cast<f32>(links));
        Vec3 reach{};
        for (u32 index = 0; index + 1 < links; ++index)
            reach = part * (reach + (at[index] - at[index + 1]));
        from = at[links - 1] + reach;
    }

    const f32 turn = angle * share;
    if (!(turn > Tiny))
        return true;

    // The topmost first: each joint has turned once more than the one above
    // it, and stands where that one's turn carried it.
    const Mat3 part = core::fromAxisAngle(axis, turn / static_cast<f32>(links));
    std::array<Carried, MaxLookJoints> moved{};
    Mat3 turned{};
    Vec3 placed = at[links - 1];
    for (u32 index = links; index-- > 0;) {
        if (index + 1 < links)
            placed = placed + turned * (at[index] - at[index + 1]);
        turned = part * turned;
        moved[links - 1 - index] = Carried{chain[index], turning(turned, at[index], placed)};
    }
    carry(joints, model, std::span<const Carried>(moved.data(), links));
    return true;
}

FeetResult solveFeet(std::span<const asset::Joint> joints, std::span<core::Mat4> model, core::u32 hips,
                     const Foot& left, const Foot& right, core::f32 clipGround, core::f32 stepHeight, bool alignToSlope,
                     core::f32 weight) noexcept
{
    FeetResult result;
    const usize count = std::min(joints.size(), model.size());
    const f32 share = share01(weight);
    if (hips >= count || !(share > 0.0f))
        return result;

    struct Leg
    {
        Limb limb;
        bool planted = false;
        f32 offset = 0.0f;
    };
    const auto legOf = [&](const Foot& foot) noexcept {
        Leg leg;
        if (!foot.hit || !limbOf(joints, count, foot.joint, leg.limb) || !below(joints, leg.limb.root, hips))
            return leg;
        const f32 rise = foot.ground - clipGround;
        if (!(std::abs(rise) <= stepHeight))
            return leg;
        leg.planted = true;
        leg.offset = rise * share;
        return leg;
    };
    const std::array<Leg, 2> legs{legOf(left), legOf(right)};
    const std::array<const Foot*, 2> feet{&left, &right};
    result.leftPlanted = legs[0].planted;
    result.rightPlanted = legs[1].planted;
    result.hipsOffset = std::min(legs[0].offset, legs[1].offset);
    const Vec3 lowered{0.0f, result.hipsOffset, 0.0f};

    // The hips, then a thigh, a shin and a foot for each leg -- every one said
    // against the pose as the clip left it, so the whole of it is one pass. A
    // joint that has nothing to do is not named, and a pose already standing
    // on its ground is not written at all.
    std::array<Carried, 7> moved{};
    usize used = 0;
    if (result.hipsOffset != 0.0f)
        moved[used++] = Carried{hips, Motion{Mat3{}, lowered}};
    for (usize side = 0; side < legs.size(); ++side) {
        const Leg& leg = legs[side];
        if (!leg.planted)
            continue;
        const Vec3 a = originOf(model[leg.limb.root]);
        const Vec3 b = originOf(model[leg.limb.mid]);
        const Vec3 c = originOf(model[leg.limb.end]);

        // Where the hips alone leave the ankle is where the lower foot's
        // belongs, and that leg is not solved: it has not been asked to bend.
        Vec3 ankle = c + lowered;
        const bool solved = leg.offset != result.hipsOffset;
        if (solved) {
            const Bent bent = bend(joints, model, leg.limb, a + lowered, b + lowered, c + lowered,
                                   c + Vec3{0.0f, leg.offset, 0.0f}, nullptr, 0.0f);
            moved[used++] = Carried{leg.limb.root, turning(bent.upper, a, a + lowered)};
            moved[used++] = Carried{leg.limb.mid, turning(bent.lower, b, bent.mid)};
            ankle = bent.end;
        }
        Mat3 turn{};
        if (alignToSlope)
            turn = swing(Up, unit(feet[side]->normal), share);
        if (solved || !(turn == Mat3{}))
            moved[used++] = Carried{leg.limb.end, turning(turn, c, ankle)};
    }
    carry(joints, model, std::span<const Carried>(moved.data(), used));
    return result;
}

} // namespace engine::render::ik
