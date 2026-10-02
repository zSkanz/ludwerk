#include "engine/script/modules.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cstdlib>

#include "engine/scene/world.h"
#include "engine/script/bytecode.h"
#include "engine/script/debugger.h"
#include "engine/script/instance_binding.h"
#include "engine/script/sandbox.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"
#include "engine/script/tasks.h"

namespace engine::script {
namespace {

[[nodiscard]] ModuleRegistry& registry(lua_State* L) noexcept
{
    return *context(L).modules;
}

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

// --- Loading -----------------------------------------------------------------

// The one load in this module, source or bytecode (ADR 0112): `bytecodeOf`
// compiles the first with the options that make `Vector3.new` a constant
// (ADR 0013) and takes the second as it is, so a packaged game's `require` and
// the editor's reach `luau_load` the same way. `chunkName` is what
// `lua_getinfo` reports back as the requiring file, which is how a relative
// specifier knows where it is relative to.
//
// A build with no compiler refuses SOURCE here -- at the single point where
// source would have become bytecode -- so every caller reports it through the
// failure path it already has, keyed and logged the same way a syntax error is.
[[nodiscard]] bool loadChunk(lua_State* co, std::string_view source, std::string_view chunkName, std::string& outError)
{
    std::string bytecode;
    if (const auto refused = bytecodeOf(source, chunkName, bytecode)) {
        outError = refused->message;
        return false;
    }

    const std::string chunk = "@" + std::string(chunkName);
    const int status = luau_load(co, chunk.c_str(), bytecode.data(), bytecode.size(), 0);
    if (status != LUA_OK) {
        const char* message = lua_tostring(co, -1);
        outError = message == nullptr ? std::string{} : std::string(message);
        lua_pop(co, 1);
        return false;
    }
    return true;
}

// The chunk name of whatever called `require`, without the `@`. Empty at the
// top of a C boundary, which is what makes a relative specifier from there a
// project-root-relative one.
[[nodiscard]] std::string requiringPath(lua_State* L)
{
    lua_Debug info{};
    // Level 1 is the Luau frame that called this C function. `lua_getinfo`
    // returns 0 when the level does not resolve, which happens when `require` is
    // reached from C rather than from a script.
    if (lua_getinfo(L, 1, "s", &info) == 0 || info.source == nullptr)
        return {};

    std::string_view source{info.source};
    if (!source.empty() && source.front() == '@')
        source.remove_prefix(1);
    return std::string(source);
}

[[noreturn]] void raiseModuleError(lua_State* L, core::TextKey key, std::string_view specifier, std::string_view from)
{
    const core::I18nArg args[] = {
        {"specifier", specifier},
        {"source", from.empty() ? std::string_view{"the host"} : from},
    };
    raise(L, key, args);
}

// Runs a module body on its own sandboxed coroutine and returns its single
// value on `L`'s stack.
//
// Its own coroutine because a module is a chunk like any other and a chunk gets
// its own globals table (api-design.md §3). It must NOT yield: a module is
// evaluated once and its result cached, and a half-evaluated module in the cache
// is a module every later requirer would get wrong.
//
// `self` is the instance the module IS, or an invalid id for a module that is
// not an instance at all -- see the `script` binding below.
[[nodiscard]] bool evaluateModule(lua_State* L, std::string_view source, std::string_view chunkName,
                                  core::InstanceId self, std::string& outError)
{
    lua_State* co = lua_newthread(L);
    const int rooted = lua_gettop(L);
    luaL_sandboxthread(co);

    // **`script` inside a module is the MODULE**, which is what makes a module
    // a place you can put things: `script:FindFirstChild("Inner")` has to ask
    // the module's own children, and until this it asked the REQUIRER's.
    //
    // `luaL_sandboxthread` gives the thread a fresh globals table whose
    // `__index` is the creating thread's, so a module inherited whatever
    // `script` the script that required it had. Nested modules therefore looked
    // for their own children under somebody else's instance and found nothing,
    // which is the defect this fixes and which reads as a module that "cannot
    // be required" from the outside.
    //
    // **Nil rather than inherited when there is no instance.** An engine module
    // (`@engine/testing`) is not a script and has none; handing it the
    // requirer's would be the same lie in a quieter place.
    //
    // **Set BEFORE the load**, and that is not a preference:
    // `luaL_sandboxthread` marks the new globals table safeenv, which makes the
    // compiler's import fast path resolve globals at load time -- so a `script`
    // assigned afterwards is invisible to every `script.X` in the chunk. The
    // same ordering `startScripts` documents, for the same reason.
    if (self.valid())
        pushInstance(co, self);
    else
        lua_pushnil(co);
    lua_setglobal(co, "script");

    if (!loadChunk(co, source, chunkName, outError)) {
        lua_remove(L, rooted);
        return false;
    }

    // A `require` chain is resumes inside resumes too (audit S4).
    if (!enterResume(L)) {
        lua_remove(L, rooted);
        raise(L, ENG_TR("script.err.resume_too_deep"));
    }
    const int status = lua_resume(co, nullptr, 0);
    leaveResume(L, co);
    // **A break is not a yield**, and saying so is the difference between a
    // breakpoint inside a `ModuleScript` working and reporting a false error
    // about a module that did nothing wrong. The thread is parked and the
    // debugger holds the only reference to it; the require that started this
    // fails, which is honest -- a module stopped in a debugger has no value to
    // hand back yet.
    if (status == LUA_BREAK) {
        outError = "module stopped at a breakpoint";
        lua_remove(L, rooted);
        return false;
    }
    if (status == LUA_YIELD) {
        outError = "module yielded during evaluation";
        lua_remove(L, rooted);
        return false;
    }
    if (status != LUA_OK) {
        const char* message = lua_tostring(co, -1);
        outError = message == nullptr ? std::string{} : std::string(message);
        lua_remove(L, rooted);
        return false;
    }

    if (lua_gettop(co) < 1) {
        outError.clear();
        lua_remove(L, rooted);
        return false;
    }

    // Exactly one value: the last, so a module that returns several is read the
    // way a `return a, b` would be assigned to one name.
    lua_xmove(co, L, 1);
    lua_remove(L, rooted);
    return true;
}

// --- require -----------------------------------------------------------------

int requireRegistered(lua_State* L, ModuleRegistry::Registered& module, std::string_view from)
{
    if (module.resultRef > 0) {
        lua_getref(L, module.resultRef);
        return 1;
    }
    if (module.loading)
        raiseModuleError(L, ENG_TR("script.err.require_cycle"), module.name, from);

    module.loading = true;
    if (module.opener != nullptr) {
        // A native module cannot fail to COMPILE and does not need the pcall
        // wrapper a source module gets: it raises the way any binding raises,
        // and that propagates to the requirer already keyed.
        module.loading = false;
        module.opener(L);
        module.resultRef = lua_ref(L, -1);
        return 1;
    }

    // No instance: an engine module is C++ or a string this binary carries, and
    // there is nothing in the world that IS it.
    std::string error;
    const bool ok = evaluateModule(L, module.source, module.name, core::InstanceId{}, error);
    module.loading = false;

    if (!ok) {
        const core::I18nArg args[] = {{"source", std::string_view{module.name}}, {"message", std::string_view{error}}};
        raise(L, ENG_TR("script.err.runtime"), args);
    }

    module.resultRef = lua_ref(L, -1);
    return 1;
}

// `require(moduleScript)` -- a module that lives in the TREE rather than on
// disk (ADR 0050).
//
// **Only a `ModuleScript`.** A `Script` runs when the world does, and requiring
// one would run it a second time in a different place; the two classes exist to
// keep those apart, so this refuses anything else by name rather than by guess.
//
// Cached by instance, with the same three states a path-keyed module has: a
// value, a failure that is re-raised rather than re-run, and "being evaluated
// right now", which is a cycle. That is api-design.md §3's contract, and it
// reads the same whichever kind of module you asked for.
[[nodiscard]] scene::ClassId moduleClassOf(const scene::World& w)
{
    return w.classes().findId(w.atoms().lookup("ModuleScript"));
}

int requireInstance(lua_State* L, core::InstanceId id, std::string_view from)
{
    ModuleRegistry& modules = registry(L);
    scene::World& w = world(L);

    if (!w.alive(id))
        raiseModuleError(L, ENG_TR("script.err.module_not_found"), "<destroyed>", from);

    const scene::ClassDescriptor* descriptor = w.classes().find(w.classOf(id));
    const std::string_view className = descriptor != nullptr ? w.atoms().text(descriptor->name) : std::string_view{};
    if (className != "ModuleScript")
        raiseModuleError(L, ENG_TR("script.err.module_not_module"), className, from);

    // The path a cycle or a failure is REPORTED with. The tree path rather than
    // the id, because an id means nothing to the person reading the error --
    // and a module mounted from a file is named by its FILE, which is also the
    // base its own relative requires resolve against (`src/shared/`, ADR 0105).
    const std::string_view mounted = mountedPathOf(L, id);
    const std::string_view name = mounted.empty() ? w.atoms().text(w.name(id)) : mounted;

    for (ModuleRegistry::TreeModule& cached : modules.treeModules) {
        if (cached.instance != id)
            continue;
        if (cached.failed) {
            const core::I18nArg args[] = {
                {"source", name},
                {"message", std::string_view{cached.error}},
            };
            raise(L, ENG_TR("script.err.runtime"), args);
        }
        if (cached.loading)
            raiseModuleError(L, ENG_TR("script.err.require_cycle"), name, from);
        lua_getref(L, cached.resultRef);
        return 1;
    }

    modules.treeModules.push_back(
        ModuleRegistry::TreeModule{.instance = id, .resultRef = -1, .loading = true, .failed = false, .error = {}});
    const usize slot = modules.treeModules.size() - 1;

    const std::optional<scene::Value> source = w.getProperty(id, w.atoms().intern("Source"));
    const std::string* text = source.has_value() ? std::get_if<std::string>(&*source) : nullptr;

    std::string error;
    const bool ok = text != nullptr && evaluateModule(L, *text, name, id, error);
    modules.treeModules[slot].loading = false;

    if (!ok) {
        // **Cached as a failure rather than retried.** A module that errors
        // propagates to its requirer and stays failed, which is what stops one
        // broken module from being re-evaluated once per requirer -- and it is
        // the thing the vendored implementation does not do (U-35).
        modules.treeModules[slot].failed = true;
        modules.treeModules[slot].error = error;
        const core::I18nArg args[] = {{"source", name}, {"message", std::string_view{error}}};
        raise(L, ENG_TR("script.err.runtime"), args);
    }

    modules.treeModules[slot].resultRef = lua_ref(L, -1);
    return 1;
}

int scriptRequire(lua_State* L)
{
    // **An instance is a module now** (ADR 0050). Checked before the string,
    // because a `ModuleScript` is not a path and asking `luaL_checklstring`
    // first would report it as a type error rather than requiring it.
    if (const core::InstanceId* id = toInstance(L, 1); id != nullptr)
        return requireInstance(L, *id, requiringPath(L));

    size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    const std::string_view specifier{text, length};
    const std::string from = requiringPath(L);

    ModuleRegistry& modules = registry(L);
    scene::World& w = world(L);

    // Engine-provided modules are matched first and never reach the loader.
    // Deliberately the opposite way round from the vendored implementation,
    // where a registered module is matched before the permission callback runs
    // and a denied chunk can still reach `@engine/…` (U-39) -- here there is one
    // gate and it is this function.
    if (!specifier.empty() && specifier.front() == '@') {
        for (ModuleRegistry::Registered& module : modules.registered) {
            if (module.name == specifier)
                return requireRegistered(L, module, from);
        }
        // NOT an error yet: `@` is also how a `.luaurc` alias is spelled, so an
        // unmatched one falls through to the loader. Engine modules win a
        // collision, which is what makes `@engine/testing` mean one thing.
    }

    if (modules.loader.resolve == nullptr || modules.loader.read == nullptr)
        raiseModuleError(L, ENG_TR("script.err.module_not_found"), specifier, from);

    std::string path;
    if (!modules.loader.resolve(modules.loader.user, from, specifier, path))
        raiseModuleError(L, ENG_TR("script.err.module_not_found"), specifier, from);

    // **A file the mount made a `ModuleScript` of is that instance** (ADR
    // 0105): `require("src/shared/ring")` and `require(Shared.ring)` are one
    // module, evaluated once, whichever a script reached for first.
    for (const ModuleRegistry::Entry& entry : modules.entries) {
        if (entry.path == path && w.alive(entry.instance) && w.classOf(entry.instance) == moduleClassOf(w))
            return requireInstance(L, entry.instance, from);
    }

    // Keyed on the resolved project-relative path, so two specifiers that name
    // the same file share one evaluation -- which is what "one evaluation per
    // module per VM" means.
    if (const auto found = modules.byPath.find(path); found != modules.byPath.end()) {
        ModuleRegistry::Module& cached = modules.modules[found->second];
        if (cached.failed) {
            // The failure is cached and re-raised, rather than the module being
            // re-run in the hope of a different answer (api-design.md §3).
            const core::I18nArg args[] = {
                {"source", std::string_view{cached.path}},
                {"message", std::string_view{cached.error}},
            };
            raise(L, ENG_TR("script.err.runtime"), args);
        }
        if (cached.loading)
            raiseModuleError(L, ENG_TR("script.err.require_cycle"), specifier, from);

        lua_getref(L, cached.resultRef);
        return 1;
    }

    std::string source;
    if (!modules.loader.read(modules.loader.user, path, source))
        raiseModuleError(L, ENG_TR("script.err.module_not_found"), specifier, from);

    const usize index = modules.modules.size();
    modules.modules.push_back(ModuleRegistry::Module{path, -1, true, false, {}});
    modules.byPath.emplace(path, index);

    // **The instance the mount made for this file**, when there is one. A file
    // under `src/scripts` is both a path and an instance (ADR 0050), and the
    // two have to agree about what `script` means inside it -- otherwise the
    // same module body behaves differently depending on which way it was
    // reached. A path the mount never saw has no instance and gets nil.
    core::InstanceId self;
    for (const ModuleRegistry::Entry& entry : modules.entries) {
        if (entry.path == path) {
            self = entry.instance;
            break;
        }
    }

    std::string error;
    const bool ok = evaluateModule(L, source, path, self, error);
    modules.modules[index].loading = false;

    if (!ok) {
        modules.modules[index].failed = true;
        modules.modules[index].error = error;
        const core::I18nArg args[] = {{"source", std::string_view{path}}, {"message", std::string_view{error}}};
        raise(L, ENG_TR("script.err.runtime"), args);
    }

    modules.modules[index].resultRef = lua_ref(L, -1);
    return 1;
}

// --- Mounting ----------------------------------------------------------------

// `src/client/enemy/patrol.luau` mounts as `GlobalScriptService/Client/enemy/
// patrol`: the directories become `Folder`s and the extension is dropped.
[[nodiscard]] std::vector<std::string_view> pathSegments(std::string_view path)
{
    std::vector<std::string_view> segments;
    usize start = 0;
    for (usize index = 0; index <= path.size(); ++index) {
        if (index == path.size() || path[index] == '/') {
            if (index > start)
                segments.push_back(path.substr(start, index - start));
            start = index + 1;
        }
    }
    return segments;
}

// The C function `startScripts` defers last. It runs after every entry script's
// first resumption, which is what lets a `game.Loaded:Connect` written at file
// scope be in the connection list the fire captures.
int fireLoadedTrampoline(lua_State* L)
{
    fireDataModelLoaded(L);
    return 0;
}

} // namespace

void registerRequire(lua_State* L)
{
    lua_pushcfunction(L, scriptRequire, "require");
    lua_setglobal(L, "require");
}

void registerModule(lua_State* L, std::string_view name, std::string_view source)
{
    ModuleRegistry& modules = registry(L);
    for (ModuleRegistry::Registered& module : modules.registered) {
        if (module.name == name) {
            module.source = std::string(source);
            module.opener = nullptr;
            return;
        }
    }
    modules.registered.push_back(ModuleRegistry::Registered{.name = std::string(name), .source = std::string(source)});
}

void registerNativeModule(lua_State* L, std::string_view name, int (*opener)(lua_State*))
{
    ModuleRegistry& modules = registry(L);
    for (ModuleRegistry::Registered& module : modules.registered) {
        if (module.name == name) {
            module.source.clear();
            module.opener = opener;
            return;
        }
    }
    // `.source` named explicitly, empty, because Clang's -Wmissing-field-initializers
    // counts a skipped designator as a missing one even where the default is what
    // was wanted. Naming it costs a word and keeps the Tier-2 build clean.
    modules.registered.push_back(ModuleRegistry::Registered{.name = std::string(name), .source = {}, .opener = opener});
}

std::vector<core::InstanceId> mountScripts(lua_State* L, std::span<const MountedScript> scripts)
{
    std::vector<core::InstanceId> made;
    ModuleRegistry& modules = registry(L);
    scene::World& w = world(L);

    std::vector<MountedScript> ordered(scripts.begin(), scripts.end());
    // Sorted here rather than trusted from the caller: the tree's shape and the
    // start order are both observable, and a directory walk's order is exactly
    // the kind of thing R10 forbids from reaching them.
    std::sort(ordered.begin(), ordered.end(),
              [](const MountedScript& a, const MountedScript& b) { return a.path < b.path; });

    const scene::ClassId folderClass = w.classes().findId(w.atoms().lookup("Folder"));
    const scene::ClassId scriptClass = w.classes().findId(w.atoms().lookup("Script"));
    const scene::ClassId moduleClass = w.classes().findId(w.atoms().lookup("ModuleScript"));
    const core::NameAtom sourceProperty = w.atoms().intern("Source");

    // A container named as a service's class and then child names: the service
    // is found by class, as `GetService` finds it, and what is under it by name.
    const auto containerOf = [&](std::string_view path) -> core::InstanceId {
        const std::vector<std::string_view> parts = pathSegments(path);
        if (parts.empty())
            return {};
        core::InstanceId at = w.findFirstChildOfClass(context(L).services->dataModel,
                                                      w.classes().findId(w.atoms().lookup(parts.front())));
        for (usize index = 1; index < parts.size() && at.valid(); ++index)
            at = w.findFirstChild(at, w.atoms().lookup(parts[index]));
        return at;
    };

    for (const MountedScript& entry : ordered) {
        const std::vector<std::string_view> segments =
            pathSegments(entry.mountPath.empty() ? entry.path : entry.mountPath);
        if (segments.empty())
            continue;
        const core::InstanceId container = containerOf(entry.container);
        if (!container.valid())
            continue;

        core::InstanceId parent = container;
        for (usize index = 0; index + 1 < segments.size(); ++index) {
            const core::NameAtom name = w.atoms().intern(segments[index]);
            core::InstanceId folder = w.findFirstChild(parent, name);
            if (!folder.valid()) {
                folder = w.create(folderClass);
                w.setName(folder, name);
                (void)w.setParent(folder, parent);
                // The mount's, so a scene leaves it to the files (ADR 0092).
                w.setMounted(folder, true);
            }
            parent = folder;
        }

        // The file's name can decide the class (`.module.luau`), over the
        // folder's default.
        const bool module = moduleByFileName(segments.back()).value_or(entry.module);
        const core::InstanceId instance = w.create(module ? moduleClass : scriptClass);
        w.setName(instance, w.atoms().intern(scriptNameOfFile(segments.back())));
        (void)w.setParent(instance, parent);
        w.setMounted(instance, true);
        // **The file's text becomes the instance's `Source`** (ADR 0057), which
        // is what ADR 0050 decided and what the mount never did. The registry
        // keeps the PATH and not a second copy of the text: two places holding
        // one script is exactly the disagreement this milestone exists to end.
        (void)w.setProperty(instance, sourceProperty, scene::Value{entry.source});

        modules.entries.push_back(ModuleRegistry::Entry{entry.path, instance, scriptTextHash(entry.source)});
        made.push_back(instance);
    }

    // Consumed rather than queued: the tree was built before any script could
    // have connected to anything, so these are facts with no observer.
    (void)w.changes().take();
    return made;
}

std::string treePathOf(const scene::World& w, core::InstanceId id)
{
    std::vector<std::string_view> parts;
    for (core::InstanceId walk = id; walk.valid(); walk = w.parentOf(walk))
        parts.push_back(w.atoms().text(w.name(walk)));

    std::string out;
    // Skipping the DataModel itself, which is called `game` and says nothing.
    for (usize index = parts.size() - 1; index > 0; --index) {
        if (!out.empty())
            out.push_back('.');
        out.append(parts[index - 1]);
    }
    return out;
}

core::u64 scriptTextHash(std::string_view text) noexcept
{
    // FNV-1a: stable across runs and machines, which a `std::hash` is not
    // promised to be -- and it only has to tell two texts apart.
    core::u64 hash = 0xCBF29CE484222325ull;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001B3ull;
    }
    return hash;
}

