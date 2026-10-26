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
// own time; when it saves less than it costs for a second it stops testing
// until the view has moved, and a frame that rests costs nothing.

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
    // Resting: it saved less than it cost, and waits for the view to move.
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
    // Frames in a row saving less than it costs before it rests, and how long
    // it rests unless the view moves (seconds), and how far it must move or
    // turn (metres, degrees) to be tried again sooner. **Each rest that
    // follows one that found nothing is twice as long and needs twice the
    // move**, up to `LongestRestRounds` doublings: a camera following a hero
    // across a game seen from above moves four metres every second, and woke
    // it to find nothing again.
    static constexpr core::u32 FramesBeforeRest = 30;
    static constexpr core::f64 RestSeconds = 3.0;
    static constexpr core::f32 MoveToWake = 4.0f;
    static constexpr core::f32 TurnToWake = 25.0f;
    static constexpr core::u32 LongestRestRounds = 4;
    // How the work is split across the job system: rows of the buffer a
    // band, and draws a run of sphere tests.
    static constexpr core::usize RowsPerBand = 16;
    static constexpr core::usize DrawsPerJob = 256;

    // One frame: draws the occluders, tests the rest, and clears
    // `inCameraFrustum` on what is hidden. `perDrawMs` is what a draw cost the
    // renderer last frame (0: not known), `nowSeconds` the wall clock.
    OcclusionStats cull(RenderWorld& world, core::f64 perDrawMs, core::f64 nowSeconds);

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

    // The self-check.
    core::u32 losingFrames_ = 0;
    // Rests in a row that ended with nothing found: each doubles the next.
    core::u32 restRounds_ = 0;
    bool resting_ = false;
    core::f64 restSince_ = 0.0;
    core::Vec3 restForward_{};
    core::DVec3 restOrigin_{};
};

} // namespace engine::render
