// The mirror between the Instance tree and the simulation (architecture.md §2:
// "scene ... built-in systems (transform hierarchy, physics sync via an
// injected `IPhysics3D*`)").
//
// **The tree is the authority and the body is a mirror of it.** A `BasePart`
// under `Workspace` has a body; one taken out of the tree loses it. `scene`
// never learns a body's identity beyond an opaque handle and `physics` never
// learns what an Instance is -- the map between them lives here and nowhere
// else, which is why neither module includes the other's vocabulary.
//
// Writes cross in both directions and each direction has exactly one legal
// point in the frame:
//
//   * **Script to physics** is collected while the script runs and applied at
//     the start of the sim tick. A script that sets `CFrame` mid-frame must not
//     teleport a body the solver is in the middle of.
//   * **Physics to script** happens after the step, as a QUIET write with a
//     batched changed set (architecture.md §4). That is the path the
//     10k-parts benchmark measures; any other route would silently forfeit the
//     equality filter that is worth about a third of it.
//
// Nothing here reads a clock. `step` takes the fixed tick and the caller owns
// the accumulator (R10).
#pragma once

#include <array>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/asset/terrain.h"
#include "engine/core/id.h"
#include "engine/core/types.h"
#include "engine/physics/physics.h"
#include "engine/scene/character_replay.h"
#include "engine/scene/components.h"
#include "engine/scene/skeleton_host.h"
#include "engine/scene/value.h"

namespace engine::scene {

class World;

// The solver shape an `Enum.PartShape` collides as.
[[nodiscard]] physics::ShapeType shapeForPartShape(i32 shape) noexcept;

// **What a part weighs, from what it is** (D472): its shape, its size and its
// density, whether or not the simulation has made it a body yet. A `MeshPart`
// that collides as its hull weighs what the solver says once it has one, and
// its box until then; a character weighs what its controller does.
[[nodiscard]] f32 partMass(const World& world, core::InstanceId id) noexcept;

// **The layer a terrain is drawn as where a ray met it** (ADR 0117): the
// voxel's own, its paint, or the one a rule lays over it -- by name, which is
// the material's `Content`. Empty where there is none. `hit` is in the
// terrain's own space, as `asset::raycastField` returns it.
[[nodiscard]] std::string_view terrainLayerAt(const TerrainComponent& terrain, const asset::TerrainHit& hit) noexcept;

// What a character is standing on, as the material it is: the part's, or the
// terrain's under its feet. Nothing in the air.
[[nodiscard]] MaterialRef floorMaterial(const World& world, core::InstanceId character);

class PhysicsSync final : public ICharacterReplay
{
public:
    // How close, in metres on every axis, something that moves has to be to a
    // chunk for the chunk to have a collider. Wide enough that a body arriving
    // at running speed finds it built before it gets there. A raycast far from
    // every moving body meets no collider -- which is why `Workspace:Raycast`
    // also asks the field itself (ADR 0082). Public for the ground's streamer,
    // which does not let go of ground this reaches (ADR 0149).
    static constexpr double TerrainCollisionReach = 24.0;

    PhysicsSync(World& world, physics::IPhysics3D& backend);
    ~PhysicsSync() override;

    // **A replica's own character, stepped again** (`ICharacterReplay`): the
    // command each step consumed, and a replay of a run of them from where the
    // authority said the character was. Through `stepController`, the one
    // function the simulation's own step moves a character with, so a replay of
    // the same commands from the same place is the same motion.
    [[nodiscard]] std::optional<CharacterCommand> lastCommand(core::InstanceId character) const override;
    [[nodiscard]] std::vector<core::CFrameD> replay(core::InstanceId character, const CharacterReplayStart& start,
                                                    std::span<const CharacterCommand> commands) override;
    // **The island, remembered after each tick's step** (ADR 0133): every
    // character this machine steps and every body it simulates -- on a
    // replica, its own character and what it owns or predicts. A replay that
    // finds the answered tick here restores it and steps the simulation again
    // with the live step, so the prediction redone is the prediction made.
    void remember(u64 tick) override;
    [[nodiscard]] std::optional<core::CFrameD> remembered(u64 tick, core::InstanceId id) const override;
    [[nodiscard]] std::optional<PredictedAttributes> rememberedAttributes(u64 tick, core::InstanceId id) const override;
    // **Who runs the predicted steps** (G37): the script runtime, handed in by
    // `app`. Null: nothing runs in the step.
    void setPredictedStepHost(PredictedStepHost* host) noexcept { m_predictedHost = host; }
    [[nodiscard]] usize rememberedTicks() const noexcept { return m_islands.size(); }

