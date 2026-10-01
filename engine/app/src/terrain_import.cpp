#include "engine/app/terrain_import.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <system_error>
#include <utility>
#include <vector>

#include "engine/asset/image.h"
#include "engine/platform/file.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"
#include "engine/script/bytecode.h"

namespace engine::app {
namespace {

using core::i32;
using core::u32;
using core::usize;

constexpr float NoGround = std::numeric_limits<float>::quiet_NaN();

// --- A heightmap ---------------------------------------------------------------

// `<anything>_x<column>_y<row>`, the stem of a piece of a tiled heightmap.
[[nodiscard]] std::optional<std::pair<u32, u32>> pieceOf(const std::string& stem)
{
    const auto numberAfter = [&](std::string_view mark, usize from, usize& end) -> std::optional<u32> {
        const usize at = stem.rfind(mark, from);
        if (at == std::string::npos)
            return std::nullopt;
        usize digit = at + mark.size();
        u32 value = 0;
        const usize first = digit;
        while (digit < stem.size() && stem[digit] >= '0' && stem[digit] <= '9' && value < 100000u) {
            value = value * 10u + static_cast<u32>(stem[digit] - '0');
            ++digit;
        }
        if (digit == first)
            return std::nullopt;
        end = digit;
        return value;
    };
    usize afterY = 0;
    const std::optional<u32> y = numberAfter("_y", std::string::npos, afterY);
    if (!y.has_value() || afterY != stem.size())
        return std::nullopt;
    const usize yAt = stem.rfind("_y");
    usize afterX = 0;
    const std::optional<u32> x = yAt > 0 ? numberAfter("_x", yAt - 1, afterX) : std::nullopt;
    if (!x.has_value() || afterX != yAt)
        return std::nullopt;
    return std::pair{*x, *y};
}

[[nodiscard]] bool decodeFile(const std::filesystem::path& file, asset::HeightImage& image, std::string& error)
{
    std::vector<std::byte> bytes;
    if (!platform::readFile(file, bytes)) {
        error = file.filename().string();
        return false;
    }
    if (const std::optional<core::EngineError> failed = asset::decodeHeightmap(bytes, file.filename().string(), image);
        failed.has_value()) {
        error = file.filename().string() + ": " + failed->message;
        return false;
    }
    return true;
}

// What comes before `_x<column>_y<row>` in a piece's name: its set's name.
[[nodiscard]] std::string setOf(const std::string& stem)
{
    const usize yAt = stem.rfind("_y");
    const usize xAt = yAt == std::string::npos || yAt == 0 ? std::string::npos : stem.rfind("_x", yAt - 1);
    return xAt == std::string::npos ? std::string{} : stem.substr(0, xAt);
}

// The pieces of a tiled heightmap, by (column, row): every piece in `folder`,
// or -- given one of them -- the pieces of its set.
[[nodiscard]] std::map<std::pair<u32, u32>, std::filesystem::path> piecesIn(const std::filesystem::path& folder,
                                                                            const std::filesystem::path* one = nullptr)
{
    std::map<std::pair<u32, u32>, std::filesystem::path> pieces;
    std::error_code ignored;
    // Sorted by name before they are taken, so two files that name one piece
    // are resolved the same way on every machine.
    std::vector<std::filesystem::path> files;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(folder, ignored)) {
        if (entry.is_regular_file(ignored))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files) {
        const std::string stem = file.stem().string();
        if (one != nullptr && (file.extension() != one->extension() || setOf(stem) != setOf(one->stem().string())))
            continue;
        if (const std::optional<std::pair<u32, u32>> piece = pieceOf(stem); piece.has_value())
            pieces.emplace(*piece, file);
    }
    return pieces;
}

class ImageSource final : public HeightmapSource
{
public:
    // How many decoded pieces are kept: the four a bilinear sample at a
    // corner reads, and the row of them a tile along an edge crosses.
    static constexpr usize PiecesKept = 6;

