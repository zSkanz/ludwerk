// A sequence as a file (ADR 0204): what is read out of `*.sequence.json`, what
// is refused with a place to look, and where a keyed track is at a moment.

#include <doctest/doctest.h>
#include <ostream>
#include <string>
#include <string_view>

#include "engine/asset/sequence.h"

using namespace engine::asset;
using engine::core::EasingDirection;
using engine::core::EasingStyle;

namespace {

constexpr const char* kMeeting = R"({
  "format": "sequence",
  "version": 1,
  "bindings": ["Anna", "Bruno", "Lamp"],
  "tracks": [
    { "type": "camera", "keys": [
        { "time": 3, "position": [1, 1.6, 3], "lookAt": "Anna", "fieldOfView": 35, "ease": "sineInOut" },
        { "time": 0, "position": [4, 1.7, 6], "lookAt": "Anna", "fieldOfView": 50, "cut": true },
        { "time": 3, "position": [-2, 1.7, 2], "lookAt": [0, 1, 0], "cut": true } ] },
    { "type": "animation", "target": "Anna", "keys": [
        { "time": 0.5, "clip": "asset://anim/wave.glb", "fade": 0.3, "hold": true },
        { "time": 4, "parameter": "Talking", "value": true } ] },
    { "type": "line", "keys": [ { "time": 3.2, "line": "meeting.anna_1", "at": "Anna" } ] },
    { "type": "sound", "keys": [ { "time": 0, "sound": "asset://music/meeting.ogg", "category": "Music", "volume": 0.5 } ] },
    { "type": "property", "target": "Lamp/PointLight", "property": "Brightness", "keys": [
        { "time": 5, "value": 0 }, { "time": 6, "value": 3, "ease": "quadOut" } ] },
    { "type": "morph", "target": "Anna/Face", "key": "Smile", "keys": [
        { "time": 6, "value": 0 }, { "time": 6.5, "value": 1 } ] },
    { "type": "event", "keys": [ { "time": 6, "name": "LampOn", "data": "warm" } ] }
  ]
})";

[[nodiscard]] std::string refusal(const char* json)
{
    Sequence sequence;
    std::string why;
    CHECK_FALSE(readSequence(json, sequence, &why));
    CHECK(sequence.tracks.empty());
    return why;
}

} // namespace

