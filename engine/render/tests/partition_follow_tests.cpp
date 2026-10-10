// A scene partitioned with a piece worn in it (ADR 0201, D626).
//
// Here and not beside the partitioner's own tests because `MeshPart` and its
// `PoseFrom` are this module's: the partitioner decides what a node can be by
// building it, against whatever registry it is handed, and the scene module's
// alone has no `MeshPart` to build.
#include <doctest/doctest.h>
#include <optional>
#include <string>

#include "../../scene/generated/class_descriptors.gen.h"
#include "../generated/class_descriptors.gen.h"
#include "engine/asset/chunk.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/partition.h"
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

TEST_CASE("D626: a part that points at something stays in the scene with what it points at")
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
