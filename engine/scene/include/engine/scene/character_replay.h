// Stepping one character again, for a replica correcting its prediction
// (ADR 0076, as amended by the owner's mandate of 2026-09-23).
//
// **The narrow interface replication needs from the physics mirror**, in the
// shape `AnimationHost` and `SkeletonHost` already set: `scene` declares it,
// `PhysicsSync` implements it, and `app` hands one to the replication module.
// Replication never learns there is a physics backend.
#pragma once

#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/math.h"
#include "engine/core/name_atom.h"
#include "engine/core/types.h"
#include "engine/scene/components.h"
#include "engine/scene/value.h"

namespace engine::scene {

// **What one tick told a character controller to do**, as `PhysicsSync`
// consumed it: the direction and speed it walked at, whether it jumped and how
// hard, and the tick's length. Everything a step of the movement model reads
// that is not the world around it.
struct CharacterCommand
{
    core::Vec3 moveDirection{0.0f, 0.0f, 0.0f};
    bool jump = false;
    core::f32 walkSpeed = 0.0f;
    core::f32 jumpSpeed = 0.0f;
    core::f32 dt = 0.0f;
    // How it was told to move when it is not on foot (D466).
    core::f32 gravityScale = 1.0f;
    core::f32 swimSpeed = 10.0f;
    core::f32 flySpeed = 16.0f;
    bool flying = false;

    // **What its player did, for the predicted step** (G37): the player's
    // tick, their intents and the buttons that went down at it. Empty for a
    // character no player has.
    core::u64 tick = 0;
    std::vector<PlayerIntent> intents;
    std::vector<core::NameAtom> presses;
};

// The attributes a predicted step writes, by name, as one moment had them; a
// name missing is an attribute that was not there.
using PredictedAttributes = std::vector<std::pair<core::NameAtom, Value>>;

// Where a replay starts: the authority's word on the character at the tick it
// last answered, including the two things a transform does not say.
struct CharacterReplayStart
{
    // The intent tick the authority answered: where a replay that re-steps
    // the simulation itself restores from (ADR 0133). Zero: unknown.
    core::u64 tick = 0;
    core::CFrameD transform;
    core::f32 verticalVelocity = 0.0f;
    // What an impulse or a written velocity gave it, as the authority had it:
    // a replay that started without it would walk a knocked-back character
    // home every snapshot.
    core::Vec3 push{0.0f, 0.0f, 0.0f};
    bool grounded = false;
    // The speeds the authority steps this character with, when the snapshot
    // said. The commands a replay steps through are the ones the authority has
    // not answered yet, and it will answer them at ITS speeds -- not at the
    // ones this replica held when it predicted them (D205).
    std::optional<core::f32> walkSpeed;
    std::optional<core::f32> jumpSpeed;
    // **The predicted parts, as the authority had them at that tick** (ADR
    // 0133): restored with the character, so what it pushed is stepped again
    // from where the authority says it was.
    struct Body
    {
        core::InstanceId id{};
        core::CFrameD cframe{};
        core::Vec3 linear{};
        core::Vec3 angular{};
    };
    std::vector<Body> bodies;
    // **Its predicted attributes, as the authority had them** (G37), put back
    // with the rest before the steps are taken again. Nothing: the authority
    // said nothing of them.
    std::optional<PredictedAttributes> attributes;
};

// **One character's predicted step** (G37): what a script bound to it is
// handed. The intents are the player's at `tick` -- on a step taken again,
// the ones it was first taken with.
struct PredictedTick
{
    core::InstanceId character;
    core::InstanceId player;
    core::u32 userId = 0;
    core::u64 tick = 0;
    core::f64 dt = 0.0;
    bool replay = false;
    std::span<const PlayerIntent> intents;
    std::span<const core::NameAtom> presses;
};

// **Scripts in the simulation step** (G37): the narrow interface the mirror
// calls them through, in the shape `ICharacterReplay` set -- `scene` declares
// it, the script runtime implements it, `app` connects the two.
class PredictedStepHost
{
public:
    virtual ~PredictedStepHost() = default;
    // Before the step: once for each character a player has that this machine
    // steps -- or, stepping again, for the one being stepped.
    virtual void predictedStep(const PredictedTick& tick) = 0;
    // Whether anything is bound to `part`'s predicted touch: asked of every
    // touch a predicted character begins, so it must be cheap.
    [[nodiscard]] virtual bool touchBound(core::InstanceId part) const = 0;
    // A predicted character began touching `part` in the step of `tick`.
    virtual void predictedTouch(const PredictedTick& tick, core::InstanceId part) = 0;
};

class ICharacterReplay
{
public:
    virtual ~ICharacterReplay() = default;

    // The command the most recent step applied to `character`, or nothing when
    // it was not stepped here -- no controller yet, or a character this
    // machine only follows.
    [[nodiscard]] virtual std::optional<CharacterCommand> lastCommand(core::InstanceId character) const = 0;

    // **Puts `character` at `start` and steps it through `commands`**, one
    // step each, with the same movement model the simulation steps it with, in
    // the world as it is now. Answers the transform after each step and leaves
    // the character -- part, controller and vertical velocity -- where the last
    // one did. Empty when the character has no controller to step.
    [[nodiscard]] virtual std::vector<core::CFrameD> replay(core::InstanceId character,
                                                            const CharacterReplayStart& start,
                                                            std::span<const CharacterCommand> commands) = 0;

    // **What this machine simulates, as it stands after the step of `tick`**
    // (ADR 0133): kept, so a correction can restore it at the tick the
    // authority answers and step it again for real. Nothing by default.
    virtual void remember(core::u64 tick) { (void)tick; }
    // Where `id` was after the step of `tick`, as remembered; nothing when it
    // was not simulated here then.
    [[nodiscard]] virtual std::optional<core::CFrameD> remembered(core::u64 tick, core::InstanceId id) const
    {
        (void)tick;
        (void)id;
        return std::nullopt;
    }
    // Its predicted attributes after the step of `tick` (G37), as
    // remembered; nothing when it was not remembered then.
    [[nodiscard]] virtual std::optional<PredictedAttributes> rememberedAttributes(core::u64 tick,
                                                                                  core::InstanceId id) const
    {
        (void)tick;
        (void)id;
        return std::nullopt;
    }
};

} // namespace engine::scene
