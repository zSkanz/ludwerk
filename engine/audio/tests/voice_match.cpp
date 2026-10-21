// **Whether what a machine heard is what was said** (ADR 0256): the voices
// one joiner of `netcode_voice` wrote with `--voice-out`, against the phrase
// the other spoke with `--voice-in` -- and against a phrase nobody spoke, so
// a voice heard from the wrong speaker, or the listener's own, fails.
//
// Played frames and a level prove there was sound. This proves it was the
// speech: both files are cut into 20 ms frames every 10 ms, each frame's
// spectral envelope taken as the log energy of 24 mel bands, and the heard
// frames lined up against the phrase -- looped, as the speaker looped it --
// by the offset whose envelopes correlate best. Each second after the first
// is lined up again within a few frames of the one before: a jitter buffer
// that ran dry and filled again moves the rest of the voice by a frame, and
// no further, so a phrase that was not said cannot be fitted second by
// second. The similarity is the mean
// correlation of a voiced heard frame's envelope with the phrase's frame at
// that offset: near 1 for the same speech through a codec, near 0 for
// another voice or noise, and lower for a stream heard twice over itself.
//
// With the steady clock's time the speaker's microphone opened and the
// listener's recording began, the offset is also the mouth-to-ear latency:
// the capture's frame, the codec, the link, and the jitter buffer.
//
// Invoked as:
//   voice_match <heard.wav> <phrase.wav> <other-phrase.wav> <recorded-from-ns> <spoken-from-ns>
// Prints one line, `[voice-match] similarity=... other=... margin=...
// voiced=... latency_ms=...`, and exits 0; 2 when a file cannot be read.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr int Rate = 16000;
constexpr int FrameSamples = 320;
constexpr int HopSamples = 160;
constexpr int Bins = FrameSamples / 2 + 1;
constexpr int Bands = 24;
constexpr double Pi = 3.14159265358979323846;
constexpr int SegmentFrames = 100;
// How far, in 10 ms hops, a second may sit from the one before it.
constexpr std::size_t Drift = 8;

struct Sound
{
    std::vector<float> samples;
    int rate = 0;
};

[[nodiscard]] std::uint32_t le32(const unsigned char* at)
{
    return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8u) |
           (static_cast<std::uint32_t>(at[2]) << 16u) | (static_cast<std::uint32_t>(at[3]) << 24u);
}

[[nodiscard]] std::uint16_t le16(const unsigned char* at)
{
    return static_cast<std::uint16_t>(at[0] | (at[1] << 8u));
}

// 16-bit PCM, any channel count, mixed to mono.
[[nodiscard]] std::optional<Sound> readWav(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return std::nullopt;
    Sound sound;
    int channels = 0;
    int bits = 0;
    for (std::size_t at = 12; at + 8 <= bytes.size();) {
        const std::uint32_t size = le32(bytes.data() + at + 4);
        const unsigned char* body = bytes.data() + at + 8;
        if (at + 8 + size > bytes.size())
            break;
        if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0 && size >= 16) {
            channels = le16(body + 2);
            sound.rate = static_cast<int>(le32(body + 4));
            bits = le16(body + 14);
        }
        else if (std::memcmp(bytes.data() + at, "data", 4) == 0 && channels > 0 && bits == 16) {
            const std::size_t frames = size / (2u * static_cast<std::size_t>(channels));
            sound.samples.resize(frames);
            for (std::size_t frame = 0; frame < frames; ++frame) {
                float sum = 0.0f;
                for (int channel = 0; channel < channels; ++channel) {
                    const auto value = static_cast<std::int16_t>(le16(
                        body + (frame * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel)) * 2));
                    sum += static_cast<float>(value) / 32768.0f;
                }
                sound.samples[frame] = sum / static_cast<float>(channels);
            }
        }
        at += 8 + size + (size & 1u);
    }
    if (sound.rate <= 0 || sound.samples.empty())
        return std::nullopt;
    return sound;
}

// To 16 kHz, linearly: what the envelope is taken at. A 48 kHz file is
// averaged over each three samples first, so what is above 8 kHz does not
// fold down into the bands.
[[nodiscard]] std::vector<float> at16k(const Sound& sound)
{
    if (sound.rate == Rate)
        return sound.samples;
    std::vector<float> source = sound.samples;
    double rate = sound.rate;
    if (sound.rate == 3 * Rate) {
        std::vector<float> averaged(source.size() / 3);
        for (std::size_t at = 0; at < averaged.size(); ++at)
            averaged[at] = (source[at * 3] + source[at * 3 + 1] + source[at * 3 + 2]) / 3.0f;
        return averaged;
    }
    const auto count = static_cast<std::size_t>(static_cast<double>(source.size()) * Rate / rate);
    std::vector<float> out(count);
    for (std::size_t at = 0; at < count; ++at) {
        const double position = static_cast<double>(at) * rate / Rate;
        const auto low = static_cast<std::size_t>(position);
        const std::size_t high = std::min(low + 1, source.size() - 1);
        const auto part = static_cast<float>(position - static_cast<double>(low));
        out[at] = source[low] * (1.0f - part) + source[high] * part;
    }
    return out;
}

