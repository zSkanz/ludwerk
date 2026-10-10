#include "engine/render/spring_chain.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace engine::render {

namespace {

using core::DVec3;
using core::Mat3;
using core::Vec3;

// The most joints a chain's frames are kept for on the stack. A cape is a
// score of them; a chain longer than this is stepped as far as this and its
// tail is carried rigid.
constexpr core::usize MaxChain = 96;

[[nodiscard]] Vec3 between(DVec3 from, DVec3 to) noexcept
{
    return core::toVec3(to - from);
}

[[nodiscard]] DVec3 offsetBy(DVec3 point, Vec3 by) noexcept
{
    return point + core::toDVec3(by);
}

// The rotation that turns `from` onto `to`, both unit vectors, by the shortest
// way. Opposite vectors have no shortest way; any half turn about an axis
// across them is one, and that is what is given.
[[nodiscard]] Mat3 swing(Vec3 from, Vec3 to) noexcept
{
    const f32 cosine = std::clamp(core::dot(from, to), -1.0f, 1.0f);
    if (cosine > 0.999999f)
        return Mat3{};
    Vec3 axis = core::cross(from, to);
    if (cosine < -0.999999f) {
        const Vec3 other = std::abs(from.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
        axis = core::cross(from, other);
    }
    return core::fromAxisAngle(core::normalize(axis), std::acos(cosine));
}

// `direction` no further than the angle whose cosine and sine are given from
// `animated`, both unit vectors.
[[nodiscard]] Vec3 limited(Vec3 direction, Vec3 animated, f32 cosine, f32 sine) noexcept
{
    const f32 along = core::dot(direction, animated);
    if (along >= cosine)
        return direction;
    const Vec3 across = direction - animated * along;
    const f32 size = core::length(across);
    if (size < 1e-6f)
        return animated;
    return core::normalize(animated * cosine + across * (sine / size));
}

// Where two segments come nearest each other: how far along the first and
// along the second, each from nought to one. (Ericson, "Real-Time Collision
// Detection", 5.1.9, with the cases a point-like segment makes.)
void nearest(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, f32& s, f32& u) noexcept
{
    const Vec3 d1 = q1 - p1;
    const Vec3 d2 = q2 - p2;
    const Vec3 r = p1 - p2;
    const f32 a = core::dot(d1, d1);
    const f32 e = core::dot(d2, d2);
    const f32 f = core::dot(d2, r);
    constexpr f32 Tiny = 1e-10f;
    if (a <= Tiny && e <= Tiny) {
        s = 0.0f;
        u = 0.0f;
        return;
    }
    if (a <= Tiny) {
        s = 0.0f;
        u = std::clamp(f / e, 0.0f, 1.0f);
        return;
    }
    const f32 c = core::dot(d1, r);
    if (e <= Tiny) {
        u = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
        return;
    }
    const f32 b = core::dot(d1, d2);
    const f32 denominator = a * e - b * b;
    s = denominator > Tiny ? std::clamp((b * f - c * e) / denominator, 0.0f, 1.0f) : 0.0f;
    u = (b * s + f) / e;
    if (u < 0.0f) {
        u = 0.0f;
        s = std::clamp(-c / a, 0.0f, 1.0f);
    }
    else if (u > 1.0f) {
        u = 1.0f;
        s = std::clamp((b - c) / a, 0.0f, 1.0f);
    }
}

// **The LINK out of every capsule, not only the joint at its end** (D607).
//
// What a chain draws is the cloth between its joints. Pushing the joints out,
// one at a time, left two ways through a body: a link whose two ends were both
// outside cut a corner of it, and a joint that crossed the whole body in one
// step -- a stop out of a dash -- was "out" on the far side with its link
// through the middle. The owner's cape hung inside his character.
//
// So the test is the link from `anchor` to `end` against the capsule's axis:
// where they come nearest, and if that is nearer than the two thicknesses, the
// end is moved by what puts that point of the link back outside -- further
// than the point itself has to go, since the link turns about its anchor. A
// link that runs through the axis has no "nearest side"; it goes back to the
// side its end was on a step ago, which is the side it came from.
//
// True if anything was moved.
bool linkOut(DVec3 anchor, DVec3& end, DVec3 before, std::span<const SpringCapsule> capsules, f32 radius) noexcept
{
    bool moved = false;
    for (const SpringCapsule& capsule : capsules) {
        // Measured from the capsule's own start, so the numbers are small.
        const Vec3 from = between(capsule.a, anchor);
        const Vec3 to = between(capsule.a, end);
        const Vec3 axis = between(capsule.a, capsule.b);
        f32 along = 0.0f;
        f32 onAxis = 0.0f;
        nearest(from, to, Vec3{}, axis, along, onAxis);
        // The anchor's end of the link is the parent's to keep clear: it is
        // pinned, or it was the end of the link before this one.
        if (along < 0.02f)
            continue;
        const Vec3 onLink = from + (to - from) * along;
        const Vec3 core = axis * onAxis;
        Vec3 away = onLink - core;
        f32 distance = core::length(away);
        const f32 clear = capsule.radius + radius;
        if (distance >= clear)
            continue;
        if (distance < clear * 0.25f) {
            // Through the middle: back the way it came.
            const Vec3 was = between(capsule.a, before) - core;
            const f32 lengthSquared = core::dot(axis, axis);
            const Vec3 across = lengthSquared > 1e-12f ? was - axis * (core::dot(was, axis) / lengthSquared) : was;
            if (core::dot(across, across) > 1e-10f) {
                away = core::normalize(across) * std::max(distance, 1e-4f);
                distance = core::length(away);
            }
        }
        const Vec3 direction = distance > 1e-6f ? away * (1.0f / distance) : Vec3{0.0f, 1.0f, 0.0f};
        // The end moves by more than the point does, by how far along the
        // link the point is; capped, for a point very near the anchor.
        const f32 reachFactor = 1.0f / std::max(along, 0.1f);
        end = offsetBy(end, direction * ((clear - distance) * reachFactor));
        moved = true;
    }
    return moved;
}

} // namespace

void seedSpringChain(std::span<SpringJoint> chain, SpringState& state, const core::CFrameD& root, f32 scale) noexcept
{
    for (core::usize index = 0; index < chain.size(); ++index) {
        SpringJoint& joint = chain[index];
        if (index == 0 || joint.parent < 0 || static_cast<core::usize>(joint.parent) >= index) {
            joint.rotation = root.rotation;
            joint.position = root.position;
        }
        else {
            const SpringJoint& parent = chain[static_cast<core::usize>(joint.parent)];
            joint.rotation = parent.rotation * joint.localRotation;
            joint.position = offsetBy(parent.position, parent.rotation * joint.localOffset * scale);
        }
        joint.previous = joint.position;
    }
    state.seeded = true;
    state.lastRoot = root.position;
    state.carry = 0.0f;
}

void stepSpringChain(std::span<SpringJoint> chain, SpringState& state, const core::CFrameD& root,
                     const SpringSettings& settings, std::span<const SpringCapsule> capsules, core::Vec3 acceleration,
                     f32 scale, f32 seconds, f32 jump, f32 step) noexcept
{
    if (chain.empty())
        return;
    step = std::clamp(step, SpringStep, SpringCoarseStep);
    const u32 maxSteps = static_cast<u32>(std::lround(SpringFrameCover / step));
    const core::usize count = std::min(chain.size(), MaxChain);

    // **Carried, not flung.** A chain that has never been placed, a frame that
    // was a hitch, and a carrier that was put somewhere else all end the same
    // way: the chain as the animation has it, at rest, where the carrier is.
    const f32 moved = core::length(between(state.lastRoot, root.position));
    if (!state.seeded || !(seconds < SpringHitchSeconds) || (jump > 0.0f && moved > jump)) {
        seedSpringChain(chain, state, root, scale);
        return;
    }

    // What of the carrier's motion the chain is NOT left behind by: moved with
    // it outright, place and past place both, so it makes no velocity.
    if (settings.inertia < 1.0f) {
        const Vec3 carried = between(state.lastRoot, root.position) * (1.0f - std::clamp(settings.inertia, 0.0f, 1.0f));
        for (core::usize index = 1; index < count; ++index) {
            chain[index].position = offsetBy(chain[index].position, carried);
            chain[index].previous = offsetBy(chain[index].previous, carried);
        }
    }
    state.lastRoot = root.position;

    state.carry += std::max(seconds, 0.0f);
    u32 steps = 0;
    while (state.carry >= step && steps < maxSteps) {
        state.carry -= step;
        ++steps;
    }
    // What those steps did not cover is dropped: a slow machine's cape moves
    // in slow motion for that frame rather than taking a step it cannot afford.
    if (state.carry >= step)
        state.carry = 0.0f;

    // Per step, from what the settings say per sixtieth.
    const f32 perSixtieth = step * 60.0f;
    const f32 pull = 1.0f - std::pow(1.0f - std::clamp(settings.stiffness, 0.0f, 1.0f), perSixtieth);
    const f32 keep = std::pow(1.0f - std::clamp(settings.damping, 0.0f, 1.0f), perSixtieth);
    const f32 limit = std::clamp(settings.limitAngle, 0.0f, 179.0f) * (3.14159265358979f / 180.0f);
    const f32 limitCos = std::cos(limit);
    const f32 limitSin = std::sin(limit);
    const Vec3 fall = acceleration * (step * step);
    const f32 thickness = settings.radius * scale;

    // Each joint's frame before its own child turns it, and after.
    std::array<Mat3, MaxChain> base{};
    std::array<Mat3, MaxChain> turned{};
    std::array<bool, MaxChain> hasChild{};

    const auto solve = [&](bool integrate) {
        hasChild.fill(false);
        base[0] = root.rotation;
        turned[0] = root.rotation;
        chain[0].position = root.position;
        chain[0].previous = root.position;
        for (core::usize index = 1; index < count; ++index) {
            SpringJoint& joint = chain[index];
            const auto parentIndex =
                static_cast<core::usize>(std::clamp<core::i32>(joint.parent, 0, static_cast<core::i32>(index) - 1));
            const DVec3 anchor = chain[parentIndex].position;
            const Vec3 offset = base[parentIndex] * joint.localOffset * scale;
            const f32 reach = core::length(offset);
            if (reach < 1e-6f) {
                // A joint on top of its parent has no direction to swing in.
                joint.position = anchor;
                joint.previous = anchor;
                base[index] = turned[parentIndex] * joint.localRotation;
                turned[index] = base[index];
                continue;
            }
            const Vec3 animated = offset * (1.0f / reach);

            DVec3 next = joint.position;
            if (integrate) {
                const Vec3 motion = between(joint.previous, joint.position) * keep;
                next = offsetBy(next, motion + fall);
                const DVec3 rest = offsetBy(anchor, offset);
                next = offsetBy(next, between(next, rest) * pull);
            }
            // At its length from its parent and inside its limit; then out of
            // the body, link and all, and at its length again, since the push
            // moved it -- a few times round, because each undoes a little of
            // the other. **The body wins the last word**: a limit that would
            // hold the cloth inside the character is a limit the artist did
            // not mean.
            const auto atLength = [&](bool withLimit) {
                Vec3 direction = between(anchor, next);
                const f32 size = core::length(direction);
                direction = size > 1e-6f ? direction * (1.0f / size) : animated;
                if (withLimit)
                    direction = limited(direction, animated, limitCos, limitSin);
                next = offsetBy(anchor, direction * reach);
            };
            atLength(true);
            if (!capsules.empty()) {
                for (int pass = 0; pass < 6; ++pass) {
                    if (!linkOut(anchor, next, joint.position, capsules, thickness))
                        break;
                    atLength(false);
                }
            }
            if (integrate) {
                joint.previous = joint.position;
            }
            joint.position = next;

            // The parent turns to face its first child; this joint's own frame
            // is what that turn carries.
            if (!hasChild[parentIndex]) {
                hasChild[parentIndex] = true;
                const Vec3 simulated = core::normalize(between(anchor, next));
                turned[parentIndex] = swing(animated, simulated) * base[parentIndex];
            }
            base[index] = turned[parentIndex] * joint.localRotation;
            turned[index] = base[index];
        }
    };

    if (steps == 0) {
        // No step this frame: the joints stay where they are, re-fitted to
        // where the carrier now is so the chain does not stretch between steps.
        solve(false);
    }
    for (u32 taken = 0; taken < steps; ++taken)
        solve(true);

    for (core::usize index = 0; index < count; ++index)
        chain[index].rotation = turned[index];
    // A tail past what is stepped: rigid from the last joint that was.
    for (core::usize index = count; index < chain.size(); ++index) {
        SpringJoint& joint = chain[index];
        const SpringJoint& parent =
            chain[static_cast<core::usize>(std::clamp<core::i32>(joint.parent, 0, static_cast<core::i32>(index) - 1))];
        joint.rotation = parent.rotation * joint.localRotation;
        joint.position = offsetBy(parent.position, parent.rotation * joint.localOffset * scale);
        joint.previous = joint.position;
    }
}

} // namespace engine::render