TEST_CASE("ADR 0204: a sequence is its bindings and its tracks, each track's keys in order of time")
{
    Sequence sequence;
    std::string why;
    REQUIRE_MESSAGE(readSequence(kMeeting, sequence, &why), why);
    REQUIRE(sequence.bindings.size() == 3);
    REQUIRE(sequence.tracks.size() == 7);
    // The end of the last key.
    CHECK(static_cast<double>(sequence.length) == doctest::Approx(6.5));

    // The camera's keys were written out of order; and of two at one moment
    // the file's order is kept -- the move's end, then the cut that follows.
    const SequenceTrack& camera = sequence.tracks[0];
    CHECK(camera.type == SequenceTrackType::Camera);
    REQUIRE(camera.keys.size() == 3);
    CHECK(camera.keys[0].time == 0.0f);
    CHECK(camera.keys[0].cut);
    CHECK(camera.keys[1].lookAtBinding == "Anna");
    CHECK(camera.keys[1].style == EasingStyle::Sine);
    CHECK(camera.keys[1].direction == EasingDirection::InOut);
    CHECK(static_cast<double>(camera.keys[1].fieldOfView) == doctest::Approx(35.0));
    CHECK(camera.keys[2].cut);
    CHECK(camera.keys[2].hasLookAt);
    CHECK(camera.keys[2].lookAtBinding.empty());
    // No field of view said: the key before it keeps its own.
    CHECK(camera.keys[2].fieldOfView == 0.0f);

    const SequenceTrack& acting = sequence.tracks[1];
    CHECK(acting.target == "Anna");
    CHECK(acting.keys[0].clip == "asset://anim/wave.glb");
    CHECK(static_cast<double>(acting.keys[0].fade) == doctest::Approx(0.3));
    CHECK(acting.keys[0].hold);
    CHECK(acting.keys[1].parameter == "Talking");
    CHECK(acting.keys[1].value.kind == SequenceValue::Kind::Flag);
    CHECK(acting.keys[1].value.flag);

    CHECK(sequence.tracks[2].keys[0].line == "meeting.anna_1");
    CHECK(sequence.tracks[2].keys[0].at == "Anna");
    CHECK(sequence.tracks[3].keys[0].category == "Music");
    CHECK(static_cast<double>(sequence.tracks[3].keys[0].volume) == doctest::Approx(0.5));

    const SequenceTrack& lamp = sequence.tracks[4];
    CHECK(lamp.target == "Lamp/PointLight");
    CHECK(lamp.property == "Brightness");
    std::string_view binding;
    std::string_view path;
    splitSequenceTarget(lamp.target, binding, path);
    CHECK(binding == "Lamp");
    CHECK(path == "PointLight");
    splitSequenceTarget("Anna", binding, path);
    CHECK(binding == "Anna");
    CHECK(path.empty());

    CHECK(sequence.tracks[5].property == "Smile");
    CHECK(sequence.tracks[6].keys[0].name == "LampOn");
    CHECK(sequence.tracks[6].keys[0].value.text == "warm");

    // A file may say it is longer than its last key.
    Sequence padded;
    REQUIRE(readSequence(R"({ "format": "sequence", "version": 1, "length": 12, "tracks": [
        { "type": "event", "keys": [ { "time": 2, "name": "A" } ] } ] })",
                         padded, &why));
    CHECK(static_cast<double>(padded.length) == doctest::Approx(12.0));
}

TEST_CASE("ADR 0204: a file that is not a sequence is refused, with where to look")
{
    CHECK(refusal("{") != "");
    CHECK(refusal(R"({ "format": "scene", "version": 1, "tracks": [] })") == "format");
    CHECK(refusal(R"({ "format": "sequence", "version": 2, "tracks": [] })") == "version");
    CHECK(refusal(R"({ "format": "sequence", "version": 1 })") == "tracks");
    CHECK(refusal(R"({ "format": "sequence", "version": 1, "tracks": [ { "type": "dance", "keys": [] } ] })") ==
          "tracks[0].type");
    // A key with no time.
    CHECK(refusal(R"({ "format": "sequence", "version": 1, "tracks": [
        { "type": "event", "keys": [ { "name": "A" } ] } ] })") == "tracks[0].keys[0].time");
    // A camera with nowhere to stand.
    CHECK(refusal(R"({ "format": "sequence", "version": 1, "tracks": [
        { "type": "camera", "keys": [ { "time": 0, "lookAt": [0, 0, 0] } ] } ] })") == "tracks[0].keys[0].position");
    // **A binding a track names that the file does not list**: a typo is
    // otherwise a track that moves nothing and says nothing.
    CHECK(refusal(R"({ "format": "sequence", "version": 1, "bindings": ["Lamp"], "tracks": [
        { "type": "property", "target": "Lmap/PointLight", "property": "Brightness",
          "keys": [ { "time": 0, "value": 1 } ] } ] })") == "tracks[0].target");
    CHECK(refusal(R"({ "format": "sequence", "version": 1, "bindings": ["Anna"], "tracks": [
        { "type": "camera", "keys": [ { "time": 0, "position": [0, 0, 0], "lookAt": "Ana" } ] } ] })") ==
          "tracks[0].keys[0].lookAt");
    CHECK(refusal(R"({ "format": "sequence", "version": 1, "bindings": ["Anna", "Anna"], "tracks": [] })") ==
          "bindings[1]");
    // An easing that is not one.
    CHECK(refusal(R"({ "format": "sequence", "version": 1, "tracks": [
        { "type": "event", "keys": [ { "time": 0, "name": "A", "ease": "wobble" } ] } ] })") ==
          "tracks[0].keys[0].ease");
    // A clip AND a parameter in one key, and neither.
    CHECK(refusal(R"({ "format": "sequence", "version": 1, "bindings": ["A"], "tracks": [
        { "type": "animation", "target": "A", "keys": [ { "time": 0 } ] } ] })") == "tracks[0].keys[0]");
    // A property keyed with a number and then with a word.
    CHECK(refusal(R"({ "format": "sequence", "version": 1, "bindings": ["A"], "tracks": [
        { "type": "property", "target": "A", "property": "Size",
          "keys": [ { "time": 0, "value": 1 }, { "time": 1, "value": "big" } ] } ] })") == "tracks[0].keys");
    // A shape key's weight is a number.
    CHECK(refusal(R"({ "format": "sequence", "version": 1, "bindings": ["A"], "tracks": [
        { "type": "morph", "target": "A", "key": "Smile", "keys": [ { "time": 0, "value": true } ] } ] })") ==
          "tracks[0].keys[0].value");
}

