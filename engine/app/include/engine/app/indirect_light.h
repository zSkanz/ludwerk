// **What a room reflects, and the light it gives back** (ADRs 0202, 0203): the
// part of both that is not drawing.
//
// A `ReflectionProbe` is six pictures of its surroundings. This decides whose
// are taken and when -- the nearest first, one picture a frame while the game
// is watched and as many as a frame has room for behind the loading curtain --
// and the frame loop draws them, because a picture is the world extracted from
// another camera, and the loop is what holds the world.
//
// An `IrradianceVolume` is worked out here, on the processor: from each of its
// points, rays against the physics world say what is seen and how it is lit.
// Nothing of the GPU is in it, so it is the same light on every backend and on
// a phone, and it is worked out again under a moving sun without costing the
// frame a pass.
//
// **When the rays run**: between ticks, inside the frame's own drawing, with
// the caller waiting for the workers. The physics world is read by many
// threads at once and stepped by none while they do.
#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "engine/app/baked_light.h"
#include "engine/app/mesh_rays.h"
#include "engine/core/math.h"
#include "engine/core/types.h"
#include "engine/render/render_world.h"
#include "engine/render/renderer.h"
#include "engine/rhi/device.h"
#include "engine/scene/physics_sync.h"
#include "engine/scene/world.h"

