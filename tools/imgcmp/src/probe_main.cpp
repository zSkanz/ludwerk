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
// Two claims about a region, `x0,y0:x1,y1` in the same fractions:
//   `x0,y0:x1,y1~r,g,b/r,g,b/...` -- every pixel in it is one of these
//   colours. What says pixel art was scaled by the nearest texel: a filter
//   makes colours between them at every edge.
//   `x0,y0:x1,y1==` -- every pixel in it is the pixel of `--against=<png>`,
//   an image the same size: what says one part of two frames is the same
//   while the rest of them is not.
//
// And one about two points: `x,y>x2,y2` -- the first is brighter than the
// second by more than the tolerance (the sum of its channels); `x,y<x2,y2`
// darker. What says a thing brightens or darkens what it lands on, whatever
// the light and the tone curve make of the colours themselves.
//
// Developer-facing English, like `imgcmp` beside it (R3 governs the engine and
// games, not a repo tool).

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "engine/imgcmp/image.h"

namespace {

constexpr std::string_view kUsage =
    "usage: imgprobe <image.png> [--tolerance=N] [--against=<image.png>]\n"
    "                <x,y=r,g,b | x,y!=r,g,b | x0,y0:x1,y1~r,g,b/r,g,b/... | x0,y0:x1,y1== |\n"
    "                 x,y>x2,y2 | x,y<x2,y2> [...]\n"
    "  x and y are fractions of the image; r, g and b are 0-255.\n";

struct Probe
{
    double x = 0.0;
    double y = 0.0;
    int rgb[3]{};
    bool negate = false;
    std::string text;
};

// One point brighter or darker than another.
struct Comparison
{
    double x = 0.0;
    double y = 0.0;
    double otherX = 0.0;
    double otherY = 0.0;
    bool brighter = true;
    std::string text;
};

enum class RegionClaim
{
    Palette,
    Against,
};

struct Region
{
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 1.0;
    double y1 = 1.0;
    RegionClaim claim = RegionClaim::Palette;
    std::vector<std::array<int, 3>> palette;
    std::string text;
};

// Numbers separated by commas, all of them consumed.
[[nodiscard]] bool readNumbers(const std::string& list, double* values, int count)
{
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
}

[[nodiscard]] bool parseComparison(std::string_view text, Comparison& out)
{
    out.text = std::string(text);
    const std::size_t split = text.find_first_of("<>");
    if (split == std::string_view::npos || split == 0)
        return false;
    out.brighter = text[split] == '>';
    double first[2]{};
    double second[2]{};
    if (!readNumbers(std::string(text.substr(0, split)), first, 2) ||
        !readNumbers(std::string(text.substr(split + 1)), second, 2))
        return false;
    out.x = first[0];
    out.y = first[1];
    out.otherX = second[0];
    out.otherY = second[1];
    const auto inside = [](double value) { return value >= 0.0 && value <= 1.0; };
    return inside(out.x) && inside(out.y) && inside(out.otherX) && inside(out.otherY);
}

[[nodiscard]] bool parseRegion(std::string_view text, Region& out)
{
    out.text = std::string(text);
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos)
        return false;
    const std::string_view rest = text.substr(colon + 1);
    std::size_t split = rest.find("==");
    std::string_view claim;
    if (split != std::string_view::npos && split + 2 == rest.size()) {
        out.claim = RegionClaim::Against;
    }
    else {
        split = rest.find('~');
        if (split == std::string_view::npos)
            return false;
        out.claim = RegionClaim::Palette;
        claim = rest.substr(split + 1);
    }
    double low[2]{};
    double high[2]{};
    if (!readNumbers(std::string(text.substr(0, colon)), low, 2) ||
        !readNumbers(std::string(rest.substr(0, split)), high, 2))
        return false;
    out.x0 = low[0];
    out.y0 = low[1];
    out.x1 = high[0];
    out.y1 = high[1];
    while (!claim.empty()) {
        const std::size_t slash = claim.find('/');
        const std::string_view one = claim.substr(0, slash);
        double colour[3]{};
        if (!readNumbers(std::string(one), colour, 3))
            return false;
        out.palette.push_back({static_cast<int>(colour[0]), static_cast<int>(colour[1]), static_cast<int>(colour[2])});
        claim = slash == std::string_view::npos ? std::string_view{} : claim.substr(slash + 1);
    }
    if (out.claim == RegionClaim::Palette && out.palette.empty())
        return false;
    return out.x0 >= 0.0 && out.y0 >= 0.0 && out.x1 <= 1.0 && out.y1 <= 1.0 && out.x0 < out.x1 && out.y0 < out.y1;
}

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
    double position[2]{};
    double colour[3]{};
    if (!readNumbers(std::string(where), position, 2) || !readNumbers(std::string(text.substr(split + 1)), colour, 3))
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
    std::string against;
    std::vector<Probe> probes;
    std::vector<Region> regions;
    std::vector<Comparison> comparisons;
    for (std::size_t index = 1; index < args.size(); ++index) {
        const std::string_view arg = args[index];
        if (arg.starts_with("--tolerance=")) {
            tolerance = std::atoi(std::string(arg.substr(12)).c_str());
            continue;
        }
        if (arg.starts_with("--against=")) {
            against = std::string(arg.substr(10));
            continue;
        }
        if (arg.find_first_of("<>") != std::string_view::npos) {
            Comparison comparison;
            if (!parseComparison(arg, comparison)) {
                std::fprintf(stderr, "imgprobe: cannot read the comparison \"%s\"\n%s", std::string(arg).c_str(),
                             kUsage.data());
                return 2;
            }
            comparisons.push_back(std::move(comparison));
            continue;
        }
        if (arg.find(':') != std::string_view::npos) {
            Region region;
            if (!parseRegion(arg, region)) {
                std::fprintf(stderr, "imgprobe: cannot read the region \"%s\"\n%s", std::string(arg).c_str(),
                             kUsage.data());
                return 2;
            }
            regions.push_back(std::move(region));
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

    for (const Comparison& comparison : comparisons) {
        const auto sumAt = [&image](double x, double y) {
            const int px = std::min(static_cast<int>(x * image.width), image.width - 1);
            const int py = std::min(static_cast<int>(y * image.height), image.height - 1);
            const std::uint8_t* pixel =
                image.rgba.data() +
                (static_cast<std::size_t>(py) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(px)) *
                    4u;
            return static_cast<int>(pixel[0]) + static_cast<int>(pixel[1]) + static_cast<int>(pixel[2]);
        };
        const int first = sumAt(comparison.x, comparison.y);
        const int second = sumAt(comparison.otherX, comparison.otherY);
        const bool ok = comparison.brighter ? first > second + tolerance : first + tolerance < second;
        std::printf("%s %s: %d against %d\n", ok ? "ok  " : "FAIL", comparison.text.c_str(), first, second);
        if (!ok)
            ++failed;
    }

    engine::imgcmp::Image other;
    const bool needsOther = std::any_of(regions.begin(), regions.end(),
                                        [](const Region& region) { return region.claim == RegionClaim::Against; });
    if (needsOther) {
        const engine::imgcmp::LoadResult second =
            against.empty() ? engine::imgcmp::LoadResult{} : engine::imgcmp::loadPngFile(against);
        if (!second.ok || second.image.width != image.width || second.image.height != image.height) {
            std::fprintf(stderr, "imgprobe: a region compared needs --against=<png> the size of the image\n");
            return 2;
        }
        other = second.image;
    }
    const auto near = [tolerance](const std::uint8_t* pixel, const int* rgb) {
        for (int channel = 0; channel < 3; ++channel) {
            if (std::abs(static_cast<int>(pixel[channel]) - rgb[channel]) > tolerance)
                return false;
        }
        return true;
    };
    const auto offsetOf = [&image](int x, int y) {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) * 4u;
    };
    for (const Region& region : regions) {
        const int left = static_cast<int>(region.x0 * image.width);
        const int top = static_cast<int>(region.y0 * image.height);
        const int right = std::min(static_cast<int>(region.x1 * image.width), image.width);
        const int bottom = std::min(static_cast<int>(region.y1 * image.height), image.height);
        long long wrong = 0;
        long long total = 0;
        int firstX = -1;
        int firstY = -1;
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                const std::uint8_t* pixel = image.rgba.data() + offsetOf(x, y);
                bool ok = false;
                if (region.claim == RegionClaim::Against) {
                    const std::uint8_t* theirs = other.rgba.data() + offsetOf(x, y);
                    const int rgb[3]{theirs[0], theirs[1], theirs[2]};
                    ok = near(pixel, rgb);
                }
                else {
                    ok = std::any_of(region.palette.begin(), region.palette.end(),
                                     [&](const std::array<int, 3>& colour) { return near(pixel, colour.data()); });
                }
                ++total;
                if (!ok) {
                    if (wrong == 0) {
                        firstX = x;
                        firstY = y;
                    }
                    ++wrong;
                }
            }
        }
        const bool ok = wrong == 0 && total > 0;
        std::printf("%s %s: %lld of %lld pixels differ", ok ? "ok  " : "FAIL", region.text.c_str(), wrong, total);
        if (wrong > 0) {
            const std::uint8_t* pixel = image.rgba.data() + offsetOf(firstX, firstY);
            std::printf(", the first at (%d, %d), %d,%d,%d", firstX, firstY, pixel[0], pixel[1], pixel[2]);
        }
        std::printf("\n");
        if (!ok)
            ++failed;
    }
    return failed == 0 ? 0 : 1;
}
