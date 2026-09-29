#include <algorithm>
#include <array>
#include <engine/asset/terrain.h>
#include <engine/asset/terrain_cell.h>
#include <engine/asset/voxel.h>
#include <engine/core/base64.h>
#include <engine/core/i18n.h>
#include <engine/core/json.h>
#include <engine/core/json_writer.h>
#include <engine/scene/class_registry.h>
#include <engine/scene/components.h>
#include <engine/scene/enum_registry.h>
#include <engine/scene/scene_file.h>
#include <engine/scene/voxel_fluid.h>
#include <engine/scene/world.h>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::scene {

// One stamp, built once and reused for every instance of it in a save. Its own
// world, so building it cannot touch the world being written.
struct StampLibrary::Entry
{
    std::unique_ptr<World> world;
    core::InstanceId root;
};

namespace {
using core::JsonValue;
using core::JsonWriter;

constexpr std::string_view kFormat = "scene";
// **2 since ADR 0090**: a part's `Material` is an asset URN and its surface
// overrides are `MaterialParameters`, where version 1 wrote a `Color`, a
// `Transparency` and an instance path to a `Material`.
constexpr core::i64 kVersion = 2;
// The oldest version this reader converts. What changed is narrow enough that
// converting it costs a few lines, and a project written last week must open.
constexpr core::i64 kOldestVersion = 1;

[[nodiscard]] bool readableVersion(core::i64 version) noexcept
{
    return version >= kOldestVersion && version <= kVersion;
}

// The version of the file being read right now. A stamp placed while a scene is
// read is a file of its own with a version of its own, so this is set for the
// span of each file's read and put back after -- a scope, not a parameter
// threaded through every reader for the one property conversion that asks.
thread_local core::i64 t_readVersion = kVersion;

class ReadingVersion
{
public:
    explicit ReadingVersion(core::i64 version) noexcept : m_outer(t_readVersion) { t_readVersion = version; }
    ~ReadingVersion() { t_readVersion = m_outer; }
    ReadingVersion(const ReadingVersion&) = delete;
    ReadingVersion& operator=(const ReadingVersion&) = delete;

private:
    core::i64 m_outer;
};

// Written as fields of their own, so writing them again as properties would be
// two spellings of one fact -- and `Parent` in particular would fight the
// nesting that already expresses it.
[[nodiscard]] bool isStructuralProperty(std::string_view name) noexcept
{
    return name == "Name" || name == "Parent" || name == "ClassName";
}

// **Whether a scene writes this property at all.** One with no setter cannot be
// applied on load, so writing it would be a field that is only ever read by a
// person -- `readOnly` is the same statement from the other side -- and a
// transient one is the running game's state rather than the scene's.
[[nodiscard]] bool savedProperty(const World& world, const PropertyDesc& property) noexcept
{
    if (property.get == nullptr || property.set == nullptr || property.readOnly || property.transient)
        return false;
    return !isStructuralProperty(world.atoms().text(property.name));
}

[[nodiscard]] core::InstanceId workspaceOf(const World& world) noexcept
{
    core::InstanceId found;
    world.workspaces().forEach([&](core::InstanceId id, const WorkspaceComponent&) {
        // One per world by construction. Taking the first rather than asserting,
        // because a world with none is a world booting, not a defect.
        if (!found.valid())
            found = id;
    });
    return found;
}

// **The services whose contents a scene carries beside the world's** (ADR
// 0080). What an editor put in them is part of what a game starts with.
//
// **Every service, not a list of three.** The Explorer lets a person put an
// instance inside any other (the owner: "whether it does anything is another
// story"), and a service the scene did not carry was one where what they put
// was there until the next save and then gone -- a `ScreenGui` in `UIService`
// was the one reported. The world is not carried here, because it is the
// file's root; the two scene script services are, minus the scripts their
// mount made from `src/`, which are saved as files (`mountedScriptTree`).
// `GlobalScriptService` is not: it is the game's, and outlives every scene
// (ADR 0105).
//
// These three come first, in this order, because files already name them in
// it; any other follows in the data model's own order. Written only when
// something is kept there, so every scene written before is the byte-for-byte
// file it was.
constexpr std::array<std::string_view, 3> FirstServices{"ReplicatedStorage", "ServerStorage", "UIService"};

// The service of that class under the world's data model, or nothing.
[[nodiscard]] core::InstanceId storageNamed(const World& world, std::string_view className) noexcept
{
    const core::InstanceId workspace = workspaceOf(world);
    const core::InstanceId dataModel = workspace.valid() ? world.parentOf(workspace) : core::InstanceId{};
    for (core::InstanceId child = dataModel.valid() ? world.firstChild(dataModel) : core::InstanceId{}; child.valid();
         child = world.nextSibling(child)) {
        const ClassDescriptor* descriptor = world.classes().find(world.classOf(child));
        if (descriptor != nullptr && world.atoms().text(descriptor->name) == className)
            return child;
    }
    return {};
}

[[nodiscard]] std::string_view classNameOf(const World& world, core::InstanceId id) noexcept
{
    const ClassDescriptor* descriptor = world.classes().find(world.classOf(id));
    return descriptor != nullptr ? world.atoms().text(descriptor->name) : std::string_view{};
}

// **Nothing but what the mount of `src/` made** (ADR 0092): a script
// read from a file, or a folder the mount made for one, with nothing authored
// anywhere inside. The files are its source, so the scene does not write it --
// writing it would put a second copy beside the one the mount makes at the
// next open. A mounted node that DOES hold something authored is written as a
// mark instead (see `writeInstance`).
// **What a script from a file carries that its file does not**: being off
// (`Enabled`), attributes and tags. The file is its source and nothing more, so
// these ride on the node's mark -- or a script disabled, tagged or pasted in
// with attributes would come back without them at the next open.
[[nodiscard]] bool disabledScript(const World& world, core::InstanceId id) noexcept
{
    const ClassDescriptor* descriptor = world.classes().find(world.classOf(id));
    if (descriptor == nullptr || world.atoms().text(descriptor->name) != "Script")
        return false;
    const std::optional<Value> enabled = world.getProperty(id, world.atoms().lookup("Enabled"));
    const bool* on = enabled.has_value() ? std::get_if<bool>(&*enabled) : nullptr;
    return on != nullptr && !*on;
}

[[nodiscard]] bool carriesOwn(const World& world, core::InstanceId id) noexcept
{
    if (disabledScript(world, id))
        return true;
    AttributeMap attributes;
    world.collectAttributes(id, attributes);
    if (!attributes.empty())
        return true;
    TagSet tags;
    world.collectTags(id, tags);
    return !tags.empty();
}

[[nodiscard]] bool mountedScriptTree(const World& world, core::InstanceId id) noexcept
{
    // `GlobalScriptService`'s fixed folders are the engine's node in the same
    // sense (ADR 0105): made at boot, found again by name, and written only as
    // the mark that holds what somebody put inside.
    if (!world.mounted(id) && !world.fixed(id))
        return false;
    if (world.mounted(id) && carriesOwn(world, id))
        return false;
    if (world.hasUnread(id))
        return false;
    for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child)) {
        if (!mountedScriptTree(world, child))
            return false;
    }
    return true;
}

// **Made by a system, so nobody wrote it down and a scene does not record
// it**: a streamed chunk (and the whole subtree with it), or a `Player`, which
// the engine makes for somebody taking part and nothing can author, or
// what the `src/` mount made.
// **Whether what the mount made is written in full**: a scene leaves it to the
// files and writes marks, but a COPY -- the clipboard, a stamp made from a
// selection -- is going somewhere the files are not, and a mark there finds
// nothing: a pasted script came back as an empty folder of its name, its code
// gone. Set for the length of one `writeCopy`.
thread_local bool t_mountedInFull = false;

[[nodiscard]] bool engineMade(const World& world, core::InstanceId id) noexcept
{
    if (world.generated(id))
        return true;
    if (classNameOf(world, id) == "Player")
        return true;
    return !t_mountedInFull && mountedScriptTree(world, id);
}

// Whether a storage holds anything a scene would write.
[[nodiscard]] bool holdsAuthored(const World& world, core::InstanceId service) noexcept
{
    if (world.hasUnread(service))
        return true;
    for (core::InstanceId child = world.firstChild(service); child.valid(); child = world.nextSibling(child)) {
        if (!engineMade(world, child))
            return true;
    }
    return false;
}

// Whether a service's contents are a scene's to carry (see `FirstServices`).
[[nodiscard]] bool carriedService(const World& world, core::InstanceId id) noexcept
{
    const ClassDescriptor* descriptor = world.classes().find(world.classOf(id));
    if (descriptor == nullptr || !hasFlag(descriptor->flags, ClassFlags::Service))
        return false;
    // Not the world, which is the file's root, and not the game's own script
    // service, which no scene owns (ADR 0105). The scene's script services are
    // carried like the rest: what the mount made is left out by `engineMade`,
    // and whatever else somebody put there is theirs (ADR 0092).
    const std::string_view name = world.atoms().text(descriptor->name);
    return name != "Workspace" && name != "GlobalScriptService";
}

// The carried services, by class name, in the order a file writes them.
[[nodiscard]] std::vector<std::pair<std::string_view, core::InstanceId>> carriedServices(const World& world)
{
    std::vector<std::pair<std::string_view, core::InstanceId>> services;
    for (const std::string_view name : FirstServices) {
        if (const core::InstanceId service = storageNamed(world, name); service.valid())
            services.emplace_back(name, service);
    }
    const core::InstanceId workspace = workspaceOf(world);
    const core::InstanceId dataModel = workspace.valid() ? world.parentOf(workspace) : core::InstanceId{};
    for (core::InstanceId child = dataModel.valid() ? world.firstChild(dataModel) : core::InstanceId{}; child.valid();
         child = world.nextSibling(child)) {
        if (!carriedService(world, child))
            continue;
        const std::string_view name = world.atoms().text(world.classes().find(world.classOf(child))->name);
        if (std::find(FirstServices.begin(), FirstServices.end(), name) == FirstServices.end())
            services.emplace_back(name, child);
    }
    return services;
}

// --- writing ---------------------------------------------------------------

// The fields an override set holds, in NAME order (ADR 0090) -- the order a
// person reads them in, whatever order the engine declares them.
[[nodiscard]] core::usize overriddenFields(const asset::MaterialOverrides& overrides,
                                           std::array<asset::MaterialField, asset::MaterialFieldCount>& out)
{
    core::usize count = 0;
    for (core::usize index = 0; index < asset::MaterialFieldCount; ++index) {
        const auto field = static_cast<asset::MaterialField>(index);
        if (overrides.has(field))
            out[count++] = field;
    }
    std::sort(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(count),
              [](asset::MaterialField a, asset::MaterialField b) {
                  return asset::materialFieldName(a) < asset::materialFieldName(b);
              });
    return count;
}

// A number-valued parameter's slot in a description.
[[nodiscard]] core::f32* parameterNumber(asset::MaterialField field, asset::MaterialProperties& values) noexcept
{
    switch (field) {
    case asset::MaterialField::Transparency:
        return &values.transparency;
    case asset::MaterialField::Metalness:
        return &values.metalness;
    case asset::MaterialField::Roughness:
        return &values.roughness;
    case asset::MaterialField::NormalScale:
        return &values.normalScale;
    case asset::MaterialField::AlphaCutoff:
        return &values.alphaCutoff;
    default:
        return nullptr;
    }
}

// A part's overrides as an object keyed by parameter name, in name order.
void writeMaterialParameters(JsonWriter& out, const asset::MaterialOverrides& overrides)
{
    std::array<asset::MaterialField, asset::MaterialFieldCount> fields{};
    const core::usize count = overriddenFields(overrides, fields);
    asset::MaterialProperties values = asset::overrideValues(overrides);
    out.beginObject();
    for (core::usize index = 0; index < count; ++index) {
        const asset::MaterialField field = fields[index];
        out.key(asset::materialFieldName(field));
        if (field == asset::MaterialField::Color || field == asset::MaterialField::Emissive) {
            const core::Color3 colour = field == asset::MaterialField::Color ? values.color : values.emissive;
            out.beginArray();
            out.value(static_cast<core::f64>(colour.r));
            out.value(static_cast<core::f64>(colour.g));
            out.value(static_cast<core::f64>(colour.b));
            out.endArray();
            continue;
        }
        const core::f32* number = parameterNumber(field, values);
        out.value(static_cast<core::f64>(number != nullptr ? *number : 0.0f));
    }
    out.endObject();
}

// The reverse: an object of parameters, or nothing for one that is not.
[[nodiscard]] std::optional<asset::MaterialOverrides> readMaterialParameters(const JsonValue& json)
{
    if (json.type() != core::JsonType::Object)
        return std::nullopt;
    asset::MaterialOverrides out;
    asset::MaterialProperties values;
    for (core::usize index = 0; index < json.size(); ++index) {
        const std::string_view name = json.keyAt(index);
        const std::optional<asset::MaterialField> field = asset::materialFieldNamed(name);
        if (!field.has_value())
            return std::nullopt;
        const JsonValue entry = json[name];
        if (*field == asset::MaterialField::Color || *field == asset::MaterialField::Emissive) {
            if (entry.type() != core::JsonType::Array || entry.size() < 3)
                return std::nullopt;
            const core::Color3 colour{static_cast<core::f32>(entry.at(0).asNumber()),
                                      static_cast<core::f32>(entry.at(1).asNumber()),
                                      static_cast<core::f32>(entry.at(2).asNumber())};
            (*field == asset::MaterialField::Color ? values.color : values.emissive) = colour;
        }
        else {
            core::f32* number = parameterNumber(*field, values);
            if (number == nullptr || entry.type() != core::JsonType::Number)
                return std::nullopt;
            *number = static_cast<core::f32>(entry.asNumber());
        }
        if (!asset::setOverride(out, *field, values))
            return std::nullopt;
    }
    return out;
}

// A sequence (ADR 0110) as an object with one key naming which, so an
// attribute -- which has no declared type -- reads back as what it was: each
// stop an inline array of its time and its colour, or its time, value and
// envelope.
void writeSequence(JsonWriter& out, const core::ColorSequence& sequence)
{
    out.beginObject();
    out.key("colorSequence");
    out.beginArray();
    for (const core::ColorKeypoint& stop : sequence.keypoints) {
        out.beginInlineArray();
        out.value(static_cast<core::f64>(stop.time));
        out.value(static_cast<core::f64>(stop.value.r));
        out.value(static_cast<core::f64>(stop.value.g));
        out.value(static_cast<core::f64>(stop.value.b));
        out.endArray();
    }
    out.endArray();
    out.endObject();
}

void writeSequence(JsonWriter& out, const core::NumberSequence& sequence)
{
    out.beginObject();
    out.key("numberSequence");
    out.beginArray();
    for (const core::NumberKeypoint& stop : sequence.keypoints) {
        out.beginInlineArray();
        out.value(static_cast<core::f64>(stop.time));
        out.value(static_cast<core::f64>(stop.value));
        out.value(static_cast<core::f64>(stop.envelope));
        out.endArray();
    }
    out.endArray();
    out.endObject();
}

