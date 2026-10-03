# 0155 — Stamps 2.0: a copy has one pivot, a stamp nests and has variants, and a designer sets its parameters

- Status: accepted -- approved by the owner on 2026-10-03
- Date: 2026-10-03
- Amends: 0049 (a stamp is a source), 0051 (inheritance and overrides)
- Ledger: `docs/briefs/stamps-kickoff.md`

## Context

ADR 0051 made a placed stamp an inherited definition: its mark, plus the
properties its copy changed, keyed by a path of names inside the stamp. A game
built on it -- the FPS game's fighters and props -- found where that stops short
of what an engine's prefabs are expected to do:

- **Every part of a placed copy stores its world `CFrame` as an override**
  (G1). A part moved inside the stamp file never moves in a copy placed away
  from the origin, because the copy says where that part is.
- **A copy cannot hold anything the stamp does not.** A child added to a copy
  is a structural change, and the save writes the copy in full and drops the
  link.
- **A stamp cannot hold a stamp** (ADR 0049), and there are no variants
  (0051 left them unanswered): a "Fighter" with a Scout, a Heavy and a Medic is
  three unrelated files.
- **An override is keyed by names.** Renaming a part in the stamp orphans every
  copy's overrides of it.
- **A designer cannot be offered the handful of numbers that matter**: a
  fence's length, a door's colour, stairs' steps. A copy shows every property
  of every part, and nothing builds parts from a number.
- **A replicated copy is sent part by part**, although every machine has the
  stamp in its package.

The owner asked for stamps at the level of the other engines' prefabs and
scenes, in one batch. This decides the whole of it; the ledger tracks the
building.

## Decision

### 1. A copy has one pivot (G1)

