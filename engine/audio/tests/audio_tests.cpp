#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/asset/localized.h"
#include "engine/audio/audio.h"
#include "engine/audio/scene_types.h"
#include "engine/platform/async_io.h"
#include "engine/platform/file.h"
#include "engine/scene/world.h"

namespace {

namespace audio = engine::audio;
namespace core = engine::core;
namespace scene = engine::scene;

using core::InstanceId;

// One tick at the default rate. Named, because every duration below is a
// multiple of it and a literal 1/60 in eight places is a number nobody can
// change.
constexpr double Tick = 1.0 / 60.0;

struct Fixture
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    std::optional<scene::World> world;
    audio::AudioSystem system;

    Fixture()
    {
        scene::generated::registerClasses(classes, atoms);
        audio::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        world.emplace(classes, enums, atoms, 1u);
        // Headless: the timeline is what these cases are about, and a test that
        // opened a device would be a test that behaved differently on a machine
        // with no sound card.
        REQUIRE_FALSE(system.start(true).has_value());
    }

    InstanceId make(const char* className)
    {
        const scene::ClassId id = classes.findId(atoms.intern(className));
        REQUIRE(id != scene::InvalidClass);
        return world->create(id);
    }

    [[nodiscard]] scene::SoundComponent& sound(InstanceId id)
    {
        scene::SoundComponent* component = world->sounds().find(id);
        REQUIRE(component != nullptr);
        return *component;
    }

    void run(int ticks)
    {
        for (int index = 0; index < ticks; ++index)
            system.tick(*world, Tick);
    }

    [[nodiscard]] std::vector<std::string> events()
    {
        std::vector<std::string> names;
        for (const scene::Change& change : world->changes().take()) {
            if (change.kind == scene::ChangeKind::InstanceEventNoArgs)
                names.emplace_back(atoms.text(change.name));
        }
        return names;
    }
};

} // namespace

TEST_CASE("the timeline is the simulation's, tick by tick")
{
    Fixture fixture;
    const InstanceId id = fixture.make("Sound");
    fixture.sound(id).playing = true;

    fixture.run(30);
    // Half a second of ticks is half a second of timeline, exactly -- no device
    // involved, no wall clock consulted. That is the whole of Decision 9.
    CHECK(fixture.sound(id).timePosition == doctest::Approx(0.5).epsilon(0.001));
}

TEST_CASE("PlaybackSpeed scales the timeline")
{
    Fixture fixture;
    const InstanceId id = fixture.make("Sound");
    fixture.sound(id).playing = true;
    fixture.sound(id).playbackSpeed = 2.0f;

    fixture.run(15);
    CHECK(fixture.sound(id).timePosition == doctest::Approx(0.5).epsilon(0.001));
}

TEST_CASE("Ended fires once, on the tick the timeline reaches the end")
{
    Fixture fixture;
    const InstanceId id = fixture.make("Sound");
    fixture.sound(id).playing = true;
    (void)fixture.events(); // `Loaded`, which fires on the first tick.

    fixture.run(30);
    const std::vector<std::string> early = fixture.events();
    CHECK(std::ranges::find(early, "Ended") == early.end());

    fixture.run(60);
    const std::vector<std::string> late = fixture.events();
    CHECK(std::ranges::count(late, "Ended") == 1);
    CHECK_FALSE(fixture.sound(id).playing);

    // And not again. It is a past-tense fact about reaching the end, and a
    // stopped sound does not keep reaching it.
    fixture.run(60);
    const std::vector<std::string> after = fixture.events();
    CHECK(std::ranges::find(after, "Ended") == after.end());
}

TEST_CASE("a looped sound wraps and never ends")
{
    Fixture fixture;
    const InstanceId id = fixture.make("Sound");
    fixture.sound(id).playing = true;
    fixture.sound(id).looped = true;
    (void)fixture.events();

    fixture.run(150);
    CHECK(fixture.sound(id).playing);
    const std::vector<std::string> seen = fixture.events();
    CHECK(std::ranges::find(seen, "Ended") == seen.end());

    // Wrapped rather than reset: 150 ticks is 2.5 seconds, so the position is
    // half a second in and not zero. A loop that reset would drift against
    // everything else in the scene by the fraction of a tick it overshot by.
    CHECK(fixture.sound(id).timePosition == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("Loaded fires once per content")
{
    Fixture fixture;
    (void)fixture.make("Sound");

    fixture.run(1);
    const std::vector<std::string> first = fixture.events();
    CHECK(std::ranges::count(first, "Loaded") == 1);

    // Bound to a local rather than compared in place: `events()` drains, so two
    // calls in one expression compare an iterator from one drain against the
    // end of a different, empty one.
    fixture.run(5);
    const std::vector<std::string> rest = fixture.events();
    CHECK(std::ranges::find(rest, "Loaded") == rest.end());
}

TEST_CASE("a headless system reports no device and counts no underruns")
{
    Fixture fixture;
    const audio::AudioStats stats = fixture.system.stats();
    // The gate's number, on the tier that has no sound card -- which is every CI
    // runner. Zero here is a real zero: no callback ran, so none could starve.
    CHECK_FALSE(stats.deviceOpen);
    CHECK(stats.underruns == 0);
    CHECK(stats.droppedCommands == 0);
}

// ---------------------------------------------------------------------------
// `Sound.Content` (roadmap M7: it stops being `Inert`).

namespace {

// A WAV written by the test rather than checked into the repository.
//
// Deliberately: a fixture asset is a binary in git that nobody can diff and that
// ADR 0032 exists to keep out, and a tone is forty lines of arithmetic. It also
// means the test states its own expectations -- the length below is not a fact
// about a file somebody has to go and open.
void writeTone(const std::filesystem::path& path, engine::core::u32 rate, engine::core::u32 frames,
               engine::core::u32 firstFrame = 0)
{
    std::vector<char> bytes;
    const auto put = [&bytes](const void* data, std::size_t size) {
        const auto* const at = static_cast<const char*>(data);
        bytes.insert(bytes.end(), at, at + size);
    };
    const auto putU32 = [&put](engine::core::u32 value) { put(&value, sizeof(value)); };
    const auto putU16 = [&put](engine::core::u16 value) { put(&value, sizeof(value)); };

    const engine::core::u32 dataBytes = frames * 2u;
    put("RIFF", 4);
    putU32(36u + dataBytes);
    put("WAVE", 4);
    put("fmt ", 4);
    putU32(16u);
    putU16(1u); // PCM
    putU16(1u); // mono
    putU32(rate);
    putU32(rate * 2u);
    putU16(2u);
    putU16(16u);
    put("data", 4);
    putU32(dataBytes);
    for (engine::core::u32 frame = 0; frame < frames; ++frame) {
        // `firstFrame` starts the tone part-way through, so a short file can
        // hold exactly the samples a long one has at some later point.
        const double phase =
            2.0 * 3.14159265358979 * 440.0 * static_cast<double>(frame + firstFrame) / static_cast<double>(rate);
        const auto sample = static_cast<engine::core::i16>(20000.0 * std::sin(phase));
        putU16(static_cast<engine::core::u16>(sample));
    }

    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A content directory with one sound in it, removed on the way out.
struct ContentFixture
{
    std::filesystem::path root;
    engine::asset::ContentMounts mounts;

    ContentFixture()
    {
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec) / "engine-audio-content";
        std::filesystem::remove_all(root, ec);
        writeTone(root / "sfx" / "tone.wav", 44100u, 11025u);
        // **Longer than the placeholder tone**, which is the whole point of it:
        // every duration case below is a comparison against one second, and a
        // fixture that was only ever shorter than that could not tell a sound
        // measured by its file from one measured by the constant.
        writeTone(root / "sfx" / "long.wav", 44100u, 44100u * 3u);
        mounts.mountDirectory(root);
    }

    ~ContentFixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

// Runs frames until the prefetch pipeline is empty, or gives up.
//
// **Bounded, and it sleeps.** A test that spins forever on a defect reports as a
// hung machine rather than as a failure -- and the sleep is the point rather
// than a delay: the read is on the IO thread and the decode is on a worker, so
// a busy loop on this thread would starve the very work it is waiting for.
void settleAudio(Fixture& fixture, int frames = 2000)
{
    for (int frame = 0; frame < frames; ++frame) {
        fixture.system.update(*fixture.world, InstanceId{});
        const audio::AudioStats stats = fixture.system.stats();
        if (stats.clipsLoaded + stats.clipsMissing > 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // One more, so the pass that installs is not the pass the loop stopped on.
    fixture.system.update(*fixture.world, InstanceId{});
}

} // namespace

TEST_CASE("a sound whose content resolves plays the file rather than the tone")
{
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);

    const InstanceId id = fixture.make("Sound");
    scene::SoundComponent& sound = fixture.sound(id);
    sound.content = "asset://sfx/tone.wav";
    sound.playing = true;
    fixture.system.tick(*fixture.world, Tick);
    fixture.system.update(*fixture.world, InstanceId{});

    // Decoded exactly once, however many frames run: the cache is what stops a
    // sound re-decoding its file sixty times a second.
    CHECK(fixture.system.stats().clipsLoaded == 1);
    CHECK(fixture.system.stats().clipsMissing == 0);

    fixture.system.tick(*fixture.world, Tick);
    fixture.system.update(*fixture.world, InstanceId{});
    CHECK(fixture.system.stats().clipsLoaded == 1);
}

TEST_CASE("a sound whose content names nothing still plays, as the placeholder")
{
    // The decision M6 made and M7 keeps: a sound that went silent because a file
    // was missing is a bug report about the sound. It is counted rather than
    // hidden, which is what makes "did my audio load" answerable.
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);

    const InstanceId id = fixture.make("Sound");
    scene::SoundComponent& sound = fixture.sound(id);
    sound.content = "asset://sfx/absent.wav";
    sound.playing = true;
    fixture.system.tick(*fixture.world, Tick);
    fixture.system.update(*fixture.world, InstanceId{});

    CHECK(fixture.system.stats().clipsLoaded == 0);
    CHECK(fixture.system.stats().clipsMissing == 1);
}

TEST_CASE("swapping the mounts drops what was decoded against the old ones")
{
    // A voice holds a raw pointer into a clip for the length of an audio
    // callback, so the cache can never drop one underneath it -- which makes
    // replacing the mounts the only moment a clip may go away, and it has to
    // take the voices with it.
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);

    const InstanceId id = fixture.make("Sound");
    scene::SoundComponent& sound = fixture.sound(id);
    sound.content = "asset://sfx/tone.wav";
    sound.playing = true;
    fixture.system.tick(*fixture.world, Tick);
    fixture.system.update(*fixture.world, InstanceId{});
    REQUIRE(fixture.system.stats().clipsLoaded == 1);

    fixture.system.setContentMounts(nullptr);
    CHECK(fixture.system.stats().clipsLoaded == 0);
}

TEST_CASE("a decoded sound is as long as its file, not as long as the placeholder")
{
    // D092. `tick` measured every sound in the world against a one-second
    // constant, which was correct for the whole of M6 -- nothing decoded a file,
    // so every sound WAS a one-second tone -- and was never revisited when M7
    // made `Content` real.
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);

    // Both directions, because a length that is merely "not one" could still be
    // any wrong number: a quarter of a second is shorter than the placeholder
    // and three seconds is longer, and each is its own file's own length.
    CHECK(fixture.system.clipDuration("asset://sfx/tone.wav") == doctest::Approx(0.25).epsilon(0.01));
    CHECK(fixture.system.clipDuration("asset://sfx/long.wav") == doctest::Approx(3.0).epsilon(0.01));

    // And a content that names nothing is still one second, because one second
    // is how long the tone such a sound plays lasts.
    CHECK(fixture.system.clipDuration("asset://sfx/absent.wav") == doctest::Approx(1.0).epsilon(0.001));
    CHECK(fixture.system.clipDuration("") == doctest::Approx(1.0).epsilon(0.001));
}

TEST_CASE("Ended fires at the end of the FILE")
{
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);

    const InstanceId id = fixture.make("Sound");
    scene::SoundComponent& sound = fixture.sound(id);
    sound.content = "asset://sfx/long.wav";
    sound.playing = true;
    (void)fixture.events(); // `Loaded`, which fires on the first tick.

    // **The second the defect stopped at.** A two-minute track that plays for
    // one second is what was reported, and this is the same thing at three.
    fixture.run(60);
    CHECK(fixture.sound(id).playing);
    const std::vector<std::string> atOne = fixture.events();
    CHECK(std::ranges::find(atOne, "Ended") == atOne.end());

    fixture.run(60);
    CHECK(fixture.sound(id).playing);

    // Past three seconds, with a tick of slack for the frame or two a resampler
    // moves the boundary by.
    fixture.run(65);
    CHECK_FALSE(fixture.sound(id).playing);
    const std::vector<std::string> atEnd = fixture.events();
    CHECK(std::ranges::count(atEnd, "Ended") == 1);
    // Rewound at the end, so starting it again plays it again.
    CHECK(fixture.sound(id).timePosition == 0.0);
}