    // --- Rollback (ADR 0101) ----------------------------------------------
    //
    // **The 3D simulation as bytes**: the solver's whole state, and for every
    // instance it holds a body or a character for, where it is and how it is
    // moving -- `CFrame`, velocities, a pending impulse, whether it sleeps, and
    // a character's ground, state, vertical velocity and command. Nothing else:
    // not which instances exist, not their other properties, not the 2D layer,
    // and not a line of Luau.
    [[nodiscard]] bool saveSimulation(std::vector<u8>& out) const;
    // Puts it all back. **Refused** -- false, nothing changed -- when the bytes
    // are not a saved simulation, or when the set of bodies and characters is
    // not the one they were saved with.
    [[nodiscard]] bool restoreSimulation(std::span<const u8> bytes);
    // One fixed step of the 3D simulation, as the tick takes it, **without the
    // touches**: a step taken again is a step whose events already fired.
    void stepQuietly(f64 fixedDt);

    PhysicsSync(const PhysicsSync&) = delete;
    PhysicsSync& operator=(const PhysicsSync&) = delete;

    // `Workspace`, handed in by the host. `scene` has no notion of the
    // DataModel root, and the host already resolves it for the renderer; an
    // invalid id means nothing in this world has a body, which is the shape of
    // the defect that cost M4 a milestone, so the host tests that it resolved.
    void setWorkspace(core::InstanceId workspace) noexcept { m_workspace = workspace; }

    // Where a joint pose is committed each tick.
    //
    // Null in every world with no skeleton, which is most of them, and injected
    // by `app` for the same reason the physics backend is: `scene` may not learn
    // what a palette is, and `render` may not learn what a body is. The mirror
    // holds it because the mirror is what knows WHEN in a tick a pose may be
    // committed -- after everything that could still move a joint.
    void setSkeleton(SkeletonHost* skeleton) noexcept { m_skeleton = skeleton; }

    // The points a `MeshPart` with a hull fidelity collides as, keyed by the
    // content id its `MeshContent` names.
    //
    // PUSHED by whoever loaded the mesh rather than pulled from here, because
    // this module is L3 and a mesh file is the asset system's (L2) to read
    // through mounts the app (L6) owns. The app is the only place that can see
    // both, which is the same arrangement the UI's font and image providers use.
    //
    // Positions only, and every one of them: a convex hull builder discards the
    // interior points itself, and a sampled subset would produce a hull SMALLER
    // than the mesh -- which is a character sinking into a rock rather than a
    // slightly wrong shape.
    void setCollisionPoints(core::NameAtom content, std::vector<core::Vec3> points);

    // How many meshes have handed over their points. For a test, and for a
    // person asking why a hull is not a hull.
    [[nodiscard]] usize collisionMeshCount() const noexcept { return m_collisionPoints.size(); }
    [[nodiscard]] core::InstanceId workspace() const noexcept { return m_workspace; }

    // One simulation tick: apply the scene's writes, advance the characters,
    // step, write the results back, and turn the contact diff into deferred
    // signals. Called from the sim tick and from nowhere else.
    void step(f64 fixedDt);

    // The mirror WITHOUT the simulation: create, update and retire the bodies
    // the tree describes, and advance nothing.
    //
    // **What an editor's collision view is drawn from.** A paused world never
    // ticks, so `step` is never called, so the backend holds no bodies at all --
    // and a wireframe of a world with no bodies in it is an empty picture that
    // looks exactly like a working one. This is what puts the shapes there, so
    // the editor can ask the backend what it thinks a part's collider is rather
    // than deriving a second answer beside it.
    //
    // **Deliberately not `step(0)`.** A zero-length step still runs the solver,
    // the character controllers and the contact diff -- so it would raise
    // `Touched` while nobody is playing, advance a character by whatever a
    // zero-dt controller does, and write results back over the tree an author is
    // editing. None of those is a thing a view may do.
    void mirror();

    // --- The floating origin (ADR 0014, architecture.md §10) ------------------
    //
    // The tree stores absolute f64 and always has; what the origin changes is
    // the f32 space the solver works in. Scripts never see it, which is ADR
    // 0014's "rebasing is invisible to Luau" one layer down: a `CFrame` read
    // after a rebase is the same `CFrame` read before it.
    //
    // **Call at a safe point.** A rebase between two ticks is a translation of
    // everything at once; a rebase inside one would move the world under a
    // solver halfway through it.
    void setOrigin(core::DVec3 origin);
    [[nodiscard]] core::DVec3 origin() const noexcept { return m_origin; }

