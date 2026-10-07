// A terrain's layers are materials (ADR 0113): the engine's eight, built in,
// and the mesher handing the shader each triangle's layers.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <vector>

#include "engine/asset/material.h"
#include "engine/asset/terrain_layers.h"
#include "engine/asset/terrain_mesher.h"

using namespace engine;
using namespace engine::asset;
using core::f64;
using core::u32;

TEST_CASE("a new terrain's layers are the engine's eight, in the old palette's order")
{
    const std::vector<std::string> layers = defaultTerrainLayers();
    REQUIRE(layers.size() == 8);
    CHECK(layers[0] == "engine://terrain/grass");
    // Id 3 is still rock, so a world painted before layers opens unchanged.
    CHECK(layers[2] == "engine://terrain/rock");
    CHECK(layers[7] == "engine://terrain/ice");
    CHECK(engineTerrainUrn(0).empty());
    CHECK(engineTerrainUrn(9).empty());
}

TEST_CASE("the engine's terrain materials resolve with no file and no mount")
{
    MaterialLibrary library;
    const ResolvedMaterial& rock = library.resolve("engine://terrain/rock");
    CHECK(rock.properties.colorMap == "engine://terrain/rock/color");
    CHECK(rock.properties.normalMap == "engine://terrain/rock/normal");
    CHECK(rock.properties.metallicRoughnessMap == "engine://terrain/rock/surface");
    CHECK(rock.properties.heightMap == "engine://terrain/rock/height");
    CHECK(rock.properties.triplanar);
    CHECK(static_cast<double>(rock.properties.tileSize) > 0.0);
    CHECK_FALSE(isEngineMaterial("engine://terrain/lava"));
    CHECK_FALSE(isEngineTexture("engine://terrain/rock/sparkle"));
}

TEST_CASE("an engine terrain texture is drawn, and it repeats with no seam")
{
    const std::optional<Image> color = engineTexture("engine://terrain/grass/color");
    REQUIRE(color.has_value());
    REQUIRE(color->valid());
    CHECK(color->width == EngineTerrainTextureSize);

    // Across the wrap, the step from the last column to the first is no larger
    // than an ordinary step between two neighbouring columns in the middle.
    const auto columnStep = [&](const Image& image, core::u32 a, core::u32 b) {
        double sum = 0.0;
        for (core::u32 y = 0; y < image.height; ++y) {
            for (core::u32 channel = 0; channel < 3; ++channel) {
                const auto at = [&](core::u32 x) {
                    return static_cast<int>(
                        image.pixels[(static_cast<std::size_t>(y) * image.width + x) * 4 + channel]);
                };
                sum += std::abs(at(a) - at(b));
            }
        }
        return sum / static_cast<double>(image.height * 3);
    };
    for (const char* map :
         {"engine://terrain/rock/height", "engine://terrain/sand/color", "engine://terrain/basalt/normal"}) {
        const std::optional<Image> image = engineTexture(map);
        REQUIRE(image.has_value());
        const double wrap = columnStep(*image, image->width - 1, 0);
        const double inside = columnStep(*image, image->width / 2 - 1, image->width / 2);
        CHECK(wrap <= inside * 2.0 + 2.0);
    }
}

TEST_CASE("every vertex carries its triangle's three layers and its corner")
{
    // Grass, with a ball of rock painted into it: triangles of one layer and
    // triangles along the seam.
    TerrainField field(FieldSettings{.voxelSize = 1.0f, .minHeight = -64.0f, .maxHeight = 64.0f});
    (void)fillFlat(field, core::DVec3{0.0, 0.0, 0.0}, 64.0f, 4.0f, 1);
    (void)paintBall(field, core::DVec3{16.0, 4.0, 16.0}, 5.0, 3);
    const TerrainMesh meshed =
        meshField(field, MeshRegion{.minX = 0, .minY = 0, .minZ = 0, .cellsX = 32, .cellsY = 32, .cellsZ = 32});
    REQUIRE_FALSE(meshed.mesh.indices.empty());

    bool sawSeam = false;
    bool sawPlain = false;
    const std::vector<Vertex>& vertices = meshed.mesh.vertices;
    for (core::usize at = 0; at + 2 < meshed.mesh.indices.size(); at += 3) {
        const Vertex& a = vertices[meshed.mesh.indices[at]];
        const Vertex& b = vertices[meshed.mesh.indices[at + 1]];
        const Vertex& c = vertices[meshed.mesh.indices[at + 2]];
        // One triangle, one set of layers, whichever corner is asked.
        CHECK(a.uv[0] == b.uv[0]);
        CHECK(b.uv[0] == c.uv[0]);
        const auto packed = static_cast<core::u32>(a.uv[0] + 0.5f);
        const core::u32 first = packed & 255u;
        const core::u32 second = (packed >> 8) & 255u;
        const core::u32 third = packed >> 16;
        CHECK((first == 1u || first == 3u));
        if (first == second && second == third) {
            sawPlain = true;
            continue;
        }
        // A seam triangle: its corners are 0, 1 and 2, in order.
        sawSeam = true;
        CHECK(a.uv[1] == 0.0f);
        CHECK(b.uv[1] == 1.0f);
        CHECK(c.uv[1] == 2.0f);
    }
    CHECK(sawPlain);
    CHECK(sawSeam);
}

