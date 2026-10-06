// The cut-over, end to end (E9 step 14).
//
// **The claim under test is that a loose `.gltf` no longer feeds the runtime**,
// and the sharp way to assert it is from the other side: compile a project, then
// DELETE the source files, and require that everything still draws. A test that
// only checked "the mesh loaded" would pass on a build where the loose reader
// was still there, because the loose reader loaded meshes perfectly well -- that
// was the problem with it.
//
// This is also where `import_matches_build` lives, the last of E9's declared
// verification. The editor's import and a command-line build go through one
// function by construction (`assetc::importOne`), which is a structural argument
// rather than an asserted fact -- so it is asserted here, per URN and per blob.
#include "engine/app/content_import.h"
#include "engine/asset/content.h"
#include "engine/asset/material.h"
#include "engine/core/i18n.h"
#include "engine/platform/file.h"
#include "engine/render/mesh_cache.h"
#include "engine/render/mesh_loader.h"
#include "engine/rhi/backends.h"
#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/enum_registry.h"
#include "engine/scene/world.h"
#include "project_fixture.h"

#if ENG_DEBUG_UI
#include "engine/assetc/compiler.h"
#endif

#include <algorithm>
#include <chrono>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace engine;

namespace {

// The REAL catalog, not a fixture -- so a key this path raises and en.json does
// not carry is a failure here rather than a `[i18n:missing:...]` line nobody
// reads. `render.err.mesh_not_compiled` is new with the cut-over and is raised
// by the third case below.
void seedRealCatalog()
{
    const auto result = core::engineCatalog().loadFromFile(ENG_TEST_CATALOG);
    REQUIRE_MESSAGE(result.ok, result.diagnostic);
}

// A scratch project: `content/models/quad.gltf` and `content/textures/base.png`,
// copied from `asset`'s own fixtures so this needs no checked-in binary of its
// own and no generator that could disagree with the real files.
struct Project
{
    std::filesystem::path root;

    Project()
    {
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec) / "engine-content-import-tests";
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root / "content" / "models", ec);
        std::filesystem::create_directories(root / "content" / "textures", ec);
        std::filesystem::copy_file(ENG_TEST_MESH, mesh(), std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::copy_file(ENG_TEST_IMAGE, image(), std::filesystem::copy_options::overwrite_existing, ec);
    }

    ~Project()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;

    [[nodiscard]] std::filesystem::path content() const { return root / "content"; }
    [[nodiscard]] std::filesystem::path mesh() const { return content() / "models" / "quad.gltf"; }
    [[nodiscard]] std::filesystem::path image() const { return content() / "textures" / "base.png"; }

    // **What makes this test what it is.** Everything the compiler read is
    // removed, so anything that still resolves came out of the store.
    void deleteSources() const
    {
        std::error_code ec;
        std::filesystem::remove_all(content(), ec);
    }
};

struct Registries
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    scene::ClassId meshPartClass = scene::InvalidClass;
    scene::ClassId workspaceClass = scene::InvalidClass;

    Registries()
    {
        meshPartClass = classes.registerClass({
            .name = atoms.intern("MeshPart"),
            .defaultName = atoms.intern("MeshPart"),
        });
        workspaceClass = classes.registerClass({
            .name = atoms.intern("Workspace"),
            .defaultName = atoms.intern("Workspace"),
        });
    }
};

} // namespace

TEST_CASE("a project compiles when it is opened, and then needs none of its sources")
{
    if (!ENG_DEBUG_UI) {
        MESSAGE("ENG_TEST_SKIP: this build carries no compiler, so a project cannot compile itself");
        return;
    }

    seedRealCatalog();

    const Project project;
    REQUIRE(std::filesystem::exists(project.mesh()));
    REQUIRE(std::filesystem::exists(project.image()));

    asset::ContentMounts mounts;
    const app::ContentImportReport report = app::openProjectContent(project.root, project.content(), mounts);

    // **It did work**, rather than finding a cache from a previous run: the
    // scratch tree is new, so every source is a miss.
    CHECK(report.failed.empty());
    CHECK(report.cacheMisses > 0);
    CHECK(report.meshes > 0);

    // Two mounts: the source tree, and the store above it.
    CHECK(mounts.mountCount() == 2);

    const asset::ResolvedContent mesh = mounts.resolve("asset://models/quad.gltf");
    REQUIRE(mesh.found());
    // `Source::Pack`, which is what a store resolves as -- so the loader takes
    // its compiled branch and there is no second answer for it to prefer.
    CHECK(mesh.source == asset::ResolvedContent::Source::Pack);
    CHECK(mesh.kind == asset::AssetKind::Mesh);

    const asset::ResolvedContent image = mounts.resolve("asset://textures/base.png");
    REQUIRE(image.found());
    CHECK(image.source == asset::ResolvedContent::Source::Pack);
    CHECK(image.kind == asset::AssetKind::Texture);

    // **And now the sources go away.** A store holds its blobs as files it owns,
    // so what resolves after this came out of the store and could have come from
    // nowhere else.
    project.deleteSources();

    asset::ContentMounts afterwards;
    (void)app::openProjectContent(project.root, project.content(), afterwards);
    // One mount, not two: there is no content directory left to mount, and the
    // store still stands.
    CHECK(afterwards.mountCount() == 1);
    CHECK(afterwards.resolve("asset://models/quad.gltf").found());
    CHECK(afterwards.resolve("asset://textures/base.png").found());
}

