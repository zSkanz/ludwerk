// `assetc` -- argv plumbing and exit codes. The work is in the library beside
// it, so the tests exercise the same code the binary runs (the `imgcmp` shape).
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/asset/archive.h"
#include "engine/asset/icon_set.h"
#include "engine/asset/image.h"
#include "engine/asset/mesh_format.h"
#include "engine/assetc/compiler.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/jobs/jobs.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"

namespace {

using engine::assetc::CompileOptions;
using engine::assetc::CompileResult;

// This tool's own console output is developer-facing diagnostics rather than
// engine messages, so it is printed directly; every ENGINE error it relays
// arrives already key-prefixed through `core::makeError` and stays that way
// (R3). The catalog is loaded beside the binary for exactly that reason -- a
// relayed error should read as prose rather than as a bare key.
void usage()
{
    std::cout << "usage: assetc --input <content-dir> --output <pack> --manifest <json> [--jobs N]\n"
                 "              [--shadercross <exe>] [--shader-include <dir>]\n"
                 "\n"
                 "  Compiles a content directory into one .lpack and its manifest.\n"
                 "  Deterministic: the same inputs produce the same bytes, and --jobs 1 is how\n"
                 "  that is CHECKED rather than asserted: the same tree built serially and in\n"
                 "  parallel must come out identical.\n";
}

[[nodiscard]] bool flagValue(int argc, char** argv, int& index, const char* name, std::string& out)
{
    if (std::strcmp(argv[index], name) != 0) {
        return false;
    }
    if (index + 1 >= argc) {
        std::cout << "assetc: " << name << " needs a value\n";
        return false;
    }
    out = argv[++index];
    return true;
}

// **`assetc icon`** (ADR 0104 §2): one square PNG in, every platform's icons
// out. `ludwerk build` runs it for the target it builds.
//
//   assetc icon <picture.png> --out <dir> [--targets windows,linux,android]
//               [--background #RRGGBB] [--foreground <drawn-foreground.png>]
[[nodiscard]] int runIcon(int argc, char** argv)
{
    std::string picture;
    std::string output;
    std::string targets = "windows,linux,android";
    std::string background = "#FFFFFF";
    std::string foreground;
    for (int i = 2; i < argc; ++i) {
        if (flagValue(argc, argv, i, "--out", output) || flagValue(argc, argv, i, "--targets", targets) ||
            flagValue(argc, argv, i, "--background", background) ||
            flagValue(argc, argv, i, "--foreground", foreground))
            continue;
        if (picture.empty() && argv[i][0] != '-') {
            picture = argv[i];
            continue;
        }
        std::cout << "assetc: unknown option " << argv[i] << "\n";
        return 2;
    }
    if (picture.empty() || output.empty()) {
        std::cout << "assetc icon <picture.png> --out <dir> [--targets windows,linux,android] "
                     "[--background #RRGGBB] [--foreground <png>]\n";
        return 2;
    }
    const auto load = [](const std::string& path, engine::asset::Image& image) -> bool {
        std::vector<std::byte> bytes;
        if (!engine::platform::readFile(path, bytes)) {
            std::cout << "assetc: cannot read " << path << "\n";
            return false;
        }
        if (const std::optional<engine::core::EngineError> error = engine::asset::decodeImage(bytes, image)) {
            std::cout << "assetc: " << path << ": " << error->message << "\n";
            return false;
        }
        return true;
    };

    engine::asset::Image image;
    if (!load(picture, image))
        return 1;
    engine::asset::IconSetOptions options;
    options.windows = targets.find("windows") != std::string::npos;
    options.linuxDesktop = targets.find("linux") != std::string::npos;
    options.android = targets.find("android") != std::string::npos;
    if (background.size() == 7 && background[0] == '#')
        options.background = static_cast<engine::core::u32>(std::stoul(background.substr(1), nullptr, 16));
    if (!foreground.empty()) {
        engine::asset::Image drawn;
        if (!load(foreground, drawn))
            return 1;
        options.foreground = std::move(drawn);
    }

    engine::asset::IconSetReport report;
    if (const std::optional<engine::core::EngineError> error =
            engine::asset::writeIconSet(image, output, options, report)) {
        std::cout << "assetc: " << error->message << "\n";
        return 1;
    }
    for (const std::string& warning : report.warnings)
        std::cout << "assetc: warning: " << warning << "\n";
    std::cout << "assetc: " << report.written.size() << " icon file(s) -> " << output << "\n";
    return 0;
}

// **`assetc archive`** (ADR 0104 §7): a folder into the `.zip` or `.tar.gz`
// beside it, by the file name's ending. The execute bit is the caller's to
// name, per file, because a folder made on Windows has none to read.
//
//   assetc archive <folder> --out <file.zip|file.tar.gz> [--prefix <dir>]
//                  [--executable <path-in-folder>]...
[[nodiscard]] int runArchive(int argc, char** argv)
{
    std::string folder;
    std::string output;
    std::string prefix;
    std::vector<std::string> executables;
    for (int i = 2; i < argc; ++i) {
        std::string executable;
        if (flagValue(argc, argv, i, "--out", output) || flagValue(argc, argv, i, "--prefix", prefix))
            continue;
        if (flagValue(argc, argv, i, "--executable", executable)) {
            executables.push_back(executable);
            continue;
        }
        if (argv[i][0] != '-' && folder.empty()) {
            folder = argv[i];
            continue;
        }
        std::cout << "assetc: unknown option " << argv[i] << "\n";
        return 2;
    }
    const bool zip = output.ends_with(".zip");
    const bool tarGz = output.ends_with(".tar.gz") || output.ends_with(".tgz");
    if (folder.empty() || (!zip && !tarGz)) {
        std::cout << "usage: assetc archive <folder> --out <file.zip|file.tar.gz> [--prefix <dir>]\n"
                     "                      [--executable <path-in-folder>]...\n";
        return 2;
    }

    std::vector<engine::asset::ArchiveEntry> entries;
    if (const std::optional<engine::core::EngineError> error =
            engine::asset::collectArchiveEntries(folder, prefix, executables, entries)) {
        std::cout << "assetc: " << error->message << "\n";
        return 1;
    }
    // The build's own marker is how a rebuild knows it may clear the folder,
    // and nothing a player needs.
    std::erase_if(entries, [&](const engine::asset::ArchiveEntry& entry) {
        return entry.path == (prefix.empty() ? std::string(".engine-build") : prefix + "/.engine-build");
    });
    const std::optional<engine::core::EngineError> error =
        zip ? engine::asset::writeZip(entries, output) : engine::asset::writeTarGz(entries, output);
    if (error) {
        std::cout << "assetc: " << error->message << "\n";
        return 1;
    }
    std::cout << "assetc: " << entries.size() << " file(s) -> " << output << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    // **The mesh format this compiler writes** (D517), as a bare number for
    // `ludwerk build` to ask the player it packages about.
    if (argc > 1 && std::strcmp(argv[1], "--mesh-format") == 0) {
        std::cout << engine::asset::MeshFormatVersion << "\n";
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "icon") == 0) {
        (void)engine::core::engineCatalog().loadFromFile(
            (engine::platform::paths().contentDir / "i18n" / "en.json").string());
        return runIcon(argc, argv);
    }
    if (argc > 1 && std::strcmp(argv[1], "archive") == 0) {
        (void)engine::core::engineCatalog().loadFromFile(
            (engine::platform::paths().contentDir / "i18n" / "en.json").string());
        return runArchive(argc, argv);
    }