// The reverse, for either: nothing for an object that is not a well-formed
// sequence, which the caller counts as refused.
[[nodiscard]] std::optional<Value> readSequence(const JsonValue& json)
{
    if (json.type() != core::JsonType::Object)
        return std::nullopt;
    const auto number = [](const JsonValue& stop, core::usize index) {
        return static_cast<core::f32>(stop.at(index).asNumber());
    };
    if (const JsonValue stops = json["colorSequence"]; stops.type() == core::JsonType::Array) {
        core::ColorSequence sequence;
        sequence.keypoints.clear();
        for (core::usize index = 0; index < stops.size(); ++index) {
            const JsonValue stop = stops.at(index);
            if (stop.type() != core::JsonType::Array || stop.size() < 4)
                return std::nullopt;
            sequence.keypoints.push_back(
                core::ColorKeypoint{number(stop, 0), core::Color3{number(stop, 1), number(stop, 2), number(stop, 3)}});
        }
        if (!core::validSequence(sequence.keypoints))
            return std::nullopt;
        return Value{std::move(sequence)};
    }
    if (const JsonValue stops = json["numberSequence"]; stops.type() == core::JsonType::Array) {
        core::NumberSequence sequence;
        sequence.keypoints.clear();
        for (core::usize index = 0; index < stops.size(); ++index) {
            const JsonValue stop = stops.at(index);
            if (stop.type() != core::JsonType::Array || stop.size() < 3)
                return std::nullopt;
            sequence.keypoints.push_back(core::NumberKeypoint{number(stop, 0), number(stop, 1), number(stop, 2)});
        }
        if (!core::validSequence(sequence.keypoints))
            return std::nullopt;
        return Value{std::move(sequence)};
    }
    return std::nullopt;
}

void writeValue(JsonWriter& out, const World& world, const Value& value,
                const std::unordered_map<core::u32, std::string>& paths, SceneIoReport& report)
{
    switch (valueType(value)) {
    case ValueType::Nil:
        out.nullValue();
        break;
    case ValueType::Bool:
        out.value(std::get<bool>(value));
        break;
    case ValueType::Number:
        out.value(std::get<core::f64>(value));
        break;
    case ValueType::String:
        out.value(std::get<std::string>(value));
        break;
    case ValueType::Vector3: {
        const core::Vec3 v = std::get<core::Vec3>(value);
        out.beginArray();
        out.value(static_cast<core::f64>(v.x));
        out.value(static_cast<core::f64>(v.y));
        out.value(static_cast<core::f64>(v.z));
        out.endArray();
        break;
    }
    case ValueType::CFrame: {
        // Position first, then the rotation's nine, in the column order `Mat3`
        // stores them. Twelve numbers rather than a position and three Euler
        // angles: Euler angles are a lossy round trip through a branch choice,
        // and a scene that moved a part by a thousandth of a degree every save
        // would make every diff noise.
        const core::CFrameD cf = std::get<core::CFrameD>(value);
        out.beginArray();
        out.value(cf.position.x);
        out.value(cf.position.y);
        out.value(cf.position.z);
        for (core::i32 column = 0; column < 3; ++column)
            for (core::i32 row = 0; row < 3; ++row)
                out.value(static_cast<core::f64>(cf.rotation.m[column][row]));
        out.endArray();
        break;
    }
    case ValueType::Color3: {
        const core::Color3 c = std::get<core::Color3>(value);
        out.beginArray();
        out.value(static_cast<core::f64>(c.r));
        out.value(static_cast<core::f64>(c.g));
        out.value(static_cast<core::f64>(c.b));
        out.endArray();
        break;
    }
    case ValueType::Instance: {
        const core::InstanceId target = std::get<core::InstanceId>(value);
        if (!target.valid()) {
            out.nullValue();
            break;
        }
        const auto found = paths.find(target.index);
        if (found == paths.end()) {
            // Outside the scene: a service, a streamed chunk, something a script
            // made. Named as nothing rather than as the wrong thing.
            ++report.droppedReferences;
            out.nullValue();
            break;
        }
        out.value(found->second);
        break;
    }
    case ValueType::EnumItem: {
        const EnumValue item = std::get<EnumValue>(value);
        const EnumDescriptor* descriptor = world.enums().find(static_cast<EnumId>(item.enumId));
        const EnumItemDesc* named =
            descriptor != nullptr ? world.enums().findValue(static_cast<EnumId>(item.enumId), item.value) : nullptr;
        // By NAME, for the reason a class is: an enum's numeric value is a
        // contract, but the name is what a person reads in a diff and what
        // survives somebody renumbering.
        if (named != nullptr)
            out.value(world.atoms().text(named->name));
        else
            out.value(static_cast<core::i64>(item.value));
        break;
    }
    case ValueType::Vector2: {
        const core::Vec2 v = std::get<core::Vec2>(value);
        out.beginArray();
        out.value(static_cast<core::f64>(v.x));
        out.value(static_cast<core::f64>(v.y));
        out.endArray();
        break;
    }
    case ValueType::UDim: {
        const core::UDim u = std::get<core::UDim>(value);
        out.beginArray();
        out.value(static_cast<core::f64>(u.scale));
        out.value(static_cast<core::f64>(u.offset));
        out.endArray();
        break;
    }
    case ValueType::UDim2: {
        const core::UDim2 u = std::get<core::UDim2>(value);
        out.beginArray();
        out.value(static_cast<core::f64>(u.x.scale));
        out.value(static_cast<core::f64>(u.x.offset));
        out.value(static_cast<core::f64>(u.y.scale));
        out.value(static_cast<core::f64>(u.y.offset));
        out.endArray();
        break;
    }
    case ValueType::Rect: {
        const core::Rect r = std::get<core::Rect>(value);
        out.beginArray();
        out.value(static_cast<core::f64>(r.min.x));
        out.value(static_cast<core::f64>(r.min.y));
        out.value(static_cast<core::f64>(r.max.x));
        out.value(static_cast<core::f64>(r.max.y));
        out.endArray();
        break;
    }
    case ValueType::Material:
        // The asset's URN. A clone is never saved (ADR 0090): what a file can
        // say about one is the asset it came from.
        out.value(std::get<MaterialRef>(value).source);
        break;
    case ValueType::MaterialParameters:
        writeMaterialParameters(out, std::get<asset::MaterialOverrides>(value));
        break;
    case ValueType::ColorSequence:
        writeSequence(out, std::get<core::ColorSequence>(value));
        break;
    case ValueType::NumberSequence:
        writeSequence(out, std::get<core::NumberSequence>(value));
        break;
    }
}

// Every instance's path, built before anything is written, because a property
// on the first instance may reference the last.
void collectPaths(const World& world, core::InstanceId id, const std::string& prefix,
                  std::unordered_map<core::u32, std::string>& out)
{
    const std::string path = prefix.empty() ? std::string(world.atoms().text(world.name(id)))
                                            : prefix + "." + std::string(world.atoms().text(world.name(id)));
    out.emplace(id.index, path);
    for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child)) {
        // Not written, so not nameable. A path collected for something the file
        // will not contain is a reference that resolves to nothing on load,
        // which is worse than the null the dropped-reference count reports.
        if (engineMade(world, child))
            continue;
        collectPaths(world, child, path, out);
    }
}

// --- Stamped instances, written as a mark plus what differs (ADR 0051) -------
//
// **The model the human asked for is INHERITANCE, not a copy.** An instance of
// a stamp follows its file: change the file and every instance changes with it.
// What an instance may have of its own is a set of property OVERRIDES -- "muda
// alguns parâmetros do novo sem influenciar o anterior" -- and those are the
// only thing about it this file records.
//
// So a stamped instance serialises as its mark, its name, its placement, and a
// map of overrides keyed by the path INSIDE the stamp. Nothing else, and none
// of its children: the children are the stamp's.
//
// **The diff is done against the stamp itself**, built into a scratch world
// once per stamp per save. There is no cheaper honest way: "what differs from
// the source" is a question about two trees, and comparing serialised text
// would compare formatting as well as values.

// One step of an override path: the child's name, and when a sibling shares it,
// `#` and which of them it is, counting from one.
//
// **Two parts both called `Part` were one path**, and the file then held two
// overrides under one key -- the loader applied the last to the first part and
// nothing to the second (the stamp audit, B1). Only when the name is shared:
// a path a person can read stays the path they can read.
[[nodiscard]] std::string overrideSegment(const World& world, core::InstanceId id)
{
    const core::NameAtom name = world.name(id);
    const core::InstanceId parent = world.parentOf(id);
    int shared = 0;
    int ordinal = 0;
    for (core::InstanceId sibling = world.firstChild(parent); sibling.valid(); sibling = world.nextSibling(sibling)) {
        if (world.name(sibling) != name)
            continue;
        ++shared;
        if (sibling == id)
            ordinal = shared;
    }
    std::string segment(world.atoms().text(name));
    if (shared > 1)
        segment += "#" + std::to_string(ordinal);
    return segment;
}

// The path of `id` inside the stamped subtree rooted at `stampRoot`: steps
// joined by '.', and empty for the root itself.
//
// **Written from the STAMP's tree, not the instance's** (B2): the loader finds
// the path in a fresh copy of the stamp, so a child renamed in one instance is
// still found by the name the stamp gives it -- and the rename is an override
// like any other.
[[nodiscard]] std::string overridePath(const World& world, core::InstanceId stampRoot, core::InstanceId id)
{
    std::string path;
    for (core::InstanceId walk = id; walk.valid() && walk != stampRoot; walk = world.parentOf(walk)) {
        const std::string segment = overrideSegment(world, walk);
        path = path.empty() ? segment : segment + "." + path;
    }
    return path;
}

// Whether two subtrees have the same SHAPE: the same classes, in the same
// order, all the way down.
//
// **A structural change is not an override.** Adding a child to one lamp post,
// or deleting one, is not "a parameter of this instance" -- it is a different
// thing -- and a format that tried to record it would be inventing Unity's
// added-and-removed-component machinery in a corner nobody designed. When the
// shapes disagree the instance is written IN FULL and its mark dropped, which
// loses nothing and says what it did.
[[nodiscard]] bool sameShape(const World& live, core::InstanceId a, const World& reference, core::InstanceId b)
{
    const ClassDescriptor* liveClass = live.classes().find(live.classOf(a));
    const ClassDescriptor* referenceClass = reference.classes().find(reference.classOf(b));
    if (liveClass == nullptr || referenceClass == nullptr)
        return false;
    if (live.atoms().text(liveClass->name) != reference.atoms().text(referenceClass->name))
        return false;
    if (live.childCount(a) != reference.childCount(b))
        return false;

    core::InstanceId liveChild = live.firstChild(a);
    core::InstanceId referenceChild = reference.firstChild(b);
    while (liveChild.valid() && referenceChild.valid()) {
        if (!sameShape(live, liveChild, reference, referenceChild))
            return false;
        liveChild = live.nextSibling(liveChild);
        referenceChild = reference.nextSibling(referenceChild);
    }
    return !liveChild.valid() && !referenceChild.valid();
}

// The path of `id` under `root`, or nothing at all when `id` is not under it.
// The root itself is the EMPTY path, which is why this answers with an optional
// rather than a string a caller has to test for emptiness -- "not in this
// subtree" and "is this subtree" are different answers and one of them is a
// reference the file has to record.
[[nodiscard]] std::optional<std::string> pathUnder(const World& world, core::InstanceId root, core::InstanceId id)
{
    if (!id.valid())
        return std::nullopt;
    std::string path;
    for (core::InstanceId walk = id; walk.valid(); walk = world.parentOf(walk)) {
        if (walk == root)
            return path;
        const std::string_view name = world.atoms().text(world.name(walk));
        path = path.empty() ? std::string(name) : std::string(name) + "." + path;
    }
    return std::nullopt;
}

// Emits the properties of `liveId` that differ from `refId`, under
// `overridePath`. Recurses into children by position.
//
// **An instance-valued property is compared by PATH, not by id** (D142). The
// live value names an instance in the live world and the reference names one in
// the stamp's own, so the two ids are not comparable -- which is true, and was
// for a long time the reason this skipped every `ValueType::Instance` property
// outright. That skipped two things it should not have: a reference RE-POINTED
// at a different member of the stamp, and a reference to anything BESIDE the
// stamp rather than under it. Both were dropped silently. The second is how a
// `Material` assigned to a part inside a placed stamp disappeared on save --
// the placed-stamp twin of D133, and it did not even reach the writer that
// counts a dropped reference.
//
// Ids are not comparable; paths under each subtree's own root are. Equal paths
// mean the stamp's own reference, untouched, and that is not an override --
// which is the case the blanket skip was protecting, because recording one per
// placed instance would turn a mark and a placement into a claim about an edit
// nobody made. Anything else is written as a full scene path, which the loader
// has resolved through its deferred pass since the format existed.
// Whether one property of one instance differs from the same property of the
// instance it was stamped from.
//
// **One definition, asked by two callers** (S5.6). The save writes what differs;
// the Properties panel MARKS what differs, and offers to revert or apply it. If
// those two answered the question separately they would disagree the first time
// either was touched, and the panel would offer to revert something the save had
// already decided was not an override -- which reads as the editor lying about
// what it is holding.
[[nodiscard]] bool differsFromReference(const World& live, const PropertyDesc& property, std::string_view name,
                                        const World& reference, core::InstanceId refId, core::InstanceId stampRoot,
                                        core::InstanceId referenceRoot, const Value& mine)
{
    // The same property on the reference, found by NAME because the two worlds
    // share their registries but not their ids.
    const core::NameAtom referenceAtom = reference.atoms().lookup(name);
    const PropertyDesc* referenceProperty =
        referenceAtom.valid() ? reference.classes().findProperty(reference.classOf(refId), referenceAtom) : nullptr;
    std::optional<Value> theirs;
    if (referenceProperty != nullptr && referenceProperty->get != nullptr)
        theirs = referenceProperty->get(reference, refId);

    // **`get_if`, and the descriptor's declared type is not enough to justify
    // `get`.** A getter answers `Value{}` -- Nil -- for an instance whose
    // component is not there, which every accessor in the tree does and which is
    // not an error; `std::get` on that throws `bad_variant_access`, and an
    // exception raised here is not caught anywhere between this and `main`. It
    // terminated the editor on save, in a project a person was in the middle of
    // building (D140). Ask what the value HOLDS, never what the schema says it
    // should.
    if (property.type == ValueType::Instance) {
        const core::InstanceId* liveHeld = std::get_if<core::InstanceId>(&mine);
        const core::InstanceId* refHeld = theirs.has_value() ? std::get_if<core::InstanceId>(&*theirs) : nullptr;
        const core::InstanceId liveTarget = liveHeld != nullptr ? *liveHeld : core::InstanceId{};
        const core::InstanceId refTarget = refHeld != nullptr ? *refHeld : core::InstanceId{};
        if (!liveTarget.valid() && !refTarget.valid())
            return false;
        // Ids are not comparable; paths under each subtree's own root are. Equal
        // paths mean the stamp's own reference untouched, which is not an
        // override -- recording one per placed instance would turn a mark and a
        // placement into a claim about an edit nobody made (D142).
        const std::optional<std::string> livePath = pathUnder(live, stampRoot, liveTarget);
        const std::optional<std::string> refPath = pathUnder(reference, referenceRoot, refTarget);
        if (livePath.has_value() && refPath.has_value() && *livePath == *refPath)
            return false;
        return true;
    }

    return !theirs.has_value() || !(*theirs == mine);
}

// **What an instance carries beside its properties**: its attributes, its tags
// and a part's own surface shader parameters (ADR 0091). One writer for a whole
// instance and for an override of a stamped one (B3: a linked instance's lost
// them on every save, because only its properties were compared).
void writeAttributes(JsonWriter& out, const World& world, core::InstanceId id,
                     const std::unordered_map<core::u32, std::string>& paths, SceneIoReport& report)
{
    AttributeMap attributes;
    world.collectAttributes(id, attributes);
    out.key("attributes");
    out.beginObject();
    // Insertion-ordered by construction, so this is stable without sorting
    // -- the same property the world hash relies on.
    for (const auto& entry : attributes) {
        out.key(world.atoms().text(entry.first));
        // **An attribute has no declared type, so the file says it** for every
        // kind whose numbers alone are ambiguous: a `Color3` and a `Vector3` are
        // both three numbers, and a `CFrame`, a `UDim2` and a `Rect` were not
        // read back at all. A number, a string, a boolean, a vector and a
        // sequence keep the spelling they always had.
        switch (valueType(entry.second)) {
        case ValueType::CFrame:
        case ValueType::Color3:
        case ValueType::Vector2:
        case ValueType::UDim:
        case ValueType::UDim2:
        case ValueType::Rect:
            out.beginObject();
            out.field("$", std::string_view(valueTypeName(valueType(entry.second))));
            out.key("v");
            writeValue(out, world, entry.second, paths, report);
            out.endObject();
            break;
        default:
            writeValue(out, world, entry.second, paths, report);
            break;
        }
    }
    out.endObject();
}

