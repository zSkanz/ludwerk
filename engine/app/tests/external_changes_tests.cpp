// **A file changed outside the editor while it is open** (the file-watching
// batch): the watcher notices it once it has settled, and the editor takes it
// in without losing anything: the editor open, and another program editing
// the scripts in VS Code.
#include <algorithm>
#include <array>
#include <doctest/doctest.h>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine/app/editor.h"
#include "engine/app/external_changes.h"
#include "engine/app/inspector.h"
#include "engine/app/project_watch.h"
#include "engine/app/script_editor.h"
#include "engine/app/world_host.h"
#include "engine/platform/file.h"
#include "engine/scene/world.h"
#include "engine/script/modules.h"
#include "project_fixture.h"

using namespace engine;
using engine::app::FileChange;
using engine::app::FileChangeKind;
using engine::app::testing::bootOptions;
using engine::app::testing::Captured;
using engine::app::testing::Project;

namespace {

// A project opened as the editor opens it, with its script editor.
struct Opened
{
    std::unique_ptr<app::WorldHost> host = std::make_unique<app::WorldHost>();
    app::ScriptEditor scripts;

    explicit Opened(const Project& project)
    {
        app::WorldHostOptions options = bootOptions(project.root);
        REQUIRE_FALSE(host->boot(options).has_value());
    }

    [[nodiscard]] scene::World& world() { return host->world(); }

    [[nodiscard]] core::InstanceId script(std::string_view folder, std::string_view name)
    {
        const core::InstanceId root = host->runtime().dataModel();
        const core::InstanceId globals = world().findFirstChildOfClass(
            root, world().classes().findId(world().atoms().lookup("GlobalScriptService")));
        const core::InstanceId holder =
            globals.valid() ? world().findFirstChild(globals, world().atoms().lookup(folder)) : core::InstanceId{};
        return holder.valid() ? world().findFirstChild(holder, world().atoms().lookup(name)) : core::InstanceId{};
    }

    [[nodiscard]] std::string source(core::InstanceId id)
    {
        const std::optional<scene::Value> value = world().getProperty(id, world().atoms().intern("Source"));
        const auto* text = value.has_value() ? std::get_if<std::string>(&*value) : nullptr;
        return text != nullptr ? *text : std::string{};
    }

    // A tab on a mounted script, as double-clicking it in the Explorer opens one.
    app::OpenScript& open(core::InstanceId id, std::string file)
    {
        return scripts.open(id, app::ScriptOrigin::Scene, file, file, "tab", source(id));
    }

    app::ExternalScriptChanges apply(std::vector<FileChange> changes)
    {
        return app::applyExternalScriptChanges(*host, scripts, changes);
    }
};

[[nodiscard]] std::string readFile(const Project& project, std::string_view relative)
{
    std::string text;
    (void)platform::readTextFile(project.root / std::filesystem::path(relative), text);
    return text;
}

// The time written moved on for sure, whatever the file system's resolution.
void ageFile(const Project& project, std::string_view relative)
{
    const std::filesystem::path file = project.root / std::filesystem::path(relative);
    std::error_code ec;
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file, ec) + std::chrono::seconds(5), ec);
}

} // namespace

TEST_CASE("the watcher says a file changed once it has stopped changing, and says nothing of its own saves")
{
    Project project;
    project.write("src/client/Main.luau", "print(1)");
    app::ProjectWatcher watcher(project.root, {"src", "content", "i18n"});
    CHECK(watcher.poll().empty());

    // Changed: not yet, while it may still be being written; then once.
    project.write("src/client/Main.luau", "print(2) -- longer");
    ageFile(project, "src/client/Main.luau");
    CHECK(watcher.poll().empty());
    CHECK(watcher.poll() == std::vector<FileChange>{{"src/client/Main.luau", FileChangeKind::Modified}});
    CHECK(watcher.poll().empty());

    // Added, in a folder that did not exist; an editor's temporary file and
    // the engine's own folder are nobody's change.
    project.write("i18n/pt-BR.json", "{}");
    project.write("src/client/Main.luau~", "x");
    project.write("src/.engine/cache.bin", "x");
    (void)watcher.poll();
    CHECK(watcher.poll() == std::vector<FileChange>{{"i18n/pt-BR.json", FileChangeKind::Added}});

    // Removed: at two looks in a row, so a save that replaces a file is not one.
    std::error_code ec;
    std::filesystem::remove(project.root / "i18n/pt-BR.json", ec);
    CHECK(watcher.poll().empty());
    CHECK(watcher.poll() == std::vector<FileChange>{{"i18n/pt-BR.json", FileChangeKind::Removed}});

    // What the editor wrote itself is not news.
    project.write("src/client/Main.luau", "print(3) -- written by the editor");
    ageFile(project, "src/client/Main.luau");
    watcher.noteWritten("src/client/Main.luau");
    CHECK(watcher.poll().empty());
    CHECK(watcher.poll().empty());
}

