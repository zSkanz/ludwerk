// Sequences being played (ADR 0204): what a tick does with each kind of
// track, what a seek, a stop and a skip leave behind, what a machine that
// does not decide the world does and does not do, and what the frame is told.
//
// The tick's side is driven here with a counting stand-in for the animation,
// so what a sequence ASKS of a clip is read exactly; the frame's shape keys go
// through the real one, whose layer they are drawn from.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <doctest/doctest.h>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/app/sequences.h"
#include "engine/app/timeline.h"
#include "engine/audio/scene_types.h"
#include "engine/render/animation.h"
#include "engine/render/scene_types.h"
#include "engine/scene/change_queue.h"
#include "engine/scene/localization.h"
#include "engine/scene/morph_host.h"
#include "engine/scene/scene_file.h"
#include "engine/scene/world.h"

using namespace engine;

namespace {

constexpr double Frame = 1.0 / 60.0;

// What a sequence asked of the animation, in order.
struct Asked
{
    std::string what;
    scene::TrackId track = 0;
    std::string text{};
    float number = 0.0f;
};

class CountingAnimation final : public scene::AnimationHost
{
public:
    std::vector<Asked> asked;
    // Each track made: playing, and how long its clip is said to be.
    struct Made
    {
        core::InstanceId player;
        std::string file;
        std::string clip;
        bool playing = false;
        bool over = false;
        float length = 0.4f;
        double time = 0.0;
        float speed = 1.0f;
    };
    std::vector<Made> made{Made{}};

    scene::GraphWrite setGraphParameter(core::InstanceId, std::string_view name, core::f32 value) override
    {
        asked.push_back(Asked{"parameter", 0, std::string(name), value});
        return scene::GraphWrite::Done;
    }
    scene::TrackId createTrack(core::InstanceId player, core::NameAtom content, std::string_view clip) override
    {
        made.push_back(Made{player, std::string(atoms->text(content)), std::string(clip)});
        asked.push_back(Asked{"create", static_cast<scene::TrackId>(made.size() - 1), std::string(clip)});
        return static_cast<scene::TrackId>(made.size() - 1);
    }
    void play(scene::TrackId track, core::f32 fadeTime, core::f32, core::f32 speed) override
    {
        made[track].playing = true;
        made[track].time = 0.0;
        made[track].speed = speed;
        asked.push_back(Asked{"play", track, {}, fadeTime});
    }
    void stop(scene::TrackId track, core::f32 fadeTime) override
    {
        made[track].playing = false;
        asked.push_back(Asked{"stop", track, {}, fadeTime});
    }
    void adjustWeight(scene::TrackId, core::f32, core::f32) override {}
    void adjustSpeed(scene::TrackId track, core::f32 speed) override
    {
        made[track].speed = speed;
        asked.push_back(Asked{"speed", track, {}, speed});
    }
    void setLooped(scene::TrackId, bool) override {}
    void setOver(scene::TrackId track, bool over) override { made[track].over = over; }
    void seek(scene::TrackId track, core::f64 seconds) override
    {
        made[track].time = seconds;
        asked.push_back(Asked{"seek", track, {}, static_cast<float>(seconds)});
    }
    [[nodiscard]] scene::TrackState state(scene::TrackId track) const override
    {
        scene::TrackState out;
        out.playing = made[track].playing;
        out.length = made[track].length;
        out.timePosition = made[track].time;
        return out;
    }
    void sample(core::f64 fixedDt) override
    {
        for (Made& one : made) {
            if (!one.playing)
                continue;
            one.time += fixedDt * static_cast<double>(one.speed);
            if (one.time >= static_cast<double>(one.length))
                one.playing = false;
        }
    }
    [[nodiscard]] std::span<const scene::TrackId> drainEnded() override { return {}; }
    void retire(const scene::World&) override {}
    scene::GraphWrite clearGraphParameter(core::InstanceId, std::string_view name) override
    {
        asked.push_back(Asked{"clear", 0, std::string(name)});
        return scene::GraphWrite::Done;
    }
    void rest() override
    {
        for (Made& one : made)
            one.playing = false;
        asked.push_back(Asked{"rest"});
    }

    const core::AtomTable* atoms = nullptr;

    [[nodiscard]] std::vector<std::string> said() const
    {
        std::vector<std::string> out;
        for (const Asked& one : asked)
            out.push_back(one.what + (one.text.empty() ? "" : ":" + one.text));
        return out;
    }
};

class CountingMorphs final : public scene::MorphHost
{
public:
    std::map<std::string, float> held;
    std::vector<std::string> cleared;
    [[nodiscard]] core::u32 morphTargetCount(core::InstanceId) const override { return 0; }
    [[nodiscard]] std::string_view morphTargetName(core::InstanceId, core::u32) const override { return {}; }
    [[nodiscard]] core::f32 morphWeight(core::InstanceId, std::string_view) const override { return 0.0f; }
    void setMorphWeight(core::InstanceId, std::string_view name, core::f32 weight) override
    {
        held[std::string(name)] = weight;
    }
    void clearMorphWeight(core::InstanceId, std::string_view name) override
    {
        held.erase(std::string(name));
        cleared.emplace_back(name);
    }
};

// What a script would have heard.
struct Heard
{
    std::string event;
    std::string name;
    bool skipped = false;
    double number = 0.0;
    std::string text;
};

struct Stage
{
    core::AtomTable atoms;
    scene::ClassRegistry classes;
    scene::EnumRegistry enums;
    std::optional<scene::World> world;
    scene::DialogueLines lines;
    std::map<std::string, std::string, std::less<>> files;
    CountingAnimation animation;
    CountingMorphs morphs;
    app::Sequences sequences;
    core::InstanceId workspace;
    core::InstanceId player;
    core::u64 serverTick = 1;
    bool authority = true;

    Stage()
    {
        scene::generated::registerClasses(classes, atoms);
        audio::registerSceneTypes(classes, atoms);
        render::registerSceneTypes(classes, atoms);
        scene::generated::registerEnums(enums, atoms);
        world.emplace(classes, enums, atoms, 1u);
        world->setDialogue(&lines);
        animation.atoms = &atoms;
        sequences.setSources(app::Sequences::Sources{
            .read = [this](std::string_view urn) -> std::optional<std::string> {
                const auto found = files.find(urn);
                return found == files.end() ? std::nullopt : std::optional<std::string>{found->second};
            },
            // Every line takes two seconds to say.
            .lineSeconds = [](std::string_view, std::string_view, core::f32) { return 2.0; },
        });
        workspace = make("Folder", {}, "Stage");
    }

    core::InstanceId make(const char* className, core::InstanceId parent = {}, const char* name = nullptr)
    {
        const scene::ClassId id = classes.findId(atoms.intern(className));
        REQUIRE(id != scene::InvalidClass);
        const core::InstanceId made = world->create(id);
        if (name != nullptr)
            world->setName(made, atoms.intern(name));
        if (parent.valid())
            REQUIRE_FALSE(world->setParent(made, parent).has_value());
        return made;
    }

    // A player of `file`, whose text is `json`.
    scene::SequencePlayerComponent& playerOf(const char* file, const char* json)
    {
        files[file] = json;
        player = make("SequencePlayer", workspace, "Cutscene");
        scene::SequencePlayerComponent* self = world->sequencePlayers().find(player);
        REQUIRE(self != nullptr);
        self->sequence = atoms.intern(file);
        return *self;
    }

    void bind(const char* name, core::InstanceId target)
    {
        const core::InstanceId binding = make("SequenceBinding", player, name);
        world->sequenceBindings().find(binding)->target = target;
    }

    void tick(int count = 1)
    {
        for (int index = 0; index < count; ++index) {
            sequences.step(app::Sequences::Tick{
                .world = &*world,
                .animation = &animation,
                .morphs = &morphs,
                .workspace = workspace,
                .authority = authority,
                .serverTick = serverTick,
                .fixedDt = Frame,
            });
            animation.sample(Frame);
            ++serverTick;
        }
    }

    // The events raised since the last ask, in order.
    std::vector<Heard> heard()
    {
        std::vector<Heard> out;
        for (const scene::Change& change : world->changes().take()) {
            if (change.kind == scene::ChangeKind::InstanceEventNote) {
                const scene::EventNote* note = world->changes().drainedNote(change);
                REQUIRE(note != nullptr);
                out.push_back(Heard{std::string(atoms.text(change.name)), note->name, note->skipped,
                                    note->kind == scene::EventNote::Kind::Number ? note->numbers[0] : 0.0, note->text});
            }
            else if (change.kind == scene::ChangeKind::InstanceEventBool) {
                Heard ended;
                ended.event = std::string(atoms.text(change.name));
                ended.skipped = change.other.index != 0;
                out.push_back(std::move(ended));
            }
            else if (change.kind == scene::ChangeKind::InstanceEventText) {
                // A hold's and a marker's: the event, and the name it carries.
                Heard named;
                named.event = std::string(atoms.text(change.name));
                named.name = std::string(world->changes().drainedText(change));
                out.push_back(std::move(named));
            }
        }
        return out;
    }

    [[nodiscard]] double number(core::InstanceId id, const char* property) const
    {
        const std::optional<scene::Value> value = world->getProperty(id, atoms.lookup(property));
        REQUIRE(value.has_value());
        return std::get<double>(*value);
    }
};

constexpr const char* LampFile = "asset://sequences/lamp.sequence.json";
constexpr const char* Lamp = R"({
  "format": "sequence", "version": 1, "bindings": ["Lamp"],
  "tracks": [
    { "type": "property", "target": "Lamp/Light", "property": "Brightness", "keys": [
        { "time": 0.5, "value": 0 }, { "time": 1, "value": 3 } ] },
    { "type": "event", "keys": [
        { "time": 0, "name": "Begin" }, { "time": 0.75, "name": "Half", "data": 7 },
        { "time": 1.5, "name": "End", "data": "warm" } ] }
  ]
})";

// A lamp, bound: a part with a light under it, burning at two.
struct LampStage : Stage
{
    core::InstanceId light;
    scene::SequencePlayerComponent* self = nullptr;

    LampStage()
    {
        self = &playerOf(LampFile, Lamp);
        const core::InstanceId lamp = make("Part", workspace, "Lamp");
        light = make("PointLight", lamp, "Light");
        REQUIRE(world->setProperty(light, atoms.intern("Brightness"), scene::Value{2.0}) !=
                scene::World::SetResult::InvalidValue);
        bind("Lamp", lamp);
        (void)heard();
    }
};

} // namespace

TEST_CASE("ADR 0204: a keyed property is the sequence's from its first key to its last, on the tick's clock")
{
    LampStage stage;
    stage.self->playing = true;
    // A quarter of a second in, before the first key: the lamp is the game's.
    stage.tick(15);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(2.0));
    // Three quarters: half way from the first key to the second.
    stage.tick(30);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(1.5).epsilon(0.1));
    CHECK(stage.self->timePosition == doctest::Approx(0.75));
    stage.tick(20);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(3.0));
    // Past its last key it is the game's again: what a script writes stays.
    (void)stage.world->setProperty(stage.light, stage.atoms.intern("Brightness"), scene::Value{5.0});
    stage.tick(10);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(5.0));
    CHECK(stage.self->playing);
    CHECK(static_cast<double>(stage.self->length) == doctest::Approx(1.5));

    // And it ends by itself, having said each event once, in order.
    stage.tick(30);
    CHECK_FALSE(stage.self->playing);
    CHECK(stage.self->timePosition == doctest::Approx(1.5));
    const std::vector<Heard> heard = stage.heard();
    REQUIRE(heard.size() == 4);
    CHECK(heard[0].name == "Begin");
    CHECK(heard[1].name == "Half");
    CHECK(heard[1].number == doctest::Approx(7.0));
    CHECK_FALSE(heard[1].skipped);
    CHECK(heard[2].name == "End");
    CHECK(heard[2].text == "warm");
    CHECK(heard[3].event == "Finished");
    CHECK_FALSE(heard[3].skipped);
    CHECK_FALSE(stage.sequences.camera(*stage.world, 0.0).has_value());

    // Played again, it starts from the start (what `Play` does at the end).
    stage.self->timePosition = 0.0;
    stage.self->seeks += 1;
    stage.self->playing = true;
    stage.tick(2);
    const std::vector<Heard> again = stage.heard();
    REQUIRE(again.size() == 1);
    CHECK(again[0].name == "Begin");
}

