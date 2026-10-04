#include <doctest/doctest.h>

// doctest stringifies whatever a CHECK compares, and that needs the stream
// operators for std::string and std::string_view to be visible here.
#include <algorithm>
#include <array>
#include <ostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "engine/scene/components.h"
#include "engine/scene/scene_file.h"
#include "scene_fixture.h"

using engine::core::InstanceId;
using engine::core::u64;
using engine::core::usize;
using engine::core::Vec3;
using engine::scene::Change;
using engine::scene::ChangeKind;
using engine::scene::Value;
using engine::scene::valueType;
using engine::scene::valueTypeName;
using engine::scene::World;
using engine::scene::testing::Fixture;

namespace {

[[nodiscard]] std::vector<Change> drain(World& world)
{
    const auto span = world.changes().take();
    return std::vector<Change>(span.begin(), span.end());
}

[[nodiscard]] usize countOf(const std::vector<Change>& changes, ChangeKind kind)
{
    return static_cast<usize>(
        std::count_if(changes.begin(), changes.end(), [kind](const Change& change) { return change.kind == kind; }));
}

[[nodiscard]] std::vector<ChangeKind> kindsOf(const std::vector<Change>& changes)
{
    std::vector<ChangeKind> out;
    for (const Change& change : changes)
        out.push_back(change.kind);
    return out;
}

// A scene is rooted at a Workspace and the fixture has no Workspace class.
// Attaching the component to a folder is what `scene_file_tests.cpp` does for
// the same reason: teaching the fixture a class would edit a header four suites
// share.
[[nodiscard]] InstanceId makeWorkspace(Fixture& fixture)
{
    const InstanceId id = fixture.world.create(fixture.schema.folderClass);
    fixture.world.setName(id, fixture.atom("Workspace"));
    fixture.world.workspaces().add(id, engine::scene::WorkspaceComponent{});
    return id;
}

} // namespace

// --- Lifetime ---------------------------------------------------------------

TEST_CASE("create refuses an abstract class and names the instance by default")
{
    Fixture fixture;

    CHECK_FALSE(fixture.world.create(fixture.schema.instanceClass).valid());
    CHECK_FALSE(fixture.world.create(fixture.schema.basePartClass).valid());
    CHECK_FALSE(fixture.world.create(4242).valid());

    const InstanceId part = fixture.world.create(fixture.schema.partClass);
    REQUIRE(part.valid());
    CHECK(fixture.nameOf(part) == "Part");
    CHECK_FALSE(fixture.world.parentOf(part).valid());
    CHECK(fixture.world.alive(part));
}

TEST_CASE("component hooks run root-first so a subclass inherits its base's storage")
{
    Fixture fixture;
    const InstanceId part = fixture.world.create(fixture.schema.partClass);

    // `PartComponent` is declared by BasePart, and Part gets it without Part
    // knowing anything about it.
    CHECK(fixture.world.parts().find(part) != nullptr);
    CHECK(fixture.world.models().find(part) == nullptr);

    const InstanceId model = fixture.world.create(fixture.schema.modelClass);
    CHECK(fixture.world.models().find(model) != nullptr);
    CHECK(fixture.world.parts().find(model) == nullptr);
}

// --- Hierarchy --------------------------------------------------------------

TEST_CASE("child order is parenting order")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId a = fixture.folder("A");
    const InstanceId b = fixture.folder("B");
    const InstanceId c = fixture.folder("C");

    CHECK_FALSE(fixture.world.setParent(a, root).has_value());
    CHECK_FALSE(fixture.world.setParent(b, root).has_value());
    CHECK_FALSE(fixture.world.setParent(c, root).has_value());
    CHECK(fixture.childNames(root) == std::vector<std::string>{"A", "B", "C"});
    CHECK(fixture.world.childCount(root) == 3);

    // A re-parented child goes last, even back under the parent it left.
    CHECK_FALSE(fixture.world.setParent(a, InstanceId{}).has_value());
    CHECK_FALSE(fixture.world.setParent(a, root).has_value());
    CHECK(fixture.childNames(root) == std::vector<std::string>{"B", "C", "A"});
}

TEST_CASE("assigning the current parent again changes nothing and says nothing")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId a = fixture.folder("A");
    const InstanceId b = fixture.folder("B");
    REQUIRE_FALSE(fixture.world.setParent(a, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(b, root).has_value());
    (void)drain(fixture.world);

    CHECK_FALSE(fixture.world.setParent(a, root).has_value());

    CHECK(fixture.childNames(root) == std::vector<std::string>{"A", "B"});
    CHECK(drain(fixture.world).empty());
}

// --- Sibling order ----------------------------------------------------------
//
// `moveChild` is the one hierarchy verb that is not a re-parent. Everything
// above this line is about a child arriving somewhere; these are about a child
// that is already there ending up somewhere else in the same list.

TEST_CASE("a child moves to the place it is given")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId a = fixture.folder("A");
    const InstanceId b = fixture.folder("B");
    const InstanceId c = fixture.folder("C");
    const InstanceId d = fixture.folder("D");
    for (const InstanceId child : {a, b, c, d})
        REQUIRE_FALSE(fixture.world.setParent(child, root).has_value());
    REQUIRE(fixture.childNames(root) == std::vector<std::string>{"A", "B", "C", "D"});

    SUBCASE("forwards, to the place it will occupy and not the one it displaces")
    {
        // The distinction is the whole of the index contract: A leaves the list
        // before it comes back, so index 2 is counted in the list of four that
        // results. Reading it as "before whatever is at 2 now" would put A one
        // place short of where a person dropped it, every time they dragged
        // downward.
        CHECK(fixture.world.moveChild(root, a, 2) == World::MoveResult::Moved);
        CHECK(fixture.childNames(root) == std::vector<std::string>{"B", "C", "A", "D"});
        CHECK(fixture.world.siblingIndex(a) == 2u);
        CHECK(fixture.world.childCount(root) == 4);
    }
    SUBCASE("backwards")
    {
        CHECK(fixture.world.moveChild(root, d, 1) == World::MoveResult::Moved);
        CHECK(fixture.childNames(root) == std::vector<std::string>{"A", "D", "B", "C"});
        CHECK(fixture.world.siblingIndex(d) == 1u);
        CHECK(fixture.world.childCount(root) == 4);
    }
    SUBCASE("to the front")
    {
        CHECK(fixture.world.moveChild(root, c, 0) == World::MoveResult::Moved);
        CHECK(fixture.childNames(root) == std::vector<std::string>{"C", "A", "B", "D"});
        CHECK(fixture.world.firstChild(root) == c);
    }
    SUBCASE("to the end, and the end is still where the next child is appended")
    {
        CHECK(fixture.world.moveChild(root, b, 3) == World::MoveResult::Moved);
        CHECK(fixture.childNames(root) == std::vector<std::string>{"A", "C", "D", "B"});

        // `lastChild` is what makes an append O(1). A reorder that moved the
        // tail without telling the parent would leave it pointing at an
        // instance in the middle, and the next thing parented here would land
        // there -- which nothing about parenting would explain.
        const InstanceId e = fixture.folder("E");
        REQUIRE_FALSE(fixture.world.setParent(e, root).has_value());
        CHECK(fixture.childNames(root) == std::vector<std::string>{"A", "C", "D", "B", "E"});
    }
    SUBCASE("and it is a move within one parent, not a re-parent")
    {
        (void)drain(fixture.world);
        REQUIRE(fixture.world.moveChild(root, a, 3) == World::MoveResult::Moved);

        CHECK(fixture.world.parentOf(a) == root);
        // **Deliberately empty, and asserted rather than assumed.** `ChangeKind`
        // has no reorder and the script-facing API has no signal one could
        // feed, so the only entries available would be a `ChildRemoved` and a
        // `ChildAdded` for a child that never left its parent -- two false
        // statements to any handler that looks. See `moveChild`'s contract.
        CHECK(drain(fixture.world).empty());
    }
}

