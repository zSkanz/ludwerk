#include <array>
#include <cstring>
#include <d3d12sdklayers.h>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "../src/d3d12/commands.h"
#include "../src/d3d12/context.h"
#include "../src/d3d12/descriptors.h"
#include "../src/d3d12/resources.h"
#include "engine/rhi/backends.h"

namespace {
using engine::rhi::d3d12::Context;
using Microsoft::WRL::ComPtr;

TEST_CASE("native D3D12 particle palette samples all seven pictures and preserves alpha order in one draw")
{
    using namespace engine::rhi;
    using namespace engine::rhi::d3d12;
    const auto load = [](const char* name) {
        std::ifstream file(std::filesystem::path(ENG_TEST_NATIVE_SHADERS) / name, std::ios::binary);
        const std::vector<char> chars{std::istreambuf_iterator<char>(file), {}};
        const auto bytes = std::as_bytes(std::span(chars));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    };
    const auto vertex = load("particle.vertex.dxil"), fragment = load("particle.fragment.dxil");
    REQUIRE_FALSE(vertex.empty());
    REQUIRE_FALSE(fragment.empty());
    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter> software;
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&software))));
    Context context;
    REQUIRE(SUCCEEDED(context.initialize(true, software.Get())));
    Resources resources(context);
    Pipelines pipelines(context);
    Descriptors descriptors(context, resources);
    REQUIRE(SUCCEEDED(descriptors.initialize()));
    Commands commands(context, resources, pipelines, descriptors);
    REQUIRE(SUCCEEDED(commands.initialize()));
    const auto vs = pipelines.createShader(
        {.stage = ShaderStage::Vertex, .format = ShaderFormat::Dxil, .code = vertex, .uniformBufferCount = 1});
    const auto fs = pipelines.createShader({.stage = ShaderStage::Fragment,
                                            .format = ShaderFormat::Dxil,
                                            .code = fragment,
                                            .samplerCount = 8,
                                            .uniformBufferCount = 1});
    const VertexBufferLayout buffers[] = {{.strideBytes = 64, .perInstance = true}};
    const VertexAttribute attributes[] = {{.location = 0, .format = VertexFormat::Float4, .offsetBytes = 0},
                                          {.location = 1, .format = VertexFormat::Float4, .offsetBytes = 16},
                                          {.location = 2, .format = VertexFormat::Float4, .offsetBytes = 32},
                                          {.location = 3, .format = VertexFormat::Float4, .offsetBytes = 48}};
    const ColorTargetDesc targets[] = {
        {.format = TextureFormat::Rgba8Unorm,
         .blend = {.enabled = true, .srcColor = BlendFactor::One, .dstColor = BlendFactor::OneMinusSrcAlpha}}};
    const auto pipeline = pipelines.createGraphics({.vertexShader = vs,
                                                    .fragmentShader = fs,
                                                    .vertexBuffers = buffers,
                                                    .vertexAttributes = attributes,
                                                    .rasterizer = {.cullMode = CullMode::None},
                                                    .colorTargets = targets});
    REQUIRE(pipeline.valid());
    const auto target = resources.createTexture(
        {.format = TextureFormat::Rgba8Unorm, .usage = TextureUsage::ColorTarget, .width = 8, .height = 1});
    const auto depth = resources.createTexture(
        {.format = TextureFormat::R32Float, .usage = TextureUsage::Sampled, .width = 1, .height = 1});
    std::array<TextureHandle, 7> pictures;
    for (auto& picture : pictures)
        picture = resources.createTexture(
            {.format = TextureFormat::Rgba8Unorm, .usage = TextureUsage::Sampled, .width = 1, .height = 1});
    std::array<std::array<float, 16>, 10> instances{};
    for (u32 at = 0; at < instances.size(); ++at) {
        auto& particle = instances[at];
        particle[0] = -1.0f + (static_cast<float>((std::min)(at, 7u)) + 0.5f) * 0.25f;
        particle[3] = 0.25f;
        particle[4] = particle[5] = particle[6] = 1.0f;
        particle[7] = at < 7 ? 1.0f : 0.5f;
        particle[11] = static_cast<float>(at < 7 ? at + 1 : at == 8 ? 2 : 7);
        particle[14] = particle[15] = 1.0f;
    }
    const auto stream = resources.createBuffer({.usage = BufferUsage::Vertex, .sizeBytes = sizeof(instances)});
    std::array<float, 24> camera{};
    camera[0] = camera[5] = camera[10] = camera[15] = camera[16] = camera[21] = 1.0f;
    std::array<float, 20> lighting{};
    lighting[0] = lighting[1] = lighting[2] = 1.0f;
    lighting[16] = -1.0f;
    lighting[17] = 10.0f;
    lighting[18] = 1.0f / 8.0f;
    lighting[19] = 1.0f;
    const float sceneDepth = 1.0f;
    const auto sampler = descriptors.createSampler({.minFilter = Filter::Nearest, .magFilter = Filter::Nearest});
    REQUIRE(context.begin() == S_OK);
    commands.uploadTexture(depth, std::as_bytes(std::span(&sceneDepth, 1)), 0);
    for (u32 slot = 0; slot < pictures.size(); ++slot) {
        const std::array<u8, 4> pixel{static_cast<u8>((slot + 1) * 20), static_cast<u8>((slot + 1) * 25),
                                      static_cast<u8>((slot + 1) * 30), 255};
        commands.uploadTexture(pictures[slot], std::as_bytes(std::span(pixel)), 0);
    }
    commands.upload(stream, std::as_bytes(std::span(instances)), 0);
    const ColorAttachment attachments[] = {{.texture = target, .clearColor = {0, 0, 0, 0}}};
    commands.beginRenderPass({.colorAttachments = attachments});
    commands.setViewport({.width = 8, .height = 1});
    commands.setScissor({0, 0, 8, 1});
    commands.setPipeline(pipeline);
    commands.bindUniforms(ShaderStage::Vertex, 0, std::as_bytes(std::span(camera)));
    commands.bindUniforms(ShaderStage::Fragment, 0, std::as_bytes(std::span(lighting)));
    std::array<TextureBinding, 8> bindings;
    bindings[0] = {depth, sampler};
    for (u32 slot = 0; slot < pictures.size(); ++slot)
        bindings[slot + 1] = {pictures[slot], sampler};
    commands.bindTextures(ShaderStage::Fragment, 0, bindings);
    const BufferHandle streams[] = {stream};
    commands.bindVertexBuffers(0, streams);
    commands.draw(6, 10, 0, 0);
    commands.endRenderPass();
    REQUIRE(commands.status() == S_OK);
    REQUIRE(SUCCEEDED(context.submit(false)));
    std::array<u8, 32> result{};
    REQUIRE(SUCCEEDED(resources.readTexture(target, std::as_writable_bytes(std::span(result)))));
    for (u32 slot = 0; slot < 7; ++slot) {
        for (u32 channel = 0; channel < 3; ++channel)
            CHECK(result[slot * 4 + channel] == (slot + 1) * (20 + 5 * channel));
        CHECK(result[slot * 4 + 3] == 255);
    }
    // Red/green/red texture slots blend in submitted instance order.
    CHECK(result[28] == doctest::Approx(97.5).epsilon(0.02));
    CHECK(result[29] == doctest::Approx(121.875).epsilon(0.02));
    CHECK(result[30] == doctest::Approx(146.25).epsilon(0.02));
    CHECK(result[31] == doctest::Approx(223.125).epsilon(0.02));
}

