#include "engine/app/voice_chat.h"

#include <algorithm>
#include <span>

#include "engine/app/network_session.h"
#include "engine/audio/audio.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/platform/platform.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"

namespace engine::app {

void VoiceChat::frame(scene::World& world, audio::AudioSystem& audio, NetworkSession& network, core::u64 nowNs)
{
    scene::EngineState::Voice& voice = world.engineState().voice;
    const audio::VoiceCodec& codec = audio::voiceCodec();
    const bool inMatch = network.active();

    // **Who hears whom by the game's word** (`Script` mode): the pairs
    // `SetCanHear` set, read where the session asks, from each match's
    // start.
    if (inMatch && !m_filterFor) {
        m_filterFor = true;
        // And the one codec it relays: this build's, or none.
        network.setVoiceCodec(static_cast<core::u8>(codec.id));
        scene::World* held = &world;
        network.setVoiceFilter([held](core::InstanceId listener, core::InstanceId speaker) {
            const scene::PlayerComponent* one = held->players().find(listener);
            const scene::PlayerComponent* two = held->players().find(speaker);
            if (one == nullptr || two == nullptr)
                return false;
            const std::vector<std::pair<core::u32, core::u32>>& pairs = held->engineState().voice.canHear;
            return std::binary_search(pairs.begin(), pairs.end(), std::pair{one->userId, two->userId});
        });
    }
    if (!inMatch)
        m_filterFor = false;

    if (voice.enabled && codec.id == audio::VoiceCodecId::None && !m_warnedNoCodec) {
        m_warnedNoCodec = true;
        core::log(core::LogLevel::Warn, ENG_TR("audio.warn.voice_no_codec"));
    }
    // Only a machine with a player of its own speaks: a dedicated server
    // relays and never opens a microphone.
    const bool sending = voice.enabled && voice.playerEnabled && inMatch && codec.id != audio::VoiceCodecId::None &&
                         scene::localPlayerOf(world).valid();
    // **The player is asked first**, where the system asks: a phone's
    // prompt, answered on another thread. Refused, voice plays and never
    // sends.
    if (sending && platform::microphoneAccess() == platform::MicrophoneAccess::NotAsked)
        platform::askForMicrophone();
    const bool allowed = platform::microphoneAccess() == platform::MicrophoneAccess::Allowed;
    if (sending && allowed && !audio.microphoneOpen())
        (void)audio.openMicrophone();
    else if ((!sending || !allowed) && audio.microphoneOpen())
        audio.closeMicrophone();

    // **What this machine says**: whole frames through the gate, encoded.
    const core::InstanceId self = scene::localPlayerOf(world);
    const scene::PlayerComponent* own = self.valid() ? world.players().find(self) : nullptr;
    bool ownSpeaking = false;
    if (sending && audio.microphoneOpen()) {
        if (m_encoder == nullptr && codec.makeEncoder != nullptr)
            m_encoder = codec.makeEncoder();
        m_captured.clear();
        audio.takeMicrophone(m_captured);
        const audio::VoiceGate::Settings settings{
            .pushToTalk = voice.pushToTalk, .thresholdDb = voice.activityThreshold, .holdFrames = 15};
        for (core::usize at = 0; at + audio::kVoiceFrameSamples <= m_captured.size(); at += audio::kVoiceFrameSamples) {
            const std::span<const core::f32> chunk(m_captured.data() + at, audio::kVoiceFrameSamples);
            if (!m_gate.admit(chunk, voice.talking, settings) || m_encoder == nullptr ||
                !m_encoder->encode(chunk, m_packet))
                continue;
            network.speak(static_cast<core::u8>(codec.id), m_sequence++, m_gate.open(), m_packet);
        }
        ownSpeaking = m_gate.open();
    }
    else {
        m_encoder.reset();
    }

    // **What this machine hears**, each speaker in their turn, unless the
    // player muted them here or turned voice off for themself.
    m_heard.clear();
    network.takeVoice(m_heard);
    for (const replication::HeardVoice& heard : m_heard) {
        if (!voice.playerEnabled ||
            std::binary_search(voice.locallyMuted.begin(), voice.locallyMuted.end(), heard.speaker))
            continue;
        audio.hearVoice(heard.speaker, heard.codec, heard.sequence, heard.frame);
        auto lately = std::find_if(m_lately.begin(), m_lately.end(),
                                   [&](const Lately& held) { return held.speaker == heard.speaker; });
        if (lately == m_lately.end()) {
            m_lately.push_back(Lately{heard.speaker});
            lately = std::prev(m_lately.end());
        }
        lately->lastNs = nowNs;
        lately->speaking = heard.speaking;
        lately->positional = heard.positional;
    }

    // Where each is heard, as their character moves; and who has gone quiet.
    std::vector<core::u32> speaking;
    for (auto at = m_lately.begin(); at != m_lately.end();) {
        if (nowNs - at->lastNs > ForgetNs) {
            audio.forgetVoice(at->speaker);
            at = m_lately.erase(at);
            continue;
        }
        const core::InstanceId player = scene::playerByUserId(world, at->speaker);
        const scene::PlayerComponent* who = player.valid() ? world.players().find(player) : nullptr;
        const scene::PartComponent* body = who != nullptr ? world.parts().find(who->character) : nullptr;
        if (at->positional && body != nullptr)
            audio.placeVoice(at->speaker, &body->cframe.position, voice.proximityDistance);
        else
            audio.placeVoice(at->speaker, nullptr, voice.proximityDistance);
        if (at->speaking && nowNs - at->lastNs <= QuietNs)
            speaking.push_back(at->speaker);
        ++at;
    }
    if (ownSpeaking && own != nullptr)
        speaking.push_back(own->userId);
    std::sort(speaking.begin(), speaking.end());
    speaking.erase(std::unique(speaking.begin(), speaking.end()), speaking.end());

    // What `SpeakingChanged` says: who began, and who stopped.
    for (const core::u32 user : speaking) {
        if (!std::binary_search(voice.speaking.begin(), voice.speaking.end(), user))
            voice.speakingChanges.emplace_back(user, true);
    }
    for (const core::u32 user : voice.speaking) {
        if (!std::binary_search(speaking.begin(), speaking.end(), user))
            voice.speakingChanges.emplace_back(user, false);
    }
    voice.speaking = std::move(speaking);
}

} // namespace engine::app