    // How far a focus may drift before the origin should follow it. Four
    // kilometres is architecture.md §10's number: f32 has about 24 bits of
    // mantissa, so at 4 km the quantum is a quarter of a millimetre and a
    // contact still resolves cleanly.
    static constexpr f64 RebaseThreshold = 4000.0;

    // The policy, separated from the mechanism so a test can drive either. True
    // when `focus` has left the tolerance around the current origin; the caller
    // then decides whether this is a safe point.
    [[nodiscard]] bool shouldRebase(core::DVec3 focus, f64 threshold = RebaseThreshold) const noexcept;

    // How many times the origin has moved. A counter rather than a log line:
    // a world that rebases every frame is a world whose threshold has hysteresis
    // it does not have, and that shows up as a number climbing.
    [[nodiscard]] u64 rebaseCount() const noexcept { return m_rebaseCount; }

    // The physics world's own handle, for the query methods the bindings
    // expose. `Workspace:Raycast` is a read of the same world the tick steps.
    [[nodiscard]] physics::WorldHandle worldHandle() const noexcept { return m_world; }
    [[nodiscard]] physics::IPhysics3D& backend() const noexcept { return m_backend; }

    // Resolves a body's opaque user data back to the instance that owns it, for
    // a query result. Invalid when the body is not ours or has been retired.
    [[nodiscard]] core::InstanceId instanceOf(u64 userData) const noexcept;
    // The other direction, for a query filter.
    [[nodiscard]] u64 userDataOf(core::InstanceId id) const noexcept;

    // Seconds inside the last `step`, in the three stages that are separable at
    // this seam (see `physics::StepTimings` for why they are not broadphase,
    // narrowphase and solver).
    struct Timings
    {
        f64 apply = 0.0;
        f64 step = 0.0;
        f64 writeback = 0.0;

        [[nodiscard]] f64 total() const noexcept { return apply + step + writeback; }
    };

    [[nodiscard]] const Timings& timings() const noexcept { return m_timings; }
    [[nodiscard]] usize bodyCount() const noexcept { return m_bodyCount; }

private:
    // Sorted by content atom id. A flat vector rather than a hash map for the
    // reason every other cache in this repository is one: a world has a handful
    // of distinct collision meshes, and a flat array has an order that an
    // unordered container does not (R10).
    // A point cloud and which version of it this is. The version is what a
    // `ShapeDesc` carries instead of the points, so a body can be asked "is your
    // hull still current" without anybody keeping a span (`physics/types.h`).
    struct CollisionMesh
    {
        std::vector<core::Vec3> points;
        u64 revision = 0;
    };
    std::vector<std::pair<core::NameAtom, CollisionMesh>> m_collisionPoints;
    // Counted up on every replacement, across all content: one counter is
    // enough, and one per mesh would be a second thing to keep in step.
    u64 m_collisionRevision = 0;

    SkeletonHost* m_skeleton = nullptr;

