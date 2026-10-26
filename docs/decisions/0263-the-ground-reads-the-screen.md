# 0263 — The ground reads the screen

- Status: accepted
- Date: 2026-10-26
- Builds on: ADR 0113 (a terrain's layers are materials, kept in arrays),
  ADR 0175 (the lean ground, and the slots it kept named), ADR 0209
  (reflections and bounce from the screen)

## The question

ADR 0209 gave every part and mesh what the screen reflects and the light it
bounces, and left the ground out: a `Terrain` was lit by probes and volumes
alone. The reason was a count. A shader stage has sixteen sampled textures,
and the ground's were all spent -- and the two screen pictures are two more.

Counted again, four of the sixteen were a mesh's material maps -- base
colour, normal, metallic-roughness, emissive -- which the ground never read.
They were bound to neutral stand-ins and kept named in a branch no frame
takes (ADR 0175), because SDL_GPU binds a stage's textures as one run from
zero, and a slot a shader does not name is one its compiler removes. The
ground's own three -- its layers' colour, normal and surface, each already an
array of one slice a layer (ADR 0113) -- sat after the light's nine.

## Decision

1. **The layers' arrays take the material maps' slots.** Colour, normal and
   surface are the first three textures; the fourth stays named, bound black,
   in the branch no frame takes. The shared forward header leaves a mesh's
   maps out for the ground (`ENG_NO_MATERIAL_MAPS`), as it does for a
   surface shader's wrapper.
2. **The two screen pictures take the arrays' old slots**, where every mesh
   reads them: the ground reflects what the screen shows and takes the light
   it bounces, by the same code as a part. Fifteen textures, where there were
   sixteen.
3. **The layers' storage buffer moves with them.** A stage's storage buffers
   come after its textures in SDL_GPU's one run: with fifteen textures it is
   the sixteenth. A first cut that left it where it was drew the ground from
   whatever the sixteenth binding held -- holes, and a black world.
4. **Off is as it was.** With both screen settings off -- every level below
   the one that turns them on, and every phone -- the screen's pictures are
   bound as nothing and the ground is drawn pixel for pixel as before.
   Blocks and grass stay lean on purpose and read no screen.

## Proof

- The screen light gate gains a scene: the glowing red wall of its `wall`
  scene standing on a terrain of sand. With the bounce on, the ground at the
  wall's foot is 19 levels redder than with it off, and the ground away from
  the wall is the same; on Direct3D 12 and on Vulkan.
- Every terrain picture gate -- the gallery, the channels, the rules, paint's
  edge, the glow, the tiling, the shadow, the far ground -- passes as it was.
- `examples/13-terrain` at its default quality: the same picture, pixel for
  pixel, before and after.
- The binding gate holds the full ground's draw to fifteen textures.