struct Envelopes
{
    // Per frame: the bands' log energies, mean removed and scaled to unit
    // length, so a dot product is a correlation; and the frame's level.
    std::vector<std::array<float, Bands>> shape;
    std::vector<float> levelDb;
};

[[nodiscard]] Envelopes envelopes(const std::vector<float>& samples)
{
    static const auto tables = [] {
        struct Tables
        {
            std::vector<float> window = std::vector<float>(FrameSamples);
            std::vector<float> cosines = std::vector<float>(static_cast<std::size_t>(Bins) * FrameSamples);
            std::vector<float> sines = std::vector<float>(static_cast<std::size_t>(Bins) * FrameSamples);
            std::array<std::vector<float>, Bands> weights{};
        } made;
        for (int n = 0; n < FrameSamples; ++n)
            made.window[static_cast<std::size_t>(n)] =
                static_cast<float>(0.5 - 0.5 * std::cos(2.0 * Pi * n / (FrameSamples - 1)));
        for (int k = 0; k < Bins; ++k)
            for (int n = 0; n < FrameSamples; ++n) {
                const double angle = 2.0 * Pi * k * n / FrameSamples;
                made.cosines[static_cast<std::size_t>(k * FrameSamples + n)] = static_cast<float>(std::cos(angle));
                made.sines[static_cast<std::size_t>(k * FrameSamples + n)] = static_cast<float>(std::sin(angle));
            }
        const auto mel = [](double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); };
        const auto hz = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };
        const double low = mel(100.0);
        const double high = mel(7000.0);
        for (int band = 0; band < Bands; ++band) {
            const double left = hz(low + (high - low) * band / (Bands + 1));
            const double middle = hz(low + (high - low) * (band + 1) / (Bands + 1));
            const double right = hz(low + (high - low) * (band + 2) / (Bands + 1));
            std::vector<float>& weight = made.weights[static_cast<std::size_t>(band)];
            weight.assign(Bins, 0.0f);
            for (int k = 0; k < Bins; ++k) {
                const double f = static_cast<double>(k) * Rate / FrameSamples;
                if (f > left && f < middle)
                    weight[static_cast<std::size_t>(k)] = static_cast<float>((f - left) / (middle - left));
                else if (f >= middle && f < right)
                    weight[static_cast<std::size_t>(k)] = static_cast<float>((right - f) / (right - middle));
            }
        }
        return made;
    }();

    Envelopes out;
    std::array<float, Bins> power{};
    for (std::size_t start = 0; start + FrameSamples <= samples.size(); start += HopSamples) {
        double energy = 0.0;
        for (int n = 0; n < FrameSamples; ++n)
            energy += static_cast<double>(samples[start + static_cast<std::size_t>(n)]) *
                      samples[start + static_cast<std::size_t>(n)];
        out.levelDb.push_back(static_cast<float>(10.0 * std::log10(energy / FrameSamples + 1e-12)));
        for (int k = 0; k < Bins; ++k) {
            double re = 0.0;
            double im = 0.0;
            for (int n = 0; n < FrameSamples; ++n) {
                const double x = static_cast<double>(samples[start + static_cast<std::size_t>(n)]) *
                                 tables.window[static_cast<std::size_t>(n)];
                re += x * tables.cosines[static_cast<std::size_t>(k * FrameSamples + n)];
                im -= x * tables.sines[static_cast<std::size_t>(k * FrameSamples + n)];
            }
            power[static_cast<std::size_t>(k)] = static_cast<float>(re * re + im * im);
        }
        std::array<float, Bands> shape{};
        for (int band = 0; band < Bands; ++band) {
            double sum = 0.0;
            const std::vector<float>& weight = tables.weights[static_cast<std::size_t>(band)];
            for (int k = 0; k < Bins; ++k)
                sum += static_cast<double>(weight[static_cast<std::size_t>(k)]) * power[static_cast<std::size_t>(k)];
            shape[static_cast<std::size_t>(band)] = static_cast<float>(std::log10(sum + 1e-10));
        }
        out.shape.push_back(shape);
    }

    // **Each band against its own average over the file**, then each frame
    // to unit length: what every voice has in common -- more energy low than
    // high -- would otherwise correlate any two voices, and what is left is
    // how this speech's spectrum moves from sound to sound. Taken over the
    // frames with a voice in them, so the silence does not set the average.
    const float loudest = out.levelDb.empty() ? -120.0f : *std::max_element(out.levelDb.begin(), out.levelDb.end());
    const float floor = std::max(-55.0f, loudest - 35.0f);
    std::array<double, Bands> average{};
    std::size_t voiced = 0;
    for (std::size_t frame = 0; frame < out.shape.size(); ++frame) {
        if (out.levelDb[frame] < floor)
            continue;
        for (int band = 0; band < Bands; ++band)
            average[static_cast<std::size_t>(band)] += out.shape[frame][static_cast<std::size_t>(band)];
        voiced += 1;
    }
    for (double& value : average)
        value = voiced > 0 ? value / static_cast<double>(voiced) : 0.0;
    for (std::array<float, Bands>& shape : out.shape) {
        float length = 0.0f;
        for (int band = 0; band < Bands; ++band) {
            float& value = shape[static_cast<std::size_t>(band)];
            value -= static_cast<float>(average[static_cast<std::size_t>(band)]);
            length += value * value;
        }
        length = std::sqrt(length);
        for (float& value : shape)
            value = length > 0.0f ? value / length : 0.0f;
    }
    return out;
}

