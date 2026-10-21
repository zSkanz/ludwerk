# 0258 — A mesh a script names is measured by the machine that owns the part

- Status: accepted
- Date: 2026-10-20
- Builds on: ADR 0010 (compiled content), ADR 0186 (what a client makes is
  its own)

## The question

`MeshPart.MeshSize` is what the mesh measures, and the part is drawn and
collides at `Size / MeshSize` times it. It was set in one place: the editor,
when a mesh was imported or dropped, from the bounds of the mesh it had
loaded to draw. A `MeshPart` a game made by script kept (1, 1, 1) whatever
it named, and whether or not `ContentProvider:PreloadAsync` had loaded the
mesh first. A game sizing its parts as `MeshSize * factor` sized them
against one metre, and one placing a mesh in a box of so many metres
(`Size = metres`) drew the mesh that many times its own size.

It could not simply be derived. The world hash is a function of what was
done to the world, and a divisor read from the GPU's copy of a mesh would
differ between a headless run, a rendered run, and a run where the load
landed a frame later.

## Decision

1. **The bounds come from the content, not from a load.** A compiled mesh's
   header holds its bounds at a fixed place (`asset::MeshHeaderBytes`), so
   `ContentMounts::meshBounds` reads them where the mesh is: in a pack, from
   the mapped bytes; in the editor's object store, from the blob's first
   forty bytes, remembered. Nothing is decoded, nothing is uploaded, and
   every machine with the same content reads the same six numbers, headless
   included. A project's sources are compiled into its store when it opens,
   before any script runs, so the answer does not depend on whether, or
   when, anything preloaded the mesh.
2. **Setting `MeshContent` from a script measures the part** at once
   (`World::measureMesh`): `MeshSize` becomes what the new mesh measures, and
   `Size` the same while it is still (1, 1, 1). Both are set as properties,
   so they are changes a script sees, they replicate, and they are in the
   hash. A flat mesh measures a hundredth of a metre on its flat side, as
   the editor's import does.
3. **Only the machine that owns the part measures it.** A server measures
   what it makes; a client measures what it makes itself. A replica's copy
   of the server's part is never measured locally -- it takes `MeshSize` and
   `Size` as the server sends them -- or a client could give a part a size
   the server never had, and the two would collide differently.
4. A name that is not a compiled mesh of the content -- a typo, a loose file
   -- leaves both properties as they were, and the machine that tried says
   so once per name (`scene.warn.mesh_unmeasured`). An engine mesh
   (`engine://mesh/...`) is made rather than imported and is not measured.
5. Loading a scene, cloning and replication set properties as they were
   saved or sent, and measure nothing. The editor's import keeps its own
   path, which reads the same bounds.

## Consequences

- A script-made `MeshPart` now draws and **collides** at its mesh's size
  rather than as a one-metre box, unless the script sets `Size`. Code that
  sets `Size = MeshSize * k` draws as it did. Code that set `Size` to a
  number meaning "times the mesh" while `MeshSize` was one now draws the
  mesh fitted to that many metres instead.
- `netcode_measured` runs a real server and a real joiner that loads no
  mesh: the server's parts have the same `MeshSize`, `Size` and collider on
  both, a set `Size` is kept, a missing mesh is said once, the joiner's own
  part is measured there, and its copy of a server part is not.
