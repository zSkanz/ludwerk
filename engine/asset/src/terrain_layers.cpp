#include "engine/asset/terrain_layers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

namespace engine::asset {
namespace {

using core::f32;
using core::u32;

// What each built-in layer looks like, as the recipe the noise below follows.
enum class Pattern : core::u8
{
    Grass,
    Sand,
    Rock,
    Snow,
    Mud,
    Sandstone,
    Basalt,
    Ice,
};

struct Layer
{
    std::string_view name;
    Pattern pattern;
    // Linear colours the pattern moves between.
    std::array<f32, 3> dark;
    std::array<f32, 3> light;
    f32 roughness;
    // How deep the height reads in the normal map.
    f32 relief;
    // Metres per repeat.
    f32 tileSize;
};

// In the old palette's order, so id `n` is still what it was. The colours
// bracket the palette's flat ones, which is what a world authored against the
// flat palette looked like from a distance.
constexpr std::array<Layer, 8> Layers{{
    {"grass", Pattern::Grass, {0.06f, 0.15f, 0.04f}, {0.19f, 0.36f, 0.10f}, 0.92f, 2.0f, 3.0f},
    {"sand", Pattern::Sand, {0.55f, 0.45f, 0.25f}, {0.82f, 0.72f, 0.45f}, 0.88f, 1.2f, 4.0f},
    {"rock", Pattern::Rock, {0.16f, 0.15f, 0.14f}, {0.44f, 0.42f, 0.40f}, 0.82f, 4.0f, 6.0f},
    {"snow", Pattern::Snow, {0.72f, 0.77f, 0.85f}, {0.93f, 0.95f, 0.98f}, 0.62f, 1.0f, 5.0f},
    {"mud", Pattern::Mud, {0.10f, 0.07f, 0.05f}, {0.33f, 0.24f, 0.16f}, 0.90f, 2.0f, 3.0f},
    {"sandstone", Pattern::Sandstone, {0.45f, 0.29f, 0.16f}, {0.76f, 0.57f, 0.37f}, 0.86f, 3.0f, 6.0f},
    {"basalt", Pattern::Basalt, {0.05f, 0.05f, 0.06f}, {0.19f, 0.19f, 0.21f}, 0.72f, 3.5f, 5.0f},
    {"ice", Pattern::Ice, {0.40f, 0.60f, 0.76f}, {0.72f, 0.86f, 0.94f}, 0.14f, 1.0f, 5.0f},
}};

// --- Tileable noise ------------------------------------------------------------
//
// Every lattice wraps at a period that divides the texture, so the right edge
// meets the left one and a terrain repeats with no seam.

[[nodiscard]] u32 hash(u32 x, u32 y, u32 seed) noexcept
{
    u32 h = x * 0x8DA6B343u ^ y * 0xD8163841u ^ seed * 0xCB1AB31Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

[[nodiscard]] f32 unit(u32 h) noexcept
{
    return static_cast<f32>(h & 0xFFFFFFu) / static_cast<f32>(0xFFFFFFu);
}

[[nodiscard]] f32 smooth(f32 t) noexcept
{
    return t * t * (3.0f - 2.0f * t);
}

// Value noise over a `period`-cell lattice; `u` and `v` in 0..1.
[[nodiscard]] f32 valueNoise(f32 u, f32 v, u32 period, u32 seed) noexcept
{
    const f32 x = u * static_cast<f32>(period);
    const f32 y = v * static_cast<f32>(period);
    const f32 fx = std::floor(x);
    const f32 fy = std::floor(y);
    const auto ix =
        static_cast<u32>(static_cast<int>(fx) % static_cast<int>(period) + static_cast<int>(period)) % period;
    const auto iy =
        static_cast<u32>(static_cast<int>(fy) % static_cast<int>(period) + static_cast<int>(period)) % period;
    const u32 jx = (ix + 1) % period;
    const u32 jy = (iy + 1) % period;
    const f32 tx = smooth(x - fx);
    const f32 ty = smooth(y - fy);
    const f32 a = unit(hash(ix, iy, seed));
    const f32 b = unit(hash(jx, iy, seed));
    const f32 c = unit(hash(ix, jy, seed));
    const f32 d = unit(hash(jx, jy, seed));
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

[[nodiscard]] f32 fbm(f32 u, f32 v, u32 period, int octaves, u32 seed) noexcept
{
    f32 sum = 0.0f;
    f32 amplitude = 0.5f;
    f32 total = 0.0f;
    for (int octave = 0; octave < octaves; ++octave) {
        sum += valueNoise(u, v, period << octave, seed + static_cast<u32>(octave) * 101u) * amplitude;
        total += amplitude;
        amplitude *= 0.5f;
    }
    return sum / total;
}

// Distance to the nearest and second-nearest feature point on a wrapping grid
// of `period` cells, in cells.
[[nodiscard]] std::array<f32, 2> worley(f32 u, f32 v, u32 period, u32 seed) noexcept
{
    const f32 x = u * static_cast<f32>(period);
    const f32 y = v * static_cast<f32>(period);
    const int cx = static_cast<int>(std::floor(x));
    const int cy = static_cast<int>(std::floor(y));
    f32 first = 9.0f;
    f32 second = 9.0f;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int gx = cx + dx;
            const int gy = cy + dy;
            const auto wx =
                static_cast<u32>((gx % static_cast<int>(period) + static_cast<int>(period)) % static_cast<int>(period));
            const auto wy =
                static_cast<u32>((gy % static_cast<int>(period) + static_cast<int>(period)) % static_cast<int>(period));
            const f32 px = static_cast<f32>(gx) + 0.15f + 0.7f * unit(hash(wx, wy, seed));
            const f32 py = static_cast<f32>(gy) + 0.15f + 0.7f * unit(hash(wx, wy, seed + 7u));
            const f32 distance = std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
            if (distance < first) {
                second = first;
                first = distance;
            }
            else if (distance < second) {
                second = distance;
            }
        }
    }
    return {first, second};
}

[[nodiscard]] f32 saturate(f32 value) noexcept
{
    return std::clamp(value, 0.0f, 1.0f);
}

// One pixel of a layer: its height, how far between the dark and the light
// colour it is, and its roughness.
struct Sample
{
    f32 height = 0.5f;
    f32 tone = 0.5f;
    f32 roughness = 0.8f;
    // A second colour mixed in -- grass's dry patches, ice's white cracks.
    f32 accent = 0.0f;
};

[[nodiscard]] Sample sampleLayer(const Layer& layer, f32 u, f32 v) noexcept
{
    Sample out;
    out.roughness = layer.roughness;
    switch (layer.pattern) {
    case Pattern::Grass: {
        const f32 clumps = fbm(u, v, 8, 4, 11u);
        // Blades: fine noise stretched along one axis, so the surface reads as
        // grain rather than as mottle.
        const f32 blades = valueNoise(u * 4.0f, v, 128, 12u) * 0.5f + valueNoise(u, v * 4.0f, 128, 13u) * 0.5f;
        out.height = saturate(clumps * 0.55f + blades * 0.45f);
        out.tone = saturate(0.2f + clumps * 0.5f + blades * 0.4f);
        out.accent = saturate((fbm(u, v, 4, 3, 14u) - 0.58f) * 3.0f);
        break;
    }
    case Pattern::Sand: {
        const f32 warp = fbm(u, v, 4, 3, 21u);
        const f32 ripple = 0.5f + 0.5f * std::sin((v * 24.0f + warp * 3.0f) * 6.2831853f);
        const f32 grain = valueNoise(u, v, 256, 22u);
        out.height = saturate(ripple * 0.6f + grain * 0.25f + warp * 0.15f);
        out.tone = saturate(0.35f + ripple * 0.25f + (grain - 0.5f) * 0.5f + (warp - 0.5f) * 0.4f);
        break;
    }
    case Pattern::Rock: {
        const f32 body = fbm(u, v, 4, 6, 31u);
        const f32 ridged = 1.0f - std::abs(fbm(u, v, 8, 4, 32u) * 2.0f - 1.0f);
        const f32 cracks = saturate((ridged - 0.88f) * 5.0f);
        out.height = saturate(body * 0.8f + 0.2f - cracks * 0.3f);
        out.tone = saturate(body * 0.9f + 0.05f - cracks * 0.3f);
        out.roughness = layer.roughness + cracks * 0.1f;
        break;
    }
    case Pattern::Snow: {
        const f32 drifts = fbm(u, v, 4, 4, 41u);
        const f32 crust = valueNoise(u, v, 128, 42u);
        out.height = saturate(drifts * 0.8f + crust * 0.2f);
        out.tone = saturate(0.55f + drifts * 0.4f + (crust - 0.5f) * 0.15f);
        out.roughness = layer.roughness + (crust - 0.5f) * 0.2f;
        break;
    }
    case Pattern::Mud: {
        const f32 body = fbm(u, v, 4, 5, 51u);
        const f32 puddle = saturate((0.38f - body) * 8.0f);
        out.height = saturate(body * (1.0f - puddle) + 0.3f * puddle);
        out.tone = saturate(body * 0.9f + 0.1f - puddle * 0.35f);
        // Wet where it pools, and a puddle is nearly a mirror.
        out.roughness = layer.roughness * (1.0f - puddle) + 0.18f * puddle;
        break;
    }
    case Pattern::Sandstone: {
        const f32 warp = fbm(u, v, 4, 4, 61u);
        const f32 layerCoordinate = v * 12.0f + warp * 2.5f;
        const f32 band = 0.5f + 0.5f * std::sin(layerCoordinate * 6.2831853f);
        const f32 grain = valueNoise(u, v, 256, 62u);
        out.height = saturate(band * 0.6f + grain * 0.2f + warp * 0.2f);
        out.tone = saturate(band * 0.6f + (grain - 0.5f) * 0.3f + warp * 0.3f);
        break;
    }
    case Pattern::Basalt: {
        // The tops of columns: cells with dark joints between them.
        const std::array<f32, 2> cell = worley(u, v, 10, 71u);
        const f32 joint = saturate((cell[1] - cell[0]) * 6.0f);
        const f32 surface = fbm(u, v, 16, 3, 72u);
        out.height = saturate(joint * 0.8f + surface * 0.2f);
        out.tone = saturate(joint * 0.7f + surface * 0.3f);
        break;
    }
    case Pattern::Ice: {
        const f32 body = fbm(u, v, 4, 3, 81u);
        const std::array<f32, 2> cell = worley(u, v, 6, 82u);
        const f32 crack = saturate(1.0f - (cell[1] - cell[0]) * 12.0f);
        out.height = saturate(body * 0.7f + 0.3f - crack * 0.3f);
        out.tone = saturate(body * 0.8f + 0.1f);
        out.accent = crack * 0.8f;
        out.roughness = layer.roughness + crack * 0.3f;
        break;
    }
    }
    out.roughness = saturate(out.roughness);
    return out;
}

[[nodiscard]] std::byte srgbByte(f32 linear) noexcept
{
    const f32 c = saturate(linear);
    const f32 encoded = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::byte>(static_cast<u32>(std::lround(encoded * 255.0f)));
}

[[nodiscard]] std::byte linearByte(f32 value) noexcept
{
    return static_cast<std::byte>(static_cast<u32>(std::lround(saturate(value) * 255.0f)));
}

// The four maps of one layer, drawn together because they share the height.
struct Sheet
{
    Image color;
    Image normal;
    Image surface;
    Image height;
};

[[nodiscard]] Image blank(u32 size)
{
    Image image;
    image.width = size;
    image.height = size;
    image.sourceChannels = 4;
    image.pixels.resize(static_cast<std::size_t>(size) * size * 4u);
    return image;
}

[[nodiscard]] Sheet drawSheet(const Layer& layer)
{
    constexpr u32 Size = EngineTerrainTextureSize;
    std::vector<Sample> samples(static_cast<std::size_t>(Size) * Size);
    for (u32 y = 0; y < Size; ++y) {
        for (u32 x = 0; x < Size; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(Size);
            const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(Size);
            samples[static_cast<std::size_t>(y) * Size + x] = sampleLayer(layer, u, v);
        }
    }

    // The accent colour: dry straw for grass, white for ice, the light colour
    // otherwise (unused).
    const std::array<f32, 3> accent = layer.pattern == Pattern::Grass ? std::array<f32, 3>{0.36f, 0.34f, 0.12f}
                                      : layer.pattern == Pattern::Ice ? std::array<f32, 3>{0.92f, 0.96f, 1.0f}
                                                                      : layer.light;

    Sheet sheet{blank(Size), blank(Size), blank(Size), blank(Size)};
    const auto at = [&](int x, int y) -> const Sample& {
        const auto wx = static_cast<u32>((x + static_cast<int>(Size)) % static_cast<int>(Size));
        const auto wy = static_cast<u32>((y + static_cast<int>(Size)) % static_cast<int>(Size));
        return samples[static_cast<std::size_t>(wy) * Size + wx];
    };
    for (u32 y = 0; y < Size; ++y) {
        for (u32 x = 0; x < Size; ++x) {
            const Sample& s = at(static_cast<int>(x), static_cast<int>(y));
            const std::size_t pixel = (static_cast<std::size_t>(y) * Size + x) * 4u;

            for (std::size_t channel = 0; channel < 3; ++channel) {
                const f32 base = layer.dark[channel] + (layer.light[channel] - layer.dark[channel]) * s.tone;
                sheet.color.pixels[pixel + channel] = srgbByte(base + (accent[channel] - base) * s.accent);
            }
            sheet.color.pixels[pixel + 3] = std::byte{255};

            // The normal from the height's slope, wrapping at the edges like
            // everything else; +Y up, as a glTF normal map is.
            const f32 dx = (at(static_cast<int>(x) + 1, static_cast<int>(y)).height -
                            at(static_cast<int>(x) - 1, static_cast<int>(y)).height) *
                           layer.relief;
            const f32 dy = (at(static_cast<int>(x), static_cast<int>(y) + 1).height -
                            at(static_cast<int>(x), static_cast<int>(y) - 1).height) *
                           layer.relief;
            const f32 length = std::sqrt(dx * dx + dy * dy + 1.0f);
            sheet.normal.pixels[pixel + 0] = linearByte(-dx / length * 0.5f + 0.5f);
            sheet.normal.pixels[pixel + 1] = linearByte(dy / length * 0.5f + 0.5f);
            sheet.normal.pixels[pixel + 2] = linearByte(1.0f / length * 0.5f + 0.5f);
            sheet.normal.pixels[pixel + 3] = std::byte{255};

            // Occlusion in R, roughness in G, metalness in B: glTF's packing.
            sheet.surface.pixels[pixel + 0] = linearByte(0.55f + 0.45f * s.height);
            sheet.surface.pixels[pixel + 1] = linearByte(s.roughness);
            sheet.surface.pixels[pixel + 2] = std::byte{0};
            sheet.surface.pixels[pixel + 3] = std::byte{255};

            const std::byte h = linearByte(s.height);
            sheet.height.pixels[pixel + 0] = h;
            sheet.height.pixels[pixel + 1] = h;
            sheet.height.pixels[pixel + 2] = h;
            sheet.height.pixels[pixel + 3] = std::byte{255};
        }
    }
    return sheet;
}

[[nodiscard]] const Layer* layerNamed(std::string_view name) noexcept
{
    for (const Layer& layer : Layers) {
        if (layer.name == name)
            return &layer;
    }
    return nullptr;
}

} // namespace

std::string engineTerrainUrn(core::u8 id)
{
    if (id == 0 || id > Layers.size())
        return {};
    return std::string(EngineTerrainPrefix).append(Layers[id - 1u].name);
}

std::vector<std::string> defaultTerrainLayers()
{
    std::vector<std::string> out;
    out.reserve(Layers.size());
    for (core::usize index = 0; index < Layers.size(); ++index)
        out.push_back(engineTerrainUrn(static_cast<core::u8>(index + 1)));
    return out;
}

bool isEngineMaterial(std::string_view urn) noexcept
{
    return urn.starts_with(EngineTerrainPrefix) && layerNamed(urn.substr(EngineTerrainPrefix.size())) != nullptr;
}

bool isEngineTexture(std::string_view urn) noexcept
{
    if (!urn.starts_with(EngineTerrainPrefix))
        return false;
    const std::string_view rest = urn.substr(EngineTerrainPrefix.size());
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos || layerNamed(rest.substr(0, slash)) == nullptr)
        return false;
    const std::string_view map = rest.substr(slash + 1);
    return map == "color" || map == "normal" || map == "surface" || map == "height";
}

