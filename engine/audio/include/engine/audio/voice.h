#pragma once

// **Players' voices** (ADR 0256): the parts of a voice that are not the wire
// and not the service -- what turns a microphone's samples into frames to
// send, which frames are speech, and how frames that arrive late, early or not
// at all are played evenly.
//
// The codec is an interface. Opus, which ADR 0122 approved, is the one a game
// ships with; a build without it has no codec, and voice says so and stays
// off. A test gives one of its own, and nothing above the interface knows
// which codec it is.

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "engine/core/types.h"

namespace engine::audio {

using core::f32;
using core::u16;
using core::u32;
using core::u8;
using core::usize;

// 48 kHz mono, 20 ms a frame: what every voice codec is built for, and what
// a frame on the wire carries.
inline constexpr u32 kVoiceSampleRate = 48000;
inline constexpr usize kVoiceFrameSamples = 960;

// Which codec made a frame: carried in each one, so a receiver can refuse what
// it cannot decode rather than play noise.
enum class VoiceCodecId : u8
{
    None = 0,
    // Raw 16-bit samples. Tests only: at 1920 bytes a frame it is far over
    // what a speaker may send (ADR 0241).
    Pcm16 = 1,
    Opus = 2,
};

class VoiceEncoder
{
public:
    virtual ~VoiceEncoder() = default;
    [[nodiscard]] virtual VoiceCodecId id() const noexcept = 0;
    // One frame of `kVoiceFrameSamples` samples into `out`, replacing it.
    // False when the codec could not.
    [[nodiscard]] virtual bool encode(std::span<const f32> frame, std::vector<u8>& out) = 0;
};

class VoiceDecoder
{
public:
    virtual ~VoiceDecoder() = default;
    [[nodiscard]] virtual VoiceCodecId id() const noexcept = 0;
    // One encoded frame into `kVoiceFrameSamples` samples. False when it is
    // not a frame this decoder reads.
    [[nodiscard]] virtual bool decode(std::span<const u8> packet, std::span<f32> out) = 0;
    // A frame that did not arrive: the codec's guess at it, or silence.
    // `next` is the frame after it when that one has already come, which a
    // codec that sends each frame's outline inside the next one rebuilds the
    // missing frame from; empty when it has not.
    virtual void conceal(std::span<const u8> next, std::span<f32> out) = 0;
};

// **The codec voice is sent with**, or none: what the build carries. A test
// installs its own (`setVoiceCodecForTests`) and puts the build's back after
// (`builtInVoiceCodec`).
struct VoiceCodec
{
    VoiceCodecId id = VoiceCodecId::None;
    std::unique_ptr<VoiceEncoder> (*makeEncoder)() = nullptr;
    std::unique_ptr<VoiceDecoder> (*makeDecoder)() = nullptr;
};
[[nodiscard]] const VoiceCodec& voiceCodec() noexcept;
[[nodiscard]] VoiceCodec builtInVoiceCodec() noexcept;
void setVoiceCodecForTests(const VoiceCodec& codec) noexcept;

// **Opus at 24 kbit/s** (ADR 0256 §2): speech, with each frame's outline sent
// again inside the next so a lone lost frame is rebuilt rather than guessed,
// and the encoder told to expect a tenth of them lost. A frame is about 60
// bytes. `None` in a build without libopus.
[[nodiscard]] VoiceCodec opusVoiceCodec() noexcept;

// --- Which frames are speech --------------------------------------------------

// **What is sent of the microphone** (ADR 0256 §3): with push to talk, the
// frames while the key is down; with voice activity, the frames whose level is
// over the threshold, and those around them -- the start of a word is quiet,
// and so are the pauses inside a sentence, and a gate that cut them chops
// speech into syllables.
class VoiceGate
{
public:
    struct Settings
    {
        bool pushToTalk = true;
        // The level a frame must reach to open the gate, in dBFS.
        f32 thresholdDb = -40.0f;
        // Frames sent after the level falls under it: 300 ms.
        u32 holdFrames = 15;
    };

    // Whether `frame` is sent: `held` is the push-to-talk key.
    [[nodiscard]] bool admit(std::span<const f32> frame, bool held, const Settings& settings) noexcept;
    // Whether the frames being admitted are a voice speaking: what
    // `SpeakingChanged` follows.
    [[nodiscard]] bool open() const noexcept { return m_open; }

    // A frame's level, in dBFS: its RMS against full scale, and -120 for
    // silence.
    [[nodiscard]] static f32 levelDb(std::span<const f32> frame) noexcept;

private:
    u32 m_holding = 0;
    bool m_open = false;
};

// --- Playing frames evenly ----------------------------------------------------

// **A speaker's frames, played at an even pace however they arrive** (ADR
// 0256 §2): held until `depth` frames are waiting, then one taken each 20 ms;
// a frame missing in its turn is concealed, one late after its turn is
// dropped, and the depth grows from two frames (40 ms) towards six (120 ms)
// each time the buffer runs dry, and shrinks again after a stretch with none.
class VoiceJitterBuffer
{
public:
    static constexpr u32 MinDepth = 2;
    static constexpr u32 MaxDepth = 6;
    static constexpr u32 FramesToShrink = 250;

    // A frame as it arrived: its sequence number and its bytes.
    void push(u16 sequence, std::span<const u8> packet);

    // What is played next, if anything is: a frame's bytes, or an empty
    // packet for one that is missing and is to be concealed. Nothing while
    // the buffer fills, or after the speaker has stopped.
    struct Turn
    {
        std::vector<u8> packet;
        bool missing = false;
        // For a missing frame, the one after it if it is already here: what
        // the decoder conceals it from (`VoiceDecoder::conceal`).
        std::vector<u8> next;
    };
    [[nodiscard]] std::optional<Turn> pop();

    [[nodiscard]] u32 depth() const noexcept { return m_depth; }
    [[nodiscard]] usize waiting() const noexcept { return m_frames.size(); }
    [[nodiscard]] u32 late() const noexcept { return m_late; }
    [[nodiscard]] u32 concealed() const noexcept { return m_concealed; }

private:
    struct Held
    {
        u16 sequence = 0;
        std::vector<u8> packet;
    };
    std::vector<Held> m_frames;
    u16 m_next = 0;
    bool m_playing = false;
    u32 m_depth = MinDepth;
    u32 m_smooth = 0;
    u32 m_late = 0;
    u32 m_concealed = 0;
};

} // namespace engine::audio