namespace engine::app {

// The renderer's view every probe's pictures are drawn as (ADR 0107): one,
// since they are all one size and none keeps a history. Far from the numbers
// the camera textures count up from.
inline constexpr core::u32 ProbePictureView = 0xFFFFFF00u;

// How many rays leave each point of a volume, and how far one goes.
inline constexpr core::u32 kBakeRays = 64;
inline constexpr core::f32 kBakeReach = 200.0f;

// What a ray of the bake met: nothing, or a surface that does not move --
// where, which way it faces, the colour it is drawn in and what it gives off.
// `inside` is a ray that began within a solid, or met the back of a shell.
struct BakeHit
{
    bool hit = false;
    bool inside = false;
    core::DVec3 position;
    core::Vec3 normal{0.0f, 1.0f, 0.0f};
    core::Color3 albedo{1.0f, 1.0f, 1.0f};
    core::Color3 emissive{0.0f, 0.0f, 0.0f};
};
// Asked from many threads at once.
using BakeTrace = std::function<BakeHit(const core::DVec3& origin, core::Vec3 direction, core::f32 reach)>;

// A lamp as the bake lights with it: what a `PointLight` or a `SpotLight` is
// to the shader, in the world's own space.
struct BakeLamp
{
    core::DVec3 position;
    core::Vec3 direction{0.0f, -1.0f, 0.0f};
    // Colour times brightness.
    core::Color3 radiance{1.0f, 1.0f, 1.0f};
    core::f32 range = 16.0f;
    // -1 for a lamp that shines every way.
    core::f32 cosOuter = -1.0f;
    bool shadows = false;
};

struct BakeLight
{
    // Towards the sun, and what it lights with: its colour times its
    // brightness, nought by night.
    core::Vec3 sunDirection{0.0f, 1.0f, 0.0f};
    core::Color3 sunRadiance{0.0f, 0.0f, 0.0f};
    // What a ray that meets nothing sees: the sky that way, without the sun's
    // own disc -- the sun lights a surface directly, and is not counted twice.
    std::function<core::Color3(core::Vec3 direction)> sky;
    std::vector<BakeLamp> lamps;
    // One number for all of the above, the caller's: what a volume says it
    // was lit by (`VolumeBake::litBy`). Nought is no light at all.
    core::u64 stamp = 0;
};

// How a box is sampled: how many points along each side, and how far apart
// they came out. `coarser` when the spacing asked for would have been more
// than a volume holds.
struct VolumeLayout
{
    core::u32 counts[3] = {1, 1, 1};
    core::f32 spacing = 2.0f;
    bool coarser = false;
};
[[nodiscard]] VolumeLayout volumeLayoutOf(core::Vec3 size, core::f32 spacing) noexcept;

// **How one `IrradianceVolume` is worked out** (ADR 0206): as one box; as
// tiles, where its box at its spacing is more than one volume holds; or as
// levels round the camera. A piece is a box of its own, with points of its
// own, worked out and drawn as a volume is.
struct VolumePiece
{
    core::CFrameD box;
    core::Vec3 size{1.0f, 1.0f, 1.0f};
    VolumeLayout layout;
    // How its light is weighed against its neighbours'. Nought: a room, whole
    // to its faces. More: a tile, and the metres from a face over which it
    // gives way to the tile beside it. Less: a level, and the same metres
    // over which it gives way to the wider level round it.
    core::f32 margin = 0.0f;
};
inline constexpr core::u32 kMaxVolumeTiles = 16;
inline constexpr core::u32 kMaxVolumeLevels = 3;
// How many of a level's cells the camera crosses before the level is carried
// after it: it moves a few cells at a time, so most of its points are points
// it already had.
inline constexpr core::i32 kLevelSnapCells = 4;

// **A box as tiles.** One piece where one volume holds it. Otherwise as few
// tiles as hold it at the spacing asked for, each sharing a cell with the
// tile beside it -- their points stand at the same places there, and each
// gives way to the other across the two cells. A box that would take more
// than sixteen is one piece at a coarser spacing, as it was before there
// were tiles (`layout.coarser`).
[[nodiscard]] std::vector<VolumePiece> volumeTilesOf(const core::CFrameD& box, core::Vec3 size, core::f32 spacing);

// **Levels round the camera.** The first is a box of `size` at `spacing`;
// each one after is twice as large with points twice as far apart, round the
// same middle. Each stands on a grid of its own cells and is carried after
// the camera a few cells at a time. Lying down: a level's own z is the
// world's up, which is the way its slices are kept, so a level that is wide
// and not tall is few slices.
[[nodiscard]] std::vector<VolumePiece> volumeLevelsOf(core::Vec3 size, core::f32 spacing, core::u32 levels,
                                                      const core::DVec3& camera);

// The direction of the `index`th of `count` rays: the same set from every
// point, spread evenly over the sphere.
[[nodiscard]] core::Vec3 bakeDirection(core::u32 index, core::u32 count) noexcept;

// **One volume, worked out.** `begin` lays its points out; `step` does as much
// as fits a budget and says when it is whole. The rays are kept: `relight`
// lights what they met again under another light without casting them again,
// which is all a moving sun asks.
//
// **A volume is lit by one moment's light, from its first point to its
// last.** `step` is handed the light as it is now every time it is called,
// and takes it once: when the lighting begins. Lit a share a frame under a
// light that changes every frame, a volume used to be lit by a different
// light each share -- slabs of it, by the order of its points.
class VolumeBake
{
public:
    void begin(const core::CFrameD& box, core::Vec3 size, const VolumeLayout& layout, core::u32 bounces);
    // True when whole. `budgetNs` of nought does all of it; otherwise it
    // stops at the first look at the clock past the budget, and every part
    // of the work -- the rays, the lighting, each bounce -- is in pieces
    // small enough for that look to come soon.
    bool step(const BakeTrace& trace, const BakeLight& light, core::u64 budgetNs);
    // Back to the lighting, the rays kept. Nothing to do before `begin`.
    void relight() noexcept;
    // **Carried to another place** (ADR 0206): the same box, `cells` of its
    // own cells along each of its sides from where it stood. The points it
    // has in common with where it stood keep their rays -- and, under the
    // same light, what the light put on them -- and only the points that are
    // new are cast for. False, and nothing done, where it is not whole, has
    // given its rays back, or has no point in common: that is `begin`'s.
    bool scroll(const core::CFrameD& box, const core::i32 (&cells)[3]);
    // The stamp of the light it is being lit by, or was: nought before any.
    [[nodiscard]] core::u64 litBy() const noexcept { return m_lightHeld ? m_lit.stamp : 0; }
    // Gives the rays' memory back: a volume that will not be lit again.
    void forgetRays() noexcept;
    // **Whole, by a light that was worked out before and kept** (ADR 0207):
    // its points are `light`, and no ray is cast for them. It has no rays to
    // be lit again by -- `begin` is what works it out afresh.
    void give(const core::CFrameD& box, core::Vec3 size, const VolumeLayout& layout, std::span<const core::f32> light);

