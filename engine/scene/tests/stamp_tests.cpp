// Stamps 2.0 (ADR 0155): a copy's pivot, its sids, nested stamps, variants,
// and the children a copy adds and disables. `scene_file_tests.cpp` holds what
// ADRs 0049 and 0051 decided; these are what 0155 changed or added.
#include <doctest/doctest.h>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "engine/scene/components.h"
#include "engine/scene/scene_file.h"
#include "engine/scene/world.h"
#include "scene_fixture.h"

using namespace engine;
using engine::scene::SceneIoReport;
using engine::scene::testing::Fixture;

namespace {

core::InstanceId workspaceOf(Fixture& fixture)
{
    const core::InstanceId id = fixture.world.create(fixture.schema.folderClass);
    fixture.world.setName(id, fixture.atom("Workspace"));
    fixture.world.workspaces().add(id, scene::WorkspaceComponent{});
    return id;
}

core::InstanceId partAt(Fixture& fixture, core::InstanceId parent, std::string_view name, core::DVec3 position)
{
    const core::InstanceId id = fixture.world.create(fixture.schema.partClass);
    fixture.world.setName(id, fixture.atom(name));
    (void)fixture.world.setParent(id, parent);
    scene::PartComponent part;
    part.cframe.position = position;
    fixture.world.parts().add(id, part);
    return id;
}

core::InstanceId modelOf(Fixture& fixture, core::InstanceId parent, std::string_view name)
{
    const core::InstanceId id = fixture.model(name);
    (void)fixture.world.setParent(id, parent);
    return id;
}

// Every stamp a test has written, by name: what a project's `content/` holds.
struct Stamps
{
    std::map<std::string, std::string, std::less<>> texts;

    [[nodiscard]] scene::StampSource source() const
    {
        return [this](std::string_view wanted) -> std::optional<std::string> {
            const auto found = texts.find(wanted);
            return found != texts.end() ? std::optional<std::string>(found->second) : std::nullopt;
        };
    }
};

// The first instance of that name anywhere under `root`.
core::InstanceId find(const scene::World& world, core::InstanceId root, std::string_view name)
{
    std::vector<core::InstanceId> all{root};
    world.collectDescendants(root, all);
    for (const core::InstanceId id : all) {
        if (world.atoms().text(world.name(id)) == name)
            return id;
    }
    return {};
}

core::DVec3 positionOf(const scene::World& world, core::InstanceId id)
{
    const scene::PartComponent* part = world.parts().find(id);
    REQUIRE(part != nullptr);
    return part->cframe.position;
}

void moveBy(Fixture& fixture, core::InstanceId root, core::DVec3 by)
{
    std::vector<core::InstanceId> all{root};
    fixture.world.collectDescendants(root, all);
    for (const core::InstanceId id : all) {
        if (scene::PartComponent* part = fixture.world.parts().find(id); part != nullptr)
            part->cframe.position = part->cframe.position + by;
    }
}

// A scene of one workspace holding `nodes`, in version `version`.
std::string sceneOf(std::string_view nodes, int version)
{
    return R"({"format":"scene","version":)" + std::to_string(version) +
           R"(,"root":{"class":"Workspace","name":"Workspace","children":[)" + std::string(nodes) + "]}}";
}

std::string frame(double x, double y, double z)
{
    return "[" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + ",1,0,0,0,1,0,0,0,1]";
}

} // namespace

// --- S1 (G1): one pivot ----------------------------------------------------------

