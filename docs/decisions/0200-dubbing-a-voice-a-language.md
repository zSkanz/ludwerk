# 0200 — Dubbing: a voice a language

- Status: accepted; built in batch 1b of the level-up roadmap (protocol 45)
- Date: 2026-10-10
- Decided by: the owner (he is making a game and wants to dub it; the batch
  right after animation); the orchestrator (the seven pieces, the system's
  language list for a script, the provider's place in the starting order);
  detailed by the agent from the code
- Builds on: `LocalizationService` (text), ADR 0131 (audio effects), ADR 0147
  (settings), ADR 0183 (the sealed container), ADR 0188 (optional provider
  modules), ADR 0196 (shape keys), protocol 44 (a `Sound` travels by name),
  R3, R10

## The question

A game is written in one language and played in ten. Its text already
follows the player's language. Its voices do not: a line is a file, and the
file is in one language. What does a game ask for so that the player hears
the line in theirs, reads it in theirs -- which may be another -- and can
turn each of them up, down and off?

## What is there

Read before this was written.

- **Only the text side exists.** `LocalizationService` has `Locale`,
  `GetLocales`, `Translate` and `LocaleChanged`; catalogs are
  `i18n/<locale>.json`. The language at start is the saved choice, else the
  first of the system's preferred languages a catalog covers, else the
  project's default. A script cannot see the system's list.
- **One resolver, exact names.** `asset::ContentMounts::resolve` matches a
  name exactly, extension included, and a sealed pack stores names as hashes:
  it can be asked for a name and never listed. There is no notion of a variant
  of an asset by language, platform or quality.
- **A sound's length is simulation.** `Sound.TimePosition` advances on the
  tick, `Ended` fires from the file's length, and both are in the world's hash.
  On a replica a sound stops when the authority's `Playing` turns false.
- **The mix is one multiplier a group.** `AudioGroup.Volume`, no nesting, no
  ducking, and a sound's group does not travel. `AudioService.MasterVolume` is
  the game's and is saved nowhere. The settings system is graphics and display
  only; ADR 0147 left audio "to its own service".
- **No subtitles, no dialogue, no loudness.** The one precedent for interface
  the engine ships is `@engine/settings`: Luau over the public API, its
  strings in the engine's catalog.
- **Export puts all of `content/` in one pack.** `ContentMounts` can hold any
  number of mounts and the host mounts exactly one.

## The decision

### 1. A localized asset is a file of the same name under `l10n/<locale>/`

`content/voice/intro_01.ogg` is the line in the project's default language.
`content/l10n/pt-BR/voice/intro_01.ogg` is the same line in Brazilian
Portuguese. A game names the first, always:
`asset://voice/intro_01.ogg`. The name keeps its extension, as every asset's
does -- the resolver matches names exactly and a sealed pack cannot be
searched for "any extension".

Resolving a name for a language tries `l10n/<locale>/`, then
`l10n/<language>/` (`pt` for `pt-BR`), then the name itself. So a missing
file is the default language's, never silence. In a dev run, a name that has
a file in some language and none in the one in force is said once.

- **Sounds follow the voice language** (below), every `Sound`, not only
  dialogue: a jingle with words in it is localized the same way.
- **Pictures in the interface follow the text language**: `ImageLabel` and
  `ImageButton`, for a logo or a sign with words in it. Textures on meshes do
  not in this batch; video does not exist yet (ADR 0122) and takes the same
  rule when it does.
- A change of language is heard at the next `Play`. What is playing finishes
  in the language it started in.

### 2. A localized sound is as long as its longest language (R10)

A line is 2.1 s in English and 2.8 s in German, and the simulation must not
depend on which the player hears: two machines in a match hear different
languages, and a replay recorded in one must reproduce in another. So
**`TimeLength`, and the tick `Ended` fires on, are those of the longest of
the languages the project has for that name** -- the same number on every
machine, whatever is installed and whatever is chosen. A shorter language is
followed by silence until the line ends; nothing is cut, on a server, a
client or a replay. It is what a dubbed film does: the slot is the line's,
and each language fits inside it.

In a dev run the lengths are read from the files' headers when a name is
first used. An export writes them into the game (`asset://l10n/index.json`,
made by the asset compiler: the voice languages, and each localized sound's
longest length in frames), so a machine without a language's files -- a
language pack not installed, a dedicated server -- still has the number.

**Its price is said to the author.** A line recorded long in one language
pads that line with silence in every other, and in a chain of lines that
reads as slow dialogue. `ludwerk voice` lists each line's shortest and
longest recording and flags a spread above 0.35 s, so the outlier is recut
instead of every other language waiting for it; and the manual says, in one
sentence, that a line lasts as long as its longest language.

Rejected: each machine's own file's length. It makes `Ended` a host fact:
replicas are cut by the authority's shorter line, and a replay diverges.
Rejected: time-stretching, which no voice survives.

### 3. A voice language apart from the text language

`LocalizationService` gains:

- `VoiceLocale: string` -- the language sounds resolve in. A host fact like
  `Locale`: outside the hash, each machine's own, refused on a dedicated
  server. **Written empty, it follows the text language** -- the default --
  narrowed to the languages the game has voice for.
- `GetVoiceLocales(): {string}` -- the languages this machine can PLAY:
  the default, and each `l10n/<locale>/` the game shipped or a mounted
  language pack brought. A menu made from it never offers a voice that is
  not installed. A saved `VoiceLocale` whose pack has since gone is narrowed
  again at start, as a language with no catalog is: the voice follows the
  text language, and nothing warns a line.
- `VoiceLocaleChanged(locale)`.
- `GetSystemLocales(): {string}` -- the system's own preferred languages, in
  its order, as the platform reports them and not narrowed to anything. For
  "your system's language" in a menu, and so the voice can start in the first
  of them the game has VOICE for, which need not be the one it has text for.

Both choices are saved with the player's preferences (`settings.json`,
`"locale"` and `"voice_locale"`), written when a script sets them.

