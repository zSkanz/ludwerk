// One drawn position per instance (ADR 0134): what hangs on a moving part is
// drawn with it between ticks, whoever draws it.
//
// The name over a character read the simulated place while the character was
// drawn between ticks, and in a match it smeared across it. These hold every
// visual consumer to the same place: the world's UI, particles, attachments,
// cameras and the 2D layer, sampled at display rates that are not the tick's.
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <vector>

#include "engine/app/picking.h"
#include "engine/app/world_host.h"
#include "engine/app/world_ui.h"
#include "engine/render/draw_poses.h"
#include "engine/render/particles.h"
#include "engine/render/render_world.h"
#include "engine/render/transform_history.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"
#include "project_fixture.h"

using namespace engine;
using engine::app::testing::bootOptions;
using engine::app::testing::Captured;
using engine::app::testing::Project;

namespace {

constexpr double TickRate = 60.0;

// A display at `hz` over the tick's clock: each frame runs the ticks that are
// due, then asks `frame` to draw at the alpha between the last one and the
// next -- what `FrameScheduler` does, without a wall clock.
template <typename Tick, typename Frame>
void drawAt(double hz, int frames, Tick&& tick, Frame&& frame)
{
    double nextTick = 0.0;
    for (int at = 0; at < frames; ++at) {
        // On the tick's clock exactly: at 120 Hz every other frame lands on a
        // tick, alpha zero, which is where the world used to jump (D253).
        const double now = static_cast<double>(at) / hz;
        while (nextTick <= now) {
            tick();
            nextTick += 1.0 / TickRate;
        }
        const double since = now - (nextTick - 1.0 / TickRate);
        frame(static_cast<core::f32>(std::clamp(since * TickRate, 0.0, 0.999999)));
    }
}

[[nodiscard]] core::InstanceId named(app::WorldHost& host, const char* name)
{
    scene::World& world = host.world();
    std::vector<core::InstanceId> all;
    world.collectDescendants(host.workspace(), all);
    for (const core::InstanceId id : all) {
        if (world.name(id) == world.atoms().lookup(name))
            return id;
    }
    return {};
}

} // namespace

