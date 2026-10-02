#include "engine/app/world_host.h"

#if ENG_ENABLE_REPLICATION
#include "engine/replication/extract.h"
#include "engine/replication/script_templates.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <sstream>

#include "class_descriptors.gen.h"
#include "engine/asset/gltf.h"
#include "engine/asset/mesh_format.h"
#include "engine/audio/scene_types.h"
#include "engine/core/build_info.h"
#include "engine/core/content_path.h"
#include "engine/core/json.h"
#include "engine/core/log.h"
#include "engine/input/scene_types.h"
#include "engine/jobs/jobs.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"
#include "engine/render/debug_draw.h"
#include "engine/render/scene_types.h"
#include "engine/scene/players.h"
#include "engine/scene/sprite_animation.h"
#include "engine/scene/voxel_fluid.h"
#include "engine/script/bytecode.h"
#include "engine/script/instance_binding.h"
#include "engine/script/net_module.h"
#include "engine/script/remote.h"
#include "engine/script/scenes.h"
#include "engine/ui/scene_types.h"

namespace engine::app {
namespace {

using core::I18nArg;
using core::LogLevel;

// The `@engine/…` modules that ship as content. One entry per module rather than
// a directory walk, because the set is the engine's own surface (ADR 0030) and
// discovering it from a directory would make an accidentally-shipped file part
// of the API.
constexpr std::string_view RuntimeModules[] = {"camera", "ragdoll", "testing", "views"};

// The conformance runner, as an ordinary entry script.
//
// Luau in a C++ string is not a thing to do lightly, and there are two reasons
// it is right here. It has to run FROM Luau: a case body may `task.wait`, and a
// `lua_call` from a C function cannot be resumed across a yield (U-34) -- so a
// C-side runner could only run suites that never wait, which is most of what
// there is to test. And it has to be an entry script rather than host
// machinery, because everything it does -- `game.Loaded`, attributes,
// `Shutdown` -- is something a project could do, and a runner with a private
// channel into the host would be testing a world no game will ever run in.
// The conformance runner is a real file staged with the rest of the runtime
// content, not a string literal here. It was written as a literal first, and
// the escaping alone made it unreadable -- as a file it is analysed by the gate
// and formatted like everything else.
constexpr std::string_view ConformanceRunnerPath = "runtime/conformance/runner.luau";

[[nodiscard]] bool readFile(const std::filesystem::path& path, std::string& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;

    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

// Project-relative paths use '/' whatever the platform does, because they are
// the cache key `require` compares and a key that reads `a\b` on one machine and
// `a/b` on another is two modules where there is one.
[[nodiscard]] std::string toProjectPath(const std::filesystem::path& path)
{
    std::string text = path.generic_string();
    return text;
}

[[nodiscard]] std::string_view directoryOf(std::string_view path)
{
    const std::string::size_type slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

// Resolves `.` and `..` without touching the filesystem, so a specifier cannot
// escape the project root by spelling enough `..`s and so the answer does not
// depend on what happens to exist -- by the one check every path from outside
// the engine goes through (audit F5): a backslash, a drive or a share is not a
// module name.
[[nodiscard]] bool normalisePath(std::string_view input, std::string& out)
{
    std::optional<std::string> safe = core::safeRelativePath(input);
    if (!safe.has_value())
        return false;
    out = std::move(*safe);
    return true;
}

// `init.luauc` is `init.luau` compiled (ADR 0112): mounted, named and
// required by its source's name, so a packaged game's chunk names -- what an
// error in a player's log says -- are the ones its author wrote.
[[nodiscard]] std::filesystem::path sourceNameOf(const std::filesystem::path& file)
{
    if (file.extension() != script::CompiledExtension)
        return file;
    std::filesystem::path renamed = file;
    renamed.replace_extension(".luau");
    return renamed;
}

// Every `.luau` file under `folder`, as entries for `container` (ADR 0105), or
// its compiled `.luauc`.
void collectScriptFiles(const std::filesystem::path& root, const std::filesystem::path& folder, std::string container,
                        bool module, std::vector<script::MountedScript>& entries)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec))
        return;
    // **`increment(ec)`, not the range-for.** The error-code CONSTRUCTOR only
    // makes the first step non-throwing; `operator++` still throws, so a folder
    // holding a junction or a directory this process cannot enter would throw
    // `filesystem_error` out of boot -- and a process that dies during mount
    // dies with no message about what it was mounting.
    for (std::filesystem::recursive_directory_iterator it(folder, ec), end; it != end && !ec; it.increment(ec)) {
        const std::filesystem::directory_entry& entry = *it;
        if (!entry.is_regular_file(ec) ||
            (entry.path().extension() != ".luau" && entry.path().extension() != script::CompiledExtension))
            continue;
        std::string source;
        if (!readFile(entry.path(), source))
            continue;
        const std::filesystem::path named = sourceNameOf(entry.path());
        entries.push_back(script::MountedScript{
            .path = toProjectPath(std::filesystem::relative(named, root, ec)),
            // Relative to its folder, so `src/client/enemy/patrol.luau` mounts
            // as `Client/enemy/patrol` rather than dragging the two directory
            // levels that got it there into the tree.
            .mountPath = toProjectPath(std::filesystem::relative(named, folder, ec)),
            .source = std::move(source),
            .container = container,
            .module = module,
        });
    }
}

// `scenes/arena.scene.json` is `arena`: the folder under `src/scenes/` whose
// `server/` and `client/` are that scene's code (ADR 0105).
[[nodiscard]] std::string sceneFolderName(const std::filesystem::path& scene)
{
    std::string name = scene.filename().string();
    constexpr std::string_view Suffix = ".scene.json";
    if (name.size() > Suffix.size() && name.ends_with(Suffix))
        name.resize(name.size() - Suffix.size());
    else
        name = scene.stem().string();
    return name;
}

} // namespace

// The `ModuleLoader` vtable, as a struct so its two functions can name the host
// without the host having to expose them.
struct WorldHostLoader
{
    static bool resolve(void* user, std::string_view fromPath, std::string_view specifier, std::string& outPath)
    {
        auto& host = *static_cast<WorldHost*>(user);
        if (host.m_root.empty() || specifier.empty())
            return false;

        std::string candidate;
        if (specifier.front() == '@') {
            // `@self` is the requiring file's own directory, and an alias is
            // whatever `.luaurc` said. Resolved here rather than through
            // `Luau::parseConfig` because that treats an unrecognised key as a
            // hard error that aborts the require (U-42) -- a `$schema` line
            // would break `require` at runtime.
            const std::string::size_type slash = specifier.find('/');
            const std::string_view head = specifier.substr(1, slash == std::string_view::npos ? slash : slash - 1);
            const std::string_view tail =
                slash == std::string_view::npos ? std::string_view{} : specifier.substr(slash + 1);

            if (head == "self") {
                candidate.assign(directoryOf(fromPath));
            }
            else {
                const auto alias = host.m_aliases.find(std::string(head));
                if (alias == host.m_aliases.end())
                    return false;
                candidate = alias->second;
            }

            if (!tail.empty()) {
                if (!candidate.empty())
                    candidate.push_back('/');
                candidate.append(tail);
            }
        }
        else if (specifier.starts_with("./") || specifier.starts_with("../")) {
            candidate.assign(directoryOf(fromPath));
            if (!candidate.empty())
                candidate.push_back('/');
            candidate.append(specifier);
        }
        else {
            // A bare specifier is project-root relative. Deliberately not a
            // search path: one place to look means one answer, and an ambiguity
            // a search path would resolve silently is a bug worth an error.
            candidate.assign(specifier);
        }

        std::string normalised;
        if (!normalisePath(candidate, normalised))
            return false;

        // The extension is added rather than required, and `init.luau` is the
        // directory form. Both are tried in a fixed order so the answer never
        // depends on which file was created first -- each as source, then
        // compiled (ADR 0112), under the source's name either way.
        const auto present = [&](const std::string& path) {
            return std::filesystem::is_regular_file(host.m_root / path) ||
                   std::filesystem::is_regular_file(host.m_root / (path + "c"));
        };
        const std::string withExtension = normalised.ends_with(".luau") ? normalised : normalised + ".luau";
        if (present(withExtension)) {
            outPath = withExtension;
            return true;
        }
        // A module outside `src/shared` says so in its name (`Tool.module.luau`)
        // and is required as `Tool` all the same.
        if (!normalised.ends_with(".luau")) {
            const std::string asModule = normalised + ".module.luau";
            if (present(asModule)) {
                outPath = asModule;
                return true;
            }
        }

        const std::string asDirectory = normalised + "/init.luau";
        if (present(asDirectory)) {
            outPath = asDirectory;
            return true;
        }
        return false;
    }

    static bool read(void* user, std::string_view path, std::string& outSource)
    {
        auto& host = *static_cast<WorldHost*>(user);
        const std::filesystem::path file = host.m_root / std::filesystem::path(path);
        if (readFile(file, outSource))
            return true;
        // A packaged game's scripts are compiled beside where their source was.
        std::filesystem::path compiled = file;
        compiled.replace_extension(script::CompiledExtension);
        return path.ends_with(".luau") && readFile(compiled, outSource);
    }
};

WorldHost::WorldHost() = default;
// A scene read and parsed off the main thread (ADR 0125 §2). Shared between
// the host and the job, and never touched by the host until `done` says the
// job has let go of it.
struct WorldHost::PrepareTask
{
    std::string path;
    std::function<std::optional<std::string>(std::string_view)> read;
    std::filesystem::path contentRoot;
    std::unique_ptr<scene::ParsedScene> parsed;
    bool found = false;
    jobs::JobHandle handle;
    std::atomic<bool> done{false};

    void run() noexcept
    {
        std::string text;
        if (read) {
            if (std::optional<std::string> got = read(path); got.has_value())
                text = std::move(*got);
        }
        if (text.empty()) {
            if (const std::optional<std::filesystem::path> file = core::resolveUnder(contentRoot, path))
                (void)platform::readTextFile(*file, text);
        }
        found = !text.empty();
        if (found)
            parsed = scene::parseScene(std::move(text));
        done.store(true, std::memory_order_release);
    }

    static void entry(void* user) noexcept { static_cast<PrepareTask*>(user)->run(); }
};

WorldHost::~WorldHost()
{
    dropPrepared();
}

