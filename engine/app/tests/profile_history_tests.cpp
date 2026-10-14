// The frames a profiler reads (ADR 0210): kept, held, chosen, found by being
// the worst, read as a tree and a table -- and taken home in a file.
#include <algorithm>
#include <cmath>
#include <doctest/doctest.h>
#include <string>
#include <vector>

#include "engine/app/profile_history.h"

namespace {

using engine::app::ProfileBand;
using engine::app::ProfileFileError;
using engine::app::ProfileHistory;
using engine::app::ProfileSample;
using engine::core::f32;
using engine::core::usize;

// A frame of `simulation`, `scripts.render` and `render.world` under `frame`,
// with `render.shadow` under the last, and whatever is left over as the
// frame's own.
[[nodiscard]] ProfileSample frameOf(f32 simulation, f32 scripts, f32 render, f32 own = 0.5f)
{
    ProfileSample sample;
    sample.scopes.push_back({"frame", -1, simulation + scripts + render + own, 1});
    sample.scopes.push_back({"simulation", 0, simulation, 2});
    sample.scopes.push_back({"scripts.render", 0, scripts, 1});
    sample.scopes.push_back({"render.world", 0, render, 1});
    sample.scopes.push_back({"render.shadow", 3, render * 0.25f, 4});
    sample.entries = 9;
    sample.scripts.push_back({"Workspace.Spinner", scripts * 0.75f, 3});
    sample.scripts.push_back({"Workspace.Idle", scripts * 0.25f, 1});
    return sample;
}

// At f32, where these are: `doctest::Approx` is a double's.
[[nodiscard]] bool nearly(f32 a, f32 b)
{
    return std::fabs(a - b) <= 1.0e-4f * std::max(1.0f, std::fabs(b));
}

[[nodiscard]] f32 band(const ProfileHistory::Frame& frame, ProfileBand which)
{
    return frame.bands[static_cast<usize>(which)];
}

} // namespace

TEST_CASE("ADR 0210: a frame is its roots together, in bands by system, with the rest its own")
{
    ProfileHistory history;
    history.push(frameOf(4.0f, 2.0f, 6.0f));
    REQUIRE(history.size() == 1);
    const ProfileHistory::Frame& frame = history.frame(0);
    CHECK(nearly(frame.ms, 12.5f));
    CHECK(nearly(band(frame, ProfileBand::Simulation), 4.0f));
    CHECK(nearly(band(frame, ProfileBand::Scripts), 2.0f));
    // What is under `render.world` is inside it, not beside it.
    CHECK(nearly(band(frame, ProfileBand::Render), 6.0f));
    CHECK(nearly(band(frame, ProfileBand::Interface), 0.0f));
    CHECK(nearly(band(frame, ProfileBand::Other), 0.5f));
    CHECK(frame.entries == 9);
}

TEST_CASE("ADR 0210: the bands follow the engine's own tree -- a scope is its band, or the one it stands in")
{
    // The frame as the host's loop nests it, by the names it uses.
    ProfileSample sample;
    sample.scopes.push_back({"frame", -1, 16.0f, 1});
    sample.scopes.push_back({"frame.simulation", 0, 5.0f, 1});
    sample.scopes.push_back({"simulation", 1, 5.0f, 1});
    sample.scopes.push_back({"tick", 2, 4.5f, 1});
    sample.scopes.push_back({"physics", 3, 2.0f, 1});
    sample.scopes.push_back({"scripts.Heartbeat", 3, 1.5f, 1});
    sample.scopes.push_back({"frame.draw", 0, 7.0f, 1});
    sample.scopes.push_back({"draw.interface", 6, 1.0f, 1});
    sample.scopes.push_back({"draw.render", 6, 5.5f, 1});
    sample.scopes.push_back({"render.world", 8, 5.0f, 1});
    sample.scopes.push_back({"environment.prefilter", 9, 1.0f, 1});
    sample.scopes.push_back({"frame.present", 0, 3.0f, 1});
    sample.scopes.push_back({"wait.present", 11, 2.9f, 1});
    ProfileHistory history;
    history.push(sample);
    const ProfileHistory::Frame& frame = history.frame(0);
    CHECK(nearly(frame.ms, 16.0f));
    // The tick's scripts are the scripts', not the simulation's.
    CHECK(nearly(band(frame, ProfileBand::Simulation), 3.5f));
    CHECK(nearly(band(frame, ProfileBand::Scripts), 1.5f));
    // A scope with no band of its own under the renderer is the renderer's.
    CHECK(nearly(band(frame, ProfileBand::Render), 5.5f));
    CHECK(nearly(band(frame, ProfileBand::Interface), 1.0f));
    CHECK(nearly(band(frame, ProfileBand::Waiting), 3.0f));
    // The frame's own second and the draw's own half.
    CHECK(nearly(band(frame, ProfileBand::Other), 1.5f));
    f32 all = 0.0f;
    for (const f32 spent : frame.bands)
        all += spent;
    CHECK(nearly(all, frame.ms));
}

