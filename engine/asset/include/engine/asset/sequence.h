#pragma once

// **A sequence** (ADR 0204): a cutscene as a file -- tracks over seconds, and
// the names it expects to be given instances for. Nothing here plays
// anything: this reads `*.sequence.json` into what a player steps through,
// and answers "where is this track at this moment".
//
// The file is written to be read: a person changes a camera key in a text
// editor and the game reloads it.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/easing.h"
#include "engine/core/types.h"

namespace engine::asset {

enum class SequenceTrackType : core::u8
{
    Camera,
    Animation,
    Line,
    Sound,
    Property,
    Morph,
    Event,
    // **Where the sequence stops until it is released** (`hold`): a choice to
    // be made, a button to be pressed. A key is a time and a name.
    Hold,
    // **A named point** (`marker`): where a shot begins. Played from, and
    // skipped to.
    Marker,
    // **The picture going to one colour and back** (`fade`): a key is how
    // much of the picture the colour covers, nought to one.
    Fade,
    // **The bars of a wider picture** (`bars`): a key is how far in they
    // are, nought to one.
    Bars,
    // **Words on the screen that nobody says** (`caption`): a key is a text
    // by its key in the game's catalog, and how long it stays.
    Caption,
    // **Somebody, or something, that is there for a while** (`cast`): an
    // instance made of a stamp when the track's first key is reached, bound
    // under the track's name so other tracks play on it, shown and hidden by
    // its keys, and gone with the sequence.
    Cast,
};

// What a key holds for a property, a parameter or a shape key: a number, a
// switch, up to six numbers (a place, a colour; a place and a turn in degrees),
// or a word (an enum's item, a content name). What it MEANS is the property's
// to say -- three numbers are a vector on one and a colour on another.
struct SequenceValue
{
    enum class Kind : core::u8
    {
        None,
        Number,
        Flag,
        Numbers,
        Text,
    };
    Kind kind = Kind::None;
    bool flag = false;
    core::u8 count = 0;
    core::f64 numbers[6]{};
    std::string text;
};

struct SequenceKey
{
    core::f32 time = 0.0f;
    // How this key is arrived at from the one before it: at once, or eased.
    bool cut = false;
    core::EasingStyle style = core::EasingStyle::Linear;
    core::EasingDirection direction = core::EasingDirection::InOut;

    // --- camera ---------------------------------------------------------
    bool hasPosition = false;
    core::f64 position[3]{};
    // **Whose frame the key is in**: a bound instance by name, or something
    // under one. `position`, a `lookAt` that is a place, and `rotation` are
    // then in that instance's own space -- two metres behind it and one up,
    // wherever it stands and whichever way it faces when the key is reached.
    // Empty is the world's.
    std::string relativeTo;
    // What it looks at: a bound instance by name -- or something under one,
    // `Anna/Bust/Face`: an attachment where a face is, is how a camera is
    // aimed at one -- or a place, or neither; and then `rotation`, a turn in
    // degrees about x, y and z.
    std::string lookAtBinding;
    bool hasLookAt = false;
    core::f64 lookAt[3]{};
    bool hasRotation = false;
    core::f64 rotation[3]{};
    // Nought keeps the field of view of the key before.
    core::f32 fieldOfView = 0.0f;
    // **A hand that is not steady**: how far the camera is shaken, in metres,
    // and how many times a second. Eased between keys like its place.
    core::f32 shake[2]{};
    // With a `lookAt` that is a bound instance: the camera is brought
    // towards it until nothing that stands still is between the two.
    bool keepInSight = false;

    // --- animation --------------------------------------------------------
    std::string clip;
    core::f32 fade = 0.2f;
    bool hold = false;
    core::f32 speed = 1.0f;
    std::string parameter;

    // --- line, sound, event -----------------------------------------------
    std::string line;
    std::string sound;
    // What it is said or heard from, a binding or something under one; empty
    // is from nowhere.
    std::string at;
    std::string category;
    core::f32 volume = 1.0f;
    // An event's name; a hold's; a marker's.
    std::string name;