void WorldHost::setContentMounts(const asset::ContentMounts* mounts)
{
    m_mounts = mounts;
    // The host's own materials read through the same mounts, compiled first
    // and loose second, so a material and the maps it names are found the
    // same way.
    if (mounts != nullptr)
        m_ownMaterials.setSource(asset::mountedMaterials(*mounts));
    else
        m_ownMaterials.setSource({});
}

void WorldHost::setMaterialLibrary(asset::MaterialLibrary* library) noexcept
{
    m_materials = library != nullptr ? library : &m_ownMaterials;
    if (m_world.has_value())
        m_world->setMaterialLibrary(m_materials);
}

std::optional<core::EngineError> WorldHost::boot(const WorldHostOptions& options)
{
    // The order is load-bearing, not incidental. `scene` owns the registry and
    // the root of the hierarchy, and a class registered by a higher module
    // names its parent's `ClassId` -- which exists only after the module that
    // owns the parent has run. So the calls go in layer order, lowest first,
    // the same order api/generator/gen_cpp.luau emits the files in
    // (architecture.md §2, rule 3: higher modules register INTO scene's
    // registries). `app` is the only place that sees every module, which is why
    // it is the only place this sequence can be written down.
    scene::generated::registerClasses(m_classes, m_atoms);
    render::registerSceneTypes(m_classes, m_atoms);
    input::registerSceneTypes(m_classes, m_atoms);
    audio::registerSceneTypes(m_classes, m_atoms);
    ui::registerSceneTypes(m_classes, m_atoms);

    // Enums have one owner and no hierarchy, so they are independent of the
    // above; they stay with `scene`, which holds the registry.
    scene::generated::registerEnums(m_enums, m_atoms);

    m_world.emplace(m_classes, m_enums, m_atoms, options.seed);
    m_world->setMaterialLibrary(m_materials);
    m_world->engineState().engineVersion = ENG_VERSION_STRING;
    m_world->engineState().luauVersion = ENG_LUAU_VERSION;
    m_world->engineState().networkTopology = options.networkTopology;
    // **Started to join** (`--join`, D433): the join is under way from the
    // first line any script runs. `State` reads `Connecting`, not `Offline`
    // -- "about to join" and "alone" looked the same -- and the scene's
    // client code waits for the server's world.
    if (options.networkTopology == scene::NetworkTopology::Replica) {
        m_world->engineState().networkState = 1;
        m_world->engineState().sceneClientHeld = true;
    }
    m_world->engineState().viewportSize = options.viewportSize;
    m_world->engineState().fixedTimestep = options.fixedTimestep;
    // Both, so a read before any write gives what the scheduler is running on
    // rather than the struct's default.
    m_world->engineState().requestedFixedTimestep = options.fixedTimestep;
    // Before the mount, which reads it: a sub-world mounts its scene's code
    // and none of the game's (ADR 0107 §3).
    m_world->engineState().subWorld = options.subWorld;
    m_world->engineState().maxSubWorlds = options.subWorld ? 0u : options.maxSubWorlds;
    m_seed = options.seed;

    m_saves = std::make_unique<script::SaveStore>(script::SaveStore::Options{
        .directory = options.saveDirectory,
        .maxSlotBytes = options.saveMaxSlotBytes,
        .maxSlots = options.saveMaxSlots,
    });
    m_developer = options.developer;
    m_sceneCloseGrace = options.sceneCloseGrace;
    m_scriptMemoryMb = options.scriptMemoryMb;
    m_prepareInBackground = !options.headless;
    m_warmContent = options.warmContent;
    m_warmedContent = options.warmedContent;
    m_runtime.emplace(*m_world);
    m_runtime->setSaveStore(m_saves.get());
    if (std::optional<core::EngineError> error = m_runtime->boot(); error.has_value())
        return error;
    script::setDeveloperWarnings(m_runtime->state(), m_developer);
    configureRuntime();

    // **The player at this machine, before any script runs** (N1). A script's
    // file scope reaches for `NetworkService.LocalPlayer`, so it has to be
    // there by the boot drain. Everybody but a dedicated server has one: solo
    // and a host are player 1, and a replica is numbered by its authority's
    // welcome, which has not arrived yet.
    if (options.networkTopology != scene::NetworkTopology::Dedicated) {
        const core::InstanceId network = scene::networkServiceOf(*m_world, m_runtime->dataModel());
        const bool replica = options.networkTopology == scene::NetworkTopology::Replica;
        (void)scene::createPlayer(*m_world, network, replica ? 0u : 1u, true);
    }

    m_runtime->setModuleLoader(script::ModuleLoader{
        .user = this,
        .resolve = &WorldHostLoader::resolve,
        .read = &WorldHostLoader::read,
    });

    // Before any script runs, because a script's file scope may call
    // `SaveState` and the bag it reaches has to be the host's by then.
    m_reloadState = options.reloadState;
    m_runtime->setReloadState(options.reloadState);
    if (options.reloadState != nullptr)
        options.reloadState->setIsReload(options.isReload);

    if (std::optional<core::EngineError> error = registerRuntimeModules(); error.has_value())
        return error;

    // Created by `registerServices` during the boot above, so this is a lookup
    // rather than a creation -- and it is cached because `extract` needs it
    // every frame.
    m_workspace = m_world->findFirstChildOfClass(m_runtime->dataModel(), m_classes.findId(m_atoms.lookup("Workspace")));
    // Cached for the same reason, and separately: `Lighting` is a sibling of
    // `Workspace` rather than a child of it, so `extract` cannot reach one from
    // the other.
    //
    // **This lookup is only a lookup because `Lighting` is a boot service.** It
    // was not, for the whole of M4: the service was created by its first
    // `GetService`, which is after this line, so the cache held an invalid id
    // for the life of the world and `extract` answered every frame with the
    // struct defaults. The renderer therefore drew a sun pinned straight up
    // over every scene, and the milestone's goldens recorded that faithfully.
    // `registerServices` now creates it at boot beside `Workspace`, which is
    // what makes the id correct by construction rather than by timing.
    //
    // An engine built without the render module registers no Lighting class at
    // all, and this stays invalid -- which `extract` reads as "no environment
    // state" and answers with the defaults, rather than as an error.
    m_lighting = m_world->findFirstChildOfClass(m_runtime->dataModel(), m_classes.findId(m_atoms.lookup("Lighting")));
    m_uiService = m_world->findFirstChildOfClass(m_runtime->dataModel(), m_classes.findId(m_atoms.lookup("UIService")));

    // The mixer opens a device on a windowed run and none on a headless one:
    // a headless run has no reason to hold one, and on a CI runner the attempt
    // costs a second and a log line nobody reads. The TIMELINE runs either way,
    // which is what makes `Ended` land on the same tick in both.
    (void)m_audio.start(options.headless);

    // Animation is created unconditionally, unlike the physics mirror: there is
    // no backend to be missing. A world whose meshes carry no skeleton simply
    // has an empty library, and every track it hands out plays nothing -- which
    // is the same answer a build with no render module gives.
    m_animation.emplace(*m_world, m_skeletons);
    m_runtime->setAnimation(&*m_animation);
    // The same object through the narrower seam that names joints, which is
    // what `Ragdoll:Build` reads a rig through.
    m_runtime->setSkeleton(&*m_animation);
    m_runtime->setInput(&m_input);

#if ENG_PHYSICS_JOLT
    // The one hand-written switch over what the build compiled in (ADR 0023),
    // the same shape the RHI backend is chosen with. A build with no physics
    // backend leaves the mirror null and every part stays where a script put
    // it -- which is what M0 through M4 were.
    m_backend = physics::createJoltPhysics();
    if (m_backend != nullptr) {
        m_physics.emplace(*m_world, *m_backend);
        // Handed over rather than looked up inside the mirror: `scene` has no
        // notion of the DataModel root. This assignment is the step M4.5's
        // lesson says to test -- an id resolved here and not there is exactly
        // how a renderer spent four milestones lighting scenes with defaults --
        // so `world_host_tests.cpp` asserts it on a world no script touched.
        m_physics->setWorkspace(m_workspace);
        // The same system that plays clips also owns the joints, and the mirror
        // is what knows when in the tick a pose may be committed.
        m_physics->setSkeleton(&*m_animation);
        // And the bindings, so `Workspace:Raycast` reads the same world the
        // tick steps rather than a second one.
        m_runtime->setPhysics(&*m_physics);
    }
#endif
    m_navigation = nav::createNavigation(*m_world);
    if (m_navigation != nullptr) {
        m_navigation->setWorkspace(m_workspace);
        m_runtime->setNavigation(m_navigation.get());
    }
#if ENG_PHYSICS_BOX2D
    m_backend2d = physics::createBox2DPhysics();
    if (m_backend2d != nullptr) {
        m_physics2d.emplace(*m_world, *m_backend2d);
        m_physics2d->setWorkspace(m_workspace);
        m_runtime->setPhysics2D(&*m_physics2d);
    }
#endif

    // The booting scene's name, for its own script folders (ADR 0105):
    // `scenes/arena.scene.json` mounts `src/scenes/arena/`.
    m_sceneName = sceneFolderName(options.bootScene);
    m_readContent = options.readContent;
    m_stamps = options.bootStamps;
#if ENG_ENABLE_REPLICATION
    m_scriptTemplates = std::make_shared<replication::ScriptTemplates>();
    m_scriptTemplates->setStampSource(m_stamps);
#endif
    m_partitionScene = options.partitionScene;
    m_resetStreaming = options.resetStreaming;
    m_world->engineState().currentScene = options.bootScenePath;
    m_world->engineState().defaultServer = options.defaultServer;
    if (!options.projectPath.empty()) {
        if (std::optional<core::EngineError> error = mountProject(options.projectPath); error.has_value())
            return error;
    }

    if (!options.conformanceRoot.empty()) {
        if (std::optional<core::EngineError> error = mountConformance(options.conformanceRoot); error.has_value())
            return error;
    }

    // After the mount, so the tree is complete, and before the scripts are
    // deferred, so the first thing any of them can observe already includes
    // what the previous world was carrying (M3 brief Decision 5).
    if (options.preserved != nullptr)
        restorePreserved(*m_world, m_runtime->dataModel(), *options.preserved, m_preserveReport);

    // **The authored world, before the behaviour that runs on it** (ADR 0047).
    // After the mount, so a scene that names a class a project's own code
    // registers finds it; after the preserved restore, so a reload's carried
    // instances are in the tree the scene replaces around them; and before the
    // scripts, which is the half D067 was.
    //
    // A scene that will not read is reported and the boot continues. The world
    // then holds whatever the scripts build, which is the same world every
    // project before `06-scene` has -- a run that refused to start because a
    // file was malformed would be a worse answer than a run that says so.
    // **The game's own contents before the scene's** (ADR 0105): no scene owns
    // them, and a scene's scripts may reach for them the moment they start.
    if (!options.bootGlobalText.empty()) {
        scene::SceneIoReport globalReport;
        if (const std::optional<core::EngineError> globalError = scene::readGlobal(
                *m_world, options.bootGlobalText, &globalReport, options.bootStamps ? &options.bootStamps : nullptr);
            globalError.has_value()) {
            core::logText(LogLevel::Error, globalError->message);
            m_globalUnreadable = true;
        }
    }

    if (!options.bootScene.empty()) {
        // **The grid gets the scene before the world does** (ADR 0053). What
        // comes back is what stayed authored; the rest is cells, and the
        // streaming host has been told about them by the same call.
        std::filesystem::path sceneFile = options.bootScene;
        if (options.partitionScene && options.bootSceneText.empty()) {
            if (std::filesystem::path partitioned = options.partitionScene(*m_world, options.bootScene);
                !partitioned.empty()) {
                sceneFile = std::move(partitioned);
            }
        }

        std::string sceneText = options.bootSceneText;
        if (sceneText.empty() && !readFile(sceneFile, sceneText)) {
            core::log(LogLevel::Warn, ENG_TR("scene.err.scene_unreadable"));
        }
        else if (const std::optional<core::EngineError> sceneError =
                     scene::readScene(*m_world, sceneText, &m_bootSceneReport, options.bootStamps);
                 sceneError.has_value()) {
            core::logText(LogLevel::Error, sceneError->message);
        }
        else {
            m_bootSceneApplied = true;
            const std::array<I18nArg, 1> args{I18nArg{"count", static_cast<core::i64>(m_bootSceneReport.instances)}};
            core::log(LogLevel::Info, ENG_TR("scene.info.scene_loaded"), args);
            // Said, because a stamp file that moved or went takes its
            // instances out of the game with nothing else to show for it (B4).
            if (m_bootSceneReport.missingStamps > 0) {
                const std::array<I18nArg, 1> missing{
                    I18nArg{"count", static_cast<core::i64>(m_bootSceneReport.missingStamps)}};
                core::log(LogLevel::Warn, ENG_TR("scene.warn.missing_stamps"), missing);
            }
        }
    }

    // The player at this machine was made before the scene brought its teams:
    // they join a side now, before any script asks which (ADR 0099).
    if (options.networkTopology != scene::NetworkTopology::Replica)
        scene::assignTeam(*m_world, scene::localPlayerOf(*m_world));

    // What the scene put under `Workspace`, counted before a line of script has
    // run -- see the warning after the drain.
    core::u32 authoredByScene = 0;
    // What each of them is, and which they are: a thing the scripts make that
    // the scene already holds by class and name is the one worth a warning.
    std::vector<std::pair<scene::ClassId, core::NameAtom>> heldByScene;
    std::vector<core::InstanceId> madeByScene;
    if (m_bootSceneApplied) {
        for (core::InstanceId child = m_world->firstChild(m_workspace); child.valid();
             child = m_world->nextSibling(child)) {
            if (!m_world->generated(child)) {
                ++authoredByScene;
                heldByScene.emplace_back(m_world->classOf(child), m_world->name(child));
                madeByScene.push_back(child);
            }
        }
    }

    // A project with no scripts boots, runs its frames and reports success
    // while doing absolutely nothing -- which is how `examples/01-instances`
    // came to render an empty screen with no diagnostic at all. Asked after the
    // scene, because the scene carries scripts too (ADR 0092); a warning rather
    // than an error, because an empty project is the user's mistake to make.
    if (m_projectIsDirectory && scriptCount() == 0 && !options.subWorld) {
        const std::array<I18nArg, 1> args{I18nArg{"path", m_root.string()}};
        core::log(LogLevel::Warn, ENG_TR("engine.project.warn.no_scripts"), args);
    }

    // **`Instance.stamp` reads from the same place a scene load does**, so a
    // prefab named in code and one named in a file mean the same file (ADR
    // 0051). A project with no source gets none, and the binding says so
    // rather than handing back an empty prefab.
    m_stampSource = options.bootStamps;
    m_runtime->setStampSource(options.bootStamps);

    // **Mounted above; started here, and only if this run is one that starts
    // them** (ADR 0058). The editor is the one caller that says no: a project
    // opened in a tool shows what its scene holds, and behaviour begins when
    // somebody presses play.
#if ENG_ENABLE_REPLICATION
    // **A replica starts no script its join is about to destroy** (S0.4, ADR
    // 0137 §5): what the authority replicates is cleared from this copy of the
    // scene before the first script starts, not after the boot drain -- a
    // script inside a replicated part ran its file scope once on a joined
    // client and then died with the part. `NetworkSession::start` clears again
    // when the socket opens, and finds nothing left to clear.
    if (options.networkTopology == scene::NetworkTopology::Replica)
        (void)replication::clearForReplica(*m_world, m_workspace, m_scriptTemplates.get());
#endif
    if (options.startScripts) {
        script::startScripts(m_runtime->state());
        warnScriptsInStorage();
    }

    // The boot drain. api-design.md §3's lifecycle reads "start each Script on
    // its own coroutine via `task.defer` … → first frame", and the arrow is
    // load-bearing: the scripts have had their first resumption *before* the
    // first frame renders, which is also what makes `game.Loaded` a boot event
    // rather than a first-tick one.
    //
    // It advances no clock. `SimTime` is still zero here, so a script reading it
    // at file scope sees the same zero a `Heartbeat` handler would see on tick
    // one -- and the first frame shows a world that has been built rather than
    // an empty one.
    m_runtime->drain(core::Phase::FrameStart);
    m_world->retireDestroyed();

    // **Two sources for one world, said out loud** (D074).
    //
    // A scene is what a project STARTS with and a script is what it then does
    // (ADR 0047). A project that has both a scene and entry scripts which build
    // a world has two answers to the same question, and the engine cannot merge
    // them -- so a character saved into the scene and a character the script
    // makes are two characters, which is exactly what a person sees the first
    // time they save a scene of a world their code built.
    //
    // Counted rather than guessed, and reported with both numbers, because the
    // useful form of this is "the file gave you 6 and the scripts added 5" and
    // not "something may be duplicated". The resolution is the project's and
    // there are only two: stop building that world in code -- which is what
    // ADR 0047 asks projects to become and what `examples/06-scene` shows -- or
    // do not give this project a scene.
    //
    // Not for a sub-world: its scene is what `Load()` names and cannot be
    // absent, so "do not give it a scene" is not advice it can take -- and a
    // small game whose scene is empty and whose code builds it is ordinary.
    //
    // **Only where something is there twice.** It fired for anything a script
    // parented to `Workspace` -- a camera of its own, an effect, a bullet --
    // which is what scripts are for, and a warning that fires for the ordinary
    // is one nobody reads the day it is true. What it is about is a thing the
    // scripts made that the scene already holds: the same class, the same
    // name.
    if (m_bootSceneApplied && !options.subWorld) {
        core::u32 twice = 0;
        for (core::InstanceId child = m_world->firstChild(m_workspace); child.valid();
             child = m_world->nextSibling(child)) {
            if (m_world->generated(child) ||
                std::find(madeByScene.begin(), madeByScene.end(), child) != madeByScene.end())
                continue;
            const std::pair<scene::ClassId, core::NameAtom> made{m_world->classOf(child), m_world->name(child)};
            if (std::find(heldByScene.begin(), heldByScene.end(), made) != heldByScene.end())
                ++twice;
        }
        if (twice > 0) {
            const std::array<I18nArg, 2> args{
                I18nArg{"scene", static_cast<core::i64>(authoredByScene)},
                I18nArg{"scripts", static_cast<core::i64>(twice)},
            };
            core::log(LogLevel::Warn, ENG_TR("scene.warn.two_sources"), args);
        }
    }

    return std::nullopt;
}

