#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/jobs/jobs.h"
#include "engine/platform/platform.h"
#include "engine/platform/window.h"
#include "engine/rhi/backends.h"

using namespace engine::rhi;

namespace {

// A GPU device is not guaranteed on every machine this runs on -- a Linux CI
// runner has no Vulkan driver unless lavapipe is installed, and the roadmap
// already allows the render gate to fall back to the dev machine. So a missing
// device is reported and skipped, never failed: a red build that means "this
// runner has no GPU" trains people to ignore red builds.
//
// What is NOT skipped is a device that exists and then misbehaves.
//
// **The message carries `ENG_TEST_SKIP` and that is load-bearing**.
// These three cases return before asserting anything when there is no device,
// and CTest counted that as a pass -- so "the SDL3 GPU backend is tested" was
// true of a machine with a GPU and of no other, and nothing said which kind of
// machine had just run. They are registered a second time as `rhi_sdlgpu`, on
// their own, with a skip pattern matching that token: with only these cases in
// the entry, "did not run" and "skipped" become the same statement.
struct GpuFixture
{
    GpuFixture()
    {
        const auto initError = engine::platform::init({.headless = true});
        REQUIRE_MESSAGE(!initError.has_value(), (initError ? initError->detail : std::string{}));

        engine::core::EngineError error;
        device = createSdlGpuDevice({.backend = BackendId::SdlGpu, .debug = true}, &error);
        if (device == nullptr)
            MESSAGE("ENG_TEST_SKIP: no GPU device on this machine: " << error.detail);
    }

    ~GpuFixture()
    {
        device.reset();
        engine::platform::shutdown();
    }

    GpuFixture(const GpuFixture&) = delete;
    GpuFixture& operator=(const GpuFixture&) = delete;

    DeviceResult device;
};

} // namespace

TEST_SUITE_BEGIN("sdlgpu");

