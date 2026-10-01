// Engine logging. Every engine-originated message is named by a TextKey and
// formatted through the catalog (rule R3) -- there is no overload taking an
// engine-authored string, because that is exactly the hole R3 exists to close.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/i18n.h"
#include "engine/core/text_key.h"
#include "engine/core/types.h"

namespace engine::core {

enum class LogLevel : u8
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

[[nodiscard]] std::string_view logLevelName(LogLevel level) noexcept;

// Receives already-formatted text. Installed by the host so tests and the
// future DebugShell can capture output.
using LogSink = std::function<void(LogLevel, std::string_view)>;

// The `[level] text\n` line the built-in writer emits. Exposed so a sink that
// replaces where output goes -- not what it looks like -- does not have to
// re-derive the format and drift from it.
[[nodiscard]] std::string formatLogLine(LogLevel level, std::string_view text);

// Returns the sink being replaced, so a caller that needs to observe the stream
// without owning it -- the replay harness counting errors, a test capturing
// output -- can wrap what was there and put it back. Without this, "install a
// sink temporarily" means "silence whatever the host installed".
LogSink setLogSink(LogSink sink);
void resetLogSink();

// Also writes every line to `path`, in addition to wherever it already goes.
//
// **An addition, never a replacement.** Every gate, the conformance runner and
// the replay harness read stdout, and a sink that redirected would take their
// eyes rather than give a human theirs. The file receives lines even while a
// sink is installed, because a captured stream is exactly when a person running
// the engine by hand would otherwise see nothing.
//
// The path is INJECTED rather than resolved. `core` is L0 and
// `platform::paths()` is L1, so this module cannot ask where anything belongs;
// `app` decides at boot and prints where it went, because a log nobody can find
// is a log nobody sends.
//
// Returns false when the file cannot be opened, which is not fatal to anything:
// the console sink is untouched and the caller reports it.
[[nodiscard]] bool openLogFile(const std::filesystem::path& path);

// **The log at `path`, or the one beside it another run is not writing**
// (the owner's queue, Q0). A log another process holds -- a second run from
// the same folder -- is left to it: this run writes `stem_2`, `stem_3`, ...
// up to `stem_9`, as the major engines number theirs. The one it takes is
// rotated first, its previous contents kept as `stem.previous` (audit A15),
// and only when nobody holds it. Which one it took, or nothing when none
// would open.
[[nodiscard]] std::optional<std::filesystem::path> openLogFileBeside(const std::filesystem::path& path);

// Flushes and closes it. Idempotent, and safe without a preceding open.
void closeLogFile() noexcept;

void log(LogLevel level, TextKey key, std::span<const I18nArg> args = {});

// Verbatim passthrough for text that did NOT originate in the engine: script
// `print`/`error` output and other user-authored strings, which must not be
// translated (architecture.md §5). Engine code must use log() instead -- the
// i18n lint treats a literal reaching this from engine sources as a violation.
void logText(LogLevel level, std::string_view text);

// **The same, with a structure beside the text** (the owner: a printed table
// opens and closes in the console). `detail` goes to whichever sink asks for
// it with `logDetail` during its call and to nobody else: the log FILE and
// every other sink get the text alone, so nothing that reads the log changes.
void logText(LogLevel level, std::string_view text, std::string_view detail);

// During a sink's call, the detail its line was logged with; empty otherwise.
[[nodiscard]] std::string_view logDetail() noexcept;

} // namespace engine::core