void writeTags(JsonWriter& out, const World& world, core::InstanceId id)
{
    TagSet tags;
    world.collectTags(id, tags);
    out.key("tags");
    out.beginArray();
    for (const core::NameAtom tag : tags)
        out.value(world.atoms().text(tag));
    out.endArray();
}

// A number, or two to four in an array. Sorted by name already, so the file is
// stable.
void writeShaderParameters(JsonWriter& out, const std::vector<asset::ShaderParameter>& own)
{
    out.key("shaderParameters");
    out.beginObject();
    for (const asset::ShaderParameter& parameter : own) {
        out.key(parameter.name);
        if (parameter.components <= 1) {
            out.value(static_cast<double>(parameter.value[0]));
            continue;
        }
        out.beginInlineArray();
        for (core::u8 component = 0; component < parameter.components; ++component)
            out.value(static_cast<double>(parameter.value[component]));
        out.endArray();
    }
    out.endObject();
}

void writeCarried(JsonWriter& out, const World& world, core::InstanceId id,
                  const std::unordered_map<core::u32, std::string>& paths, SceneIoReport& report)
{
    AttributeMap attributes;
    world.collectAttributes(id, attributes);
    if (!attributes.empty())
        writeAttributes(out, world, id, paths, report);
    TagSet tags;
    world.collectTags(id, tags);
    if (!tags.empty())
        writeTags(out, world, id);
    if (const std::vector<asset::ShaderParameter>* own = world.partShaderParameters(id); own != nullptr)
        writeShaderParameters(out, *own);
}

// Whether two instances carry the same attributes, tags and shader parameters.
[[nodiscard]] bool sameAttributes(const World& live, core::InstanceId a, const World& reference, core::InstanceId b)
{
    AttributeMap mine;
    AttributeMap theirs;
    live.collectAttributes(a, mine);
    reference.collectAttributes(b, theirs);
    if (mine.size() != theirs.size())
        return false;
    for (const auto& [name, value] : mine) {
        const core::NameAtom other = reference.atoms().lookup(live.atoms().text(name));
        const auto found =
            std::find_if(theirs.begin(), theirs.end(), [&](const auto& entry) { return entry.first == other; });
        if (!other.valid() || found == theirs.end() || !(found->second == value))
            return false;
    }
    return true;
}

[[nodiscard]] bool sameTags(const World& live, core::InstanceId a, const World& reference, core::InstanceId b)
{
    TagSet mine;
    TagSet theirs;
    live.collectTags(a, mine);
    reference.collectTags(b, theirs);
    if (mine.size() != theirs.size())
        return false;
    for (const core::NameAtom tag : mine) {
        const core::NameAtom other = reference.atoms().lookup(live.atoms().text(tag));
        if (!other.valid() || std::find(theirs.begin(), theirs.end(), other) == theirs.end())
            return false;
    }
    return true;
}

[[nodiscard]] bool sameShaderParameters(const World& live, core::InstanceId a, const World& reference,
                                        core::InstanceId b)
{
    const std::vector<asset::ShaderParameter>* mine = live.partShaderParameters(a);
    const std::vector<asset::ShaderParameter>* theirs = reference.partShaderParameters(b);
    const bool mineEmpty = mine == nullptr || mine->empty();
    const bool theirsEmpty = theirs == nullptr || theirs->empty();
    if (mineEmpty || theirsEmpty)
        return mineEmpty == theirsEmpty;
    return *mine == *theirs;
}

void collectOverrides(JsonWriter& out, bool& anyOverride, const World& live, core::InstanceId liveId,
                      const World& reference, core::InstanceId refId, core::InstanceId stampRoot,
                      core::InstanceId referenceRoot, const std::unordered_map<core::u32, std::string>& paths,
                      SceneIoReport& report)
{
    const ClassDescriptor* descriptor = live.classes().find(live.classOf(liveId));
    bool anyHere = false;
    const auto open = [&]() {
        if (anyHere)
            return;
        if (!anyOverride) {
            out.key("overrides");
            out.beginObject();
            anyOverride = true;
        }
        out.key(overridePath(reference, referenceRoot, refId));
        out.beginObject();
        anyHere = true;
    };

    // A child renamed in this instance (B2). The root's own name is the
    // instance's and is written beside its mark.
    if (liveId != stampRoot && live.atoms().text(live.name(liveId)) != reference.atoms().text(reference.name(refId))) {
        open();
        out.key("Name");
        out.value(live.atoms().text(live.name(liveId)));
        ++report.overrides;
    }

    for (const ClassDescriptor* current = descriptor; current != nullptr;
         current = live.classes().find(current->super)) {
        for (const PropertyDesc& property : current->properties) {
            if (!savedProperty(live, property))
                continue;
            const std::string_view name = live.atoms().text(property.name);

            const std::optional<Value> mine = property.get(live, liveId);
            if (!mine.has_value())
                continue;

            if (!differsFromReference(live, property, name, reference, refId, stampRoot, referenceRoot, *mine))
                continue;
            open();
            out.key(name);
            writeValue(out, live, *mine, paths, report);
            ++report.overrides;
        }
    }
    // The carried sets, each whole when it differs: an attribute removed in
    // this instance is one the stamp's copy must lose, which a list of the
    // changed ones cannot say.
    if (!sameAttributes(live, liveId, reference, refId)) {
        open();
        writeAttributes(out, live, liveId, paths, report);
        ++report.overrides;
    }
    if (!sameTags(live, liveId, reference, refId)) {
        open();
        writeTags(out, live, liveId);
        ++report.overrides;
    }
    if (!sameShaderParameters(live, liveId, reference, refId)) {
        open();
        const std::vector<asset::ShaderParameter>* own = live.partShaderParameters(liveId);
        writeShaderParameters(out, own != nullptr ? *own : std::vector<asset::ShaderParameter>{});
        ++report.overrides;
    }
    if (anyHere)
        out.endObject();

    core::InstanceId liveChild = live.firstChild(liveId);
    core::InstanceId referenceChild = reference.firstChild(refId);
    while (liveChild.valid() && referenceChild.valid()) {
        collectOverrides(out, anyOverride, live, liveChild, reference, referenceChild, stampRoot, referenceRoot, paths,
                         report);
        liveChild = live.nextSibling(liveChild);
        referenceChild = reference.nextSibling(referenceChild);
    }
}

// `expandStamped` is the instance whose own stamp mark is IGNORED, and there is
// exactly one situation with one: writing the stamp FILE, whose root is an
// instance of the stamp it is being written from. Every other stamped instance
// collapses to its mark.
void writeUnread(JsonWriter& out, const World& world, core::InstanceId parent);

void writeInstance(JsonWriter& out, const World& world, core::InstanceId id,
                   const std::unordered_map<core::u32, std::string>& paths, SceneIoReport& report,
                   core::InstanceId expandStamped = core::InstanceId{}, StampLibrary* stamps = nullptr)
{
    out.beginObject();

    const ClassDescriptor* descriptor = world.classes().find(world.classOf(id));
    out.field("class", descriptor != nullptr ? world.atoms().text(descriptor->name) : std::string_view{});
    out.field("name", world.atoms().text(world.name(id)));
    ++report.instances;

    // **A node the mount made, holding something authored, is written as its
    // MARK** (ADR 0092): its class and name say which one, the file is its
    // source, and what is written is only what somebody put inside it -- a
    // `Loader` script's modules, say. `readInstance` finds the mount's node by
    // the mark and puts them back under it.
    if ((world.mounted(id) && !t_mountedInFull) || world.fixed(id)) {
        out.field("mounted", true);
        if (world.mounted(id)) {
            if (disabledScript(world, id)) {
                out.key("properties");
                out.beginObject();
                out.field("Enabled", false);
                out.endObject();
                ++report.properties;
            }
            writeCarried(out, world, id, paths, report);
        }
        out.key("children");
        out.beginArray();
        for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child)) {
            if (!engineMade(world, child))
                writeInstance(out, world, child, paths, report, expandStamped, stamps);
        }
        writeUnread(out, world, id);
        out.endArray();
        out.endObject();
        return;
    }

    // **A stamped instance is written as its MARK, its name and where it is, and
    // nothing else** (ADR 0049). Its children belong to the stamp file; writing
    // them again would be the full copy that makes the whole idea worthless, and
    // it would go stale the moment the stamp changed.
    //
    // There is nothing else to write, and that falls out of the break rule
    // rather than being a second decision: if every other edit breaks the mark,
    // a marked instance cannot have any other override.
    const core::NameAtom stamp = world.stampOf(id);
    if (stamp.valid() && id != expandStamped) {
        const std::string stampName(world.atoms().text(stamp));
        const StampLibrary::Entry* reference = stamps != nullptr ? stamps->reference(stampName) : nullptr;

        // **The shape has to match, or this is not an instance of that stamp
        // any more.** Somebody added a child, or deleted one, or the file moved
        // on structurally -- and a format that tried to record THAT would be
        // inventing an added-and-removed-object machinery nobody designed. The
        // instance is written in full instead, which loses nothing, and the
        // count says it happened.
        if (reference != nullptr && sameShape(world, id, *reference->world, reference->root)) {
            out.field("stamp", stampName);
            bool anyOverride = false;
            collectOverrides(out, anyOverride, world, id, *reference->world, reference->root, id, reference->root,
                             paths, report);
            if (anyOverride)
                out.endObject();
            out.endObject();
            ++report.stamped;
            return;
        }
        ++report.unlinkedStamps;
    }

    // The same ancestry walk the world hash makes, and in the same order, so a
    // property redeclared by a subclass is written once and by the subclass.
    bool anyProperty = false;
    for (const ClassDescriptor* current = descriptor; current != nullptr;
         current = world.classes().find(current->super)) {
        for (const PropertyDesc& property : current->properties) {
            if (!savedProperty(world, property))
                continue;
            const std::string_view name = world.atoms().text(property.name);

            const std::optional<Value> value = property.get(world, id);
            if (!value.has_value())
                continue;

            if (!anyProperty) {
                out.key("properties");
                out.beginObject();
                anyProperty = true;
            }
            out.key(name);
            writeValue(out, world, *value, paths, report);
            ++report.properties;
        }
    }
    if (anyProperty)
        out.endObject();

    writeCarried(out, world, id, paths, report);

    // **The ground, because a sculpted world is somebody's afternoon.**
    //
    // Terrain is the one piece of world state that is not a property: a field is
    // chunks of voxels rather than a number, so nothing in the property loop
    // above can reach it, and for one commit a save wrote every instance in the
    // scene and none of the ground.
    //
    // Its own key beside `attributes` and `tags` rather than a side-car file,
    // and the reason is the contract: `writeScene` returns a string and a scene
    // is one text file, which the editor, the packager, `readScene` and every
    // round-trip test all depend on. Base64 keeps that true at a size the cell
    // format's run coder makes reasonable.
    //
    // **`.lterrain`, the same format a streamed cell uses, and not a second
    // one.** Two encoders for one thing is two behaviours for one thing, which
    // is the argument this repository already made about importers. An authored
    // world is written as the cell at the origin -- the whole field, unsplit --
    // and when streaming arrives that field is what gets divided into real
    // cells. A field with nothing in it writes nothing, which is what keeps
    // every existing scene byte-identical.
    // **A tilemap's cells** (the 2D layer), which no property carries, as the
    // terrain's field is not: each painted 16 by 16 block as its corner and its
    // cells, two bytes each, little-endian, in base64. Blocks in key order, so
    // the same map writes the same bytes. An empty map writes nothing.
    if (const Tilemap2DComponent* tilemap = world.tilemaps2d().find(id);
        tilemap != nullptr && !tilemap->chunks.empty()) {
        out.key("tiles");
        out.beginArray();
        for (const auto& [key, chunk] : tilemap->chunks) {
            std::vector<core::u8> bytes;
            bytes.reserve(chunk.size() * 2);
            for (const core::u16 tile : chunk) {
                bytes.push_back(static_cast<core::u8>(tile & 0xFFu));
                bytes.push_back(static_cast<core::u8>(tile >> 8u));
            }
            out.beginObject();
            out.field("x", static_cast<core::i64>(key.x));
            out.field("y", static_cast<core::i64>(key.y));
            out.field("cells", core::base64Encode(bytes));
            out.endObject();
        }
        out.endArray();
        ++report.properties;
    }

    if (const TerrainComponent* terrain = world.terrains().find(id); terrain != nullptr) {
        // **A terrain saved as cells writes where they are, not what they hold**
        // (ADR 0087): the field in memory is only what is resident, so writing
        // it here would write a fraction of the ground and call it the world.
        // The cells themselves are the saver's to write, before this runs.
        if (!terrain->cellIndex.empty()) {
            out.key("terrainCells");
            out.beginObject();
            out.field("index", terrain->cellIndex);
            out.field("voxelSize", static_cast<core::f64>(terrain->field.settings().voxelSize));
            out.field("minHeight", static_cast<core::f64>(terrain->minHeight));
            out.field("maxHeight", static_cast<core::f64>(terrain->maxHeight));
            out.endObject();
            ++report.properties;
        }
        else if (!terrain->field.empty()) {
            asset::TerrainCell cell;
            cell.settings = terrain->field.settings();
            cell.field = terrain->field;
            const std::vector<std::byte> encoded = asset::encodeTerrainCell(cell);
            out.field("terrain", core::base64Encode(std::span<const core::u8>{
                                     reinterpret_cast<const core::u8*>(encoded.data()), encoded.size()}));
            ++report.properties;
        }
        // **The layers, only when they are not the engine's eight** (ADR
        // 0113): a scene written before them and one that kept them read the
        // same, and neither carries a list it does not need. A terrain with
        // none writes an empty one, which is not what absent means.
        if (terrain->layers != asset::defaultTerrainLayers()) {
            out.key("terrainLayers");
            out.beginInlineArray();
            for (const std::string& layer : terrain->layers)
                out.value(layer);
            out.endArray();
            ++report.properties;
        }
        // The rules, likewise only when they are not the default slope rock.
        if (terrain->rules != asset::defaultTerrainRules()) {
            out.key("terrainRules");
            out.beginArray();
            for (const asset::TerrainRule& rule : terrain->rules) {
                out.beginObject();
                out.field("enabled", rule.enabled);
                out.field("material", static_cast<core::i64>(rule.material));
                out.field("slopeMin", static_cast<core::f64>(rule.slopeMin));
                out.field("slopeMax", static_cast<core::f64>(rule.slopeMax));
                out.field("heightMin", static_cast<core::f64>(rule.heightMin));
                out.field("heightMax", static_cast<core::f64>(rule.heightMax));
                out.field("blend", static_cast<core::f64>(rule.blend));
                out.field("noise", static_cast<core::f64>(rule.noise));
                out.key("appliesTo");
                out.beginInlineArray();
                for (const core::u8 layer : rule.appliesTo)
                    out.value(static_cast<core::i64>(layer));
                out.endArray();
                out.endObject();
            }
            out.endArray();
            ++report.properties;
        }
    }

    // What a foliage layer grows on (ADR 0116), only when it names some.
    if (const FoliageLayerComponent* layer = world.foliageLayers().find(id);
        layer != nullptr && !layer->materials.empty()) {
        out.key("foliageMaterials");
        out.beginArray();
        for (const FoliageMaterial& entry : layer->materials) {
            out.beginObject();
            out.field("material", static_cast<core::i64>(entry.material));
            out.field("density", static_cast<core::f64>(entry.density));
            out.endObject();
        }
        out.endArray();
        ++report.properties;
    }
    // And the density painted by hand, a chunk column at a time.
    if (const FoliageLayerComponent* layer = world.foliageLayers().find(id); layer != nullptr && !layer->mask.empty()) {
        out.key("foliageMask");
        out.beginArray();
        for (const FoliageMaskColumn& column : layer->mask) {
            out.beginObject();
            out.field("x", static_cast<core::i64>(column.x));
            out.field("z", static_cast<core::i64>(column.z));
            out.field("density", core::base64Encode(column.density));
            out.endObject();
        }
        out.endArray();
        ++report.properties;
    }

    if (world.firstChild(id).valid() || world.hasUnread(id)) {
        out.key("children");
        out.beginArray();
        // Sibling order, which is observable through `GetChildren` and is
        // therefore part of what a scene has to reproduce.
        for (core::InstanceId child = world.firstChild(id); child.valid(); child = world.nextSibling(child)) {
            // A system made it, so nobody wrote it down and a scene does not
            // record it -- and the whole subtree goes with it, because the parts
            // inside a streamed chunk were not separately authored either.
            if (engineMade(world, child))
                continue;
            writeInstance(out, world, child, paths, report, expandStamped, stamps);
        }
        writeUnread(out, world, id);
        out.endArray();
    }

    out.endObject();
}

