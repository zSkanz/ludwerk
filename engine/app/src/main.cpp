// engine-host -- the M0 engine host: boot a sandboxed Luau VM, run one script,
// report failures as structured, key-identified engine errors.

#include <Luau/Bytecode.h>
#include <lua.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/app/backends.h"
#include "engine/app/bench.h"
#include "engine/app/engine.h"
#include "engine/app/project_config.h"
#include "engine/app/replay.h"
#include "engine/app/script_package.h"
#include "engine/app/script_sides.h"
#include "engine/app/two_worlds.h"
#include "engine/core/build_info.h"
#include "engine/core/error.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/platform/console.h"
#include "engine/platform/crash.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"
#include "engine/platform/stop_signal.h"

#if defined(_WIN32) && defined(ENG_GUI_SUBSYSTEM)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using engine::core::I18nArg;
using engine::core::LogLevel;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitScriptError = 1;
constexpr int kExitNoCatalog = 3;

// The port a networked posture uses when none is given. Any unprivileged number
// would do; a fixed one means `--host` and `--join=address` meet without either
// person having to agree on anything.
constexpr engine::core::u16 kDefaultGamePort = 7777;

// Distinct because "this machine has no usable GPU" is not a failure of
// anything under test. CTest maps it to SKIP, so a runner without a driver
// reports a skipped render test instead of a red build nobody can act on.
constexpr int kExitNoGraphicsDevice = 4;

// The graphics device was lost while the engine ran -- a driver reset, most
// often a shader that ran too long. Its own code so a test can tell "survived
// and said so" from a crash or a script error.
constexpr int kExitDeviceLost = 5;

// Catalogs are UTF-8 (ADR 0019); a Windows console decodes raw byte writes with
// its own codepage and mangles anything non-ASCII, which would quietly reduce
// "adding a locale is adding a file" to "adding a locale nobody on Windows can
// read". `platform::writeConsole` is the fix, and routing the log through it is
// how every engine message gets it.
void installConsoleLogSink()
{
    engine::core::setLogSink([](LogLevel level, std::string_view text) {
        // Warnings and errors go to stderr so a headless CI run can
        // separate them from ordinary output without parsing.
        const auto stream = (level == LogLevel::Warn || level == LogLevel::Error)
                                ? engine::platform::ConsoleStream::Err
                                : engine::platform::ConsoleStream::Out;
        engine::platform::writeConsole(stream, engine::core::formatLogLine(level, text));
    });
}

// The one place a user-facing string may be hardcoded: if the catalog itself
// failed to load there is, by definition, nothing to translate through. Kept
// deliberately to stderr and to this single call site (ADR 0019).
void reportCatalogFailure(const std::string& diagnostic)
{
    engine::platform::writeConsole(engine::platform::ConsoleStream::Err,
                                   "engine-host: cannot load the message catalog: " + diagnostic + "\n");
}

void printVersion()
{
    const std::array<I18nArg, 2> engineArgs{I18nArg{"version", ENG_VERSION_STRING},
                                            I18nArg{"profile", ENG_PROFILE_NAME}};
    engine::core::log(LogLevel::Info, ENG_TR("engine.cli.version.engine"), engineArgs);

    // Version and commit come from third_party/manifest.json via the generated
    // provenance header (ADR 0031) -- Luau itself ships no version constant.
    const std::array<I18nArg, 2> luauArgs{I18nArg{"version", ENG_LUAU_VERSION}, I18nArg{"commit", ENG_LUAU_COMMIT}};
    engine::core::log(LogLevel::Info, ENG_TR("engine.cli.version.luau"), luauArgs);

    // These come from the vendored headers at compile time, so they describe
    // the VM actually linked into this binary rather than what we intended.
    const std::array<I18nArg, 4> abiArgs{
        I18nArg{"bytecode", static_cast<engine::core::i64>(LBC_VERSION_TARGET)},
        I18nArg{"types", static_cast<engine::core::i64>(LBC_TYPE_VERSION_TARGET)},
        I18nArg{"vectorSize", static_cast<engine::core::i64>(LUA_VECTOR_SIZE)},
        I18nArg{"vectorPrecision", LUA_VECTOR_DOUBLE ? std::string_view{"f64"} : std::string_view{"f32"}}};
    engine::core::log(LogLevel::Info, ENG_TR("engine.cli.version.abi"), abiArgs);
}

