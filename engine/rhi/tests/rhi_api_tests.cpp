#include <doctest/doctest.h>

#include "engine/rhi/descs.h"
#include "engine/rhi/types.h"

using namespace engine::rhi;

// Header-only: this file links no backend on purpose. If it ever stops
// compiling without one, the seam has grown an implementation dependency.

TEST_CASE("handles default to null and are not interchangeable")
{
    // The point of separate structs is that a compiler, not a code reviewer,
    // catches a TextureHandle passed where a BufferHandle belongs. That cannot
    // be asserted at runtime, so what is asserted here is the other half of the
    // contract: zero is null, and a backend never hands zero out.
    CHECK_FALSE(BufferHandle{}.valid());
    CHECK_FALSE(TextureHandle{}.valid());
    CHECK_FALSE(SamplerHandle{}.valid());
    CHECK_FALSE(ShaderHandle{}.valid());
    CHECK_FALSE(PipelineHandle{}.valid());

    CHECK(BufferHandle{7} == BufferHandle{7});
    CHECK_FALSE(BufferHandle{7} == BufferHandle{8});
}

TEST_CASE("usage flags compose")
{
    constexpr auto usage = TextureUsage::Sampled | TextureUsage::ColorTarget;

    CHECK(hasUsage(usage, TextureUsage::Sampled));
    CHECK(hasUsage(usage, TextureUsage::ColorTarget));
    CHECK_FALSE(hasUsage(usage, TextureUsage::DepthStencilTarget));

    CHECK(isDepthFormat(TextureFormat::D32Float));
    CHECK(isDepthFormat(TextureFormat::D24UnormS8Uint));
    CHECK_FALSE(isDepthFormat(TextureFormat::Rgba8Unorm));
}

TEST_CASE("descriptors default to the common case")
{
    // Call sites name only what they care about, which is what keeps a
    // pipeline description readable and a capture stream diffable.
    constexpr GraphicsPipelineDesc pipeline{};

    CHECK(pipeline.primitive == PrimitiveType::TriangleList);
    CHECK(pipeline.rasterizer.cullMode == CullMode::Back);
    CHECK(pipeline.rasterizer.frontFace == FrontFace::CounterClockwise);
    CHECK_FALSE(pipeline.depthStencil.depthTest);
    CHECK(pipeline.depthStencilFormat == TextureFormat::Undefined);

    constexpr ColorAttachment attachment{};
    CHECK(attachment.loadOp == LoadOp::Clear);
    CHECK(attachment.storeOp == StoreOp::Store);
}

TEST_CASE("a texture a compute pass writes says so, apart from one it only samples")
{
    // ADR 0164. Two flags and not one: a texture a pass reads back through the
    // binding it writes it by is another kind of resource to every backend.
    constexpr auto written = TextureUsage::Sampled | TextureUsage::ComputeStorageWrite;
    constexpr auto readBack = TextureUsage::Sampled | TextureUsage::ComputeStorageReadWrite;

    CHECK(hasUsage(written, TextureUsage::ComputeStorageWrite));
    CHECK_FALSE(hasUsage(written, TextureUsage::ComputeStorageReadWrite));
    CHECK(hasUsage(readBack, TextureUsage::ComputeStorageReadWrite));
    CHECK_FALSE(hasUsage(readBack, TextureUsage::ComputeStorageWrite));
    CHECK_FALSE(hasUsage(TextureUsage::Sampled | TextureUsage::ColorTarget, TextureUsage::ComputeStorageWrite));

    // The image an atomic is done on is not a depth, whatever it holds.
    CHECK_FALSE(isDepthFormat(TextureFormat::R32Uint));

    const ComputePipelineDesc pipeline{};
    CHECK(pipeline.readwriteStorageTextureCount == 0u);
    const ComputeTextureWrite write{};
    CHECK_FALSE(write.texture.valid());
    CHECK(write.mipLevel == 0u);
}
