#include "engine/app/language_packs.h"

#include <algorithm>
#include <array>
#include <optional>
#include <system_error>

#include "engine/asset/pack.h"
#include "engine/asset/seal.h"
#include "engine/core/error.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"

namespace engine::app {
namespace {

using core::I18nArg;
using core::LogLevel;

// Why `path` is not this language's pack, or nothing when it is. Opened here
// with every blob hashed, and let go of before it is mounted: the mount opens
// it again, which is a mapping and costs nothing.
[[nodiscard]] std::optional<std::string> refusal(const std::filesystem::path& path, std::string_view locale)
{
    asset::Pack pack;
    if (const std::optional<core::EngineError> error = asset::openPackFile(path, pack, true))
        return error->message;

    if (const asset::PackEntry* const table = pack.names(); table != nullptr) {
        std::vector<asset::PackName> names;
        if (!asset::decodePackNames(pack.blob(table->hash), names)) {
            const I18nArg args[] = {{"hash", table->hash.toHex()}};
            return core::makeError(ENG_TR("asset.pack.err.hash_mismatch"), args).message;
        }
        if (asset::findPackName(names, asset::GameScheme) != nullptr)
            return std::string(core::tr(ENG_TR("app.err.language_pack_is_a_game")));
        return std::nullopt;
    }

    // Plain: its names can be read, so they are.
    std::vector<asset::ManifestRow> rows;
    if (const std::optional<core::EngineError> error = asset::readContentManifest(asset::packManifestPath(path), rows))
        return error->message;
    std::string prefix(asset::AssetScheme);
    prefix.append("l10n/").append(locale).append("/");
    for (const asset::ManifestRow& row : rows) {
        if (!row.urn.starts_with(prefix))
            return core::tr(ENG_TR("app.err.language_pack_other_names"),
                            {I18nArg{"content", row.urn}, I18nArg{"locale", std::string(locale)}});
    }
    return std::nullopt;
}

} // namespace

std::filesystem::path languagePackPath(const std::filesystem::path& engineFolder, std::string_view locale)
{
    std::string name("l10n-");
    name.append(locale).append(".lpack");
    return engineFolder / std::filesystem::path(name);
}

std::vector<std::string> mountLanguagePacks(asset::ContentMounts& mounts, const std::filesystem::path& engineFolder,
                                            std::span<const std::string> voice, std::span<const std::string> shipped)
{
    std::vector<std::string> mounted;
    for (const std::string& locale : voice) {
        if (std::find(shipped.begin(), shipped.end(), locale) != shipped.end() ||
            std::find(mounted.begin(), mounted.end(), locale) != mounted.end())
            continue;
        const std::filesystem::path path = languagePackPath(engineFolder, locale);
        std::error_code ec;
        // Not installed is the ordinary case, and is said by nobody.
        if (!std::filesystem::is_regular_file(path, ec))
            continue;

        std::optional<std::string> reason = refusal(path, locale);
        if (!reason.has_value()) {
            if (const std::optional<core::EngineError> error = mounts.mountPack(path))
                reason = error->message;
        }
        if (reason.has_value()) {
            const std::array<I18nArg, 3> args{I18nArg{"path", path.generic_string()}, I18nArg{"locale", locale},
                                              I18nArg{"reason", *reason}};
            core::log(LogLevel::Warn, ENG_TR("app.warn.language_pack_refused"), args);
            continue;
        }
        const std::array<I18nArg, 2> args{I18nArg{"locale", locale}, I18nArg{"path", path.generic_string()}};
        core::log(LogLevel::Info, ENG_TR("app.info.language_pack_mounted"), args);
        mounted.push_back(locale);
    }
    return mounted;
}

} // namespace engine::app