TEST_CASE("ADR 0204: a track whose property cannot be written is counted, and the others are written")
{
    // The lamp's sequence with two tracks more: a name the light has not got,
    // and a colour keyed as one number.
    LampStage stage;
    stage.files[LampFile] = R"json({
        "format": "sequence", "version": 1, "bindings": ["Lamp"],
        "tracks": [
            { "type": "property", "target": "Lamp/Light", "property": "Brightness", "keys": [
                { "time": 0, "value": 0 }, { "time": 1, "value": 3 } ] },
            { "type": "property", "target": "Lamp/Light", "property": "Brightnes", "keys": [
                { "time": 0, "value": 0 }, { "time": 1, "value": 3 } ] },
            { "type": "property", "target": "Lamp/Light", "property": "Color", "keys": [
                { "time": 0, "value": 0 }, { "time": 1, "value": 1 } ] }
        ]
    })json";
    stage.self->playing = true;
    stage.tick(30);
    CHECK(stage.sequences.stats().refused == 2);
    CHECK(stage.sequences.stats().written == 1);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(1.5).epsilon(0.1));
}

TEST_CASE("ADR 0204: a skip lands the world where the sequence would have left it, and says so")
{
    LampStage stage;
    stage.self->playing = true;
    stage.tick(10);
    (void)stage.heard();
    stage.self->skips += 1;
    stage.tick();
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(3.0));
    CHECK_FALSE(stage.self->playing);
    CHECK(stage.self->timePosition == doctest::Approx(1.5));
    const std::vector<Heard> heard = stage.heard();
    REQUIRE(heard.size() == 3);
    CHECK(heard[0].name == "Half");
    CHECK(heard[0].skipped);
    CHECK(heard[1].name == "End");
    CHECK(heard[1].skipped);
    CHECK(heard[2].event == "Finished");
    CHECK(heard[2].skipped);
    // Once: nothing more is heard of it.
    stage.tick(5);
    CHECK(stage.heard().empty());

    // One that was never played is skipped the same way: the whole of it.
    LampStage unseen;
    unseen.self->skips += 1;
    unseen.tick();
    CHECK(unseen.number(unseen.light, "Brightness") == doctest::Approx(3.0));
    const std::vector<Heard> all = unseen.heard();
    REQUIRE(all.size() == 4);
    CHECK(all[0].name == "Begin");
    CHECK(all[0].skipped);
    CHECK(all[3].event == "Finished");
}

TEST_CASE("ADR 0204: a stop leaves the world as it is, and nothing is finished")
{
    LampStage stage;
    stage.self->playing = true;
    stage.tick(45);
    (void)stage.heard();
    const double reached = stage.number(stage.light, "Brightness");
    CHECK(reached > 1.0);
    CHECK(reached < 2.0);
    stage.self->playing = false;
    stage.self->stops += 1;
    stage.tick(3);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(reached));
    CHECK(stage.self->timePosition == 0.0);
    CHECK(stage.heard().empty());
}

TEST_CASE("ADR 0204: a seek puts every track where it is at that moment and fires nothing")
{
    LampStage stage;
    // Held still at a second: the lamp is where its track is, and no event
    // has been said.
    stage.self->timePosition = 1.0;
    stage.self->seeks += 1;
    stage.tick();
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(3.0));
    CHECK(stage.self->timePosition == doctest::Approx(1.0));
    CHECK(stage.heard().empty());

    // Played from there, what is still to come comes, and nothing before it.
    stage.self->playing = true;
    stage.tick(40);
    const std::vector<Heard> heard = stage.heard();
    REQUIRE(heard.size() == 2);
    CHECK(heard[0].name == "End");
    CHECK(heard[1].event == "Finished");

    // Back before the first key, the lamp is left as it is: the track has
    // nothing to say about that moment.
    stage.self->timePosition = 0.25;
    stage.self->seeks += 1;
    stage.tick();
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(3.0));
    // Between the keys, it is put between them.
    stage.self->timePosition = 0.75;
    stage.self->seeks += 1;
    stage.tick();
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(1.5));
    // And an event AT the moment sought to is one still to come.
    stage.self->playing = true;
    stage.tick();
    const std::vector<Heard> at = stage.heard();
    REQUIRE(at.size() == 1);
    CHECK(at[0].name == "Half");
}

TEST_CASE("ADR 0204: a looped sequence starts again and keeps what it overshot by")
{
    LampStage stage;
    stage.self->looped = true;
    stage.self->playing = true;
    stage.tick(100);
    CHECK(stage.self->playing);
    CHECK(stage.self->timePosition == doctest::Approx(100.0 / 60.0 - 1.5).epsilon(0.02));
    const std::vector<Heard> heard = stage.heard();
    // Once round and a little: every event, then the first again; never finished.
    REQUIRE(heard.size() == 4);
    CHECK(heard[2].name == "End");
    CHECK(heard[3].name == "Begin");
}

TEST_CASE("ADR 0204: a clip is begun over the graph, faded into the next, and held still with the sequence")
{
    Stage stage;
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/act.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": ["Anna"], "length": 1.5,
      "tracks": [ { "type": "animation", "target": "Anna", "keys": [
          { "time": 0.1, "clip": "asset://anim/anna.glb#Wave", "fade": 0.3 },
          { "time": 0.3, "parameter": "Talking", "value": true },
          { "time": 0.4, "clip": "Bow", "hold": true } ] } ]
    })");
    const core::InstanceId anna = stage.make("Folder", stage.workspace, "Anna");
    const core::InstanceId animator = stage.make("AnimationPlayer", anna);
    stage.bind("Anna", anna);

    self.playing = true;
    stage.tick(12);
    // The first clip: made on the character's player, from the file it
    // names, over everything, and faded in.
    REQUIRE(stage.animation.made.size() == 2);
    CHECK(stage.animation.made[1].player == animator);
    CHECK(stage.animation.made[1].file == "asset://anim/anna.glb");
    CHECK(stage.animation.made[1].clip == "Wave");
    CHECK(stage.animation.made[1].over);
    CHECK(stage.animation.said() == std::vector<std::string>{"create:Wave", "play"});
    CHECK(stage.animation.asked[1].number == doctest::Approx(0.3));

    // Held still, its clips are; running again, they run.
    self.playing = false;
    stage.tick();
    CHECK(stage.animation.asked.back().what == "speed");
    CHECK(stage.animation.asked.back().number == 0.0f);
    self.playing = true;
    stage.tick();
    CHECK(stage.animation.asked.back().number == 1.0f);

    // The parameter is set, and the next clip takes over by ITS fade.
    stage.animation.asked.clear();
    stage.tick(20);
    CHECK(stage.animation.said() == std::vector<std::string>{"parameter:Talking", "create:Bow", "stop", "play"});
    CHECK(stage.animation.asked[0].number == 1.0f);
    CHECK(stage.animation.asked[2].track == 1);
    CHECK(stage.animation.asked[2].number == doctest::Approx(0.2));
    CHECK(stage.animation.made[2].file.empty());

    // It holds its last frame -- the clip is 0.4 s and the sequence goes on
    // -- until the sequence ends, and then goes by the fade it came in by.
    stage.animation.asked.clear();
    stage.tick(40);
    CHECK(stage.animation.asked.empty());
    stage.tick(30);
    CHECK_FALSE(self.playing);
    CHECK(stage.animation.said() == std::vector<std::string>{"stop"});
    CHECK(stage.animation.asked[0].track == 2);

    // Played a second time, the same two tracks are played again: none is made.
    self.timePosition = 0.0;
    self.seeks += 1;
    self.playing = true;
    stage.tick(40);
    CHECK(stage.animation.made.size() == 3);

    // A seek into a clip puts it at the frame that moment falls on.
    stage.animation.asked.clear();
    self.timePosition = 0.2;
    self.seeks += 1;
    stage.tick();
    bool sought = false;
    for (const Asked& one : stage.animation.asked) {
        if (one.what == "seek" && one.track == 1) {
            sought = true;
            CHECK(one.number == doctest::Approx(0.1));
        }
    }
    CHECK(sought);
}

TEST_CASE("ADR 0204: a line is said from what it is bound to, and its slot is part of the length")
{
    Stage stage;
    REQUIRE(stage.lines.load("meeting", R"({ "lines": { "anna_1": { "sound": "asset://voice/anna_1.wav" } } })"));
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/meet.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": ["Anna"],
      "tracks": [
        { "type": "line", "keys": [ { "time": 0.5, "line": "meeting.anna_1", "at": "Anna" } ] },
        { "type": "sound", "keys": [ { "time": 0, "sound": "asset://music/theme.ogg", "category": "Music",
                                       "volume": 0.25 } ] } ]
    })");
    const core::InstanceId anna = stage.make("Part", stage.workspace, "Anna");
    stage.bind("Anna", anna);

    // Half a second, and the two seconds the line takes in its longest language.
    CHECK(static_cast<double>(stage.sequences.lengthOf(*stage.world, stage.player)) == doctest::Approx(2.5));

    self.playing = true;
    stage.tick(2);
    // The music: from nowhere, so in the world; as loud and of the kind the
    // key says; and gone when it has played.
    core::InstanceId music;
    for (core::InstanceId child = stage.world->firstChild(stage.workspace); child.valid();
         child = stage.world->nextSibling(child)) {
        if (stage.world->sounds().find(child) != nullptr)
            music = child;
    }
    REQUIRE(music.valid());
    CHECK(stage.world->sounds().find(music)->category == 1);
    CHECK(static_cast<double>(stage.world->sounds().find(music)->volume) == doctest::Approx(0.25));
    CHECK(stage.world->sounds().find(music)->playing);
    CHECK(stage.world->sounds().find(music)->ownsItself);
    CHECK_FALSE(stage.world->firstChild(anna).valid());

    stage.tick(32);
    // The line: a voice under who says it, with its caption, as `Say` makes one.
    const core::InstanceId voice = stage.world->firstChild(anna);
    REQUIRE(voice.valid());
    const scene::SoundComponent* said = stage.world->sounds().find(voice);
    REQUIRE(said != nullptr);
    CHECK(said->content == "asset://voice/anna_1.wav");
    CHECK(said->category == 2);
    CHECK(said->playing);
    CHECK(stage.world->captions().find(stage.world->firstChild(voice)) != nullptr);

    // Skipped, what it began is ended.
    self.skips += 1;
    stage.tick();
    CHECK_FALSE(stage.world->sounds().find(voice)->playing);
    CHECK_FALSE(stage.world->sounds().find(music)->playing);
    CHECK(stage.world->sounds().find(voice)->endedFor == 0);
}

TEST_CASE("ADR 0204: the camera is a view the frame is told, cut to and moved, looking at what moves")
{
    Stage stage;
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/cam.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": ["Anna"],
      "tracks": [ { "type": "camera", "keys": [
          { "time": 0, "position": [0, 2, 10], "lookAt": "Anna", "fieldOfView": 50, "cut": true },
          { "time": 2, "position": [4, 2, 10], "lookAt": "Anna" },
          { "time": 2, "position": [0, 8, 0], "rotation": [-90, 0, 0], "fieldOfView": 30, "cut": true },
          { "time": 3, "position": [0, 8, 0], "rotation": [-90, 0, 0] } ] } ]
    })");
    const core::InstanceId anna = stage.make("Part", stage.workspace, "Anna");
    (void)stage.world->setProperty(anna, stage.atoms.intern("Position"), scene::Value{core::Vec3{0.0f, 2.0f, 0.0f}});
    stage.bind("Anna", anna);

    // Not playing, nobody's view is taken.
    stage.tick();
    CHECK_FALSE(stage.sequences.camera(*stage.world, 0.0).has_value());

    self.playing = true;
    stage.tick(60);
    const std::optional<app::Sequences::Camera> moving = stage.sequences.camera(*stage.world, 0.0);
    REQUIRE(moving.has_value());
    CHECK(moving->player == stage.player);
    CHECK(moving->cframe.position.x == doctest::Approx(2.0).epsilon(0.02));
    CHECK(moving->cframe.position.z == doctest::Approx(10.0));
    // Looking at her from where it is now.
    const core::Mat3 towards =
        core::lookAtCFrame(moving->cframe.position, core::DVec3{0.0, 2.0, 0.0}, core::Vec3{0.0f, 1.0f, 0.0f}).rotation;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column)
            CHECK(static_cast<double>(moving->cframe.rotation.m[row][column]) ==
                  doctest::Approx(static_cast<double>(towards.m[row][column])).epsilon(0.001));
    }
    // The second key says no field of view: it keeps the first's.
    CHECK(static_cast<double>(moving->fieldOfView) == doctest::Approx(50.0));
    // Between two ticks the frame asks for a moment behind the last one.
    const std::optional<app::Sequences::Camera> behind = stage.sequences.camera(*stage.world, -Frame);
    REQUIRE(behind.has_value());
    CHECK(behind->cframe.position.x < moving->cframe.position.x);
    // And she is followed where the frame draws her.
    const std::optional<app::Sequences::Camera> followed =
        stage.sequences.camera(*stage.world, 0.0, [](core::InstanceId) { return core::DVec3{2.0, 2.0, 0.0}; });
    REQUIRE(followed.has_value());
    CHECK(static_cast<double>(followed->cframe.rotation.m[0][0]) !=
          doctest::Approx(static_cast<double>(moving->cframe.rotation.m[0][0])));

    // The cut: at once somewhere else, looking down, through another lens.
    stage.tick(62);
    const std::optional<app::Sequences::Camera> cut = stage.sequences.camera(*stage.world, 0.0);
    REQUIRE(cut.has_value());
    CHECK(cut->cframe.position.y == doctest::Approx(8.0));
    CHECK(cut->cframe.position.x == doctest::Approx(0.0));
    CHECK(static_cast<double>(cut->fieldOfView) == doctest::Approx(30.0));

    // Off, the track is ignored; ended, the view is given back.
    self.takesCamera = false;
    CHECK_FALSE(stage.sequences.camera(*stage.world, 0.0).has_value());
    self.takesCamera = true;
    stage.tick(70);
    CHECK_FALSE(self.playing);
    CHECK_FALSE(stage.sequences.camera(*stage.world, 0.0).has_value());
}