    [[nodiscard]] bool begun() const noexcept { return m_stage != Stage::Idle; }
    [[nodiscard]] bool whole() const noexcept { return m_stage == Stage::Done; }
    // Whether it has the rays it would be lit again by: not one that gave
    // them back, and not one that was given its light.
    [[nodiscard]] bool hasRays() const noexcept { return !m_rays.empty(); }
    [[nodiscard]] core::f32 progress() const noexcept;
    [[nodiscard]] const VolumeLayout& layout() const noexcept { return m_layout; }
    // Twelve numbers a point (`RenderIrradianceVolume::samples`).
    [[nodiscard]] std::span<const core::f32> result() const noexcept { return m_light; }
    // How many points were inside something and took their neighbours' light.
    [[nodiscard]] core::u32 dropped() const noexcept { return m_dropped; }

private:
    enum class Stage : core::u8
    {
        Idle,
        Trace,
        Light,
        Gather,
        Done,
    };
    struct Ray
    {
        // 0 nothing, 1 a surface, 2 inside something.
        core::u8 met = 0;
        // In the box's own space; the normal in the world's.
        core::Vec3 place;
        core::Vec3 normal;
        core::Color3 albedo;
        core::Color3 emissive;
        // What the sun and the lamps put on it, over pi.
        core::Color3 direct;
    };

    [[nodiscard]] core::usize sampleCount() const noexcept;
    [[nodiscard]] core::DVec3 samplePlace(core::usize sample) const noexcept;
    void traceSample(core::usize sample, const BakeTrace& trace);
    void lightSample(core::usize sample, const BakeTrace& trace);
    // The gathering, in pieces: laid out, one pass over some of the points,
    // and what ends a pass -- true when it was the last.
    void gatherBegin();
    void gatherSamples(core::usize begin, core::usize end);
    [[nodiscard]] bool gatherPassEnd();
    void fillDropped(std::vector<core::f32>& values) const;
    [[nodiscard]] core::Color3 lookUp(std::span<const core::f32> from, core::Vec3 place,
                                      core::Vec3 normal) const noexcept;

    Stage m_stage = Stage::Idle;
    core::CFrameD m_box;
    core::Vec3 m_half{1.0f, 1.0f, 1.0f};
    VolumeLayout m_layout;
    core::u32 m_bounces = 1;
    core::usize m_cursor = 0;
    // The light the lighting began with, held until the volume is whole.
    BakeLight m_lit;
    bool m_lightHeld = false;
    // After a scroll: which points are new, the stamp of the light the others
    // were lit by, and whether that is still the light -- then only the new
    // ones are lit.
    std::vector<core::u8> m_fresh;
    core::u64 m_scrolledStamp = 0;
    bool m_lightFreshOnly = false;
    // Which pass the gathering is in: the sky and what glows, then a bounce
    // each. And what each ray that met nothing sees, the same from every point.
    core::u32 m_pass = 0;
    std::array<core::Color3, kBakeRays> m_sky{};
    std::array<core::Vec3, kBakeRays> m_directions{};
    std::vector<Ray> m_rays;
    std::vector<core::u8> m_inside;
    std::vector<core::f32> m_light;
    std::vector<core::f32> m_scratch;
    core::u32 m_dropped = 0;
};

// One volume that could be lit again, as the choice between them sees it.
struct RelightCandidate
{
    // Whole, `Dynamic`, and lit by a light that is no longer the light.
    bool stale = false;
    bool inView = false;
    core::f64 distance = 0.0;
    // When it was last chosen, by a count of choices: nought for never.
    core::u64 served = 0;
};
// **Which volume is lit again next**, or `volumes.size()` for none. One at a
// time: what has begun is finished and shown before another begins, so the
// work never grows faster than it is done. Those in view before those that
// are not; among them the one that has waited longest -- so the nearest
// volume, lit again and stale again a moment later, does not keep every
// other from ever being lit -- and the nearest of those that have waited
// equally.
[[nodiscard]] core::usize pickRelight(std::span<const RelightCandidate> volumes) noexcept;

class IndirectLight
{
public:
    struct Frame
    {
        scene::World* world = nullptr;
        const scene::PhysicsSync* physics = nullptr;
        render::IRenderer* renderer = nullptr;
        // The main view's snapshot, extracted: where the camera is and what
        // the sun and the sky are.
        const render::RenderWorld* snapshot = nullptr;
        core::InstanceId workspace;
        // The meshes and pictures the world is drawn with: an anchored mesh is
        // asked what a ray meets by its own triangles, and a surface gives
        // back its picture's mean colour. Without them a mesh is its
        // collision shape and a surface its tint.
        const render::MeshLibrary* meshes = nullptr;
        const render::TextureLibrary* textures = nullptr;
        // Off, a mesh is asked by its hull even so: a measurement's.
        bool traceMeshes = true;
        // Meshes and pictures are all in: nothing is taken or worked out of a
        // room whose walls have not arrived.
        bool loadersIdle = true;
        // Behind the loading curtain: as much a frame as a frame has room for.
        bool flatOut = false;
        core::u64 nowNs = 0;
        // The simulation's tick: a volume waits two of them after it is asked
        // for, because the physics world its rays are cast against is brought
        // up to the scene by a tick, and a wall made this frame is not in it.
        core::u64 tick = 0;
        // Seconds since the last frame, for what fades in.
        core::f32 seconds = 0.0f;
        // **A content file's bytes by its name**, or false: what a volume's
        // `Baked` names (ADR 0207). Empty, no volume has a file.
        std::function<bool(std::string_view urn, std::vector<std::byte>& out)> readContent{};
        // An editor or a development run: said once where a room is not the
        // room its file was baked in.
        bool development = false;
        // Baking: every volume is worked out by rays, on a machine of any
        // setting, and no file is read -- what is written is what the rays
        // said, not what the last file said.
        bool baking = false;
    };
    // Once a frame, after the main view is extracted and before any is drawn.
    void update(const Frame& frame);

