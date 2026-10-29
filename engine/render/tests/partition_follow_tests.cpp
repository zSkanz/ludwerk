// A scene partitioned with a piece worn in it (ADR 0201).
//
// Here and not beside the partitioner's own tests because `MeshPart` and its
// `PoseFrom` are this module's: the partitioner decides what a node can be by
// building it, against whatever registry it is handed, and the scene module's
// alone has no `MeshPart` to build.
#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <vector>

#include "../../scene/generated/class_descriptors.gen.h"
#include "../generated/class_descriptors.gen.h"
#include "engine/asset/chunk.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/partition.h"
#include "engine/scene/scene_file.h"
#include "engine/scene/streaming_glue.h"
#include "engine/scene/world.h"

using namespace engine;

namespace {

[[nodiscard]] std::string frame(double x, double y, double z)
{
    return "[" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z) + ",1,0,0,0,1,0,0,0,1]";
}

[[nodiscard]] std::string mesh(const char* name, double x, const std::string& more = {})
{
    return std::string(R"({"class":"MeshPart","name":")") + name + R"(","properties":{"CFrame":)" + frame(x, 1.0, 4.0) +
           R"(,"Size":[2,2,2],"Anchored":true)" + more + "}}";
}

} // namespace

TEST_CASE("a part that points at something stays in the scene with what it points at")
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::generated::registerEnums(enums, atoms);
    scene::generated::registerClasses(classes, atoms);
    render::generated::registerClasses(classes, atoms);
    scene::World world{classes, enums, atoms, 7u};

    // A hood worn on a body: a mesh with nothing under it, which is what the
    // grid takes -- and whose `PoseFrom` is a path. A cell's record has a
    // place, a size and a colour and nowhere to put a reference, so the hood
    // came back from its cell following nobody: a second head at rest where
    // the file put it. What points stays, as what is pointed at does.
    const std::string text =
        R"({"format":"scene","version":2,"root":{"class":"Workspace","name":"Workspace","children":[)" +
        mesh("Body", 4.0) + "," + mesh("Hood", 4.0, R"(,"PoseFrom":"Workspace.Body")") + "," + mesh("Rock", 40.0) +
        "]}}";

    scene::PartitionResult result;
    const scene::PartitionSink sink = [](const asset::Chunk&) { return scene::PartitionCellWritten{}; };
    const std::optional<core::EngineError> error = scene::partitionScene(world, text, {}, {}, sink, result);
    REQUIRE(!error.has_value());

    // The rock goes into a cell; the two that are tied by a path do not.
    CHECK(result.report.records == 1);
    CHECK(result.report.pinned == 2);
    CHECK(result.scene.find("\"Rock\"") == std::string::npos);
    CHECK(result.scene.find("\"Body\"") != std::string::npos);
    CHECK(result.scene.find("\"Hood\"") != std::string::npos);
    CHECK(result.scene.find("\"PoseFrom\":\"Workspace.Body\"") != std::string::npos);
}

TEST_CASE("a resized mesh comes back from its cell the size it was")
{
    // `MeshPart.MeshSize` is the divisor `Size` is drawn and collided over, and
    // the record had no field for it: a house whose mesh measures 7 m, sized
    // 7 m, came back over a mesh of one and was drawn seven times its height.
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::generated::registerEnums(enums, atoms);
    scene::generated::registerClasses(classes, atoms);
    render::generated::registerClasses(classes, atoms);
    scene::World world{classes, enums, atoms, 7u};

    const std::string text =
        R"({"format":"scene","version":2,"root":{"class":"Workspace","name":"Workspace","children":[)" +
        mesh("House", 40.0, R"(,"MeshSize":[0.5,1,0.25])") + "]}}";
    asset::Chunk written;
    scene::PartitionResult result;
    const scene::PartitionSink sink = [&written](const asset::Chunk& cell) {
        written = cell;
        return scene::PartitionCellWritten{};
    };
    REQUIRE(!scene::partitionScene(world, text, {}, {}, sink, result).has_value());
    REQUIRE(result.report.records == 1);

    asset::Chunk cell;
    REQUIRE_FALSE(asset::decodeChunk(asset::encodeChunk(written), cell).has_value());
    REQUIRE(cell.instances.size() == 1);
    CHECK(cell.instances[0].meshSize == core::Vec3{0.5f, 1.0f, 0.25f});

    scene::World streamed{classes, enums, atoms, 9u};
    const core::InstanceId workspace = streamed.create(classes.findId(atoms.lookup("Workspace")));
    streamed.setName(workspace, atoms.intern("Workspace"));
    // (The residual first: a record is born under what it was authored under.)
    REQUIRE(!scene::readScene(streamed, result.scene).has_value());
    scene::StreamingGlue glue(streamed, workspace);
    (void)glue.materialize(cell.id, cell);
    std::vector<core::InstanceId> descendants;
    streamed.collectDescendants(workspace, descendants);
    const scene::MeshPartComponent* house = nullptr;
    const scene::PartComponent* part = nullptr;
    for (const core::InstanceId id : descendants) {
        if (streamed.atoms().text(streamed.name(id)) == "House") {
            house = streamed.meshParts().find(id);
            part = streamed.parts().find(id);
        }
    }
    REQUIRE(house != nullptr);
    REQUIRE(part != nullptr);
    CHECK(house->meshSize == core::Vec3{0.5f, 1.0f, 0.25f});
    CHECK(part->size == core::Vec3{2.0f, 2.0f, 2.0f});
}