TEST_CASE("ADR 0204: the controls are held while a sequence that asks for it plays, and given back")
{
    LampStage stage;
    CHECK_FALSE(stage.sequences.suspendsControls(*stage.world));
    stage.self->playing = true;
    stage.tick();
    CHECK_FALSE(stage.sequences.suspendsControls(*stage.world));
    stage.self->controls = 1;
    CHECK(stage.sequences.suspendsControls(*stage.world));
    // Held still it is still the sequence's moment.
    stage.self->playing = false;
    stage.tick();
    CHECK(stage.sequences.suspendsControls(*stage.world));
    stage.self->stops += 1;
    stage.tick();
    CHECK_FALSE(stage.sequences.suspendsControls(*stage.world));
}

TEST_CASE("ADR 0204: a machine that does not decide the world hears the events and writes nothing")
{
    LampStage stage;
    stage.authority = false;
    // As the authority's player arrives: playing, a quarter of a second in
    // at its tick 100 -- and this machine has applied its tick 121.
    stage.serverTick = 121;
    stage.self->playing = true;
    stage.self->anchorTime = 0.25f;
    stage.self->anchorTick = 100;
    stage.self->anchored = true;
    stage.tick();
    // It joins where the sequence has got to on the authority at the tick this
    // machine knows of: 0.25 s and 21 ticks (its time is the
    // authority's, and no tick of this machine's own is added to it).
    CHECK(stage.self->timePosition == doctest::Approx(0.25 + 21.0 / 60.0));
    // What came before it arrived is not said to it.
    CHECK(stage.heard().empty());

    stage.tick(70);
    // The lamp is the authority's to write: it arrives as the lamp does.
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(2.0));
    // At its end it has said what it passed, and waits to be told it is over:
    // the authority may yet skip it there.
    std::vector<Heard> heard = stage.heard();
    REQUIRE(heard.size() == 2);
    CHECK(heard[0].name == "Half");
    CHECK(heard[1].name == "End");
    CHECK(stage.self->playing);
    stage.self->playing = false;
    stage.self->anchorTime = 1.5f;
    stage.self->anchorTick = 175;
    stage.self->anchored = true;
    stage.tick();
    heard = stage.heard();
    REQUIRE(heard.size() == 1);
    CHECK(heard[0].event == "Finished");
    CHECK_FALSE(heard[0].skipped);

    // A skip on the authority is heard as one: its count arrives.
    LampStage skipped;
    skipped.authority = false;
    skipped.self->playing = true;
    skipped.self->anchored = true;
    skipped.tick(5);
    (void)skipped.heard();
    skipped.self->playing = false;
    skipped.self->skips += 1;
    skipped.self->anchorTime = 1.5f;
    skipped.self->anchored = true;
    skipped.tick();
    const std::vector<Heard> late = skipped.heard();
    REQUIRE(late.size() == 3);
    CHECK(late[0].skipped);
    CHECK(late[2].event == "Finished");
    CHECK(late[2].skipped);
    CHECK(skipped.number(skipped.light, "Brightness") == doctest::Approx(2.0));

    // And what the authority sends of its time is where it was and when:
    // written when it begins, and not a tick after.
    LampStage authority;
    authority.serverTick = 500;
    authority.self->playing = true;
    authority.tick();
    CHECK(authority.self->anchorTick == 500);
    CHECK(static_cast<double>(authority.self->anchorTime) == doctest::Approx(Frame));
    authority.tick(20);
    CHECK(authority.self->anchorTick == 500);
    authority.self->playing = false;
    authority.tick();
    CHECK(authority.self->anchorTick == 521);
}

TEST_CASE("a replica's sequence keeps the authority's time, and is over when the authority says so")
{
    // The authority's player arrives playing from nought at its tick 100. A
    // second and a half long: ninety ticks.
    LampStage stage;
    stage.authority = false;
    stage.serverTick = 100;
    stage.self->playing = true;
    stage.self->anchorTime = 0.0f;
    stage.self->anchorTick = 100;
    stage.self->anchored = true;

    // **An authority that runs slow** -- a loaded machine, a hitch: thirty of
    // its ticks are heard of while this machine runs ninety of its own. Run
    // on by its own ticks, the replica was at the end, had said `End` and
    // `Finished`, and was no longer playing -- while the authority was a
    // third of the way through and about to be skipped.
    for (int local = 0; local < 90; ++local) {
        stage.serverTick = 100 + static_cast<core::u64>(local / 3);
        stage.tick();
    }
    CHECK(stage.self->timePosition == doctest::Approx(0.5).epsilon(0.1));
    CHECK(stage.self->playing);
    // Its first event, which is at nought, and nothing past where it is.
    std::vector<Heard> heard = stage.heard();
    REQUIRE(heard.size() == 1);
    CHECK(heard[0].name == "Begin");

    // The authority's clock, heard of two seconds on at once -- this machine
    // stood still, or the route did: the sequence is brought to where it is,
    // and what it passed is said. At its end it waits to be told so.
    stage.serverTick = 220;
    stage.tick();
    CHECK(stage.self->timePosition == doctest::Approx(1.5));
    CHECK(stage.self->playing);
    heard = stage.heard();
    REQUIRE(heard.size() == 2);
    CHECK(heard[0].name == "Half");
    CHECK(heard[1].name == "End");
    stage.tick(10);
    CHECK(stage.heard().empty());

    // And the authority says it is over: once.
    stage.self->playing = false;
    stage.self->anchorTime = 1.5f;
    stage.self->anchorTick = 190;
    stage.self->anchored = true;
    stage.tick();
    heard = stage.heard();
    REQUIRE(heard.size() == 1);
    CHECK(heard[0].event == "Finished");
    CHECK_FALSE(heard[0].skipped);
    stage.tick(5);
    CHECK(stage.heard().empty());
}

TEST_CASE("what is asked of a sequence says from when with the ask, on the machine that decides")
{
    // A snapshot taken between a script's ask and the tick that does it is
    // what a replica reads the present off: the anchor is written with the
    // ask, and not only by the host a tick later -- when "playing" had
    // travelled with the anchor of the time before, which for a sequence
    // never played is the session's first tick.
    LampStage stage;
    scene::EngineState& state = stage.world->engineState();
    state.networkTopology = scene::NetworkTopology::Host;
    state.networkServerTick = 500;
    CHECK(stage.self->anchorTick == 0);

    // Sought: the place, and now.
    REQUIRE(stage.world->setProperty(stage.player, stage.atoms.intern("TimePosition"), scene::Value{0.5}) !=
            scene::World::SetResult::UnknownProperty);
    CHECK(stage.self->anchorTick == 500);
    CHECK(static_cast<double>(stage.self->anchorTime) == doctest::Approx(0.5));

    // As a script's `Play` and `Pause` say it.
    state.networkServerTick = 620;
    stage.self->timePosition = 0.75;
    scene::anchorSequence(*stage.world, stage.player, *stage.self);
    CHECK(stage.self->anchorTick == 620);
    CHECK(static_cast<double>(stage.self->anchorTime) == doctest::Approx(0.75));

    // A change of speed: where it had got to at the speed before.
    state.networkServerTick = 700;
    stage.self->timePosition = 1.0;
    REQUIRE(stage.world->setProperty(stage.player, stage.atoms.intern("Speed"), scene::Value{2.0}) !=
            scene::World::SetResult::UnknownProperty);
    CHECK(stage.self->anchorTick == 700);
    CHECK(static_cast<double>(stage.self->anchorTime) == doctest::Approx(1.0));

    // A replica decides none of it: what it holds is the authority's word.
    state.networkTopology = scene::NetworkTopology::Replica;
    state.networkServerTick = 900;
    scene::anchorSequence(*stage.world, stage.player, *stage.self);
    CHECK(stage.self->anchorTick == 700);
}

TEST_CASE("ADR 0204: a character whose place is keyed is carried, and set down on its feet")
{
    Stage stage;
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/walk.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": ["Hero"], "length": 2,
      "tracks": [ { "type": "property", "target": "Hero", "property": "CFrame", "keys": [
          { "time": 0, "value": [0, 3, 0, 0, 90, 20] }, { "time": 1, "value": [6, 3, 0, 0, 90, 20] } ] } ]
    })");
    const core::InstanceId hero = stage.make("CharacterBody", stage.workspace, "Hero");
    stage.bind("Hero", hero);
    const core::NameAtom anchored = stage.atoms.intern("Anchored");
    const core::NameAtom frame = stage.atoms.intern("CFrame");
    REQUIRE(stage.world->getProperty(hero, anchored).has_value());
    scene::CharacterBodyComponent* body = stage.world->characterBodies().find(hero);
    REQUIRE(body != nullptr);

    self.playing = true;
    stage.tick(30);
    // Held where the keys say, leaning as they say, and not simulated.
    CHECK(std::get<bool>(*stage.world->getProperty(hero, anchored)));
    const core::CFrameD carried = std::get<core::CFrameD>(*stage.world->getProperty(hero, frame));
    CHECK(carried.position.x == doctest::Approx(3.0).epsilon(0.02));
    CHECK(carried.position.y == doctest::Approx(3.0));
    CHECK(std::abs(core::toEulerYxz(carried.rotation).z) > 0.1f);

    // The track runs out: at the last key's place, upright, its own again.
    body->verticalVelocity = -4.0f;
    stage.tick(35);
    CHECK(self.playing);
    CHECK_FALSE(std::get<bool>(*stage.world->getProperty(hero, anchored)));
    const core::CFrameD down = std::get<core::CFrameD>(*stage.world->getProperty(hero, frame));
    CHECK(down.position.x == doctest::Approx(6.0));
    const core::Vec3 turn = core::toEulerYxz(down.rotation);
    CHECK(static_cast<double>(turn.x) == doctest::Approx(0.0).epsilon(0.001));
    CHECK(static_cast<double>(turn.z) == doctest::Approx(0.0).epsilon(0.001));
    CHECK(static_cast<double>(turn.y) == doctest::Approx(1.5708).epsilon(0.001));
    CHECK(body->verticalVelocity == 0.0f);

    // Stopped part way, it is set down where it had got to.
    self.timePosition = 0.0;
    self.seeks += 1;
    stage.tick(20);
    CHECK(std::get<bool>(*stage.world->getProperty(hero, anchored)));
    self.playing = false;
    self.stops += 1;
    stage.tick();
    CHECK_FALSE(std::get<bool>(*stage.world->getProperty(hero, anchored)));
    CHECK(std::get<core::CFrameD>(*stage.world->getProperty(hero, frame)).position.x < 3.0);
}

