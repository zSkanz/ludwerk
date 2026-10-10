// A mouth that moves with what is said (ADR 0200, `LipSync`): by a line's
// viseme track, by the sound's frequency bands, by its loudness -- each the
// fallback of the one before -- and what it costs a frame.
//
// The audio system here has no device, as in every test: what a mouth reads
// is the recording at the place the TICK's timeline has got to, which is the
// same on a machine with speakers and on one with none.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/app/mouths.h"
#include "engine/asset/content.h"
#include "engine/asset/localized.h"
#include "engine/audio/audio.h"
#include "engine/audio/scene_types.h"
#include "engine/render/animation.h"
#include "engine/render/scene_types.h"
#include "engine/scene/world.h"

using namespace engine;

namespace {

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
}

// A tone at full scale: its level reads one, and it lights one band.
void writeTone(const std::filesystem::path& path, double seconds, double hertz)
{
    constexpr core::u32 Rate = 48000;
    std::vector<char> bytes;
    const auto put = [&bytes](const void* data, std::size_t size) {
        const auto* const at = static_cast<const char*>(data);
        bytes.insert(bytes.end(), at, at + size);
    };
    const auto putU32 = [&put](core::u32 value) { put(&value, sizeof(value)); };
    const auto putU16 = [&put](core::u16 value) { put(&value, sizeof(value)); };
    const auto frames = static_cast<core::u32>(std::llround(seconds * Rate));
    put("RIFF", 4);
    putU32(36u + frames * 2u);
    put("WAVE", 4);
    put("fmt ", 4);
    putU32(16u);
    putU16(1u);
    putU16(1u);
    putU32(Rate);
    putU32(Rate * 2u);
    putU16(2u);
    putU16(16u);
    put("data", 4);
    putU32(frames * 2u);
    for (core::u32 frame = 0; frame < frames; ++frame) {
        const double phase = 2.0 * 3.14159265358979 * hertz * static_cast<double>(frame) / Rate;
        putU16(static_cast<core::u16>(static_cast<core::i16>(32767.0 * std::sin(phase))));
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

constexpr const char* FaceModel = "asset://models/face.glb";
constexpr double Frame = 1.0 / 60.0;

struct Stage
{
    std::filesystem::path root;
    asset::ContentMounts mounts;
    asset::LocalizationIndex index;
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    std::optional<scene::World> world;
    render::SkeletonLibrary skeletons;
    std::optional<render::AnimationSystem> animation;
    audio::AudioSystem audio;
    app::Mouths mouths;

    explicit Stage(const char* folder)
    {
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec) / folder;
        std::filesystem::remove_all(root, ec);

        // A line of a second at 200 Hz, with a track; the same line "in
        // Portuguese" with a track of its own; and two lines with none, one
        // low and one high.
        writeTone(root / "voice" / "line.wav", 1.0, 200.0);
        writeText(
            root / "voice" / "line.visemes.json",
            R"({ "cues": [ { "start": 0.0, "end": 0.4, "value": "D" }, { "start": 0.5, "end": 1.0, "value": "A" } ] })");
        writeTone(root / "l10n" / "pt-BR" / "voice" / "line.wav", 1.0, 200.0);
        writeText(root / "l10n" / "pt-BR" / "voice" / "line.visemes.json",
                  R"({ "mouthCues": [ { "start": 0.0, "end": 1.0, "value": "O" } ] })");
        writeTone(root / "voice" / "low.wav", 1.0, 200.0);
        writeTone(root / "voice" / "high.wav", 1.0, 5000.0);
        writeText(root / "models" / "face.face.json",
                  R"({ "visemes": { "D": { "JawOpen": 0.8, "MouthWide": 0.3 }, "A": { "MouthClosed": 1 },
                                    "O": { "MouthRound": 1 } },
                       "bands": { "open": "JawOpen", "wide": "MouthWide", "round": "MouthRound" },
                       "loudness": "JawOpen" })");
        mounts.mountDirectory(root);
        asset::scanLocalizedContent(root, index);

        scene::generated::registerClasses(classes, atoms);
        audio::registerSceneTypes(classes, atoms);
        render::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        world.emplace(classes, enums, atoms, 1u);

        render::SkeletonLibrary::Entry face;
        face.morphNames = {"JawOpen", "MouthWide", "MouthRound", "MouthClosed"};
        face.morphDefaults = {0.0f, 0.0f, 0.0f, 0.0f};
        skeletons.set(atoms.intern(FaceModel), std::move(face));
        animation.emplace(*world, skeletons);

        REQUIRE_FALSE(audio.start(true).has_value());
        audio.setContentMounts(&mounts);
        audio.setLocalized(&index);
    }

    ~Stage()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    core::InstanceId make(const char* className, core::InstanceId parent = {})
    {
        const scene::ClassId id = classes.findId(atoms.intern(className));
        REQUIRE(id != scene::InvalidClass);
        const core::InstanceId made = world->create(id);
        if (parent.valid())
            REQUIRE_FALSE(world->setParent(made, parent).has_value());
        return made;
    }

    // A speaker: something to say things under, a face on it, and the
    // `LipSync` that moves the face.
    struct Speaker
    {
        core::InstanceId character;
        core::InstanceId face;
        core::InstanceId lips;
    };
    Speaker speaker()
    {
        Speaker made;
        made.character = make("Folder");
        made.face = make("MeshPart", made.character);
        world->meshParts().find(made.face)->meshContent = atoms.intern(FaceModel);
        made.lips = make("LipSync", made.face);
        // Snapped, unless a case is about the easing.
        world->lipSyncs().find(made.lips)->smoothing = 0.0f;
        return made;
    }

    core::InstanceId say(core::InstanceId under, const char* content, double at)
    {
        const core::InstanceId sound = make("Sound", under);
        scene::SoundComponent* voice = world->sounds().find(sound);
        voice->content = content;
        voice->category = 2;
        voice->playing = true;
        voice->timePosition = at;
        return sound;
    }

    void frame(float seconds = static_cast<float>(Frame))
    {
        audio.update(*world, core::InstanceId{});
        mouths.step(*world, audio, *animation, &mounts, seconds);
    }

    [[nodiscard]] float key(core::InstanceId face, std::size_t target)
    {
        const std::span<const float> weights = animation->drawnMorphWeights(face);
        return target < weights.size() ? weights[target] : 0.0f;
    }
};

