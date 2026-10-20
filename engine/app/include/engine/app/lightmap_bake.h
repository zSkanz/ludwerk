// **Light baked into what stands still** (ADR 0253): the lightmap of each
// baked part's surface and the mask of where the baked sun reaches it,
// worked out on the processor by the rays ADR 0203's volumes are.
//
// Nothing here knows of a scene, a part or the GPU: it is surfaces placed in
// the world with their lightmap coordinates, a rectangle of a page for each,
// and the trace and the light a volume is worked out against. The caller
// gathers the surfaces, keeps the pages and draws with them.
#pragma once

#include <span>
#include <vector>

#include "engine/app/indirect_light.h"
#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/types.h"

namespace engine::app {

// Texels a metre a baked surface asks for (`Lighting.LightmapDensity`): one
// six centimetres across.
inline constexpr core::f32 kLightmapDensity = 16.0f;
// A page's side, and how many a scene asks for at most.
inline constexpr core::u32 kLightmapPageSize = 1024;
inline constexpr core::u32 kLightmapMaxPages = 4;
// Texels between two rectangles, which the bake fills outward from each
// chart's edge: a bilinear tap at an edge reads only its own surface.
inline constexpr core::u32 kLightmapPadding = 2;
// How many times light bounces in a bake, after the first.
inline constexpr core::u32 kLightmapBounces = 2;

struct LightmapRect
{
    core::u32 page = 0;
    core::u32 x = 0;
    core::u32 y = 0;
    core::u32 width = 0;
    core::u32 height = 0;
};

struct LightmapLayout
{
    // One a request, in its order.
    std::vector<LightmapRect> rects;
    core::u32 pages = 0;
    // The density the rectangles were sized at: the one asked for, or less
    // where the requests asked for more than `maxPages` hold.
    core::f32 density = kLightmapDensity;
    bool lowered = false;
};

// **Each chart its rectangle of a page**: `sizes[i]` metres across and down,
// times the density and `scales[i]` (`BasePart.LightmapScale`), with
// `kLightmapPadding` round it, packed in rows tallest first. A density whose
// rectangles do not fit `maxPages` pages is lowered until they do, and the
// layout says so.
[[nodiscard]] LightmapLayout layOutLightmaps(std::span<const core::Vec2> sizes, std::span<const core::f32> scales,
                                             core::f32 density, core::u32 pageSize, core::u32 maxPages);

// The area of a mesh's triangles, stretched along its own axes, in square
// metres.
[[nodiscard]] core::f32 surfaceArea(std::span<const core::Vec3> positions, std::span<const core::u32> indices,
                                    core::Vec3 stretch) noexcept;

// **What each chart of a surface spans in the world**, across and down, in
// metres: one a cell of its grid (`columns` by `rows`, row by row), or the
// whole square as one where `columns` is nought. A mesh charted as one square
// is as wide as it is tall, the square root of its area; a primitive's face
// is as long each way as it is stretched, so a wall's broad side is given
// the density asked for and its edge no more than it needs.
[[nodiscard]] std::vector<core::Vec2> chartSizes(std::span<const core::Vec3> positions, std::span<const core::Vec2> uvs,
                                                 std::span<const core::u32> indices, core::Vec3 stretch,
                                                 core::u32 columns, core::u32 rows);

// One baked part's surface: its mesh in its own space, stretched along its
// own axes and then placed, and where each chart of its lightmap goes.
struct LightmapSurface
{
    core::CFrameD place;
    core::Vec3 stretch{1.0f, 1.0f, 1.0f};
    std::span<const core::Vec3> positions;
    // One a vertex, as `positions`; the face's own normal where empty.
    std::span<const core::Vec3> normals;
    // The lightmap coordinates, one a vertex, in the unit square.
    std::span<const core::Vec2> uvs;
    std::span<const core::u32> indices;
    // A primitive's grid of charts (`asset::Mesh::lightmapColumns`), each
    // within `margin` of its cell; nought for one chart, the whole square.
    core::u32 columns = 0;
    core::u32 rows = 0;
    core::f32 margin = 0.0f;
    // One a chart, row by row: one for a mesh charted as one square.
    std::vector<LightmapRect> rects;
};

// A page as the bake leaves it: `size` by `size` texels, rows from the top.
struct LightmapPage
{
    core::u32 size = 0;
    // Four numbers a texel: the light that reaches it from everything but
    // the sun's own disc, over pi -- what a matte white surface there gives
    // back, as a volume's light is -- and one where a surface is, nought in
    // the gaps between rectangles.
    std::vector<core::f32> light;
    // One a texel: how much of the sun's disc it sees past what stands
    // still, from the direction the bake was lit by.
    std::vector<core::f32> mask;
};

// **A scene's light baked into what stands still**, as the bake leaves it.
struct LightmapBakeResult
{
    // The parts baked, and the rectangles of a page their charts were given.
    struct Part
    {
        core::InstanceId id;
        // Where it stood and how large it was: a part found anywhere else
        // since is drawn unbaked until the next bake.
        core::CFrameD place;
        core::Vec3 size{1.0f, 1.0f, 1.0f};
        // The grid the charts stand in, nought for one chart.
        core::u32 columns = 0;
        core::u32 rows = 0;
        std::vector<LightmapRect> rects;
    };
    std::vector<Part> parts;
    // Parts asked for that could not be: a mesh with no lightmap coordinates,
    // which are lit as a moving thing is until it is imported with them.
    std::vector<core::InstanceId> unmapped;
    std::vector<LightmapPage> pages;
    // Towards the sun the mask was made for.
    core::Vec3 sunDirection{0.0f, 1.0f, 0.0f};
    // The density the rectangles were given, and whether it is less than the
    // scene asked for.
    core::f32 density = kLightmapDensity;
    bool lowered = false;
    core::usize texels = 0;
    core::u32 inside = 0;
};

// **One bake, worked out a piece at a time.** `begin` places every texel of
// every surface; `step` does as much as fits a budget and says when it is
// whole: the mask, then a pass of the light for the first bounce and one for
// each after it, then a filter that keeps edges and the charts' borders
// filled outward. Lit by the light `step` is first handed, and no other.
class LightmapBake
{
public:
    void begin(std::span<const LightmapSurface> surfaces, core::u32 pageSize, core::u32 pages, core::u32 bounces);
    // True when whole. `budgetNs` of nought does all of it.
    bool step(const BakeTrace& trace, const BakeLight& light, core::u64 budgetNs);