    // What the mirror last pushed down for one instance, so that a tick can
    // tell a script's write from its own writeback without either side
    // carrying a dirty flag. `scene` cannot ask the body what it looks like
    // without a virtual call per field per part, and a dirty flag on the
    // component would be mirror bookkeeping in state the world hashes.
    struct BodyRecord
    {
        // Zero means the slot holds no body. Matched against the instance's own
        // generation, so a slot reused by a new instance is not mistaken for the
        // old one's body.
        u32 generation = 0;
        // The `writeBack` that last found this body active (audit E4).
        u64 activeStamp = 0;
        physics::BodyHandle handle;
        // **Whether the backend actually holds a body for this record.**
        //
        // False for one that was asked for and refused -- a degenerate hull is
        // the case that reaches it. The generation is still this instance's, so
        // the refusal is REMEMBERED: without that, `createBody` was re-attempted
        // every tick for ever, silently burning a body generation each time.
        // `retireUnseen` and `bodyHandleOf` both read this, because destroying
        // or handing out a handle that names nothing is worse than either.
        bool live = false;
        // **What was last handed to the backend, not what it has.** They differ
        // only when it refused, and nothing reads this except the comparison
        // that decides whether to hand it something new -- so "attempted" is the
        // honest reading and it is what stops a refusal being retried per tick.
        //
        // The hull's `points` span is CLEARED before it is stored. Its
        // documented lifetime is the create call and no longer, and the vector
        // behind it is replaced whenever a mesh loads; `pointsRevision` is what
        // carries the same information without keeping a pointer to it.
        physics::ShapeDesc shape;
        physics::MotionType motion = physics::MotionType::Dynamic;
        // **What the backend actually has**, as against `motion`, which is what
        // was last ASKED for (D135).
        //
        // They differ only after a refused rebuild -- and they have to, because
        // `motion` is part of the comparison that decides whether to ask again
        // and this one is the answer to "is this body kinematic". Reading the
        // attempted one for that question tells `writeBack` to skip a body the
        // solver is still moving, and tells the kinematic re-target to keep
        // aiming a body that is not kinematic.
        physics::MotionType backendMotion = physics::MotionType::Dynamic;
        bool collidable = true;
        bool queryable = true;
        u16 group = 0;
        f32 friction = 0.3f;
        f32 restitution = 0.0f;
        f32 density = 1.0f;
        f32 linearDamping = 0.05f;
        f32 angularDamping = 0.05f;
        // **What the worn material is to touch** (ADR 0117), remembered so a
        // part wearing one costs a lookup when the material or the library
        // changes and not once a tick.
        core::NameAtom wornMaterial;
        u64 wornLibrary = ~u64{0};
        f32 wornFriction = 0.3f;
        f32 wornRestitution = 0.0f;
        // The transform this mirror last wrote INTO the component. A component
        // that differs from it now is a script's write.
        core::CFrameD written;
        // Until which TICK an anchored part stays `Kinematic` (D031). Set when a
        // script writes its `CFrame`; a part past it goes back to `Static`.
        //
        // A tick count and never a duration: R10 forbids a simulation decision
        // that a wall clock could change, and "kinematic for a fifth of a
        // second" would make a body's broadphase layer depend on how fast the
        // machine was running.
        u64 movingUntilTick = 0;
        // Marked each tick during the sweep; anything unmarked afterwards has
        // left the world and its body is destroyed.
        bool seen = false;
    };

public:
    // **Where another player's stand-in is put, given this machine's own
    // character** (ADR 0163, amended; D590). `known` is where its newest
    // snapshot put that character and `expected` where the session expects
    // it by the tick being predicted -- `known` carried on by the way it was
    // going. It is somewhere between the two: it kept going, or it stopped.
    //
    // **This machine's own character is kept out of all of it.** The stand-in
    // is put at the place between the two that is NEAREST the own character,
    // and where the way from `known` to `expected` runs into the own
    // character, it stops there, touching -- as that character does on the
    // authority, where a character walks until it meets a body and the own
    // character is one. So:
    //
    // - one coming at the player is met as far forward as it may be, and never
    //   inside the player (it was put there, a body inside a character pushes
    //   it out, and a friend walking into a standing player pushed that
    //   player's own hero away on that player's screen -- half a metre from
    //   where the host had it, corrected and pushed again every other frame);
    // - one going away is met where it was last known to be, not where it is
    //   guessed to have got to (a player following a friend who stopped walked
    //   on through him, as far as the guess had run, and was pulled back out);
    // - one standing still is where it is, and does not give way.
    //
    // The price is the cheap kind of wrong: a player close behind a running
    // friend is stopped a step short of where the host lets him go, and
    // carried forward to it -- the way he was going -- and never taken back
    // out of somebody.
    //
    // Across the ground only, as the carrying is. `overlapping` says whether
    // the two capsules share any height; when they do not -- one is above the
    // other -- nothing is in the way, and it is where it is expected.
    // `reach` is the distance between two axes that touch.
    [[nodiscard]] static core::DVec3 standInPlace(const core::DVec3& known, const core::DVec3& expected,
                                                  const core::DVec3& own, f64 reach, bool overlapping) noexcept;

private:
    struct CharacterRecord
    {
        physics::CharacterHandle handle;
        f32 height = 5.0f;
        f32 diameter = 2.0f;
        f32 maxSlopeAngle = 46.0f;
        f32 stepHeight = 0.5f;
        u16 group = 0;
        core::CFrameD written;
        bool seen = false;
        // On a replica, a character somebody else plays: it follows the
        // authority's snapshots and is not simulated here (ADR 0076).
        bool follower = false;
        // Whether the backend holds a stand-in for it (ADR 0163).
        bool standIn = false;
        // What the last step that moved it was told, for `lastCommand`.
        std::optional<CharacterCommand> last;
        // **Its player's input at its last predicted step** (G37): the tick,
        // the intents and the presses, and whether it has had one -- a tick
        // the authority holds a player's input for is not stepped twice.
        CharacterCommand input;
        bool predicted = false;
    };

