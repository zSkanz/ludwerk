# The profiler

Where a frame's time went: by system, by script, by pass of the GPU. In the
editor, **View > Profiler** (or the command palette, `Ctrl+Shift+P`, and
*Profiler*). It opens under the world, beside the Console.

Open it before you press Play if you use the palette: the palette is the
editor's, and a running game has the keyboard.

## Reading it

**The strip** across the top is the last six hundred frames -- ten seconds at
sixty a second -- a bar a frame. A bar is
as tall as the frame was long and is colored by what the time went on:
simulation, scripts, rendering, interface, waiting, and the rest. The line
across it is your display's frame -- 16.7 ms at 60 Hz. A frame that ran over
that by more than a tenth has a red mark at the top, and the line of words
above the strip counts them.

*Waiting* is not work: it is the frame held for the display, or for the rate
you capped it to. A game that keeps to the display is mostly waiting.

**Press on the strip** to choose a frame; drag to move along it; the left and
right arrows step one frame. Choosing a frame holds the run -- nothing new
comes in while you read -- and **Resume** lets go.

**Worst frame** goes to the longest frame on the strip. **Next worst** goes to
the next longest after the one you are on. A hitch is usually one of the
first three.

With no frame chosen, the tabs show the *median* of the frames on the strip:
what an ordinary frame costs.

## The tabs

| Tab | Shows |
|---|---|
| Systems | The engine's own tree of what it did, each row with its time, the part of it that the rows under it do not account for (*Its own*), and how many times it ran. |
| Scripts | Each script's time in the frame and how many times it was resumed -- and, with lines sampled, where in the scripts the time goes. |
| GPU | Each pass of the frame and its time, once you ask for them. |
| Memory | Textures and buffers on the graphics device, and the script heap by what holds it. |
| Network | In a match: ping, jitter, loss, and the corrections that landed in this frame, for each player at this machine. |

**Systems is a tree, heaviest first.** Under each row are the things it
did, the longest at the top, so reading down the first rows is reading where
the frame went. Press a row's arrow to fold or open it. When you choose
another frame the tree opens along that frame's heaviest path and shuts the
rest. Press the *Scope* column's header to have the rows in the order the
engine did them instead; press *Time* to go back.

## Two switches, and what they cost

Reading the frames costs little: under a tenth of a millisecond a frame on a
desktop. Two things cost more, and are off until you ask.

**Time the GPU's passes.** A pass can only be timed by stopping the frame
after it, so the frame is slower while this is on -- by milliseconds. The GPU
tab says how much of the frame you are looking at was that. Turn it on to
find *which* pass is dear; do not read the frame's length while it is on.

**Sample script lines.** The Scripts tab gains a table of lines. **It is a
sample, not a measurement**: every fifth of a millisecond of script time the
engine notes which line is running, and the table is each line's share of
those notes. A line with a third of the samples is where a third of the
script time goes, near enough; a line that ran once for a microsecond may
never be noted at all. The table says how many samples it is made of -- a
few dozen is a hint, a few thousand is an answer.

A profiler that is shut costs nothing. So does one that is a tab behind
another: it measures only while you can see it.

## A profile from another machine

The hitch is on a player's machine, or a phone, or a server with no screen.
Run the game there with:

```text
engine-host <project> --profile-capture=run.profile.json
engine-host <project> --profile-capture=run.profile.json --profile-lines
```

It keeps the last minute of frames and writes them to the file as the run
ends. It needs no editor and no graphics device (`--rhi=null` works). Bring
the file back, type its path into the field on the profiler's bar and press
**Open**: it is read exactly as a run is -- the strip, the worst frame, every
tab. **Back to the run** returns to the game in front of you.

**Save** writes what the panel is showing to the path in the field: hold the
run on the hitch you just watched, save it, and send it to somebody.

The GPU's passes are in a capture only if the run timed them
(`--gpu-pass-times`, see (manual:rendering/quality)).

## What it does not do

It shows a frame's totals, not the order things ran in within the frame; it
reads a game on another machine from a file and not live; and the Memory tab
has the device's totals and not a list of textures.