std::optional<core::EngineError> WorldHost::restartRuntime()
{
    if (!m_runtime.has_value() || !m_world.has_value())
        return std::nullopt;

    // A sub-world was started by the play that is ending, and goes with it.
    closeSubWorlds();

    // Read BEFORE the teardown, because both live in the VM that is about to go.
    const core::InstanceId dataModel = m_runtime->dataModel();
    std::vector<script::ModuleRegistry::Entry> mounted = script::mountedEntries(m_runtime->state());

    // Every connection, every required module, every queued resumption and every
    // timer goes with the reset below. That is the whole point of it.

    // What the ending run saved is on disk before its VM goes.
    flushSaves();
    m_runtime.reset();

    m_runtime.emplace(*m_world);
    m_runtime->setSaveStore(m_saves.get());
    if (std::optional<core::EngineError> error = m_runtime->boot(dataModel); error.has_value())
        return error;
    script::setDeveloperWarnings(m_runtime->state(), m_developer);
    configureRuntime();
    m_sceneClose.reset();
    dropPrepared();
    m_activationPending = false;

    m_runtime->setModuleLoader(script::ModuleLoader{
        .user = this,
        .resolve = &WorldHostLoader::resolve,
        .read = &WorldHostLoader::read,
    });
    m_runtime->setReloadState(m_reloadState);
    if (std::optional<core::EngineError> error = registerRuntimeModules(); error.has_value())
        return error;

    m_runtime->setStampSource(m_stampSource);
    script::adoptMountedEntries(m_runtime->state(), std::move(mounted));

    if (m_animation.has_value())
        m_runtime->setAnimation(&*m_animation);
    // The same object through the narrower seam that names joints, which is
    // what `Ragdoll:Build` reads a rig through.
    m_runtime->setSkeleton(&*m_animation);
    m_runtime->setInput(&m_input);
    if (m_physics.has_value())
        m_runtime->setPhysics(&*m_physics);
    if (m_physics2d.has_value())
        m_runtime->setPhysics2D(&*m_physics2d);
    if (m_navigation != nullptr)
        m_runtime->setNavigation(m_navigation.get());

    // **The services are the ones that were already there.** `registerServices`
    // adopted the DataModel, and everything under it is found rather than made,
    // so these three ids are the same ids -- asserted rather than assumed,
    // because a service the host cached and the VM rebuilt would be exactly the
    // M4 lighting defect again (see `boot`).
    m_workspace = m_world->findFirstChildOfClass(dataModel, m_classes.findId(m_atoms.lookup("Workspace")));
    m_lighting = m_world->findFirstChildOfClass(dataModel, m_classes.findId(m_atoms.lookup("Lighting")));
    m_uiService = m_world->findFirstChildOfClass(dataModel, m_classes.findId(m_atoms.lookup("UIService")));

    // Consumed rather than queued: a rebuild is not a thing the world did, and
    // there is nothing connected yet that could observe it.
    (void)m_world->changes().take();
    return std::nullopt;
}