    // What a controller carries from one step to the next that its transform
    // does not say.
    struct CharacterMotion
    {
        f32 vertical = 0.0f;
        core::Vec3 push{0.0f, 0.0f, 0.0f};
        // 0 on foot or falling, 2 swimming, 3 flying.
        i32 mode = 0;
    };
    // One step of the movement model: gravity, a jump, the walk -- or the swim
    // or the flight -- what pushed it, and the sweep. Answers what the step
    // leaves. `groundNormal` is the ground's, where `grounded`; `position` is
    // where the character is as the step begins, which is what says whether it
    // is in a fluid.
    [[nodiscard]] CharacterMotion stepController(const CharacterRecord& record, const CharacterCommand& command,
                                                 CharacterMotion motion, bool grounded, core::Vec3 groundNormal,
                                                 core::DVec3 position);
    // Whether a place is under a `Water`'s surface or inside a fluid block.
    [[nodiscard]] bool inFluid(core::DVec3 at) const;

    void syncCollisionGroups();
    void applyScene();
    void applyBody(core::InstanceId id, PartComponent& part, RigidBodyComponent& body);
    void applyCharacter(core::InstanceId id, PartComponent& part, RigidBodyComponent& body,
                        CharacterBodyComponent& character, f32 fixedDt);
    void retireUnseen();
    // What was destroyed, out of the simulation before anything new is put in
    // (D467).
    void retireGone();
    void resolveWelds();
    // One weld, and everything it hangs from, resolved once. Returns the
    // driven part's new transform.
    void resolveWeld(core::InstanceId weldId, WeldComponent& weld);

    // Every `Attachment` and `Bone`, given its world transform.
    //
    // **Interleaved with weld resolution rather than a pass after it**, and
    // through the same memo recursion: a bone's world transform depends on its
    // `MeshPart`'s own `CFrame`, which may itself be driven by a weld. Two
    // separate passes would reintroduce exactly the one-frame lag
    // `resolveWeld`'s recursion exists to prevent.
    void resolveAttachments();

    // Every enabled `Ragdoll`, written back into the pose it is driving.
    //
    // After the attachments, because a bone's world transform is what says where
    // a limb's joint ENDED UP -- and before `commitOverrides`, which is what
    // makes the joints nobody simulates ride along on the ones somebody does.
    void driveRagdolls();

    // Where a weld's end is: the part's own frame, or the attachment's world
    // frame when one was named.
    //
    // One helper, so `resolveWeld` has ONE formula either way -- exactly as
    // `c1` already gives it one formula for `Weld` and `WeldConstraint`.
    // Invalid when the id is neither.
    [[nodiscard]] bool anchorFrame(core::InstanceId id, core::CFrameD& out);

    // One attachment, and whatever it depends on first.
    void resolveAttachment(core::InstanceId id);

    // The part an id IS or SITS ON: itself for a part, its parent for an
    // attachment, invalid for anything else.
    // The rig a `Bone` belongs to: the nearest `MeshPart` above it that has a
    // skeleton. Itself for a bone on a character, the character for a bone on
    // one of a ragdoll's limbs.
    [[nodiscard]] core::InstanceId rigAbove(core::InstanceId id) const;

    [[nodiscard]] core::InstanceId ownerOf(core::InstanceId id) const;

    // One constraint, created or updated. Called from a SECOND pool-order walk
    // inside `applyScene`, after every body exists -- a joint whose bodies have
    // not been made yet is a joint that cannot be built, and creating bodies
    // lazily from here would put body creation order in the constraint pool's
    // order instead of the rigid-body pool's.
    [[nodiscard]] physics::BodyHandle bodyHandleOf(core::InstanceId id) const;

public:
    // **A query sees the world a script has just written** (D475): every part
    // that has come into the world since the simulation last looked is given
    // its body now, so a ray cast in the line after `Parent = workspace` meets
    // the part. Called before a query; costs one compare when nothing was
    // written since the last call.
    //
    // **Only what is NEW.** A part that is already a body and was moved by
    // its `CFrame` is the tick's to move: an anchored part is swept to where
    // it was written, so what stands on it is carried, and putting it there
    // early would turn that sweep into a jump.
    void syncForQuery();

private:
    core::u64 m_querySynced = ~core::u64{0};

    void applyConstraint(core::InstanceId id, ConstraintComponent& constraint);