// --- what could not be read, kept -----------------------------------------------

void writeJsonValue(JsonWriter& out, const JsonValue& value)
{
    switch (value.type()) {
    case core::JsonType::Object:
        out.beginObject();
        for (core::usize index = 0; index < value.size(); ++index) {
            const std::string_view key = value.keyAt(index);
            out.key(key);
            writeJsonValue(out, value[key]);
        }
        out.endObject();
        return;
    case core::JsonType::Array:
        out.beginArray();
        for (core::usize index = 0; index < value.size(); ++index)
            writeJsonValue(out, value.at(index));
        out.endArray();
        return;
    case core::JsonType::String:
        out.value(value.asString());
        return;
    case core::JsonType::Number:
        out.value(value.asNumber());
        return;
    case core::JsonType::Boolean:
        out.value(value.asBool());
        return;
    default:
        out.nullValue();
        return;
    }
}

// The JSON of one node, as text a later write can put back.
[[nodiscard]] std::string jsonText(const JsonValue& value)
{
    JsonWriter writer;
    writeJsonValue(writer, value);
    return writer.text();
}

// What a read of this parent left out, written back where it was.
void writeUnread(JsonWriter& out, const World& world, core::InstanceId parent)
{
    for (const std::string_view text : world.unreadUnder(parent)) {
        core::JsonDocument document;
        if (!document.parse(text).ok)
            continue;
        writeJsonValue(out, document.root());
    }
}

// --- reading ---------------------------------------------------------------

// A reference cannot be resolved while the tree is being built, because it may
// name something that does not exist yet. Collected and applied at the end.
struct PendingReference
{
    core::InstanceId owner;
    core::NameAtom property;
    std::string path;
    bool isAttribute = false;
};

[[nodiscard]] std::optional<Value> readValue(ValueType expected, const JsonValue& json,
                                             std::vector<PendingReference>& pending, core::InstanceId owner,
                                             core::NameAtom property, bool isAttribute)
{
    const auto number = [](const JsonValue& v, core::usize index) -> core::f64 { return v.at(index).asNumber(); };
    const auto f32At = [&number](const JsonValue& v, core::usize index) -> core::f32 {
        return static_cast<core::f32>(number(v, index));
    };

    switch (expected) {
    case ValueType::Nil:
        return Value{};
    case ValueType::Bool:
        return Value{json.asBool()};
    case ValueType::Number:
        return Value{json.asNumber()};
    case ValueType::String:
        return Value{std::string(json.asString())};
    case ValueType::Vector3:
        if (json.size() < 3)
            return std::nullopt;
        return Value{core::Vec3{f32At(json, 0), f32At(json, 1), f32At(json, 2)}};
    case ValueType::CFrame: {
        if (json.size() < 12)
            return std::nullopt;
        core::CFrameD cf;
        cf.position = {number(json, 0), number(json, 1), number(json, 2)};
        core::usize at = 3;
        for (core::i32 column = 0; column < 3; ++column)
            for (core::i32 row = 0; row < 3; ++row)
                cf.rotation.m[column][row] = f32At(json, at++);
        return Value{cf};
    }
    case ValueType::Color3:
        if (json.size() < 3)
            return std::nullopt;
        return Value{core::Color3{f32At(json, 0), f32At(json, 1), f32At(json, 2)}};
    case ValueType::Instance:
        if (json.isNull())
            return Value{core::InstanceId{}};
        // Deferred: the target may be written later in the file.
        pending.push_back(PendingReference{owner, property, std::string(json.asString()), isAttribute});
        return Value{core::InstanceId{}};
    case ValueType::EnumItem:
        // Resolved by the caller, which is the only place that knows WHICH enum
        // the property accepts.
        return std::nullopt;
    case ValueType::Vector2:
        if (json.size() < 2)
            return std::nullopt;
        return Value{core::Vec2{f32At(json, 0), f32At(json, 1)}};
    case ValueType::UDim:
        if (json.size() < 2)
            return std::nullopt;
        return Value{core::UDim{f32At(json, 0), f32At(json, 1)}};
    case ValueType::UDim2:
        if (json.size() < 4)
            return std::nullopt;
        return Value{
            core::UDim2{core::UDim{f32At(json, 0), f32At(json, 1)}, core::UDim{f32At(json, 2), f32At(json, 3)}}};
    case ValueType::Rect:
        if (json.size() < 4)
            return std::nullopt;
        return Value{
            core::Rect{core::Vec2{f32At(json, 0), f32At(json, 1)}, core::Vec2{f32At(json, 2), f32At(json, 3)}}};
    case ValueType::Material:
        if (json.isNull())
            return Value{};
        if (json.type() != core::JsonType::String)
            return std::nullopt;
        return Value{MaterialRef{std::string(json.asString()), 0}};
    case ValueType::MaterialParameters:
        if (const std::optional<asset::MaterialOverrides> overrides = readMaterialParameters(json))
            return Value{*overrides};
        return std::nullopt;
    case ValueType::ColorSequence:
    case ValueType::NumberSequence:
        if (std::optional<Value> sequence = readSequence(json); sequence && valueType(*sequence) == expected)
            return sequence;
        return std::nullopt;
    }
    return std::nullopt;
}

// **Version 1's surface, as version 2 says it** (ADR 0090). A part's `Color`
// and `Transparency` become overrides on the default material when they are
// not the default -- which is lossless for every part that wore no material,
// because the default material's white times a colour IS that colour. A
// `Material` named an instance by path, and an instance is not a material any
// more: dropped, and counted as the reference it was.
//
// Answers whether it consumed the property.
[[nodiscard]] bool convertVersion1(World& world, core::InstanceId id, std::string_view name, const JsonValue& json,
                                   SceneIoReport& report)
{
    if (t_readVersion >= 2 || world.parts().find(id) == nullptr)
        return false;
    if (name == "Material") {
        if (!json.isNull())
            ++report.droppedReferences;
        return true;
    }
    if (name != "Color" && name != "Transparency")
        return false;

    const core::NameAtom parameters = world.atoms().intern("MaterialParameters");
    const std::optional<Value> current = world.getProperty(id, parameters);
    const auto* held = current.has_value() ? std::get_if<asset::MaterialOverrides>(&*current) : nullptr;
    asset::MaterialOverrides overrides = held != nullptr ? *held : asset::MaterialOverrides{};
    asset::MaterialProperties values;
    if (name == "Color") {
        if (json.type() != core::JsonType::Array || json.size() < 3) {
            ++report.refusedProperties;
            return true;
        }
        values.color =
            core::Color3{static_cast<core::f32>(json.at(0).asNumber()), static_cast<core::f32>(json.at(1).asNumber()),
                         static_cast<core::f32>(json.at(2).asNumber())};
        if (values.color == core::Color3{1.0f, 1.0f, 1.0f})
            return true;
        (void)asset::setOverride(overrides, asset::MaterialField::Color, values);
    }
    else {
        values.transparency = static_cast<core::f32>(json.asNumber());
        if (values.transparency == 0.0f)
            return true;
        (void)asset::setOverride(overrides, asset::MaterialField::Transparency, values);
    }
    if (world.setProperty(id, parameters, Value{overrides}) == World::SetResult::InvalidValue)
        ++report.refusedProperties;
    else
        ++report.properties;
    return true;
}

// **One load's stamps** (audit F6): where they are read from, and each one
// read and parsed once however often the load places it. A stamp whose
// children name it again was re-read and re-parsed at every placement, which
// with K children is K^4 of work before the depth limit said no.
struct StampLoad
{
    explicit StampLoad(const StampSource* from) noexcept : source(from) {}

    const StampSource* source = nullptr;
    // By name; a null document is a stamp that could not be read or parsed.
    std::map<std::string, std::shared_ptr<const core::JsonDocument>, std::less<>> parsed;
};

core::InstanceId readInstance(World& world, core::InstanceId parent, const JsonValue& json,
                              std::vector<PendingReference>& pending, SceneIoReport& report,
                              StampLoad* stamps = nullptr, int depth = 0);

// How deep a stamp may name another stamp before this stops asking.
//
// A stamp of a stamp is refused at authoring time (ADR 0049), so the only way
// to reach this is a hand-edited file -- including one that names ITSELF, which
// without a limit is an infinite tree and a dead process. Four rather than one,
// because refusing a legal-looking file outright is a worse answer than
// refusing an absurd one.
constexpr int kMaxStampDepth = 4;

// The seed a reference world is built with. A constant: nothing in one is
// simulated and nothing reads the generator, and a seed from anywhere else
// would make a save's bytes depend on when it ran.
constexpr core::u64 kReferenceSeed = 0x5245'4645u;

// **A service as the engine makes it**: what "has anybody changed this
// service's settings?" is asked against, and what a new scene puts a service
// back to.
//
// A world of its own, sharing the live world's registries the way a stamp's
// reference world does, with one untouched instance of each class asked about.
// The registries are reached through a `const` world because saving is a read:
// building a world interns two names every world already has, and creating an
// instance changes nothing but the scratch world.
class ServiceDefaults
{
public:
    explicit ServiceDefaults(const World& world)
        : m_scratch(const_cast<ClassRegistry&>(world.classes()), const_cast<EnumRegistry&>(world.enums()),
                    const_cast<core::AtomTable&>(world.atoms()), kReferenceSeed)
    {}

    // What an untouched instance of `classId` holds for `property`.
    [[nodiscard]] std::optional<Value> of(ClassId classId, const PropertyDesc& property)
    {
        auto [made, fresh] = m_made.try_emplace(classId);
        if (fresh)
            made->second = m_scratch.create(classId);
        if (!made->second.valid())
            return std::nullopt;
        return property.get(m_scratch, made->second);
    }

private:
    World m_scratch;
    std::unordered_map<ClassId, core::InstanceId> m_made;
};

// Whether a service's settings are anything but the engine's own: one property
// somebody set, in the Properties panel or from the command bar, or an
// attribute. **Asked beside `holdsAuthored`**, because a service with nothing
// under it was one whose settings were never written -- `Lighting.ClockTime`
// set on an empty `Lighting` was lost at the next save.
[[nodiscard]] bool settingsChanged(const World& world, core::InstanceId service, ServiceDefaults& defaults)
{
    const ClassId classId = world.classOf(service);
    for (const ClassDescriptor* current = world.classes().find(classId); current != nullptr;
         current = world.classes().find(current->super)) {
        for (const PropertyDesc& property : current->properties) {
            if (!savedProperty(world, property))
                continue;
            const std::optional<Value> mine = property.get(world, service);
            const std::optional<Value> engineDefault = defaults.of(classId, property);
            if (mine.has_value() != engineDefault.has_value() || (mine.has_value() && !(*mine == *engineDefault)))
                return true;
        }
    }
    AttributeMap attributes;
    world.collectAttributes(service, attributes);
    return !attributes.empty();
}

// Reads the stamp `name` through the caller's source and builds it under
// `parent`, marked. An invalid id means the source had nothing, the text was
// not a stamp, or the class it names is one this build does not have.
[[nodiscard]] core::InstanceId placeStamp(World& world, core::InstanceId parent, std::string_view name,
                                          SceneIoReport& report, StampLoad* stamps, int depth);

// A dotted path BELOW `root`. Defined beside `resolvePath`, declared here
// because a stamped node applies its overrides through it.
[[nodiscard]] core::InstanceId resolveInside(const World& world, core::InstanceId root, std::string_view path);

// The properties in one JSON object, applied to one instance.
//
// Split out of `applyNode` because a stamped instance's OVERRIDES are exactly
// this shape at a path inside it (ADR 0051) -- and two copies of "how a
// property is read back" would disagree the first time either moved.
// Properties renamed since a scene may have been written, old name first.
constexpr std::pair<std::string_view, std::string_view> kRenamedProperties[]{
    {"HorizontalAlignment", "TextXAlignment"},
    {"VerticalAlignment", "TextYAlignment"},
};

void applyProperties(World& world, core::InstanceId id, const JsonValue& properties,
                     std::vector<PendingReference>& pending, SceneIoReport& report)
{
    const ClassId classId = world.classOf(id);
    if (properties.type() == core::JsonType::Object) {
        for (core::usize index = 0; index < properties.size(); ++index) {
            const std::string_view name = properties.keyAt(index);
            // An override's carried sets, applied by `applyCarried`.
            if (name == "attributes" || name == "tags" || name == "shaderParameters")
                continue;
            // Only an override carries it -- an instance's own name is its
            // `name` -- and it is a child of a stamp renamed in one instance.
            if (name == "Name") {
                if (const JsonValue renamed = properties[name]; renamed.type() == core::JsonType::String)
                    world.setName(id, world.atoms().intern(renamed.asString()));
                else
                    ++report.refusedProperties;
                continue;
            }
            if (convertVersion1(world, id, name, properties[name], report))
                continue;
            core::NameAtom atom = world.atoms().intern(name);
            const PropertyDesc* property = world.classes().findProperty(classId, atom);
            // **A property that was renamed reads under its new name.** Only
            // when the class no longer has the old one: a `UIListLayout` still
            // has `HorizontalAlignment`, and a `TextLabel` now calls its own
            // `TextXAlignment`.
            if (property == nullptr) {
                for (const auto& [before, after] : kRenamedProperties) {
                    if (name != before)
                        continue;
                    atom = world.atoms().intern(after);
                    property = world.classes().findProperty(classId, atom);
                }
            }
            if (property == nullptr || property->set == nullptr) {
                // A scene written by a newer build should still open here, minus
                // what this one cannot express. Counted, never fatal.
                ++report.refusedProperties;
                continue;
            }

            const JsonValue json2 = properties[name];
            std::optional<Value> value;
            if (property->type == ValueType::EnumItem) {
                // The domain comes from the descriptor, which is what
                // `PropertyDesc::enumName` was added for -- resolving it from
                // the current value would need a value to already be there.
                const EnumId enumId = world.enums().findId(property->enumName);
                if (const EnumItemDesc* item =
                        enumId != InvalidEnum ? world.enums().findItem(enumId, world.atoms().intern(json2.asString()))
                                              : nullptr;
                    item != nullptr) {
                    value = Value{EnumValue{static_cast<core::u16>(enumId), item->value}};
                }
            }
            else {
                value = readValue(property->type, json2, pending, id, atom, false);
            }

            if (!value.has_value()) {
                ++report.refusedProperties;
                continue;
            }
            if (world.setProperty(id, atom, *value) == World::SetResult::InvalidValue)
                ++report.refusedProperties;
            else
                ++report.properties;
        }
    }
}

