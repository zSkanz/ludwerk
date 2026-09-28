// **The files in `src/` follow the tree at every save** (ADR 0105), and nothing
// is lost or doubled by getting there. Each case opens a project from disk,
// does what a person does in the Explorer, saves as the editor saves, and opens
// the project again in a new host -- because "it looked right until I reopened"
// is exactly how the owner's friend found the defect this exists for: scripts
// dragged and pasted between client and server that would not save, and came
// back twice.

#include <doctest/doctest.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/app/editor.h"
#include "engine/app/inspector.h"
#include "engine/app/script_files.h"
#include "engine/app/world_host.h"
#include "engine/platform/file.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"
#include "project_fixture.h"

using namespace engine;
using engine::app::testing::bootOptions;
using engine::app::testing::Captured;
using engine::app::testing::Project;

namespace {

constexpr std::string_view SceneFile = "scenes/main.scene.json";

// A project opened as the editor opens it: the scripts from `src/`, the game's
// own `global.json`, and the scene.
struct Opened
{
    std::unique_ptr<app::WorldHost> host = std::make_unique<app::WorldHost>();
    app::Editor editor;
    app::Inspector inspector;

    explicit Opened(const Project& project)
    {
        app::WorldHostOptions options = bootOptions(project.root);
        const std::filesystem::path content = project.root / "content";
        options.bootScene = content / std::filesystem::path(SceneFile);
        (void)platform::readTextFile(content / "global.json", options.bootGlobalText);
        REQUIRE_FALSE(host->boot(options).has_value());
        editor.openContent(content);
        editor.adoptOpenScene(SceneFile);
        app::WorldHost* raw = host.get();
        editor.setScriptFileSaver([raw](scene::World& world, const std::filesystem::path& scenePath) {
            if (&world != &raw->world())
                return std::string{};
            std::string scene = scenePath.filename().string();
            if (constexpr std::string_view Suffix = ".scene.json"; scene.ends_with(Suffix))
                scene.resize(scene.size() - Suffix.size());
            return app::syncScriptFiles(*raw, scene).summary();
        });
    }

    [[nodiscard]] scene::World& world() { return host->world(); }
    [[nodiscard]] core::InstanceId root() { return host->runtime().dataModel(); }

    [[nodiscard]] core::InstanceId service(std::string_view className)
    {
        return world().findFirstChildOfClass(root(), world().classes().findId(world().atoms().lookup(className)));
    }
    [[nodiscard]] core::InstanceId global(std::string_view folder)
    {
        const core::InstanceId globals = service("GlobalScriptService");
        return globals.valid() ? world().findFirstChild(globals, world().atoms().lookup(folder)) : core::InstanceId{};
    }
    // Every child of `parent` with this name.
    [[nodiscard]] std::vector<core::InstanceId> named(core::InstanceId parent, std::string_view name)
    {
        std::vector<core::InstanceId> out;
        const core::NameAtom atom = world().atoms().lookup(name);
        for (core::InstanceId child = world().firstChild(parent); child.valid(); child = world().nextSibling(child)) {
            if (world().name(child) == atom)
                out.push_back(child);
        }
        return out;
    }
    [[nodiscard]] core::InstanceId one(core::InstanceId parent, std::string_view name)
    {
        const std::vector<core::InstanceId> found = named(parent, name);
        REQUIRE_MESSAGE(found.size() == 1, "expected exactly one ", std::string(name));
        return found.front();
    }
    [[nodiscard]] std::string source(core::InstanceId id)
    {
        const std::optional<scene::Value> value = world().getProperty(id, world().atoms().intern("Source"));
        const auto* text = value.has_value() ? std::get_if<std::string>(&*value) : nullptr;
        return text != nullptr ? *text : std::string{};
    }
    [[nodiscard]] std::string className(core::InstanceId id)
    {
        return std::string(world().atoms().text(world().classes().find(world().classOf(id))->name));
    }
    bool save() { return editor.saveOpenScene(world()); }
};

[[nodiscard]] std::string readFile(const Project& project, std::string_view relative)
{
    std::string text;
    (void)platform::readTextFile(project.root / std::filesystem::path(relative), text);
    return text;
}

[[nodiscard]] bool exists(const Project& project, std::string_view relative)
{
    return platform::fileExists(project.root / std::filesystem::path(relative));
}

// Every file under the project's trash, as project-relative paths inside it.
[[nodiscard]] std::vector<std::string> trashed(const Project& project)
{
    std::vector<std::string> out;
    const std::filesystem::path trash = project.root / ".engine" / "trash";
    std::error_code ec;
    if (!std::filesystem::is_directory(trash, ec))
        return out;
    for (std::filesystem::recursive_directory_iterator it(trash, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec))
            out.push_back(std::filesystem::relative(it->path(), trash, ec).generic_string());
    }
    return out;
}