TEST_CASE("ADR 0204: a scrub shows a moment and puts everything back")
{
    LampStage stage;
    stage.sequences.preview(*stage.world, stage.player, 0.75);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(1.5));
    CHECK(stage.sequences.previewed() == stage.player);
    stage.sequences.preview(*stage.world, stage.player, 5.0);
    CHECK(stage.sequences.previewTime() == doctest::Approx(1.5));
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(3.0));
    // Before the first key the track says nothing, and the lamp is as it was.
    stage.sequences.preview(*stage.world, stage.player, 0.1);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(2.0));
    stage.sequences.preview(*stage.world, stage.player, 1.0);
    stage.sequences.endPreview(*stage.world);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(2.0));
    CHECK_FALSE(stage.sequences.previewed().valid());
    // Nothing was played, and nothing was said.
    CHECK(stage.heard().empty());
    CHECK_FALSE(stage.self->playing);
}

TEST_CASE("ADR 0204: a shape key is drawn by the frame while it plays, and held where it was left")
{
    Stage stage;
    constexpr const char* FaceModel = "asset://models/face.glb";
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/smile.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": ["Anna"], "length": 1.5,
      "tracks": [ { "type": "morph", "target": "Anna/Face", "key": "Smile", "keys": [
          { "time": 0.5, "value": 0 }, { "time": 1, "value": 0.8 } ] } ]
    })");
    const core::InstanceId anna = stage.make("Folder", stage.workspace, "Anna");
    const core::InstanceId face = stage.make("MeshPart", anna, "Face");
    stage.world->meshParts().find(face)->meshContent = stage.atoms.intern(FaceModel);
    stage.bind("Anna", anna);

    render::SkeletonLibrary skeletons;
    render::SkeletonLibrary::Entry entry;
    entry.morphNames = {"Smile", "JawOpen"};
    entry.morphDefaults = {0.0f, 0.0f};
    skeletons.set(stage.atoms.intern(FaceModel), std::move(entry));
    render::AnimationSystem drawn(*stage.world, skeletons);

    // A weight a script left held on the key would sit over the track: the
    // sequence takes the key when it begins.
    stage.morphs.held["Smile"] = 1.0f;
    self.playing = true;
    stage.tick(45);
    CHECK(stage.morphs.cleared == std::vector<std::string>{"Smile"});

    drawn.clearPresentedMorphWeights();
    stage.sequences.present(*stage.world, drawn, 0.0);
    const std::span<const float> weights = drawn.drawnMorphWeights(face);
    REQUIRE(weights.size() == 2);
    CHECK(static_cast<double>(weights[0]) == doctest::Approx(0.4).epsilon(0.05));
    CHECK(weights[1] == 0.0f);

    // Ended, the frame's layer goes with it, and the key is held at where
    // the track left it.
    stage.tick(60);
    CHECK_FALSE(self.playing);
    drawn.clearPresentedMorphWeights();
    stage.sequences.present(*stage.world, drawn, 0.0);
    CHECK(drawn.drawnMorphWeights(face).empty());
    REQUIRE(stage.morphs.held.count("Smile") == 1);
    CHECK(static_cast<double>(stage.morphs.held["Smile"]) == doctest::Approx(0.8));
}

TEST_CASE("ADR 0204: a file that is not there, or not a sequence, plays nothing and is asked for once")
{
    Stage stage;
    int asked = 0;
    stage.sequences.setSources(app::Sequences::Sources{
        .read = [&asked](std::string_view) -> std::optional<std::string> {
            ++asked;
            return std::string{"{ \"format\": \"scene\" }"};
        },
        .lineSeconds = {},
    });
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/none.sequence.json", "");
    self.playing = true;
    stage.tick(5);
    CHECK_FALSE(self.playing);
    CHECK(asked == 1);
    CHECK(stage.heard().empty());
    // Written again, it is read again.
    const core::NameAtom urn = self.sequence;
    stage.sequences.forget(std::span<const core::NameAtom>{&urn, 1});
    self.playing = true;
    stage.tick();
    CHECK(asked == 2);
}

TEST_CASE("ADR 0204: what fifty tracks cost a tick")
{
    Stage stage;
    std::string json = R"({ "format": "sequence", "version": 1, "bindings": ["Lamps"], "tracks": [)";
    const core::InstanceId lamps = stage.make("Folder", stage.workspace, "Lamps");
    for (int index = 0; index < 50; ++index) {
        const std::string name = "L" + std::to_string(index);
        (void)stage.make("PointLight", lamps, name.c_str());
        json += std::string(index == 0 ? "" : ",") + R"({ "type": "property", "target": "Lamps/)" + name +
                R"(", "property": "Brightness", "keys": [ { "time": 0, "value": 0 }, { "time": 20, "value": 5 } ] })";
    }
    json += "] }";
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/fifty.sequence.json", json.c_str());
    stage.bind("Lamps", lamps);
    self.playing = true;
    stage.tick(10);
    CHECK(stage.sequences.stats().tracks == 50);
    CHECK(stage.sequences.stats().written == 50);
    CHECK(stage.sequences.stats().playing == 1);

    const auto begin = std::chrono::steady_clock::now();
    constexpr int Ticks = 600;
    for (int tick = 0; tick < Ticks; ++tick) {
        stage.sequences.step(app::Sequences::Tick{
            .world = &*stage.world,
            .animation = &stage.animation,
            .morphs = &stage.morphs,
            .workspace = stage.workspace,
            .authority = true,
            .serverTick = stage.serverTick++,
            .fixedDt = Frame,
        });
        // The changes a tick's drain would have taken.
        (void)stage.world->changes().take();
    }
    const double micros =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / Ticks;
    MESSAGE("fifty property tracks, all written: ", micros, " microseconds a tick");
    CHECK(self.playing);
}

// --- The timeline: a sequence being edited ---------------------------------------

TEST_CASE("ADR 0204: a timeline scrubs the scene, keys what is moved while it records, and leaves the scene as it was")
{
    LampStage stage;
    app::Timeline timeline;
    REQUIRE(timeline.open(*stage.world, stage.sequences, stage.player));
    CHECK(timeline.sequence().tracks.size() == 2);
    CHECK_FALSE(timeline.dirty());

    // The playhead at three quarters: the lamp is where its track is.
    timeline.scrub(0.75);
    CHECK(timeline.update(*stage.world, stage.sequences) == 0);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(1.5));
    // Changed without a track recording, nothing is keyed.
    (void)stage.world->setProperty(stage.light, stage.atoms.intern("Brightness"), scene::Value{9.0});
    CHECK(timeline.update(*stage.world, stage.sequences) == 0);
    CHECK_FALSE(timeline.dirty());

    // **Recording, what a person changes is a key at the playhead** -- in
    // the viewport, in Properties, by anything that writes it.
    timeline.scrub(0.8);
    (void)timeline.update(*stage.world, stage.sequences);
    timeline.setRecording(0, true);
    (void)stage.world->setProperty(stage.light, stage.atoms.intern("Brightness"), scene::Value{4.0});
    CHECK(timeline.update(*stage.world, stage.sequences) == 1);
    CHECK(timeline.dirty());
    REQUIRE(timeline.sequence().tracks[0].keys.size() == 3);
    CHECK(static_cast<double>(timeline.sequence().tracks[0].keys[1].time) == doctest::Approx(0.8));
    CHECK(timeline.sequence().tracks[0].keys[1].value.numbers[0] == doctest::Approx(4.0));
    // And it is shown at once, from the copy and not from the file.
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(4.0));
    // Held, nothing more is keyed.
    CHECK(timeline.update(*stage.world, stage.sequences) == 0);
    // Changed again at the same moment, the key is replaced and not doubled.
    (void)stage.world->setProperty(stage.light, stage.atoms.intern("Brightness"), scene::Value{2.5});
    CHECK(timeline.update(*stage.world, stage.sequences) == 1);
    CHECK(timeline.sequence().tracks[0].keys.size() == 3);
    timeline.scrub(0.65);
    (void)timeline.update(*stage.world, stage.sequences);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(1.25));

    // A key is moved and removed; an event is set where the playhead is.
    CHECK(timeline.moveKey(0, 1, 1.2f) == 2);
    CHECK(static_cast<double>(timeline.sequence().tracks[0].keys[2].time) == doctest::Approx(1.2));
    timeline.removeKey(0, 2);
    CHECK(timeline.sequence().tracks[0].keys.size() == 2);
    CHECK(timeline.keyNamed(1, "Mid"));
    CHECK_FALSE(timeline.keyNamed(0, "Mid"));
    CHECK_FALSE(timeline.keyNumber(1, 0.5));
    CHECK(timeline.sequence().tracks[1].keys[1].name == "Mid");

    // **The scene is about to be saved**: it is put back first, and shown
    // again after.
    timeline.settle(*stage.world, stage.sequences);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(2.0));
    (void)timeline.update(*stage.world, stage.sequences);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(0.9).epsilon(0.02));

    // What it would save is a file that reads back as the copy.
    asset::Sequence saved;
    REQUIRE(asset::readSequence(timeline.text(), saved));
    CHECK(saved.tracks[1].keys.size() == 4);

    // Let go without saving: the scene as it was, and the player plays its file.
    timeline.close(*stage.world, stage.sequences);
    CHECK_FALSE(timeline.isOpen());
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(2.0));
    REQUIRE(stage.sequences.sequenceOf(*stage.world, stage.player) != nullptr);
    CHECK(stage.sequences.sequenceOf(*stage.world, stage.player)->tracks[1].keys.size() == 3);
}

TEST_CASE("ADR 0204: a timeline begins a file that is not there, tracks and camera keys from the view included")
{
    Stage stage;
    stage.player = stage.make("SequencePlayer", stage.workspace, "Cutscene");
    scene::SequencePlayerComponent* self = stage.world->sequencePlayers().find(stage.player);
    app::Timeline timeline;
    // No file named: nothing to open.
    CHECK_FALSE(timeline.open(*stage.world, stage.sequences, stage.player));
    self->sequence = stage.atoms.intern("asset://sequences/new.sequence.json");
    REQUIRE(timeline.open(*stage.world, stage.sequences, stage.player));
    CHECK(timeline.sequence().tracks.empty());

    const core::InstanceId door = stage.make("Part", stage.workspace, "Door");
    stage.bind("Door", door);
    const core::u32 track = timeline.addTrack(asset::SequenceTrackType::Property, "Door", "CFrame");
    CHECK(timeline.sequence().bindings == std::vector<std::string>{"Door"});
    (void)timeline.update(*stage.world, stage.sequences);
    // Keyed where it stands, then moved and turned and keyed again a second on.
    REQUIRE(timeline.keyFromScene(*stage.world, stage.sequences, track));
    timeline.scrub(1.0);
    (void)timeline.update(*stage.world, stage.sequences);
    timeline.setRecording(track, true);
    core::CFrameD open;
    open.position = core::DVec3{2.0, 0.0, 0.0};
    open.rotation = core::fromEulerYxz(core::Vec3{0.0f, 1.5707963f, 0.0f});
    (void)stage.world->setProperty(door, stage.atoms.intern("CFrame"), scene::Value{open});
    CHECK(timeline.update(*stage.world, stage.sequences) == 1);
    REQUIRE(timeline.sequence().tracks[track].keys.size() == 2);
    const asset::SequenceValue& keyed = timeline.sequence().tracks[track].keys[1].value;
    CHECK(keyed.count == 6);
    CHECK(keyed.numbers[0] == doctest::Approx(2.0));
    CHECK(keyed.numbers[4] == doctest::Approx(90.0).epsilon(0.001));
    // Half way, the door is half way: in place and in turn.
    timeline.scrub(0.5);
    (void)timeline.update(*stage.world, stage.sequences);
    const core::CFrameD half = std::get<core::CFrameD>(*stage.world->getProperty(door, stage.atoms.intern("CFrame")));
    CHECK(half.position.x == doctest::Approx(1.0));
    CHECK(static_cast<double>(core::toEulerYxz(half.rotation).y) == doctest::Approx(0.7854).epsilon(0.001));

    // A camera key from a view makes the camera track, and the view is that.
    core::CFrameD view;
    view.position = core::DVec3{0.0, 3.0, 8.0};
    view.rotation = core::fromEulerYxz(core::Vec3{-0.3f, 0.5f, 0.0f});
    CHECK(timeline.keyCamera(view, 40.0f, true) == 0);
    (void)timeline.update(*stage.world, stage.sequences);
    const std::optional<app::Sequences::Camera> seen = stage.sequences.camera(*stage.world, 0.0);
    REQUIRE(seen.has_value());
    CHECK(seen->cframe.position.z == doctest::Approx(8.0));
    CHECK(static_cast<double>(seen->fieldOfView) == doctest::Approx(40.0));
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column)
            CHECK(static_cast<double>(seen->cframe.rotation.m[row][column]) ==
                  doctest::Approx(static_cast<double>(view.rotation.m[row][column])).epsilon(0.001));
    }
    // Not looked through, the scrub still shows the door and the view is the editor's.
    stage.sequences.setPreviewLooks(false);
    CHECK_FALSE(stage.sequences.camera(*stage.world, 0.0).has_value());
    stage.sequences.setPreviewLooks(true);

    // Saved, it is the file; closed, the door is where the scene has it.
    asset::Sequence saved;
    REQUIRE(asset::readSequence(timeline.text(), saved));
    CHECK(saved.tracks.size() == 2);
    timeline.close(*stage.world, stage.sequences);
    CHECK(std::get<core::CFrameD>(*stage.world->getProperty(door, stage.atoms.intern("CFrame"))).position.x ==
          doctest::Approx(0.0));

    // A file that is there and is not a sequence is not opened over.
    stage.files["asset://sequences/bad.sequence.json"] = "{ \"format\": \"scene\" }";
    self->sequence = stage.atoms.intern("asset://sequences/bad.sequence.json");
    CHECK_FALSE(timeline.open(*stage.world, stage.sequences, stage.player));
}

