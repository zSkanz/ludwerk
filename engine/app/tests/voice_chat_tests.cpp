// Voice in a build with no codec (ADR 0256): unavailable, said once, and the
// microphone never opened -- a game that turns voice on before its codec is in
// the build sends nothing, and certainly not raw samples.
#include <algorithm>
#include <doctest/doctest.h>
#include <string>

#include "engine/app/network_session.h"
#include "engine/app/voice_chat.h"
#include "engine/app/world_host.h"
#include "engine/audio/audio.h"
#include "engine/audio/voice.h"
#include "project_fixture.h"

using namespace engine;
using engine::app::testing::bootOptions;
using engine::app::testing::Captured;
using engine::app::testing::Project;

TEST_CASE("ADR 0256: with no codec in the build, voice is unavailable, says so once, and opens no microphone")
{
    REQUIRE(audio::voiceCodec().id == audio::VoiceCodecId::None);
    Captured log;
    Project project;
    project.write("src/client/init.luau", R"(
        local VoiceChatService = game:GetService("VoiceChatService")
        print(`voice-available:{VoiceChatService.Available}`)
        VoiceChatService.Enabled = true
        VoiceChatService.PushToTalk = false
    )");
    app::WorldHost host;
    REQUIRE_FALSE(host.boot(bootOptions(project.root)).has_value());
    host.tick();
    CHECK_MESSAGE(log.firstError().empty(), log.firstError());
    CHECK(log.contains("voice-available:false"));
    REQUIRE(host.world().engineState().voice.enabled);

    app::NetworkSession network([&host]() { return &host; }, replication::Config{});
    app::VoiceChat voice;
    for (int frame = 0; frame < 30; ++frame) {
        voice.frame(host.world(), host.audio(), network, static_cast<core::u64>(frame) * 16'000'000ull);
        CHECK_FALSE(host.audio().microphoneOpen());
    }
    const auto said = std::count_if(log.lines.begin(), log.lines.end(), [](const std::string& line) {
        return line.find("this build carries no voice codec") != std::string::npos;
    });
    CHECK(said == 1);
    CHECK(host.world().engineState().voice.speaking.empty());
}