TEST_CASE("native D3D12 owns queue, rotates allocators and completes real GPU copies")
{
    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter> software;
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&software))));
    Context context;
    REQUIRE(SUCCEEDED(context.initialize(true, software.Get())));
    CHECK(context.backbuffer() == nullptr);
    CHECK(context.begin() == S_OK);
    CHECK(context.begin() == E_UNEXPECTED);
    REQUIRE(SUCCEEDED(context.submit(false)));
    CHECK(context.submit(false) == E_UNEXPECTED);
    context.suspend(true);
    CHECK(context.begin() == S_FALSE);
    CHECK_FALSE(context.recording());
    context.suspend(false);

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload;
    REQUIRE(SUCCEEDED(context.device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))));
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    REQUIRE(SUCCEEDED(context.device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))));
    std::array<std::uint32_t, 64> expected{};
    for (std::uint32_t index = 0; index < expected.size(); ++index)
        expected[index] = index * 17 + 23;
    void* mapped = nullptr;
    D3D12_RANGE empty{0, 0};
    REQUIRE(SUCCEEDED(upload->Map(0, &empty, &mapped)));
    std::memcpy(mapped, expected.data(), sizeof(expected));
    upload->Unmap(0, nullptr);
    // More submissions than allocators: reuse must wait for its own fence,
    // rather than relying on an idle GPU or on the test's final readback wait.
    for (unsigned frame = 0; frame < 12; ++frame) {
        REQUIRE(context.begin() == S_OK);
        context.commands()->CopyBufferRegion(readback.Get(), 0, upload.Get(), 0, sizeof(expected));
        REQUIRE(SUCCEEDED(context.submit(false)));
    }
    REQUIRE(SUCCEEDED(context.waitIdle()));
    D3D12_RANGE range{0, sizeof(expected)};
    REQUIRE(SUCCEEDED(readback->Map(0, &range, &mapped)));
    CHECK(std::memcmp(mapped, expected.data(), sizeof(expected)) == 0);
    readback->Unmap(0, &empty);
    CHECK(context.status() == S_OK);
    REQUIRE(context.begin() == S_OK);
    context.commands()->CopyBufferRegion(readback.Get(), 0, upload.Get(), sizeof(std::uint32_t), sizeof(expected) - 4);
    context.abandon();
    REQUIRE(SUCCEEDED(context.waitIdle()));
    REQUIRE(SUCCEEDED(readback->Map(0, &range, &mapped)));
    CHECK(std::memcmp(mapped, expected.data(), sizeof(expected)) == 0);
    readback->Unmap(0, &empty);
    CHECK(context.begin() == E_UNEXPECTED);
}
} // namespace