std::optional<core::EngineError> WorldHost::registerRuntimeModules()
{
    const std::filesystem::path base = platform::paths().contentDir / "runtime" / "engine";
    for (const std::string_view name : RuntimeModules) {
        const std::filesystem::path path = base / std::filesystem::path(name) / "init.luau";
        std::string source;
        if (!readFile(path, source)) {
            const std::array<I18nArg, 1> args{I18nArg{"path", path.string()}};
            return core::makeError(ENG_TR("engine.cli.err.script_missing"), args);
        }
        script::registerModule(m_runtime->state(), std::string("@engine/").append(name), source);
    }
    return std::nullopt;
}

std::optional<core::EngineError> WorldHost::mountProject(const std::filesystem::path& path)
{
    std::error_code ec;
    const bool isDirectory = std::filesystem::is_directory(path, ec);

    std::vector<script::MountedScript> entries;
    if (isDirectory) {
        // A directory is a project root and gets the full mount (M2 brief,
        // Decision 9).
        m_root = path;

        std::string config;
        if (readFile(m_root / ".luaurc", config)) {
            core::JsonDocument document;
            if (document.parse(config, ".luaurc")) {
                const core::JsonValue aliases = document.root()["aliases"];
                for (core::usize index = 0; index < aliases.size(); ++index) {
                    const std::string_view key = aliases.keyAt(index);
                    m_aliases.emplace(std::string(key), std::string(aliases[key].asString()));
                }
            }
            else {
                // Reported rather than fatal: a project with a malformed
                // `.luaurc` should still boot far enough to say so, which is not
                // what the vendored config reader does (U-42).
                core::logText(LogLevel::Warn, document.parse(config, ".luaurc").diagnostic);
            }
        }

        // **Five folders, each into the service its code runs from** (ADR 0105):
        // the game's three under `GlobalScriptService`, and the booting scene's
        // two under the scene's own services. `src/scripts/` is the one before
        // it, read as `src/client/` for one release and said out loud.
        const auto mountFolder = [&](const std::filesystem::path& folder, std::string container, bool module) {
            collectScriptFiles(m_root, folder, std::move(container), module, entries);
        };
        const std::filesystem::path source = m_root / "src";
        const std::filesystem::path legacy = source / "scripts";
        // A sub-world is a scene played alone inside the game, not the game:
        // `GlobalScriptService` is the host's (ADR 0107 §3).
        if (!m_world->engineState().subWorld) {
            if (std::filesystem::is_directory(legacy, ec)) {
                core::log(LogLevel::Warn, ENG_TR("engine.boot.warn.src_scripts_moved"));
                mountFolder(legacy, "GlobalScriptService/Client", false);
            }
            mountFolder(source / "client", "GlobalScriptService/Client", false);
            mountFolder(source / "server", "GlobalScriptService/Server", false);
            mountFolder(source / "shared", "GlobalScriptService/Shared", true);
        }
        if (!m_sceneName.empty()) {
            const std::filesystem::path scene = source / "scenes" / m_sceneName;
            mountFolder(scene / "server", "ServerScriptService", false);
            mountFolder(scene / "client", "ClientScriptService", false);
        }
    }
    else {
        // A file is mounted as a single entry `Script`, which is what M0's and
        // M1's tests already do and will keep doing.
        std::string source;
        if (!readFile(path, source)) {
            const std::array<I18nArg, 1> args{I18nArg{"path", path.string()}};
            return core::makeError(ENG_TR("engine.cli.err.script_missing"), args);
        }

        m_root = path.parent_path();
        // Directly under `GlobalScriptService`, in none of its folders: a
        // single file is a whole game with no side, and runs on every machine
        // -- a dedicated server included -- as it did before ADR 0105.
        entries.push_back(script::MountedScript{
            .path = toProjectPath(path.filename()),
            .mountPath = toProjectPath(path.filename()),
            .source = std::move(source),
            .container = "GlobalScriptService",
            .module = false,
        });
    }

    // Whether a project with no scripts is worth a warning is decided after
    // the scene has loaded, because a scene carries scripts too (ADR 0092).
    m_projectIsDirectory = isDirectory;

    script::mountScripts(m_runtime->state(), entries);
    return std::nullopt;
}

core::InstanceId WorldHost::mountScriptFile(std::string_view relative)
{
    return mountScriptFile(std::string("src/client/").append(relative), "src/client", "GlobalScriptService/Client",
                           false);
}

core::InstanceId WorldHost::mountScriptFile(std::string_view relative, std::string_view root,
                                            std::string_view container, bool module)
{
    const std::filesystem::path scriptsRoot = m_root / std::filesystem::path(std::string(root));
    const std::filesystem::path file = m_root / std::filesystem::path(std::string(relative));
    std::string source;
    if (!readFile(file, source))
        return {};
    std::error_code ec;
    const std::array<script::MountedScript, 1> entry{script::MountedScript{
        .path = toProjectPath(std::filesystem::relative(file, m_root, ec)),
        .mountPath = toProjectPath(std::filesystem::relative(file, scriptsRoot, ec)),
        .source = std::move(source),
        .container = std::string(container),
        .module = module,
    }};
    const std::vector<core::InstanceId> made = script::mountScripts(m_runtime->state(), entry);
    return made.empty() ? core::InstanceId{} : made.front();
}

std::optional<core::EngineError> WorldHost::mountConformance(const std::filesystem::path& root)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        const std::array<I18nArg, 1> args{I18nArg{"path", root.string()}};
        return core::makeError(ENG_TR("engine.cli.err.script_missing"), args);
    }

    // The specs are mounted from their own root and the project's are not
    // touched: a conformance run is a run of the engine's own suite, and a
    // project's entry scripts have nothing to do with it.
    m_root = root;
    m_aliases.clear();

    std::vector<script::MountedScript> entries;
    // The same rule as the mount above, and for the same reason: an iterator
    // whose constructor took an error code still throws on the way forward.
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        const std::filesystem::directory_entry& entry = *it;
        if (!entry.is_regular_file(ec) || !entry.path().filename().string().ends_with(".spec.luau"))
            continue;
        std::string source;
        if (!readFile(entry.path(), source))
            continue;

        const std::string relative = toProjectPath(std::filesystem::relative(entry.path(), root, ec));
        entries.push_back(script::MountedScript{relative, relative, std::move(source)});
    }

    // The runner's own path sorts wherever it sorts, and it does not matter: it
    // hangs off `game.Loaded`, which is raised after every entry script has had
    // its first resumption whatever order they ran in.
    std::string runnerSource;
    const std::filesystem::path runnerPath = platform::paths().contentDir / ConformanceRunnerPath;
    if (!readFile(runnerPath, runnerSource)) {
        const std::array<I18nArg, 1> args{I18nArg{"path", runnerPath.string()}};
        return core::makeError(ENG_TR("engine.cli.err.script_missing"), args);
    }

    entries.push_back(script::MountedScript{
        "__conformance_runner.luau",
        "__conformance_runner.luau",
        std::move(runnerSource),
    });

    script::mountScripts(m_runtime->state(), entries);
    return std::nullopt;
}

void WorldHost::firePreReload()
{
    m_runtime->fireHotReload(true);
    m_runtime->drain(core::Phase::FrameStart);
    m_world->retireDestroyed();
}

void WorldHost::firePostReload()
{
    m_runtime->fireHotReload(false);
    m_runtime->drain(core::Phase::FrameStart);
    m_world->retireDestroyed();
}

core::u64 WorldHost::mountedScriptCount() const
{
    return static_cast<core::u64>(script::mountedScriptCount(m_runtime->state()));
}

core::u64 WorldHost::scriptCount() const
{
    const scene::ClassId scriptClass = m_world->classes().findId(m_world->atoms().lookup("Script"));
    if (scriptClass == scene::InvalidClass)
        return 0;
    std::vector<core::InstanceId> all;
    m_world->collectDescendants(m_runtime->dataModel(), all);
    return static_cast<core::u64>(std::count_if(
        all.begin(), all.end(), [&](core::InstanceId id) { return m_world->classOf(id) == scriptClass; }));
}

core::u64 WorldHost::scriptLoadFailures() const
{
    return static_cast<core::u64>(script::scriptLoadFailures(m_runtime->state()));
}

core::InstanceId WorldHost::dataModel() const noexcept
{
    return m_runtime.has_value() ? m_runtime->dataModel() : core::InstanceId{};
}

ConformanceReport WorldHost::conformanceReport() const
{
    const core::InstanceId root = dataModel();
    const auto attribute = [&](const char* name) -> core::i64 {
        const scene::Value value = m_world->getAttribute(root, m_world->atoms().lookup(name));
        const auto* number = std::get_if<f64>(&value);
        return number == nullptr ? -1 : static_cast<core::i64>(*number);
    };

    ConformanceReport report;
    report.total = attribute("ConformanceTotal");
    report.passed = attribute("ConformancePassed");
    report.failed = attribute("ConformanceFailed");
    report.ran = report.total >= 0;

    const scene::Value json = m_world->getAttribute(root, m_world->atoms().lookup("ConformanceReport"));
    if (const auto* text = std::get_if<std::string>(&json); text != nullptr)
        report.json = *text;

    return report;
}

