#pragma once

#include <engine/core/error.h>
#include <engine/core/id.h>
#include <engine/core/json.h>
#include <engine/core/name_atom.h>
#include <engine/scene/value.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// A world, written down.
//
// **This is what ADR 0047 decided a project's authored world lives in**: the
// scene is the source of truth for what a game starts with, and scripts are
// what it then does. The editor authors this file; boot reads it and then
// starts the scripts.
//
// **It is deliberately the same walk the world hash makes.**
// `engine/scene/src/world_hash.cpp` enumerates every instance in slot order,
// class by name, parent, children in sibling order, attributes, tags, and every
// property of every superclass through the generated accessors -- a complete,
// ordered, deterministic traversal that was written for a different purpose and
// is exactly the traversal a file format needs. Writing a second one would mean
// two definitions of "everything about a world", and they would disagree the
// first time somebody added a property.
//
// Its three warnings are this format's correctness rules, and they are not
// stylistic:
//
//   - **A class is written by NAME, never by `ClassId`.** An id depends on
//     registration order, which is a property of an engine build.
//   - **A name is written as TEXT, never as a `NameAtom`.** An atom is an index
//     into a table this world happens to have interned in this order.
//   - **Nothing is written as struct bytes.** Padding is not a value.
//
// A fourth belongs to a file and not to a hash: **an `InstanceId` is never
// written**. Ids are minted at runtime, so hierarchy is expressed by nesting
// and a reference to another instance is written as a path.
//
// ## What is in scope, and what is not, and why that is stated rather than
// implied
//
// A scene describes **the contents of `Workspace`** and **the properties of the
// services that describe a world** -- `Lighting` today. It does not describe
// services that are the engine's own machinery, it does not contain scripts
// (those are mounted from `src/`, which is ADR 0047's whole point), and it does
// not contain the streamed chunks: those are `chunk-source`'s job, that
// format is deliberately narrow, and the two meet at streaming and stay two
// things.
//
// ## What a reference can and cannot say
//
// A property whose value is an instance is written as a path from the scene's
// root (`Workspace.Tower.Door`). A reference to something OUTSIDE the scene --
// a service, a streamed chunk, an instance a script made -- cannot be named by
// such a path and is dropped, with a count in the result rather than silently.
// `preserved.h` made the same choice for the same reason and it is worth
// matching: a dangling reference restored as something else is worse than a
// dangling reference restored as nil.

namespace engine::scene {

// **A compiled script in a scene file** (S0.3, ADR 0138 §5): a package's scene,
// stamp and `global.json` carry a script's `Source` as this prefix and its
// bytecode in base64, since bytecode is not text and a JSON string is. The
// reader turns it back into the bytecode, which a script loads as it loads a
// compiled file; the writer writes bytecode back this way.
inline constexpr std::string_view CompiledSourcePrefix = "luauc:";
class World;

// What a load or a save actually did. Not a bool: a scene that loaded with
// three references dropped and one unknown class is a scene a person should be
// told about, and a caller that only knows "it worked" cannot tell them.
struct SceneIoReport
{
    core::u32 instances = 0;
    core::u32 properties = 0;
    // Instance-valued properties that named something outside the scene.
    core::u32 droppedReferences = 0;
    // Classes the file names that this build does not have. The instance and
    // its whole subtree are skipped -- a `Part` standing in for a class nobody
    // recognises would be a lie shaped like a recovery.
    core::u32 unknownClasses = 0;
    // Properties the file names that the class does not have, or that refused
    // the value. Counted rather than fatal: a scene written by a newer build
    // should still open in an older one, minus what it cannot express.
    core::u32 refusedProperties = 0;
    // **Which**, as `Class.Property`, the first few of them (D616): a count
    // nobody is shown is a property that was dropped in silence, and a
    // misspelt one in a file somebody wrote by hand looks exactly like a
    // property that does nothing.
    std::string refusedNames;
    static constexpr core::u32 MostRefusedNames = 6;
    core::u32 refusedNamed = 0;
    // Stamped instances placed, and stamps the file names that could not be
    // read (ADR 0049). The second is counted rather than fatal for the same
    // reason as an unknown class: a scene that names a stamp somebody deleted
    // should still open, minus what is gone.
    core::u32 stamped = 0;
    core::u32 missingStamps = 0;
    // **How many instances this load may build before it stops placing
    // stamps** (audit F6) -- an input, set before the read: a million unless
    // a caller wants fewer. A stamp past it is counted missing and kept.
    core::u32 instanceLimit = 1'000'000;
    // Property overrides written or applied: what an instance has of its own
    // (ADR 0051).
    core::u32 overrides = 0;
    // Copies written IN FULL because their root is no longer their stamp's
    // class, or their stamp could not be read. Everything else a person does
    // inside a copy -- an added child, a disabled one -- keeps it linked (ADR
    // 0155). Counted rather than refused: a save must never lose what is in
    // the world.
    core::u32 unlinkedStamps = 0;
    // **Stamps that reached themselves** (ADR 0155 §3), refused, and the last
    // chain that did, `a -> b -> a`, for the message a person is shown.
    core::u32 stampCycles = 0;
    std::string stampCycle;
    // Marks of a node the `src/` mount made whose file has gone: what
    // was authored inside it is kept, in a `Folder` of its name (ADR 0092).
    core::u32 orphanedMounts = 0;
};

// How a scene gets the TEXT of a stamp it names (ADR 0049).
//
// `scene` is L3 and has no filesystem -- it cannot open `content/stamps/`, and
// giving it one would be the layering mistake `architecture.md` §2 exists to
// prevent. The caller knows where stamps live; this is the one question the
// format has to ask it, and a caller that supplies nothing gets a scene whose
// stamped instances are skipped with a count rather than a crash.
using StampSource = std::function<std::optional<std::string>(std::string_view stamp)>;

// The stamps a SAVE needs to read, built once each and kept for the write.
//
// **A stamped instance is written as its mark plus what differs from the stamp**
// (ADR 0051), and "what differs" is a question about two trees -- so the stamp
// has to be built, once, into a world of its own. A world with forty lamp posts
// reads one file, not forty.
//
// Constructed from any world that shares the registries the save is about: a
// `ClassId` is an index into a registry, so the reference tree has to be built
// against the same ones or nothing in it could be compared with anything.
class StampLibrary
{
public:
    StampLibrary(World& registriesFrom, StampSource source);
    ~StampLibrary();

