// A terrain's layers are materials (ADR 0113): the engine's eight, built in,
// and the mesher handing the shader each triangle's layers.
#include <cmath>
#include <cstdlib>
#include <doctest/doctest.h>
#include <vector>

#include "engine/asset/material.h"
#include "engine/asset/terrain_layers.h"
#include "engine/asset/terrain_mesher.h"

using namespace engine;
using namespace engine::asset;

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