TEST_CASE("ADR 0204: a file changed under an open timeline is taken up, unless the timeline has changes of its own")
{
    LampStage stage;
    app::Timeline timeline;
    REQUIRE(timeline.open(*stage.world, stage.sequences, stage.player));
    timeline.scrub(1.0);
    (void)timeline.update(*stage.world, stage.sequences);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(3.0));

    // Somebody writes the file: the lamp's last key is now eight.
    std::string changed = Lamp;
    changed.replace(changed.find("\"value\": 3"), 10, "\"value\": 8");
    stage.files[LampFile] = changed;
    const core::NameAtom urn = stage.atoms.intern(LampFile);
    stage.sequences.forget(std::span<const core::NameAtom>{&urn, 1});
    (void)timeline.update(*stage.world, stage.sequences);
    CHECK(timeline.sequence().tracks[0].keys[1].value.numbers[0] == doctest::Approx(8.0));
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(8.0));
    CHECK_FALSE(timeline.dirty());

    // With a change of its own, the copy is what is kept and what is shown.
    CHECK(timeline.keyNamed(1, "Mine"));
    (void)timeline.update(*stage.world, stage.sequences);
    stage.files[LampFile] = Lamp;
    stage.sequences.forget(std::span<const core::NameAtom>{&urn, 1});
    (void)timeline.update(*stage.world, stage.sequences);
    CHECK(timeline.sequence().tracks[0].keys[1].value.numbers[0] == doctest::Approx(8.0));
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(8.0));
    CHECK(timeline.dirty());
    timeline.close(*stage.world, stage.sequences);
}

TEST_CASE("ADR 0204: a replica told of a seek hears what is at the place sought to, as the authority did")
{
    LampStage stage;
    stage.authority = false;
    // Watched to its end.
    stage.self->playing = true;
    stage.self->anchored = true;
    stage.tick(95);
    (void)stage.heard();
    // At its end, and over when the authority says so.
    CHECK(stage.self->playing);
    stage.self->playing = false;
    stage.self->anchorTime = 1.5f;
    stage.self->anchorTick = static_cast<core::u32>(stage.serverTick);
    stage.self->anchored = true;
    stage.tick();
    (void)stage.heard();
    CHECK_FALSE(stage.self->playing);

    // The authority puts it back at nought and plays it: what arrives is its
    // count of seeks, that it plays, and where it was a tick after.
    stage.self->seeks += 1;
    stage.self->playing = true;
    stage.self->anchorTime = static_cast<float>(Frame);
    stage.self->anchorTick = static_cast<core::u32>(stage.serverTick);
    stage.self->anchored = true;
    stage.tick(3);
    const std::vector<Heard> again = stage.heard();
    REQUIRE(again.size() == 1);
    CHECK(again[0].name == "Begin");
    // A tick after nought at the anchor, and two of the authority's since.
    CHECK(stage.self->timePosition == doctest::Approx(3.0 * Frame));

    // And one sought into the middle and held: nothing is said, and the time
    // is the place sought to.
    stage.self->seeks += 1;
    stage.self->playing = false;
    stage.self->anchorTime = 1.0f;
    stage.self->anchorTick = static_cast<core::u32>(stage.serverTick);
    stage.self->anchored = true;
    stage.tick(3);
    CHECK(stage.heard().empty());
    CHECK(stage.self->timePosition == doctest::Approx(1.0));
}

TEST_CASE("ADR 0204: a scrub poses a bound character at the playhead, and puts it back when it lets go")
{
    Stage stage;
    (void)stage.playerOf("asset://sequences/act.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": ["Anna"], "length": 3,
      "tracks": [ { "type": "animation", "target": "Anna", "keys": [
          { "time": 0.5, "clip": "asset://anim/anna.glb#Wave", "fade": 0.2 },
          { "time": 1.5, "parameter": "Talking", "value": true },
          { "time": 2, "clip": "Bow", "hold": true } ] } ]
    })");
    const core::InstanceId anna = stage.make("Folder", stage.workspace, "Anna");
    const core::InstanceId animator = stage.make("AnimationPlayer", anna);
    stage.bind("Anna", anna);
    int prepared = 0;
    stage.sequences.setSources(app::Sequences::Sources{
        .read = [&stage](std::string_view urn) -> std::optional<std::string> {
            const auto found = stage.files.find(urn);
            return found == stage.files.end() ? std::nullopt : std::optional<std::string>{found->second};
        },
        .lineSeconds = {},
        .prepareRigs = [&prepared] { ++prepared; },
    });
    stage.sequences.setPoser(&stage.animation);

    // Before the first clip: nothing is posed with, and the poses are made.
    stage.sequences.preview(*stage.world, stage.player, 0.25);
    CHECK(prepared == 1);
    CHECK(stage.animation.made.size() == 1);
    CHECK(stage.sequences.previewSettled());

    // A tenth of a second into the wave: the clip, over the graph, at that
    // frame, half faded in, and still.
    stage.animation.asked.clear();
    stage.sequences.preview(*stage.world, stage.player, 0.6);
    REQUIRE(stage.animation.made.size() == 2);
    CHECK(stage.animation.made[1].player == animator);
    CHECK(stage.animation.made[1].clip == "Wave");
    CHECK(stage.animation.made[1].over);
    CHECK(stage.animation.made[1].time == doctest::Approx(0.1));
    CHECK(stage.animation.made[1].speed == 0.0f);
    CHECK(stage.animation.made[1].playing);

    // Past its end -- the stand-in's clips are 0.4 s long -- it is not there;
    // and the parameter its track set by then is set.
    stage.animation.asked.clear();
    stage.sequences.preview(*stage.world, stage.player, 1.7);
    CHECK_FALSE(stage.animation.made[1].playing);
    bool talking = false;
    for (const Asked& one : stage.animation.asked)
        talking = talking || (one.what == "parameter" && one.text == "Talking" && one.number == 1.0f);
    CHECK(talking);

    // Back before the parameter's key, it is handed back to the graph.
    stage.animation.asked.clear();
    stage.sequences.preview(*stage.world, stage.player, 0.6);
    CHECK(stage.animation.asked.front().what == "clear");
    CHECK(stage.animation.asked.front().text == "Talking");
    // The same clip is posed with again: no second track is made for it.
    CHECK(stage.animation.made.size() == 2);

    // A held clip is still asked for long after its end: put at its last
    // frame, where the animation holds it.
    stage.animation.asked.clear();
    stage.sequences.preview(*stage.world, stage.player, 2.9);
    REQUIRE(stage.animation.made.size() == 3);
    CHECK(stage.animation.made[2].clip == "Bow");
    bool held = false;
    for (const Asked& one : stage.animation.asked) {
        held = held || (one.what == "seek" && one.track == 2 && one.number > 0.8f);
        CHECK_FALSE((one.what == "stop" && one.track == 2));
    }
    CHECK(held);

    // Let go: every body as its file rests it.
    stage.animation.asked.clear();
    stage.sequences.endPreview(*stage.world);
    REQUIRE_FALSE(stage.animation.asked.empty());
    CHECK(stage.animation.asked.back().what == "rest");
}

namespace {

// A lamp keyed over three seconds, with three shots marked, a hold in the
// second, and an event each side of it.
constexpr const char* Shots = R"({
  "format": "sequence", "version": 1, "bindings": ["Lamp"],
  "tracks": [
    { "type": "property", "target": "Lamp/Light", "property": "Brightness", "keys": [
        { "time": 0, "value": 0 }, { "time": 3, "value": 3 } ] },
    { "type": "marker", "keys": [
        { "time": 0, "name": "Opening" }, { "time": 1, "name": "Choice" }, { "time": 2, "name": "Leaving" } ] },
    { "type": "hold", "keys": [ { "time": 1.5, "name": "Answer" } ] },
    { "type": "event", "keys": [
        { "time": 0.5, "name": "Before" }, { "time": 1.5, "name": "AtTheHold" }, { "time": 2.5, "name": "After" } ] }
  ]
})";

} // namespace

TEST_CASE("ADR 0204: a hold stops the sequence at its key until it is released, and says so once")
{
    LampStage stage;
    stage.files[LampFile] = Shots;
    stage.self->playing = true;
    stage.tick(120);
    // It went as far as the hold and no further: what is AT the key was done.
    CHECK(stage.self->held);
    CHECK_FALSE(stage.self->playing);
    CHECK(stage.self->timePosition == doctest::Approx(1.5));
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(1.5).epsilon(0.02));
    CHECK(stage.sequences.sequenceHoldAt(*stage.world, stage.self->sequence, stage.self->timePosition) == "Answer");
    std::vector<std::string> said;
    for (const Heard& one : stage.heard())
        said.push_back(one.event + ":" + one.name);
    CHECK(said == std::vector<std::string>{"MarkerReached:Opening", "EventReached:Before", "MarkerReached:Choice",
                                           "EventReached:AtTheHold", "HoldReached:Answer"});
    // And it waits, however long: nothing more is said or keyed.
    stage.tick(300);
    CHECK(stage.self->timePosition == doctest::Approx(1.5));
    CHECK(stage.heard().empty());

    // Released, as a script's `Release` does it: on from there, and the hold
    // is not met a second time.
    stage.self->held = false;
    stage.self->playing = true;
    stage.tick(30);
    CHECK_FALSE(stage.self->held);
    CHECK(stage.self->playing);
    CHECK(stage.self->timePosition == doctest::Approx(2.0).epsilon(0.02));
    stage.tick(90);
    CHECK_FALSE(stage.self->playing);
    said.clear();
    for (const Heard& one : stage.heard())
        said.push_back(one.event + ":" + one.name);
    CHECK(said == std::vector<std::string>{"MarkerReached:Leaving", "EventReached:After", "Finished:"});
}

TEST_CASE("ADR 0204: a skip passes a hold, and a seek onto one holds when it is played from there")
{
    LampStage stage;
    stage.files[LampFile] = Shots;
    stage.self->playing = true;
    stage.tick(30);
    stage.self->skips += 1;
    stage.tick();
    CHECK_FALSE(stage.self->held);
    CHECK_FALSE(stage.self->playing);
    CHECK(stage.self->timePosition == doctest::Approx(3.0));
    (void)stage.heard();

    // Sought to the hold's own moment and played: held at once, having gone
    // nowhere, and let go of by a seek that leaves it paused.
    stage.self->timePosition = 1.5;
    stage.self->seeks += 1;
    stage.self->playing = true;
    stage.tick(2);
    CHECK(stage.self->held);
    CHECK(stage.self->timePosition == doctest::Approx(1.5));
    stage.self->timePosition = 0.25;
    stage.self->seeks += 1;
    stage.tick();
    CHECK_FALSE(stage.self->held);
}