TEST_CASE("S1 (G1): a part moved in the stamp file moves in a copy placed away from the origin")
{
    // The FPS game's G1: a copy stored every part's place in the world, so a
    // lantern raised in the stamp stayed where it was in every copy that had
    // been moved.
    Stamps stamps;
    Fixture author;
    const core::InstanceId authorRoot = workspaceOf(author);
    const core::InstanceId post = partAt(author, authorRoot, "Post", {});
    (void)partAt(author, post, "Lantern", {0.0, 3.0, 0.0});
    stamps.texts["post"] = scene::writeStamp(author.world, post);

    Fixture game;
    const core::InstanceId workspace = workspaceOf(game);
    const core::InstanceId copy = scene::readStamp(game.world, stamps.texts["post"], workspace, "post");
    REQUIRE(copy.valid());
    moveBy(game, copy, {10.0, 0.0, 0.0});

    scene::StampLibrary library(game.world, stamps.source());
    SceneIoReport wrote;
    const std::string text = scene::writeScene(game.world, &wrote, &library);
    // Where it stands, and nothing of its own: the lantern is where the stamp
    // puts it, relative to the post.
    CHECK(text.find("\"pivot\"") != std::string::npos);
    CHECK(wrote.overrides == 0);

    // The stamp changes: the lantern a metre higher.
    Fixture edit;
    const core::InstanceId editing = scene::readStamp(edit.world, stamps.texts["post"], {}, "post");
    edit.world.parts().find(edit.world.firstChild(editing))->cframe.position = core::DVec3{0.0, 4.0, 0.0};
    stamps.texts["post"] = scene::writeStamp(edit.world, editing);

    Fixture next;
    const core::InstanceId nextRoot = workspaceOf(next);
    REQUIRE_FALSE(scene::readScene(next.world, text, nullptr, stamps.source()).has_value());
    const core::InstanceId lantern = find(next.world, next.world.firstChild(nextRoot), "Lantern");
    REQUIRE(lantern.valid());
    CHECK(positionOf(next.world, lantern).x == doctest::Approx(10.0));
    CHECK(positionOf(next.world, lantern).y == doctest::Approx(4.0));
}

TEST_CASE("S1: a version 2 copy whose parts all moved loads where it was, and saves with one pivot")
{
    Stamps stamps;
    Fixture author;
    const core::InstanceId authorRoot = workspaceOf(author);
    const core::InstanceId crate = modelOf(author, authorRoot, "Crate");
    (void)partAt(author, crate, "A", {});
    (void)partAt(author, crate, "B", {2.0, 0.0, 0.0});
    stamps.texts["crate"] = scene::writeStamp(author.world, crate);

    // Written by the build before: each part's place in the world.
    const std::string old = sceneOf(R"({"stamp":"crate","name":"Crate","overrides":{"A":{"CFrame":)" +
                                        frame(10.0, 0.0, 5.0) + R"(},"B":{"CFrame":)" + frame(12.0, 0.0, 5.0) + "}}}",
                                    2);
    Fixture game;
    const core::InstanceId gameRoot = workspaceOf(game);
    REQUIRE_FALSE(scene::readScene(game.world, old, nullptr, stamps.source()).has_value());
    const core::InstanceId root = game.world.firstChild(gameRoot);
    CHECK(positionOf(game.world, find(game.world, root, "A")).x == doctest::Approx(10.0));
    CHECK(positionOf(game.world, find(game.world, root, "B")).x == doctest::Approx(12.0));

    scene::StampLibrary library(game.world, stamps.source());
    SceneIoReport wrote;
    const std::string text = scene::writeScene(game.world, &wrote, &library);
    CHECK(text.find("\"pivot\"") != std::string::npos);
    CHECK(wrote.overrides == 0);

    Fixture again;
    const core::InstanceId againRoot = workspaceOf(again);
    REQUIRE_FALSE(scene::readScene(again.world, text, nullptr, stamps.source()).has_value());
    const core::InstanceId reread = again.world.firstChild(againRoot);
    CHECK(positionOf(again.world, find(again.world, reread, "A")).x == doctest::Approx(10.0));
    CHECK(positionOf(again.world, find(again.world, reread, "B")).x == doctest::Approx(12.0));
    CHECK(positionOf(again.world, find(again.world, reread, "B")).z == doctest::Approx(5.0));
    // And the bytes settle: what was read is what is written.
    scene::StampLibrary libraryAgain(again.world, stamps.source());
    CHECK(scene::writeScene(again.world, nullptr, &libraryAgain) == text);
}