// Fills `options` from the command line. Returns kExitOk when the caller should
// proceed, or the exit code to return.
//
// Deliberately hand-rolled and small: a getopt-style dependency for six flags
// would be a dependency (R5) bought for nothing, and the engine's real CLI is
// `ludwerk` (M3), which lives in Lute and is the thing users will actually type.
// This is the host's own switchboard.
int parseOptions(std::span<const std::string_view> args, engine::app::EngineOptions& options,
                 engine::app::GraphicsOverrides& graphics, bool& sizeFromFlags)
{
    const auto numericValue = [](std::string_view text, engine::core::u64& out) {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    };

    // `strtod` rather than `from_chars`, for the reason `core/json.cpp` records:
    // the floating-point overloads of from_chars are missing from one of the
    // standard libraries this engine builds against. The WHOLE token has to
    // convert, so `--render-scale=0.75x` is a usage error rather than 0.75.
    const auto decimalValue = [](std::string_view text, double& out) {
        const std::string buffer(text);
        char* end = nullptr;
        const double value = std::strtod(buffer.c_str(), &end);
        if (end != buffer.c_str() + buffer.size())
            return false;
        out = value;
        return true;
    };

    for (const std::string_view arg : args) {
        if (!arg.empty() && arg.front() != '-') {
            options.scriptPath = std::filesystem::path(arg);
            continue;
        }

        if (arg == "--headless") {
            options.headless = true;
            continue;
        }
        // **The three networked postures** (ADR 0070). Each is a flag rather
        // than a subcommand because each is still the same game -- the same
        // project, the same scene -- run in a different relationship to other
        // copies of itself. One posture per process: two is a usage error.
        if (arg == "--host" || arg.starts_with("--host=") || arg == "--serve" || arg.starts_with("--serve=")) {
            const bool serve = arg.starts_with("--serve");
            if (options.network.topology != engine::replication::Topology::Solo) {
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.one_posture"));
                return kExitUsage;
            }
            options.network.topology =
                serve ? engine::replication::Topology::Dedicated : engine::replication::Topology::Host;
            options.network.port = kDefaultGamePort;
            if (const std::size_t equals = arg.find('='); equals != std::string_view::npos) {
                engine::core::u64 port = 0;
                if (!numericValue(arg.substr(equals + 1), port) || port == 0 || port > 65535) {
                    engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_port"));
                    return kExitUsage;
                }
                options.network.port = static_cast<engine::core::u16>(port);
            }
            // A dedicated server has no window by definition.
            if (serve)
                options.headless = true;
            continue;
        }
        if (arg.starts_with("--join=")) {
            if (options.network.topology != engine::replication::Topology::Solo) {
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.one_posture"));
                return kExitUsage;
            }
            std::string_view target = arg.substr(std::string_view("--join=").size());
            options.network.topology = engine::replication::Topology::Replica;
            options.network.port = kDefaultGamePort;
            if (const std::size_t colon = target.rfind(':'); colon != std::string_view::npos) {
                engine::core::u64 port = 0;
                if (!numericValue(target.substr(colon + 1), port) || port == 0 || port > 65535) {
                    engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_port"));
                    return kExitUsage;
                }
                options.network.port = static_cast<engine::core::u16>(port);
                target = target.substr(0, colon);
            }
            if (target.empty()) {
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_port"));
                return kExitUsage;
            }
            options.network.address = std::string(target);
            continue;
        }
        if (arg.starts_with("--max-players=")) {
            engine::core::u64 count = 0;
            if (!numericValue(arg.substr(std::string_view("--max-players=").size()), count) || count == 0 ||
                count > 4095) {
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_max_players"));
                return kExitUsage;
            }
            options.network.maxPeers = static_cast<engine::core::u32>(count);
            continue;
        }
        if (arg == "--exit") {
            options.exitAfterFrames = true;
            continue;
        }
        if (arg == "--launcher") {
            options.launcher = true;
            continue;
        }
        if (arg == "--edit") {
            options.editor = true;
            continue;
        }
        if (arg == "--frame-stats") {
            options.frameStats = true;
            continue;
        }
        if (arg.starts_with("--pace=")) {
            engine::core::u64 hz = 0;
            if (!numericValue(arg.substr(7), hz) || hz == 0 || hz > 1000) {
                const std::array<I18nArg, 1> badValue{I18nArg{"option", arg}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            options.paceHz = static_cast<engine::core::u32>(hz);
            continue;
        }
        if (arg == "--vsync" || arg == "--no-vsync") {
            graphics.vsync = arg == "--vsync";
            continue;
        }
        if (arg.starts_with("--max-frame-rate=") || arg.starts_with("--background-frame-rate=")) {
            const bool background = arg.starts_with("--background-frame-rate=");
            engine::core::u64 rate = 0;
            if (!numericValue(arg.substr(arg.find('=') + 1), rate) || rate > 1000) {
                const std::array<I18nArg, 1> badValue{I18nArg{"option", arg}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            (background ? graphics.backgroundFrameRate : graphics.maxFrameRate) = static_cast<engine::core::u32>(rate);
            continue;
        }
        if (arg == "--gpu-debug") {
            options.gpuDebug = true;
            continue;
        }

        // M7's gate. The report path turns the recorder on; the ceiling is
        // separate because a soak that only wants the histogram should not have
        // to invent a memory number to get one.
        if (arg.starts_with("--soak-report=")) {
            options.soakReportPath = std::filesystem::path(arg.substr(arg.find('=') + 1));
            continue;
        }
        if (arg.starts_with("--soak-ceiling-mb=")) {
            const std::string_view value = arg.substr(arg.find('=') + 1);
            engine::core::u64 parsed = 0;
            if (!numericValue(value, parsed) || parsed == 0) {
                const std::array<I18nArg, 2> badValue{I18nArg{"option", arg}, I18nArg{"value", value}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            options.soakCeilingBytes = parsed * 1024 * 1024;
            continue;
        }
        if (arg.starts_with("--soak-return-radius=")) {
            // Whole metres, like every other numeric flag here parses: a radius
            // in centimetres is not a thing a fly-through declares, and the
            // shared `numericValue` is what keeps a bad value a NAMED error
            // rather than a silent zero -- which for this option would turn the
            // check off instead of failing.
            const std::string_view value = arg.substr(arg.find('=') + 1);
            engine::core::u64 parsed = 0;
            if (!numericValue(value, parsed) || parsed == 0) {
                const std::array<I18nArg, 2> badValue{I18nArg{"option", arg}, I18nArg{"value", value}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            options.soakReturnRadiusMetres = static_cast<engine::core::f32>(parsed);
            continue;
        }
        if (arg.starts_with("--soak-min-instances=")) {
            const std::string_view value = arg.substr(arg.find('=') + 1);
            engine::core::u64 parsed = 0;
            if (!numericValue(value, parsed) || parsed == 0) {
                const std::array<I18nArg, 2> badValue{I18nArg{"option", arg}, I18nArg{"value", value}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            options.soakMinimumInstances = parsed;
            continue;
        }
        if (arg.starts_with("--soak-min-ground=")) {
            const std::string_view value = arg.substr(arg.find('=') + 1);
            engine::core::u64 parsed = 0;
            if (!numericValue(value, parsed) || parsed == 0) {
                const std::array<I18nArg, 2> badValue{I18nArg{"option", arg}, I18nArg{"value", value}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            options.soakMinimumGroundCells = parsed;
            continue;
        }
        if (arg.starts_with("--soak-memory-growth=") || arg.starts_with("--soak-frame-p99-ms=")) {
            const std::string_view value = arg.substr(arg.find('=') + 1);
            engine::core::u64 parsed = 0;
            if (!numericValue(value, parsed) || parsed == 0) {
                const std::array<I18nArg, 2> badValue{I18nArg{"option", arg}, I18nArg{"value", value}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            (arg.starts_with("--soak-memory-growth=") ? options.soakMemoryGrowthPercent : options.soakFrameP99Ms) =
                parsed;
            continue;
        }
        // The render target's size. Windowed it is the window; headless it is the
        // offscreen texture. The M4 gate records a frame-time baseline at 1080p
        // and the host had no way to be asked for one.
        if (arg.starts_with("--width=") || arg.starts_with("--height=")) {
            const std::string_view value = arg.substr(arg.find('=') + 1);
            engine::core::u64 parsed = 0;
            // Bounded rather than merely positive: a target larger than any GPU
            // will allocate fails inside the backend with a message about
            // memory, which is a long way from "you typed a silly number".
            if (!numericValue(value, parsed) || parsed == 0 || parsed > 16384) {
                const std::array<I18nArg, 2> badValue{I18nArg{"option", arg}, I18nArg{"value", value}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            (arg.starts_with("--width=") ? options.width : options.height) = static_cast<engine::core::i32>(parsed);
            // Remembered so `[window] size` in the project file can fill in for
            // a size nobody asked for and stay out of the way of one somebody
            // did.
            sizeFromFlags = true;
            continue;
        }
        // A match's windows (ADR 0106 §5): where each goes, what each is
        // called, and a log of its own.
        if (arg.starts_with("--window=")) {
            engine::platform::WindowPlacement placement;
            // Four integers, comma-separated: x, y, width, height.
            std::array<int*, 4> slots{&placement.x, &placement.y, &placement.width, &placement.height};
            std::string_view rest = arg.substr(9);
            bool parsed = true;
            for (std::size_t index = 0; index < slots.size() && parsed; ++index) {
                const std::size_t comma = index + 1 < slots.size() ? rest.find(',') : rest.size();
                const std::string_view piece = rest.substr(0, comma);
                const auto [end, failure] = std::from_chars(piece.data(), piece.data() + piece.size(), *slots[index]);
                parsed =
                    failure == std::errc{} && end == piece.data() + piece.size() && comma != std::string_view::npos;
                rest = comma < rest.size() ? rest.substr(comma + 1) : std::string_view{};
            }
            if (!parsed || placement.width <= 0 || placement.height <= 0) {
                const std::array<I18nArg, 1> badValue{I18nArg{"option", arg}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            options.windowPlacement = placement;
            options.width = placement.width;
            options.height = placement.height;
            sizeFromFlags = true;
            continue;
        }
        if (arg.starts_with("--label=")) {
            options.windowLabel = std::string(arg.substr(8));
            continue;
        }
        if (arg.starts_with("--log-file=")) {
            options.logFile = std::filesystem::path(std::string(arg.substr(11)));
            continue;
        }
        if (arg.starts_with("--frames=")) {
            if (!numericValue(arg.substr(9), options.frames)) {
                const std::array<I18nArg, 1> badValue{I18nArg{"option", arg}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            continue;
        }
        if (arg.starts_with("--simulate-device-loss=")) {
            if (!numericValue(arg.substr(23), options.simulateDeviceLossAt)) {
                const std::array<I18nArg, 1> badValue{I18nArg{"option", arg}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            continue;
        }
        if (arg.starts_with("--surface-cache=")) {
            options.surfaceCache = std::filesystem::path(arg.substr(16));
            continue;
        }
        if (arg.starts_with("--screenshot=")) {
            options.screenshotPath = std::filesystem::path(arg.substr(13));
            continue;
        }
        if (arg.starts_with("--screenshot-every=")) {
            if (!numericValue(arg.substr(19), options.screenshotEvery) || options.screenshotEvery == 0) {
                const std::array<I18nArg, 1> badValue{I18nArg{"option", arg}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            continue;
        }
        if (arg.starts_with("--terrain-detail=")) {
            const std::string_view value = arg.substr(arg.find('=') + 1);
            if (value != "full" && value != "distance") {
                const std::array<I18nArg, 2> badValue{I18nArg{"option", arg}, I18nArg{"value", value}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            options.terrainFullDetail = value == "full";
            continue;
        }
        if (arg.starts_with("--debug-view=")) {
            const std::string_view value = arg.substr(arg.find('=') + 1);
            const std::optional<engine::render::DebugView> view = engine::render::parseDebugView(value);
            if (!view.has_value()) {
                const std::array<I18nArg, 2> badValue{I18nArg{"option", arg}, I18nArg{"value", value}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            graphics.debugView = *view;
            continue;
        }
        if (arg.starts_with("--save-scene=")) {
            options.saveScenePath = std::filesystem::path(arg.substr(13));
            continue;
        }
        if (arg == "--write-types") {
            // A build step like `--partition`: headless, and gone after one
            // write.
            options.writeTypesOnly = true;
            options.headless = true;
            continue;
        }
        if (arg.starts_with("--import-terrain=")) {
            // A build step like `--partition`: headless, and gone once the
            // scene is saved (ADR 0149 §2). `hills`, a `.luau` file that
            // returns a function, or a heightmap -- a file or a folder of
            // tiles.
            engine::app::EngineOptions::TerrainImport spec =
                options.terrainImport.value_or(engine::app::EngineOptions::TerrainImport{});
            const std::string_view source = arg.substr(17);
            using Kind = engine::app::EngineOptions::TerrainImport::Kind;
            if (source == "hills") {
                spec.kind = Kind::Hills;
            }
            else {
                spec.source = std::filesystem::path(source);
                spec.kind = spec.source.extension() == ".luau" ? Kind::Function : Kind::Heightmap;
            }
            options.terrainImport = spec;
            options.headless = true;
            continue;
        }
        if (arg.starts_with("--import-")) {
            // The numbers of an import, in any order round `--import-terrain`.
            engine::app::EngineOptions::TerrainImport spec =
                options.terrainImport.value_or(engine::app::EngineOptions::TerrainImport{});
            const std::string_view::size_type equals = arg.find('=');
            const std::string_view name = arg.substr(0, equals);
            const std::string_view value =
                equals == std::string_view::npos ? std::string_view{} : arg.substr(equals + 1);
            double number = 0.0;
            const bool parsed = decimalValue(value, number);
            bool known = true;
            if (name == "--import-size")
                spec.size = static_cast<float>(number);
            else if (name == "--import-low")
                spec.low = static_cast<float>(number);
            else if (name == "--import-high")
                spec.high = static_cast<float>(number);
            else if (name == "--import-scale")
                spec.scale = static_cast<float>(number);
            else if (name == "--import-material")
                spec.material = static_cast<engine::core::u8>(std::clamp(number, 0.0, 255.0));
            else if (name == "--import-seed")
                spec.seed = static_cast<engine::core::u32>(std::max(number, 0.0));
            else if (name == "--import-octaves")
                spec.octaves = static_cast<engine::core::u32>(std::clamp(number, 1.0, 10.0));
            else
                known = false;
            if (!known || !parsed) {
                const std::array<I18nArg, 1> badValue{I18nArg{"option", arg}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            options.terrainImport = spec;
            options.headless = true;
            continue;
        }
        if (arg == "--partition") {
            // Partition the project's scene into the cache and stop. Headless
            // and windowless for the same reason `--save-scene` is: it is a
            // build step, not a session.
            options.partitionOnly = true;
            options.headless = true;
            continue;
        }
        if (arg.starts_with("--capture-out=")) {
            options.capturePath = std::filesystem::path(arg.substr(14));
            continue;
        }
        if (arg.starts_with("--run-tests=")) {
            options.conformanceRoot = std::filesystem::path(arg.substr(12));
            // A conformance run has no window and ends when the suite does, so
            // the two flags a headless run needs are implied rather than typed
            // out at every call site.
            options.headless = true;
            options.exitAfterFrames = true;
            continue;
        }
        if (arg.starts_with("--test-report=")) {
            options.testReportPath = std::filesystem::path(arg.substr(14));
            continue;
        }
        if (arg.starts_with("--dev-control=")) {
            options.devControlUrl = std::string(arg.substr(14));
            continue;
        }
        if (arg.starts_with("--dev-token=")) {
            options.devControlToken = std::string(arg.substr(12));
            continue;
        }
        if (arg.starts_with("--replay=")) {
            options.replayRoot = std::filesystem::path(arg.substr(9));
            continue;
        }
        if (arg == "--record-replay") {
            options.replayRecord = true;
            continue;
        }
        if (arg.starts_with("--two-worlds=")) {
            options.twoWorldsRoot = std::filesystem::path(arg.substr(arg.find('=') + 1));
            continue;
        }
        if (arg.starts_with("--replica-gate=")) {
            options.replicaGateProject = std::filesystem::path(arg.substr(arg.find('=') + 1));
            continue;
        }
        if (arg.starts_with("--two-worlds-out=")) {
            options.twoWorldsOutDir = std::filesystem::path(arg.substr(arg.find('=') + 1));
            continue;
        }
        if (arg.starts_with("--bench=")) {
            options.benchRoot = std::filesystem::path(arg.substr(8));
            continue;
        }
        if (arg.starts_with("--bench-repeats=")) {
            if (!numericValue(arg.substr(16), options.benchRepeats)) {
                const std::array<I18nArg, 1> badValue{I18nArg{"option", arg}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            continue;
        }
        // --- The graphics settings family (roadmap M8, ADR 0044) -----------
        //
        // The outermost of the three layers: a preset, then the project file,
        // then these. Each is an OVERRIDE rather than a value, so that "nobody
        // said anything" and "somebody asked for the default" stay different
        // answers -- see `project_config.h`.
        // ADR 0091's proof instrument: every static part drawn with a surface
        // shader the engine ships, in place of the built-in surface.
        if (arg.starts_with("--force-surface=")) {
            graphics.forcedSurface = std::string(arg.substr(arg.find('=') + 1));
            continue;
        }
        if (arg.starts_with("--quality=")) {
            const std::string_view value = arg.substr(arg.find('=') + 1);
            const std::optional<engine::render::QualityLevel> level = engine::render::parseQuality(value);
            if (!level.has_value()) {
                const std::array<I18nArg, 2> badValue{I18nArg{"option", arg}, I18nArg{"value", value}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            graphics.quality = *level;
            continue;
        }
        if (arg.starts_with("--render-scale=") || arg.starts_with("--shadow-distance=")) {
            const std::string_view value = arg.substr(arg.find('=') + 1);
            double parsed = 0.0;
            if (!decimalValue(value, parsed) || parsed <= 0.0) {
                const std::array<I18nArg, 2> badValue{I18nArg{"option", arg}, I18nArg{"value", value}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            if (arg.starts_with("--render-scale="))
                graphics.renderScale = static_cast<engine::core::f32>(parsed);
            else
                graphics.shadowDistance = static_cast<engine::core::f32>(parsed);
            continue;
        }
        if (arg.starts_with("--shadow-resolution=") || arg.starts_with("--shadow-cascades=") ||
            arg.starts_with("--light-budget=")) {
            const std::string_view value = arg.substr(arg.find('=') + 1);
            engine::core::u64 parsed = 0;
            // Zero is legal for two of the three -- no cascades is "the sun
            // casts no shadow" and no lights is a scene lit by the sky alone --
            // so only the resolution refuses it.
            if (!numericValue(value, parsed) || (arg.starts_with("--shadow-resolution=") && parsed == 0)) {
                const std::array<I18nArg, 2> badValue{I18nArg{"option", arg}, I18nArg{"value", value}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
                return kExitUsage;
            }
            if (arg.starts_with("--shadow-resolution="))
                graphics.shadowResolution = static_cast<engine::core::u32>(parsed);
            else if (arg.starts_with("--shadow-cascades="))
                graphics.shadowCascades = static_cast<engine::core::u32>(parsed);
            else
                graphics.lightBudget = static_cast<engine::core::u32>(parsed);
            continue;
        }
        // Both directions, because a preset is a starting point rather than a
        // menu: somebody on `low` may still want bloom, and somebody measuring
        // may want the frame held still with `--no-auto-exposure`.
        if (arg == "--bloom" || arg == "--no-bloom") {
            graphics.bloom = arg == "--bloom";
            continue;
        }
        if (arg == "--contact-shadows" || arg == "--no-contact-shadows") {
            graphics.contactShadows = arg == "--contact-shadows";
            continue;
        }
        if (arg == "--ambient-occlusion" || arg == "--no-ambient-occlusion") {
            graphics.ambientOcclusion = arg == "--ambient-occlusion";
            continue;
        }
        if (arg == "--anti-aliasing" || arg == "--no-anti-aliasing") {
            graphics.antiAliasing = arg == "--anti-aliasing";
            continue;
        }
        if (arg == "--auto-exposure" || arg == "--no-auto-exposure") {
            graphics.autoExposure = arg == "--auto-exposure";
            continue;
        }

        if (arg.starts_with("--editor-drive=")) {
            options.editorDrive = std::filesystem::path(arg.substr(15));
            continue;
        }

        if (arg.starts_with("--rhi=")) {
            const std::optional<engine::rhi::BackendId> backend = engine::app::parseBackendId(arg.substr(6));
            if (!backend.has_value()) {
                const std::array<I18nArg, 2> unknownBackend{I18nArg{"name", arg.substr(6)},
                                                            I18nArg{"available", engine::app::availableBackendNames()}};
                engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.unknown_backend"), unknownBackend);
                return kExitUsage;
            }
            options.backend = *backend;
            options.backendChosen = true;
            continue;
        }

        const std::array<I18nArg, 1> unknownArgs{I18nArg{"option", arg}};
        engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.unknown_option"), unknownArgs);
        return kExitUsage;
    }

    // A replay answers to none of the checks below: it has no window, no frame
    // budget and no backend, because it never opens a device at all. Validating
    // it as though it were a session would demand `--headless --frames=N` for a
    // mode in which neither means anything.
    //
    // `--partition` is the same shape one step along: it boots, writes the
    // cache and returns without ever reaching the loop, so a frame budget would
    // be a ceiling on a loop that does not run.
    // `--launcher` is the third: it has its own loop, no world and no frame
    // budget, and it ends when somebody chooses a project or closes the window.
    // An import's numbers with nothing to import: `--import-terrain` names it.
    if (options.terrainImport.has_value() && options.terrainImport->source.empty() &&
        options.terrainImport->kind != engine::app::EngineOptions::TerrainImport::Kind::Hills) {
        const std::array<I18nArg, 1> badValue{I18nArg{"option", "--import-terrain"}};
        engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.bad_value"), badValue);
        return kExitUsage;
    }
    if (!options.replayRoot.empty() || !options.benchRoot.empty() || !options.twoWorldsRoot.empty() ||
        !options.replicaGateProject.empty() || options.partitionOnly || options.writeTypesOnly || options.launcher ||
        options.terrainImport.has_value())
        return kExitOk;

    // A conformance run needs a ceiling for the same reason, and a generous one:
    // it ends when the suite calls `Shutdown`, and the budget is only there so a
    // suite that hangs fails rather than running until CI gives up.
    if (!options.conformanceRoot.empty() && options.frames == 0)
        options.frames = 100000;

    // A dev session is driven by its dev server and ends when that server says
    // so, so a frame budget would be a timer on a loop nobody asked to time.
    // Requiring one for `--headless --dev-control` -- which is what the E2E
    // gate runs -- would mean guessing how long a test needs.
    if (!options.devControlUrl.empty() && options.headless && options.frames == 0)
        return kExitOk;

    // A headless run with no frame budget would never terminate and nothing
    // could tell you why, since there is no window to close. Saying so beats
    // hanging a CI job until its timeout.
    // **A server is the one headless run that is meant to run until stopped**,
    // so it is the one exception: it ends when its game calls `game:Shutdown()`
    // -- a match over -- or when it is sent a stop (Ctrl+C, SIGTERM), which
    // closes it the same way (`platform::installStopSignals`).
    if (options.headless && options.frames == 0 &&
        options.network.topology != engine::replication::Topology::Dedicated) {
        engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.headless_needs_frames"));
        return kExitUsage;
    }

    // A windowed frame renders into the swapchain, which has been presented and
    // is gone before anything could read it back. Headless renders into a
    // target the engine owns, which is why the harness uses it.
    if (!options.screenshotPath.empty() && !options.headless) {
        engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.screenshot_needs_headless"));
        return kExitUsage;
    }

    // An editor with no window is not an editor. Refusing beats quietly
    // starting a headless session that draws the world into a texture nobody
    // will ever see.
    if (options.editor && options.headless) {
        engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.editor_needs_window"));
        return kExitUsage;
    }

    // Only the capture backend records a stream. Asking any other one for it
    // would produce an empty file, and an empty golden matches forever.
    if (!options.capturePath.empty() && options.backend != engine::rhi::BackendId::Capture) {
        engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.capture_needs_backend"));
        return kExitUsage;
    }

    return kExitOk;
}

} // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32) && defined(ENG_GUI_SUBSYSTEM)
    // A Windows-subsystem program has no console of its own. Started from a
    // terminal, it borrows that one, so a person who typed its name still sees
    // what it says; started with its streams redirected -- a pipe, a test --
    // it already has them and is left alone.
    if (GetStdHandle(STD_OUTPUT_HANDLE) == nullptr && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* stream = nullptr;
        (void)freopen_s(&stream, "CONOUT$", "w", stdout);
        (void)freopen_s(&stream, "CONOUT$", "w", stderr);
    }
#endif
    installConsoleLogSink();
    // Before anything that can run for long: a stop asked for from outside
    // closes the engine as `game:Shutdown()` would, close handlers and all.
    engine::platform::installStopSignals();

    const auto& paths = engine::platform::paths();
    const auto catalogLoad = engine::core::engineCatalog().loadFromFile(paths.contentDir / "i18n" / "en.json");
    if (!catalogLoad) {
        reportCatalogFailure(catalogLoad.diagnostic);
        return kExitNoCatalog;
    }

    std::vector<std::string_view> args;
    args.reserve(static_cast<std::size_t>(argc > 1 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);

    // **A packaged game is a folder that runs** (roadmap M8). `ludwerk build`
    // produces `<Game>.exe` beside a `game/` directory, and the player is this
    // same host: given no script, it mounts the project next to itself. That is
    // the whole of what makes the artifact double-clickable, and it is a
    // convention rather than a configuration file, because a configuration file
    // would be a second thing that can go missing.
    const std::filesystem::path packagedProject = engine::platform::paths().executableDir / "game";
    std::error_code packagedError;
    const bool hasPackagedProject = std::filesystem::is_directory(packagedProject, packagedError);

    // **No project is no longer a usage error** (ADR 0055): given nothing at all,
    // the host shows the project browser, which is what makes the engine
    // something a person can double-click. Decided here and DISPATCHED at the
    // bottom, so the launcher gets the log file and the crash handler like every
    // other session -- a launcher whose failure went only to a console nobody
    // opened would be the hardest thing here to diagnose.
    //
    // A host given a path that is not a project still refuses exactly as it did.
    // This is the case where there is no path, not a fallback that swallows a
    // bad one.
    const bool noProjectGiven = args.empty() && !hasPackagedProject;

    if (!args.empty() && args[0] == "--version") {
        printVersion();
        return kExitOk;
    }

    if (!args.empty() && args[0] == "--help") {
        engine::core::log(LogLevel::Info, ENG_TR("engine.cli.usage"));
        return kExitOk;
    }

    // **A build step, and nothing else** (ADR 0112): `ludwerk build` hands the
    // laid-out game here so its scripts are compiled by this engine's compiler
    // with the options it runs them with. No window, no world.
    // The scripts a scene, a stamp or `global.json` carries (S0.3), in a staged
    // content directory `ludwerk build` is about to pack.
    // **`ludwerk check`'s side pass** (ADR 0138 §8): every script a project
    // carries, linted for the side it runs on. Warnings, so the exit is clean.
    if (args.size() == 2 && args[0] == "--check-sides") {
        const engine::core::usize found = engine::app::checkProjectSides(std::filesystem::path(args[1]));
        const std::array<I18nArg, 1> counted{I18nArg{"count", static_cast<engine::core::i64>(found)}};
        engine::core::log(LogLevel::Info, ENG_TR("engine.cli.sides_checked"), counted);
        return kExitOk;
    }
    if (args.size() == 2 && args[0] == "--compile-content-scripts") {
        engine::app::ScriptPackageReport report;
        if (!engine::app::compileContentScripts(std::filesystem::path(args[1]), report)) {
            const std::array<I18nArg, 2> failed{I18nArg{"path", report.failed}, I18nArg{"message", report.message}};
            engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.compile_script"), failed);
            return kExitScriptError;
        }
        const std::array<I18nArg, 1> compiled{I18nArg{"count", static_cast<engine::core::i64>(report.compiled)}};
        engine::core::log(LogLevel::Info, ENG_TR("engine.cli.compiled_scripts"), compiled);
        return kExitOk;
    }
    if (args.size() == 2 && args[0] == "--compile-scripts") {
        engine::app::ScriptPackageReport report;
        if (!engine::app::compileGameScripts(std::filesystem::path(args[1]), report)) {
            const std::array<I18nArg, 2> failed{I18nArg{"path", report.failed}, I18nArg{"message", report.message}};
            engine::core::log(LogLevel::Error, ENG_TR("engine.cli.err.compile_script"), failed);
            return kExitScriptError;
        }
        const std::array<I18nArg, 1> compiled{I18nArg{"count", static_cast<engine::core::i64>(report.compiled)}};
        engine::core::log(LogLevel::Info, ENG_TR("engine.cli.compiled_scripts"), compiled);
        return kExitOk;
    }

    engine::app::EngineOptions options;
    for (int index = 1; index < argc; ++index)
        options.arguments.emplace_back(argv[index]);
    if (hasPackagedProject)
        options.scriptPath = packagedProject;
    engine::app::GraphicsOverrides graphicsOverrides;
    bool sizeFromFlags = false;
    if (const int usageExit = parseOptions(args, options, graphicsOverrides, sizeFromFlags); usageExit != kExitOk)
        return usageExit;
    if (noProjectGiven)
        options.launcher = true;

    // The project file, and the three-layer resolution it completes. A bare
    // script has no project and gets the preset plus the flags, which is the
    // same code path with an empty root.
    {
        std::error_code projectError;
        const bool isProject =
            !options.scriptPath.empty() && std::filesystem::is_directory(options.scriptPath, projectError);
        std::string configDiagnostic;
        const engine::app::ProjectConfig config = engine::app::loadProjectConfig(
            isProject ? options.scriptPath : std::filesystem::path{}, graphicsOverrides, &configDiagnostic);

        if (!configDiagnostic.empty()) {
            // Named and survivable, like a content pack that will not open: a
            // malformed project file leaves the engine's own defaults standing
            // rather than refusing to start, and says which line stopped it.
            const std::array<I18nArg, 1> configArgs{I18nArg{"reason", configDiagnostic}};
            engine::core::log(LogLevel::Warn, ENG_TR("app.warn.project_config"), configArgs);
        }

        options.graphics = config.graphics;
        options.pacing = engine::app::pacingWith(config.pacing, graphicsOverrides);
        options.windowTitle = config.windowTitle;
        options.startupScene = config.scene;
        options.defaultServer = config.networkServer;
        options.network.timeoutMs = config.networkTimeoutSeconds * 1000u;
        options.maxViewsPerFrame = config.maxViewsPerFrame;
        options.foliageDensity = config.foliageDensity;
        options.foliageShadowDistance = config.foliageShadowDistance;
        options.maxViewResolution = config.maxViewResolution;
        options.maxSubWorlds = config.maxSubWorlds;

        // **Where a game's saves go** (ADR 0111): a player's own folder, named
        // by the game's company and name, for a game; the project's
        // `.engine/saves/` for the editor's Play, `ludwerk dev` and a match's
        // windows -- one folder each, so four players do not share one -- so a
        // test run never touches a real player's saves; and a folder of its
        // own for a conformance run.
        options.saveMaxSlotBytes = config.saveMaxSlotBytes;
        options.saveMaxSlots = config.saveMaxSlots;
        options.sceneCloseGrace = config.sceneCloseGrace;
        options.scriptMemoryMb = config.scriptMemoryMb;
        options.developerWarnings =
            isProject && (options.editor || !options.devControlUrl.empty() || !options.windowLabel.empty());
        if (!options.conformanceRoot.empty()) {
            std::error_code tempError;
            options.saveDirectory = std::filesystem::temp_directory_path(tempError) /
                                    ("engine-conformance-saves-" + std::to_string(engine::platform::nowNs()));
        }
        else if (isProject && (options.editor || !options.devControlUrl.empty() || !options.windowLabel.empty())) {
            options.saveDirectory = options.scriptPath / ".engine" / "saves";
            if (!options.windowLabel.empty()) {
                std::string folder = options.windowLabel;
                for (char& c : folder) {
                    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                    c = plain ? c : '-';
                }
                options.saveDirectory /= folder;
            }
        }
        else if (isProject) {
            const std::string game = config.name.empty() ? config.id : config.name;
            if (const std::filesystem::path home =
                    engine::platform::preferencePath(config.company.empty() ? game : config.company, game);
                !home.empty())
                options.saveDirectory = home / "saves";
        }
        // A server package, run with no posture of its own: what `--serve`
        // would have been, on the default port. A posture on the command line
        // still wins -- the package is also something a person can `--host` --
        // and a build step (`--partition`, which `ludwerk build` runs on the
        // package itself) is not a session and serves nothing.
        const bool buildStep = options.partitionOnly || options.writeTypesOnly || !options.saveScenePath.empty() ||
                               options.terrainImport.has_value();
        if (config.serverRole && options.network.topology == engine::replication::Topology::Solo && !options.editor &&
            !buildStep) {
            options.network.topology = engine::replication::Topology::Dedicated;
            options.network.port = kDefaultGamePort;
            options.headless = true;
        }
        if (!config.icon.empty())
            options.projectIcon = options.scriptPath / std::filesystem::path(config.icon);
        options.fullscreen = engine::app::startsFullscreen(config);
        options.resizable = config.resizable;

        // Before any window exists, because the shell reads a process's
        // identity when it first shows one -- and a pinned shortcut that lost
        // its icon is not something a later call can undo.
        engine::platform::setApplicationId(config.id);

        if (!sizeFromFlags && config.windowWidth > 0 && config.windowHeight > 0) {
            options.width = config.windowWidth;
            options.height = config.windowHeight;
        }
    }

    // The two artifacts `architecture.md` §app has promised since M0, and the
    // reason they are HERE: `core` is L0 and cannot ask where a file belongs,
    // `platform::paths()` is L1, and this is the layer that sees both. The
    // directory is the process's own, so `run.bat` leaves them beside the
    // example a person was running.
    //
    // Neither failure is fatal. An engine that refuses to start because it could
    // not open a log is an engine that a read-only directory takes away
    // entirely, which is a worse trade than losing the log.
#if defined(__ANDROID__)
    // An app's working directory is `/`, which it may not write; its own
    // storage is where the log and a crash report can go (and `adb pull` finds
    // them).
    std::filesystem::path artifactDir =
        engine::platform::paths().userDir.empty() ? std::filesystem::current_path() : engine::platform::paths().userDir;
#else
    std::filesystem::path artifactDir = std::filesystem::current_path();
#endif
    // **The run before keeps its log** (audit A15): the log was opened over
    // itself, so the run that crashed lost its log to the run started to see
    // why. One generation, beside it.
    const auto openRotated = [](const std::filesystem::path& path) {
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            std::filesystem::path previous = path;
            previous.replace_filename(path.stem().string() + ".previous" + path.extension().string());
            std::filesystem::rename(path, previous, error);
        }
        return engine::core::openLogFile(path);
    };
    std::filesystem::path logPath = options.logFile.empty() ? artifactDir / "engine.log" : options.logFile;
    // **A second run from the same folder writes the log beside the first's**
    // (`openLogFileBeside`), rather than taking it from the run still writing it.
    const auto openBeside = [&logPath]() {
        const std::optional<std::filesystem::path> opened = engine::core::openLogFileBeside(logPath);
        if (opened.has_value())
            logPath = *opened;
        return opened.has_value();
    };
    bool logOpened = options.logFile.empty() ? openBeside() : openRotated(logPath);
    // **Where it can write, when it cannot write here** (audit A15): a game
    // installed where a player may not write -- Program Files -- had no log
    // and no crash report at all. Its own folder then, as a phone's already is.
    if (!logOpened && options.logFile.empty() && !engine::platform::paths().userDir.empty()) {
        std::error_code error;
        const std::filesystem::path fallback = engine::platform::paths().userDir / "logs";
        std::filesystem::create_directories(fallback, error);
        artifactDir = fallback;
        logPath = artifactDir / "engine.log";
        logOpened = openBeside();
    }
    const bool handlerInstalled = engine::platform::installCrashHandler(artifactDir);

    const std::array<I18nArg, 1> bootArgs{I18nArg{"version", ENG_VERSION_STRING}};
    engine::core::log(LogLevel::Info, ENG_TR("engine.boot.hello"), bootArgs);

    // Printed, not assumed. The whole failure this closes is a human reporting a
    // crash from memory, and a log whose path nobody knows is a log nobody
    // sends. It goes out at Info so it is in the log file as well -- the first
    // line of which then says where the log file is, which sounds circular and
    // is not: the copy in the terminal is the one a person reads.
    if (logOpened) {
        const std::array<I18nArg, 1> logArgs{I18nArg{"path", logPath.string()}};
        engine::core::log(LogLevel::Info, ENG_TR("engine.boot.info.log_file"), logArgs);
    }
    else {
        const std::array<I18nArg, 1> logArgs{I18nArg{"path", logPath.string()}};
        engine::core::log(LogLevel::Warn, ENG_TR("engine.boot.warn.log_file_failed"), logArgs);
    }
    if (handlerInstalled) {
        const std::array<I18nArg, 1> crashArgs{I18nArg{"path", engine::platform::crashArtifactPath().string()}};
        engine::core::log(LogLevel::Info, ENG_TR("engine.boot.info.crash_artifact"), crashArgs);
    }

    if (!options.benchRoot.empty()) {
        // A benchmark is a measuring run with no window (D193): see
        // `platform::raiseProcessPriority` for what that protects it from.
        if (engine::platform::raiseProcessPriority())
            engine::core::log(LogLevel::Info, ENG_TR("engine.info.priority_raised"));
        std::vector<engine::app::BenchResult> results;
        if (const std::optional<engine::core::EngineError> error =
                engine::app::runBenchmarks(options.benchRoot, options.benchRepeats, results)) {
            engine::core::logText(LogLevel::Error, error->message);
            return kExitScriptError;
        }
        return kExitOk;
    }

    if (!options.twoWorldsRoot.empty()) {
        if (const std::optional<engine::core::EngineError> error = engine::app::runTwoWorldsGate({
                .root = options.twoWorldsRoot,
                .outputDir = options.twoWorldsOutDir,
                .backend = options.backend,
                .ticks = options.frames == 0 ? 8 : options.frames,
                .width = options.width,
                .height = options.height,
            })) {
            engine::core::logText(LogLevel::Error, error->message);

            // Same mapping the session path uses, and for the same reason: a
            // runner with no driver has not found anything about the seam.
            if (error->key.hash == ENG_TR("rhi.err.device_create_failed").hash)
                return kExitNoGraphicsDevice;
            return kExitScriptError;
        }
        return kExitOk;
    }

    if (!options.replicaGateProject.empty()) {
        if (const std::optional<engine::core::EngineError> error = engine::app::runReplicaGate({
                .project = options.replicaGateProject,
                .outputDir = options.twoWorldsOutDir,
                .backend = options.backend,
                .ticks = options.frames == 0 ? 240 : options.frames,
                .width = options.width,
                .height = options.height,
            })) {
            engine::core::logText(LogLevel::Error, error->message);
            if (error->key.hash == ENG_TR("rhi.err.device_create_failed").hash)
                return kExitNoGraphicsDevice;
            return kExitScriptError;
        }
        return kExitOk;
    }

    if (!options.replayRoot.empty()) {
        if (const std::optional<engine::core::EngineError> error =
                engine::app::runReplayGate(options.replayRoot, options.replayRecord)) {
            engine::core::logText(LogLevel::Error, error->message);
            return kExitScriptError;
        }
        return kExitOk;
    }

    if (options.launcher) {
        if (const std::optional<engine::core::EngineError> error = engine::app::runLauncher(options)) {
            engine::core::logText(LogLevel::Error, error->message);
            if (!error->detail.empty())
                engine::core::logText(LogLevel::Error, error->detail);
            if (error->key.hash == ENG_TR("rhi.err.device_create_failed").hash)
                return kExitNoGraphicsDevice;
            return kExitScriptError;
        }
        return kExitOk;
    }

    // **A dedicated server opens no graphics device** (ADR 0105, ADR 0107: a
    // server draws nothing). It used to open the ordinary one and render every
    // frame offscreen for nobody -- and on a Linux host with no GPU, the
    // machine a server most often runs on, that device is Mesa's software
    // rasteriser, which crashed the server a few seconds after it started. The
    // no-op backend hands out handles and draws nothing; `--rhi=` still wins.
    if (options.network.topology == engine::replication::Topology::Dedicated && options.headless &&
        !options.backendChosen) {
        if (const std::optional<engine::rhi::BackendId> none = engine::app::parseBackendId("null"); none.has_value())
            options.backend = *none;
    }

    // **Nor does an import** (D413): it lays ground and saves it, and draws
    // none of it. With the ordinary device it could not run where there is no
    // GPU -- a build machine, CI's Linux runner -- and `ludwerk terrain
    // import` there said that no graphics device could be created.
    if (options.terrainImport.has_value() && !options.backendChosen) {
        if (const std::optional<engine::rhi::BackendId> none = engine::app::parseBackendId("null"); none.has_value())
            options.backend = *none;
    }

    if (const std::optional<engine::core::EngineError> error = engine::app::run(options)) {
        engine::core::logText(LogLevel::Error, error->message);
        if (!error->detail.empty())
            engine::core::logText(LogLevel::Error, error->detail);
        // **A server that did not start says so where it can be read** (the
        // multiplayer smoothness brief): opened with a double click, its
        // console closes as it exits, and the reason with it.
        if (options.network.topology == engine::replication::Topology::Dedicated &&
            engine::platform::consoleClosesWithProcess()) {
            const engine::core::Catalog& text = engine::core::engineCatalog();
            (void)engine::platform::askChoice(nullptr, text.format(ENG_TR("engine.server.err.title")), error->message,
                                              {text.format(ENG_TR("engine.server.ok"))});
        }
        if (error->key.hash == ENG_TR("engine.err.device_lost").hash)
            return kExitDeviceLost;

        // The key IS the identity of an engine error (ADR 0019), so matching on
        // it is the intended way to tell one failure from another -- no second
        // channel, no parsing of prose that translation would break.
        if (error->key.hash == ENG_TR("rhi.err.device_create_failed").hash)
            return kExitNoGraphicsDevice;

        return kExitScriptError;
    }

    return kExitOk;
}