// Everything an instance carries that is not its identity or its children.
//
// Split out because the scene's ROOT is applied to the `Workspace` that already
// exists rather than created -- and its own properties are as much a part of the
// world as its children's are. `CurrentCamera` is the one that proves it: a
// scene that restored every part and not the camera would load into a world
// nothing can see.
// The attributes, tags and shader parameters in `json`, onto `id` -- replacing
// what it had of each set the JSON names when `replace` is set, which is what an
// override of a stamped instance means (`writeCarried`).
void applyCarried(World& world, core::InstanceId id, const JsonValue& json, SceneIoReport& report, bool replace)
{
    if (replace && json["attributes"].type() == core::JsonType::Object) {
        AttributeMap had;
        world.collectAttributes(id, had);
        for (const auto& entry : had)
            (void)world.setAttribute(id, entry.first, Value{});
    }
    if (replace && json["tags"].type() == core::JsonType::Array) {
        TagSet had;
        world.collectTags(id, had);
        for (const core::NameAtom tag : had)
            (void)world.removeTag(id, tag);
    }
    if (replace && json["shaderParameters"].type() == core::JsonType::Object) {
        if (const std::vector<asset::ShaderParameter>* had = world.partShaderParameters(id); had != nullptr) {
            std::vector<std::string> names;
            for (const asset::ShaderParameter& parameter : *had)
                names.push_back(parameter.name);
            for (const std::string& name : names)
                (void)world.clearPartShaderParameter(id, name);
        }
    }

    if (const JsonValue attributes = json["attributes"]; attributes.type() == core::JsonType::Object) {
        for (core::usize index = 0; index < attributes.size(); ++index) {
            const std::string_view name = attributes.keyAt(index);
            const JsonValue entry = attributes[name];
            // An attribute has no declared type, so its JSON shape is the only
            // thing that says what it is -- which is why the writer's encoding
            // has to stay unambiguous for the shapes an attribute can hold.
            std::optional<Value> value;
            switch (entry.type()) {
            case core::JsonType::Boolean:
                value = Value{entry.asBool()};
                break;
            case core::JsonType::Number:
                value = Value{entry.asNumber()};
                break;
            case core::JsonType::String:
                value = Value{std::string(entry.asString())};
                break;
            case core::JsonType::Array:
                if (entry.size() == 3)
                    value = Value{core::Vec3{static_cast<core::f32>(entry.at(0).asNumber()),
                                             static_cast<core::f32>(entry.at(1).asNumber()),
                                             static_cast<core::f32>(entry.at(2).asNumber())}};
                break;
            case core::JsonType::Object:
                // A typed value (`writeAttributes`), or a sequence (ADR 0110).
                if (const std::string_view tag = entry["$"].asString(); !tag.empty()) {
                    for (const ValueType candidate : {ValueType::CFrame, ValueType::Color3, ValueType::Vector2,
                                                      ValueType::UDim, ValueType::UDim2, ValueType::Rect}) {
                        if (tag == valueTypeName(candidate)) {
                            std::vector<PendingReference> none;
                            value = readValue(candidate, entry["v"], none, id, world.atoms().intern(name), true);
                            break;
                        }
                    }
                }
                else {
                    value = readSequence(entry);
                }
                break;
            default:
                break;
            }
            if (value.has_value())
                (void)world.setAttribute(id, world.atoms().intern(name), *value);
            else
                ++report.refusedProperties;
        }
    }

    if (const JsonValue tags = json["tags"]; tags.type() == core::JsonType::Array) {
        for (core::usize index = 0; index < tags.size(); ++index)
            (void)world.addTag(id, world.atoms().intern(tags.at(index).asString()));
    }

    if (const JsonValue own = json["shaderParameters"]; own.type() == core::JsonType::Object) {
        for (core::usize index = 0; index < own.size(); ++index) {
            const std::string_view name = own.keyAt(index);
            const JsonValue entry = own[name];
            asset::ShaderParameter parameter;
            parameter.name = std::string(name);
            bool whole = asset::isShaderParameterName(name);
            if (entry.type() == core::JsonType::Number) {
                parameter.value[0] = static_cast<core::f32>(entry.asNumber());
            }
            else if (entry.type() == core::JsonType::Array && entry.size() >= 2 && entry.size() <= 4) {
                parameter.components = static_cast<core::u8>(entry.size());
                for (core::usize component = 0; component < entry.size(); ++component) {
                    whole = whole && entry.at(component).type() == core::JsonType::Number;
                    parameter.value[component] = static_cast<core::f32>(entry.at(component).asNumber());
                }
            }
            else {
                whole = false;
            }
            if (whole)
                world.setPartShaderParameter(id, std::move(parameter));
            else
                ++report.refusedProperties;
        }
    }
}

void applyNode(World& world, core::InstanceId id, const JsonValue& json, std::vector<PendingReference>& pending,
               SceneIoReport& report)
{
    applyProperties(world, id, json["properties"], pending, report);
    applyCarried(world, id, json, report, false);

    // A tilemap's painted blocks.
    if (const JsonValue tiles = json["tiles"]; tiles.type() == core::JsonType::Array) {
        if (Tilemap2DComponent* tilemap = world.tilemaps2d().find(id); tilemap != nullptr) {
            bool whole = true;
            for (core::usize at = 0; at < tiles.size(); ++at) {
                const JsonValue row = tiles.at(at);
                const std::optional<std::vector<core::u8>> bytes = core::base64Decode(row["cells"].asString());
                if (!bytes.has_value() || bytes->size() != sizeof(TileChunk::value_type) * TileChunk{}.size()) {
                    whole = false;
                    continue;
                }
                TileChunk chunk{};
                bool painted = false;
                for (core::usize cell = 0; cell < chunk.size(); ++cell) {
                    chunk[cell] = static_cast<core::u16>((*bytes)[cell * 2] | ((*bytes)[cell * 2 + 1] << 8u));
                    painted = painted || chunk[cell] != 0;
                }
                if (painted) {
                    tilemap->chunks[TileChunkKey{static_cast<core::i32>(row["x"].asInteger()),
                                                 static_cast<core::i32>(row["y"].asInteger())}] = chunk;
                }
            }
            tilemap->revision += 1;
            if (whole)
                ++report.properties;
            else
                ++report.droppedReferences;
        }
    }

    // Ground saved as cells (ADR 0087): the settings and where the cells are,
    // and an empty field, which whoever streams the cells fills.
    if (const JsonValue cells = json["terrainCells"]; cells.type() == core::JsonType::Object) {
        if (TerrainComponent* component = world.terrains().find(id); component != nullptr) {
            asset::FieldSettings settings;
            settings.voxelSize =
                static_cast<core::f32>(cells["voxelSize"].asNumber(static_cast<core::f64>(settings.voxelSize)));
            settings.minHeight =
                static_cast<core::f32>(cells["minHeight"].asNumber(static_cast<core::f64>(settings.minHeight)));
            settings.maxHeight =
                static_cast<core::f32>(cells["maxHeight"].asNumber(static_cast<core::f64>(settings.maxHeight)));
            const std::string_view index = cells["index"].asString();
            // Checked as the floats they now are (audit F9).
            if (asset::saneFieldSettings(settings) && !index.empty()) {
                component->field = asset::TerrainField(settings);
                component->shipped = asset::TerrainField(settings);
                component->minHeight = settings.minHeight;
                component->maxHeight = settings.maxHeight;
                component->cellIndex = std::string(index);
                component->fieldRevision += 1;
                ++report.properties;
            }
            else {
                ++report.droppedReferences;
            }
        }
    }

    if (const JsonValue materials = json["foliageMaterials"]; materials.type() == core::JsonType::Array) {
        if (FoliageLayerComponent* layer = world.foliageLayers().find(id); layer != nullptr) {
            layer->materials.clear();
            for (core::usize index = 0; index < materials.size() && index < 32; ++index) {
                const JsonValue entry = materials.at(index);
                FoliageMaterial read;
                read.material = static_cast<core::u8>(std::clamp(entry["material"].asNumber(1.0), 1.0, 255.0));
                read.density = static_cast<core::f32>(std::max(0.0, entry["density"].asNumber(1.0)));
                layer->materials.push_back(read);
            }
            ++report.properties;
        }
    }
    if (const JsonValue mask = json["foliageMask"]; mask.type() == core::JsonType::Array) {
        if (FoliageLayerComponent* layer = world.foliageLayers().find(id); layer != nullptr) {
            layer->mask.clear();
            constexpr core::usize Bytes = static_cast<core::usize>(asset::ChunkEdge) * asset::ChunkEdge;
            for (core::usize index = 0; index < mask.size(); ++index) {
                const JsonValue entry = mask.at(index);
                const std::optional<std::vector<core::u8>> bytes = core::base64Decode(entry["density"].asString());
                // A column of the wrong size is somebody else's file, not ours.
                if (!bytes.has_value() || bytes->size() != Bytes) {
                    ++report.droppedReferences;
                    continue;
                }
                layer->mask.push_back(FoliageMaskColumn{static_cast<core::i32>(entry["x"].asNumber(0.0)),
                                                        static_cast<core::i32>(entry["z"].asNumber(0.0)), *bytes});
            }
            ++report.properties;
        }
    }

    // What the terrain's material bytes mean (ADR 0113). **Absent is the
    // engine's eight and its slope rock**, which is what every scene written
    // before a terrain could start with none meant; a new terrain holds none
    // (2026-09-29) and writes its empty list.
    if (TerrainComponent* component = world.terrains().find(id);
        component != nullptr && json["terrainLayers"].type() != core::JsonType::Array) {
        component->layers = asset::defaultTerrainLayers();
        component->layersRevision += 1;
    }
    if (TerrainComponent* component = world.terrains().find(id);
        component != nullptr && json["terrainRules"].type() != core::JsonType::Array)
        component->rules = asset::defaultTerrainRules();
    if (const JsonValue layers = json["terrainLayers"]; layers.type() == core::JsonType::Array) {
        if (TerrainComponent* component = world.terrains().find(id); component != nullptr) {
            std::vector<std::string> read;
            for (core::usize index = 0; index < layers.size() && read.size() < asset::MaxTerrainLayers; ++index)
                read.emplace_back(layers.at(index).asString());
            component->layers = std::move(read);
            component->layersRevision += 1;
            ++report.properties;
        }
    }

    if (const JsonValue rules = json["terrainRules"]; rules.type() == core::JsonType::Array) {
        if (TerrainComponent* component = world.terrains().find(id); component != nullptr) {
            std::vector<asset::TerrainRule> read;
            for (core::usize index = 0; index < rules.size() && read.size() < asset::MaxTerrainRules; ++index) {
                const JsonValue entry = rules.at(index);
                asset::TerrainRule rule;
                const auto number = [&](std::string_view key, core::f32 fallback) {
                    return static_cast<core::f32>(entry[key].asNumber(static_cast<core::f64>(fallback)));
                };
                rule.enabled = entry["enabled"].asBool(true);
                rule.material = static_cast<core::u8>(std::clamp(entry["material"].asNumber(3.0), 1.0, 255.0));
                rule.slopeMin = number("slopeMin", rule.slopeMin);
                rule.slopeMax = number("slopeMax", rule.slopeMax);
                rule.heightMin = number("heightMin", rule.heightMin);
                rule.heightMax = number("heightMax", rule.heightMax);
                rule.blend = number("blend", rule.blend);
                rule.noise = number("noise", rule.noise);
                const JsonValue applies = entry["appliesTo"];
                for (core::usize at = 0; at < applies.size(); ++at)
                    rule.appliesTo.push_back(
                        static_cast<core::u8>(std::clamp(applies.at(at).asNumber(0.0), 0.0, 255.0)));
                read.push_back(std::move(rule));
            }
            component->rules = std::move(read);
            ++report.properties;
        }
    }

    // The ground. Counted as one property either way, so a load that could not
    // read it says so in the same report a dropped reference would.
    if (const JsonValue terrain = json["terrain"]; terrain.type() == core::JsonType::String) {
        TerrainComponent* component = world.terrains().find(id);
        if (component != nullptr) {
            const std::optional<std::vector<core::u8>> bytes = core::base64Decode(terrain.asString());
            asset::TerrainCell cell;
            const bool decoded =
                bytes.has_value() &&
                !asset::decodeTerrainCell(
                     std::span<const std::byte>{reinterpret_cast<const std::byte*>(bytes->data()), bytes->size()}, cell,
                     asset::WholeFieldLimits)
                     .has_value();
            if (decoded) {
                component->field = std::move(cell.field);
                component->shipped = component->field;
                // **The settings ride with the field**, so `MinHeight` and
                // `MaxHeight` are whatever the ground was actually sculpted
                // under rather than whatever the properties happened to say.
                // A terrain saved before the voxel grid (ADR 0082) arrives
                // here already converted, at its own voxel size.
                component->minHeight = component->field.settings().minHeight;
                component->maxHeight = component->field.settings().maxHeight;
                component->fieldRevision += 1;
                ++report.properties;
            }
            else {
                ++report.droppedReferences;
            }
        }
    }
}

// **What an instance has of its own** (ADR 0051), keyed by the path inside the
// stamp: `""` is the instance itself and `Lantern.Bulb` is something under it.
//
// Applied AFTER the stamp has been built, which is what makes an override an
// override -- the stamp says what a thing is and these say what this one of them
// is like. One function rather than two, because a load and a live refresh
// (`restamp`) put the same overrides back on top of the same file and two
// spellings of that would disagree the first time either moved.
void applyOverrides(World& world, core::InstanceId placed, const JsonValue& overrides,
                    std::vector<PendingReference>& pending, SceneIoReport& report)
{
    if (overrides.type() != core::JsonType::Object)
        return;

    for (core::usize index = 0; index < overrides.size(); ++index) {
        const std::string_view path = overrides.keyAt(index);
        const core::InstanceId target = path.empty() ? placed : resolveInside(world, placed, path);
        if (!target.valid()) {
            // The stamp moved on and no longer has what this override names.
            // Counted rather than fatal, exactly as an unknown class is: a scene
            // should still open, minus what is gone.
            ++report.refusedProperties;
            continue;
        }
        applyProperties(world, target, overrides[path], pending, report);
        applyCarried(world, target, overrides[path], report, true);
        ++report.overrides;
    }
}