void setMountedHash(lua_State* L, std::string_view path, core::u64 diskHash)
{
    for (ModuleRegistry::Entry& entry : registry(L).entries) {
        if (entry.path == path)
            entry.diskHash = diskHash;
    }
}

void setMountedPath(lua_State* L, core::InstanceId instance, std::string path, core::u64 diskHash)
{
    std::vector<ModuleRegistry::Entry>& entries = registry(L).entries;
    std::erase_if(entries,
                  [&](const ModuleRegistry::Entry& entry) { return entry.instance == instance || entry.path == path; });
    // Kept in path order, as the mount made it, so a start order read off
    // the table is the one opening the project would give (R10).
    const auto at = std::lower_bound(
        entries.begin(), entries.end(), path,
        [](const ModuleRegistry::Entry& entry, const std::string& wanted) { return entry.path < wanted; });
    entries.insert(at, ModuleRegistry::Entry{std::move(path), instance, diskHash});
}

void forgetMountedPath(lua_State* L, std::string_view path)
{
    std::erase_if(registry(L).entries, [&](const ModuleRegistry::Entry& entry) { return entry.path == path; });
}

std::string_view scriptNameOfFile(std::string_view fileName) noexcept
{
    for (const std::string_view suffix : {std::string_view{".luauc"}, std::string_view{".luau"}}) {
        if (fileName.size() > suffix.size() && fileName.ends_with(suffix)) {
            fileName.remove_suffix(suffix.size());
            break;
        }
    }
    for (const std::string_view kind : {std::string_view{".module"}, std::string_view{".script"}}) {
        if (fileName.size() > kind.size() && fileName.ends_with(kind)) {
            fileName.remove_suffix(kind.size());
            break;
        }
    }
    return fileName;
}

