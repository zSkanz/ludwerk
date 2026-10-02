#include "engine/scene/water.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "engine/asset/terrain.h"
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

namespace {

// How far apart a course's samples are, along a stretch.
constexpr f64 CourseStep = 2.0;

[[nodiscard]] f64 flatDistance(const DVec3& a, const DVec3& b) noexcept
{
    const f64 dx = b.x - a.x;
    const f64 dz = b.z - a.z;
    return std::sqrt(dx * dx + dz * dz);
}

// A point of the centripetal Catmull-Rom curve from `p1` to `p2`, `u` of the
// way, across the ground: the knots a square root of each chord apart, which
// is what keeps it from looping or overshooting where points crowd.
[[nodiscard]] DVec3 curvePoint(const DVec3& p0, const DVec3& p1, const DVec3& p2, const DVec3& p3, f64 u) noexcept
{
    const auto knot = [](const DVec3& a, const DVec3& b) { return std::max(std::sqrt(flatDistance(a, b)), 1e-4); };
    const f64 t0 = 0.0;
    const f64 t1 = t0 + knot(p0, p1);
    const f64 t2 = t1 + knot(p1, p2);
    const f64 t3 = t2 + knot(p2, p3);
    const f64 t = t1 + (t2 - t1) * u;
    const auto mix = [](const DVec3& a, const DVec3& b, f64 ta, f64 tb, f64 at) {
        const f64 w = (at - ta) / (tb - ta);
        return DVec3{a.x + (b.x - a.x) * w, 0.0, a.z + (b.z - a.z) * w};
    };
    const DVec3 a1 = mix(p0, p1, t0, t1, t);
    const DVec3 a2 = mix(p1, p2, t1, t2, t);
    const DVec3 a3 = mix(p2, p3, t2, t3, t);
    const DVec3 b1 = mix(a1, a2, t0, t2, t);
    const DVec3 b2 = mix(a2, a3, t1, t3, t);
    return mix(b1, b2, t1, t2, t);
}

} // namespace