// --- S2: identities ----------------------------------------------------------------

TEST_CASE("S2: renaming a part in the stamp keeps a copy's override of it")
{
    Stamps stamps;
    Fixture author;
    const core::InstanceId authorRoot = workspaceOf(author);
    const core::InstanceId post = partAt(author, authorRoot, "Post", {});
    (void)partAt(author, post, "Lantern", {0.0, 3.0, 0.0});
    stamps.texts["post"] = scene::writeStamp(author.world, post);

    Fixture game;
    const core::InstanceId workspace = workspaceOf(game);
    const core::InstanceId copy = scene::readStamp(game.world, stamps.texts["post"], workspace, "post");
    scene::testing::setTransparencyOf(*game.world.parts().find(game.world.firstChild(copy)), 0.5f);
    scene::StampLibrary library(game.world, stamps.source());
    const std::string text = scene::writeScene(game.world, nullptr, &library);

    // The stamp's lantern is a lamp now.
    Fixture edit;
    const core::InstanceId editing = scene::readStamp(edit.world, stamps.texts["post"], {}, "post");
    edit.world.setName(edit.world.firstChild(editing), edit.atom("Lamp"));
    stamps.texts["post"] = scene::writeStamp(edit.world, editing);

    Fixture next;
    const core::InstanceId nextRoot = workspaceOf(next);
    REQUIRE_FALSE(scene::readScene(next.world, text, nullptr, stamps.source()).has_value());
    const core::InstanceId lamp = find(next.world, next.world.firstChild(nextRoot), "Lamp");
    REQUIRE(lamp.valid());
    CHECK(static_cast<double>(scene::testing::transparencyOf(*next.world.parts().find(lamp))) == doctest::Approx(0.5));
}

TEST_CASE("S2: a version 2 override keyed by a name is applied, and written back by sid")
{
    Stamps stamps;
    Fixture author;
    const core::InstanceId authorRoot = workspaceOf(author);
    const core::InstanceId post = partAt(author, authorRoot, "Post", {});
    (void)partAt(author, post, "Lantern", {0.0, 3.0, 0.0});
    stamps.texts["post"] = scene::writeStamp(author.world, post);

    Fixture game;
    const core::InstanceId gameRoot = workspaceOf(game);
    REQUIRE_FALSE(
        scene::readScene(game.world,
                         sceneOf(R"({"stamp":"post","name":"Post","overrides":{"Lantern":{"Transparency":0.5}}})", 2),
                         nullptr, stamps.source())
            .has_value());
    const core::InstanceId lantern = find(game.world, game.world.firstChild(gameRoot), "Lantern");
    CHECK(static_cast<double>(scene::testing::transparencyOf(*game.world.parts().find(lantern))) ==
          doctest::Approx(0.5));

    scene::StampLibrary library(game.world, stamps.source());
    SceneIoReport wrote;
    const std::string text = scene::writeScene(game.world, &wrote, &library);
    CHECK(wrote.overrides == 1);
    CHECK(text.find("\"Lantern\"") == std::string::npos);
}

// --- S3: a stamp holds stamps --------------------------------------------------------

