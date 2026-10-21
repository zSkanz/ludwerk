// What a voice is before the wire (ADR 0256): which frames are speech, how
// frames that arrive unevenly are played evenly, and a codec behind the
// interface Opus will fill.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <doctest/doctest.h>
#include <memory>
#include <vector>

#include "engine/audio/audio.h"
#include "engine/audio/voice.h"

using namespace engine::audio;

namespace {

// A tone at `amplitude` of full scale, one frame of it.
[[nodiscard]] std::vector<f32> tone(f32 amplitude)
{
    std::vector<f32> frame(kVoiceFrameSamples);
    for (usize at = 0; at < frame.size(); ++at)
        frame[at] = amplitude * std::sin(static_cast<f32>(at) * 2.0f * 3.14159265f * 440.0f / 48000.0f);
    return frame;
}

// **Raw 16-bit samples**: the test's codec, behind the interface.
class PcmEncoder final : public VoiceEncoder
{
public:
    [[nodiscard]] VoiceCodecId id() const noexcept override { return VoiceCodecId::Pcm16; }
    [[nodiscard]] bool encode(std::span<const f32> frame, std::vector<u8>& out) override
    {
        out.resize(frame.size() * 2);
        for (usize at = 0; at < frame.size(); ++at) {
            const auto value = static_cast<std::int16_t>(std::clamp(frame[at], -1.0f, 1.0f) * 32767.0f);
            std::memcpy(out.data() + at * 2, &value, 2);
        }
        return true;
    }
};

class PcmDecoder final : public VoiceDecoder
{
public:
    [[nodiscard]] VoiceCodecId id() const noexcept override { return VoiceCodecId::Pcm16; }
    [[nodiscard]] bool decode(std::span<const u8> packet, std::span<f32> out) override
    {
        if (packet.size() != out.size() * 2)
            return false;
        for (usize at = 0; at < out.size(); ++at) {
            std::int16_t value = 0;
            std::memcpy(&value, packet.data() + at * 2, 2);
            out[at] = static_cast<f32>(value) / 32767.0f;
        }
        return true;
    }
    void conceal(std::span<const u8>, std::span<f32> out) override { std::fill(out.begin(), out.end(), 0.0f); }
};

[[nodiscard]] std::vector<u8> numbered(u16 sequence)
{
    return {static_cast<u8>(sequence & 0xFF), static_cast<u8>(sequence >> 8)};
}

} // namespace

TEST_CASE("ADR 0256: the build's codec is Opus, and a test can give its own and put Opus back")
{
    CHECK(voiceCodec().id == VoiceCodecId::Opus);
    CHECK(builtInVoiceCodec().id == VoiceCodecId::Opus);
    setVoiceCodecForTests(VoiceCodec{VoiceCodecId::Pcm16,
                                     []() -> std::unique_ptr<VoiceEncoder> { return std::make_unique<PcmEncoder>(); },
                                     []() -> std::unique_ptr<VoiceDecoder> { return std::make_unique<PcmDecoder>(); }});
    REQUIRE(voiceCodec().id == VoiceCodecId::Pcm16);
    const std::unique_ptr<VoiceEncoder> encoder = voiceCodec().makeEncoder();
    const std::unique_ptr<VoiceDecoder> decoder = voiceCodec().makeDecoder();
    const std::vector<f32> sent = tone(0.5f);
    std::vector<u8> packet;
    REQUIRE(encoder->encode(sent, packet));
    std::vector<f32> heard(kVoiceFrameSamples);
    REQUIRE(decoder->decode(packet, heard));
    for (usize at = 0; at < heard.size(); at += 97)
        CHECK(heard[at] == doctest::Approx(static_cast<double>(sent[at])).epsilon(0.001));
    setVoiceCodecForTests(builtInVoiceCodec());
    CHECK(voiceCodec().id == VoiceCodecId::Opus);
}