WaterCourse courseOf(const World& world, InstanceId water)
{
    WaterCourse course;
    const WaterComponent* component = world.waters().find(water);
    if (component == nullptr || !(waterIsRiver(component->shape) || component->shape == water_shape::Lake))
        return course;

    struct Knot
    {
        DVec3 at;
        f64 width = 0.0;
        f64 depth = 0.0;
        bool sharp = false;
    };
    // Only a `River` reads its points' heights.
    const bool descends = component->shape == water_shape::River;
    std::vector<Knot> knots;
    for (InstanceId child = world.firstChild(water); child.valid(); child = world.nextSibling(child)) {
        const WaterPointComponent* point = world.waterPoints().find(child);
        if (point == nullptr || world.destroyed(child))
            continue;
        Knot knot;
        knot.at = DVec3{static_cast<f64>(point->position.x),
                        descends ? static_cast<f64>(point->position.y) : component->surfaceLevel,
                        static_cast<f64>(point->position.z)};
        knot.width = point->width > 0.0 ? point->width : static_cast<f64>(component->size.x);
        knot.depth = point->depth > 0.0 ? point->depth : static_cast<f64>(component->size.y);
        // **A `Spline` is as it always was**: straight from point to point,
        // which is every point a corner. What was made before the curve does
        // not change shape under whoever made it.
        knot.sharp = point->sharp || component->shape == water_shape::Spline;
        knots.push_back(knot);
    }
    const usize count = knots.size();
    course.closed = component->shape == water_shape::Lake && count >= 3;
    const auto put = [&course](const DVec3& at, f64 width, f64 depth) {
        course.samples.push_back(WaterCourseSample{at, width, depth});
    };
    if (count == 1) {
        course.points.push_back(0);
        put(knots[0].at, knots[0].width, knots[0].depth);
    }
    const usize stretches = count < 2 ? 0 : (course.closed ? count : count - 1);
    for (usize stretch = 0; stretch < stretches; ++stretch) {
        const Knot& from = knots[stretch];
        const Knot& to = knots[(stretch + 1) % count];
        // The points either side of the stretch shape its bend; at an end, and
        // at a point that is a corner, the stretch is mirrored instead -- so
        // it leaves a corner straight.
        const bool firstEnd = !course.closed && stretch == 0;
        const bool lastEnd = !course.closed && stretch + 2 == count;
        const DVec3 mirroredBefore{2.0 * from.at.x - to.at.x, from.at.y, 2.0 * from.at.z - to.at.z};
        const DVec3 mirroredAfter{2.0 * to.at.x - from.at.x, to.at.y, 2.0 * to.at.z - from.at.z};
        const DVec3 before = from.sharp || firstEnd ? mirroredBefore : knots[(stretch + count - 1) % count].at;
        const DVec3 after = to.sharp || lastEnd ? mirroredAfter : knots[(stretch + 2) % count].at;
        const f64 chord = flatDistance(from.at, to.at);
        const auto cuts = static_cast<core::u32>(std::clamp(std::ceil(chord / CourseStep), 4.0, 256.0));
        course.points.push_back(static_cast<core::u32>(course.samples.size()));
        for (core::u32 cut = 0; cut < cuts; ++cut) {
            const f64 u = static_cast<f64>(cut) / static_cast<f64>(cuts);
            DVec3 at = cut == 0 ? from.at : curvePoint(before, from.at, to.at, after, u);
            at.y = from.at.y + (to.at.y - from.at.y) * u;
            put(at, from.width + (to.width - from.width) * u, from.depth + (to.depth - from.depth) * u);
        }
    }
    if (stretches > 0 && !course.closed) {
        course.points.push_back(static_cast<core::u32>(course.samples.size()));
        put(knots[count - 1].at, knots[count - 1].width, knots[count - 1].depth);
    }
    if (!course.samples.empty()) {
        const WaterCourseSample& first = course.samples.front();
        course.minX = course.maxX = first.position.x;
        course.minZ = course.maxZ = first.position.z;
        course.top = first.position.y;
        f64 widest = 0.0;
        for (const WaterCourseSample& sample : course.samples) {
            course.minX = std::min(course.minX, sample.position.x);
            course.maxX = std::max(course.maxX, sample.position.x);
            course.minZ = std::min(course.minZ, sample.position.z);
            course.maxZ = std::max(course.maxZ, sample.position.z);
            course.top = std::max(course.top, sample.position.y);
            widest = std::max(widest, sample.width);
        }
        const f64 margin = course.closed ? 0.0 : 0.5 * widest;
        course.minX -= margin;
        course.maxX += margin;
        course.minZ -= margin;
        course.maxZ += margin;
    }
    return course;
}

