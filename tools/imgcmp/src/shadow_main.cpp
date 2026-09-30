// `imgshadow` -- counts where a picture of the sun's shadow on terrain darkens
// ground that faces the sun (terrain audit T3). See `engine/imgcmp/shadow.h`
// for what it counts and why.
//
// Exit 0 when the counts are within their limits (zero by default), 1 when
// not, 2 on a usage or file error. Developer-facing English, like the tools
// beside it.

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>

#include "engine/imgcmp/shadow.h"

namespace {

constexpr std::string_view Usage = "usage: imgshadow <shot.png> [--max-map=N] [--max-contact=N]\n"
                                   "  Counts pixels of a --debug-view=shadow picture that face the sun (blue)\n"
                                   "  and that the shadow map (red) or the contact mask (green) darkens.\n";

[[nodiscard]] bool number(std::string_view text, long long& out)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size() && out >= 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string shotPath;
    long long maxMap = 0;
    long long maxContact = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        const auto value = [&](std::string_view name) { return arg.substr(name.size()); };
        if (arg.starts_with("--max-map=") && number(value("--max-map="), maxMap))
            continue;
        else if (arg.starts_with("--max-contact=") && number(value("--max-contact="), maxContact))
            continue;
        else if (!arg.starts_with("--") && shotPath.empty())
            shotPath = std::string(arg);
        else {
            std::fputs(Usage.data(), stderr);
            return 2;
        }
    }
    if (shotPath.empty()) {
        std::fputs(Usage.data(), stderr);
        return 2;
    }

    const engine::imgcmp::LoadResult shot = engine::imgcmp::loadPngFile(shotPath);
    if (!shot.ok) {
        std::fprintf(stderr, "imgshadow: %s: %s\n", shotPath.c_str(), shot.error.c_str());
        return 2;
    }
    const engine::imgcmp::ShadowReport report = engine::imgcmp::findSelfShadow(shot.image);
    std::printf("%s: %zu facing the sun, %zu darkened by the map, %zu by the contact mask\n", shotPath.c_str(),
                report.facing, report.mapDark, report.contactDark);
    const bool within =
        static_cast<long long>(report.mapDark) <= maxMap && static_cast<long long>(report.contactDark) <= maxContact;
    return within ? 0 : 1;
}