    std::string input;
    std::string output;
    std::string manifest;
    std::string jobsArgument;
    std::string shadercross;
    std::string shaderInclude;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
        if (flagValue(argc, argv, i, "--input", input) || flagValue(argc, argv, i, "--output", output) ||
            flagValue(argc, argv, i, "--manifest", manifest) || flagValue(argc, argv, i, "--jobs", jobsArgument) ||
            flagValue(argc, argv, i, "--shadercross", shadercross) ||
            flagValue(argc, argv, i, "--shader-include", shaderInclude)) {
            continue;
        }
        std::cout << "assetc: unknown option " << argv[i] << "\n";
        usage();
        return 2;
    }

    if (input.empty() || output.empty() || manifest.empty()) {
        usage();
        return 2;
    }

    // **The pool, and the flag that turns it off** (E9 step 11).
    //
    // Textures are encoded one per worker while basis's own threading stays off
    // -- `texture.cpp` says why that distinction matters -- and the merge is in
    // source order, so the pack this produces is byte-identical however many
    // workers ran. `--jobs=1` is how that claim is CHECKED rather than asserted:
    // `tests/assetdeterminism` builds the same tree twice, once serial and once
    // parallel, and requires the same bytes. A flag nobody can pass would leave
    // the determinism argument as prose.
    //
    // Zero is "as many as this machine has", which is `jobs::init`'s own default.
    if (jobsArgument == "1") {
        // Not initialising at all IS the serial mode: an uninitialised pool runs
        // every callable on the calling thread. Saying so here rather than
        // passing 1 keeps the two paths one path.
    }
    else {
        engine::jobs::init(jobsArgument.empty() ? 0u : static_cast<engine::core::u32>(std::stoul(jobsArgument)));
    }

    // Best effort: a missing catalog degrades a relayed engine error to its
    // key, which is still identifiable. It must not stop a build.
    (void)engine::core::engineCatalog().loadFromFile(
        (engine::platform::paths().contentDir / "i18n" / "en.json").string());

    CompileOptions options;
    options.inputRoot = input;
    // **The surface shader compiler** (ADR 0091): given, or beside this binary
    // as a packaged editor carries it, or the build tree's own. The engine's
    // headers the same way: given, staged beside the binary, or the source
    // tree's.
    const std::filesystem::path here = engine::platform::paths().executableDir;
