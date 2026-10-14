// `imgflicker` -- how much a sequence of frames flickers (ADR 0158): the mean
// second difference of each pixel's luma over time. See
// `engine/imgcmp/flicker.h` for what it measures and why.
//
// Prints the measure; exit 0 when it is within `--max` (any, by default), 1
// when not, 2 on a usage or file error. Developer-facing English, like the
// tools beside it.

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "engine/imgcmp/flicker.h"

namespace {

constexpr std::string_view Usage = "usage: imgflicker <frame.png> <frame.png> <frame.png>... [--max=X] [--ignore=N]\n"
                                   "  The mean of |L(t+1) - 2 L(t) + L(t-1)| past N (4) over every pixel and\n"
                                   "  inner frame, in luma steps of 0 to 255: how much the frames pop rather\n"
                                   "  than change steadily.\n"
                                   "       imgflicker --pops=LEAST,MOST [--max-pops=N] <frame.png>...\n"
                                   "  How many pixels, over every inner frame, are more than LEAST and no more\n"
                                   "  than MOST from the frame before and back within 2 of it the frame after.\n";

// `--pops`: three frames at a time, so a run of hundreds is three in memory.
int countThePops(const std::vector<std::string>& paths, double least, double most, long long allowed)
{
    if (paths.size() < 3) {
        std::fputs(Usage.data(), stderr);
        return 2;
    }
    std::vector<engine::imgcmp::Image> window;
    long long pops = 0;
    for (const std::string& path : paths) {
        engine::imgcmp::LoadResult loaded = engine::imgcmp::loadPngFile(path);
        if (!loaded.ok) {
            std::fprintf(stderr, "imgflicker: %s\n", loaded.error.c_str());
            return 2;
        }
        window.push_back(std::move(loaded.image));
        if (window.size() < 3)
            continue;
        const long long here = engine::imgcmp::countPops(window[0], window[1], window[2], least, most);
        if (here < 0) {
            std::fputs("imgflicker: the frames are not all one size\n", stderr);
            return 2;
        }
        pops += here;
        window.erase(window.begin());
    }
    const bool within = allowed < 0 || pops <= allowed;
    std::printf("imgflicker: %zu frames, %lld pops of %g to %g%s\n", paths.size(), pops, least, most,
                within ? "" : ": FAIL");
    return within ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> paths;
    double most = -1.0;
    double ignore = 4.0;
    bool pops = false;
    double popLeast = 0.0;
    double popMost = 0.0;
    long long popsAllowed = -1;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        if (arg.starts_with("--pops=")) {
            const std::string_view value = arg.substr(7);
            const std::size_t comma = value.find(',');
            if (comma == std::string_view::npos) {
                std::fputs(Usage.data(), stderr);
                return 2;
            }
            const std::string_view low = value.substr(0, comma);
            const std::string_view high = value.substr(comma + 1);
            const auto [lowEnd, lowError] = std::from_chars(low.data(), low.data() + low.size(), popLeast);
            const auto [highEnd, highError] = std::from_chars(high.data(), high.data() + high.size(), popMost);
            if (lowError != std::errc{} || highError != std::errc{} || lowEnd != low.data() + low.size() ||
                highEnd != high.data() + high.size() || popLeast < 0.0 || popMost < popLeast) {
                std::fputs(Usage.data(), stderr);
                return 2;
            }
            pops = true;
            continue;
        }
        if (arg.starts_with("--max-pops=")) {
            const std::string_view value = arg.substr(11);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), popsAllowed);
            if (error != std::errc{} || end != value.data() + value.size() || popsAllowed < 0) {
                std::fputs(Usage.data(), stderr);
                return 2;
            }
            continue;
        }
        if (arg.starts_with("--ignore=")) {
            const std::string_view value = arg.substr(9);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), ignore);
            if (error != std::errc{} || end != value.data() + value.size() || ignore < 0.0) {
                std::fputs(Usage.data(), stderr);
                return 2;
            }
            continue;
        }
        if (arg.starts_with("--max=")) {
            const std::string_view value = arg.substr(6);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), most);
            if (error != std::errc{} || end != value.data() + value.size() || most < 0.0) {
                std::fputs(Usage.data(), stderr);
                return 2;
            }
            continue;
        }
        if (arg.starts_with("--")) {
            std::fputs(Usage.data(), stderr);
            return 2;
        }
        paths.emplace_back(arg);
    }
    if (pops)
        return countThePops(paths, popLeast, popMost, popsAllowed);
    std::vector<engine::imgcmp::Image> frames;
    frames.reserve(paths.size());
    for (const std::string& path : paths) {
        engine::imgcmp::LoadResult loaded = engine::imgcmp::loadPngFile(path);
        if (!loaded.ok) {
            std::fprintf(stderr, "imgflicker: %s\n", loaded.error.c_str());
            return 2;
        }
        frames.push_back(std::move(loaded.image));
    }
    const engine::imgcmp::FlickerReport report = engine::imgcmp::measureFlicker(frames, ignore);
    if (!report.error.empty()) {
        std::fprintf(stderr, "imgflicker: %s\n%s", report.error.c_str(), Usage.data());
        return 2;
    }
    const bool within = most < 0.0 || report.flicker <= most;
    std::printf("imgflicker: %zu frames, flicker %.4f%s\n", report.frames, report.flicker, within ? "" : ": FAIL");
    return within ? 0 : 1;
}