// A project with a scene file already saved, so the editor's save writes back
// to it as it would to any project a person opened.
void seedScene(const Project& project)
{
    project.write("content/scenes/main.scene.json",
                  R"({"format": "scene", "version": 2, "root": {"class": "Workspace", "name": "Workspace"}})");
}

} // namespace

TEST_CASE(
    "a script copied from client to server and the original deleted is one script, on the server, after reopening")
{
    // The friend's report, step for step.
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/client/Hello.luau", "print('hello')");
    {
        Opened open(project);
        const core::InstanceId hello = open.one(open.global("Client"), "Hello");
        const std::array<core::InstanceId, 1> picked{hello};
        open.editor.copySelection(open.world(), picked, open.root());
        REQUIRE(open.editor.paste(open.world(), open.global("Server"), open.root(), open.inspector));
        REQUIRE(open.editor.deleteInstances(open.world(), picked, open.root(), open.inspector));
        REQUIRE(open.save());
    }
    CHECK(readFile(project, "src/server/Hello.luau") == "print('hello')");
    CHECK_FALSE(exists(project, "src/client/Hello.luau"));
    // Not deleted: moved aside, where it can be got back.
    const std::vector<std::string> bin = trashed(project);
    REQUIRE(bin.size() == 1);
    CHECK(bin.front().ends_with("src/client/Hello.luau"));
    // And nothing written into the game's own file on top of the script file.
    CHECK(readFile(project, "content/global.json").find("hello") == std::string::npos);

    Opened again(project);
    CHECK(again.named(again.global("Client"), "Hello").empty());
    const core::InstanceId moved = again.one(again.global("Server"), "Hello");
    CHECK(again.source(moved) == "print('hello')");
    CHECK(again.world().mounted(moved));
}

TEST_CASE("a script dragged from client to server moves its file at the save")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/client/Mover.luau", "print('moving')");
    {
        Opened open(project);
        const core::InstanceId mover = open.one(open.global("Client"), "Mover");
        const std::array<core::InstanceId, 1> picked{mover};
        REQUIRE(open.editor.reparent(open.world(), picked, open.global("Server"), open.root(), open.inspector));
        // Edited after the move and never saved on its own: what is in the world
        // is what goes to the file.
        (void)open.world().setProperty(mover, open.world().atoms().intern("Source"),
                                       scene::Value{std::string("print('moved')")});
        REQUIRE(open.save());
    }
    CHECK(readFile(project, "src/server/Mover.luau") == "print('moved')");
    CHECK_FALSE(exists(project, "src/client/Mover.luau"));

    Opened again(project);
    CHECK(again.named(again.global("Client"), "Mover").empty());
    CHECK(again.source(again.one(again.global("Server"), "Mover")) == "print('moved')");
}

TEST_CASE("a copy pasted beside its original keeps both, and a second paste takes the next free name")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/server/Rules.luau", "return 1");
    {
        Opened open(project);
        const std::array<core::InstanceId, 1> picked{open.one(open.global("Server"), "Rules")};
        open.editor.copySelection(open.world(), picked, open.root());
        REQUIRE(open.editor.paste(open.world(), open.global("Server"), open.root(), open.inspector));
        REQUIRE(open.editor.paste(open.world(), open.global("Server"), open.root(), open.inspector));
        REQUIRE(open.save());
        // The tree says what the folder says, the moment the save is done.
        CHECK(open.named(open.global("Server"), "Rules").size() == 1);
        CHECK(open.named(open.global("Server"), "Rules2").size() == 1);
        CHECK(open.named(open.global("Server"), "Rules3").size() == 1);
    }
    CHECK(readFile(project, "src/server/Rules.luau") == "return 1");
    CHECK(readFile(project, "src/server/Rules2.luau") == "return 1");
    CHECK(readFile(project, "src/server/Rules3.luau") == "return 1");
    CHECK(trashed(project).empty());

    Opened again(project);
    CHECK(again.named(again.global("Server"), "Rules").size() == 1);
    CHECK(again.named(again.global("Server"), "Rules2").size() == 1);
    CHECK(again.named(again.global("Server"), "Rules3").size() == 1);
    CHECK(again.world().childCount(again.global("Server")) == 3);
}