WaterHere waterHere(const World& world, InstanceId water, f64 x, f64 z, const WaterCourse* course)
{
    WaterHere here;
    const WaterComponent* component = world.waters().find(water);
    if (component == nullptr)
        return here;
    here.level = component->surfaceLevel;
    here.depth = static_cast<f64>(component->size.y);
    if (component->shape == water_shape::Ocean) {
        // A sea covers every column, has no floor and flows nowhere.
        here.covered = true;
        here.depth = std::numeric_limits<f64>::max();
        return here;
    }
    if (waterIsPool(component->shape)) {
        const f64 inX =
            0.5 * static_cast<f64>(component->size.x) - std::abs(x - static_cast<f64>(component->position.x));
        const f64 inZ =
            0.5 * static_cast<f64>(component->size.z) - std::abs(z - static_cast<f64>(component->position.z));
        here.covered = inX >= 0.0 && inZ >= 0.0;
        here.inside = std::max(0.0, std::min(inX, inZ));
        return here;
    }
    WaterCourse made;
    if (course == nullptr) {
        made = courseOf(world, water);
        course = &made;
    }
    const std::vector<WaterCourseSample>& samples = course->samples;
    if (samples.size() < 2 || x < course->minX || x > course->maxX || z < course->minZ || z > course->maxZ)
        return here;

    if (component->shape == water_shape::Lake) {
        // Inside the outline: a ray along +x crosses it an odd number of times.
        if (!course->closed)
            return here;
        bool inside = false;
        f64 nearest = std::numeric_limits<f64>::max();
        for (usize at = 0, last = samples.size() - 1; at < samples.size(); last = at++) {
            const DVec3& a = samples[at].position;
            const DVec3& b = samples[last].position;
            if ((a.z > z) != (b.z > z) && x < (b.x - a.x) * (z - a.z) / (b.z - a.z) + a.x)
                inside = !inside;
            // And how far the outline is, for the bank.
            const f64 dx = b.x - a.x;
            const f64 dz = b.z - a.z;
            const f64 length = dx * dx + dz * dz;
            const f64 t = length > 1e-12 ? std::clamp(((x - a.x) * dx + (z - a.z) * dz) / length, 0.0, 1.0) : 0.0;
            const f64 ox = x - (a.x + dx * t);
            const f64 oz = z - (a.z + dz * t);
            nearest = std::min(nearest, ox * ox + oz * oz);
        }
        here.covered = inside;
        here.inside = inside ? std::sqrt(nearest) : 0.0;
        return here;
    }

    // A river: within half its width of the nearest stretch of its course,
    // at that stretch's height, flowing along it.
    f64 nearest = std::numeric_limits<f64>::max();
    for (usize at = 0; at + 1 < samples.size(); ++at) {
        const WaterCourseSample& a = samples[at];
        const WaterCourseSample& b = samples[at + 1];
        const f64 dx = b.position.x - a.position.x;
        const f64 dz = b.position.z - a.position.z;
        const f64 length = std::sqrt(dx * dx + dz * dz);
        if (length <= 1e-6)
            continue;
        const f64 along = std::clamp(((x - a.position.x) * dx + (z - a.position.z) * dz) / (length * length), 0.0, 1.0);
        const f64 ox = x - (a.position.x + dx * along);
        const f64 oz = z - (a.position.z + dz * along);
        const f64 away = std::sqrt(ox * ox + oz * oz);
        const f64 reach = 0.5 * (a.width + (b.width - a.width) * along);
        if (away > reach || away > nearest)
            continue;
        nearest = away;
        here.covered = true;
        here.inside = reach - away;
        here.level = a.position.y + (b.position.y - a.position.y) * along;
        here.depth = a.depth + (b.depth - a.depth) * along;
        // Faster where it drops, never slower where it climbs.
        const f64 slope = std::max(0.0, (a.position.y - b.position.y) / length);
        const f64 speed = 1.0 + RiverSlopeSpeed * slope;
        here.flow =
            Vec3{static_cast<core::f32>(dx / length * speed), 0.0f, static_cast<core::f32>(dz / length * speed)};
    }
    return here;
}

