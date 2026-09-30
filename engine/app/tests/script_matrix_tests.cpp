// The script-sides audit's matrix (S3, `docs/briefs/script-sides-kickoff.md`):
// every container a script can be authored in, times every `RunContext`, times
// every topology a machine boots in -- and every way a script comes into the
// world at run time, times the same. Each cell is checked against an oracle
// written from ADR 0137's "live" and ADR 0138's table, not from the code, so
// the code and the decision are compared rather than the code with itself.
//
// The cells no test here can reach are named in the audit, with the reason.

#include <algorithm>
#include <array>
#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/app/world_host.h"
#include "engine/platform/file.h"
#include "project_fixture.h"

using namespace engine;
using app::testing::bootOptions;
using app::testing::Captured;
using app::testing::Project;

namespace {

enum class Side
{
    Server,
    Client,
    Shared,
};

constexpr std::array<Side, 3> Sides{Side::Server, Side::Client, Side::Shared};

[[nodiscard]] std::string_view nameOf(Side side)
{
    return side == Side::Server ? "Server" : side == Side::Client ? "Client" : "Shared";
}

// Where a script is authored, and what that says about its side.
struct Container
{
    std::string_view tag;
    // The service that decides the side, if one does.
    std::optional<Side> decides;
    // Storage: never live (ADR 0137 §3).
    bool storage = false;
    // Inside something the authority replicates: a replica's own copy runs
    // only once the authority sends it (ADR 0138 §6), so a replica booted
    // with no authority runs none of it.
    bool replicated = false;
};

constexpr std::array<Container, 11> Containers{{
    {"World", std::nullopt, false, false},
    {"Part", std::nullopt, false, true},
    {"Model", std::nullopt, false, true},
    {"Screen", std::nullopt, false, false},
    {"ServerScriptService", Side::Server, false, false},
    {"ClientScriptService", Side::Client, false, false},
    {"GlobalServer", Side::Server, false, false},
    {"GlobalClient", Side::Client, false, false},
    {"GlobalShared", std::nullopt, false, false},
    {"ServerStorage", std::nullopt, true, false},
    {"ReplicatedStorage", std::nullopt, true, false},
}};

// **The oracle**: how many times a script of `side`, authored in `container`,
// runs on a machine of `topology` -- 0 or 1, never more (ADR 0138 §3: once
// per machine).
[[nodiscard]] int expected(const Container& container, Side side, scene::NetworkTopology topology)
{
    if (container.storage)
        return 0;
    if (container.replicated && topology == scene::NetworkTopology::Replica)
        return 0;
    const Side effective = container.decides.value_or(side);
    switch (effective) {
    case Side::Server:
        return topology == scene::NetworkTopology::Replica ? 0 : 1;
    case Side::Client:
        return topology == scene::NetworkTopology::Dedicated ? 0 : 1;
    case Side::Shared:
        return 1;
    }
    return 0;
}

[[nodiscard]] std::string jsonText(std::string_view text)
{
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out;
}

[[nodiscard]] std::string script(std::string_view name, std::string_view source, Side side, bool enabled = true,
                                 std::string_view entry = {})
{
    std::string node = R"({"class":"Script","name":")" + std::string(name) + R"(","properties":{"Source":")" +
                       jsonText(source) + R"(","RunContext":")" + std::string(nameOf(side)) + "\"";
    if (!enabled)
        node += R"(,"Enabled":false)";
    node += "}";
    if (!entry.empty())
        node += R"(,"attributes":{"Entry":")" + std::string(entry) + "\"}";
    return node + "}";
}

// The three scripts one container holds, each printing `m:boot:<tag>:<side>`.
[[nodiscard]] std::string threeIn(std::string_view tag)
{
    std::string out;
    for (const Side side : Sides) {
        if (!out.empty())
            out += ",";
        const std::string mark = "m:boot:" + std::string(tag) + ":" + std::string(nameOf(side));
        out += script(std::string(tag) + std::string(nameOf(side)), "print('" + mark + "')", side);
    }
    return out;
}

// A script that says where it came in: `m:<Entry attribute>:<its side>`.
constexpr std::string_view EntrySource =
    "print('m:' .. tostring(script:GetAttribute('Entry')) .. ':' .. script.RunContext.Name)";

[[nodiscard]] std::string entryScripts(std::string_view prefix, bool enabled, std::string_view entry)
{
    std::string out;
    for (const Side side : Sides) {
        if (!out.empty())
            out += ",";
        out += script(std::string(prefix) + std::string(nameOf(side)), EntrySource, side, enabled, entry);
    }
    return out;
}

// The driver, as a world script with `Shared`, which runs on every machine:
// a stamp placed, clones of its scripts, a folder moved out of storage, and
// `Enabled` written. A replica's join clear took the storage folder and the
// sleepers (both replicate), so it finds neither.
constexpr std::string_view Driver = R"(
local placed = Instance.stamp("entry")
placed.Parent = workspace
for _, child in placed:GetChildren() do
    local copy = child:Clone()
    copy:SetAttribute("Entry", "clone")
    copy.Parent = workspace
end
local kept = game:GetService("ReplicatedStorage"):FindFirstChild("Kept")
if kept then
    kept.Parent = workspace
end
local sleepers = workspace:FindFirstChild("Sleepers")
if sleepers then
    for _, sleeper in sleepers:GetChildren() do
        (sleeper :: Script).Enabled = true
    end
end
)";