TEST_CASE("a looped sound wraps at the file's length")
{
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);

    const InstanceId id = fixture.make("Sound");
    scene::SoundComponent& sound = fixture.sound(id);
    sound.content = "asset://sfx/long.wav";
    sound.playing = true;
    sound.looped = true;
    (void)fixture.events();

    // Three and a half seconds of a three-second file is half a second in --
    // wrapped by the FILE's length.
    fixture.run(210);
    CHECK(fixture.sound(id).playing);
    CHECK(fixture.sound(id).timePosition == doctest::Approx(0.5).epsilon(0.02));

    // And the quarter-second file is what makes the case discriminating: 0.7 s
    // of it is 0.2 s in, which is a number neither the old constant nor the
    // other file could produce.
    fixture.sound(id).content = "asset://sfx/tone.wav";
    fixture.sound(id).timePosition = 0.0;
    fixture.run(42);
    CHECK(fixture.sound(id).timePosition == doctest::Approx(0.2).epsilon(0.05));
}

TEST_CASE("TimeLength is the file's, read before playing, and TimePosition stays inside it")
{
    // **The owner: "Properties should show a sound's length, and its time
    // position should be clamped to 0..Length".**
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);

    const InstanceId id = fixture.make("Sound");
    scene::World& world = *fixture.world;
    const core::NameAtom length = fixture.atoms.intern("TimeLength");
    const core::NameAtom position = fixture.atoms.intern("TimePosition");
    using Result = scene::World::SetResult;
    const auto number = [&](core::NameAtom name) { return std::get<double>(world.getProperty(id, name).value()); };
    const auto written = [](Result result) { return result == Result::Changed || result == Result::Unchanged; };

    REQUIRE(written(
        world.setProperty(id, fixture.atoms.intern("Content"), scene::Value{std::string("asset://sfx/long.wav")})));
    CHECK(number(length) == 0.0);
    // One tick reads it, with the sound stopped.
    fixture.run(1);
    CHECK_FALSE(fixture.sound(id).playing);
    CHECK(number(length) == doctest::Approx(3.0).epsilon(0.01));
    // Read-only to a script.
    CHECK_FALSE(written(world.setProperty(id, length, scene::Value{10.0})));

    // Past the end is the end; a loop wraps; below zero is refused.
    REQUIRE(written(world.setProperty(id, position, scene::Value{10.0})));
    CHECK(number(position) == doctest::Approx(number(length)));
    fixture.sound(id).looped = true;
    REQUIRE(written(world.setProperty(id, position, scene::Value{number(length) + 0.5})));
    CHECK(number(position) == doctest::Approx(0.5).epsilon(0.02));
    CHECK_FALSE(written(world.setProperty(id, position, scene::Value{-1.0})));

    // A new content is a new length.
    REQUIRE(written(
        world.setProperty(id, fixture.atoms.intern("Content"), scene::Value{std::string("asset://sfx/tone.wav")})));
    CHECK(number(length) == 0.0);
    fixture.run(1);
    CHECK(number(length) == doctest::Approx(0.25).epsilon(0.05));
}

TEST_CASE("the mixer keeps its cursor unless the timeline has really moved")
{
    // D093, as the rule rather than as the plumbing. The two clocks are the same
    // clock in a healthy run and are quantised differently -- the simulation
    // steps in whole ticks, the device in whole buffers -- so on any given frame
    // one is ahead of the other by less than either quantum.
    constexpr double tolerance = 0.25;

    // A tick and a buffer apart is the ordinary case, and taking the timeline
    // here is what replayed sixteen milliseconds of audio sixty times a second.
    CHECK_FALSE(audio::detail::shouldTakeTimeline(1.000, 0.984, 3.0, false, tolerance));
    CHECK_FALSE(audio::detail::shouldTakeTimeline(0.984, 1.000, 3.0, false, tolerance));
    CHECK_FALSE(audio::detail::shouldTakeTimeline(1.0, 1.0, 3.0, false, tolerance));

    // A script seeking is not a quantisation, and the mixer follows it.
    CHECK(audio::detail::shouldTakeTimeline(1.0, 2.5, 3.0, false, tolerance));
    CHECK(audio::detail::shouldTakeTimeline(2.5, 0.0, 3.0, false, tolerance));

    // **A loop is compared the short way round.** The frame in which the mixer
    // has wrapped and the timeline has not is a whole clip apart by subtraction
    // and a millisecond apart in fact; calling it a seek would put a click at
    // the top of every loop.
    CHECK_FALSE(audio::detail::shouldTakeTimeline(0.001, 2.999, 3.0, true, tolerance));
    CHECK_FALSE(audio::detail::shouldTakeTimeline(2.999, 0.001, 3.0, true, tolerance));
    // And the same two numbers on a sound that does NOT loop are exactly the
    // seek they look like.
    CHECK(audio::detail::shouldTakeTimeline(0.001, 2.999, 3.0, false, tolerance));
    // A seek inside a looped sound is still a seek.
    CHECK(audio::detail::shouldTakeTimeline(0.1, 1.6, 3.0, true, tolerance));
}

TEST_CASE("an audition plays a file without a Sound and without the world")
{
    // The editor's preview. It is deliberately not a `Sound` and deliberately
    // not a voice: the one thing it has that neither of those may have is a
    // cursor the wall clock drives.
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);

    CHECK_FALSE(fixture.system.auditioning());

    fixture.system.audition("asset://sfx/long.wav", 0.5f, 1.0f);
    CHECK(fixture.system.auditioning());
    CHECK(fixture.system.auditioning("asset://sfx/long.wav"));
    // Which is what a play button on one row asks so that the OTHER rows do not
    // draw themselves as playing.
    CHECK_FALSE(fixture.system.auditioning("asset://sfx/tone.wav"));

    // Starting another replaces it. Two previews at once is not a thing anybody
    // asked for.
    fixture.system.audition("asset://sfx/tone.wav", 0.5f, 1.0f);
    CHECK(fixture.system.auditioning("asset://sfx/tone.wav"));
    CHECK_FALSE(fixture.system.auditioning("asset://sfx/long.wav"));

    fixture.system.stopAudition();
    CHECK_FALSE(fixture.system.auditioning());

    // **A file that will not decode is not auditioned.** A button that latched
    // on silence would say the opposite of what happened; the warning the
    // decoder already logged is the honest answer.
    fixture.system.audition("asset://sfx/absent.wav", 0.5f, 1.0f);
    CHECK_FALSE(fixture.system.auditioning());

    // And it holds a clip pointer like every voice does, so replacing the
    // mounts has to take it with them.
    fixture.system.audition("asset://sfx/tone.wav", 0.5f, 1.0f);
    REQUIRE(fixture.system.auditioning());
    fixture.system.setContentMounts(nullptr);
    CHECK_FALSE(fixture.system.auditioning());
}

