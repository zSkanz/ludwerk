# tests/perf — the scenes the perf table is measured on

Scripted projects that exist to be **measured by hand**, not by CTest.

A frame time is a property of the machine. A gate built on one goes red the day
CI allocates a slower runner, and a gate that goes red for a reason nobody
controls is a gate people learn to ignore — the same argument
`tests/screenshots/README.md` makes about pixel goldens, and the same conclusion:
the blocking gates are the ones that need no GPU, and the numbers live in
`docs/perf-baselines.md` where a human puts them.

`tests/bench/` is a different thing and stays where it is: those are SIMULATION
benchmarks, driven by `engine-host --bench`, and `perf_budget` does run them in
CTest because a tick has a budget that holds on any machine.

## Running one

```
engine-host tests/perf/horde --headless --frames=300 --exit --frame-stats \
  --width=1920 --height=1080
```

The last line is the measurement: median and worst frame time over the frames
after the warm-up, then **the count of draw calls beside the count of visible
objects**. Those two numbers were the same until M7.5; the roadmap's gate for
the instanced path is that they are not.

The reduced-CPU row `docs/perf-baselines.md` asks for is the same command with
the process pinned to a subset of cores:

```powershell
$p = Start-Process -FilePath engine-host.exe -ArgumentList @(...) -PassThru -NoNewWindow
$p.ProcessorAffinity = 3   # two cores
$p.WaitForExit()
```

Measure on a quiet machine. `churn10k` once read 3.50 ms/tick while a Docker
build ran in the background and 2.02 ms three runs in a row once it finished — a
73% error, larger than any regression worth recording.

## The scenes

| Scene | What it is for |
|---|---|
| `horde` | Two thousand enemies sharing one mesh and one material, chasing a circling player, written from Luau every tick, under a shadow-casting sun. It prices **instanced draws**, which is M7.5's one scope item that is not about looking. Asked for on 2026-08-20 as "could I build a survivors-like on this engine"; answered at M2 by building one outside the repository, and committed here because M7.5 is where the answer became a number something defends. |
| `blocksprint` | A block world built round a viewer who never stops running: hills, shore and a sea of fluid from a formula, one column of sixteen by sixteen blocks built every four ticks and one let go behind. It prices **a block world's chunks arriving** -- the frame that draws while they do (the voxel-world ledger's B0, D449). Its numbers are in `docs/perf-baselines.md`, "A block world being built". |
| `farflight` | A flight corner to corner over imported ground and back, 200 m/s an axis and 150 m up, the ground streamed at 512 m and seen to three kilometres: it prices **the far ground** (ADR 0150) -- every node past the load radius is built from the files the import made -- and the streaming under a camera that never stops. The project has no ground of its own: lay it first, `engine-host <copy> --import-terrain=hills --import-size=16383 --import-high=120 --import-scale=600`, and set `Half` in its script to half the size. `terrain_far_flight` does that with four kilometres on every run of the gate and asserts what no machine changes -- ground in and out, the far ground read from its files, memory the same on the second lap; the sixteen-kilometre numbers are in `docs/perf-baselines.md`, "A world that size, flown". |
| `terrainflight` | The terrain gallery's shapes flown over from 1 200 m down to 60 m and back once in 1 500 frames, the flight the terrain audit of 2026-09-29 measured the owner's place with: it prices **rebuilding the levels of detail** under a moving camera. Its numbers, and the owner's place's, are in `docs/perf-baselines.md` under the terrain flight. |
