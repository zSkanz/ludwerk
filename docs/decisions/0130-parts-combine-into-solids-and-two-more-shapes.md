# 0130 — Parts combine into solids, and two more shapes

- Status: accepted (to be built; see `docs/briefs/toolkit-kickoff.md`, F5).
  **The dependency Manifold awaits the owner's approval (R5)**; nothing is
  vendored before it.
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, approving solid modelling (union,
  subtraction, intersection of parts) and more part shapes from a survey of
  public documentation (R7).
- Relates to: [0010](0010-asset-stack.md) (the asset stack), [0121](0121-a-script-can-write-an-image-a-sound-and-a-mesh.md)
  (`EditableMesh`, whose collision modes this reuses).

## Context

A wall with a doorway is three parts today, and a window in it five. Building
from parts — the way a person coming from the other platform builds — wants
solid modelling: join parts into one, cut one out of another, keep what two
share. `Enum.PartShape` is `Block`, `Ball`, `Cylinder`, `Capsule` and `Wedge`.

Mesh booleans that do not break on real input (coplanar faces, touching edges,
thin slivers) are a research-grade problem. **Manifold** (Apache-2.0, used by
OpenSCAD and Blender's geometry nodes) is the library that solves it and
guarantees a closed, manifold result.

## Decision

### 1. Operations

- `BasePart:UnionAsync(parts)`, `BasePart:SubtractAsync(parts)`,
  `BasePart:IntersectAsync(parts)` return a new part; the editor gains
  **Union**, **Negate** and **Separate** on the selection.
- The result is a **`UnionOperation`** (a part with a mesh). **`NegateOperation`**
  marks a part as a cutter in the editor before a union.
- A union **keeps its sources** (hidden, in the instance), so **Separate**
  restores them and an edit re-runs the operation.
- Colours and materials: each face keeps the material of the part it came from
  (`UsePartColor` makes it one).
- Computed off the main thread; `...Async` yields.

### 2. Collision

- `CollisionFidelity`: `Box`, `Hull`, `Default`. An anchored union collides as
  its triangle mesh; an unanchored one as its convex hull. A decomposition into
  several hulls is later (it would be one more library).

### 3. Two more shapes

- `Enum.PartShape` gains `CornerWedge` and `Truss` (a lattice beam, drawn and
  collided as its box). Values appended at the end of the enum.

### 4. The dependency

- **Manifold**, vendored at its latest release tag when the owner approves,
  compiled without its optional parallel backend (the engine's job system
  schedules the calls).

## Consequences

- Walls with doors and windows, arches, holes, and custom shapes from parts,
  in the editor and from a script.

## Not decided here

- Convex decomposition for unanchored unions.