    bool open(const std::filesystem::path& fileOrFolder, std::string& error)
    {
        std::error_code ignored;
        const bool folder = std::filesystem::is_directory(fileOrFolder, ignored);
        const bool ofSet = !folder && pieceOf(fileOrFolder.stem().string()).has_value();
        if (!folder && !ofSet) {
            Piece whole;
            if (!decodeFile(fileOrFolder, whole.image, error))
                return false;
            m_pieceWidth = whole.image.width;
            m_pieceHeight = whole.image.height;
            m_across = 1;
            m_down = 1;
            whole.at = {0, 0};
            m_kept.push_back(std::move(whole));
            m_single = true;
            return true;
        }
        m_files = ofSet ? piecesIn(fileOrFolder.parent_path(), &fileOrFolder) : piecesIn(fileOrFolder);
        if (m_files.empty()) {
            error = fileOrFolder.filename().string();
            return false;
        }
        asset::HeightImage first;
        if (!decodeFile(m_files.begin()->second, first, error))
            return false;
        m_pieceWidth = first.width;
        m_pieceHeight = first.height;
        for (const auto& piece : m_files) {
            m_across = std::max(m_across, piece.first.first + 1u);
            m_down = std::max(m_down, piece.first.second + 1u);
        }
        return true;
    }

    void map(const TerrainImportPlan& plan, float low, float high) noexcept override
    {
        m_plan = plan;
        m_low = low;
        m_high = high;
    }

    [[nodiscard]] HeightmapSize size() const noexcept override
    {
        return HeightmapSize{m_pieceWidth * m_across, m_pieceHeight * m_down};
    }

    bool read(i32 x, i32 z, u32 width, u32 depth, std::span<float> out, std::string& error) override
    {
        const HeightmapSize whole = size();
        // Corner to corner, `asset::resampleHeights`'s mapping: the first and
        // last columns land on the first and last pixels.
        const auto scale = [](u32 target, u32 source) {
            return target > 1 ? static_cast<double>(source - 1u) / static_cast<double>(target - 1u) : 0.0;
        };
        const double stepX = scale(m_plan.columns, whole.width);
        const double stepY = scale(m_plan.rows, whole.height);
        for (u32 row = 0; row < depth; ++row) {
            const i32 planRow =
                std::clamp(z + static_cast<i32>(row) - m_plan.firstZ, 0, static_cast<i32>(m_plan.rows) - 1);
            const double v = static_cast<double>(planRow) * stepY;
            const auto y0 = std::min(static_cast<u32>(v), whole.height - 1u);
            const u32 y1 = std::min(y0 + 1u, whole.height - 1u);
            const auto fy = static_cast<float>(v - static_cast<double>(y0));
            for (u32 column = 0; column < width; ++column) {
                const i32 planColumn =
                    std::clamp(x + static_cast<i32>(column) - m_plan.firstX, 0, static_cast<i32>(m_plan.columns) - 1);
                const double u = static_cast<double>(planColumn) * stepX;
                const auto x0 = std::min(static_cast<u32>(u), whole.width - 1u);
                const u32 x1 = std::min(x0 + 1u, whole.width - 1u);
                const auto fx = static_cast<float>(u - static_cast<double>(x0));
                float a = 0.0f;
                float b = 0.0f;
                float c = 0.0f;
                float d = 0.0f;
                if (!pixel(x0, y0, a, error) || !pixel(x1, y0, b, error) || !pixel(x0, y1, c, error) ||
                    !pixel(x1, y1, d, error))
                    return false;
                const float top = a + (b - a) * fx;
                const float bottom = c + (d - c) * fx;
                const float unit = top + (bottom - top) * fy;
                // A missing piece is NaN, and so is everything it touches.
                out[static_cast<usize>(row) * width + column] = m_low + unit * (m_high - m_low);
            }
        }
        return true;
    }

private:
    struct Piece
    {
        std::pair<u32, u32> at{};
        asset::HeightImage image;
        bool missing = false;
    };