TEST_CASE("ADR 0204: a sequence has markers, and is skipped to the next as it would have been played to it")
{
    LampStage stage;
    stage.files[LampFile] = Shots;
    std::vector<scene::SequenceMarker> markers;
    stage.sequences.sequenceMarkers(*stage.world, stage.self->sequence, markers);
    REQUIRE(markers.size() == 3);
    CHECK(markers[0].name == "Opening");
    CHECK(markers[1].name == "Choice");
    CHECK(static_cast<double>(markers[1].time) == doctest::Approx(1.0));
    CHECK(markers[2].name == "Leaving");

    stage.self->playing = true;
    stage.tick(6);
    (void)stage.heard();
    // To the second shot: the lamp is where its keys would have put it there,
    // the event on the way is heard as skipped, the marker is reached, and
    // it plays on.
    stage.self->skipTo = markers[1].time;
    stage.tick();
    CHECK(stage.self->playing);
    CHECK(stage.self->timePosition == doctest::Approx(1.0).epsilon(0.03));
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(1.0).epsilon(0.05));
    // (A track at a time, in the file's order: the markers' before the events'.)
    const std::vector<Heard> heard = stage.heard();
    REQUIRE(heard.size() == 2);
    CHECK(heard[0].event == "MarkerReached");
    CHECK(heard[0].name == "Choice");
    CHECK(heard[1].name == "Before");
    CHECK(heard[1].skipped);

    // To the third: the hold between is passed, and what was at it is heard
    // as skipped.
    stage.self->skipTo = markers[2].time;
    stage.tick();
    CHECK_FALSE(stage.self->held);
    CHECK(stage.self->playing);
    CHECK(stage.self->timePosition == doctest::Approx(2.0).epsilon(0.03));
    const std::vector<Heard> after = stage.heard();
    REQUIRE(after.size() == 2);
    CHECK(after[0].name == "Leaving");
    CHECK(after[1].name == "AtTheHold");
    CHECK(after[1].skipped);
}

TEST_CASE("ADR 0204: a camera key in somebody's frame stands where they stand and turns as they turn")
{
    Stage stage;
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/over.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": ["Anna", "Bruno"],
      "tracks": [ { "type": "camera", "keys": [
          { "time": 0, "position": [0, 1, 3], "relativeTo": "Anna", "cut": true },
          { "time": 2, "position": [0, 1, 3], "relativeTo": "Bruno" },
          { "time": 4, "position": [0, 1, 3], "relativeTo": "Bruno", "lookAt": [0, 1, 0] } ] } ]
    })");
    const core::InstanceId anna = stage.make("Part", stage.workspace, "Anna");
    const core::InstanceId bruno = stage.make("Part", stage.workspace, "Bruno");
    // Anna at ten along x, facing as she was made; Bruno at the origin,
    // turned a quarter round, so "three behind him" is three along x.
    core::CFrameD there;
    there.position = core::DVec3{10.0, 0.0, 0.0};
    (void)stage.world->setProperty(anna, stage.atoms.intern("CFrame"), scene::Value{there});
    core::CFrameD turned;
    turned.rotation = core::fromEulerYxz(core::Vec3{0.0f, 1.57079633f, 0.0f});
    (void)stage.world->setProperty(bruno, stage.atoms.intern("CFrame"), scene::Value{turned});
    stage.bind("Anna", anna);
    stage.bind("Bruno", bruno);

    self.playing = true;
    stage.tick();
    // The first key: a metre up and three behind her, where she is.
    const std::optional<app::Sequences::Camera> first = stage.sequences.camera(*stage.world, -Frame);
    REQUIRE(first.has_value());
    CHECK(first->cframe.position.x == doctest::Approx(10.0));
    CHECK(first->cframe.position.y == doctest::Approx(1.0));
    CHECK(first->cframe.position.z == doctest::Approx(3.0));

    // Half way to the second: half way between the two places IN THE WORLD.
    stage.tick(59);
    const std::optional<app::Sequences::Camera> between = stage.sequences.camera(*stage.world, 0.0);
    REQUIRE(between.has_value());
    CHECK(between->cframe.position.x == doctest::Approx(6.5).epsilon(0.02));
    CHECK(between->cframe.position.z == doctest::Approx(1.5).epsilon(0.05));

    // At the second: three behind HIM, which is three along x, and turned as
    // he is turned.
    stage.tick(60);
    const std::optional<app::Sequences::Camera> second = stage.sequences.camera(*stage.world, 0.0);
    REQUIRE(second.has_value());
    CHECK(second->cframe.position.x == doctest::Approx(3.0).epsilon(0.01));
    CHECK(second->cframe.position.z == doctest::Approx(0.0).epsilon(0.01));
    CHECK(static_cast<double>(second->cframe.rotation.m[0][0]) ==
          doctest::Approx(static_cast<double>(turned.rotation.m[0][0])).epsilon(0.01));

    // And he is followed where the frame draws him: moved, the camera moves.
    const std::optional<app::Sequences::Camera> drawn =
        stage.sequences.camera(*stage.world, 0.0, {}, [&](core::InstanceId) {
            core::CFrameD moved = turned;
            moved.position = core::DVec3{0.0, 0.0, 5.0};
            return moved;
        });
    REQUIRE(drawn.has_value());
    CHECK(drawn->cframe.position.x == doctest::Approx(3.0).epsilon(0.01));
    CHECK(drawn->cframe.position.z == doctest::Approx(5.0).epsilon(0.01));
}

TEST_CASE("ADR 0204: a fade and the bars are what the frame is told, eased between their keys")
{
    Stage stage;
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/fade.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": [],
      "tracks": [
        { "type": "fade", "color": [1, 1, 1], "keys": [
            { "time": 0, "value": 1 }, { "time": 1, "value": 0 }, { "time": 2, "value": 0 } ] },
        { "type": "bars", "aspect": 2, "keys": [ { "time": 0, "value": 0 }, { "time": 1, "value": 1 } ] }
      ]
    })");
    // Nothing playing: nothing over the picture.
    stage.tick();
    CHECK(static_cast<double>(stage.sequences.screen(*stage.world, 0.0).fade) == doctest::Approx(0.0));

    self.playing = true;
    stage.tick(30);
    const app::Sequences::Screen half = stage.sequences.screen(*stage.world, 0.0);
    CHECK(static_cast<double>(half.fade) == doctest::Approx(0.5).epsilon(0.05));
    CHECK(static_cast<double>(half.fadeColor.g) == doctest::Approx(1.0));
    CHECK(static_cast<double>(half.bars) == doctest::Approx(0.5).epsilon(0.05));
    CHECK(static_cast<double>(half.aspect) == doctest::Approx(2.0));
    stage.tick(45);
    const app::Sequences::Screen in = stage.sequences.screen(*stage.world, 0.0);
    CHECK(static_cast<double>(in.fade) == doctest::Approx(0.0));
    CHECK(static_cast<double>(in.bars) == doctest::Approx(1.0));
    // Over, the picture is the game's again.
    stage.tick(60);
    CHECK_FALSE(self.playing);
    CHECK(static_cast<double>(stage.sequences.screen(*stage.world, 0.0).bars) == doctest::Approx(0.0));
}

TEST_CASE("ADR 0204: a caption key shows words nobody says, as a line shows its own")
{
    Stage stage;
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/words.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": [],
      "tracks": [ { "type": "caption", "keys": [
          { "time": 0.1, "caption": "story.three_years_later", "seconds": 2 },
          { "time": 0.5, "caption": "story.the_harbour", "speaker": "story.narrator" } ] } ]
    })");
    self.playing = true;
    stage.tick(12);
    std::vector<std::string> shown;
    stage.world->captions().forEach([&](core::InstanceId, const scene::CaptionComponent& caption) {
        shown.emplace_back(stage.atoms.text(caption.text));
        CHECK(static_cast<double>(caption.seconds) == doctest::Approx(2.0));
        CHECK(caption.speaker.id == 0);
    });
    CHECK(shown == std::vector<std::string>{"story.three_years_later"});
    stage.tick(30);
    CHECK(stage.world->captions().size() == 2);
    // Skipped over, words are not shown at all.
    Stage other;
    scene::SequencePlayerComponent& skipped = other.playerOf(
        "asset://sequences/words.sequence.json", stage.files["asset://sequences/words.sequence.json"].c_str());
    skipped.skips += 1;
    other.tick();
    CHECK(other.world->captions().size() == 0);
}

TEST_CASE("ADR 0204: a camera's path is one curve through its keys where the track says so")
{
    constexpr const char* Keys = R"("keys": [
          { "time": 0, "position": [0, 0, 0], "rotation": [0, 0, 0] },
          { "time": 1, "position": [10, 0, 0], "rotation": [0, 0, 0] },
          { "time": 2, "position": [10, 0, 10], "rotation": [0, 0, 0] },
          { "time": 3, "position": [0, 0, 10], "rotation": [0, 0, 0] } ] } ] })";
    const std::string head =
        R"({ "format": "sequence", "version": 1, "bindings": [], "tracks": [ { "type": "camera", )";
    Stage curved;
    curved.playerOf("asset://sequences/path.sequence.json", (head + R"("path": "curve", )" + Keys).c_str());
    Stage straight;
    straight.playerOf("asset://sequences/path.sequence.json", (head + Keys).c_str());
    const auto at = [](Stage& stage, double seconds) {
        stage.sequences.preview(*stage.world, stage.player, seconds);
        const std::optional<app::Sequences::Camera> view = stage.sequences.camera(*stage.world, 0.0);
        REQUIRE(view.has_value());
        return view->cframe.position;
    };
    // Every key is still passed through.
    CHECK(at(curved, 1.0).x == doctest::Approx(10.0));
    CHECK(at(curved, 1.0).z == doctest::Approx(0.0));
    CHECK(at(curved, 2.0).z == doctest::Approx(10.0));
    // Between two the straight path is a line, and the curve swings wide of
    // it: it is still going the way it came when it leaves the corner.
    CHECK(at(straight, 1.5).x == doctest::Approx(10.0));
    CHECK(at(curved, 1.5).x == doctest::Approx(11.25));
    CHECK(at(curved, 1.5).z == doctest::Approx(5.0));
    // **And it has no corner**: its speed either side of a key is one speed,
    // where the line's turns a right angle there.
    const auto speed = [&](Stage& stage, double from, double to) {
        const core::DVec3 a = at(stage, from);
        const core::DVec3 b = at(stage, to);
        return core::DVec3{(b.x - a.x) / (to - from), 0.0, (b.z - a.z) / (to - from)};
    };
    const core::DVec3 arriving = speed(curved, 0.99, 1.0);
    const core::DVec3 leaving = speed(curved, 1.0, 1.01);
    CHECK(arriving.x == doctest::Approx(leaving.x).epsilon(0.05));
    CHECK(arriving.z == doctest::Approx(leaving.z).epsilon(0.05));
    CHECK(arriving.x == doctest::Approx(5.0).epsilon(0.05));
    CHECK(speed(straight, 0.99, 1.0).x == doctest::Approx(10.0));
    CHECK(speed(straight, 1.0, 1.01).x == doctest::Approx(0.0));
}

TEST_CASE("ADR 0204: a shaken camera wanders by no more than its key says, the same on every machine")
{
    constexpr const char* File = R"({ "format": "sequence", "version": 1, "bindings": [],
      "tracks": [ { "type": "camera", "keys": [
          { "time": 0, "position": [0, 2, 0], "rotation": [0, 0, 0] },
          { "time": 1, "position": [0, 2, 0], "rotation": [0, 0, 0], "shake": [0.1, 6] },
          { "time": 3, "position": [0, 2, 0], "rotation": [0, 0, 0], "shake": [0.1, 6] } ] } ] })";
    Stage stage;
    stage.playerOf("asset://sequences/shake.sequence.json", File);
    Stage other;
    other.playerOf("asset://sequences/shake.sequence.json", File);
    const auto at = [](Stage& on, double seconds) {
        on.sequences.preview(*on.world, on.player, seconds);
        const std::optional<app::Sequences::Camera> view = on.sequences.camera(*on.world, 0.0);
        REQUIRE(view.has_value());
        return view->cframe;
    };
    // The first key says none: it is where it was put, and looks ahead.
    CHECK(at(stage, 0.0).position.x == doctest::Approx(0.0));
    CHECK(static_cast<double>(at(stage, 0.0).rotation.m[0][0]) == doctest::Approx(1.0));
    // Shaken: off its place by no more than a tenth of a metre, turned a
    // little, and not standing still -- over a second it is in many places.
    double furthest = 0.0;
    double travelled = 0.0;
    core::DVec3 last = at(stage, 1.0).position;
    for (int step = 1; step <= 60; ++step) {
        const core::CFrameD now = at(stage, 1.0 + static_cast<double>(step) / 60.0);
        furthest = std::max({furthest, std::abs(now.position.x), std::abs(now.position.y - 2.0)});
        travelled += std::abs(now.position.x - last.x) + std::abs(now.position.y - last.y);
        CHECK(now.position.z == doctest::Approx(0.0));
        last = now.position;
    }
    CHECK(furthest <= 0.1 + 1.0e-6);
    CHECK(furthest > 0.02);
    CHECK(travelled > 0.2);
    CHECK(static_cast<double>(at(stage, 1.7).rotation.m[0][0]) < 1.0);
    // Eased in from the key that says none.
    CHECK(std::abs(at(stage, 0.1).position.x) <= 0.011);
    // **A function of the sequence's time**: the same moment is the same
    // picture -- asked twice, and on another machine.
    const core::CFrameD once = at(stage, 2.2);
    const core::CFrameD twice = at(stage, 2.2);
    const core::CFrameD elsewhere = at(other, 2.2);
    CHECK(once.position.x == twice.position.x);
    CHECK(once.position.x == elsewhere.position.x);
    CHECK(once.position.y == elsewhere.position.y);
    CHECK(once.rotation.m[0][2] == elsewhere.rotation.m[0][2]);
}

