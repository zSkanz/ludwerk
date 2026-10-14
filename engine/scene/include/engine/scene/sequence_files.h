// The seam between a `SequencePlayer`'s properties and the files the host
// reads (ADR 0204), shaped as `animation_host.h` is and for its reason.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/name_atom.h"
#include "engine/scene/types.h"

namespace engine::scene {

class World;

// **What the host knows of a sequence's file** (ADR 0204): how long it is --
// its last key, or its last line's slot. The host reads the file and measures
// it; a property asks. As `AnimationHost` is for a clip: `app` implements it,
// and neither `render`, which declares `SequencePlayer`, nor `script` includes
// the other.
// A named point of a sequence: where a shot begins.
struct SequenceMarker
{
    std::string name;
    f32 time = 0.0f;
};

class SequenceFiles
{
public:
    virtual ~SequenceFiles() = default;
    // Its markers, in order of time. None for a file that is not there.
    virtual void sequenceMarkers(const World& world, core::NameAtom file, std::vector<SequenceMarker>& out) = 0;
    // The name of the hold AT a moment of it, or nothing.
    [[nodiscard]] virtual std::string sequenceHoldAt(const World& world, core::NameAtom file, f64 seconds) = 0;
    // Nought for a file that is not there or is not a sequence, which the
    // host has said once where it looked for it.
    [[nodiscard]] virtual f32 sequenceLength(const World& world, core::NameAtom file) = 0;
    // **A sequence a script wrote** (`LoadSequence`): `json` is a file's text,
    // and `player` plays it from now. False, with `what` saying where, for a
    // text that is not a sequence -- and then the player is left as it was.
    [[nodiscard]] virtual bool loadSequence(World& world, core::InstanceId player, std::string_view json,
                                            std::string& what) = 0;
};

} // namespace engine::scene
