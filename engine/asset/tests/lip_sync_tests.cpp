// The two files a mouth is moved from (ADR 0200): a track of mouth shapes
// beside a recording, and what each shape does to a face beside its model.

#include <doctest/doctest.h>
#include <string>

#include "engine/asset/lip_sync.h"

using namespace engine::asset;

TEST_CASE("ADR 0200: a viseme track is cues in order, and the file the free lip-sync tool writes is one")
{
    VisemeTrack track;
    std::string why;
    // Written out of order, as a hand might: read in order of start.
    REQUIRE(readVisemeTrack(R"({ "cues": [ { "start": 0.30, "end": 0.45, "value": "B" },
                                         { "start": 0.00, "end": 0.12, "value": "X" },
                                         { "start": 0.12, "end": 0.30, "value": "D" } ] })",
                            track, &why));
    REQUIRE(track.cues.size() == 3);
    CHECK(track.cues[0].value == "X");
    CHECK(track.cues[2].value == "B");
    CHECK(static_cast<double>(track.duration()) == doctest::Approx(0.45));

    // The cue a moment falls in: the last that has started and not ended.
    REQUIRE(track.at(0.0f) != nullptr);
    CHECK(track.at(0.0f)->value == "X");
    CHECK(track.at(0.2f)->value == "D");
    CHECK(track.at(0.44f)->value == "B");
    CHECK(track.at(0.45f) == nullptr);
    CHECK(track.at(9.0f) == nullptr);

    // As the tool writes it: its own name for the list, and what else it
    // says of the recording is not asked.
    REQUIRE(readVisemeTrack(R"({ "metadata": { "soundFile": "intro.wav", "duration": 0.5 },
                                 "mouthCues": [ { "start": 0.0, "end": 0.2, "value": "A" },
                                                { "start": 0.25, "end": 0.5, "value": "F" } ] })",
                            track, &why));
    REQUIRE(track.cues.size() == 2);
    CHECK(track.at(0.1f)->value == "A");
    // Between two cues nothing is being said.
    CHECK(track.at(0.22f) == nullptr);
    CHECK(track.at(0.3f)->value == "F");

    // What is not a track is refused whole.
    CHECK_FALSE(readVisemeTrack("not json", track, &why));
    CHECK_FALSE(why.empty());
    CHECK_FALSE(readVisemeTrack(R"({ "cues": 3 })", track, &why));
    CHECK_FALSE(readVisemeTrack(R"({ "cues": [ { "start": 0.5, "end": 0.1, "value": "A" } ] })", track, &why));
    CHECK_FALSE(readVisemeTrack(R"({ "cues": [ { "start": 0.0, "end": 0.1 } ] })", track, &why));
    CHECK(track.cues.empty());
}

TEST_CASE("ADR 0200: a face map says what a shape, a band and loudness do to one face")
{
    FaceMap map;
    std::string why;
    REQUIRE(readFaceMap(R"({ "visemes": { "D": { "JawOpen": 0.8, "MouthWide": 0.3 }, "A": { "MouthClosed": 1 },
                                          "X": {} },
                             "bands": { "open": "JawOpen", "wide": "MouthWide", "round": "MouthRound" },
                             "loudness": "JawOpen" })",
                        map, &why));
    REQUIRE(map.viseme("D") != nullptr);
    REQUIRE(map.viseme("D")->size() == 2);
    CHECK((*map.viseme("D"))[0].key == "JawOpen");
    CHECK(static_cast<double>((*map.viseme("D"))[0].weight) == doctest::Approx(0.8));
    // A shape that is the face at rest is a shape, with no key.
    REQUIRE(map.viseme("X") != nullptr);
    CHECK(map.viseme("X")->empty());
    CHECK(map.viseme("Q") == nullptr);
    CHECK(map.hasBands());
    CHECK(map.wide == "MouthWide");
    CHECK(map.loudness == "JawOpen");

    // Every part is optional: one jaw key, and no more said.
    REQUIRE(readFaceMap(R"({ "loudness": "Jaw" })", map, &why));
    CHECK_FALSE(map.hasBands());
    CHECK(map.visemes.empty());
    CHECK(map.loudness == "Jaw");

    CHECK_FALSE(readFaceMap(R"({ "visemes": { "A": { "Jaw": -1 } } })", map, &why));
    CHECK_FALSE(readFaceMap(R"({ "visemes": [1] })", map, &why));
    CHECK_FALSE(readFaceMap("[]", map, &why));
}

TEST_CASE("ADR 0200: a track is beside its recording and a face map beside its model, by name")
{
    CHECK(visemeTrackName("asset://voice/act1/intro_01.ogg") == "asset://voice/act1/intro_01.visemes.json");
    // A language's own, beside that language's recording.
    CHECK(visemeTrackName("asset://l10n/pt-BR/voice/act1/intro_01.wav") ==
          "asset://l10n/pt-BR/voice/act1/intro_01.visemes.json");
    // A dot in a folder's name is not an extension.
    CHECK(visemeTrackName("asset://voice.v2/line") == "asset://voice.v2/line.visemes.json");

    CHECK(faceMapName("asset://models/hero.glb") == "asset://models/hero.face.json");
    // A piece of a split model is its file's: one map a file.
    CHECK(faceMapName("asset://models/hero.glb#Head") == "asset://models/hero.face.json");
}