TEST_CASE("ADR 0204: a camera kept in sight of who it looks at comes this side of what stands between")
{
    Stage stage;
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/sight.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": ["Anna"],
      "tracks": [ { "type": "camera", "keys": [
          { "time": 0, "position": [0, 2, 10], "lookAt": "Anna", "keepInSight": true },
          { "time": 2, "position": [0, 2, 10], "lookAt": "Anna" } ] } ] })");
    const core::InstanceId anna = stage.make("Part", stage.workspace, "Anna");
    (void)stage.world->setProperty(anna, stage.atoms.intern("Position"), scene::Value{core::Vec3{0.0f, 2.0f, 0.0f}});
    stage.bind("Anna", anna);
    self.playing = true;
    stage.tick();

    int asked = 0;
    core::DVec3 from;
    core::DVec3 to;
    core::InstanceId looked;
    const app::Sequences::Sight wall = [&](core::DVec3 a, core::DVec3 b,
                                           core::InstanceId who) -> std::optional<core::f64> {
        ++asked;
        from = a;
        to = b;
        looked = who;
        return 4.0;
    };
    // A wall four metres from her, between her and the camera: the camera is
    // a hand's breadth this side of it, still looking at her.
    const std::optional<app::Sequences::Camera> near = stage.sequences.camera(*stage.world, 0.0, {}, {}, wall);
    REQUIRE(near.has_value());
    CHECK(asked == 1);
    CHECK(looked == anna);
    CHECK(from.z == doctest::Approx(0.0));
    CHECK(to.z == doctest::Approx(10.0).epsilon(0.01));
    CHECK(near->cframe.position.z == doctest::Approx(3.8).epsilon(0.02));
    CHECK(near->cframe.position.y == doctest::Approx(2.0));
    // Nothing between them, it is where its key put it; and so it is where
    // nobody can say what stands between.
    const app::Sequences::Sight open = [](core::DVec3, core::DVec3, core::InstanceId) -> std::optional<core::f64> {
        return std::nullopt;
    };
    CHECK(stage.sequences.camera(*stage.world, 0.0, {}, {}, open)->cframe.position.z == doctest::Approx(10.0));
    CHECK(stage.sequences.camera(*stage.world, 0.0)->cframe.position.z == doctest::Approx(10.0));
    // **Eased out with the key that asks for it**: half way to a key that
    // does not, it has come half way back.
    stage.tick(59);
    const std::optional<app::Sequences::Camera> half = stage.sequences.camera(*stage.world, 0.0, {}, {}, wall);
    REQUIRE(half.has_value());
    CHECK(half->cframe.position.z == doctest::Approx(6.9).epsilon(0.02));
    // And a key that does not ask casts no ray.
    stage.tick(70);
    asked = 0;
    stage.sequences.preview(*stage.world, stage.player, 2.0);
    CHECK(stage.sequences.camera(*stage.world, 0.0, {}, {}, wall)->cframe.position.z == doctest::Approx(10.0));
    CHECK(asked == 0);
}

TEST_CASE("ADR 0204: a picture key after a line is where this machine's voices put it, and the length is everyone's")
{
    Stage stage;
    REQUIRE(stage.lines.load("meeting", R"({ "lines": { "anna_1": { "sound": "asset://voice/anna_1.wav" } } })"));
    // The slot is two seconds; as heard here, a second and a quarter -- and
    // in the other language, the whole two.
    double heardFor = 1.25;
    stage.sequences.setSources(app::Sequences::Sources{
        .read = [&stage](std::string_view urn) -> std::optional<std::string> {
            const auto found = stage.files.find(urn);
            return found == stage.files.end() ? std::nullopt : std::optional<std::string>{found->second};
        },
        .lineSeconds = [](std::string_view, std::string_view, core::f32) { return 2.0; },
        .lineHeard = [&heardFor](std::string_view, std::string_view, core::f32) { return heardFor; },
    });
    stage.playerOf("asset://sequences/after.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": [],
      "tracks": [
        { "type": "line", "keys": [ { "time": 1, "line": "meeting.anna_1" } ] },
        { "type": "fade", "keys": [
            { "time": 0, "value": 0 },
            { "time": 0, "afterLine": "meeting.anna_1", "value": 0 },
            { "time": 0.5, "afterLine": "meeting.anna_1", "value": 1 } ] } ] })");
    const auto fadeAt = [&](double seconds) {
        stage.sequences.preview(*stage.world, stage.player, seconds);
        return static_cast<double>(stage.sequences.screen(*stage.world, 0.0).fade);
    };
    // **As long on every machine**: the line's slot, and the key's own time
    // after it -- whatever is heard here.
    CHECK(static_cast<double>(stage.sequences.lengthOf(*stage.world, stage.player)) == doctest::Approx(3.5));
    // The line begins at one and is heard for a second and a quarter: the
    // fade begins as its last word is said, and is whole half a second on.
    CHECK(fadeAt(2.2) == doctest::Approx(0.0));
    CHECK(fadeAt(2.5) == doctest::Approx(0.5).epsilon(0.01));
    CHECK(fadeAt(2.75) == doctest::Approx(1.0));
    // **In another language the same file cuts later**: the voices are
    // changed, and the picture is laid out again by what is heard now.
    heardFor = 2.0;
    stage.world->engineState().voiceLocale = "pt";
    CHECK(fadeAt(2.5) == doctest::Approx(0.0));
    CHECK(fadeAt(3.25) == doctest::Approx(0.5).epsilon(0.01));
    CHECK(static_cast<double>(stage.sequences.lengthOf(*stage.world, stage.player)) == doctest::Approx(3.5));
    stage.sequences.endPreview(*stage.world);
}

TEST_CASE("ADR 0204: a value written as a variable is the player's attribute of that name, read when it is used")
{
    Stage stage;
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/variable.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": ["Lamp"],
      "tracks": [
        { "type": "property", "target": "Lamp/Light", "property": "Brightness", "keys": [
            { "time": 0.5, "value": 0 }, { "time": 1, "value": "$Glow" } ] },
        { "type": "property", "target": "Lamp/Light", "property": "Color", "keys": [
            { "time": 0, "value": "$Tint" } ] },
        { "type": "fade", "keys": [ { "time": 0, "value": "$Cover" } ] } ] })");
    const core::InstanceId lamp = stage.make("Part", stage.workspace, "Lamp");
    const core::InstanceId light = stage.make("PointLight", lamp, "Light");
    (void)stage.world->setProperty(light, stage.atoms.intern("Brightness"), scene::Value{2.0});
    stage.bind("Lamp", lamp);
    REQUIRE(stage.world->setAttribute(stage.player, stage.atoms.intern("Glow"), scene::Value{6.0}));
    REQUIRE(stage.world->setAttribute(stage.player, stage.atoms.intern("Tint"),
                                      scene::Value{core::Color3{1.0f, 0.5f, 0.25f}}));
    REQUIRE(stage.world->setAttribute(stage.player, stage.atoms.intern("Cover"), scene::Value{0.75}));

    self.playing = true;
    stage.tick(45);
    // Half way to what the game said: three of six.
    CHECK(stage.number(light, "Brightness") == doctest::Approx(3.0).epsilon(0.1));
    const std::optional<scene::Value> colour = stage.world->getProperty(light, stage.atoms.intern("Color"));
    REQUIRE(colour.has_value());
    CHECK(static_cast<double>(std::get<core::Color3>(*colour).g) == doctest::Approx(0.5));
    CHECK(static_cast<double>(stage.sequences.screen(*stage.world, 0.0).fade) == doctest::Approx(0.75));
    // **Read when it is used**: said again while the sequence plays, it is
    // what the track goes to from here.
    REQUIRE(stage.world->setAttribute(stage.player, stage.atoms.intern("Glow"), scene::Value{10.0}));
    stage.tick(20);
    CHECK(stage.number(light, "Brightness") == doctest::Approx(10.0));
    CHECK(stage.sequences.stats().refused == 0);
}

TEST_CASE("ADR 0204: a variable the game has not said is no value, and the track writes nothing for it")
{
    Stage stage;
    scene::SequencePlayerComponent& self = stage.playerOf("asset://sequences/unsaid.sequence.json", R"({
      "format": "sequence", "version": 1, "bindings": ["Lamp"],
      "tracks": [ { "type": "property", "target": "Lamp/Light", "property": "Brightness", "keys": [
          { "time": 0, "value": "$Glow" } ] } ] })");
    const core::InstanceId lamp = stage.make("Part", stage.workspace, "Lamp");
    const core::InstanceId light = stage.make("PointLight", lamp, "Light");
    (void)stage.world->setProperty(light, stage.atoms.intern("Brightness"), scene::Value{2.0});
    stage.bind("Lamp", lamp);
    self.playing = true;
    stage.tick(5);
    CHECK(stage.number(light, "Brightness") == doctest::Approx(2.0));
}

namespace {

// A guard as a stamp's text: a part with a light under it.
[[nodiscard]] std::string guardStamp(Stage& stage)
{
    const core::InstanceId guard = stage.make("Part", {}, "Guard");
    (void)stage.make("PointLight", guard, "Lantern");
    const std::string text = scene::writeStamp(*stage.world, guard);
    (void)stage.world->destroy(guard);
    stage.world->retireDestroyed();
    return text;
}

constexpr const char* CastFile = "asset://sequences/cast.sequence.json";
constexpr const char* CastText = R"({
  "format": "sequence", "version": 1, "bindings": [],
  "tracks": [
    { "type": "property", "target": "Guard", "property": "Fade", "keys": [
        { "time": 0.5, "value": 0 }, { "time": 1, "value": 0.5 } ] },
    { "type": "cast", "as": "Guard", "model": "guard", "keys": [
        { "time": 0.5 }, { "time": 1.5, "show": false }, { "time": 2 } ] },
    { "type": "event", "keys": [ { "time": 2.5, "name": "End" } ] }
  ]
})";

struct CastStage : Stage
{
    scene::SequencePlayerComponent* self = nullptr;
    std::vector<std::string> asked;

    CastStage()
    {
        const std::string stamp = guardStamp(*this);
        sequences.setSources(app::Sequences::Sources{
            .read = [this](std::string_view urn) -> std::optional<std::string> {
                const auto found = files.find(urn);
                return found == files.end() ? std::nullopt : std::optional<std::string>{found->second};
            },
            .lineSeconds = [](std::string_view, std::string_view, core::f32) { return 2.0; },
            .stamp = [this, stamp](std::string_view name) -> std::optional<std::string> {
                asked.emplace_back(name);
                return name == "stamps/guard.stamp.json" ? std::optional<std::string>{stamp} : std::nullopt;
            },
        });
        self = &playerOf(CastFile, CastText);
        (void)heard();
    }

    // Who the track brought, where they are in the world; invalid for nobody.
    [[nodiscard]] core::InstanceId guard() const { return sequences.resolve(*world, player, "Guard"); }
    [[nodiscard]] int bindings() const
    {
        int count = 0;
        for (core::InstanceId child = world->firstChild(player); child.valid(); child = world->nextSibling(child))
            count += world->sequenceBindings().find(child) != nullptr ? 1 : 0;
        return count;
    }
};

} // namespace

