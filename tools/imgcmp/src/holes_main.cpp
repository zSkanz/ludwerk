// `imgholes` -- counts where a picture of the ground shows the sky through it
// (terrain audit T0). See `engine/imgcmp/holes.h` for what it counts and why.
//
// Exit 0 when the counts are within their limits (zero by default), 1 when
// not, 2 on a usage or file error. Developer-facing English, like the tools
// beside it.

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>

#include "engine/imgcmp/holes.h"

namespace {

constexpr std::string_view Usage =
    "usage: imgholes <shot.png> [--reference=full-detail.png] [--slack=N] [--max-enclosed=N] [--max-vanished=N]\n"
    "  Counts sky-coloured pixels enclosed by ground, and, against a reference,\n"
    "  pixels that were ground there (more than N pixels inside it) and are sky here.\n";

[[nodiscard]] bool number(std::string_view text, long long& out)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size() && out >= 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string shotPath;
    std::string referencePath;
    long long slack = 2;
    long long maxEnclosed = 0;
    long long maxVanished = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        const auto value = [&](std::string_view name) { return arg.substr(name.size()); };
        if (arg.starts_with("--reference="))
            referencePath = std::string(value("--reference="));
        else if (arg.starts_with("--slack=") && number(value("--slack="), slack))
            continue;
        else if (arg.starts_with("--max-enclosed=") && number(value("--max-enclosed="), maxEnclosed))
            continue;
        else if (arg.starts_with("--max-vanished=") && number(value("--max-vanished="), maxVanished))
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
        std::fprintf(stderr, "imgholes: %s: %s\n", shotPath.c_str(), shot.error.c_str());
        return 2;
    }
    engine::imgcmp::LoadResult reference;
    if (!referencePath.empty()) {
        reference = engine::imgcmp::loadPngFile(referencePath);
        if (!reference.ok) {
            std::fprintf(stderr, "imgholes: %s: %s\n", referencePath.c_str(), reference.error.c_str());
            return 2;
        }
        if (reference.image.width != shot.image.width || reference.image.height != shot.image.height) {
            std::fprintf(stderr, "imgholes: the reference is not the shot's size\n");
            return 2;
        }
    }
    const engine::imgcmp::HoleReport report = engine::imgcmp::findHoles(
        shot.image, referencePath.empty() ? nullptr : &reference.image, static_cast<int>(slack));
    std::printf("%s: %zu ground pixel(s), %zu enclosed sky, %zu vanished\n", shotPath.c_str(), report.ground,
                report.enclosed, report.vanished);
    const bool within = static_cast<long long>(report.enclosed) <= maxEnclosed &&
                        static_cast<long long>(report.vanished) <= maxVanished;
    return within ? 0 : 1;
}