TEST_CASE("an audition does not touch the sound it was started from")
{
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);

    const InstanceId id = fixture.make("Sound");
    scene::SoundComponent& sound = fixture.sound(id);
    sound.content = "asset://sfx/long.wav";
    sound.timePosition = 1.25;

    fixture.system.audition(sound.content, sound.volume, sound.playbackSpeed);
    fixture.run(30);

    // `Playing` is the game's state and `TimePosition` is the simulation's, and
    // a preview owns neither. The timeline did not advance because the sound is
    // not playing -- which is exactly the state an audition has to work in.
    CHECK(fixture.system.auditioning());
    CHECK_FALSE(fixture.sound(id).playing);
    CHECK(fixture.sound(id).timePosition == doctest::Approx(1.25));
    const std::vector<std::string> seen = fixture.events();
    CHECK(std::ranges::find(seen, "Ended") == seen.end());
}

// --- Which side a sound comes out of -----------------------------------------
//
// Distance alone is half of positional: it says a sound is near without saying
// it is on your left, so turning around changed nothing at all. These cases are
// the other half, and they are written against the LISTENER's frame rather than
// against world axes, because "left" is a fact about where somebody is facing.

namespace {

// A listener at `at`, facing `towards`. Built through the engine's own
// `lookAtCFrame` rather than by writing a matrix here, so the case cannot pass
// against a convention this file invented -- the look axis is -Z and column 0
// is right, and a test that assumed otherwise would agree with itself forever.
[[nodiscard]] engine::core::CFrameD listenerAt(engine::core::DVec3 at, engine::core::DVec3 towards)
{
    return engine::core::lookAtCFrame(at, towards, engine::core::Vec3{0.0f, 1.0f, 0.0f});
}

} // namespace

TEST_CASE("a source is panned by where it is across the listener")
{
    // Facing -Z, which is where `lookAtCFrame` puts a listener told to look at
    // something in front of it.
    const engine::core::CFrameD ear = listenerAt({0.0, 0.0, 0.0}, {0.0, 0.0, -10.0});

    // Straight ahead and straight behind are both centre, and that is honest
    // rather than wrong: two channels cannot tell front from back without a
    // head model, and choosing a side for a sound that is on neither would be
    // an invention.
    CHECK(audio::detail::panOf(ear, {0.0, 0.0, -10.0}) == doctest::Approx(0.0).epsilon(0.001));
    CHECK(audio::detail::panOf(ear, {0.0, 0.0, 10.0}) == doctest::Approx(0.0).epsilon(0.001));

    // +X is the listener's right, because column 0 is the right vector.
    CHECK(audio::detail::panOf(ear, {10.0, 0.0, 0.0}) == doctest::Approx(1.0).epsilon(0.001));
    CHECK(audio::detail::panOf(ear, {-10.0, 0.0, 0.0}) == doctest::Approx(-1.0).epsilon(0.001));

    // **Distance does not change the side.** A source at the same angle pans
    // the same however far away it is -- the falloff is what distance is for,
    // and mixing the two would make a far sound drift towards the middle.
    CHECK(audio::detail::panOf(ear, {1.0, 0.0, 0.0}) == doctest::Approx(1.0).epsilon(0.001));
    CHECK(audio::detail::panOf(ear, {1000.0, 0.0, 0.0}) == doctest::Approx(1.0).epsilon(0.001));

    // Forty-five degrees to the right is the sine of forty-five degrees.
    CHECK(audio::detail::panOf(ear, {5.0, 0.0, -5.0}) == doctest::Approx(0.7071).epsilon(0.01));
}

TEST_CASE("turning around swaps the sides")
{
    // The case the reporter would run: stand still, look the other way, and the
    // sound has to move. Against a listener read as a POSITION rather than as a
    // frame -- which is what it was -- both of these answer zero.
    const engine::core::CFrameD facingAway = listenerAt({0.0, 0.0, 0.0}, {0.0, 0.0, -10.0});
    const engine::core::CFrameD facingBack = listenerAt({0.0, 0.0, 0.0}, {0.0, 0.0, 10.0});

    const engine::core::DVec3 source{10.0, 0.0, 0.0};
    CHECK(audio::detail::panOf(facingAway, source) == doctest::Approx(1.0).epsilon(0.001));
    CHECK(audio::detail::panOf(facingBack, source) == doctest::Approx(-1.0).epsilon(0.001));
}

TEST_CASE("a source with no side is centred rather than guessed at")
{
    const engine::core::CFrameD ear = listenerAt({0.0, 0.0, 0.0}, {0.0, 0.0, -10.0});

    // Directly overhead and directly below have no horizontal direction at all,
    // and a sound standing exactly on the listener is the one case where
    // hard-panning would be most wrong.
    CHECK(audio::detail::panOf(ear, {0.0, 10.0, 0.0}) == doctest::Approx(0.0).epsilon(0.001));
    CHECK(audio::detail::panOf(ear, {0.0, -10.0, 0.0}) == doctest::Approx(0.0).epsilon(0.001));
    CHECK(audio::detail::panOf(ear, {0.0, 0.0, 0.0}) == doctest::Approx(0.0).epsilon(0.001));

    // And elevation does not steal the side: a source up and to the right is
    // still fully to the right, because the pan is an azimuth.
    CHECK(audio::detail::panOf(ear, {10.0, 50.0, 0.0}) == doctest::Approx(1.0).epsilon(0.001));
}

TEST_CASE("the pan holds its power across the field")
{
    engine::core::f32 left = 0.0f;
    engine::core::f32 right = 0.0f;

    audio::detail::panGains(-1.0f, left, right);
    CHECK(left == doctest::Approx(1.0).epsilon(0.001));
    CHECK(right == doctest::Approx(0.0).epsilon(0.001));

    audio::detail::panGains(1.0f, left, right);
    CHECK(left == doctest::Approx(0.0).epsilon(0.001));
    CHECK(right == doctest::Approx(1.0).epsilon(0.001));

    audio::detail::panGains(0.0f, left, right);
    CHECK(left == doctest::Approx(0.7071).epsilon(0.001));
    CHECK(right == doctest::Approx(0.7071).epsilon(0.001));

    // **Constant POWER, which is the whole reason it is a quarter turn and not
    // a straight line.** Two channels at half amplitude are quieter than one at
    // full, so a linear pan dips in the middle and a sound crossing in front of
    // you audibly ducks as it passes. The sum of squares is one everywhere.
    for (const engine::core::f32 pan : {-1.0f, -0.5f, 0.0f, 0.3f, 1.0f}) {
        audio::detail::panGains(pan, left, right);
        CHECK(left * left + right * right == doctest::Approx(1.0).epsilon(0.001));
    }

    // Out of range is clamped rather than wrapped: a pan of two is a caller's
    // arithmetic error, and cos of a bigger angle would swing it back towards
    // the middle and then to the wrong side entirely.
    audio::detail::panGains(4.0f, left, right);
    CHECK(right == doctest::Approx(1.0).epsilon(0.001));
    audio::detail::panGains(-4.0f, left, right);
    CHECK(left == doctest::Approx(1.0).epsilon(0.001));
}

// --- The prefetch (D129) -----------------------------------------------------
//
// **What this is for, and what it deliberately is not.** A sound's first play
// used to decode the whole file wherever it was first asked for, and for a long
// track that is tens of megabytes of f32 produced before the frame could end.
// The prefetch moves that work off the frame -- an asynchronous read and a
// decode job, both started from `update` and never from `tick`.
//
// The floor is what makes it legal. If the tick asks for a clip the prefetch has
// not landed, it decodes it itself, exactly as it always did. So the ANSWER is
// the same whether the prefetch won its race or not, and the world hash cannot
// learn anything about how fast the disk is (R10). The cases below assert both
// halves, because either one alone is either a stall or a determinism break.

TEST_CASE("a sound that is not playing still has its clip fetched")
{
    // The whole point: a sound authored in a scene is prefetched from the first
    // frame the scene is alive, so by the time anything plays it there is
    // nothing left to decode. `playing` is never set here.
    REQUIRE(engine::platform::initIo());
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);

    const InstanceId id = fixture.make("Sound");
    fixture.sound(id).content = "asset://sfx/tone.wav";

    CHECK(fixture.system.stats().clipsLoaded == 0);

    settleAudio(fixture);

    CHECK(fixture.system.stats().clipsLoaded == 1);
}

TEST_CASE("the tick answers the same whether the prefetch won or not")
{
    // **The determinism assertion, and it is the reason the synchronous path
    // stays.** One system is given every chance to prefetch; the other is given
    // none and goes straight to a tick. Both must report the same duration, or
    // the world hash depends on the disk.
    ContentFixture content;

    double withPrefetch = 0.0;
    {
        REQUIRE(engine::platform::initIo());
        Fixture fixture;
        fixture.system.setContentMounts(&content.mounts);
        const InstanceId id = fixture.make("Sound");
        fixture.sound(id).content = "asset://sfx/long.wav";
        settleAudio(fixture);
        withPrefetch = fixture.system.clipDuration("asset://sfx/long.wav");
    }

    double without = 0.0;
    {
        Fixture fixture;
        fixture.system.setContentMounts(&content.mounts);
        // No `update` at all, so nothing was ever prefetched.
        without = fixture.system.clipDuration("asset://sfx/long.wav");
    }

    CHECK(withPrefetch > 2.9);
    CHECK(withPrefetch == doctest::Approx(without));
}