TEST_CASE("native D3D12 resources transfer padded rows, regions and GPU render targets")
{
    using namespace engine::rhi;
    using engine::rhi::d3d12::Resources;
    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter> software;
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&software))));
    Context context;
    REQUIRE(SUCCEEDED(context.initialize(true, software.Get())));
    Resources resources(context);
    const auto buffer = resources.createBuffer({.usage = BufferUsage::Vertex, .sizeBytes = 64});
    REQUIRE(buffer.valid());
    const auto texture = resources.createTexture({.format = TextureFormat::Rgba8Unorm,
                                                  .usage = TextureUsage::Sampled | TextureUsage::ColorTarget,
                                                  .width = 7,
                                                  .height = 5,
                                                  .mipLevels = 3});
    REQUIRE(texture.valid());
    std::array<std::byte, 64> data{};
    for (std::size_t index = 0; index < data.size(); ++index)
        data[index] = static_cast<std::byte>(index * 3);
    std::array<std::byte, 140> pixels{};
    for (std::size_t index = 0; index < pixels.size(); ++index)
        pixels[index] = static_cast<std::byte>(index);
    std::array<std::byte, 24> patch{};
    patch.fill(std::byte{0xA7});
    REQUIRE(context.begin() == S_OK);
    CHECK(SUCCEEDED(resources.upload(buffer, data, 0)));
    CHECK(FAILED(resources.upload(buffer, data, 1)));
    CHECK(SUCCEEDED(resources.uploadTexture(texture, pixels, 0)));
    CHECK(SUCCEEDED(resources.uploadTextureRegion(texture, 2, 1, 3, 2, patch)));
    CHECK(FAILED(resources.uploadTextureRegion(texture, 6, 1, 3, 2, patch)));
    CHECK(FAILED(resources.collect()));
    REQUIRE(SUCCEEDED(context.submit(false)));
    std::array<std::byte, 64> copied{};
    REQUIRE(SUCCEEDED(resources.readBuffer(buffer, 0, copied)));
    CHECK(copied == data);
    std::array<std::byte, 140> image{};
    REQUIRE(SUCCEEDED(resources.readTexture(texture, image)));
    for (unsigned y = 0; y < 2; ++y)
        std::memcpy(pixels.data() + ((y + 1) * 7 + 2) * 4, patch.data() + y * 12, 12);
    CHECK(image == pixels);

    REQUIRE(SUCCEEDED(resources.collect()));
    REQUIRE(context.begin() == S_OK);
    auto* target = resources.texture(texture);
    REQUIRE(target != nullptr);
    resources.transition(*target, D3D12_RESOURCE_STATE_RENDER_TARGET, 0);
    const float clear[] = {0.25f, 0.5f, 1, 1};
    context.commands()->ClearRenderTargetView(target->target(), clear, 0, nullptr);
    REQUIRE(SUCCEEDED(context.submit(false)));
    REQUIRE(SUCCEEDED(resources.readTexture(texture, image)));
    for (unsigned index = 0; index < image.size(); index += 4) {
        CHECK(std::to_integer<unsigned>(image[index]) == 64);
        CHECK(std::to_integer<unsigned>(image[index + 1]) == 128);
        CHECK(image[index + 2] == std::byte{255});
        CHECK(image[index + 3] == std::byte{255});
    }
    CHECK(resources.memory().textureBytes == 168);
    CHECK(resources.memory().bufferBytes == 64);
    REQUIRE(context.begin() == S_OK);
    REQUIRE(SUCCEEDED(resources.upload(buffer, data, 0)));
    resources.destroy(buffer);
    CHECK(resources.buffer(buffer) == nullptr);
    resources.destroy(texture);
    CHECK(resources.texture(texture) == nullptr);
    REQUIRE(SUCCEEDED(context.submit(false)));
    REQUIRE(SUCCEEDED(resources.collect()));
    CHECK(resources.memory().textureBytes == 0);
    CHECK(resources.memory().bufferBytes == 0);
    const auto fresh = resources.createBuffer({.usage = BufferUsage::Vertex, .sizeBytes = 64});
    CHECK(fresh.valid());
    CHECK(fresh != buffer);
}

