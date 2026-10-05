# 0176 — The world is drawn behind the loading curtain before it lifts

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, in the mobile performance batch (ludwerk-08, on the
  owner's request that the engine be made fast on a phone), under the
  standing rule to decide as professional engines do and record it
- Builds on: ADR 0159 (the loading curtain), D507, ADR 0171

## Context

A game's world is shown from behind a curtain once it has arrived (ADR 0159):
while the curtain is up the world is not drawn at all, and the frame after it
lifts is the first frame the renderer sees it.

That frame is also where the renderer makes everything it makes at first use:
the pipelines of the terrain and of whatever else is in view, the environment
map, the cluster tables. On a desk that is a few milliseconds. On the owner's
phone, read from `--frame-stats`: frame 213, the first after the curtain,
1317 ms -- 1120 of preparing and 142 of lights -- with the game already
running under it. And the same price is paid again, smaller, the first time a
weapon makes a spark or an enemy shows a name: a pipeline is tens of
milliseconds on a phone's driver, and each family is made the frame it is
first drawn.

## How mature engines do it

- **Unreal**: PSO precaching and bundled PSO caches -- pipelines are compiled
  on the loading screen from a list recorded in play, exactly because "hitch
  on first use" is what a pipeline compiled on demand is.
- **Unity**: shader variant collections with `WarmUp`, called from a loading
  screen; the documentation's advice for mobile is to warm what the scene
  will use before it is shown.
- **Godot**: 4.4's "ubershader" and pipeline pre-compilation at load, for the
  same hitch.

All three: what a frame would compile on first use is made behind the loading
screen.

## Decision

1. **While the curtain is settling, the world is drawn behind it.** The
   curtain lifts after three frames in a row with nothing left to wait for
   (`LoadingCurtain::SettleFrames`); from the first of them
   (`LoadingCurtain::settling`) the frame is rendered as it will be and the
   backdrop is drawn over it. Whatever the first picture of the world costs is
   paid there. While things are still arriving nothing is drawn, as before: a
   load is not slowed by pictures nobody sees.

2. **And the renderer is warmed once** (`IRenderer::warm`), on the first
   settling frame of each raising of the curtain: the pipelines of particles,
   GPU particles, beams and trails, decals, the world's interface, highlights
   and skinned runs are made then, whether or not the scene shows one yet.
   They are what a game first asks for in the middle of play.

3. What the scene itself shows -- terrain, foliage, blocks, sprites, the sky
   -- needs no list: drawing it makes it.

## What it does not do

- A pipeline is still compiled again at every launch. SDL's GPU API passes no
  pipeline cache to the driver (its Vulkan backend says so in a comment);
  keeping one between runs is a patch to carry in `third_party/patches` and
  is its own decision.
- A curtain that gives up (ten seconds; a minute while a script holds it)
  lifts without having settled, and its first frame is what it was.
- Surface shaders a game compiles for a material first seen in play are made
  when they are first seen.

## Amended 2026-10-05: what a report of a slow frame needs to be read

On the phone the slowest frame of a run was still one of a second and a half,
made of the frame's preparation (1.1 s) -- and nothing in the log said whether
the loading screen was still up when it happened, nor which pipeline was being
made. Three things, so the next report answers itself:

- **The loading screen says when it lifts**: the frame, and how long it was up.
- **A pipeline that takes over twenty milliseconds to make is said by name**,
  with the time: making one is where a driver compiles its shaders.
- **The frames behind the loading screen are not frames of play** in
  `--frame-stats`' list of the slowest: the world is drawn there on purpose,
  so that the cost of a first draw falls where nobody watches, and was then
  listed as the run's worst frame.

## Consequences

- The loading screen is up for as long as the world's first frame takes to
  draw, and the first frame of play is an ordinary one.
- A headless run and the editor have no curtain and are unchanged: no capture
  golden moves.
- Protocol unchanged (40).