TEST_CASE("a URN that names nothing is fetched once and does not leak its slot")
{
    // D131's class of defect: a failed asynchronous read whose slot nobody
    // releases, and at five hundred and twelve every read in the process is
    // refused. Six hundred attempts is past that by a margin.
    REQUIRE(engine::platform::initIo());
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);

    const InstanceId id = fixture.make("Sound");
    for (int attempt = 0; attempt < 600; ++attempt) {
        fixture.sound(id).content = "asset://sfx/not-a-file.wav";
        fixture.system.update(*fixture.world, InstanceId{});
    }

    // And a real one still resolves afterwards, which is the assertion that
    // actually catches exhaustion: the counters would look fine either way.
    fixture.sound(id).content = "asset://sfx/tone.wav";
    settleAudio(fixture);
    CHECK(fixture.system.clipDuration("asset://sfx/tone.wav") > 0.0);
}

TEST_CASE("tearing down with a fetch in flight leaves nothing behind")
{
    // A decode job writes into memory the system owns, and returning while one
    // runs is a use-after-free that reproduces on a fast machine and never on a
    // slow one -- the lesson `MeshLoader` and `UiText` each paid for once.
    REQUIRE(engine::platform::initIo());
    ContentFixture content;
    {
        Fixture fixture;
        fixture.system.setContentMounts(&content.mounts);
        const InstanceId id = fixture.make("Sound");
        fixture.sound(id).content = "asset://sfx/long.wav";
        // One pass only: the read is started and deliberately not waited for.
        fixture.system.update(*fixture.world, InstanceId{});
    }
    // Reaching here without a fault is the assertion.
    CHECK(true);
}

// --- The rest of D129: a header for the tick, and a stream for long files ------
//
// The prefetch left two things. A sound made and played on one tick still made
// the TICK decode its file, because the tick needs its length; and a long file
// was decoded whole and held, seventy megabytes of f32 for three minutes. The
// tick reads the file's header now, and a long file streams.

namespace {

// A content directory with a long file, and short ones holding exactly the
// samples the long one has at its start and at five seconds in.
struct StreamFixture
{
    std::filesystem::path root;
    engine::asset::ContentMounts mounts;

    StreamFixture()
    {
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec) / "engine-audio-stream";
        std::filesystem::remove_all(root, ec);
        // 48 kHz, so nothing is resampled and two decodes of the same samples
        // are the same numbers.
        writeTone(root / "music" / "song.wav", 48000u, 48000u * 12u);
        writeTone(root / "sfx" / "head.wav", 48000u, 48000u);
        writeTone(root / "sfx" / "five.wav", 48000u, 48000u, 48000u * 5u);
        mounts.mountDirectory(root);
    }

    ~StreamFixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

// What one sound sounds like for `frames` frames from `at` seconds in.
std::vector<float> listen(const engine::asset::ContentMounts& mounts, const char* content, double at, core::u32 frames,
                          bool looped = false)
{
    Fixture fixture;
    fixture.system.setContentMounts(&mounts);
    const InstanceId id = fixture.make("Sound");
    fixture.sound(id).content = content;
    fixture.sound(id).timePosition = at;
    fixture.sound(id).looped = looped;
    fixture.sound(id).playing = true;
    fixture.system.update(*fixture.world, InstanceId{});
    std::vector<float> out(static_cast<std::size_t>(frames) * 2u);
    fixture.system.renderInto(out);
    return out;
}

} // namespace

TEST_CASE("a sound made and played on one tick is timed from its header, and the tick decodes nothing")
{
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);

    const InstanceId id = fixture.make("Sound");
    fixture.sound(id).content = "asset://sfx/long.wav";
    fixture.sound(id).playing = true;
    fixture.system.tick(*fixture.world, Tick);

    // The length came from the WAV header: nothing was decoded, by the tick or
    // by anything else.
    CHECK(fixture.system.stats().tickDecodes == 0);
    CHECK(fixture.system.stats().clipsLoaded == 0);
    CHECK(fixture.system.clipDuration("asset://sfx/long.wav") == doctest::Approx(3.0).epsilon(0.001));
}

TEST_CASE("a file's declared length is read from its header, Ogg Vorbis included")
{
    // A WAV, through the decoder's header.
    {
        StreamFixture content;
        std::vector<std::byte> bytes;
        REQUIRE(engine::platform::readFile(content.root / "sfx" / "head.wav", bytes));
        const std::optional<core::u64> frames = audio::detail::probeFrames(bytes);
        REQUIRE(frames.has_value());
        CHECK(*frames == 48000u);
    }

    // An Ogg Vorbis stream, by hand: an identification header on the first
    // page saying 44.1 kHz, and a last page whose granule is ten seconds of
    // source samples. Nothing here decodes; the probe reads two numbers.
    std::vector<std::byte> ogg;
    const auto put = [&ogg](std::initializer_list<int> values) {
        for (const int value : values)
            ogg.push_back(static_cast<std::byte>(value));
    };
    // Wider than eight bytes is zero padding: shifting a u64 by 64 or more is
    // undefined, and the nightly's UBSan says so.
    const auto putLe = [&ogg](core::u64 value, int width) {
        for (int index = 0; index < width; ++index)
            ogg.push_back(static_cast<std::byte>(index < 8 ? (value >> (8 * index)) & 0xFFu : 0u));
    };
    const auto page = [&](core::u64 granule, std::initializer_list<int> segments) {
        put({'O', 'g', 'g', 'S', 0, 2});
        putLe(granule, 8);
        putLe(1, 4);
        putLe(0, 4);
        putLe(0, 4);
        ogg.push_back(static_cast<std::byte>(segments.size()));
        put(segments);
    };
    page(0, {30});
    put({1, 'v', 'o', 'r', 'b', 'i', 's'});
    putLe(0, 4);
    put({2});
    putLe(44100, 4);
    putLe(0, 12);
    // A page no packet ends in carries a granule of all ones and is skipped.
    page(441000, {});
    page(~core::u64{0}, {});
    const std::optional<core::u64> vorbis = audio::detail::probeFrames(ogg);
    REQUIRE(vorbis.has_value());
    CHECK(*vorbis == 480000u);

    // And a file that says nothing is nothing, not a guess.
    const std::vector<std::byte> noise(64, std::byte{0x5A});
    CHECK_FALSE(audio::detail::probeFrames(noise).has_value());
}

TEST_CASE("a long file streams: it is not decoded whole, and it sounds the same")
{
    StreamFixture content;

    {
        Fixture fixture;
        fixture.system.setContentMounts(&content.mounts);
        const InstanceId id = fixture.make("Sound");
        fixture.sound(id).content = "asset://music/song.wav";
        fixture.sound(id).playing = true;
        fixture.system.tick(*fixture.world, Tick);
        fixture.system.update(*fixture.world, InstanceId{});
        CHECK(fixture.system.stats().clipsLoaded == 1);
        CHECK(fixture.system.stats().clipsStreamed == 1);
        CHECK(fixture.system.clipDuration("asset://music/song.wav") == doctest::Approx(12.0).epsilon(0.0001));
    }

    // The first tenth of a second of the streamed song is the same samples as
    // the decoded file holding the same tone.
    const std::vector<float> streamed = listen(content.mounts, "asset://music/song.wav", 0.0, 4800u);
    const std::vector<float> decoded = listen(content.mounts, "asset://sfx/head.wav", 0.0, 4800u);
    CHECK(streamed == decoded);
    CHECK(std::ranges::any_of(streamed, [](float sample) { return std::abs(sample) > 0.1f; }));
}

TEST_CASE("a streamed sound seeks: five seconds in is what the file holds at five seconds")
{
    StreamFixture content;
    const std::vector<float> seeked = listen(content.mounts, "asset://music/song.wav", 5.0, 4800u);
    const std::vector<float> expected = listen(content.mounts, "asset://sfx/five.wav", 0.0, 4800u);
    CHECK(seeked == expected);
}

TEST_CASE("a streamed sound that is looped wraps at the file's end and keeps playing")
{
    StreamFixture content;
    // A tenth of a second before the end, and a fifth of a second of output:
    // half of it is the tail and half is the head again.
    const std::vector<float> once = listen(content.mounts, "asset://music/song.wav", 11.9, 9600u);
    CHECK(std::all_of(once.begin() + 4800 * 2, once.end(), [](float sample) { return sample == 0.0f; }));

    const std::vector<float> looped = listen(content.mounts, "asset://music/song.wav", 11.9, 9600u, true);
    const std::vector<float> head = listen(content.mounts, "asset://sfx/head.wav", 0.0, 4800u);
    CHECK(std::equal(head.begin(), head.end(), looped.begin() + 4800 * 2));
}

TEST_CASE("a decoded sound that is looped loses no frame at the loop point")
{
    // Found writing the streamed case above: wrapping the cursor skipped the
    // output frame it happened on, so every loop of every looped sound had one
    // frame of silence in it -- a click, sixty times a minute on a short loop.
    StreamFixture content;
    const std::vector<float> looped = listen(content.mounts, "asset://sfx/head.wav", 0.9, 9600u, true);
    const std::vector<float> head = listen(content.mounts, "asset://sfx/head.wav", 0.0, 4800u);
    CHECK(std::equal(head.begin(), head.end(), looped.begin() + 4800 * 2));
}