    StampLibrary(const StampLibrary&) = delete;
    StampLibrary& operator=(const StampLibrary&) = delete;

    // Opaque to callers: what a save needs from this is that it exists.
    struct Entry;
    [[nodiscard]] const Entry* reference(const std::string& stamp);
    // Where it reads stamps from, for a caller building one more.
    [[nodiscard]] const StampSource& source() const noexcept { return m_source; }

private:
    World& m_registries;
    StampSource m_source;
    std::unordered_map<std::string, std::unique_ptr<Entry>> m_built;
};

// Serialises the world's authored contents to JSON text.
//
// Deterministic: the same world produces the same bytes, which is what makes a
// scene file diffable and what makes a round-trip test possible at all.
// `stamps` is what lets a stamped instance be written as a mark plus its
// overrides. Without one, every stamped instance is written IN FULL and its
// mark dropped -- which loses nothing and is counted, and is what a caller with
// no content root can honestly do.
[[nodiscard]] std::string writeScene(const World& world, SceneIoReport* report = nullptr,
                                     StampLibrary* stamps = nullptr);

// **`GlobalScriptService`'s own file** (ADR 0105): what is authored under
// the game's script service and is not code -- a `Folder` of values, an
// attribute on the service -- saved as `content/global.json`, because no scene
// owns it. Empty text when there is nothing to save. `readGlobal` puts it back
// under the service, into its fixed folders by name.
//
// A stamped instance in it is written as its mark when `stamps` is given, and
// read back through `source` (B11: without them, every one was saved unlinked).
[[nodiscard]] std::string writeGlobal(const World& world, SceneIoReport* report = nullptr,
                                      StampLibrary* stamps = nullptr);
[[nodiscard]] std::optional<core::EngineError>
readGlobal(World& world, std::string_view json, SceneIoReport* report = nullptr, const StampSource* source = nullptr);

// **Every authored instance numbered as a read of the saved scene would
// number it** (ADR 0138 §6, the script-sides audit): what the editor's Play
// calls, since an edit -- an instance inserted, removed or moved -- leaves the
// numbers a read made out of step with the file a client reads. Each carried
// tree in preorder, generated instances left out, as the writer leaves them.
void renumberOrigins(World& world);

// Removes everything a scene describes, leaving the world otherwise intact.
//
// It is `readScene`'s first half, exposed because an editor asking for a NEW
// scene wants exactly that and nothing else. Defining it here rather than in
// the editor is what keeps "what a new scene clears" and "what a save writes"
// the same set -- two definitions of that would disagree the first time either
// moved.
//
// What survives: the services, the `Workspace` itself, and anything marked
// generated. A streamed chunk is not somebody's authored work, and a new scene
// is not a reason to evict the ground.
void clearScene(World& world);

// Applies a scene to a world, replacing whatever `Workspace` currently holds.
//
// Replacing rather than merging, because a scene IS the world's contents: a
// load that merged would double every instance the second time it ran, and
// "load a scene" would stop being idempotent.
[[nodiscard]] std::optional<core::EngineError>
readScene(World& world, std::string_view json, SceneIoReport* report = nullptr, const StampSource& stamps = {});

// **A scene read and parsed, and nothing more** (ADR 0125): what a scene
// prepared in the background holds until it is activated. Made on any thread,
// because it touches no world; applied on the main one by the overload below.
// Not movable, because the parse points into `text`.
struct ParsedScene
{
    std::string text;
    core::JsonDocument document;
    // Set when the file is not a scene this build reads.
    std::optional<core::EngineError> error;