TEST_CASE("native D3D12 shipping fullscreen shaders sample native descriptors and render real pixels")
{
    using namespace engine::rhi;
    using namespace engine::rhi::d3d12;
    const std::filesystem::path directory(ENG_TEST_NATIVE_SHADERS);
    const auto load = [&](const char* name) {
        std::ifstream file(directory / name, std::ios::binary);
        const std::vector<char> chars{std::istreambuf_iterator<char>(file), {}};
        const auto bytes = std::as_bytes(std::span(chars));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    };
    const auto vertex = load("look_resample.vertex.dxil"), fragment = load("look_resample.fragment.dxil");
    if (vertex.empty() || fragment.empty()) {
        MESSAGE("ENG_TEST_SKIP: build the host shader content before the native shader validation");
        return;
    }
    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter> software;
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&software))));
    Context context;
    REQUIRE(SUCCEEDED(context.initialize(true, software.Get())));
    Resources resources(context);
    Pipelines pipelines(context);
    Descriptors descriptors(context, resources);
    REQUIRE(SUCCEEDED(descriptors.initialize()));
    Commands commands(context, resources, pipelines, descriptors);
    REQUIRE(SUCCEEDED(commands.initialize()));
    const auto vs =
        pipelines.createShader({.stage = ShaderStage::Vertex, .format = ShaderFormat::Dxil, .code = vertex});
    const auto fs = pipelines.createShader(
        {.stage = ShaderStage::Fragment, .format = ShaderFormat::Dxil, .code = fragment, .samplerCount = 1});
    REQUIRE(vs.valid());
    REQUIRE(fs.valid());
    const ColorTargetDesc targets[] = {{.format = TextureFormat::Rgba8Unorm}};
    const auto pipeline = pipelines.createGraphics({.vertexShader = vs,
                                                    .fragmentShader = fs,
                                                    .rasterizer = {.cullMode = CullMode::None},
                                                    .colorTargets = targets});
    REQUIRE(pipeline.valid());
    auto* native = pipelines.graphics(pipeline);
    REQUIRE(native != nullptr);
    CHECK(native->stages[0].readCount == 0);
    CHECK(native->stages[1].samplerCount == 1);
    CHECK(native->stages[1].readCount == 1);
    const auto source = resources.createTexture(
        {.format = TextureFormat::Rgba8Unorm, .usage = TextureUsage::Sampled, .width = 1, .height = 1});
    const auto secondSource = resources.createTexture(
        {.format = TextureFormat::Rgba8Unorm, .usage = TextureUsage::Sampled, .width = 1, .height = 1});
    const auto indices = resources.createBuffer({.usage = BufferUsage::Index, .sizeBytes = 6});
    const auto indirect = resources.createBuffer({.usage = BufferUsage::Indirect, .sizeBytes = 20});
    const auto target = resources.createTexture(
        {.format = TextureFormat::Rgba8Unorm, .usage = TextureUsage::ColorTarget, .width = 8, .height = 8});
    REQUIRE(source.valid());
    REQUIRE(target.valid());
    const auto sampler = descriptors.createSampler({.minFilter = Filter::Nearest, .magFilter = Filter::Nearest});
    const std::array<std::byte, 4> pixel{std::byte{37}, std::byte{149}, std::byte{231}, std::byte{255}};
    const std::array<std::byte, 4> secondPixel{std::byte{191}, std::byte{83}, std::byte{17}, std::byte{255}};
    const std::array<u16, 3> triangle{0, 1, 2};
    const std::array<DrawIndexedIndirectCommand, 1> arguments{{{3, 1, 0, 0, 0}}};
    REQUIRE(context.begin() == S_OK);
    commands.uploadTexture(source, pixel, 0);
    commands.uploadTexture(secondSource, secondPixel, 0);
    commands.upload(indices, std::as_bytes(std::span(triangle)), 0);
    commands.upload(indirect, std::as_bytes(std::span(arguments)), 0);
    const ColorAttachment attachments[] = {{.texture = target}};
    commands.beginRenderPass({.colorAttachments = attachments});
    commands.setPipeline(pipeline);
    commands.setScissor({0, 0, 4, 8});
    const TextureBinding firstBinding[] = {{source, sampler}};
    commands.bindTextures(ShaderStage::Fragment, 0, firstBinding);
    commands.draw(3, 1, 0, 0);
    // Rebinding cannot overwrite descriptors still used by an earlier draw.
    // The other half also exercises real indexed indirect argument execution.
    const TextureBinding secondBinding[] = {{secondSource, sampler}};
    commands.bindTextures(ShaderStage::Fragment, 0, secondBinding);
    commands.setScissor({4, 0, 4, 8});
    commands.bindIndexBuffer(indices, IndexType::U16);
    commands.drawIndexedIndirect(indirect, 0, 1);
    commands.endRenderPass();
    REQUIRE(commands.status() == S_OK);
    REQUIRE(SUCCEEDED(context.submit(false)));
    std::array<std::byte, 256> image{};
    REQUIRE(SUCCEEDED(resources.readTexture(target, image)));
    for (std::size_t index = 0; index < image.size(); ++index)
        CHECK(image[index] == (((index / 4) % 8) < 4 ? pixel : secondPixel)[index % 4]);
    pipelines.destroy(pipeline);
    CHECK(pipelines.graphics(pipeline) == nullptr);
    REQUIRE(SUCCEEDED(pipelines.collect()));

    // Invalid argument ranges are reported before submission; shutting down
    // must discard this recording, never execute potentially incomplete work.
    descriptors.reset();
    commands.reset();
    REQUIRE(context.begin() == S_OK);
    commands.bindTextures(ShaderStage::Fragment, 16, firstBinding);
    CHECK(commands.status() == E_INVALIDARG);
    context.abandon();
    CHECK_FALSE(context.recording());
    CHECK(context.begin() == E_UNEXPECTED);
    CHECK(SUCCEEDED(context.waitIdle()));
}