TEST_CASE("S3: a change to a stamp reaches it inside another, and an override inside reaches one copy")
{
    Stamps stamps;
    Fixture author;
    const core::InstanceId authorRoot = workspaceOf(author);
    const core::InstanceId lamp = partAt(author, authorRoot, "Lamp", {});
    (void)partAt(author, lamp, "Bulb", {0.0, 3.0, 0.0});
    stamps.texts["lamp"] = scene::writeStamp(author.world, lamp);

    // A street of two lamps, each still a lamp.
    const core::InstanceId street = modelOf(author, authorRoot, "Street");
    const core::InstanceId west = scene::readStamp(author.world, stamps.texts["lamp"], street, "lamp");
    const core::InstanceId east = scene::readStamp(author.world, stamps.texts["lamp"], street, "lamp");
    author.world.setName(west, author.atom("West"));
    author.world.setName(east, author.atom("East"));
    moveBy(author, east, {8.0, 0.0, 0.0});
    scene::StampLibrary authoring(author.world, stamps.source());
    stamps.texts["street"] = scene::writeStamp(author.world, street, nullptr, &authoring);
    CHECK(stamps.texts["street"].find("\"stamp\":\"lamp\"") != std::string::npos);

    Fixture game;
    const core::InstanceId workspace = workspaceOf(game);
    scene::SceneIoReport placing;
    const scene::StampSource others = stamps.source();
    const core::InstanceId copy =
        scene::readStamp(game.world, stamps.texts["street"], workspace, "street", &placing, &others);
    REQUIRE(copy.valid());
    // The east lamp's bulb dimmed, in this street only.
    const core::InstanceId eastBulb = game.world.firstChild(find(game.world, copy, "East"));
    scene::testing::setTransparencyOf(*game.world.parts().find(eastBulb), 0.5f);
    scene::StampLibrary library(game.world, stamps.source());
    const std::string text = scene::writeScene(game.world, nullptr, &library);

    // The lamp gains a glass.
    Fixture edit;
    const core::InstanceId editing = scene::readStamp(edit.world, stamps.texts["lamp"], {}, "lamp");
    (void)partAt(edit, editing, "Glass", {0.0, 3.5, 0.0});
    stamps.texts["lamp"] = scene::writeStamp(edit.world, editing);

    Fixture next;
    const core::InstanceId nextRoot = workspaceOf(next);
    REQUIRE_FALSE(scene::readScene(next.world, text, nullptr, stamps.source()).has_value());
    const core::InstanceId placed = next.world.firstChild(nextRoot);
    const core::InstanceId westLamp = find(next.world, placed, "West");
    const core::InstanceId eastLamp = find(next.world, placed, "East");
    REQUIRE(westLamp.valid());
    REQUIRE(eastLamp.valid());
    CHECK(next.childNames(westLamp) == std::vector<std::string>{"Bulb", "Glass"});
    CHECK(next.childNames(eastLamp) == std::vector<std::string>{"Bulb", "Glass"});
    CHECK(positionOf(next.world, find(next.world, eastLamp, "Glass")).x == doctest::Approx(8.0));
    CHECK(static_cast<double>(scene::testing::transparencyOf(
              *next.world.parts().find(next.world.firstChild(eastLamp)))) == doctest::Approx(0.5));
    CHECK(static_cast<double>(scene::testing::transparencyOf(
              *next.world.parts().find(next.world.firstChild(westLamp)))) != doctest::Approx(0.5));
}

TEST_CASE("S3: a stamp that reaches itself is refused, and the chain is named")
{
    Stamps stamps;
    stamps.texts["a"] = R"({"format":"scene","version":3,"root":{"class":"Model","name":"A","children":[)"
                        R"({"class":"Model","name":"B","stamp":"b","sid":"00000001"}]}})";
    stamps.texts["b"] = R"({"format":"scene","version":3,"root":{"class":"Model","name":"B","children":[)"
                        R"({"class":"Model","name":"A","stamp":"a","sid":"00000002"}]}})";
    Fixture game;
    (void)workspaceOf(game);
    SceneIoReport read;
    REQUIRE_FALSE(
        scene::readScene(game.world, sceneOf(R"({"stamp":"a","name":"A"})", 3), &read, stamps.source()).has_value());
    CHECK(read.stampCycles == 1);
    CHECK(read.stampCycle == "a -> b -> a");
}

// --- S4: variants -----------------------------------------------------------------------