// --- Sound effects (ADR 0131) -------------------------------------------------
//
// What an effect does is asked of the samples: the same sound is rendered
// offline with the effect and without it, and what came out is compared -- to
// the dry render, and to what that effect is for. The sound is the placeholder
// tone (a sine between 220 and 880 Hz at a quarter of full scale), which makes
// every expectation below arithmetic.

namespace {

struct Heard
{
    std::vector<float> samples;
    double rms = 0.0;
    float peak = 0.0f;
    int crossings = 0;
};

[[nodiscard]] Heard measure(std::vector<float> samples)
{
    Heard heard;
    double squares = 0.0;
    float before = 0.0f;
    for (std::size_t index = 0; index < samples.size(); index += 2) {
        const float left = samples[index];
        squares += static_cast<double>(left) * static_cast<double>(left);
        heard.peak = std::max(heard.peak, std::fabs(left));
        if ((before <= 0.0f && left > 0.0f) || (before >= 0.0f && left < 0.0f))
            ++heard.crossings;
        if (left != 0.0f)
            before = left;
    }
    heard.rms = std::sqrt(squares / static_cast<double>(samples.size() / 2));
    heard.samples = std::move(samples);
    return heard;
}

// One sound, playing, with whatever effects a case gives it or its group.
struct EffectRig
{
    Fixture fixture;
    InstanceId sound;
    InstanceId group;

    EffectRig()
    {
        group = fixture.make("AudioGroup");
        sound = fixture.make("Sound");
        fixture.sound(sound).playing = true;
        fixture.sound(sound).volume = 0.25f;
    }

    scene::SoundEffectComponent& effect(const char* className, InstanceId parent)
    {
        const InstanceId id = fixture.make(className);
        REQUIRE_FALSE(fixture.world->setParent(id, parent).has_value());
        scene::SoundEffectComponent* component = fixture.world->soundEffects().find(id);
        REQUIRE(component != nullptr);
        return *component;
    }

    // The next `seconds` of what the speakers would get.
    Heard hear(double seconds)
    {
        fixture.system.update(*fixture.world, InstanceId{});
        std::vector<float> samples(static_cast<std::size_t>(seconds * 48000.0) * 2, 0.0f);
        fixture.system.renderInto(samples);
        return measure(std::move(samples));
    }
};

[[nodiscard]] Heard dryTone(double seconds)
{
    EffectRig rig;
    return rig.hear(seconds);
}

} // namespace

TEST_CASE("an effect that is off, or under nothing that plays, changes nothing")
{
    const Heard dry = dryTone(0.25);
    REQUIRE(dry.rms == doctest::Approx(0.25 / std::sqrt(2.0)).epsilon(0.02));

    EffectRig rig;
    rig.effect("LowPassSoundEffect", rig.sound).enabled = false;
    // On a group the sound is not in.
    rig.effect("DistortionSoundEffect", rig.group);
    const Heard heard = rig.hear(0.25);
    CHECK(heard.samples == dry.samples);
}

TEST_CASE("a low pass takes a tone above its cutoff away, and a high pass one below")
{
    const Heard dry = dryTone(0.5);

    EffectRig low;
    low.effect("LowPassSoundEffect", low.sound).cutoff = 60.0f;
    // Past the filter's first moments.
    (void)low.hear(0.1);
    CHECK(low.hear(0.4).rms < dry.rms * 0.3);

    EffectRig high;
    high.effect("HighPassSoundEffect", high.sound).cutoff = 8000.0f;
    (void)high.hear(0.1);
    CHECK(high.hear(0.4).rms < dry.rms * 0.1);

    // And one that lets the tone through leaves it as loud.
    EffectRig open;
    open.effect("LowPassSoundEffect", open.sound).cutoff = 18000.0f;
    (void)open.hear(0.1);
    CHECK(open.hear(0.4).rms == doctest::Approx(dry.rms).epsilon(0.05));
}

TEST_CASE("an equalizer turns down the band the tone is in and no other")
{
    const Heard dry = dryTone(0.5);

    // The tone is below a middle band that starts at four kilohertz: in the
    // low band.
    EffectRig cut;
    scene::SoundEffectComponent& lows = cut.effect("EqualizerSoundEffect", cut.sound);
    lows.midLow = 4000.0f;
    lows.midHigh = 8000.0f;
    lows.lowGain = -40.0f;
    (void)cut.hear(0.1);
    CHECK(cut.hear(0.4).rms < dry.rms * 0.2);

    EffectRig other;
    scene::SoundEffectComponent& highs = other.effect("EqualizerSoundEffect", other.sound);
    highs.midLow = 4000.0f;
    highs.midHigh = 8000.0f;
    highs.highGain = -40.0f;
    (void)other.hear(0.1);
    CHECK(other.hear(0.4).rms == doctest::Approx(dry.rms).epsilon(0.1));
}

TEST_CASE("distortion squares a tone off, and a compressor turns a loud one down")
{
    const Heard dry = dryTone(0.5);
    // A sine's level is 0.707 of its peak.
    CHECK(dry.rms / static_cast<double>(dry.peak) == doctest::Approx(0.707).epsilon(0.02));

    EffectRig driven;
    driven.effect("DistortionSoundEffect", driven.sound).level = 1.0f;
    const Heard square = driven.hear(0.5);
    CHECK(square.rms / static_cast<double>(square.peak) > 0.85);
    CHECK(square.peak <= 1.0f);

    EffectRig squeezed;
    scene::SoundEffectComponent& compressor = squeezed.effect("CompressorSoundEffect", squeezed.sound);
    compressor.threshold = -40.0f;
    compressor.ratio = 20.0f;
    compressor.attack = 0.001f;
    (void)squeezed.hear(0.1);
    const Heard quiet = squeezed.hear(0.4);
    CHECK(quiet.rms < dry.rms * 0.4);
    // And its makeup gain gives it back.
    compressor.makeupGain = 12.0f;
    (void)squeezed.hear(0.1);
    CHECK(squeezed.hear(0.4).rms > quiet.rms * 3.0);
}

TEST_CASE("a pitch shift of an octave doubles the tone and leaves how long it is")
{
    const Heard dry = dryTone(0.5);

    EffectRig rig;
    rig.effect("PitchShiftSoundEffect", rig.sound).octave = 2.0f;
    (void)rig.hear(0.2);
    const Heard up = rig.hear(0.5);
    CHECK(up.crossings == doctest::Approx(dry.crossings * 2).epsilon(0.15));

    EffectRig lower;
    lower.effect("PitchShiftSoundEffect", lower.sound).octave = 0.5f;
    (void)lower.hear(0.2);
    CHECK(lower.hear(0.5).crossings == doctest::Approx(dry.crossings / 2).epsilon(0.15));
}

TEST_CASE("a chorus changes what is heard and stays inside full scale")
{
    const Heard dry = dryTone(0.5);
    EffectRig rig;
    rig.effect("ChorusSoundEffect", rig.sound);
    const Heard wet = rig.hear(0.5);
    double difference = 0.0;
    for (std::size_t index = 0; index < wet.samples.size(); ++index)
        difference += std::fabs(static_cast<double>(wet.samples[index] - dry.samples[index]));
    CHECK(difference / static_cast<double>(wet.samples.size()) > 0.01);
    CHECK(wet.peak <= 1.0f);
}

TEST_CASE("an echo and a reverb on a group go on after the sound has stopped; on the sound they stop with it")
{
    // Without either, a stopped sound is silence.
    {
        EffectRig rig;
        (void)rig.hear(0.1);
        rig.fixture.sound(rig.sound).playing = false;
        CHECK(rig.hear(0.5).peak == 0.0f);
    }
    for (const char* className : {"EchoSoundEffect", "ReverbSoundEffect"}) {
        CAPTURE(className);
        EffectRig rig;
        rig.fixture.sound(rig.sound).group = rig.group;
        (void)rig.effect(className, rig.group);
        (void)rig.hear(0.1);
        rig.fixture.sound(rig.sound).playing = false;
        const Heard tail = rig.hear(0.5);
        CHECK(tail.rms > 0.005);

        // The same effect on the sound itself has nothing to ring through.
        EffectRig own;
        (void)own.effect(className, own.sound);
        (void)own.hear(0.1);
        own.fixture.sound(own.sound).playing = false;
        CHECK(own.hear(0.5).peak == 0.0f);
    }

    // The echo comes back when its delay says: silence until then.
    EffectRig rig;
    rig.fixture.sound(rig.sound).group = rig.group;
    scene::SoundEffectComponent& echo = rig.effect("EchoSoundEffect", rig.group);
    echo.delay = 0.3f;
    echo.feedback = 0.0f;
    (void)rig.hear(0.1);
    rig.fixture.sound(rig.sound).playing = false;
    // The tone ran from 0 to 0.1 s, so its echo runs from 0.3 to 0.4 s: the
    // next 0.15 s hold none of it, and the 0.15 s after that hold it.
    CHECK(rig.hear(0.15).peak == 0.0f);
    CHECK(rig.hear(0.15).rms > 0.02);
}

TEST_CASE("effects under one parent apply in the order of their priority")
{
    const auto chain = [](bool distortionFirst) {
        EffectRig rig;
        scene::SoundEffectComponent& distortion = rig.effect("DistortionSoundEffect", rig.sound);
        distortion.level = 1.0f;
        distortion.priority = distortionFirst ? 0.0f : 2.0f;
        scene::SoundEffectComponent& filter = rig.effect("LowPassSoundEffect", rig.sound);
        filter.cutoff = 120.0f;
        filter.priority = 1.0f;
        (void)rig.hear(0.1);
        return rig.hear(0.4);
    };
    // Squared off and then filtered is a quiet rounded tone; filtered and then
    // squared off is a loud square one.
    const Heard thenFiltered = chain(true);
    const Heard thenSquared = chain(false);
    CHECK(thenSquared.rms > thenFiltered.rms * 1.5);
}

