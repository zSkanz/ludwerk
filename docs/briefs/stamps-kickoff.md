# Stamps 2.0: the kickoff and the ledger

Opened on 2026-10-03 by the owner's word, through ludwerk-08: stamps at the
level of the other engines' prefabs and scenes, built in one batch and
validated once at the end. The decision is ADR 0155, which amends 0049 and
0051; the FPS game's G1 (a copy that stores a world transform on every part)
heads it.

**The owner's ruling:** every row below in one batch, the inner loop only, one
full gate, one push, one package and one report. A row that cannot be made
solid ships without it, and the report says which.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every row

- A failing test first, as everywhere: a format change has a round trip that
  did not hold before it; an editor command has a test that drives it.
- A scene written by the build before this one opens, and saves as version 3
  without losing anything it held.
- Nothing about where a part came from enters the world hash: a sid, a pivot
  and a constructed mark are what a person wrote down, as the stamp mark is.

## Engine and format

- [x] **S1** (G1) A copy writes its pivot, and a part's `CFrame` is an
  override only where it differs from the stamp relative to it. A v2 copy
  converts as it loads. Tests: a part moved in the stamp file moves in a copy
  placed away from the origin; a v2 scene with per-part world overrides loads
  where it was and saves with none.
  Tests: `stamp_tests.cpp` S1 (G1), S1 (a v2 copy converts); the anchor design is in ADR 0155 §1.
- [x] **S2** Every node of a stamp has a sid; overrides key on it; a v2 path
  key migrates. Tests: renaming a part in the stamp keeps a copy's override of
  it; a v2 path-keyed override is applied and written back by sid.
  Tests: `stamp_tests.cpp` S2 (rename), S2 (v2 key).
- [x] **S3** A stamp holds placed stamps, each still linked; a cycle is
  refused with the chain named; depth eight. Tests: a change to the inner stamp
  reaches a copy of the outer; a stamp that reaches itself is refused.
  Tests: `stamp_tests.cpp` S3 (nested), S3 (cycle); `editor_tests.cpp` S3 (a stamp of a copy).
- [x] **S4** A variant is a stamp whose root is a copy of its base, and chains.
  Tests: a change to the base reaches the variant except where it overrides; a
  variant of a variant reads and writes.
  Test: `stamp_tests.cpp` S4 (base, variant, variant of a variant).
- [x] **S5** A copy adds children and disables the stamp's without unlinking;
  a moved or replaced child is a disabled one and an added one. Tests: an added
  child survives a save and a change to the stamp; a disabled child stays
  unbuilt and comes back when enabled.
  Tests: `stamp_tests.cpp` S5 (disabled, moved); `editor_tests.cpp` S5 (added keeps the link, enable).
- [x] **S6** A stamp declares parameters; on a copy each is an attribute of
  the root, and writing it applies what it drives at once, in play too.
  Tests: a copy's parameter drives two parts' properties; a write in play
  applies at once.
  Tests: `stamp_parameters.spec.luau`; `editor_tests.cpp` S6 (declared on the stage, driven).
- [x] **S7** `Instance.stamp(name, linked, parameters)`, validated against the
  declaration and refused with a catalog message, never clamped;
  `Instance:GetStamp()`. Tests: a parameter set at placement; a value out of
  range refused.
  Spec: `stamp_parameters.spec.luau` (set, refused four ways, `GetStamp`).
- [x] **S8** A `Construct` module builds parts from the parameters: in the
  editor on a change, at load and at placement, not again in play; what it
  builds is never saved. Tests: a fence builds its posts from its length; the
  same parameters build the same world hash.
  Spec: `stamp_construct.spec.luau`; `stamp_tests.cpp` S8 (never saved).
- [ ] **S9** -- **deferred from this batch** (ADR 0155 §9 says why and what
  it needs). A replicated copy is sent as its stamp (`StampSpawn`, protocol
  36), and as a subtree to a replica without the file (`NeedFull`), which then
  does not construct again; a copy that streams out and back in comes back the
  same way. Tests: the replica's world hash equals the subtree path's, after a
  stream out and in too; `NeedFull` does not build twice. Measured: the bytes a
  copy costs both ways.
- [x] **S10** `ContentProvider:PreloadAsync` takes stamp names, and
  `@engine/stamppool` takes and returns copies. Tests: a preloaded stamp is not
  read again; a returned copy comes back as the stamp has it.
  Spec: `stamp_pool.spec.luau`.
## Editor

- [x] **S11** An overridden property is marked; revert per property, per part
  and per copy; **Apply** to each level, the variant or its base. Tests: apply
  to the base from a copy of a variant changes the base file.
  Tests: `editor_tests.cpp` S11 (revert a copy), S11/S13 (apply to the base from a variant's copy).
- [x] **S12** A stamp is edited alone or in place, and a save restamps every
  open copy. Test: an edit in place saves the stamp and refreshes another copy.
  Test: `editor_tests.cpp` S12 (edited where it stands, applied whole).
- [x] **S13** Create a variant; open the base; go to a copy's stamp; select
  every copy; replace a copy with another stamp keeping its pivot and the
  overrides its sids still name; unlink. Tests: one per command.
  Tests: `editor_tests.cpp` S13 (select every copy, replace), S11/S13 (variant from a copy).
- [x] **S14** Overrides whose sid is gone are listed on the copy and can be
  removed. Test: a part deleted from the stamp leaves an override that is
  listed and cleaned.
  Test: `editor_tests.cpp` S14.
- [x] **S15** A stamp has a thumbnail in Content; a stamp created from a
  selection keeps the references inside it; disabled children show greyed and
  can be enabled from the menu. Tests: the thumbnail exists; a reference inside
  survives creation.
  Tests: `editor_tests.cpp` S15 (references kept), S5/S15 (disabled listed and enabled).
## Documentation

- [x] `docs/manual/world/prefabs.md`: the whole model, with a worked example --
  a `Fighter` stamp and its class variants.

## Findings

- **A pivot stored on the instance was the wrong shape.** It had to follow
  every way a copy moves -- `PivotTo`, the gizmo, a script writing each part --
  or go stale. Measured from an ANCHOR instead (the root when it is a part,
  else the first part of the stamp the copy still holds), nothing is stored,
  the file writes the anchor's place exactly so the bytes settle, and a v2
  `Model` copy converts with no code of its own.
- **`Position` and `Orientation` are saved properties of a part**, and they
  are its `CFrame` again: written in a stamp's frame they would undo the
  `CFrame` on read. A copy never overrides them, and a node written in a
  stamp's frame leaves them to the `CFrame`.
- **The replica's states are in the authority's terms** -- its atoms and its
  network ids -- because the checksum is taken over them. That is what keeps
  S9 from being the simple message it looked like; ADR 0155 §9 has the shape
  that works.
- **Edit in place, the dimmed-world version**, would draw two worlds in one
  viewport. The professional equivalent shipped: edit the copy where it stands
  and apply the whole copy to its stamp.
