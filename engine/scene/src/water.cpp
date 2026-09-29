#include "engine/scene/water.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include "engine/core/dmath.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"

namespace engine::scene {

using core::DVec3;
using core::f64;
using core::InstanceId;
using core::usize;
using core::Vec3;

WaterSample WaterSurface::sample(f64 x, f64 z, f64 time) const noexcept
{
    WaterSample out;
    for (usize at = 0; at < count; ++at) {
        const WaveTerm& term = terms[at];
        const f64 p = term.k * (term.dirX * x + term.dirZ * z) - term.omega * time + term.phase;
        const f64 sinP = core::dmath::sin(p);
        const f64 cosP = core::dmath::cos(p);
        // The crest sharpened by the second harmonic, whose mean is zero, so
        // the still surface stays where `SurfaceLevel` says. **The double
        // angle from the single's sine and cosine**, not two more calls: the
        // floating samples this 27 times a body a tick, and the calls were
        // most of what it cost.
        out.height += term.amplitude * (sinP - 0.5 * term.steepness * (cosP * cosP - sinP * sinP));
        const f64 along = term.amplitude * (cosP + term.steepness * 2.0 * sinP * cosP) * term.k;
        out.slopeX += along * term.dirX;
        out.slopeZ += along * term.dirZ;
    }
    return out;
}

DVec3 WaterSurface::motion(f64 x, f64 z, f64 depth, f64 time) const noexcept
{
    DVec3 out{};
    for (usize at = 0; at < count; ++at) {
        const WaveTerm& term = terms[at];
        const f64 p = term.k * (term.dirX * x + term.dirZ * z) - term.omega * time + term.phase;
        const f64 fade = depth > 0.0 ? core::dmath::exp(-term.k * depth) : 1.0;
        const f64 speed = term.amplitude * term.omega * fade;
        const f64 sinP = core::dmath::sin(p);
        const f64 cosP = core::dmath::cos(p);
        // The height's own rate of change, crest sharpening and all...
        out.y -= speed * (cosP + term.steepness * 2.0 * sinP * cosP);
        // ...and forward under the crest, back under the trough.
        const f64 along = speed * sinP;
        out.x += along * term.dirX;
        out.z += along * term.dirZ;
    }
    return out;
}

f64 WaterSurface::heightAt(f64 x, f64 z, f64 time) const noexcept
{
    // The height alone takes one call a wave: `cos 2p` is `1 - 2 sin^2 p`.
    f64 height = level;
    for (usize at = 0; at < count; ++at) {
        const WaveTerm& term = terms[at];
        const f64 p = term.k * (term.dirX * x + term.dirZ * z) - term.omega * time + term.phase;
        const f64 sinP = core::dmath::sin(p);
        height += term.amplitude * (sinP - 0.5 * term.steepness * (1.0 - 2.0 * sinP * sinP));
    }
    return height;
}

WaterSurface surfaceOf(const World& world, InstanceId water)
{
    WaterSurface surface;
    const WaterComponent* component = world.waters().find(water);
    if (component == nullptr)
        return surface;
    surface.level = component->surfaceLevel;
    // Its waves, in child order: the first eight.
    for (InstanceId child = world.firstChild(water); child.valid() && surface.count < MaxWaterWaves;
         child = world.nextSibling(child)) {
        const WaterWaveComponent* wave = world.waterWaves().find(child);
        if (wave == nullptr || world.destroyed(child) || !(wave->wavelength > 0.0))
            continue;
        WaveTerm& term = surface.terms[surface.count++];
        term.k = 2.0 * std::numbers::pi / wave->wavelength;
        const f64 angle = wave->direction * std::numbers::pi / 180.0;
        term.dirX = core::dmath::cos(angle);
        term.dirZ = core::dmath::sin(angle);
        term.omega = std::sqrt(WaveGravity * term.k);
        term.amplitude = wave->amplitude;
        term.phase = wave->phase;
        term.steepness = std::clamp(wave->steepness, 0.0, 1.0);
    }
    return surface;
}

bool waterCovers(const World& world, InstanceId water, f64 x, f64 z, Vec3* flow)
{
    if (flow != nullptr)
        *flow = Vec3{};
    const WaterComponent* component = world.waters().find(water);
    if (component == nullptr)
        return false;
    switch (component->shape) {
    case 1:
        return std::abs(x - static_cast<f64>(component->position.x)) <= 0.5 * static_cast<f64>(component->size.x) &&
               std::abs(z - static_cast<f64>(component->position.z)) <= 0.5 * static_cast<f64>(component->size.z);
    case 2: {
        // Along its points, in order: within half its width of the nearest
        // stretch, flowing along that stretch.
        const f64 reach = 0.5 * static_cast<f64>(component->size.x);
        bool have = false;
        Vec3 last{};
        f64 nearest = reach;
        bool inside = false;
        for (InstanceId child = world.firstChild(water); child.valid(); child = world.nextSibling(child)) {
            const WaterPointComponent* point = world.waterPoints().find(child);
            if (point == nullptr || world.destroyed(child))
                continue;
            if (have) {
                const f64 ax = static_cast<f64>(last.x);
                const f64 az = static_cast<f64>(last.z);
                const f64 dx = static_cast<f64>(point->position.x) - ax;
                const f64 dz = static_cast<f64>(point->position.z) - az;
                const f64 length = std::sqrt(dx * dx + dz * dz);
                if (length > 1e-6) {
                    const f64 along = std::clamp(((x - ax) * dx + (z - az) * dz) / (length * length), 0.0, 1.0);
                    const f64 ox = x - (ax + dx * along);
                    const f64 oz = z - (az + dz * along);
                    const f64 away = std::sqrt(ox * ox + oz * oz);
                    if (away <= nearest) {
                        nearest = away;
                        inside = true;
                        if (flow != nullptr)
                            *flow =
                                Vec3{static_cast<core::f32>(dx / length), 0.0f, static_cast<core::f32>(dz / length)};
                    }
                }
            }
            last = point->position;
            have = true;
        }
        return inside;
    }
    default:
        return true;
    }
}

namespace {

[[nodiscard]] DVec3 times(DVec3 v, f64 by) noexcept
{
    return DVec3{v.x * by, v.y * by, v.z * by};
}

// How much of a part's bounding box its shape fills, by `Enum.PartShape`.
[[nodiscard]] f64 shapeFill(core::i32 shape) noexcept
{
    switch (shape) {
    case 1:
        return std::numbers::pi / 6.0;
    case 2:
        return std::numbers::pi / 4.0;
    case 3:
        return 0.72;
    case 4:
        return 0.5;
    default:
        return 1.0;
    }
}

struct Pool
{
    InstanceId id;
    const WaterComponent* water = nullptr;
    WaterSurface surface;
    // The highest the surface ever reaches: nothing above it is in the water.
    f64 crest = 0.0;
};

// A sine and a cosine of one angle.
struct Turn
{
    f64 sin = 0.0;
    f64 cos = 1.0;
};

// `a` turned by `b`, or by minus `b`.
[[nodiscard]] Turn turned(Turn a, Turn b, int sign) noexcept
{
    if (sign == 0)
        return a;
    const f64 s = sign > 0 ? b.sin : -b.sin;
    return Turn{a.sin * b.cos + a.cos * s, a.cos * b.cos - a.sin * s};
}

// One water's waves over one part's 27 cells, cell `(ix + 1) * 9 + (iy + 1)
// * 3 + (iz + 1)`.
struct Lattice
{
    struct Wave
    {
        std::array<Turn, 27> turns{};
        // The depth fade, never above one.
        std::array<f64, 27> fades{};
    };
    const Pool* pool = nullptr;
    std::array<Wave, MaxWaterWaves> waves{};