constexpr std::size_t JawOpen = 0;
constexpr std::size_t MouthWide = 1;
constexpr std::size_t MouthRound = 2;
constexpr std::size_t MouthClosed = 3;

} // namespace

TEST_CASE("ADR 0200: a line with a viseme track moves the mouth by it, cue by cue")
{
    Stage stage("engine-mouths-track");
    const Stage::Speaker hero = stage.speaker();
    const core::InstanceId line = stage.say(hero.character, "asset://voice/line.wav", 0.1);

    stage.frame();
    // The first cue is "D": the jaw eight tenths open, the mouth a little wide.
    CHECK(static_cast<double>(stage.key(hero.face, JawOpen)) == doctest::Approx(0.8));
    CHECK(static_cast<double>(stage.key(hero.face, MouthWide)) == doctest::Approx(0.3));
    CHECK(stage.key(hero.face, MouthClosed) == 0.0f);
    CHECK(stage.mouths.stats().byTrack == 1);
    CHECK(stage.mouths.stats().moved == 1);

    // Between two cues nothing is being said: the face is at rest.
    stage.world->sounds().find(line)->timePosition = 0.45;
    stage.frame();
    CHECK(stage.key(hero.face, JawOpen) == 0.0f);

    // The second is "A": closed.
    stage.world->sounds().find(line)->timePosition = 0.7;
    stage.frame();
    CHECK(static_cast<double>(stage.key(hero.face, MouthClosed)) == doctest::Approx(1.0));
    CHECK(stage.key(hero.face, JawOpen) == 0.0f);

    // The line stops, and the mouth with it.
    stage.world->sounds().find(line)->playing = false;
    stage.frame();
    CHECK(stage.key(hero.face, MouthClosed) == 0.0f);
    CHECK(stage.mouths.stats().moved == 0);
}