TEST_CASE("uniform snapshots preserve repeated values, edits, pipeline changes and timed buffer replay")
{
    GpuFixture gpu;
    if (gpu.device == nullptr)
        return;
    IDevice& device = *gpu.device;
    const ShaderFormat format = device.caps().shaderFormat;
    const char* directory = format == ShaderFormat::Dxil ? "dxil" : format == ShaderFormat::Msl ? "msl" : "spirv";
    const char* suffix = format == ShaderFormat::Dxil ? ".dxil" : format == ShaderFormat::Msl ? ".msl" : ".spv";
    const auto load = [&](const char* stage) {
        const auto path =
            std::filesystem::path(ENG_TEST_SHADER_CONTENT) / directory / (std::string("debug_line.") + stage + suffix);
        std::ifstream file(path, std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
        const auto view = std::as_bytes(std::span(bytes));
        return std::vector<std::byte>(view.begin(), view.end());
    };
    const auto vertex = load("vertex"), fragment = load("fragment");
    if (vertex.empty() || fragment.empty()) {
        MESSAGE("ENG_TEST_SKIP: build host shaders before uniform GPU validation");
        return;
    }
    const auto vs = device.createShader({.stage = ShaderStage::Vertex,
                                         .format = format,
                                         .code = vertex,
                                         .entryPoint = "VertexMain",
                                         .uniformBufferCount = 1});
    const auto fs = device.createShader(
        {.stage = ShaderStage::Fragment, .format = format, .code = fragment, .entryPoint = "FragmentMain"});
    REQUIRE(vs.valid());
    REQUIRE(fs.valid());
    const VertexBufferLayout buffers[] = {{.strideBytes = 7 * sizeof(f32)}};
    const VertexAttribute attributes[] = {
        {.location = 0, .format = VertexFormat::Float3},
        {.location = 1, .format = VertexFormat::Float4, .offsetBytes = 3 * sizeof(f32)}};
    const ColorTargetDesc targets[] = {{.format = TextureFormat::Rgba8Unorm}};
    const GraphicsPipelineDesc desc{.vertexShader = vs,
                                    .fragmentShader = fs,
                                    .vertexBuffers = buffers,
                                    .vertexAttributes = attributes,
                                    .rasterizer = {.cullMode = CullMode::None},
                                    .colorTargets = targets};
    const auto pipeline = device.createGraphicsPipeline(desc);
    const auto other = device.createGraphicsPipeline(desc);
    REQUIRE(pipeline.valid());
    REQUIRE(other.valid());
    const std::array<f32, 21> vertices{-1, -1, 0, 1, 0, 0, 1, 3, -1, 0, 1, 0, 0, 1, -1, 3, 0, 1, 0, 0, 1};
    const auto stream = device.createBuffer({.usage = BufferUsage::Vertex, .sizeBytes = sizeof(vertices)});
    const auto target = device.createTexture(
        {.format = TextureFormat::Rgba8Unorm, .usage = TextureUsage::ColorTarget, .width = 8, .height = 1});
    REQUIRE(stream.valid());
    REQUIRE(target.valid());
    for (u32 frame = 0; frame < 7; ++frame) {
        device.setPassTiming(frame == 2 || frame == 3);
        auto* cmd = device.beginFrame();
        REQUIRE(cmd != nullptr);
        if (frame == 0)
            cmd->upload(stream, std::as_bytes(std::span(vertices)), 0);
        std::array<f32, 16> matrix{};
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
        matrix[12] = frame % 2 == 0 ? 0.0f : 4.0f;
        const ColorAttachment clear[] = {{.texture = target, .clearColor = {0, 0, 0, 1}}};
        cmd->beginRenderPass({.colorAttachments = clear, .debugName = "uniform-first"});
        cmd->setPipeline(pipeline);
        cmd->setPipeline(pipeline);
        const BufferHandle streams[] = {stream};
        cmd->bindVertexBuffers(0, streams);
        cmd->bindUniforms(ShaderStage::Vertex, 0, std::as_bytes(std::span(matrix)));
        cmd->setScissor({0, 0, 4, 1});
        cmd->draw(3, 1, 0, 0);
        cmd->endRenderPass();
        const ColorAttachment loadTarget[] = {{.texture = target, .loadOp = LoadOp::Load}};
        cmd->beginRenderPass({.colorAttachments = loadTarget, .debugName = "uniform-second"});
        cmd->setPipeline(other);
        cmd->bindVertexBuffers(0, streams);
        cmd->bindUniforms(ShaderStage::Vertex, 0, std::as_bytes(std::span(matrix)));
        cmd->setScissor({4, 0, 2, 1});
        cmd->draw(3, 1, 0, 0);
        cmd->setPipeline(pipeline);
        matrix[12] = frame % 2 == 0 ? 4.0f : 0.0f;
        cmd->bindUniforms(ShaderStage::Vertex, 0, std::as_bytes(std::span(matrix)));
        cmd->setScissor({6, 0, 2, 1});
        cmd->draw(3, 1, 0, 0);
        cmd->endRenderPass();
        device.submitAndPresent();
        std::array<std::byte, 32> pixels{};
        REQUIRE(device.readTexture(target, pixels));
        for (u32 x = 0; x < 8; ++x) {
            const bool red = (x < 6) == (frame % 2 == 0);
            CHECK(pixels[x * 4] == (red ? std::byte{255} : std::byte{0}));
            CHECK(pixels[x * 4 + 1] == std::byte{0});
            CHECK(pixels[x * 4 + 3] == std::byte{255});
        }
    }
    device.destroy(target);
    device.destroy(stream);
    device.destroy(other);
    device.destroy(pipeline);
    device.destroy(fs);
    device.destroy(vs);
}

TEST_CASE("a real device reports itself and a usable shader format")
{
    GpuFixture gpu;
    if (gpu.device == nullptr)
        return;

    CHECK(gpu.device->backend() == BackendId::SdlGpu);

    // The difference that matters to a caller deciding whether a readback is
    // worth doing.
    CHECK(gpu.device->caps().rendersPixels);
    CHECK(gpu.device->caps().shaderFormat != ShaderFormat::Unknown);
}

TEST_CASE("a device asked for a headset where no runtime answers is made for the screen (ADR 0244)")
{
    // The loader pointed at a runtime that is not there -- never at the
    // machine's own, which a test has no business starting.
    const auto setRuntime = [](const char* value) {
#if defined(_WIN32)
        (void)_putenv_s("XR_RUNTIME_JSON", value);
#else
        if (std::string_view{value}.empty())
            (void)unsetenv("XR_RUNTIME_JSON");
        else
            (void)setenv("XR_RUNTIME_JSON", value, 1);
#endif
    };
    setRuntime("no-such-runtime.json");
    const auto initError = engine::platform::init({.headless = true});
    REQUIRE_MESSAGE(!initError.has_value(), (initError ? initError->detail : std::string{}));
    engine::core::EngineError error;
    DeviceResult device = createSdlGpuDevice(
        {.backend = BackendId::SdlGpu, .debug = true, .xr = {.enabled = true, .application = "rhi-tests"}}, &error);
    setRuntime("");
    if (device == nullptr) {
        MESSAGE("ENG_TEST_SKIP: no GPU device on this machine: " << error.detail);
    }
    else {
        CHECK(device->headset() == nullptr);
        CHECK(device->caps().rendersPixels);
    }
    device.reset();
    engine::platform::shutdown();
}

TEST_CASE("first light: a cleared target reads back as the colour it was cleared to")
{
    GpuFixture gpu;
    if (gpu.device == nullptr)
        return;

    IDevice& device = *gpu.device;

    // No shaders, no draws: a clear is a complete frame, and it exercises
    // exactly the path the screenshot harness depends on -- create a target,
    // open a render pass, submit, read the pixels back. If this works, the
    // tool has eyes.
    constexpr u32 kSize = 8;
    const TextureHandle target = device.createTexture({
        .format = TextureFormat::Rgba8Unorm,
        .usage = TextureUsage::ColorTarget,
        .width = kSize,
        .height = kSize,
        .debugName = "first-light",
    });
    REQUIRE(target.valid());

    // 0.5 is deliberate: it is not 0 or 255, so a channel that is dropped,
    // swizzled or written as a constant shows up rather than matching by luck.
    const std::array<ColorAttachment, 1> colors{ColorAttachment{
        .texture = target,
        .loadOp = LoadOp::Clear,
        .storeOp = StoreOp::Store,
        .clearColor = {1.0f, 0.5f, 0.0f, 1.0f},
    }};

    ICmdList* cmd = device.beginFrame();
    REQUIRE(cmd != nullptr);
    cmd->pushDebugGroup("first-light");
    cmd->beginRenderPass({.colorAttachments = colors, .debugName = "clear"});
    cmd->endRenderPass();
    cmd->popDebugGroup();
    device.submitAndPresent();

    std::vector<std::byte> pixels(static_cast<std::size_t>(kSize) * kSize * 4);
    REQUIRE(device.readTexture(target, pixels));

    const auto channel = [&pixels](std::size_t index) { return static_cast<int>(pixels[index]); };

    // Unorm8 rounding of 0.5 lands on 127 or 128 depending on the backend's
    // convention, so the green channel is checked as a range rather than a
    // value. Red, blue and alpha are exact.
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(kSize) * kSize; ++pixel) {
        const std::size_t base = pixel * 4;
        REQUIRE(channel(base + 0) == 255);
        REQUIRE(channel(base + 1) >= 127);
        REQUIRE(channel(base + 1) <= 128);
        REQUIRE(channel(base + 2) == 0);
        REQUIRE(channel(base + 3) == 255);
    }

    device.destroy(target);
}