    // One of a probe's six pictures to draw now, or nothing more this frame.
    struct Face
    {
        rhi::TextureHandle target;
        core::u32 size = 0;
        // Looking out of the probe's place, a quarter turn wide.
        core::CFrameD lens;
    };
    [[nodiscard]] std::optional<Face> nextFace(rhi::IDevice& device);
    // After it is drawn, with the camera's matrix it was drawn by. The sixth
    // hands the probe to the renderer.
    void faceDrawn(rhi::IDevice& device, rhi::ICmdList& cmd, render::IRenderer& renderer,
                   const core::Mat4& viewProjection, core::u64 nowNs);

    // The probes and volumes in view into a snapshot of the same world: the
    // main view's, a camera's feed, a probe's own picture. `whole` for a
    // probe's picture: what is still coming in over a moment for the eye is
    // all there for a picture that will be kept.
    void append(render::RenderWorld& out, bool whole = false) const;

    // **Nothing in view of the camera is still the sky standing in**: what
    // the loading curtain waits for.
    [[nodiscard]] bool settled() const noexcept { return m_settled; }

    // **A volume's light, to keep** (ADR 0207): every piece of it as it was
    // last worked out, with what it was worked out as. False while any piece
    // is not whole, for one carried with the camera, and for one this frame
    // did not work out.
    [[nodiscard]] bool keep(const scene::World& world, core::InstanceId volume, BakedLight& out) const;
    // The files read are read again when next asked for: one was written, or
    // changed on disk.
    void forgetBaked() noexcept { m_kept.clear(); }
    // **What stands in a volume's box, as one number**: every part that does
    // not move and is in or against the box -- where it stands in the box's
    // own space, how it is turned and how large it is, to the millimetre. The
    // same room is the same number in any order of its parts; a wall moved,
    // added or taken away is another. Its colours are not in it. Never nought.
    [[nodiscard]] static core::u64 roomOf(const scene::World& world, core::InstanceId workspace, core::InstanceId own,
                                          const core::CFrameD& box, core::Vec3 size);

    void destroy(rhi::IDevice& device);

    struct Stats
    {
        core::u32 probes = 0;
        core::u32 probesHeld = 0;
        core::u32 picturesDrawn = 0;
        core::u32 volumes = 0;
        core::u32 volumesWhole = 0;
        // The last volume to finish: how many points, and how long its rays
        // and its lighting took in all.
        core::u32 lastVolumePoints = 0;
        core::f64 lastVolumeMs = 0.0;
        // From the first frame anything was outstanding to the frame nothing
        // was: what the curtain waited for it.
        core::f64 settledMs = -1.0;
        // The meshes the last volume begun asked by their triangles, and how
        // many triangles that was.
        core::u32 tracedMeshes = 0;
        core::u64 tracedTriangles = 0;
        // The last volume lit AGAIN, its rays kept: its points, and how long
        // the lighting took.
        core::u32 lastRelightPoints = 0;
        core::f64 lastRelightMs = 0.0;
        // The last volume carried after the camera: how long that took, and
        // how many times one has been.
        core::f64 lastCarryMs = 0.0;
        core::u64 carries = 0;
        // How many times a volume has been lit again, in all.
        core::u64 relights = 0;
        // Pieces whose light was read from a file and not worked out, in
        // all; and volumes drawn this frame as their file's one colour.
        core::u64 kept = 0;
        core::u32 flat = 0;
    };