TEST_CASE("ADR 0200: a mouth plays the track of the language its machine hears")
{
    Stage stage("engine-mouths-language");
    stage.audio.setVoiceLocale("pt-BR");
    const Stage::Speaker hero = stage.speaker();
    (void)stage.say(hero.character, "asset://voice/line.wav", 0.1);

    stage.frame();
    // Portuguese has a track of its own beside its own recording, and at
    // this moment it says "O" where the default language's says "D".
    CHECK(static_cast<double>(stage.key(hero.face, MouthRound)) == doctest::Approx(1.0));
    CHECK(stage.key(hero.face, JawOpen) == 0.0f);
    CHECK(stage.mouths.stats().byTrack == 1);
}

TEST_CASE("ADR 0200: a line with no track is read as it sounds, and a face with one jaw key opens by loudness")
{
    Stage stage("engine-mouths-bands");
    const Stage::Speaker low = stage.speaker();
    const Stage::Speaker high = stage.speaker();
    const Stage::Speaker plain = stage.speaker();
    // 2 is `Enum.LipSyncMode.Loudness`.
    stage.world->lipSyncs().find(plain.lips)->mode = 2;
    (void)stage.say(low.character, "asset://voice/low.wav", 0.5);
    (void)stage.say(high.character, "asset://voice/high.wav", 0.5);
    (void)stage.say(plain.character, "asset://voice/high.wav", 0.5);

    stage.frame();
    // A low tone: an open mouth, and a round one -- energy low with little
    // above it.
    CHECK(stage.key(low.face, JawOpen) > 0.9f);
    CHECK(stage.key(low.face, MouthRound) > 0.9f);
    CHECK(stage.key(low.face, MouthWide) < 0.1f);
    // A high one: wide, and not open.
    CHECK(stage.key(high.face, MouthWide) > 0.9f);
    CHECK(stage.key(high.face, JawOpen) < 0.1f);
    CHECK(stage.key(high.face, MouthRound) < 0.1f);
    // Loudness alone knows neither: the jaw opens, whatever the pitch.
    CHECK(stage.key(plain.face, JawOpen) > 0.9f);
    CHECK(stage.key(plain.face, MouthWide) == 0.0f);
    CHECK(stage.mouths.stats().byBands == 2);
    CHECK(stage.mouths.stats().byLoudness == 1);

    // 1 is `Bands`: a line that has a track is read as it sounds all the same.
    const Stage::Speaker asked = stage.speaker();
    stage.world->lipSyncs().find(asked.lips)->mode = 1;
    (void)stage.say(asked.character, "asset://voice/line.wav", 0.1);
    stage.frame();
    CHECK(stage.key(asked.face, MouthRound) > 0.9f);
    CHECK(stage.mouths.stats().byTrack == 0);
}

TEST_CASE("ADR 0200: a mouth is moved by the voice said under its own speaker, and by no other sound")
{
    Stage stage("engine-mouths-source");
    const Stage::Speaker hero = stage.speaker();
    const Stage::Speaker other = stage.speaker();

    // Somebody else's line.
    (void)stage.say(other.character, "asset://voice/line.wav", 0.1);
    // And a sound of the hero's own that is not a voice.
    const core::InstanceId step = stage.say(hero.character, "asset://voice/low.wav", 0.5);
    stage.world->sounds().find(step)->category = 0;
    stage.frame();
    CHECK(stage.key(hero.face, JawOpen) == 0.0f);
    CHECK(stage.key(other.face, JawOpen) > 0.7f);

    // Told whose voice it is, it follows that one.
    stage.world->lipSyncs().find(hero.lips)->source = other.character;
    stage.frame();
    CHECK(stage.key(hero.face, JawOpen) > 0.7f);

    // And switched off it follows nobody.
    stage.world->lipSyncs().find(hero.lips)->enabled = false;
    stage.frame();
    CHECK(stage.key(hero.face, JawOpen) == 0.0f);
}