TEST_CASE("S4: a change to the base reaches a variant and a variant of it, except where they override")
{
    Stamps stamps;
    Fixture author;
    const core::InstanceId authorRoot = workspaceOf(author);
    const core::InstanceId fighter = partAt(author, authorRoot, "Fighter", {});
    (void)partAt(author, fighter, "Hat", {0.0, 2.0, 0.0});
    stamps.texts["fighter"] = scene::writeStamp(author.world, fighter);

    // The scout: a dimmer hat and a scope.
    const scene::StampSource others = stamps.source();
    Fixture scoutAuthor;
    const core::InstanceId scout = scene::readStamp(scoutAuthor.world, stamps.texts["fighter"], {}, "fighter");
    scene::testing::setTransparencyOf(*scoutAuthor.world.parts().find(scoutAuthor.world.firstChild(scout)), 0.25f);
    (void)partAt(scoutAuthor, scout, "Scope", {0.5, 2.0, 0.0});
    scene::StampLibrary scoutLibrary(scoutAuthor.world, stamps.source());
    stamps.texts["scout"] = scene::writeStamp(scoutAuthor.world, scout, nullptr, &scoutLibrary, "fighter");
    CHECK(scene::stampBaseOf(stamps.texts["scout"]) == "fighter");
    // Its own, and nothing of the base's: no hat in full.
    CHECK(stamps.texts["scout"].find("\"Hat\"") == std::string::npos);

    // The ranger: a scout with a cloak.
    Fixture rangerAuthor;
    const core::InstanceId ranger =
        scene::readStamp(rangerAuthor.world, stamps.texts["scout"], {}, "scout", nullptr, &others);
    REQUIRE(ranger.valid());
    CHECK(rangerAuthor.childNames(ranger) == std::vector<std::string>{"Hat", "Scope"});
    (void)partAt(rangerAuthor, ranger, "Cloak", {0.0, 1.0, 0.0});
    scene::StampLibrary rangerLibrary(rangerAuthor.world, stamps.source());
    stamps.texts["ranger"] = scene::writeStamp(rangerAuthor.world, ranger, nullptr, &rangerLibrary, "scout");

    Fixture game;
    const core::InstanceId workspace = workspaceOf(game);
    const core::InstanceId placed =
        scene::readStamp(game.world, stamps.texts["ranger"], workspace, "ranger", nullptr, &others);
    REQUIRE(placed.valid());
    scene::StampLibrary library(game.world, stamps.source());
    const std::string text = scene::writeScene(game.world, nullptr, &library);

    // The base gains boots.
    Fixture edit;
    const core::InstanceId editing = scene::readStamp(edit.world, stamps.texts["fighter"], {}, "fighter");
    (void)partAt(edit, editing, "Boots", {0.0, 0.0, 0.0});
    stamps.texts["fighter"] = scene::writeStamp(edit.world, editing);

    Fixture next;
    const core::InstanceId nextRoot = workspaceOf(next);
    REQUIRE_FALSE(scene::readScene(next.world, text, nullptr, stamps.source()).has_value());
    const core::InstanceId reread = next.world.firstChild(nextRoot);
    CHECK(next.childNames(reread) == std::vector<std::string>{"Hat", "Boots", "Scope", "Cloak"});
    CHECK(static_cast<double>(scene::testing::transparencyOf(
              *next.world.parts().find(find(next.world, reread, "Hat")))) == doctest::Approx(0.25));
    CHECK(next.world.atoms().text(next.world.stampOf(reread)) == "ranger");
}

// --- S5: added and disabled -----------------------------------------------------------------

