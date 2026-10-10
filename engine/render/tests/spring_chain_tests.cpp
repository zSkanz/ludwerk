// The chain solver (ADR 0194), on numbers: what it owes at any frame rate.

#include <algorithm>
#include <array>
#include <cmath>
#include <doctest/doctest.h>
#include <span>
#include <vector>

#include "engine/render/spring_chain.h"

using namespace engine;
using core::DVec3;
using core::Vec3;

namespace {

// Five joints in a line along -Y, a unit apart: a strip of cape hanging from
// its first joint.
[[nodiscard]] std::vector<render::SpringJoint> strip()
{
    std::vector<render::SpringJoint> chain(5);
    for (core::usize index = 0; index < chain.size(); ++index) {
        chain[index].parent = static_cast<core::i32>(index) - 1;
        chain[index].localOffset = index == 0 ? Vec3{} : Vec3{0.0f, -1.0f, 0.0f};
    }
    return chain;
}

constexpr Vec3 Gravity{0.0f, -9.81f, 0.0f};

[[nodiscard]] core::f64 distance(DVec3 a, DVec3 b)
{
    return static_cast<core::f64>(core::length(core::toVec3(a - b)));
}

// The chain carried sideways at `speed` for `seconds`, then held still for as
// long again, at `rate` frames a second. Where its last joint ends.
[[nodiscard]] DVec3 carriedThenHeld(core::f32 rate, core::f32 speed, core::f32 seconds, core::f64* furthest = nullptr)
{
    std::vector<render::SpringJoint> chain = strip();
    render::SpringState state;
    const render::SpringSettings settings;
    core::CFrameD root;
    const core::f32 frame = 1.0f / rate;
    const auto frames = static_cast<int>(seconds * rate);
    core::f64 most = 0.0;
    for (int at = 0; at < frames * 2; ++at) {
        if (at < frames)
            root.position.x += static_cast<core::f64>(speed * frame);
        render::stepSpringChain(chain, state, root, settings, {}, Gravity, 1.0f, frame, 0.0f);
        for (const render::SpringJoint& joint : chain)
            most = std::max(most, distance(joint.position, root.position));
    }
    if (furthest != nullptr)
        *furthest = most;
    DVec3 tip = chain.back().position;
    tip.x -= root.position.x;
    return tip;
}

} // namespace

TEST_CASE("a spring chain hangs where the animation has it, and stays its length")
{
    std::vector<render::SpringJoint> chain = strip();
    render::SpringState state;
    const core::CFrameD root;
    for (int frame = 0; frame < 240; ++frame)
        render::stepSpringChain(chain, state, root, {}, {}, Gravity, 1.0f, 1.0f / 60.0f, 0.0f);
    for (core::usize index = 1; index < chain.size(); ++index) {
        CHECK(distance(chain[index].position, chain[index - 1].position) == doctest::Approx(1.0).epsilon(1e-4));
        CHECK(chain[index].position.x == doctest::Approx(0.0).epsilon(1e-3));
        CHECK(chain[index].position.y == doctest::Approx(-static_cast<core::f64>(index)).epsilon(1e-3));
    }
}

TEST_CASE("a spring chain settles in the same place at 30, 60 and 144 frames a second")
{
    // Dragged for a second and left for one: what is left of the swing is the
    // same whatever the frames were, because the steps are.
    const DVec3 at30 = carriedThenHeld(30.0f, 6.0f, 1.0f);
    const DVec3 at60 = carriedThenHeld(60.0f, 6.0f, 1.0f);
    const DVec3 at144 = carriedThenHeld(144.0f, 6.0f, 1.0f);
    CHECK(distance(at30, at60) < 0.05);
    CHECK(distance(at144, at60) < 0.05);
    // And it has come back under the joint it hangs from.
    CHECK(std::abs(at60.x) < 0.1);
    CHECK(at60.y == doctest::Approx(-4.0).epsilon(0.02));
}

TEST_CASE("a spring chain on the coarse step settles where the fine one does, in half the steps")
{
    // What a lower quality takes: sixty steps a second. The dials are said
    // per sixtieth and scaled to the step, so the same chain comes to the
    // same rest; what differs is how finely the swing on the way is drawn.
    const auto settle = [](core::f32 step) {
        std::vector<render::SpringJoint> chain = strip();
        render::SpringState state;
        core::CFrameD root;
        for (int frame = 0; frame < 120; ++frame) {
            if (frame < 60)
                root.position.x += 0.1;
            render::stepSpringChain(chain, state, root, {}, {}, Gravity, 1.0f, 1.0f / 60.0f, 0.0f, step);
        }
        DVec3 tip = chain.back().position;
        tip.x -= root.position.x;
        return tip;
    };
    const DVec3 fine = settle(render::SpringStep);
    const DVec3 coarse = settle(render::SpringCoarseStep);
    CHECK(distance(fine, coarse) < 0.08);
    CHECK(std::abs(coarse.x) < 0.1);
    CHECK(coarse.y == doctest::Approx(-4.0).epsilon(0.02));
}

