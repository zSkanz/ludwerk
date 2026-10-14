// `imgmirror` -- says whether what a flat mirror shows is what is in front of
// it, from three pictures (ADR 0209). See `engine/imgcmp/mirror.h` for what it
// counts and why.
//
// Exit 0 when enough pixels reflect and no more than the allowed number of
// them are wrong, 1 when not, 2 on a usage or file error. Developer-facing
// English, like the tools beside it.

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "engine/imgcmp/mirror.h"

namespace {

constexpr std::string_view Usage =
    "usage: imgmirror <reflected.png> <plain.png> <mirrored.png> <sky.png> [--tolerance=N] [--reach=N]\n"
    "                 [--min-reflecting=N] [--max-wrong=N]\n"
    "  reflected: the scene with the mirror reflecting it; plain: the same frame with the\n"
    "  reflection off; mirrored: the scene with no mirror, through the camera mirrored in its\n"
    "  plane; sky: what that camera sees of an empty scene. A pixel the reflection changed (by\n"
    "  more than N, 24, over its three channels) is wrong when the mirrored picture, turned top\n"
    "  to bottom, shows the sky within N (2) pixels of it, or a thing the change went away from.\n";

[[nodiscard]] bool number(std::string_view text, long long& out)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size() && out >= 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> paths;
    long long tolerance = 24;
    long long reach = 2;
    long long least = 1;
    long long most = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        const auto value = [&](std::string_view name) { return arg.substr(name.size()); };
        if (arg.starts_with("--tolerance=") && number(value("--tolerance="), tolerance))
            continue;
        else if (arg.starts_with("--reach=") && number(value("--reach="), reach))
            continue;
        else if (arg.starts_with("--min-reflecting=") && number(value("--min-reflecting="), least))
            continue;
        else if (arg.starts_with("--max-wrong=") && number(value("--max-wrong="), most))
            continue;
        else if (!arg.starts_with("--") && paths.size() < 4)
            paths.emplace_back(arg);
        else {
            std::fputs(Usage.data(), stderr);
            return 2;
        }
    }
    if (paths.size() != 4) {
        std::fputs(Usage.data(), stderr);
        return 2;
    }

    engine::imgcmp::Image images[4];
    for (std::size_t index = 0; index < 4; ++index) {
        engine::imgcmp::LoadResult loaded = engine::imgcmp::loadPngFile(paths[index]);
        if (!loaded.ok) {
            std::fprintf(stderr, "imgmirror: %s: %s\n", paths[index].c_str(), loaded.error.c_str());
            return 2;
        }
        images[index] = std::move(loaded.image);
    }
    for (std::size_t index = 1; index < 4; ++index) {
        if (images[index].width != images[0].width || images[index].height != images[0].height) {
            std::fprintf(stderr, "imgmirror: the four pictures are not the same size\n");
            return 2;
        }
    }

    const engine::imgcmp::MirrorReport report = engine::imgcmp::checkMirror(
        images[0], images[1], images[2], images[3], static_cast<int>(tolerance), static_cast<int>(reach));
    std::printf("%s: %zu pixel(s) reflect, %zu of them not what the mirror shows", paths[0].c_str(), report.reflecting,
                report.wrong);
    if (report.wrong > 0)
        std::printf(", the first at (%d, %d)", report.firstX, report.firstY);
    std::printf("\n");
    const bool enough = static_cast<long long>(report.reflecting) >= least;
    if (!enough)
        std::printf("imgmirror: fewer than %lld pixels reflect -- nothing was mirrored to check\n", least);
    return enough && static_cast<long long>(report.wrong) <= most ? 0 : 1;
}