TEST_CASE("ADR 0204: a keyed track is eased between its keys, and a cut is not moved towards")
{
    Sequence sequence;
    REQUIRE(readSequence(kMeeting, sequence));
    const std::vector<SequenceKey>& camera = sequence.tracks[0].keys;
    SequenceSpan span;

    // Half way into the move: between the first key and the second, eased --
    // a sine in and out is exactly a half at its middle.
    REQUIRE(sequenceSpanAt(camera, 1.5f, span));
    CHECK(span.from == 0);
    CHECK(span.to == 1);
    CHECK(static_cast<double>(span.alpha) == doctest::Approx(0.5));
    // A quarter of the way in time is less than a quarter of the way there.
    REQUIRE(sequenceSpanAt(camera, 0.75f, span));
    CHECK(span.alpha < 0.25f);
    CHECK(span.alpha > 0.0f);

    // At three seconds the move has ended AND the cut has come: the picture is
    // the cut's from that moment on.
    REQUIRE(sequenceSpanAt(camera, 3.0f, span));
    CHECK(span.from == 2);
    CHECK(span.to == 2);
    // And a moment before, it is still arriving at the move's end.
    REQUIRE(sequenceSpanAt(camera, 2.99f, span));
    CHECK(span.to == 1);
    CHECK(span.alpha > 0.99f);
    // Past the last key, the last key.
    REQUIRE(sequenceSpanAt(camera, 60.0f, span));
    CHECK(span.from == 2);

    // Before a track's first key it is at its first key.
    const std::vector<SequenceKey>& lamp = sequence.tracks[4].keys;
    REQUIRE(sequenceSpanAt(lamp, 1.0f, span));
    CHECK(span.from == 0);
    CHECK(span.to == 0);
    REQUIRE(sequenceSpanAt(lamp, 5.5f, span));
    const SequenceValue half = mixSequenceValues(lamp[span.from].value, lamp[span.to].value, span.alpha);
    // Eased out: past half of the way there at half of the time.
    CHECK(half.numbers[0] > 1.5);
    CHECK(half.numbers[0] < 3.0);

    // No keys is no answer.
    CHECK_FALSE(sequenceSpanAt({}, 1.0f, span));
}

