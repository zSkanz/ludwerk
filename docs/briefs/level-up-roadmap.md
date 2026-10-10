# The level-up roadmap: nine categories, in order

The owner, on 2026-10-10, set the engine's next work as nine categories and
their order ("nessa ordem mesmo"), relayed by the orchestrating session and
confirmed by him the same day. This is the ledger: what each category is,
the stages it is expected to take, and where it stands.

## How it is run

- **The unit is the category**, not the stage (the owner, 2026-10-10: the
  work was going in pieces too small, and too much of the time to gates and
  CI). A whole category is built -- or, where one is truly too large, a half
  named in advance -- before the full gate. While building: the inner loop
  only, the build and the tests of the module touched. At the end: the
  determinism test, the full gate once, one push, one package, one report.
  What the gate finds is fixed in the same batch and only the lane that
  failed is run again. CI is read after the push and not waited on; a red
  one is fixed at the head of the next batch, and `main` moves only to a
  green commit.
- **What exists is read first.** The list below was named from what an
  engine of this class has, not from this code. Before a category starts,
  what the engine already does in it is read and written down here, under
  *What is there*; anything already built becomes an amendment, not a new
  feature.
- **One ADR a feature**, the ADRs of a category written together at its
  start and sent for approval in one message; the code does not wait for the
  answer. Each says which side of the world's hash its feature is on (R10)
  and names any dependency with its licence and pin (R5, R6: permissive
  only).
- **Each category leaves** an example under `examples/`, a manual page, and
  its tests. A defect found on the way is fixed in the batch that found it,
  with its test failing first.

## Before the first category

One short batch, together with the last stage of the shape keys (ADR 0196):

| Item | State |
| --- | --- |
| Shape keys: the editor's sliders, `examples/36-face`, the measurements | built |
| A start that fails leaves a trace (D608) | built |
| A recording carries each controller; ADR 0195's owed tests | built |
| The 2 GB phone (D609): the interface's pictures keep to a budget, and `TextureQuality` is applied and starts at the machine's memory | built |

**A scene that is left takes what it alone held** (D610): fixed at the head
of the animation batch, on the orchestrator's objection that it is a leak on
a real game and not a streaming feature.

**Left for its category, and written down so it is not found again**: a
budget for the world's own textures WITHIN a scene.
`GraphicsService.TextureStreamingBudget` is kept and read by nobody, and
nothing releases a texture or a mesh when the chunk that used it is streamed
out. That is category 7's (the world), beside occlusion culling.

## 1. Animation

A character that moves like one without game code doing the blending. One
batch, the three ADRs written together:

1. **Animation graphs** (ADR 0197): states that are clips or blends of clips
   along one parameter or across two, transitions with a condition and a
   fade, layers that replace or add over a mask, events on a clip; an asset,
   driven by parameters a script sets or that are read from what replicates.
   No root motion.
2. **Inverse kinematics** (ADR 0198): a limb to a target, a look shared up a
   chain, two feet on the ground -- as picture, in the presented pose.
3. **Retargeting** (ADR 0199): one clip on bodies of other proportions, role
   to role, as turns from rest.

At the head of the batch, a defect the orchestrator would not see deferred:
a scene that is left takes what it alone held (D610). And two the survey
found: the skeletal manual, stale in three places (D614); a re-exported
model's joints and clips, which did not reload (D611).

**Built, 2026-10-10 and 11.** What the batch turned out to hold beyond its
three decisions, each found by proving something before building on it:

- **Protocol 44.** The graph was designed to add nothing to the wire, and
  the two-process proof of that failed: a character a server spawns reached
  a joiner without its `AnimationPlayer` -- and without its lights, its
  cape, its sounds, its trail (D612), and with its emitter's colours and
  sizes missing (D613). Nothing had exercised it; every test character was
  in a scene file. The classes a spawned thing carries now travel, as
  authored, with nothing sent a tick; the multiplayer guide has the table,
  and a development run says which classes still do not.