TEST_CASE("a reordered child list is still linked both ways")
{
    // `prevSibling` is private and nothing reads it back, so the only way to
    // ask whether a reorder left it right is to make the world walk it:
    // detaching a child from the middle is the operation that does.
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId a = fixture.folder("A");
    const InstanceId b = fixture.folder("B");
    const InstanceId c = fixture.folder("C");
    for (const InstanceId child : {a, b, c})
        REQUIRE_FALSE(fixture.world.setParent(child, root).has_value());

    REQUIRE(fixture.world.moveChild(root, c, 0) == World::MoveResult::Moved);
    REQUIRE(fixture.childNames(root) == std::vector<std::string>{"C", "A", "B"});

    // From the middle, then from the head, then the tail: each fixes up a
    // different pair of links.
    REQUIRE_FALSE(fixture.world.setParent(a, InstanceId{}).has_value());
    CHECK(fixture.childNames(root) == std::vector<std::string>{"C", "B"});
    REQUIRE_FALSE(fixture.world.setParent(c, InstanceId{}).has_value());
    CHECK(fixture.childNames(root) == std::vector<std::string>{"B"});
    REQUIRE_FALSE(fixture.world.setParent(b, InstanceId{}).has_value());
    CHECK(fixture.childNames(root).empty());
    CHECK(fixture.world.childCount(root) == 0);
    CHECK_FALSE(fixture.world.firstChild(root).valid());
}

TEST_CASE("moving a child to where it already is changes nothing and says so")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId a = fixture.folder("A");
    const InstanceId b = fixture.folder("B");
    REQUIRE_FALSE(fixture.world.setParent(a, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(b, root).has_value());
    (void)drain(fixture.world);
    const u64 before = fixture.world.worldHash();

    // Not a refusal and not a change. The caller's undo stack is why the two
    // successes are told apart at all: a step that undoes nothing eats a press
    // of ctrl-Z, and worse, recording one clears the redo stack (D134).
    CHECK(fixture.world.moveChild(root, b, 1) == World::MoveResult::Unchanged);

    CHECK(fixture.childNames(root) == std::vector<std::string>{"A", "B"});
    CHECK(fixture.world.worldHash() == before);
    CHECK(drain(fixture.world).empty());
}