TEST_CASE("ADR 0204: numbers are mixed, and a switch or a word waits for the later key")
{
    SequenceValue from;
    from.kind = SequenceValue::Kind::Numbers;
    from.count = 3;
    from.numbers[0] = 0.0;
    from.numbers[1] = 10.0;
    from.numbers[2] = -4.0;
    SequenceValue to = from;
    to.numbers[0] = 2.0;
    to.numbers[1] = 20.0;
    to.numbers[2] = 4.0;
    const SequenceValue middle = mixSequenceValues(from, to, 0.25f);
    CHECK(middle.numbers[0] == doctest::Approx(0.5));
    CHECK(middle.numbers[1] == doctest::Approx(12.5));
    CHECK(middle.numbers[2] == doctest::Approx(-2.0));

    SequenceValue off;
    off.kind = SequenceValue::Kind::Flag;
    SequenceValue on = off;
    on.flag = true;
    CHECK_FALSE(mixSequenceValues(off, on, 0.99f).flag);
    CHECK(mixSequenceValues(off, on, 1.0f).flag);

    EasingStyle style = EasingStyle::Linear;
    EasingDirection direction = EasingDirection::In;
    CHECK(parseSequenceEase("backOut", style, direction));
    CHECK(style == EasingStyle::Back);
    CHECK(direction == EasingDirection::Out);
    CHECK(parseSequenceEase("linear", style, direction));
    CHECK(style == EasingStyle::Linear);
    CHECK_FALSE(parseSequenceEase("sine", style, direction));
    CHECK_FALSE(parseSequenceEase("", style, direction));
}

