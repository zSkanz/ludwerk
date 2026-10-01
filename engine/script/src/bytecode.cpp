#include "engine/script/bytecode.h"

#include <Luau/Bytecode.h>

#include <cstdlib>

#include "engine/core/i18n.h"

#if ENG_LUAU_COMPILER
#include <luacode.h>

#include "engine/script/compile_options.h"
#endif

namespace engine::script {

#if ENG_LUAU_COMPILER
namespace {

// **A UTF-8 byte-order mark is not Luau** (D415): the compiler refuses the
// first line of a script for a character nobody can see, and Notepad and
// Windows PowerShell's `Set-Content -Encoding utf8` put one there. Skipped at
// the very start, as the readers of `project.toml` and of JSON skip it
// (D185); lines keep their numbers, the mark being on the first.
[[nodiscard]] std::string_view withoutMark(std::string_view source) noexcept
{
    constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
    if (source.starts_with(kUtf8Bom))
        source.remove_prefix(kUtf8Bom.size());
    return source;
}

} // namespace
#endif

bool isBytecode(std::string_view chunk) noexcept
{
    if (chunk.empty())
        return false;
    const auto version = static_cast<unsigned char>(chunk[0]);
    if (version == 0)
        return true;
    return chunk.size() >= 2 && version < 0x20u && static_cast<unsigned char>(chunk[1]) < 0x09u;
}

std::optional<core::EngineError> bytecodeOf(std::string_view chunk, std::string_view chunkName,
                                            std::string& outBytecode)
{
    if (isBytecode(chunk)) {
        // Zero is the compiler's own "this did not compile" and is left for
        // `luau_load`, which reports the message the bytecode carries.
        const auto version = static_cast<unsigned char>(chunk.front());
        // Up to what this VM reads rather than what its compiler writes, as
        // `luau_load` itself does.
        if (version != 0 && (version < LBC_VERSION_MIN || version > LBC_VERSION_MAX)) {
            // Only ever a package run by another engine build than the one that
            // exported it, so the message says both numbers.
            const core::I18nArg args[] = {
                {"source", chunkName},
                {"version", static_cast<core::i64>(version)},
                {"min", static_cast<core::i64>(LBC_VERSION_MIN)},
                {"max", static_cast<core::i64>(LBC_VERSION_MAX)},
            };
            return core::makeError(ENG_TR("script.err.bytecode_version"), args);
        }
        outBytecode.assign(chunk);
        return std::nullopt;
    }

#if ENG_LUAU_COMPILER
    lua_CompileOptions options{};
    configureCompileOptions(options);
    std::size_t size = 0;
    chunk = withoutMark(chunk);
    char* bytecode = luau_compile(chunk.data(), chunk.size(), &options, &size);
    if (bytecode == nullptr) {
        const core::I18nArg args[] = {{"source", chunkName},
                                      {"message", std::string_view{"compilation produced no bytecode"}}};
        return core::makeError(ENG_TR("script.err.syntax"), args);
    }
    outBytecode.assign(bytecode, size);
    std::free(bytecode);
    return std::nullopt;
#else
    // The chunk name goes in `detail`, which is developer context and never
    // localised -- the message itself says what the build cannot do, once.
    return core::makeError(ENG_TR("script.err.no_compiler"), {}, std::string(chunkName));
#endif
}

bool compileForPackage(std::string_view source, std::string& outBytecode, std::string& outError)
{
#if ENG_LUAU_COMPILER
    lua_CompileOptions options{};
    configureCompileOptions(options);
    options.debugLevel = 1;
    std::size_t size = 0;
    source = withoutMark(source);
    char* bytecode = luau_compile(source.data(), source.size(), &options, &size);
    if (bytecode == nullptr) {
        outError = "compilation produced no bytecode";
        return false;
    }
    std::string compiled(bytecode, size);
    std::free(bytecode);
    // A compile error is bytecode too: a zero version byte, then the message.
    if (compiled.empty() || compiled.front() == '\0') {
        outError = compiled.size() > 1 ? compiled.substr(1) : std::string("compilation failed");
        return false;
    }
    outBytecode = std::move(compiled);
    return true;
#else
    (void)source;
    (void)outBytecode;
    outError = core::formatKeyPrefixed(ENG_TR("script.err.no_compiler"));
    return false;
#endif
}

} // namespace engine::script
