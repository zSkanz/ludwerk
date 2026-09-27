# The owner's queue, 2026-09-27

The order of work the owner set on 2026-09-27, while the Views ledger
([`views-kickoff.md`](views-kickoff.md)) had just been started. **Bugs come
first, always**; then the items below, in this order. Views (ADR 0107) and the
AI panel (ADR 0108) resume after them.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## Q0 — defects, before anything else

The owner reported an error while testing the export and asked for every export
feature to be tried before anything new. What that sweep found:

- [x] **The Android export failed on every machine**: the rename left the
      Gradle signing configuration named `ludwerk` and its two references
      `signingConfigs.engine` (`platforms/android/player/app/build.gradle`).
- [x] **`ludwerk keystore new --alias upload --out release.keystore`** -- the
      form its own usage text shows -- made a keystore named `true` with a key
      named `true`: the CLI read only `--flag=value`. Options that carry a value
      now take either form (`tools/cli/main.luau`, `ValueFlags`).
- [x] **A dedicated server opened a graphics device and drew every frame** for
      nobody. On a Linux host with no GPU that device is Mesa's software
      rasteriser, and the exported server crashed in it (`SIGSEGV` in
      `libvulkan_lvp.so`) seconds after starting. A dedicated server now takes
      the no-op backend unless `--rhi=` names one.
- [x] **The message catalog still spoke the old names**: `luaug-host`,
      `.luaug/types/`, `luaug.toml`, `LUAUG_TEXTURE`/`LUAUG_PARAM`,
      `LUAUG_ENABLE_REPLICATION`, `luaug build-assets`, a surface parameter
      that "must not start with Luaug" (the rule is `Engine`). The brand lint
      did not look at `i18n/`, `icons/` or `.vscode/`; it does now. The usage
      text also lists the network postures, which it never did.
- [x] **Every exported desktop game carried the editor's files**: the
      new-project template (a starter project's scripts included) and the
      editor's icon theme. The phone's export already left both out.
- [ ] The owner's own error, which is not yet known -- asked for on
      2026-09-27.
- [ ] Two games started from the same folder share one `engine.log`, and the
      second says it cannot open it. A match tested from one folder always
      does this.

## Q1 — `UIGradient` and `UIStroke`, with the new features

Both classes as the other platform ships them today, including the features it
released in 2025-26 (researched from its public documentation and announcements
on 2026-09-27; concepts only, R7). A new ADR records the decision.

**`UIGradient`** -- a child of a `GuiObject` (frame, text, image and button
classes) that colours and fades its parent: `Color` (a colour sequence),
`Transparency` (a number sequence), `Offset` (a `Vector2`, fractions of the
parent's size), `Rotation` (degrees, clockwise), `Enabled`, and the new three:

- `Type`: `Linear` (the classic), `Radial` (out from the centre plus
  `Offset`, radius `(width + height) / 4`, `Rotation` ignored), `Conical`
  (sweeping clockwise round the centre, `Rotation` is the start angle).
- `TileMode`: `Clamp` (the end colours fill the rest), `Repeat` (wraps, a seam
  where the ends differ), `Mirror` (ping-pongs, no seam).
- `Scale`: how much of the sequence spans the element -- the distance between
  stops for linear, the radius for radial, the angular sweep for conical
  (clamped to one turn). Below 1 the rest is filled by `TileMode`.

A `UIGradient` under a `UIStroke` colours the stroke instead.

**`UIStroke`** -- an outline on its parent's text or border: `Color`,
`Thickness`, `Transparency`, `Enabled`, `LineJoinMode` (`Round`, `Bevel`,
`Miter`; a `UICorner` makes it `Round`), `ApplyStrokeMode` (`Contextual` --
the text on a text object, the border otherwise -- or `Border`), and the new
ones:

- `StrokeSizingMode`: `FixedSize` (pixels) or `ScaledSize` (a fraction of the
  parent's shorter side, or of the font size on text).
- `BorderStrokePosition`: `Outer` (the default), `Center`, `Inner`.
- `BorderOffset` (`UDim`): moves a border stroke out (or in), scale relative
  to the parent's shorter side.
- `ZIndex`: the order among sibling strokes; **several border strokes on one
  element** are allowed (one contextual stroke).
- Rich text's `<stroke color thickness transparency joins sizing>` tag.
- Strokes do not affect layout.

- [ ] ADR, IDL (classes and five enums appended), accessors, `.d.luau`,
      reference pages, editor icons.
- [ ] The 2D and world UI shaders: gradient and stroke on the one pipeline.
- [ ] Text strokes: stroked glyphs in the atlas.
- [ ] Rich text `<stroke>`.
- [ ] An example, screenshots, tests, docs.

## Q2 — the editor, remade after VS Code

The owner, 2026-09-27: every frame of the editor looked at by a critical user
who knows VS Code well, and remade to be as close to it as possible in how it
works and how it looks -- responsive, practical for development, the smallest
details included. After Q1, and before anything else; defects still come first.

- [x] Home no longer carries the match controls (player count, dedicated
      server); they live on the Test tab alone. Play on Home still plays what
      Test says.
- [ ] The survey: every panel, every state, screenshot by screenshot, with
      what VS Code does in its place.
- [ ] The remake, item by item, from that survey.

## Q3 — then

Views ([`views-kickoff.md`](views-kickoff.md), ADR 0107), whose V0 was begun
and set aside with its first change saved outside the tree; then the AI panel
([`ai-kickoff.md`](ai-kickoff.md), ADR 0108).