// ---------------------------------------------------------------------------
// Ogg Vorbis, from files a real encoder wrote (D546).

namespace {

// The tone was encoded at half scale and a `Sound` plays at half volume.
constexpr double Heard = 0.25;

struct VorbisFixture
{
    engine::asset::ContentMounts mounts;
    VorbisFixture() { mounts.mountDirectory(std::filesystem::path(ENG_AUDIO_TEST_DATA)); }
};

// The largest step between two samples next to each other, a channel.
[[nodiscard]] float largestStep(const std::vector<float>& samples, std::size_t first, std::size_t last)
{
    float largest = 0.0f;
    for (std::size_t at = first + 2; at < last; ++at)
        largest = std::max(largest, std::abs(samples[at] - samples[at - 2]));
    return largest;
}

} // namespace

TEST_CASE("D546: an Ogg Vorbis file decodes, and is as long as it says")
{
    // **The format the manual, the class and the warning itself all name.**
    // Its length was read from the stream's pages by hand (D094), with a test
    // made of a header and no audio; nothing ever decoded one. No Vorbis
    // decoder was compiled in: every `.ogg` played the placeholder tone.
    VorbisFixture content;
    Fixture fixture;
    fixture.system.setContentMounts(&content.mounts);
    const InstanceId id = fixture.make("Sound");
    fixture.sound(id).content = "asset://tone.ogg";
    fixture.sound(id).playing = true;
    fixture.system.tick(*fixture.world, Tick);
    fixture.system.update(*fixture.world, InstanceId{});
    CHECK(fixture.system.stats().clipsLoaded == 1);
    CHECK(fixture.system.stats().clipsMissing == 0);
    CHECK(fixture.system.stats().clipsStreamed == 0);
    CHECK(fixture.system.clipDuration("asset://tone.ogg") == doctest::Approx(1.0).epsilon(0.00001));

    // And it is the tone that was encoded: 375 Hz on the left, 750 Hz on the
    // right. Lossy, so near it and not it.
    const std::vector<float> heard = listen(content.mounts, "asset://tone.ogg", 0.25, 4800u);
    double worst = 0.0;
    for (std::size_t frame = 0; frame < 4800u; ++frame) {
        const double t = 0.25 + static_cast<double>(frame) / 48000.0;
        worst = std::max(worst, std::abs(static_cast<double>(heard[frame * 2]) -
                                         Heard * std::sin(2.0 * 3.14159265358979 * 375.0 * t)));
        worst = std::max(worst, std::abs(static_cast<double>(heard[frame * 2 + 1]) -
                                         Heard * std::sin(2.0 * 3.14159265358979 * 750.0 * t)));
    }
    MESSAGE("the decoded tone is within ", worst, " of the tone that was encoded");
    CHECK(worst < 0.03);

    // A file at another rate is resampled to the mixer's, and is a second too.
    Fixture other;
    other.system.setContentMounts(&content.mounts);
    const InstanceId slow = other.make("Sound");
    other.sound(slow).content = "asset://tone44.ogg";
    other.sound(slow).playing = true;
    other.system.tick(*other.world, Tick);
    other.system.update(*other.world, InstanceId{});
    CHECK(other.system.stats().clipsMissing == 0);
    CHECK(other.system.clipDuration("asset://tone44.ogg") == doctest::Approx(1.0).epsilon(0.0001));
    const std::vector<float> resampled = listen(content.mounts, "asset://tone44.ogg", 0.5, 4800u);
    CHECK(std::ranges::any_of(resampled, [](float sample) { return std::abs(sample) > 0.15f; }));
}

TEST_CASE("D546: a looped Ogg Vorbis sound wraps at the stream's own end, with no gap")
{
    // The tone is a whole number of periods long: where it ends it begins. A
    // loop that ran past the last sample the stream declares -- a codec pads
    // its last block -- or stopped short of it would put a step in the wave.
    VorbisFixture content;
    const std::vector<float> looped = listen(content.mounts, "asset://tone.ogg", 0.9, 9600u, true);
    const std::vector<float> head = listen(content.mounts, "asset://tone.ogg", 0.0, 4800u);
    // After the wrap, the file's first samples again, to the sample.
    CHECK(std::equal(head.begin(), head.end(), looped.begin() + 4800 * 2));
    // And no step across the seam larger than the tone's own: 750 Hz as it is
    // heard moves at most 0.025 a sample.
    CHECK(largestStep(looped, 4700u * 2u, 4900u * 2u) < 0.04f);
    CHECK(std::ranges::any_of(looped, [](float sample) { return std::abs(sample) > 0.15f; }));
}

TEST_CASE("D546: a long Ogg Vorbis file streams, seeks, and loops with no gap")
{
    VorbisFixture content;
    {
        Fixture fixture;
        fixture.system.setContentMounts(&content.mounts);
        const InstanceId id = fixture.make("Sound");
        fixture.sound(id).content = "asset://song.ogg";
        fixture.sound(id).playing = true;
        fixture.system.tick(*fixture.world, Tick);
        fixture.system.update(*fixture.world, InstanceId{});
        CHECK(fixture.system.stats().clipsLoaded == 1);
        CHECK(fixture.system.stats().clipsMissing == 0);
        CHECK(fixture.system.stats().clipsStreamed == 1);
        CHECK(fixture.system.clipDuration("asset://song.ogg") == doctest::Approx(12.0).epsilon(0.00001));
    }

    // Five seconds in is the tone at five seconds.
    const std::vector<float> seeked = listen(content.mounts, "asset://song.ogg", 5.0, 4800u);
    double worst = 0.0;
    for (std::size_t frame = 0; frame < 4800u; ++frame) {
        const double t = 5.0 + static_cast<double>(frame) / 48000.0;
        worst = std::max(worst, std::abs(static_cast<double>(seeked[frame * 2]) -
                                         Heard * std::sin(2.0 * 3.14159265358979 * 375.0 * t)));
    }
    CHECK(worst < 0.03);

    // Played once it ends at the stream's end, and is silent after it.
    const std::vector<float> once = listen(content.mounts, "asset://song.ogg", 11.9, 9600u);
    CHECK(std::all_of(once.begin() + 4800 * 2, once.end(), [](float sample) { return sample == 0.0f; }));
    CHECK(std::ranges::any_of(once, [](float sample) { return std::abs(sample) > 0.15f; }));

    // Looped it goes round: the head again right after the tail, no step.
    const std::vector<float> looped = listen(content.mounts, "asset://song.ogg", 11.9, 9600u, true);
    const std::vector<float> head = listen(content.mounts, "asset://song.ogg", 0.0, 4800u);
    CHECK(std::equal(head.begin(), head.end(), looped.begin() + 4800 * 2));
    CHECK(largestStep(looped, 4700u * 2u, 4900u * 2u) < 0.04f);
}

// --- A sound in the player's language (ADR 0200) ---------------------------------

namespace {

// A tone of a pitch and a level of the test's choosing, 48 kHz unless said.
void writeSine(const std::filesystem::path& path, double seconds, double hertz, double level = 0.6,
               engine::core::u32 rate = 48000u)
{
    std::vector<char> bytes;
    const auto put = [&bytes](const void* data, std::size_t size) {
        const auto* const at = static_cast<const char*>(data);
        bytes.insert(bytes.end(), at, at + size);
    };
    const auto putU32 = [&put](engine::core::u32 value) { put(&value, sizeof(value)); };
    const auto putU16 = [&put](engine::core::u16 value) { put(&value, sizeof(value)); };
    const auto frames = static_cast<engine::core::u32>(std::llround(seconds * static_cast<double>(rate)));
    const engine::core::u32 dataBytes = frames * 2u;
    put("RIFF", 4);
    putU32(36u + dataBytes);
    put("WAVE", 4);
    put("fmt ", 4);
    putU32(16u);
    putU16(1u);
    putU16(1u);
    putU32(rate);
    putU32(rate * 2u);
    putU16(2u);
    putU16(16u);
    put("data", 4);
    putU32(dataBytes);
    for (engine::core::u32 frame = 0; frame < frames; ++frame) {
        const double phase = 2.0 * 3.14159265358979 * hertz * static_cast<double>(frame) / static_cast<double>(rate);
        const auto sample = static_cast<engine::core::i16>(32767.0 * level * std::sin(phase));
        putU16(static_cast<engine::core::u16>(sample));
    }
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A project's content with one line in three languages of three lengths --
// half a second in the default, eight tenths in Portuguese, three tenths in
// Japanese -- and one line recorded in the default alone.
struct VoiceFixture
{
    std::filesystem::path root;
    engine::asset::ContentMounts mounts;
    engine::asset::LocalizationIndex index;

    explicit VoiceFixture(const char* folder = "engine-audio-voices")
    {
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec) / folder;
        std::filesystem::remove_all(root, ec);
        writeSine(root / "voice" / "line.wav", 0.5, 440.0);
        writeSine(root / "l10n" / "pt-BR" / "voice" / "line.wav", 0.8, 440.0);
        writeSine(root / "l10n" / "ja" / "voice" / "line.wav", 0.3, 440.0);
        writeSine(root / "voice" / "default_only.wav", 0.5, 440.0);
        mounts.mountDirectory(root);
        engine::asset::scanLocalizedContent(root, index);
    }

    ~VoiceFixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    void serve(Fixture& fixture, const char* voice)
    {
        fixture.system.setContentMounts(&mounts);
        fixture.system.setLocalized(&index);
        fixture.system.setVoiceLocale(voice);
    }
};

[[nodiscard]] InstanceId playing(Fixture& fixture, const char* content, int category = 0)
{
    const InstanceId id = fixture.make("Sound");
    fixture.sound(id).content = content;
    fixture.sound(id).category = category;
    fixture.sound(id).playing = true;
    return id;
}

// The loudest sample of a rendered stretch.
[[nodiscard]] float loudest(Fixture& fixture, std::size_t frames)
{
    std::vector<float> out(frames * 2u);
    fixture.system.renderInto(out);
    float most = 0.0f;
    for (const float sample : out)
        most = std::max(most, std::abs(sample));
    return most;
}

// How many ticks a sound plays for before it says it ended.
[[nodiscard]] int ticksUntilEnded(Fixture& fixture, InstanceId id)
{
    int ticks = 0;
    while (fixture.sound(id).playing && ticks < 600) {
        fixture.system.tick(*fixture.world, Tick);
        ++ticks;
    }
    return ticks;
}

} // namespace