std::optional<bool> moduleByFileName(std::string_view fileName) noexcept
{
    for (const std::string_view suffix : {std::string_view{".luauc"}, std::string_view{".luau"}}) {
        if (fileName.size() > suffix.size() && fileName.ends_with(suffix)) {
            fileName.remove_suffix(suffix.size());
            break;
        }
    }
    if (fileName.ends_with(".module"))
        return true;
    if (fileName.ends_with(".script"))
        return false;
    return std::nullopt;
}

std::string_view mountedPathOf(lua_State* L, core::InstanceId instance)
{
    for (const ModuleRegistry::Entry& entry : registry(L).entries) {
        if (entry.instance == instance)
            return entry.path;
    }
    return {};
}

std::string scriptChunkName(lua_State* L, core::InstanceId instance)
{
    scene::World& w = world(L);
    if (!w.alive(instance))
        return {};
    if (const std::string_view path = mountedPathOf(L, instance); !path.empty())
        return std::string(path);
    return treePathOf(w, instance);
}

std::optional<ScriptSide> serviceSideOf(const scene::World& w, core::InstanceId instance)
{
    const scene::ClassId serverService = w.classes().findId(w.atoms().lookup("ServerScriptService"));
    const scene::ClassId clientService = w.classes().findId(w.atoms().lookup("ClientScriptService"));
    const scene::ClassId globalService = w.classes().findId(w.atoms().lookup("GlobalScriptService"));
    const core::NameAtom serverFolder = w.atoms().lookup("Server");
    const core::NameAtom clientFolder = w.atoms().lookup("Client");
    for (core::InstanceId walk = w.parentOf(instance); walk.valid(); walk = w.parentOf(walk)) {
        const scene::ClassId classId = w.classOf(walk);
        if (classId == serverService)
            return ScriptSide::Server;
        if (classId == clientService)
            return ScriptSide::Client;
        // The global service's fixed folders, and only those: a `Server` folder
        // somebody made elsewhere is a folder with a name.
        if (w.fixed(walk) && w.classOf(w.parentOf(walk)) == globalService) {
            if (w.name(walk) == serverFolder)
                return ScriptSide::Server;
            if (w.name(walk) == clientFolder)
                return ScriptSide::Client;
        }
    }
    return std::nullopt;
}