std::optional<MaterialAsset> engineMaterial(std::string_view urn)
{
    if (!isEngineMaterial(urn))
        return std::nullopt;
    const Layer& layer = *layerNamed(urn.substr(EngineTerrainPrefix.size()));
    const std::string base(urn);
    MaterialAsset out;
    out.written = AllMaterialFields;
    MaterialProperties& p = out.properties;
    // The textures carry the colour and the roughness; the factors pass them
    // through, as a glTF material's do.
    p.color = core::Color3{1.0f, 1.0f, 1.0f};
    p.roughness = 1.0f;
    p.metalness = 0.0f;
    p.colorMap = base + "/color";
    p.normalMap = base + "/normal";
    p.metallicRoughnessMap = base + "/surface";
    p.heightMap = base + "/height";
    p.tileSize = layer.tileSize;
    p.triplanar = true;
    p.blendSharpness = 0.5f;
    // **What each is to touch** (ADR 0117). Only what is unlike the ground a
    // body has always met says so: ice and snow slide, mud grips. The rest is
    // the 0.3 everything collided with before a material said anything, so a
    // world on grass, sand and rock simulates as it did.
    const std::string_view name = urn.substr(EngineTerrainPrefix.size());
    if (name == "ice")
        p.friction = 0.03f;
    else if (name == "snow")
        p.friction = 0.15f;
    else if (name == "mud")
        p.friction = 0.6f;
    p.tags = {std::string(name)};
    return out;
}