TEST_CASE("a spring chain trails behind what carries it, and never further than its length")
{
    core::f64 furthest = 0.0;
    (void)carriedThenHeld(60.0f, 12.0f, 1.0f, &furthest);
    CHECK(furthest <= 4.0 + 1e-3);

    // While it is carried the tip is behind the root: that is the point.
    std::vector<render::SpringJoint> chain = strip();
    render::SpringState state;
    core::CFrameD root;
    for (int frame = 0; frame < 60; ++frame) {
        root.position.x += 0.2;
        render::stepSpringChain(chain, state, root, {}, {}, Gravity, 1.0f, 1.0f / 60.0f, 0.0f);
    }
    CHECK(chain.back().position.x < root.position.x - 0.5);
}

TEST_CASE("a long frame or a jump puts a spring chain back at rest instead of flinging it")
{
    std::vector<render::SpringJoint> chain = strip();
    render::SpringState state;
    core::CFrameD root;
    for (int frame = 0; frame < 30; ++frame) {
        root.position.x += 0.3;
        render::stepSpringChain(chain, state, root, {}, {}, Gravity, 1.0f, 1.0f / 60.0f, 4.0f);
    }

    // **Half a second in one frame**, the carrier having gone on moving.
    root.position.x += 9.0;
    render::stepSpringChain(chain, state, root, {}, {}, Gravity, 1.0f, 0.5f, 4.0f);
    for (core::usize index = 1; index < chain.size(); ++index) {
        CHECK(chain[index].position.x == doctest::Approx(root.position.x));
        CHECK(distance(chain[index].position, chain[index].previous) == doctest::Approx(0.0));
    }

    // **A teleport** in an ordinary frame: further than the chain is long.
    root.position = DVec3{500.0, 40.0, -300.0};
    render::stepSpringChain(chain, state, root, {}, {}, Gravity, 1.0f, 1.0f / 60.0f, 4.0f);
    CHECK(chain.back().position.x == doctest::Approx(500.0));
    CHECK(chain.back().position.y == doctest::Approx(36.0));
    CHECK(chain.back().position.z == doctest::Approx(-300.0));

    // And the frames after it are quiet: nothing was stored up.
    for (int frame = 0; frame < 120; ++frame)
        render::stepSpringChain(chain, state, root, {}, {}, Gravity, 1.0f, 1.0f / 60.0f, 4.0f);
    CHECK(distance(chain.back().position, DVec3{500.0, 36.0, -300.0}) < 0.01);
}

TEST_CASE("a spring chain never bends past its limit")
{
    // Gravity sideways and no stiffness: it would lie flat. Thirty degrees.
    std::vector<render::SpringJoint> chain = strip();
    render::SpringState state;
    render::SpringSettings settings;
    settings.stiffness = 0.0f;
    settings.limitAngle = 30.0f;
    const core::CFrameD root;
    for (int frame = 0; frame < 300; ++frame)
        render::stepSpringChain(chain, state, root, settings, {}, Vec3{30.0f, 0.0f, 0.0f}, 1.0f, 1.0f / 60.0f, 0.0f);
    const Vec3 first = core::normalize(core::toVec3(chain[1].position - chain[0].position));
    const core::f32 bend = std::acos(core::dot(first, Vec3{0.0f, -1.0f, 0.0f})) * (180.0f / 3.14159265f);
    CHECK(bend <= 30.5f);
    CHECK(bend > 25.0f);
}

TEST_CASE("a spring chain stays out of a capsule")
{
    // A ball where the middle of the strip hangs.
    std::vector<render::SpringJoint> chain = strip();
    render::SpringState state;
    render::SpringSettings settings;
    settings.radius = 0.1f;
    const std::array<render::SpringCapsule, 1> body{
        render::SpringCapsule{DVec3{0.0, -2.0, 0.3}, DVec3{0.0, -2.0, 0.3}, 0.6f}};
    const core::CFrameD root;
    for (int frame = 0; frame < 240; ++frame)
        render::stepSpringChain(chain, state, root, settings, body, Gravity, 1.0f, 1.0f / 60.0f, 0.0f);
    for (core::usize index = 1; index < chain.size(); ++index) {
        CHECK(distance(chain[index].position, body[0].a) >= 0.7 - 0.02);
        CHECK(distance(chain[index].position, chain[index - 1].position) == doctest::Approx(1.0).epsilon(1e-3));
    }
}

TEST_CASE("a spring chain's first joint turns to face the joint that hangs from it")
{
    std::vector<render::SpringJoint> chain = strip();
    render::SpringState state;
    render::SpringSettings settings;
    settings.stiffness = 0.0f;
    const core::CFrameD root;
    for (int frame = 0; frame < 300; ++frame)
        render::stepSpringChain(chain, state, root, settings, {}, Vec3{9.81f, -9.81f, 0.0f}, 1.0f, 1.0f / 60.0f, 0.0f);
    // The joint's own "down" is carried to where its child is.
    const Vec3 carried = chain[0].rotation * Vec3{0.0f, -1.0f, 0.0f};
    const Vec3 actual = core::normalize(core::toVec3(chain[1].position - chain[0].position));
    CHECK(core::dot(carried, actual) > 0.9999f);
}