- `ContentProvider:Keep` and `:Release`, the late-load warning and the
  written contract of what a scene's sweep counts as named (D610's second
  half).
- A third value of `Enum.Retargeting`, `Off`, was drawn and not built: by
  name the clip's transforms already are as they are.
- The engine's own text editor does not open a graph (it knows two
  languages and JSON is neither): a graph is edited outside, and the drawn
  editor is the fourth category's.

It leaves `examples/37-character`, the manual's three pages (graphs,
retargeting, reaching and looking), and its tests: the graph alone and in the
pose walk, the roles and the map, the solvers and the controls, the
conformance suite's API cases, a determinism scenario, and two two-process
gates (`netcode_carried`, `netcode_animated`).

**Animation's next steps**, named and not scheduled: root motion (a clip
that moves the body it plays on); hiding what a worn piece covers, so a body
does not poke through its armour; and merging a character's finished set of
pieces into one mesh, which a crowd of dressed characters will want.

*What is there*: `AnimationPlayer` and `AnimationTrack` with weights and
fades, clips from another file by joint name, `Bone` for attachments and
turns, ragdolls, `SpringBone` for secondary motion, shape keys. To be read
in full before the category's ADRs.

*To decide in the ADRs*: which of these drive what is SIMULATED. A pose
feeds sockets, hitboxes and ragdolls today, so a state machine's blend is on
the simulation's side of the hash; a foot planted by IK may be picture only.

## 1b. Dubbing: a voice a language

The owner's priority, 2026-10-10: the batch right after animation, in front
of category 2. He is making a game now and wants to dub it. One piece:

1. **Localized assets.** A game asks for one name (`asset://voice/intro_01`)
   and the engine resolves the file of the language in force, falling back to
   the project's default language and, in a dev run, warning once a missing
   file. Sounds first; the same resolution for images and video with text in
   them. A change of language at run time takes effect on the next play, with
   `LocaleChanged` as the signal a game already has.
2. **A voice language apart from the text language**: two settings, the
   second defaulting to the first -- voice in one language and subtitles in
   another is the ordinary case. Both in the settings system that exists,
   saved with the player's preferences.
3. **A dialogue line as a unit**: text key, sound, speaker and its length, so
   playing a line plays the voice in the voice language and shows the
   subtitle in the text language for as long as it lasts. One call from a
   script; shaped now for the sequencer of category 3, which gets a track of
   these.
4. **Subtitles ready to use**: a component that shows the current line with
   its speaker, size and background adjustable by the player (an
   accessibility requirement on consoles), on by default when the voice
   language differs from the text language, through the catalog like every
   other string (R3).
5. **A voice bus**: a voice volume the player sets apart from music and
   effects, and music ducking under a line without game code -- on what
   `AudioGroup` already gives.
6. **Export: language packs.** Voice is heavy: `[export]` names the languages
   that ship in the base pack, and the others can ship as separate packs the
   game mounts when present. The ADR says how this sits with the sealed
   container of ADR 0183.
7. **A report**: one command listing, a language, the lines with no sound and
   the keys with no text, for use before a release (and in `ludwerk check` if
   it is cheap).

In a match, a line a server starts is heard by every player in their OWN
voice language: `Sound` travels by name since protocol 44, and the name is
resolved on each machine. An explicit case in the tests.

8. **A mouth that moves with the line** (the owner, 2026-10-11; it was the
   named next step and is in the batch): what a machine is playing of a
   sound read by code -- its loudness, and three frequency bands -- and a
   viseme track beside a recording, a language its own, that the engine
   plays through a mapping the model's author gives; loudness is the
   fallback of the bands and the bands of the track. `LipSync` moves a mouth
   with no code. The engine reads a track and never makes one: a tool does,
   and `ludwerk voice --visemes` runs the common free one when the developer
   has it installed.