TEST_CASE("ADR 0210: a history keeps the last frames it has room for, and none while held")
{
    ProfileHistory history(3);
    for (int index = 0; index < 5; ++index)
        history.push(frameOf(static_cast<f32>(index), 1.0f, 1.0f));
    REQUIRE(history.size() == 3);
    // The last three of five: the frames are numbered as they were handed in.
    CHECK(history.frame(0).number == 2);
    CHECK(history.frame(2).number == 4);

    history.hold();
    history.push(frameOf(50.0f, 1.0f, 1.0f));
    CHECK(history.size() == 3);
    CHECK(history.frame(2).number == 4);
    history.resume();
    history.push(frameOf(7.0f, 1.0f, 1.0f));
    CHECK(history.frame(2).number == 5);
    CHECK(nearly(history.frame(2).ms, 9.5f));
}

TEST_CASE("ADR 0210: choosing a frame holds it, stepping moves one, and resuming lets both go")
{
    ProfileHistory history;
    for (int index = 0; index < 4; ++index)
        history.push(frameOf(static_cast<f32>(index), 1.0f, 1.0f));
    CHECK_FALSE(history.held());
    CHECK_FALSE(history.chosen().has_value());

    // From nothing chosen, a step is the newest frame.
    history.step(-1);
    REQUIRE(history.chosen().has_value());
    CHECK(*history.chosen() == 3);
    CHECK(history.held());
    history.step(-1);
    CHECK(*history.chosen() == 2);
    history.step(-10);
    CHECK(*history.chosen() == 0);
    history.step(100);
    CHECK(*history.chosen() == 3);
    history.choose(99);
    CHECK(*history.chosen() == 3);

    history.resume();
    CHECK_FALSE(history.held());
    CHECK_FALSE(history.chosen().has_value());
}

TEST_CASE("ADR 0210: the worst frame is found, then the next worst, and round again")
{
    ProfileHistory history;
    // Lengths 3.5, 12.5, 2.5, 12.5, 7.5.
    for (const f32 simulation : {1.0f, 10.0f, 0.0f, 10.0f, 5.0f})
        history.push(frameOf(simulation, 1.0f, 1.0f));
    CHECK(history.overBudget(5.0f) == 3);
    CHECK(history.overBudget(100.0f) == 0);

    REQUIRE(history.chooseWorst());
    // Two are as long: the earlier is the first found.
    CHECK(*history.chosen() == 1);
    CHECK(history.held());
    REQUIRE(history.chooseNextWorst());
    CHECK(*history.chosen() == 3);
    REQUIRE(history.chooseNextWorst());
    CHECK(*history.chosen() == 4);
    REQUIRE(history.chooseNextWorst());
    CHECK(*history.chosen() == 0);
    REQUIRE(history.chooseNextWorst());
    CHECK(*history.chosen() == 2);
    // Past the shortest, the longest again.
    REQUIRE(history.chooseNextWorst());
    CHECK(*history.chosen() == 1);

    ProfileHistory empty;
    CHECK_FALSE(empty.chooseWorst());
    CHECK_FALSE(empty.chooseNextWorst());
}