    // What one constraint looked like when it was last built, so a tick can tell
    // a change from a no-op without a dirty flag on state the world hashes.
    struct ConstraintRecord
    {
        core::u32 generation = 0;
        physics::ConstraintHandle handle;
        bool seen = false;
        // The two BODIES, not the two attachments: an attachment moving to
        // another part is a different joint even when the instance is the same.
        core::InstanceId body0;
        core::InstanceId body1;
        i32 kind = 0;
        bool collideConnected = true;
        f32 limitLow = 0.0f;
        f32 limitHigh = 0.0f;
        f32 swingLimit = 0.0f;
        f32 twistLimit = 0.0f;
        bool limitsEnabled = false;
        bool enabled = true;
        // A distance joint's range, as it was last built or driven with.
        f32 minDistance = 0.0f;
        f32 maxDistance = 0.0f;
        // A drive (ADR 0127, D471): the two frames it was built with, and the
        // solver bodies. A frame is what the drive IS -- its axes are the
        // axes its motors push along -- so one that changed is another drive.
        core::CFrameD frame0;
        core::CFrameD frame1;
        physics::BodyHandle handle0;
        physics::BodyHandle handle1;
        bool atCenter = false;
    };

    std::vector<ConstraintRecord> m_constraints;

    // --- ADR 0127 (physics_movers.cpp) ---------------------------------------
    //
    // `VectorForce`, `Torque` and the springs, as impulses, before the step.
    void applyMovers(f32 fixedDt);
    // `AlignPosition`, `AlignOrientation`, `LinearVelocity` and
    // `AngularVelocity`: a motor in the solver, between the body and the world
    // or between two bodies, built and driven as a joint's is.
    void applyDrive(core::InstanceId id, const ConstraintComponent& constraint);
    // A joint's actuator, into the description it is built or driven with.
    void motorOf(const ConstraintComponent& constraint, const ConstraintRecord& record, f32 fixedDt,
                 physics::ConstraintDesc& desc) const;
    // `NoCollisionConstraint`: which pairs of bodies are told to pass through
    // each other, and told to stop when the instance goes or changes.
    void applyNoCollisions();
    // After the step: what each joint carried, and whether it gave.
    void readConstraints(f32 fixedDt);
    struct NoCollisionRecord
    {
        core::u32 generation = 0;
        physics::BodyHandle first;
        physics::BodyHandle second;
        bool applied = false;
        bool seen = false;
    };
    std::vector<NoCollisionRecord> m_noCollisions;
    // Whether this tick's walk over the joints found a spring: a world with
    // no mover and no spring does not walk them a second time.
    bool m_anySpring = false;
    void writeBack();
    void writeCharacters();
    void publishContacts(f64 fixedDt);
    void publishTouches(std::span<const physics::ContactEvent> events);

    [[nodiscard]] bool inWorld(core::InstanceId id) const;
    // Whether a part is the driven end of an active weld, which is what makes
    // it kinematic rather than dynamic: it is moved by the weld and by nothing
    // else, so the solver may not integrate it.
    [[nodiscard]] bool isDriven(core::InstanceId id) const;
    // Whether this machine solves what holds `part` (NA34): everywhere but a
    // replica, and there only a body it simulates itself.
    [[nodiscard]] bool solvedHere(core::InstanceId part) const;
    [[nodiscard]] physics::ShapeDesc shapeOf(core::InstanceId id, const PartComponent& part) const;
    [[nodiscard]] physics::BodyDesc descOf(core::InstanceId id, const PartComponent& part,
                                           const RigidBodyComponent& body, bool movingAnchored) const;

    World& m_scene;
    physics::IPhysics3D& m_backend;
    physics::WorldHandle m_world;

