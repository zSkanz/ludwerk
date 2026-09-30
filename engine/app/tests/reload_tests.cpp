// The fast world restart (ADR 0024 + addendum, M3 brief Decisions 11 and 14).
//
// The claim under test is the one the whole model rests on: a reloaded world is
// indistinguishable from one that never reloaded. Anything weaker -- "the hash
// changed" -- passes against a reload that half-applied the edit.

#include <doctest/doctest.h>
#include <memory>
#include <string>

#include "engine/app/reload.h"
#include "engine/core/i18n.h"
#include "project_fixture.h"

using namespace engine;
using engine::app::testing::bootOptions;
using engine::app::testing::Captured;
using engine::app::testing::Project;

namespace {

// A script whose observable result is entirely decided by `marker`, so two
// worlds built from two different values of it cannot hash the same.
std::string scriptNamed(std::string_view marker)
{
    return std::string(R"(
        local root = Instance.new("Folder")
        root.Name = ")") +
           std::string(marker) + R"("
        root.Parent = workspace

        local run = game:GetService("RunService")
        local ticks = 0
        run.Heartbeat:Connect(function()
            ticks += 1
            local part = Instance.new("Part")
            part.Name = ")" +
           std::string(marker) + R"(-" .. tostring(ticks)
            part.Position = vector.create(ticks, 0, 0)
            part.Parent = root
        end)
    )";
}

std::unique_ptr<app::WorldHost> bootHost(const Project& project)
{
    auto host = std::make_unique<app::WorldHost>();
    REQUIRE_FALSE(host->boot(bootOptions(project.root)).has_value());
    return host;
}

void tickTimes(app::WorldHost& host, int count)
{
    for (int i = 0; i < count; ++i)
        host.tick();
}

// The hash of a world built cold from `project` and ticked `count` times --
// the reference a reloaded world has to match.
core::u64 hashOfColdBoot(const Project& project, int count)
{
    auto host = std::make_unique<app::WorldHost>();
    REQUIRE_FALSE(host->boot(bootOptions(project.root)).has_value());
    tickTimes(*host, count);
    return host->world().worldHash();
}

} // namespace

TEST_CASE("a reloaded world is indistinguishable from one that never reloaded")
{
    Captured log;
    Project project;
    project.write("src/client/main.luau", scriptNamed("first"));

    constexpr int kTicks = 30;

    std::unique_ptr<app::WorldHost> host = bootHost(project);
    tickTimes(*host, kTicks);
    const core::u64 beforeEdit = host->world().worldHash();

    project.write("src/client/main.luau", scriptNamed("second"));

    const app::ReloadReport report = app::reloadWorld(host, bootOptions(project.root));
    if (report.error.has_value())
        FAIL(report.error->message);
    REQUIRE(report.ok);
    CHECK(report.mountedScripts == 1);

    tickTimes(*host, kTicks);
    const core::u64 afterReload = host->world().worldHash();

    // The whole claim, in one line: reload equals restart.
    CHECK(afterReload == hashOfColdBoot(project, kTicks));

    // And the edit actually took. Without this the assertion above would hold
    // just as well against a reload that did nothing at all.
    CHECK(afterReload != beforeEdit);
}

TEST_CASE("the tick counter and the instance ids restart with the world")
{
    Captured log;
    Project project;
    project.write("src/client/main.luau", scriptNamed("same"));

    std::unique_ptr<app::WorldHost> host = bootHost(project);
    tickTimes(*host, 12);
    const core::u64 firstRun = host->world().worldHash();

    // Nothing edited: reloading the same source must land on the same world as
    // the run that just happened, which is only true if a reload really is a
    // restart rather than a continuation.
    const app::ReloadReport report = app::reloadWorld(host, bootOptions(project.root));
    REQUIRE(report.ok);

    tickTimes(*host, 12);
    CHECK(host->world().worldHash() == firstRun);
}

TEST_CASE("a project that will not mount leaves the world that was running alone")
{
    Captured log;
    Project project;
    project.write("src/client/main.luau", scriptNamed("survivor"));

    std::unique_ptr<app::WorldHost> host = bootHost(project);
    tickTimes(*host, 5);
    const app::WorldHost* before = host.get();
    const core::u64 hashBefore = host->world().worldHash();

    // The common case in a loop whose point is that you save often.
    project.write("src/client/main.luau", "this is not valid Luau ((");

    const app::ReloadReport report = app::reloadWorld(host, bootOptions(project.root));
    CHECK_FALSE(report.ok);
    REQUIRE(report.error.has_value());
    CHECK(report.error->key.hash == ENG_TR("engine.reload.err.script_failed").hash);
    CHECK(report.loadFailures == 1);

    // Same object, same world, and it still runs.
    CHECK(host.get() == before);
    CHECK(host->world().worldHash() == hashBefore);
    tickTimes(*host, 5);
    CHECK(host->world().worldHash() != hashBefore);
}

TEST_CASE("one broken script among several still refuses the whole reload")
{
    // The single-script case is caught by the emptiness check as well; this is
    // the one that needs the compile count, because nine of the ten scripts
    // mounted and started perfectly well.
    Captured log;
    Project project;
    project.write("src/client/a.luau", scriptNamed("alpha"));
    project.write("src/client/b.luau", scriptNamed("beta"));

    std::unique_ptr<app::WorldHost> host = bootHost(project);
    tickTimes(*host, 4);
    const app::WorldHost* before = host.get();

    project.write("src/client/b.luau", "local x = ((");

    const app::ReloadReport report = app::reloadWorld(host, bootOptions(project.root));
    CHECK_FALSE(report.ok);
    CHECK(report.mountedScripts == 2);
    CHECK(report.loadFailures == 1);
    CHECK(host.get() == before);
}

