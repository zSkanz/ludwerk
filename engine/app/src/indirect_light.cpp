#include "engine/app/indirect_light.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

#include "engine/app/lightmap_bake.h"
#include "engine/asset/primitives.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/jobs/jobs.h"
#include "engine/platform/platform.h"
#include "engine/render/environment.h"
#include "engine/render/lighting.h"

namespace engine::app {

namespace {

using core::Color3;
using core::DVec3;
using core::f32;
using core::f64;
using core::InstanceId;
using core::Mat4;
using core::u32;
using core::u64;
using core::usize;
using core::Vec3;

constexpr f32 kPi = 3.14159265358979f;
// How much of a frame the pictures and the rays may take: nearly all of it
// behind the loading curtain, which still has to turn its own picture, and a
// sliver of it while the game is watched.
constexpr u64 kCurtainBudgetNs = 12'000'000;
// How many points a worker takes at a time, and how many are handed out
// between two looks at the clock.
constexpr usize kGrain = 4;
constexpr usize kChunk = 96;
// And of the gathering, which asks no ray: a point is sixty-four looks into
// the volume's own light.
constexpr usize kGatherChunk = 384;
// How fast what arrives while the world is watched comes in: half a second.
constexpr f32 kFadeRate = 2.0f;
// A ray's start off the surface it leaves, in metres.
constexpr f32 kOffSurface = 0.02f;
// What ground is to light that lands on it: terrain has no one colour, and
// this is the grey-green of earth and grass together.
constexpr Color3 kGroundAlbedo{0.42f, 0.45f, 0.36f};
// How far past a box something still counts as in view of it.
constexpr f64 kNearMetres = 6.0;

[[nodiscard]] u64 keyOf(InstanceId id) noexcept
{
    return (static_cast<u64>(id.generation) << 32) | id.index;
}

[[nodiscard]] Color3 operator*(Color3 a, f32 b) noexcept
{
    return Color3{a.r * b, a.g * b, a.b * b};
}
[[nodiscard]] Color3 operator*(Color3 a, Color3 b) noexcept
{
    return Color3{a.r * b.r, a.g * b.g, a.b * b.b};
}
[[nodiscard]] Color3 operator+(Color3 a, Color3 b) noexcept
{
    return Color3{a.r + b.r, a.g + b.g, a.b + b.b};
}

[[nodiscard]] bool inWorkspace(const scene::World& world, InstanceId id, InstanceId workspace) noexcept
{
    for (InstanceId cursor = world.parentOf(id); cursor.valid(); cursor = world.parentOf(cursor)) {
        if (cursor == workspace)
            return true;
    }
    return false;
}

// How far a point is from a box, nought inside it.
[[nodiscard]] f64 distanceToBox(const core::CFrameD& box, Vec3 half, DVec3 point) noexcept
{
    const DVec3 local = core::transformPoint(core::inverse(box), point);
    const f64 dx = std::max(std::abs(local.x) - static_cast<f64>(half.x), 0.0);
    const f64 dy = std::max(std::abs(local.y) - static_cast<f64>(half.y), 0.0);
    const f64 dz = std::max(std::abs(local.z) - static_cast<f64>(half.z), 0.0);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

[[nodiscard]] bool boxInView(const render::RenderCamera& camera, const core::CFrameD& box, Vec3 size) noexcept
{
    const Mat4 placed = core::toRenderMatrixScaled(box, camera.origin, size);
    const core::AABB bounds = core::transformed(placed, core::AABB{Vec3{-0.5f, -0.5f, -0.5f}, Vec3{0.5f, 0.5f, 0.5f}});
    return core::intersects(camera.frustum, bounds);
}

struct Hasher
{
    u64 value = 1469598103934665603ull;
    void add(u64 bits) noexcept { value = (value ^ bits) * 1099511628211ull; }
    void add(f32 number, f32 step) noexcept
    {
        add(static_cast<u64>(static_cast<core::i64>(std::lround(static_cast<f64>(number) / static_cast<f64>(step)))));
    }
    void add(Color3 colour) noexcept
    {
        add(colour.r, 0.01f);
        add(colour.g, 0.01f);
        add(colour.b, 0.01f);
    }
};

// The lamps whose light reaches a box, and one number for them and the sun:
// what says the light a probe or a volume was taken under has changed.
[[nodiscard]] u64 lightAround(const BakeLight& light, const core::CFrameD& box, Vec3 half,
                              std::vector<BakeLamp>* reaching)
{
    Hasher hash;
    // The sun by about a degree, which is what rebuilds the sky's own map.
    hash.add(light.sunDirection.x, 0.017f);
    hash.add(light.sunDirection.y, 0.017f);
    hash.add(light.sunDirection.z, 0.017f);
    hash.add(light.sunRadiance);
    if (light.sky) {
        hash.add(light.sky(Vec3{0.0f, 1.0f, 0.0f}));
        hash.add(light.sky(Vec3{1.0f, 0.0f, 0.0f}));
    }
    for (const BakeLamp& lamp : light.lamps) {
        if (distanceToBox(box, half, lamp.position) > static_cast<f64>(lamp.range))
            continue;
        hash.add(static_cast<f32>(lamp.position.x), 0.01f);
        hash.add(static_cast<f32>(lamp.position.y), 0.01f);
        hash.add(static_cast<f32>(lamp.position.z), 0.01f);
        hash.add(lamp.direction.x, 0.01f);
        hash.add(lamp.direction.y, 0.01f);
        hash.add(lamp.direction.z, 0.01f);
        hash.add(lamp.radiance);
        hash.add(lamp.range, 0.01f);
        hash.add(lamp.cosOuter, 0.001f);
        hash.add(lamp.shadows ? 1u : 0u);
        if (reaching != nullptr)
            reaching->push_back(lamp);
    }
    return hash.value;
}

// What a ray meets in the physics world: the nearest body that does not move
// and is not see-through. What is unanchored is not scenery, and a pane mostly
// transparent lets the light by; each is stepped past and the ray goes on.
struct Met
{
    bool lets = false;
    Color3 albedo{1.0f, 1.0f, 1.0f};
    Color3 emissive{0.0f, 0.0f, 0.0f};
};

// What an instance is to a ray: a part by its material, the ground by one
// colour, and anything else -- a character -- something that will not be
// there next frame.
[[nodiscard]] Met surfaceOf(const scene::World& world, InstanceId id, const render::TextureLibrary* textures)
{
    Met met;
    if (const scene::PartComponent* part = world.parts().find(id); part != nullptr) {
        const scene::RigidBodyComponent* body = world.rigidBodies().find(id);
        const render::PartSurface surface = render::partSurfaceOf(world, *part, textures);
        met.lets = (body != nullptr && !body->anchored) || world.characterBodies().find(id) != nullptr ||
                   surface.transparency >= 0.5f;
        met.albedo = surface.color;
        met.emissive = surface.emissive;
    }
    else if (world.terrains().find(id) != nullptr) {
        met.albedo = kGroundAlbedo;
    }
    else {
        met.lets = true;
    }
    return met;
}

// **And an anchored mesh by its own triangles** (`TriangleTree`): to the
// physics world it is one hull, its doorway closed. Its body there is stepped
// past like anything else a ray goes on by, and the mesh is asked itself,
// in its own space, for what is nearer than whatever the physics world met.
template <class Surfaces, class Meshes>
[[nodiscard]] BakeTrace traceOf(const scene::World& world, const scene::PhysicsSync& physics, const Surfaces& surfaces,
                                const Meshes& meshes, const BoxTree& boxes, std::span<const u64> meshBodies,
                                const render::TextureLibrary* textures)
{
    return [&world, &physics, &surfaces, &meshes, &boxes, meshBodies, textures](const DVec3& origin, Vec3 direction,
                                                                                f32 reach) -> BakeHit {
        BakeHit met;
        f32 nearest = reach;
        // What a cast leaves out: the meshes asked by their triangles, known
        // beforehand, then whatever it goes on past as it is met.
        std::array<u64, 40> passed{};
        usize passes = std::min(meshBodies.size(), usize{28});
        std::copy_n(meshBodies.begin(), passes, passed.begin());
        for (;;) {
            physics::QueryFilter filter;
            filter.userData = std::span<const u64>(passed.data(), passes);
            filter.backFaces = true;
            physics::RayHit hit;
            if (!physics.backend().raycast(physics.worldHandle(), physics::RayD{origin, direction * reach}, filter,
                                           hit))
                break;
            const InstanceId id = physics.instanceOf(hit.userData);
            bool lets = false;
            Color3 albedo{1.0f, 1.0f, 1.0f};
            Color3 emissive{0.0f, 0.0f, 0.0f};
            if (id.index < surfaces.size() && surfaces[id.index].kind != 0 &&
                surfaces[id.index].generation == id.generation) {
                // 2 goes on past; 3 is a mesh asked by its triangles below.
                lets = surfaces[id.index].kind >= 2;
                albedo = surfaces[id.index].albedo;
                emissive = surfaces[id.index].emissive;
            }
            else {
                const Met read = surfaceOf(world, id, textures);
                lets = read.lets;
                albedo = read.albedo;
                emissive = read.emissive;
            }
            if (!lets) {
                met.hit = true;
                met.position = hit.position;
                met.normal = hit.normal;
                met.inside = hit.distance < 1.0e-4f || core::dot(hit.normal, direction) > 0.0f;
                met.albedo = albedo;
                met.emissive = emissive;
                nearest = hit.distance;
                break;
            }
            if (passes == passed.size())
                break;
            passed[passes++] = hit.userData;
        }

        boxes.cast(origin, direction, nearest, [&](u32 which, f32 reachNow) {
            const auto& mesh = meshes[which];
            // Into the mesh's own space, the stretch taken out: the ray is
            // still a ray, and a length along it is still that length.
            const Vec3 local = core::toVec3(core::transformPoint(mesh.intoMesh, origin));
            const Vec3 turned = core::transformDirection(mesh.intoMesh, direction);
            const Vec3 start{local.x / mesh.stretch.x, local.y / mesh.stretch.y, local.z / mesh.stretch.z};
            const Vec3 along{turned.x / mesh.stretch.x, turned.y / mesh.stretch.y, turned.z / mesh.stretch.z};
            TriangleTree::Hit hit;
            if (!mesh.tree->cast(start, along, reachNow, hit))
                return reachNow;
            met.hit = true;
            met.position = origin + core::toDVec3(direction * hit.distance);
            // A normal crosses a stretch the other way round.
            const Vec3 across{hit.normal.x / mesh.stretch.x, hit.normal.y / mesh.stretch.y,
                              hit.normal.z / mesh.stretch.z};
            met.normal = core::normalize(mesh.turn * across);
            met.inside = hit.distance < 1.0e-4f || core::dot(met.normal, direction) > 0.0f;
            met.albedo = mesh.tint;
            met.emissive = mesh.emissive;
            for (const auto& bounce : *mesh.bounces) {
                const u32 index = hit.triangle * 3u;
                if (index >= bounce.firstIndex && index < bounce.firstIndex + bounce.indexCount) {
                    met.albedo = bounce.albedo * mesh.tint;
                    met.emissive = bounce.emissive + mesh.emissive;
                    break;
                }
            }
            return hit.distance;
        });
        return met;
    };
}

} // namespace

VolumeLayout volumeLayoutOf(Vec3 size, f32 spacing) noexcept
{
    VolumeLayout layout;
    layout.spacing = std::max(spacing, 0.5f);
    const auto lay = [&]() {
        usize all = 1;
        for (u32 axis = 0; axis < 3; ++axis) {
            const f32 side = (&size.x)[axis];
            const auto count = static_cast<u32>(std::lround(side / layout.spacing));
            layout.counts[axis] = std::clamp<u32>(count, 1u, render::kIrradianceVolumeSide);
            all *= layout.counts[axis];
        }
        return all;
    };
    // The spacing asked for, and when that is more than a volume holds --
    // along one side or in all -- a coarser one, a tenth at a time.
    const auto fits = [&]() {
        if (lay() > render::kIrradianceVolumeSamples)
            return false;
        for (u32 axis = 0; axis < 3; ++axis) {
            if (static_cast<u32>(std::lround((&size.x)[axis] / layout.spacing)) > render::kIrradianceVolumeSide)
                return false;
        }
        return true;
    };
    while (!fits()) {
        layout.spacing *= 1.1f;
        layout.coarser = true;
    }
    return layout;
}

std::vector<VolumePiece> volumeTilesOf(const core::CFrameD& box, Vec3 size, f32 spacing)
{
    const VolumeLayout whole = volumeLayoutOf(size, spacing);
    std::vector<VolumePiece> pieces;
    const auto one = [&]() {
        pieces.assign(1, VolumePiece{.box = box, .size = size, .layout = whole, .margin = 0.0f});
        return pieces;
    };
    if (!whole.coarser)
        return one();

    // The cells the box is at the spacing asked for, and how long each is.
    const f32 asked = std::max(spacing, 0.5f);
    u32 cells[3];
    f32 cell[3];
    for (u32 axis = 0; axis < 3; ++axis) {
        const f32 side = (&size.x)[axis];
        cells[axis] = std::max<u32>(1u, static_cast<u32>(std::lround(side / asked)));
        cell[axis] = side / static_cast<f32>(cells[axis]);
    }
    // Tiles along each side -- more along whichever side's tiles are longest
    // -- until a tile, with the cell it shares with each neighbour, is one
    // volume's worth.
    u32 tiles[3] = {1, 1, 1};
    const auto span = [&](u32 axis) {
        const u32 own = (cells[axis] + tiles[axis] - 1) / tiles[axis];
        return own + (tiles[axis] > 1 ? 2u : 0u);
    };
    const auto fits = [&]() {
        usize all = 1;
        for (u32 axis = 0; axis < 3; ++axis) {
            if (span(axis) > render::kIrradianceVolumeSide)
                return false;
            all *= span(axis);
        }
        return all <= render::kIrradianceVolumeSamples;
    };
    while (!fits()) {
        u32 longest = 3;
        for (u32 axis = 0; axis < 3; ++axis) {
            if (tiles[axis] < cells[axis] && (longest == 3 || span(axis) > span(longest)))
                longest = axis;
        }
        if (longest == 3)
            return one();
        ++tiles[longest];
        if (tiles[0] * tiles[1] * tiles[2] > kMaxVolumeTiles)
            return one();
    }

    // Across the two cells two tiles share, each gives way to the other.
    f32 margin = 0.0f;
    for (u32 axis = 0; axis < 3; ++axis) {
        if (tiles[axis] > 1)
            margin = margin == 0.0f ? 2.0f * cell[axis] : std::min(margin, 2.0f * cell[axis]);
    }
    for (u32 tz = 0; tz < tiles[2]; ++tz) {
        for (u32 ty = 0; ty < tiles[1]; ++ty) {
            for (u32 tx = 0; tx < tiles[0]; ++tx) {
                const u32 at[3] = {tx, ty, tz};
                VolumePiece piece;
                piece.margin = margin;
                piece.layout.spacing = asked;
                Vec3 middle;
                for (u32 axis = 0; axis < 3; ++axis) {
                    // Its own cells, on the box's own grid, and one more
                    // towards each tile it has beside it.
                    const u32 first = at[axis] * cells[axis] / tiles[axis];
                    const u32 last = (at[axis] + 1) * cells[axis] / tiles[axis];
                    const u32 before = at[axis] > 0 ? 1u : 0u;
                    const u32 after = at[axis] + 1 < tiles[axis] ? 1u : 0u;
                    const u32 count = last - first + before + after;
                    const f32 low = -(&size.x)[axis] * 0.5f + static_cast<f32>(first - before) * cell[axis];
                    const f32 length = static_cast<f32>(count) * cell[axis];
                    piece.layout.counts[axis] = count;
                    (&piece.size.x)[axis] = length;
                    (&middle.x)[axis] = low + length * 0.5f;
                }
                piece.box = box * core::CFrameD{.position = core::toDVec3(middle), .rotation = core::Mat3{}};
                pieces.push_back(piece);
            }
        }
    }
    return pieces;
}

std::vector<VolumePiece> volumeLevelsOf(Vec3 size, f32 spacing, u32 levels, const DVec3& camera)
{
    // Lying down: the box's x is the world's, its y the world's z, and its z
    // the world's up or down.
    const core::Mat3 lying = core::fromEulerYxz(Vec3{kPi * 0.5f, 0.0f, 0.0f});
    const Vec3 laid{size.x, size.z, size.y};
    const VolumeLayout layout = volumeLayoutOf(laid, spacing);
    std::vector<VolumePiece> pieces;
    const u32 count = std::clamp<u32>(levels, 1u, kMaxVolumeLevels);
    for (u32 level = 0; level < count; ++level) {
        const f32 scale = static_cast<f32>(1u << level);
        VolumePiece piece;
        piece.layout = layout;
        piece.layout.spacing = layout.spacing * scale;
        piece.size = Vec3{laid.x * scale, laid.y * scale, laid.z * scale};
        // The cells it has along the world's x, y and z, and so the grid it
        // stands on: a few cells at a time, so a step of the camera is not a
        // step of the level.
        const f64 cellX = static_cast<f64>(piece.size.x) / static_cast<f64>(layout.counts[0]);
        const f64 cellZ = static_cast<f64>(piece.size.y) / static_cast<f64>(layout.counts[1]);
        const f64 cellY = static_cast<f64>(piece.size.z) / static_cast<f64>(layout.counts[2]);
        const auto snapped = [](f64 at, f64 cell) {
            const f64 step = cell * static_cast<f64>(kLevelSnapCells);
            return std::round(at / step) * step;
        };
        piece.box.position = DVec3{snapped(camera.x, cellX), snapped(camera.y, cellY), snapped(camera.z, cellZ)};
        piece.box.rotation = lying;
        piece.margin = -2.0f * static_cast<f32>(std::min(cellX, std::min(cellY, cellZ)));
        pieces.push_back(piece);
    }
    return pieces;
}

Vec3 bakeDirection(u32 index, u32 count) noexcept
{
    // A spiral down the sphere by the golden angle: no two rays close, none
    // of the sphere left out, and the same set from every point.
    const f32 y = 1.0f - (2.0f * static_cast<f32>(index) + 1.0f) / static_cast<f32>(count);
    const f32 radius = std::sqrt(std::max(1.0f - y * y, 0.0f));
    const f32 turn = static_cast<f32>(index) * 2.39996323f;
    return Vec3{radius * std::cos(turn), y, radius * std::sin(turn)};
}

// --- One volume -------------------------------------------------------------

usize VolumeBake::sampleCount() const noexcept
{
    return static_cast<usize>(m_layout.counts[0]) * m_layout.counts[1] * m_layout.counts[2];
}

DVec3 VolumeBake::samplePlace(usize sample) const noexcept
{
    const u32 nx = m_layout.counts[0];
    const u32 ny = m_layout.counts[1];
    const u32 nz = m_layout.counts[2];
    const auto x = static_cast<u32>(sample % nx);
    const auto y = static_cast<u32>((sample / nx) % ny);
    const auto z = static_cast<u32>(sample / (static_cast<usize>(nx) * ny));
    // The middle of its cell.
    const Vec3 local{
        -m_half.x + (static_cast<f32>(x) + 0.5f) * (2.0f * m_half.x / static_cast<f32>(nx)),
        -m_half.y + (static_cast<f32>(y) + 0.5f) * (2.0f * m_half.y / static_cast<f32>(ny)),
        -m_half.z + (static_cast<f32>(z) + 0.5f) * (2.0f * m_half.z / static_cast<f32>(nz)),
    };
    return core::transformPoint(m_box, core::toDVec3(local));
}

void VolumeBake::begin(const core::CFrameD& box, Vec3 size, const VolumeLayout& layout, u32 bounces)
{
    m_box = box;
    m_half = Vec3{size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
    m_layout = layout;
    m_bounces = std::clamp<u32>(bounces, 1u, 3u);
    m_rays.assign(sampleCount() * kBakeRays, Ray{});
    m_inside.assign(sampleCount(), 0);
    m_light.clear();
    m_dropped = 0;
    m_cursor = 0;
    m_lightHeld = false;
    m_fresh.clear();
    m_lightFreshOnly = false;
    m_stage = Stage::Trace;
}

bool VolumeBake::scroll(const core::CFrameD& box, const core::i32 (&cells)[3])
{
    if (m_stage != Stage::Done || m_rays.empty())
        return false;
    const core::i32 counts[3] = {static_cast<core::i32>(m_layout.counts[0]), static_cast<core::i32>(m_layout.counts[1]),
                                 static_cast<core::i32>(m_layout.counts[2])};
    for (u32 axis = 0; axis < 3; ++axis) {
        if (std::abs(cells[axis]) >= counts[axis])
            return false;
    }
    // How far it went, in its own space: what every kept ray's place is
    // moved back by, since a place is kept from the box's middle.
    const Vec3 moved{static_cast<f32>(cells[0]) * (2.0f * m_half.x / static_cast<f32>(counts[0])),
                     static_cast<f32>(cells[1]) * (2.0f * m_half.y / static_cast<f32>(counts[1])),
                     static_cast<f32>(cells[2]) * (2.0f * m_half.z / static_cast<f32>(counts[2]))};
    const usize samples = sampleCount();
    std::vector<Ray> rays(m_rays.size());
    std::vector<core::u8> inside(samples, 0);
    m_fresh.assign(samples, 1);
    for (core::i32 z = 0; z < counts[2]; ++z) {
        for (core::i32 y = 0; y < counts[1]; ++y) {
            for (core::i32 x = 0; x < counts[0]; ++x) {
                const core::i32 ox = x + cells[0];
                const core::i32 oy = y + cells[1];
                const core::i32 oz = z + cells[2];
                if (ox < 0 || oy < 0 || oz < 0 || ox >= counts[0] || oy >= counts[1] || oz >= counts[2])
                    continue;
                const usize now = (static_cast<usize>(z) * static_cast<usize>(counts[1]) + static_cast<usize>(y)) *
                                      static_cast<usize>(counts[0]) +
                                  static_cast<usize>(x);
                const usize was = (static_cast<usize>(oz) * static_cast<usize>(counts[1]) + static_cast<usize>(oy)) *
                                      static_cast<usize>(counts[0]) +
                                  static_cast<usize>(ox);
                for (u32 index = 0; index < kBakeRays; ++index) {
                    Ray ray = m_rays[was * kBakeRays + index];
                    ray.place = Vec3{ray.place.x - moved.x, ray.place.y - moved.y, ray.place.z - moved.z};
                    rays[now * kBakeRays + index] = ray;
                }
                inside[now] = m_inside[was];
                m_fresh[now] = 0;
            }
        }
    }
    m_rays = std::move(rays);
    m_inside = std::move(inside);
    m_box = box;
    m_cursor = 0;
    m_scrolledStamp = m_lightHeld ? m_lit.stamp : 0;
    m_lightHeld = false;
    m_lightFreshOnly = false;
    m_stage = Stage::Trace;
    return true;
}

void VolumeBake::relight() noexcept
{
    if (m_stage == Stage::Idle || m_stage == Stage::Trace || m_rays.empty())
        return;
    m_stage = Stage::Light;
    m_cursor = 0;
    m_lightHeld = false;
    m_fresh.clear();
    m_lightFreshOnly = false;
}

void VolumeBake::give(const core::CFrameD& box, Vec3 size, const VolumeLayout& layout, std::span<const f32> light)
{
    m_box = box;
    m_half = Vec3{size.x * 0.5f, size.y * 0.5f, size.z * 0.5f};
    m_layout = layout;
    m_rays.clear();
    m_rays.shrink_to_fit();
    m_inside.clear();
    m_fresh.clear();
    m_light.assign(light.begin(), light.end());
    m_dropped = 0;
    m_cursor = 0;
    m_lightHeld = false;
    m_lightFreshOnly = false;
    m_stage = Stage::Done;
}

void VolumeBake::forgetRays() noexcept
{
    if (m_stage != Stage::Done)
        return;
    m_rays.clear();
    m_rays.shrink_to_fit();
}

f32 VolumeBake::progress() const noexcept
{
    const f32 done = sampleCount() == 0 ? 1.0f : static_cast<f32>(m_cursor) / static_cast<f32>(sampleCount());
    switch (m_stage) {
    case Stage::Idle:
        return 0.0f;
    case Stage::Trace:
        return 0.6f * done;
    case Stage::Light:
        return 0.6f + 0.35f * done;
    case Stage::Gather:
        return 0.95f + 0.05f * (static_cast<f32>(m_pass) + done) / static_cast<f32>(1 + m_bounces);
    case Stage::Done:
        break;
    }
    return 1.0f;
}

void VolumeBake::traceSample(usize sample, const BakeTrace& trace)
{
    // Carried here with its rays: nothing to cast.
    if (!m_fresh.empty() && m_fresh[sample] == 0)
        return;
    const DVec3 from = samplePlace(sample);
    const core::CFrameD intoBox = core::inverse(m_box);
    u32 inside = 0;
    for (u32 index = 0; index < kBakeRays; ++index) {
        Ray& ray = m_rays[sample * kBakeRays + index];
        ray = Ray{};
        const BakeHit met = trace(from, bakeDirection(index, kBakeRays), kBakeReach);
        if (!met.hit)
            continue;
        if (met.inside) {
            ray.met = 2;
            ++inside;
            continue;
        }
        ray.met = 1;
        ray.place = core::toVec3(core::transformPoint(intoBox, met.position));
        ray.normal = met.normal;
        ray.albedo = met.albedo;
        ray.emissive = met.emissive;
    }
    // Most of what it sees is the inside of something: it is in a wall.
    m_inside[sample] = inside * 2 >= kBakeRays ? 1 : 0;
}

void VolumeBake::lightSample(usize sample, const BakeTrace& trace)
{
    if (m_inside[sample] != 0)
        return;
    // Carried here under this same light: what it put on these is kept.
    if (m_lightFreshOnly && m_fresh[sample] == 0)
        return;
    for (u32 index = 0; index < kBakeRays; ++index) {
        Ray& ray = m_rays[sample * kBakeRays + index];
        if (ray.met != 1)
            continue;
        const DVec3 place =
            core::transformPoint(m_box, core::toDVec3(ray.place)) +
            core::toDVec3(Vec3{ray.normal.x * kOffSurface, ray.normal.y * kOffSurface, ray.normal.z * kOffSurface});
        ray.direct = bakeDirectLight(place, ray.normal, m_lit, trace);
    }
}

Color3 bakeDirectLight(const DVec3& place, Vec3 normal, const BakeLight& light, const BakeTrace& trace)
{
    Color3 direct{0.0f, 0.0f, 0.0f};
    // As the shader lights a matte surface: what the light gives, by how
    // squarely it lands, over pi.
    if (light.sunRadiance.r + light.sunRadiance.g + light.sunRadiance.b > 0.0f) {
        const f32 facing = core::dot(normal, light.sunDirection);
        if (facing > 0.0f && !trace(place, light.sunDirection, kBakeReach).hit)
            direct = direct + light.sunRadiance * (facing / kPi);
    }
    for (const BakeLamp& lamp : light.lamps) {
        const Vec3 toLamp = core::toVec3(lamp.position - place);
        const f32 distanceSquared = std::max(core::dot(toLamp, toLamp), 1.0e-6f);
        const f32 ratio = distanceSquared / (lamp.range * lamp.range);
        if (ratio >= 1.0f)
            continue;
        const f32 distance = std::sqrt(distanceSquared);
        const Vec3 towards{toLamp.x / distance, toLamp.y / distance, toLamp.z / distance};
        const f32 facing = core::dot(normal, towards);
        if (facing <= 0.0f)
            continue;
        // The engine's own falloff and cone (`engine_brdf.hlsli`).
        const f32 window = std::clamp(1.0f - ratio * ratio, 0.0f, 1.0f);
        const f32 falloff = window * window / (1.0f + 25.0f * ratio);
        const f32 cosInner = lamp.cosOuter + (1.0f - lamp.cosOuter) * 0.1f;
        const f32 along = -core::dot(lamp.direction, towards);
        // A lamp that shines every way has no cone (as the shader).
        const f32 cone =
            lamp.cosOuter <= -1.0f
                ? 1.0f
                : std::clamp((along - lamp.cosOuter) / std::max(cosInner - lamp.cosOuter, 1.0e-6f), 0.0f, 1.0f);
        if (falloff * cone <= 0.0f)
            continue;
        // A lamp that casts no shadow in the picture casts none here: the
        // light a wall bounces is the light the wall is drawn with.
        if (lamp.shadows && trace(place, towards, std::max(distance - 2.0f * kOffSurface, 0.0f)).hit)
            continue;
        direct = direct + lamp.radiance * (falloff * cone * facing / kPi);
    }
    return direct;
}

Color3 VolumeBake::lookUp(std::span<const f32> from, Vec3 place, Vec3 normal) const noexcept
{
    const u32 counts[3] = {m_layout.counts[0], m_layout.counts[1], m_layout.counts[2]};
    f32 grid[3];
    u32 low[3];
    u32 high[3];
    f32 between[3];
    for (u32 axis = 0; axis < 3; ++axis) {
        const f32 half = (&m_half.x)[axis];
        const f32 at = ((&place.x)[axis] + half) / (2.0f * half) * static_cast<f32>(counts[axis]) - 0.5f;
        grid[axis] = std::clamp(at, 0.0f, static_cast<f32>(counts[axis] - 1));
        low[axis] = static_cast<u32>(grid[axis]);
        high[axis] = std::min(low[axis] + 1, counts[axis] - 1);
        between[axis] = grid[axis] - static_cast<f32>(low[axis]);
    }
    Color3 light{0.0f, 0.0f, 0.0f};
    for (u32 corner = 0; corner < 8; ++corner) {
        const u32 x = (corner & 1u) != 0 ? high[0] : low[0];
        const u32 y = (corner & 2u) != 0 ? high[1] : low[1];
        const u32 z = (corner & 4u) != 0 ? high[2] : low[2];
        const f32 weight = ((corner & 1u) != 0 ? between[0] : 1.0f - between[0]) *
                           ((corner & 2u) != 0 ? between[1] : 1.0f - between[1]) *
                           ((corner & 4u) != 0 ? between[2] : 1.0f - between[2]);
        const f32* at = &from[((static_cast<usize>(z) * counts[1] + y) * counts[0] + x) * 12];
        for (u32 channel = 0; channel < 3; ++channel) {
            const f32 value =
                at[channel] + at[3 + channel] * normal.x + at[6 + channel] * normal.y + at[9 + channel] * normal.z;
            (&light.r)[channel] += std::max(value, 0.0f) * weight;
        }
    }
    return light;
}

void VolumeBake::fillDropped(std::vector<f32>& values) const
{
    // **A point in a wall takes its neighbours' light**: what is left there is
    // only ever read between two points that are not, and nought in it would
    // be a dark line along every wall.
    const usize samples = sampleCount();
    std::vector<core::u8> known(samples);
    bool none = true;
    for (usize sample = 0; sample < samples; ++sample) {
        known[sample] = m_inside[sample] == 0 ? 1 : 0;
        none = none && known[sample] != 0;
    }
    if (none)
        return;
    const u32 nx = m_layout.counts[0];
    const u32 ny = m_layout.counts[1];
    const u32 nz = m_layout.counts[2];
    for (u32 round = 0; round < 6; ++round) {
        std::vector<core::u8> next = known;
        bool any = false;
        for (usize sample = 0; sample < samples; ++sample) {
            if (known[sample] != 0)
                continue;
            const auto x = static_cast<core::i32>(sample % nx);
            const auto y = static_cast<core::i32>((sample / nx) % ny);
            const auto z = static_cast<core::i32>(sample / (static_cast<usize>(nx) * ny));
            f32 sum[12]{};
            u32 neighbours = 0;
            static constexpr core::i32 kSteps[6][3] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
            for (const auto& step : kSteps) {
                const core::i32 ox = x + step[0];
                const core::i32 oy = y + step[1];
                const core::i32 oz = z + step[2];
                if (ox < 0 || oy < 0 || oz < 0 || ox >= static_cast<core::i32>(nx) ||
                    oy >= static_cast<core::i32>(ny) || oz >= static_cast<core::i32>(nz))
                    continue;
                const usize other =
                    (static_cast<usize>(oz) * ny + static_cast<usize>(oy)) * nx + static_cast<usize>(ox);
                if (known[other] == 0)
                    continue;
                for (u32 value = 0; value < 12; ++value)
                    sum[value] += values[other * 12 + value];
                ++neighbours;
            }
            if (neighbours == 0)
                continue;
            for (u32 value = 0; value < 12; ++value)
                values[sample * 12 + value] = sum[value] / static_cast<f32>(neighbours);
            next[sample] = 1;
            any = true;
        }
        known = std::move(next);
        if (!any)
            break;
    }
}

void VolumeBake::gatherBegin()
{
    // What a ray that meets nothing sees, once a direction: the same sixty-four
    // from every point.
    for (u32 index = 0; index < kBakeRays; ++index) {
        m_directions[index] = bakeDirection(index, kBakeRays);
        m_sky[index] = m_lit.sky ? m_lit.sky(m_directions[index]) : Color3{0.0f, 0.0f, 0.0f};
    }
    const usize samples = sampleCount();
    m_light.assign(samples * 12, 0.0f);
    m_scratch.assign(samples * 12, 0.0f);
    m_pass = 0;
}

// One pass over some of the points. The first pass is what each sees of the
// sky and of what glows; each one after it, the surfaces round it lit by the
// sun, the lamps and the light the pass before found there.
void VolumeBake::gatherSamples(usize begin, usize end)
{
    jobs::parallelFor(
        "indirect.gather", jobs::Domain::Render, begin, end, 16,
        [](void* user, usize first, usize last, u32) noexcept {
            VolumeBake& self = *static_cast<VolumeBake*>(user);
            const bool bounced = self.m_pass > 0;
            for (usize sample = first; sample < last; ++sample) {
                if (self.m_inside[sample] != 0)
                    continue;
                // The light from every side as one colour, and how it leans: the
                // mean of what the rays saw, and twice the mean of each ray's
                // light along its own direction -- which, read against a normal,
                // is the light on a surface facing that way, over pi.
                f32 mean[3]{};
                f32 lean[3][3]{};
                for (u32 index = 0; index < kBakeRays; ++index) {
                    const Ray& ray = self.m_rays[sample * kBakeRays + index];
                    Color3 seen{0.0f, 0.0f, 0.0f};
                    if (ray.met == 0) {
                        seen = self.m_sky[index];
                    }
                    else if (ray.met == 1) {
                        seen = ray.emissive;
                        if (bounced) {
                            const Color3 around = self.lookUp(self.m_scratch, ray.place, ray.normal);
                            seen = seen + ray.albedo * (ray.direct + around);
                        }
                    }
                    const Vec3 direction = self.m_directions[index];
                    for (u32 channel = 0; channel < 3; ++channel) {
                        const f32 value = (&seen.r)[channel];
                        mean[channel] += value;
                        lean[0][channel] += value * direction.x;
                        lean[1][channel] += value * direction.y;
                        lean[2][channel] += value * direction.z;
                    }
                }
                f32* out = &self.m_light[sample * 12];
                for (u32 channel = 0; channel < 3; ++channel) {
                    const f32 constant = mean[channel] / static_cast<f32>(kBakeRays);
                    f32 axis[3] = {2.0f * lean[0][channel] / static_cast<f32>(kBakeRays),
                                   2.0f * lean[1][channel] / static_cast<f32>(kBakeRays),
                                   2.0f * lean[2][channel] / static_cast<f32>(kBakeRays)};
                    // Never so lopsided that the side turned from the light is
                    // lit by less than nothing.
                    const f32 length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
                    const f32 scale = length > constant && length > 0.0f ? constant / length : 1.0f;
                    out[channel] = constant;
                    out[3 + channel] = axis[0] * scale;
                    out[6 + channel] = axis[1] * scale;
                    out[9 + channel] = axis[2] * scale;
                }
            }
        },
        this);
}

bool VolumeBake::gatherPassEnd()
{
    fillDropped(m_light);
    if (m_pass < m_bounces) {
        m_scratch = m_light;
        ++m_pass;
        return false;
    }
    const usize samples = sampleCount();
    m_dropped = static_cast<u32>(std::count(m_inside.begin(), m_inside.end(), core::u8{1}));

    // **Each point takes a little of the points beside it.** Sixty-four rays
    // find a patch of sun on a floor with two of them from here and three
    // from the next point over, and a wall lit by the difference is blotched;
    // light that has bounced does not change that fast from one metre to the
    // next, so what does is noise. One pass, a half of itself and a quarter
    // of each neighbour along each side in turn.
    const u32 counts[3] = {m_layout.counts[0], m_layout.counts[1], m_layout.counts[2]};
    const usize strides[3] = {1, counts[0], static_cast<usize>(counts[0]) * counts[1]};
    for (u32 axis = 0; axis < 3; ++axis) {
        if (counts[axis] < 3)
            continue;
        m_scratch = m_light;
        for (usize sample = 0; sample < samples; ++sample) {
            const usize along = (sample / strides[axis]) % counts[axis];
            const usize before = along > 0 ? sample - strides[axis] : sample;
            const usize after = along + 1 < counts[axis] ? sample + strides[axis] : sample;
            for (u32 value = 0; value < 12; ++value) {
                m_light[sample * 12 + value] = 0.5f * m_scratch[sample * 12 + value] +
                                               0.25f * (m_scratch[before * 12 + value] + m_scratch[after * 12 + value]);
            }
        }
    }
    return true;
}

bool VolumeBake::step(const BakeTrace& trace, const BakeLight& light, u64 budgetNs)
{
    if (m_stage == Stage::Idle)
        return false;
    const u64 started = platform::nowNs();
    struct Work
    {
        VolumeBake* self;
        const BakeTrace* trace;
    };
    Work work{this, &trace};
    while (m_stage != Stage::Done) {
        const usize samples = sampleCount();
        if (m_stage == Stage::Trace) {
            const usize end = budgetNs == 0 ? samples : std::min(samples, m_cursor + kChunk);
            jobs::parallelFor(
                "indirect.rays", jobs::Domain::Render, m_cursor, end, kGrain,
                [](void* user, usize begin, usize last, u32) noexcept {
                    const Work& asked = *static_cast<const Work*>(user);
                    for (usize sample = begin; sample < last; ++sample)
                        asked.self->traceSample(sample, *asked.trace);
                },
                &work);
            m_cursor = end;
            if (m_cursor >= samples) {
                m_stage = Stage::Light;
                m_cursor = 0;
                m_lightHeld = false;
            }
        }
        else if (m_stage == Stage::Light) {
            // The light as it is when the lighting begins, and no other until
            // the volume is whole.
            if (!m_lightHeld) {
                m_lightFreshOnly = !m_fresh.empty() && light.stamp != 0 && light.stamp == m_scrolledStamp;
                m_lit = light;
                m_lightHeld = true;
            }
            const usize end = budgetNs == 0 ? samples : std::min(samples, m_cursor + kChunk);
            jobs::parallelFor(
                "indirect.light", jobs::Domain::Render, m_cursor, end, kGrain,
                [](void* user, usize begin, usize last, u32) noexcept {
                    const Work& asked = *static_cast<const Work*>(user);
                    for (usize sample = begin; sample < last; ++sample)
                        asked.self->lightSample(sample, *asked.trace);
                },
                &work);
            m_cursor = end;
            if (m_cursor >= samples) {
                m_stage = Stage::Gather;
                m_cursor = 0;
                gatherBegin();
            }
        }
        else {
            // **The gathering is in pieces too.** It was done in one go
            // whatever the budget -- every point, every ray, once a bounce --
            // which for a volume of a few thousand points was several
            // milliseconds of one frame each time it was lit again.
            const usize end = budgetNs == 0 ? samples : std::min(samples, m_cursor + kGatherChunk);
            gatherSamples(m_cursor, end);
            m_cursor = end;
            if (m_cursor >= samples) {
                m_cursor = 0;
                if (gatherPassEnd()) {
                    m_stage = Stage::Done;
                    m_fresh.clear();
                    m_lightFreshOnly = false;
                }
            }
        }
        if (budgetNs != 0 && platform::nowNs() - started >= budgetNs)
            break;
    }
    return m_stage == Stage::Done;
}

usize pickRelight(std::span<const RelightCandidate> volumes) noexcept
{
    usize chosen = volumes.size();
    for (usize index = 0; index < volumes.size(); ++index) {
        const RelightCandidate& one = volumes[index];
        if (!one.stale)
            continue;
        if (chosen == volumes.size()) {
            chosen = index;
            continue;
        }
        const RelightCandidate& best = volumes[chosen];
        if (one.inView != best.inView) {
            if (one.inView)
                chosen = index;
            continue;
        }
        if (one.served != best.served) {
            if (one.served < best.served)
                chosen = index;
            continue;
        }
        if (one.distance < best.distance)
            chosen = index;
    }
    return chosen;
}

// --- The host ---------------------------------------------------------------

IndirectLight::ProbeState& IndirectLight::probeState(u64 key)
{
    for (ProbeState& state : m_probeStates) {
        if (state.key == key)
            return state;
    }
    m_probeStates.push_back(ProbeState{.key = key});
    return m_probeStates.back();
}

IndirectLight::VolumeState& IndirectLight::volumeState(u64 key)
{
    for (VolumeState& state : m_volumeStates) {
        if (state.key == key)
            return state;
    }
    m_volumeStates.emplace_back();
    m_volumeStates.back().key = key;
    return m_volumeStates.back();
}

void IndirectLight::update(const Frame& frame)
{
    m_frameStartNs = frame.nowNs;
    m_flatOut = frame.flatOut;
    m_facesThisFrame = 0;
    m_probes.clear();
    m_volumes.clear();
    m_stats.probes = 0;
    m_stats.volumes = 0;
    if (frame.world == nullptr || frame.renderer == nullptr || frame.snapshot == nullptr ||
        !frame.snapshot->camera.valid) {
        m_settled = true;
        if (frame.world != nullptr && frame.renderer == nullptr) {
            // Nothing takes a picture or casts a ray here: every probe and
            // volume is as ready as it will ever be. (A frame with a
            // renderer and no camera yet says nothing either way.)
            m_probeBudget = 0;
            m_volumesOn = false;
            sayReady(frame);
        }
        return;
    }
    scene::World& world = *frame.world;
    const render::RenderWorld& snapshot = *frame.snapshot;
    const render::GraphicsSettings& settings = frame.renderer->settings();
    m_probeBudget = settings.reflectionProbes;
    m_probeSize = settings.reflectionProbeSize;
    m_volumesOn = (settings.indirectLight || frame.baking) && frame.physics != nullptr;
    m_workspace = frame.workspace;
    for (Flat& flat : m_flats)
        flat.seen = false;
    m_watchedBudgetNs = static_cast<u64>(std::clamp(settings.indirectLightBudgetMs, 0.25f, 16.0f) * 1.0e6f);
    m_camera = snapshot.camera.origin;

    // --- The light, as the frame is lit ---------------------------------------
    const bool air = snapshot.look.atmosphere.present;
    render::SkyParams sky = render::skyParamsFor(snapshot.environment.sunDirection,
                                                 air ? snapshot.look.atmosphere.color : snapshot.environment.fogColor);
    if (snapshot.look.sky.present && snapshot.look.sky.image.valid())
        sky.skybox = snapshot.look.sky.radiance;
    m_light = BakeLight{};
    m_light.sunDirection = sky.lightDirection;
    const f32 strength = snapshot.environment.sunBrightness * sky.lightFactor;
    m_light.sunRadiance = Color3{sky.lightColor.r * snapshot.environment.lightTint.r * strength,
                                 sky.lightColor.g * snapshot.environment.lightTint.g * strength,
                                 sky.lightColor.b * snapshot.environment.lightTint.b * strength};
    // No disc: the sun lights a surface by itself, and a ray that happened
    // upon it would count it twice and make one point of a room blaze.
    render::SkyParams open = sky;
    open.celestial = false;
    const f32 skyScale = snapshot.environment.environmentDiffuseScale;
    m_light.sky = [open, skyScale](Vec3 direction) {
        const Vec3 seen = render::evaluateSky(open, direction);
        return Color3{std::min(seen.x, 16.0f) * skyScale, std::min(seen.y, 16.0f) * skyScale,
                      std::min(seen.z, 16.0f) * skyScale};
    };
    const auto lamp = [&](InstanceId id, Color3 colour, f32 brightness, f32 range, f32 cosOuter, bool shadows) {
        const std::optional<render::LightAnchor> anchored = render::lightAnchorOf(world, id);
        if (!anchored.has_value() || brightness <= 0.0f || range <= 0.0f)
            return;
        const core::CFrameD anchor = anchored->partFrame * anchored->offset;
        m_light.lamps.push_back(BakeLamp{
            .position = anchor.position,
            .direction = core::normalize(core::transformDirection(anchor, Vec3{0.0f, 0.0f, -1.0f})),
            .radiance = colour * brightness,
            .range = range,
            .cosOuter = cosOuter,
            .shadows = shadows,
        });
    };
    world.pointLights().forEach([&](InstanceId id, const scene::PointLightComponent& light) {
        if (light.enabled && inWorkspace(world, id, frame.workspace))
            lamp(id, light.color, light.brightness, light.range, -1.0f, light.shadows);
    });
    world.spotLights().forEach([&](InstanceId id, const scene::SpotLightComponent& light) {
        if (light.enabled && inWorkspace(world, id, frame.workspace))
            lamp(id, light.color, light.brightness, light.range, std::cos(light.angle * 0.5f * kPi / 180.0f),
                 light.shadows);
    });

    // --- What there is ----------------------------------------------------------
    for (ProbeState& state : m_probeStates)
        state.seen = false;
    for (VolumeState& state : m_volumeStates)
        state.seen = false;
    if (m_probeBudget > 0) {
        world.reflectionProbes().forEach([&](InstanceId id, const scene::ReflectionProbeComponent& set) {
            const scene::PartComponent* part = world.parts().find(world.parentOf(id));
            if (!set.enabled || part == nullptr || !inWorkspace(world, id, frame.workspace))
                return;
            Probe probe;
            probe.id = id;
            probe.key = keyOf(id);
            probe.box = part->cframe;
            probe.half = Vec3{part->size.x * 0.5f, part->size.y * 0.5f, part->size.z * 0.5f};
            probe.set = set;
            probe.distance = distanceToBox(probe.box, probe.half, m_camera);
            probe.inView = probe.distance < kNearMetres || boxInView(snapshot.camera, probe.box, part->size);
            m_probes.push_back(probe);
        });
    }
    std::stable_sort(m_probes.begin(), m_probes.end(),
                     [](const Probe& a, const Probe& b) { return a.distance < b.distance; });
    // Past how many the machine keeps, the furthest are the sky.
    for (usize index = m_probeBudget; index < m_probes.size(); ++index)
        frame.renderer->dropReflectionProbe(m_probes[index].key);
    if (m_probes.size() > m_probeBudget)
        m_probes.resize(m_probeBudget);
    for (const Probe& probe : m_probes)
        probeState(probe.key).seen = true;
    // Baking one volume on a machine that works none out: that one is worked
    // out, and every other is still drawn as its file's colour.
    const bool bakingOnlyOne = !settings.indirectLight && frame.bakingOne.valid();
    if (m_volumesOn) {
        world.irradianceVolumes().forEach([&](InstanceId id, const scene::IrradianceVolumeComponent& set) {
            const scene::PartComponent* part = world.parts().find(world.parentOf(id));
            if (!set.enabled || part == nullptr || !inWorkspace(world, id, frame.workspace))
                return;
            if (bakingOnlyOne && id != frame.bakingOne)
                return;
            // **As pieces** (ADR 0206): levels round the camera, or the box
            // itself -- whole, or as tiles where it is more than one volume
            // holds. Each piece is a volume of its own from here on.
            const std::vector<VolumePiece> pieces =
                set.followsCamera
                    ? volumeLevelsOf(part->size, set.spacing, static_cast<u32>(std::max(set.levels, 1)), m_camera)
                    : volumeTilesOf(part->cframe, part->size, set.spacing);
            for (usize index = 0; index < pieces.size(); ++index) {
                const VolumePiece& piece = pieces[index];
                Volume volume;
                volume.id = id;
                volume.key = keyOf(id) ^ (static_cast<u64>(index + 1) << 56);
                volume.piece = static_cast<u32>(index);
                volume.pieces = static_cast<u32>(pieces.size());
                volume.box = piece.box;
                volume.size = piece.size;
                volume.layout = piece.layout;
                volume.margin = piece.margin;
                volume.follows = set.followsCamera;
                volume.set = set;
                const Vec3 half{piece.size.x * 0.5f, piece.size.y * 0.5f, piece.size.z * 0.5f};
                volume.distance = distanceToBox(volume.box, half, m_camera);
                volume.inView = volume.distance < kNearMetres || boxInView(snapshot.camera, volume.box, piece.size);
                m_volumes.push_back(volume);
            }
        });
    }
    if (!m_volumesOn || bakingOnlyOne) {
        // **A machine that works no volume out** (ADR 0207): one that has a
        // file is drawn as the file's one colour, the mean of its light, over
        // the whole of its box. Nothing is cast, nothing is uploaded.
        world.irradianceVolumes().forEach([&](InstanceId id, const scene::IrradianceVolumeComponent& set) {
            const scene::PartComponent* part = world.parts().find(world.parentOf(id));
            if (!set.enabled || set.followsCamera || set.baked.id == 0 || part == nullptr ||
                !inWorkspace(world, id, frame.workspace))
                return;
            if (bakingOnlyOne && id == frame.bakingOne)
                return;
            // Two ticks in, as a volume that is worked out waits: what a
            // script builds as it starts is in the room by then.
            if (frame.tick < 2)
                return;
            const u64 key = keyOf(id);
            Flat* held = nullptr;
            for (Flat& flat : m_flats) {
                if (flat.key == key)
                    held = &flat;
            }
            // Looked at once, and again when it is another volume: its box
            // moved or resized, or what it is worked out as changed -- or its
            // file was read again.
            const bool another = held == nullptr || !(held->box == part->cframe) || !(held->size == part->size) ||
                                 held->set.baked.id != set.baked.id || held->set.spacing != set.spacing ||
                                 held->set.bounces != set.bounces || held->set.mode != set.mode ||
                                 held->read != keptFor(frame, set.baked).read;
            if (another) {
                if (held == nullptr) {
                    m_flats.push_back(Flat{});
                    held = &m_flats.back();
                    held->key = key;
                }
                held->box = part->cframe;
                held->size = part->size;
                bool said = false;
                const BakedLight* kept =
                    bakedFor(frame, id, set,
                             static_cast<u32>(volumeTilesOf(part->cframe, part->size, set.spacing).size()), said);
                held->lit = kept != nullptr;
                held->read = keptFor(frame, set.baked).read;
                if (kept != nullptr) {
                    held->mean = kept->mean;
                    const std::array<core::I18nArg, 5> args{
                        core::I18nArg{"name", world.atoms().text(world.name(world.parentOf(id)))},
                        core::I18nArg{"path", world.atoms().text(set.baked)},
                        core::I18nArg{"r", static_cast<f64>(kept->mean.r)},
                        core::I18nArg{"g", static_cast<f64>(kept->mean.g)},
                        core::I18nArg{"b", static_cast<f64>(kept->mean.b)}};
                    core::log(core::LogLevel::Info, ENG_TR("app.info.irradiance_flat"), args);
                }
            }
            // What it is drawn with is its own to change every frame.
            held->set = set;
            held->seen = true;
        });
    }
    std::erase_if(m_flats, [](const Flat& flat) { return !flat.seen; });
    m_stats.flat =
        static_cast<u32>(std::count_if(m_flats.begin(), m_flats.end(), [](const Flat& flat) { return flat.lit; }));
    std::stable_sort(m_volumes.begin(), m_volumes.end(),
                     [](const Volume& a, const Volume& b) { return a.distance < b.distance; });
    for (const Volume& volume : m_volumes)
        volumeState(volume.key).seen = true;
    // What is gone -- destroyed, turned off, another scene -- is forgotten,
    // and its map given back.
    for (const ProbeState& state : m_probeStates) {
        if (!state.seen)
            frame.renderer->dropReflectionProbe(state.key);
    }
    std::erase_if(m_probeStates, [](const ProbeState& state) { return !state.seen; });
    std::erase_if(m_volumeStates, [](const VolumeState& state) { return !state.seen; });
    if (m_job.live &&
        std::none_of(m_probes.begin(), m_probes.end(), [&](const Probe& probe) { return probe.key == m_job.key; }))
        m_job = Job{};
    m_stats.probes = static_cast<u32>(m_probes.size());
    m_stats.volumes = static_cast<u32>(m_volumes.size());

    // --- The work ---------------------------------------------------------------
    if (frame.loadersIdle) {
        stepVolumes(frame);
        stepLightmaps(frame);
    }
    else if (!frame.bakingLightmaps) {
        m_lightmap.reset();
    }
    // What arrives while the world is watched comes in over a moment.
    for (ProbeState& state : m_probeStates)
        state.presence = std::min(1.0f, state.presence + frame.seconds * kFadeRate);
    for (VolumeState& state : m_volumeStates) {
        state.presence = std::min(1.0f, state.presence + frame.seconds * kFadeRate);
        if (state.mix < 1.0f && !state.to.empty()) {
            state.mix = std::min(1.0f, state.mix + frame.seconds * kFadeRate);
            for (usize index = 0; index < state.shown.size(); ++index)
                state.shown[index] = state.from[index] + (state.to[index] - state.from[index]) * state.mix;
            ++state.revision;
        }
    }
    if (frame.loadersIdle)
        planProbe(frame);
    sayReady(frame);

    // --- Whether anything in view is still the sky standing in -----------------
    bool outstanding = false;
    m_stats.probesHeld = 0;
    m_stats.volumesWhole = 0;
    for (const Probe& probe : m_probes) {
        const ProbeState& state = probeState(probe.key);
        const bool held = state.taken && frame.renderer->holdsReflectionProbe(probe.key);
        m_stats.probesHeld += held ? 1 : 0;
        outstanding = outstanding || (probe.inView && (!held || state.takes < 2));
    }
    for (const Volume& volume : m_volumes) {
        const bool whole = volumeState(volume.key).hasResult;
        m_stats.volumesWhole += whole ? 1 : 0;
        outstanding = outstanding || (volume.inView && !whole);
    }
    if (outstanding && m_outstandingSinceNs == 0)
        m_outstandingSinceNs = frame.nowNs;
    if (!outstanding && m_outstandingSinceNs != 0) {
        m_stats.settledMs = static_cast<f64>(frame.nowNs - m_outstandingSinceNs) / 1'000'000.0;
        m_outstandingSinceNs = 0;
    }
    m_settled = !outstanding;
}

void IndirectLight::stepVolumes(const Frame& frame)
{
    if (!m_volumesOn || m_volumes.empty())
        return;
    scene::World& world = *frame.world;
    const BakeTrace trace =
        traceOf(world, *frame.physics, m_surfaces, m_traced, m_tracedBoxes, m_tracedBodies, frame.textures);
    const u64 budget = frame.flatOut ? kCurtainBudgetNs : m_watchedBudgetNs;
    const u64 started = platform::nowNs();

    // --- What each volume is, and what it is owed -------------------------------
    // The light round each as one number, and whether one is being lit again.
    std::vector<u64> stamps(m_volumes.size());
    std::vector<RelightCandidate> candidates(m_volumes.size());
    bool relighting = false;
    for (usize index = 0; index < m_volumes.size(); ++index) {
        const Volume& volume = m_volumes[index];
        VolumeState& state = volumeState(volume.key);
        const Vec3 half{volume.size.x * 0.5f, volume.size.y * 0.5f, volume.size.z * 0.5f};
        stamps[index] = lightAround(m_light, volume.box, half, nullptr);

        // Laid out again when the box is another box or `Refresh` was called;
        // lit again, the rays kept, when only the light changed.
        // **A level the camera drew after it is the same volume, carried**
        // (ADR 0206): not begun again, but queued with the volumes whose
        // light is old, and given its turn.
        const bool moved = !(state.box == volume.box);
        const bool another = !state.asked || (moved && !volume.follows) || !(state.size == volume.size) ||
                             state.spacing != volume.set.spacing || state.bounces != volume.set.bounces ||
                             state.mode != volume.set.mode || state.refreshes != volume.set.refreshes ||
                             // Made `Dynamic` after it gave its rays back, or
                             // was given its light by a file: there is nothing
                             // to light again, so it is worked out again.
                             (volume.set.dynamic && !state.pending && state.bake.whole() && !state.bake.hasRays()) ||
                             // **Its file read again** -- baked again, or
                             // changed on disk -- or another file named: given
                             // the new one, as it was given the first.
                             (state.fileRead != 0 && !frame.baking &&
                              (volume.set.baked.id == 0 || keptFor(frame, volume.set.baked).read != state.fileRead));
        if (another) {
            // Asked for now, begun two ticks on: what its rays are cast
            // against is the physics world, which a tick brings up to the
            // scene -- a wall built this frame is not in it yet, and neither
            // is a scene that has only just opened.
            state.asked = true;
            state.askedTick = frame.tick;
            state.box = volume.box;
            state.size = volume.size;
            state.spacing = volume.set.spacing;
            state.bounces = volume.set.bounces;
            state.mode = volume.set.mode;
            state.refreshes = volume.set.refreshes;
            state.pending = true;
            state.ready = false;
            state.fileRead = 0;
        }
        if (state.pending && frame.tick >= state.askedTick + 2) {
            state.pending = false;
            state.relit = false;
            readSurfaces(frame);
            // **A mesh is, to a ray, its collision shape**: a hull or a box,
            // not its triangles. Said once a volume, by name, so an author
            // knows why a doorway modelled into one mesh lets no light in.
            if (!state.warnedMeshes) {
                state.warnedMeshes = true;
                const core::CFrameD intoBox = core::inverse(volume.box);
                u32 count = 0;
                std::string names;
                world.meshParts().forEach([&](InstanceId id, const scene::MeshPartComponent&) {
                    const scene::PartComponent* part = world.parts().find(id);
                    const scene::RigidBodyComponent* body = world.rigidBodies().find(id);
                    if (part == nullptr || body == nullptr || !body->anchored || !body->canQuery ||
                        !inWorkspace(world, id, frame.workspace))
                        return;
                    // One asked by its own triangles is its own shape.
                    if (std::any_of(m_traced.begin(), m_traced.end(),
                                    [id](const TracedMesh& traced) { return traced.id == id; }))
                        return;
                    const DVec3 local = core::transformPoint(intoBox, part->cframe.position);
                    if (std::abs(local.x) > static_cast<f64>(half.x) || std::abs(local.y) > static_cast<f64>(half.y) ||
                        std::abs(local.z) > static_cast<f64>(half.z))
                        return;
                    if (count < 4) {
                        if (!names.empty())
                            names += ", ";
                        names += world.atoms().text(world.name(id));
                    }
                    ++count;
                });
                if (count > 0) {
                    const std::array<core::I18nArg, 3> args{
                        core::I18nArg{"name", world.atoms().text(world.name(world.parentOf(volume.id)))},
                        core::I18nArg{"count", static_cast<core::i64>(count)},
                        core::I18nArg{"names", std::string_view{names}}};
                    core::log(core::LogLevel::Warn, ENG_TR("app.warn.irradiance_volume_meshes"), args);
                }
            }
            const VolumeLayout& layout = volume.layout;
            if (layout.coarser && !state.warned) {
                state.warned = true;
                const std::array<core::I18nArg, 2> args{
                    core::I18nArg{"name", world.atoms().text(world.name(world.parentOf(volume.id)))},
                    core::I18nArg{"spacing", static_cast<f64>(layout.spacing)}};
                core::log(core::LogLevel::Warn, ENG_TR("app.warn.irradiance_volume_coarser"), args);
            }
            state.startedNs = frame.nowNs;
            state.spentNs = 0;
            // **Kept in a file** (ADR 0207): the light is the file's, there
            // the frame the volume is first asked for, and no ray is cast.
            // `Dynamic`, the file is its first light and it is worked out
            // all the same, to be lit again as the light moves. `Refresh`
            // is an author asking for rays.
            bool given = false;
            if (!frame.baking && !volume.follows && volume.set.baked.id != 0 && volume.set.refreshes == 0) {
                const BakedLight* kept = bakedFor(frame, volume.id, volume.set, volume.pieces, state.warnedBaked);
                state.fileRead = keptFor(frame, volume.set.baked).read;
                if (kept != nullptr) {
                    const BakedPiece& piece = kept->pieces[volume.piece];
                    state.shown = piece.samples;
                    state.to.clear();
                    state.mix = 1.0f;
                    state.shownBox = state.box;
                    state.shownSize = state.size;
                    state.presence = 1.0f;
                    state.hasResult = true;
                    state.ready = true;
                    ++state.revision;
                    ++m_stats.kept;
                    // Said once a volume: an author sees the file is in use.
                    if (volume.piece == 0) {
                        core::usize points = 0;
                        for (const BakedPiece& one : kept->pieces)
                            points += one.samples.size() / 12;
                        const std::array<core::I18nArg, 3> args{
                            core::I18nArg{"name", world.atoms().text(world.name(world.parentOf(volume.id)))},
                            core::I18nArg{"path", world.atoms().text(volume.set.baked)},
                            core::I18nArg{"points", static_cast<core::i64>(points)}};
                        core::log(core::LogLevel::Info, ENG_TR("app.info.irradiance_kept"), args);
                    }
                    if (!volume.set.dynamic) {
                        state.bake.give(volume.box, volume.size, layout, piece.samples);
                        given = true;
                    }
                }
            }
            if (!given)
                state.bake.begin(volume.box, volume.size, layout, static_cast<u32>(volume.set.bounces));
        }

        const bool working = !state.pending && state.bake.begun() && !state.bake.whole();
        relighting = relighting || (working && state.relit);
        candidates[index] = RelightCandidate{
            .stale = !state.pending && state.bake.whole() &&
                     ((volume.set.dynamic && state.bake.litBy() != stamps[index]) || (volume.follows && moved)),
            .inView = volume.inView,
            .distance = volume.distance,
            .served = state.served,
        };
    }

    // --- One volume lit again at a time ---------------------------------------
    // **What has begun finishes, with the light it began with, and is shown;
    // then the next begins.** A volume used to begin again the frame after
    // it finished whenever the light had moved in the meantime -- under a
    // sun that sets, or beside a torch that flickers, always -- and being
    // the nearest it was served first: every other volume waited for ever,
    // one that had never been lit at all among them.
    if (!relighting) {
        if (const usize next = pickRelight(candidates); next < m_volumes.size()) {
            const Volume& volume = m_volumes[next];
            VolumeState& state = volumeState(volume.key);
            readSurfaces(frame);
            state.relit = true;
            state.carried = false;
            state.served = ++m_relightClock;
            state.startedNs = frame.nowNs;
            state.spentNs = 0;
            if (volume.follows && !(state.box == volume.box)) {
                // How far, in its own cells along its own sides.
                const Vec3 gone = core::transformDirection(core::inverse(state.box),
                                                           core::toVec3(volume.box.position - state.box.position));
                core::i32 cells[3];
                for (u32 axis = 0; axis < 3; ++axis) {
                    const f32 cell = (&volume.size.x)[axis] / static_cast<f32>(volume.layout.counts[axis]);
                    cells[axis] = static_cast<core::i32>(std::lround((&gone.x)[axis] / cell));
                }
                if (state.bake.scroll(volume.box, cells)) {
                    state.box = volume.box;
                    state.carried = true;
                    std::copy(std::begin(cells), std::end(cells), std::begin(state.carriedCells));
                }
                else {
                    // Further than it is long: nothing of where it stood is
                    // of use where it stands. Begun again, as a new one is.
                    state.relit = false;
                    state.asked = false;
                }
            }
            else {
                state.bake.relight();
            }
        }
    }

    // --- The work, within the frame's budget -------------------------------------
    // A volume with no light yet before one whose light is old; among each,
    // what is in view first, and the nearest of that.
    std::vector<usize> order;
    order.reserve(m_volumes.size());
    for (u32 turn = 0; turn < 4; ++turn) {
        for (usize index = 0; index < m_volumes.size(); ++index) {
            const VolumeState& state = volumeState(m_volumes[index].key);
            if (state.pending || !state.bake.begun() || state.bake.whole())
                continue;
            const u32 rank = (state.hasResult ? 2u : 0u) + (m_volumes[index].inView ? 0u : 1u);
            if (rank == turn)
                order.push_back(index);
        }
    }
    for (const usize index : order) {
        const u64 spent = platform::nowNs() - started;
        if (spent >= budget)
            break;
        const Volume& volume = m_volumes[index];
        VolumeState& state = volumeState(volume.key);
        const Vec3 half{volume.size.x * 0.5f, volume.size.y * 0.5f, volume.size.z * 0.5f};
        BakeLight light = m_light;
        light.lamps.clear();
        light.stamp = lightAround(m_light, volume.box, half, &light.lamps);
        // **What bounces alone** (`Enum.IrradianceMode.BounceOnly`): a ray
        // that meets nothing sees nothing. The sky lights the surface itself,
        // as it does outside the volume, and is not counted here as well.
        if (state.mode == 1)
            light.sky = nullptr;
        const u64 before = platform::nowNs();
        const bool whole = state.bake.step(trace, light, budget - spent);
        state.spentNs += platform::nowNs() - before;
        if (!whole)
            continue;
        const std::span<const f32> result = state.bake.result();
        // **Carried, what is drawn is carried with it**: the points it had
        // where it stood keep the light that was drawn there, to go to the
        // new as any volume's does; the points that are new are simply
        // there.
        if (state.carried && state.hasResult && state.shown.size() == result.size()) {
            const core::i32 counts[3] = {static_cast<core::i32>(volume.layout.counts[0]),
                                         static_cast<core::i32>(volume.layout.counts[1]),
                                         static_cast<core::i32>(volume.layout.counts[2])};
            std::vector<f32> was(result.begin(), result.end());
            for (core::i32 z = 0; z < counts[2]; ++z) {
                for (core::i32 y = 0; y < counts[1]; ++y) {
                    for (core::i32 x = 0; x < counts[0]; ++x) {
                        const core::i32 ox = x + state.carriedCells[0];
                        const core::i32 oy = y + state.carriedCells[1];
                        const core::i32 oz = z + state.carriedCells[2];
                        if (ox < 0 || oy < 0 || oz < 0 || ox >= counts[0] || oy >= counts[1] || oz >= counts[2])
                            continue;
                        const usize now =
                            ((static_cast<usize>(z) * static_cast<usize>(counts[1]) + static_cast<usize>(y)) *
                                 static_cast<usize>(counts[0]) +
                             static_cast<usize>(x)) *
                            12;
                        const usize old =
                            ((static_cast<usize>(oz) * static_cast<usize>(counts[1]) + static_cast<usize>(oy)) *
                                 static_cast<usize>(counts[0]) +
                             static_cast<usize>(ox)) *
                            12;
                        std::copy_n(state.shown.begin() + static_cast<std::ptrdiff_t>(old), 12,
                                    was.begin() + static_cast<std::ptrdiff_t>(now));
                    }
                }
            }
            state.shown = std::move(was);
        }
        state.shownBox = state.box;
        state.shownSize = state.size;
        // Behind the curtain, and the first time, it is simply there;
        // watched, the old light goes to the new.
        if (!state.hasResult || state.shown.size() != result.size() || frame.flatOut) {
            state.shown.assign(result.begin(), result.end());
            state.to.clear();
            state.mix = 1.0f;
            if (!state.hasResult)
                state.presence = frame.flatOut ? 1.0f : 0.0f;
        }
        else {
            state.from = state.shown;
            state.to.assign(result.begin(), result.end());
            state.mix = 0.0f;
        }
        state.hasResult = true;
        ++state.revision;
        if (state.relit && state.carried) {
            m_stats.lastCarryMs = static_cast<f64>(state.spentNs) / 1'000'000.0;
            ++m_stats.carries;
        }
        else if (state.relit) {
            m_stats.lastRelightPoints = static_cast<u32>(result.size() / 12);
            m_stats.lastRelightMs = static_cast<f64>(state.spentNs) / 1'000'000.0;
            ++m_stats.relights;
        }
        else {
            m_stats.lastVolumePoints = static_cast<u32>(result.size() / 12);
            m_stats.lastVolumeMs = static_cast<f64>(state.spentNs) / 1'000'000.0;
            // Worked out, as it was last asked for.
            state.ready = true;
        }
        state.carried = false;
        // The rays are only worth their memory to a volume that will be lit
        // again, or carried.
        if (!volume.set.dynamic && !volume.follows)
            state.bake.forgetRays();
    }

    // **Ready is ready.** A volume that has been worked out says so from then
    // on: being lit again under a light that moved is upkeep, and a script
    // that waits for a room to be lit is not made to wait for the sun to
    // stand still.
    //
    // **And asked for again is not ready**: `Refresh`, or a box, a spacing, a
    // number of bounces or a mode that is another -- until that is whole.
    //
    // A volume in pieces is as far on as its pieces are between them.
    world.irradianceVolumes().forEach([&](InstanceId id, scene::IrradianceVolumeComponent& set) {
        f32 done = 0.0f;
        u32 pieces = 0;
        for (const Volume& volume : m_volumes) {
            if (volume.id != id)
                continue;
            const VolumeState& state = volumeState(volume.key);
            done += state.ready ? 1.0f : state.pending ? 0.0f : std::min(state.bake.progress(), 0.999f);
            ++pieces;
        }
        if (pieces > 0)
            set.progress = std::min(done / static_cast<f32>(pieces), set.ready ? 1.0f : 0.999f);
    });
}

void IndirectLight::sayReady(const Frame& frame)
{
    scene::World& world = *frame.world;
    world.engineState().volumesWorked = m_volumesOn;
    world.engineState().probesTaken = m_probeBudget > 0;
    const core::NameAtom ready = world.atoms().intern("Completed");
    const auto say = [&](InstanceId id, bool& flag, bool now) {
        if (now && !flag)
            world.changes().push(scene::Change{scene::ChangeKind::InstanceEventNoArgs, id, InstanceId{}, ready});
        flag = now;
    };
    // Pool order, which is the order they were made in: the events of one
    // frame are said in an order that does not depend on the camera.
    world.irradianceVolumes().forEach([&](InstanceId id, scene::IrradianceVolumeComponent& set) {
        // Every piece of it worked out. One the frame did not look at --
        // disabled, out of the world -- has nothing being done for it.
        bool now = true;
        if (m_volumesOn) {
            for (const Volume& volume : m_volumes) {
                if (volume.id != id)
                    continue;
                const VolumeState& state = volumeState(volume.key);
                now = now && state.ready && state.refreshes == set.refreshes;
            }
        }
        if (now)
            set.progress = 1.0f;
        say(id, set.ready, now);
    });
    world.reflectionProbes().forEach([&](InstanceId id, scene::ReflectionProbeComponent& set) {
        // Kept, with both its takes: or past the number the machine keeps,
        // or not looked at at all, and then nothing more will be done.
        bool now = true;
        for (const Probe& probe : m_probes) {
            if (probe.id != id)
                continue;
            const ProbeState& state = probeState(probe.key);
            const bool asked = state.refreshes == probe.set.refreshes;
            const bool taken = state.taken && state.takes >= 2 && frame.renderer != nullptr &&
                               frame.renderer->holdsReflectionProbe(probe.key);
            // Taken again because the light moved, it stays ready: upkeep,
            // as a volume lit again is.
            now = asked && (taken || set.ready);
        }
        say(id, set.ready, now);
    });
}

void IndirectLight::readSurfaces(const Frame& frame)
{
    const scene::World& world = *frame.world;
    m_surfaces.clear();
    const auto note = [&](InstanceId id) {
        if (id.index >= m_surfaces.size())
            m_surfaces.resize(static_cast<usize>(id.index) + 1);
        const Met met = surfaceOf(world, id, frame.textures);
        m_surfaces[id.index] = Surface{.generation = id.generation,
                                       .kind = static_cast<core::u8>(met.lets ? 2 : 1),
                                       .albedo = met.albedo,
                                       .emissive = met.emissive};
    };
    world.parts().forEach([&](InstanceId id, const scene::PartComponent&) { note(id); });
    world.terrains().forEach([&](InstanceId id, const scene::TerrainComponent&) { note(id); });

    // **What is drawn and anchored, by its own triangles**, where the physics
    // world cannot say: a mesh, which is a hull there, and a plain part with
    // its queries off, which is not there at all. One rule for what blocks
    // light and gives it back -- what is drawn and does not move -- whichever
    // class an author happened to use, and whatever its collision is set to.
    m_traced.clear();
    m_tracedBodies.clear();
    m_tracedBoxes = BoxTree{};
    m_stats.tracedMeshes = 0;
    m_stats.tracedTriangles = 0;
    if (frame.meshes == nullptr || !frame.traceMeshes)
        return;
    const auto ask = [&](InstanceId id, const scene::PartComponent& part, core::NameAtom content,
                         const render::MeshLibrary::Entry* entry, Vec3 stretch, Color3 tint, Color3 emissive) {
        if (entry == nullptr || entry->triangles.empty() || entry->positions.empty())
            return;
        Tree* held = nullptr;
        for (const std::unique_ptr<Tree>& tree : m_trees) {
            if (tree->content == content)
                held = tree.get();
        }
        if (held == nullptr) {
            m_trees.push_back(std::make_unique<Tree>());
            held = m_trees.back().get();
            held->content = content;
        }
        if (held->indices != entry->triangles.size() || held->tree.empty()) {
            // What lets light through is left out of the tree: a pane in a
            // window frame is one mesh with the wall round it.
            std::vector<u32> solid;
            solid.reserve(entry->triangles.size());
            held->bounces.clear();
            // The tree numbers its triangles by the list it was given, so the
            // sections are re-ranged to that list.
            u32 next = 0;
            for (const render::MeshLibrary::Entry::Bounce& bounce : entry->bounces) {
                if (bounce.clear)
                    continue;
                for (u32 index = bounce.firstIndex;
                     index < bounce.firstIndex + bounce.indexCount && index < entry->triangles.size(); ++index)
                    solid.push_back(entry->triangles[index]);
                render::MeshLibrary::Entry::Bounce kept = bounce;
                kept.firstIndex = next;
                next += bounce.indexCount;
                held->bounces.push_back(kept);
            }
            held->tree.build(entry->positions, solid);
            held->indices = entry->triangles.size();
        }
        if (held->tree.empty())
            return;
        TracedMesh traced;
        traced.id = id;
        traced.intoMesh = core::inverse(part.cframe);
        traced.turn = part.cframe.rotation;
        traced.stretch = stretch;
        traced.tint = tint;
        traced.emissive = emissive;
        traced.tree = &held->tree;
        traced.bounces = &held->bounces;
        // Its box in the world: the eight corners of the mesh's own.
        traced.low = DVec3{1.0e300, 1.0e300, 1.0e300};
        traced.high = DVec3{-1.0e300, -1.0e300, -1.0e300};
        for (u32 corner = 0; corner < 8; ++corner) {
            const Vec3 local{((corner & 1u) != 0 ? entry->bounds.max.x : entry->bounds.min.x) * stretch.x,
                             ((corner & 2u) != 0 ? entry->bounds.max.y : entry->bounds.min.y) * stretch.y,
                             ((corner & 4u) != 0 ? entry->bounds.max.z : entry->bounds.min.z) * stretch.z};
            const DVec3 placed = core::transformPoint(part.cframe, core::toDVec3(local));
            traced.low = DVec3{std::min(traced.low.x, placed.x), std::min(traced.low.y, placed.y),
                               std::min(traced.low.z, placed.z)};
            traced.high = DVec3{std::max(traced.high.x, placed.x), std::max(traced.high.y, placed.y),
                                std::max(traced.high.z, placed.z)};
        }
        // Its body in the physics world, where it has one, is stepped past.
        if (id.index < m_surfaces.size())
            m_surfaces[id.index].kind = 3;
        if (frame.physics != nullptr) {
            if (const u64 bodyOf = frame.physics->userDataOf(id); bodyOf != 0)
                m_tracedBodies.push_back(bodyOf);
        }
        m_traced.push_back(traced);
        ++m_stats.tracedMeshes;
        m_stats.tracedTriangles += held->tree.triangleCount();
    };
    world.parts().forEach([&](InstanceId id, const scene::PartComponent& part) {
        const scene::RigidBodyComponent* body = world.rigidBodies().find(id);
        if ((body != nullptr && !body->anchored) || world.characterBodies().find(id) != nullptr ||
            !inWorkspace(world, id, frame.workspace))
            return;
        if (const scene::MeshPartComponent* shaped = world.meshParts().find(id); shaped != nullptr) {
            if (!shaped->meshContent.valid())
                return;
            // The file's own materials, under the part's tint.
            const render::PartSurface surface = render::partSurfaceOf(world, part, nullptr);
            if (surface.transparency >= 0.5f)
                return;
            ask(id, part, shaped->meshContent, frame.meshes->find(shaped->meshContent),
                Vec3{part.size.x / shaped->meshSize.x, part.size.y / shaped->meshSize.y,
                     part.size.z / shaped->meshSize.z},
                surface.color, Color3{0.0f, 0.0f, 0.0f});
            return;
        }
        // A plain part the physics world answers for is left to it: its shape
        // there is exact, and there are scenes of ten thousand of them.
        if (body != nullptr && body->canQuery)
            return;
        const render::PartSurface surface = render::partSurfaceOf(world, part, frame.textures);
        if (surface.transparency >= 0.5f)
            return;
        const render::PartShape shape = render::partShapeOf(world, *frame.meshes, id, part);
        ask(id, part, shape.content, shape.entry, shape.stretch, surface.color, surface.emissive);
    });
    std::vector<DVec3> lows;
    std::vector<DVec3> highs;
    lows.reserve(m_traced.size());
    highs.reserve(m_traced.size());
    for (const TracedMesh& traced : m_traced) {
        lows.push_back(traced.low);
        highs.push_back(traced.high);
    }
    m_tracedBoxes.build(lows, highs);
}

void IndirectLight::beginLightmaps(const Frame& frame)
{
    const scene::World& world = *frame.world;
    readSurfaces(frame);
    f32 density = kLightmapDensity;
    world.lighting().forEach(
        [&](InstanceId, const scene::LightingComponent& lighting) { density = lighting.lightmapDensity; });

    auto done = std::make_shared<LightmapBakeResult>();
    std::vector<LightmapSurface> surfaces;
    // Every chart of every surface, as the layout is asked for them.
    std::vector<core::Vec2> sizes;
    std::vector<f32> scales;
    m_lightmapParts.clear();
    world.parts().forEach([&](InstanceId id, const scene::PartComponent& part) {
        if (!part.bakedLight || !inWorkspace(world, id, frame.workspace))
            return;
        // **What is baked stands still**: a part that is not anchored is lit
        // as a moving thing is, whatever it asks.
        const scene::RigidBodyComponent* body = world.rigidBodies().find(id);
        if ((body != nullptr && !body->anchored) || world.characterBodies().find(id) != nullptr)
            return;
        const render::MeshLibrary::Entry* entry = nullptr;
        Vec3 stretch{1.0f, 1.0f, 1.0f};
        if (const scene::MeshPartComponent* shaped = world.meshParts().find(id); shaped != nullptr) {
            if (shaped->meshContent.valid() && frame.meshes != nullptr)
                entry = frame.meshes->find(shaped->meshContent);
            stretch = Vec3{part.size.x / shaped->meshSize.x, part.size.y / shaped->meshSize.y,
                           part.size.z / shaped->meshSize.z};
        }
        else if (frame.meshes != nullptr) {
            const render::PartShape shape = render::partShapeOf(world, *frame.meshes, id, part);
            entry = shape.entry;
            stretch = shape.stretch;
        }
        if (entry == nullptr || entry->lightmapUvs.size() != entry->positions.size() ||
            entry->normals.size() != entry->positions.size() || entry->triangles.empty()) {
            done->unmapped.push_back(id);
            return;
        }
        LightmapSurface surface;
        surface.place = part.cframe;
        surface.stretch = stretch;
        surface.positions = entry->positions;
        surface.normals = entry->normals;
        surface.uvs = entry->lightmapUvs;
        surface.indices = entry->triangles;
        const bool grid = entry->lightmapColumns != 0 && entry->lightmapRows != 0;
        surface.columns = grid ? entry->lightmapColumns : 0;
        surface.rows = grid ? entry->lightmapRows : 0;
        surface.margin = grid ? asset::kPrimitiveChartMargin : 0.0f;
        for (const core::Vec2 size : chartSizes(entry->positions, entry->lightmapUvs, entry->triangles, stretch,
                                                surface.columns, surface.rows)) {
            // A chart's own margin is texels the rectangle need not have.
            const f32 inner = 1.0f - 2.0f * surface.margin;
            sizes.push_back(core::Vec2{size.x * inner, size.y * inner});
            scales.push_back(part.lightmapScale);
        }
        surfaces.push_back(surface);
        m_lightmapParts.push_back(id);
    });
    const LightmapLayout layout = layOutLightmaps(sizes, scales, density, kLightmapPageSize, kLightmapMaxPages);
    usize next = 0;
    for (usize index = 0; index < surfaces.size(); ++index) {
        LightmapSurface& surface = surfaces[index];
        const usize charts = surface.columns == 0 ? 1 : static_cast<usize>(surface.columns) * surface.rows;
        surface.rects.assign(layout.rects.begin() + static_cast<std::ptrdiff_t>(next),
                             layout.rects.begin() + static_cast<std::ptrdiff_t>(next + charts));
        next += charts;
        done->parts.push_back(LightmapBakeResult::Part{
            .id = m_lightmapParts[index],
            .columns = surface.columns,
            .rows = surface.rows,
            .rects = surface.rects,
        });
    }
    done->density = layout.density;
    done->lowered = layout.lowered;
    m_lightmapDone = std::move(done);
    m_lightmap = std::make_shared<LightmapBake>();
    m_lightmap->begin(surfaces, kLightmapPageSize, surfaces.empty() ? 0 : layout.pages, kLightmapBounces);
}

void IndirectLight::stepLightmaps(const Frame& frame)
{
    if (!frame.bakingLightmaps) {
        m_lightmap.reset();
        return;
    }
    if (frame.physics == nullptr || frame.world == nullptr)
        return;
    if (!m_lightmap) {
        // Whole and not yet taken: nothing more until it is.
        if (m_lightmapDone && !m_lightmapDone->pages.empty())
            return;
        beginLightmaps(frame);
    }
    if (m_lightmap->whole())
        return;
    const BakeTrace trace =
        traceOf(*frame.world, *frame.physics, m_surfaces, m_traced, m_tracedBoxes, m_tracedBodies, frame.textures);
    if (!m_lightmap->step(trace, m_light, kCurtainBudgetNs))
        return;
    LightmapBakeResult& done = *m_lightmapDone;
    done.pages.assign(m_lightmap->pages().begin(), m_lightmap->pages().end());
    done.sunDirection = m_lightmap->sunDirection();
    done.texels = m_lightmap->texelCount();
    done.inside = m_lightmap->insideCount();
    // A scene with nothing to bake still finishes: with no pages.
    if (done.pages.empty())
        done.pages.push_back(LightmapPage{});
}

bool IndirectLight::takeLightmapBake(LightmapBakeResult& out)
{
    if (!m_lightmap || !m_lightmap->whole() || !m_lightmapDone)
        return false;
    out = std::move(*m_lightmapDone);
    // An empty page stood for "whole" in a scene with nothing baked.
    if (out.pages.size() == 1 && out.pages.front().size == 0)
        out.pages.clear();
    m_lightmapDone.reset();
    m_lightmap.reset();
    return true;
}

f32 IndirectLight::lightmapProgress() const noexcept
{
    return m_lightmap ? m_lightmap->progress() : -1.0f;
}

void IndirectLight::planProbe(const Frame& frame)
{
    if (m_job.live || m_probeBudget == 0)
        return;
    // A room's pictures are taken once its bounced light is in them: not
    // while a volume in view is still waited for, worked out, or going from
    // its old light to its new.
    for (const Volume& volume : m_volumes) {
        const VolumeState& state = volumeState(volume.key);
        const bool busy =
            !state.hasResult || state.pending || (state.bake.begun() && !state.bake.whole()) || state.mix < 1.0f;
        if (volume.inView && busy)
            return;
    }
    for (const Probe& probe : m_probes) {
        ProbeState& state = probeState(probe.key);
        const u64 lightNow = lightAround(m_light, probe.box, probe.half, nullptr);
        const bool held = state.taken && frame.renderer->holdsReflectionProbe(probe.key);
        // Asked for again -- `Refresh`, or the light changed under a `Dynamic`
        // one -- it is taken twice, as it was at first: a mirror in the room
        // otherwise goes on reflecting the room as it was before.
        if (state.refreshes != probe.set.refreshes || (probe.set.dynamic && state.lightSeen != lightNow))
            state.takes = 0;
        const bool stale = state.takes < 2;
        if (held && !stale)
            continue;
        // Not kept any more -- the renderer gave its map to another, or made
        // its maps again -- is not taken at all.
        if (!held)
            state.takes = 0;
        m_job = Job{};
        m_job.live = true;
        m_job.key = probe.key;
        m_job.place = core::transformPoint(probe.box, core::toDVec3(probe.set.offset));
        m_job.refreshes = probe.set.refreshes;
        m_job.light = lightNow;
        // Recorded now, so the second of the two takes is not itself a reason
        // to begin again.
        state.refreshes = probe.set.refreshes;
        state.lightSeen = lightNow;
        return;
    }
}

std::optional<IndirectLight::Face> IndirectLight::nextFace(rhi::IDevice& device)
{
    if (!m_job.live || m_probeSize == 0)
        return std::nullopt;
    // One picture a frame while the game is watched; behind the curtain, as
    // many as the frame has room for.
    if (m_facesThisFrame > 0 && (!m_flatOut || platform::nowNs() - m_frameStartNs >= kCurtainBudgetNs))
        return std::nullopt;
    if (m_faceSize != m_probeSize) {
        // Another size of picture: the ones taken so far are no use to it.
        destroy(device);
        m_faceSize = m_probeSize;
        m_job.face = 0;
    }
    for (rhi::TextureHandle& face : m_faces) {
        if (face.valid())
            continue;
        face = device.createTexture({
            .format = rhi::TextureFormat::Rgba16Float,
            .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::ColorTarget,
            .width = m_faceSize,
            .height = m_faceSize,
            .debugName = "probe-picture",
        });
        if (!face.valid())
            return std::nullopt;
    }
    // Six quarter turns that between them look every way.
    static constexpr f32 kQuarter = kPi * 0.5f;
    core::Mat3 turn;
    switch (m_job.face) {
    case 1:
        turn = core::rotationY(kQuarter);
        break;
    case 2:
        turn = core::rotationY(2.0f * kQuarter);
        break;
    case 3:
        turn = core::rotationY(-kQuarter);
        break;
    case 4:
        turn = core::rotationX(kQuarter);
        break;
    case 5:
        turn = core::rotationX(-kQuarter);
        break;
    default:
        break;
    }
    return Face{.target = m_faces[m_job.face],
                .size = m_faceSize,
                .lens = core::CFrameD{.position = m_job.place, .rotation = turn}};
}

void IndirectLight::faceDrawn(rhi::IDevice& device, rhi::ICmdList& cmd, render::IRenderer& renderer,
                              const Mat4& viewProjection, u64 nowNs)
{
    if (!m_job.live)
        return;
    (void)nowNs;
    m_job.viewProjections[m_job.face] = viewProjection;
    ++m_job.face;
    ++m_facesThisFrame;
    ++m_stats.picturesDrawn;
    if (m_job.face < 6)
        return;
    const std::span<const rhi::TextureHandle, 6> faces{m_faces};
    const std::span<const Mat4, 6> matrices{m_job.viewProjections};
    ProbeState& state = probeState(m_job.key);
    if (renderer.storeReflectionProbe(device, cmd, m_job.key, faces, matrices)) {
        // The first time while the world is watched, it comes in over a
        // moment; taken again, the new pictures are simply the old ones' heirs.
        if (!state.taken)
            state.presence = m_flatOut ? 1.0f : 0.0f;
        state.taken = true;
        ++state.takes;
    }
    else {
        // No map could be kept for it: not asked for again until something
        // about it changes.
        state.takes = 2;
    }
    // Asked for at these, taken or not: a probe no map could be kept for is
    // not asked again every frame, only when something about it changes.
    state.refreshes = m_job.refreshes;
    state.lightSeen = m_job.light;
    m_job = Job{};
}

void IndirectLight::append(render::RenderWorld& out, bool whole) const
{
    if (!out.camera.valid)
        return;
    for (const Probe& probe : m_probes) {
        const ProbeState* state = nullptr;
        for (const ProbeState& candidate : m_probeStates) {
            if (candidate.key == probe.key)
                state = &candidate;
        }
        if (state == nullptr || !state->taken)
            continue;
        out.reflectionProbes.push_back(render::RenderReflectionProbe{
            .key = probe.key,
            .worldToBox = core::inverse(core::toRenderMatrix(probe.box, out.camera.origin)),
            .halfSize = probe.half,
            .capture = probe.set.offset,
            .blend = probe.set.blend,
            .intensity = probe.set.intensity,
            .presence = whole ? 1.0f : state->presence,
            .priority = probe.set.priority,
            .boxProjection = probe.set.boxProjection,
        });
    }
    for (const Volume& volume : m_volumes) {
        const VolumeState* state = nullptr;
        for (const VolumeState& candidate : m_volumeStates) {
            if (candidate.key == volume.key)
                state = &candidate;
        }
        if (state == nullptr || !state->hasResult)
            continue;
        const VolumeLayout& layout = state->bake.layout();
        render::RenderIrradianceVolume drawn;
        drawn.key = volume.key;
        drawn.revision = state->revision;
        drawn.worldToBox = core::inverse(core::toRenderMatrix(state->shownBox, out.camera.origin));
        drawn.halfSize = Vec3{state->shownSize.x * 0.5f, state->shownSize.y * 0.5f, state->shownSize.z * 0.5f};
        drawn.blend = volume.set.blend;
        drawn.intensity = volume.set.intensity;
        drawn.tint = volume.set.tint;
        drawn.added = state->mode == 1;
        drawn.margin = volume.margin;
        drawn.presence = whole ? 1.0f : state->presence;
        drawn.counts[0] = layout.counts[0];
        drawn.counts[1] = layout.counts[1];
        drawn.counts[2] = layout.counts[2];
        drawn.samples = state->shown;
        out.irradianceVolumes.push_back(drawn);
    }
    for (const Flat& flat : m_flats) {
        if (!flat.lit)
            continue;
        render::RenderIrradianceVolume drawn;
        drawn.key = flat.key;
        drawn.worldToBox = core::inverse(core::toRenderMatrix(flat.box, out.camera.origin));
        drawn.halfSize = Vec3{flat.size.x * 0.5f, flat.size.y * 0.5f, flat.size.z * 0.5f};
        drawn.blend = flat.set.blend;
        drawn.intensity = flat.set.intensity;
        drawn.tint = flat.set.tint;
        drawn.added = flat.set.mode == 1;
        drawn.counts[0] = 0;
        drawn.counts[1] = 0;
        drawn.counts[2] = 0;
        drawn.flat = flat.mean;
        out.irradianceVolumes.push_back(drawn);
    }
}

// --- A volume's light, kept (ADR 0207) -------------------------------------------

const IndirectLight::Kept& IndirectLight::keptFor(const Frame& frame, core::NameAtom urn)
{
    for (const Kept& kept : m_kept) {
        if (kept.urn.id == urn.id)
            return kept;
    }
    Kept read;
    read.urn = urn;
    read.read = ++m_reads;
    std::vector<std::byte> bytes;
    if (frame.readContent && frame.readContent(frame.world->atoms().text(urn), bytes))
        read.found = readBakedLight(bytes, read.light);
    m_kept.push_back(std::move(read));
    return m_kept.back();
}

void IndirectLight::forgetBaked(std::span<const core::NameAtom> urns)
{
    std::erase_if(m_kept, [&](const Kept& kept) {
        return std::any_of(urns.begin(), urns.end(), [&](core::NameAtom urn) { return urn.id == kept.urn.id; });
    });
}

const BakedLight* IndirectLight::bakedFor(const Frame& frame, InstanceId id,
                                          const scene::IrradianceVolumeComponent& set, u32 pieces, bool& warned)
{
    const scene::World& world = *frame.world;
    const scene::PartComponent* part = world.parts().find(world.parentOf(id));
    if (part == nullptr)
        return nullptr;
    const auto say = [&](core::TextKey key) {
        if (warned)
            return;
        warned = true;
        const std::array<core::I18nArg, 2> args{
            core::I18nArg{"name", world.atoms().text(world.name(world.parentOf(id)))},
            core::I18nArg{"path", world.atoms().text(set.baked)}};
        core::log(core::LogLevel::Warn, key, args);
    };
    const Kept& kept = keptFor(frame, set.baked);
    if (!kept.found) {
        say(ENG_TR("app.warn.irradiance_baked_missing"));
        return nullptr;
    }
    // **The light of this volume, or nobody's**: where its box stands and how
    // large it is, how far apart its points are, how many bounces, which
    // mode -- and so how many pieces it is in, each with as many points.
    const BakedLight& light = kept.light;
    const auto near = [](f64 a, f64 b, f64 within) { return std::abs(a - b) <= within; };
    bool same = near(light.box.position.x, part->cframe.position.x, 0.01) &&
                near(light.box.position.y, part->cframe.position.y, 0.01) &&
                near(light.box.position.z, part->cframe.position.z, 0.01) &&
                near(static_cast<f64>(light.size.x), static_cast<f64>(part->size.x), 0.001) &&
                near(static_cast<f64>(light.size.y), static_cast<f64>(part->size.y), 0.001) &&
                near(static_cast<f64>(light.size.z), static_cast<f64>(part->size.z), 0.001) &&
                near(static_cast<f64>(light.spacing), static_cast<f64>(set.spacing), 0.0001) &&
                light.bounces == set.bounces && light.mode == set.mode && light.pieces.size() == pieces;
    for (int row = 0; same && row < 3; ++row) {
        for (int column = 0; column < 3; ++column)
            same = same && near(static_cast<f64>(light.box.rotation.m[row][column]),
                                static_cast<f64>(part->cframe.rotation.m[row][column]), 0.001);
    }
    if (same) {
        const std::vector<VolumePiece> laid = volumeTilesOf(part->cframe, part->size, set.spacing);
        same = laid.size() == light.pieces.size();
        for (usize index = 0; same && index < laid.size(); ++index) {
            for (u32 axis = 0; axis < 3; ++axis)
                same = same && laid[index].layout.counts[axis] == light.pieces[index].counts[axis];
        }
    }
    if (!same) {
        say(ENG_TR("app.warn.irradiance_baked_other"));
        return nullptr;
    }
    // **And is the room still the room?** Where somebody is there to be
    // told: an editor, a development run. The file's light is drawn all the
    // same -- it is the nearest thing to the room's light there is.
    if (frame.development && !warned && !set.dynamic && light.room != 0 &&
        roomOf(world, frame.workspace, world.parentOf(id), part->cframe, part->size) != light.room)
        say(ENG_TR("app.warn.irradiance_baked_stale"));
    return &light;
}

bool IndirectLight::keep(const scene::World& world, InstanceId volume, BakedLight& out) const
{
    const scene::IrradianceVolumeComponent* set = world.irradianceVolumes().find(volume);
    const scene::PartComponent* part = world.parts().find(world.parentOf(volume));
    if (set == nullptr || part == nullptr || set->followsCamera)
        return false;
    out = BakedLight{};
    out.box = part->cframe;
    out.size = part->size;
    out.spacing = set->spacing;
    out.bounces = set->bounces;
    out.mode = set->mode;
    u32 pieces = 0;
    for (const Volume& piece : m_volumes) {
        if (piece.id == volume)
            pieces = piece.pieces;
    }
    if (pieces == 0)
        return false;
    out.pieces.resize(pieces);
    u32 whole = 0;
    for (const Volume& piece : m_volumes) {
        if (piece.id != volume || piece.piece >= pieces)
            continue;
        const VolumeState* state = nullptr;
        for (const VolumeState& candidate : m_volumeStates) {
            if (candidate.key == piece.key)
                state = &candidate;
        }
        if (state == nullptr || !state->ready || !state->bake.whole())
            return false;
        const std::span<const f32> light = state->bake.result();
        BakedPiece& kept = out.pieces[piece.piece];
        const VolumeLayout& layout = state->bake.layout();
        std::copy(std::begin(layout.counts), std::end(layout.counts), std::begin(kept.counts));
        kept.samples.assign(light.begin(), light.end());
        ++whole;
    }
    if (whole != pieces)
        return false;
    out.room = roomOf(world, m_workspace, world.parentOf(volume), part->cframe, part->size);
    out.mean = bakedMean(out.pieces);
    return true;
}

core::u64 IndirectLight::roomOf(const scene::World& world, InstanceId workspace, InstanceId own,
                                const core::CFrameD& box, Vec3 size)
{
    const core::CFrameD intoBox = core::inverse(box);
    const auto mix = [](u64 held, core::i64 value) {
        u64 x = held ^ (static_cast<u64>(value) + 0x9E3779B97F4A7C15ull + (held << 6) + (held >> 2));
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        return x;
    };
    u64 sum = 0;
    world.parts().forEach([&](InstanceId id, const scene::PartComponent& part) {
        const scene::RigidBodyComponent* body = world.rigidBodies().find(id);
        // The part that IS the box is not something in the room.
        if (id == own || body == nullptr || !body->anchored || !inWorkspace(world, id, workspace))
            return;
        const core::CFrameD local = intoBox * part.cframe;
        const f64 reach = 0.5 * static_cast<f64>(core::length(part.size));
        if (std::abs(local.position.x) > static_cast<f64>(size.x) * 0.5 + reach ||
            std::abs(local.position.y) > static_cast<f64>(size.y) * 0.5 + reach ||
            std::abs(local.position.z) > static_cast<f64>(size.z) * 0.5 + reach)
            return;
        u64 one = 0x51ED270B0F1A3C65ull;
        one = mix(one, std::llround(local.position.x * 1000.0));
        one = mix(one, std::llround(local.position.y * 1000.0));
        one = mix(one, std::llround(local.position.z * 1000.0));
        one = mix(one, std::llround(static_cast<f64>(part.size.x) * 1000.0));
        one = mix(one, std::llround(static_cast<f64>(part.size.y) * 1000.0));
        one = mix(one, std::llround(static_cast<f64>(part.size.z) * 1000.0));
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column)
                one = mix(one, std::llround(static_cast<f64>(local.rotation.m[row][column]) * 100.0));
        }
        // In any order: a sum.
        sum += one;
    });
    return sum == 0 ? 1 : sum;
}

void IndirectLight::destroy(rhi::IDevice& device)
{
    for (rhi::TextureHandle& face : m_faces) {
        if (face.valid())
            device.destroy(face);
        face = {};
    }
    m_faceSize = 0;
}

} // namespace engine::app
