# 0153 — A sprite is drawn in the colours it was painted in

- Status: accepted (built, 2026-10-02)
- Date: 2026-10-02
- Decided by: ludwerk-08 for the owner, from a 2D game made on the packaged
  engine (finding L2, defect D464): the game's palette did not reach the
  screen. The mechanism below is the builder's, approved by him before it was
  built.
- Amends: [0038](0038-visual-fidelity-is-a-v1-target.md) (what the resolve
  does to a pixel), the 2D layer's sprite pass (phase 3).

## Context

A sprite was drawn where everything else is: into the HDR target, before the
resolve. It was unlit -- the picture times its colour -- but what the resolve
does to light it then did to the picture: the frame's automatic exposure, the
tone curve, any colour correction, bloom added on top, a half step of dither.
`Color3.fromRGB(200, 80, 40)` under the default `Lighting` arrived as
181, 64, 7; a white tile was not white; a palette chosen colour by colour was a
different palette at noon and at dusk.

That is right for light and wrong for art. A sprite's pixel is not an amount of
light in a scene: it is a colour somebody chose for a screen.

Two ways were open. Drawing the sprites after the resolve, over the finished
picture, is what an engine with a separate 2D canvas does -- and it takes the
sprite out of the world: a particle or a pane of glass in front of it can no
longer be drawn over it, because they were resolved before it existed. Here the
plane is in the world (phase 3), and a sprite keeps its place in it.

## Decision

1. **`Part2D.ExactColor` and `Tilemap2D.ExactColor`, true by default.** On, the
   colours on the screen are the colours in the picture, times `Color`,
   whatever `Lighting` holds. Off, the sprite is one more lit thing: exposed,
   tone-mapped, graded and glowing like a part beside it.
2. **A mask, written by the sprite pass.** In a frame that has an exact sprite,
   the sprites are drawn in a pass of their own with two targets: the scene's
   colour, as before, and one channel that says how much of each pixel is an
   exact sprite -- a fraction, because an anti-aliased edge and a transparent
   texel cover part of one. Every sprite of such a frame goes through it, exact
   or lit, so they keep their order among themselves; a lit one writes zero
   over what is behind it. The forward pass is closed before and reopened
   after, as it is around the decals and the air, so what blends in front of a
   sprite is still drawn over it.
3. **The resolve passes those pixels through.** Where the mask is one, what
   reaches the screen is the scene colour encoded to sRGB and nothing else: no
   exposure, no curve, no grade, no bloom, no dither. Where it is a fraction,
   the two answers are mixed by it.
4. **An exact sprite starts no glow**: the bloom's first level multiplies every
   tap by what is not such a sprite.
5. **A target of its own, because the scene's alpha is taken.** The scene
   colour's alpha is its coverage, which a view with nothing behind it hands to
   the interface (ADR 0107). So the mask is one `R8` target a view, made the
   first frame that view has an exact sprite.
6. **Nothing is built for a world without one.** The mask, the sprite pipeline
   with two targets, the masked bloom level and the four resolves that read the
   mask (plain or graded, into a texture or the window) are each made the first
   frame they are needed, as twins of the shaders they extend -- the way the
   graded resolve already is. A frame without an exact sprite draws through
   exactly the command stream it always had: no capture golden moves.
7. **It replicates** (protocol 32): a sprite the server made lit is lit on
   every screen.

What is NOT changed: the automatic exposure still measures the whole picture,
sprites included -- a bright level is a bright thing in view, and the 3D world
beside it is exposed accordingly.

## Consequences

- A 2D game's palette is its palette. `sprite_color_gate` asks it of a
  screenshot square by square, to within one step; today every square is exact
  to the byte.
- What blends over an exact sprite -- glass, a particle -- is mixed with it as
  colours for a screen and passed through with it. Over a lit surface the same
  particle is exposed and tone-mapped. In a 2D game everything is a sprite and
  the two never meet; in a mixed scene a spark crossing from a wall onto a
  sprite changes brightness at the edge.
- The effects that work on the scene before the resolve -- blur, depth of
  field, sun rays -- move a sprite's colour while its mask stays where it was.
  A blurred menu background is still blurred; its edges are less so.
- At a reduced render scale the mask is upscaled as the scene is, so a sprite's
  edge is mixed over a pixel or two.
- One more full-resolution `R8` target a view that has sprites, and a second
  target in the sprite pass. No extra full-screen pass.
- The 2D examples and the sprite golden changed colour, towards what their
  scripts say.

## Not decided here

- A display-referred interface for `BillboardGui` and the world UI, which are
  drawn through their own pass and are tone-mapped today.
- Sprites lit by 2D lights.