TEST_CASE("ADR 0204: a sequence written down is read back the same, and reads as a person would have written it")
{
    Sequence sequence;
    REQUIRE(readSequence(kMeeting, sequence));
    const std::string text = writeSequence(sequence);
    Sequence again;
    std::string why;
    REQUIRE_MESSAGE(readSequence(text, again, &why), why);
    // Written a second time it is the same text: nothing drifts.
    CHECK(writeSequence(again) == text);
    REQUIRE(again.tracks.size() == sequence.tracks.size());
    CHECK(again.bindings == sequence.bindings);
    CHECK(again.length == sequence.length);
    for (std::size_t track = 0; track < sequence.tracks.size(); ++track) {
        REQUIRE(again.tracks[track].keys.size() == sequence.tracks[track].keys.size());
        CHECK(again.tracks[track].type == sequence.tracks[track].type);
        CHECK(again.tracks[track].target == sequence.tracks[track].target);
        CHECK(again.tracks[track].property == sequence.tracks[track].property);
        for (std::size_t key = 0; key < sequence.tracks[track].keys.size(); ++key) {
            const SequenceKey& a = sequence.tracks[track].keys[key];
            const SequenceKey& b = again.tracks[track].keys[key];
            CHECK(a.time == b.time);
            CHECK(a.cut == b.cut);
            CHECK(a.style == b.style);
            CHECK(a.direction == b.direction);
            CHECK(a.fieldOfView == b.fieldOfView);
            CHECK(a.lookAtBinding == b.lookAtBinding);
            CHECK(a.clip == b.clip);
            CHECK(a.fade == b.fade);
            CHECK(a.hold == b.hold);
            CHECK(a.line == b.line);
            CHECK(a.volume == b.volume);
            CHECK(a.name == b.name);
            CHECK(a.value.kind == b.value.kind);
            CHECK(a.value.numbers[0] == b.value.numbers[0]);
            CHECK(a.value.text == b.value.text);
        }
    }
    // A key a line, and nothing said that is its default.
    CHECK(text.find(R"({ "time": 0.5, "clip": "asset://anim/wave.glb", "fade": 0.3, "hold": true })") !=
          std::string::npos);
    CHECK(text.find(R"({ "time": 6, "name": "LampOn", "data": "warm" })") != std::string::npos);
    CHECK(text.find(R"({ "time": 6, "value": 3, "ease": "quadOut" })") != std::string::npos);
    CHECK(text.find("\"speed\"") == std::string::npos);
    CHECK(text.find("\"length\"") == std::string::npos);

    // A word with a quote in it is still a word.
    sequence.tracks[6].keys[0].value.text = "say \"warm\"\n";
    REQUIRE(readSequence(writeSequence(sequence), again));
    CHECK(again.tracks[6].keys[0].value.text == "say \"warm\"\n");
    // And a length past the last key is said.
    sequence.length = 30.0f;
    CHECK(writeSequence(sequence).find("\"length\": 30") != std::string::npos);
    CHECK(sequenceEaseName(EasingStyle::Linear, EasingDirection::InOut) == "linear");
    CHECK(sequenceEaseName(EasingStyle::Back, EasingDirection::Out) == "backOut");
}

TEST_CASE("ADR 0204: holds, markers, a fade, bars, captions and a camera key in somebody's frame are read and written")
{
    constexpr const char* text = R"({
      "format": "sequence", "version": 1, "bindings": ["Anna"],
      "tracks": [
        { "type": "camera", "keys": [
            { "time": 0, "position": [0, 1, 3], "relativeTo": "Anna", "lookAt": [0, 1.5, 0], "cut": true } ] },
        { "type": "marker", "keys": [ { "time": 0, "name": "Opening" }, { "time": 4, "name": "Choice" } ] },
        { "type": "hold", "keys": [ { "time": 5, "name": "Answer" } ] },
        { "type": "fade", "color": [1, 0.5, 0], "keys": [ { "time": 0, "value": 1 }, { "time": 1, "value": 0 } ] },
        { "type": "bars", "aspect": 1.85, "keys": [ { "time": 0, "value": 1 } ] },
        { "type": "caption", "keys": [
            { "time": 2, "caption": "story.later", "speaker": "story.narrator", "seconds": 3 } ] }
      ]
    })";
    Sequence sequence;
    std::string why;
    REQUIRE_MESSAGE(readSequence(text, sequence, &why), why);
    REQUIRE(sequence.tracks.size() == 6);
    CHECK(sequence.tracks[0].keys[0].relativeTo == "Anna");
    CHECK(sequence.tracks[1].type == SequenceTrackType::Marker);
    CHECK(sequence.tracks[1].keys[1].name == "Choice");
    CHECK(sequence.tracks[2].type == SequenceTrackType::Hold);
    CHECK(sequence.tracks[3].type == SequenceTrackType::Fade);
    CHECK(static_cast<double>(sequence.tracks[3].color[1]) == doctest::Approx(0.5));
    CHECK(sequence.tracks[4].type == SequenceTrackType::Bars);
    CHECK(static_cast<double>(sequence.tracks[4].aspect) == doctest::Approx(1.85));
    CHECK(sequence.tracks[5].keys[0].caption == "story.later");
    CHECK(sequence.tracks[5].keys[0].speaker == "story.narrator");
    CHECK(static_cast<double>(sequence.tracks[5].keys[0].seconds) == doctest::Approx(3.0));
    CHECK(static_cast<double>(sequence.length) == doctest::Approx(5.0));

    // Written and read again it is the same sequence, and the same text.
    const std::string written = writeSequence(sequence);
    Sequence again;
    REQUIRE_MESSAGE(readSequence(written, again, &why), why);
    CHECK(writeSequence(again) == written);
    CHECK(again.tracks[0].keys[0].relativeTo == "Anna");
    CHECK(static_cast<double>(again.tracks[3].color[0]) == doctest::Approx(1.0));
    CHECK(static_cast<double>(again.tracks[4].aspect) == doctest::Approx(1.85));
    CHECK(again.tracks[5].keys[0].speaker == "story.narrator");

    // And each refuses what it cannot mean, saying where.
    const auto refused = [&](const char* tracks) {
        Sequence none;
        std::string where;
        const std::string file =
            std::string("{ \"format\": \"sequence\", \"version\": 1, \"bindings\": [\"Anna\"], \"tracks\": [") +
            tracks + "] }";
        CHECK_FALSE(readSequence(file, none, &where));
        return where;
    };
    CHECK(refused(R"({ "type": "hold", "keys": [ { "time": 1 } ] })") == "tracks[0].keys[0].name");
    CHECK(refused(R"({ "type": "fade", "keys": [ { "time": 1, "value": 2 } ] })") == "tracks[0].keys[0].value");
    CHECK(refused(R"({ "type": "bars", "aspect": 0.5, "keys": [] })") == "tracks[0].aspect");
    CHECK(refused(R"({ "type": "caption", "keys": [ { "time": 1 } ] })") == "tracks[0].keys[0].caption");
    CHECK(refused(R"({ "type": "camera", "keys": [ { "time": 0, "position": [0, 0, 0], "relativeTo": "Bruno" } ] })") ==
          "tracks[0].keys[0].relativeTo");
}

