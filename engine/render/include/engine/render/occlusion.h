#pragma once

// **What a wall hides is not drawn** (ADR 0262): a small depth buffer of the
// biggest occluders the camera sees, drawn in software each frame, and every
// draw the frustum kept tested against it by its bounding sphere. A draw
// behind is left out of the camera's passes (`DrawItem::inCameraFrustum`);
// its shadow is still cast.
//
// The occluders are boxes -- opaque blocks, and what an author marked solid
// -- and the ground near the camera, a terrain's or a block world's, as a
// grid of heights (`OccluderGrid`).
//
// Conservative: an occluder covers a pixel only where the whole pixel is
// inside it, at the farthest depth it has there, and a sphere is hidden only
// where every pixel it covers has an occluder nearer than its nearest point.
// A false hide is a missing thing; a missed hide is only a draw.
//
// It pays for itself or rests: what it hid, at what a draw costs, against its
// own time. Losing for a sixth of a second, it stops testing, and tries one
// frame a second to see whether it would pay again; a frame that rests costs
// nothing. With fewer draws in view than it could ever pay for, it does not
// test at all.

#include <array>
#include <limits>
#include <span>
#include <vector>

#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::render {

struct RenderWorld;
struct OccluderGrid;

// **A tile of ground**, what an occluder grid is made from (ADR 0262): the
// top of what is solid all the way down under the whole of it, and where
// that solid starts, in metres above the grid's origin. Not `solid` where it
// is not ground to the bottom, or where the ground is not drawn.
struct GroundTile
{
    core::f32 top = 0.0f;
    core::f32 bottom = 0.0f;
    bool solid = false;
};

// **The grid over `columns` by `rows` tiles**, from (columns + 1) by
// (rows + 1) of them, a row of x at a time -- one past the grid on +x and +z,
// because a tile's triangles reach the first column of the tiles after it.
// A corner is the lowest top of the solid tiles round it, so a triangle never
// rises above what is solid under it; a tile is drawn only where it and the
// three after it are solid, and its corners are above where they start.
// Fills `heights` and `solid`; the caller places it (`origin`, `tile`).
void makeGroundGrid(std::span<const GroundTile> tiles, core::u32 columns, core::u32 rows, OccluderGrid& out);

// What the culler did with one frame, for the frame report and F3.
struct OcclusionStats
{
    // It tested this frame (false: off, or resting).
    bool tested = false;
    // Resting: it saved less than it cost, or there is too little to save,
    // and it waits for a frame that would pay.
    bool resting = false;
    core::u32 occluders = 0;
    core::u32 candidates = 0;
    core::u32 hidden = 0;
    // Its own time this frame, in milliseconds.
    core::f64 ms = 0.0;
};

class OcclusionCuller
{
public:
    static constexpr core::u32 Width = 256;
    static constexpr core::u32 Height = 128;
    // The most occluders drawn a frame: the biggest on screen first.
    static constexpr core::u32 MostOccluders = 96;
    // An occluder smaller than this on screen, in the buffer's pixels across,
    // hides too little to draw.
    static constexpr core::f32 SmallestOccluderPixels = 6.0f;
    // Frames in a row saving less than it costs before it rests, and how
    // often a resting culler tests one frame to see whether it would pay
    // (seconds). **Not woken by the view moving**: a camera following a hero
    // across a game seen from above moves every frame, and woke it to find
    // nothing again -- a quarter of a millisecond a frame, every frame, for
    // nothing. One frame a second is the most a rest costs.
    static constexpr core::u32 FramesBeforeRest = 10;
    static constexpr core::f64 ProbeSeconds = 1.0;
    // Fewer draws in view than this, and it does not test: hiding every one
    // of them would not pay for drawing the occluders.
    static constexpr core::u32 FewestDraws = 64;
    // Hiding one in this many of the draws in view pays, whatever the record
    // time says: what it saves is the GPU's.
    static constexpr core::u64 HiddenShareToPay = 10;
    // How the work is split across the job system: rows of the buffer a
    // band, and draws a run of sphere tests.
    static constexpr core::usize RowsPerBand = 16;
    static constexpr core::usize DrawsPerJob = 256;

    // One frame: draws the occluders, tests the rest, and clears
    // `inCameraFrustum` on what is hidden. `perDrawMs` is what a draw cost the
    // renderer last frame (0: not known), `nowSeconds` the wall clock.
    OcclusionStats cull(RenderWorld& world, core::f64 perDrawMs, core::f64 nowSeconds);
    // **Every frame tested** (`--occlusion-culling-always`): never resting,
    // whatever it saves. A picture gate's instrument.
    void setAlwaysTest(bool always) noexcept { alwaysTest_ = always; }

