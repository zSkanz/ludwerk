#pragma once

// **A mouth that moves with what is said** (ADR 0200): the two files it is
// moved from. Nothing here plays anything -- the engine READS a track of
// mouth shapes; making one from a recording is a tool's job, and no speech
// recognition is in the engine.
//
// - A **viseme track** sits beside a recording under the recording's own
//   name: `voice/act1/intro_01.visemes.json` beside `intro_01.ogg`, and a
//   language's own beside that language's recording under `l10n/<locale>/`.
//   It is a list of cues, each a mouth shape's name over a stretch of time.
// - A **face map** sits beside a model the way its rig's roles do --
//   `hero.glb`, `hero.face.json` -- and says what each shape, each frequency
//   band and loudness itself do to THAT face's shape keys.
//
// A track is data with a start and an end, by name: a sequencer puts it on a
// timeline as it is.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/types.h"

namespace engine::asset {

// One mouth shape, held from `start` to `end`, in seconds of the recording.
struct VisemeCue
{
    core::f32 start = 0.0f;
    core::f32 end = 0.0f;
    // The shape's name, as the tool that made the track calls it: a letter,
    // a phoneme class. What it does to a face is the face map's to say.
    std::string value;
};

struct VisemeTrack
{
    // In order of `start`.
    std::vector<VisemeCue> cues;

    // The cue a moment falls in, or null between cues and past the last.
    [[nodiscard]] const VisemeCue* at(core::f32 seconds) const noexcept;
    [[nodiscard]] core::f32 duration() const noexcept { return cues.empty() ? 0.0f : cues.back().end; }
};

// Reads a track: `{ "cues": [ { "start": 0.0, "end": 0.12, "value": "A" } ] }`.
// The file the common free lip-sync tool writes is read as it is -- its list
// is called `mouthCues`, and whatever else it says is not asked. False, with
// `what` saying why, for anything that is neither.
[[nodiscard]] bool readVisemeTrack(std::string_view json, VisemeTrack& out, std::string* what = nullptr);

// The name a recording's track is looked for under: the recording's own with
// `.visemes.json` where its extension was.
[[nodiscard]] std::string visemeTrackName(std::string_view sound);

// One shape key and how far it goes.
struct FaceKey
{
    std::string key;
    core::f32 weight = 1.0f;
};

// What a mouth shape, a frequency band and loudness do to one face.
struct FaceMap
{
    // A shape's name to the keys it sets, sorted by name.
    std::vector<std::pair<std::string, std::vector<FaceKey>>> visemes;
    // The keys the three bands drive: a mouth that opens, widens, rounds.
    std::string open;
    std::string wide;
    std::string round;
    // The one key loudness alone opens.
    std::string loudness;

    [[nodiscard]] const std::vector<FaceKey>* viseme(std::string_view name) const noexcept;
    [[nodiscard]] bool hasBands() const noexcept { return !open.empty() || !wide.empty() || !round.empty(); }
};

// Reads a face map:
//
//     { "visemes": { "A": { "MouthClosed": 1 }, "D": { "JawOpen": 0.8, "MouthWide": 0.3 }, "X": {} },
//       "bands": { "open": "JawOpen", "wide": "MouthWide", "round": "MouthRound" },
//       "loudness": "JawOpen" }
//
// Every part is optional: a face with one jaw key says `loudness` and no more.
[[nodiscard]] bool readFaceMap(std::string_view json, FaceMap& out, std::string* what = nullptr);

// The name a model's face map is looked for under: the model's own, less the
// piece a split model names after `#`, with `.face.json` where its extension
// was.
[[nodiscard]] std::string faceMapName(std::string_view model);

} // namespace engine::asset
