// Help before a script runs on the wrong side (ADR 0138 §8).
//
// **One definition, two readers**: the script editor underlines what this
// finds as the text comes to rest, and `ludwerk check` runs it over a
// project's scenes, stamps, `global.json` and `src/` through `engine-host
// --check-sides`. Words, not types: a client script that names
// `ServerStorage` is worth a warning whatever it does with it, because on a
// client that joined there is nothing there.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/script/modules.h"

namespace engine::app {

struct SideFinding
{
    // Zero-based. A finding about the whole script has length 0 at 0:0.
    core::u32 line = 0;
    core::u32 column = 0;
    core::u32 length = 0;
    core::TextKey key;
    // The word found, for the message's `{word}`.
    std::string word;
};

// **What a `Script` of `side` should be told** (ADR 0138 §8), in a project
// whose `[export] multiplayer` is not `none` -- a solo game has one side:
// - a client-side script that names `ServerStorage` or `ServerScriptService`
//   works solo and finds nothing on a client that joined;
// - a server-side script that names `CurrentCamera`, `UIService` or
//   `InputService` asks for what a dedicated server has none of;
// - a `Shared` script outside the services that never names `Authority` runs
//   on the server and on every player alike.
// Comments are skipped; strings are not, since `GetService("UIService")` names
// the service in one.
[[nodiscard]] std::vector<SideFinding> lintScriptSide(std::string_view source, script::ScriptSide side,
                                                      bool decidedByService, bool multiplayer);

// Whether the project at `root` says `[export] multiplayer` is not `none`.
[[nodiscard]] bool projectIsMultiplayer(const std::filesystem::path& root);

// **`engine-host --check-sides <project>`**: every script a project carries --
// in its scenes, its stamps, `global.json` and `src/` -- linted, and each
// finding printed as `file:line:column: warning: message`. Returns how many.
core::usize checkProjectSides(const std::filesystem::path& root);

} // namespace engine::app