ScriptSide scriptSideOf(const scene::World& w, core::InstanceId instance)
{
    if (const std::optional<ScriptSide> side = serviceSideOf(w, instance); side.has_value())
        return *side;
    // **Anywhere else, the script's own `RunContext`** (ADR 0138 §2). A
    // `ModuleScript` carries the field unread: it runs where it is required.
    if (const scene::ScriptComponent* script = w.scripts().find(instance);
        script != nullptr && w.classOf(instance) == w.classes().findId(w.atoms().lookup("Script"))) {
        if (script->runContext == RunContextServer)
            return ScriptSide::Server;
        if (script->runContext == RunContextClient)
            return ScriptSide::Client;
    }
    return ScriptSide::Anywhere;
}

bool scriptSideRunsHere(const scene::World& w, ScriptSide side)
{
    const scene::NetworkTopology topology = w.engineState().networkTopology;
    switch (side) {
    case ScriptSide::Server:
        return topology != scene::NetworkTopology::Replica;
    case ScriptSide::Client:
        return topology != scene::NetworkTopology::Dedicated;
    case ScriptSide::Anywhere:
        break;
    }
    return true;
}

namespace {

// **Whose run a globals table is** (the script-sides audit, S3): a table in
// the registry, weak in its keys, from each run's globals table to its
// script. Read from here rather than from the `script` global, which a script
// can clear -- and one that did made its own threads unstoppable. Weak, so a
// run's table goes when nothing holds it any more, and its entry with it.
constexpr const char* RunOwnersKey = "engine.runOwners";

void pushRunOwners(lua_State* L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, RunOwnersKey);
    if (lua_istable(L, -1))
        return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_newtable(L);
    lua_pushstring(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, RunOwnersKey);
}