TEST_CASE("a deleted script's file goes to the trash at the save, and an undo before the next save brings it back")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/client/Gone.luau", "print('still here')");
    Opened open(project);
    const std::array<core::InstanceId, 1> picked{open.one(open.global("Client"), "Gone")};
    REQUIRE(open.editor.deleteInstances(open.world(), picked, open.root(), open.inspector));

    // Deleted but not saved: the disk has not moved.
    CHECK(exists(project, "src/client/Gone.luau"));
    REQUIRE(open.save());
    CHECK_FALSE(exists(project, "src/client/Gone.luau"));
    REQUIRE(trashed(project).size() == 1);

    // Undone and saved again: the file is back, from what the world held.
    REQUIRE(open.editor.undo(open.world(), open.inspector));
    REQUIRE(open.save());
    CHECK(readFile(project, "src/client/Gone.luau") == "print('still here')");

    Opened again(project);
    CHECK(again.source(again.one(again.global("Client"), "Gone")) == "print('still here')");
}

TEST_CASE("a renamed script and a renamed folder rename their files, and the old ones go to the trash")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/client/enemy/patrol.luau", "print('patrol')");
    project.write("src/client/enemy/chase.luau", "print('chase')");
    {
        Opened open(project);
        const core::InstanceId enemy = open.one(open.global("Client"), "enemy");
        REQUIRE(open.editor.renameInstance(open.world(), open.one(enemy, "patrol"), open.root(), "guard"));
        REQUIRE(open.editor.renameInstance(open.world(), enemy, open.root(), "foes"));
        REQUIRE(open.save());
    }
    CHECK(readFile(project, "src/client/foes/guard.luau") == "print('patrol')");
    CHECK(readFile(project, "src/client/foes/chase.luau") == "print('chase')");
    CHECK_FALSE(exists(project, "src/client/enemy/patrol.luau"));
    CHECK_FALSE(exists(project, "src/client/enemy/chase.luau"));
    // The folder the files left is not left behind empty.
    CHECK_FALSE(std::filesystem::exists(project.root / "src/client/enemy"));

    Opened again(project);
    const core::InstanceId foes = again.one(again.global("Client"), "foes");
    CHECK(again.named(again.global("Client"), "enemy").empty());
    CHECK(again.source(again.one(foes, "guard")) == "print('patrol')");
    CHECK(again.source(again.one(foes, "chase")) == "print('chase')");
}

TEST_CASE("a script moved out of the script services is saved in the scene, and its file goes to the trash")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/client/Door.luau", "print('door')");
    {
        Opened open(project);
        const std::array<core::InstanceId, 1> picked{open.one(open.global("Client"), "Door")};
        REQUIRE(open.editor.reparent(open.world(), picked, open.host->workspace(), open.root(), open.inspector));
        REQUIRE(open.save());
    }
    CHECK_FALSE(exists(project, "src/client/Door.luau"));
    CHECK(readFile(project, "content/scenes/main.scene.json").find("print('door')") != std::string::npos);

    Opened again(project);
    CHECK(again.named(again.global("Client"), "Door").empty());
    CHECK(again.source(again.one(again.host->workspace(), "Door")) == "print('door')");
}

TEST_CASE("a file somebody else put where a script wants to go is never written over")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/client/Tool.luau", "print('mine')");
    {
        Opened open(project);
        // Written by somebody else after the project opened, so nothing mounted it.
        project.write("src/server/Tool.luau", "print('theirs')");
        const std::array<core::InstanceId, 1> picked{open.one(open.global("Client"), "Tool")};
        REQUIRE(open.editor.reparent(open.world(), picked, open.global("Server"), open.root(), open.inspector));
        REQUIRE(open.save());
        CHECK(open.named(open.global("Server"), "Tool2").size() == 1);
    }
    CHECK(readFile(project, "src/server/Tool.luau") == "print('theirs')");
    CHECK(readFile(project, "src/server/Tool2.luau") == "print('mine')");
}