    // One pixel of the whole image. False when its piece will not decode, or
    // is not the size the others are.
    bool pixel(u32 x, u32 y, float& value, std::string& error)
    {
        const std::pair<u32, u32> at{x / m_pieceWidth, y / m_pieceHeight};
        if (m_last == nullptr || m_last->at != at) {
            m_last = pieceAt(at, error);
            if (m_last == nullptr)
                return false;
        }
        value = m_last->missing
                    ? NoGround
                    : m_last->image.samples[static_cast<usize>(y % m_pieceHeight) * m_pieceWidth + (x % m_pieceWidth)];
        return true;
    }

    Piece* pieceAt(std::pair<u32, u32> at, std::string& error)
    {
        for (usize index = 0; index < m_kept.size(); ++index) {
            if (m_kept[index].at != at)
                continue;
            // Newest last.
            std::rotate(m_kept.begin() + static_cast<std::ptrdiff_t>(index),
                        m_kept.begin() + static_cast<std::ptrdiff_t>(index) + 1, m_kept.end());
            return &m_kept.back();
        }
        if (m_single)
            return nullptr;
        Piece piece;
        piece.at = at;
        if (const auto file = m_files.find(at); file == m_files.end()) {
            piece.missing = true;
        }
        else {
            if (!decodeFile(file->second, piece.image, error))
                return nullptr;
            if (piece.image.width != m_pieceWidth || piece.image.height != m_pieceHeight) {
                error = file->second.filename().string();
                return nullptr;
            }
        }
        if (m_kept.size() >= PiecesKept)
            m_kept.erase(m_kept.begin());
        m_kept.push_back(std::move(piece));
        return &m_kept.back();
    }

    std::map<std::pair<u32, u32>, std::filesystem::path> m_files;
    std::vector<Piece> m_kept;
    Piece* m_last = nullptr;
    bool m_single = false;
    u32 m_pieceWidth = 0;
    u32 m_pieceHeight = 0;
    u32 m_across = 0;
    u32 m_down = 0;
    TerrainImportPlan m_plan;
    float m_low = 0.0f;
    float m_high = 1.0f;
};

// --- Hills ------------------------------------------------------------------------

class HillSource final : public HeightSource
{
public:
    HillSource(const asset::FieldSettings& settings, const asset::HillSettings& hills)
        : m_field(settings), m_hills(hills)
    {}

    bool read(i32 x, i32 z, u32 width, u32 depth, std::span<float> out, std::string&) override
    {
        const std::vector<float> heights = asset::hillHeights(m_field, x, z, width, depth, m_hills);
        std::copy(heights.begin(), heights.end(), out.begin());
        return true;
    }

private:
    // For its settings alone: where a voxel column's middle is.
    asset::TerrainField m_field;
    asset::HillSettings m_hills;
};

// --- A Luau function ------------------------------------------------------------

class FunctionSource final : public HeightSource
{
public:
    FunctionSource(lua_State* L, core::DVec3 origin, float voxel) : m_main(L), m_origin(origin), m_voxel(voxel) {}

    ~FunctionSource() override
    {
        if (m_thread == nullptr)
            return;
        if (m_function != LUA_NOREF)
            lua_unref(m_thread, m_function);
        lua_unref(m_main, m_threadRef);
    }

    bool load(std::string_view source, std::string_view chunkName, std::string& error)
    {
        std::string bytecode;
        if (const std::optional<core::EngineError> failed = script::bytecodeOf(source, chunkName, bytecode);
            failed.has_value()) {
            error = failed->message;
            return false;
        }
        m_thread = lua_newthread(m_main);
        m_threadRef = lua_ref(m_main, -1);
        lua_pop(m_main, 1);
        luaL_sandboxthread(m_thread);
        const std::string name(chunkName);
        if (luau_load(m_thread, name.c_str(), bytecode.data(), bytecode.size(), 0) != 0 ||
            lua_pcall(m_thread, 0, 1, 0) != 0) {
            const char* message = lua_tostring(m_thread, -1);
            error = message != nullptr ? message : name;
            lua_settop(m_thread, 0);
            return false;
        }
        if (!lua_isfunction(m_thread, -1)) {
            lua_settop(m_thread, 0);
            error = name;
            return false;
        }
        m_function = lua_ref(m_thread, -1);
        lua_settop(m_thread, 0);
        return true;
    }

