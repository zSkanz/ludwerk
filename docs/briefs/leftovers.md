# Leftovers: what the closed ledgers still owed

Written on 2026-10-02, from an audit of every unticked box in the ledgers whose
work had ended. Each of 115 boxes was checked against the tree: 82 were done
and are now ticked where they stand; the 33 that were not are gathered here, so
a closed ledger is closed and what it still owed has one place.

In an old ledger, **`[>]` means "moved"**: the item is open here, or in the
ledger this file names for it. The full text of each item stays where it was
written; this file says what is left of it and where it came from.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## World

Built with the world's other leftovers (water, foliage, views, the streaming
clean-up).

- [ ] **A raise-ground stroke fires no spurious `Touched`**: a character stands
  on ground that is sculpted under it for sixty frames, and hears nothing. Only
  the general reshape case is tested (D151); a terrain collider is now
  destroyed and made again, which is exactly what could fire one.
  (`phase-2-4-plan.md`, F1's gate)
- [ ] **A terrain's colliders name the terrain.** They carry no instance, so a
  character's `Landed` on terrain hands over nil, a part touching the ground
  fires no `Touched` for it, and a ray that meets the collider before the field
  has no `Instance`. `FloorMaterial` asks the ground directly meanwhile. Giving
  them the instance changes which contacts are published, which is why it is a
  piece of work and not a line. (ADR 0117's B7)
- [ ] **The solver's rounds are a `Workspace` setting.** A mover's pull travels
  a joint a round: a rigid chain of five turned from its middle wants forty,
  where a tick has ten (D471, measured). Every island would pay for a higher
  default, so it is a number a world sets -- as other engines' solver
  iterations are. (ADR 0127's second amendments)
- [ ] **A mover's cap is a magnitude.** `MaxForce` and `MaxTorque` are along
  each axis the solver's motor has, so a pull along a diagonal may use up to
  the square root of three times the number. (ADR 0127's second amendments)
- [ ] **`worldHash` is O(objects), not O(bytes)**, asserted as a timing ratio.
  The per-chunk digest is there; the test that would notice it going is not.
  (`phase-2-4-plan.md`)
- [ ] **`Enum.CollisionFidelity.Precise` collides against the triangles.** It
  asks for them and gets the hull, and the enum's own page says so.
  (`phase-2-4-plan.md`)
- [ ] **A `MeshContent` naming a reserved scheme is never blacklisted** (D2).
  (`phase-2-4-plan.md`)
- [ ] **The `MeshUsage::Dynamic` hazard is measured, not assumed** (A4). The
  block world's own measurement (D449) found the cost in creating buffers and
  answered it with pooled pages; the dynamic path's number is still not
  written down. (`phase-2-4-plan.md`)
- [ ] **A prepared scene warms more than meshes and pictures**: materials,
  sounds and terrain cells, inside `warm_radius` and `max_prepared_bytes`.
  (`foundation-kickoff.md`)
- [ ] **F3 and Stats show a prepared scene**: its status and its bytes.
  (`foundation-kickoff.md`)
- [ ] **The terrain audit's P6, the rest of it**: the foliage's column height
  and the fallback when a layer map does not resolve.
  (`terrain-audit-2026-09-29.md`)
- [~] **CPU tests at every level of the terrain pyramid**: shapes, flat ground,
  material and sky are tested per level; a grid of rays across the seam between
  a level and the next, and winding against the sign at every level, are not.
  (`terrain-audit-2026-09-29-full.md`)
- [ ] **A large paint ball touches the surface only**: a ball of 24 m paints
  every voxel inside it, and past that size the cost is the voxels themselves
  (ADR 0114 to amend). (`terrain-editing-perf.md`, P1)
- [ ] **P2, revisited**: the coarse levels gathered a sheet at a time, as level
  0 is. (`terrain-editing-perf.md`)

## Network

- [ ] **`Highlight`, `Beam` and `Trail` travel.** ADR 0129 has them replicate
  as instances; they are excluded, because a highlight names what it marks by
  reference, a beam and a trail name two attachments, and the wire has no
  reference field but `Parent`, no `Attachment`, and no sequence. (ADR 0129,
  as built)
- [ ] **Clients prepare a scene too, in a match**: today only the authority
  does, and a client's call is a keyed error. It needs a message on the wire.
  (`foundation-kickoff.md`)
- [ ] **R6: a joiner is sent the changed terrain over several ticks**, not
  every differing chunk in one. (`terrain-audit-2026-09-29.md`)

## Interface and the editor

- [ ] **A second catalog**, and the editor and the CLI seen in it: there is
  only `en.json` to try. (`r3-editor-i18n.md`)
- [ ] **A picture of each manipulator mode** for E2's gate record: the
  manipulators are tested and the stage is signed off, and the row still says
  "pending". (`e2-kickoff.md`)

- [ ] **A drag detector's `DragRelativity`, `DragSpace` and a drag function**
  (ADR 0128): `Scriptable` and the custom responses hand a script the numbers;
  the three that shape a function's answer are not built.
- [ ] **A scroll frame that coasts**: a dragged list stops where the finger
  lets go. Inertia and an elastic end are what a phone's own lists do.
- [ ] **A `CanvasGroup` on a world canvas** is a frame (ADR 0128): the world's
  UI pass has no pictures to draw into.
- [ ] **The raw input events do not say `uiConsumed`** for a key or button the
  selection used: the interface only takes what the game has not bound, so
  nothing is taken from an action, and `InputBegan` still reports the key as
  the game's.

## The repository

- [x] **A windowed game does not pay 95 ms for gamepads at start** (D476):
  the joystick subsystem is started by the event pump after the first frame is
  presented, and never by a run with no hands. `platform_tests.cpp` times the
  way to the first frame with and without, and they are within ten
  milliseconds; `GamepadAvailable` at a script's first line is false, and the
  manual says so.
- [x] **Gamepads work** (D476, P1): the vendored SDL's joystick, haptic and
  HID support are on, and `gamepad_tests.cpp` goes from a virtual device's
  button to an `InputAction`.
- [ ] **A clean-machine job in CI**: fresh clone, bootstrap, build,
  `ludwerk new`, run. The local gate does it (`tests/packaging`); no workflow
  does. (`m8-kickoff.md`)

## Waits for the owner

Nothing here can be built: each is a look with his eyes, a device in his hand,
or a decision that is his.

- [ ] **A menu in a hand** (ADR 0128): `examples/32-menus` with a mouse, a
  finger on the phone and a controller -- the swipe between pages, the list
  under a thumb, the slider, the selection walking the buttons. Every one is
  tested by made events and looked at in a still frame; nobody has used it.
- [ ] **A real controller in a real hand** (D476): a gamepad's buttons and
  sticks in a game, its rumble through `HapticService:SetMotor`, and a phone
  that shakes on `HapticService:Vibrate`. The tests go through a virtual
  device; nobody has felt it.
- [ ] A person drags a brush across the ground at a window, and the ground
  changes. (`phase-2-4-plan.md`)
- [ ] The end to end, by hand: open the flagship, sculpt a valley, dig, save,
  reopen. (`phase-2-4-plan.md`)
- [ ] The end to end with the model that started the importer's ledger -- a
  file that is not in the repository. (`e9-kickoff.md`)
- [ ] Register `ludwerk.com` and `ludwerk.dev`; check the trademark offices.
  (`rename-kickoff.md`)
- [ ] Release 0.0.1 on his word: the tag, the notes, the package.
  (`rename-kickoff.md`)
- [ ] The on-screen keyboard of each type, on the phone.
  (`text-input-kickoff.md`)
- [ ] The export window's tiling on a real screen, recorded.
  (`export-and-server-kickoff.md`)
- [ ] Reference pictures of terrain he approves, for visual goldens.
  (`terrain-audit-2026-09-29-full.md`)
- [ ] His place photographed again at the audit's distances, after.
  (`terrain-audit-2026-09-29-full.md`)
- [ ] His own look at the terrain, packaged: he sculpts, paints and flies.
  (`terrain-audit-2026-09-29-full.md`)
- [ ] A streamed terrain on a phone, run and measured.
  (`terrain-editing-perf.md`)

## Moved to a ledger that already had the item

No box here: each is counted where it lives.

| From `terrain-editing-perf.md` | Lives in |
|---|---|
| The far ground's cut is never seen | `settings-kickoff.md`, G1 |
| The session cache as one file | `streaming-cleanup.md`, C2 |
| The streaming manager walks every row a tick | `streaming-cleanup.md`, C3 |
| The editor during an import | `streaming-cleanup.md`, C4 |
| The block world has no far ground | `streaming-cleanup.md`, C5, and `voxel-world-kickoff.md` |

## Superseded

- `phase-2-4-plan.md`: "a cell that promotes whole degrades into pure voxel and
  says so". ADR 0082 left the terrain one encoding; nothing promotes, and there
  is nothing to degrade.