TEST_CASE("ADR 0210: the systems are a tree with each scope's own time, of the frame chosen or of the run")
{
    ProfileHistory history;
    history.push(frameOf(1.0f, 1.0f, 4.0f));
    history.push(frameOf(2.0f, 1.0f, 8.0f));
    history.push(frameOf(3.0f, 1.0f, 40.0f));

    history.choose(2);
    std::vector<ProfileHistory::Row> rows = history.systems(engine::app::ProfileOrder::Entered);
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].name == "frame");
    CHECK(rows[0].depth == 0);
    CHECK(nearly(rows[0].ms, 44.5f));
    // The frame's own: what none of the three under it accounts for.
    CHECK(nearly(rows[0].selfMs, 0.5f));
    CHECK(rows[3].name == "render.world");
    CHECK(rows[3].depth == 1);
    CHECK(nearly(rows[3].ms, 40.0f));
    CHECK(nearly(rows[3].selfMs, 30.0f));
    CHECK(rows[4].name == "render.shadow");
    CHECK(rows[4].depth == 2);
    CHECK(nearly(rows[4].count, 4.0f));
    CHECK(nearly(history.shownMs(), 44.5f));

    // **Heaviest first, which is how it is read unless asked otherwise**:
    // the renderer and what is under it before the simulation and the
    // scripts, and the path down the longest of each level marked -- the
    // frame, its renderer, the renderer's shadows -- and nothing else.
    rows = history.systems();
    REQUIRE(rows.size() == 5);
    CHECK(rows[0].name == "frame");
    CHECK(rows[1].name == "render.world");
    CHECK(rows[2].name == "render.shadow");
    CHECK(rows[3].name == "simulation");
    CHECK(rows[4].name == "scripts.render");
    CHECK(rows[0].heaviest);
    CHECK(rows[1].heaviest);
    CHECK(rows[2].heaviest);
    CHECK_FALSE(rows[3].heaviest);
    CHECK_FALSE(rows[4].heaviest);
    // What folds, and what has nothing under it to fold.
    CHECK_FALSE(rows[0].leaf);
    CHECK_FALSE(rows[1].leaf);
    CHECK(rows[2].leaf);
    CHECK(rows[3].leaf);

    // With none chosen, the median of the run: the middle frame's, here.
    history.resume();
    rows = history.systems(engine::app::ProfileOrder::Entered);
    REQUIRE(rows.size() == 5);
    CHECK(rows[1].name == "simulation");
    CHECK(nearly(rows[1].ms, 2.0f));
    CHECK(nearly(rows[3].ms, 8.0f));
    CHECK(nearly(history.shownMs(), 11.5f));
}

TEST_CASE(
    "ADR 0210: a scope that ran in one frame of many is a row of the run at nought, and of that frame at its time")
{
    ProfileHistory history;
    history.push(frameOf(1.0f, 1.0f, 1.0f));
    history.push(frameOf(1.0f, 1.0f, 1.0f));
    ProfileSample hitch = frameOf(1.0f, 1.0f, 1.0f);
    hitch.scopes.push_back({"content.meshes", 0, 30.0f, 1});
    hitch.scopes[0].ms += 30.0f;
    history.push(hitch);

    std::vector<ProfileHistory::Row> rows = history.systems();
    REQUIRE(rows.size() == 6);
    CHECK(rows[5].name == "content.meshes");
    CHECK(nearly(rows[5].ms, 0.0f));

    REQUIRE(history.chooseWorst());
    CHECK(*history.chosen() == 2);
    // In the frame it ran in it is the heaviest thing under the frame, and
    // so the first: the hitch is the first screen and not the last row.
    rows = history.systems();
    CHECK(rows[1].name == "content.meshes");
    CHECK(nearly(rows[1].ms, 30.0f));
    CHECK(rows[1].heaviest);
    CHECK(nearly(history.frame(2).bands[static_cast<usize>(ProfileBand::Render)], 31.0f));
}

TEST_CASE("ADR 0210: scripts are a table longest first, and lines add up over the run")
{
    ProfileHistory history;
    ProfileSample first = frameOf(1.0f, 4.0f, 1.0f);
    first.lines.push_back({"src/client/init.luau", 12, 3});
    first.lines.push_back({"src/client/init.luau", 40, 1});
    history.push(first);
    ProfileSample second = frameOf(1.0f, 8.0f, 1.0f);
    second.lines.push_back({"src/client/init.luau", 40, 6});
    history.push(second);

    history.choose(1);
    const std::vector<ProfileHistory::Row> scripts = history.scriptRows();
    REQUIRE(scripts.size() == 2);
    CHECK(scripts[0].name == "Workspace.Spinner");
    CHECK(nearly(scripts[0].ms, 6.0f));
    CHECK(nearly(scripts[0].count, 3.0f));
    CHECK(scripts[1].name == "Workspace.Idle");

    const std::vector<ProfileHistory::Line> lines = history.lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].line == 40);
    CHECK(lines[0].samples == 7);
    CHECK(lines[1].line == 12);
    CHECK(history.lineSamples() == 10);
}

TEST_CASE("ADR 0210: corrections are counted in the frame they landed in, a player apiece")
{
    ProfileHistory history;
    ProfileSample sample = frameOf(1.0f, 1.0f, 1.0f);
    sample.network.connected = true;
    sample.network.corrections = 10;
    sample.network.playerPredicted = {true, true, false, false};
    sample.network.playerCorrections = {7, 3, 0, 0};
    history.push(sample);
    // The first frame has nothing to be a difference from.
    CHECK(history.frame(0).corrections == 0);

    sample.network.corrections = 13;
    sample.network.playerCorrections = {7, 6, 0, 0};
    history.push(sample);
    CHECK(history.frame(1).corrections == 3);
    CHECK(history.frame(1).playerCorrections[0] == 0);
    CHECK(history.frame(1).playerCorrections[1] == 3);

    // Held frames are dropped, and what landed in them is not the next one's.
    history.hold();
    sample.network.corrections = 113;
    history.push(sample);
    history.resume();
    sample.network.corrections = 114;
    history.push(sample);
    CHECK(history.frame(2).corrections == 1);
}

