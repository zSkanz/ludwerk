# 0248 — A VR game ships to the Quest, plays online, and is tested without a headset

- Status: accepted
- Date: 2026-10-17
- Builds on: ADRs 0244-0247 (XR), the Android export (an APK and an App
  Bundle), ADR 0208 (every local player's character is predicted)

## The question

Three things finish the XR work: the same game running standalone on the
Meta Quest 3, players in headsets and players on screens in one
match, and a way to make and test an XR game -- in the editor with a headset,
and in CI with none.

## Decision

### 1. The Quest: an Android export with XR

The Quest runs Android, which the engine already exports to (arm64, an APK
and an App Bundle). A project with `[xr] enabled` exports for the Quest with
the target `quest` (`ludwerk export quest`, and in the Export window): the
same arm64 build with the Khronos OpenXR loader for Android beside it, and a
manifest that makes it a VR app -- `org.khronos.openxr.intent.category.
IMMERSIVE_HMD`, `com.oculus.intent.category.VR`, `com.oculus.supportedDevices`
(`quest3|quest3s|questpro`), the OpenXR permissions and `<queries>` for the
runtime, and the scene permission when the game asks for the room (ADR 0247).
Without the Oculus category the Quest starts an app in a flat window, which
is the most common mistake there is; the export puts it in and the export's
check refuses an XR build without it. The APK is delivered to the maintainer, who
installs it.

The Quest renders on Vulkan; foveation (`XR_FB_foveation`) is on by default,
and the default quality level for the target is `Medium` at a render scale the
runtime recommends, measured on the device before it is promised.

### 2. In a match: the head and hands are replicated as presentation

A player in a headset is a player like any: their character is predicted on
their machine (ADR 0208) and their inputs are actions (ADR 0245). Their
**head and hands** are sent to the server and on to the other players as
presentation -- 30 times a second, unreliable, the newest kept -- so another
player sees where they look and what they point at, drawn on their avatar.
They never enter the simulation's hash, and a server does not trust them for
anything a game decides: a hit is the server's (ADR 0235), and a hand that
says it is two metres from its head is held to a reach the game sets
(`XRService.MaxHandReach`, 1.2 m by default). **A flat player and a VR
player** share a match with nothing more: the flat one has no hands to send.

### 3. In the editor: Play in headset

The Play button gains **Play in headset**: the game runs in the editor's
process on the headset the PC's runtime offers (Link, Air Link, SteamVR),
while the editor's window shows the mirror (ADR 0244 §5). Stop ends the
session. Without a runtime the choice is greyed out and says why.

### 4. For tests and CI: a simulated headset

`--xr=simulated` runs a game with a headset that is the engine's own: two
eyes at a set separation and field of view, a head and two hands that follow
a recorded path (a JSON file of poses and inputs over time) or a script
(`XRService:SimulatePose`, available only with it), its "swapchains"
ordinary textures, and its frames pace at 72 Hz. It needs no OpenXR runtime
and no GPU beyond the one a test already uses (lavapipe in the container), so
every XR test in ADRs 0244-0247 runs in CI. It is not a stand-in for the
device: what only the device shows -- the rate held on the Quest, the
passthrough's colour -- is the maintainer's to see, and the ADRs say so.

### 5. The example and the manual

`examples/xr-room`: a room with objects to grab and throw, a teleport and a
snap turn (`@engine/xrcomfort`), a panel of UI pointed at by laser, a
passthrough switch, and, with a second player, the other's head and hands.
A manual section, "VR and AR": turning XR on, the play space and the camera,
input and hands, UI, comfort, passthrough and the room, the Quest export,
and multiplayer.

## Proof

- `ludwerk export quest` of the example: an APK whose manifest has every
  entry above, rejected by the export's own check when one is removed.
- **On the maintainer's Quest 3**, standalone: the example starts in VR (not in
  a window), holds 72 Hz in a minute's play, grabs and throws, teleports,
  and turns passthrough on (the maintainer's to see; the APK is delivered).
- Two clients under the simulated headset and one flat, in one match: each
  sees the others' heads and hands where they are within a tick of
  interpolation; the flat one sends none; a hand sent 3 m from its head is
  held at 1.2.
- Play in headset with no runtime: greyed out, with the reason.
- The XR tests of ADRs 0244-0247 run in the Linux container under the
  simulated headset.

## Consequences

- New: the `quest` export target; `--xr=simulated` and its recorded paths;
  `XRService.MaxHandReach`, `SimulatePose`; Play in headset; replicated head
  and hands (presentation, one protocol bump); `examples/xr-room`; the
  manual's "VR and AR".
- The Quest is the first standalone headset; Android XR headsets take the
  same build with their own manifest entries, added when one is tested.

**For later, so nobody is surprised**: a game published on the Meta Horizon
Store must pass Meta's Virtual Reality Checks (VRCs) -- among them the frame
rate held, the app's behaviour when the headset is taken off or the system
menu opens (the session's `STOPPING` and focus states), the guardian, the
controllers' and hands' input, and the store's own packaging. They are not
this work's proof, but each of these behaviours is the engine's to get right
first, and the export's check will grow the ones a build can be checked for.

## Not decided here

Publishing to the Meta Horizon Store (the maintainer's account and decision),
Apple's visionOS (closed under R15), and full-body avatars from the head and
hands (inverse kinematics is the animation category's, and a game's).

## As built, first step (2026-10-18)

- **The `quest` target** (§1) of `ludwerk build`, and a card for it in the
  Export window: the Android player and its settings, with the Khronos
  OpenXR loader the Android build already makes copied beside `libmain.so`
  -- in the package and in the APK -- and the VR entries written into the
  manifest. The entries are data in the Android template
  (`platforms/android/player/xr-entries.xml`), three sections copied between
  three pairs of marks in its manifest that a phone's build leaves empty:
  head tracking, hand tracking, OpenXR's permissions and the `<queries>`
  for the runtime; the Oculus and Khronos categories in the launcher's
  intent filter; `com.oculus.supportedDevices`. The export refuses a
  headset's build whose manifest lacks the Oculus category, and a project
  without `[xr] enabled`.
- `examples/48-xr-room` (§5) builds for it: an APK of 13.8 MB with
  `lib/arm64-v8a/libopenxr_loader.so` and every entry above; its Android
  build has neither.
- **Not yet**: foveation and the target's default quality (§1, to be
  measured on the device), the head and hands in a match (§2), Play in
  headset (§3), recorded paths for the simulated headset (§4), and the
  scene permission (with ADR 0247). Whether the APK starts in the headset
  is the maintainer's to see.