TEST_CASE("native D3D12 read tables survive reuse, intervening writes and fenced frame rotation")
{
    using namespace engine::rhi;
    using namespace engine::rhi::d3d12;
    const auto load = [](const char* name) {
        std::ifstream file(std::filesystem::path(ENG_TEST_NATIVE_SHADERS) / name, std::ios::binary);
        const std::vector<char> chars{std::istreambuf_iterator<char>(file), {}};
        const auto bytes = std::as_bytes(std::span(chars));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    };
    const auto vertex = load("look_resample.vertex.dxil"), fragment = load("look_resample.fragment.dxil");
    REQUIRE_FALSE(vertex.empty());
    REQUIRE_FALSE(fragment.empty());
    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter> software;
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&software))));
    Context context;
    REQUIRE(SUCCEEDED(context.initialize(true, software.Get())));
    Resources resources(context);
    Pipelines pipelines(context);
    Descriptors descriptors(context, resources);
    REQUIRE(SUCCEEDED(descriptors.initialize()));
    Commands commands(context, resources, pipelines, descriptors);
    REQUIRE(SUCCEEDED(commands.initialize()));
    const auto vs =
        pipelines.createShader({.stage = ShaderStage::Vertex, .format = ShaderFormat::Dxil, .code = vertex});
    const auto fs = pipelines.createShader(
        {.stage = ShaderStage::Fragment, .format = ShaderFormat::Dxil, .code = fragment, .samplerCount = 1});
    const ColorTargetDesc formats[] = {{.format = TextureFormat::Rgba8Unorm}};
    const auto pipeline = pipelines.createGraphics({.vertexShader = vs,
                                                    .fragmentShader = fs,
                                                    .rasterizer = {.cullMode = CullMode::None},
                                                    .colorTargets = formats});
    REQUIRE(pipeline.valid());
    const auto source = resources.createTexture({.format = TextureFormat::Rgba8Unorm,
                                                 .usage = TextureUsage::Sampled | TextureUsage::ColorTarget,
                                                 .width = 1,
                                                 .height = 1});
    const auto target = resources.createTexture(
        {.format = TextureFormat::Rgba8Unorm, .usage = TextureUsage::ColorTarget, .width = 2, .height = 1});
    REQUIRE(source.valid());
    REQUIRE(target.valid());
    const auto sampler = descriptors.createSampler({.minFilter = Filter::Nearest, .magFilter = Filter::Nearest});
    const TextureBinding binding[] = {{source, sampler}};
    for (u32 frame = 0; frame < 7; ++frame) {
        REQUIRE(SUCCEEDED(resources.collect()));
        REQUIRE(context.begin() == S_OK);
        descriptors.reset();
        commands.reset();
        const std::array<u8, 4> pixel{static_cast<u8>(31 + frame), 127, 231, 255};
        commands.uploadTexture(source, std::as_bytes(std::span(pixel)), 0);
        const ColorAttachment output[] = {{.texture = target, .loadOp = LoadOp::Load}};
        commands.beginRenderPass({.colorAttachments = output});
        commands.setPipeline(pipeline);
        commands.bindTextures(ShaderStage::Fragment, 0, binding);
        commands.setScissor({0, 0, 1, 1});
        commands.draw(3, 1, 0, 0);
        commands.endRenderPass();
        // Alter the sampled resource without altering its identity. A cache hit
        // must restore its SRV state rather than skipping resource transitions.
        const ColorAttachment clear[] = {{.texture = source, .clearColor = {0, 1, 0, 1}}};
        commands.beginRenderPass({.colorAttachments = clear});
        commands.endRenderPass();
        commands.beginRenderPass({.colorAttachments = output});
        commands.setScissor({1, 0, 1, 1});
        commands.draw(3, 1, 0, 0);
        commands.endRenderPass();
        // A repeated table must not consume a new descriptor per bind. This
        // exceeds the heap capacity while recording no additional GPU draws.
        Bindings repeated;
        repeated.textures[0] = binding[0];
        for (u32 repeat = 0; repeat < 66000; ++repeat) {
            const HRESULT status = descriptors.bind(pipelines.graphics(pipeline)->stages[1], repeated, false);
            if (FAILED(status)) {
                REQUIRE(status == S_OK);
                break;
            }
        }
        REQUIRE(commands.status() == S_OK);
        REQUIRE(SUCCEEDED(context.submit(false)));
        std::array<u8, 8> actual{};
        REQUIRE(SUCCEEDED(resources.readTexture(target, std::as_writable_bytes(std::span(actual)))));
        for (u32 channel = 0; channel < 4; ++channel)
            CHECK(actual[channel] == pixel[channel]);
        CHECK(actual[4] == 0);
        CHECK(actual[5] == 255);
        CHECK(actual[6] == 0);
        CHECK(actual[7] == 255);
    }
    ComPtr<ID3D12InfoQueue> messages;
    REQUIRE(SUCCEEDED(context.device()->QueryInterface(IID_PPV_ARGS(&messages))));
    for (UINT64 at = 0; at < messages->GetNumStoredMessages(); ++at) {
        SIZE_T size = 0;
        REQUIRE(SUCCEEDED(messages->GetMessage(at, nullptr, &size)));
        std::vector<std::byte> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        REQUIRE(SUCCEEDED(messages->GetMessage(at, message, &size)));
        INFO(message->pDescription);
        CHECK(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR);
        CHECK(message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION);
    }
}

TEST_CASE("native D3D12 shipping blits preserve alpha and generate isolated array mipmaps")
{
    using namespace engine::rhi;
    using namespace engine::rhi::d3d12;
    const auto load = [](const char* name) {
        std::ifstream file(std::filesystem::path(ENG_TEST_NATIVE_SHADERS) / name, std::ios::binary);
        const std::vector<char> code{std::istreambuf_iterator<char>(file), {}};
        const auto bytes = std::as_bytes(std::span(code));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    };
    const auto vertex = load("rhi_blit.vertex.dxil"), fragment = load("rhi_blit.fragment.dxil");
    if (vertex.empty() || fragment.empty()) {
        MESSAGE("ENG_TEST_SKIP: build host native blit shaders before blit validation");
        return;
    }
    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter> software;
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&software))));
    Context context;
    REQUIRE(SUCCEEDED(context.initialize(true, software.Get())));
    Resources resources(context);
    Pipelines pipelines(context);
    Descriptors descriptors(context, resources);
    REQUIRE(SUCCEEDED(descriptors.initialize()));
    Commands commands(context, resources, pipelines, descriptors);
    REQUIRE(SUCCEEDED(commands.initialize()));
    REQUIRE(SUCCEEDED(commands.initializeBlit(vertex, fragment)));
    const auto source = resources.createTexture(
        {.format = TextureFormat::Rgba8Unorm, .usage = TextureUsage::Sampled, .width = 2, .height = 2});
    const auto target = resources.createTexture({.format = TextureFormat::Rgba8Unorm,
                                                 .usage = TextureUsage::Sampled | TextureUsage::ColorTarget,
                                                 .width = 4,
                                                 .height = 4,
                                                 .layers = 2,
                                                 .mipLevels = 3});
    // Different channels and alpha at every corner. Bilinear filtering of the
    // complete chain must converge to their average, independently per layer.
    const std::array<u8, 16> corners{0, 40, 80, 0, 80, 120, 160, 80, 160, 200, 240, 160, 240, 120, 0, 240};
    REQUIRE(context.begin() == S_OK);
    commands.uploadTexture(source, std::as_bytes(std::span(corners)), 0);
    commands.blitTexture(source, target, 1);
    const ColorAttachment layerZero[] = {{.texture = target, .clearColor = {0, 1, 0, 0.5f}, .layer = 0}};
    commands.beginRenderPass({.colorAttachments = layerZero});
    commands.endRenderPass();
    commands.generateMipmaps(target);
    REQUIRE(commands.status() == S_OK);
    REQUIRE(SUCCEEDED(context.submit(false)));
    std::array<std::byte, 4> pixel{};
    REQUIRE(SUCCEEDED(resources.readTexture(target, pixel, 2, 1)));
    const std::array<unsigned, 4> expected{120, 120, 120, 120};
    for (unsigned channel = 0; channel < 4; ++channel)
        CHECK(std::abs(static_cast<int>(std::to_integer<unsigned>(pixel[channel])) -
                       static_cast<int>(expected[channel])) <= 1);
    REQUIRE(SUCCEEDED(resources.readTexture(target, pixel, 2, 0)));
    CHECK(pixel == std::array<std::byte, 4>{std::byte{0}, std::byte{255}, std::byte{0}, std::byte{128}});
    REQUIRE(SUCCEEDED(resources.collect()));
    descriptors.reset();
    commands.reset();
    REQUIRE(context.begin() == S_OK);
    commands.blitTexture(source, target, 2);
    CHECK(commands.status() == E_INVALIDARG);
    context.abandon();
}