core::InstanceId readInstance(World& world, core::InstanceId parent, const JsonValue& json,
                              std::vector<PendingReference>& pending, SceneIoReport& report, StampLoad* stamps,
                              int depth)
{
    // **A node that names a stamp is not built; it is STAMPED** (ADR 0049).
    // What the scene holds for it is a mark, a name and where it is, and
    // everything else comes from the stamp file -- which is the whole reason
    // the mark is worth having, and the reason changing a stamp changes every
    // unbroken instance of it.
    if (const std::string_view stampName = json["stamp"].asString(); !stampName.empty()) {
        // **Deep and wide both bounded** (audit F6): the depth limit stops a
        // hand-edited stamp that names itself going down for ever, and the
        // load's instance limit stops it going wide -- K children of one are
        // K^3 instances at depth four.
        const bool placeable = stamps != nullptr && stamps->source != nullptr && *stamps->source &&
                               depth < kMaxStampDepth && report.instances < report.instanceLimit;
        const core::InstanceId placed =
            placeable ? placeStamp(world, parent, stampName, report, stamps, depth) : core::InstanceId{};
        if (!placed.valid()) {
            // Counted rather than fatal, for the same reason an unknown class
            // is: a scene that names a stamp somebody deleted should still
            // open, minus what is gone -- and KEPT, so the next save writes the
            // mark back and a stamp restored brings it back.
            ++report.missingStamps;
            world.keepUnread(parent, jsonText(json));
            return {};
        }
        world.setName(placed, world.atoms().intern(json["name"].asString()));

        applyOverrides(world, placed, json["overrides"], pending, report);
        ++report.stamped;
        return placed;
    }

    // **A mark of a node the mount made** (ADR 0092): the node is the one the
    // file made, found by class and name, and only its children are read. A
    // mark whose file has gone keeps its children in a `Folder` of that name,
    // because they are somebody's work and the file was not.
    if (json["mounted"].asBool()) {
        const core::NameAtom name = world.atoms().intern(json["name"].asString());
        const ClassId markedClass = world.classes().findId(world.atoms().intern(json["class"].asString()));
        core::InstanceId node;
        for (core::InstanceId child = world.firstChild(parent); child.valid(); child = world.nextSibling(child)) {
            if ((world.mounted(child) || world.fixed(child)) && world.name(child) == name &&
                world.classOf(child) == markedClass) {
                node = child;
                break;
            }
        }
        if (!node.valid()) {
            const ClassId folderClass = world.classes().findId(world.atoms().intern("Folder"));
            if (folderClass == InvalidClass)
                return {};
            node = world.create(folderClass);
            world.setName(node, name);
            (void)world.setParent(node, parent);
            ++report.orphanedMounts;
        }
        // What the file does not hold: `Enabled` on the script itself, and
        // attributes and tags on whatever the node became.
        if (world.classOf(node) == markedClass) {
            if (const JsonValue properties = json["properties"]; properties.type() == core::JsonType::Object) {
                if (const JsonValue enabled = properties["Enabled"]; enabled.type() == core::JsonType::Boolean) {
                    const World::SetResult set =
                        world.setProperty(node, world.atoms().intern("Enabled"), Value{enabled.asBool()});
                    if (set == World::SetResult::Changed || set == World::SetResult::Unchanged)
                        ++report.properties;
                }
            }
        }
        applyCarried(world, node, json, report, false);
        if (const JsonValue children = json["children"]; children.type() == core::JsonType::Array) {
            for (core::usize index = 0; index < children.size(); ++index)
                readInstance(world, node, children.at(index), pending, report, stamps, depth);
        }
        return node;
    }

    const std::string_view className = json["class"].asString();
    const ClassId classId = world.classes().findId(world.atoms().intern(className));
    if (classId == InvalidClass) {
        // The whole subtree goes with it. A `Part` standing in for a class this
        // build does not have would be a lie shaped like a recovery, and the
        // children under it would be parented to something that is not what
        // they were authored against.
        ++report.unknownClasses;
        // Kept as it was written, so a save by this build does not delete what
        // a build that has the class made.
        world.keepUnread(parent, jsonText(json));
        return {};
    }

    const core::InstanceId id = world.create(classId);
    world.setName(id, world.atoms().intern(json["name"].asString()));
    (void)world.setParent(id, parent);
    ++report.instances;

    applyNode(world, id, json, pending, report);

    if (const JsonValue children = json["children"]; children.type() == core::JsonType::Array) {
        for (core::usize index = 0; index < children.size(); ++index)
            readInstance(world, id, children.at(index), pending, report, stamps, depth);
    }
    return id;
}

// A dotted path BELOW `root`, where `resolvePath` takes one whose first segment
// names the root itself. Two functions rather than a flag, because the two
// shapes come from two formats and conflating them is how a path resolves to
// the wrong instance in exactly one of them.
[[nodiscard]] core::InstanceId resolveInside(const World& world, core::InstanceId root, std::string_view path)
{
    core::InstanceId at = root;
    while (!path.empty() && at.valid()) {
        const core::usize cursor = path.find('.');
        const std::string_view segment = cursor == std::string_view::npos ? path : path.substr(0, cursor);
        // The name itself first: a child may really be called `Door#2`.
        if (const core::NameAtom whole = world.atoms().lookup(segment);
            whole.valid() && world.findFirstChild(at, whole).valid()) {
            at = world.findFirstChild(at, whole);
        }
        else {
            // `name#n`: the n-th child of that name (`overrideSegment`).
            const core::usize hash = segment.rfind('#');
            int ordinal = 0;
            if (hash != std::string_view::npos) {
                for (const char digit : segment.substr(hash + 1))
                    ordinal = digit >= '0' && digit <= '9' ? ordinal * 10 + (digit - '0') : -1000000;
            }
            const core::NameAtom atom = ordinal > 0 ? world.atoms().lookup(segment.substr(0, hash)) : core::NameAtom{};
            if (!atom.valid())
                return {};
            core::InstanceId found;
            int seen = 0;
            for (core::InstanceId child = world.firstChild(at); child.valid(); child = world.nextSibling(child)) {
                if (world.name(child) == atom && ++seen == ordinal) {
                    found = child;
                    break;
                }
            }
            at = found;
        }
        if (cursor == std::string_view::npos)
            break;
        path.remove_prefix(cursor + 1);
    }
    return at;
}

[[nodiscard]] core::InstanceId resolvePath(const World& world, core::InstanceId root, std::string_view path)
{
    // The first segment names the root itself, which is how `collectPaths`
    // wrote it.
    core::usize cursor = path.find('.');
    core::InstanceId at = root;
    if (cursor == std::string_view::npos)
        return at;
    path.remove_prefix(cursor + 1);

    while (!path.empty()) {
        cursor = path.find('.');
        const std::string_view segment = cursor == std::string_view::npos ? path : path.substr(0, cursor);
        const core::NameAtom atom = world.atoms().lookup(segment);
        if (!atom.valid())
            return {};
        at = world.findFirstChild(at, atom);
        if (!at.valid())
            return {};
        if (cursor == std::string_view::npos)
            break;
        path.remove_prefix(cursor + 1);
    }
    return at;
}
// **A stamp's internal references resolve against the STAMP's own root**, not
// against the scene's. A path inside a stamp names something inside that stamp
// -- `Post.Lantern` is the lantern on this post -- and resolving it against the
// scene would find some other instance with that path, or nothing.
core::InstanceId placeStamp(World& world, core::InstanceId parent, std::string_view name, SceneIoReport& report,
                            StampLoad* stamps, int depth)
{
    std::shared_ptr<const core::JsonDocument> document;
    if (const auto known = stamps->parsed.find(name); known != stamps->parsed.end()) {
        document = known->second;
    }
    else {
        if (const std::optional<std::string> text = (*stamps->source)(name); text.has_value()) {
            auto fresh = std::make_shared<core::JsonDocument>();
            if (fresh->parse(*text).ok)
                document = std::move(fresh);
        }
        stamps->parsed.emplace(std::string(name), document);
    }
    if (document == nullptr)
        return {};

    const JsonValue root = document->root();
    if (root["format"].asString() != kFormat || !readableVersion(root["version"].asInteger()))
        return {};
    const ReadingVersion reading(root["version"].asInteger());

    const JsonValue rootNode = root["root"];
    if (rootNode.type() != core::JsonType::Object)
        return {};

    std::vector<PendingReference> pending;
    const core::InstanceId placed = readInstance(world, parent, rootNode, pending, report, stamps, depth + 1);
    if (!placed.valid())
        return {};

    for (const PendingReference& reference : pending) {
        const core::InstanceId target = resolvePath(world, placed, reference.path);
        if (!target.valid()) {
            ++report.droppedReferences;
            continue;
        }
        if (reference.isAttribute)
            (void)world.setAttribute(reference.owner, reference.property, Value{target});
        else
            (void)world.setProperty(reference.owner, reference.property, Value{target});
    }

    world.setStamp(placed, world.atoms().intern(name));
    return placed;
}
} // namespace

namespace {

// --- The block world (V1) ------------------------------------------------------
//
// **At the top of the scene, not inside the tree**, because `VoxelService` is a
// service and the tree a scene carries is Workspace's. One key, written only
// when there is something in it, so a scene with no blocks is byte-identical to
// one written before blocks existed.

VoxelComponent* voxelsOf(World& world) noexcept
{
    VoxelComponent* found = nullptr;
    world.voxels().forEach([&found](core::InstanceId, VoxelComponent& voxels) {
        if (found == nullptr)
            found = &voxels;
    });
    return found;
}

void writeVoxels(JsonWriter& writer, const World& world)
{
    const VoxelComponent* voxels = nullptr;
    world.voxels().forEach([&voxels](core::InstanceId, const VoxelComponent& found) {
        if (voxels == nullptr)
            voxels = &found;
    });
    if (voxels == nullptr || (voxels->grid.chunkCount() == 0 && voxels->types.empty() && voxels->blockSize == 1.0f))
        return;

    writer.key("voxels");
    writer.beginObject();
    writer.field("blockSize", static_cast<f64>(voxels->blockSize));
    writer.key("types");
    writer.beginArray();
    for (const VoxelBlockType& type : voxels->types) {
        writer.beginObject();
        writer.field("name", world.atoms().text(type.name));
        const auto colour = [&writer](std::string_view key, const core::Color3& value) {
            writer.key(key);
            writer.beginArray();
            writer.value(static_cast<f64>(value.r));
            writer.value(static_cast<f64>(value.g));
            writer.value(static_cast<f64>(value.b));
            writer.endArray();
        };
        colour("color", type.color);
        colour("side", type.side);
        colour("bottom", type.bottom);
        // Written only when set, so a block world made before textures reads
        // back byte for byte.
        if (type.texture.valid())
            writer.field("texture", world.atoms().text(type.texture));
        if (type.sideTexture.valid())
            writer.field("sideTexture", world.atoms().text(type.sideTexture));
        if (type.bottomTexture.valid())
            writer.field("bottomTexture", world.atoms().text(type.bottomTexture));
        if (type.opacity != 0) {
            writer.field("opacity", static_cast<f64>(type.opacity));
            writer.field("transparency", static_cast<f64>(type.transparency));
        }
        if (type.fluidReach > 0) {
            writer.field("fluidReach", static_cast<f64>(type.fluidReach));
            writer.field("fluidTicks", static_cast<f64>(type.fluidTicks));
        }
        writer.endObject();
    }
    writer.endArray();
    // Written only when there are any, so a block world with none reads back
    // byte for byte.
    if (!voxels->fluidReactions.empty()) {
        writer.key("fluidReactions");
        writer.beginArray();
        for (const VoxelComponent::FluidReaction& reaction : voxels->fluidReactions) {
            writer.beginArray();
            writer.value(static_cast<f64>(reaction.from));
            writer.value(static_cast<f64>(reaction.touching));
            writer.value(static_cast<f64>(reaction.result));
            writer.endArray();
        }
        writer.endArray();
    }
    writer.key("chunks");
    writer.beginArray();
    for (const asset::VoxelChunkKey key : voxels->grid.chunkKeys()) {
        const asset::VoxelChunk* chunk = voxels->grid.findChunk(key);
        if (chunk == nullptr)
            continue;
        writer.beginObject();
        writer.field("x", static_cast<core::i64>(key.x));
        writer.field("y", static_cast<core::i64>(key.y));
        writer.field("z", static_cast<core::i64>(key.z));
        const std::vector<core::u8> bytes = asset::encodeVoxelChunk(*chunk);
        writer.field("blocks", core::base64Encode(bytes));
        writer.endObject();
    }
    writer.endArray();
    writer.endObject();
}

// Replaces the block world with what the scene says -- nothing, when it has no
// `voxels` key. A chunk that does not decode is dropped and counted, the way a
// malformed terrain cell is.
void readVoxels(World& world, const JsonValue& root, SceneIoReport& out)
{
    VoxelComponent* voxels = voxelsOf(world);
    if (voxels == nullptr)
        return;
    voxels->grid.clear();
    // The last scene's package too: a scene with no block world ships none,
    // and a copy left over was the old scene's ground sent to every joiner
    // as removed (terrain audit R5).
    voxels->shipped.clear();
    voxels->types.clear();
    voxels->fluidWakes.clear();
    voxels->fluidReactions.clear();
    voxels->blockSize = 1.0f;
    voxels->revision += 1;

    const JsonValue node = root["voxels"];
    if (node.type() != core::JsonType::Object)
        return;
    // **As the float it becomes, and within what a block can be** (audit F9):
    // a positive double can still narrow to zero, a denormal or an infinity,
    // and every loop over a block's metres divides by it.
    const auto size = static_cast<f32>(node["blockSize"].asNumber(1.0));
    if (size >= asset::MinVoxelSize && size <= asset::MaxVoxelSize)
        voxels->blockSize = size;
    if (const JsonValue types = node["types"]; types.type() == core::JsonType::Array) {
        for (core::usize at = 0; at < types.size(); ++at) {
            const JsonValue type = types.at(at);
            const auto colour = [](const JsonValue& value, core::Color3 fallback) {
                if (value.type() != core::JsonType::Array)
                    return fallback;
                return core::Color3{static_cast<f32>(value.at(0).asNumber(1.0)),
                                    static_cast<f32>(value.at(1).asNumber(1.0)),
                                    static_cast<f32>(value.at(2).asNumber(1.0))};
            };
            const core::Color3 top = colour(type["color"], core::Color3{1.0f, 1.0f, 1.0f});
            const core::Color3 side = colour(type["side"], top);
            const auto image = [&world](const JsonValue& value) {
                return value.type() == core::JsonType::String && !value.asString().empty()
                           ? world.atoms().intern(value.asString())
                           : core::NameAtom{};
            };
            const auto opacity = static_cast<core::i32>(std::clamp(type["opacity"].asNumber(0.0), 0.0, 2.0));
            const auto transparency = static_cast<f32>(std::clamp(type["transparency"].asNumber(0.5), 0.0, 1.0));
            VoxelBlockType read{world.atoms().intern(type["name"].asString()),
                                top,
                                side,
                                colour(type["bottom"], side),
                                image(type["texture"]),
                                image(type["sideTexture"]),
                                image(type["bottomTexture"]),
                                opacity,
                                transparency};
            read.fluidReach = static_cast<core::u8>(
                std::clamp(type["fluidReach"].asNumber(0.0), 0.0, static_cast<f64>(asset::MaxFluidReach)));
            read.fluidTicks = static_cast<core::u32>(std::clamp(type["fluidTicks"].asNumber(5.0), 1.0, 65535.0));
            voxels->types.push_back(read);
        }
    }
    if (const JsonValue reactions = node["fluidReactions"]; reactions.type() == core::JsonType::Array) {
        const auto id = [](const JsonValue& value) {
            return static_cast<asset::BlockId>(
                std::clamp(value.asNumber(0.0), 0.0, static_cast<f64>(asset::MaxBlockType)));
        };
        for (core::usize at = 0; at < reactions.size(); ++at) {
            const JsonValue reaction = reactions.at(at);
            setFluidReaction(*voxels, id(reaction.at(0)), id(reaction.at(1)), id(reaction.at(2)));
        }
    }
    if (const JsonValue chunks = node["chunks"]; chunks.type() == core::JsonType::Array) {
        std::vector<asset::BlockId> blocks;
        for (core::usize at = 0; at < chunks.size(); ++at) {
            const JsonValue chunk = chunks.at(at);
            const std::optional<std::vector<core::u8>> bytes = core::base64Decode(chunk["blocks"].asString());
            if (!bytes.has_value() || !asset::decodeVoxelChunk(*bytes, blocks)) {
                ++out.refusedProperties;
                continue;
            }
            // A key past the block world's reach is refused, not wrapped into
            // one inside it (audit F12).
            const auto inReach = [&chunk](const char* axis) {
                const auto value = chunk[axis].asInteger();
                return value >= -asset::MaxVoxelChunkKey && value <= asset::MaxVoxelChunkKey;
            };
            if (!inReach("x") || !inReach("y") || !inReach("z")) {
                ++out.refusedProperties;
                continue;
            }
            voxels->grid.setChunk(asset::VoxelChunkKey{static_cast<core::i32>(chunk["x"].asInteger()),
                                                       static_cast<core::i32>(chunk["y"].asInteger()),
                                                       static_cast<core::i32>(chunk["z"].asInteger())},
                                  blocks);
        }
    }
    // **A scene keeps its water, not the steps it was due.** Every fluid block
    // is looked at once when the world is read, so a spring placed in the
    // editor and saved runs when the game does; a lake that is already level
    // settles in that one look and costs nothing after it.
    wakeAllFluids(*voxels);
    voxels->shipped = voxels->grid;
}

} // namespace