TEST_CASE("ADR 0204: a cast, a curved and shaken camera, a key after a line and a variable are read and written")
{
    constexpr const char* text = R"({
      "format": "sequence", "version": 1, "bindings": ["Anna"],
      "tracks": [
        { "type": "camera", "path": "curve", "keys": [
            { "time": 0, "position": [0, 1, 3], "lookAt": "Anna", "keepInSight": true, "shake": [0.05, 4] },
            { "time": 2, "position": [2, 1, 3], "lookAt": "Guard" },
            { "time": 0.25, "afterLine": "meeting.anna_1", "position": [4, 1, 3], "lookAt": "Anna", "cut": true } ] },
        { "type": "line", "keys": [ { "time": 1, "line": "meeting.anna_1", "at": "Guard" } ] },
        { "type": "cast", "as": "Guard", "model": "asset://stamps/guard.stamp.json", "keys": [
            { "time": 0.5 }, { "time": 3, "show": false } ] },
        { "type": "animation", "target": "Guard", "keys": [ { "time": 0.5, "clip": "Salute" } ] },
        { "type": "property", "target": "Guard", "property": "Fade", "keys": [
            { "time": 0.5, "value": 1 }, { "time": 1, "value": "$Seen" } ] },
        { "type": "fade", "keys": [ { "time": 0, "value": "$Cover" }, { "time": 1, "value": 0 } ] }
      ]
    })";
    Sequence sequence;
    std::string why;
    REQUIRE_MESSAGE(readSequence(text, sequence, &why), why);
    REQUIRE(sequence.tracks.size() == 6);
    // Who a cast track brings is a name any track may play on, and not one
    // the file expects to be given.
    CHECK(sequence.bindings == std::vector<std::string>{"Anna"});
    const SequenceTrack& camera = sequence.tracks[0];
    CHECK(camera.curve);
    REQUIRE(camera.keys.size() == 3);
    CHECK(camera.keys[0].keepInSight);
    CHECK(static_cast<double>(camera.keys[0].shake[0]) == doctest::Approx(0.05));
    CHECK(static_cast<double>(camera.keys[0].shake[1]) == doctest::Approx(4.0));
    // In the file's order of time, where its own time is all a key has: the
    // one after a line is placed by whoever plays it.
    CHECK(camera.keys[1].afterLine == "meeting.anna_1");
    CHECK(static_cast<double>(camera.keys[1].time) == doctest::Approx(0.25));
    CHECK(camera.keys[2].lookAtBinding == "Guard");
    const SequenceTrack& cast = sequence.tracks[2];
    CHECK(cast.type == SequenceTrackType::Cast);
    CHECK(cast.target == "Guard");
    CHECK(cast.model == "asset://stamps/guard.stamp.json");
    CHECK(cast.keys[0].show);
    CHECK_FALSE(cast.keys[1].show);
    CHECK(sequence.tracks[4].keys[1].variable == "Seen");
    CHECK(sequence.tracks[4].keys[1].value.kind == SequenceValue::Kind::None);
    CHECK(sequence.tracks[5].keys[0].variable == "Cover");

    const std::string written = writeSequence(sequence);
    Sequence again;
    REQUIRE_MESSAGE(readSequence(written, again, &why), why);
    CHECK(writeSequence(again) == written);
    CHECK(written.find("\"path\": \"curve\"") != std::string::npos);
    CHECK(written.find("\"as\": \"Guard\"") != std::string::npos);
    CHECK(written.find("\"value\": \"$Seen\"") != std::string::npos);
    CHECK(written.find("\"afterLine\": \"meeting.anna_1\"") != std::string::npos);
    CHECK(written.find("\"shake\": [0.05, 4]") != std::string::npos);
}