// The script whose run the table at `index` is, or an invalid id.
[[nodiscard]] core::InstanceId runOwnerOf(lua_State* L, int index)
{
    if (!lua_istable(L, index))
        return {};
    const int table = lua_absindex(L, index);
    pushRunOwners(L);
    lua_pushvalue(L, table);
    lua_rawget(L, -2);
    const core::InstanceId* id = toInstance(L, -1);
    const core::InstanceId out = id != nullptr ? *id : core::InstanceId{};
    lua_pop(L, 2);
    return out;
}

} // namespace

bool startScript(lua_State* L, core::InstanceId instance)
{
    ModuleRegistry& modules = registry(L);
    scene::World& w = world(L);
    const scene::ClassId scriptClass = w.classes().findId(w.atoms().lookup("Script"));

    // The exact class, not `IsA`: a `ModuleScript` shares `Source` with a
    // `Script` and deliberately does not start by itself.
    if (!w.alive(instance) || w.classOf(instance) != scriptClass)
        return false;

    // **Never started twice** (S0.5, ADR 0137 §2): a script that is running
    // keeps its run. A scene change used to start the scripts that survived
    // it -- a kept screen's, a player's -- again beside their first run.
    if (instance.index < modules.runs.size()) {
        const ModuleRegistry::Run& run = modules.runs[instance.index];
        if (run.generation == instance.generation && run.envRef != -1)
            return false;
    }

    // **Only a live script starts** (ADR 0137 §1): enabled, with code, in
    // the world, under no inert storage, its side running here (ADR 0105).
    if (!scriptLive(L, instance))
        return false;
    const std::optional<scene::Value> stored = w.getProperty(instance, w.atoms().intern("Source"));
    const auto* source = stored.has_value() ? std::get_if<std::string>(&stored.value()) : nullptr;
    if (source == nullptr)
        return false;

    // The file it was mounted from, when there was one; its place in the
    // tree otherwise. The same function the editor and the debugger use, so
    // that a breakpoint's key is the name the VM reports.
    const std::string chunkName = scriptChunkName(L, instance);

    lua_State* co = lua_newthread(L);
    const int rooted = lua_gettop(L);
    luaL_sandboxthread(co);

    // BEFORE the load. `luaL_sandboxthread` marks the new globals table
    // safeenv, which makes the compiler's import fast path resolve globals
    // at load time -- so a `script` set afterwards would be invisible to
    // every `script.Name` the chunk contains.
    pushInstance(co, instance);
    lua_setglobal(co, "script");

    // The run: this start's globals table, which every thread and handler the
    // script makes inherits.
    const auto recordRun = [&modules, co, instance] {
        if (modules.runs.size() <= instance.index)
            modules.runs.resize(static_cast<usize>(instance.index) + 1);
        ModuleRegistry::Run& run = modules.runs[instance.index];
        // **The slot's previous run is ended, not overwritten** (the
        // script-sides audit): a slot reused by another script -- a scene
        // change reuses the old scene's -- overwrote the reference and kept
        // every table it had ever held.
        if (run.envRef != -1)
            modules.ended.push_back(ModuleRegistry::Ended{run.env, run.envRef});
        lua_pushvalue(co, LUA_GLOBALSINDEX);
        run.env = lua_topointer(co, -1);
        run.envRef = lua_ref(co, -1);
        // Whose run this table is, kept where the script cannot reach it.
        pushRunOwners(co);
        lua_pushvalue(co, -2);
        pushInstance(co, instance);
        lua_rawset(co, -3);
        lua_pop(co, 2);
        run.generation = instance.generation;
    };

    std::string error;
    if (!loadChunk(co, *source, chunkName, error)) {
        const core::I18nArg args[] = {
            {"source", std::string_view{chunkName}},
            {"message", std::string_view{error}},
        };
        core::logText(core::LogLevel::Error, core::formatKeyPrefixed(ENG_TR("script.err.syntax"), args));
        ++modules.loadFailures;
        lua_remove(L, rooted);
        return false;
    }

    // **The chunk's own closure, remembered before it is consumed** (ADR 0057).
    // `lua_breakpoint` needs a function, `luaG_breakpoint` recurses into every
    // nested proto from it, and the resume below is what takes this one off the
    // stack -- so a breakpoint bound any later would have to recompile to find a
    // function, and would patch a `Proto` nothing is executing.
    if (Debugger* debugger = context(L).debugger; debugger != nullptr)
        debugger->bindChunk(L, co, chunkName, -1);

    recordRun();

    // Deferred rather than resumed here, so every entry script's first
    // resumption happens inside a drain and in the order they were mounted.
    const int threadRef = lua_ref(L, rooted);
    if (!enqueueTaskCallback(L, threadRef, 0, 0))
        (void)lua_unref(L, threadRef);
    lua_remove(L, rooted);
    return true;
}