core::u64 carveWaterBed(World& world, InstanceId water, InstanceId terrainId)
{
    const WaterComponent* component = world.waters().find(water);
    TerrainComponent* terrain = world.terrains().find(terrainId);
    if (component == nullptr || terrain == nullptr || component->shape == water_shape::Ocean || terrain->field.empty())
        return 0;
    asset::TerrainField& field = terrain->field;
    const WaterCourse course = courseOf(world, water);
    // What the water covers across the ground, in the world's metres.
    f64 minX = 0.0;
    f64 maxX = 0.0;
    f64 minZ = 0.0;
    f64 maxZ = 0.0;
    if (waterIsPool(component->shape)) {
        minX = static_cast<f64>(component->position.x) - 0.5 * static_cast<f64>(component->size.x);
        maxX = static_cast<f64>(component->position.x) + 0.5 * static_cast<f64>(component->size.x);
        minZ = static_cast<f64>(component->position.z) - 0.5 * static_cast<f64>(component->size.z);
        maxZ = static_cast<f64>(component->position.z) + 0.5 * static_cast<f64>(component->size.z);
    }
    else {
        if (course.samples.size() < 2)
            return 0;
        minX = course.minX;
        maxX = course.maxX;
        minZ = course.minZ;
        maxZ = course.maxZ;
    }
    // In the field's own space: a terrain is moved by its origin.
    const core::i32 firstX = field.voxelIndex(minX - terrain->origin.x);
    const core::i32 lastX = field.voxelIndex(maxX - terrain->origin.x);
    const core::i32 firstZ = field.voxelIndex(minZ - terrain->origin.z);
    const core::i32 lastZ = field.voxelIndex(maxZ - terrain->origin.z);
    if (lastX < firstX || lastZ < firstZ)
        return 0;
    const f64 bank = std::max(component->bankWidth, 0.0);
    constexpr f64 CarveSlack = 0.05;
    const float skip = std::numeric_limits<float>::quiet_NaN();
    core::u64 lowered = 0;
    // A tile of columns at a time, so a river a kilometre long is not one
    // table of a million heights -- and a tile the water misses is not laid.
    constexpr core::i32 Tile = 128;
    for (core::i32 tileZ = firstZ; tileZ <= lastZ; tileZ += Tile) {
        for (core::i32 tileX = firstX; tileX <= lastX; tileX += Tile) {
            const auto columns = static_cast<core::u32>(std::min(Tile, lastX - tileX + 1));
            const auto rows = static_cast<core::u32>(std::min(Tile, lastZ - tileZ + 1));
            std::vector<float> heights = asset::columnHeights(field, tileX, tileZ, columns, rows, skip);
            core::u64 here = 0;
            for (core::u32 row = 0; row < rows; ++row) {
                for (core::u32 column = 0; column < columns; ++column) {
                    float& height = heights[static_cast<usize>(row) * columns + column];
                    // A column with no ground has nothing to cut.
                    if (!(height == height))
                        continue;
                    const f64 x = field.voxelCenter(tileX + static_cast<core::i32>(column)) + terrain->origin.x;
                    const f64 z = field.voxelCenter(tileZ + static_cast<core::i32>(row)) + terrain->origin.z;
                    const WaterHere at = waterHere(world, water, x, z, &course);
                    if (!at.covered) {
                        height = skip;
                        continue;
                    }
                    // At the surface along the edge, down to the depth a bank
                    // in, by a curve with no corner at either end.
                    const f64 t = bank > 0.0 ? std::clamp(at.inside / bank, 0.0, 1.0) : 1.0;
                    const f64 bed = at.level - at.depth * (t * t * (3.0 - 2.0 * t)) - terrain->origin.y;
                    // **Within what a column's height is stored to**: ground
                    // laid at a height reads back a few millimetres off it, and
                    // a carve of its own bed would cut those again for ever.
                    if (static_cast<f64>(height) <= bed + CarveSlack) {
                        height = skip;
                        continue;
                    }
                    height = static_cast<float>(bed);
                    ++here;
                }
            }
            if (here == 0)
                continue;
            (void)asset::writeHeights(field, tileX, tileZ, columns, heights, core::u8{1});
            lowered += here;
        }
    }
    if (lowered > 0)
        terrain->fieldRevision += 1;
    return lowered;
}

bool waterCovers(const World& world, InstanceId water, f64 x, f64 z, Vec3* flow)
{
    const WaterHere here = waterHere(world, water, x, z);
    if (flow != nullptr)
        *flow = here.flow;
    return here.covered;
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
    // A river's course or a lake's outline, made once a tick.
    WaterCourse course;
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
        Pool pool{id, &water, surfaceOf(world, id), 0.0, courseOf(world, id)};
        // A river that descends is highest at its highest point.
        pool.crest =
            water.shape == water_shape::River && !pool.course.samples.empty() ? pool.course.top : pool.surface.level;
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
            near =
                near || (bottom <= pool.crest && waterHere(world, pool.id, centre.x, centre.z, &pool.course).covered);
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
                        // A sea covers every column and flows nowhere; a
                        // river's surface is as high as its course is there.
                        const WaterHere here = waterHere(world, pool.id, at.x, at.z, &pool.course);
                        if (!here.covered)
                            continue;
                        const Vec3 along = here.flow;
                        if (at.y < here.level - here.depth)
                            continue;
                        const f64 height = lattice.heightAt(cell, pool.surface) - pool.surface.level + here.level;
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