**The starting order gains a place**: the saved choice, then **the platform
provider's language when a provider gives one**, then the system's list,
then the project's default. A store's client has a language setting of its
own, and a player who set the game to French there expects French on a
machine whose system is in English. No store's SDK enters the engine for
this (ADR 0188): the order asks `platform::providerLocales()`, which is empty
until an optional provider module answers it.

### 4. Three categories, the player's volumes, and music under a voice

- `Sound.Category: Enum.SoundCategory` -- `Effects` (the default), `Music`,
  `Voice`. On the sound and not on an `AudioGroup`: a group is each machine's
  own and does not travel, and a line a server starts has to arrive as a
  voice. An `AudioGroup` stays what it is -- the game's own sub-mix and where
  an effect chain sits.
- `AudioService.PlayerVolume`, `MusicVolume`, `EffectsVolume`, `VoiceVolume`
  -- the PLAYER's, 0 to 1, host facts saved with the preferences.
  `MasterVolume` stays the game's (a fade to black writes it every frame, and
  that must not become somebody's saved setting).
- **Music lowers itself under a voice**: while a `Voice` sound is audible on
  this machine, `Music` is multiplied by `AudioService.MusicUnderVoice`
  (default 0.4; 1 turns it off), eased over 0.15 s down and 0.6 s back. No
  game code, and no sidechain to wire.

A sound's gain is its own volume, its group's, the game's master, the
player's master, the player's volume for its category, the ducking if it is
music, and the falloff.

### 5. A caption is what a sound says; a line is a sound with one

- **`Caption`**, a child of a `Sound`: `Text` (a catalog key), `Speaker` (a
  catalog key, or empty), `Color` (the speaker's), `Seconds` (how long it
  shows when the sound has no file; 0 is "a reading time for the text").
  While the sound plays and can be heard on this machine -- in range, for a
  positional one; the player's volumes are not asked -- the caption is
  current. It serves a line of dialogue and "[a door creaks]" alike.