    // Each cell's phase from the centre's and one step along each axis, in a
    // cascade: 3 + 9 + 27 turns a wave rather than three a cell.
    void fill(usize wave, Turn base, const std::array<Turn, 3>& step, f64 fade, const std::array<f64, 3>& lift)
    {
        Wave& out = waves[wave];
        for (int ix = -1; ix <= 1; ++ix) {
            const Turn x = turned(base, step[0], ix);
            const f64 fx = ix > 0 ? fade * lift[0] : ix < 0 ? fade / lift[0] : fade;
            for (int iy = -1; iy <= 1; ++iy) {
                const Turn xy = turned(x, step[1], iy);
                const f64 fxy = iy > 0 ? fx * lift[1] : iy < 0 ? fx / lift[1] : fx;
                for (int iz = -1; iz <= 1; ++iz) {
                    const usize cell = static_cast<usize>((ix + 1) * 9 + (iy + 1) * 3 + (iz + 1));
                    out.turns[cell] = turned(xy, step[2], iz);
                    out.fades[cell] = std::min(iz > 0 ? fxy * lift[2] : iz < 0 ? fxy / lift[2] : fxy, 1.0);
                }
            }
        }
    }

    [[nodiscard]] f64 heightAt(usize cell, const WaterSurface& surface) const noexcept
    {
        f64 height = surface.level;
        for (usize n = 0; n < surface.count; ++n) {
            const WaveTerm& term = surface.terms[n];
            const Turn p = waves[n].turns[cell];
            height += term.amplitude * (p.sin - 0.5 * term.steepness * (p.cos * p.cos - p.sin * p.sin));
        }
        return height;
    }

