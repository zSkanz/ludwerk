#pragma once

// **A sequence being edited** (ADR 0204, "Authoring"): what the editor's
// Timeline panel is a picture of. A working copy of one `SequencePlayer`'s
// file, a playhead, and the verbs a person has -- scrub, set a key from where
// a thing is, set a camera key from the view, move and remove keys, save.
//
// Nothing here draws or reads a mouse: the panel calls these, and so can a
// test. What the scene shows while a sequence is open is `Sequences`'
// preview, which remembers every value it wrote and puts each back -- so a
// timeline can be scrubbed all day and the scene that is saved is the scene.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/asset/sequence.h"
#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/name_atom.h"
#include "engine/core/types.h"
#include "engine/scene/value.h"

namespace engine::scene {
class World;
}

namespace engine::app {

class Sequences;

class Timeline
{
public:
    // Opens `player`'s sequence: a copy of what it plays. A player that names
    // a file that is not there yet opens on an empty sequence, which the
    // first save writes. False for a player that names no file, or a file
    // that is there and is not a sequence.
    bool open(scene::World& world, Sequences& sequences, core::InstanceId player);
    // Lets go: the scene is put back as it was, and what was not saved is
    // forgotten -- the player plays its file again.
    void close(scene::World& world, Sequences& sequences);
    [[nodiscard]] bool isOpen() const noexcept { return m_player.valid(); }
    [[nodiscard]] core::InstanceId player() const noexcept { return m_player; }
    [[nodiscard]] const asset::Sequence& sequence() const noexcept { return m_sequence; }
    [[nodiscard]] core::f64 time() const noexcept { return m_time; }
    [[nodiscard]] bool dirty() const noexcept { return m_dirty; }
    [[nodiscard]] core::NameAtom file() const noexcept { return m_file; }

    // **Once a frame while it is open.** Shows the playhead's moment -- again,
    // if the copy changed or something put the scene back -- and, for each
    // track that is recording, sets a key where its property is no longer
    // what the sequence put there: moved in the viewport, typed into
    // Properties, written by anything. How many keys it set.
    core::u32 update(scene::World& world, Sequences& sequences);
    // Moves the playhead; `update` shows it.
    void scrub(core::f64 seconds) noexcept;
    // The scene is about to be saved or played: it is put back first, and the
    // next `update` shows the moment again.
    void settle(scene::World& world, Sequences& sequences);

    // --- Tracks -------------------------------------------------------------
    //
    // A track of `type` on `target` (a binding, then a path under it); the
    // binding joins the file's list if it is new. `property` is the
    // property, or a morph track's shape key. Its index.
    core::u32 addTrack(asset::SequenceTrackType type, std::string_view target, std::string_view property);
    void removeTrack(core::u32 track);
    void setRecording(core::u32 track, bool on);
    [[nodiscard]] bool recording(core::u32 track) const noexcept;
    // Its last key's time, or what was asked where that is more.
    void setLength(core::f32 seconds);

    // --- Keys ---------------------------------------------------------------
    //
    // A key at the playhead on a property track, from what the property is
    // now. False for a track with nothing bound, or a value a key cannot hold.
    bool keyFromScene(const scene::World& world, const Sequences& sequences, core::u32 track);
    // A camera key at the playhead from a view: where it stands, how it is
    // turned, its lens. The camera track is made if there is none. The key's
    // index in it.
    core::u32 keyCamera(const core::CFrameD& view, core::f32 fieldOfView, bool cut);
    // A key at the playhead that is a name: an event's name, a line's id, a
    // sound's content, a clip (`file#Clip`). False for a track of another kind.
    bool keyNamed(core::u32 track, std::string_view name);
    // And one that is a number: a shape key's weight.
    bool keyNumber(core::u32 track, core::f64 value);
    // And one on a cast track: whoever it brings is there from the playhead on,
    // or gone from it.
    bool keyShown(core::u32 track, bool shown);
    // Moves a key in time; its index afterwards.
    core::u32 moveKey(core::u32 track, core::u32 key, core::f32 seconds);
    void removeKey(core::u32 track, core::u32 key);
    void setCut(core::u32 track, core::u32 key, bool cut);
    void setEase(core::u32 track, core::u32 key, core::EasingStyle style, core::EasingDirection direction);

    // The file as it would be saved, and that it was.
    [[nodiscard]] std::string text() const;
    void saved() noexcept { m_dirty = false; }

private:
    void touched() noexcept;
    [[nodiscard]] core::u32 place(std::vector<asset::SequenceKey>& keys, asset::SequenceKey key);

    core::InstanceId m_player;
    core::NameAtom m_file;
    asset::Sequence m_sequence;
    core::f64 m_time = 0.0;
    core::f64 m_shownTime = -1.0;
    bool m_dirty = false;
    // The copy changed since `Sequences` was last given it.
    bool m_stale = false;
    // The reading of the file the copy was taken from, or given as.
    core::u32 m_revision = 0;
    // Frames a clip the scrub wants has been waited for.
    core::u32 m_waited = 0;
    // A track each: whether it records, and what its property was when the
    // moment was last shown.
    std::vector<bool> m_recording;
    std::vector<std::optional<scene::Value>> m_shown;
};

// **A property's value as a key holds one**: a number, a switch, a word, or
// up to six numbers -- a `CFrame` as its place and its turn in degrees, or as
// its place alone when `like` is a key of three. False for a value no key can
// hold. An enum's item is its name.
[[nodiscard]] bool sequenceValueOf(const scene::World& world, const scene::Value& value,
                                   const asset::SequenceValue* like, asset::SequenceValue& out);

} // namespace engine::app