TEST_CASE("S5: a child disabled in a copy stays unbuilt while the stamp moves on, and comes back when enabled")
{
    Stamps stamps;
    Fixture author;
    const core::InstanceId authorRoot = workspaceOf(author);
    const core::InstanceId post = partAt(author, authorRoot, "Post", {});
    (void)partAt(author, post, "Lantern", {0.0, 3.0, 0.0});
    (void)partAt(author, post, "Banner", {0.0, 2.0, 0.0});
    stamps.texts["post"] = scene::writeStamp(author.world, post);

    Fixture game;
    const core::InstanceId workspace = workspaceOf(game);
    const core::InstanceId copy = scene::readStamp(game.world, stamps.texts["post"], workspace, "post");
    (void)game.world.destroy(find(game.world, copy, "Banner"));
    game.world.retireDestroyed();
    scene::StampLibrary library(game.world, stamps.source());
    SceneIoReport wrote;
    const std::string text = scene::writeScene(game.world, &wrote, &library);
    CHECK(wrote.stamped == 1);
    CHECK(text.find("\"disabled\"") != std::string::npos);

    // The stamp gains a flag.
    Fixture edit;
    const core::InstanceId editing = scene::readStamp(edit.world, stamps.texts["post"], {}, "post");
    (void)partAt(edit, editing, "Flag", {0.0, 4.0, 0.0});
    stamps.texts["post"] = scene::writeStamp(edit.world, editing);

    Fixture next;
    const core::InstanceId nextRoot = workspaceOf(next);
    REQUIRE_FALSE(scene::readScene(next.world, text, nullptr, stamps.source()).has_value());
    CHECK(next.childNames(next.world.firstChild(nextRoot)) == std::vector<std::string>{"Lantern", "Flag"});

    // Enabled again: the list without it, and the banner is the stamp's.
    const std::string enabled = std::regex_replace(text, std::regex(R"(,\s*"disabled"\s*:\s*\[[^\]]*\])"), "");
    REQUIRE(enabled.find("\"disabled\"") == std::string::npos);
    Fixture back;
    const core::InstanceId backRoot = workspaceOf(back);
    REQUIRE_FALSE(scene::readScene(back.world, enabled, nullptr, stamps.source()).has_value());
    CHECK(back.childNames(back.world.firstChild(backRoot)) == std::vector<std::string>{"Lantern", "Banner", "Flag"});
}

TEST_CASE("S5: a stamp's child moved under another is disabled where it was and added where it is")
{
    Stamps stamps;
    Fixture author;
    const core::InstanceId authorRoot = workspaceOf(author);
    const core::InstanceId post = partAt(author, authorRoot, "Post", {});
    (void)partAt(author, post, "Lantern", {0.0, 3.0, 0.0});
    (void)partAt(author, post, "Banner", {0.0, 2.0, 0.0});
    stamps.texts["post"] = scene::writeStamp(author.world, post);

    Fixture game;
    const core::InstanceId workspace = workspaceOf(game);
    const core::InstanceId copy = scene::readStamp(game.world, stamps.texts["post"], workspace, "post");
    (void)game.world.setParent(find(game.world, copy, "Banner"), find(game.world, copy, "Lantern"));
    scene::StampLibrary library(game.world, stamps.source());
    SceneIoReport wrote;
    const std::string text = scene::writeScene(game.world, &wrote, &library);
    CHECK(wrote.stamped == 1);
    CHECK(wrote.unlinkedStamps == 0);
    CHECK(text.find("\"disabled\"") != std::string::npos);
    CHECK(text.find("\"added\"") != std::string::npos);

    Fixture next;
    const core::InstanceId nextRoot = workspaceOf(next);
    REQUIRE_FALSE(scene::readScene(next.world, text, nullptr, stamps.source()).has_value());
    const core::InstanceId placed = next.world.firstChild(nextRoot);
    CHECK(next.childNames(placed) == std::vector<std::string>{"Lantern"});
    CHECK(next.childNames(find(next.world, placed, "Lantern")) == std::vector<std::string>{"Banner"});
    CHECK(next.world.atoms().text(next.world.stampOf(placed)) == "post");
}

// --- S8: what a Construct built ------------------------------------------------------

