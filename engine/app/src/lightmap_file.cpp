#include "engine/app/lightmap_file.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>

#include "engine/core/profile.h"
#include "engine/render/environment.h"
#include "engine/scene/world.h"

#if ENG_DEBUG_UI
#include "engine/assetc/compiler.h"
#endif

namespace engine::app {
namespace {

using core::f32;
using core::f64;
using core::u16;
using core::u32;
using core::u64;
using core::usize;
using core::Vec3;

constexpr char Magic[8] = {'E', 'N', 'G', 'L', 'M', 'A', 'P', '1'};
// 1 kept the pages as half floats; 2 says how it keeps them
// (`PageEncoding`).
constexpr u32 Version = 2;
// What a file is ever asked to hold: no more pages, parts or charts than a
// bake makes, and no grid larger than its side.
constexpr u32 MostPages = 64;
constexpr u32 MostParts = 1u << 20;
constexpr u32 MostCharts = 64;
constexpr u32 MostPageSide = 8192;
constexpr u32 MostPath = 4096;
// The most bytes one page's picture may be: its half floats.
constexpr u64 MostPictureBytes = 8ull * MostPageSide * MostPageSide;

enum class PageEncoding : u32
{
    Halves = 0,
    UastcHdr = 1,
};

template <class T>
void put(std::vector<std::byte>& out, const T& value)
{
    const usize at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

struct Reader
{
    std::span<const std::byte> bytes;
    usize at = 0;
    bool ok = true;

    template <class T>
    T take()
    {
        T value{};
        if (!ok || bytes.size() - at < sizeof(T)) {
            ok = false;
            return value;
        }
        std::memcpy(&value, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return value;
    }
    f32 number()
    {
        const f32 value = take<f32>();
        ok = ok && std::isfinite(value);
        return value;
    }
    f64 wide()
    {
        const f64 value = take<f64>();
        ok = ok && std::isfinite(value);
        return value;
    }
};

// A part's names from the workspace down, `/` between: where it stands in the
// scene, the same each time the scene is opened.
[[nodiscard]] std::string pathOf(const scene::World& world, core::InstanceId id)
{
    std::vector<std::string_view> names;
    for (core::InstanceId at = id; at.valid(); at = world.parentOf(at))
        names.push_back(world.atoms().text(world.name(at)));
    std::string path;
    for (auto name = names.rbegin(); name != names.rend(); ++name) {
        if (!path.empty())
            path += '/';
        path += *name;
    }
    return path;
}

// Where a part stands now is where the file says, to a millimetre and a
// hair of a turn: a scene written down and read back is not bit for bit.
[[nodiscard]] bool standsAt(const scene::PartComponent& part, const KeptLightmapPart& kept)
{
    const core::DVec3 apart = part.cframe.position - kept.place.position;
    if (std::abs(apart.x) > 1.0e-3 || std::abs(apart.y) > 1.0e-3 || std::abs(apart.z) > 1.0e-3)
        return false;
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 3; ++column) {
            if (std::abs(part.cframe.rotation.m[row][column] - kept.place.rotation.m[row][column]) > 1.0e-4f)
                return false;
        }
    }
    const Vec3 grown = part.size - kept.size;
    return std::abs(grown.x) <= 1.0e-3f && std::abs(grown.y) <= 1.0e-3f && std::abs(grown.z) <= 1.0e-3f;
}

} // namespace

std::vector<std::byte> writeLightmap(const KeptLightmap& kept)
{
    std::vector<std::byte> out;
    out.reserve(kept.texels.size() * sizeof(u16) + 4096);
    out.resize(sizeof(Magic));
    std::memcpy(out.data(), Magic, sizeof(Magic));
    put(out, Version);
    put(out, kept.sunDirection.x);
    put(out, kept.sunDirection.y);
    put(out, kept.sunDirection.z);
    put(out, kept.density);
    put(out, kept.pageSize);
    put(out, kept.pageCount);
    put(out, static_cast<u32>(kept.parts.size()));
    for (const KeptLightmapPart& part : kept.parts) {
        put(out, static_cast<u32>(part.path.size()));
        const usize at = out.size();
        out.resize(at + part.path.size());
        std::memcpy(out.data() + at, part.path.data(), part.path.size());
        put(out, part.place.position.x);
        put(out, part.place.position.y);
        put(out, part.place.position.z);
        for (u32 row = 0; row < 3; ++row) {
            for (u32 column = 0; column < 3; ++column)
                put(out, part.place.rotation.m[row][column]);
        }
        put(out, part.size.x);
        put(out, part.size.y);
        put(out, part.size.z);
        put(out, part.columns);
        put(out, part.rows);
        put(out, static_cast<u32>(part.rects.size()));
        for (const LightmapRect& rect : part.rects) {
            put(out, rect.page);
            put(out, rect.x);
            put(out, rect.y);
            put(out, rect.width);
            put(out, rect.height);
        }
    }
    const LightmapSunGrid& grid = kept.sunGrid;
    put(out, grid.low.x);
    put(out, grid.low.y);
    put(out, grid.low.z);
    put(out, grid.spacing);
    for (const u32 count : grid.counts)
        put(out, count);
    for (const f32 seen : grid.sun)
        put(out, static_cast<std::uint8_t>(std::lround(std::clamp(seen, 0.0f, 1.0f) * 255.0f)));
    const bool pictures =
        !kept.light.empty() && kept.light.size() == kept.pageCount && kept.masks.size() == kept.pageCount;
    put(out, static_cast<u32>(pictures ? PageEncoding::UastcHdr : PageEncoding::Halves));
    const auto append = [&out](std::span<const std::byte> bytes) {
        const usize at = out.size();
        out.resize(at + bytes.size());
        if (!bytes.empty())
            std::memcpy(out.data() + at, bytes.data(), bytes.size());
    };
    if (pictures) {
        for (u32 page = 0; page < kept.pageCount; ++page) {
            for (const std::vector<std::byte>* picture : {&kept.light[page], &kept.masks[page]}) {
                put(out, static_cast<u64>(picture->size()));
                append(*picture);
            }
        }
        return out;
    }
    append(std::as_bytes(std::span(kept.texels)));
    return out;
}

bool readLightmap(std::span<const std::byte> bytes, KeptLightmap& out)
{
    out = KeptLightmap{};
    if (bytes.size() < sizeof(Magic) || std::memcmp(bytes.data(), Magic, sizeof(Magic)) != 0)
        return false;
    Reader in{bytes, sizeof(Magic)};
    const u32 version = in.take<u32>();
    if (version != 1 && version != Version)
        return false;
    out.sunDirection = Vec3{in.number(), in.number(), in.number()};
    out.density = in.number();
    out.pageSize = in.take<u32>();
    out.pageCount = in.take<u32>();
    const u32 parts = in.take<u32>();
    if (!in.ok || out.pageSize == 0 || out.pageSize > MostPageSide || out.pageCount > MostPages || parts > MostParts)
        return false;
    out.parts.resize(parts);
    for (KeptLightmapPart& part : out.parts) {
        const u32 length = in.take<u32>();
        if (!in.ok || length > MostPath || bytes.size() - in.at < length)
            return false;
        part.path.assign(reinterpret_cast<const char*>(bytes.data() + in.at), length);
        in.at += length;
        part.place.position = core::DVec3{in.wide(), in.wide(), in.wide()};
        for (u32 row = 0; row < 3; ++row) {
            for (u32 column = 0; column < 3; ++column)
                part.place.rotation.m[row][column] = in.number();
        }
        part.size = Vec3{in.number(), in.number(), in.number()};
        part.columns = in.take<u32>();
        part.rows = in.take<u32>();
        const u32 charts = in.take<u32>();
        if (!in.ok || charts > MostCharts || (part.columns == 0 ? 1u : part.columns * part.rows) != charts)
            return false;
        part.rects.resize(charts);
        for (LightmapRect& rect : part.rects) {
            rect.page = in.take<u32>();
            rect.x = in.take<u32>();
            rect.y = in.take<u32>();
            rect.width = in.take<u32>();
            rect.height = in.take<u32>();
            if (!in.ok || rect.page >= out.pageCount || rect.x + rect.width > out.pageSize ||
                rect.y + rect.height > out.pageSize)
                return false;
        }
    }
    LightmapSunGrid& grid = out.sunGrid;
    grid.low = core::DVec3{in.wide(), in.wide(), in.wide()};
    grid.spacing = in.number();
    for (u32& count : grid.counts) {
        count = in.take<u32>();
        if (count > kSunGridSide)
            return false;
    }
    const usize points = static_cast<usize>(grid.counts[0]) * grid.counts[1] * grid.counts[2];
    if (!in.ok || bytes.size() - in.at < points)
        return false;
    grid.sun.resize(points);
    for (f32& seen : grid.sun)
        seen = static_cast<f32>(in.take<std::uint8_t>()) / 255.0f;
    const u32 encoding = version == 1 ? static_cast<u32>(PageEncoding::Halves) : in.take<u32>();
    if (!in.ok)
        return false;
    if (encoding == static_cast<u32>(PageEncoding::UastcHdr)) {
        out.light.resize(out.pageCount);
        out.masks.resize(out.pageCount);
        for (u32 page = 0; page < out.pageCount; ++page) {
            for (std::vector<std::byte>* picture : {&out.light[page], &out.masks[page]}) {
                const u64 size = in.take<u64>();
                if (!in.ok || size == 0 || size > MostPictureBytes || bytes.size() - in.at < size)
                    return false;
                picture->assign(bytes.begin() + static_cast<std::ptrdiff_t>(in.at),
                                bytes.begin() + static_cast<std::ptrdiff_t>(in.at + size));
                in.at += static_cast<usize>(size);
            }
        }
        return in.at == bytes.size();
    }
    if (encoding != static_cast<u32>(PageEncoding::Halves))
        return false;
    const usize texels = static_cast<usize>(out.pageSize) * out.pageSize * 4 * out.pageCount;
    if (bytes.size() - in.at != texels * sizeof(u16))
        return false;
    out.texels.resize(texels);
    std::memcpy(out.texels.data(), bytes.data() + in.at, texels * sizeof(u16));
    return true;
}

bool compressLightmap(KeptLightmap& kept)
{
#if ENG_DEBUG_UI
    ENG_PROFILE_SCOPE("lightmap.compress");
    const usize perPage = static_cast<usize>(kept.pageSize) * kept.pageSize;
    // Blocks are four texels a side.
    if (kept.pageCount == 0 || kept.pageSize % 4 != 0 || kept.texels.size() != perPage * 4 * kept.pageCount)
        return false;
    const u32 threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::vector<std::byte>> light(kept.pageCount);
    std::vector<std::vector<std::byte>> masks(kept.pageCount);
    std::vector<f32> colour(perPage * 3);
    std::vector<f32> grey(perPage * 3);
    for (u32 page = 0; page < kept.pageCount; ++page) {
        const u16* from = &kept.texels[page * perPage * 4];
        for (usize texel = 0; texel < perPage; ++texel) {
            for (usize channel = 0; channel < 3; ++channel)
                colour[texel * 3 + channel] = std::max(render::halfToFloat(from[texel * 4 + channel]), 0.0f);
            const f32 mask = std::clamp(render::halfToFloat(from[texel * 4 + 3]), 0.0f, 1.0f);
            grey[texel * 3] = mask;
            grey[texel * 3 + 1] = mask;
            grey[texel * 3 + 2] = mask;
        }
        if (assetc::encodeHdrPicture(colour, kept.pageSize, kept.pageSize, threads, light[page]).has_value() ||
            assetc::encodeHdrPicture(grey, kept.pageSize, kept.pageSize, threads, masks[page]).has_value())
            return false;
    }
    kept.light = std::move(light);
    kept.masks = std::move(masks);
    kept.texels.clear();
    kept.texels.shrink_to_fit();
    return true;
#else
    (void)kept;
    return false;
#endif
}

KeptLightmap keptLightmapOf(const scene::World& world, const LightmapBakeResult& baked)
{
    KeptLightmap kept;
    kept.sunDirection = baked.sunDirection;
    kept.density = baked.density;
    kept.sunGrid = baked.sunGrid;
    if (baked.pages.empty())
        return kept;
    kept.pageSize = baked.pages.front().size;
    kept.pageCount = static_cast<u32>(baked.pages.size());
    const usize perPage = static_cast<usize>(kept.pageSize) * kept.pageSize;
    kept.texels.assign(perPage * 4 * kept.pageCount, 0);
    for (usize page = 0; page < baked.pages.size(); ++page) {
        const LightmapPage& from = baked.pages[page];
        if (from.light.size() != perPage * 4 || from.mask.size() != perPage)
            continue;
        u16* into = &kept.texels[page * perPage * 4];
        for (usize texel = 0; texel < perPage; ++texel) {
            into[texel * 4] = render::floatToHalf(from.light[texel * 4]);
            into[texel * 4 + 1] = render::floatToHalf(from.light[texel * 4 + 1]);
            into[texel * 4 + 2] = render::floatToHalf(from.light[texel * 4 + 2]);
            // Where the bake left a gap the sun is the map's to say.
            into[texel * 4 + 3] = render::floatToHalf(from.light[texel * 4 + 3] > 0.0f ? from.mask[texel] : 1.0f);
        }
    }
    for (const LightmapBakeResult::Part& part : baked.parts) {
        kept.parts.push_back(KeptLightmapPart{
            .path = pathOf(world, part.id),
            .place = part.place,
            .size = part.size,
            .columns = part.columns,
            .rows = part.rows,
            .rects = part.rects,
        });
    }
    return kept;
}

LightmapBakeResult lightmapFromKept(const scene::World& world, const KeptLightmap& kept, u32& missing)
{
    LightmapBakeResult baked;
    missing = 0;
    baked.sunDirection = kept.sunDirection;
    baked.density = kept.density;
    baked.sunGrid = kept.sunGrid;
    const usize perPage = static_cast<usize>(kept.pageSize) * kept.pageSize;
    if (!kept.light.empty()) {
        // As the file keeps them: transcoded where they are uploaded.
        if (kept.light.size() != kept.pageCount || kept.masks.size() != kept.pageCount)
            return baked;
        baked.encodedSize = kept.pageSize;
        baked.encodedLight = kept.light;
        baked.encodedMasks = kept.masks;
    }
    else if (kept.texels.size() != perPage * 4 * kept.pageCount) {
        return baked;
    }
    for (u32 page = 0; page < kept.pageCount && kept.light.empty(); ++page) {
        LightmapPage out;
        out.size = kept.pageSize;
        out.light.resize(perPage * 4);
        out.mask.resize(perPage);
        const u16* from = &kept.texels[page * perPage * 4];
        for (usize texel = 0; texel < perPage; ++texel) {
            out.light[texel * 4] = render::halfToFloat(from[texel * 4]);
            out.light[texel * 4 + 1] = render::halfToFloat(from[texel * 4 + 1]);
            out.light[texel * 4 + 2] = render::halfToFloat(from[texel * 4 + 2]);
            // The mask is what the alpha keeps; every texel of a kept page is
            // one the scene reads, gap or not.
            out.light[texel * 4 + 3] = 1.0f;
            out.mask[texel] = render::halfToFloat(from[texel * 4 + 3]);
        }
        baked.pages.push_back(std::move(out));
    }
    // **Each kept part found by its names**, and then by where it stands:
    // two parts may share a name, and not a place.
    std::multimap<std::string, core::InstanceId> byPath;
    world.parts().forEach([&](core::InstanceId id, const scene::PartComponent& part) {
        if (part.bakedLight)
            byPath.emplace(pathOf(world, id), id);
    });
    for (const KeptLightmapPart& part : kept.parts) {
        core::InstanceId found;
        const auto [first, last] = byPath.equal_range(part.path);
        for (auto candidate = first; candidate != last; ++candidate) {
            const scene::PartComponent* now = world.parts().find(candidate->second);
            if (now != nullptr && standsAt(*now, part)) {
                found = candidate->second;
                break;
            }
        }
        const scene::PartComponent* now = found.valid() ? world.parts().find(found) : nullptr;
        if (now == nullptr) {
            ++missing;
            continue;
        }
        // Where it stands now: within a millimetre of the file, and what a
        // move from now on is told apart from.
        baked.parts.push_back(LightmapBakeResult::Part{
            .id = found,
            .place = now->cframe,
            .size = now->size,
            .columns = part.columns,
            .rows = part.rows,
            .rects = part.rects,
        });
    }
    return baked;
}

} // namespace engine::app
