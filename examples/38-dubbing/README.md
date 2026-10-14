# 38 — Dubbing

Three speakers, six lines, two languages (ADR 0200): voices that are heard in
one language while the text is read in another, subtitles, an options screen
for both, and mouths that move with what is said.

Run it with `run.bat`, or `engine-host examples/38-dubbing`. The three buttons
open the options screen on its Audio page, change the voice language and
change the text language; `--locale=en --voice-locale=pt-BR` starts it in a
pair of your choosing.

- **The lines** are `content/dialogue/booth.lines.json`: who says each, in
  what colour, and where its recording is. `DialogueService:Say("booth.anna_1",
  bust)` is the whole of saying one.
- **A second language is a folder.** `content/voice/booth/anna_1.wav` is the
  line in the default language, and
  `content/l10n/pt-BR/voice/booth/anna_1.wav` is the same line in Portuguese:
  the same path under `l10n/<locale>/`. Nothing in the script names a
  language's file.
- **A line lasts as long as its longest language.** `anna_1` is 2.98 s in
  English and 3.88 s in Portuguese, and takes 3.88 s whichever is heard, so
  what follows it starts at the same moment for everybody.
- **Subtitles** are `require("@engine/subtitles").mount()`. As they come --
  "when needed" -- they show when what is heard is not what is read: a voice
  in the other language, `bruno_2`, which nobody dubbed into Portuguese, and
  `clara_2`, which nobody recorded at all and which lasts as long as it takes
  to read.
- **Three mouths, three ways.** Each bust has a `LipSync` under it, in the
  scene, and no script moves a mouth. Anna's follows the track beside each
  recording (`anna_1.visemes.json`, one for each language); Bruno's follows
  three bands of the sound, which tell an open mouth from a wide one from a
  round one; Clara's follows loudness alone. What each shape does to this
  face is `content/models/head.face.json`.
- **A language can ship apart.** `[export] voice = ["en"]` in `project.toml`
  puts English in the game and makes `dist/languages/l10n-pt-BR.lpack` of the
  Portuguese, for a player who wants it. `ludwerk voice` says which lines
  each language lacks.

**The recordings are not voices.** `tools/make_voices.py` is a small formant
synthesizer that reads each line's letters, and writes the track of what it
said; they are there so the example has two languages to play without
anybody's recording in the repository. Replace the files with a studio's and
nothing else changes. `tools/make_head.py` draws the bust and
`tools/make_scene.py` writes the scene.

See [Dubbing](../../docs/manual/audio/dubbing.md).
