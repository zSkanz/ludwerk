#include "engine/audio/voice.h"

#include <algorithm>
#include <cmath>

namespace engine::audio {
namespace {

VoiceCodec& installed() noexcept
{
    static VoiceCodec codec = builtInVoiceCodec();
    return codec;
}

// Sequence numbers wrap at 65536: `a` is before `b` when it is less than half
// the range behind it.
[[nodiscard]] bool before(u16 a, u16 b) noexcept
{
    return static_cast<core::i16>(static_cast<u16>(a - b)) < 0;
}

// More than this waiting is a speaker sending faster than it is played, or a
// burst after a stall: the oldest go.
constexpr usize MostWaiting = VoiceJitterBuffer::MaxDepth * 4;

} // namespace

const VoiceCodec& voiceCodec() noexcept
{
    return installed();
}

VoiceCodec builtInVoiceCodec() noexcept
{
    return opusVoiceCodec();
}

void setVoiceCodecForTests(const VoiceCodec& codec) noexcept
{
    VoiceCodec& held = installed();
    held.id = codec.id;
    held.makeEncoder = codec.makeEncoder;
    held.makeDecoder = codec.makeDecoder;
}

// --- VoiceGate ---------------------------------------------------------------

f32 VoiceGate::levelDb(std::span<const f32> frame) noexcept
{
    if (frame.empty())
        return -120.0f;
    double sum = 0.0;
    for (const f32 sample : frame)
        sum += static_cast<double>(sample) * static_cast<double>(sample);
    const double rms = std::sqrt(sum / static_cast<double>(frame.size()));
    if (rms < 1.0e-6)
        return -120.0f;
    return static_cast<f32>(20.0 * std::log10(rms));
}

bool VoiceGate::admit(std::span<const f32> frame, bool held, const Settings& settings) noexcept
{
    if (settings.pushToTalk) {
        m_holding = 0;
        m_open = held;
        return held;
    }
    if (levelDb(frame) >= settings.thresholdDb) {
        m_holding = settings.holdFrames;
        m_open = true;
        return true;
    }
    if (m_holding > 0) {
        m_holding -= 1;
        return true;
    }
    m_open = false;
    return false;
}

// --- VoiceJitterBuffer -------------------------------------------------------

void VoiceJitterBuffer::push(u16 sequence, std::span<const u8> packet)
{
    // Too late for its turn: played around already, as a concealed frame.
    if (m_playing && before(sequence, m_next)) {
        m_late += 1;
        return;
    }
    const auto at = std::find_if(m_frames.begin(), m_frames.end(),
                                 [&](const Held& held) { return !before(held.sequence, sequence); });
    if (at != m_frames.end() && at->sequence == sequence)
        return;
    m_frames.insert(at, Held{sequence, std::vector<u8>(packet.begin(), packet.end())});
    while (m_frames.size() > MostWaiting)
        m_frames.erase(m_frames.begin());
}

std::optional<VoiceJitterBuffer::Turn> VoiceJitterBuffer::pop()
{
    if (!m_playing) {
        if (m_frames.size() < m_depth)
            return std::nullopt;
        m_playing = true;
        m_next = m_frames.front().sequence;
    }
    if (m_frames.empty()) {
        // **Run dry**: the link is less even than the depth allowed for. One
        // frame deeper from here, filled again before playing on; this turn
        // is concealed, so what was playing does not stop on a click.
        m_depth = std::min(m_depth + 1, MaxDepth);
        m_smooth = 0;
        m_playing = false;
        m_concealed += 1;
        m_next = static_cast<u16>(m_next + 1);
        return Turn{{}, true, {}};
    }
    if (m_frames.front().sequence != m_next) {
        // Its frame is missing, and a later one has come: concealed in its
        // turn, from the frame right after it when that is the one.
        m_concealed += 1;
        m_smooth = 0;
        m_next = static_cast<u16>(m_next + 1);
        Turn turn{{}, true, {}};
        if (m_frames.front().sequence == m_next)
            turn.next = m_frames.front().packet;
        return turn;
    }
    Turn turn{std::move(m_frames.front().packet), false, {}};
    m_frames.erase(m_frames.begin());
    m_next = static_cast<u16>(m_next + 1);
    // **And shallower again** after a long stretch with nothing missing: a
    // moment of a bad link does not cost every word after it 40 ms.
    if (++m_smooth >= FramesToShrink && m_depth > MinDepth) {
        m_depth -= 1;
        m_smooth = 0;
    }
    return turn;
}

} // namespace engine::audio