**A copy stands on an anchor**: its root when the root is a part or a camera,
and otherwise the first part of its stamp it still holds -- a `Model` or a
`Folder` has no place of its own. The pivot is the transform that takes the
anchor from where the stamp puts it to where it stands, and the file writes
the anchor's place exactly as `"pivot"` (and the anchor's key as `"anchor"`
when it is not the root), so a copy read and written again is the same bytes.
A part's `CFrame` inside the copy is an override only where `pivot⁻¹ ·
part.CFrame` differs from its place in the stamp by more than a tenth of a
millimetre; a part's `Position` and `Orientation` are its `CFrame` again and
never an override of their own. Moving the copy moves everything with it;
moving a part inside the stamp file moves it in every copy.

Nothing is stored on the instance for this, so `PivotTo`, the gizmo and a
script move a copy as they move anything; the next save finds the move.

The scene format is version 3. Version 2 still reads: its per-part `CFrame`
overrides are places in the world, and the next save finds the move they share
from the anchor -- a v2 copy whose parts all moved together saves with one
pivot and no overrides; a part that does not fit the move stays an override.

### 2. Every node of a stamp has an identity

Each instance in a stamp file carries `"sid"`, eight hex digits assigned when
the stamp is first written and never changed. It is kept on the instance
record of every built node. **Overrides are keyed by sid**, so renaming or
reordering a part in the stamp keeps every copy's overrides. A node inside a
nested copy is keyed by the path of sids down to it, `a1b2c3d4/0f0e0d0c`; the
copy's root is `""`. A v2 override keyed by a name path resolves once when it
is read and is written back by sid.

### 3. A stamp may hold placed stamps

A node inside a stamp file may itself be a placed copy (`"stamp"`, `"pivot"`,
`"overrides"`), still linked to its own file. Expansion carries the chain of
stamps being built: a stamp that reaches itself is refused with a message that
names the chain, never a stack overflow. The depth limit is eight; the
instance limit stands.

### 4. A variant is a stamp whose root is a copy of its base

A variant file's root is a linked copy of another stamp, with the copy's
overrides, added children and disabled children. Variants chain: a variant of a
variant is a copy of a copy. A change to the base reaches every variant, except
where a variant overrides it.

### 5. A copy may add children and disable the stamp's

`"added"` lists children a copy (or a variant) holds that its stamp does not,
each under the sid of its parent; the link stays. `"disabled"` lists sids of
the stamp's children that this copy does not build. A stamp's part cannot be
deleted from a copy -- deleting it disables it -- so a change to the stamp
still reaches the rest, and enabling it again brings it back as the stamp has
it. The copy's stamp menu in the Explorer lists its disabled children, greyed,
each with **Enable**, and a stamp's child offers **Disable**.

**Nothing inside a copy unlinks it any more.** A stamp's child moved under
another parent, or replaced by one of another class, is that child disabled
and a new one added; only a copy whose root is no longer the stamp's class is
written in full.

### 6. A stamp declares its parameters

A stamp's root may declare `"parameters"`: a name, a type, a default, an
optional range or list of choices, and the properties it **drives** (by sid and
property name). On a copy, each parameter is an **attribute of the copy's
root**, so it replicates and scripts read it as any attribute. Writing it --
in the editor or in play -- applies the value to every property it drives at
once. Properties shows a copy's parameters before anything else, and a copy's
value is that copy's own.

The declaration is authored where the stamp is edited: the stamp's root lists
its parameters in Properties, and a property of any part offers **Drive from**
one of them.

### 7. A script places a stamp with its parameters

`Instance.stamp(name, linked?, parameters?)` sets the parameters before
anything else runs on the copy. A value of the wrong type, out of range or not
among the choices is refused with a message from the catalog -- never clamped.
`Instance:GetStamp()` is the name of the stamp a copy was placed from, or
`nil`.

### 8. A stamp may construct its own parts

A `ModuleScript` named `Construct` under a stamp's root returns
`function(root, parameters)`, which builds parts from the parameters: a fence's
posts by its length, stairs by their steps. It runs sandboxed, as any script,
with no clock and a random generator seeded from the stamp and the parameters,
so the same parameters build the same parts. It runs **in the editor when a
parameter changes, and when a copy is loaded or placed** -- not again in play,
as a construction script runs at construction only. What it builds is marked
as constructed: never saved, never an override, built again each time. To a
replica, what it built travels as any instance does: a replica never runs a
copy's `Construct`, so nothing is built twice.

### 9. A copy sent as its stamp -- deferred

Approved, and **not built in this batch**: done as first designed it saves the
spawn message and nothing else. A replica keeps every received state in the
AUTHORITY's terms -- its name atoms and network ids -- because that is what
the snapshot's checksum is taken over, so the fields of a copy built from the
replica's own stamp cannot serve as a baseline it can prove, and every
instance would still arrive as a whole record. The saving is in those records.

What it needs, written down so the next batch starts from it: the authority
offers a stamp to a peer once (`StampOffer`: its path and content hash, and
the authority's atom for every name the stamp's fields use); the peer answers
whether its package holds that file (`StampHave`), off the critical path, the
first copy going as an ordinary subtree meanwhile. Once a peer has it, a copy
is sent as `StampSpawn` -- its stamp, its copy node as a scene writes it, and
its instances' network ids in the order a build of that node makes them -- and
its records are diffs against the state a build of the node extracts, which
both ends compute, mapping names through the offered atoms and parents and
references through the id list. Streaming a copy out and back in takes the
same path. Protocol 36.

### 10. A stamp can be preloaded and pooled

`ContentProvider:PreloadAsync` accepts stamp names: the file is read and parsed
once and kept, so the first `Instance.stamp` of it does not hitch.
`require("@engine/stamppool")` keeps copies of one stamp to take and give back:
`.new(name, size?)`, `:Take(parent?)`, `:Return(copy)`; a returned copy is put
back as the stamp has it.

### 11. The editor

- **An overridden property is marked** in Properties, and is reverted per
  property, per part or per copy. **Apply** offers each level the change can go
  to: this copy's variant, or its base.
- **A stamp is edited alone, or where a copy stands**: alone on the stage, as
  before; or a copy is edited in the world and **Apply the whole copy to its
  stamp** writes it back as the stamp -- its overrides, its added and its
  disabled children -- in the stamp's frame, every copy following, its
  variants' too. A copy of a variant applies to the variant.
- **Commands**: create a variant from a stamp or a copy; open the base; go to a
  copy's stamp; select every copy of a stamp; replace a copy with another
  stamp, keeping its pivot and the overrides whose sids the new stamp has; and
  unlink.
- **An override whose sid no longer exists** is listed on the copy, with a
  command that removes them.
- **A stamp has a thumbnail in Content**, and creating a stamp from a
  selection keeps the references that point inside it.

## Consequences

- The scene format moves to version 3, and a v2 scene converts as it loads; a
  save writes v3. A build before this one cannot read a v3 scene.
- The wire stays protocol 35 until §9 is built.
- A stamp written by this build gains sids the first time it is saved; a copy
  of a stamp without them keys its overrides by path until the stamp is saved.
- ADR 0049's "a stamp of a stamp is refused" and ADR 0051's "a structural
  change unlinks" no longer hold; ADR 0051's open question on variants is
  answered by §4.