    // One anchored mesh as a ray is asked of it: where it stands, how it is
    // stretched, and its triangles.
    struct TracedMesh
    {
        core::InstanceId id;
        core::CFrameD intoMesh;
        core::Mat3 turn;
        core::Vec3 stretch{1.0f, 1.0f, 1.0f};
        core::DVec3 low;
        core::DVec3 high;
        core::Color3 tint{1.0f, 1.0f, 1.0f};
        core::Color3 emissive{0.0f, 0.0f, 0.0f};
        const TriangleTree* tree = nullptr;
        const std::vector<render::MeshLibrary::Entry::Bounce>* bounces = nullptr;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return m_stats; }

private:
    struct Probe
    {
        core::InstanceId id;
        core::u64 key = 0;
        core::CFrameD box;
        core::Vec3 half{1.0f, 1.0f, 1.0f};
        scene::ReflectionProbeComponent set;
        core::f64 distance = 0.0;
        bool inView = false;
    };
    struct ProbeState
    {
        core::u64 key = 0;
        core::u32 refreshes = 0;
        core::u64 lightSeen = 0;
        bool taken = false;
        // How many times: the first pictures of a room are taken with nothing
        // yet for its own shiny surfaces to reflect but the sky, so they are
        // taken once more, with the first to reflect.
        core::u32 takes = 0;
        core::f32 presence = 1.0f;
        bool seen = false;
    };
    // **One piece of an `IrradianceVolume`** (ADR 0206): the whole of it,
    // one of its tiles, or one of its levels round the camera. Worked out
    // and drawn as a volume of its own.
    struct Volume
    {
        // The instance, and this piece of it: `key` is the piece's own.
        core::InstanceId id;
        core::u64 key = 0;
        core::u32 piece = 0;
        // How many pieces the instance is in.
        core::u32 pieces = 1;
        core::CFrameD box;
        core::Vec3 size{1.0f, 1.0f, 1.0f};
        VolumeLayout layout;
        core::f32 margin = 0.0f;
        // A level: carried after the camera, its rays kept for that.
        bool follows = false;
        scene::IrradianceVolumeComponent set;
        core::f64 distance = 0.0;
        bool inView = false;
    };
    struct VolumeState
    {
        core::u64 key = 0;
        VolumeBake bake;
        // What it was laid out from: a box moved, resized or respaced is
        // another volume.
        core::CFrameD box;
        core::Vec3 size;
        core::f32 spacing = 0.0f;
        core::i32 bounces = 0;
        core::i32 mode = 0;
        core::u32 refreshes = 0;
        // When it was last chosen to be lit again (`pickRelight`).
        core::u64 served = 0;
        // The tick it was last asked for at: seen first, moved, or refreshed.
        core::u64 askedTick = 0;
        bool asked = false;
        bool pending = false;
        core::u64 startedNs = 0;
        core::u64 spentNs = 0;
        // Lit again with its rays kept, not laid out afresh.
        bool relit = false;
        // What is drawn: the last whole result, moving to the newest -- and
        // the box it is the light OF, which a volume being carried somewhere
        // else is not at yet.
        core::CFrameD shownBox;
        core::Vec3 shownSize{1.0f, 1.0f, 1.0f};
        // Being carried: how many of its cells along each side, for what is
        // drawn to be carried with it when it gets there.
        bool carried = false;
        core::i32 carriedCells[3]{};
        std::vector<core::f32> shown;
        std::vector<core::f32> from;
        std::vector<core::f32> to;
        core::f32 mix = 1.0f;
        core::u32 revision = 0;
        core::f32 presence = 1.0f;
        bool hasResult = false;
        // Worked out as it was last asked for: false from the moment it is
        // asked for again until that is whole.
        bool ready = false;
        bool warned = false;
        bool warnedMeshes = false;
        // Said once: its file is not there, is another volume's, or is of a
        // room that has changed.
        bool warnedBaked = false;
        bool seen = false;
    };
    // A file read (ADR 0207), or looked for and not found.
    struct Kept
    {
        core::NameAtom urn;
        bool found = false;
        BakedLight light;
    };
    // A volume drawn as its file's one colour: what it was when its file
    // was looked at, so the file is looked at once and not every frame.
    struct Flat
    {
        core::u64 key = 0;
        core::CFrameD box;
        core::Vec3 size{1.0f, 1.0f, 1.0f};
        scene::IrradianceVolumeComponent set;
        // The file is this volume's light.
        bool lit = false;
        core::Color3 mean;
        bool seen = false;
    };
    [[nodiscard]] const Kept& keptFor(const Frame& frame, core::NameAtom urn);
    // The file a volume names, where it is the light of THAT volume; null,
    // with why said once, otherwise.
    [[nodiscard]] const BakedLight* bakedFor(const Frame& frame, core::InstanceId id,
                                             const scene::IrradianceVolumeComponent& set, core::u32 pieces,
                                             bool& warned);
    std::vector<Kept> m_kept;
    std::vector<Flat> m_flats;
    core::InstanceId m_workspace;