TEST_CASE("what a script's file does not hold rides with it: off, attributes and tags")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/server/Spawner.luau", "print('spawn')");
    {
        Opened open(project);
        const core::InstanceId spawner = open.one(open.global("Server"), "Spawner");
        (void)open.world().setProperty(spawner, open.world().atoms().intern("Enabled"), scene::Value{false});
        (void)open.world().setAttribute(spawner, open.world().atoms().intern("Rate"), scene::Value{2.5});
        (void)open.world().addTag(spawner, open.world().atoms().intern("Wave"));
        REQUIRE(open.save());
    }
    Opened again(project);
    const core::InstanceId spawner = again.one(again.global("Server"), "Spawner");
    const std::optional<scene::Value> enabled =
        again.world().getProperty(spawner, again.world().atoms().lookup("Enabled"));
    REQUIRE(enabled.has_value());
    CHECK_FALSE(std::get<bool>(*enabled));
    const scene::Value rate = again.world().getAttribute(spawner, again.world().atoms().lookup("Rate"));
    REQUIRE(std::holds_alternative<double>(rate));
    CHECK(std::get<double>(rate) == doctest::Approx(2.5));
    CHECK(again.world().hasTag(spawner, again.world().atoms().lookup("Wave")));
}

TEST_CASE("a module outside shared and a script inside it keep their class across a reopen")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/shared/Util.luau", "return {}");
    project.write("src/client/Boot.luau", "print('boot')");
    {
        Opened open(project);
        const std::array<core::InstanceId, 1> util{open.one(open.global("Shared"), "Util")};
        REQUIRE(open.editor.reparent(open.world(), util, open.global("Client"), open.root(), open.inspector));
        const std::array<core::InstanceId, 1> boot{open.one(open.global("Client"), "Boot")};
        REQUIRE(open.editor.reparent(open.world(), boot, open.global("Shared"), open.root(), open.inspector));
        REQUIRE(open.save());
    }
    CHECK(readFile(project, "src/client/Util.module.luau") == "return {}");
    CHECK(readFile(project, "src/shared/Boot.script.luau") == "print('boot')");

    Opened again(project);
    CHECK(again.className(again.one(again.global("Client"), "Util")) == "ModuleScript");
    CHECK(again.className(again.one(again.global("Shared"), "Boot")) == "Script");
}

TEST_CASE("a name the file system refuses is made one it takes, in the tree and on disk alike")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/client/Plain.luau", "print('plain')");
    {
        Opened open(project);
        const core::InstanceId plain = open.one(open.global("Client"), "Plain");
        REQUIRE(open.editor.renameInstance(open.world(), plain, open.root(), "What?Now"));
        REQUIRE(open.save());
        CHECK(open.named(open.global("Client"), "What_Now").size() == 1);
    }
    CHECK(readFile(project, "src/client/What_Now.luau") == "print('plain')");
    Opened again(project);
    CHECK(again.source(again.one(again.global("Client"), "What_Now")) == "print('plain')");
}

TEST_CASE("the scene's own scripts are files under its folder, and Save As copies them rather than moving them")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/scenes/main/server/Round.luau", "print('round')");
    {
        Opened open(project);
        const core::InstanceId server = open.service("ServerScriptService");
        const std::array<core::InstanceId, 1> picked{open.one(server, "Round")};
        open.editor.copySelection(open.world(), picked, open.root());
        REQUIRE(open.editor.paste(open.world(), open.service("ClientScriptService"), open.root(), open.inspector));
        REQUIRE(open.save());
        CHECK(readFile(project, "src/scenes/main/client/Round.luau") == "print('round')");

        REQUIRE(open.editor.saveSceneAs(open.world(), "scenes/other.scene.json"));
    }
    // The first scene keeps its code, and the second has its own copy.
    CHECK(readFile(project, "src/scenes/main/server/Round.luau") == "print('round')");
    CHECK(readFile(project, "src/scenes/main/client/Round.luau") == "print('round')");
    CHECK(readFile(project, "src/scenes/other/server/Round.luau") == "print('round')");
    CHECK(readFile(project, "src/scenes/other/client/Round.luau") == "print('round')");
    CHECK(trashed(project).empty());
}

TEST_CASE("saving twice with nothing changed writes nothing and trashes nothing")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/client/Still.luau", "print('still')");
    Opened open(project);
    REQUIRE(open.save());
    app::ScriptFileSync sync = app::syncScriptFiles(*open.host, "main");
    CHECK_FALSE(sync.changedAnything());
    CHECK(sync.problems.empty());
    CHECK(trashed(project).empty());
    CHECK(readFile(project, "src/client/Still.luau") == "print('still')");
}