TEST_CASE("D507: opening a project again reads none of its unchanged sources, and says what it compiles")
{
    // Every open asked the compiler about every source -- read it, hash it,
    // find it cached, write the store's index again -- and a project of two
    // hundred sources spent 3.8 s on that before its first frame, with nothing
    // to compile. And a first open said nothing while it compiled for minutes.
    if (!ENG_DEBUG_UI) {
        MESSAGE("ENG_TEST_SKIP: this build carries no compiler, so a project cannot compile itself");
        return;
    }

    seedRealCatalog();
    const Project project;

    std::vector<std::string> told;
    const app::ImportProgress progress = [&told](core::usize done, core::usize total, std::string_view name) {
        if (done < total && (told.empty() || told.back() != name))
            told.emplace_back(name);
    };

    asset::ContentMounts first;
    const app::ContentImportReport compiled = app::openProjectContent(project.root, project.content(), first, progress);
    CHECK(compiled.failed.empty());
    CHECK(compiled.compiled.size() == 2);
    // Each source named before it compiled.
    CHECK(told == std::vector<std::string>{"models/quad.gltf", "textures/base.png"});
    CHECK(std::filesystem::is_regular_file(app::importSourcesPath(project.root)));

    // **Again, nothing changed: nothing compiled, nothing asked about**, and
    // the store still answers.
    told.clear();
    asset::ContentMounts second;
    const app::ContentImportReport again = app::openProjectContent(project.root, project.content(), second, progress);
    CHECK(again.compiled.empty());
    CHECK(again.cacheHits == 0);
    CHECK(again.cacheMisses == 0);
    CHECK(told.empty());
    CHECK(second.resolve("asset://models/quad.gltf").found());
    CHECK(second.resolve("asset://textures/base.png").found());

    // **A source that changed is compiled again**, and only it.
    {
        std::error_code ec;
        const auto when = std::filesystem::last_write_time(project.image(), ec);
        REQUIRE_FALSE(ec);
        std::filesystem::last_write_time(project.image(), when + std::chrono::seconds(5), ec);
        REQUIRE_FALSE(ec);
    }
    asset::ContentMounts third;
    const app::ContentImportReport changed = app::openProjectContent(project.root, project.content(), third);
    CHECK(changed.compiled == std::vector<std::string>{"textures/base.png"});
}

TEST_CASE("D554: a source the store has lost is compiled again, whatever the stamps remember")
{
    // Two processes opened one project at once -- two lanes of the gate, on
    // one example -- and each merged what it compiled into the store's index
    // from the index it had read: the second to write left out what the first
    // had added. Both had stamped both sources as compiled. From then on the
    // project opened with a mesh that "has no compiled form" and was never
    // compiled again: the stamp said it was done.
    if (!ENG_DEBUG_UI) {
        MESSAGE("ENG_TEST_SKIP: this build carries no compiler, so a project cannot compile itself");
        return;
    }

    seedRealCatalog();
    const Project project;
    asset::ContentMounts first;
    REQUIRE(app::openProjectContent(project.root, project.content(), first).compiled.size() == 2);

    // The index as the process that lost the race left it: nothing of the
    // mesh in it, the texture still there.
    const std::filesystem::path index = project.root / ".engine" / "import" / "index.json";
    std::string text;
    REQUIRE(engine::platform::readTextFile(index, text));
    const std::size_t entry = text.find("{\"urn\":\"asset://models/quad.gltf\"");
    REQUIRE(entry != std::string::npos);
    const std::size_t end = text.find('}', entry);
    REQUIRE(end != std::string::npos);
    // The entry and the comma that joined it to the next.
    std::size_t stop = end + 1;
    if (stop < text.size() && text[stop] == ',')
        ++stop;
    text.erase(entry, stop - entry);
    {
        std::ofstream out(index, std::ios::binary | std::ios::trunc);
        out << text;
    }

    // Opened again: the mesh is compiled again, and only the mesh.
    asset::ContentMounts second;
    const app::ContentImportReport again = app::openProjectContent(project.root, project.content(), second);
    CHECK(again.failed.empty());
    CHECK(again.compiled == std::vector<std::string>{"models/quad.gltf"});
    CHECK(second.resolve("asset://models/quad.gltf").found());
    CHECK(second.resolve("asset://textures/base.png").found());

    // And once more: nothing to do.
    asset::ContentMounts third;
    CHECK(app::openProjectContent(project.root, project.content(), third).compiled.empty());
}