TEST_CASE("what hangs on a moving part is drawn with it between ticks, and through a correction's slide (ADR 0134)")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local RunService = game:GetService("RunService")
        local eye = Instance.new("Camera")
        eye.CFrame = CFrame.lookAt(vector.create(4, 8, 12), vector.create(4, 5, -20))
        eye.Parent = workspace
        workspace.CurrentCamera = eye

        local mover = Instance.new("Part")
        mover.Name = "Mover"
        mover.Anchored = true
        mover.Size = vector.create(2, 2, 2)
        mover.Position = vector.create(0, 5, -20)
        mover.Parent = workspace

        local board = Instance.new("BillboardGui")
        board.Name = "Board"
        board.WorldOffset = vector.create(0, 2, 0)
        board.Parent = mover
        local sign = Instance.new("SurfaceGui")
        sign.Name = "Sign"
        sign.Parent = mover
        local socket = Instance.new("Attachment")
        socket.Name = "Socket"
        socket.CFrame = CFrame.new(0, 1.5, 0)
        socket.Parent = mover
        local sparks = Instance.new("ParticleEmitter")
        sparks.Rate = 3000
        sparks.Speed = 0
        sparks.SpreadAngle = 0
        sparks.Lifetime = 0.2
        sparks.Parent = socket
        local onboard = Instance.new("Camera")
        onboard.Name = "Onboard"
        onboard.Parent = mover
        local feed = Instance.new("CameraTexture")
        feed.Camera = onboard
        feed.ViewName = "onboard"
        feed.Parent = workspace

        -- Twelve metres a second, and the onboard camera kept on it by a
        -- script, as a following camera is.
        RunService.Heartbeat:Connect(function()
            mover.Position += vector.create(0.2, 0, 0)
            onboard.CFrame = mover.CFrame * CFrame.new(0, 2, 6)
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    scene::World& world = host.world();
    render::TransformHistory history;
    const auto tick = [&]() {
        history.capture(world);
        host.tick();
    };
    for (int at = 0; at < 10; ++at)
        tick();
    const core::InstanceId mover = named(host, "Mover");
    const core::InstanceId board = named(host, "Board");
    const core::InstanceId sign = named(host, "Sign");
    const core::InstanceId socket = named(host, "Socket");
    const core::InstanceId onboard = named(host, "Onboard");
    REQUIRE(mover.valid());
    REQUIRE(board.valid());
    REQUIRE(sign.valid());
    REQUIRE(socket.valid());
    REQUIRE(onboard.valid());

    render::DrawPoses poses;
    render::ParticleSystem particles;
    const core::Vec2 viewport{1280.0f, 720.0f};
    const app::ViewportRect rect{0.0f, 0.0f, viewport.x, viewport.y};
    render::RenderWorld snapshot;

    // Each item's place less the part's, in the world, per frame; and the
    // pixel each is drawn at against where the part as drawn, plus that
    // offset as it first was, puts it.
    struct Item
    {
        std::vector<core::DVec3> offsets;
        float worstPixels = 0.0f;
    };
    Item boardItem, signItem, socketItem, sparksItem, onboardItem;
    int frames = 0;
    int slid = 0;
    int ticked = 0;
    drawAt(
        144.0, 144 * 2,
        [&]() {
            tick();
            // A correction's slide in the second half: held a third of a
            // second, then decaying as a replica's does.
            if (++ticked == 60)
                history.setVisualOffset(mover, core::DVec3{0.6, -0.2, 0.3});
            else if (ticked > 80) {
                const core::DVec3 left = history.visualOffset();
                history.setVisualOffset(mover, core::DVec3{left.x * 0.6, left.y * 0.6, left.z * 0.6});
            }
        },
        [&](core::f32 alpha) {
            poses.begin(world, &history, alpha);
            render::extract(world, host.workspace(), host.lighting(), render::MeshLibrary{}, viewport.x / viewport.y,
                            0.0f, nullptr, poses, snapshot);
            REQUIRE(snapshot.camera.valid);
            particles.update(world, host.workspace(), 1.0 / 144.0, &poses);
            particles.append(snapshot);
            const auto screen = [&](core::DVec3 at) {
                const std::optional<core::Vec2> pixel = app::worldToViewport(
                    snapshot.camera.projection, snapshot.camera.view, snapshot.camera.origin, rect, at);
                REQUIRE(pixel.has_value());
                return *pixel;
            };
            const core::DVec3 part = poses.part(mover).position;
            const auto track = [&](Item& item, core::DVec3 at) {
                const core::DVec3 offset{at.x - part.x, at.y - part.y, at.z - part.z};
                item.offsets.push_back(offset);
                const core::DVec3& first = item.offsets.front();
                const core::Vec2 drawn = screen(at);
                const core::Vec2 rigid = screen(core::DVec3{part.x + first.x, part.y + first.y, part.z + first.z});
                item.worstPixels = std::max(item.worstPixels, std::hypot(drawn.x - rigid.x, drawn.y - rigid.y));
            };
            const auto fromCamera = [&](core::Vec3 relative) {
                return core::DVec3{snapshot.camera.origin.x + static_cast<double>(relative.x),
                                   snapshot.camera.origin.y + static_cast<double>(relative.y),
                                   snapshot.camera.origin.z + static_cast<double>(relative.z)};
            };

            for (const app::PlacedCanvas& placed :
                 app::placeWorldCanvases(world, host.workspace(), {}, viewport, snapshot.camera, &poses)) {
                // The canvas's middle: a billboard turns to face the camera
                // about it, so its corner is not fixed to the part and its
                // middle is.
                const app::CanvasPlacement& at = placed.placement;
                const core::Vec3 middle = at.topLeft + at.right * (at.canvas.x * 0.5f) + at.down * (at.canvas.y * 0.5f);
                if (placed.canvas == board)
                    track(boardItem, fromCamera(middle));
                if (placed.canvas == sign)
                    track(signItem, fromCamera(middle));
            }
            const core::DVec3 anchor = poses.attachment(socket).position;
            track(socketItem, anchor);
            // A spark born this frame, where the socket is drawn.
            double nearest = 1e9;
            core::DVec3 spark{};
            for (const render::RenderParticle& particle : snapshot.particles) {
                const core::DVec3 at = fromCamera(particle.position);
                const double d = std::hypot(at.x - anchor.x, at.y - anchor.y, at.z - anchor.z);
                if (d < nearest) {
                    nearest = d;
                    spark = at;
                }
            }
            CHECK(nearest < 1e-3);
            track(sparksItem, spark);
            track(onboardItem, poses.camera(onboard).position);
            ++frames;
            slid += history.visualOffset().x > 0.01 ? 1 : 0;
        });

    CHECK(frames == 288);
    // The slide really was drawn for a while, so the check covers it.
    CHECK(slid > 20);
    // Every item stays where it is on its part: to a millimetre in the world,
    // and to a pixel on the screen, on every frame.
    // Measured against where each one is put on the part -- not against its
    // own first frame, which a constant lag would pass.
    const auto offBy = [](const Item& item, core::DVec3 expected) {
        REQUIRE_FALSE(item.offsets.empty());
        double worst = 0.0;
        for (const core::DVec3& offset : item.offsets)
            worst = std::max(worst, std::hypot(offset.x - expected.x, offset.y - expected.y, offset.z - expected.z));
        CHECK(item.worstPixels <= 1.0f);
        return worst;
    };
    CHECK(boardItem.offsets.size() == 288);
    CHECK(signItem.offsets.size() == 288);
    // Its `WorldOffset`.
    CHECK(offBy(boardItem, core::DVec3{0.0, 2.0, 0.0}) < 1e-3);
    // The middle of the front face, a millimetre off it.
    CHECK(offBy(signItem, core::DVec3{0.0, 0.0, -1.001}) < 1e-3);
    // The socket's `CFrame`, and the sparks born there.
    CHECK(offBy(socketItem, core::DVec3{0.0, 1.5, 0.0}) < 1e-3);
    CHECK(offBy(sparksItem, core::DVec3{0.0, 1.5, 0.0}) < 1e-3);
    // Where the script keeps the onboard camera.
    CHECK(offBy(onboardItem, core::DVec3{0.0, 2.0, 6.0}) < 1e-3);
}

