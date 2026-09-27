// `imgprobe` -- asserts the colour of named points in a screenshot.
//
// A golden image says "this frame, exactly"; this says "here, this colour" --
// which is what a claim about a feature usually is. The left end of a health
// bar is red and its right end green; a stroke sits outside its box and not
// inside it. A probe survives every change to the frame that does not break
// the claim, which a golden does not.
//
// Each probe is `x,y=r,g,b` with `x` and `y` fractions of the image (so a
// probe holds at any resolution) and the channels 0-255; `--tolerance=N` is
// the per-channel slack, 24 by default -- a rasteriser's rounding and a
// gradient's sampling, not a different colour. `x,y!=r,g,b` asserts the
// opposite: that the colour is NOT within the tolerance there.
//
// Developer-facing English, like `imgcmp` beside it (R3 governs the engine and
// games, not a repo tool).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "engine/imgcmp/image.h"

namespace {

constexpr std::string_view kUsage = "usage: imgprobe <image.png> [--tolerance=N] <x,y=r,g,b | x,y!=r,g,b> [...]\n"
                                    "  x and y are fractions of the image; r, g and b are 0-255.\n";

struct Probe
{
    double x = 0.0;
    double y = 0.0;
    int rgb[3]{};
    bool negate = false;
    std::string text;
};

[[nodiscard]] bool parseProbe(std::string_view text, Probe& out)
{
    out.text = std::string(text);
    const std::size_t split = text.find('=');
    if (split == std::string_view::npos || split == 0)
        return false;
    std::string_view where = text.substr(0, split);
    if (where.back() == '!') {
        out.negate = true;
        where.remove_suffix(1);
    }
    // Numbers separated by commas, all of them consumed.
    const auto numbers = [](const std::string& list, double* values, int count) {
        const char* cursor = list.c_str();
        for (int index = 0; index < count; ++index) {
            char* end = nullptr;
            values[index] = std::strtod(cursor, &end);
            if (end == cursor)
                return false;
            cursor = end;
            if (index + 1 < count) {
                if (*cursor != ',')
                    return false;
                ++cursor;
            }
        }
        return *cursor == '\0';
    };
    double position[2]{};
    double colour[3]{};
    if (!numbers(std::string(where), position, 2) || !numbers(std::string(text.substr(split + 1)), colour, 3))
        return false;
    out.x = position[0];
    out.y = position[1];
    for (int channel = 0; channel < 3; ++channel)
        out.rgb[channel] = static_cast<int>(colour[channel]);
    return out.x >= 0.0 && out.x <= 1.0 && out.y >= 0.0 && out.y <= 1.0;
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() < 2) {
        std::fputs(kUsage.data(), stderr);
        return 2;
    }

    int tolerance = 24;
    std::vector<Probe> probes;
    for (std::size_t index = 1; index < args.size(); ++index) {
        const std::string_view arg = args[index];
        if (arg.starts_with("--tolerance=")) {
            tolerance = std::atoi(std::string(arg.substr(12)).c_str());
            continue;
        }
        Probe probe;
        if (!parseProbe(arg, probe)) {
            std::fprintf(stderr, "imgprobe: cannot read the probe \"%s\"\n%s", std::string(arg).c_str(), kUsage.data());
            return 2;
        }
        probes.push_back(probe);
    }

    const engine::imgcmp::LoadResult loaded = engine::imgcmp::loadPngFile(std::string(args[0]));
    if (!loaded.ok) {
        std::fprintf(stderr, "imgprobe: %s\n", loaded.error.c_str());
        return 2;
    }
    const engine::imgcmp::Image& image = loaded.image;

    int failed = 0;
    for (const Probe& probe : probes) {
        const int px = std::min(static_cast<int>(probe.x * image.width), image.width - 1);
        const int py = std::min(static_cast<int>(probe.y * image.height), image.height - 1);
        const std::uint8_t* pixel =
            image.rgba.data() +
            (static_cast<std::size_t>(py) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(px)) * 4u;
        bool within = true;
        for (int channel = 0; channel < 3; ++channel) {
            if (std::abs(static_cast<int>(pixel[channel]) - probe.rgb[channel]) > tolerance)
                within = false;
        }
        const bool ok = probe.negate ? !within : within;
        std::printf("%s %s: (%d, %d) is %d,%d,%d\n", ok ? "ok  " : "FAIL", probe.text.c_str(), px, py, pixel[0],
                    pixel[1], pixel[2]);
        if (!ok)
            ++failed;
    }
    return failed == 0 ? 0 : 1;
}
