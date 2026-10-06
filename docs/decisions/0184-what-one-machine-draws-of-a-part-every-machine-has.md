# 0184 — What one machine draws of a part every machine has: `BasePart.Fade`

- Status: accepted
- Date: 2026-10-06
- Decided by: the agent, on a request from the game being made with the
  engine (through the coordinator), under the standing rule to decide as
  professional engines do and record it
- Builds on: ADR 0090 (what a part looks like), the replication of a part's
  look (the wire's `BasePart` fields)

## Context

A game thins the trees that stand between a camera and its player's hero, so
the hero shows through. It did so by easing each tree's `Transparency` from a
client's script, and on a machine that had joined a match that is that
machine's business alone. On a listen host it is not: the host is the
authority, what it writes of a part is sent, and every friend's trees went
thin with the host's -- 105 of them, measured. The game switched the effect
off for a host with friends.

What was wanted is not a property of the tree. It is a fact about one
screen.

## How mature engines do it

Both of the engines this one follows give a renderer's object state that is
local to the machine drawing it, apart from what is replicated: a per-view
visibility or fade on the primitive (Unreal's per-view custom data and its
camera-fade materials; Unity's renderer state, which a networking layer never
touches). What is sent is the simulation; how one screen chooses to show it
is not.

## Decision

1. **`BasePart.Fade`**, a number from 0 to 1, 0 by default: how much of the
   part this machine leaves out of its own picture. What is drawn has the
   opacity `Transparency` and the material give it, multiplied by one less
   this.

2. **It is this machine's alone.** It is not on the wire -- an authority's
   is not sent, and a replica's is not overwritten by what arrives -- it is
   not saved in a scene (`Transient`), and it is not in the world's hash.

3. **A part thinned this way still casts the shadow its `Transparency` gives
   it** (`DrawItem::fadedOnly`, `castsShadow`). What is hidden from a camera
   has not left the world: a tree thinned so a hero shows through still
   shades the ground, and its shadow does not come and go as the camera
   turns. Thinned to nothing, the part is in the frame's list for its shadow
   and in none of the camera's passes.

4. A part that is see-through of its own is see-through: thinning it further
   does not give it a shadow.

## What it does not do

- It is one number a part, for the machine: a machine with two views -- a
  split screen, a camera drawn into a texture -- thins the part in both.
- It thins by blending, as `Transparency` does: a thinned part is drawn in
  the blended pass, one draw of its own, out of the instanced run it was in.
  A screen-door (dithered) fade that stays in the opaque pass would keep the
  run and the depth; it is another way of drawing, not decided here.
- Nothing else of a part is given a local twin by this.

## Consequences

- The game eases `Fade` where it eased `Transparency`, and the effect is on
  for a host with friends.
- The wire is what it was: protocol 41.