TEST_CASE(
    "native D3D12 shipping frames preserve uploads descriptors uniforms and retired objects behind a blocked queue")
{
    using namespace engine::rhi;
    using namespace engine::rhi::d3d12;
    const auto load = [](const char* name) {
        std::ifstream file(std::filesystem::path(ENG_TEST_NATIVE_SHADERS) / name, std::ios::binary);
        const std::vector<char> chars{std::istreambuf_iterator<char>(file), {}};
        const auto bytes = std::as_bytes(std::span(chars));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    };
    const auto vertex = load("look_resample.vertex.dxil"), fragment = load("look_resample.fragment.dxil"),
               compute = load("foliage_finalize.compute.dxil");
    REQUIRE_FALSE(vertex.empty());
    REQUIRE_FALSE(fragment.empty());
    REQUIRE_FALSE(compute.empty());
    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter> software;
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&software))));
    Context context;
    REQUIRE(SUCCEEDED(context.initialize(true, software.Get())));
    Resources resources(context);
    Pipelines pipelines(context);
    Descriptors descriptors(context, resources);
    REQUIRE(SUCCEEDED(descriptors.initialize()));
    Commands commands(context, resources, pipelines, descriptors);
    REQUIRE(SUCCEEDED(commands.initialize()));
    ComPtr<ID3D12InfoQueue> validation;
    (void)context.device()->QueryInterface(IID_PPV_ARGS(&validation));
    const auto vs =
        pipelines.createShader({.stage = ShaderStage::Vertex, .format = ShaderFormat::Dxil, .code = vertex});
    const auto fs = pipelines.createShader(
        {.stage = ShaderStage::Fragment, .format = ShaderFormat::Dxil, .code = fragment, .samplerCount = 1});
    const ColorTargetDesc colors[] = {{.format = TextureFormat::Rgba8Unorm}};
    const auto graphics = pipelines.createGraphics(
        {.vertexShader = vs, .fragmentShader = fs, .rasterizer = {.cullMode = CullMode::None}, .colorTargets = colors});
    const auto kernel = pipelines.createCompute({.format = ShaderFormat::Dxil,
                                                 .code = compute,
                                                 .readonlyStorageBufferCount = 2,
                                                 .readwriteStorageBufferCount = 1,
                                                 .uniformBufferCount = 1,
                                                 .threadCountX = 64});
    REQUIRE(graphics.valid());
    REQUIRE(kernel.valid());
    const auto sampler = descriptors.createSampler({.minFilter = Filter::Nearest, .magFilter = Filter::Nearest});
    std::array<TextureHandle, Context::FrameCount> targets;
    std::array<BufferHandle, Context::FrameCount> outputs;
    std::array<std::array<std::byte, 4>, Context::FrameCount> pixels;
    const std::array<u32, 6> entries{0, 5, 1, 7, 2, 11};
    const std::array<u32, 3> visible{2, 99, 3};
    const std::array<u32, 15> original{3, 101, 0, 0, 0, 6, 102, 3, 0, 0, 9, 103, 9, 0, 0};

    // Always unblock before Resources/Pipelines destructors wait, including a
    // failed REQUIRE. No frame can complete until all three snapshots exist.
    struct Gate
    {
        ComPtr<ID3D12Fence> fence;
        ~Gate()
        {
            if (fence)
                (void)fence->Signal(1);
        }
    } gate;
    REQUIRE(SUCCEEDED(context.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence))));
    REQUIRE(SUCCEEDED(context.queue()->Wait(gate.fence.Get(), 1)));
    for (u32 frame = 0; frame < Context::FrameCount; ++frame) {
        REQUIRE(SUCCEEDED(resources.collect()));
        REQUIRE(SUCCEEDED(pipelines.collect()));
        REQUIRE(context.begin() == S_OK);
        CHECK(context.frameSlot() == frame);
        descriptors.reset();
        commands.reset();
        const auto source = resources.createTexture(
            {.format = TextureFormat::Rgba8Unorm, .usage = TextureUsage::Sampled, .width = 1, .height = 1});
        targets[frame] = resources.createTexture(
            {.format = TextureFormat::Rgba8Unorm, .usage = TextureUsage::ColorTarget, .width = 2, .height = 2});
        const auto table = resources.createBuffer({.usage = BufferUsage::ComputeStorageRead, .sizeBytes = 24});
        const auto counters = resources.createBuffer({.usage = BufferUsage::ComputeStorageRead, .sizeBytes = 12});
        outputs[frame] = resources.createBuffer({.usage = BufferUsage::ComputeStorageWrite, .sizeBytes = 60});
        REQUIRE(source.valid());
        REQUIRE(targets[frame].valid());
        REQUIRE(table.valid());
        REQUIRE(counters.valid());
        REQUIRE(outputs[frame].valid());
        pixels[frame] = {static_cast<std::byte>(37 + frame * 61), static_cast<std::byte>(149 - frame * 33),
                         static_cast<std::byte>(231 - frame * 59), std::byte{255}};
        commands.uploadTexture(source, pixels[frame], 0);
        commands.upload(table, std::as_bytes(std::span(entries)), 0);
        commands.upload(counters, std::as_bytes(std::span(visible)), 0);
        commands.upload(outputs[frame], std::as_bytes(std::span(original)), 0);
        const ColorAttachment attachment[] = {{.texture = targets[frame]}};
        commands.beginRenderPass({.colorAttachments = attachment});
        commands.setPipeline(graphics);
        const TextureBinding binding[] = {{source, sampler}};
        commands.bindTextures(ShaderStage::Fragment, 0, binding);
        commands.draw(3, 1, 0, 0);
        commands.endRenderPass();
        const BufferHandle writes[] = {outputs[frame]}, reads[] = {table, counters};
        commands.beginComputePass(writes, {});
        commands.setComputePipeline(kernel);
        commands.bindComputeStorageBuffers(0, reads);
        std::array<u32, 4> constants{frame + 1, 0, 0, 0};
        commands.bindComputeUniforms(0, std::as_bytes(std::span(constants)));
        constants[0] = 0;
        commands.dispatch(1, 1, 1);
        commands.endComputePass();
        REQUIRE(commands.status() == S_OK);
        // These objects are referenced by the current list, not just an older
        // submitted frame. Retirement must retain them through its future fence.
        resources.destroy(source);
        resources.destroy(table);
        resources.destroy(counters);
        if (frame == 0) {
            const auto pending = context.retirementValue();
            REQUIRE(SUCCEEDED(context.waitIdle()));
            CHECK(context.retirementValue() == pending);
        }
        if (frame + 1 == Context::FrameCount) {
            pipelines.destroy(graphics);
            pipelines.destroy(kernel);
        }
        REQUIRE(SUCCEEDED(context.submit(false)));
        REQUIRE(SUCCEEDED(resources.collect()));
        REQUIRE(SUCCEEDED(pipelines.collect()));
        std::uint64_t completed = 0;
        REQUIRE(SUCCEEDED(context.completed(completed)));
        CHECK(completed == 0);
    }
    REQUIRE(SUCCEEDED(gate.fence->Signal(1)));
    REQUIRE(SUCCEEDED(context.waitIdle()));
    for (u32 frame = 0; frame < Context::FrameCount; ++frame) {
        std::array<std::byte, 16> actualPixels{};
        REQUIRE(SUCCEEDED(resources.readTexture(targets[frame], actualPixels)));
        for (std::size_t index = 0; index < actualPixels.size(); ++index)
            CHECK(actualPixels[index] == pixels[frame][index % 4]);
        std::array<u32, 15> actual{};
        REQUIRE(SUCCEEDED(resources.readBuffer(outputs[frame], 0, std::as_writable_bytes(std::span(actual)))));
        auto expected = original;
        expected[1] = 2;
        if (frame >= 1)
            expected[6] = 7;
        if (frame >= 2)
            expected[11] = 3;
        CHECK(actual == expected);
    }
    REQUIRE(SUCCEEDED(resources.collect()));
    REQUIRE(SUCCEEDED(pipelines.collect()));
    if (validation) {
        for (UINT64 index = 0; index < validation->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index) {
            SIZE_T size = 0;
            REQUIRE(SUCCEEDED(validation->GetMessage(index, nullptr, &size)));
            std::vector<std::byte> bytes(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            REQUIRE(SUCCEEDED(validation->GetMessage(index, message, &size)));
            INFO(message->pDescription);
            CHECK(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR);
        }
    }
}

