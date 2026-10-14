#include "engine/render/particles.h"

#include <algorithm>
#include <cmath>

#include "engine/asset/terrain.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/profile.h"
#include "engine/core/sequence.h"
#include "engine/render/render_world.h"
#include "engine/scene/wind.h"
#include "engine/scene/world.h"

namespace engine::render {
namespace {

using core::f32;
using core::f64;
using core::u64;
using core::usize;

constexpr f32 kPi = 3.14159265358979f;

[[nodiscard]] core::Vec3 lerp(core::Vec3 a, core::Vec3 b, f32 t) noexcept
{
    return core::Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

[[nodiscard]] f32 lerp(f32 a, f32 b, f32 t) noexcept
{
    return a + (b - a) * t;
}

// Where an emitter's generator starts: its instance, mixed so neighbouring ids
// do not start neighbouring sequences.
[[nodiscard]] u64 seedOf(core::InstanceId id) noexcept
{
    u64 seed = (static_cast<u64>(id.generation) << 32) ^ id.index;
    seed ^= seed >> 33;
    seed *= 0xFF51AFD7ED558CCDull;
    seed ^= seed >> 33;
    return seed;
}

} // namespace

bool ParticleSystem::anchorOf(const scene::World& world, const DrawPoses& poses, core::InstanceId id,
                              core::CFrameD& frame, core::Vec3& extent)
{
    const core::InstanceId parent = world.parentOf(id);
    // An attachment is a point, and a direction -- the usual way to aim a jet.
    if (world.attachments().find(parent) != nullptr) {
        frame = poses.attachment(parent);
        extent = core::Vec3{0.0f, 0.0f, 0.0f};
        return true;
    }
    // A part is a volume: particles are born anywhere inside it, which is what
    // makes a burning crate burn all over rather than from its centre.
    for (core::InstanceId cursor = parent; cursor.valid(); cursor = world.parentOf(cursor)) {
        if (const scene::PartComponent* part = world.parts().find(cursor); part != nullptr) {
            frame = poses.part(cursor);
            extent = cursor == parent ? part->size : core::Vec3{0.0f, 0.0f, 0.0f};
            return true;
        }
    }
    return false;
}

void ParticleSystem::spawn(Emitter& emitter, const core::CFrameD& frame, core::Vec3 extent, u64 count)
{
    const scene::ParticleEmitterComponent& config = emitter.config;
    if (config.lifetime <= 0.0f)
        return;
    // The emitter's up, and two directions square to it, for the cone.
    const core::Vec3 up = core::normalize(frame.rotation * core::Vec3{0.0f, 1.0f, 0.0f});
    const core::Vec3 side = core::normalize(frame.rotation * core::Vec3{1.0f, 0.0f, 0.0f});
    const core::Vec3 back = core::cross(side, up);
    const f32 spread = std::clamp(config.spreadAngle, 0.0f, 180.0f) * (kPi / 180.0f);

    for (u64 born = 0; born < count && emitter.particles.size() < MaxPerEmitter; ++born) {
        Particle particle;
        // Uniform over the cap of the cone, not over its angle: an even spray
        // rather than one bunched along the axis.
        const f32 cosTheta = 1.0f - static_cast<f32>(emitter.random.nextDouble()) * (1.0f - std::cos(spread));
        const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        const f32 phi = static_cast<f32>(emitter.random.nextDouble()) * 2.0f * kPi;
        const core::Vec3 direction{
            up.x * cosTheta + (side.x * std::cos(phi) + back.x * std::sin(phi)) * sinTheta,
            up.y * cosTheta + (side.y * std::cos(phi) + back.y * std::sin(phi)) * sinTheta,
            up.z * cosTheta + (side.z * std::cos(phi) + back.z * std::sin(phi)) * sinTheta,
        };
        particle.velocity = direction * config.speed;

        const core::Vec3 local{
            (static_cast<f32>(emitter.random.nextDouble()) - 0.5f) * extent.x,
            (static_cast<f32>(emitter.random.nextDouble()) - 0.5f) * extent.y,
            (static_cast<f32>(emitter.random.nextDouble()) - 0.5f) * extent.z,
        };
        const core::Vec3 offset = frame.rotation * local;
        particle.position =
            core::DVec3{frame.position.x + static_cast<f64>(offset.x), frame.position.y + static_cast<f64>(offset.y),
                        frame.position.z + static_cast<f64>(offset.z)};
        // A tenth either way, so a stream does not pulse with every particle
        // born in one frame dying in one frame.
        particle.lifetime = config.lifetime * (0.9f + 0.2f * static_cast<f32>(emitter.random.nextDouble()));
        // **Turned and spinning** (ADR 0160). A spread draws from the
        // emitter's generator only when there is one, so an emitter that asks
        // for none is born exactly as it was before there was any.
        constexpr f32 Radians = 3.14159265f / 180.0f;
        f32 rotation = config.rotation;
        if (config.rotationSpread > 0.0f)
            rotation += config.rotationSpread * (2.0f * static_cast<f32>(emitter.random.nextDouble()) - 1.0f);
        f32 spin = config.rotationSpeed;
        if (config.rotationSpeedSpread > 0.0f)
            spin += config.rotationSpeedSpread * (2.0f * static_cast<f32>(emitter.random.nextDouble()) - 1.0f);
        particle.rotation = rotation * Radians;
        particle.spin = spin * Radians;
        const u32 frames = flipbookFrames(config);
        if (config.flipbookMode == 2 && frames > 1)
            particle.frame = static_cast<u32>(emitter.random.nextDouble() * static_cast<f64>(frames)) % frames;
        emitter.particles.push_back(particle);
    }
}

u32 ParticleSystem::flipbookFrames(const scene::ParticleEmitterComponent& config) noexcept
{
    return static_cast<u32>(std::clamp(config.flipbookColumns, 1, MaxFlipbookEdge)) *
           static_cast<u32>(std::clamp(config.flipbookRows, 1, MaxFlipbookEdge));
}

u32 ParticleSystem::flipbookFrame(const scene::ParticleEmitterComponent& config, u32 frames, u32 own, f32 age,
                                  f32 life) noexcept
{
    if (frames <= 1)
        return 0;
    switch (config.flipbookMode) {
    case 1:
        // Every frame once, the last one held at death.
        return std::min(static_cast<u32>(life * static_cast<f32>(frames)), frames - 1);
    case 2:
        return own % frames;
    default: {
        const f32 rate = std::max(config.flipbookFramerate, 0.0f);
        return (static_cast<u32>(age * rate) + own) % frames;
    }
    }
}

bool ParticleSystem::groundFromMap(f64 x, f64 z, f32& top, core::Vec3& normal) const noexcept
{
    if (m_ground.heights.empty() || m_ground.cells < 4)
        return false;
    // In cells, from the middle of the first: where a texel's height is.
    const f64 cell = static_cast<f64>(m_ground.side) / static_cast<f64>(m_ground.cells);
    const f64 u = (x - m_ground.corner.x) / cell - 0.5;
    const f64 v = (z - m_ground.corner.z) / cell - 0.5;
    // A cell in from each edge, so the slope has a neighbour either side.
    const f64 last = static_cast<f64>(m_ground.cells - 2);
    if (!(u >= 1.0) || !(v >= 1.0) || !(u < last) || !(v < last))
        return false;
    const auto column = static_cast<u32>(u);
    const auto row = static_cast<u32>(v);
    const f32 fu = static_cast<f32>(u - static_cast<f64>(column));
    const f32 fv = static_cast<f32>(v - static_cast<f64>(row));
    const auto heightOf = [&](u32 c, u32 r) { return m_ground.heights[static_cast<usize>(r) * m_ground.cells + c]; };
    const f32 a = heightOf(column, row);
    const f32 b = heightOf(column + 1, row);
    const f32 c = heightOf(column, row + 1);
    const f32 d = heightOf(column + 1, row + 1);
    // No ground under a corner: not the map's to say.
    if (a < -1.0e8f || b < -1.0e8f || c < -1.0e8f || d < -1.0e8f) {
        top = -1.0e9f;
        normal = core::Vec3{0.0f, 1.0f, 0.0f};
        return true;
    }
    top = lerp(lerp(a, b, fu), lerp(c, d, fu), fv);
    const f32 metres = static_cast<f32>(cell);
    const f32 slopeX = (lerp(b, d, fv) - lerp(a, c, fv)) / metres;
    const f32 slopeZ = (lerp(c, d, fu) - lerp(a, b, fu)) / metres;
    normal = core::normalize(core::Vec3{-slopeX, 1.0f, -slopeZ});
    return true;
}

void ParticleSystem::collide(const scene::World& world, const scene::ParticleEmitterComponent& config,
                             Particle& particle, core::DVec3 before)
{
    const f32 size =
        lerp(config.size, config.sizeEnd, std::clamp(particle.age / std::max(particle.lifetime, 1e-4f), 0.0f, 1.0f));
    const f64 radius = static_cast<f64>(config.collisionRadius > 0.0f ? config.collisionRadius : size * 0.5f);
    bool hit = false;
    core::DVec3 at = particle.position;
    core::Vec3 normal{0.0f, 1.0f, 0.0f};

    // **The ground, wherever it is** (Terrain): the field itself, so a spark
    // that lands behind the camera or far from anything that moves lands all
    // the same. The surface's slope from the heights either side of it.
    //
    // **From the map of heights round the camera where it reaches** -- the one
    // the GPU's particles read -- and from the field only where it does not: a
    // height from the field is a walk down a column of voxels, and a hundred
    // thousand of them a frame was a quarter of a second (measured: 241 ms for
    // 96 000 particles, against a tenth of a millisecond with no collision).
    f32 mapped = 0.0f;
    core::Vec3 mappedNormal;
    const bool onMap =
        (config.collision & 2) != 0 && groundFromMap(particle.position.x, particle.position.z, mapped, mappedNormal);
    if (onMap) {
        const f64 top = static_cast<f64>(mapped);
        // Under every terrain: nothing there to land on.
        if (mapped > -1.0e8f && particle.position.y - radius < top) {
            normal = mappedNormal;
            at = core::DVec3{particle.position.x, top + radius, particle.position.z};
            hit = true;
        }
    }
    else if ((config.collision & 2) != 0) {
        world.terrains().forEach([&](core::InstanceId, const scene::TerrainComponent& terrain) {
            const f64 x = particle.position.x - terrain.origin.x;
            const f64 z = particle.position.z - terrain.origin.z;
            const std::optional<float> height = asset::heightAt(terrain.field, x, z);
            if (!height.has_value())
                return;
            const f64 top = static_cast<f64>(*height) + terrain.origin.y;
            if (particle.position.y - radius >= top || (hit && top + radius <= at.y))
                return;
            const f64 reach = std::max(static_cast<f64>(terrain.field.settings().voxelSize), 0.25);
            const auto heightOr = [&](f64 px, f64 pz) {
                const std::optional<float> found = asset::heightAt(terrain.field, px, pz);
                return found.has_value() ? static_cast<f64>(*found) : static_cast<f64>(*height);
            };
            const f64 slopeX = (heightOr(x + reach, z) - heightOr(x - reach, z)) / (2.0 * reach);
            const f64 slopeZ = (heightOr(x, z + reach) - heightOr(x, z - reach)) / (2.0 * reach);
            normal = core::normalize(core::Vec3{static_cast<f32>(-slopeX), 1.0f, static_cast<f32>(-slopeZ)});
            at = core::DVec3{particle.position.x, top + radius, particle.position.z};
            hit = true;
        });
    }

    // **What stands in its way** (SceneDepth, on the CPU a ray): the parts
    // between where it was and where it is.
    if (!hit && (config.collision & 1) != 0 && m_raycast && m_lastRays < MaxRaysPerUpdate) {
        const core::Vec3 delta = core::toVec3(particle.position - before);
        const f32 length = core::length(delta);
        if (length > 1e-5f) {
            m_lastRays += 1;
            const core::Vec3 direction = delta * (1.0f / length);
            core::DVec3 where;
            core::Vec3 surface;
            if (m_raycast(before, direction * (length + static_cast<f32>(radius)), where, surface)) {
                normal = core::normalize(surface);
                at = core::DVec3{where.x + static_cast<f64>(normal.x) * radius,
                                 where.y + static_cast<f64>(normal.y) * radius,
                                 where.z + static_cast<f64>(normal.z) * radius};
                hit = true;
            }
        }
    }
    if (!hit)
        return;

    m_lastCollisions += 1;
    particle.position = at;
    if (config.collisionResponse == 2) {
        // Killed: gone at the end of this update.
        particle.age = particle.lifetime;
        return;
    }
    if (config.collisionResponse == 1) {
        particle.stuck = true;
        particle.velocity = core::Vec3{};
        particle.spin = 0.0f;
        return;
    }
    // Bounced: the speed into the surface comes back by `bounce`, and the
    // speed along it loses `friction` of itself. Moving out of it already, it
    // is only put on it.
    const f32 into = core::dot(particle.velocity, normal);
    if (into < 0.0f) {
        const core::Vec3 along = particle.velocity - normal * into;
        const f32 out = -into * std::clamp(config.bounce, 0.0f, 1.0f);
        // **At rest once it barely rises**: a spark that came back up a
        // centimetre a frame for ever never settled.
        const f32 kept = out < RestSpeed ? 0.0f : out;
        particle.velocity = along * (1.0f - std::clamp(config.friction, 0.0f, 1.0f)) + normal * kept;
    }
}

void ParticleSystem::update(const scene::World& world, core::InstanceId root, f64 dt, const DrawPoses* poses)
{
    ENG_PROFILE_SCOPE("particles.update");
    DrawPoses still;
    const DrawPoses& posed =
        poses != nullptr && poses->world() == &world ? *poses : (still.begin(world, nullptr, 0.0f), still);
    const auto step = static_cast<f32>(std::clamp(dt, 0.0, MaxStep));
    for (Emitter& emitter : m_emitters)
        emitter.seen = false;
    m_lastRays = 0;
    m_lastCollisions = 0;
    m_updates += 1;
    bool groundWanted = false;

    // The world's wind, and the clock it blows on: the simulation's, as
    // `Workspace:GetWindAt` reads it.
    scene::WindSettings wind;
    if (const scene::WorkspaceComponent* workspace = world.workspaces().find(root); workspace != nullptr)
        wind = scene::WindSettings{workspace->globalWind, workspace->windGusts, workspace->windTurbulence};
    const bool windy = wind.global.x != 0.0f || wind.global.y != 0.0f || wind.global.z != 0.0f;
    const auto windTime = static_cast<f32>(world.engineState().simTime);

    world.particleEmitters().forEach([&](core::InstanceId id, const scene::ParticleEmitterComponent& config) {
        if (world.destroyed(id))
            return;
        // Only what is in the world being drawn: an emitter in a stamp being
        // edited elsewhere, or under nothing, has no place to be.
        bool inWorld = false;
        for (core::InstanceId cursor = world.parentOf(id); cursor.valid(); cursor = world.parentOf(cursor))
            inWorld = inWorld || cursor == root;
        if (!inWorld)
            return;

        auto at = std::lower_bound(m_emitters.begin(), m_emitters.end(), id, [](const Emitter& e, core::InstanceId v) {
            return e.id.index != v.index ? e.id.index < v.index : e.id.generation < v.generation;
        });
        if (at == m_emitters.end() || !(at->id == id)) {
            Emitter fresh;
            fresh.id = id;
            fresh.random = core::Pcg32{seedOf(id), 0x7061727469636C65ull};
            at = m_emitters.insert(at, std::move(fresh));
        }
        Emitter& emitter = *at;
        emitter.seen = true;
        emitter.config = config;

        // **On the GPU** (ADR 0160): nothing is simulated here. This update
        // says where its particles are born, how many, and how long the step
        // is; the renderer's compute pass does the rest in its own buffer.
        if (config.simulation == 1 && !m_gpuAvailable && !m_gpuSaid) {
            m_gpuSaid = true;
            core::log(core::LogLevel::Warn, ENG_TR("render.warn.particles_gpu_unavailable"));
        }
        emitter.gpu = config.simulation == 1 && m_gpuAvailable;
        if (emitter.gpu) {
            emitter.particles.clear();
            emitter.gpuSpawn = 0;
            emitter.gpuStep = step;
            emitter.gpuAnchored = anchorOf(world, posed, id, emitter.gpuFrame, emitter.gpuExtent);
            if (!emitter.gpuAnchored) {
                emitter.carry = 0.0;
                emitter.spawnedBursts = config.emitted;
                return;
            }
            u64 owed = 0;
            if (config.emitted > emitter.spawnedBursts)
                owed = std::min<u64>(config.emitted - emitter.spawnedBursts, MaxGpuPerEmitter);
            emitter.spawnedBursts = config.emitted;
            f32 rate = 0.0f;
            if (config.enabled && config.rate > 0.0f && config.lifetime > 0.0f) {
                // As many as the buffer sustains, thinned evenly past that.
                rate = std::min(config.rate, static_cast<f32>(MaxGpuPerEmitter) / (config.lifetime * 1.1f));
                emitter.carry += static_cast<f64>(rate) * static_cast<f64>(step);
                const auto whole = static_cast<u64>(emitter.carry);
                emitter.carry -= static_cast<f64>(whole);
                owed += whole;
            }
            emitter.gpuSpawn = static_cast<u32>(std::min<u64>(owed, MaxGpuPerEmitter));
            // The buffer holds a stream's living and this frame's burst, in
            // powers of two; grown, never shrunk.
            const u64 living = static_cast<u64>(std::ceil(rate * config.lifetime * 1.1f)) + emitter.gpuSpawn;
            u32 capacity = 1024;
            while (capacity < living && capacity < MaxGpuPerEmitter)
                capacity *= 2;
            emitter.gpuCapacity = std::max(emitter.gpuCapacity, capacity);
            emitter.gpuWind = core::Vec3{};
            if (config.windAffectsDrift && windy) {
                const core::Vec3 where{static_cast<f32>(emitter.gpuFrame.position.x),
                                       static_cast<f32>(emitter.gpuFrame.position.y),
                                       static_cast<f32>(emitter.gpuFrame.position.z)};
                emitter.gpuWind = scene::windAt(wind, where, windTime);
            }
            groundWanted = groundWanted || (config.collision & 2) != 0;
            return;
        }

        groundWanted = groundWanted || (config.collision & 2) != 0;

        // Age and move what is alive, and let the dead go. **The wind carries
        // what is set to drift with it** (ADR 0115): added to how each one moves
        // rather than to its velocity, so a particle is carried by the air
        // around it and does not keep a gust after it passes.
        const f32 keep = std::max(0.0f, 1.0f - config.drag * step);
        for (Particle& particle : emitter.particles) {
            if (particle.stuck) {
                particle.age += step;
                continue;
            }
            const core::DVec3 before = particle.position;
            particle.velocity = (particle.velocity + config.acceleration * step) * keep;
            core::Vec3 moving = particle.velocity;
            if (config.windAffectsDrift && windy) {
                const core::Vec3 where{static_cast<f32>(particle.position.x), static_cast<f32>(particle.position.y),
                                       static_cast<f32>(particle.position.z)};
                moving = moving + scene::windAt(wind, where, windTime);
            }
            particle.position.x += static_cast<f64>(moving.x * step);
            particle.position.y += static_cast<f64>(moving.y * step);
            particle.position.z += static_cast<f64>(moving.z * step);
            particle.age += step;
            particle.rotation += particle.spin * step;
            if (config.collision != 0)
                collide(world, config, particle, before);
        }
        std::erase_if(emitter.particles, [](const Particle& particle) { return particle.age >= particle.lifetime; });

        core::CFrameD frame;
        core::Vec3 extent;
        if (!anchorOf(world, posed, id, frame, extent)) {
            emitter.carry = 0.0;
            emitter.spawnedBursts = config.emitted;
            return;
        }

        // Bursts first: what a script asked for since the last look.
        if (config.emitted > emitter.spawnedBursts) {
            const u64 owed = std::min<u64>(config.emitted - emitter.spawnedBursts, MaxBurstPerFrame);
            spawn(emitter, frame, extent, owed);
        }
        emitter.spawnedBursts = config.emitted;

        // Then the stream.
        if (config.enabled && config.rate > 0.0f) {
            const f32 rate = effectiveRate(config.rate, config.lifetime);
            // Said once for an emitter, where the person who set it will read it.
            if (rate < config.rate && !emitter.cappedSaid) {
                emitter.cappedSaid = true;
                const core::I18nArg args[] = {
                    {"name", std::string(world.atoms().text(world.name(id)))},
                    {"rate", static_cast<core::f64>(config.rate)},
                    {"sustained", static_cast<core::f64>(std::floor(rate))},
                    {"cap", static_cast<core::i64>(MaxPerEmitter)},
                };
                core::log(core::LogLevel::Warn, ENG_TR("render.warn.emitter_capped"), args);
            }
            else if (!(rate < config.rate)) {
                emitter.cappedSaid = false;
            }
            emitter.carry += static_cast<f64>(rate) * static_cast<f64>(step);
            const auto whole = static_cast<u64>(emitter.carry);
            emitter.carry -= static_cast<f64>(whole);
            spawn(emitter, frame, extent, whole);
        }
    });

    std::erase_if(m_emitters, [](const Emitter& emitter) { return !emitter.seen; });

    // The ground the GPU's particles land on, a few rows of it.
    if (groundWanted)
        buildGround(world);
    else {
        m_ground = RenderParticleGround{};
        m_groundBuilding = false;
    }
}

void ParticleSystem::buildGround(const scene::World& world)
{
    if (!m_cameraKnown)
        return;
    // What the terrains are now: when it changes, the map is of other ground.
    u64 signature = 0;
    world.terrains().forEach([&](core::InstanceId id, const scene::TerrainComponent& terrain) {
        signature = signature * 1099511628211ull + (static_cast<u64>(id.index) << 32) + terrain.fieldRevision;
    });
    const f64 side = static_cast<f64>(GroundCells) * static_cast<f64>(GroundCellMetres);
    const auto snapped = [](f64 value) { return std::floor(value / GroundStepMetres + 0.5) * GroundStepMetres; };
    const core::DVec3 corner{snapped(m_camera.x) - side * 0.5, 0.0, snapped(m_camera.z) - side * 0.5};
    const bool stale = m_ground.heights.empty() || m_ground.corner.x != corner.x || m_ground.corner.z != corner.z ||
                       m_groundBuilt != signature;
    const bool restart = m_groundBuilding && (m_groundNext.corner.x != corner.x || m_groundNext.corner.z != corner.z ||
                                              m_groundSignature != signature);
    if (!m_groundBuilding && !stale)
        return;
    if (!m_groundBuilding || restart) {
        m_groundBuilding = true;
        m_groundRows = 0;
        m_groundSignature = signature;
        m_groundNext.corner = corner;
        m_groundNext.side = static_cast<f32>(side);
        m_groundNext.cells = GroundCells;
        m_groundNext.heights.assign(static_cast<usize>(GroundCells) * GroundCells, -1.0e9f);
    }
    const u32 last = std::min(GroundCells, m_groundRows + GroundRowsPerUpdate);
    for (u32 row = m_groundRows; row < last; ++row) {
        const f64 z = m_groundNext.corner.z + (static_cast<f64>(row) + 0.5) * static_cast<f64>(GroundCellMetres);
        for (u32 column = 0; column < GroundCells; ++column) {
            const f64 x = m_groundNext.corner.x + (static_cast<f64>(column) + 0.5) * static_cast<f64>(GroundCellMetres);
            f32 top = -1.0e9f;
            world.terrains().forEach([&](core::InstanceId, const scene::TerrainComponent& terrain) {
                const std::optional<float> height =
                    asset::heightAt(terrain.field, x - terrain.origin.x, z - terrain.origin.z);
                if (height.has_value())
                    top = std::max(top, *height + static_cast<f32>(terrain.origin.y));
            });
            m_groundNext.heights[static_cast<usize>(row) * GroundCells + column] = top;
        }
    }
    m_groundRows = last;
    if (m_groundRows < GroundCells)
        return;
    // Whole: it is the map from here, and says so.
    m_groundNext.revision = m_ground.revision + 1;
    std::swap(m_ground, m_groundNext);
    m_groundBuilt = m_groundSignature;
    m_groundBuilding = false;
}

usize ParticleSystem::gpuEmitterCount() const noexcept
{
    usize count = 0;
    for (const Emitter& emitter : m_emitters)
        count += emitter.gpu ? 1 : 0;
    return count;
}

void ParticleSystem::append(RenderWorld& out, const TextureLibrary* textures) const
{
    const core::DVec3 origin = out.camera.origin;
    m_camera = origin;
    m_cameraKnown = out.camera.valid;
    out.particleGround = m_ground.heights.empty() ? nullptr : &m_ground;
    const usize first = out.particles.size();
    for (const Emitter& emitter : m_emitters) {
        const scene::ParticleEmitterComponent& config = emitter.config;
        const rhi::TextureHandle picture =
            textures != nullptr && config.texture.valid() ? textures->find(config.texture) : rhi::TextureHandle{};
        if (emitter.gpu) {
            if (!emitter.gpuAnchored)
                continue;
            constexpr f32 Radians = 3.14159265f / 180.0f;
            RenderGpuEmitter asked;
            asked.id = emitter.id;
            asked.serial = m_updates;
            asked.frame = emitter.gpuFrame;
            asked.extent = emitter.gpuExtent;
            asked.spawn = emitter.gpuSpawn;
            asked.capacity = emitter.gpuCapacity;
            const u64 seed = seedOf(emitter.id);
            asked.seed = static_cast<u32>(seed ^ (seed >> 32));
            asked.step = emitter.gpuStep;
            asked.speed = config.speed;
            asked.spread = std::clamp(config.spreadAngle, 0.0f, 180.0f) * Radians;
            asked.lifetime = config.lifetime;
            asked.drag = config.drag;
            asked.acceleration = config.acceleration;
            asked.wind = emitter.gpuWind;
            asked.rotation = config.rotation * Radians;
            asked.rotationSpread = config.rotationSpread * Radians;
            asked.spin = config.rotationSpeed * Radians;
            asked.spinSpread = config.rotationSpeedSpread * Radians;
            asked.collision = config.collision;
            asked.response = config.collisionResponse;
            asked.bounce = config.bounce;
            asked.friction = config.friction;
            asked.radius = config.collisionRadius > 0.0f ? config.collisionRadius : config.size * 0.5f;
            asked.restSpeed = RestSpeed;
            asked.emission = std::clamp(config.lightEmission, 0.0f, 1.0f);
            asked.shape = config.shape;
            asked.texture = picture;
            asked.columns = static_cast<u32>(std::clamp(config.flipbookColumns, 1, MaxFlipbookEdge));
            asked.rows = static_cast<u32>(std::clamp(config.flipbookRows, 1, MaxFlipbookEdge));
            asked.flipbookMode = config.flipbookMode;
            asked.framerate = std::max(config.flipbookFramerate, 0.0f);
            for (int at = 0; at < 16; ++at) {
                const f32 t = static_cast<f32>(at) / 15.0f;
                const core::Color3 tint = core::evaluate(config.colorOverLife, t);
                const core::Vec3 color = lerp(core::Vec3{config.color.r, config.color.g, config.color.b},
                                              core::Vec3{config.colorEnd.r, config.colorEnd.g, config.colorEnd.b}, t);
                const f32 seeThrough = std::clamp(core::evaluate(config.transparencyOverLife, t), 0.0f, 1.0f);
                asked.colorOverLife[at][0] = color.x * tint.r * config.brightness;
                asked.colorOverLife[at][1] = color.y * tint.g * config.brightness;
                asked.colorOverLife[at][2] = color.z * tint.b * config.brightness;
                asked.colorOverLife[at][3] =
                    std::clamp(1.0f - lerp(config.transparency, config.transparencyEnd, t), 0.0f, 1.0f) *
                    (1.0f - seeThrough);
                asked.sizeOverLife[at] =
                    std::max(0.0f, lerp(config.size, config.sizeEnd, t) * core::evaluate(config.sizeOverLife, t));
            }
            out.gpuEmitters.push_back(asked);
            continue;
        }
        const u32 columns = static_cast<u32>(std::clamp(config.flipbookColumns, 1, MaxFlipbookEdge));
        const u32 rows = static_cast<u32>(std::clamp(config.flipbookRows, 1, MaxFlipbookEdge));
        const u32 frames = columns * rows;
        for (const Particle& particle : emitter.particles) {
            const f32 t = std::clamp(particle.age / std::max(particle.lifetime, 1e-4f), 0.0f, 1.0f);
            // The start and end, times the curve over life (ADR 0160).
            const core::Color3 tint = core::evaluate(config.colorOverLife, t);
            const core::Vec3 color = lerp(core::Vec3{config.color.r, config.color.g, config.color.b},
                                          core::Vec3{config.colorEnd.r, config.colorEnd.g, config.colorEnd.b}, t);
            const f32 seeThrough = std::clamp(core::evaluate(config.transparencyOverLife, t), 0.0f, 1.0f);
            const f32 opacity = std::clamp(1.0f - lerp(config.transparency, config.transparencyEnd, t), 0.0f, 1.0f) *
                                (1.0f - seeThrough);
            if (opacity <= 0.0f)
                continue;
            RenderParticle drawn;
            drawn.position = core::toVec3(particle.position - origin);
            drawn.size = std::max(0.0f, lerp(config.size, config.sizeEnd, t) * core::evaluate(config.sizeOverLife, t));
            drawn.color[0] = color.x * tint.r * config.brightness;
            drawn.color[1] = color.y * tint.g * config.brightness;
            drawn.color[2] = color.z * tint.b * config.brightness;
            drawn.color[3] = opacity;
            drawn.emission = std::clamp(config.lightEmission, 0.0f, 1.0f);
            drawn.shape = config.shape;
            drawn.distance = core::length(drawn.position);
            drawn.rotation = particle.rotation;
            if (picture.valid()) {
                drawn.texture = picture;
                const u32 frame = flipbookFrame(config, frames, particle.frame, particle.age, t);
                const f32 width = 1.0f / static_cast<f32>(columns);
                const f32 height = 1.0f / static_cast<f32>(rows);
                drawn.uv[0] = static_cast<f32>(frame % columns) * width;
                drawn.uv[1] = static_cast<f32>(frame / columns) * height;
                drawn.uv[2] = drawn.uv[0] + width;
                drawn.uv[3] = drawn.uv[1] + height;
            }
            out.particles.push_back(drawn);
        }
    }

    // Back to front, which is what blending over one another needs; and when
    // there are more than can be drawn, the nearest are the ones kept.
    std::sort(out.particles.begin() + static_cast<std::ptrdiff_t>(first), out.particles.end(),
              [](const RenderParticle& a, const RenderParticle& b) { return a.distance > b.distance; });
    if (out.particles.size() - first > MaxDrawn)
        out.particles.erase(out.particles.begin() + static_cast<std::ptrdiff_t>(first),
                            out.particles.end() - static_cast<std::ptrdiff_t>(MaxDrawn));
}

usize ParticleSystem::liveCount() const noexcept
{
    usize count = 0;
    for (const Emitter& emitter : m_emitters)
        count += emitter.particles.size();
    return count;
}

} // namespace engine::render