TEST_CASE("ADR 0200: a sound is heard in the voice language, and in the default one where that has no file")
{
    VoiceFixture voices;
    Fixture fixture;
    voices.serve(fixture, "pt-BR");
    REQUIRE(voices.index.voice == std::vector<std::string>{"ja", "pt-BR"});

    const InstanceId line = playing(fixture, "asset://voice/line.wav");
    const InstanceId plain = playing(fixture, "asset://voice/default_only.wav");
    fixture.system.update(*fixture.world, InstanceId{});
    // The game named one file, and what is heard is the language's.
    CHECK(fixture.system.heardAs(*fixture.world, line) == "asset://l10n/pt-BR/voice/line.wav");
    // A line nobody recorded in it is the default language's, never silence.
    CHECK(fixture.system.heardAs(*fixture.world, plain) == "asset://voice/default_only.wav");
    CHECK(loudest(fixture, 480u) > 0.2f);

    // A language the game has nothing in hears the files as written.
    Fixture other;
    voices.serve(other, "de");
    const InstanceId same = playing(other, "asset://voice/line.wav");
    other.system.update(*other.world, InstanceId{});
    CHECK(other.system.heardAs(*other.world, same) == "asset://voice/line.wav");
}

TEST_CASE("ADR 0200: a localized sound lasts as long as its longest language, whatever language is heard")
{
    // R10: `TimeLength` and the tick `Ended` fires on are in the world's
    // hash, and two machines in a match hear different languages. The
    // default is half a second, Portuguese eight tenths, Japanese three.
    VoiceFixture voices;
    int ticks[3] = {0, 0, 0};
    const char* languages[3] = {"", "pt-BR", "ja"};
    for (int index = 0; index < 3; ++index) {
        Fixture fixture;
        voices.serve(fixture, languages[index]);
        CHECK(fixture.system.clipDuration("asset://voice/line.wav") == doctest::Approx(0.8).epsilon(0.0001));
        const InstanceId id = playing(fixture, "asset://voice/line.wav");
        fixture.system.tick(*fixture.world, Tick);
        CHECK(fixture.sound(id).timeLength == doctest::Approx(0.8).epsilon(0.0001));
        ticks[index] = 1 + ticksUntilEnded(fixture, id);
        // A line with one language is as long as its file, as it always was.
        CHECK(fixture.system.clipDuration("asset://voice/default_only.wav") == doctest::Approx(0.5).epsilon(0.0001));
    }
    CHECK(ticks[0] >= 48);
    CHECK(ticks[0] <= 49);
    CHECK(ticks[1] == ticks[0]);
    CHECK(ticks[2] == ticks[0]);
}