void WorldHost::syncSkeletons()
{
    m_world->meshParts().forEach([&](core::InstanceId id, const scene::MeshPartComponent& meshPart) {
        (void)id;
        const core::NameAtom content = meshPart.meshContent;
        if (content.id == 0 || m_skeletons.find(content) != nullptr)
            return;
        // **A set, not a scan** (D126). This runs for every `MeshPart` in the
        // world on every tick, for ever, long after all the parsing is done --
        // and a linear search made that cost `meshParts x distinct
        // skeleton-less meshes` per tick. A world of five hundred props whose
        // meshes have no rigs paid a quarter of a million comparisons a tick to
        // conclude nothing, which is a shape nobody notices until the numbers
        // are large and then notices badly.
        //
        // Remembered before anything can go wrong, so a file with no skeleton --
        // which is most of them -- costs one parse rather than one per tick
        // forever.
        if (!m_skeletonsTried.insert(content.id).second)
            return;

        // **Out of the compiled mesh** (E9 step 14). This used to build a path
        // by hand and parse the source `.gltf` with `skeletonOnly` -- the third
        // of the three loose feeds the cut-over removed, and the only one that
        // ran on the SIM thread.
        //
        // Resolving rather than path-joining also fixes something the old code
        // could not do: a URN with a fragment (`...gltf#Torso`) named a file
        // that does not exist on disk, so a ragdoll built from one piece of a
        // split model found no rig at all.
        if (m_mounts == nullptr)
            return;
        const std::string urn(m_world->atoms().text(content));
        const asset::ResolvedContent resolved = m_mounts->resolve(urn);
        if (resolved.source != asset::ResolvedContent::Source::Pack || resolved.kind != asset::AssetKind::Mesh)
            return;

        // **The joints and the clips only.** `decodeMesh` also produces the
        // vertex, index and meshlet streams, and every one of them is thrown
        // away here -- which is the cost this pays for reading one set of bytes
        // instead of two. It is paid ONCE per URN, guarded by `m_skeletonsTried`
        // above, so a world of five hundred props pays it five hundred times at
        // boot and never again; the alternative is a second decoder that reads
        // only the joint chunk, which is a format-shaped answer to a question
        // that has not been measured.
        asset::CompiledMesh compiled;
        if (asset::decodeMesh(resolved.bytes, compiled).has_value())
            return;
        if (compiled.joints.empty())
            return;

        m_skeletons.set(content, render::SkeletonLibrary::Entry{std::move(compiled.joints), std::move(compiled.clips)});
    });
}

void WorldHost::tick()
{
    scene::EngineState& state = m_world->engineState();
    if (state.paused)
        return;

    // Before anything else in the tick: a track loaded by a script this tick has
    // to find the skeleton its mesh names, and a headless replay has no render
    // loop to have loaded it.
    syncSkeletons();

    state.tick += 1;
    state.simTime = static_cast<f64>(state.tick) * state.fixedTimestep;

    // **The camera the player last saw becomes the tick's** (ADR 0136): a
    // camera a render phase presented since the last tick is its simulated
    // `CFrame` from here, before anything in the tick reads it -- the only
    // point a frame's write reaches the simulation. Without a window nothing
    // is ever presented, so a replay or a gate never takes this branch.
    m_world->cameras().forEach([](core::InstanceId, scene::CameraComponent& camera) {
        if (!camera.presentedSinceTick)
            return;
        camera.cframe = camera.presented;
        camera.presentedSinceTick = false;
    });

    // Step 5a of architecture.md §3's frame: gameplay action signals, resolved
    // deterministically, BEFORE `PreAnimation`. Ahead of the first drain on
    // purpose -- a `Pressed` raised here is drained by the same drain the
    // phase's own handlers go through, so a script that jumps on a press sees
    // the press in the tick it happened rather than in the next one.
    m_input.dispatchSimTick(*m_world, state.tick);

    // The raw events the same dispatch produced (ADR 0041), enqueued right
    // beside the action signals it raised. They are what a caller reaching for
    // the familiar surface gets, and they come out of THIS dispatch rather than
    // from the OS -- so they sink like an action, replay like an action, and a
    // handler that writes to the world is deterministic.
    const std::span<const input::RawInputEvent> rawEvents = m_input.drainRawEvents();
    // Clicks and prompts (ADR 0126) first: the same events, resolved against
    // the world as this tick begins.
    m_runtime->stepDetectors(state.fixedTimestep, rawEvents);
    m_runtime->fireInputEvents(rawEvents);

    // `RemoteEvent` messages that arrived since the last tick (ADR 0077):
    // beside the input events, for the same reason -- arrival was a network
    // event at a wall-clock moment, and this is where it becomes a tick.
    script::fireRemoteMessages(m_runtime->state());
    // And what crossed from a sub-world, or into one (ADR 0107 §3), for the
    // same reason: it arrived between ticks, and this is where it becomes one.
    script::fireSubWorldMessages(m_runtime->state());

    // This tick's input, as the local player's intent (N1): after the dispatch
    // that resolved it, before any phase a script reads it in.
    scene::captureLocalIntents(*m_world);

    // The sound timeline, beside the input dispatch and for the same reason:
    // both are simulation state advanced by the tick, and both raise their
    // events into the drain the phases below go through.
    m_audio.tick(*m_world, state.fixedTimestep);

    // Each resumption point runs its engine phase, then drains (api-design.md
    // §3.1). `task` timers resume in their own phase between `PostSimulation`
    // and `Heartbeat`, and anything they defer drains at `Heartbeat`.
    //
    // The simulation sits between `PreSimulation` and `PostSimulation`, which is
    // architecture.md §3's order and not an arrangement of convenience: a script
    // pushes a part in `PreSimulation` and reads where it ended up in
    // `PostSimulation`, and the contacts the step produced are drained by the
    // `PostSimulation` drain rather than a frame later.
    m_runtime->firePhase(core::Phase::PreAnimation, state.fixedTimestep);
    m_runtime->drain(core::Phase::PreAnimation);

    // Tweens step here, in `PreAnimation`'s half of the tick, because that is
    // what they are: a property animated on the SimClock. After the drain, so a
    // tween started by a handler in this phase begins on the next tick rather
    // than half-advancing on the tick it was created in.
    m_runtime->stepTweens(state.fixedTimestep);

    // Skeletal animation, in the same half of the tick and after the drain for
    // the same reason a tween is: a track played by a `PreAnimation` handler
    // starts on the next tick rather than half-advancing on the one that
    // created it. `Ended` is enqueued here and drains with `PreSimulation`,
    // which is the deferred-signal rule (ADR 0015) and not a delay -- it is the
    // next resumption point either way.
    m_animation->sample(state.fixedTimestep);
    m_runtime->fireAnimationEnded(m_animation->drainEnded());
    m_animation->retire(*m_world);
    // Sprite sheets too (ADR 0102): the same clock, the same place, and the
    // same reason to be after the drain.
    scene::stepSpriteAnimators(*m_world, state.fixedTimestep);

    // What stands in the world is gathered at most once per tick, however
    // many paths the tick's scripts ask for.
    if (m_navigation != nullptr)
        m_navigation->setTick(state.tick);
    m_runtime->firePhase(core::Phase::PreSimulation, state.fixedTimestep);
    m_runtime->drain(core::Phase::PreSimulation);

    // **The crowd walks** (ADR 0098): after the scripts have given this tick's
    // orders and before physics sees where everything stands. Every
    // `NavigationAgent` in instance order, its part moved through the world's
    // own verb so the move is a write like a script's, and `Reached` fired on
    // the tick it arrives -- deferred like every other signal (ADR 0015).
    if (m_navigation != nullptr) {
        std::vector<nav::CrowdAgentState> crowd;
        m_world->navigationAgents().forEach([&](core::InstanceId id, const scene::NavigationAgentComponent& agent) {
            const core::InstanceId body = m_world->parentOf(id);
            const scene::PartComponent* part = body.valid() ? m_world->parts().find(body) : nullptr;
            if (part == nullptr || !m_world->isAncestorOf(m_workspace, body))
                return;
            crowd.push_back(nav::CrowdAgentState{id, agent.agentType, part->cframe.position, agent.target, agent.active,
                                                 agent.maxSpeed});
        });
        if (!crowd.empty()) {
            std::vector<nav::CrowdAgentStep> steps;
            m_navigation->stepCrowd(crowd, static_cast<f32>(state.fixedTimestep), steps);
            const core::NameAtom cframeName = m_world->atoms().intern("CFrame");
            const core::NameAtom reachedName = m_world->atoms().intern("Reached");
            for (const nav::CrowdAgentStep& step : steps) {
                const core::InstanceId body = m_world->parentOf(step.id);
                const scene::PartComponent* part = body.valid() ? m_world->parts().find(body) : nullptr;
                if (part == nullptr)
                    continue;
                if (part->cframe.position.x != step.position.x || part->cframe.position.y != step.position.y ||
                    part->cframe.position.z != step.position.z) {
                    core::CFrameD moved = part->cframe;
                    moved.position = step.position;
                    (void)m_world->setProperty(body, cframeName, scene::Value{moved});
                }
                if (step.reached) {
                    if (scene::NavigationAgentComponent* agent = m_world->navigationAgents().find(step.id);
                        agent != nullptr)
                        agent->active = false;
                    m_world->changes().push(
                        scene::Change{scene::ChangeKind::InstanceEventNoArgs, step.id, {}, reachedName});
                }
            }
        }
    }

    if (m_physics.has_value())
        m_physics->step(state.fixedTimestep);
    if (m_physics2d.has_value())
        m_physics2d->step(state.fixedTimestep);
    // Fluids are simulation too, and move in the same half of the tick: a
    // script that breaks a dam in `PreSimulation` sees the first block of
    // water move in `PostSimulation`.
    m_world->voxels().forEach(
        [&state](core::InstanceId, scene::VoxelComponent& voxels) { (void)scene::stepFluids(voxels, state.tick); });

    m_runtime->firePhase(core::Phase::PostSimulation, state.fixedTimestep);
    m_runtime->drain(core::Phase::PostSimulation);

    m_runtime->resumeTimers();
    // Preloads (ADR 0131 §3): what scripts asked for handed to the loader, and
    // every call whose content has arrived resumed -- beside the timers, so
    // what it defers drains at `Heartbeat`.
    if (std::vector<std::string> wanted = script::takePreloadContent(m_runtime->state()); !wanted.empty()) {
        if (m_warmContent)
            m_warmContent(*m_world, wanted);
    }
    script::resumePreloads(m_runtime->state(), [this](std::string_view content) { return contentState(content); });
    m_runtime->firePhase(core::Phase::Heartbeat, state.fixedTimestep);
    m_runtime->drain(core::Phase::Heartbeat);

    // **The safe point for a scene change** (ADR 0106): every phase of this
    // tick has drained, `SceneLoading` handlers included, and nothing of the
    // next has started.
    stepSceneLoad();
    (void)applyPendingScene();

    // **Then the sub-worlds' tick** (ADR 0107 §3): one of theirs for one of
    // this world's, after it, so what this tick sent reaches them in theirs.
    stepSubWorlds();
}

