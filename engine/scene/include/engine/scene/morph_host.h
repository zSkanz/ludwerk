// A mesh's morph targets and their weights, from below `render` (ADR 0196).
//
// The fourth instance of the pattern `IPhysics3D*`, `AnimationHost*` and
// `SkeletonHost*` established: an interface in `scene`, an implementation in
// the module that owns the data, and `app` holding the wire. What a script's
// `MeshPart:SetMorphWeight` reaches.
//
// **A weight is picture, not simulation.** Nothing in the world's state reads
// one: it is not in the hash, not replicated and not in a replay, and what a
// script sets here is kept on the machine the script ran on and no other. A
// clip's weight channels are no different in that -- a track plays where it
// was played, and animation is not on the wire -- but every machine that
// plays the same clip works the same weights out of it.
//
// **A target is named by its name.** The file may not have arrived when a
// script first asks -- a mesh loads over frames -- and an index means nothing
// until it has; a name set early is kept and takes effect when the mesh does.
#pragma once

#include <string_view>

#include "engine/core/id.h"
#include "engine/core/types.h"

namespace engine::scene {

class MorphHost
{
public:
    virtual ~MorphHost() = default;

    // How many targets the mesh has, in its file's order; nought for a mesh
    // with none and for one that has not loaded.
    [[nodiscard]] virtual core::u32 morphTargetCount(core::InstanceId meshPart) const = 0;
    // Empty for a target the mesh does not have.
    [[nodiscard]] virtual std::string_view morphTargetName(core::InstanceId meshPart, core::u32 target) const = 0;

    // **The weight the mesh is drawn with on this machine**: what a script set
    // for the target, else what the clips playing on it make of it, else the
    // weight its file gives it at rest. Nought for a name the mesh lacks.
    [[nodiscard]] virtual core::f32 morphWeight(core::InstanceId meshPart, std::string_view name) const = 0;

    // A script's weight for one target, over whatever a clip says of it, until
    // it is cleared. Any number: a file's own weights go past one and below
    // nought, and so may a script's.
    virtual void setMorphWeight(core::InstanceId meshPart, std::string_view name, core::f32 weight) = 0;
    // Gives the target back to the clips and the file.
    virtual void clearMorphWeight(core::InstanceId meshPart, std::string_view name) = 0;
};

} // namespace engine::scene