TEST_CASE("a file script edited in the editor is written by the scene's save, and the save is remembered")
{
    // An audit finding: typing into a `src/` script's tab and saving the scene
    // wrote nothing, and quitting did not ask.
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/client/Edited.luau", "print('before')");
    Opened open(project);
    const core::InstanceId edited = open.one(open.global("Client"), "Edited");
    (void)open.world().setProperty(edited, open.world().atoms().intern("Source"),
                                   scene::Value{std::string("print('after')")});
    REQUIRE(open.save());
    CHECK(readFile(project, "src/client/Edited.luau") == "print('after')");
    // Saving again writes nothing: the file is what the editor holds.
    CHECK_FALSE(app::syncScriptFiles(*open.host, "main").changedAnything());
}

TEST_CASE("a file changed outside the editor and in it keeps both: the disk as it is, the editor's text aside")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/client/Shared.luau", "print('original')");
    Opened open(project);
    const core::InstanceId script = open.one(open.global("Client"), "Shared");
    (void)open.world().setProperty(script, open.world().atoms().intern("Source"),
                                   scene::Value{std::string("print('from the editor')")});
    project.write("src/client/Shared.luau", "print('from outside')");
    REQUIRE(open.save());
    CHECK(readFile(project, "src/client/Shared.luau") == "print('from outside')");
    bool keptAside = false;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(project.root / ".engine" / "conflicts", ec), end;
         it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            std::string text;
            (void)platform::readTextFile(it->path(), text);
            keptAside = keptAside || text == "print('from the editor')";
        }
    }
    CHECK(keptAside);
}

TEST_CASE("a save during play is refused, and Save As never writes over another scene")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("content/scenes/other.scene.json",
                  R"({"format": "scene", "version": 2, "root": {"class": "Workspace", "name": "Workspace"}})");
    Opened open(project);

    open.editor.play(open.world());
    CHECK_FALSE(open.save());
    CHECK(open.editor.status().message.find("stop the game") != std::string::npos);
    open.editor.stop(open.world(), open.inspector);
    CHECK(open.save());

    const std::string before = readFile(project, "content/scenes/other.scene.json");
    CHECK_FALSE(open.editor.saveSceneAs(open.world(), "scenes/other.scene.json"));
    CHECK(readFile(project, "content/scenes/other.scene.json") == before);
    // Its own name is fine: that is Save.
    CHECK(open.editor.saveSceneAs(open.world(), std::string(SceneFile)));
}

TEST_CASE("a global.json that could not be read is never written over or deleted")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("content/global.json", "{ this is not json");
    Opened open(project);
    open.editor.setGlobalUnreadable(open.host->globalUnreadable());
    CHECK(open.host->globalUnreadable());
    REQUIRE(open.save());
    CHECK(readFile(project, "content/global.json") == "{ this is not json");
}

TEST_CASE("the local Player takes nothing, because a scene never saves what is inside it")
{
    Captured log;
    Project project;
    seedScene(project);
    Opened open(project);
    const core::InstanceId player = scene::localPlayerOf(open.world());
    REQUIRE(player.valid());
    CHECK_FALSE(app::Editor::canParentInto(open.world(), player, open.root()));
}

TEST_CASE("a scene renamed or duplicated takes its own code with it")
{
    Captured log;
    Project project;
    seedScene(project);
    project.write("src/scenes/main/server/Round.luau", "print('round')");
    Opened open(project);
    const app::ScriptFileSync copied = app::followSceneScripts(*open.host, "main", "main 2", /*copy=*/true);
    CHECK(copied.problems.empty());
    CHECK(readFile(project, "src/scenes/main 2/server/Round.luau") == "print('round')");
    CHECK(readFile(project, "src/scenes/main/server/Round.luau") == "print('round')");

    // A rename of the open scene moves the folder, and the next save sees
    // nothing to move: the mount table followed.
    const app::ScriptFileSync moved = app::followSceneScripts(*open.host, "main", "arena", /*copy=*/false);
    CHECK(moved.problems.empty());
    CHECK(readFile(project, "src/scenes/arena/server/Round.luau") == "print('round')");
    CHECK_FALSE(exists(project, "src/scenes/main/server/Round.luau"));
    CHECK(open.host->sceneName() == "arena");
    const app::ScriptFileSync after = app::syncScriptFiles(*open.host, "arena");
    CHECK_FALSE(after.changedAnything());

    // Never into a folder that is already there.
    const app::ScriptFileSync refused = app::followSceneScripts(*open.host, "arena", "main 2", /*copy=*/false);
    CHECK_FALSE(refused.problems.empty());
    CHECK(readFile(project, "src/scenes/arena/server/Round.luau") == "print('round')");
}