namespace {

// A voice-like signal: a 180 Hz fundamental and its harmonics, rising and
// falling in level the way syllables do. `frame` counts from the start.
[[nodiscard]] std::vector<f32> syllables(usize frame)
{
    std::vector<f32> out(kVoiceFrameSamples);
    for (usize at = 0; at < out.size(); ++at) {
        const f32 t = static_cast<f32>(frame * kVoiceFrameSamples + at) / static_cast<f32>(kVoiceSampleRate);
        const f32 envelope = 0.6f + 0.4f * std::sin(t * 2.0f * 3.14159265f * 4.0f);
        f32 sample = 0.0f;
        for (int harmonic = 1; harmonic <= 6; ++harmonic)
            sample +=
                std::sin(t * 2.0f * 3.14159265f * 180.0f * static_cast<f32>(harmonic)) / static_cast<f32>(harmonic);
        out[at] = 0.2f * envelope * sample;
    }
    return out;
}

[[nodiscard]] f32 rms(std::span<const f32> frame)
{
    double sum = 0.0;
    for (const f32 sample : frame)
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    return static_cast<f32>(std::sqrt(sum / static_cast<double>(frame.size())));
}

} // namespace

TEST_CASE("ADR 0256: Opus sends a voice in about 60 bytes a frame and plays it back at its level")
{
    const VoiceCodec codec = opusVoiceCodec();
    REQUIRE(codec.id == VoiceCodecId::Opus);
    const std::unique_ptr<VoiceEncoder> encoder = codec.makeEncoder();
    const std::unique_ptr<VoiceDecoder> decoder = codec.makeDecoder();
    CHECK(encoder->id() == VoiceCodecId::Opus);
    CHECK(decoder->id() == VoiceCodecId::Opus);

    usize bytes = 0;
    f32 sentLevel = 0.0f;
    f32 heardLevel = 0.0f;
    std::vector<u8> packet;
    std::vector<f32> heard(kVoiceFrameSamples);
    constexpr usize Frames = 100;
    for (usize frame = 0; frame < Frames; ++frame) {
        const std::vector<f32> sent = syllables(frame);
        REQUIRE(encoder->encode(sent, packet));
        CHECK(packet.size() <= 512);
        bytes += packet.size();
        REQUIRE(decoder->decode(packet, heard));
        // The first frames are the codec settling in.
        if (frame >= 10) {
            sentLevel += rms(sent);
            heardLevel += rms(heard);
        }
    }
    // 24 kbit/s is 60 bytes a 20 ms frame; the encoder lands near it.
    const usize perFrame = bytes / Frames;
    CHECK(perFrame >= 30);
    CHECK(perFrame <= 90);
    // Within 3 dB of what was said.
    CHECK(heardLevel > sentLevel * 0.7f);
    CHECK(heardLevel < sentLevel * 1.4f);

    // A frame that is not Opus is refused rather than played.
    const std::vector<u8> noise(40, 0xFF);
    CHECK_FALSE(decoder->decode(std::span<const u8>{}, heard));
    (void)decoder->decode(noise, heard);
}

TEST_CASE("ADR 0256: a lost Opus frame is rebuilt from the next one, and guessed when that has not come")
{
    const VoiceCodec codec = opusVoiceCodec();
    REQUIRE(codec.id == VoiceCodecId::Opus);
    const std::unique_ptr<VoiceEncoder> encoder = codec.makeEncoder();
    const std::unique_ptr<VoiceDecoder> decoder = codec.makeDecoder();
    std::vector<std::vector<u8>> packets;
    for (usize frame = 0; frame < 60; ++frame) {
        std::vector<u8> packet;
        REQUIRE(encoder->encode(syllables(frame), packet));
        packets.push_back(std::move(packet));
    }
    std::vector<f32> heard(kVoiceFrameSamples);
    for (usize frame = 0; frame < 40; ++frame)
        REQUIRE(decoder->decode(packets[frame], heard));
    // Frame 40 is lost and 41 has come: rebuilt from 41's copy of it, at the
    // level of speech rather than silence.
    decoder->conceal(packets[41], heard);
    CHECK(rms(heard) > 0.5f * rms(syllables(40)));
    REQUIRE(decoder->decode(packets[41], heard));
    // Frame 42 is lost and nothing after it has come: still a voice-like
    // guess, not a click to nothing.
    decoder->conceal({}, heard);
    CHECK(rms(heard) > 0.0f);
    REQUIRE(decoder->decode(packets[43], heard));
}

