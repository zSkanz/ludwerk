#include "engine/app/baked_light.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/render/render_world.h"

namespace engine::app {
namespace {

using core::f32;
using core::f64;
using core::u32;
using core::u64;
using core::usize;

constexpr char Magic[8] = {'E', 'N', 'G', 'L', 'I', 'G', 'H', 'T'};
constexpr u32 Version = 1;
// As many tiles as a volume is ever made of.
constexpr u32 MostPieces = 16;

// Every target this builds for keeps its numbers low byte first, and the file
// is those bytes.
template <class T>
void put(std::vector<std::byte>& out, const T& value)
{
    const usize at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

struct Reader
{
    std::span<const std::byte> bytes;
    usize at = 0;
    bool ok = true;

    template <class T>
    T take()
    {
        T value{};
        if (!ok || bytes.size() - at < sizeof(T)) {
            ok = false;
            return value;
        }
        std::memcpy(&value, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return value;
    }
    f32 number()
    {
        const f32 value = take<f32>();
        ok = ok && std::isfinite(value);
        return value;
    }
};

} // namespace

core::Color3 bakedMean(std::span<const BakedPiece> pieces) noexcept
{
    f64 sum[3] = {0.0, 0.0, 0.0};
    usize points = 0;
    for (const BakedPiece& piece : pieces) {
        for (usize at = 0; at + 12 <= piece.samples.size(); at += 12) {
            sum[0] += static_cast<f64>(piece.samples[at]);
            sum[1] += static_cast<f64>(piece.samples[at + 1]);
            sum[2] += static_cast<f64>(piece.samples[at + 2]);
            ++points;
        }
    }
    if (points == 0)
        return {};
    const f64 each = 1.0 / static_cast<f64>(points);
    return core::Color3{static_cast<f32>(std::max(sum[0] * each, 0.0)), static_cast<f32>(std::max(sum[1] * each, 0.0)),
                        static_cast<f32>(std::max(sum[2] * each, 0.0))};
}

std::vector<std::byte> writeBakedLight(const BakedLight& light)
{
    std::vector<std::byte> out;
    usize numbers = 0;
    for (const BakedPiece& piece : light.pieces)
        numbers += piece.samples.size();
    out.reserve(160 + light.pieces.size() * 12 + numbers * sizeof(f32));
    for (const char letter : Magic)
        put(out, letter);
    put(out, Version);
    put(out, light.room);
    put(out, light.box.position.x);
    put(out, light.box.position.y);
    put(out, light.box.position.z);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column)
            put(out, light.box.rotation.m[row][column]);
    }
    put(out, light.size.x);
    put(out, light.size.y);
    put(out, light.size.z);
    put(out, light.spacing);
    put(out, light.bounces);
    put(out, light.mode);
    const core::Color3 mean = bakedMean(light.pieces);
    put(out, mean.r);
    put(out, mean.g);
    put(out, mean.b);
    put(out, static_cast<u32>(light.pieces.size()));
    for (const BakedPiece& piece : light.pieces) {
        put(out, piece.counts[0]);
        put(out, piece.counts[1]);
        put(out, piece.counts[2]);
        const usize at = out.size();
        out.resize(at + piece.samples.size() * sizeof(f32));
        if (!piece.samples.empty())
            std::memcpy(out.data() + at, piece.samples.data(), piece.samples.size() * sizeof(f32));
    }
    return out;
}

bool readBakedLight(std::span<const std::byte> bytes, BakedLight& out)
{
    out = BakedLight{};
    Reader read{bytes};
    for (const char letter : Magic) {
        if (read.take<char>() != letter)
            return false;
    }
    if (read.take<u32>() != Version)
        return false;
    out.room = read.take<u64>();
    out.box.position.x = read.take<f64>();
    out.box.position.y = read.take<f64>();
    out.box.position.z = read.take<f64>();
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column)
            out.box.rotation.m[row][column] = read.number();
    }
    out.size.x = read.number();
    out.size.y = read.number();
    out.size.z = read.number();
    out.spacing = read.number();
    out.bounces = read.take<core::i32>();
    out.mode = read.take<core::i32>();
    out.mean.r = read.number();
    out.mean.g = read.number();
    out.mean.b = read.number();
    const u32 pieces = read.take<u32>();
    if (!read.ok || pieces == 0 || pieces > MostPieces || !std::isfinite(out.box.position.x) ||
        !std::isfinite(out.box.position.y) || !std::isfinite(out.box.position.z))
        return false;
    out.pieces.resize(pieces);
    for (BakedPiece& piece : out.pieces) {
        for (u32& count : piece.counts) {
            count = read.take<u32>();
            if (!read.ok || count == 0 || count > render::kIrradianceVolumeSide)
                return false;
        }
        const usize numbers = static_cast<usize>(piece.counts[0]) * piece.counts[1] * piece.counts[2] * 12;
        if (bytes.size() - read.at < numbers * sizeof(f32))
            return false;
        piece.samples.resize(numbers);
        std::memcpy(piece.samples.data(), bytes.data() + read.at, numbers * sizeof(f32));
        read.at += numbers * sizeof(f32);
        for (const f32 value : piece.samples) {
            if (!std::isfinite(value))
                return false;
        }
    }
    // Nothing after the last piece: a file with more in it is another file.
    return read.ok && read.at == bytes.size();
}

} // namespace engine::app