TEST_CASE("a timed frame draws what an untimed one does, and says what each pass took (ADR 0171)")
{
    GpuFixture gpu;
    if (gpu.device == nullptr)
        return;

    IDevice& device = *gpu.device;
    constexpr u32 kSize = 8;
    const auto makeTarget = [&device](std::string_view name) {
        return device.createTexture({
            .format = TextureFormat::Rgba8Unorm,
            .usage = TextureUsage::ColorTarget,
            .width = kSize,
            .height = kSize,
            .debugName = name,
        });
    };
    const TextureHandle red = makeTarget("timed-red");
    const TextureHandle green = makeTarget("timed-green");
    REQUIRE(red.valid());
    REQUIRE(green.valid());
    const auto clearTo = [](TextureHandle target, f32 r, f32 g) {
        return std::array<ColorAttachment, 1>{ColorAttachment{
            .texture = target,
            .loadOp = LoadOp::Clear,
            .storeOp = StoreOp::Store,
            .clearColor = {r, g, 0.0f, 1.0f},
        }};
    };
    const auto frame = [&] {
        ICmdList* cmd = device.beginFrame();
        REQUIRE(cmd != nullptr);
        cmd->pushDebugGroup("first");
        const auto toRed = clearTo(red, 1.0f, 0.0f);
        cmd->beginRenderPass({.colorAttachments = toRed, .debugName = "first"});
        cmd->endRenderPass();
        cmd->popDebugGroup();
        // Two passes under one name are one entry: the frame is stopped where
        // the name changes, not at every pass.
        cmd->pushDebugGroup("second");
        const auto toBlack = clearTo(green, 0.0f, 0.0f);
        cmd->beginRenderPass({.colorAttachments = toBlack, .debugName = "second"});
        cmd->endRenderPass();
        const auto toGreen = clearTo(green, 0.0f, 1.0f);
        cmd->beginRenderPass({.colorAttachments = toGreen, .debugName = "second"});
        cmd->endRenderPass();
        // A pass with a name of its own inside a group is said under both.
        const auto again = clearTo(red, 1.0f, 0.0f);
        cmd->beginRenderPass({.colorAttachments = again, .debugName = "again"});
        cmd->endRenderPass();
        cmd->popDebugGroup();
        device.submitAndPresent();
    };

    // Untimed: nothing is said.
    frame();
    CHECK(device.passTimes().empty());

    device.setPassTiming(true);
    frame();
    const std::span<const PassTime> times = device.passTimes();
    REQUIRE(times.size() == 5);
    CHECK(times[0].name == "first");
    CHECK(times[0].submits == 1);
    CHECK(times[1].name == "second");
    CHECK(times[1].submits == 1);
    CHECK(times[2].name == "second/again");
    CHECK(times[3].name == "floor");
    // And the same fixed work, timed with every timed frame: how fast the GPU
    // was running while the passes above were timed.
    CHECK(times[4].name == "clock");
    CHECK(times[4].submits == 1);
    for (const PassTime& time : times) {
        CHECK(time.milliseconds >= 0.0);
        // A clear of sixty-four pixels: a second would be a wait that hung.
        CHECK(time.milliseconds < 1000.0);
    }

    // And the picture is the one an untimed frame draws.
    std::vector<std::byte> pixels(static_cast<std::size_t>(kSize) * kSize * 4);
    REQUIRE(device.readTexture(red, pixels));
    CHECK(static_cast<int>(pixels[0]) == 255);
    CHECK(static_cast<int>(pixels[1]) == 0);
    REQUIRE(device.readTexture(green, pixels));
    CHECK(static_cast<int>(pixels[0]) == 0);
    CHECK(static_cast<int>(pixels[1]) == 255);

    // Off again: the next frame is one buffer, and nothing is said of it.
    device.setPassTiming(false);
    frame();
    CHECK(device.passTimes().empty());

    device.destroy(red);
    device.destroy(green);
}