    bool read(i32 x, i32 z, u32 width, u32 depth, std::span<float> out, std::string& error) override
    {
        const double voxel = static_cast<double>(m_voxel);
        for (u32 row = 0; row < depth; ++row) {
            const double worldZ = m_origin.z + (static_cast<double>(z + static_cast<i32>(row)) + 0.5) * voxel;
            for (u32 column = 0; column < width; ++column) {
                const double worldX = m_origin.x + (static_cast<double>(x + static_cast<i32>(column)) + 0.5) * voxel;
                lua_getref(m_thread, m_function);
                lua_pushnumber(m_thread, worldX);
                lua_pushnumber(m_thread, worldZ);
                if (lua_pcall(m_thread, 2, 1, 0) != 0 || lua_type(m_thread, -1) != LUA_TNUMBER) {
                    const char* message = lua_type(m_thread, -1) == LUA_TSTRING ? lua_tostring(m_thread, -1) : nullptr;
                    error = message != nullptr ? message : std::string{};
                    lua_settop(m_thread, 0);
                    return false;
                }
                // The world's height in, the field's out. A NaN is no ground.
                out[static_cast<usize>(row) * width + column] =
                    static_cast<float>(lua_tonumber(m_thread, -1) - m_origin.y);
                lua_pop(m_thread, 1);
            }
        }
        return true;
    }

private:
    lua_State* m_main = nullptr;
    lua_State* m_thread = nullptr;
    int m_threadRef = LUA_NOREF;
    int m_function = LUA_NOREF;
    core::DVec3 m_origin;
    float m_voxel = 1.0f;
};

} // namespace

std::unique_ptr<HeightmapSource> openHeightmap(const std::filesystem::path& fileOrFolder, std::string& error)
{
    auto source = std::make_unique<ImageSource>();
    if (!source->open(fileOrFolder, error))
        return nullptr;
    return source;
}

bool isHeightmapPiece(const std::filesystem::path& file)
{
    return !file.empty() && pieceOf(file.stem().string()).has_value();
}

std::unique_ptr<HeightSource> hillSource(const asset::FieldSettings& settings, const asset::HillSettings& hills)
{
    return std::make_unique<HillSource>(settings, hills);
}

std::unique_ptr<HeightSource> functionSource(lua_State* L, std::string_view source, std::string_view chunkName,
                                             core::DVec3 origin, float voxel, std::string& error)
{
    auto made = std::make_unique<FunctionSource>(L, origin, voxel);
    if (!made->load(source, chunkName, error))
        return nullptr;
    return made;
}

TerrainImport::TerrainImport(std::unique_ptr<HeightSource> source, const TerrainImportPlan& plan)
    : m_source(std::move(source)), m_plan(plan)
{
    // On the chunk grid, whatever was asked.
    constexpr auto edge = static_cast<u32>(asset::ChunkEdge);
    m_plan.tile = std::max(edge, m_plan.tile / edge * edge);
    if (m_plan.columns == 0 || m_plan.rows == 0)
        return;
    const auto tile = static_cast<i32>(m_plan.tile);
    m_tileX0 = asset::floorDiv(m_plan.firstX, tile);
    m_tileZ0 = asset::floorDiv(m_plan.firstZ, tile);
    m_tilesX =
        static_cast<u32>(asset::floorDiv(m_plan.firstX + static_cast<i32>(m_plan.columns) - 1, tile) - m_tileX0 + 1);
    m_tilesZ =
        static_cast<u32>(asset::floorDiv(m_plan.firstZ + static_cast<i32>(m_plan.rows) - 1, tile) - m_tileZ0 + 1);
}