**At the head of the batch**, from the review of the animation batch: the
lint that every property a script can write on a replicated class is sent or
withheld by name (D617's lesson as a gate); what a send costs, which protocol
44 had raised by a fifth (found in the periodic reading of every instance,
not in the new fields: a write now stamps the instance it is made on);
retargeting run over real rigs -- the owner's own heroes and two from the
wild -- and `examples/37-character` remade on a public-domain character and
clip library.

**And inside it, the owner's decision of 2026-10-10: a mesh that wears
another's pose** (ADR 0201, `MeshPart.PoseFrom`). A weapon with joints of its
own moved by the body's clip, and the modular character -- armour, clothes,
hair swapped in play -- as one thing with the body: the same joints in the
same places on the same frame, under IK and spring chains, at the cost of a
draw and not of a pose. `examples/37-character` gains a bow whose string the
draw pulls and a piece of armour that is swapped.

Protocol 45 for the three: `Sound.Category`, `Caption` and `LipSync`,
`MeshPart.PoseFrom`.

It leaves an example a person can open -- `examples/38-dubbing`: a short
scene, speakers in two languages, subtitles, and three mouths side by side,
one moved by a track, one by the bands and one by loudness -- and its manual
page.

**Built, 2026-10-10.** ADRs 0200 and 0201, protocol 45. What the batch turned
out to hold beyond what it was asked for, each found by proving something
before building on it:

- **A line lasts as long as its longest language**, on every machine. It was
  the one decision the pieces above did not make and could not go without: a
  sound's length is the tick's, and two machines hearing two languages would
  have ended a line on two ticks. The lengths of every language are in an
  index a built game carries, so a server with no voices and a player with
  one language agree; `ludwerk voice` lists the lines whose takes are far
  apart, because the price is a pause after the shorter one.
- **A property whose write is a choice** (D622): `VoiceLocale` reads as the
  language in force and is written as the player's choice, and the property
  layer dropped a write that read the same.
- **Real rigs**, read and carried: the owner's heroes (one skeleton of
  sixteen joints; not a body before -- their forearms had no role -- a body
  of fifteen roles now), two public-domain character packs and a
  public-domain mannequin. What that corrected: a lower arm called `Fore`,
  bare finger names, rigs that face the other way or stand along another
  axis, hips named on a joint the legs do not hang from, joints above the
  hips matched by name, and a back that starts below the hips. And three
  things that were not retargeting at all: a piece parented to a bone stayed
  at rest (D624), a graph read speed in metres whatever the legs (D625, the
  slide), and a worn piece left its scene for a streamed cell (D626).
- **`examples/37-character` on free content**: one pack's rig and eleven of
  its clips as the library, that pack's rogue as the player, another of its
  characters stretched as the walker and another pack's mannequin standing.
  The held thing is a crossbow and not a bow -- the library has a shot and a
  reload for one and nothing for the other, and a string a clip draws is the
  point -- with a `String` joint added to the library's rig and keyed in
  those two clips. The mannequin was tried as the walker and stands instead:
  a short body's walk on legs twice as long is a march, and the example says
  so rather than hiding it.
- **A label sized to its words wrapped on a scaled screen** (D623), found in
  the first picture of a subtitle; a re-recorded sound was not heard until a
  restart (D620); `PlayLocal` kept every sound it ever played (D621).

Not built, and said: the `LipSync.Map` picker lists nothing (it is a plain
name); a sealed language pack is not checked to hold only its own language's
names; in the editor, which does not tick, a follower is drawn at rest; the
Android export of voices and `ludwerk voice --visemes` against a real tool
were not run here. `DialogueLines::load`'s diagnostics are English inside a
catalogued sentence.

Its tests: the conformance suite's dubbing, subtitles, options and `LipSync`
cases; two determinism scenarios (`dubbed`, replayed in two voice languages
against one trace, and `worn`); the two-process gate `netcode_dubbed` (a
joiner hears and reads another language than the server said the line in)
and the worn piece in `netcode_animated`; the packaging test's language
pack; `example_dubbing_speaks_en` and `_pt-BR`.