    // Only the buffer and the test, for a test: draw these boxes (each a
    // camera-relative transform of the unit cube) and grids of ground, then
    // ask about spheres.
    void drawOccluders(const core::Mat4& viewProjection, std::span<const core::Mat4> boxes,
                       std::span<const OccluderGrid> grids, core::f32 nearPlane);
    void drawBoxes(const core::Mat4& viewProjection, std::span<const core::Mat4> boxes, core::f32 nearPlane)
    {
        drawOccluders(viewProjection, boxes, {}, nearPlane);
    }
    [[nodiscard]] bool hides(const core::Mat4& viewProjection, core::Vec3 centre, core::f32 radius,
                             core::f32 nearPlane) const;
    // The farthest an occluder is over a pixel of the buffer; infinite where
    // none covers it.
    [[nodiscard]] core::f32 depthAt(core::u32 x, core::u32 y) const noexcept
    {
        return depth_.empty() ? std::numeric_limits<core::f32>::infinity()
                              : depth_[static_cast<core::usize>(y) * Width + x];
    }

private:
    struct Clip
    {
        core::f32 x;
        core::f32 y;
        core::f32 z;
        core::f32 w;
    };
    // A point in the buffer: x right, y down, in its pixels.
    struct ScreenPoint
    {
        core::f32 x;
        core::f32 y;
        core::f32 invW;
    };
    [[nodiscard]] static Clip toClip(const core::Mat4& m, core::Vec3 p) noexcept;
    [[nodiscard]] static ScreenPoint toScreen(const Clip& clip) noexcept;

    // **One flat face of an occluder on the screen**, three or four corners,
    // with the face drawn across each edge from corner `i` to `i + 1` -- its
    // index among the faces drawn with it, and which of its edges that is --
    // or -1 where the occluder ends there.
    struct Face
    {
        std::array<ScreenPoint, 4> v{};
        core::u32 count = 0;
        bool valid = false;
        std::array<core::f32, 4> a{};
        std::array<core::f32, 4> b{};
        std::array<core::f32, 4> c{};
        std::array<core::f32, 4> half{};
        std::array<core::i32, 4> across{-1, -1, -1, -1};
        std::array<core::u32, 4> acrossEdge{};
        core::f32 slopeX = 0.0f;
        core::f32 slopeY = 0.0f;
        core::f32 reach = 0.0f;
        core::f32 lowX = 0.0f;
        core::f32 highX = 0.0f;
        core::f32 lowY = 0.0f;
        core::f32 highY = 0.0f;
    };
    // Its edges, its plane and its box on the screen, from its corners;
    // false for one that covers nothing.
    [[nodiscard]] static bool setUpFace(Face& face) noexcept;
    // The farthest its plane is over the pixel centred at (x, y).
    [[nodiscard]] static core::f32 farthest(const Face& face, core::f32 x, core::f32 y) noexcept;

    void clear();
    // **The faces of one occluder.** A pixel wholly inside a face is as far as
    // the face is over it; one on an edge two faces share, wholly inside the
    // two, as far as the farther; one the outline or a corner crosses is
    // left: part of it may be what is behind. The nearest of all is kept.
    // Rows `bandLow` to `bandHigh` of them, both included.
    void drawFaces(std::span<const Face> faces, core::f32 bandLow, core::f32 bandHigh);
    // **A corner where drawn faces meet all round**: the faces and which of
    // their corners it is. The pixels near it no face takes alone are wholly
    // inside the faces round it out to the nearest edge that does not touch
    // it, and as far as the farthest of them there.
    struct Corner
    {
        std::array<core::i32, 6> faces{};
        std::array<core::u32, 6> slot{};
        core::u32 count = 0;
    };
    void drawCorners(std::span<const Face> faces, std::span<const Corner> corners, core::f32 bandLow,
                     core::f32 bandHigh);
    // The frame's faces and corners, a band of rows a job.
    void rasterize();
    void drawBox(const core::Mat4& viewProjection, const core::Mat4& box, core::f32 nearPlane);
    void drawGrid(const core::Mat4& viewProjection, const OccluderGrid& grid, core::f32 nearPlane);
    void buildPyramid();

    // The farthest depth an occluder has over each pixel; +inf where none.
    std::vector<core::f32> depth_;
    // Every occluder's faces and corners this frame, gathered before any is
    // drawn, and a grid's corners and its triangles' places among them.
    std::vector<Face> frameFaces_;
    std::vector<Corner> frameCorners_;
    std::vector<core::Vec3> gridPlaced_;
    std::vector<Clip> gridClip_;
    std::vector<core::i32> gridIndex_;
    // Each run of draws' candidates and hidden, in the run's order.
    std::vector<core::u32> runCounts_;
    // Each level the farthest of four below it, for the sphere's test.
    std::vector<std::vector<core::f32>> pyramid_;

    // The self-check: frames in a row it lost, whether it rests, and when it
    // last tried a frame while resting.
    core::u32 losingFrames_ = 0;
    bool resting_ = false;
    bool alwaysTest_ = false;
    core::f64 lastProbe_ = 0.0;
};

} // namespace engine::render