namespace {

// How far inside a capsule a point is: positive is inside, by that much.
[[nodiscard]] core::f64 depthIn(const render::SpringCapsule& capsule, DVec3 point, core::f32 radius)
{
    const Vec3 axis = core::toVec3(capsule.b - capsule.a);
    const Vec3 to = core::toVec3(point - capsule.a);
    const core::f32 lengthSquared = core::dot(axis, axis);
    const core::f32 along = lengthSquared > 1e-12f ? std::clamp(core::dot(to, axis) / lengthSquared, 0.0f, 1.0f) : 0.0f;
    return static_cast<core::f64>(capsule.radius + radius) - static_cast<core::f64>(core::length(to - axis * along));
}

// The deepest any joint, or any point along any link between two joints, is
// inside any capsule.
[[nodiscard]] core::f64 deepest(const std::vector<render::SpringJoint>& chain,
                                std::span<const render::SpringCapsule> capsules, core::f32 radius)
{
    core::f64 worst = -1.0e9;
    for (core::usize index = 1; index < chain.size(); ++index) {
        const DVec3 from = chain[static_cast<core::usize>(chain[index].parent)].position;
        const DVec3 to = chain[index].position;
        // The joint itself and nine points along its link; the link's first
        // tenth is the parent's, which is pinned or was tested as a joint.
        for (int step = 1; step <= 10; ++step) {
            const core::f64 t = static_cast<core::f64>(step) / 10.0;
            const DVec3 point{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t, from.z + (to.z - from.z) * t};
            for (const render::SpringCapsule& capsule : capsules)
                worst = std::max(worst, depthIn(capsule, point, radius));
        }
    }
    return worst;
}

} // namespace

TEST_CASE("D607: a cape rests on the body it is on, through a stop, a dash, a teleport and a wind")
{
    // The body: a capsule the height of a figure, behind which the strip
    // hangs a little clear. It moves along x and the strip trails along -x --
    // so "into the body" is the strip swinging forward through it, which is
    // what it does on a dead stop and what a wind from behind does standing.
    //
    // Held: after every frame of all of it, no joint and no point along a link
    // is inside the capsule by more than a millimetre. The solver pushed
    // JOINTS out, a step at a time, with nothing for the link between two of
    // them and nothing for a joint that crossed the whole body in one step.
    render::SpringSettings settings;
    settings.stiffness = 0.1f;
    settings.radius = 0.04f;
    core::CFrameD root;
    root.position = DVec3{-0.3, 0.0, 0.0};

    const auto body = [&](const core::CFrameD& at) {
        // Its axis a quarter of a unit in front of where the strip hangs.
        return render::SpringCapsule{DVec3{at.position.x + 0.3, -4.2, 0.0}, DVec3{at.position.x + 0.3, 0.2, 0.0}, 0.2f};
    };

    std::vector<render::SpringJoint> chain = strip();
    render::SpringState state;
    core::f64 worst = -1.0e9;
    const auto frame = [&](Vec3 push, core::f32 seconds) {
        const std::array<render::SpringCapsule, 1> capsules{body(root)};
        render::stepSpringChain(chain, state, root, settings, capsules, Gravity + push, 1.0f, seconds, 8.0f);
        worst = std::max(worst, deepest(chain, capsules, settings.radius));
    };

    // A run, the strip streaming out behind.
    for (int at = 0; at < 90; ++at) {
        root.position.x += 0.12;
        frame({}, 1.0f / 60.0f);
    }
    // A dead stop: it swings forward, into the body.
    for (int at = 0; at < 120; ++at)
        frame({}, 1.0f / 60.0f);
    CHECK(worst < 0.001);

    // A dash: thirty metres a second for a third of a second, then nothing.
    for (int at = 0; at < 20; ++at) {
        root.position.x += 0.5;
        frame({}, 1.0f / 60.0f);
    }
    for (int at = 0; at < 120; ++at)
        frame({}, 1.0f / 60.0f);
    CHECK(worst < 0.001);

    // Put somewhere else, and left.
    root.position = DVec3{400.0, 0.0, 0.0};
    for (int at = 0; at < 60; ++at)
        frame({}, 1.0f / 60.0f);
    CHECK(worst < 0.001);

    // A gale from behind, pressing it onto the body, at thirty frames a
    // second -- the fewest steps a frame.
    for (int at = 0; at < 120; ++at)
        frame(Vec3{60.0f, 0.0f, 0.0f}, 1.0f / 30.0f);
    CHECK(worst < 0.001);
    // And it is ON the body, not flung clear of it: the tip is within reach.
    CHECK(chain.back().position.x < root.position.x + 0.3 + 0.2 + 0.04 + 0.6);
}