TEST_CASE("native D3D12 shipping compute commands preserve uniforms and order UAV writes")
{
    using namespace engine::rhi;
    using namespace engine::rhi::d3d12;
    std::ifstream file(std::filesystem::path(ENG_TEST_NATIVE_SHADERS) / "foliage_finalize.compute.dxil",
                       std::ios::binary);
    const std::vector<char> code{std::istreambuf_iterator<char>(file), {}};
    if (code.empty()) {
        MESSAGE("ENG_TEST_SKIP: build host compute shaders before native compute validation");
        return;
    }
    ComPtr<IDXGIFactory4> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter> software;
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&software))));
    Context context;
    REQUIRE(SUCCEEDED(context.initialize(true, software.Get())));
    Resources resources(context);
    Pipelines pipelines(context);
    Descriptors descriptors(context, resources);
    REQUIRE(SUCCEEDED(descriptors.initialize()));
    Commands commands(context, resources, pipelines, descriptors);
    REQUIRE(SUCCEEDED(commands.initialize()));
    const auto pipeline = pipelines.createCompute({.format = ShaderFormat::Dxil,
                                                   .code = std::as_bytes(std::span(code)),
                                                   .readonlyStorageBufferCount = 2,
                                                   .readwriteStorageBufferCount = 1,
                                                   .uniformBufferCount = 1,
                                                   .threadCountX = 64});
    REQUIRE(pipeline.valid());
    const auto table = resources.createBuffer({.usage = BufferUsage::ComputeStorageRead, .sizeBytes = 24});
    const auto counters = resources.createBuffer({.usage = BufferUsage::ComputeStorageRead, .sizeBytes = 12});
    const auto output = resources.createBuffer({.usage = BufferUsage::ComputeStorageWrite, .sizeBytes = 60});
    REQUIRE(table.valid());
    REQUIRE(counters.valid());
    REQUIRE(output.valid());
    const std::array<u32, 6> entries{0, 5, 1, 7, 2, 11};
    const std::array<u32, 3> visible{2, 99, 3};
    const std::array<u32, 15> original{3, 101, 0, 0, 0, 6, 102, 3, 0, 0, 9, 103, 9, 0, 0};
    std::array<u32, 4> constants{3, 0, 0, 0};
    REQUIRE(context.begin() == S_OK);
    commands.upload(table, std::as_bytes(std::span(entries)), 0);
    commands.upload(counters, std::as_bytes(std::span(visible)), 0);
    commands.upload(output, std::as_bytes(std::span(original)), 0);
    const BufferHandle writes[] = {output};
    const BufferHandle reads[] = {table, counters};
    commands.beginComputePass(writes, {});
    commands.setComputePipeline(pipeline);
    commands.bindComputeStorageBuffers(0, reads);
    commands.bindComputeUniforms(0, std::as_bytes(std::span(constants)));
    // The caller's memory is not retained by bindComputeUniforms.
    constants[0] = 0;
    commands.dispatch(1, 1, 1);
    commands.dispatch(1, 1, 1);
    commands.endComputePass();
    REQUIRE(commands.status() == S_OK);
    REQUIRE(SUCCEEDED(context.submit(false)));
    std::array<u32, 15> actual{};
    REQUIRE(SUCCEEDED(resources.readBuffer(output, 0, std::as_writable_bytes(std::span(actual)))));
    auto expected = original;
    expected[1] = 2;
    expected[6] = 7;
    expected[11] = 3;
    CHECK(actual == expected);
    REQUIRE(SUCCEEDED(resources.collect()));
    descriptors.reset();
    commands.reset();
    REQUIRE(context.begin() == S_OK);
    const BufferHandle invalidWrites[] = {table};
    commands.beginComputePass(invalidWrites, {});
    commands.setComputePipeline(pipeline);
    commands.bindComputeStorageBuffers(0, reads);
    commands.dispatch(1, 1, 1);
    CHECK(commands.status() == E_INVALIDARG);
    context.abandon();
}