std::optional<Image> engineTexture(std::string_view urn)
{
    if (!isEngineTexture(urn))
        return std::nullopt;
    const std::string_view rest = urn.substr(EngineTerrainPrefix.size());
    const std::size_t slash = rest.find('/');
    const Layer& layer = *layerNamed(rest.substr(0, slash));
    const std::string_view map = rest.substr(slash + 1);

    // Drawn once per layer and kept: a terrain asks for four maps of each of
    // its layers, and a second terrain for the same ones again.
    //
    // **One layer's drawing holds up nobody else's** (D544): the table is
    // locked for as long as it takes to find a layer's place in it, and the
    // sheet is drawn under that layer's own flag -- so eight layers asked for
    // by eight jobs are drawn side by side, and four maps of one are one sheet.
    struct Drawn
    {
        std::once_flag once;
        Sheet sheet;
    };
    static std::mutex mutex;
    static std::map<std::string_view, std::shared_ptr<Drawn>> sheets;
    std::shared_ptr<Drawn> drawn;
    {
        const std::lock_guard lock(mutex);
        std::shared_ptr<Drawn>& place = sheets[layer.name];
        if (place == nullptr)
            place = std::make_shared<Drawn>();
        drawn = place;
    }
    std::call_once(drawn->once, [&] { drawn->sheet = drawSheet(layer); });
    const Sheet& sheet = drawn->sheet;
    return map == "color"     ? sheet.color
           : map == "normal"  ? sheet.normal
           : map == "surface" ? sheet.surface
                              : sheet.height;
}

} // namespace engine::asset