    // What each part is to a ray, looked up by the instance's index: read
    // from the scene once when a volume begins, because a ray that had to
    // resolve a part's material every time it met one would spend its time
    // there. `kind` 0 is a part made since, which is looked up as it is met.
    struct Surface
    {
        core::u32 generation = 0;
        // 1 a surface that stays, 2 one a ray goes on past: something that
        // moves, or a pane mostly see-through.
        core::u8 kind = 0;
        core::Color3 albedo{1.0f, 1.0f, 1.0f};
        core::Color3 emissive{0.0f, 0.0f, 0.0f};
    };
    std::vector<Surface> m_surfaces;
    void readSurfaces(const Frame& frame);

    // A mesh's triangles as a tree, made the first time a volume is begun
    // with the mesh in the world and kept: by its content, and remade if the
    // content's triangles are no longer the ones it was made of.
    struct Tree
    {
        core::NameAtom content;
        core::usize indices = 0;
        TriangleTree tree;
        std::vector<render::MeshLibrary::Entry::Bounce> bounces;
    };
    std::vector<std::unique_ptr<Tree>> m_trees;
    std::vector<TracedMesh> m_traced;
    BoxTree m_tracedBoxes;
    // Their bodies in the physics world, to leave out of a ray's cast there:
    // each is a hull the ray would otherwise meet, step past and be cast
    // again for. The first few; past them a body is stepped past as it is met.
    std::vector<core::u64> m_tracedBodies;

    [[nodiscard]] ProbeState& probeState(core::u64 key);
    [[nodiscard]] VolumeState& volumeState(core::u64 key);
    void planProbe(const Frame& frame);
    void stepVolumes(const Frame& frame);
    // `IsReady` and `Completed` (ADRs 0202, 0203): what each probe and volume
    // of the world says of itself, and the event when it becomes so.
    void sayReady(const Frame& frame);

    std::vector<Probe> m_probes;
    std::vector<ProbeState> m_probeStates;
    std::vector<Volume> m_volumes;
    std::vector<VolumeState> m_volumeStates;
    core::DVec3 m_camera;
    BakeLight m_light;
    bool m_settled = true;
    bool m_flatOut = false;
    core::u64 m_frameStartNs = 0;
    core::u64 m_outstandingSinceNs = 0;
    core::u32 m_probeBudget = 0;
    core::u32 m_probeSize = 0;
    bool m_volumesOn = false;
    // How many times a volume has been chosen to be lit again.
    core::u64 m_relightClock = 0;
    // What a watched frame may spend on the volumes
    // (`GraphicsSettings::indirectLightBudgetMs`).
    core::u64 m_watchedBudgetNs = 1'500'000;

    // The probe being taken: its pictures so far.
    struct Job
    {
        bool live = false;
        core::u64 key = 0;
        core::DVec3 place;
        core::u32 face = 0;
        core::u32 refreshes = 0;
        core::u64 light = 0;
        core::Mat4 viewProjections[6];
    };
    Job m_job;
    core::u32 m_facesThisFrame = 0;
    rhi::TextureHandle m_faces[6]{};
    core::u32 m_faceSize = 0;
    Stats m_stats;
};

} // namespace engine::app