namespace {

// What the start walks leave to the moved-scripts queue: every moved script
// running and no longer live stops, and one under `excluded` -- which the walk
// skipped -- starts if it became live.
void answerMoves(lua_State* L, const std::vector<core::InstanceId>& moved, core::InstanceId excluded)
{
    scene::World& w = world(L);
    for (const core::InstanceId script : moved) {
        if (scriptRunning(L, script) && !scriptLive(L, script))
            endRun(L, script);
    }
    if (!excluded.valid())
        return;
    for (const core::InstanceId script : moved) {
        if (w.alive(script) && w.isAncestorOf(excluded, script) && !scriptRunning(L, script))
            (void)startScript(L, script);
    }
}

} // namespace

void startScripts(lua_State* L)
{
    scene::World& w = world(L);
    registry(L).started = true;

    // **Every enabled Script in the WORLD, in document order** (ADR 0057). The
    // mounted-file list is no longer the population: a Script the scene brought,
    // or one somebody made in the editor, is a script and runs.
    //
    // `collectDescendants` is depth-first preorder -- "the same document order
    // the Find family tie-breaks on" -- which is what makes the start order a
    // property of the tree rather than of the pool. Pool order is an allocation
    // artefact and R10 forbids it reaching anything observable.
    std::vector<core::InstanceId> everything;
    w.collectDescendants(context(L).services->dataModel, everything);

    const std::vector<core::InstanceId> moved = w.takeMovedScripts();
    for (const core::InstanceId instance : everything)
        (void)startScript(L, instance);
    // Every script in the world was just weighed, so a move is answered only
    // by a stop: a script that failed to load is not tried again.
    answerMoves(L, moved, core::InstanceId{});

    lua_State* loaded = lua_newthread(L);
    const int rooted = lua_gettop(L);
    lua_pushcfunction(loaded, fireLoadedTrampoline, "loaded");
    const int loadedRef = lua_ref(L, rooted);
    if (!enqueueTaskCallback(L, loadedRef, 0, 0))
        (void)lua_unref(L, loadedRef);
    lua_remove(L, rooted);
}

void startScriptsExcept(lua_State* L, core::InstanceId excluded)
{
    scene::World& w = world(L);
    const std::vector<core::InstanceId> moved = w.takeMovedScripts();
    std::vector<core::InstanceId> everything;
    w.collectDescendants(context(L).services->dataModel, everything);
    for (const core::InstanceId instance : everything) {
        if (excluded.valid() && (instance == excluded || w.isAncestorOf(excluded, instance)))
            continue;
        (void)startScript(L, instance);
    }
    // **And the moves the walk did not weigh** (the script-sides audit): the
    // old scene's scripts, destroyed, whose runs end here; one taken out of
    // the world in the tick the scene changed; one put under `excluded`.
    answerMoves(L, moved, excluded);
}

core::InstanceId scriptOfThread(lua_State* thread)
{
    if (thread == nullptr)
        return {};
    lua_pushvalue(thread, LUA_GLOBALSINDEX);
    const core::InstanceId owner = runOwnerOf(thread, -1);
    lua_pop(thread, 1);
    if (owner.valid())
        return owner;
    lua_getglobal(thread, "script");
    const core::InstanceId* id = toInstance(thread, -1);
    const core::InstanceId out = id != nullptr ? *id : core::InstanceId{};
    lua_pop(thread, 1);
    return out;
}

