#include "engine/render/ribbons.h"

#include <algorithm>
#include <cmath>

#include "engine/core/sequence.h"
#include "engine/render/render_world.h"
#include "engine/scene/world.h"

namespace engine::render {
namespace {

using core::f32;
using core::f64;
using core::u32;
using core::usize;
using core::Vec3;

[[nodiscard]] f32 lengthOf(Vec3 v) noexcept
{
    return std::sqrt(core::dot(v, v));
}

[[nodiscard]] Vec3 normalizedOr(Vec3 v, Vec3 fallback) noexcept
{
    const f32 length = lengthOf(v);
    return length > 1.0e-6f ? v * (1.0f / length) : fallback;
}

[[nodiscard]] Vec3 lerp(Vec3 a, Vec3 b, f32 t) noexcept
{
    return a + (b - a) * t;
}

[[nodiscard]] core::DVec3 middleOf(core::DVec3 a, core::DVec3 b) noexcept
{
    return core::DVec3{(a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5};
}

// Across a ribbon that faces the camera: at right angles to the way it runs
// and to the line of sight. The camera is the origin of every position here.
[[nodiscard]] Vec3 facing(Vec3 along, Vec3 position, Vec3 fallback) noexcept
{
    return normalizedOr(core::cross(along, position * -1.0f), fallback);
}

[[nodiscard]] bool inWorld(const scene::World& world, core::InstanceId id, core::InstanceId root) noexcept
{
    for (core::InstanceId cursor = world.parentOf(id); cursor.valid(); cursor = world.parentOf(cursor)) {
        if (cursor == root)
            return true;
    }
    return false;
}

[[nodiscard]] bool isEnd(const scene::World& world, core::InstanceId attachment) noexcept
{
    return attachment.valid() && world.alive(attachment) && !world.destroyed(attachment) &&
           world.attachments().find(attachment) != nullptr;
}

// One cross-section of a ribbon: its two edges, and what is drawn there.
struct Rung
{
    Vec3 left;
    Vec3 right;
    f32 color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    f32 u = 0.0f;
};

// What a whole ribbon is drawn with.
struct Look
{
    rhi::TextureHandle texture;
    f32 emission = 0.0f;
    f32 lightInfluence = 1.0f;
};

// One ribbon, waiting to be put in order with the others.
struct Built
{
    usize firstVertex = 0;
    usize vertexCount = 0;
    rhi::TextureHandle texture;
    f32 distance = 0.0f;
};

void put(std::vector<RenderRibbonVertex>& out, const Rung& rung, bool right, const Look& look)
{
    RenderRibbonVertex vertex;
    vertex.position = right ? rung.right : rung.left;
    vertex.halfWidth = lengthOf(rung.right - rung.left) * 0.5f;
    for (usize channel = 0; channel < 4; ++channel)
        vertex.color[channel] = rung.color[channel];
    vertex.u = rung.u;
    vertex.v = right ? 1.0f : 0.0f;
    vertex.emission = look.emission;
    vertex.lightInfluence = look.lightInfluence;
    out.push_back(vertex);
}

// The quads between consecutive rungs, as two triangles each.
[[nodiscard]] u32 strip(std::vector<RenderRibbonVertex>& out, const std::vector<Rung>& rungs, const Look& look)
{
    u32 pieces = 0;
    for (usize index = 1; index < rungs.size(); ++index) {
        const Rung& from = rungs[index - 1];
        const Rung& to = rungs[index];
        // Nothing to see where both ends are gone.
        if (from.color[3] <= 0.0f && to.color[3] <= 0.0f)
            continue;
        put(out, from, false, look);
        put(out, from, true, look);
        put(out, to, true, look);
        put(out, from, false, look);
        put(out, to, true, look);
        put(out, to, false, look);
        ++pieces;
    }
    return pieces;
}

} // namespace

const RibbonSystem::Trail* RibbonSystem::find(core::InstanceId id) const noexcept
{
    const auto at = std::lower_bound(m_trails.begin(), m_trails.end(), id, [](const Trail& t, core::InstanceId v) {
        return t.id.index != v.index ? t.id.index < v.index : t.id.generation < v.generation;
    });
    return at != m_trails.end() && at->id == id ? &*at : nullptr;
}

f32 RibbonSystem::trailLength(core::InstanceId trail) const noexcept
{
    const Trail* found = find(trail);
    if (found == nullptr || found->pieces.empty())
        return 0.0f;
    const f64 newest = found->hasHead ? found->head.travelled : found->pieces.back().travelled;
    return static_cast<f32>(newest - found->pieces.front().travelled);
}

usize RibbonSystem::trailPieces(core::InstanceId trail) const noexcept
{
    const Trail* found = find(trail);
    return found != nullptr ? found->pieces.size() : 0;
}

void RibbonSystem::update(const scene::World& world, core::InstanceId root, f64 dt, const DrawPoses* poses)
{
    DrawPoses still;
    const DrawPoses& posed =
        poses != nullptr && poses->world() == &world ? *poses : (still.begin(world, nullptr, 0.0f), still);
    m_time += std::clamp(dt, 0.0, MaxStep);
    for (Trail& trail : m_trails)
        trail.seen = false;

    world.trails().forEach([&](core::InstanceId id, const scene::TrailComponent& config) {
        if (world.destroyed(id) || !inWorld(world, id, root))
            return;
        auto at = std::lower_bound(m_trails.begin(), m_trails.end(), id, [](const Trail& t, core::InstanceId v) {
            return t.id.index != v.index ? t.id.index < v.index : t.id.generation < v.generation;
        });
        if (at == m_trails.end() || !(at->id == id)) {
            Trail fresh;
            fresh.id = id;
            fresh.cleared = config.cleared;
            at = m_trails.insert(at, std::move(fresh));
        }
        Trail& trail = *at;
        trail.seen = true;

        // `Clear` was called since the last frame: everything goes, and the
        // ribbon begins again from where the ends are now.
        if (trail.cleared != config.cleared) {
            trail.pieces.clear();
            trail.cleared = config.cleared;
        }

        // What has outlived its lifetime, oldest first.
        const f64 lifetime = static_cast<f64>(std::max(config.lifetime, 1.0e-3f));
        usize expired = 0;
        while (expired < trail.pieces.size() && trail.pieces[expired].born + lifetime < m_time)
            ++expired;
        if (expired > 0)
            trail.pieces.erase(trail.pieces.begin(), trail.pieces.begin() + static_cast<std::ptrdiff_t>(expired));

        trail.hasHead = false;
        if (!config.enabled || !isEnd(world, config.attachment0) || !isEnd(world, config.attachment1))
            return;

        Piece now;
        now.a = posed.attachment(config.attachment0).position;
        now.b = posed.attachment(config.attachment1).position;
        now.born = m_time;
        if (trail.pieces.empty()) {
            trail.pieces.push_back(now);
        }
        else {
            const Piece& last = trail.pieces.back();
            const core::DVec3 middle = middleOf(now.a, now.b);
            const core::DVec3 before = middleOf(last.a, last.b);
            const f64 moved = static_cast<f64>(lengthOf(core::toVec3(middle - before)));
            now.travelled = last.travelled + moved;
            // A piece for every `MinLength` of travel; between them the live
            // edge is drawn where the ends are, so the ribbon never lags them.
            if (moved >= static_cast<f64>(std::max(config.minLength, 1.0e-3f))) {
                trail.pieces.push_back(now);
                if (trail.pieces.size() > MaxTrailPieces)
                    trail.pieces.erase(trail.pieces.begin());
            }
        }
        trail.head = now;
        trail.hasHead = true;

        // No longer than it may be: what is furthest back goes first.
        if (config.maxLength > 0.0f) {
            usize beyond = 0;
            while (beyond + 1 < trail.pieces.size() &&
                   now.travelled - trail.pieces[beyond + 1].travelled > static_cast<f64>(config.maxLength))
                ++beyond;
            if (beyond > 0)
                trail.pieces.erase(trail.pieces.begin(), trail.pieces.begin() + static_cast<std::ptrdiff_t>(beyond));
        }
    });

    std::erase_if(m_trails, [](const Trail& trail) { return !trail.seen; });
}

void RibbonSystem::append(const scene::World& world, core::InstanceId root, RenderWorld& out,
                          const TextureLibrary* textures, const DrawPoses* poses) const
{
    DrawPoses still;
    const DrawPoses& posed =
        poses != nullptr && poses->world() == &world ? *poses : (still.begin(world, nullptr, 0.0f), still);
    const core::DVec3 origin = out.camera.origin;
    const auto textureOf = [&](core::NameAtom content) {
        return textures != nullptr && content.valid() ? textures->find(content) : rhi::TextureHandle{};
    };

    std::vector<RenderRibbonVertex> vertices;
    std::vector<Built> built;
    std::vector<Rung> rungs;
    m_stats = Stats{};

    const auto finish = [&](const Look& look, Vec3 middle, bool beam) {
        Built ribbon;
        ribbon.firstVertex = vertices.size();
        const u32 pieces = strip(vertices, rungs, look);
        ribbon.vertexCount = vertices.size() - ribbon.firstVertex;
        if (ribbon.vertexCount == 0)
            return;
        ribbon.texture = look.texture;
        ribbon.distance = lengthOf(middle);
        built.push_back(ribbon);
        m_stats.pieces += pieces;
        (beam ? m_stats.beams : m_stats.trails) += 1;
    };

    // --- Beams: a cubic between two attachments, built where they are drawn.
    world.beams().forEach([&](core::InstanceId id, const scene::BeamComponent& beam) {
        if (!beam.enabled || world.destroyed(id) || !inWorld(world, id, root) || !isEnd(world, beam.attachment0) ||
            !isEnd(world, beam.attachment1))
            return;
        const core::CFrameD end0 = posed.attachment(beam.attachment0);
        const core::CFrameD end1 = posed.attachment(beam.attachment1);
        const Vec3 p0 = core::toVec3(end0.position - origin);
        const Vec3 p3 = core::toVec3(end1.position - origin);
        const Vec3 x0 = end0.rotation * Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 x1 = end1.rotation * Vec3{1.0f, 0.0f, 0.0f};
        const Vec3 y0 = end0.rotation * Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 y1 = end1.rotation * Vec3{0.0f, 1.0f, 0.0f};
        const Vec3 p1 = p0 + x0 * beam.curveSize0;
        const Vec3 p2 = p3 - x1 * beam.curveSize1;
        const bool straight = beam.curveSize0 == 0.0f && beam.curveSize1 == 0.0f;
        const int segments = straight ? 1 : std::clamp(beam.segments, 1, 64);
        const Vec3 chord = normalizedOr(p3 - p0, Vec3{1.0f, 0.0f, 0.0f});

        rungs.clear();
        f32 run = 0.0f;
        Vec3 previous = p0;
        const auto time = static_cast<f32>(m_time);
        for (int index = 0; index <= segments; ++index) {
            const f32 t = static_cast<f32>(index) / static_cast<f32>(segments);
            const f32 s = 1.0f - t;
            Vec3 point = p0 * (s * s * s) + p1 * (3.0f * s * s * t) + p2 * (3.0f * s * t * t) + p3 * (t * t * t);
            const Vec3 along = normalizedOr(
                (p1 - p0) * (3.0f * s * s) + (p2 - p1) * (6.0f * s * t) + (p3 - p2) * (3.0f * t * t), chord);
            run += lengthOf(point - previous);
            previous = point;
            // Towards the camera, so a beam lying on a wall is not cut by it.
            if (beam.zOffset != 0.0f)
                point = point + normalizedOr(point * -1.0f, Vec3{0.0f, 0.0f, 1.0f}) * beam.zOffset;

            const Vec3 flat = normalizedOr(lerp(y0, y1, t), Vec3{0.0f, 1.0f, 0.0f});
            const Vec3 across = beam.faceCamera ? facing(along, point, flat) : flat;
            const f32 half = (beam.width0 + (beam.width1 - beam.width0) * t) * 0.5f;

            Rung rung;
            rung.left = point - across * half;
            rung.right = point + across * half;
            const core::Color3 color = core::evaluate(beam.color, t);
            rung.color[0] = color.r;
            rung.color[1] = color.g;
            rung.color[2] = color.b;
            rung.color[3] = std::clamp(1.0f - core::evaluate(beam.transparency, t), 0.0f, 1.0f);
            // 0 Stretch: once over the whole length, running in repeats a
            // second. 1 Wrap: every `TextureLength` metres, running in metres
            // a second. 2 Static: the same, and still.
            const f32 repeat = std::max(beam.textureLength, 1.0e-3f);
            rung.u = beam.textureMode == 0   ? t - time * beam.textureSpeed
                     : beam.textureMode == 1 ? (run - time * beam.textureSpeed) / repeat
                                             : run / repeat;
            rungs.push_back(rung);
        }
        Look look;
        look.texture = textureOf(beam.texture);
        look.emission = std::clamp(beam.lightEmission, 0.0f, 1.0f);
        look.lightInfluence = std::clamp(beam.lightInfluence, 0.0f, 1.0f);
        finish(look, (p0 + p3) * 0.5f, true);
    });

    // --- Trails: the pieces each has kept, and its live edge.
    for (const Trail& trail : m_trails) {
        const scene::TrailComponent* config = world.trails().find(trail.id);
        if (config == nullptr || trail.pieces.empty())
            continue;
        const usize count = trail.pieces.size() + (trail.hasHead ? 1 : 0);
        if (count < 2)
            continue;
        const auto pieceAt = [&](usize index) -> const Piece& {
            return index < trail.pieces.size() ? trail.pieces[index] : trail.head;
        };
        const f64 newest = pieceAt(count - 1).travelled;
        const f64 oldest = pieceAt(0).travelled;
        // A trail that has gone nowhere is a line seen end on: nothing.
        if (newest - oldest < 1.0e-4)
            continue;
        const auto whole = static_cast<f32>(newest - oldest);
        const f32 lifetime = std::max(config->lifetime, 1.0e-3f);
        const f32 repeat = std::max(config->textureLength, 1.0e-3f);

        rungs.clear();
        Vec3 sum{0.0f, 0.0f, 0.0f};
        for (usize index = 0; index < count; ++index) {
            const Piece& piece = pieceAt(index);
            const Vec3 a = core::toVec3(piece.a - origin);
            const Vec3 b = core::toVec3(piece.b - origin);
            const Vec3 middle = (a + b) * 0.5f;
            sum = sum + middle;
            const f32 age = std::clamp(static_cast<f32>(m_time - piece.born) / lifetime, 0.0f, 1.0f);
            const f32 scale = std::max(core::evaluate(config->widthScale, age), 0.0f);
            Vec3 half = (b - a) * (0.5f * scale);
            if (config->faceCamera) {
                // About its own middle line, as wide as the ends are apart.
                const Piece& before = pieceAt(index > 0 ? index - 1 : index);
                const Piece& after = pieceAt(index + 1 < count ? index + 1 : index);
                const Vec3 along = core::toVec3(middleOf(after.a, after.b) - middleOf(before.a, before.b));
                half = facing(along, middle, normalizedOr(b - a, Vec3{0.0f, 1.0f, 0.0f})) * lengthOf(half);
            }
            Rung rung;
            rung.left = middle - half;
            rung.right = middle + half;
            const core::Color3 color = core::evaluate(config->color, age);
            rung.color[0] = color.r;
            rung.color[1] = color.g;
            rung.color[2] = color.b;
            rung.color[3] = std::clamp(1.0f - core::evaluate(config->transparency, age), 0.0f, 1.0f);
            const auto behind = static_cast<f32>(newest - piece.travelled);
            // Stretch: once from the live edge to the oldest piece. Wrap: laid
            // from the live edge, so it slides as the trail moves. Static:
            // laid by where the trail has been, so it stays on the ground.
            rung.u = config->textureMode == 0   ? behind / whole
                     : config->textureMode == 1 ? behind / repeat
                                                : static_cast<f32>(piece.travelled / static_cast<f64>(repeat));
            rungs.push_back(rung);
        }
        Look look;
        look.texture = textureOf(config->texture);
        look.emission = std::clamp(config->lightEmission, 0.0f, 1.0f);
        look.lightInfluence = std::clamp(config->lightInfluence, 0.0f, 1.0f);
        finish(look, sum * (1.0f / static_cast<f32>(count)), false);
    }

    // Back to front, a ribbon at a time: what blending over one another
    // needs. When there are more than can be drawn, the furthest are left out.
    std::stable_sort(built.begin(), built.end(),
                     [](const Built& a, const Built& b) { return a.distance > b.distance; });
    usize total = 0;
    for (const Built& ribbon : built)
        total += ribbon.vertexCount;
    usize skip = 0;
    while (skip < built.size() && total > MaxVertices) {
        total -= built[skip].vertexCount;
        ++skip;
    }
    for (usize index = skip; index < built.size(); ++index) {
        const Built& ribbon = built[index];
        const auto first = static_cast<u32>(out.ribbonVertices.size());
        out.ribbonVertices.insert(
            out.ribbonVertices.end(), vertices.begin() + static_cast<std::ptrdiff_t>(ribbon.firstVertex),
            vertices.begin() + static_cast<std::ptrdiff_t>(ribbon.firstVertex + ribbon.vertexCount));
        // Neighbours with one texture are one draw.
        if (!out.ribbonRuns.empty() && out.ribbonRuns.back().texture == ribbon.texture) {
            out.ribbonRuns.back().vertexCount += static_cast<u32>(ribbon.vertexCount);
            continue;
        }
        out.ribbonRuns.push_back(RenderRibbonRun{ribbon.texture, first, static_cast<u32>(ribbon.vertexCount)});
    }
}

} // namespace engine::render