TEST_CASE("D592: no built-in ground carries a feature a field of it would show as a grid")
{
    // **The owner, of a swamp**: "a square inside it that keeps reflecting, and
    // it follows a grid." A tile is repeated across a field, and whatever is
    // large in it is seen once a tile. Mud's body was noise four cells across
    // and its puddles the two or three lowest of them, each nearly a mirror;
    // grass's dry patches and rock's body were the same four cells.
    //
    // Asked of the maps themselves: cut a tile into eight by eight blocks, and
    // of all the variation in it, how much is BETWEEN the blocks -- what is
    // left when everything finer than an eighth of the tile is averaged away.
    // That is what a field shows from above. Mud was 0.67 in colour and 0.54
    // in roughness, grass 0.55, rock 0.67; a ground with no such feature is
    // under 0.2. A map that barely varies at all (snow's colour) shows nothing
    // either way and is not asked.
    constexpr u32 Blocks = 8;
    const auto between = [](const Image& image, int channels, int first, f64& contrast) {
        const u32 size = image.width;
        const u32 block = size / Blocks;
        std::vector<f64> means(static_cast<std::size_t>(Blocks) * Blocks, 0.0);
        f64 sum = 0.0;
        f64 squares = 0.0;
        for (u32 y = 0; y < size; ++y) {
            for (u32 x = 0; x < size; ++x) {
                f64 value = 0.0;
                for (int channel = 0; channel < channels; ++channel) {
                    const std::size_t at =
                        (static_cast<std::size_t>(y) * size + x) * 4u + static_cast<std::size_t>(first + channel);
                    value += static_cast<f64>(std::to_integer<u32>(image.pixels[at])) / 255.0;
                }
                value /= static_cast<f64>(channels);
                sum += value;
                squares += value * value;
                means[static_cast<std::size_t>(y / block) * Blocks + x / block] += value;
            }
        }
        const f64 count = static_cast<f64>(size) * static_cast<f64>(size);
        const f64 mean = sum / count;
        const f64 variance = squares / count - mean * mean;
        contrast = std::sqrt(std::max(variance, 0.0));
        f64 across = 0.0;
        for (f64& each : means) {
            each /= static_cast<f64>(block) * static_cast<f64>(block);
            across += (each - mean) * (each - mean);
        }
        across /= static_cast<f64>(means.size());
        return variance > 0.0 ? across / variance : 0.0;
    };

    for (const char* name : {"grass", "sand", "rock", "snow", "mud", "sandstone", "basalt", "ice"}) {
        CAPTURE(name);
        const std::optional<Image> color = engineTexture(std::string("engine://terrain/") + name + "/color");
        const std::optional<Image> surface = engineTexture(std::string("engine://terrain/") + name + "/surface");
        REQUIRE(color.has_value());
        REQUIRE(surface.has_value());
        f64 contrast = 0.0;
        const f64 colour = between(*color, 3, 0, contrast);
        if (contrast >= 0.01)
            CHECK(colour < 0.25);
        // Roughness is the surface map's green.
        const f64 roughness = between(*surface, 1, 1, contrast);
        if (contrast >= 0.01)
            CHECK(roughness < 0.25);
    }

    // And a puddle is wet, not a mirror: the least roughness mud has.
    const std::optional<Image> mud = engineTexture("engine://terrain/mud/surface");
    REQUIRE(mud.has_value());
    u32 least = 255;
    for (std::size_t at = 1; at < mud->pixels.size(); at += 4)
        least = std::min(least, std::to_integer<u32>(mud->pixels[at]));
    CHECK(least >= 80);
}