core::InstanceId scriptOfFunction(lua_State* L, int index)
{
    if (!lua_isfunction(L, index))
        return {};
    // A C function has no Luau environment worth reading; `lua_getfenv` answers
    // with the globals in that case, and the `script` lookup below then finds
    // nothing, which is the right answer for a function the engine installed.
    lua_getfenv(L, index);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return {};
    }
    if (const core::InstanceId owner = runOwnerOf(L, -1); owner.valid()) {
        lua_pop(L, 1);
        return owner;
    }
    lua_getfield(L, -1, "script");
    const core::InstanceId* id = toInstance(L, -1);
    const core::InstanceId out = id != nullptr ? *id : core::InstanceId{};
    lua_pop(L, 2);
    return out;
}

SuppressReason suppressionFor(lua_State* L, core::InstanceId script)
{
    if (!script.valid())
        return SuppressReason::None;
    scene::World& w = world(L);
    // **A destroyed script is stopped** (D097). The `Script` class already
    // documents this -- "one that is not in the world does not run, which is the
    // whole difference between storing a script and using it" -- and the
    // sentence was false in one direction: a destroyed script is not in the
    // world and its threads went on being resumed, forever, with nothing left
    // that could ever stop them.
    //
    // Decided rather than inherited, and it decides only this: `Enabled = false`
    // already suppresses resumption, and it would be incoherent for the weaker
    // operation to stop a script and the stronger one not to. What the script
    // already built stays, exactly as rule 2 says for `Enabled` -- a part it
    // made before it died is a part, not a script.
    //
    // Asked before the class, because a retired instance has none to ask. The
    // `script` global is set by the engine when it starts one, so an id that is
    // valid, dead and reached this function named a script that used to exist.
    // Destroyed-but-not-retired counts: `World::destroy` unlinks and marks, and
    // a paused world may not drain for many frames.
    if (!w.alive(script))
        return SuppressReason::Retired;
    if (w.destroyed(script))
        return SuppressReason::Destroyed;
    if (w.classOf(script) != w.classes().findId(w.atoms().lookup("Script")))
        return SuppressReason::None;
    const std::optional<scene::Value> value = w.getProperty(script, w.atoms().intern("Enabled"));
    if (!value.has_value())
        return SuppressReason::None;
    const auto* flag = std::get_if<bool>(&value.value());
    return flag != nullptr && !*flag ? SuppressReason::Disabled : SuppressReason::None;
}

bool resumptionSuppressed(lua_State* L, core::InstanceId script)
{
    return suppressionFor(L, script) != SuppressReason::None;
}

SuppressReason suppressionFor(lua_State* L, core::InstanceId script, const void* env)
{
    const SuppressReason reason = suppressionFor(L, script);
    if (reason != SuppressReason::None || !script.valid() || env == nullptr)
        return reason;
    // **A `Script`'s thread or handler runs only in its current run**: the old
    // run of a script disabled and enabled again, and every run of one that
    // left the world, stay stopped. Only a `Script`: a module's table carries
    // its `ModuleScript`, which has no run and is not stopped by one.
    scene::World& w = world(L);
    if (w.classOf(script) != w.classes().findId(w.atoms().lookup("Script")))
        return SuppressReason::None;
    const ModuleRegistry& modules = registry(L);
    if (script.index < modules.runs.size()) {
        const ModuleRegistry::Run& run = modules.runs[script.index];
        if (run.generation == script.generation && run.envRef != -1 && run.env == env)
            return SuppressReason::None;
    }
    return SuppressReason::Ended;
}

namespace {

// The globals table on top of `L`'s stack as a run's identity, or null when it
// is no run's: a module's table, or the one a function an `@engine` module made
// for a script runs in (a camera rig's render step) -- comparing that with a
// run would stop it. A run's table is the one `runOwnerOf` knows. Pops the
// table.
[[nodiscard]] const void* ownRunEnv(lua_State* L)
{
    const void* env = runOwnerOf(L, -1).valid() ? lua_topointer(L, -1) : nullptr;
    lua_pop(L, 1);
    return env;
}

} // namespace

const void* runEnvOfThread(lua_State* thread)
{
    if (thread == nullptr)
        return nullptr;
    lua_pushvalue(thread, LUA_GLOBALSINDEX);
    return ownRunEnv(thread);
}

const void* runEnvOfFunction(lua_State* L, int index)
{
    if (!lua_isfunction(L, index))
        return nullptr;
    lua_getfenv(L, index);
    return ownRunEnv(L);
}

bool scriptLive(lua_State* L, core::InstanceId instance)
{
    scene::World& w = world(L);
    if (!w.alive(instance) || w.destroyed(instance))
        return false;
    if (w.classOf(instance) != w.classes().findId(w.atoms().lookup("Script")))
        return false;
    const core::InstanceId dataModel = context(L).services->dataModel;
    if (!dataModel.valid() || !w.isAncestorOf(dataModel, instance))
        return false;
    // **Storage does not run** (ADR 0137 §3): a template kept there must not
    // run itself.
    const scene::ClassId serverStorage = w.classes().findId(w.atoms().lookup("ServerStorage"));
    const scene::ClassId replicatedStorage = w.classes().findId(w.atoms().lookup("ReplicatedStorage"));
    for (core::InstanceId walk = w.parentOf(instance); walk.valid(); walk = w.parentOf(walk)) {
        const scene::ClassId classId = w.classOf(walk);
        if (classId == serverStorage || classId == replicatedStorage)
            return false;
    }
    if (!scriptSideRunsHere(w, scriptSideOf(w, instance)))
        return false;
    // **A scene's client code waits for its world** (D433): while a join is
    // under way, what is under `ClientScriptService` does not run.
    if (w.engineState().sceneClientHeld) {
        const scene::ClassId clientScripts = w.classes().findId(w.atoms().lookup("ClientScriptService"));
        for (core::InstanceId walk = w.parentOf(instance); walk.valid(); walk = w.parentOf(walk)) {
            if (w.classOf(walk) == clientScripts)
                return false;
        }
    }
    // A Script whose `Enabled` is false never starts -- it is still in the tree,
    // but no coroutine is created for it (api-design.md 3).
    const std::optional<scene::Value> enabled = w.getProperty(instance, w.atoms().intern("Enabled"));
    if (enabled.has_value()) {
        if (const auto* flag = std::get_if<bool>(&enabled.value()); flag != nullptr && !*flag)
            return false;
    }
    // Nothing to run is not a failure. An empty Script is what somebody has
    // the moment they create one, and reporting it would put an error in the
    // log for every script anybody starts writing.
    const std::optional<scene::Value> stored = w.getProperty(instance, w.atoms().intern("Source"));
    const auto* source = stored.has_value() ? std::get_if<std::string>(&stored.value()) : nullptr;
    return source != nullptr && !source->empty();
}