void WorldHost::cancelSceneChanges()
{
    scene::EngineState& state = m_world->engineState();
    std::string path;
    if (state.pendingSceneLoad.has_value())
        path = state.pendingSceneLoad->path;
    state.pendingSceneLoad.reset();
    if (m_sceneClose.has_value()) {
        path = m_sceneClose->path;
        // The handlers that were saving on the way out finish as a join's
        // clear finishes them: given up, their scene going with the match.
        script::abandonCloseHandlers(m_runtime->state());
        m_sceneClose.reset();
    }
    m_activationPending = false;
    if (script::SceneLoadRecord* record = script::activeSceneLoad(m_runtime->state()); record != nullptr) {
        path = record->path;
        const std::array<I18nArg, 1> args{I18nArg{"path", record->path}};
        script::sceneLoadFailed(m_runtime->state(),
                                core::makeError(ENG_TR("scene.err.cancelled_by_join"), args).message);
    }
    dropPrepared();
    if (!path.empty()) {
        const std::array<I18nArg, 1> args{I18nArg{"path", path}};
        core::log(LogLevel::Warn, ENG_TR("scene.err.cancelled_by_join"), args);
    }
}

bool WorldHost::applyPendingScene()
{
    scene::EngineState& state = m_world->engineState();
    // A replica's scene is its authority's, followed through the network:
    // nothing it asked for itself opens a scene here (audit A10).
    if (state.networkTopology == scene::NetworkTopology::Replica) {
        if (state.pendingSceneLoad.has_value() || m_sceneClose.has_value() || m_prepared.has_value())
            cancelSceneChanges();
        return false;
    }
    // **A scene closes before the next opens** (ADR 0124 §4): its
    // `scene:BindToClose` handlers run, and the change waits for them -- a
    // tick at a time, so the old scene goes on running under whatever loading
    // screen `SceneLoading` put up -- up to `[scene] close_grace_seconds` of
    // simulated time. Simulated, not wall clock: the tick the change lands on
    // is part of what a replay reproduces (R10).
    if (state.pendingSceneLoad.has_value()) {
        scene::EngineState::PendingSceneLoad pending = std::move(*state.pendingSceneLoad);
        state.pendingSceneLoad.reset();
        // **Found and read before anything closes** (audit A8): a scene that
        // is not there, or does not parse, is refused while the open one is
        // still whole -- its close handlers not run, its scripts running --
        // and the game is told. Checked, it was torn down first.
        std::string text;
        if (const std::optional<core::EngineError> problem = readSceneText(pending.path, text, true);
            problem.has_value()) {
            core::logText(LogLevel::Error, problem->message);
            script::fireSceneLoadFailed(m_runtime->state(), pending.path, problem->message);
        }
        else if (m_sceneClose.has_value()) {
            // Another `LoadScene` while this scene closes: the close goes on,
            // and ends in the scene asked for last.
            m_sceneClose->path = std::move(pending.path);
            m_sceneClose->data = std::move(pending.data);
        }
        else {
            m_sceneClose = SceneClose{std::move(pending.path), std::move(pending.data), state.tick, nullptr, 0};
            script::runSceneCloseHandlers(m_runtime->state());
        }
    }
    // A prepared scene's activation, whose `SceneLoading` has run by now.
    if (m_activationPending && !m_sceneClose.has_value()) {
        m_activationPending = false;
        if (script::SceneLoadRecord* record = script::activeSceneLoad(m_runtime->state());
            record != nullptr && m_prepared.has_value() && record->id == m_prepared->id) {
            m_sceneClose = SceneClose{record->path, record->data, state.tick, m_prepared->task, record->scene};
            script::runSceneCloseHandlers(m_runtime->state());
        }
    }
    if (!m_sceneClose.has_value())
        return false;

    const bool waiting = script::closeHandlersPending(m_runtime->state());
    const double waited = static_cast<double>(state.tick - m_sceneClose->startedTick) * state.fixedTimestep;
    if (waiting && waited < m_sceneCloseGrace)
        return false;
    if (waiting) {
        const std::array<I18nArg, 1> args{I18nArg{"seconds", m_sceneCloseGrace}};
        core::log(LogLevel::Warn, ENG_TR("scene.warn.close_grace_expired"), args);
        script::abandonCloseHandlers(m_runtime->state());
    }
    SceneClose close = std::move(*m_sceneClose);
    m_sceneClose.reset();
    const scene::ParsedScene* parsed = close.prepared != nullptr ? close.prepared->parsed.get() : nullptr;
    if (const std::optional<core::EngineError> error =
            loadScene(close.path, std::move(close.data), true, parsed, close.preparedScene);
        error.has_value()) {
        core::logText(LogLevel::Error, error->message);
        if (close.prepared != nullptr)
            script::sceneLoadFailed(m_runtime->state(), error->message);
        else
            script::fireSceneLoadFailed(m_runtime->state(), close.path, error->message);
        dropPrepared();
        return false;
    }
    if (close.prepared != nullptr) {
        script::sceneLoadDone(m_runtime->state());
        dropPrepared();
    }
    return true;
}

script::ContentState WorldHost::contentState(std::string_view content)
{
    if (m_warmedContent) {
        if (const std::optional<bool> warmed = m_warmedContent(*m_world, content); warmed.has_value())
            return *warmed ? script::ContentState::Loaded : script::ContentState::Failed;
        // On its way, or not a kind the loader warms: which is it?
        const bool loaderKind = content.ends_with(".gltf") || content.ends_with(".glb") || content.ends_with(".png") ||
                                content.ends_with(".jpg") || content.ends_with(".jpeg") || content.ends_with(".ktx2");
        if (loaderKind)
            return script::ContentState::Pending;
    }
    // A sound is opened and its length read, which is what a voice needs first.
    if (content.ends_with(".ogg") || content.ends_with(".wav") || content.ends_with(".mp3") ||
        content.ends_with(".flac"))
        return m_audio.clipDuration(content) > 0.0 ? script::ContentState::Loaded : script::ContentState::Failed;
    // Anything else, and everything in a run that draws nothing: found or not.
    if (m_mounts != nullptr)
        return m_mounts->resolve(content).source != asset::ResolvedContent::Source::Missing
                   ? script::ContentState::Loaded
                   : script::ContentState::Failed;
    std::string_view relative = content;
    if (relative.starts_with("asset://"))
        relative.remove_prefix(8);
    // Under `content/` or nowhere (audit F5): a name is not a path to probe.
    const std::optional<std::filesystem::path> file = core::resolveUnder(m_root / "content", relative);
    std::error_code error;
    return file.has_value() && std::filesystem::exists(*file, error) ? script::ContentState::Loaded
                                                                     : script::ContentState::Failed;
}

void WorldHost::configureRuntime()
{
    // **The owner's watchdog** (audit S5, 2026-09-28): where a person is
    // testing, a hitch past 10 ms is said and a script stuck for a second is
    // stopped; in a player's game nothing is said and the limit is five
    // seconds -- a slow phone loading a level is not a stuck script, and a
    // stuck script is never a frozen game.
    if (m_developer)
        m_runtime->setWatchdog(0.010, 1.0);
    else
        m_runtime->setWatchdog(0.0, 5.0);
    m_runtime->setMemoryLimit(static_cast<core::usize>(m_scriptMemoryMb) * 1024u * 1024u);
}

void WorldHost::dropPrepared()
{
    if (!m_prepared.has_value())
        return;
    // The job holds a raw pointer to the task: it is let go only once done.
    if (m_prepared->task != nullptr && m_prepared->task->handle.valid() &&
        !m_prepared->task->done.load(std::memory_order_acquire))
        jobs::wait(m_prepared->task->handle);
    m_preparedContent.clear();
    m_prepared.reset();
}

void WorldHost::stepSceneLoad()
{
    lua_State* L = m_runtime->state();
    if (std::optional<script::SceneLoadRequest> request = script::takeSceneLoadRequest(L)) {
        dropPrepared();
        auto task = std::make_shared<PrepareTask>();
        task->path = request->path;
        task->read = m_readContent;
        task->contentRoot = m_root / "content";
        m_prepared = Prepared{request->id, task, false};
        if (m_prepareInBackground && jobs::initialized())
            task->handle = jobs::schedule("scene.prepare", jobs::Domain::AssetIo, &PrepareTask::entry, task.get());
        else
            task->run();
    }
    if (!m_prepared.has_value())
        return;
    script::SceneLoadRecord* record = script::activeSceneLoad(L);
    if (record == nullptr || record->id != m_prepared->id) {
        // Cancelled, or given way to a plain `LoadScene`.
        if (!m_sceneClose.has_value() || m_sceneClose->prepared != m_prepared->task)
            dropPrepared();
        return;
    }

    if (record->status == script::SceneLoadStatus::Preparing) {
        PrepareTask& task = *m_prepared->task;
        if (!task.done.load(std::memory_order_acquire))
            return;
        if (!task.found || task.parsed == nullptr || task.parsed->error.has_value()) {
            std::string message;
            if (!task.found) {
                const std::array<I18nArg, 1> args{I18nArg{"path", task.path}};
                message = core::makeError(ENG_TR("scene.err.scene_not_found"), args).message;
            }
            else if (task.parsed != nullptr && task.parsed->error.has_value()) {
                message = task.parsed->error->message;
            }
            core::logText(LogLevel::Error, message);
            script::sceneLoadFailed(L, message);
            dropPrepared();
            return;
        }
        // Parsed: half. Then what it names, as it arrives.
        if (!m_prepared->warming) {
            m_prepared->warming = true;
            m_preparedContent = scene::sceneContent(*task.parsed);
            if (m_warmContent)
                m_warmContent(*m_world, m_preparedContent);
        }
        // What the loader was handed and has not yet answered for; a name it
        // does not load (a sound, a material) answers at once.
        core::usize arrived = 0;
        for (const std::string& content : m_preparedContent) {
            if (!m_warmedContent || m_warmedContent(*m_world, content).has_value())
                ++arrived;
        }
        const core::f64 warmed = m_preparedContent.empty() ? 1.0
                                                           : static_cast<core::f64>(arrived) /
                                                                 static_cast<core::f64>(m_preparedContent.size());
        script::setSceneLoadProgress(L, 0.5 + 0.5 * warmed);
        if (warmed < 1.0)
            return;
        script::sceneLoadReady(L);
    }
    if (record->status == script::SceneLoadStatus::Ready && record->activate && !m_activationPending &&
        !m_sceneClose.has_value()) {
        script::sceneLoadActivating(L);
        script::fireSceneLoading(L, record->path);
        m_activationPending = true;
    }
}