    // `WaterSurface::motion`, with the depth taken from the still level and
    // never fading above it.
    [[nodiscard]] DVec3 motionAt(usize cell, const WaterSurface& surface) const noexcept
    {
        DVec3 out{};
        for (usize n = 0; n < surface.count; ++n) {
            const WaveTerm& term = surface.terms[n];
            const Turn p = waves[n].turns[cell];
            const f64 speed = term.amplitude * term.omega * waves[n].fades[cell];
            out.y -= speed * (p.cos + term.steepness * 2.0 * p.sin * p.cos);
            const f64 along = speed * p.sin;
            out.x += along * term.dirX;
            out.z += along * term.dirZ;
        }
        return out;
    }
};

} // namespace

void applyWaterForces(World& world, InstanceId workspace, f64 dt)
{
    if (!(dt > 0.0) || world.waters().size() == 0)
        return;
    std::vector<Pool> pools;
    world.waters().forEach([&](InstanceId id, const WaterComponent& water) {
        if (world.destroyed(id) || !world.isAncestorOf(workspace, id))
            return;
        Pool pool{id, &water, surfaceOf(world, id), 0.0};
        pool.crest = pool.surface.level;
        for (usize at = 0; at < pool.surface.count; ++at)
            pool.crest += pool.surface.terms[at].amplitude * (1.0 + 0.5 * pool.surface.terms[at].steepness);
        pools.push_back(pool);
    });
    if (pools.empty())
        return;
    const WorkspaceComponent* space = world.workspaces().find(workspace);
    const f64 gravity = space != nullptr ? static_cast<f64>(core::length(space->gravity)) : WaveGravity;
    const f64 time = world.engineState().simTime;

    // **A part a weld drives is not floated**: it is placed where its weld
    // says, not simulated, and an impulse on it moves nothing. The part it is
    // welded to is what floats -- a boat is its hull, with a mast along for
    // the ride.
    const auto before = [](InstanceId a, InstanceId b) {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    };
    std::vector<InstanceId> driven;
    world.welds().forEach([&](InstanceId, const WeldComponent& weld) {
        if (weld.enabled && weld.part0.valid() && weld.part1.valid())
            driven.push_back(weld.part1);
    });
    std::sort(driven.begin(), driven.end(), before);
    std::vector<Lattice> lattices;

    world.rigidBodies().forEach([&](InstanceId id, RigidBodyComponent& body) {
        if (body.anchored || !body.buoyant || world.characterBodies().find(id) != nullptr)
            return;
        if (std::binary_search(driven.begin(), driven.end(), id, before))
            return;
        const PartComponent* part = world.parts().find(id);
        if (part == nullptr || world.destroyed(id) || !world.isAncestorOf(workspace, id))
            return;
        const auto& m = part->cframe.rotation.m;
        const Vec3 size = part->size;
        // The box's vertical reach, for the quick answer and each cell's.
        const f64 reachY = 0.5 * (std::abs(static_cast<f64>(m[1][0])) * static_cast<f64>(size.x) +
                                  std::abs(static_cast<f64>(m[1][1])) * static_cast<f64>(size.y) +
                                  std::abs(static_cast<f64>(m[1][2])) * static_cast<f64>(size.z));
        const DVec3 centre = part->cframe.position;
        const f64 bottom = centre.y - reachY;
        bool near = false;
        for (const Pool& pool : pools)
            near = near || (bottom <= pool.crest && waterCovers(world, pool.id, centre.x, centre.z));
        if (!near)
            return;

        const f64 volume =
            static_cast<f64>(size.x) * static_cast<f64>(size.y) * static_cast<f64>(size.z) * shapeFill(part->shape);
        const f64 cellVolume = volume / 27.0;
        const f64 cellHeight = 2.0 * reachY / 3.0;
        // The cells' steps along the part's own three axes.
        const std::array<Vec3, 3> axes{part->cframe.rotation * Vec3{size.x / 3.0f, 0.0f, 0.0f},
                                       part->cframe.rotation * Vec3{0.0f, size.y / 3.0f, 0.0f},
                                       part->cframe.rotation * Vec3{0.0f, 0.0f, size.z / 3.0f}};

        // **Each wave's phase is linear in the cell's offset**, so its sine and
        // cosine at all 27 cells come from four pairs by the angle-sum rule,
        // and the depth fade -- measured from the still level, as the linear
        // theory it comes from measures it -- from four exponentials. Evaluated
        // per cell the floating cost about ten times as many calls, and was
        // most of the physics step with a few hundred things afloat.
        // Reused, not cleared: a lattice is a few kilobytes, and zeroing one
        // a part a tick would cost what filling it does.
        usize used = 0;
        for (const Pool& pool : pools) {
            if (bottom > pool.crest)
                continue;
            if (lattices.size() == used)
                lattices.emplace_back();
            Lattice& lattice = lattices[used++];
            lattice.pool = &pool;
            for (usize at = 0; at < pool.surface.count; ++at) {
                const WaveTerm& term = pool.surface.terms[at];
                const auto phaseOf = [&term](Vec3 step) {
                    return term.k * (term.dirX * static_cast<f64>(step.x) + term.dirZ * static_cast<f64>(step.z));
                };
                const f64 p0 = term.k * (term.dirX * centre.x + term.dirZ * centre.z) - term.omega * time + term.phase;
                std::array<Turn, 3> steps{};
                std::array<f64, 3> lifts{};
                for (usize axis = 0; axis < 3; ++axis) {
                    const f64 step = phaseOf(axes[axis]);
                    steps[axis] = Turn{core::dmath::sin(step), core::dmath::cos(step)};
                    lifts[axis] = core::dmath::exp(term.k * static_cast<f64>(axes[axis].y));
                }
                lattice.fill(at, Turn{core::dmath::sin(p0), core::dmath::cos(p0)}, steps,
                             core::dmath::exp(term.k * (centre.y - pool.surface.level)), lifts);
            }
        }

        DVec3 push{};
        DVec3 twist{};
        for (int ix = -1; ix <= 1; ++ix) {
            for (int iy = -1; iy <= 1; ++iy) {
                for (int iz = -1; iz <= 1; ++iz) {
                    const usize cell = static_cast<usize>((ix + 1) * 9 + (iy + 1) * 3 + (iz + 1));
                    const Vec3 offset = axes[0] * static_cast<core::f32>(ix) + axes[1] * static_cast<core::f32>(iy) +
                                        axes[2] * static_cast<core::f32>(iz);
                    const DVec3 at = centre + core::toDVec3(offset);
                    // **Where waters overlap, the one whose surface is
                    // highest there holds the point**, and of two at one
                    // level the one that moves: a river running out into a
                    // lake carries what it carries until it is in the lake.
                    const Lattice* holder = nullptr;
                    Vec3 flow{};
                    f64 surface = 0.0;
                    for (usize n = 0; n < used; ++n) {
                        const Lattice& lattice = lattices[n];
                        const Pool& pool = *lattice.pool;
                        if (at.y - 0.5 * cellHeight > pool.crest)
                            continue;
                        // A sea covers every column and flows nowhere.
                        Vec3 along{};
                        if (pool.water->shape != 0 && !waterCovers(world, pool.id, at.x, at.z, &along))
                            continue;
                        if (pool.water->shape != 0 && at.y < pool.surface.level - static_cast<f64>(pool.water->size.y))
                            continue;
                        const f64 height = lattice.heightAt(cell, pool.surface);
                        const bool moves = pool.water->flowSpeed != 0.0;
                        if (holder != nullptr &&
                            (height < surface ||
                             (height == surface && (!moves || holder->pool->water->flowSpeed != 0.0))))
                            continue;
                        holder = &lattice;
                        flow = along;
                        surface = height;
                    }
                    if (holder == nullptr)
                        continue;
                    const WaterComponent& water = *holder->pool->water;
                    const f64 under = cellHeight > 0.0
                                          ? std::clamp((surface - (at.y - 0.5 * cellHeight)) / cellHeight, 0.0, 1.0)
                                          : (at.y < surface ? 1.0 : 0.0);
                    if (under <= 0.0)
                        continue;
                    const f64 displaced = cellVolume * under;
                    // Held up by the water it displaces...
                    DVec3 impulse{0.0, water.density * gravity * displaced * dt, 0.0};
                    // ...and dragged towards the water's own motion, never past
                    // it: no more than the momentum that part of it carries is
                    // taken in one tick.
                    const Vec3 spin = core::cross(body.angularVelocity, offset);
                    const Vec3 moving = body.linearVelocity + spin;
                    const Vec3 carried = water.current + flow * static_cast<core::f32>(water.flowSpeed) +
                                         core::toVec3(holder->motionAt(cell, holder->pool->surface));
                    const Vec3 relative = moving - carried;
                    const f64 hold = std::min(water.viscosity * water.density * displaced * dt,
                                              static_cast<f64>(body.density) * displaced);
                    impulse = impulse - times(core::toDVec3(relative), hold);
                    push = push + impulse;
                    const Vec3 applied = core::toVec3(impulse);
                    twist = twist + core::toDVec3(core::cross(offset, applied));
                }
            }
        }
        body.pendingImpulse = body.pendingImpulse + core::toVec3(push);
        body.pendingAngularImpulse = body.pendingAngularImpulse + core::toVec3(twist);
    });
}

} // namespace engine::scene