TEST_CASE("S8: what a stamp's Construct built is never saved, and never taken for an added child")
{
    Stamps stamps;
    Fixture author;
    const core::InstanceId authorRoot = workspaceOf(author);
    const core::InstanceId fence = modelOf(author, authorRoot, "Fence");
    (void)partAt(author, fence, "Gate", {});
    stamps.texts["fence"] = scene::writeStamp(author.world, fence);

    Fixture game;
    const core::InstanceId workspace = workspaceOf(game);
    const core::InstanceId copy = scene::readStamp(game.world, stamps.texts["fence"], workspace, "fence");
    const core::InstanceId post = partAt(game, copy, "Post1", {2.0, 0.0, 0.0});
    game.world.setConstructed(post, true);

    scene::StampLibrary library(game.world, stamps.source());
    SceneIoReport wrote;
    const std::string text = scene::writeScene(game.world, &wrote, &library);
    CHECK(text.find("Post1") == std::string::npos);
    CHECK(text.find("\"added\"") == std::string::npos);
    CHECK(wrote.overrides == 0);
}

// --- G31: an attribute override is that attribute ------------------------------------

TEST_CASE("G31: a copy that sets one attribute keeps its stamp's others, and the stamp's changes to them")
{
    Stamps stamps;
    Fixture author;
    const core::InstanceId authorRoot = workspaceOf(author);
    const core::InstanceId spawn = partAt(author, authorRoot, "Spawn", {});
    REQUIRE(author.world.setAttribute(spawn, author.atom("Team"), scene::Value{std::string()}));
    REQUIRE(author.world.setAttribute(spawn, author.atom("Marker"), scene::Value{std::string("Spawn")}));
    REQUIRE(author.world.setAttribute(spawn, author.atom("Weight"), scene::Value{1.0}));
    REQUIRE(author.world.addTag(spawn, author.atom("Pad")));
    stamps.texts["spawn"] = scene::writeStamp(author.world, spawn);

    Fixture game;
    const core::InstanceId workspace = workspaceOf(game);
    const core::InstanceId copy = scene::readStamp(game.world, stamps.texts["spawn"], workspace, "spawn");
    REQUIRE(game.world.setAttribute(copy, game.atom("Team"), scene::Value{std::string("Red")}));
    REQUIRE(game.world.setAttribute(copy, game.atom("Weight"), scene::Value{}));
    REQUIRE(game.world.addTag(copy, game.atom("Red")));
    scene::StampLibrary library(game.world, stamps.source());
    const std::string text = scene::writeScene(game.world, nullptr, &library);
    // Only what it changed.
    CHECK(text.find("Marker") == std::string::npos);

    // The stamp's marker changes, and it gains a tag.
    Fixture edit;
    const core::InstanceId editing = scene::readStamp(edit.world, stamps.texts["spawn"], {}, "spawn");
    REQUIRE(edit.world.setAttribute(editing, edit.atom("Marker"), scene::Value{std::string("Start")}));
    REQUIRE(edit.world.addTag(editing, edit.atom("Glow")));
    stamps.texts["spawn"] = scene::writeStamp(edit.world, editing);

    Fixture next;
    const core::InstanceId nextRoot = workspaceOf(next);
    REQUIRE_FALSE(scene::readScene(next.world, text, nullptr, stamps.source()).has_value());
    const core::InstanceId placed = next.world.firstChild(nextRoot);
    REQUIRE(placed.valid());
    CHECK(std::get<std::string>(next.world.getAttribute(placed, next.atom("Team"))) == "Red");
    CHECK(std::get<std::string>(next.world.getAttribute(placed, next.atom("Marker"))) == "Start");
    CHECK(scene::valueType(next.world.getAttribute(placed, next.atom("Weight"))) == scene::ValueType::Nil);
    CHECK(next.world.hasTag(placed, next.atom("Pad")));
    CHECK(next.world.hasTag(placed, next.atom("Red")));
    CHECK(next.world.hasTag(placed, next.atom("Glow")));
}