TEST_CASE("ADR 0204: a cast track brings somebody made of a stamp, bound under its name, there and gone by its keys")
{
    CastStage stage;
    stage.self->playing = true;
    // Before its first key nobody is made, and nothing is bound.
    stage.tick(15);
    CHECK_FALSE(stage.guard().valid());
    CHECK(stage.bindings() == 0);
    CHECK(stage.asked.empty());

    // Brought: in the world, known to the other tracks by the name -- a
    // property track written ABOVE the cast track has been moving them since
    // the tick they arrived.
    stage.tick(30);
    const core::InstanceId guard = stage.guard();
    REQUIRE(guard.valid());
    CHECK(stage.asked == std::vector<std::string>{"stamps/guard.stamp.json"});
    CHECK(stage.world->parentOf(guard) == stage.workspace);
    CHECK(stage.atoms.text(stage.world->name(guard)) == "Guard");
    CHECK(stage.world->firstChild(guard).valid());
    CHECK(stage.bindings() == 1);
    CHECK(stage.number(guard, "Fade") == doctest::Approx(0.25).epsilon(0.1));

    // Gone by a key: out of the world and kept, so they come back as they left.
    stage.tick(60);
    CHECK(stage.world->alive(guard));
    CHECK_FALSE(stage.world->parentOf(guard).valid());
    stage.tick(30);
    CHECK(stage.world->parentOf(guard) == stage.workspace);
    CHECK(stage.guard() == guard);
    CHECK(stage.number(guard, "Fade") == doctest::Approx(0.5));

    // **At its end they are as its last key left them**: there.
    stage.tick(40);
    CHECK_FALSE(stage.self->playing);
    CHECK(stage.world->parentOf(guard) == stage.workspace);
    CHECK(stage.bindings() == 1);

    // Played again from the start, it is before it brought anybody.
    stage.self->timePosition = 0.0;
    stage.self->seeks += 1;
    stage.self->playing = true;
    stage.tick(2);
    stage.world->retireDestroyed();
    CHECK_FALSE(stage.world->alive(guard));
    CHECK(stage.bindings() == 0);

    // **Stopped, whoever it brought goes with it.**
    stage.tick(40);
    const core::InstanceId second = stage.guard();
    REQUIRE(second.valid());
    stage.self->playing = false;
    stage.self->stops += 1;
    stage.tick(2);
    stage.world->retireDestroyed();
    CHECK_FALSE(stage.world->alive(second));
    CHECK(stage.bindings() == 0);
}

TEST_CASE("ADR 0204: a skip leaves a cast as the last key says, and a machine that does not decide makes nobody")
{
    {
        CastStage stage;
        stage.self->skips += 1;
        stage.tick();
        // Never watched, and there: the last key has them there.
        const core::InstanceId guard = stage.guard();
        REQUIRE(guard.valid());
        CHECK(stage.world->parentOf(guard) == stage.workspace);
        CHECK(stage.number(guard, "Fade") == doctest::Approx(0.5));
    }
    {
        // A file whose last key has them gone leaves nobody, and no binding.
        CastStage stage;
        stage.files[CastFile] = R"({ "format": "sequence", "version": 1, "bindings": [], "tracks": [
            { "type": "cast", "as": "Guard", "model": "guard", "keys": [ { "time": 0.5 }, { "time": 1, "show": false } ] } ] })";
        stage.self->skips += 1;
        stage.tick();
        stage.world->retireDestroyed();
        CHECK_FALSE(stage.guard().valid());
        CHECK(stage.bindings() == 0);
        CHECK(stage.world->parts().size() == 0);
    }
    {
        // A replica is sent who the authority brought; it makes nobody itself.
        CastStage stage;
        stage.authority = false;
        stage.self->playing = true;
        stage.self->anchorTick = 1;
        stage.tick(60);
        CHECK(stage.asked.empty());
        CHECK(stage.world->parts().size() == 0);
        CHECK(stage.bindings() == 0);
    }
    {
        // A stamp that is not there: nobody, said once, and the rest plays.
        CastStage stage;
        stage.files[CastFile] = R"({ "format": "sequence", "version": 1, "bindings": [], "tracks": [
            { "type": "cast", "as": "Guard", "model": "nobody", "keys": [ { "time": 0 } ] },
            { "type": "event", "keys": [ { "time": 0.1, "name": "On" } ] } ] })";
        stage.self->playing = true;
        stage.tick(10);
        CHECK_FALSE(stage.guard().valid());
        // (And then it is over: a tenth of a second is all it is.)
        const std::vector<Heard> heard = stage.heard();
        REQUIRE(heard.size() == 2);
        CHECK(heard[0].name == "On");
        CHECK(heard[1].event == "Finished");
    }
}

TEST_CASE("ADR 0204: a scrub shows who a cast track brings between its keys, and leaves nobody in the scene")
{
    CastStage stage;
    const core::usize before = stage.world->parts().size();
    stage.sequences.preview(*stage.world, stage.player, 0.25);
    CHECK_FALSE(stage.guard().valid());
    stage.sequences.preview(*stage.world, stage.player, 0.75);
    const core::InstanceId guard = stage.guard();
    REQUIRE(guard.valid());
    CHECK(stage.world->parentOf(guard) == stage.workspace);
    // In the scene and not of it: never saved, and bound by no instance.
    CHECK(stage.world->generated(guard));
    CHECK(stage.bindings() == 0);
    CHECK(stage.number(guard, "Fade") == doctest::Approx(0.25));
    // The same one a moment later, and not there where a key has them gone.
    stage.sequences.preview(*stage.world, stage.player, 1.0);
    CHECK(stage.guard() == guard);
    stage.sequences.preview(*stage.world, stage.player, 1.75);
    stage.world->retireDestroyed();
    CHECK_FALSE(stage.guard().valid());
    stage.sequences.preview(*stage.world, stage.player, 2.25);
    REQUIRE(stage.guard().valid());
    stage.sequences.endPreview(*stage.world);
    stage.world->retireDestroyed();
    CHECK_FALSE(stage.guard().valid());
    CHECK(stage.world->parts().size() == before);
}

TEST_CASE("ADR 0204: a sequence a script wrote is played as a file is, and is that machine's own")
{
    LampStage stage;
    const core::u32 file = stage.self->sequence.id;
    std::string what;
    // Not a sequence: said, with where, and the player is left as it was.
    CHECK_FALSE(stage.sequences.loadSequence(
        *stage.world, stage.player,
        R"({ "format": "sequence", "version": 1, "tracks": [ { "type": "property", "target": "Nobody", "property": "X", "keys": [] } ] })",
        what));
    CHECK(what == "tracks[0].target");
    CHECK(stage.self->sequence.id == file);

    REQUIRE(stage.sequences.loadSequence(*stage.world, stage.player, R"({
      "format": "sequence", "version": 1, "bindings": ["Lamp"],
      "tracks": [ { "type": "property", "target": "Lamp/Light", "property": "Brightness", "keys": [
          { "time": 0, "value": 9 } ] } ] })",
                                         what));
    const std::string name(stage.atoms.text(stage.self->sequence));
    CHECK(name.rfind("memory://sequence/", 0) == 0);
    stage.self->playing = true;
    stage.tick(2);
    CHECK(stage.number(stage.light, "Brightness") == doctest::Approx(9.0));
    // Files read again, what a script wrote is still there: it is no file.
    stage.sequences.forgetAll();
    CHECK(stage.sequences.sequenceOf(*stage.world, stage.player) != nullptr);

    // **A name another machine gave one of its own names nothing here**, and
    // nothing is said of it: there is no such file to miss.
    Stage replica;
    replica.world->engineState().networkTopology = scene::NetworkTopology::Replica;
    scene::SequencePlayerComponent& sent = replica.playerOf("memory://sequence/1", "{}");
    replica.files.clear();
    CHECK(replica.sequences.sequenceOf(*replica.world, replica.player) == nullptr);
    CHECK_FALSE(replica.sequences.unreadable(*replica.world, replica.player));
    // And one it writes for itself has a name the authority's cannot be.
    REQUIRE(replica.sequences.loadSequence(*replica.world, replica.player,
                                           R"({ "format": "sequence", "version": 1, "tracks": [] })", what));
    CHECK(std::string(replica.atoms.text(sent.sequence)).rfind("memory://here/", 0) == 0);
}

TEST_CASE("ADR 0204: a timeline adds a cast track, keys who it brings there and gone, and scrubs them in")
{
    CastStage stage;
    stage.self->sequence = stage.atoms.intern("asset://sequences/new.sequence.json");
    app::Timeline timeline;
    REQUIRE(timeline.open(*stage.world, stage.sequences, stage.player));
    // A name and a stamp: and the name is not a binding the file expects.
    const core::u32 track = timeline.addTrack(asset::SequenceTrackType::Cast, "Guard", "guard");
    REQUIRE(timeline.sequence().tracks.size() == 1);
    CHECK(timeline.sequence().tracks[track].type == asset::SequenceTrackType::Cast);
    CHECK(timeline.sequence().tracks[track].target == "Guard");
    CHECK(timeline.sequence().tracks[track].model == "guard");
    CHECK(timeline.sequence().tracks[track].property.empty());
    CHECK(timeline.sequence().bindings.empty());
    // A key of another kind is not a cast track's.
    CHECK_FALSE(timeline.keyNumber(track, 0.5));
    CHECK_FALSE(timeline.keyNamed(track, "Guard"));

    timeline.scrub(0.5);
    REQUIRE(timeline.keyShown(track, true));
    timeline.scrub(2.0);
    REQUIRE(timeline.keyShown(track, false));
    REQUIRE(timeline.sequence().tracks[track].keys.size() == 2);
    CHECK(timeline.sequence().tracks[track].keys[0].show);
    CHECK_FALSE(timeline.sequence().tracks[track].keys[1].show);
    // What it writes is a file that reads back the same.
    asset::Sequence read;
    std::string why;
    REQUIRE_MESSAGE(asset::readSequence(timeline.text(), read, &why), why);
    CHECK(asset::writeSequence(read) == timeline.text());

    // And under its playhead, whoever the track brings is there between the keys.
    timeline.scrub(1.0);
    (void)timeline.update(*stage.world, stage.sequences);
    CHECK(stage.guard().valid());
    timeline.scrub(2.5);
    (void)timeline.update(*stage.world, stage.sequences);
    stage.world->retireDestroyed();
    CHECK_FALSE(stage.guard().valid());
}

TEST_CASE("a replica told of a stop the authority has not done yet does not hear the first event again")
{
    // The run that showed it: `netcode_cutscene`, once, on a loaded machine.
    // The server played a sequence again and stopped it half a second in;
    // the joiner heard `Begin` twice. A snapshot taken between the script's
    // `Stop()` and the tick that does it carried "stopped" beside the anchor
    // of the time it was playing from: the replica put itself at nought,
    // read the present off that anchor -- a tick into the sequence -- and
    // went through its first moment a second time.
    LampStage stage;
    stage.authority = false;
    // Met playing from its start.
    stage.self->playing = true;
    stage.self->anchorTime = 0.0f;
    stage.self->anchorTick = static_cast<core::u32>(stage.serverTick);
    stage.tick(20);
    std::vector<Heard> heard = stage.heard();
    REQUIRE(heard.size() == 1);
    CHECK(heard[0].name == "Begin");

    // The authority's own tick anchored it a tick in, as it does a sequence
    // that has just begun; and then the stop, as the snapshot between the
    // ask and the tick has it: stopped, and that anchor still.
    stage.self->anchorTime = static_cast<float>(Frame);
    stage.self->anchorTick = static_cast<core::u32>(stage.serverTick - 19);
    stage.self->playing = false;
    stage.self->stops += 1;
    stage.self->anchored = true;
    stage.tick();
    CHECK(stage.heard().empty());
    CHECK(stage.self->timePosition == 0.0);
    CHECK_FALSE(stage.self->playing);

    // The snapshot after it, with the stop done: at nought, and still nothing.
    stage.self->anchorTime = 0.0f;
    stage.self->anchorTick = static_cast<core::u32>(stage.serverTick);
    stage.self->anchored = true;
    stage.tick(3);
    CHECK(stage.heard().empty());
    CHECK(stage.self->timePosition == 0.0);

    // **And the ask says from when** (as `Play` and `Pause` do): a
    // stop is at nought from the tick it was asked in.
    LampStage server;
    scene::EngineState& state = server.world->engineState();
    state.networkTopology = scene::NetworkTopology::Host;
    state.networkServerTick = 640;
    server.self->timePosition = 1.25;
    server.self->anchorTime = 1.0f;
    server.self->anchorTick = 600;
    scene::anchorSequenceStopped(*server.world, server.player, *server.self);
    CHECK(server.self->anchorTick == 640);
    CHECK(server.self->anchorTime == 0.0f);
    // Where it had got to is still the host's to end it by.
    CHECK(server.self->timePosition == 1.25);
}