#if defined(_WIN32)
    const std::filesystem::path besideCompiler = here / "shadercross.exe";
#else
    const std::filesystem::path besideCompiler = here / "shadercross";
#endif
    std::error_code missing;
    options.shadercross = !shadercross.empty() ? std::filesystem::path(shadercross)
                          : std::filesystem::exists(besideCompiler, missing)
                              ? besideCompiler
                              : std::filesystem::path(ENG_DEV_SHADERCROSS);
    const std::filesystem::path besideInclude = engine::platform::paths().contentDir / "shaders" / "include";
    options.surfaceInclude = !shaderInclude.empty() ? std::filesystem::path(shaderInclude)
                             : std::filesystem::exists(besideInclude, missing)
                                 ? besideInclude
                                 : std::filesystem::path(ENG_DEV_SURFACE_INCLUDE);

    const CompileResult result = engine::assetc::compile(options);

    // **Joined before anything else.** A pool left running when a process
    // returns from `main` has its workers alive while static destructors run,
    // and this one died with 0xC0000409 and no output at all -- which reads as a
    // crash in the compile and is a crash in the exit.
    engine::jobs::shutdown();
    if (!result.ok) {
        std::cout << "assetc: " << result.diagnostic << "\n";
        return 1;
    }

    std::string diagnostic;
    if (!engine::assetc::writeFile(output, result.pack, diagnostic)) {
        std::cout << "assetc: " << diagnostic << "\n";
        return 1;
    }
    const std::span<const std::byte> manifestBytes(reinterpret_cast<const std::byte*>(result.manifest.data()),
                                                   result.manifest.size());
    if (!engine::assetc::writeFile(manifest, manifestBytes, diagnostic)) {
        std::cout << "assetc: " << diagnostic << "\n";
        return 1;
    }

    // The chunks go beside the pack rather than in it, and the directory is
    // derived rather than asked for: `asset/chunk.h` explains why a chunk is
    // its own file, and a second flag naming where they land would be a second
    // way for the index and the files to disagree.
    if (result.chunkCount > 0) {
        const std::filesystem::path chunkRoot = std::filesystem::path(output).parent_path() / "content";
        for (const engine::assetc::ChunkOutput& chunk : result.chunks) {
            if (!engine::assetc::writeFile(chunkRoot / chunk.relativePath, chunk.bytes, diagnostic)) {
                std::cout << "assetc: " << diagnostic << "\n";
                return 1;
            }
        }

        const std::filesystem::path indexPath = std::filesystem::path(output).parent_path() / "content.chunks.json";
        const std::span<const std::byte> indexBytes(reinterpret_cast<const std::byte*>(result.chunkIndex.data()),
                                                    result.chunkIndex.size());
        if (!engine::assetc::writeFile(indexPath, indexBytes, diagnostic)) {
            std::cout << "assetc: " << diagnostic << "\n";
            return 1;
        }
    }

    std::cout << "assetc: " << result.meshCount << " mesh(es), " << result.textureCount << " texture(s), "
              << result.materialCount << " material(s), " << result.surfaceCount << " surface shader(s), "
              << result.chunkCount << " chunk(s), " << result.rawCount << " raw file(s) -> " << result.pack.size()
              << " bytes\n";
    if (result.surfacesUncompiled > 0) {
        // Said, because the pack is complete and the game will run: what it
        // will not do is draw these, and a person should hear that now rather
        // than from a magenta ocean.
        std::cout << "assetc: warning: " << result.surfacesUncompiled
                  << " surface shader(s) packed as source only -- no shader compiler at "
                  << options.shadercross.string() << "\n";
    }
    return 0;
}
