#include <array>
#include <cstddef>
#include <doctest/doctest.h>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/i18n.h"
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
// **The message carries `ENG_TEST_SKIP` and that is load-bearing** (S7.10).
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

TEST_CASE("first light: a cleared target reads back as the colour it was cleared to")
{
    GpuFixture gpu;
    if (gpu.device == nullptr)
        return;

    IDevice& device = *gpu.device;

    // No shaders, no draws: a clear is a complete frame, and it exercises
    // exactly the path the screenshot harness depends on -- create a target,
    // open a render pass, submit, read the pixels back. If this works, the
    // agent has eyes.
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

TEST_SUITE_END();