std::optional<core::EngineError> WorldHost::readSceneText(const std::string& path, std::string& text, bool parse) const
{
    // **Under `content/` or refused** (audit F5), whoever asked: a script, the
    // editor, or an authority whose scene a replica follows.
    if (!core::safeRelativePath(path).has_value()) {
        const std::array<I18nArg, 1> args{I18nArg{"path", path}};
        return core::makeError(ENG_TR("scene.err.scene_path_invalid"), args);
    }
    if (m_readContent) {
        if (std::optional<std::string> read = m_readContent(path); read.has_value())
            text = std::move(*read);
    }
    const std::optional<std::filesystem::path> file = core::resolveUnder(m_root / "content", path);
    if (text.empty() && (!file.has_value() || !readFile(*file, text))) {
        const std::array<I18nArg, 1> args{I18nArg{"path", path}};
        return core::makeError(ENG_TR("scene.err.scene_not_found"), args);
    }
    if (parse) {
        const std::unique_ptr<scene::ParsedScene> parsed = scene::parseScene(text);
        if (parsed != nullptr && parsed->error.has_value())
            return parsed->error;
    }
    return std::nullopt;
}

std::optional<core::EngineError> WorldHost::loadScene(const std::string& path, std::vector<core::u8> data,
                                                      bool closeHandlersRan, const scene::ParsedScene* prepared,
                                                      core::u32 preparedScene)
{
    std::string text;
    // Read and parsed already, off the main thread (ADR 0125) -- or read and
    // parsed here, **before anything is torn down** (audit A8): a file that
    // does not parse leaves the open scene as it was.
    if (prepared == nullptr) {
        if (const std::optional<core::EngineError> problem = readSceneText(path, text, true); problem.has_value())
            return problem;
    }
    else if (!core::safeRelativePath(path).has_value()) {
        const std::array<I18nArg, 1> args{I18nArg{"path", path}};
        return core::makeError(ENG_TR("scene.err.scene_path_invalid"), args);
    }
    const std::optional<std::filesystem::path> file = core::resolveUnder(m_root / "content", path);

    scene::World& w = *m_world;
    const core::InstanceId dataModel = m_runtime->dataModel();

    // **What goes with the game** (ADR 0106): a screen marked to stay is
    // lifted out of the scene before it is cleared, and put back after.
    std::vector<core::InstanceId> kept;
    const core::InstanceId ui = w.findFirstChildOfClass(dataModel, w.classes().findId(w.atoms().lookup("UIService")));
    for (core::InstanceId child = ui.valid() ? w.firstChild(ui) : core::InstanceId{}; child.valid();
         child = w.nextSibling(child)) {
        const scene::ScreenGuiComponent* screen = w.screenGuis().find(child);
        if (screen != nullptr && screen->keepOnSceneLoad)
            kept.push_back(child);
    }
    for (const core::InstanceId screen : kept)
        (void)w.setParent(screen, core::InstanceId{});

    // **The old scene closes** (ADR 0124): its `scene:BindToClose` handlers
    // have run -- waited for when a script asked, given one pass when a match
    // follows its authority, which has already moved on -- what it saved is
    // written, and what its scripts registered anywhere goes with them.
    if (!closeHandlersRan) {
        script::runSceneCloseHandlers(m_runtime->state());
        script::abandonCloseHandlers(m_runtime->state());
    }
    flushSaves();
    (void)script::closeScene(m_runtime->state(), preparedScene);

    // The old scene's own code goes with it; the new scene's is mounted below.
    remountSceneScripts({});

    // **The old scene's streamed cells go with it, and the new scene meets the
    // grid as the boot's did** (audit A4). Only the boot scene was ever
    // partitioned: a scene changed to loaded whole, while the boot scene's
    // cells went on streaming into it -- and a return to the boot scene
    // brought its parts twice, once from the file and once from the cells
    // `clearScene` leaves, being the engine's. A scene prepared in the
    // background was parsed whole, and loads whole.
    if (m_resetStreaming)
        m_resetStreaming();
    if (prepared == nullptr && m_partitionScene && file.has_value()) {
        if (const std::filesystem::path partitioned = m_partitionScene(w, *file); !partitioned.empty()) {
            std::string cut;
            if (readFile(partitioned, cut))
                text = std::move(cut);
        }
    }

    scene::SceneIoReport report;
    // **Named before it is read** (ADR 0138 §6): the read gives what it makes
    // an origin that says which scene, and on failure the name goes back.
    const std::string previousScene = w.engineState().currentScene;
    w.engineState().currentScene = path;
    const std::optional<core::EngineError> error = prepared != nullptr
                                                       ? scene::readScene(w, *prepared, &report, m_stamps)
                                                       : scene::readScene(w, text, &report, m_stamps);
    // Retired rather than only destroyed, so the old scene leaves the pools now
    // and not at a drain nothing may be running (the editor's own reason).
    w.retireDestroyed();
    for (const core::InstanceId screen : kept) {
        if (w.alive(screen) && ui.valid())
            (void)w.setParent(screen, ui);
    }
    if (error.has_value()) {
        w.engineState().currentScene = previousScene;
        return error;
    }
    m_bootSceneReport = report;
    m_bootSceneApplied = true;

    w.engineState().currentScene = path;
    w.engineState().sceneLoadData = std::move(data);
    remountSceneScripts(path);

    // **The new scene's scripts start**, and only those: `GlobalScriptService`'s
    // have been running all along. `SceneLoaded` is deferred behind their first
    // resumption, as `game.Loaded` is behind the boot's.
    const core::InstanceId global =
        w.findFirstChildOfClass(dataModel, w.classes().findId(w.atoms().lookup("GlobalScriptService")));
    script::startScriptsExcept(m_runtime->state(), global);
    warnScriptsInStorage();
    // What was sent to it while it was prepared, behind its scripts' first
    // resumption and ahead of `SceneLoaded` (ADR 0124 §6).
    script::deliverHeldMessages(m_runtime->state());
    script::fireSceneLoaded(m_runtime->state(), path);
    const std::array<I18nArg, 2> args{I18nArg{"path", path},
                                      I18nArg{"count", static_cast<core::i64>(report.instances)}};
    core::log(LogLevel::Info, ENG_TR("scene.info.scene_changed"), args);
    return std::nullopt;
}

// **A script kept in storage does not run** (ADR 0137 §3): one warning a
// scene, naming the first, so a game that relied on it is told where to move
// it. `ModuleScript`s are what storage is for, and are not counted.
void WorldHost::warnScriptsInStorage()
{
    scene::World& w = *m_world;
    const scene::ClassId scriptClass = w.classes().findId(w.atoms().lookup("Script"));
    const core::InstanceId dataModel = m_runtime->dataModel();
    for (const std::string_view storage : {std::string_view("ServerStorage"), std::string_view("ReplicatedStorage")}) {
        const core::InstanceId service =
            w.findFirstChildOfClass(dataModel, w.classes().findId(w.atoms().lookup(storage)));
        if (!service.valid())
            continue;
        std::vector<core::InstanceId> below;
        w.collectDescendants(service, below);
        core::i64 count = 0;
        core::InstanceId first;
        for (const core::InstanceId id : below) {
            if (w.classOf(id) != scriptClass)
                continue;
            if (count++ == 0)
                first = id;
        }
        if (count == 0)
            continue;
        const std::array<I18nArg, 3> args{I18nArg{"count", count}, I18nArg{"storage", storage},
                                          I18nArg{"script", std::string(w.atoms().text(w.name(first)))}};
        core::log(LogLevel::Warn, ENG_TR("scene.warn.script_in_storage"), args);
    }
}

// **Back to solo is a solo boot of the scene it is in** (ADR 0137 §5): the
// scene read again from this machine's own package -- its parts and the
// scripts inside them, which the join had replaced with the authority's --
// and the server code started again, fresh. The simplest honest mechanism, as
// the ADR allows: a game that went solo is a game that just loaded its scene.
void WorldHost::returnToSolo()
{
#if ENG_ENABLE_REPLICATION
    // What a join kept for the authority's instances: the scene read again
    // brings this machine's own scripts back where they were.
    if (m_scriptTemplates)
        m_scriptTemplates->clear(*m_world);
#endif
    const std::string current = m_world->engineState().currentScene;
    if (!current.empty()) {
        if (const std::optional<core::EngineError> error = loadScene(current); error.has_value())
            core::logText(LogLevel::Error, error->message);
    }
#if ENG_ENABLE_REPLICATION
    else {
        // **A game with no scene has nothing to load over what the server
        // sent**, and it stayed: the dead session's parts stood beside the
        // ones this machine's own code then made (D432). What an authority
        // replicates goes, as it went when this machine joined.
        (void)replication::clearForReplica(*m_world, m_workspace);
    }
#endif
    restartServerCode();
    script::reconcileAllScripts(m_runtime->state());
}

void WorldHost::holdSceneClientCode(bool held)
{
    m_world->engineState().sceneClientHeld = held;
    script::reconcileAllScripts(m_runtime->state());
}

void WorldHost::restartServerCode()
{
    scene::World& w = *m_world;
    const core::InstanceId dataModel = m_runtime->dataModel();
    const core::InstanceId global =
        w.findFirstChildOfClass(dataModel, w.classes().findId(w.atoms().lookup("GlobalScriptService")));
    const core::InstanceId serverFolder =
        global.valid() ? w.findFirstChild(global, w.atoms().lookup("Server")) : core::InstanceId{};
    const core::InstanceId serverScripts =
        w.findFirstChildOfClass(dataModel, w.classes().findId(w.atoms().lookup("ServerScriptService")));

    // What is left of the old copies, gone first, so nothing runs twice.
    std::vector<core::InstanceId> stale;
    for (const core::InstanceId container : {serverFolder, serverScripts}) {
        for (core::InstanceId child = container.valid() ? w.firstChild(container) : core::InstanceId{}; child.valid();
             child = w.nextSibling(child)) {
            if (w.mounted(child))
                stale.push_back(child);
        }
    }
    for (const core::InstanceId node : stale)
        (void)w.destroy(node);
    w.retireDestroyed();

    std::vector<script::MountedScript> entries;
    collectScriptFiles(m_root, m_root / "src" / "server", "GlobalScriptService/Server", false, entries);
    if (!m_sceneName.empty()) {
        collectScriptFiles(m_root, m_root / "src" / "scenes" / m_sceneName / "server", "ServerScriptService", false,
                           entries);
    }
    for (const core::InstanceId made : script::mountScripts(m_runtime->state(), entries))
        (void)script::startScript(m_runtime->state(), made);
}