    [[nodiscard]] bool whole() const noexcept { return m_stage == Stage::Done; }
    [[nodiscard]] core::f32 progress() const noexcept;
    [[nodiscard]] std::span<const LightmapPage> pages() const noexcept { return m_pages; }
    // Towards the sun the mask was made for.
    [[nodiscard]] core::Vec3 sunDirection() const noexcept { return m_lit.sunDirection; }
    // How many texels a surface covers, and how many of them were inside
    // something and took their neighbours' light.
    [[nodiscard]] core::usize texelCount() const noexcept { return m_texelCount; }
    [[nodiscard]] core::u32 insideCount() const noexcept { return m_insideCount; }

private:
    enum class Stage : core::u8
    {
        Idle,
        Mask,
        Gather,
        Finish,
        Done,
    };
    struct Texel
    {
        core::DVec3 place;
        core::Vec3 normal{0.0f, 1.0f, 0.0f};
        // Across the texel, in metres, along two directions in its surface.
        core::Vec3 across{0.0f, 0.0f, 0.0f};
        core::Vec3 down{0.0f, 0.0f, 0.0f};
        core::u32 page = 0;
        core::u32 x = 0;
        core::u32 y = 0;
        core::u32 surface = 0;
    };

    void rasterize(const LightmapSurface& surface, core::u32 index);
    void maskTexel(core::usize texel, const BakeTrace& trace);
    void gatherTexel(core::usize texel, const BakeTrace& trace);
    void passEnd();
    void finish();
    [[nodiscard]] core::Color3 around(const core::DVec3& place, core::Vec3 normal) const noexcept;

    Stage m_stage = Stage::Idle;
    core::u32 m_bounces = 1;
    core::u32 m_pass = 0;
    core::usize m_cursor = 0;
    core::u32 m_pageSize = 0;
    BakeLight m_lit;
    bool m_lightHeld = false;
    core::DVec3 m_origin;
    std::vector<Texel> m_texels;
    core::usize m_texelCount = 0;
    // Per page, the surface each texel's rectangle is (one more than its
    // index; nought in the gaps).
    std::vector<std::vector<core::u32>> m_owner;
    std::vector<core::Color3> m_indirect;
    std::vector<core::Color3> m_previous;
    std::vector<core::u8> m_inside;
    core::u32 m_insideCount = 0;
    std::vector<core::f32> m_mask;
    // The light of the pass before, by a cell of the world and the way the
    // surface there faces, sorted by key: what a ray that meets a baked
    // surface finds has bounced onto it.
    struct Cell
    {
        core::u64 key = 0;
        core::Color3 light{0.0f, 0.0f, 0.0f};
        core::u32 count = 0;
    };
    std::vector<Cell> m_cells;
    std::vector<LightmapPage> m_pages;
};

} // namespace engine::app
