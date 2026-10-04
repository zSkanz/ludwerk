// Particles (F2): what a `ParticleEmitter` makes, simulated on the frame.
//
// **A picture, not the world.** A particle collides with nothing, no script can
// find one and nothing the simulation decides depends on where it went -- so it
// lives here, on the render clock, and a scene with ten thousand of them pays
// for them in one draw rather than in the world hash, the snapshot and the
// replication budget. What IS world state is the emitter: its configuration,
// and a running total of the bursts a script asked for, which this reads and
// never writes.
//
// **Seeded per emitter**, from its instance id, so two runs of one headless
// capture spawn the same particles in the same places -- the property a golden
// image needs, bought for the price of one generator per emitter.
#pragma once

#include <functional>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/random.h"
#include "engine/core/types.h"
#include "engine/render/draw_poses.h"
#include "engine/render/render_world.h"
#include "engine/scene/components.h"

namespace engine::scene {
class World;
}

namespace engine::render {

struct RenderWorld;
class TextureLibrary;

class ParticleSystem
{
public:
    // Particles one emitter may hold at once; past it, new ones are not born.
    static constexpr core::usize MaxPerEmitter = 4096;
    // **The rate a stream is born at**: what was asked, or -- where that many
    // living at once would pass the cap -- the most the cap sustains. A stream
    // of 3000 a second living two seconds filled the emitter, stopped, and
    // began again as the old ones died: clumps and gaps where a column was
    // asked for. Thinned evenly it is a column again. The tenth is the spread
    // a particle's life is given either way (`spawn`).
    [[nodiscard]] static core::f32 effectiveRate(core::f32 rate, core::f32 lifetime) noexcept
    {
        if (!(rate > 0.0f) || !(lifetime > 0.0f))
            return 0.0f;
        const core::f32 sustained = static_cast<core::f32>(MaxPerEmitter) / (lifetime * 1.1f);
        return rate < sustained ? rate : sustained;
    }
    // Particles drawn in one frame, nearest first when there are more.
    static constexpr core::usize MaxDrawn = 32768;
    // The longest step one update takes. A frame that stalled for a second
    // would otherwise fire a second's worth of stream in one place.
    static constexpr core::f64 MaxStep = 0.1;
    // The most a burst spawns in one frame, whatever a script asked for.
    static constexpr core::u64 MaxBurstPerFrame = 1000;
    // The speed out of a surface, in metres a second, under which a bounced
    // particle rests on it instead (ADR 0160).
    static constexpr core::f32 RestSpeed = 0.35f;
    // **On the GPU** (ADR 0160): the most particles one emitter's buffer
    // holds, and the ground map particles there collide with -- a square of
    // `GroundCells` heights a metre apart, round the camera.
    static constexpr core::u32 MaxGpuPerEmitter = 262144;
    static constexpr core::u32 GroundCells = 128;
    static constexpr core::f32 GroundCellMetres = 1.0f;
    // How far the camera moves before the map is made again round it, and how
    // many of its rows one update fills: a map is sixteen thousand heights,
    // and all of them in one frame is a frame nobody wants.
    static constexpr core::f64 GroundStepMetres = 16.0;
    static constexpr core::u32 GroundRowsPerUpdate = 16;
    // Whether the device can simulate on the GPU. Without, an emitter that
    // asks for it is simulated here, and the log is told once.
    void setGpuSimulation(bool available) noexcept { m_gpuAvailable = available; }
    [[nodiscard]] core::usize gpuEmitterCount() const noexcept;

    // A flipbook is at most this many frames across and down (ADR 0160).
    static constexpr core::i32 MaxFlipbookEdge = 16;
    // How many frames an emitter's flipbook has, and which one a particle
    // shows: its own random one, its share of a loop, or its place in life.
    [[nodiscard]] static core::u32 flipbookFrames(const scene::ParticleEmitterComponent& config) noexcept;
    [[nodiscard]] static core::u32 flipbookFrame(const scene::ParticleEmitterComponent& config, core::u32 frames,
                                                 core::u32 own, core::f32 age, core::f32 life) noexcept;

    // **A ray against the parts of the world** (ADR 0160): from `from` along
    // `delta`, answering where it first hits and the surface's normal there.
    // The host's, because what a part collides as is the physics module's to
    // say and this one does not reach it. Unset, a particle meets no part.
    using Raycast = std::function<bool(core::DVec3 from, core::Vec3 delta, core::DVec3& hit, core::Vec3& normal)>;
    void setRaycast(Raycast raycast) { m_raycast = std::move(raycast); }
    // The most rays one update casts for particles: past it, the rest of that
    // frame's particles fly on untested, which a spark does not show.
    static constexpr core::u32 MaxRaysPerUpdate = 2048;
    // How many rays the last update cast, and how many particles it bounced,
    // stuck or killed.
    [[nodiscard]] core::u32 lastRays() const noexcept { return m_lastRays; }
    [[nodiscard]] core::u32 lastCollisions() const noexcept { return m_lastCollisions; }