    ParsedScene() = default;
    ParsedScene(const ParsedScene&) = delete;
    ParsedScene& operator=(const ParsedScene&) = delete;
};
[[nodiscard]] std::unique_ptr<ParsedScene> parseScene(std::string text);
// Every `asset://` name the parse holds, once each, in the order first met:
// what a prepared scene warms before it opens.
[[nodiscard]] std::vector<std::string> sceneContent(const ParsedScene& parsed);
[[nodiscard]] std::optional<core::EngineError>
readScene(World& world, const ParsedScene& parsed, SceneIoReport* report = nullptr, const StampSource& stamps = {});

// Builds ONE scene node -- a plain instance or a stamped one -- under `parent`,
// exactly as `readScene` would build it.
//
// Exposed for the partitioner (ADR 0053), which has to see what a node's
// subtree actually IS and cannot always read that out of the scene: a stamped
// node carries a mark and its overrides, and its parts live in the stamp file.
// The partitioner builds such a node into a scratch world, writes it back out
// in full, and goes on walking text -- so one stamp is resident at a time and
// the expansion is the same code the editor and the loader use, rather than a
// second reading of what a stamp means.
[[nodiscard]] core::InstanceId readSceneNode(World& world, std::string_view nodeJson, core::InstanceId parent,
                                             SceneIoReport* report = nullptr, const StampSource& stamps = {});

// --- Stamps (ADR 0049) -------------------------------------------------------
//
// **A stamp file is a scene of one subtree, and it is the same format.** Same
// writer, same reader, same four correctness rules; the only difference is that
// `root` is one instance instead of `Workspace`. Writing a second format would
// mean two definitions of "everything about a subtree" and they would disagree
// the first time somebody added a property -- which is the argument this file
// already makes about the world hash, one level down.

// Serialises `root` and its subtree as a stamp.
//
// `root`'s own stamp mark is ignored, because this is the file that mark points
// at: a stamp made from an instance of itself would otherwise write a one-line
// file that refers to the file being written.
//
// **Every node is written with its sid** (ADR 0155 §2) -- the one it has, or
// the one planned for it; call `assignStampSids` first when the live tree
// should keep them. With `base`, `root` is a VARIANT (§4) and is written as a
// copy of that stamp: what it has of its own, and nothing else.
[[nodiscard]] std::string writeStamp(const World& world, core::InstanceId root, SceneIoReport* report = nullptr,
                                     StampLibrary* stamps = nullptr, std::string_view base = {});

// Gives every node of `root`'s stamp tree the sid a write would give it, so a
// stamp written from a live tree and the tree itself agree on which node is
// which (ADR 0155 §2). A node that has one keeps it.
void assignStampSids(World& world, core::InstanceId root);

// The base a variant stamp file names, or empty for a stamp that is not one
// (ADR 0155 §4).
[[nodiscard]] std::string stampBaseOf(std::string_view stampText);

// **The same, for a copy that leaves the tree**: the clipboard, a stamp made
// from a selection. What the `src/` mount made is written in full -- class,
// code, everything -- where `writeStamp` writes a mark that only a reopen of
// the same project could resolve. Read back with `readStamp`, it is ordinary
// instances; saved under a script service, they become files of their own.
[[nodiscard]] std::string writeCopy(const World& world, core::InstanceId root, SceneIoReport* report = nullptr);

// **What a stamp name means, as a path under `content/`**: `\` as `/`, a
// typed `content/` dropped, a bare name in `stamps/`, a whole file name at the
// root, and `.stamp.json` put on when it is not there. One rule for the editor
// and for `Instance.stamp`, whose documentation promised it (B12).
[[nodiscard]] std::string normalizeStampPath(std::string_view typed);

// Instantiates a stamp under `parent` and marks the new root with `stamp`.
//
// Returns the new instance, or an invalid id when the text is not a stamp this
// build can read. Nothing is left half-built on failure: a subtree that could
// not be completed is destroyed rather than parented.
//
// `others` answers for every other stamp the text names -- the stamps it
// holds and, for a variant, its base (ADR 0155). Without it, those are missing.
[[nodiscard]] core::InstanceId readStamp(World& world, std::string_view json, core::InstanceId parent,
                                         std::string_view stamp, SceneIoReport* report = nullptr,
                                         const StampSource* others = nullptr);

// Re-applies a stamp file to every live instance of it, in the subtree at
// `root`. Returns how many were refreshed.
//
// **A stamp is a definition, so changing it changes what already exists** (ADR
// 0051) -- and until this, that was only true across a save and a load. A
// linked instance in a running editor is an ordinary subtree carrying a mark;
// nothing re-read the file while it sat there. This is the same arithmetic done
// live: what each instance has of its own is measured against the file it was
// built from, its children are rebuilt from the file as it stands now, and the
// overrides go back on top.
//
// **`before` is the file's PREVIOUS text and it is not optional.** An instance
// that differs from the new file is either overridden or merely out of date, and
// only the text it was built from tells those two apart.
//
// The instances keep their ids, their parents and their place among their
// siblings, so a reference held to one of them survives -- see the body for what
// that costs and why the alternative costs more. **A subtree whose shape has
// moved on is left alone** and counted in `unlinkedStamps`: it is not an
// instance of that stamp any more, which is the rule the writer already applies.
//
// `others` answers for the other stamps the copies are built from now, and
// `othersBefore`, when given, for what they were when the copies were built --
// a variant whose BASE changed is measured against the base it came from.
[[nodiscard]] core::u32 restamp(World& world, core::InstanceId root, std::string_view stamp, std::string_view before,
                                std::string_view after, SceneIoReport* report = nullptr,
                                const StampSource* others = nullptr, const StampSource* othersBefore = nullptr);

// One instance's overrides: the properties of `id` that differ from the stamp it
// was placed from, in declaration order.
//
// **The same question the save asks, asked out loud** (S5.6). ADR 0049 was
// reversed to inheritance-with-overrides -- an instance inherits from its stamp,
// a change to one instance stays local, a change to the stamp reaches every
// instance that has not overridden that property -- and the SAVE has understood
// that since ADR 0051. Nothing else could: a person editing a placed lamp post
// could not see which of its properties were its own and which came from the
// file, so there was nothing to revert and nothing to push back up.
//
// `id` need not be the stamped instance itself. Anything inside a placed stamp
// answers, measured against the corresponding instance in the stamp's own tree,
// which is found by walking the same child indices from each root.
//
// **Empty is three answers at once and deliberately not told apart**: `id` is
// not inside a placed stamp, the stamp cannot be read, or nothing differs. A
// caller draws a mark for a non-empty set and nothing otherwise, and none of the
// three is a mark.
//
// Structural changes are not overrides and never appear here: a child added or
// removed inside an instance makes it a different shape from the file, which the
// save writes in full and counts, and which this reports as no overrides at all
// rather than as every property being one.
[[nodiscard]] std::vector<core::NameAtom> stampOverrides(const World& world, core::InstanceId id, StampLibrary& stamps);

// What the stamp says one property of `id` should be.
//
// The other half of an override being actionable: `stampOverrides` says WHICH
// properties are the instance's own, and this says what reverting one would put
// back. Absent for anything not inside a placed stamp, for a stamp that cannot
// be read, and for a property the stamp's own class does not have.
[[nodiscard]] std::optional<Value> stampReferenceValue(const World& world, core::InstanceId id, core::NameAtom property,
                                                       StampLibrary& stamps);

// **Which node of its copy `id` is** (ADR 0155 §2): its key under `copyRoot`
// -- `""` for the root, a sid, or a path of sids through nested copies -- or
// nothing when it is not inside it.
[[nodiscard]] std::optional<std::string> stampKeyOf(const World& world, core::InstanceId copyRoot, core::InstanceId id);

// The node of `copyRoot` that `key` names, or an invalid id.
[[nodiscard]] core::InstanceId stampNodeAt(const World& world, core::InstanceId copyRoot, std::string_view key);

// `value`, a value of `property` on `id` in the world, as the stamp `id`'s copy
// was placed from would hold it: a place in the stamp's frame (ADR 0155 §1),
// anything else as it is. What applying an override to the stamp writes.
[[nodiscard]] Value stampLocalValue(const World& world, core::InstanceId id, core::NameAtom property,
                                    const Value& value, StampLibrary& stamps);

// The keys of a copy's stamp nodes it does not build -- what it disabled
// (ADR 0155 §5) -- each with the name the stamp gives it, for a panel to show
// greyed and offer to enable.
struct DisabledStampNode
{
    std::string key;
    std::string name;
    std::string className;
    // The key of the node it sits under in the stamp.
    std::string under;
};
[[nodiscard]] std::vector<DisabledStampNode> stampDisabled(const World& world, core::InstanceId copyRoot,
                                                           StampLibrary& stamps);

// Builds the stamp's node `key` back into the copy at `copyRoot` -- the stamp's
// version of it, under the node it sits under, after its siblings (ADR 0155
// §5). False when the copy already has it, or its parent is not there.
bool enableStampNode(World& world, core::InstanceId copyRoot, std::string_view key, StampLibrary& stamps);

// **The copy as its stamp's new text** (ADR 0155 §12, "apply all"): the whole
// copy, as it stands, written back in its stamp's frame -- its overrides, the
// children it added and the ones it left out become the stamp's. A variant is
// written as one, relative to its base. Empty when the stamp cannot be read.
[[nodiscard]] std::string writeCopyAsStamp(const World& world, core::InstanceId copy, StampLibrary& stamps);

// The keys of the overrides a copy holds for nodes its stamp no longer has
// (ADR 0155 §14). `World::setStampOrphans(copy, {})` forgets them.
[[nodiscard]] std::vector<std::string> stampOrphanKeys(const World& world, core::InstanceId copyRoot);

// **A copy replaced by a copy of another stamp** (ADR 0155 §13), standing
// where it stood, under the same parent, with the overrides whose sids name a
// node of the same class in the new stamp -- the rest kept as orphans. The old
// copy is destroyed; the new one is returned, or an invalid id when either
// stamp cannot be read.
[[nodiscard]] core::InstanceId replaceStampCopy(World& world, core::InstanceId copy, std::string_view stamp,
                                                StampLibrary& stamps, SceneIoReport* report = nullptr);

// --- A stamp's parameters (ADR 0155 §6) ---------------------------------------
//
// **What a stamp offers a designer**: a handful of named values -- a fence's
// length, a door's colour -- each an ATTRIBUTE of a copy's root, so a script
// reads and writes it as any attribute and it replicates as one (ADR 0106).
// The declaration says its type, its default, an optional range or list of
// choices, and the properties it DRIVES: writing the attribute sets them, at
// once, in the editor and in play.

struct StampDrive
{
    // The driven node's key in the copy (`""` the root) and its property; a
    // number may drive one component of a `Vector3`, `X`, `Y` or `Z`.
    std::string node;
    std::string property;
    std::string component;
};

struct StampParameter
{
    std::string name;
    // `Number`, `Bool`, `String`, `Color3` or `Vector3`.
    ValueType type = ValueType::Number;
    Value defaultValue;
    std::optional<core::f64> minimum;
    std::optional<core::f64> maximum;
    std::vector<Value> choices;
    std::vector<StampDrive> drives;
};

// The parameters the copy at `copyRoot` declares, in the order its stamp gives
// them; empty for anything that declares none.
[[nodiscard]] std::vector<StampParameter> stampParametersOf(const World& world, core::InstanceId copyRoot);

// Parses a declaration, as a stamp file holds it, or nothing when it is not one.
[[nodiscard]] std::optional<std::vector<StampParameter>> parseStampParameters(std::string_view json);

// A declaration as a stamp file holds it -- what `parseStampParameters` reads.
[[nodiscard]] std::string writeStampParameters(const World& world, const std::vector<StampParameter>& parameters);

// Why `value` cannot be `parameter`, as a reason a message names -- `type`,
// `range` or `choice` -- or empty when it can. **Refused, never clamped**: a
// value out of range is a mistake to be told about, not one to correct.
[[nodiscard]] std::string_view stampParameterRefusal(const StampParameter& parameter, const Value& value);

// Sets every property `parameter` drives on the copy at `copyRoot` from the
// root's attribute of that name; every parameter's when `parameter` is empty.
void applyStampDrives(World& world, core::InstanceId copyRoot, core::NameAtom parameter = {});

} // namespace engine::scene