TEST_CASE("a 2D part at a steady speed moves the same distance every frame at 120 Hz (ADR 0134)")
{
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local RunService = game:GetService("RunService")
        local runner = Instance.new("Part2D")
        runner.Name = "Runner"
        runner.Anchored = true
        runner.Size = Vector2.new(1, 1)
        runner.Position = Vector2.new(0, 0)
        runner.Parent = workspace
        RunService.Heartbeat:Connect(function()
            runner.Position += Vector2.new(0.1, 0)
        end)
    )");

    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    scene::World& world = host.world();
    render::TransformHistory history;
    const auto tick = [&]() {
        history.capture(world);
        host.tick();
    };
    for (int at = 0; at < 5; ++at)
        tick();
    const core::InstanceId runner = named(host, "Runner");
    REQUIRE(runner.valid());

    render::DrawPoses poses;
    std::vector<double> xs;
    drawAt(120.0, 240, tick, [&](core::f32 alpha) {
        poses.begin(world, &history, alpha);
        xs.push_back(poses.part2d(runner).position.x);
    });
    // Six metres a second at 120 frames a second: five centimetres a frame,
    // every frame -- not ten and then none.
    REQUIRE(xs.size() == 240);
    double worst = 0.0;
    for (std::size_t at = 1; at < xs.size(); ++at)
        worst = std::max(worst, std::abs((xs[at] - xs[at - 1]) - 0.05));
    CHECK(worst < 1e-4);
}