    // Indexed by the instance's SLOT, not hashed by its id, and that is a
    // measurement rather than a preference: with ten thousand parts the hash
    // map spent 214 ns per body per tick doing nothing, most of it missing
    // cache on a lookup whose key the pool walk already had in hand. A vector
    // indexed by slot is walked in the same ascending order the component pool
    // is, which is what makes it prefetchable.
    //
    // The generation in the record is what makes a stale slot detectable: a
    // slot reused by a different instance has a different generation, and a
    // record whose generation does not match is not that instance's.
    // **Terrain's colliders, which are not per-instance and so cannot be
    // `BodyRecord`s** (ADR 0066, ADR 0067). One `Terrain` produces one static
    // body per height tile plus one per bricked region, and every one of them
    // outlives the tick that made it.
    //
    // Keyed by chunk and kept SORTED, never in a hash map (R10): this decides
    // the order bodies are created in, and therefore the order the backend
    // assigns its own ids in.
    struct TerrainCollider
    {
        core::InstanceId terrain;
        // A `TriangleMesh` over one chunk's surface.
        asset::ChunkKey key;
        // Invalid when the chunk has no surface: remembered, so it is not
        // remeshed every tick to find nothing again.
        physics::BodyHandle body;
        // `fieldRevision` when this was last checked, which answers "nothing
        // was written" for the cost of a compare.
        core::u64 revision = 0;
        // **What the collider was built FROM**: the digests of the chunk and
        // the twenty-six round it, which its mesh reads. A write anywhere
        // bumps `fieldRevision` for the whole terrain; this is what keeps a
        // brush from rebuilding every collider in the world.
        core::u64 content = 0;
        // The terrain's origin, folded, so a moved terrain moves its bodies.
        core::u64 placement = 0;
        bool seen = false;
    };
    std::vector<TerrainCollider> m_terrainColliders;
    // **What each terrain's colliders were last decided from** (audit E9): the
    // boxes of chunks round its movers, its revision and its placement, and
    // whether that pass finished its rebuilds. The same again is the same
    // answer: every collider kept, nothing gathered, sorted or digested.
    struct TerrainWant
    {
        core::InstanceId terrain;
        std::vector<std::array<i32, 6>> boxes;
        u64 revision = 0;
        u64 placement = 0;
        bool settled = false;
    };
    std::vector<TerrainWant> m_terrainWants;

    // **What each of a terrain's layers is to touch** (ADR 0117): the friction
    // and the bounce of the material each layer names, by layer id, worked out
    // again only when the layers, the material library or the rules change.
    // `key` is zero when every layer is the default surface -- a terrain that
    // says nothing collides exactly as it did before a material could.
    struct TerrainSurfaces
    {
        core::InstanceId terrain;
        u64 layersRevision = ~u64{0};
        u64 libraryRevision = ~u64{0};
        u64 rules = 0;
        std::vector<physics::SurfaceMaterial> table;
        u64 key = 0;
    };
    std::vector<TerrainSurfaces> m_terrainSurfaces;
    [[nodiscard]] const TerrainSurfaces& terrainSurfacesOf(core::InstanceId id, const TerrainComponent& terrain);

    // **A count, never a millisecond budget.** A collider is part of the world,
    // so how many get rebuilt in a tick has to be a fact about the operation
    // sequence rather than about how fast the machine was that day.
    static constexpr core::u32 TerrainRebuildsPerTick = 4;

    void applyTerrain();
    void retireUnseenTerrain();

    // **The block world's colliders (V1)**: one static triangle mesh per chunk,
    // built from the same greedy faces the renderer draws, and only for chunks
    // within reach of something that moves -- the caves' bargain, for the same
    // reason. Sorted by key (R10): this decides body creation order.
    struct VoxelCollider
    {
        asset::VoxelChunkKey key;
        physics::BodyHandle body;
        // The chunk's own digest and its six face neighbours' -- a face on the
        // chunk's edge is hidden by a neighbour's block -- and the block size.
        core::u64 content = 0;
        // What its mesh read: the chunk and its one-block shell
        // (`asset::shellDigestOf`), for when `content` moved.
        core::u64 exact = 0;
        bool seen = false;
    };
    std::vector<VoxelCollider> m_voxelColliders;
    // A chunk's collider is a few thousand triangles at most; two a tick keeps a
    // mining game inside its frame and still builds a region in well under a
    // second.
    static constexpr core::u32 VoxelRebuildsPerTick = 2;
    // How close, in metres, something that moves must be to a chunk for the
    // chunk to have a collider.
    static constexpr double VoxelCollisionReach = 24.0;
    void applyVoxels();

    // One remembered island: the solver's half and ours.
    struct IslandEntity
    {
        core::InstanceId id{};
        core::CFrameD cframe{};
        bool body = false;
        core::Vec3 linear{};
        core::Vec3 angular{};
        bool character = false;
        bool grounded = false;
        i32 state = 0;
        core::InstanceId groundPart{};
        core::Vec3 move{};
        bool jump = false;
        f32 vertical = 0.0f;
        core::Vec3 push{};
        i32 mode = 0;
        // A character's predicted attributes (G37).
        PredictedAttributes attributes;
        // **What a script wrote after the step, waiting for the next** (D525):
        // a predicted touch runs after the solve, and a velocity, an impulse
        // or a place it gives is the next step's. Remembered with the rest, so
        // a replay starting at this tick takes them as the live step did.
        struct Pending
        {
            // Where the backend has it: a place written and not yet applied
            // differs from `cframe`.
            core::CFrameD written{};
            bool velocityWritten = false;
            core::Vec3 velocity{};
            core::Vec3 impulse{};
            core::Vec3 angularImpulse{};
        };
        Pending pending{};
    };
    struct Island
    {
        std::vector<u8> solver;
        std::vector<IslandEntity> entities;
    };
    // By tick, the newest `IslandMemory` of them.
    static constexpr usize IslandMemory = 128;
    // A replay's correction of its character under this is a nudge, which
    // keeps the controller's contacts; over it, a move, which finds them anew.
    static constexpr f64 NudgeMetres = 0.05;
    std::map<u64, Island> m_islands;
    [[nodiscard]] bool restoreIsland(const Island& island);