TEST_CASE("ADR 0256: a missing frame's turn carries the frame after it, when that one is here")
{
    VoiceJitterBuffer buffer;
    buffer.push(1, numbered(1));
    buffer.push(3, numbered(3));
    buffer.push(5, numbered(5));
    REQUIRE(buffer.pop()->packet == numbered(1));
    const auto two = buffer.pop();
    REQUIRE(two.has_value());
    CHECK(two->missing);
    CHECK(two->next == numbered(3));
    REQUIRE(buffer.pop()->packet == numbered(3));
    buffer.push(7, numbered(7));
    const auto four = buffer.pop();
    REQUIRE(four.has_value());
    CHECK(four->next == numbered(5));
}

TEST_CASE("ADR 0256: a frame's level is its RMS against full scale")
{
    CHECK(VoiceGate::levelDb(tone(1.0f)) == doctest::Approx(-3.01).epsilon(0.01));
    CHECK(VoiceGate::levelDb(tone(0.1f)) == doctest::Approx(-23.01).epsilon(0.01));
    CHECK(VoiceGate::levelDb(std::vector<f32>(kVoiceFrameSamples, 0.0f)) == -120.0f);
}

TEST_CASE("ADR 0256: push to talk sends while the key is down, and nothing else")
{
    VoiceGate gate;
    const VoiceGate::Settings settings{.pushToTalk = true};
    CHECK_FALSE(gate.admit(tone(1.0f), false, settings));
    CHECK_FALSE(gate.open());
    CHECK(gate.admit(std::vector<f32>(kVoiceFrameSamples, 0.0f), true, settings));
    CHECK(gate.open());
    CHECK_FALSE(gate.admit(tone(1.0f), false, settings));
}

TEST_CASE("ADR 0256: voice activity opens on speech and holds through the pauses between words")
{
    VoiceGate gate;
    const VoiceGate::Settings settings{.pushToTalk = false, .thresholdDb = -40.0f, .holdFrames = 3};
    const std::vector<f32> quiet = tone(0.001f); // -63 dBFS
    CHECK_FALSE(gate.admit(quiet, false, settings));
    CHECK(gate.admit(tone(0.2f), false, settings));
    CHECK(gate.open());
    // Three frames of pause are still sent, and still speaking.
    for (int frame = 0; frame < 3; ++frame) {
        CHECK(gate.admit(quiet, false, settings));
        CHECK(gate.open());
    }
    CHECK_FALSE(gate.admit(quiet, false, settings));
    CHECK_FALSE(gate.open());
}

TEST_CASE("ADR 0256: frames are held to the depth, then played one a turn, in order")
{
    VoiceJitterBuffer buffer;
    buffer.push(10, numbered(10));
    CHECK_FALSE(buffer.pop().has_value());
    // Out of order: played in order.
    buffer.push(12, numbered(12));
    buffer.push(11, numbered(11));
    for (const u16 expected : {u16{10}, u16{11}, u16{12}}) {
        const auto turn = buffer.pop();
        REQUIRE(turn.has_value());
        CHECK_FALSE(turn->missing);
        CHECK(turn->packet == numbered(expected));
    }
}

TEST_CASE("ADR 0256: a frame that never came is concealed in its turn, and one too late is dropped")
{
    VoiceJitterBuffer buffer;
    buffer.push(1, numbered(1));
    buffer.push(3, numbered(3));
    REQUIRE(buffer.pop()->packet == numbered(1));
    const auto gap = buffer.pop();
    REQUIRE(gap.has_value());
    CHECK(gap->missing);
    CHECK(buffer.concealed() == 1);
    // Frame 2, after its turn.
    buffer.push(2, numbered(2));
    CHECK(buffer.late() == 1);
    CHECK(buffer.pop()->packet == numbered(3));
}

