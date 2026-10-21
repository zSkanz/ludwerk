// **Opus behind the voice codec interface** (ADR 0256 §2), in a translation
// unit of its own: nothing else in the engine names a libopus type.
#include <algorithm>

#include "engine/audio/voice.h"

#if defined(ENG_HAVE_OPUS)
#include <opus.h>
#endif

namespace engine::audio {

#if defined(ENG_HAVE_OPUS)
namespace {

// What a speaker sends: 24 kbit/s is clear speech at about 60 bytes a frame,
// well under the 512 a frame may be and the four kilobytes a second a speaker
// may send (ADR 0241).
constexpr opus_int32 Bitrate = 24000;
// The share of frames the encoder spends bits against losing. A tenth is what
// a phone on a busy network loses, and below the share where the rebuilt
// frames would cost speech more than the lost ones do.
constexpr opus_int32 ExpectedLossPercent = 10;
// Middle of the 0-10 range: the difference above it is not heard in a voice,
// and a phone's core is shared with the game.
constexpr opus_int32 Complexity = 5;
constexpr opus_int32 MostBytes = 512;

class OpusVoiceEncoder final : public VoiceEncoder
{
public:
    OpusVoiceEncoder()
    {
        int error = OPUS_OK;
        m_encoder = opus_encoder_create(static_cast<opus_int32>(kVoiceSampleRate), 1, OPUS_APPLICATION_VOIP, &error);
        if (error != OPUS_OK || m_encoder == nullptr) {
            m_encoder = nullptr;
            return;
        }
        opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(Bitrate));
        opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(m_encoder, OPUS_SET_PACKET_LOSS_PERC(ExpectedLossPercent));
        opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(Complexity));
    }
    ~OpusVoiceEncoder() override
    {
        if (m_encoder != nullptr)
            opus_encoder_destroy(m_encoder);
    }
    OpusVoiceEncoder(const OpusVoiceEncoder&) = delete;
    OpusVoiceEncoder& operator=(const OpusVoiceEncoder&) = delete;

    [[nodiscard]] VoiceCodecId id() const noexcept override { return VoiceCodecId::Opus; }
    [[nodiscard]] bool encode(std::span<const f32> frame, std::vector<u8>& out) override
    {
        if (m_encoder == nullptr || frame.size() != kVoiceFrameSamples)
            return false;
        out.resize(static_cast<usize>(MostBytes));
        const opus_int32 written =
            opus_encode_float(m_encoder, frame.data(), static_cast<int>(kVoiceFrameSamples), out.data(), MostBytes);
        if (written <= 0) {
            out.clear();
            return false;
        }
        out.resize(static_cast<usize>(written));
        return true;
    }

private:
    OpusEncoder* m_encoder = nullptr;
};

class OpusVoiceDecoder final : public VoiceDecoder
{
public:
    OpusVoiceDecoder()
    {
        int error = OPUS_OK;
        m_decoder = opus_decoder_create(static_cast<opus_int32>(kVoiceSampleRate), 1, &error);
        if (error != OPUS_OK)
            m_decoder = nullptr;
    }
    ~OpusVoiceDecoder() override
    {
        if (m_decoder != nullptr)
            opus_decoder_destroy(m_decoder);
    }
    OpusVoiceDecoder(const OpusVoiceDecoder&) = delete;
    OpusVoiceDecoder& operator=(const OpusVoiceDecoder&) = delete;

    [[nodiscard]] VoiceCodecId id() const noexcept override { return VoiceCodecId::Opus; }
    [[nodiscard]] bool decode(std::span<const u8> packet, std::span<f32> out) override
    {
        if (m_decoder == nullptr || packet.empty() || out.size() != kVoiceFrameSamples)
            return false;
        const int samples = opus_decode_float(m_decoder, packet.data(), static_cast<opus_int32>(packet.size()),
                                              out.data(), static_cast<int>(kVoiceFrameSamples), 0);
        return samples == static_cast<int>(kVoiceFrameSamples);
    }
    void conceal(std::span<const u8> next, std::span<f32> out) override
    {
        if (m_decoder == nullptr || out.size() != kVoiceFrameSamples) {
            std::fill(out.begin(), out.end(), 0.0f);
            return;
        }
        // From the outline the next frame carries of this one, when it has
        // come; otherwise the decoder's own guess, which fades to silence over
        // a run of them.
        const bool rebuilt =
            !next.empty() &&
            opus_decode_float(m_decoder, next.data(), static_cast<opus_int32>(next.size()), out.data(),
                              static_cast<int>(kVoiceFrameSamples), 1) == static_cast<int>(kVoiceFrameSamples);
        if (rebuilt)
            return;
        if (opus_decode_float(m_decoder, nullptr, 0, out.data(), static_cast<int>(kVoiceFrameSamples), 0) !=
            static_cast<int>(kVoiceFrameSamples))
            std::fill(out.begin(), out.end(), 0.0f);
    }

private:
    OpusDecoder* m_decoder = nullptr;
};

} // namespace

VoiceCodec opusVoiceCodec() noexcept
{
    return VoiceCodec{VoiceCodecId::Opus,
                      []() -> std::unique_ptr<VoiceEncoder> { return std::make_unique<OpusVoiceEncoder>(); },
                      []() -> std::unique_ptr<VoiceDecoder> { return std::make_unique<OpusVoiceDecoder>(); }};
}
#else
VoiceCodec opusVoiceCodec() noexcept
{
    return VoiceCodec{};
}
#endif

} // namespace engine::audio