TEST_CASE("ADR 0210: a history written to a file and read back is the same history, held")
{
    ProfileHistory history(8);
    ProfileSample sample = frameOf(2.0f, 3.0f, 5.0f);
    sample.passes.push_back({"forward", 0.75f});
    sample.passes.push_back({"shadow", 0.25f});
    sample.gpuTimingMs = 1.5f;
    sample.memory = {1024, 4096, 3, 2048, 2};
    sample.scriptMemory.push_back({"src/client/init.luau", 900});
    sample.lines.push_back({"src/client/init.luau", 7, 4});
    sample.network.connected = true;
    sample.network.pingMs = 42.0f;
    sample.network.playerPredicted = {true, false, false, false};
    history.push(sample);
    sample.network.corrections = 2;
    sample.network.playerCorrections = {2, 0, 0, 0};
    history.push(frameOf(9.0f, 1.0f, 1.0f));
    history.push(sample);
    // A wait inside a stretch that has no band of its own.
    ProfileSample nested;
    nested.scopes.push_back({"frame", -1, 10.0f, 1});
    nested.scopes.push_back({"frame.begin", 0, 6.0f, 1});
    nested.scopes.push_back({"wait.rate", 1, 6.0f, 1});
    history.push(nested);

    ProfileHistory read;
    REQUIRE(read.fromJson(history.toJson()) == ProfileFileError::None);
    // Nobody is pushing into a file: it is held as it was written.
    CHECK(read.held());
    REQUIRE(read.size() == 4);
    CHECK(read.capacity() == 8);
    CHECK(nearly(read.frame(1).ms, history.frame(1).ms));
    CHECK(read.frame(1).bands == history.frame(1).bands);
    // **A file's bands are the run's**, by the one rule: the file's were
    // worked out by a copy of it, the copy kept an older rule, and a capture
    // opened in the editor drew this wait as the rest.
    CHECK(nearly(band(history.frame(3), ProfileBand::Waiting), 6.0f));
    CHECK(read.frame(3).bands == history.frame(3).bands);
    CHECK(read.frame(2).number == 2);
    CHECK(read.frame(2).corrections == 2);
    CHECK(read.frame(2).playerCorrections[0] == 2);
    CHECK(nearly(read.frame(0).network.pingMs, 42.0f));
    CHECK(read.frame(0).memory.textureBytes == 4096);
    CHECK(nearly(read.frame(0).gpuTimingMs, 1.5f));
    CHECK(read.lineSamples() == 8);

    read.choose(0);
    history.choose(0);
    const std::vector<ProfileHistory::Row> a = read.systems();
    const std::vector<ProfileHistory::Row> b = history.systems();
    REQUIRE(a.size() == b.size());
    for (usize index = 0; index < a.size(); ++index) {
        CHECK(a[index].name == b[index].name);
        CHECK(a[index].depth == b[index].depth);
        CHECK(nearly(a[index].ms, b[index].ms));
        CHECK(nearly(a[index].selfMs, b[index].selfMs));
    }
    REQUIRE(read.passRows().size() == 2);
    CHECK(read.passRows()[0].name == "forward");
    REQUIRE(read.scriptRows().size() == 2);
    CHECK(read.scriptRows()[0].name == "Workspace.Spinner");
    REQUIRE(read.memoryScripts().size() == 1);
    CHECK(read.memoryScripts()[0] == "src/client/init.luau");
}

TEST_CASE("ADR 0210: a file that is not a capture is refused with its reason, and the history is left as it was")
{
    ProfileHistory history;
    history.push(frameOf(1.0f, 1.0f, 1.0f));

    CHECK(history.fromJson("not json at all {") == ProfileFileError::NotJson);
    CHECK(history.fromJson("{\"format\":\"something-else\"}") == ProfileFileError::NotACapture);
    CHECK(history.fromJson("{\"format\":\"profile-capture\",\"version\":99}") == ProfileFileError::OtherVersion);
    // A frame that names a scope the file does not list.
    CHECK(history.fromJson("{\"format\":\"profile-capture\",\"version\":1,\"scopes\":[],"
                           "\"frames\":[{\"number\":0,\"ms\":1,\"scopes\":[5,1.0,1]}]}") == ProfileFileError::Broken);
    CHECK(history.load("no/such/file.profile.json") == ProfileFileError::Unreadable);

    CHECK(history.size() == 1);
    CHECK_FALSE(history.held());
}