void clearScene(World& world)
{
    const core::InstanceId workspace = workspaceOf(world);
    if (!workspace.valid())
        return;

    // Collected first. `destroy` unlinks as it goes, so walking and destroying
    // in one pass drops the rest of the list. The storages are the scene's
    // too (ADR 0080).
    std::vector<core::InstanceId> containers{workspace};
    for (const auto& [name, service] : carriedServices(world))
        containers.push_back(service);
    std::vector<core::InstanceId> authored;
    const auto collect = [&](const auto& self, core::InstanceId container) -> void {
        for (core::InstanceId child = world.firstChild(container); child.valid(); child = world.nextSibling(child)) {
            // Not authored, so not a scene's to remove. A new scene is not a
            // reason to evict the ground a streaming system put there, or the
            // player somebody is.
            if (engineMade(world, child))
                continue;
            // The mount's node stays with its file; what is inside it goes.
            if (world.mounted(child)) {
                self(self, child);
                continue;
            }
            authored.push_back(child);
        }
    };
    for (const core::InstanceId container : containers)
        collect(collect, container);
    for (const core::InstanceId child : authored)
        (void)world.destroy(child);

    // **And every service's settings back to the engine's**, attributes with
    // them. A scene writes only the settings somebody changed, so one that says
    // nothing about `Lighting` means the engine's `Lighting` -- not whatever the
    // scene opened before it happened to leave behind.
    ServiceDefaults defaults(world);
    for (const auto& [name, service] : carriedServices(world)) {
        const ClassId classId = world.classOf(service);
        for (const ClassDescriptor* current = world.classes().find(classId); current != nullptr;
             current = world.classes().find(current->super)) {
            for (const PropertyDesc& property : current->properties) {
                if (!savedProperty(world, property))
                    continue;
                if (const std::optional<Value> engineDefault = defaults.of(classId, property);
                    engineDefault.has_value())
                    (void)property.set(world, service, *engineDefault);
            }
        }
        AttributeMap attributes;
        world.collectAttributes(service, attributes);
        for (const auto& entry : attributes)
            (void)world.setAttribute(service, entry.first, Value{});
    }
}

StampLibrary::StampLibrary(World& registriesFrom, StampSource source)
    : m_registries(registriesFrom), m_source(std::move(source))
{}

StampLibrary::~StampLibrary() = default;

const StampLibrary::Entry* StampLibrary::reference(const std::string& stamp)
{
    if (const auto found = m_built.find(stamp); found != m_built.end())
        return found->second == nullptr ? nullptr : found->second.get();

    // **A stamp that cannot be read is remembered as unreadable**, so a world
    // with forty instances of a deleted stamp asks the filesystem once.
    if (!m_source) {
        m_built.emplace(stamp, nullptr);
        return nullptr;
    }
    const std::optional<std::string> text = m_source(stamp);
    if (!text.has_value()) {
        m_built.emplace(stamp, nullptr);
        return nullptr;
    }

    auto entry = std::make_unique<Entry>();
    entry->world =
        std::make_unique<World>(m_registries.classes(), m_registries.enums(), m_registries.atoms(), kReferenceSeed);
    SceneIoReport ignored;
    // Unparented, because a reference tree is never looked at through a
    // hierarchy -- only walked from its root.
    entry->root = readStamp(*entry->world, *text, core::InstanceId{}, stamp, &ignored);
    if (!entry->root.valid()) {
        m_built.emplace(stamp, nullptr);
        return nullptr;
    }

    const Entry* raw = entry.get();
    m_built.emplace(stamp, std::move(entry));
    return raw;
}

std::string writeScene(const World& world, SceneIoReport* report, StampLibrary* stamps)
{
    SceneIoReport local;
    SceneIoReport& out = report != nullptr ? *report : local;

    JsonWriter writer;
    writer.beginObject();
    writer.field("format", kFormat);
    writer.field("version", kVersion);

    const core::InstanceId workspace = workspaceOf(world);
    if (workspace.valid()) {
        // Every root's paths before anything is written, so a part in the
        // world can name a template in storage and the other way round.
        std::unordered_map<core::u32, std::string> paths;
        collectPaths(world, workspace, {}, paths);
        std::vector<std::pair<std::string_view, core::InstanceId>> storages;
        ServiceDefaults defaults(world);
        for (const auto& [name, service] : carriedServices(world)) {
            if (holdsAuthored(world, service) || settingsChanged(world, service, defaults)) {
                collectPaths(world, service, {}, paths);
                storages.emplace_back(name, service);
            }
        }
        writer.key("root");
        writeInstance(writer, world, workspace, paths, out, core::InstanceId{}, stamps);
        // **Only when something is kept there, or set**, so a scene with empty
        // storages at the engine's settings is the byte-for-byte file it was
        // before they existed.
        if (!storages.empty()) {
            writer.key("storage");
            writer.beginObject();
            for (const auto& [name, service] : storages) {
                writer.key(name);
                writeInstance(writer, world, service, paths, out, core::InstanceId{}, stamps);
            }
            writer.endObject();
        }
    }
    writeVoxels(writer, world);

    writer.endObject();
    return writer.text();
}

namespace {

constexpr std::string_view kGlobalFormat = "global";
constexpr core::i64 kGlobalVersion = 1;

} // namespace

std::string writeGlobal(const World& world, SceneIoReport* report, StampLibrary* stamps)
{
    SceneIoReport local;
    SceneIoReport& out = report != nullptr ? *report : local;

    const core::InstanceId service = storageNamed(world, "GlobalScriptService");
    if (!service.valid())
        return {};
    AttributeMap attributes;
    world.collectAttributes(service, attributes);
    // Nothing authored and nothing set: no file, so a project that never used
    // it carries nothing for it.
    if (!holdsAuthored(world, service) && attributes.empty())
        return {};

    std::unordered_map<core::u32, std::string> paths;
    if (const core::InstanceId workspace = workspaceOf(world); workspace.valid())
        collectPaths(world, workspace, {}, paths);
    collectPaths(world, service, {}, paths);

    JsonWriter writer;
    writer.beginObject();
    writer.field("format", kGlobalFormat);
    writer.field("version", kGlobalVersion);
    writer.key("root");
    writeInstance(writer, world, service, paths, out, core::InstanceId{}, stamps);
    writer.endObject();
    return writer.text();
}

std::optional<core::EngineError> readGlobal(World& world, std::string_view json, SceneIoReport* report,
                                            const StampSource* source)
{
    SceneIoReport local;
    SceneIoReport& out = report != nullptr ? *report : local;

    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(json); !parsed.ok)
        return core::makeError(ENG_TR("scene.err.scene_parse"), {}, parsed.diagnostic);
    const JsonValue root = document.root();
    if (root["format"].asString() != kGlobalFormat)
        return core::makeError(ENG_TR("scene.err.scene_format"));
    if (root["version"].asInteger() != kGlobalVersion)
        return core::makeError(ENG_TR("scene.err.scene_version"));

    const core::InstanceId service = storageNamed(world, "GlobalScriptService");
    if (!service.valid())
        return std::nullopt;

    std::vector<PendingReference> pending;
    if (const JsonValue node = root["root"]; node.type() == core::JsonType::Object) {
        applyNode(world, service, node, pending, out);
        if (const JsonValue children = node["children"]; children.type() == core::JsonType::Array) {
            StampLoad load{source};
            for (core::usize index = 0; index < children.size(); ++index)
                (void)readInstance(world, service, children.at(index), pending, out, &load, 0);
        }
    }

    const core::InstanceId workspace = workspaceOf(world);
    for (const PendingReference& reference : pending) {
        const std::string_view first = std::string_view(reference.path).substr(0, reference.path.find('.'));
        const core::InstanceId pathRoot = first == world.atoms().text(world.name(service)) ? service : workspace;
        const core::InstanceId target = resolvePath(world, pathRoot, reference.path);
        if (!target.valid()) {
            ++out.droppedReferences;
            continue;
        }
        if (reference.isAttribute)
            (void)world.setAttribute(reference.owner, reference.property, Value{target});
        else
            (void)world.setProperty(reference.owner, reference.property, Value{target});
    }
    return std::nullopt;
}

namespace {

// The half of reading a scene that needs no world: parsed, and its format and
// version checked.
[[nodiscard]] std::optional<core::EngineError> parseSceneText(core::JsonDocument& document, std::string_view json)
{
    if (const core::JsonDocument::ParseResult parsed = document.parse(json); !parsed.ok)
        return core::makeError(ENG_TR("scene.err.scene_parse"), {}, parsed.diagnostic);
    const JsonValue root = document.root();
    if (root["format"].asString() != kFormat)
        return core::makeError(ENG_TR("scene.err.scene_format"));
    if (!readableVersion(root["version"].asInteger()))
        return core::makeError(ENG_TR("scene.err.scene_version"));
    return std::nullopt;
}

std::optional<core::EngineError> applyScene(World& world, const JsonValue root, SceneIoReport& out,
                                            const StampSource& stamps);

} // namespace

std::unique_ptr<ParsedScene> parseScene(std::string text)
{
    auto parsed = std::make_unique<ParsedScene>();
    parsed->text = std::move(text);
    parsed->error = parseSceneText(parsed->document, parsed->text);
    return parsed;
}

std::vector<std::string> sceneContent(const ParsedScene& parsed)
{
    std::vector<std::string> names;
    if (parsed.error.has_value())
        return names;
    // A walk over the text's values rather than a list of which properties
    // name content: any property that holds a name is one to warm.
    std::vector<JsonValue> stack{parsed.document.root()};
    while (!stack.empty()) {
        const JsonValue value = stack.back();
        stack.pop_back();
        switch (value.type()) {
        case core::JsonType::String:
            if (const std::string_view text = value.asString(); text.starts_with("asset://")) {
                if (std::find(names.begin(), names.end(), text) == names.end())
                    names.emplace_back(text);
            }
            break;
        case core::JsonType::Array:
        case core::JsonType::Object:
            // Pushed backwards, so the walk meets them in the file's order.
            for (core::usize index = value.size(); index > 0; --index)
                stack.push_back(value.type() == core::JsonType::Array ? value.at(index - 1)
                                                                      : value[value.keyAt(index - 1)]);
            break;
        default:
            break;
        }
    }
    return names;
}

std::optional<core::EngineError> readScene(World& world, const ParsedScene& parsed, SceneIoReport* report,
                                           const StampSource& stamps)
{
    if (parsed.error.has_value())
        return parsed.error;
    SceneIoReport local;
    return applyScene(world, parsed.document.root(), report != nullptr ? *report : local, stamps);
}

std::optional<core::EngineError> readScene(World& world, std::string_view json, SceneIoReport* report,
                                           const StampSource& stamps)
{
    core::JsonDocument document;
    if (std::optional<core::EngineError> error = parseSceneText(document, json); error.has_value())
        return error;
    SceneIoReport local;
    return applyScene(world, document.root(), report != nullptr ? *report : local, stamps);
}

namespace {

std::optional<core::EngineError> applyScene(World& world, const JsonValue root, SceneIoReport& out,
                                            const StampSource& stamps)
{
    StampLoad load{&stamps};

    const ReadingVersion reading(root["version"].asInteger());

    const core::InstanceId workspace = workspaceOf(world);
    if (!workspace.valid())
        return core::makeError(ENG_TR("scene.err.scene_no_workspace"));

    // Replacing, not merging: a scene IS the world's contents, and a load that
    // merged would double everything the second time it ran.
    clearScene(world);
    readVoxels(world, root, out);

    std::vector<PendingReference> pending;
    if (const JsonValue rootNode = root["root"]; rootNode.type() == core::JsonType::Object) {
        // The file's root IS the workspace, so its own properties apply to the
        // workspace and only its children are created.
        applyNode(world, workspace, rootNode, pending, out);
        if (const JsonValue children = rootNode["children"]; children.type() == core::JsonType::Array) {
            for (core::usize index = 0; index < children.size(); ++index)
                (void)readInstance(world, workspace, children.at(index), pending, out, &load, 0);
        }
    }

    // The storages (ADR 0080), each into this world's own service of that
    // class. One this build does not have is skipped, as an unknown class is.
    std::vector<std::pair<std::string, core::InstanceId>> roots{
        {std::string(world.atoms().text(world.name(workspace))), workspace}};
    if (const JsonValue storage = root["storage"]; storage.type() == core::JsonType::Object) {
        // In the file's order, and whichever services it names: one this build
        // does not have, or one a scene does not carry, is skipped.
        for (core::usize entry = 0; entry < storage.size(); ++entry) {
            const std::string_view name = storage.keyAt(entry);
            const JsonValue node = storage[name];
            // **A scene written before ADR 0105** kept its player code in
            // `ScriptService`, which is `ClientScriptService` now: read into it,
            // so the scene keeps working, and saved under the new name.
            const core::InstanceId service =
                storageNamed(world, name == "ScriptService" ? "ClientScriptService" : name);
            if (node.type() != core::JsonType::Object || !service.valid() || !carriedService(world, service))
                continue;
            applyNode(world, service, node, pending, out);
            if (const JsonValue children = node["children"]; children.type() == core::JsonType::Array) {
                for (core::usize index = 0; index < children.size(); ++index)
                    (void)readInstance(world, service, children.at(index), pending, out, &load, 0);
            }
            roots.emplace_back(std::string(world.atoms().text(world.name(service))), service);
            // A reference written as `ScriptService.X` still finds X.
            if (name == "ScriptService")
                roots.emplace_back(std::string(name), service);
        }
    }

    for (const PendingReference& reference : pending) {
        // A path's first segment names its root: the world, or a storage.
        const std::string_view first = std::string_view(reference.path).substr(0, reference.path.find('.'));
        core::InstanceId pathRoot = workspace;
        for (const auto& [name, id] : roots) {
            if (name == first)
                pathRoot = id;
        }
        const core::InstanceId target = resolvePath(world, pathRoot, reference.path);
        if (!target.valid()) {
            ++out.droppedReferences;
            continue;
        }
        if (reference.isAttribute)
            (void)world.setAttribute(reference.owner, reference.property, Value{target});
        else
            (void)world.setProperty(reference.owner, reference.property, Value{target});
    }

    return std::nullopt;
}

} // namespace

core::InstanceId readSceneNode(World& world, std::string_view nodeJson, core::InstanceId parent, SceneIoReport* report,
                               const StampSource& stamps)
{
    SceneIoReport local;
    SceneIoReport& out = report != nullptr ? *report : local;

    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(nodeJson); !parsed.ok)
        return {};

    const JsonValue node = document.root();
    if (node.type() != core::JsonType::Object)
        return {};

    std::vector<PendingReference> pending;
    StampLoad load{&stamps};
    const core::InstanceId built = readInstance(world, parent, node, pending, out, &load, 0);
    if (!built.valid())
        return {};

    // Resolved against the NODE rather than against a scene root, because that
    // is what this subtree has: a path leaving it names something the caller's
    // document holds and this world does not, and it is dropped with a count
    // exactly as a scene drops one leaving the scene.
    for (const PendingReference& reference : pending) {
        const core::InstanceId target = resolvePath(world, built, reference.path);
        if (!target.valid()) {
            ++out.droppedReferences;
            continue;
        }
        if (reference.isAttribute)
            (void)world.setAttribute(reference.owner, reference.property, Value{target});
        else
            (void)world.setProperty(reference.owner, reference.property, Value{target});
    }
    return built;
}

std::string writeStamp(const World& world, core::InstanceId root, SceneIoReport* report, StampLibrary* stamps)
{
    SceneIoReport local;
    SceneIoReport& out = report != nullptr ? *report : local;

    JsonWriter writer;
    writer.beginObject();
    writer.field("format", kFormat);
    writer.field("version", kVersion);

    if (world.alive(root)) {
        std::unordered_map<core::u32, std::string> paths;
        collectPaths(world, root, {}, paths);
        writer.key("root");
        // `root`'s own mark is IGNORED, because this is the file that mark
        // points at: a stamp made from an instance of itself would otherwise
        // write a one-line file referring to the file being written.
        writeInstance(writer, world, root, paths, out, root, stamps);
    }

    writer.endObject();
    return writer.text();
}