core::DVec3 TerrainImport::cursor(const scene::TerrainComponent& terrain) const noexcept
{
    const u32 at = std::min(m_next, tiles() > 0 ? tiles() - 1u : 0u);
    const double tile = static_cast<double>(m_plan.tile) * static_cast<double>(terrain.field.settings().voxelSize);
    const double x =
        (static_cast<double>(m_tileX0) + static_cast<double>(m_tilesX > 0 ? at % m_tilesX : 0u) + 0.5) * tile;
    const double z =
        (static_cast<double>(m_tileZ0) + static_cast<double>(m_tilesX > 0 ? at / m_tilesX : 0u) + 0.5) * tile;
    return core::DVec3{terrain.origin.x + x, terrain.origin.y, terrain.origin.z + z};
}

TerrainImport::Step TerrainImport::step(scene::World& world, scene::TerrainComponent& terrain, std::string& error)
{
    if (done() || m_source == nullptr)
        return Step::Done;
    const auto tile = static_cast<i32>(m_plan.tile);
    const i32 lastX = m_plan.firstX + static_cast<i32>(m_plan.columns) - 1;
    const i32 lastZ = m_plan.firstZ + static_cast<i32>(m_plan.rows) - 1;
    const i32 tileX = m_tileX0 + static_cast<i32>(m_next % m_tilesX);
    const i32 tileZ = m_tileZ0 + static_cast<i32>(m_next / m_tilesX);
    // What this tile lays, and the table it is laid from: the same and one
    // column more each way, where the import has one.
    const i32 x0 = std::max(m_plan.firstX, tileX * tile);
    const i32 z0 = std::max(m_plan.firstZ, tileZ * tile);
    const i32 x1 = std::min(lastX, tileX * tile + tile - 1);
    const i32 z1 = std::min(lastZ, tileZ * tile + tile - 1);
    const i32 tableX0 = std::max(m_plan.firstX, x0 - 1);
    const i32 tableZ0 = std::max(m_plan.firstZ, z0 - 1);
    const i32 tableX1 = std::min(lastX, x1 + 1);
    const i32 tableZ1 = std::min(lastZ, z1 + 1);
    const auto width = static_cast<u32>(tableX1 - tableX0 + 1);
    const auto depth = static_cast<u32>(tableZ1 - tableZ0 + 1);
    m_table.assign(static_cast<usize>(width) * depth, NoGround);
    if (!m_source->read(tableX0, tableZ0, width, depth, m_table, error))
        return Step::Failed;

    // The ground under it first, where it streams (terrain audit U2) -- and
    // under it only. A brush reads a margin round itself because its ramp
    // reaches sideways; a table lays columns, the window's and no other, so
    // the cells beside the tile are not touched and are not read: half a
    // voxel inside the tile's edge, which a neighbour's bounds do not reach.
    // Read, they were every cell along every seam of the world, read back
    // from the cache to be let go again.
    const double voxel = static_cast<double>(terrain.field.settings().voxelSize);
    const double inside = 0.5 * voxel;
    if (!world.loadGround(core::DVec3{terrain.origin.x + static_cast<double>(x0) * voxel + inside, 0.0,
                                      terrain.origin.z + static_cast<double>(z0) * voxel + inside},
                          core::DVec3{terrain.origin.x + static_cast<double>(x1 + 1) * voxel - inside, 0.0,
                                      terrain.origin.z + static_cast<double>(z1 + 1) * voxel - inside}))
        return Step::Refused;

    const asset::HeightWindow window{static_cast<u32>(x0 - tableX0), static_cast<u32>(z0 - tableZ0),
                                     static_cast<u32>(x1 - x0 + 1), static_cast<u32>(z1 - z0 + 1)};
    if (asset::writeHeights(terrain.field, tableX0, tableZ0, width, m_table, m_plan.material, window).refused)
        return Step::Refused;
    terrain.fieldRevision += 1;
    m_next += 1;
    return done() ? Step::Done : Step::More;
}

} // namespace engine::app