TEST_CASE("a move that cannot mean anything is refused and the tree is untouched")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId a = fixture.folder("A");
    const InstanceId b = fixture.folder("B");
    const InstanceId elsewhere = fixture.folder("Elsewhere");
    const InstanceId stranger = fixture.folder("Stranger");
    const InstanceId loose = fixture.folder("Loose");
    REQUIRE_FALSE(fixture.world.setParent(a, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(b, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(elsewhere, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(stranger, elsewhere).has_value());
    (void)drain(fixture.world);
    const u64 before = fixture.world.worldHash();

    SUBCASE("a child of somebody else")
    {
        CHECK(fixture.world.moveChild(root, stranger, 0) == World::MoveResult::NotAChild);
    }
    SUBCASE("something with no parent at all")
    {
        CHECK(fixture.world.moveChild(root, loose, 0) == World::MoveResult::NotAChild);
    }
    SUBCASE("a handle that no longer resolves")
    {
        REQUIRE(fixture.world.destroy(a));
        fixture.world.retireDestroyed();
        (void)drain(fixture.world);
        CHECK(fixture.world.moveChild(root, a, 0) == World::MoveResult::NotAChild);
    }
    SUBCASE("a parent that no longer resolves")
    {
        REQUIRE(fixture.world.destroy(elsewhere));
        fixture.world.retireDestroyed();
        (void)drain(fixture.world);
        CHECK(fixture.world.moveChild(elsewhere, stranger, 0) == World::MoveResult::NotAChild);
    }
    SUBCASE("one past the end")
    {
        // Three children, so 3 is not a place. Refused rather than clamped: the
        // index comes from where a person let go of a row, and a clamp would
        // put the instance somewhere else and report success.
        CHECK(fixture.world.moveChild(root, a, 3) == World::MoveResult::IndexOutOfRange);
        CHECK(fixture.childNames(root) == std::vector<std::string>{"A", "B", "Elsewhere"});
        CHECK(fixture.world.worldHash() == before);
    }
    SUBCASE("far past the end")
    {
        CHECK(fixture.world.moveChild(root, a, 4242) == World::MoveResult::IndexOutOfRange);
    }
    SUBCASE("an index into an empty child list")
    {
        CHECK(fixture.world.moveChild(stranger, a, 0) == World::MoveResult::NotAChild);
    }

    CHECK(drain(fixture.world).empty());
}

TEST_CASE("siblingIndex answers for a child and refuses to guess otherwise")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId a = fixture.folder("A");
    const InstanceId b = fixture.folder("B");
    REQUIRE_FALSE(fixture.world.setParent(a, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(b, root).has_value());

    CHECK(fixture.world.siblingIndex(a) == 0u);
    CHECK(fixture.world.siblingIndex(b) == 1u);

    // Nullopt rather than zero. An unparented instance has no siblings, and an
    // answer of "first" would be indistinguishable from a real one -- which is
    // the caller deciding not to record an undo step for a move that would in
    // fact have done something.
    CHECK_FALSE(fixture.world.siblingIndex(root).has_value());
    CHECK_FALSE(fixture.world.siblingIndex(InstanceId{}).has_value());
}

TEST_CASE("a reorder puts the duplicate-name chain back in child order")
{
    // The name chain is in CHILD order (ADR 0026), so a reorder has to rebuild
    // it exactly as a rename does. Leaving it would make `FindFirstChild`
    // answer with a sibling that is no longer first -- a wrong answer that
    // nothing about the reorder would explain, from a lookup that never moved.
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId first = fixture.folder("Tree");
    const InstanceId second = fixture.folder("Tree");
    const InstanceId third = fixture.folder("Tree");
    const auto tree = fixture.atom("Tree");
    for (const InstanceId child : {first, second, third})
        REQUIRE_FALSE(fixture.world.setParent(child, root).has_value());
    REQUIRE(fixture.world.findFirstChild(root, tree) == first);

    SUBCASE("the last one moved to the front is found first")
    {
        REQUIRE(fixture.world.moveChild(root, third, 0) == World::MoveResult::Moved);
        CHECK(fixture.world.findFirstChild(root, tree) == third);

        // And the rest of the chain still runs: renaming the new head away has
        // to promote the one child order now puts first.
        fixture.world.setName(third, fixture.atom("Bush"));
        CHECK(fixture.world.findFirstChild(root, tree) == first);
        fixture.world.setName(first, fixture.atom("Bush"));
        CHECK(fixture.world.findFirstChild(root, tree) == second);
    }
    SUBCASE("the first one moved to the end is found last")
    {
        REQUIRE(fixture.world.moveChild(root, first, 2) == World::MoveResult::Moved);
        CHECK(fixture.world.findFirstChild(root, tree) == second);
        fixture.world.setName(second, fixture.atom("Bush"));
        CHECK(fixture.world.findFirstChild(root, tree) == third);
        fixture.world.setName(third, fixture.atom("Bush"));
        CHECK(fixture.world.findFirstChild(root, tree) == first);
    }
    SUBCASE("one moved through the middle keeps every link")
    {
        const InstanceId other = fixture.folder("Bush");
        REQUIRE_FALSE(fixture.world.setParent(other, root).has_value());
        REQUIRE(fixture.world.moveChild(root, other, 1) == World::MoveResult::Moved);
        CHECK(fixture.childNames(root) == std::vector<std::string>{"Tree", "Bush", "Tree", "Tree"});

        // A name the chain does not hold must not have been dragged into it.
        CHECK(fixture.world.findFirstChild(root, tree) == first);
        CHECK(fixture.world.findFirstChild(root, fixture.atom("Bush")) == other);
        fixture.world.setName(first, fixture.atom("Stump"));
        CHECK(fixture.world.findFirstChild(root, tree) == second);
    }
}

TEST_CASE("the world hash follows sibling order, and only when it moves")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId a = fixture.folder("A");
    const InstanceId b = fixture.folder("B");
    const InstanceId c = fixture.folder("C");
    for (const InstanceId child : {a, b, c})
        REQUIRE_FALSE(fixture.world.setParent(child, root).has_value());
    const u64 base = fixture.world.worldHash();

    REQUIRE(fixture.world.moveChild(root, c, 0) == World::MoveResult::Moved);
    const u64 reordered = fixture.world.worldHash();
    // A reorder the hash could not see is a scene that saves differently from
    // what is on screen: the hash is the same walk the serializer makes.
    CHECK(reordered != base);

    REQUIRE(fixture.world.moveChild(root, c, 0) == World::MoveResult::Unchanged);
    CHECK(fixture.world.worldHash() == reordered);

    // The hash is a function of the order, not of how it got there.
    REQUIRE(fixture.world.moveChild(root, c, 2) == World::MoveResult::Moved);
    CHECK(fixture.world.worldHash() == base);
}

TEST_CASE("a reordered child list is what the scene file writes back")
{
    // Sibling order is part of what a scene reproduces, and until this verb
    // existed every order a file could hold was one that parenting had
    // produced. This is the round trip for an order somebody rearranged: it
    // lives here rather than beside the format's own cases because what is
    // under test is the verb, not the writer.
    Fixture fixture;
    const InstanceId workspace = makeWorkspace(fixture);
    const InstanceId a = fixture.folder("A");
    const InstanceId b = fixture.folder("B");
    const InstanceId c = fixture.folder("C");
    for (const InstanceId child : {a, b, c})
        REQUIRE_FALSE(fixture.world.setParent(child, workspace).has_value());
    REQUIRE(fixture.world.moveChild(workspace, c, 0) == World::MoveResult::Moved);
    REQUIRE(fixture.childNames(workspace) == std::vector<std::string>{"C", "A", "B"});

    const std::string text = engine::scene::writeScene(fixture.world);

    Fixture reloaded;
    const InstanceId reloadedWorkspace = makeWorkspace(reloaded);
    REQUIRE_FALSE(engine::scene::readScene(reloaded.world, text).has_value());

    CHECK(reloaded.childNames(reloadedWorkspace) == std::vector<std::string>{"C", "A", "B"});
    // And the file is the same bytes the second time, which is the format's own
    // oracle for "nothing was lost and nothing was invented".
    CHECK(engine::scene::writeScene(reloaded.world) == text);
}

TEST_CASE("descendants come back depth-first in document order")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId a = fixture.folder("A");
    const InstanceId a1 = fixture.folder("A1");
    const InstanceId a2 = fixture.folder("A2");
    const InstanceId b = fixture.folder("B");
    const InstanceId b1 = fixture.folder("B1");

    REQUIRE_FALSE(fixture.world.setParent(a, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(b, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(a1, a).has_value());
    REQUIRE_FALSE(fixture.world.setParent(a2, a).has_value());
    REQUIRE_FALSE(fixture.world.setParent(b1, b).has_value());

    // Each child immediately followed by its own subtree -- the same order
    // FindFirstChild tie-breaks on (api-design.md §2.2).
    CHECK(fixture.descendantNames(root) == std::vector<std::string>{"A", "A1", "A2", "B", "B1"});
    CHECK(fixture.descendantNames(a) == std::vector<std::string>{"A1", "A2"});
    CHECK(fixture.descendantNames(a1).empty());
}

TEST_CASE("ancestry queries are strict about self")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId child = fixture.folder("Child");
    const InstanceId grandchild = fixture.folder("Grandchild");
    REQUIRE_FALSE(fixture.world.setParent(child, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(grandchild, child).has_value());

    CHECK(fixture.world.isAncestorOf(root, grandchild));
    CHECK(fixture.world.isAncestorOf(child, grandchild));
    CHECK_FALSE(fixture.world.isAncestorOf(grandchild, root));
    CHECK_FALSE(fixture.world.isAncestorOf(root, root));
}

TEST_CASE("a parent cycle is refused and leaves the tree untouched")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId child = fixture.folder("Child");
    const InstanceId grandchild = fixture.folder("Grandchild");
    REQUIRE_FALSE(fixture.world.setParent(child, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(grandchild, child).has_value());
    (void)drain(fixture.world);

    const auto cycleKey = ENG_TR("scene.err.parent_cycle");

    SUBCASE("to itself")
    {
        const auto error = fixture.world.setParent(root, root);
        REQUIRE(error.has_value());
        CHECK(error->hash == cycleKey.hash);
    }
    SUBCASE("to its own child")
    {
        const auto error = fixture.world.setParent(root, child);
        REQUIRE(error.has_value());
        CHECK(error->hash == cycleKey.hash);
    }
    SUBCASE("to a deep descendant")
    {
        const auto error = fixture.world.setParent(root, grandchild);
        REQUIRE(error.has_value());
        CHECK(error->hash == cycleKey.hash);
    }

    // Both halves matter: a refusal that half-applied would be worse than one
    // that raised nothing.
    CHECK_FALSE(fixture.world.parentOf(root).valid());
    CHECK(fixture.world.parentOf(child) == root);
    CHECK(fixture.world.parentOf(grandchild) == child);
    CHECK(drain(fixture.world).empty());
}

// --- Duplicate sibling names (ADR 0026) -------------------------------------

TEST_CASE("FindFirstChild returns the first duplicate in child order")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId first = fixture.folder("Tree");
    const InstanceId second = fixture.folder("Tree");
    const InstanceId third = fixture.folder("Tree");
    const auto tree = fixture.atom("Tree");

    REQUIRE_FALSE(fixture.world.setParent(first, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(second, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(third, root).has_value());

    CHECK(fixture.world.findFirstChild(root, tree) == first);

    SUBCASE("renaming the first away promotes the second")
    {
        fixture.world.setName(first, fixture.atom("Bush"));
        CHECK(fixture.world.findFirstChild(root, tree) == second);
        CHECK(fixture.world.findFirstChild(root, fixture.atom("Bush")) == first);
    }
    SUBCASE("renaming it back puts it where child order says, not at the end")
    {
        fixture.world.setName(first, fixture.atom("Bush"));
        fixture.world.setName(first, tree);
        // This subcase asserted the opposite until 2026-08-20, and it was
        // asserting the implementation rather than ADR 0026: the chain is in
        // CHILD order, and `first` is still the first child. An append put it
        // last, so `FindFirstChild` answered `second` for an instance that had
        // never moved -- which the conformance suite caught, written from the
        // document by an author who had not read this file.
        CHECK(fixture.childNames(root) == std::vector<std::string>{"Tree", "Tree", "Tree"});
        CHECK(fixture.world.findFirstChild(root, tree) == first);
    }
    SUBCASE("detaching the first promotes the second")
    {
        REQUIRE_FALSE(fixture.world.setParent(first, InstanceId{}).has_value());
        CHECK(fixture.world.findFirstChild(root, tree) == second);
    }
    SUBCASE("detaching one from the middle keeps the chain intact")
    {
        REQUIRE_FALSE(fixture.world.setParent(second, InstanceId{}).has_value());
        CHECK(fixture.world.findFirstChild(root, tree) == first);
        fixture.world.setName(first, fixture.atom("Bush"));
        CHECK(fixture.world.findFirstChild(root, tree) == third);
    }
    SUBCASE("destroying the first promotes the second")
    {
        REQUIRE(fixture.world.destroy(first));
        CHECK(fixture.world.findFirstChild(root, tree) == second);
    }
    SUBCASE("moving one to another parent re-indexes it there")
    {
        const InstanceId other = fixture.folder("Other");
        REQUIRE_FALSE(fixture.world.setParent(second, other).has_value());
        CHECK(fixture.world.findFirstChild(root, tree) == first);
        CHECK(fixture.world.findFirstChild(other, tree) == second);
        fixture.world.setName(first, fixture.atom("Bush"));
        CHECK(fixture.world.findFirstChild(root, tree) == third);
    }
    SUBCASE("emptying the chain makes the name unknown again")
    {
        fixture.world.setName(first, fixture.atom("A"));
        fixture.world.setName(second, fixture.atom("B"));
        fixture.world.setName(third, fixture.atom("C"));
        CHECK_FALSE(fixture.world.findFirstChild(root, tree).valid());
    }
}

TEST_CASE("the Find family agrees on class and document order")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId folder = fixture.folder("Child");
    const InstanceId firstPart = fixture.part("Child");
    const InstanceId secondPart = fixture.part("Child");
    REQUIRE_FALSE(fixture.world.setParent(folder, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(firstPart, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(secondPart, root).has_value());

    // Exact class: asking for BasePart never finds a Part.
    CHECK(fixture.world.findFirstChildOfClass(root, fixture.schema.partClass) == firstPart);
    CHECK_FALSE(fixture.world.findFirstChildOfClass(root, fixture.schema.basePartClass).valid());
    // Through the hierarchy: an abstract base name is accepted.
    CHECK(fixture.world.findFirstChildWhichIsA(root, fixture.schema.basePartClass) == firstPart);
    CHECK(fixture.world.findFirstChildWhichIsA(root, fixture.schema.instanceClass) == folder);

    CHECK(fixture.world.findFirstAncestor(firstPart, fixture.atom("Root")) == root);
    CHECK_FALSE(fixture.world.findFirstAncestor(root, fixture.atom("Root")).valid());
    CHECK(fixture.world.findFirstAncestorOfClass(firstPart, fixture.schema.folderClass) == root);
}

// --- Destroy ----------------------------------------------------------------

TEST_CASE("destroy removes the subtree now and tells about it later")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId victim = fixture.folder("Victim");
    const InstanceId child = fixture.folder("Child");
    REQUIRE_FALSE(fixture.world.setParent(victim, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(child, victim).has_value());
    fixture.world.addTag(victim, fixture.atom("Climbable"));
    (void)drain(fixture.world);

    REQUIRE(fixture.world.destroy(victim));

    // Synchronous: the tree is already consistent when destroy returns, which
    // is what stops a handler from seeing a Parent that disagrees with
    // GetChildren.
    CHECK(fixture.world.childCount(root) == 0);
    CHECK_FALSE(fixture.world.parentOf(victim).valid());
    CHECK_FALSE(fixture.world.hasTag(victim, fixture.atom("Climbable")));

    // And the whole subtree is dismantled, not only its root: a destroyed child
    // is a child whose Parent is nil (api-design.md §3.1), so `victim` has no
    // children left either.
    CHECK(fixture.world.childCount(victim) == 0);
    CHECK_FALSE(fixture.world.parentOf(child).valid());

    const auto changes = drain(fixture.world);
    CHECK(countOf(changes, ChangeKind::Destroying) == 2);
    // Two, not one: `victim` left `root` and `child` left `victim`.
    CHECK(countOf(changes, ChangeKind::ChildRemoved) == 2);
    CHECK(countOf(changes, ChangeKind::TagRemoved) == 1);

    // The handle still resolves for the whole window in which a Destroying
    // handler could reach it, and stops afterwards (divergence #25).
    CHECK(fixture.world.alive(victim));
    fixture.world.retireDestroyed();
    CHECK_FALSE(fixture.world.alive(victim));
    CHECK_FALSE(fixture.world.alive(child));
}

TEST_CASE("a destroyed instance cannot be re-parented")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId victim = fixture.folder("Victim");
    REQUIRE_FALSE(fixture.world.setParent(victim, root).has_value());
    REQUIRE(fixture.world.destroy(victim));

    const auto error = fixture.world.setParent(victim, root);
    REQUIRE(error.has_value());
    CHECK(error->hash == ENG_TR("scene.err.parent_locked").hash);
    CHECK(fixture.world.childCount(root) == 0);

    CHECK_FALSE(fixture.world.destroy(victim));
}

TEST_CASE("nothing can be parented under a destroyed instance, and retiring one leaves no stale link (audit E1)")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId doomed = fixture.folder("Doomed");
    const InstanceId orphan = fixture.folder("Orphan");
    const InstanceId twin = fixture.folder("Orphan");
    REQUIRE_FALSE(fixture.world.setParent(doomed, root).has_value());
    REQUIRE(fixture.world.destroy(doomed));

    // Destroyed, not yet retired: still in the pool, and not a place.
    const auto error = fixture.world.setParent(orphan, doomed);
    REQUIRE(error.has_value());
    CHECK(error->hash == ENG_TR("scene.err.parent_locked").hash);
    CHECK_FALSE(fixture.world.parentOf(orphan).valid());

    // After retirement the tree still reads: a same-name sibling walk and a
    // child walk under the root find the live one and end.
    fixture.world.retireDestroyed();
    REQUIRE_FALSE(fixture.world.setParent(twin, root).has_value());
    REQUIRE_FALSE(fixture.world.setParent(orphan, root).has_value());
    CHECK(fixture.world.findFirstChild(root, fixture.atom("Orphan")).valid());
    CHECK(fixture.world.childCount(root) == 2);
    REQUIRE_FALSE(fixture.world.setParent(orphan, InstanceId{}).has_value());
    CHECK(fixture.world.findFirstChild(root, fixture.atom("Orphan")) == twin);
}

// --- The change queue -------------------------------------------------------

TEST_CASE("one reparent raises its fires in the documented order")
{
    Fixture fixture;
    const InstanceId oldParent = fixture.folder("Old");
    const InstanceId newParent = fixture.folder("New");
    const InstanceId moved = fixture.folder("Moved");
    REQUIRE_FALSE(fixture.world.setParent(moved, oldParent).has_value());
    (void)drain(fixture.world);
    // Listened on, as a script reaching the events marks them (audit E10).
    fixture.world.listenForTree(oldParent, engine::scene::World::TreeListen::Descendants);
    fixture.world.listenForTree(newParent, engine::scene::World::TreeListen::Descendants);
    fixture.world.listenForTree(moved, engine::scene::World::TreeListen::Ancestry);

    REQUIRE_FALSE(fixture.world.setParent(moved, newParent).has_value());

    // api-design.md §3.1: removed from the old parent, then added to the new,
    // then the ancestry fires.
    CHECK(kindsOf(drain(fixture.world)) ==
          std::vector<ChangeKind>{ChangeKind::ChildRemoved, ChangeKind::DescendantRemoving, ChangeKind::ChildAdded,
                                  ChangeKind::DescendantAdded, ChangeKind::AncestryChanged});
}

TEST_CASE("a reparent nobody listens to the tree for enqueues only what its parents fire (audit E10)")
{
    // A thousand-deep tree destroyed used to enqueue every ancestor times every
    // member, twice over, for events no script had reached.
    Fixture fixture;
    const InstanceId oldParent = fixture.folder("Old");
    const InstanceId newParent = fixture.folder("New");
    const InstanceId moved = fixture.folder("Moved");
    InstanceId deepest = moved;
    for (int depth = 0; depth < 200; ++depth) {
        const InstanceId child = fixture.folder("Link");
        REQUIRE_FALSE(fixture.world.setParent(child, deepest).has_value());
        deepest = child;
    }
    REQUIRE_FALSE(fixture.world.setParent(moved, oldParent).has_value());
    (void)drain(fixture.world);

    REQUIRE_FALSE(fixture.world.setParent(moved, newParent).has_value());
    CHECK(kindsOf(drain(fixture.world)) == std::vector<ChangeKind>{ChangeKind::ChildRemoved, ChangeKind::ChildAdded});
}

TEST_CASE("a property write enqueues only when the value changes, and only when watched")
{
    Fixture fixture;
    const InstanceId part = fixture.part("Part");
    const auto transparency = fixture.schema.transparencyProperty;
    (void)drain(fixture.world);

    // Unwatched: the write lands, and nothing is said about it. This is the
    // quiet path the 10k-parts benchmark rests on.
    CHECK(fixture.world.setProperty(part, transparency, Value{0.5}) == World::SetResult::Changed);
    CHECK(drain(fixture.world).empty());
    CHECK(fixture.world.getProperty(part, transparency).value() == Value{0.5});

    fixture.world.setPropertySubscribed(part, transparency, true);

    // Equal value: written, but no change happened, so there is no past-tense
    // fact to report (api-design.md §3.1).
    CHECK(fixture.world.setProperty(part, transparency, Value{0.5}) == World::SetResult::Unchanged);
    CHECK(drain(fixture.world).empty());

    CHECK(fixture.world.setProperty(part, transparency, Value{0.25}) == World::SetResult::Changed);
    CHECK(drain(fixture.world).size() == 1);

    // Three distinct writes are three fires: no coalescing.
    CHECK(fixture.world.setProperty(part, transparency, Value{0.1}) == World::SetResult::Changed);
    CHECK(fixture.world.setProperty(part, transparency, Value{0.2}) == World::SetResult::Changed);
    CHECK(fixture.world.setProperty(part, transparency, Value{0.3}) == World::SetResult::Changed);
    CHECK(drain(fixture.world).size() == 3);

    fixture.world.setPropertySubscribed(part, transparency, false);
    CHECK(fixture.world.setProperty(part, transparency, Value{0.9}) == World::SetResult::Changed);
    CHECK(drain(fixture.world).empty());
}

TEST_CASE("property writes report why they failed")
{
    Fixture fixture;
    const InstanceId part = fixture.part("Part");

    CHECK(fixture.world.setProperty(part, fixture.atom("NoSuchProperty"), Value{1.0}) ==
          World::SetResult::UnknownProperty);
    // Wrong type for the property: the setter rejects it and the instance is
    // untouched.
    CHECK(fixture.world.setProperty(part, fixture.schema.transparencyProperty, Value{std::string("x")}) ==
          World::SetResult::InvalidValue);
    CHECK(fixture.world.getProperty(part, fixture.schema.transparencyProperty).value() == Value{0.0});
    CHECK_FALSE(fixture.world.getProperty(part, fixture.atom("NoSuchProperty")).has_value());
}

// --- Attributes and tags ----------------------------------------------------

TEST_CASE("attributes hold the value domain and nothing else")
{
    Fixture fixture;
    const InstanceId part = fixture.part("Part");
    const auto health = fixture.atom("Health");
    (void)drain(fixture.world);

    CHECK(fixture.world.setAttribute(part, health, Value{100.0}));
    CHECK(fixture.world.getAttribute(part, health) == Value{100.0});
    CHECK(drain(fixture.world).size() == 1);

    // Equality-filtered like a property.
    CHECK(fixture.world.setAttribute(part, health, Value{100.0}));
    CHECK(drain(fixture.world).empty());

    // An Instance reference would be a second kind of tree edge that nothing
    // maintains, so it is refused and the attribute is left alone.
    CHECK_FALSE(fixture.world.setAttribute(part, fixture.atom("Owner"), Value{part}));
    CHECK(fixture.world.getAttribute(part, fixture.atom("Owner")) == Value{});

    CHECK(fixture.world.setAttribute(part, fixture.atom("Label"), Value{std::string("hi")}));
    engine::scene::AttributeMap attributes;
    fixture.world.collectAttributes(part, attributes);
    REQUIRE(attributes.size() == 2);
    // Insertion order, so nothing observes a container's own ordering (R10).
    CHECK(attributes[0].first == health);
    CHECK(attributes[1].first == fixture.atom("Label"));

    // nil removes.
    CHECK(fixture.world.setAttribute(part, health, Value{}));
    CHECK(fixture.world.getAttribute(part, health) == Value{});
}

TEST_CASE("tags are instance state, independent of the tree")
{
    Fixture fixture;
    const InstanceId part = fixture.part("Part");
    const auto climbable = fixture.atom("Climbable");
    (void)drain(fixture.world);

    CHECK(fixture.world.addTag(part, climbable));
    CHECK(fixture.world.hasTag(part, climbable));
    CHECK(drain(fixture.world).size() == 1);

    // Idempotent, and silent the second time.
    CHECK(fixture.world.addTag(part, climbable));
    CHECK(drain(fixture.world).empty());

    // Never parented, and still listed.
    std::vector<InstanceId> tagged;
    fixture.world.collectTagged(climbable, tagged);
    CHECK(tagged == std::vector<InstanceId>{part});

    engine::scene::TagSet all;
    fixture.world.collectAllTags(all);
    CHECK(all.size() == 1);

    CHECK(fixture.world.removeTag(part, climbable));
    CHECK_FALSE(fixture.world.removeTag(part, climbable));
    all.clear();
    fixture.world.collectAllTags(all);
    // A tag with no carriers stops existing, which is what makes GetAllTags
    // mean "currently carried".
    CHECK(all.empty());
}

TEST_CASE("all tags come back sorted by text, not by atom")
{
    Fixture fixture;
    const InstanceId part = fixture.part("Part");
    // Interned in an order that is deliberately not alphabetical, so a sort by
    // atom number would produce a different answer than a sort by text.
    fixture.world.addTag(part, fixture.atom("zebra"));
    fixture.world.addTag(part, fixture.atom("apple"));
    fixture.world.addTag(part, fixture.atom("mango"));

    engine::scene::TagSet all;
    fixture.world.collectAllTags(all);
    std::vector<std::string> text;
    for (const auto tag : all)
        text.push_back(std::string(fixture.schema.atoms.text(tag)));
    CHECK(text == std::vector<std::string>{"apple", "mango", "zebra"});
}

// --- Clone ------------------------------------------------------------------

TEST_CASE("clone deep-copies and rewires only the references that point inside")
{
    Fixture fixture;
    const InstanceId model = fixture.model("Model");
    const InstanceId inside = fixture.part("Inside");
    const InstanceId outside = fixture.part("Outside");
    REQUIRE_FALSE(fixture.world.setParent(inside, model).has_value());

    REQUIRE(fixture.world.setProperty(model, fixture.schema.primaryPartProperty, Value{inside}) ==
            World::SetResult::Changed);
    fixture.world.setAttribute(inside, fixture.atom("Health"), Value{50.0});
    fixture.world.addTag(inside, fixture.atom("Climbable"));
    REQUIRE(fixture.world.setProperty(inside, fixture.schema.transparencyProperty, Value{0.25}) ==
            World::SetResult::Changed);

    const InstanceId copy = fixture.world.clone(model);
    REQUIRE(copy.valid());
    CHECK(copy != model);
    CHECK_FALSE(fixture.world.parentOf(copy).valid());
    CHECK(fixture.childNames(copy) == std::vector<std::string>{"Inside"});

    const InstanceId copiedChild = fixture.world.firstChild(copy);
    CHECK(fixture.world.getProperty(copiedChild, fixture.schema.transparencyProperty).value() == Value{0.25});
    CHECK(fixture.world.getAttribute(copiedChild, fixture.atom("Health")) == Value{50.0});
    CHECK(fixture.world.hasTag(copiedChild, fixture.atom("Climbable")));

    // The reference pointed inside the copied subtree, so it follows the copy.
    CHECK(fixture.world.getProperty(copy, fixture.schema.primaryPartProperty).value() == Value{copiedChild});

    // And one pointing outside stays on the original: cloning a model must not
    // clone the world it sits in.
    REQUIRE(fixture.world.setProperty(model, fixture.schema.primaryPartProperty, Value{outside}) ==
            World::SetResult::Changed);
    const InstanceId second = fixture.world.clone(model);
    CHECK(fixture.world.getProperty(second, fixture.schema.primaryPartProperty).value() == Value{outside});

    // The copies are independent of the source.
    fixture.world.setAttribute(inside, fixture.atom("Health"), Value{10.0});
    CHECK(fixture.world.getAttribute(copiedChild, fixture.atom("Health")) == Value{50.0});
}

TEST_CASE("clone preserves duplicate names and child order")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    for (int index = 0; index < 3; ++index) {
        const InstanceId child = fixture.folder("Tree");
        REQUIRE_FALSE(fixture.world.setParent(child, root).has_value());
    }

    const InstanceId copy = fixture.world.clone(root);
    REQUIRE(copy.valid());
    CHECK(fixture.childNames(copy) == std::vector<std::string>{"Tree", "Tree", "Tree"});
    CHECK(fixture.world.findFirstChild(copy, fixture.atom("Tree")) == fixture.world.firstChild(copy));
}

// --- World hash -------------------------------------------------------------

namespace {

// Builds the same world twice through the same calls. `internExtra` interns a
// batch of unrelated strings first, which shifts every atom number without
// changing a single thing the world is supposed to contain.
[[nodiscard]] u64 buildAndHash(bool internExtra)
{
    Fixture fixture;
    if (internExtra) {
        for (int index = 0; index < 16; ++index)
            (void)fixture.atom("filler" + std::to_string(index));
    }

    const InstanceId root = fixture.folder("Root");
    const InstanceId part = fixture.part("Brick");
    (void)fixture.world.setParent(part, root);
    (void)fixture.world.setProperty(part, fixture.schema.transparencyProperty, Value{0.5});
    (void)fixture.world.setAttribute(part, fixture.atom("Health"), Value{100.0});
    fixture.world.addTag(part, fixture.atom("Climbable"));
    return fixture.world.worldHash();
}

} // namespace

TEST_CASE("the world hash reflects observable state and nothing else")
{
    CHECK(buildAndHash(false) == buildAndHash(false));

    // The one that matters: an atom's NUMBER depends on the order strings were
    // interned, which depends on the order things were built. Hashing it
    // instead of the text produces a hash that reproduces perfectly on one
    // machine and disagrees with another run of the same script.
    CHECK(buildAndHash(false) == buildAndHash(true));
}

// Not "does the hash change", which the case below covers -- "does every
// alternative of `Value` reach the hasher at all". A `Value` alternative with no
// arm in `hashValue` would hash its tag and nothing else, so two different
// UDim2s would be indistinguishable to the determinism gate while every script
// that read them disagreed. `-Wswitch` catches an omitted arm, and this catches
// an arm that hashes nothing.
TEST_CASE("every Value alternative reaches the hasher")
{
    // The two material alternatives (ADR 0090) are not attribute values, so
    // they cannot be reached through one; `material_world_tests.cpp` hashes
    // them through `BasePart.Material` and `BasePart.MaterialParameters`.
    static_assert(std::variant_size_v<Value> == 17, "a new Value alternative needs a row below");

    // Two distinct values per alternative, chosen to differ in every field so
    // that a partial hash -- one that reads `min` and forgets `max` -- fails
    // here rather than in a replay six weeks from now.
    const std::pair<Value, Value> pairs[] = {
        {Value{false}, Value{true}},
        {Value{1.0}, Value{2.0}},
        {Value{std::string("a")}, Value{std::string("b")}},
        {Value{engine::core::Vec3{1.0f, 2.0f, 3.0f}}, Value{engine::core::Vec3{3.0f, 2.0f, 1.0f}}},
        {Value{engine::core::Color3{0.1f, 0.2f, 0.3f}}, Value{engine::core::Color3{0.3f, 0.2f, 0.1f}}},
        {Value{engine::core::Vec2{1.0f, 2.0f}}, Value{engine::core::Vec2{2.0f, 1.0f}}},
        {Value{engine::core::UDim{0.5f, 8.0f}}, Value{engine::core::UDim{0.5f, 9.0f}}},
        {Value{engine::core::UDim2{engine::core::UDim{0.5f, 8.0f}, engine::core::UDim{1.0f, 2.0f}}},
         Value{engine::core::UDim2{engine::core::UDim{0.5f, 8.0f}, engine::core::UDim{1.0f, 3.0f}}}},
        {Value{engine::core::Rect{engine::core::Vec2{0.0f, 0.0f}, engine::core::Vec2{4.0f, 4.0f}}},
         Value{engine::core::Rect{engine::core::Vec2{0.0f, 0.0f}, engine::core::Vec2{4.0f, 5.0f}}}},
        // ADR 0110: a stop's colour, and a stop's time, each changed alone.
        {Value{engine::core::ColorSequence{}},
         Value{engine::core::ColorSequence{
             {{0.0f, engine::core::Color3{1.0f, 1.0f, 1.0f}}, {1.0f, engine::core::Color3{1.0f, 1.0f, 0.5f}}}}}},
        {Value{engine::core::NumberSequence{{{0.0f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}}},
         Value{engine::core::NumberSequence{{{0.0f, 0.0f, 0.0f}, {0.6f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}}}},
    };

    for (const auto& [first, second] : pairs) {
        Fixture fixture;
        const InstanceId part = fixture.part("Brick");
        const engine::core::NameAtom name = fixture.atom("Probe");

        CAPTURE(valueTypeName(valueType(first)));
        REQUIRE(fixture.world.setAttribute(part, name, first));
        const u64 withFirst = fixture.world.worldHash();
        REQUIRE(fixture.world.setAttribute(part, name, second));
        CHECK(fixture.world.worldHash() != withFirst);
    }
}

TEST_CASE("a restore moves a terrain's revisions on, and its package is what the disk now holds (terrain audit G2, "
          "P6, R5)")
{
    using engine::asset::ChunkKey;
    using engine::asset::FullOccupancy;
    using engine::asset::Voxel;
    Fixture fixture;
    const InstanceId ground = fixture.folder("Terrain");
    engine::scene::TerrainComponent component;
    component.field = engine::asset::TerrainField(engine::asset::FieldSettings{});
    (void)component.shipped.setVoxel(1, 1, 1, Voxel{FullOccupancy, 1});
    (void)fixture.world.terrains().add(ground, std::move(component));
    engine::scene::TerrainComponent* terrain = fixture.world.terrains().find(ground);
    terrain->fieldRevision = 4;
    const engine::scene::WorldSnapshot before = fixture.world.snapshot();

    // Edits after it, a save that changed what the disk holds at a key it
    // had loaded, and a cell loaded after it.
    terrain->fieldRevision = 9;
    (void)terrain->shipped.setVoxel(1, 1, 1, Voxel{FullOccupancy, 2});
    (void)terrain->shipped.setVoxel(100, 1, 1, Voxel{FullOccupancy, 1});
    const engine::asset::TerrainChunk* saved = terrain->shipped.findChunk(ChunkKey{0, 0, 0});
    fixture.world.restore(before);

    terrain = fixture.world.terrains().find(ground);
    // A revision the renderer has seen with other ground behind it is not
    // one the restored ground may have.
    CHECK(terrain->fieldRevision > 9);
    // The key it had loaded, as the disk now holds it; not one loaded since.
    CHECK(terrain->shipped.findChunk(ChunkKey{0, 0, 0}) == saved);
    CHECK(terrain->shipped.findChunk(ChunkKey{3, 0, 0}) == nullptr);
}

// ADR 0117: what the ground is made of decides what slides on it, what a ray
// reports and what a foot stands on -- so it is world state, where there is
// any. A terrain with no layers and no rules hashes as it did before either
// could be observed.
TEST_CASE("a terrain's layers and rules are in the world hash, and their absence is not")
{
    Fixture fixture;
    const InstanceId ground = fixture.folder("Terrain");
    engine::scene::TerrainComponent component;
    component.field = engine::asset::TerrainField(engine::asset::FieldSettings{});
    (void)fixture.world.terrains().add(ground, std::move(component));
    const auto bare = fixture.world.worldHash();

    engine::scene::TerrainComponent* terrain = fixture.world.terrains().find(ground);
    terrain->layers = {"engine://terrain/grass", "engine://terrain/ice"};
    const auto layered = fixture.world.worldHash();
    CHECK(layered != bare);

    terrain->layers[1] = "engine://terrain/snow";
    CHECK(fixture.world.worldHash() != layered);
    terrain->layers[1] = "engine://terrain/ice";
    CHECK(fixture.world.worldHash() == layered);

    engine::asset::TerrainRule rule;
    rule.material = 2;
    rule.heightMin = 20.0f;
    terrain->rules.push_back(rule);
    const auto ruled = fixture.world.worldHash();
    CHECK(ruled != layered);
    terrain->rules[0].heightMin = 21.0f;
    CHECK(fixture.world.worldHash() != ruled);
    terrain->rules[0].heightMin = 20.0f;
    terrain->rules[0].appliesTo = {1};
    CHECK(fixture.world.worldHash() != ruled);

    terrain->rules.clear();
    terrain->layers.clear();
    CHECK(fixture.world.worldHash() == bare);
}

TEST_CASE("the world hash reads a player's intents by action name, not by atom number (D304)")
{
    // The same intent in two worlds whose atom tables were filled in another
    // order -- which is what one method more in a build does to every name
    // interned after it, and what moved the character replay's trace.
    const auto hashWith = [](bool internFirst) {
        Fixture fixture;
        if (internFirst)
            (void)fixture.atom("SomethingInternedEarlier");
        const InstanceId player = fixture.folder("Player");
        engine::scene::PlayerComponent component;
        component.userId = 7;
        component.intents.push_back(engine::scene::PlayerIntent{fixture.atom("Jump"), 0, Vec3{}, true});
        (void)fixture.world.players().add(player, std::move(component));
        return fixture.world.worldHash();
    };
    CHECK(hashWith(false) == hashWith(true));
}

namespace {

// A world of one class whose one property reads an item of the enum called
// `SpecShape` -- whatever number this world's registry gave that enum.
struct EnumWorld
{
    engine::core::AtomTable atoms;
    engine::scene::ClassRegistry classes;
    engine::scene::EnumRegistry enums;
    std::vector<engine::scene::PropertyDesc> properties;
    engine::scene::ClassId thing = engine::scene::InvalidClass;

    explicit EnumWorld(bool registerFirst, std::string_view enumName)
    {
        static const std::array<engine::scene::EnumItemDesc, 2> Items{engine::scene::EnumItemDesc{},
                                                                      engine::scene::EnumItemDesc{}};
        if (registerFirst) {
            (void)enums.registerEnum(
                engine::scene::EnumDescriptor{atoms.intern("SomethingDeclaredEarlier"), {}, Items});
        }
        REQUIRE(enums.registerEnum(engine::scene::EnumDescriptor{atoms.intern(enumName), {}, Items}) !=
                engine::scene::InvalidEnum);
        properties = {engine::scene::PropertyDesc{
            .name = atoms.intern("Kind"),
            .type = engine::scene::ValueType::EnumItem,
            .get =
                [](const World& world, InstanceId) {
                    // The enum registered last: the one this world is about.
                    return Value{
                        engine::scene::EnumValue{static_cast<engine::scene::EnumId>(world.enums().enumCount() - 1), 1}};
                },
        }};
        engine::scene::ClassDescriptor desc;
        desc.name = atoms.intern("Thing");
        desc.defaultName = atoms.intern("Thing");
        desc.properties = properties;
        thing = classes.registerClass(desc);
    }

    [[nodiscard]] u64 hash()
    {
        World world{classes, enums, atoms, 1234u};
        REQUIRE(world.create(thing).valid());
        return world.worldHash();
    }
};

} // namespace

TEST_CASE("the world hash reads an enum's item by the enum's name, not by its number (D550)")
{
    // The same value in two worlds whose enums were registered in another
    // order -- which is what one enum more in a build does to every enum
    // declared after it, and what moved the water replay's trace at tick 0
    // when nothing in the water had changed.
    CHECK(EnumWorld(false, "SpecShape").hash() == EnumWorld(true, "SpecShape").hash());
    // And it is still the enum that is hashed: the same item of an enum by
    // another name is another value.
    CHECK(EnumWorld(false, "SpecShape").hash() != EnumWorld(false, "SpecOther").hash());
}

TEST_CASE("the world hash changes when anything observable does")
{
    Fixture fixture;
    const InstanceId root = fixture.folder("Root");
    const InstanceId part = fixture.part("Brick");
    REQUIRE_FALSE(fixture.world.setParent(part, root).has_value());
    const u64 base = fixture.world.worldHash();

    SUBCASE("a property")
    {
        REQUIRE(fixture.world.setProperty(part, fixture.schema.transparencyProperty, Value{0.5}) ==
                World::SetResult::Changed);
        CHECK(fixture.world.worldHash() != base);
    }
    SUBCASE("a name")
    {
        fixture.world.setName(part, fixture.atom("Renamed"));
        CHECK(fixture.world.worldHash() != base);
    }
    SUBCASE("an attribute")
    {
        REQUIRE(fixture.world.setAttribute(part, fixture.atom("Health"), Value{1.0}));
        CHECK(fixture.world.worldHash() != base);
    }
    SUBCASE("a tag")
    {
        REQUIRE(fixture.world.addTag(part, fixture.atom("Climbable")));
        CHECK(fixture.world.worldHash() != base);
    }
    SUBCASE("child order")
    {
        const InstanceId second = fixture.part("Other");
        REQUIRE_FALSE(fixture.world.setParent(second, root).has_value());
        const u64 withTwo = fixture.world.worldHash();
        REQUIRE_FALSE(fixture.world.setParent(part, InstanceId{}).has_value());
        REQUIRE_FALSE(fixture.world.setParent(part, root).has_value());
        CHECK(fixture.world.worldHash() != withTwo);
    }
    SUBCASE("but not a subscription, which nothing simulated can see")
    {
        fixture.world.setPropertySubscribed(part, fixture.schema.transparencyProperty, true);
        CHECK(fixture.world.worldHash() == base);
    }
}