TEST_CASE("a timed frame's passes to a window are one entry, after every pass before them (ADR 0171)")
{
    // A window's texture is acquired on the frame's own command buffer and
    // presented by it, so what is drawn to it cannot be stopped pass by pass.
    // Where there is no display, or a window nobody sees is given no texture
    // (lavapipe), there is nothing to draw to and nothing is claimed here: the
    // case above is the one that runs everywhere.
    if (const auto initError = engine::platform::init({.headless = false}); initError.has_value()) {
        MESSAGE("no display on this machine, so no window to draw to: " << initError->detail);
        return;
    }
    struct PlatformScope
    {
        ~PlatformScope() { engine::platform::shutdown(); }
    } platformScope;

    engine::core::EngineError error;
    const auto window = engine::platform::createWindow(
        {.titleKey = ENG_TR("platform.window.title"), .width = 64, .height = 64, .visible = false}, &error);
    if (window == nullptr)
        return;
    const DeviceResult made = createSdlGpuDevice({.backend = BackendId::SdlGpu, .debug = true}, &error);
    if (made == nullptr || !made->claimWindow(*window))
        return;
    IDevice& device = *made;

    const TextureHandle world = device.createTexture({
        .format = TextureFormat::Rgba8Unorm,
        .usage = TextureUsage::ColorTarget,
        .width = 8,
        .height = 8,
        .debugName = "timed-world",
    });
    REQUIRE(world.valid());
    const auto onto = [](TextureHandle target, LoadOp load) {
        return std::array<ColorAttachment, 1>{ColorAttachment{
            .texture = target,
            .loadOp = load,
            .storeOp = StoreOp::Store,
            .clearColor = {0.0f, 0.0f, 1.0f, 1.0f},
        }};
    };

    device.setPassTiming(true);
    ICmdList* cmd = device.beginFrame();
    REQUIRE(cmd != nullptr);
    const Swapchain screen = device.acquireSwapchain(*window);
    if (screen.texture.valid()) {
        const auto pass = [&](std::string_view name, TextureHandle target, LoadOp load) {
            cmd->pushDebugGroup(name);
            const auto colors = onto(target, load);
            cmd->beginRenderPass({.colorAttachments = colors, .debugName = name});
            cmd->endRenderPass();
            cmd->popDebugGroup();
        };
        pass("world", world, LoadOp::Clear);
        pass("resolve", screen.texture, LoadOp::Clear);
        // After the first pass to the window, a pass elsewhere stays in the
        // frame's buffer: put in one of its own it would run before `resolve`.
        pass("late", world, LoadOp::Load);
        pass("ui", screen.texture, LoadOp::Load);
    }
    device.submitAndPresent();

    if (screen.texture.valid()) {
        const std::span<const PassTime> times = device.passTimes();
        REQUIRE(times.size() == 4);
        CHECK(times[0].name == "world");
        CHECK(times[1].name == "floor");
        CHECK(times[2].name == "clock");
        CHECK(times[3].name == "resolve+late+ui");
        CHECK(times[3].submits == 1);

        std::vector<std::byte> pixels(8 * 8 * 4);
        REQUIRE(device.readTexture(world, pixels));
        CHECK(static_cast<int>(pixels[2]) == 255);
    }

    device.waitIdle();
    device.destroy(world);
    device.releaseWindow(*window);
}

