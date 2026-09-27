#include "engine/asset/icon_set.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "engine/core/i18n.h"

namespace engine::asset {
namespace {

using core::u32;

constexpr std::array<u32, 7> WindowsSizes{16, 24, 32, 48, 64, 128, 256};
constexpr u32 LinuxSize = 512;
constexpr u32 SmallWarning = 256;

// Android's densities: the legacy launcher icon is 48 dp and the adaptive
// layers 108 dp, scaled by each density's factor.
struct Density
{
    const char* name;
    u32 launcher;
    u32 adaptive;
};
constexpr std::array<Density, 5> Densities{{
    {"mdpi", 48, 108},
    {"hdpi", 72, 162},
    {"xhdpi", 96, 216},
    {"xxhdpi", 144, 324},
    {"xxxhdpi", 192, 432},
}};

[[nodiscard]] bool writeBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

[[nodiscard]] bool writeText(const std::filesystem::path& path, const std::string& text)
{
    std::vector<std::byte> bytes(text.size());
    std::transform(text.begin(), text.end(), bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return writeBytes(path, bytes);
}

void putU16(std::vector<std::byte>& out, u32 value)
{
    out.push_back(static_cast<std::byte>(value & 0xFF));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
}

void putU32(std::vector<std::byte>& out, u32 value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
}

// The picture centred on a transparent canvas of `canvas` pixels, at `inner`.
[[nodiscard]] Image padded(const Image& source, u32 canvas, u32 inner)
{
    Image out;
    out.width = canvas;
    out.height = canvas;
    out.sourceChannels = 4;
    out.pixels.assign(static_cast<std::size_t>(canvas) * canvas * 4u, std::byte{0});
    const Image scaled = resizeImage(source, inner);
    const u32 offset = (canvas - inner) / 2;
    for (u32 y = 0; y < inner; ++y) {
        const std::byte* from = scaled.pixels.data() + static_cast<std::size_t>(y) * inner * 4u;
        std::byte* to = out.pixels.data() + (static_cast<std::size_t>(y + offset) * canvas + offset) * 4u;
        std::copy(from, from + static_cast<std::size_t>(inner) * 4u, to);
    }
    return out;
}

[[nodiscard]] std::optional<core::EngineError>
writePngFile(const std::filesystem::path& root, const std::string& relative, const Image& image, IconSetReport& report)
{
    std::vector<std::byte> png;
    if (std::optional<core::EngineError> error = encodePng(image.pixels, image.width, image.height, png))
        return error;
    if (!writeBytes(root / relative, png)) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"path", (root / relative).string()}};
        return core::makeError(ENG_TR("asset.image.err.write_failed"), args);
    }
    report.written.push_back(relative);
    return std::nullopt;
}

} // namespace

Image resizeImage(const Image& source, u32 size)
{
    Image out;
    out.width = size;
    out.height = size;
    out.sourceChannels = 4;
    out.pixels.assign(static_cast<std::size_t>(size) * size * 4u, std::byte{0});
    if (!source.valid() || size == 0)
        return out;

    // A box filter over premultiplied colour: each output pixel averages the
    // source area it covers, weighted by how much of each source pixel falls in
    // it, so a transparent corner does not bleed a dark fringe into the edge.
    const double scaleX = static_cast<double>(source.width) / size;
    const double scaleY = static_cast<double>(source.height) / size;
    const auto at = [&](u32 x, u32 y, u32 channel) {
        return static_cast<double>(
            std::to_integer<u32>(source.pixels[(static_cast<std::size_t>(y) * source.width + x) * 4u + channel]));
    };
    for (u32 oy = 0; oy < size; ++oy) {
        const double y0 = oy * scaleY;
        const double y1 = y0 + scaleY;
        for (u32 ox = 0; ox < size; ++ox) {
            const double x0 = ox * scaleX;
            const double x1 = x0 + scaleX;
            double r = 0.0;
            double g = 0.0;
            double b = 0.0;
            double a = 0.0;
            double area = 0.0;
            for (u32 sy = static_cast<u32>(y0); sy < source.height && sy < y1; ++sy) {
                const double wy = std::min<double>(sy + 1.0, y1) - std::max<double>(sy, y0);
                for (u32 sx = static_cast<u32>(x0); sx < source.width && sx < x1; ++sx) {
                    const double wx = std::min<double>(sx + 1.0, x1) - std::max<double>(sx, x0);
                    const double weight = wx * wy;
                    const double alpha = at(sx, sy, 3) / 255.0;
                    r += at(sx, sy, 0) * alpha * weight;
                    g += at(sx, sy, 1) * alpha * weight;
                    b += at(sx, sy, 2) * alpha * weight;
                    a += alpha * weight;
                    area += weight;
                }
            }
            std::byte* pixel = out.pixels.data() + (static_cast<std::size_t>(oy) * size + ox) * 4u;
            if (area <= 0.0 || a <= 0.0)
                continue;
            const auto channel = [](double value) {
                return static_cast<std::byte>(static_cast<u32>(std::clamp(value + 0.5, 0.0, 255.0)));
            };
            pixel[0] = channel(r / a);
            pixel[1] = channel(g / a);
            pixel[2] = channel(b / a);
            pixel[3] = channel(a / area * 255.0);
        }
    }
    return out;
}