TEST_CASE("the watcher's thread hands over what settled, one change a path, and stops when asked")
{
    Project project;
    project.write("src/client/Main.luau", "print(1)");
    std::vector<FileChange> seen;
    {
        app::ProjectWatcherThread watcher(project.root, {"src"}, std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        project.write("src/client/Main.luau", "print(2) -- changed");
        ageFile(project, "src/client/Main.luau");
        for (int attempt = 0; attempt < 200 && seen.empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            seen = watcher.take();
        }
        CHECK(watcher.take().empty());
    }
    CHECK(seen == std::vector<FileChange>{{"src/client/Main.luau", FileChangeKind::Modified}});
}

TEST_CASE("a script changed outside takes the disk's text, its tab too, and a touch changes nothing")
{
    Captured log;
    Project project;
    project.write("src/client/Main.luau", "print('before')");
    Opened open(project);
    const core::InstanceId main = open.script("Client", "Main");
    REQUIRE(main.valid());
    app::OpenScript& tab = open.open(main, "src/client/Main.luau");
    tab.caret.head = tab.caret.anchor = app::Position{0, 6};

    project.write("src/client/Main.luau", "print('after')\nprint('more')");
    const app::ExternalScriptChanges changed = open.apply({{"src/client/Main.luau", FileChangeKind::Modified}});
    CHECK(changed.reloaded == std::vector<std::string>{"src/client/Main.luau"});
    // The next Play compiles `Source`, and `Source` is the disk's.
    CHECK(open.source(main) == "print('after')\nprint('more')");
    CHECK(tab.document.text() == "print('after')\nprint('more')");
    CHECK_FALSE(tab.dirty());
    CHECK(tab.caret.head.line == 0);

    // The same bytes again -- a touch, a save of nothing -- is not a change.
    CHECK_FALSE(open.apply({{"src/client/Main.luau", FileChangeKind::Modified}}).any());
}

TEST_CASE("a script edited here and changed outside asks, and either answer loses nothing")
{
    Captured log;
    Project project;
    project.write("src/client/Main.luau", "print('disk')");
    Opened open(project);
    const core::InstanceId main = open.script("Client", "Main");
    REQUIRE(main.valid());
    app::OpenScript& tab = open.open(main, "src/client/Main.luau");
    // Typed in the tab, which writes `Source` as it goes.
    (void)tab.document.setText("print('mine')");
    (void)open.world().setProperty(main, open.world().atoms().intern("Source"),
                                   scene::Value{std::string("print('mine')")});

    project.write("src/client/Main.luau", "print('theirs')");
    const app::ExternalScriptChanges changed = open.apply({{"src/client/Main.luau", FileChangeKind::Modified}});
    CHECK(changed.conflicts == std::vector<std::string>{"src/client/Main.luau"});
    CHECK(tab.changedOnDisk);
    // Neither side moved yet.
    CHECK(open.source(main) == "print('mine')");
    CHECK(readFile(project, "src/client/Main.luau") == "print('theirs')");

    SUBCASE("take the disk's")
    {
        REQUIRE(app::takeDiskText(*open.host, tab));
        CHECK(open.source(main) == "print('theirs')");
        CHECK(tab.document.text() == "print('theirs')");
        CHECK_FALSE(tab.changedOnDisk);
    }
    SUBCASE("keep mine")
    {
        REQUIRE(app::keepEditorText(*open.host, tab));
        CHECK(readFile(project, "src/client/Main.luau") == "print('mine')");
        CHECK(open.source(main) == "print('mine')");
        CHECK_FALSE(tab.changedOnDisk);
        // And the file now holds what was read: no further question.
        CHECK_FALSE(open.apply({{"src/client/Main.luau", FileChangeKind::Modified}}).any());
    }
}

TEST_CASE("a new file under src becomes a script where the project's open puts it, and a file gone takes its")
{
    Captured log;
    Project project;
    project.write("src/client/Main.luau", "print('main')");
    Opened open(project);

    project.write("src/server/Spawner.luau", "print('spawn')");
    project.write("src/shared/Tools.luau", "return {}");
    project.write("src/scenes/elsewhere/server/Other.luau", "print('not this scene')");
    const app::ExternalScriptChanges added =
        open.apply({{"src/scenes/elsewhere/server/Other.luau", FileChangeKind::Added},
                    {"src/server/Spawner.luau", FileChangeKind::Added},
                    {"src/shared/Tools.luau", FileChangeKind::Added}});
    CHECK(added.mounted == std::vector<std::string>{"src/server/Spawner.luau", "src/shared/Tools.luau"});
    const core::InstanceId spawner = open.script("Server", "Spawner");
    REQUIRE(spawner.valid());
    CHECK(open.source(spawner) == "print('spawn')");
    const core::InstanceId tools = open.script("Shared", "Tools");
    REQUIRE(tools.valid());
    CHECK(open.world().classOf(tools) == open.world().classes().findId(open.world().atoms().lookup("ModuleScript")));
    CHECK(script::mountedPathOf(open.host->runtime().state(), spawner) == "src/server/Spawner.luau");

    // Gone: an unedited script goes with its file; an edited one stays.
    const core::InstanceId main = open.script("Client", "Main");
    (void)open.world().setProperty(main, open.world().atoms().intern("Source"),
                                   scene::Value{std::string("print('edited')")});
    std::error_code ec;
    std::filesystem::remove(project.root / "src/server/Spawner.luau", ec);
    std::filesystem::remove(project.root / "src/client/Main.luau", ec);
    const app::ExternalScriptChanges gone = open.apply(
        {{"src/client/Main.luau", FileChangeKind::Removed}, {"src/server/Spawner.luau", FileChangeKind::Removed}});
    CHECK(gone.removed == std::vector<std::string>{"src/server/Spawner.luau"});
    CHECK(gone.keptEdited == std::vector<std::string>{"src/client/Main.luau"});
    // Out of the tree at once; its handle stops resolving at the end of the
    // frame's drain, as every `Destroy`'s does.
    CHECK_FALSE(open.script("Server", "Spawner").valid());
    CHECK(open.script("Client", "Main") == main);
    CHECK(script::mountedPathOf(open.host->runtime().state(), spawner).empty());
}

TEST_CASE("a scene changed outside is never written over: the save keeps the disk, and keep mine is a choice")
{
    Captured log;
    Project project;
    constexpr std::string_view Scene = "scenes/main.scene.json";
    project.write("content/scenes/main.scene.json",
                  R"({"format": "scene", "version": 2, "root": {"class": "Workspace", "name": "Workspace"}})");
    app::WorldHostOptions options = bootOptions(project.root);
    options.bootScene = project.root / "content" / std::filesystem::path(Scene);
    auto host = std::make_unique<app::WorldHost>();
    REQUIRE_FALSE(host->boot(options).has_value());
    app::Editor editor;
    editor.openContent(project.root / "content");
    editor.adoptOpenScene(Scene);

    // Its own save is not a change from outside.
    REQUIRE(editor.saveOpenScene(host->world()));
    CHECK_FALSE(editor.changedOutside(Scene));

    // Changed outside: noticed, and the save refuses to write over it.
    const std::string theirs =
        R"({"format": "scene", "version": 2, "root": {"class": "Workspace", "name": "Workspace", "children": []}})";
    project.write("content/scenes/main.scene.json", theirs);
    CHECK(editor.changedOutside(Scene));
    CHECK_FALSE(editor.saveOpenScene(host->world()));
    CHECK(readFile(project, "content/scenes/main.scene.json") == theirs);
    // The editor's version is beside it, not lost.
    CHECK_FALSE(readFile(project, ".engine/conflicts/scenes/main.scene.json").empty());

    // Asked once, however often it is noticed.
    editor.askAboutOutsideChange({std::string(Scene), app::Editor::OutsideKind::Scene});
    editor.askAboutOutsideChange({std::string(Scene), app::Editor::OutsideKind::Scene});
    CHECK(editor.outsideChanges().size() == 1);

    // Keep mine: the disk as it is now is taken as read, and the save writes.
    editor.keepMineOver(Scene);
    CHECK(editor.outsideChanges().empty());
    CHECK_FALSE(editor.changedOutside(Scene));
    CHECK(editor.saveOpenScene(host->world()));
    CHECK(readFile(project, "content/scenes/main.scene.json") != theirs);
}

TEST_CASE("the next Play runs what the disk holds, not what the editor opened with")
{
    // **Reported**: the editor open, an agent editing a script in VS Code,
    // and Play running the old code until the project was opened again.
    Captured log;
    Project project;
    project.write("src/client/Main.luau", "print('opened with')");
    app::WorldHost host;
    app::WorldHostOptions options = bootOptions(project.root);
    options.startScripts = false;
    REQUIRE_FALSE(host.boot(options).has_value());
    app::ScriptEditor scripts;

    project.write("src/client/Main.luau", "print('edited outside')");
    const std::array<FileChange, 1> changed{{{"src/client/Main.luau", FileChangeKind::Modified}}};
    CHECK(app::applyExternalScriptChanges(host, scripts, changed).reloaded.size() == 1);

    // What the play button does.
    script::startScripts(host.runtime().state());
    host.tick();
    CHECK(log.contains("edited outside"));
    CHECK_FALSE(log.contains("opened with"));
}
