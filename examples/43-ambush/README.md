# 43-ambush

What a game tells a sequence as it runs (ADR 0204).

```
run.bat
```

An alley at dusk. A guard who is not in the scene steps out, says his line
and strikes; the camera goes round a wall to watch.

- **The guard is brought by the sequence**: a `cast` track names a stamp
  (`content/stamps/guard.stamp.json`) and a name, and the other tracks walk
  him in and aim at him under it.
- **How hard the blow lands is the game's**: the flash is keyed to
  `"$Blow"`, the `SequencePlayer`'s attribute, rolled by the script.
- **The camera's path is one curve**, and its third key is behind a wall on
  purpose: `"keepInSight": true` brings it this side of the wall.
- **The cut waits for the line to be over, as it is heard here**
  (`"afterLine"`), and the hand shakes when the blow lands (`"shake"`).
- **`BindCamera`** leans the lens in as the two close.
- **What comes after is a table**, written by the script and played by
  `LoadSequence`: the harder the blow, the longer the look.

Enter or Start: again, with another blow.

**To see where its frames go**: open it in the editor, *View > Profiler*,
then Play -- the cutscene's scripts are on the Scripts tab, and with *Sample
script lines* on, the lines of `@engine/subtitles` that draw its captions.
Or with no editor: `--profile-capture=ambush.profile.json`, and open the file
from the Profiler's bar.