// The adapter itself, declared here because its header shows it only to a
// build that offers `--rhi=d3d12`; the gate runs it in every Windows build.
namespace engine::rhi {
DeviceResult createD3D12Device(const DeviceDesc& desc, std::span<const std::byte> blitVertex,
                               std::span<const std::byte> blitFragment, core::EngineError* outError);
}

TEST_CASE("native D3D12 device runs a frame through the interface a game draws by")
{
    // The parts above are tested one at a time. This is the adapter that
    // makes them an `IDevice` -- the 340 lines a game's frame actually goes
    // through, and until now the only ones of the backend no test compiled.
    using namespace engine::rhi;
    const auto load = [](const char* name) {
        std::ifstream file(std::filesystem::path(ENG_TEST_NATIVE_SHADERS) / name, std::ios::binary);
        const std::vector<char> code{std::istreambuf_iterator<char>(file), {}};
        const auto bytes = std::as_bytes(std::span(code));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    };
    const auto vertex = load("rhi_blit.vertex.dxil"), fragment = load("rhi_blit.fragment.dxil");
    if (vertex.empty() || fragment.empty()) {
        MESSAGE("ENG_TEST_SKIP: build host native blit shaders before the device's frame");
        return;
    }
    engine::core::EngineError error;
    const DeviceResult device =
        createD3D12Device({.backend = BackendId::D3D12, .debug = true}, vertex, fragment, &error);
    if (device == nullptr) {
        MESSAGE("ENG_TEST_SKIP: no Direct3D 12 device on this machine: " << error.detail);
        return;
    }
    CHECK(device->backend() == BackendId::D3D12);
    CHECK(device->caps().rendersPixels);
    CHECK_FALSE(device->lost());

    const TextureHandle target = device->createTexture({.format = TextureFormat::Rgba8Unorm,
                                                        .usage = TextureUsage::Sampled | TextureUsage::ColorTarget,
                                                        .width = 4,
                                                        .height = 4});
    REQUIRE(target.valid());

    // Two frames with no window: each records a pass that clears the target
    // to another colour, and what is read back is that frame's.
    const std::array<std::array<float, 4>, 2> colours{{{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 0.5f}}};
    const std::array<std::array<unsigned, 4>, 2> expected{{{255, 0, 0, 255}, {0, 0, 255, 128}}};
    for (std::size_t frame = 0; frame < colours.size(); ++frame) {
        ICmdList* commands = device->beginFrame();
        REQUIRE(commands != nullptr);
        const ColorAttachment attachment[] = {
            {.texture = target,
             .clearColor = {colours[frame][0], colours[frame][1], colours[frame][2], colours[frame][3]}}};
        commands->beginRenderPass({.colorAttachments = attachment});
        commands->endRenderPass();
        device->submitAndPresent();
        device->waitIdle();
        REQUIRE_FALSE(device->lost());
        std::array<std::byte, 4 * 4 * 4> pixels{};
        REQUIRE(device->readTexture(target, pixels));
        for (unsigned channel = 0; channel < 4; ++channel)
            CHECK(std::abs(static_cast<int>(std::to_integer<unsigned>(pixels[channel])) -
                           static_cast<int>(expected[frame][channel])) <= 1);
    }

    // A frame is not begun twice: the second asks while the first records.
    ICmdList* first = device->beginFrame();
    REQUIRE(first != nullptr);
    CHECK(device->beginFrame() == nullptr);
    device->submitAndPresent();
    device->waitIdle();
    device->destroy(target);
}
