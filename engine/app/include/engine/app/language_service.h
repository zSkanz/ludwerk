#pragma once

// The script editor's language service (ADR 0093): Luau's own type checker,
// at the pinned Luau, reading the engine's generated definitions and a
// snapshot of the scripts in the world.
//
// **Two halves, and the line between them is the thread.** `LanguageTree` is
// taken on the main thread from the world -- every script, every ancestor of
// one, and the text of each -- and handed over whole; the world never crosses
// to the worker. `LanguageCore` is the checker over one such snapshot, and it
// is synchronous, which is what the tests drive. `LanguageService` runs a core
// on a worker so a frame never waits on a check.
//
// Built only where the editor is (`ENG_DEBUG_UI`): a shipped game carries no
// Analysis (ADR 0002).

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/app/require_paths.h"
#include "engine/app/script_complete.h"
#include "engine/app/script_document.h"
#include "engine/core/id.h"
#include "engine/core/types.h"

namespace engine::scene {
class World;
}

namespace engine::app {

// **The part of the world a require can walk**: each script, and each
// ancestor of one, by name and class, with a script's text. A require names a
// module by walking from `game`, `workspace` or `script` through names,
// `Parent` and `GetService` / `WaitForChild` / `FindFirstChild` -- and every
// step of such a walk is an ancestor of the module it reaches or of the script
// it starts from, so nothing else is needed.
//
// **And what a require by PATH needs** (`require("../shared/ring")`,
// `require("@engine/settings")`), which is how a project laid out as files
// names its modules: the file each script was mounted from, the project's
// folder for a module no scene mounts, its `.luaurc` aliases, and where the
// engine's own modules are. The rule is `resolveRequire`'s, the one the world
// host runs a require by -- the checker took a path for something it could not
// know, and a project that requires by path was a page of unknown types.
struct LanguageTree
{
    struct Node
    {
        std::string name;
        std::string className;
        // Index of the parent in `nodes`, or -1 for the DataModel.
        core::i32 parent = -1;
        std::vector<core::u32> children;
        // The dotted path from `game`, which is also the module's name.
        std::string path;
        // Set for a `Script` or `ModuleScript`.
        bool script = false;
        bool module = false;
        std::string source;
        // The instance it was taken from, which is how a tab finds its module.
        core::InstanceId id;
        // The file it was mounted from, relative to the project
        // (`src/shared/ring.luau`), or empty for a script a scene carries.
        std::string file;
    };
    std::vector<Node> nodes;
    std::map<std::string, std::string, std::less<>> optionalServices;
    std::vector<std::string> enabledIntegrations;

    // The project's folder; empty when there is none, and a path then names
    // nothing.
    std::filesystem::path projectRoot;
    RequireAliases aliases;
    // `content/runtime`: `@engine/camera` is `engine/camera/init.luau` under
    // it, and `@std/net` is `std/net/init.luau`.
    std::filesystem::path libraryRoot;

    [[nodiscard]] std::optional<core::u32> find(std::string_view path) const;
    // The module name of `id`, or empty when it is not a script in the tree.
    [[nodiscard]] std::string pathOf(core::InstanceId id) const;
};

// What the tree cannot be asked: see `LanguageTree`.
struct LanguageFiles
{
    std::filesystem::path projectRoot;
    RequireAliases aliases;
    std::filesystem::path libraryRoot;
    // The file a script was mounted from, relative to the project, or empty.
    std::function<std::string(core::InstanceId)> fileOf;
};

// Walks `world` from `dataModel`. A tab writes `Source` as it is typed (ADR
// 0057), so the world's text is the buffer's. With no `files`, a require by
// path names nothing, which is a world with no project.
[[nodiscard]] LanguageTree captureLanguageTree(const scene::World& world, core::InstanceId dataModel,
                                               const LanguageFiles* files = nullptr);

// What one check of one module found.
struct LanguageCheck
{
    std::vector<Diagnostic> diagnostics;
};

// What completion found at a caret.
struct LanguageCompletions
{
    std::vector<Completion> items;
    // The caret is where a TYPE is written (`x :: Snake.`), so a value's
    // members are the wrong answer there even when the checker has none.
    bool inType = false;
};

class LanguageCore
{
public:
    // `definitions` is the text of `engine.d.luau`. A core whose definitions
    // do not load still answers, as plain Luau, and says why in `loadError`.
    explicit LanguageCore(std::string_view definitions);
    ~LanguageCore();
    LanguageCore(const LanguageCore&) = delete;
    LanguageCore& operator=(const LanguageCore&) = delete;

    [[nodiscard]] const std::string& loadError() const noexcept;

    // Replaces the snapshot; a module whose text changed is checked again, and
    // so is everything that requires it.
    void update(LanguageTree tree);

    [[nodiscard]] LanguageCheck check(const std::string& module);
    // Completion at `at` in `module`, empty when the checker has nothing to
    // offer there (inside a comment, say).
    [[nodiscard]] LanguageCompletions complete(const std::string& module, Position at);
    [[nodiscard]] std::optional<SignatureHelp> signature(const std::string& module, Position at);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// A core on a worker thread. Requests coalesce -- only the newest of each kind
// matters -- and answers are picked up by the frame that asks next.
class LanguageService
{
public:
    explicit LanguageService(std::string definitions);
    ~LanguageService();
    LanguageService(const LanguageService&) = delete;
    LanguageService& operator=(const LanguageService&) = delete;

    // Each carries the document revision it was asked at, and an answer comes
    // back with it, so a stale one is recognised rather than shown.
    void requestCheck(LanguageTree tree, std::string module, core::u64 revision);
    void requestCompletion(LanguageTree tree, std::string module, Position at, core::u64 revision);
    void requestSignature(LanguageTree tree, std::string module, Position at, core::u64 revision);

    struct Answer
    {
        std::string module;
        core::u64 revision = 0;
        Position at;
    };
    struct CheckAnswer : Answer
    {
        LanguageCheck check;
    };
    struct CompletionAnswer : Answer
    {
        LanguageCompletions completions;
    };
    struct SignatureAnswer : Answer
    {
        std::optional<SignatureHelp> signature;
    };
    [[nodiscard]] std::optional<CheckAnswer> takeCheck();
    [[nodiscard]] std::optional<CompletionAnswer> takeCompletion();
    [[nodiscard]] std::optional<SignatureAnswer> takeSignature();

private:
    enum class Kind : core::u8
    {
        Check,
        Completion,
        Signature,
    };
    struct Request
    {
        LanguageTree tree;
        std::string module;
        Position at;
        core::u64 revision = 0;
    };
    void run();

    std::string m_definitions;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::optional<Request> m_pending[3];
    std::optional<CheckAnswer> m_check;
    std::optional<CompletionAnswer> m_completion;
    std::optional<SignatureAnswer> m_signature;
    bool m_stopping = false;
    std::thread m_worker;
};

} // namespace engine::app