std::string writeCopy(const World& world, core::InstanceId root, SceneIoReport* report)
{
    struct Full
    {
        Full() { t_mountedInFull = true; }
        ~Full() { t_mountedInFull = false; }
        Full(const Full&) = delete;
        Full& operator=(const Full&) = delete;
    } full;
    return writeStamp(world, root, report, nullptr);
}

std::string normalizeStampPath(std::string_view typed)
{
    constexpr std::string_view StampExtension = ".stamp.json";
    std::string path(typed);

    // The same normalisation a scene path gets, for the same reason: the box is
    // labelled `content/`, so typing the prefix is the natural thing to do and
    // the wrong thing to keep (D068).
    for (char& c : path) {
        if (c == '\\')
            c = '/';
    }
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    constexpr std::string_view kContentPrefix = "content/";
    while (path.compare(0, kContentPrefix.size(), kContentPrefix) == 0)
        path.erase(0, kContentPrefix.size());

    if (path.empty())
        return path;

    // **A bare name lands in `content/stamps/`**, and a name with a folder in it
    // is taken at its word. A default that a person can step outside of, which
    // is what makes it a convention rather than a rule.
    //
    // A whole file name is a path too, one at the content's root: that is what
    // the browser hands over for a stamp kept there, which was "not there any
    // more" when it was sent to `stamps/` (B12).
    const bool named = path.size() >= StampExtension.size() &&
                       path.compare(path.size() - StampExtension.size(), StampExtension.size(), StampExtension) == 0;
    if (path.find('/') == std::string::npos && !named)
        path = std::string("stamps") + "/" + path;

    if (path.size() < StampExtension.size() ||
        path.compare(path.size() - StampExtension.size(), StampExtension.size(), StampExtension) != 0) {
        path += StampExtension;
    }
    return path;
}

core::InstanceId readStamp(World& world, std::string_view json, core::InstanceId parent, std::string_view stamp,
                           SceneIoReport* report)
{
    SceneIoReport local;
    SceneIoReport& out = report != nullptr ? *report : local;

    // The one-file case: the text is in hand, so the source it is read through
    // answers for this stamp and nothing else. A stamp naming another stamp is
    // refused at authoring time and would be a hand-edited file here.
    const StampSource source = [json, stamp](std::string_view wanted) -> std::optional<std::string> {
        return wanted == stamp ? std::optional<std::string>(std::string(json)) : std::nullopt;
    };
    StampLoad load{&source};
    return placeStamp(world, parent, stamp, out, &load, 0);
}

namespace {

// The position of `id` under `root`, child index by child index; empty for the
// root. A restamp rebuilds a subtree of the same SHAPE, so a position names the
// same instance before and after, where an id does not.
[[nodiscard]] std::vector<core::usize> positionUnder(const World& world, core::InstanceId root, core::InstanceId id)
{
    std::vector<core::usize> path;
    for (core::InstanceId walk = id; walk.valid() && walk != root; walk = world.parentOf(walk)) {
        core::usize index = 0;
        for (core::InstanceId sibling = world.firstChild(world.parentOf(walk)); sibling.valid() && sibling != walk;
             sibling = world.nextSibling(sibling))
            ++index;
        path.insert(path.begin(), index);
    }
    return path;
}

[[nodiscard]] core::InstanceId atPosition(const World& world, core::InstanceId root,
                                          const std::vector<core::usize>& path)
{
    core::InstanceId at = root;
    for (const core::usize index : path) {
        core::InstanceId child = world.firstChild(at);
        for (core::usize step = 0; step < index && child.valid(); ++step)
            child = world.nextSibling(child);
        if (!child.valid())
            return {};
        at = child;
    }
    return at;
}

// **A reference a restamp has to put back** (B5): one held BY an instance the
// rebuild replaces, or held TO one. Each end is a position under the stamped
// instance when it is inside it, and an id when it is not -- an id outside
// survives the rebuild and a position inside names the new instance there.
struct Repoint
{
    bool ownerInside = false;
    core::InstanceId owner;
    std::vector<core::usize> ownerAt;
    core::NameAtom property;
    bool valueInside = false;
    core::InstanceId value;
    std::vector<core::usize> valueAt;
};

// Every instance-valued property in `world` that crosses into the part of
// `target` a rebuild replaces -- its descendants, not itself. From inside, only
// what this instance changed from `reference`: what it did not change takes the
// file's new value, which is the point of a restamp.
void collectRepoints(const World& world, core::InstanceId target, const World& reference,
                     core::InstanceId referenceRoot, std::vector<Repoint>& out)
{
    const auto inside = [&](core::InstanceId id) { return id != target && world.isAncestorOf(target, id); };
    std::vector<core::InstanceId> everyone;
    for (core::InstanceId top = world.firstChild(core::InstanceId{}); top.valid(); top = world.nextSibling(top)) {
        everyone.push_back(top);
        world.collectDescendants(top, everyone);
    }
    for (const core::InstanceId owner : everyone) {
        const bool ownerInside = inside(owner);
        for (ClassId cls = world.classOf(owner); cls != InvalidClass;) {
            const ClassDescriptor* descriptor = world.classes().find(cls);
            if (descriptor == nullptr)
                break;
            for (const PropertyDesc& property : descriptor->properties) {
                if (property.type != ValueType::Instance || !savedProperty(world, property))
                    continue;
                const std::optional<Value> held = property.get(world, owner);
                const core::InstanceId* value = held.has_value() ? std::get_if<core::InstanceId>(&*held) : nullptr;
                if (value == nullptr || !value->valid())
                    continue;
                const bool valueInside = inside(*value);
                if (!ownerInside && !valueInside)
                    continue;
                if (ownerInside) {
                    const core::InstanceId refOwner =
                        atPosition(reference, referenceRoot, positionUnder(world, target, owner));
                    if (refOwner.valid() && !differsFromReference(world, property, world.atoms().text(property.name),
                                                                  reference, refOwner, target, referenceRoot, *held))
                        continue;
                }
                Repoint entry;
                entry.ownerInside = ownerInside;
                entry.owner = owner;
                if (ownerInside)
                    entry.ownerAt = positionUnder(world, target, owner);
                entry.property = property.name;
                entry.valueInside = valueInside;
                entry.value = *value;
                if (valueInside)
                    entry.valueAt = positionUnder(world, target, *value);
                out.push_back(std::move(entry));
            }
            cls = descriptor->super;
        }
    }
}

} // namespace

core::u32 restamp(World& world, core::InstanceId root, std::string_view stamp, std::string_view before,
                  std::string_view after, SceneIoReport* report)
{
    SceneIoReport local;
    SceneIoReport& out = report != nullptr ? *report : local;

    const core::NameAtom mark = world.atoms().lookup(stamp);
    if (!mark.valid() || !world.alive(root))
        return 0;

    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(after); !parsed.ok)
        return 0;
    const JsonValue file = document.root();
    const JsonValue rootNode = file["root"];
    if (file["format"].asString() != kFormat || !readableVersion(file["version"].asInteger()) ||
        rootNode.type() != core::JsonType::Object)
        return 0;
    const ReadingVersion reading(file["version"].asInteger());

    // **The file as the live instances were built from it**, in a world of its
    // own. "What has this one got of its own" is a question about two trees and
    // there is no cheaper honest way to ask it -- the same argument the writer
    // makes, one save earlier.
    World reference(world.classes(), world.enums(), world.atoms(), kReferenceSeed);
    SceneIoReport ignored;
    const core::InstanceId referenceRoot = readStamp(reference, before, {}, stamp, &ignored);
    if (!referenceRoot.valid())
        return 0;

    const ClassId rootClass = world.classes().findId(world.atoms().intern(rootNode["class"].asString()));
    if (rootClass == InvalidClass)
        return 0;

    // Collected before anything is touched: the walk and the rebuild cannot be
    // one pass, because the rebuild replaces the children the walk is standing
    // in.
    std::vector<core::InstanceId> subtree;
    subtree.push_back(root);
    world.collectDescendants(root, subtree);

    const StampSource source = [after, stamp](std::string_view wanted) -> std::optional<std::string> {
        return wanted == stamp ? std::optional<std::string>(std::string(after)) : std::nullopt;
    };

    core::u32 refreshed = 0;
    for (const core::InstanceId target : subtree) {
        if (!world.alive(target) || world.stampOf(target) != mark)
            continue;

        // **Not an instance of that stamp any more**, so it is left exactly as
        // it is and counted. Somebody added a child to this one, or deleted
        // one, and rebuilding it from the file would throw that away -- which is
        // the same rule the writer applies for the same reason.
        if (world.classOf(target) != rootClass || !sameShape(world, target, reference, referenceRoot)) {
            ++out.unlinkedStamps;
            continue;
        }

        // What it has of its own, measured against the file it came from and
        // kept as text: the tree it was measured on is about to stop existing.
        std::unordered_map<core::u32, std::string> paths;
        JsonWriter kept;
        kept.beginObject();
        bool anyOverride = false;
        SceneIoReport measured;
        collectOverrides(kept, anyOverride, world, target, reference, referenceRoot, target, referenceRoot, paths,
                         measured);
        if (anyOverride)
            kept.endObject();
        kept.endObject();

        core::JsonDocument overrides;
        const bool hasOverrides = anyOverride && overrides.parse(kept.text()).ok;
        // And the references the text above cannot carry: they name instances
        // by id, and the ids inside are about to be new ones.
        std::vector<Repoint> repoints;
        collectRepoints(world, target, reference, referenceRoot, repoints);

        // **The instance itself survives**: its id, its parent, its place among
        // its siblings and every reference anything else holds to it. Destroying
        // and rebuilding it would move it to the end of its parent, and an
        // Explorer whose rows jump every time somebody saves is one nobody
        // trusts.
        std::vector<core::InstanceId> children;
        world.collectChildren(target, children);
        for (const core::InstanceId child : children)
            (void)world.destroy(child);

        std::vector<PendingReference> pending;
        applyNode(world, target, rootNode, pending, out);
        StampLoad load{&source};
        if (const JsonValue nodes = rootNode["children"]; nodes.type() == core::JsonType::Array) {
            for (core::usize index = 0; index < nodes.size(); ++index)
                (void)readInstance(world, target, nodes.at(index), pending, out, &load, 1);
        }
        if (hasOverrides)
            applyOverrides(world, target, overrides.root()["overrides"], pending, out);

        // Inside the stamp, against the stamp's own root -- the rule
        // `placeStamp` states and for the same reason: a path inside a stamp
        // names something inside that stamp.
        for (const PendingReference& entry : pending) {
            const core::InstanceId found = resolvePath(world, target, entry.path);
            if (!found.valid()) {
                ++out.droppedReferences;
                continue;
            }
            if (entry.isAttribute)
                (void)world.setAttribute(entry.owner, entry.property, Value{found});
            else
                (void)world.setProperty(entry.owner, entry.property, Value{found});
        }
        for (const Repoint& entry : repoints) {
            const core::InstanceId owner = entry.ownerInside ? atPosition(world, target, entry.ownerAt) : entry.owner;
            const core::InstanceId value = entry.valueInside ? atPosition(world, target, entry.valueAt) : entry.value;
            if (!owner.valid() || !world.alive(owner) || !value.valid()) {
                ++out.droppedReferences;
                continue;
            }
            (void)world.setProperty(owner, entry.property, Value{value});
        }

        ++out.stamped;
        ++refreshed;
    }

    return refreshed;
}

namespace {

// Where an instance sits inside the stamp it came from: the stamp's root in the
// LIVE world, and the instance that corresponds to it in the stamp's own tree.
//
// Shared by the two questions a panel asks -- which properties are overridden,
// and what the stamp says one of them should be -- because a second copy of this
// walk is a second chance to pair the wrong instances.
struct ReferenceSite
{
    const World* world = nullptr;
    core::InstanceId id;
    core::InstanceId referenceRoot;
    core::InstanceId stampRoot;

    [[nodiscard]] bool found() const noexcept { return world != nullptr && id.valid(); }
};

[[nodiscard]] ReferenceSite locateInStamp(const World& world, core::InstanceId id, StampLibrary& stamps)
{
    if (!world.alive(id))
        return {};

    // **Up to the nearest stamped ancestor, including `id` itself.** A person
    // selects the part inside the lamp post as readily as the lamp post, and
    // both questions are the same one measured from the same root.
    core::InstanceId stampRoot = id;
    core::NameAtom mark{};
    while (stampRoot.valid()) {
        mark = world.stampOf(stampRoot);
        if (mark.valid())
            break;
        stampRoot = world.parentOf(stampRoot);
    }
    if (!stampRoot.valid() || !mark.valid())
        return {};

    const StampLibrary::Entry* entry = stamps.reference(std::string(world.atoms().text(mark)));
    if (entry == nullptr || entry->world == nullptr || !entry->root.valid())
        return {};

    // The child indices from the stamp root down to `id`, then the same walk
    // from the reference's root. Indices rather than names, because two siblings
    // may share a name and the save pairs them positionally too.
    std::vector<core::u32> descent;
    for (core::InstanceId step = id; step != stampRoot; step = world.parentOf(step)) {
        const core::InstanceId parent = world.parentOf(step);
        if (!parent.valid())
            return {};
        core::u32 index = 0;
        core::InstanceId child = world.firstChild(parent);
        while (child.valid() && child != step) {
            child = world.nextSibling(child);
            ++index;
        }
        if (!child.valid())
            return {};
        descent.push_back(index);
    }

    const World& reference = *entry->world;
    core::InstanceId refId = entry->root;
    for (auto step = descent.rbegin(); step != descent.rend(); ++step) {
        core::InstanceId child = reference.firstChild(refId);
        for (core::u32 skipped = 0; skipped < *step && child.valid(); ++skipped)
            child = reference.nextSibling(child);
        if (!child.valid())
            return {};
        refId = child;
    }

    // A different class is a different instance, not an instance with every
    // property overridden.
    if (world.classOf(id) != reference.classOf(refId))
        return {};

    return ReferenceSite{&reference, refId, entry->root, stampRoot};
}

} // namespace

std::optional<Value> stampReferenceValue(const World& world, core::InstanceId id, core::NameAtom property,
                                         StampLibrary& stamps)
{
    const ReferenceSite site = locateInStamp(world, id, stamps);
    if (!site.found())
        return std::nullopt;

    const std::string_view name = world.atoms().text(property);
    const core::NameAtom referenceAtom = site.world->atoms().lookup(name);
    if (!referenceAtom.valid())
        return std::nullopt;
    const PropertyDesc* referenceProperty =
        site.world->classes().findProperty(site.world->classOf(site.id), referenceAtom);
    if (referenceProperty == nullptr || referenceProperty->get == nullptr)
        return std::nullopt;
    return referenceProperty->get(*site.world, site.id);
}

std::vector<core::NameAtom> stampOverrides(const World& world, core::InstanceId id, StampLibrary& stamps)
{
    if (!world.alive(id))
        return {};

    const ReferenceSite site = locateInStamp(world, id, stamps);
    if (!site.found())
        return {};

    const World& reference = *site.world;
    const core::InstanceId refId = site.id;
    const core::InstanceId stampRoot = site.stampRoot;

    std::vector<core::NameAtom> overridden;
    const ClassDescriptor* descriptor = world.classes().find(world.classOf(id));
    for (const ClassDescriptor* current = descriptor; current != nullptr;
         current = world.classes().find(current->super)) {
        for (const PropertyDesc& property : current->properties) {
            // What the save writes, and nothing else: a transient property is
            // the running game's, and marking it overridden offered to revert
            // or apply something no file will ever hold (B15).
            if (!savedProperty(world, property))
                continue;
            const std::string_view name = world.atoms().text(property.name);
            const std::optional<Value> mine = property.get(world, id);
            if (!mine.has_value())
                continue;
            if (differsFromReference(world, property, name, reference, refId, stampRoot, site.referenceRoot, *mine))
                overridden.push_back(property.name);
        }
    }
    return overridden;
}

} // namespace engine::scene
