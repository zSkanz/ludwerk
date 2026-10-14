# 40-cinematic

A cutscene that is a file (ADR 0204): two people meet in a room, the camera
cuts between them and looks at whoever speaks, each says a line, the lamp
comes up and a door opens.

```
run.bat
```

- **Space** or the south button skips it: the lamp is up and the door is
  open, as at the end, and the event that says so arrives marked `skipped`.
- **P** pauses and carries on. **Enter** or Start plays it again.
- **V** changes the voice's language and **T** the text's -- English and
  Brazilian Portuguese. The sequence is as long in one as in the other.

## What is where

| | |
|---|---|
| `content/sequences/meeting.sequence.json` | The sequence: a camera track, three lines, a clip, a blink, the lamp's brightness and colour, the door's place and turn, two events. Written to be read. |
| `content/scenes/meeting.scene.json` | The room, the two speakers, and `Meeting` -- the `SequencePlayer`, with a `SequenceBinding` for each name the file expects. |
| `content/dialogue/meeting.lines.json` | The three lines: who says each and where its recording is. |
| `content/voice/meeting/`, `content/l10n/pt-BR/voice/meeting/` | The recordings in two languages, with a viseme track beside each. A synthesizer reading the lines, not voices (`tools/make_voices.py`). |
| `src/client/init.luau` | Plays it, skips it, pauses it, and says what it hears. Nothing else: no camera is handed over, no door is tweened. |

## In the editor

Open the project, select `Meeting` in the Explorer and open **View >
Timeline**. Drag on the ruler: the room is shown where the sequence is at
that moment -- the lamp, the door, the view -- and put back when the panel
lets go. Turn **Look through the camera** off to fly somewhere, then **Key
camera from view**.

## What to notice

- `workspace.CurrentCamera` is never written. The game's own camera stands
  back by the door the whole time; the sequence takes the view and gives it
  back, and no line of the script is about either.
- The camera looks at `Anna/Bust/Face`: an `Attachment` where the face is.
  Looking at `Anna`, the model, is looking at the middle of a stand.
- The lamp and the door have a key at nought. A property is the sequence's
  from its track's first key to its last, so those keys are what put the
  room back when it is played again.
- `Meeting.Controls` is `Suspend`: a game with a character would have it
  held still. The keys above still arrive.

The files under `tools/` regenerate the scene, the head and the voices; what
they write is checked in.
