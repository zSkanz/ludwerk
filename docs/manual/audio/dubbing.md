# Dubbing: voices, languages and subtitles

A game is written in one language and played in ten. Its text follows the
player's language through `LocalizationService`; this page is the other half
-- the voices, which language they are heard in, the subtitles under them,
the volumes a player sets, and a mouth that moves with a line.

`examples/38-dubbing` is all of it in one scene: three speakers, six lines,
English and Brazilian Portuguese.

## A second language is a folder

```text
content/voice/act1/intro_01.ogg              the line, in the project's default language
content/l10n/pt-BR/voice/act1/intro_01.ogg   the same line, in Brazilian Portuguese
content/l10n/ja/voice/act1/intro_01.ogg      and in Japanese
```

The same path and the same name, extension included, under
`l10n/<locale>/`. A game names the first, always:

```luau
--!strict
local line = Instance.new("Sound")
line.Content = "asset://voice/act1/intro_01.ogg"
line.Category = Enum.SoundCategory.Voice
line.Parent = speaker -- a BasePart: heard from there
line:Play()
```

`Play` is where the language is chosen. The file is looked for under
`l10n/<VoiceLocale>/`, then under the language alone (`l10n/pt/` for
`pt-BR`), then as it is named -- so a line nobody dubbed is heard in the
default language rather than not at all, and what is playing finishes in the
language it started in.

**To record a second language**: make `content/l10n/<locale>/`, put each
recording at the path its default-language file has under `content/`, with
the same name and extension, and run `ludwerk voice` to see what is still
missing.

It is not only sounds. An image under `l10n/<locale>/` is drawn in place of
the one an `ImageLabel` names when the text is read in that language: a sign,
a logo with words in it.

## The voice is not the text

`LocalizationService.Locale` is the language that is READ.
`LocalizationService.VoiceLocale` is the language that is HEARD, and a player
may want them apart -- the original voices under subtitles in their own
language is the commonest wish there is.

```luau
--!strict
local LocalizationService = game:GetService("LocalizationService")

print(LocalizationService:GetVoiceLocales()) --> { "en", "pt-BR" }  what this machine can play
LocalizationService.VoiceLocale = "pt-BR"    -- the player chose
LocalizationService.VoiceLocale = ""         -- and went back to following the text
print(LocalizationService.VoiceFollowsLocale) --> true
```

- **Until a player chooses, the voice follows the text**: the text's language
  when this machine has voices for it, else the first of the system's
  languages it has voices for, else the project's default.
- A choice is the player's and is kept for the next run, like their graphics
  settings. Writing a language this machine cannot play is narrowed to one of
  that language (`pt` hears `pt-BR`), and refused when there is none.
- `GetVoiceLocales` is what THIS machine can play: the languages inside the
  game and the language packs it has (below). The default language is always
  first. `GetSystemLocales` is the operating system's own list, for a game
  that wants to make its own first guess.
- `VoiceLocaleChanged` fires when the language in force changes, whichever
  of the two properties moved it.

## A line lasts as long as its longest language

A sound's length is part of the simulation: `Ended` fires on a tick, and in a
match every machine must agree which. But the Portuguese take of a line is
not as long as the English one. So **a sound recorded in several languages
lasts as long as the longest of them, on every machine, whatever language
each one hears**; a shorter take is followed by silence until the sound ends.

`Sound.TimeLength` is that length. It is the same number on a dedicated
server that ships no voices at all, because a built game carries the lengths
of every language in a small index beside its content.

The price is a pause after the shorter takes, so keep the takes of a line
close: `ludwerk voice` lists every line whose languages are more than 0.35
seconds apart.

## Lines: `DialogueService`

A game with more than a handful of lines keeps them in files:

```json
{
  "speakers": {
    "guide": { "name": "speaker.guide", "color": "#E8B04A" }
  },
  "lines": {
    "intro_01": { "speaker": "guide" },
    "intro_02": { "speaker": "guide", "sound": "asset://voice/act1/other_name.ogg" },
    "note":     { "text": "act1.note_on_the_door", "seconds": 4 }
  }
}
```

That is `content/dialogue/act1.lines.json`, and its lines are `act1.intro_01`,
`act1.intro_02` and `act1.note`. What an entry does not say comes from the
line's name: its words are the catalog key of the same name (`act1.intro_01`
in `i18n/<locale>.json`), and its recording is
`asset://voice/act1/intro_01.ogg`. A speaker's `name` is a catalog key too.

```luau
--!strict
local DialogueService = game:GetService("DialogueService")

local said = DialogueService:Say("act1.intro_01", guide.Head)
said.Ended:Wait()
```

`Say` makes a `Voice` sound with a `Caption` under it, parents it to the
instance given -- heard from there, when that is a part -- plays it, and
destroys it a moment after it ends. The sound it returns is what a script
waits on or stops.

**A line with no recording is still a line.** It is silent, its caption is
shown, and it lasts its `seconds` -- or, with none, a reading time counted
from its words in the default language: a second and a half, and six
hundredths of a second a character, twelve seconds at most. A game can be
written, played and timed before anybody is in the booth.

A `Caption` can also be made by hand, under any `Sound`: `Text` and `Speaker`
are catalog keys, `Color` is the speaker's, `Seconds` is how long a line with
no file lasts.

## Subtitles

```luau
--!strict
local subtitles = require("@engine/subtitles")

subtitles.mount()
```