TEST_CASE("ADR 0204: a string that begins with a dollar is written with two, and read as the string it was")
{
    const std::string text = R"({
      "format": "sequence", "version": 1, "bindings": ["Sign"],
      "tracks": [
        { "type": "property", "target": "Sign", "property": "Text", "keys": [
            { "time": 0, "value": "$$5 a ticket" }, { "time": 1, "value": "$" }, { "time": 2, "value": "$Price" } ] }
      ]
    })";
    Sequence sequence;
    std::string why;
    REQUIRE_MESSAGE(readSequence(text, sequence, &why), why);
    const std::vector<SequenceKey>& keys = sequence.tracks[0].keys;
    REQUIRE(keys.size() == 3);
    CHECK(keys[0].variable.empty());
    CHECK(keys[0].value.text == "$5 a ticket");
    // One dollar alone names nothing, and is the string it is.
    CHECK(keys[1].variable.empty());
    CHECK(keys[1].value.text == "$");
    CHECK(keys[2].variable == "Price");

    const std::string written = writeSequence(sequence);
    CHECK(written.find("\"value\": \"$$5 a ticket\"") != std::string::npos);
    Sequence again;
    REQUIRE_MESSAGE(readSequence(written, again, &why), why);
    CHECK(again.tracks[0].keys[0].value.text == "$5 a ticket");
    CHECK(again.tracks[0].keys[1].value.text == "$");
    CHECK(again.tracks[0].keys[2].variable == "Price");
    CHECK(writeSequence(again) == written);
}

TEST_CASE("ADR 0204: what the new keys cannot mean is refused, with where")
{
    const auto refused = [](const char* tracks) {
        const std::string text =
            std::string(R"({ "format": "sequence", "version": 1, "bindings": ["Anna"], "tracks": [ )") + tracks +
            " ] }";
        Sequence sequence;
        std::string why;
        CHECK_FALSE(readSequence(text, sequence, &why));
        return why;
    };
    // A cast with nothing to be made of, with no name, or under a name the
    // file already expects to be given.
    CHECK(refused(R"({ "type": "cast", "as": "Guard", "keys": [] })") == "tracks[0].model");
    CHECK(refused(R"({ "type": "cast", "model": "guard", "keys": [] })") == "tracks[0].as");
    CHECK(refused(R"({ "type": "cast", "as": "Anna", "model": "guard", "keys": [] })") == "tracks[0].as");
    // Kept in sight of nobody.
    CHECK(refused(R"({ "type": "camera", "keys": [ { "time": 0, "position": [0, 0, 0], "keepInSight": true } ] })") ==
          "tracks[0].keys[0].keepInSight");
    CHECK(refused(R"({ "type": "camera", "keys": [ { "time": 0, "position": [0, 0, 0], "shake": [0.1] } ] })") ==
          "tracks[0].keys[0].shake");
    CHECK(refused(R"({ "type": "camera", "path": "spiral", "keys": [] })") == "tracks[0].path");
    // **After a line is a picture's alone**: what moves the world keeps the
    // simulation's clock, which is every machine's.
    CHECK(refused(R"({ "type": "line", "keys": [ { "time": 0, "line": "a.b" } ] },
                     { "type": "event", "keys": [ { "time": 0, "afterLine": "a.b", "name": "Now" } ] })") ==
          "tracks[1].keys[0].afterLine");
    CHECK(refused(R"({ "type": "line", "keys": [ { "time": 0, "line": "a.b" } ] },
                     { "type": "property", "target": "Anna", "property": "Fade",
                       "keys": [ { "time": 0, "afterLine": "a.b", "value": 1 } ] })") == "tracks[1].keys[0].afterLine");
    // And after a line the file does not say.
    CHECK(refused(R"({ "type": "fade", "keys": [ { "time": 0, "afterLine": "a.b", "value": 1 } ] })") ==
          "tracks[0].keys[0].afterLine");
}