TEST_CASE("a window's swapchain made again presents as it did, and goes on presenting")
{
    // What a window moved to another display is given: its swapchain released
    // and made again, with the present mode it had. One display is enough to
    // say the window still presents, and presents the way it was asked.
    if (const auto initError = engine::platform::init({.headless = false}); initError.has_value()) {
        MESSAGE("no display on this machine, so no window to draw to: " << initError->detail);
        return;
    }
    struct PlatformScope
    {
        ~PlatformScope() { engine::platform::shutdown(); }
    } platformScope;

    engine::core::EngineError error;
    const auto window = engine::platform::createWindow(
        {.titleKey = ENG_TR("platform.window.title"), .width = 64, .height = 64, .visible = false}, &error);
    if (window == nullptr)
        return;
    const DeviceResult made = createSdlGpuDevice({.backend = BackendId::SdlGpu, .debug = true}, &error);
    if (made == nullptr || !made->claimWindow(*window))
        return;
    IDevice& device = *made;

    const auto frame = [&] {
        ICmdList* cmd = device.beginFrame();
        REQUIRE(cmd != nullptr);
        const Swapchain screen = device.acquireSwapchain(*window);
        if (screen.texture.valid()) {
            const std::array<ColorAttachment, 1> colors{ColorAttachment{
                .texture = screen.texture,
                .loadOp = LoadOp::Clear,
                .storeOp = StoreOp::Store,
                .clearColor = {0.0f, 1.0f, 0.0f, 1.0f},
            }};
            cmd->beginRenderPass({.colorAttachments = colors, .debugName = "screen"});
            cmd->endRenderPass();
        }
        device.submitAndPresent();
    };

    const bool unsynced = device.setVSync(*window, false);
    const PresentMode asked = device.presentMode(*window);
    frame();
    REQUIRE(device.remakeSwapchain(*window));
    CHECK(device.presentMode(*window) == asked);
    if (unsynced)
        CHECK(asked != PresentMode::Vsync);
    for (int index = 0; index < 3; ++index)
        frame();
    CHECK_FALSE(device.lost());

    device.waitIdle();
    device.releaseWindow(*window);
    // A window that is not claimed has no swapchain to make again.
    CHECK_FALSE(device.remakeSwapchain(*window));
}