*To be read first*: `LocalizationService` (the orchestrator found only its
text side: `Locale`, `GetLocales`, `Translate`, `LocaleChanged`), how a sound
is resolved and loaded, `AudioGroup`, the settings system, and the export's
packs.

## 2. Graphics: indirect light

Interiors and shadowed sides that are not flat.

1. **Reflection probes**: captured or baked cubemaps, placed and blended, in
   place of the one sky environment everything reflects today.
2. **A simple global illumination**: baked, or by probes -- the ADR
   recommends one, with what each costs a phone.

Folded in from the old queue: decals without the mask drawn again (audit c),
the prepass on a handheld and runs of two (audit d), what a change of scene
uploads and compiles (audit e).

*To be read first*: what ambient occlusion, contact shadows and image-based
lighting already do, and whether anything like screen-space reflections
exists.

## 3. Cinematics

1. **A sequencer**: a timeline with tracks for a camera, an animation, a
   sound and an event, authored in the editor and played from a script.

## 3b. Several players at one screen

The owner's, 2026-10-10; a batch of its own, after cinematics and before the
editor, and not a high priority. His case: a game seen from above for two to
four players on controllers at one machine, on a console, the camera framing
everybody and splitting when they walk apart. All four are "must have
support":

1. **Split screen in the window itself**: a view a local player, drawn into
   the window -- not a `CameraTexture` shown through the interface, with its
   cap on resolution and a whole scene pass each -- in fixed layouts for two,
   three and four.
2. **The dynamic split**: one shared view while the players are close; a
   dividing line that appears, turns with the direction between them and
   goes when they rejoin. The engine's is the mask and the composite; where
   the line sits and when it appears may stay the game's (the ADR decides).
3. **Interface a player**: each local player navigating their own piece of
   interface with their own controller at the same time -- four players each
   picking a card. ADR 0195 lists "no interface focus a player".
4. **Local guests in an online match**: a machine with two players at it
   hosting or joining. ADR 0195 refuses it today. This is the one that
   reaches the wire: the ADR says whether it is protocol 44, folded with
   whatever other change of protocol is pending then.

Its ADR amends 0107 (which names split screen as the next use of its base)
and 0195, and says: what views share and what they do not (the sun's shadow
maps, culling, exposure and history a view, by ADR 0107's view state); the
cost measured for one, two and four views against one; audio with several
listeners; and what a run with three and four controllers on hardware still
needs from the owner.

## 4. Editor

1. **A visual profiler** inside it: frame by frame, by system and by script.
2. **A node editor for materials**, on top of the surface shaders.
3. **A drawn editor for animation graphs** (ADR 0197), on the same canvas as
   the material nodes: states and arrows, saving the `*.animgraph.json` a
   graph already is. Until then a graph is a file, with a live readout in
   Properties.

Folded in: an export that reuses the import cache (audit b), meshes compiled
side by side (audit f), the rest of the settings work (3c).

*To be read first*: what `--frame-stats`, the frame report and the Stats
panel already show.

## 5. Physics

1. **A vehicle**: wheels and a suspension.
2. **Mesh cloth**: the second tier of ADR 0194, on its colliders and dials.
3. **Destruction**.

## 6. Audio

1. **Occlusion**: a sound muffled behind a wall.
2. **Voice chat** for multiplayer.

## 7. World

1. **Levels of detail made automatically**.
2. **Occlusion culling** of what is hidden.

*To be read first*: the compiler already makes a chain of levels for a mesh
(`asset/mesh_format.h`) and the renderer chooses among them; what is missing
is to be established before this is called a feature.

## 8. Platforms

1. **The web**: a game that opens from a link. Opened by the owner on
   2026-10-10 (R15).
2. iOS follows the web and is **not open**: it needs the owner's Mac and his
   Apple account.

Folded in: G19, G20's fallback fonts, mobile M2 and M3, the gamepad proof.

## 9. Ecosystem

1. **A package manager** for sharing Luau modules and assets.

## What jumps the line

D596 -- a Windows 10 player's Direct3D 12 -- when its log arrives.