TEST_CASE("ADR 0200: a mouth's layer is under what a script sets, and no script's read sees it")
{
    Stage stage("engine-mouths-layer");
    const Stage::Speaker hero = stage.speaker();
    (void)stage.say(hero.character, "asset://voice/line.wav", 0.1);
    stage.frame();
    REQUIRE(static_cast<double>(stage.key(hero.face, JawOpen)) == doctest::Approx(0.8));
    // What a script reads back is what a script or a clip put there.
    CHECK(stage.animation->morphWeight(hero.face, "JawOpen") == 0.0f);

    // A script holds the jaw: its value is the one drawn, and the mouth goes
    // on moving the keys it did not take.
    stage.animation->setMorphWeight(hero.face, "JawOpen", 0.2f);
    stage.frame();
    CHECK(static_cast<double>(stage.key(hero.face, JawOpen)) == doctest::Approx(0.2));
    CHECK(static_cast<double>(stage.key(hero.face, MouthWide)) == doctest::Approx(0.3));

    stage.animation->clearMorphWeight(hero.face, "JawOpen");
    stage.frame();
    CHECK(static_cast<double>(stage.key(hero.face, JawOpen)) == doctest::Approx(0.8));
}

TEST_CASE("ADR 0200: a key is eased to where it is going, and a mouth closes when its line ends")
{
    Stage stage("engine-mouths-ease");
    const Stage::Speaker hero = stage.speaker();
    stage.world->lipSyncs().find(hero.lips)->smoothing = 0.06f;
    stage.world->lipSyncs().find(hero.lips)->weight = 0.5f;
    const core::InstanceId line = stage.say(hero.character, "asset://voice/line.wav", 0.1);

    // Half of it, as `Weight` says: four tenths. A sixtieth of a second
    // towards it is a quarter of the way.
    stage.frame();
    const float first = stage.key(hero.face, JawOpen);
    CHECK(first > 0.05f);
    CHECK(first < 0.2f);
    for (int frames = 0; frames < 30; ++frames)
        stage.frame();
    CHECK(static_cast<double>(stage.key(hero.face, JawOpen)) == doctest::Approx(0.4).epsilon(0.01));

    // The line ends: closing, not gone.
    stage.world->sounds().find(line)->playing = false;
    stage.frame();
    const float closing = stage.key(hero.face, JawOpen);
    CHECK(closing > 0.1f);
    CHECK(closing < 0.4f);
    for (int frames = 0; frames < 60; ++frames)
        stage.frame();
    CHECK(stage.key(hero.face, JawOpen) == 0.0f);
    CHECK(stage.mouths.stats().moved == 0);
}

TEST_CASE("ADR 0200: what a mouth costs a frame in each of the three ways, for one speaker and for twenty")
{
    // The mean of the mouths' own step over three hundred frames, in
    // microseconds, with the line moving on as a line does.
    const auto cost = [](int speakers, int mode) {
        Stage stage("engine-mouths-cost");
        std::vector<core::InstanceId> lines;
        for (int index = 0; index < speakers; ++index) {
            const Stage::Speaker one = stage.speaker();
            stage.world->lipSyncs().find(one.lips)->mode = mode;
            stage.world->lipSyncs().find(one.lips)->smoothing = 0.06f;
            // A track for `Auto`, and no track for the other two anyway.
            lines.push_back(
                stage.say(one.character, mode == 0 ? "asset://voice/line.wav" : "asset://voice/low.wav", 0.01 * index));
        }
        double total = 0.0;
        int measured = 0;
        for (int frame = 0; frame < 360; ++frame) {
            for (const core::InstanceId line : lines) {
                scene::SoundComponent* voice = stage.world->sounds().find(line);
                voice->timePosition = std::fmod(voice->timePosition + Frame, 0.95);
            }
            stage.audio.update(*stage.world, core::InstanceId{});
            const auto from = std::chrono::steady_clock::now();
            stage.mouths.step(*stage.world, stage.audio, *stage.animation, &stage.mounts, static_cast<float>(Frame));
            const double micros =
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - from).count();
            if (frame >= 60) {
                total += micros;
                ++measured;
            }
        }
        return total / measured;
    };
    const double track1 = cost(1, 0);
    const double bands1 = cost(1, 1);
    const double loud1 = cost(1, 2);
    const double track20 = cost(20, 0);
    const double bands20 = cost(20, 1);
    const double loud20 = cost(20, 2);
    MESSAGE("a mouth, microseconds a frame -- one speaker: track "
            << track1 << ", bands " << bands1 << ", loudness " << loud1 << "; twenty: track " << track20 << ", bands "
            << bands20 << ", loudness " << loud20);
    // Twenty mouths read from the sound itself are well inside a millisecond.
    CHECK(bands20 < 2000.0);
}