TEST_CASE("resources round-trip through the device")
{
    GpuFixture gpu;
    if (gpu.device == nullptr)
        return;

    IDevice& device = *gpu.device;

    const BufferHandle vertices =
        device.createBuffer({.usage = BufferUsage::Vertex, .sizeBytes = 64, .debugName = "verts"});
    const SamplerHandle sampler = device.createSampler({.debugName = "linear"});

    CHECK(vertices.valid());
    CHECK(sampler.valid());

    SUBCASE("an upload outside a render pass is accepted")
    {
        const std::array<std::byte, 16> data{};
        ICmdList* cmd = device.beginFrame();
        cmd->upload(vertices, data, 0);
        device.submitAndPresent();
        device.waitIdle();
    }

    SUBCASE("a destroyed handle does not resolve to something else")
    {
        device.destroy(vertices);

        // Ids are never recycled, so the stale handle stays dead rather than
        // silently addressing whatever was created next.
        const BufferHandle next = device.createBuffer({.usage = BufferUsage::Vertex, .sizeBytes = 32});
        CHECK_FALSE(next == vertices);
    }

    device.destroy(sampler);
}

TEST_CASE("a pipeline made off the frame is made on a job and taken in when asked (ADR 0257)")
{
    GpuFixture gpu;
    if (gpu.device == nullptr)
        return;
    IDevice& device = *gpu.device;
    const ShaderFormat format = device.caps().shaderFormat;
    const char* directory = format == ShaderFormat::Dxil ? "dxil" : format == ShaderFormat::Msl ? "msl" : "spirv";
    const char* suffix = format == ShaderFormat::Dxil ? ".dxil" : format == ShaderFormat::Msl ? ".msl" : ".spv";
    const auto load = [&](const char* stage) {
        const auto path =
            std::filesystem::path(ENG_TEST_SHADER_CONTENT) / directory / (std::string("debug_line.") + stage + suffix);
        std::ifstream file(path, std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
        const auto view = std::as_bytes(std::span(bytes));
        return std::vector<std::byte>(view.begin(), view.end());
    };
    const auto vertex = load("vertex"), fragment = load("fragment");
    if (vertex.empty() || fragment.empty()) {
        MESSAGE("ENG_TEST_SKIP: build host shaders before the off-frame pipeline test");
        return;
    }
    const auto vs = device.createShader({.stage = ShaderStage::Vertex,
                                         .format = format,
                                         .code = vertex,
                                         .entryPoint = "VertexMain",
                                         .uniformBufferCount = 1});
    const auto fs = device.createShader(
        {.stage = ShaderStage::Fragment, .format = format, .code = fragment, .entryPoint = "FragmentMain"});
    REQUIRE(vs.valid());
    REQUIRE(fs.valid());
    const VertexBufferLayout buffers[] = {{.strideBytes = 7 * sizeof(f32)}};
    const VertexAttribute attributes[] = {
        {.location = 0, .format = VertexFormat::Float3},
        {.location = 1, .format = VertexFormat::Float4, .offsetBytes = 3 * sizeof(f32)}};
    const ColorTargetDesc targets[] = {{.format = TextureFormat::Rgba8Unorm}};
    const GraphicsPipelineDesc desc{.vertexShader = vs,
                                    .fragmentShader = fs,
                                    .vertexBuffers = buffers,
                                    .vertexAttributes = attributes,
                                    .rasterizer = {.cullMode = CullMode::None},
                                    .colorTargets = targets};
    // With its twin at four samples, as an eye's pass draws it.
    const PipelineVariant variants[] = {{.samples = 4}};
    const PipelineHandle pipeline = device.createGraphicsPipelineOffFrame(desc, variants);
    REQUIRE(pipeline.valid());
    std::optional<bool> made;
    for (int tries = 0; tries < 5000 && !made.has_value(); ++tries) {
        made = device.pipelineMade(pipeline);
        if (!made.has_value())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(made.has_value());
    CHECK(*made);
    // Taken in: asked again, it is simply there.
    CHECK(device.pipelineMade(pipeline) == std::optional<bool>{true});
    device.destroy(pipeline);

    // One destroyed while it is made is waited for and let go, not leaked
    // into a slot that says it was made.
    const PipelineHandle dropped = device.createGraphicsPipelineOffFrame(desc, variants);
    REQUIRE(dropped.valid());
    device.destroy(dropped);
    CHECK(device.pipelineMade(dropped) == std::optional<bool>{false});

    // And one whose shaders go first is made before they do.
    const PipelineHandle orphan = device.createGraphicsPipelineOffFrame(desc, {});
    device.destroy(vs);
    device.destroy(fs);
    CHECK(device.pipelineMade(orphan).has_value());
    device.destroy(orphan);
}

namespace {

// The line shaders the off-frame tests make their pipelines of, or empty
// where the host's shaders have not been built.
struct LineShaders
{
    ShaderHandle vertex;
    ShaderHandle fragment;
};

[[nodiscard]] std::optional<LineShaders> lineShaders(IDevice& device)
{
    const ShaderFormat format = device.caps().shaderFormat;
    const char* directory = format == ShaderFormat::Dxil ? "dxil" : format == ShaderFormat::Msl ? "msl" : "spirv";
    const char* suffix = format == ShaderFormat::Dxil ? ".dxil" : format == ShaderFormat::Msl ? ".msl" : ".spv";
    const auto load = [&](const char* stage) {
        const auto path =
            std::filesystem::path(ENG_TEST_SHADER_CONTENT) / directory / (std::string("debug_line.") + stage + suffix);
        std::ifstream file(path, std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
        const auto view = std::as_bytes(std::span(bytes));
        return std::vector<std::byte>(view.begin(), view.end());
    };
    const auto vertex = load("vertex"), fragment = load("fragment");
    if (vertex.empty() || fragment.empty())
        return std::nullopt;
    LineShaders shaders;
    shaders.vertex = device.createShader({.stage = ShaderStage::Vertex,
                                          .format = format,
                                          .code = vertex,
                                          .entryPoint = "VertexMain",
                                          .uniformBufferCount = 1});
    shaders.fragment = device.createShader(
        {.stage = ShaderStage::Fragment, .format = format, .code = fragment, .entryPoint = "FragmentMain"});
    if (!shaders.vertex.valid() || !shaders.fragment.valid())
        return std::nullopt;
    return shaders;
}

// Asked until it is made or `limit` passes; how long it took, or nothing.
[[nodiscard]] std::optional<std::chrono::milliseconds> madeWithin(IDevice& device, std::span<const PipelineHandle> made,
                                                                  std::chrono::milliseconds limit)
{
    const auto start = std::chrono::steady_clock::now();
    std::vector<bool> done(made.size(), false);
    while (std::chrono::steady_clock::now() - start < limit) {
        bool all = true;
        for (usize at = 0; at < made.size(); ++at) {
            if (!done[at])
                done[at] = device.pipelineMade(made[at]).has_value();
            all = all && done[at];
        }
        if (all)
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("a pipeline is made while the job system is busy with the world's work (ADR 0257)")
{
    // Every worker of the pool held by work longer than the test: a
    // pipeline made on the pool would wait for all of it. Made on the
    // device's own threads, it is made as if the pool were idle.
    GpuFixture gpu;
    if (gpu.device == nullptr)
        return;
    IDevice& device = *gpu.device;
    const std::optional<LineShaders> shaders = lineShaders(device);
    if (!shaders.has_value()) {
        MESSAGE("ENG_TEST_SKIP: build host shaders before the off-frame pipeline test");
        return;
    }
    const bool started = !engine::jobs::initialized();
    if (started)
        engine::jobs::init(2);
    std::atomic<bool> release{false};
    std::vector<engine::jobs::JobHandle> busy;
    for (u32 index = 0; index < engine::jobs::workerCount() * 4; ++index) {
        busy.push_back(engine::jobs::schedule("test.world", engine::jobs::Domain::AssetIo, [&release]() noexcept {
            const auto start = std::chrono::steady_clock::now();
            while (!release.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() - start < std::chrono::seconds(20))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }));
    }

    const VertexBufferLayout buffers[] = {{.strideBytes = 7 * sizeof(f32)}};
    const VertexAttribute attributes[] = {
        {.location = 0, .format = VertexFormat::Float3},
        {.location = 1, .format = VertexFormat::Float4, .offsetBytes = 3 * sizeof(f32)}};
    const ColorTargetDesc targets[] = {{.format = TextureFormat::Rgba8Unorm}};
    const GraphicsPipelineDesc desc{.vertexShader = shaders->vertex,
                                    .fragmentShader = shaders->fragment,
                                    .vertexBuffers = buffers,
                                    .vertexAttributes = attributes,
                                    .rasterizer = {.cullMode = CullMode::Back},
                                    .colorTargets = targets};
    const PipelineHandle pipeline[] = {device.createGraphicsPipelineOffFrame(desc, {})};
    REQUIRE(pipeline[0].valid());
    const std::optional<std::chrono::milliseconds> took = madeWithin(device, pipeline, std::chrono::seconds(5));
    // Made while every worker of the pool was still busy.
    bool poolBusy = true;
    for (const engine::jobs::JobHandle& job : busy)
        poolBusy = poolBusy && !engine::jobs::finished(job);
    release.store(true, std::memory_order_release);
    for (const engine::jobs::JobHandle& job : busy)
        engine::jobs::wait(job);
    REQUIRE(took.has_value());
    CHECK(poolBusy);
    MESSAGE("made in " << took->count() << " ms with the job system's every worker busy");
    CHECK(device.pipelineMade(pipeline[0]) == std::optional<bool>{true});
    device.destroy(pipeline[0]);
    device.destroy(shaders->vertex);
    device.destroy(shaders->fragment);
    if (started)
        engine::jobs::shutdown();
}

TEST_CASE("a scene's burst of new materials is made by two threads, within a bound (ADR 0257)")
{
    // Twenty pipelines asked for in one frame, as a scene that brings twenty
    // materials does, on a device given two pipeline threads: all made, and
    // in a time a loading screen covers.
    const auto initError = engine::platform::init({.headless = true});
    REQUIRE_MESSAGE(!initError.has_value(), (initError ? initError->detail : std::string{}));
    struct PlatformScope
    {
        ~PlatformScope() { engine::platform::shutdown(); }
    } platformScope;
    engine::core::EngineError error;
    const DeviceResult made =
        createSdlGpuDevice({.backend = BackendId::SdlGpu, .debug = true, .pipelineThreads = 2}, &error);
    if (made == nullptr) {
        MESSAGE("ENG_TEST_SKIP: no GPU device on this machine: " << error.detail);
        return;
    }
    IDevice& device = *made;
    const std::optional<LineShaders> shaders = lineShaders(device);
    if (!shaders.has_value()) {
        MESSAGE("ENG_TEST_SKIP: build host shaders before the off-frame pipeline test");
        return;
    }
    const VertexBufferLayout buffers[] = {{.strideBytes = 7 * sizeof(f32)}};
    const VertexAttribute attributes[] = {
        {.location = 0, .format = VertexFormat::Float3},
        {.location = 1, .format = VertexFormat::Float4, .offsetBytes = 3 * sizeof(f32)}};
    // Pipelines that differ, so that the driver makes each: the target's
    // format, the faces culled and the depth test, every combination.
    const TextureFormat formats[] = {TextureFormat::Rgba8Unorm, TextureFormat::Bgra8Unorm, TextureFormat::Rgba16Float,
                                     TextureFormat::Rgba8UnormSrgb, TextureFormat::Bgra8UnormSrgb};
    const CullMode culls[] = {CullMode::None, CullMode::Back};
    const bool depthTests[] = {false, true};
    std::vector<PipelineHandle> burst;
    for (const TextureFormat format : formats) {
        for (const CullMode cull : culls) {
            for (const bool depthTest : depthTests) {
                const ColorTargetDesc targets[] = {{.format = format}};
                GraphicsPipelineDesc desc{.vertexShader = shaders->vertex,
                                          .fragmentShader = shaders->fragment,
                                          .vertexBuffers = buffers,
                                          .vertexAttributes = attributes,
                                          .rasterizer = {.cullMode = cull},
                                          .colorTargets = targets};
                if (depthTest) {
                    desc.depthStencilFormat = TextureFormat::D32Float;
                    desc.depthStencil.depthTest = true;
                    desc.depthStencil.depthWrite = true;
                }
                burst.push_back(device.createGraphicsPipelineOffFrame(desc, {}));
                REQUIRE(burst.back().valid());
            }
        }
    }
    REQUIRE(burst.size() == 20);
    const std::optional<std::chrono::milliseconds> took = madeWithin(device, burst, std::chrono::seconds(30));
    REQUIRE(took.has_value());
    MESSAGE("twenty pipelines made in " << took->count() << " ms on two threads");
    for (const PipelineHandle pipeline : burst) {
        CHECK(device.pipelineMade(pipeline) == std::optional<bool>{true});
        device.destroy(pipeline);
    }
    device.destroy(shaders->vertex);
    device.destroy(shaders->fragment);
}

TEST_SUITE_END();