[[nodiscard]] float dot(const std::array<float, Bands>& a, const std::array<float, Bands>& b)
{
    float sum = 0.0f;
    for (int band = 0; band < Bands; ++band)
        sum += a[static_cast<std::size_t>(band)] * b[static_cast<std::size_t>(band)];
    return sum;
}

struct Match
{
    double similarity = 0.0;
    // The offset of the first second lined up, in frames: heard frame `j` is
    // the phrase's frame `j + offset`, looped.
    std::size_t offset = 0;
    std::size_t voiced = 0;
};

// The heard frames with a voice in them, a second at a time, each lined up
// against the looped phrase by the offset that correlates best.
[[nodiscard]] Match match(const Envelopes& heard, const Envelopes& phrase)
{
    Match result;
    const float loudest =
        heard.levelDb.empty() ? -120.0f : *std::max_element(heard.levelDb.begin(), heard.levelDb.end());
    const float floor = std::max(-55.0f, loudest - 35.0f);
    const std::size_t looped = phrase.shape.size();
    double total = 0.0;
    bool first = true;
    std::size_t previous = 0;
    for (std::size_t segment = 0; segment < heard.shape.size(); segment += SegmentFrames) {
        const std::size_t end = std::min(segment + SegmentFrames, heard.shape.size());
        std::vector<std::size_t> voiced;
        for (std::size_t j = segment; j < end; ++j)
            if (heard.levelDb[j] >= floor)
                voiced.push_back(j);
        if (voiced.size() < SegmentFrames / 4)
            continue;
        // The first second is lined up at any offset; each one after only
        // near the last, since a voice moves by a frame when its buffer
        // refills and not by a word -- so a phrase that was not said cannot
        // be fitted to each second by itself.
        double best = -1e30;
        std::size_t bestOffset = 0;
        const std::size_t span = first ? looped : 2 * Drift + 1;
        for (std::size_t step = 0; step < span; ++step) {
            const std::size_t offset = first ? step : (previous + looped - Drift + step) % looped;
            double sum = 0.0;
            for (const std::size_t j : voiced)
                sum += dot(heard.shape[j], phrase.shape[(j + offset) % looped]);
            if (sum > best) {
                best = sum;
                bestOffset = offset;
            }
        }
        total += best;
        result.voiced += voiced.size();
        previous = bestOffset;
        if (first) {
            result.offset = bestOffset;
            first = false;
        }
    }
    if (result.voiced > 0)
        result.similarity = total / static_cast<double>(result.voiced);
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 6) {
        std::fprintf(stderr, "voice_match <heard.wav> <phrase.wav> <other-phrase.wav> <recorded-from-ns> "
                             "<spoken-from-ns>\n");
        return 2;
    }
    const std::optional<Sound> heard = readWav(argv[1]);
    const std::optional<Sound> phrase = readWav(argv[2]);
    const std::optional<Sound> other = readWav(argv[3]);
    if (!heard || !phrase || !other) {
        std::fprintf(stderr, "voice_match: a file could not be read as 16-bit PCM WAV\n");
        return 2;
    }
    const Envelopes heardEnvelopes = envelopes(at16k(*heard));
    const Envelopes phraseEnvelopes = envelopes(at16k(*phrase));
    const Envelopes otherEnvelopes = envelopes(at16k(*other));
    const Match said = match(heardEnvelopes, phraseEnvelopes);
    const Match notSaid = match(heardEnvelopes, otherEnvelopes);

    // Heard frame 0 began at `recorded`; the phrase's frame `offset` is what
    // it held, and the speaker said that frame `offset` hops after `spoken`,
    // or a whole number of phrases later.
    const double recorded = std::strtod(argv[4], nullptr);
    const double spoken = std::strtod(argv[5], nullptr);
    const double phraseNs = static_cast<double>(phraseEnvelopes.shape.size()) * HopSamples * 1e9 / Rate;
    double latency = recorded - spoken - static_cast<double>(said.offset) * HopSamples * 1e9 / Rate;
    if (phraseNs > 0.0)
        latency = std::fmod(std::fmod(latency, phraseNs) + phraseNs, phraseNs);
    const double voiced = heardEnvelopes.shape.empty()
                              ? 0.0
                              : static_cast<double>(said.voiced) / static_cast<double>(heardEnvelopes.shape.size());
    std::printf("[voice-match] similarity=%.3f other=%.3f margin=%.3f voiced=%.3f latency_ms=%.0f\n", said.similarity,
                notSaid.similarity, said.similarity - notSaid.similarity, voiced, latency / 1e6);
    return 0;
}