TEST_CASE("ADR 0200: a machine without a language's files has the long length from the export's index")
{
    // A dedicated server, or a player who did not install a language pack:
    // the default's half second is all there is to measure, and the line is
    // eight tenths long all the same.
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::temp_directory_path(ec) / "engine-audio-index-only";
    std::filesystem::remove_all(root, ec);
    writeSine(root / "voice" / "line.wav", 0.5, 440.0);
    engine::asset::ContentMounts mounts;
    mounts.mountDirectory(root);
    engine::asset::LocalizationIndex index;
    REQUIRE(engine::asset::readLocalizationIndex(
        R"({ "version": 1, "voice": ["pt-BR"], "shipped": [], "lengths": { "voice/line.wav": 38400 }, "lines": [] })",
        index));

    Fixture fixture;
    fixture.system.setContentMounts(&mounts);
    fixture.system.setLocalized(&index);
    fixture.system.setVoiceLocale("pt-BR");
    CHECK(fixture.system.clipDuration("asset://voice/line.wav") == doctest::Approx(0.8).epsilon(0.0001));
    const InstanceId id = playing(fixture, "asset://voice/line.wav");
    fixture.system.tick(*fixture.world, Tick);
    const int ticks = 1 + ticksUntilEnded(fixture, id);
    CHECK(ticks >= 48);
    CHECK(ticks <= 49);
    // And it is heard in what is here: the default's recording.
    fixture.sound(id).playing = true;
    fixture.system.update(*fixture.world, InstanceId{});
    CHECK(fixture.system.heardAs(*fixture.world, id) == "asset://voice/line.wav");
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("ADR 0200: a shorter language is followed by silence until the line ends, and a loop turns at the slot")
{
    VoiceFixture voices;
    // Japanese is three tenths of a second inside a slot of eight.
    {
        Fixture fixture;
        voices.serve(fixture, "ja");
        const InstanceId id = playing(fixture, "asset://voice/line.wav");
        fixture.sound(id).timePosition = 0.1;
        fixture.system.update(*fixture.world, InstanceId{});
        CHECK(loudest(fixture, 480u) > 0.2f);
    }
    {
        Fixture fixture;
        voices.serve(fixture, "ja");
        const InstanceId id = playing(fixture, "asset://voice/line.wav");
        fixture.sound(id).timePosition = 0.5;
        fixture.system.update(*fixture.world, InstanceId{});
        CHECK(loudest(fixture, 480u) == 0.0f);
    }
    {
        // Looped, from just before the slot's end: silence, then the top of
        // the recording again -- at eight tenths, not at three.
        Fixture fixture;
        voices.serve(fixture, "ja");
        const InstanceId id = playing(fixture, "asset://voice/line.wav");
        fixture.sound(id).looped = true;
        fixture.sound(id).timePosition = 0.79;
        fixture.system.update(*fixture.world, InstanceId{});
        // Ten milliseconds of silence to the slot's end...
        CHECK(loudest(fixture, 470u) == 0.0f);
        // ...and then it is playing.
        CHECK(loudest(fixture, 960u) > 0.2f);
    }
}

TEST_CASE("ADR 0200: a language changed while a line plays leaves the line, and the next Play takes it")
{
    VoiceFixture voices;
    Fixture fixture;
    voices.serve(fixture, "pt-BR");
    const InstanceId id = playing(fixture, "asset://voice/line.wav");
    fixture.system.update(*fixture.world, InstanceId{});
    REQUIRE(fixture.system.heardAs(*fixture.world, id) == "asset://l10n/pt-BR/voice/line.wav");

    fixture.system.setVoiceLocale("ja");
    fixture.system.update(*fixture.world, InstanceId{});
    CHECK(fixture.system.heardAs(*fixture.world, id) == "asset://l10n/pt-BR/voice/line.wav");

    // `Play` again, as a script's call counts it.
    fixture.sound(id).plays += 1;
    fixture.sound(id).timePosition = 0.0;
    fixture.system.update(*fixture.world, InstanceId{});
    CHECK(fixture.system.heardAs(*fixture.world, id) == "asset://l10n/ja/voice/line.wav");
}

TEST_CASE("ADR 0200: the player's volumes are by category, and a muted voice is still being said")
{
    VoiceFixture voices;
    Fixture fixture;
    voices.serve(fixture, "");
    const InstanceId voice = playing(fixture, "asset://voice/line.wav", 2);
    fixture.sound(voice).volume = 1.0f;

    fixture.system.update(*fixture.world, InstanceId{});
    const float full = loudest(fixture, 480u);
    REQUIRE(full > 0.3f);

    scene::PlayerSound& player = fixture.world->engineState().playerSound;
    // The effects' volume is not a voice's.
    player.effectsVolume = 0.0f;
    fixture.sound(voice).timePosition = 0.0;
    fixture.system.update(*fixture.world, InstanceId{});
    CHECK(loudest(fixture, 480u) == doctest::Approx(static_cast<double>(full)).epsilon(0.05));

    player.voiceVolume = 0.5f;
    player.playerVolume = 0.5f;
    fixture.sound(voice).timePosition = 0.0;
    fixture.system.update(*fixture.world, InstanceId{});
    CHECK(loudest(fixture, 480u) == doctest::Approx(static_cast<double>(full * 0.25f)).epsilon(0.05));

    // Turned off, nothing sounds -- and it can still be heard here, which
    // is what a caption is current by.
    player.voiceVolume = 0.0f;
    fixture.sound(voice).timePosition = 0.0;
    fixture.system.update(*fixture.world, InstanceId{});
    CHECK(loudest(fixture, 480u) == 0.0f);
    REQUIRE(fixture.system.heardNow().size() == 1);
    CHECK(fixture.system.heardNow()[0] == voice);
}

TEST_CASE("ADR 0200: music lowers itself under a voice and comes back, and a voice turned off lowers nothing")
{
    VoiceFixture voices;
    Fixture fixture;
    voices.serve(fixture, "");
    const InstanceId music = playing(fixture, "asset://voice/default_only.wav", 1);
    fixture.sound(music).looped = true;
    fixture.system.update(*fixture.world, InstanceId{});
    (void)loudest(fixture, 4800u);
    CHECK(fixture.system.musicDuck() == doctest::Approx(1.0));

    const InstanceId voice = playing(fixture, "asset://voice/line.wav", 2);
    fixture.sound(voice).looped = true;
    fixture.system.update(*fixture.world, InstanceId{});
    // A quarter of a second of audio: down, in well under that.
    for (int block = 0; block < 25; ++block)
        (void)loudest(fixture, 480u);
    CHECK(static_cast<double>(fixture.system.musicDuck()) == doctest::Approx(0.4).epsilon(0.03));

    // The game's own number is what it is lowered to.
    fixture.world->engineState().musicUnderVoice = 0.7f;
    fixture.system.update(*fixture.world, InstanceId{});
    for (int block = 0; block < 100; ++block)
        (void)loudest(fixture, 480u);
    CHECK(static_cast<double>(fixture.system.musicDuck()) == doctest::Approx(0.7).epsilon(0.03));

    // The voice stops: back, in about six tenths.
    fixture.sound(voice).playing = false;
    fixture.system.update(*fixture.world, InstanceId{});
    for (int block = 0; block < 20; ++block)
        (void)loudest(fixture, 480u);
    CHECK(fixture.system.musicDuck() < 0.95f);
    for (int block = 0; block < 100; ++block)
        (void)loudest(fixture, 480u);
    CHECK(static_cast<double>(fixture.system.musicDuck()) == doctest::Approx(1.0).epsilon(0.01));

    // A voice the player turned off is not something music makes room for.
    fixture.world->engineState().playerSound.voiceVolume = 0.0f;
    fixture.sound(voice).playing = true;
    fixture.system.update(*fixture.world, InstanceId{});
    for (int block = 0; block < 50; ++block)
        (void)loudest(fixture, 480u);
    CHECK(static_cast<double>(fixture.system.musicDuck()) == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("ADR 0200: a line with a caption and no recording is silent and lasts its caption")
{
    VoiceFixture voices;
    Fixture fixture;
    voices.serve(fixture, "");

    // With no caption a missing file is the tone, one second of it.
    const InstanceId bare = playing(fixture, "asset://voice/not_recorded.wav");
    fixture.system.tick(*fixture.world, Tick);
    CHECK(fixture.sound(bare).timeLength == doctest::Approx(1.0));
    fixture.system.update(*fixture.world, InstanceId{});
    CHECK(loudest(fixture, 480u) > 0.05f);
    fixture.sound(bare).playing = false;

    const InstanceId line = playing(fixture, "asset://voice/not_recorded.wav", 2);
    const InstanceId caption = fixture.make("Caption");
    REQUIRE_FALSE(fixture.world->setParent(caption, line).has_value());
    // Eleven characters and no catalog: the key is the text. A second and a
    // half, and six hundredths a character.
    fixture.world->captions().find(caption)->text = fixture.atoms.intern("hello there");
    fixture.system.tick(*fixture.world, Tick);
    CHECK(fixture.sound(line).timeLength == doctest::Approx(1.5 + 11 * 0.06).epsilon(0.001));
    fixture.system.update(*fixture.world, InstanceId{});
    // No tone: a line that is only text makes no sound, and music is not
    // lowered for it.
    CHECK(loudest(fixture, 4800u) == 0.0f);
    CHECK(fixture.system.musicDuck() == doctest::Approx(1.0));
    // It is being said all the same.
    REQUIRE(fixture.system.heardNow().size() == 1);
    CHECK(fixture.system.heardNow()[0] == line);

    // The author's own number, when they gave one.
    fixture.world->captions().find(caption)->seconds = 3.0f;
    fixture.system.tick(*fixture.world, Tick);
    CHECK(fixture.sound(line).timeLength == doctest::Approx(3.0));
}

TEST_CASE("ADR 0200: loudness and three bands of what a machine is playing, worked out only when asked")
{
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::temp_directory_path(ec) / "engine-audio-bands";
    std::filesystem::remove_all(root, ec);
    writeSine(root / "low.wav", 1.0, 200.0, 1.0);
    writeSine(root / "mid.wav", 1.0, 1000.0, 1.0);
    writeSine(root / "high.wav", 1.0, 5000.0, 1.0);
    writeSine(root / "quiet.wav", 1.0, 1000.0, 0.25);
    engine::asset::ContentMounts mounts;
    mounts.mountDirectory(root);

    Fixture fixture;
    fixture.system.setContentMounts(&mounts);
    const InstanceId low = playing(fixture, "asset://low.wav");
    const InstanceId mid = playing(fixture, "asset://mid.wav", 1);
    const InstanceId high = playing(fixture, "asset://high.wav");
    const InstanceId quiet = playing(fixture, "asset://quiet.wav");
    const InstanceId still = fixture.make("Sound");
    fixture.sound(still).content = "asset://mid.wav";
    for (const InstanceId id : {low, mid, high, quiet})
        fixture.sound(id).timePosition = 0.5;
    fixture.system.update(*fixture.world, InstanceId{});

    // **A sound nobody asked about cost nothing.**
    CHECK(fixture.system.meterWork() == 0);

    // A tone at full scale reads one, and a quarter of it a quarter: the
    // recording's own level, with no volume in it.
    fixture.sound(low).volume = 0.1f;
    CHECK(static_cast<double>(fixture.system.loudness(*fixture.world, low)) == doctest::Approx(1.0).epsilon(0.03));
    CHECK(static_cast<double>(fixture.system.loudness(*fixture.world, quiet)) == doctest::Approx(0.25).epsilon(0.03));
    CHECK(fixture.system.loudness(*fixture.world, still) == 0.0f);
    CHECK(fixture.system.meterWork() == 2);
    // Asked again the same frame, it is the answer it had.
    CHECK(static_cast<double>(fixture.system.loudness(*fixture.world, low)) == doctest::Approx(1.0).epsilon(0.03));
    CHECK(fixture.system.meterWork() == 2);

    const std::array<float, 3> lows = fixture.system.bands(*fixture.world, low);
    const std::array<float, 3> mids = fixture.system.bands(*fixture.world, mid);
    const std::array<float, 3> highs = fixture.system.bands(*fixture.world, high);
    CHECK(lows[0] > 0.9f);
    CHECK(lows[1] < 0.1f);
    CHECK(lows[2] < 0.1f);
    CHECK(mids[0] < 0.1f);
    CHECK(mids[1] > 0.9f);
    CHECK(mids[2] < 0.1f);
    CHECK(highs[0] < 0.1f);
    CHECK(highs[1] < 0.1f);
    CHECK(highs[2] > 0.9f);
    const std::array<float, 3> silence = fixture.system.bands(*fixture.world, still);
    CHECK(silence[0] + silence[1] + silence[2] == 0.0f);

    // A category is what the game mixes of it: the music here is the mid tone.
    const std::array<float, 3> music = fixture.system.categoryBands(*fixture.world, 1);
    CHECK(music[1] > 0.4f);
    CHECK(music[0] < 0.1f);
    const std::array<float, 3> nobody = fixture.system.categoryBands(*fixture.world, 2);
    CHECK(nobody[0] + nobody[1] + nobody[2] == 0.0f);
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("D620: a sound recorded again is heard as it is now, without the world being made anew")
{
    // The cache of what was read lived as long as the audio system, and hot
    // reload forgot meshes, materials and skeletons and never a sound: a
    // line recorded again was the old line until the game was restarted.
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::temp_directory_path(ec) / "engine-audio-again";
    std::filesystem::remove_all(root, ec);
    writeSine(root / "line.wav", 0.5, 440.0);
    engine::asset::ContentMounts mounts;
    mounts.mountDirectory(root);

    Fixture fixture;
    fixture.system.setContentMounts(&mounts);
    const InstanceId id = playing(fixture, "asset://line.wav");
    fixture.system.tick(*fixture.world, Tick);
    fixture.system.update(*fixture.world, InstanceId{});
    REQUIRE(fixture.system.clipDuration("asset://line.wav") == doctest::Approx(0.5).epsilon(0.0001));

    writeSine(root / "line.wav", 1.25, 440.0);
    // Until it is told, it has what it read.
    CHECK(fixture.system.clipDuration("asset://line.wav") == doctest::Approx(0.5).epsilon(0.0001));
    const std::vector<std::string> changed{"asset://line.wav"};
    fixture.system.forget(changed);
    CHECK(fixture.system.clipDuration("asset://line.wav") == doctest::Approx(1.25).epsilon(0.0001));
    // And the sound that was playing it goes on, in the new recording.
    fixture.sound(id).timePosition = 1.0;
    fixture.system.update(*fixture.world, InstanceId{});
    CHECK(loudest(fixture, 480u) > 0.2f);
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("D621: a sound made to be played once is gone a couple of ticks after it ends")
{
    // `PlayLocal` parented a sound to the service and left it there for
    // ever: a click a frame was a thousand instances a quarter of a minute.
    Fixture fixture;
    ContentFixture content;
    fixture.system.setContentMounts(&content.mounts);
    const InstanceId kept = playing(fixture, "asset://sfx/tone.wav");
    const InstanceId once = playing(fixture, "asset://sfx/tone.wav");
    fixture.sound(once).ownsItself = true;

    // A quarter of a second, and the two ticks it is kept for whoever
    // listens to `Ended`.
    fixture.run(16);
    REQUIRE_FALSE(fixture.sound(once).playing);
    CHECK_FALSE(fixture.world->destroyed(once));
    fixture.run(scene::SoundLinger + 1);
    // Destroyed: it resolves until the drain that carries its `Destroying`.
    CHECK(fixture.world->destroyed(once));
    // One that nobody said was to be played once stays.
    CHECK_FALSE(fixture.world->destroyed(kept));
}
