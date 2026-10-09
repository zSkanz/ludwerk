#pragma once

#include <array>
#include <vector>

#include "engine/rhi/types.h"

namespace engine::render {

// Seven pictures plus scene depth fit the ordinary eight-sampler layout in
// particle.hlsl. Explicit shader bindings need no descriptor-indexing feature.
inline constexpr core::u32 MaxParticleTextures = 7;

struct ParticleBatch
{
    std::array<rhi::TextureHandle, MaxParticleTextures> textures{};
    core::u32 textureCount = 0;
    core::u32 first = 0;
    core::u32 count = 0;
};

// Append in the original back-to-front order. Pictures can alternate inside a
// batch; a new one starts only when another distinct picture would not fit.
// Zero means procedural shape; pictures use one-based slots in Params.w.
inline core::u32 appendParticle(std::vector<ParticleBatch>& batches, rhi::TextureHandle texture, core::u32 at)
{
    if (batches.empty())
        batches.push_back(ParticleBatch{.first = at});
    core::u32 slot = 0;
    if (texture.valid()) {
        for (; slot < batches.back().textureCount; ++slot)
            if (batches.back().textures[slot] == texture)
                break;
        if (slot == MaxParticleTextures) {
            batches.push_back(ParticleBatch{.first = at});
            slot = 0;
        }
        ParticleBatch& batch = batches.back();
        if (slot == batch.textureCount)
            batch.textures[batch.textureCount++] = texture;
        ++slot;
    }
    ++batches.back().count;
    return slot;
}

} // namespace engine::render
