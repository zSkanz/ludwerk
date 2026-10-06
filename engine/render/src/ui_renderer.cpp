#include "engine/render/ui_renderer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

#include "engine/core/text_key.h"

namespace engine::render {
namespace {

constexpr core::u32 kVertexStride = sizeof(UiVertex);

// A first allocation big enough for a HUD of a hundred elements, so the common
// case never reallocates. Six vertices a quad -- two triangles, no index buffer:
// an index buffer would save a third of the bandwidth on geometry that is
// already the smallest thing in the frame, and cost a second upload.
constexpr core::u32 kInitialVertices = 4096;

} // namespace

rhi::PipelineHandle UiRenderer::makePipeline(rhi::IDevice& device, bool premultiplied)
{
    const std::array<rhi::VertexBufferLayout, 1> buffers{
        rhi::VertexBufferLayout{.slot = 0, .strideBytes = kVertexStride}};

    const std::array<rhi::VertexAttribute, 9> attributes{
        rhi::VertexAttribute{
            .location = 0,
            .bufferSlot = 0,
            .format = rhi::VertexFormat::Float2,
            .offsetBytes = offsetof(UiVertex, x),
        },
        rhi::VertexAttribute{
            .location = 1,
            .bufferSlot = 0,
            .format = rhi::VertexFormat::Ubyte4Unorm,
            .offsetBytes = offsetof(UiVertex, r),
        },
        // The rounded-corner frame (D030): the vertex's offset from the quad's
        // centre and the quad's half-extent, packed as one `Float4` because two
        // pairs that always travel together are one attribute.
        rhi::VertexAttribute{
            .location = 2,
            .bufferSlot = 0,
            .format = rhi::VertexFormat::Float4,
            .offsetBytes = offsetof(UiVertex, localX),
        },
        rhi::VertexAttribute{
            .location = 3,
            .bufferSlot = 0,
            .format = rhi::VertexFormat::Float1,
            .offsetBytes = offsetof(UiVertex, radius),
        },
        rhi::VertexAttribute{
            .location = 4,
            .bufferSlot = 0,
            .format = rhi::VertexFormat::Float2,
            .offsetBytes = offsetof(UiVertex, u),
        },
        // The gradient and the stroke (ADR 0110), `UiVertexAppearance` as
        // four attributes.
        rhi::VertexAttribute{
            .location = 5,
            .bufferSlot = 0,
            .format = rhi::VertexFormat::Float4,
            .offsetBytes = offsetof(UiVertex, look) + offsetof(UiVertexAppearance, gradientX),
        },
        rhi::VertexAttribute{
            .location = 6,
            .bufferSlot = 0,
            .format = rhi::VertexFormat::Float4,
            .offsetBytes = offsetof(UiVertex, look) + offsetof(UiVertexAppearance, gradientRow),
        },
        rhi::VertexAttribute{
            .location = 7,
            .bufferSlot = 0,
            .format = rhi::VertexFormat::Float4,
            .offsetBytes = offsetof(UiVertex, look) + offsetof(UiVertexAppearance, gradientOffsetX),
        },
        rhi::VertexAttribute{
            .location = 8,
            .bufferSlot = 0,
            .format = rhi::VertexFormat::Float1,
            .offsetBytes = offsetof(UiVertex, look) + offsetof(UiVertexAppearance, strokeJoin),
        },
    };

    const std::array<rhi::ColorTargetDesc, 1> targets{rhi::ColorTargetDesc{
        .format = colorFormat_,
        // Alpha blending, because `BackgroundTransparency` is a real property
        // and a HUD is mostly translucent panels. The UI is drawn OVER the
        // world, never into it.
        //
        // **For a picture drawn by this same pipeline** (ADR 0128), one times
        // the source instead. Drawing with alpha blending into a clear target
        // leaves colour times alpha in it, so showing it with the blend above
        // would multiply by alpha twice -- a dark fringe on every soft edge
        // and a half-transparent panel twice as dark as it is.
        .blend = {.enabled = true, .srcColor = premultiplied ? rhi::BlendFactor::One : rhi::BlendFactor::SrcAlpha},
    }};

    return device.createGraphicsPipeline({
        .vertexShader = vertexShader_,
        .fragmentShader = fragmentShader_,
        .vertexBuffers = buffers,
        .vertexAttributes = attributes,
        .primitive = rhi::PrimitiveType::TriangleList,
        // No culling: quads are emitted in one winding and a UI has no back
        // faces, so a cull mode would be a rule with nothing to enforce and one
        // more thing to get backwards.
        .rasterizer = {.cullMode = rhi::CullMode::None},
        .colorTargets = targets,
        .debugName = premultiplied ? "ui2d-picture" : "ui2d",
    });
}

std::optional<core::EngineError> UiRenderer::create(rhi::IDevice& device, const ShaderLibrary& shaders,
                                                    rhi::TextureFormat colorFormat)
{
    core::EngineError error;

    vertexShader_ = shaders.create(device, "ui2d", rhi::ShaderStage::Vertex, &error);
    if (!vertexShader_.valid())
        return error;

    fragmentShader_ = shaders.create(device, "ui2d", rhi::ShaderStage::Fragment, &error);
    if (!fragmentShader_.valid())
        return error;

    colorFormat_ = colorFormat;
    pipeline_ = makePipeline(device, false);
    if (!pipeline_.valid())
        return core::makeError(ENG_TR("render.err.ui_pipeline_failed"));

    // The one-pixel white texture every untextured run samples, and the sampler
    // every run uses. One pixel of white multiplies by one, which is what lets
    // a coloured quad and a glyph and a picture share one pipeline.
    //
    // Its pixel is written on the first frame rather than here, for the reason
    // `renderer_default` writes its own defaults there: `create` runs outside a
    // frame and has no command list.
    whitePixel_ = device.createTexture({
        .format = rhi::TextureFormat::Rgba8Unorm,
        .usage = rhi::TextureUsage::Sampled,
        .width = 1,
        .height = 1,
        .debugName = "ui2d-white",
    });
    // CLAMPED, not repeated. A glyph sampled past its cell would fetch its
    // neighbour in the atlas, which is how text acquires faint marks nobody can
    // explain; tiling is a quad-level decision (`ScaleType`), not a sampler one.
    //
    // **Half a level sharper than the size on screen asks for** (D578). A
    // picture drawn small is sampled from its smaller levels, and between two
    // of them a sampler blends: an icon of 256 texels at seventeen pixels came
    // out of the 16-texel level almost alone, which is soft. Half a level up
    // is the two levels round it in near equal parts. Measured on six of a
    // game's icons at 17, 22, 28 and 44 pixels, as error against a Lanczos
    // reduction (0 to 255): the top level alone, as it was, 19.7, 16.1, 13.7,
    // 9.3; the levels unbiased 12.7, 11.4, 10.1, 7.8; at minus a half 7.6,
    // 6.2, 6.1, 4.4; at minus one it is worse again. A glyph page and a
    // view's picture have one level, and no bias reaches below it.
    sampler_ = device.createSampler({.addressU = rhi::AddressMode::ClampToEdge,
                                     .addressV = rhi::AddressMode::ClampToEdge,
                                     .mipLodBias = -0.5f,
                                     .debugName = "ui2d"});
    if (!whitePixel_.valid() || !sampler_.valid())
        return core::makeError(ENG_TR("render.err.ui_pipeline_failed"));

    // The gradient table (ADR 0110): a row per distinct gradient of a frame.
    // Created whole, because `uploadTexture` writes a whole level.
    gradientTable_ = device.createTexture({
        .format = rhi::TextureFormat::Rgba8Unorm,
        .usage = rhi::TextureUsage::Sampled,
        .width = UiGradientWidth,
        .height = UiGradientRows,
        .debugName = "ui-gradients",
    });
    gradientSampler_ = device.createSampler({.addressU = rhi::AddressMode::ClampToEdge,
                                             .addressV = rhi::AddressMode::ClampToEdge,
                                             .debugName = "ui-gradients"});
    if (!gradientTable_.valid() || !gradientSampler_.valid())
        return core::makeError(ENG_TR("render.err.ui_pipeline_failed"));

    return std::nullopt;
}

void UiRenderer::destroy(rhi::IDevice& device)
{
    if (gradientTable_.valid())
        device.destroy(gradientTable_);
    if (gradientSampler_.valid())
        device.destroy(gradientSampler_);
    gradientTable_ = {};
    gradientSampler_ = {};
    uploadedGradients_.clear();
    gradientsUploaded_ = false;
    if (whitePixel_.valid())
        device.destroy(whitePixel_);
    if (sampler_.valid())
        device.destroy(sampler_);
    if (vertices_.valid())
        device.destroy(vertices_);
    if (pipeline_.valid())
        device.destroy(pipeline_);
    if (premultipliedPipeline_.valid())
        device.destroy(premultipliedPipeline_);
    premultipliedPipeline_ = {};
    if (fragmentShader_.valid())
        device.destroy(fragmentShader_);
    if (vertexShader_.valid())
        device.destroy(vertexShader_);

    vertexShader_ = {};
    fragmentShader_ = {};
    pipeline_ = {};
    vertices_ = {};
    whitePixel_ = {};
    sampler_ = {};
    whiteUploaded_ = false;
    capacityVertices_ = 0;
    pendingVertices_ = 0;
    runs_.clear();
}

void UiRenderer::upload(rhi::IDevice& device, rhi::ICmdList& cmd, std::span<const UiVertex> vertices,
                        std::span<const UiScissorRun> runs)
{
    if (!whiteUploaded_ && whitePixel_.valid()) {
        const std::array<std::byte, 4> white{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
        cmd.uploadTexture(whitePixel_, white, 0);
        whiteUploaded_ = true;
    }

    pendingVertices_ = static_cast<core::u32>(vertices.size());
    runs_.assign(runs.begin(), runs.end());
    // **The picture pipeline, the first time a picture is shown** (ADR 0128),
    // and not at start: a pipeline made at start is a call in every run's
    // command stream, for a thing most interfaces never draw.
    if (!premultipliedPipeline_.valid() && pipeline_.valid() &&
        std::any_of(runs_.begin(), runs_.end(), [](const UiScissorRun& run) { return run.premultiplied; }))
        premultipliedPipeline_ = makePipeline(device, true);
    if (pendingVertices_ == 0)
        return;

    if (pendingVertices_ > capacityVertices_) {
        core::u32 capacity = capacityVertices_ > 0 ? capacityVertices_ : kInitialVertices;
        while (capacity < pendingVertices_)
            capacity *= 2;

        if (vertices_.valid())
            device.destroy(vertices_);

        vertices_ = device.createBuffer({
            .usage = rhi::BufferUsage::Vertex,
            .sizeBytes = capacity * kVertexStride,
            .debugName = "ui2d-vertices",
        });

        if (!vertices_.valid()) {
            capacityVertices_ = 0;
            pendingVertices_ = 0;
            runs_.clear();
            return;
        }
        capacityVertices_ = capacity;
    }

    cmd.upload(vertices_, std::as_bytes(vertices), 0);
}

void UiRenderer::uploadGradients(rhi::ICmdList& cmd, std::span<const core::u8> rows, core::u32 rowCount)
{
    if (!gradientTable_.valid())
        return;
    const core::usize used =
        std::min<core::usize>(static_cast<core::usize>(rowCount) * UiGradientRowBytes, rows.size());
    // Nothing changed, nothing sent: the common frame, where a HUD's
    // gradients are the ones it had last frame.
    if (gradientsUploaded_ && used == uploadedGradients_.size() &&
        std::equal(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(used), uploadedGradients_.begin()))
        return;
    uploadedGradients_.assign(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(used));

    std::vector<std::byte> whole(static_cast<core::usize>(UiGradientRows) * UiGradientRowBytes, std::byte{0xFF});
    // No rows is a table of white: `memcpy` from an empty vector's null data
    // is undefined even for zero bytes.
    if (!uploadedGradients_.empty())
        std::memcpy(whole.data(), uploadedGradients_.data(), uploadedGradients_.size());
    cmd.uploadTexture(gradientTable_, whole, 0);
    gradientsUploaded_ = true;
}

void UiRenderer::render(rhi::ICmdList& cmd, core::Vec2 viewport, core::u32 group)
{
    if (pendingVertices_ == 0 || !pipeline_.valid() || !vertices_.valid())
        return;
    if (viewport.x <= 0.0f || viewport.y <= 0.0f)
        return;

    cmd.pushDebugGroup("ui2d");

    // Pixels to clip space. The negative y is the whole of the UI's y-down
    // convention meeting the API's y-up one, and it happens here rather than in
    // the layout so that `AbsolutePosition` means what a script expects.
    const std::array<core::f32, 4> screenToClip{2.0f / viewport.x, -2.0f / viewport.y, -1.0f, 1.0f};
    const std::array<rhi::BufferHandle, 1> buffers{vertices_};

    rhi::TextureHandle bound{};
    rhi::PipelineHandle pipeline{};
    for (const UiScissorRun& run : runs_) {
        if (run.vertexCount == 0 || run.group != group)
            continue;

        // The pipeline, and with it everything bound through it: a run that
        // shows a picture blends differently (ADR 0128), and those are rare
        // enough that the switch is paid a handful of times a frame.
        const rhi::PipelineHandle wanted =
            run.premultiplied && premultipliedPipeline_.valid() ? premultipliedPipeline_ : pipeline_;
        if (!(wanted == pipeline)) {
            cmd.setPipeline(wanted);
            cmd.bindUniforms(rhi::ShaderStage::Vertex, 0, std::as_bytes(std::span{screenToClip}));
            cmd.bindVertexBuffers(0, buffers);
            pipeline = wanted;
            bound = {};
        }

        // Rebound only when it CHANGES. Most frames are one texture -- white for
        // the panels, the atlas for the text -- so this is two binds rather than
        // one per run, and a run already breaks on a texture change.
        const rhi::TextureHandle texture = run.texture.valid() ? run.texture : whitePixel_;
        if (!(texture == bound)) {
            // The sampler travels WITH the texture: `TextureBinding` is one
            // pair, which is SDL_GPU's own shape and the reason `rhi` has no
            // separate sampler bind.
            const std::array<rhi::TextureBinding, 2> textures{
                rhi::TextureBinding{texture, sampler_},
                rhi::TextureBinding{gradientTable_.valid() ? gradientTable_ : whitePixel_, gradientSampler_}};
            cmd.bindTextures(rhi::ShaderStage::Fragment, 0, textures);
            bound = texture;
        }

        cmd.setScissor(run.scissor);
        cmd.draw(run.vertexCount, 1, run.firstVertex, 0);
    }

    // Restored, so a later pass does not inherit a UI element's clip. A scissor
    // left in force is the kind of state leak that shows up as "half the screen
    // is missing" three passes later.
    cmd.setScissor(rhi::Rect{0, 0, static_cast<core::i32>(viewport.x), static_cast<core::i32>(viewport.y)});
    cmd.popDebugGroup();
}

} // namespace engine::render
