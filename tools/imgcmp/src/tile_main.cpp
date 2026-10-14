// `imgtile` -- how strongly a picture of ground repeats at a terrain layer's
// tile period, and how much its colour varies from tile to tile (ADR 0113's
// amendment). See `engine/imgcmp/tile.h` for what it measures and why.
//
// Exit 0 when the picture is within its limits (none by default), 1 when not,
// 2 on a usage or file error. Developer-facing English, like the tools beside
// it.

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>

#include "engine/imgcmp/tile.h"

namespace {

constexpr std::string_view Usage = "usage: imgtile <shot.png> --period=PX [--max-repeat=R] [--min-repeat=R]\n"
                                   "               [--min-blocks=B]\n"
                                   "  The autocorrelation of the picture's detail at a shift of PX pixels,\n"
                                   "  across, down and any way (the largest is held to the limits), and the\n"
                                   "  spread of PX-sized blocks' mean luminance as a share of the mean.\n";

[[nodiscard]] bool number(std::string_view text, double& out)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

} // namespace

int main(int argc, char** argv)
{
    std::string shotPath;
    double period = 0.0;
    double maxRepeat = 1.0;
    double minRepeat = -1.0;
    double minBlocks = 0.0;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        const auto value = [&](std::string_view name) { return arg.substr(name.size()); };
        if (arg.starts_with("--period=") && number(value("--period="), period))
            continue;
        else if (arg.starts_with("--max-repeat=") && number(value("--max-repeat="), maxRepeat))
            continue;
        else if (arg.starts_with("--min-repeat=") && number(value("--min-repeat="), minRepeat))
            continue;
        else if (arg.starts_with("--min-blocks=") && number(value("--min-blocks="), minBlocks))
            continue;
        else if (!arg.starts_with("--") && shotPath.empty())
            shotPath = std::string(arg);
        else {
            std::fputs(Usage.data(), stderr);
            return 2;
        }
    }
    if (shotPath.empty() || !(period >= 4.0)) {
        std::fputs(Usage.data(), stderr);
        return 2;
    }

    const engine::imgcmp::LoadResult shot = engine::imgcmp::loadPngFile(shotPath);
    if (!shot.ok) {
        std::fprintf(stderr, "imgtile: %s: %s\n", shotPath.c_str(), shot.error.c_str());
        return 2;
    }
    const engine::imgcmp::TileReport report = engine::imgcmp::measureTiling(shot.image, period);
    double repeat = report.across > report.down ? report.across : report.down;
    repeat = report.ring > repeat ? report.ring : repeat;
    std::printf("%s: repeat %.3f across, %.3f down, %.3f any way; blocks %.4f\n", shotPath.c_str(), report.across,
                report.down, report.ring, report.blocks);
    return repeat <= maxRepeat && repeat >= minRepeat && report.blocks >= minBlocks ? 0 : 1;
}