bool scriptRunning(lua_State* L, core::InstanceId script)
{
    const ModuleRegistry& modules = registry(L);
    if (!script.valid() || script.index >= modules.runs.size())
        return false;
    const ModuleRegistry::Run& run = modules.runs[script.index];
    return run.generation == script.generation && run.envRef != -1;
}

void reconcileScripts(lua_State* L, const std::vector<core::InstanceId>& moved,
                      const std::vector<core::InstanceId>& enabled)
{
    if (moved.empty() && enabled.empty())
        return;
    // Before the world's scripts have started, a move is an edit: nothing
    // starts, and nothing has a run to stop.
    const bool started = registry(L).started;
    // **Stopped: what is no longer live** -- moved out of the world, into
    // storage, destroyed, onto a side this machine does not run.
    for (const core::InstanceId script : moved) {
        if (scriptRunning(L, script) && !scriptLive(L, script))
            endRun(L, script);
    }
    // **Re-enabled: a stop and a fresh run** (ADR 0137 §2), in document order,
    // as boot starts scripts. The walk is the whole world, and only an
    // `Enabled` write pays it; sorted so that a thousand writes are not a
    // thousand-by-world scan.
    // Not before the world's scripts have started: in the editor, with the
    // game stopped, `Enabled` is a scene edit (ADR 0058; the audit found an
    // edit started the script).
    if (started && !enabled.empty()) {
        const auto before = [](core::InstanceId a, core::InstanceId b) {
            return a.index != b.index ? a.index < b.index : a.generation < b.generation;
        };
        std::vector<core::InstanceId> enabledSorted = enabled;
        std::sort(enabledSorted.begin(), enabledSorted.end(), before);
        std::vector<core::InstanceId> everything;
        world(L).collectDescendants(context(L).services->dataModel, everything);
        for (const core::InstanceId instance : everything) {
            if (std::binary_search(enabledSorted.begin(), enabledSorted.end(), instance, before)) {
                endRun(L, instance);
                (void)startScript(L, instance);
            }
        }
    }
    if (!started)
        return;
    // **Moved in: what became live**, in the order of the moves, and within one
    // moved subtree in its document order -- `setParent` queues a subtree's
    // scripts in the preorder it walks it in. Deterministic (R10) without a
    // walk of the world, which a game cloning one scripted projectile a frame
    // would otherwise pay every frame. A script queued twice starts once: the
    // second finds it running.
    for (const core::InstanceId script : moved) {
        if (!scriptRunning(L, script))
            (void)startScript(L, script);
    }
}

void reconcileAllScripts(lua_State* L)
{
    if (!registry(L).started)
        return;
    scene::World& w = world(L);
    std::vector<core::InstanceId> everything;
    w.collectDescendants(context(L).services->dataModel, everything);
    const scene::ClassId scriptClass = w.classes().findId(w.atoms().lookup("Script"));
    // Stopped first, then started, each in document order.
    for (const core::InstanceId instance : everything) {
        if (w.classOf(instance) == scriptClass && scriptRunning(L, instance) && !scriptLive(L, instance))
            endRun(L, instance);
    }
    for (const core::InstanceId instance : everything) {
        if (w.classOf(instance) == scriptClass && !scriptRunning(L, instance))
            (void)startScript(L, instance);
    }
}

void endRun(lua_State* L, core::InstanceId script)
{
    ModuleRegistry& modules = registry(L);
    if (!script.valid() || script.index >= modules.runs.size())
        return;
    ModuleRegistry::Run& run = modules.runs[script.index];
    if (run.generation != script.generation)
        return;
    // Stopped now -- nothing of it is resumed from here on -- and cleaned up
    // after the drain, the table held until then (`finishEndedRuns`).
    if (run.envRef != -1)
        modules.ended.push_back(ModuleRegistry::Ended{run.env, run.envRef});
    run = ModuleRegistry::Run{};
}

void finishEndedRuns(lua_State* L)
{
    ModuleRegistry& modules = registry(L);
    if (modules.ended.empty())
        return;
    const std::vector<ModuleRegistry::Ended> ended = std::exchange(modules.ended, {});
    std::vector<const void*> runs;
    runs.reserve(ended.size());
    for (const ModuleRegistry::Ended& run : ended) {
        disconnectRun(L, run.env);
        runs.push_back(run.env);
    }
    dropTimersOf(L, runs);
    for (const ModuleRegistry::Ended& run : ended)
        (void)lua_unref(L, run.envRef);
}

std::vector<ModuleRegistry::Entry> mountedEntries(lua_State* L)
{
    return registry(L).entries;
}

void adoptMountedEntries(lua_State* L, std::vector<ModuleRegistry::Entry> entries)
{
    // Assigned rather than appended: the caller is handing over the whole table
    // a previous VM held, and a fresh VM that had already mounted something
    // would end up with each script twice.
    registry(L).entries = std::move(entries);
}

usize mountedScriptCount(lua_State* L)
{
    return registry(L).entries.size();
}

usize scriptLoadFailures(lua_State* L)
{
    return registry(L).loadFailures;
}

} // namespace engine::script