// **The scene**: three scripts in every container, and what the run-time
// entries start from -- a stamp, storage templates, disabled scripts.
void writeProject(Project& project)
{
    project.write(
        "content/scenes/main.scene.json",
        R"({"format":"scene","version":2,"root":{"name":"Workspace","children":[)" + threeIn("World") + "," +
            script("Driver", Driver, Side::Shared) +
            R"(,{"class":"Part","name":"Crate","properties":{"Anchored":true},"children":[)" + threeIn("Part") +
            R"(]},{"class":"Model","name":"House","children":[)" + threeIn("Model") +
            R"(]},{"class":"Folder","name":"Sleepers","children":[)" + entryScripts("Sleeper", false, "enable") +
            R"(]}]},"storage":{)"
            R"("ServerScriptService":{"class":"ServerScriptService","name":"ServerScriptService","children":[)" +
            threeIn("ServerScriptService") +
            R"(]},"ClientScriptService":{"class":"ClientScriptService","name":"ClientScriptService","children":[)" +
            threeIn("ClientScriptService") +
            R"(]},"ServerStorage":{"class":"ServerStorage","name":"ServerStorage","children":[)" +
            threeIn("ServerStorage") +
            R"(]},"ReplicatedStorage":{"class":"ReplicatedStorage","name":"ReplicatedStorage","children":[)" +
            threeIn("ReplicatedStorage") + R"(,{"class":"Folder","name":"Kept","children":[)" +
            entryScripts("Kept", true, "fromstorage") +
            R"(]}]},"UIService":{"class":"UIService","name":"UIService","children":[)"
            R"({"class":"ScreenGui","name":"Hud","children":[)" +
            threeIn("Screen") + R"(]}]}}})");
    project.write("content/global.json",
                  R"({"format":"global","version":1,"root":{"class":"GlobalScriptService",)"
                  R"("name":"GlobalScriptService","children":[)"
                  R"({"class":"Folder","name":"Server","mounted":true,"children":[)" +
                      threeIn("GlobalServer") + R"(]},{"class":"Folder","name":"Client","mounted":true,"children":[)" +
                      threeIn("GlobalClient") + R"(]},{"class":"Folder","name":"Shared","mounted":true,"children":[)" +
                      threeIn("GlobalShared") + R"(]}]}})");
    project.write("content/stamps/entry.stamp.json",
                  R"({"format":"scene","version":2,"root":{"class":"Model","name":"Entry","children":[)" +
                      entryScripts("Stamped", true, "stamp") + "]}}");
}

struct Boot
{
    Captured log;
    Project project;
    app::WorldHost host;

    explicit Boot(scene::NetworkTopology topology)
    {
        writeProject(project);
        app::WorldHostOptions options = bootOptions(project.root);
        options.bootScene = project.root / "content" / "scenes" / "main.scene.json";
        options.bootScenePath = "scenes/main.scene.json";
        std::string global;
        REQUIRE(platform::readTextFile(project.root / "content" / "global.json", global));
        options.bootGlobalText = global;
        options.bootStamps = [root = project.root](std::string_view stamp) -> std::optional<std::string> {
            std::string text;
            if (!platform::readTextFile(root / "content" / std::filesystem::path(stamp), text))
                return std::nullopt;
            return text;
        };
        options.networkTopology = topology;
        REQUIRE_FALSE(host.boot(options).has_value());
        for (int tick = 0; tick < 4; ++tick)
            host.tick();
    }

    [[nodiscard]] int count(std::string_view needle) const
    {
        return static_cast<int>(std::count_if(log.lines.begin(), log.lines.end(), [&](const std::string& line) {
            return line.size() >= needle.size() && line.substr(line.size() - needle.size()) == needle;
        }));
    }
};

[[nodiscard]] std::string_view topologyName(scene::NetworkTopology topology)
{
    switch (topology) {
    case scene::NetworkTopology::Solo:
        return "solo";
    case scene::NetworkTopology::Host:
        return "host";
    case scene::NetworkTopology::Dedicated:
        return "dedicated";
    case scene::NetworkTopology::Replica:
        return "replica";
    }
    return "?";
}

} // namespace

TEST_CASE("the matrix: every container, every RunContext, every topology a machine boots in (S3)")
{
    for (const scene::NetworkTopology topology : {scene::NetworkTopology::Solo, scene::NetworkTopology::Host,
                                                  scene::NetworkTopology::Dedicated, scene::NetworkTopology::Replica}) {
        const Boot boot(topology);
        CHECK_MESSAGE(boot.log.firstError().empty(), boot.log.firstError());
        for (const Container& container : Containers) {
            for (const Side side : Sides) {
                const std::string mark = "m:boot:" + std::string(container.tag) + ":" + std::string(nameOf(side));
                CHECK_MESSAGE(boot.count(mark) == expected(container, side, topology), topologyName(topology), " ",
                              mark, " ran ", boot.count(mark));
            }
        }
    }
}

TEST_CASE("the matrix: every way a script comes in at run time, every RunContext, every topology (S3)")
{
    // Stamp, clone, a move out of storage, `Enabled`. On a replica the storage
    // folder and the sleepers were the authority's to send, and none came.
    for (const scene::NetworkTopology topology : {scene::NetworkTopology::Solo, scene::NetworkTopology::Host,
                                                  scene::NetworkTopology::Dedicated, scene::NetworkTopology::Replica}) {
        const Boot boot(topology);
        const Container world{"World", std::nullopt, false, false};
        for (const std::string_view entry : {"stamp", "clone", "fromstorage", "enable"}) {
            const bool gone =
                topology == scene::NetworkTopology::Replica && (entry == "fromstorage" || entry == "enable");
            for (const Side side : Sides) {
                const std::string mark = "m:" + std::string(entry) + ":" + std::string(nameOf(side));
                const int want = gone ? 0 : expected(world, side, topology);
                CHECK_MESSAGE(boot.count(mark) == want, topologyName(topology), " ", mark, " ran ", boot.count(mark));
            }
        }
    }
}