- **A sound with a caption and no file in any language is silent and lasts
  its caption** (`Seconds`, or 1.5 s plus 0.06 s a character of the DEFAULT
  language's text, the same on every machine). A game is written before it
  is recorded: its lines play as text from the first day, and the report
  below lists what is still to record.
- **`DialogueService`**:
  - `Say(line: string, at: Instance?): Sound` -- one call. Makes a `Voice`
    sound with its caption from the line's entry, under `at` (heard from
    there if it is a part) or under `Workspace`, plays it, and destroys it
    when it ends. On a server it is an instance like any other: it travels,
    and **each player hears the line in their own voice language and reads
    it in their own text language**, because a name is resolved where it is
    played. Category 3's sequencer gets a track of these.
  - `GetCaptions(): {Caption}` and `CaptionStarted` / `CaptionEnded` -- what
    is being said on this machine now. Player side.
  - `Subtitles: Enum.SubtitleMode` (`Auto`, `On`, `Off`), `SubtitleScale`
    (0.75 to 2), `SubtitleBackground` (0 to 1) -- the player's, saved. `Auto`
    shows a caption when what is heard is not in the language being read: the
    voice language differs from the text language, the line fell back to the
    default language, or it has no recording.
- **Lines are files**: `content/dialogue/<name>.lines.json`, a table of
  lines by id and of speakers. A line's id in the game is `<name>.<id>`; its
  text key is that id and its sound is `asset://voice/<name>/<id>.ogg` unless
  the entry says otherwise, so a file of five hundred lines is five hundred
  ids and who says each.

### 6. Subtitles ready to use

`@engine/subtitles`, beside `@engine/settings` and made the same way: Luau
over the public API. `Subtitles.mount()` shows the current captions at the
bottom of the screen -- the speaker's name in their colour, the text wrapped,
two lines at most a caption, over a background -- sized by `SubtitleScale`
and `SubtitleBackground`, shown as `Subtitles` says. Its text is the catalog's
(R3) and follows `LocaleChanged`. `@engine/settings` gains an **Audio** page:
the four volumes, the two languages, and the subtitles' three settings.

### 7. Export: language packs

`[export] voice = ["en", "pt-BR"]` names the voice languages that ship in
the game's pack; absent, all of them do. Every other language's SOUNDS go
into a pack of their own, `l10n-<locale>.lpack`, written beside the export
under `languages/`. A language pack is a sealed container like the game's
(ADR 0183: hashed names, verified at start) that holds nothing but
`asset://l10n/<locale>/...`; the host mounts each one it finds in the game's
folder at start, after the game's own pack. Pictures stay in the game's pack
in every language: they are small.

`GetVoiceLocales` answers with what is mounted, so a menu offers what is
installed. **Not in this batch**: mounting a pack while the game runs, and
any delivery of one (a store's optional content, an Android asset pack on
demand) -- on Android every language named ships inside the game.

### 8. A report

`ludwerk voice [path]` lists, a language: the lines with no text, the lines
with no recording (and so heard in the default language), the lines with no
recording in any, and the sound files under `l10n/` that no line names.
`ludwerk check` fails on a line with no text in the default language and
says the rest.

### 9. A mouth that moves with the line

(The owner, 2026-10-11: a character's mouth moves with what it says, read
from the sound by code. Three ways, each better than the one before and each
the fallback of the next. All of it is picture: each machine's own, of the
language that machine hears, never on the tick.)

- **`Sound.Loudness`** -- how loud what this machine is playing of the sound
  is right now, 0 to 1. A jaw that opens with the line.
- **`Sound:GetBands()`** -- the same moment split in three: low (80 to 500
  Hz), mid (500 to 2,500) and high (2,500 to 10,000), each 0 to 1, as three
  numbers and no table. Enough to tell an open mouth from a wide or a round
  one with no file at all, and what a music visualiser reads. Worked out
  when asked and not otherwise: a sound nobody asks about costs nothing.
  `AudioService:GetBands(category)` answers for everything of a category the
  mixer is playing.