That is subtitles: the captions of the sounds this machine can hear, at the
foot of the screen, with who is speaking in their colour and the words in the
language the player READS. `mount` takes a table of looks -- `Font`,
`TextSize`, `TextColor`, `Background`, `Width`, `Bottom`, `MaxCaptions`,
`DisplayOrder` -- and returns the display, whose `Unmount` takes it away.

**The player decides whether they are shown, and how**, with three
properties of `DialogueService` that are theirs and are kept for them:

| Property | Default | |
|---|---|---|
| `Subtitles` | `Auto` | `Auto`, `On` or `Off`. |
| `SubtitleScale` | 1 | How large, as a multiple. |
| `SubtitleBackground` | 0.6 | How solid what is behind the words is, 0 to 1. |

`Auto` is "when needed": a caption is shown when what is heard is not what is
read -- the voice in another language than the text, a line with no
recording in the player's language, a line nobody has recorded.

Nothing of the module is private. `DialogueService:GetCaptions()` is the
captions current on this machine in the order their sounds started,
`ShowsCaption(caption)` is whether the player's setting wants one shown, and
`CaptionStarted` and `CaptionEnded` say when one comes and goes: a game that
wants its own subtitles writes them with those.

## The player's volumes

Four numbers of `AudioService` belong to the player: `PlayerVolume`, which
turns everything, and `MusicVolume`, `EffectsVolume` and `VoiceVolume`, which
turn the sounds of each `Sound.Category`. They are kept with the player's
preferences and they multiply with whatever the game's own
[groups and master volume](manual:audio/mixing) say.

**Music gives way to a voice.** While a `Voice` sound is heard, every `Music`
sound is turned down to `AudioService.MusicUnderVoice` -- 0.4 as it comes, 1
for no ducking -- over about a sixth of a second, and comes back over a little more than
half a second when the line ends.

## The options screen

`require("@engine/settings").open()` has an **Audio** page: the four volumes,
the text's language, the voice's language -- with "same as the text" as its
first choice -- and the three subtitle settings. Every row of it is written
the moment it is chosen and kept at once; there is nothing for Apply to do.
A game in one language, or with voices in one, shows no row for that choice.

## A mouth that moves with the line

Parent a `LipSync` to the `MeshPart` that has the face's
[shape keys](manual:animation/morph-targets), and the mouth moves while a
`Voice` sound is heard from that character -- under the mesh's parent, or
under `LipSync.Source` when that is set. No script moves it. There are three
ways it is moved, and `Mode = Auto` takes the best there is:

1. **A track beside the recording.** `intro_01.visemes.json`, next to
   `intro_01.ogg`, says which mouth shape from when to when -- one for each
   language, beside that language's file. The free lip-sync tools write this
   file from a recording, and `ludwerk voice --visemes` runs one that is
   installed on your machine for every recording that has no track.
2. **Three bands of the sound**: how much of it is low, middle and high tells
   an open mouth from a wide one from a round one. No file at all.
3. **Loudness alone**: the jaw opens as far as the line is loud.

What a shape, a band and loudness do to ONE face is a file beside its model,
`hero.face.json` for `hero.glb`:

```json
{
  "visemes": { "A": {}, "D": { "JawOpen": 0.9 }, "F": { "JawOpen": 0.15, "Round": 0.95 } },
  "bands": { "open": "JawOpen", "wide": "Wide", "round": "Round" },
  "loudness": "JawOpen"
}
```

Every part is optional, and a face with no map at all and a key called
`JawOpen` is moved by loudness. `Smoothing` is how quickly the mouth follows,
`Weight` how far, and what a script sets with `SetMorphWeight` still wins.

It is picture: each machine moves the mouths it draws from what it is
playing, in the language it hears. A script that wants the numbers has
`Sound.Loudness` and `Sound:GetBands()`.

## Shipping languages apart

Voices are most of a game's size, and a player wants one or two of them.
`[export] voice` in `project.toml` names the languages that go INSIDE the
game; each of the others becomes a language pack of its own:

```toml
[export]
voice = ["en"]
```

`ludwerk build` then writes `dist/languages/l10n-pt-BR.lpack`, and a player
who has that file in the game's `.engine` folder can choose Portuguese. Left
out, every language ships inside. Either way the game times every line the
same, because the index of lengths is always inside.

## `ludwerk voice`

```text
ludwerk voice [path] [--locale=<locale>] [--all] [--visemes]
```

What each language has and lacks: lines with no words in its catalog, lines
with no recording, recordings no line names, speakers with no name, lines
whose languages differ in length by more than 0.35 seconds, and recordings
with no viseme track. `ludwerk check` says the totals in one line.

## In a match

- A line a server says is an instance like any other: the `Sound`, its
  `Category` and its `Caption` travel, and **every player hears it in their
  own voice language and reads it in their own text language**. A line a
  client script says is that machine's own.
- The language a player chose, their volumes and their subtitle settings are
  theirs: none of it is sent, and a server cannot set them.
- A `LipSync` travels with what it is on; the mouth is moved on each machine.

## Where to look next

- [Sounds](manual:audio/sounds) · [Groups, mixing and the listener](manual:audio/mixing)
- [Faces and shape keys](manual:animation/morph-targets)
- [Shipping a game](manual:guides/shipping)
- [`DialogueService`](api:DialogueService) · [`LocalizationService`](api:LocalizationService) ·
  [`Caption`](api:Caption) · [`LipSync`](api:LipSync)
