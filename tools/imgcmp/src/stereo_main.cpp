// `imgstereo` -- whether two eyes' pictures of one scene fuse.
//
// A pair of eyes looks along parallel axes from two points a head's width
// apart, so a point of the world lands on the same ROW of both pictures and
// only its column differs, by a disparity that shrinks with its distance. A
// row that differs -- a vertical disparity -- is what a pair of eyes cannot
// fuse: the picture doubles, or the brain fuses it at a depth the world does
// not have, and the shadow under a cube seems not to touch it.
//
// The scene holds markers of three colours, each its own blob: red, green
// and yellow. For each, this finds its centre in both pictures and prints
// the rows and columns, and fails where the rows differ by more than
// `--rows=N` pixels (one by default), or where the right eye sees a marker
// further right than the left does (the eyes swapped), or where a marker is
// missing from either picture.
//
// Developer-facing English, like `imgcmp` beside it (R3 governs the engine and
// games, not a repo tool).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "engine/imgcmp/image.h"

namespace {

constexpr std::string_view kUsage = "usage: imgstereo <left.png> <right.png> [--rows=N]\n";

struct Centre
{
    double x = 0.0;
    double y = 0.0;
    long count = 0;
};

enum class Marker
{
    Red,
    Green,
    Yellow,
};

// A marker's pixels by which channels lead, not by their values: the light,
// the tone curve and the sky change every value, and not which channel is
// the largest by far.
bool isMarker(Marker marker, int r, int g, int b)
{
    switch (marker) {
    case Marker::Red:
        return r > 90 && r > 2 * g && r > 2 * b;
    case Marker::Green:
        return g > 90 && g > 2 * r && g > 2 * b;
    case Marker::Yellow:
        return r > 90 && g > 90 && r > 2 * b && g > 2 * b;
    }
    return false;
}

Centre centreOf(const engine::imgcmp::Image& image, Marker marker)
{
    Centre centre;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) *
                4u;
            if (isMarker(marker, image.rgba[at], image.rgba[at + 1], image.rgba[at + 2])) {
                centre.x += x;
                centre.y += y;
                ++centre.count;
            }
        }
    }
    if (centre.count > 0) {
        centre.x /= static_cast<double>(centre.count);
        centre.y /= static_cast<double>(centre.count);
    }
    return centre;
}

} // namespace

int main(int argc, char** argv)
{
    std::string leftPath;
    std::string rightPath;
    double rows = 1.0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--rows="))
            rows = std::atof(std::string(arg.substr(7)).c_str());
        else if (leftPath.empty())
            leftPath = std::string(arg);
        else if (rightPath.empty())
            rightPath = std::string(arg);
    }
    if (leftPath.empty() || rightPath.empty()) {
        std::fputs(std::string(kUsage).c_str(), stderr);
        return 2;
    }
    const engine::imgcmp::LoadResult left = engine::imgcmp::loadPngFile(leftPath);
    const engine::imgcmp::LoadResult right = engine::imgcmp::loadPngFile(rightPath);
    if (!left.ok || !right.ok) {
        std::fprintf(stderr, "imgstereo: %s\n", (!left.ok ? left.error : right.error).c_str());
        return 2;
    }

    bool fused = true;
    double worst = 0.0;
    const char* names[] = {"red", "green", "yellow"};
    const Marker markers[] = {Marker::Red, Marker::Green, Marker::Yellow};
    for (int index = 0; index < 3; ++index) {
        const Centre l = centreOf(left.image, markers[index]);
        const Centre r = centreOf(right.image, markers[index]);
        if (l.count < 20 || r.count < 20) {
            std::printf("imgstereo: the %s marker is missing (left %ld px, right %ld px)\n", names[index], l.count,
                        r.count);
            fused = false;
            continue;
        }
        const double vertical = r.y - l.y;
        const double horizontal = r.x - l.x;
        worst = std::max(worst, std::fabs(vertical));
        std::printf("imgstereo: %s at (%.1f, %.1f) left, (%.1f, %.1f) right: %.2f px across, %.2f px up or down\n",
                    names[index], l.x, l.y, r.x, r.y, horizontal, vertical);
        if (std::fabs(vertical) > rows)
            fused = false;
        // The right eye sits to the right, so it sees every point further
        // left than the left eye does.
        if (horizontal > 0.5) {
            std::printf("imgstereo: the right eye sees the %s marker to the right of where the left eye does: the "
                        "eyes are swapped\n",
                        names[index]);
            fused = false;
        }
    }
    if (!fused)
        return 1;
    std::printf("imgstereo: the eyes fuse: no marker differs by more than %.2f px in height\n", worst);
    return 0;
}
