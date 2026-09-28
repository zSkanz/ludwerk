#pragma once

// **A terrain's layers are material assets** (ADR 0113): a voxel's material
// byte `n` is the terrain's `Layers[n]`, a material URN, and zero stays "no
// ground". A new terrain starts with the eight the engine ships, in the order
// the old palette had them, so id 3 is still rock.
//
// **The engine's eight are built in, not files.** Their URNs are
// `engine://terrain/grass` and so on, answered by the material library without
// a mount, and their textures -- colour, normal, surface and height, each
// `engine://terrain/<name>/<map>` -- are drawn here, from noise, the first time
// something asks. Nothing binary is in the repository, a package carries
// nothing for them, and every profile and platform has the same eight.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/asset/image.h"
#include "engine/asset/material.h"
#include "engine/core/types.h"

namespace engine::asset {

inline constexpr std::string_view EngineTerrainPrefix = "engine://terrain/";

// Ids 1 to 255; a list may be shorter.
inline constexpr core::usize MaxTerrainLayers = 255;

// The side of every built-in texture, in pixels.
inline constexpr core::u32 EngineTerrainTextureSize = 512;

// `engine://terrain/grass` for id 1, and so on to 8; empty past the eight.
[[nodiscard]] std::string engineTerrainUrn(core::u8 id);

// What a new terrain's `Layers` are: the eight, in id order.
[[nodiscard]] std::vector<std::string> defaultTerrainLayers();

// Whether `urn` names one of the engine's built-in materials or textures.
[[nodiscard]] bool isEngineMaterial(std::string_view urn) noexcept;
[[nodiscard]] bool isEngineTexture(std::string_view urn) noexcept;

// The built-in material a URN names, or nothing.
[[nodiscard]] std::optional<MaterialAsset> engineMaterial(std::string_view urn);

// The built-in texture a URN names, RGBA8 -- the colour map in sRGB, the rest
// linear -- or nothing. Drawn on first ask and kept; safe from any thread.
[[nodiscard]] std::optional<Image> engineTexture(std::string_view urn);

} // namespace engine::asset