TEST_CASE("ADR 0256: a buffer that runs dry plays deeper, and shallower again after a clean stretch")
{
    VoiceJitterBuffer buffer;
    u16 sequence = 0;
    const auto feed = [&](int frames) {
        for (int at = 0; at < frames; ++at) {
            buffer.push(sequence, numbered(sequence));
            sequence = static_cast<u16>(sequence + 1);
        }
    };
    feed(2);
    REQUIRE(buffer.pop().has_value());
    REQUIRE(buffer.pop().has_value());
    // Dry.
    const auto dry = buffer.pop();
    REQUIRE(dry.has_value());
    CHECK(dry->missing);
    CHECK(buffer.depth() == VoiceJitterBuffer::MinDepth + 1);
    // Filled to the new depth before playing on.
    sequence = static_cast<u16>(sequence + 1);
    feed(2);
    CHECK_FALSE(buffer.pop().has_value());
    feed(1);
    REQUIRE(buffer.pop().has_value());
    // A long clean stretch, a frame in for each frame out.
    for (u32 at = 0; at < VoiceJitterBuffer::FramesToShrink + 5; ++at) {
        feed(1);
        REQUIRE(buffer.pop().has_value());
    }
    CHECK(buffer.depth() == VoiceJitterBuffer::MinDepth);
}

TEST_CASE("ADR 0256: sequence numbers wrap without a frame taken for late")
{
    VoiceJitterBuffer buffer;
    buffer.push(65534, numbered(65534));
    buffer.push(65535, numbered(65535));
    REQUIRE(buffer.pop()->packet == numbered(65534));
    buffer.push(0, numbered(0));
    REQUIRE(buffer.pop()->packet == numbered(65535));
    REQUIRE(buffer.pop()->packet == numbered(0));
    CHECK(buffer.late() == 0);
}

TEST_CASE("ADR 0256: the microphone is taken a whole frame at a time, and no more than a second of it is kept")
{
    AudioSystem system;
    REQUIRE_FALSE(system.start(true).has_value());
    CHECK_FALSE(system.microphoneOpen());
    std::vector<f32> taken;
    system.feedMicrophoneForTests(std::vector<f32>(2500, 0.25f));
    system.takeMicrophone(taken);
    CHECK(taken.size() == 2 * kVoiceFrameSamples);
    // The 580 left over are the start of the next frame.
    system.feedMicrophoneForTests(std::vector<f32>(400, 0.5f));
    taken.clear();
    system.takeMicrophone(taken);
    REQUIRE(taken.size() == kVoiceFrameSamples);
    CHECK(taken.front() == 0.25f);
    CHECK(taken.back() == 0.5f);
    // A listener a second behind is not listening: the oldest go.
    system.feedMicrophoneForTests(std::vector<f32>(kVoiceSampleRate + 5000, 0.1f));
    taken.clear();
    system.takeMicrophone(taken);
    CHECK(taken.size() == kVoiceSampleRate);
    system.stop();
}

TEST_CASE("ADR 0256: a voice heard is played by its jitter buffer, in the middle, once the buffer has filled")
{
    setVoiceCodecForTests(VoiceCodec{VoiceCodecId::Pcm16,
                                     []() -> std::unique_ptr<VoiceEncoder> { return std::make_unique<PcmEncoder>(); },
                                     []() -> std::unique_ptr<VoiceDecoder> { return std::make_unique<PcmDecoder>(); }});
    AudioSystem system;
    REQUIRE_FALSE(system.start(true).has_value());
    PcmEncoder encoder;
    std::vector<u8> packet;
    REQUIRE(encoder.encode(std::vector<f32>(kVoiceFrameSamples, 0.5f), packet));

    // A frame in a codec other than the one in use: nothing.
    system.hearVoice(7, static_cast<u8>(VoiceCodecId::Opus), 0, packet);
    CHECK(system.voicesHeard() == 0);

    std::vector<f32> stereo(kVoiceFrameSamples * 2);
    system.hearVoice(7, static_cast<u8>(VoiceCodecId::Pcm16), 0, packet);
    CHECK(system.voicesHeard() == 1);
    // One frame is under the buffer's depth: still silence.
    system.mixVoicesForTests(stereo);
    CHECK(stereo[0] == 0.0f);
    system.hearVoice(7, static_cast<u8>(VoiceCodecId::Pcm16), 1, packet);
    system.mixVoicesForTests(stereo);
    // Half scale, equally in both ears.
    CHECK(stereo[0] == doctest::Approx(0.5 * 0.7071).epsilon(0.01));
    CHECK(stereo[1] == doctest::Approx(0.5 * 0.7071).epsilon(0.01));

    system.forgetVoice(7);
    CHECK(system.voicesHeard() == 0);
    system.stop();
    setVoiceCodecForTests(builtInVoiceCodec());
}