    std::vector<BodyRecord> m_bodies;
    // The parts a constraint holds, gathered each tick before the bodies are
    // synced: they keep a body even when nothing else would (D513).
    std::vector<core::InstanceId> m_constrainedParts;
    // Counted up by each `writeBack`: a body stamped with the current count
    // was in this step's active list.
    u64 m_writeBackStamp = 0;
    // Set while `stepQuietly` runs: contacts are drained and not published.
    bool m_quiet = false;
    // **The predicted steps** (G37): who runs them; the characters this step
    // ran them for, with their input, for the touches the step begins; and,
    // while a replay steps again, the character and the command it steps.
    PredictedStepHost* m_predictedHost = nullptr;
    struct PredictedNow
    {
        core::InstanceId character;
        core::InstanceId player;
        u32 userId = 0;
        CharacterCommand input;
    };
    std::vector<PredictedNow> m_predictedNow;
    core::InstanceId m_replayCharacter;
    const CharacterCommand* m_replayCommand = nullptr;
    void runPredictedSteps(f64 fixedDt);
    void runPredictedTouches(std::span<const physics::ContactEvent> events, f64 fixedDt);
    [[nodiscard]] PredictedAttributes predictedAttributesOf(core::InstanceId id) const;
    [[nodiscard]] IslandEntity::Pending pendingOf(core::InstanceId id) const;
    void putPending(core::InstanceId id, const IslandEntity::Pending& pending);
    void putPredictedAttributes(core::InstanceId id, const PredictedAttributes& attributes);
    [[nodiscard]] std::vector<core::InstanceId> simulatedIds() const;
    // Characters are few and are not on this path, so a map stays a map --
    // an ORDERED one (audit E6): `writeCharacters` fires `Landed` and writes
    // state walking it, and retirement destroys walking it, and a hash map's
    // order is MSVC's on one machine and libstdc++'s on the next (R10).
    std::map<u64, CharacterRecord> m_characters;
    // How many slots in `m_bodies` are live, so `bodyCount` does not walk.
    usize m_bodyCount = 0;

    // Scratch reused across ticks so that a thousand moving bodies allocate
    // nothing per frame.
    std::vector<physics::ActiveBody> m_active;

    // **Marks by instance slot, for this tick's resolution round** (audit E5):
    // the welds resolved so far, so a chain -- A welded to B, B welded to C --
    // resolves each link once and in dependency order rather than in whatever
    // order the pool holds them (R10); the attachments resolved so far; and the
    // parts an active weld drives, rebuilt each round before the bodies are
    // applied (a driven part is kinematic; a released one goes back to being
    // whatever `Anchored` says). Each was a list searched once per entry -- a
    // thousand welds, a million compares a tick.
    struct Mark
    {
        u32 generation = 0;
        u64 round = 0;
    };
    u64 m_resolveRound = 0;
    std::vector<Mark> m_weldMarks;
    std::vector<Mark> m_attachmentMarks;
    std::vector<Mark> m_drivenMarks;
    [[nodiscard]] bool marked(const std::vector<Mark>& marks, core::InstanceId id) const noexcept
    {
        return id.index < marks.size() && marks[id.index].generation == id.generation &&
               marks[id.index].round == m_resolveRound;
    }
    void mark(std::vector<Mark>& marks, core::InstanceId id)
    {
        if (marks.size() <= id.index)
            marks.resize(static_cast<std::size_t>(id.index) + 1);
        marks[id.index] = Mark{id.generation, m_resolveRound};
    }

    // The one-entry memo `inWorld` keeps. Mutable because the question is a
    // read and the answer is a cache; reset every tick so a reparent cannot
    // outlive the frame it happened in.
    mutable core::InstanceId m_lastParent;
    mutable bool m_lastParentInWorld = false;
    mutable core::u64 m_lastParentMutations = 0;

    u32 m_groupRevision = 0xffffffffu;
    core::InstanceId m_workspace;
    Timings m_timings;
    core::DVec3 m_origin;
    u64 m_rebaseCount = 0;
};

} // namespace engine::scene