void WorldHost::remountSceneScripts(std::string_view path)
{
    scene::World& w = *m_world;
    const core::InstanceId dataModel = m_runtime->dataModel();

    // What the previous scene mounted: every file-made node under the two
    // scene script services, which `readScene` leaves alone because they are
    // the files'.
    std::vector<core::InstanceId> mounted;
    for (const char* serviceName : {"ServerScriptService", "ClientScriptService"}) {
        const core::InstanceId service =
            w.findFirstChildOfClass(dataModel, w.classes().findId(w.atoms().lookup(serviceName)));
        for (core::InstanceId child = service.valid() ? w.firstChild(service) : core::InstanceId{}; child.valid();
             child = w.nextSibling(child)) {
            if (w.mounted(child))
                mounted.push_back(child);
        }
    }
    for (const core::InstanceId node : mounted)
        (void)w.destroy(node);
    w.retireDestroyed();

    if (path.empty())
        return;
    m_sceneName = sceneFolderName(std::filesystem::path(std::string(path)));
    std::vector<script::MountedScript> entries;
    const std::filesystem::path scene = m_root / "src" / "scenes" / m_sceneName;
    collectScriptFiles(m_root, scene / "server", "ServerScriptService", false, entries);
    collectScriptFiles(m_root, scene / "client", "ClientScriptService", false, entries);
    script::mountScripts(m_runtime->state(), entries);
}

void WorldHost::pumpInput(std::span<const platform::Event> events)
{
    m_input.pumpFrame(events);

    // Losing focus releases everything held. An alt-tab that left W down is how
    // a character keeps walking into a wall while its window is in the
    // background, and the release has to happen HERE rather than at the next
    // dispatch: the window may stay unfocused for minutes, and the game should
    // not spend them running forward.
    for (const platform::Event& event : events) {
        if (event.type == platform::EventType::WindowFocusLost)
            m_input.releaseAll(*m_world);
        // The last moment a phone promises (ADR 0111 section 5).
        if (event.type == platform::EventType::WillEnterBackground)
            flushSaves();
    }
}

void WorldHost::publishNetworkResults()
{
    // At a FRAME SAFE POINT, not where the worker finished. A response arrives
    // on a thread of its own at a wall-clock moment; resuming the coroutine
    // there would put game code into the frame wherever the socket happened to
    // land it. Same rule and same reason as `resumeAreaWaiters` below (R10).
    script::resumeNetWaiters(m_runtime->state());
}

void WorldHost::publishStats(const script::FrameStats& stats)
{
    script::publishFrameStats(m_runtime->state(), stats);
}

void WorldHost::publishStreamingResults(const std::vector<core::InstanceId>& streamedOut,
                                        const std::function<bool(core::DVec3, f64)>& areaResident)
{
    for (const core::InstanceId instance : streamedOut) {
        script::fireStreamedOut(m_runtime->state(), instance);
        m_husks.push_back(instance);
    }

    // **A husk lives as long as a script holds it, and no longer.** Without
    // this sweep a streamed world would keep every husk it ever made, and a
    // player walking back and forth past something a script once looked at
    // would grow the world by one instance per pass.
    scene::World& world = *m_world;
    const auto heldBelow = [&](core::InstanceId top) {
        std::vector<core::InstanceId> stack{top};
        while (!stack.empty()) {
            const core::InstanceId at = stack.back();
            stack.pop_back();
            if (instanceHeld(at))
                return true;
            for (core::InstanceId child = world.firstChild(at); child.valid(); child = world.nextSibling(child))
                stack.push_back(child);
        }
        return false;
    };
    std::erase_if(m_husks, [&](core::InstanceId husk) {
        if (!world.alive(husk) || world.parentOf(husk).valid())
            return true;
        // A husk's subtree came with it, and a child a script holds keeps the
        // husk above it, since destroying the husk would destroy the child.
        if (heldBelow(husk))
            return false;
        (void)world.destroy(husk);
        return true;
    });
    script::resumeAreaWaiters(m_runtime->state(), areaResident);
}

bool WorldHost::instanceHeld(core::InstanceId id)
{
    return script::instanceHeld(m_runtime->state(), id);
}

void WorldHost::preRender(f64 renderDt, const render::DrawPoses* poses)
{
    scene::EngineState& state = m_world->engineState();
    state.renderPhase = true;
    if (poses != nullptr) {
        m_runtime->setDrawnPoseSink(script::DrawnPoseSink{
            .user = const_cast<render::DrawPoses*>(poses),
            .pose =
                [](void* user, core::InstanceId id, script::DrawnKind kind, core::CFrameD& out) {
                    const auto& frame = *static_cast<const render::DrawPoses*>(user);
                    switch (kind) {
                    case script::DrawnKind::Part:
                        out = frame.part(id);
                        return true;
                    case script::DrawnKind::Attachment:
                        out = frame.attachment(id);
                        return true;
                    case script::DrawnKind::Camera:
                        out = frame.camera(id);
                        return true;
                    }
                    return false;
                },
        });
    }
    // Step 3 of the frame: the render-rate half of the dispatch split
    // (ADR 0039), before anything else of the phase, so a camera reads the
    // look delta the frame it happened.
    m_input.dispatchRenderRate(*m_world);
    // Then the render steps, in priority order, before `PreRender`'s handlers
    // (ADR 0136).
    m_runtime->runRenderSteps(renderDt);
    m_runtime->firePhase(core::Phase::PreRender, renderDt);
    m_runtime->drain(core::Phase::PreRender);
    m_runtime->setDrawnPoseSink(script::DrawnPoseSink{});
    state.renderPhase = false;
}

void WorldHost::setGizmoTarget(render::DebugDraw* draw)
{
    m_gizmos = draw;
    if (draw == nullptr) {
        m_runtime->setGizmoSink({});
        return;
    }

    m_runtime->setGizmoSink(script::GizmoSink{
        .user = this,
        .line =
            [](void* user, core::Vec3 a, core::Vec3 b, core::Color3 color) {
                auto& host = *static_cast<WorldHost*>(user);
                host.m_gizmos->line(a, b, render::DebugColor::fromLinear(color.r, color.g, color.b));
            },
        .box =
            [](void* user, const core::CFrameD& frame, core::Vec3 size, core::Color3 color) {
                auto& host = *static_cast<WorldHost*>(user);
                // Half-extents, because `DebugDraw` takes a centre and a radius and
                // halving at each call site is where the sign errors live.
                host.m_gizmos->wireBox(core::toRenderMatrix(frame, {}),
                                       core::Vec3{size.x * 0.5f, size.y * 0.5f, size.z * 0.5f},
                                       render::DebugColor::fromLinear(color.r, color.g, color.b));
            },
        .sphere =
            [](void* user, core::Vec3 position, f32 radius, core::Color3 color) {
                auto& host = *static_cast<WorldHost*>(user);
                host.m_gizmos->wireSphere(position, radius, render::DebugColor::fromLinear(color.r, color.g, color.b));
            },
    });
}

bool WorldHost::shutdownRequested()
{
    return script::shutdownRequested(m_runtime->state());
}

void WorldHost::close(core::f64 graceSeconds, const std::function<void()>& pump)
{
    // What this world runs closes before it does.
    closeSubWorlds();
    // Inside to outside (ADR 0124 §4): the open scene's handlers, then the
    // game's, under one grace period. A scene still closing is closed by this.
    m_sceneClose.reset();
    m_activationPending = false;
    dropPrepared();
    script::runSceneCloseHandlers(m_runtime->state());
    script::runCloseHandlers(m_runtime->state());
    // One drain, so anything a close handler deferred already runs.
    m_runtime->drain(core::Phase::Heartbeat);

    // Then the grace period: keep ticking while a handler is still parked. A
    // handler that yields is the whole reason `BindToClose` takes a function
    // rather than being a signal, and cutting it off at the first drain made
    // the promise `architecture.md` §app carries untrue for five milestones
    // (D016).
    //
    // A paused world's `tick` does nothing, and the loop used to spin on it
    // for the whole grace period without resuming a thing (audit A1): the
    // world is closing, so it runs. The ticks are paced to the fixed step, so
    // a `task.wait(1)` is a second as it would have been, and the CPU is not
    // spent spinning; results from the HTTP worker reach their threads here
    // as they do in the frame.
    const auto graceNs = static_cast<core::u64>(std::max(0.0, graceSeconds) * 1'000'000'000.0);
    const core::u64 started = platform::nowNs();
    scene::EngineState& state = m_world->engineState();
    state.paused = false;
    const auto stepNs =
        std::max<core::u64>(1'000'000, static_cast<core::u64>(std::max(0.0, state.fixedTimestep) * 1'000'000'000.0));
    core::u64 due = started;
    while (script::closeHandlersPending(m_runtime->state())) {
        if (platform::nowNs() - started >= graceNs) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"seconds", static_cast<core::i64>(graceSeconds)}};
            core::log(core::LogLevel::Warn, ENG_TR("engine.close.warn.grace_expired"), args);
            script::abandonCloseHandlers(m_runtime->state());
            break;
        }
        if (pump)
            pump();
        publishNetworkResults();
        tick();
        due += stepNs;
        const core::u64 now = platform::nowNs();
        if (due > now)
            platform::sleepNs(std::min(due - now, stepNs));
        else
            due = now; // behind: no burst of ticks to catch up
    }
    // Whatever the close handlers saved, on disk before the process goes.
    flushSaves();
}

void WorldHost::flushSaves()
{
    if (m_saves != nullptr && m_world.has_value())
        m_saves->flush(m_world->engineState().saveVersion);
}

} // namespace engine::app