std::optional<core::EngineError> encodeIco(const std::vector<Image>& images, std::vector<std::byte>& out)
{
    out.clear();
    std::vector<std::vector<std::byte>> encoded;
    for (const Image& image : images) {
        std::vector<std::byte> png;
        if (std::optional<core::EngineError> error = encodePng(image.pixels, image.width, image.height, png))
            return error;
        encoded.push_back(std::move(png));
    }
    putU16(out, 0);
    putU16(out, 1);
    putU16(out, static_cast<u32>(images.size()));
    u32 offset = 6 + 16 * static_cast<u32>(images.size());
    for (std::size_t index = 0; index < images.size(); ++index) {
        // A side of 256 is written as 0: the field is a byte.
        out.push_back(static_cast<std::byte>(images[index].width >= 256 ? 0 : images[index].width));
        out.push_back(static_cast<std::byte>(images[index].height >= 256 ? 0 : images[index].height));
        out.push_back(std::byte{0});
        out.push_back(std::byte{0});
        putU16(out, 1);
        putU16(out, 32);
        putU32(out, static_cast<u32>(encoded[index].size()));
        putU32(out, offset);
        offset += static_cast<u32>(encoded[index].size());
    }
    for (const std::vector<std::byte>& png : encoded)
        out.insert(out.end(), png.begin(), png.end());
    return std::nullopt;
}

std::optional<core::EngineError> writeIconSet(const Image& source, const std::filesystem::path& directory,
                                              const IconSetOptions& options, IconSetReport& report)
{
    if (!source.valid())
        return core::makeError(ENG_TR("asset.image.err.bad_pixels"));
    if (source.width != source.height) {
        const std::array<core::I18nArg, 2> args{core::I18nArg{"width", static_cast<core::i64>(source.width)},
                                                core::I18nArg{"height", static_cast<core::i64>(source.height)}};
        return core::makeError(ENG_TR("asset.icon.err.not_square"), args);
    }
    if (source.width < SmallWarning) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"size", static_cast<core::i64>(source.width)}};
        report.warnings.push_back(core::engineCatalog().format(ENG_TR("asset.icon.warn.small"), args));
    }

    if (options.windows) {
        std::vector<Image> sizes;
        for (const u32 size : WindowsSizes)
            sizes.push_back(resizeImage(source, size));
        std::vector<std::byte> ico;
        if (std::optional<core::EngineError> error = encodeIco(sizes, ico))
            return error;
        if (!writeBytes(directory / "windows" / "icon.ico", ico)) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"path", (directory / "windows").string()}};
            return core::makeError(ENG_TR("asset.image.err.write_failed"), args);
        }
        report.written.emplace_back("windows/icon.ico");
    }

    if (options.linuxDesktop) {
        if (std::optional<core::EngineError> error =
                writePngFile(directory, "linux/icon.png", resizeImage(source, LinuxSize), report))
            return error;
    }

    if (options.android) {
        const std::string res = "android/res/";
        for (const Density& density : Densities) {
            const std::string folder = res + "mipmap-" + density.name + "/";
            if (std::optional<core::EngineError> error =
                    writePngFile(directory, folder + "ic_launcher.png", resizeImage(source, density.launcher), report))
                return error;
            // The foreground layer: a drawn one full-bleed, or the icon inside
            // the 72-of-108 dp the launcher never masks away.
            const Image foreground = options.foreground.has_value()
                                         ? resizeImage(*options.foreground, density.adaptive)
                                         : padded(source, density.adaptive, density.adaptive * 2 / 3);
            if (std::optional<core::EngineError> error =
                    writePngFile(directory, folder + "ic_launcher_foreground.png", foreground, report))
                return error;
        }
        const std::string adaptive = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                     "<adaptive-icon xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
                                     "    <background android:drawable=\"@color/ic_launcher_background\"/>\n"
                                     "    <foreground android:drawable=\"@mipmap/ic_launcher_foreground\"/>\n"
                                     "</adaptive-icon>\n";
        char colour[8];
        std::snprintf(colour, sizeof(colour), "#%06X", options.background & 0xFFFFFFu);
        const std::string background = std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n"
                                                   "    <color name=\"ic_launcher_background\">") +
                                       colour + "</color>\n</resources>\n";
        if (!writeText(directory / "android" / "res" / "mipmap-anydpi-v26" / "ic_launcher.xml", adaptive) ||
            !writeText(directory / "android" / "res" / "values" / "ic_launcher_background.xml", background)) {
            const std::array<core::I18nArg, 1> args{core::I18nArg{"path", (directory / "android").string()}};
            return core::makeError(ENG_TR("asset.image.err.write_failed"), args);
        }
        report.written.emplace_back(res + "mipmap-anydpi-v26/ic_launcher.xml");
        report.written.emplace_back(res + "values/ic_launcher_background.xml");
    }
    return std::nullopt;
}

} // namespace engine::asset
