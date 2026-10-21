#pragma once

// **Players talk to each other** (ADR 0256): each frame, this machine's
// microphone to the match, and the match's voices to its speakers.
//
// The microphone is open only while the game's `VoiceChatService.Enabled`
// and the player's `PlayerEnabled` both allow it, a match is on and the build
// carries a codec. Its frames pass the gate -- push to talk or voice activity
// -- are encoded and spoken; what the match sends is played by each speaker's
// jitter buffer, placed where their character is in `Proximity` mode, and
// who is speaking is kept for `SpeakingChanged`.

#include <memory>
#include <vector>

#include "engine/audio/voice.h"
#include "engine/core/types.h"
#include "engine/replication/types.h"

namespace engine::audio {
class AudioSystem;
}
namespace engine::scene {
class World;
}

namespace engine::app {

class NetworkSession;

class VoiceChat
{
public:
    // A speaker this long without a frame has stopped; this long, and they are
    // forgotten.
    static constexpr core::u64 QuietNs = 300'000'000ull;
    static constexpr core::u64 ForgetNs = 10'000'000'000ull;

    // One frame: `nowNs` is this machine's clock, which decides who has
    // stopped speaking.
    void frame(scene::World& world, audio::AudioSystem& audio, NetworkSession& network, core::u64 nowNs);

private:
    std::unique_ptr<audio::VoiceEncoder> m_encoder;
    audio::VoiceGate m_gate;
    core::u16 m_sequence = 0;
    bool m_warnedNoCodec = false;
    bool m_filterFor = false;
    std::vector<core::f32> m_captured;
    std::vector<core::u8> m_packet;
    std::vector<replication::HeardVoice> m_heard;
    struct Lately
    {
        core::u32 speaker = 0;
        core::u64 lastNs = 0;
        bool speaking = false;
        bool positional = false;
    };
    std::vector<Lately> m_lately;
};

} // namespace engine::app