    // Advances every emitter under `root` by `dt` seconds: births, motion and
    // deaths. An emitter that has gone takes its particles with it. Born where
    // the emitter is drawn this frame (`poses`, ADR 0134) -- from the simulated
    // place, a moving emitter's particles came out ahead of it, a tick at a
    // time; null is the simulated place, for a world drawn at its tick.
    void update(const scene::World& world, core::InstanceId root, core::f64 dt, const DrawPoses* poses = nullptr);

    // Appends this frame's particles to `out`, relative to its camera and
    // sorted back to front, which is the order blending needs. `textures`
    // resolves an emitter's picture; null, or a picture not loaded yet, draws
    // its shape.
    void append(RenderWorld& out, const TextureLibrary* textures = nullptr) const;

    [[nodiscard]] core::usize liveCount() const noexcept;
    [[nodiscard]] core::usize emitterCount() const noexcept { return m_emitters.size(); }
    void clear() noexcept { m_emitters.clear(); }

private:
    struct Particle
    {
        core::DVec3 position;
        core::Vec3 velocity;
        core::f32 age = 0.0f;
        core::f32 lifetime = 1.0f;
        // Radians at birth and radians a second (ADR 0160).
        core::f32 rotation = 0.0f;
        core::f32 spin = 0.0f;
        // A Random flipbook's frame, or where a Loop starts.
        core::u32 frame = 0;
        // Stuck where it landed (ADR 0160): it ages and moves no more.
        bool stuck = false;
    };

    struct Emitter
    {
        core::InstanceId id;
        // The configuration as of the last update, for drawing.
        scene::ParticleEmitterComponent config;
        std::vector<Particle> particles;
        // The fraction of a particle the stream owes from earlier frames.
        core::f64 carry = 0.0;
        // How much of `config.emitted` has been spawned.
        core::u64 spawnedBursts = 0;
        core::Pcg32 random{1};
        bool seen = false;
        // Whether the log has been told this emitter's stream is capped.
        bool cappedSaid = false;
        // **Simulated on the GPU** (ADR 0160): no particles here, only what
        // this update asks of its buffer -- where they are born, how many, how
        // long the step is -- and how large the buffer has had to be.
        bool gpu = false;
        bool gpuAnchored = false;
        core::CFrameD gpuFrame;
        core::Vec3 gpuExtent;
        core::Vec3 gpuWind;
        core::u32 gpuSpawn = 0;
        core::u32 gpuCapacity = 0;
        core::f32 gpuStep = 0.0f;
    };

    // Where an emitter's parent is, and the box particles are born in: a part's
    // volume, or a point for an attachment. False when it has neither.
    [[nodiscard]] static bool anchorOf(const scene::World& world, const DrawPoses& poses, core::InstanceId id,
                                       core::CFrameD& frame, core::Vec3& extent);
    static void spawn(Emitter& emitter, const core::CFrameD& frame, core::Vec3 extent, core::u64 count);

    // One particle against what its emitter says it meets, moved from `before`
    // to where it now is: bounced, stuck or killed there (ADR 0160).
    void collide(const scene::World& world, const scene::ParticleEmitterComponent& config, Particle& particle,
                 core::DVec3 before);

    // Sorted by instance id, so an update walks them in one order on every run.
    std::vector<Emitter> m_emitters;
    Raycast m_raycast;
    core::u32 m_lastRays = 0;
    core::u32 m_lastCollisions = 0;

    // The ground map (ADR 0160), filled a few rows an update while any
    // emitter collides with the terrain: what the GPU's particles read, and
    // what the CPU's read in place of a walk down the field's voxels.
    void buildGround(const scene::World& world);
    // The ground's height and slope at (x, z) from the map; false where the
    // map does not reach, or there is none yet.
    [[nodiscard]] bool groundFromMap(core::f64 x, core::f64 z, core::f32& top, core::Vec3& normal) const noexcept;
    bool m_gpuAvailable = false;
    bool m_gpuSaid = false;
    core::u64 m_updates = 0;
    RenderParticleGround m_ground;
    // The map being made: its corner, how many rows are filled, what the
    // terrains were when it began.
    RenderParticleGround m_groundNext;
    core::u32 m_groundRows = 0;
    core::u64 m_groundSignature = 0;
    core::u64 m_groundBuilt = 0;
    bool m_groundBuilding = false;
    // Where the last view drawn stood: what the map is made round.
    mutable core::DVec3 m_camera;
    mutable bool m_cameraKnown = false;
};

} // namespace engine::render
