// `imgsteps` -- counts where a picture of how the terrain's shading bends its
// normal steps between two pixels (terrain audit T3), and how far a picture's
// colours spread over the ground. See `engine/imgcmp/steps.h` for what it
// counts and why.
//
// Exit 0 when the counts are within their limits (no step and any spread by
// default), 1 when not, 2 on a usage or file error. Developer-facing English,
// like the tools beside it.

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>

#include "engine/imgcmp/steps.h"

namespace {

constexpr std::string_view Usage = "usage: imgsteps <shot.png> [--step=N] [--max=N] [--max-spread=N]\n"
                                   "  Counts pixels of a --debug-view=bend picture whose right or lower\n"
                                   "  neighbour differs from them by more than N (24) in some channel, and\n"
                                   "  how far any channel ranges over the ground (the albedo view's check).\n";

[[nodiscard]] bool number(std::string_view text, long long& out)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size() && out >= 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::string shotPath;
    long long step = 24;
    long long most = 0;
    long long spread = 255;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        const auto value = [&](std::string_view name) { return arg.substr(name.size()); };
        if (arg.starts_with("--step=") && number(value("--step="), step))
            continue;
        else if (arg.starts_with("--max=") && number(value("--max="), most))
            continue;
        else if (arg.starts_with("--max-spread=") && number(value("--max-spread="), spread))
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
        std::fprintf(stderr, "imgsteps: %s: %s\n", shotPath.c_str(), shot.error.c_str());
        return 2;
    }
    const engine::imgcmp::StepReport report = engine::imgcmp::findSteps(shot.image, static_cast<int>(step));
    std::printf("%s: %zu ground pixel(s), %zu step(s), spread %d\n", shotPath.c_str(), report.ground, report.steps,
                report.spread);
    return static_cast<long long>(report.steps) <= most && report.spread <= spread ? 0 : 1;
}