TEST_CASE("a project that stopped containing scripts is refused, not swapped in")
{
    Captured log;
    Project project;
    project.write("src/client/main.luau", scriptNamed("present"));

    std::unique_ptr<app::WorldHost> host = bootHost(project);
    tickTimes(*host, 3);
    const app::WorldHost* before = host.get();

    std::error_code ec;
    std::filesystem::remove_all(project.root / "src" / "client", ec);

    const app::ReloadReport report = app::reloadWorld(host, bootOptions(project.root));
    CHECK_FALSE(report.ok);
    CHECK(report.mountedScripts == 0);
    REQUIRE(report.error.has_value());
    CHECK(report.error->key.hash == ENG_TR("engine.reload.err.no_scripts").hash);
    CHECK(host.get() == before);
}

TEST_CASE("a world with no project at all still reloads")
{
    // `--version` and the render gates boot one of these. Refusing it as empty
    // would make the emptiness check reject the one case that is meant to be.
    Captured log;
    auto host = std::make_unique<app::WorldHost>();
    REQUIRE_FALSE(host->boot({}).has_value());

    const app::ReloadReport report = app::reloadWorld(host, {});
    CHECK(report.ok);
    CHECK(report.mountedScripts == 0);
}

TEST_CASE("the reload reports the span its budget is measured against")
{
    Captured log;
    Project project;
    project.write("src/client/main.luau", scriptNamed("timed"));

    std::unique_ptr<app::WorldHost> host = bootHost(project);
    const app::ReloadReport report = app::reloadWorld(host, bootOptions(project.root));

    REQUIRE(report.ok);
    // Not a performance assertion -- ADR 0024's 500 ms budget is gated on a
    // real project by the M3 gate, not on a one-file fixture here. What this
    // pins is that the number is measured and reported at all, because a
    // budget nobody reports is a budget nobody checks.
    CHECK(report.spanMs > 0.0);
    CHECK(report.spanMs < 5000.0);
}

TEST_CASE("a hot reload keeps what a script saved just before it (audit A11)")
{
    // A slot is written a second after its last change, off the main thread;
    // the reload booted the fresh host from disk before that second was up, and
    // the old host went without writing, so the change was lost.
    Captured log;
    Project project;
    project.write("src/client/main.luau", R"(
        local slot = game:GetService("SaveService"):GetSlotAsync("reloaded")
        local count = (slot:Get("count") or 0) + 1
        slot:Set("count", count)
        print(`count={count}`)
    )");

    // On disk, as `ludwerk dev` keeps them: a reload reads them back from there.
    app::WorldHostOptions options = bootOptions(project.root);
    options.saveDirectory = project.root / ".engine" / "saves";
    auto host = std::make_unique<app::WorldHost>();
    REQUIRE_FALSE(host->boot(options).has_value());
    tickTimes(*host, 2);
    REQUIRE(log.contains("count=1"));

    const app::ReloadReport report = app::reloadWorld(host, options);
    REQUIRE(report.ok);
    tickTimes(*host, 2);
    CHECK(log.contains("count=2"));
}

TEST_CASE("a reload boots again with the scene and the topology the world is in now (S0.7)")
{
    // The boot's options were kept and reused: a world that had changed scene,
    // or joined, came back in the first scene, solo.
    Captured log;
    Project project;
    project.write("content/scenes/a.scene.json",
                  R"json({"format":"scene","version":2,"root":{"children":[{"class":"Folder","name":"InA"}]}})json");
    project.write("content/scenes/b.scene.json",
                  R"json({"format":"scene","version":2,"root":{"children":[{"class":"Folder","name":"InB"}]}})json");
    // A reload refuses a project with no scripts at all.
    project.write("src/client/init.luau", "print('here')");
    app::WorldHostOptions options = bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "a.scene.json";
    options.bootScenePath = "scenes/a.scene.json";

    auto host = std::make_unique<app::WorldHost>();
    REQUIRE_FALSE(host->boot(options).has_value());
    REQUIRE_FALSE(host->loadScene("scenes/b.scene.json").has_value());
    host->world().engineState().networkTopology = scene::NetworkTopology::Host;

    const app::ReloadReport report = app::reloadWorld(host, options);
    if (report.error.has_value())
        FAIL(report.error->message);
    const scene::World& world = host->world();
    CHECK(world.engineState().currentScene == "scenes/b.scene.json");
    CHECK(world.engineState().networkTopology == scene::NetworkTopology::Host);
    CHECK(world.findFirstChild(host->workspace(), world.atoms().lookup("InB")).valid());
    CHECK_FALSE(world.findFirstChild(host->workspace(), world.atoms().lookup("InA")).valid());
}

TEST_CASE("a reload that cannot find the current scene keeps the boot scene and says the boot scene (script sides S3)")
{
    // When the boot scene's path was not a suffix of its file, the reload
    // named the current scene and loaded the boot one.
    Project project;
    project.write("content/scenes/a.scene.json", R"json({"format":"scene","version":2,"root":{}})json");
    project.write("content/scenes/b.scene.json", R"json({"format":"scene","version":2,"root":{}})json");
    app::WorldHostOptions options = bootOptions(project.root);
    options.bootScene = project.root / "content" / "scenes" / "a.scene.json";
    options.bootScenePath = "levels/a.scene.json";
    auto host = std::make_unique<app::WorldHost>();
    REQUIRE_FALSE(host->boot(options).has_value());
    host->world().engineState().currentScene = "scenes/b.scene.json";

    const app::WorldHostOptions next = app::currentOptions(*host, options);
    CHECK(next.bootScene == options.bootScene);
    CHECK(next.bootScenePath == options.bootScenePath);
}