TEST_CASE("D523: an import with no window says what it compiles while it does, every few seconds")
{
    // A first open headless compiled for a minute and a half without a word,
    // and a slow import looked like a stopped process.
    seedRealCatalog();
    engine::app::testing::Captured log;
    app::ImportLog quiet(5'000'000'000ull);
    const core::u64 second = 1'000'000'000ull;
    quiet.report(0, 200, "models/a.glb", 10 * second);
    CHECK(log.contains("200 source(s) to compile"));
    const auto progress = [&log] {
        int count = 0;
        for (const std::string& line : log.lines)
            count += line.find("Compiling the project's content") != std::string::npos ? 1 : 0;
        return count;
    };
    // Not again within the interval; again once it has passed, with where it is.
    quiet.report(1, 200, "models/b.glb", 12 * second);
    CHECK(progress() == 0);
    quiet.report(40, 200, "textures/wall.png", 16 * second);
    CHECK(progress() == 1);
    CHECK(log.contains("40 of 200 done, now textures/wall.png"));
    // Done is said by the totals, not by this.
    quiet.report(200, 200, "", 99 * second);
    CHECK(progress() == 1);
}

TEST_CASE("the loader draws a compiled mesh and a compiled map, with no source file left")
{
    if (!ENG_DEBUG_UI) {
        MESSAGE("ENG_TEST_SKIP: this build carries no compiler, so a project cannot compile itself");
        return;
    }

    seedRealCatalog();

    const Project project;
    asset::ContentMounts mounts;
    const app::ContentImportReport report = app::openProjectContent(project.root, project.content(), mounts);
    REQUIRE(report.failed.empty());

    // **The whole point of the case.** From here on there is no `.gltf` and no
    // `.png` anywhere on disk, so a loader that still had a loose reader would
    // load nothing and this would fail.
    project.deleteSources();

    Registries registries;
    scene::World world(registries.classes, registries.enums, registries.atoms, 1234u);
    const core::InstanceId workspace = world.create(registries.workspaceClass);

    const core::InstanceId part = world.create(registries.meshPartClass);
    world.parts().add(part, scene::PartComponent{});
    scene::MeshPartComponent meshPart;
    meshPart.meshContent = registries.atoms.intern("asset://models/quad.gltf");
    world.meshParts().add(part, meshPart);
    (void)world.setParent(part, workspace);

    // The part wears a material that names the map (ADR 0090); the loader
    // loads the maps of what the parts wear.
    asset::MaterialLibrary materials;
    asset::MaterialAsset base;
    base.properties.colorMap = "asset://textures/base.png";
    base.written = asset::AllMaterialFields;
    materials.put("asset://materials/base.material.json", base);
    world.setMaterialLibrary(&materials);
    world.parts().find(part)->material = registries.atoms.intern("asset://materials/base.material.json");

    rhi::DeviceResult device = rhi::createNullDevice({.backend = rhi::BackendId::Null});
    REQUIRE(device != nullptr);
    rhi::ICmdList* cmd = device->beginFrame();
    REQUIRE(cmd != nullptr);

    render::MeshCache cache;
    render::MeshLibrary library;
    render::TextureLibrary textures;
    render::MeshLoader loader;
    loader.setContentMounts(&mounts);
    loader.setDeferredTextures(false);

    CHECK(loader.sync(*device, *cmd, world, workspace, cache, library) == 1u);
    CHECK(library.find(meshPart.meshContent) != nullptr);

    // **The map came out of the store, transcoded rather than decoded**, which
    // is the branch that makes "BC7 and mips reach editor content" true. It was
    // being produced before this and read by nobody: `syncTextures` went to the
    // raw PNG beside it every time.
    CHECK(loader.syncTextures(*device, *cmd, world, textures) == 1u);
    CHECK(textures.find(registries.atoms.intern("asset://textures/base.png")).valid());

    loader.destroy(*device);
    cache.destroy(*device);
}

TEST_CASE("D564: a compiled map is transcoded off the frame that asks for it")
{
    if (!ENG_DEBUG_UI) {
        MESSAGE("ENG_TEST_SKIP: this build carries no compiler, so a project cannot compile itself");
        return;
    }

    seedRealCatalog();

    const Project project;
    asset::ContentMounts mounts;
    const app::ContentImportReport report = app::openProjectContent(project.root, project.content(), mounts);
    REQUIRE(report.failed.empty());

    Registries registries;
    scene::World world(registries.classes, registries.enums, registries.atoms, 1234u);
    const core::InstanceId workspace = world.create(registries.workspaceClass);
    const core::InstanceId part = world.create(registries.meshPartClass);
    world.parts().add(part, scene::PartComponent{});
    (void)world.setParent(part, workspace);

    asset::MaterialLibrary materials;
    asset::MaterialAsset base;
    base.properties.colorMap = "asset://textures/base.png";
    base.written = asset::AllMaterialFields;
    materials.put("asset://materials/base.material.json", base);
    world.setMaterialLibrary(&materials);
    world.parts().find(part)->material = registries.atoms.intern("asset://materials/base.material.json");

    rhi::DeviceResult device = rhi::createNullDevice({.backend = rhi::BackendId::Null});
    REQUIRE(device != nullptr);
    rhi::ICmdList* cmd = device->beginFrame();
    REQUIRE(cmd != nullptr);

    render::TextureLibrary textures;
    render::MeshLoader loader;
    loader.setContentMounts(&mounts);
    // As a window's loader is: what can wait for a later frame does.
    loader.setDeferredTextures(true);

    // The frame that first draws with the map pays for none of it -- a
    // transcode is ten milliseconds and more of a frame, and was done here --
    // and the map is on its way.
    const core::NameAtom map = registries.atoms.intern("asset://textures/base.png");
    CHECK(loader.syncTextures(*device, *cmd, world, textures) == 0u);
    CHECK_FALSE(textures.find(map).valid());
    CHECK(loader.texturesInFlight() == 1u);

    // A frame or a few later it is there, once, and nothing is left in flight.
    core::u32 loaded = 0;
    for (int frame = 0; frame < 20000 && !textures.find(map).valid(); ++frame) {
        loaded += loader.syncTextures(*device, *cmd, world, textures);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(textures.find(map).valid());
    CHECK(loaded == 1u);
    CHECK(loader.texturesInFlight() == 0u);
    CHECK(loader.syncTextures(*device, *cmd, world, textures) == 0u);

    loader.destroy(*device);
}

TEST_CASE("a mesh with no compiled form draws nothing, rather than parsing a source file")
{
    seedRealCatalog();

    const Project project;

    // Mounted WITHOUT compiling: the source tree is there and the store is not,
    // which is precisely the state the loose feed used to serve.
    asset::ContentMounts mounts;
    mounts.mountDirectory(project.content());
    REQUIRE(mounts.resolve("asset://models/quad.gltf").found());
    // Found, and found as a LOOSE file -- so this is not a test about a missing
    // asset. The file is right there, and the runtime declines to read it.
    CHECK(mounts.resolve("asset://models/quad.gltf").source == asset::ResolvedContent::Source::Loose);

    Registries registries;
    scene::World world(registries.classes, registries.enums, registries.atoms, 1234u);
    const core::InstanceId workspace = world.create(registries.workspaceClass);
    const core::InstanceId part = world.create(registries.meshPartClass);
    world.parts().add(part, scene::PartComponent{});
    scene::MeshPartComponent meshPart;
    meshPart.meshContent = registries.atoms.intern("asset://models/quad.gltf");
    world.meshParts().add(part, meshPart);
    (void)world.setParent(part, workspace);

    rhi::DeviceResult device = rhi::createNullDevice({.backend = rhi::BackendId::Null});
    REQUIRE(device != nullptr);
    rhi::ICmdList* cmd = device->beginFrame();
    REQUIRE(cmd != nullptr);

    render::MeshCache cache;
    render::MeshLibrary library;
    render::MeshLoader loader;
    loader.setContentMounts(&mounts);

    CHECK(loader.sync(*device, *cmd, world, workspace, cache, library) == 0u);
    CHECK(library.find(meshPart.meshContent) == nullptr);

    // And it is remembered, so the refusal costs one attempt rather than one
    // per frame for ever.
    CHECK(loader.sync(*device, *cmd, world, workspace, cache, library) == 0u);

    loader.destroy(*device);
    cache.destroy(*device);
}

#if ENG_DEBUG_UI

TEST_CASE("import_matches_build: the editor's import and a command-line build agree, blob for blob")
{
    // E9's declared verification, and the reason it is worth asserting even
    // though `importOne` makes it structural: two entry points that agree by
    // inspection stop agreeing the first time one of them is changed.
    seedRealCatalog();

    const Project project;

    asset::ContentMounts mounts;
    const app::ContentImportReport report = app::openProjectContent(project.root, project.content(), mounts);
    REQUIRE(report.failed.empty());

    assetc::CompileOptions options;
    options.inputRoot = project.content();
    // No cache: the command-line side must produce these bytes from the sources,
    // not read back what the import just wrote.
    const assetc::CompileResult built = assetc::compile(options);
    REQUIRE_MESSAGE(built.ok, built.diagnostic);
    REQUIRE_FALSE(built.entries.empty());

    // **Per URN and per blob.** The manifest says which hash a URN has; the
    // store answers with the bytes under that hash. Both have to match, because
    // a matching hash over bytes nobody stored is a store that fails at first
    // use.
    for (const assetc::ManifestEntry& entry : built.entries) {
        const asset::ResolvedContent found = mounts.resolve(entry.urn);
        INFO("urn: ", entry.urn);
        REQUIRE(found.found());
        CHECK(found.source == asset::ResolvedContent::Source::Pack);
        CHECK(found.hash == entry.hash);
        CHECK(found.kind == entry.kind);

        const std::span<const std::byte> blob = mounts.blob(entry.hash);
        CHECK_FALSE(blob.empty());
        CHECK(blob.size() == entry.storedBytes);
    }
}

#endif

// --- The materials of an imported model (ADR 0090) ----------------------------

TEST_CASE("an imported model's materials become assets its pieces can wear, and a re-import keeps edits")
{
    std::error_code ec;
    const std::filesystem::path content =
        std::filesystem::temp_directory_path(ec) / "engine-import-materials" / "content";
    std::filesystem::remove_all(content.parent_path(), ec);
    std::filesystem::create_directories(content / "models", ec);
    const std::filesystem::path data = std::filesystem::path(ENG_TEST_MESH).parent_path();
    std::filesystem::copy_file(data / "textured.gltf", content / "models" / "textured.gltf",
                               std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::copy_file(data / "checker.png", content / "models" / "checker.png",
                               std::filesystem::copy_options::overwrite_existing, ec);

    const app::ModelMaterials first = app::writeModelMaterials(content, "models/textured.gltf");
    // `Plain` samples `checker.png` beside the model and is written; `Bumpy`
    // glows with a data URI, which no material can name, and is left in the
    // mesh.
    REQUIRE(first.written.size() == 1);
    CHECK(first.written[0] == "materials/textured/plain.material.json");
    std::string text;
    REQUIRE(platform::readTextFile(content / first.written[0], text));
    const std::optional<asset::MaterialAsset> plain = asset::readMaterialAsset(text);
    REQUIRE(plain.has_value());
    CHECK(plain->properties.colorMap == "asset://models/checker.png");
    // glTF's own defaults where the file says nothing.
    CHECK(plain->properties.metalness == 1.0f);
    CHECK(plain->properties.roughness == 1.0f);

    // Some submesh wears it and some keeps the file's own.
    CHECK(std::find(first.bySubmesh.begin(), first.bySubmesh.end(), first.written[0]) != first.bySubmesh.end());
    CHECK(std::find(first.bySubmesh.begin(), first.bySubmesh.end(), std::string{}) != first.bySubmesh.end());

    // Somebody edits it; a re-import leaves it alone.
    REQUIRE(platform::writeTextFile(content / first.written[0], "edited"));
    const app::ModelMaterials second = app::writeModelMaterials(content, "models/textured.gltf");
    CHECK(second.written.empty());
    REQUIRE(platform::readTextFile(content / first.written[0], text));
    CHECK(text == "edited");
    std::filesystem::remove_all(content.parent_path(), ec);
}