- **A viseme track**: a file beside a recording, of the same name --
  `voice/act1/intro_01.visemes.json` beside `intro_01.ogg`, and a language's
  own beside that language's recording under `l10n/<locale>/`. It holds mouth
  shapes over time: `{ "cues": [ { "start": 0.0, "end": 0.12, "value": "A" },
  ... ] }`. **The engine reads a track; making one is a tool's job**: no
  speech recognition and no new dependency enters the engine. The file the
  common free lip-sync tool writes is read as it is (`mouthCues` is `cues`,
  its other keys are ignored), so an author can use that tool today, and
  `ludwerk voice --visemes` runs it over the recordings that have no track
  when it is found in the developer's own installation -- located, never
  bundled, never fetched, the rule for anything of a vendor's.

**`LipSync`**, a child of a `MeshPart` with shape keys, is what moves a mouth
with no code: `Source` (the instance whose `Voice` sounds are this mouth's:
a head, a character), `Mode` (`Auto`, `Bands`, `Loudness`), `Smoothing` and
`Weight`. Under `Auto`, a line that has a track in the language being heard
plays it; one that has none falls back to the bands, and a mesh with one
jaw key to loudness. What a viseme, a band and loudness DO to the face is the
model author's, in a file beside the model as its roles are (`hero.glb`,
`hero.face.json`): a viseme's name to shape keys and weights, which key
opens, which widens, which rounds. `LipSync.Map` names another file.

It writes a layer of its own on the mesh's shape keys, under what a script
sets and over what a clip plays, and leaves both alone.

A line's track is data with a start and an end, by name: category 3's
sequencer puts it on a timeline as it is.

`ludwerk voice` lists, a language, the lines with a recording and no track.

**What a mouth costs a frame**, measured (`mouths_tests.cpp`, the mean of the
mouths' own step over three hundred frames, three runs): by a track, 0.35
microseconds for one speaker and 8 for twenty; by the bands, 7 and 137; by
loudness, 2 and 39. The bands were five times that when they were taken from
the whole window a level is averaged over: they are read from its last ten
milliseconds, which is enough to tell a mouth that opens from one that
rounds. A script's own `Sound.Loudness` and `Sound:GetBands()` are the same
work, about two and seven microseconds a call, once a frame a sound however
many ask.

## The wire

Protocol 45 (with ADR 0201 and the capture's write stamps): `Sound.Category`
and the classes `Caption` and `LipSync` travel, authored, as protocol 44's
rule has it -- a mouth on a character a server spawned moves on every
machine, each from the track of the language it hears. `VoiceLocale`, the
player's volumes, the subtitle settings, loudness, bands and what a track
does to a face are each machine's own and are not sent.

## What it leaves

`examples/38-dubbing` -- a short scene, two speakers, two languages, voice
and text chosen apart, subtitles -- a manual page (`audio/dubbing`), and
tests: resolution and fallback; the longest language's length on a machine
that has only one; a line a server starts heard in each joiner's own voice
language (two processes); the ducking; the report.

## Found on the way, and fixed with it

- A changed sound file is heard without restarting the world (hot reload did
  not reach the audio cache) -- a re-recorded line is the case.
- `AudioService:PlayLocal` left its `Sound` behind for ever.
- The manual's audio pages against the code: `Play` on a playing sound,
  `Resume`, `TimeLength`, panning.

## Decided while it was built

- **A write of `VoiceLocale` is a choice** (D622). The property reads as the
  language in force and is written as what the player chose, and the property
  layer drops a write that reads the same: choosing the language already
  heard was not kept. A property may now say so (`Choice`), and this is the
  one that does.
- **A sound the engine made for one playing takes itself away** (D621), two
  ticks after it ends: `PlayLocal`'s and `Say`'s. Two ticks, so that what
  listens to `Ended` has heard it.
- **The options screen writes these at once.** A volume, a language and a
  subtitle setting are the player's the moment they are chosen and are kept
  at once; Apply and Revert are for what moves the window.
- **The example's recordings are synthesized.** No voice is in the
  repository: `examples/38-dubbing/tools/make_voices.py` is a formant
  synthesizer that reads each line's letters and writes the viseme track of
  what it said, in the place and under the names a studio's files have.