    // --- caption ------------------------------------------------------------
    // The text and who says it, each by its key in the game's catalog; and
    // the seconds it stays, nought for the time it takes to read.
    std::string caption;
    std::string speaker;
    core::f32 seconds = 0.0f;

    // --- property, morph, an animation parameter, an event's data ----------
    SequenceValue value;
    // **A value that is the game's to say**: written `"$Name"`, it is read
    // from the player's attribute of that name when the key is used -- which
    // travels as attributes do, so a server's is every machine's. The key
    // then has no value of its own.
    std::string variable;

    // --- cast -----------------------------------------------------------------
    // Whether it is there from this key on.
    bool show = true;

    // --- a key of a picture track ---------------------------------------------
    // **After a line, as heard**: the key is at the END of this line as the
    // machine it plays on hears it -- shorter in one language than another
    // -- with its `time` counted on from there. Only on what is each
    // machine's own picture: the camera, a fade, the bars, a shape key.
    std::string afterLine;
};

struct SequenceTrack
{
    SequenceTrackType type = SequenceTrackType::Event;
    // A binding, then a path of children's names under it: `Lamp/PointLight`.
    std::string target;
    // The property's name; a morph track's shape key.
    std::string property;
    // A fade's colour, and the shape of the picture a bars track's bars
    // leave: its width over its height.
    core::f32 color[3]{0.0f, 0.0f, 0.0f};
    core::f32 aspect = 2.39f;
    // A camera track whose place goes through its keys as one smooth curve,
    // where it is otherwise a straight line from each to the next. A cut
    // begins another curve.
    bool curve = false;
    // A cast track: the stamp it is made of, by content name. The name it is
    // bound under is the track's `target`.
    std::string model;
    // In order of time; two at one time keep the file's order.
    std::vector<SequenceKey> keys;
};

struct Sequence
{
    std::vector<std::string> bindings;
    std::vector<SequenceTrack> tracks;
    // The end of the last key, or what the file says where that is more. A
    // line's slot is the player's to add: it is not in this file.
    core::f32 length = 0.0f;
};

// **A sequence as its file's text**: what `readSequence` reads back as the
// same sequence, laid out as a person would have written it -- a track a
// paragraph, a key a line, nothing said that is its default. What the
// editor's timeline saves.
[[nodiscard]] std::string writeSequence(const Sequence& sequence);

// The word an easing is written as: `linear`, `sineInOut`.
[[nodiscard]] std::string sequenceEaseName(core::EasingStyle style, core::EasingDirection direction);
// And a track type's.
[[nodiscard]] std::string_view sequenceTrackTypeName(SequenceTrackType type) noexcept;

// Reads a sequence. False, with `what` saying why and where, for a file that
// is not one: a track of no known type, a key with no time, a camera key with
// nowhere to stand, a binding a track names that the file does not list.
[[nodiscard]] bool readSequence(std::string_view json, Sequence& out, std::string* what = nullptr);

// **Where a keyed track is at a moment.** `from` and `to` are the keys either
// side -- the same key before the first and after the last -- and `alpha` how
// far between them, already eased by the key being arrived at and nought for
// a cut that has not been reached. No keys answers false.
struct SequenceSpan
{
    core::usize from = 0;
    core::usize to = 0;
    core::f32 alpha = 0.0f;
};
[[nodiscard]] bool sequenceSpanAt(std::span<const SequenceKey> keys, core::f32 seconds, SequenceSpan& out) noexcept;

// A value part of the way from one to another: numbers are mixed; a switch
// and a word are the earlier one until the later is reached.
[[nodiscard]] SequenceValue mixSequenceValues(const SequenceValue& from, const SequenceValue& to, core::f32 alpha);

// The easing a key names: `linear`, or a style and a direction as one word --
// `sineInOut`, `quadOut`, `backIn`. False for a word that is neither.
[[nodiscard]] bool parseSequenceEase(std::string_view word, core::EasingStyle& style,
                                     core::EasingDirection& direction) noexcept;

// Splits a track's target into its binding and the path under it.
void splitSequenceTarget(std::string_view target, std::string_view& binding, std::string_view& path) noexcept;

} // namespace engine::asset
