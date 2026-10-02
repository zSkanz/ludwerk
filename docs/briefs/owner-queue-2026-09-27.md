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
- [x] **The owner's own error**, found by driving the packaged editor's
      Export window (he asked for it to be found without him): ImGui's
      "Code uses SetCursorPos()/SetCursorScreenPos() to extend window/parent
      boundaries" box over the window, and a red frame round the target
      list. Each target card moved the cursor into itself to draw its icon
      and back, and the last card left it past what the list had drawn. The
      icon is painted now. Interface errors of that kind now also reach
      the engine's log (`engine.overlay.warn.interface_error`), once each,
      so the console and the log file show what the red box said.
- [x] Found on the way, by the same sweep: **Ctrl+Shift+S overwrote the open
      scene** instead of asking for a name (the Ctrl+S handler never read
      Shift); **Escape did not close the Export window**; a client of a
      dedicated server with no address read "joins ?".
- [x] The nightly's sanitizers: `memcpy` from an empty vector's null data
      in the UI gradient upload (every capture gate aborted on it) and in an
      archive test. `-Only asan` passes all 70 locally.
- [x] Two games started from the same folder shared one `engine.log`, and the
      second said it could not open it; a match tested from one folder always
      did this. The second run writes `engine_2.log` now, and never rotates a
      log another run holds (D401, 2026-10-01).

### The owner's list of 2026-09-28

Found by the owner building a multiplayer test and a snake game, outside the
repository. Each was reproduced by running before it was fixed.

- [x] P0-1 **Typing reaches a `TextInput` in an exported game**, and a phone's
      keyboard opens with it (D204).
- [x] P0-2 **`WalkSpeed`, `JumpSpeed`, `MaxSlopeAngle` and `AutoStepHeight`
      replicate**, and a correction replays at the server's speeds (D205,
      protocol 20).
- [x] P1-3 **`.Parent` of a destroyed instance reads nil**; everything else on
      it still raises (D206, the owner's decision).
- [x] P1-4 **A script's `Join` to the same server again is the same player**
      (D207).
- [x] P1-5 **A silent peer is gone in ten seconds**, `[network] timeout` (D208).
- [x] P2-6 A headless client back in solo no longer measures an
      eighteen-trillion millisecond frame (D209).
- [x] P2-7 `Enum.KeyCode` has punctuation, the navigation keys and the keypad,
      appended (D210).
- [x] P2-8 `ludwerk check` and `fmt` skip `dist/` (D211).
- [x] P2-9 The analyser names the engine's definitions `@engine` (D212).
- [x] P2-10 A headless process that has had real time keeps it (D213).
- [x] Found re-exporting the owner's test: `TextInput.FocusLost` delivers
      `submitted`, so Enter submits (D215), and a focused, empty field shows its
      caret rather than its placeholder (D216).
- [x] And: `NetworkService:Disconnect()` never told the server, which held the
      player until its timeout (D217).

### The owner's report of 2026-10-01

- [x] **Anything placed in the world is clickable and movable** (D399). A friend
      of the owner's could not move a `Water`. The sweep of the class registry
      found every class with a writable world `Position`, `CFrame` or `From`
      that is not a `PVInstance`, and what each now does:

      | Class | Clicked by | Moved as |
      |---|---|---|
      | `Water` | its surface: an ocean anywhere, a lake over its box, a river over its ribbon | its surface: `SurfaceLevel` up and down; a lake's `Position`; a river's points together. The scale gizmo sizes a lake and a river's width |
      | `WaterPoint` | a marker on it, clickable through the water | its own `Position` |
      | `Terrain` | its ground | its origin, `Position` |
      | `Decal` | a marker | its `CFrame`, relative to the part it is on |
      | `NavigationLink` | a marker between its ends | both ends together |
      | `Tilemap2D` | its tiles (the 2D view) | its `Position` on the plane |
      | `PointLight`, `SpotLight`, `Attachment`, `Bone`, `Camera`, `Part2D` | already: a marker, a sprite | already |

      Held for every class to come by `editor_tests`' registry sweep: a new
      class with a writable `Position`, `CFrame` or `From` and no gizmo fails
      by name. A screen's UI is not in it -- its position is on the screen. A
      river's points are edited one by one here; drawing and inserting them is
      the water ledger's (ADR 0146).

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

- [x] ADR 0110; IDL (the two classes, four datatypes, six enums appended),
      accessors, `.d.luau`, reference pages, editor icons.
- [x] `ColorSequence` and `NumberSequence` as values: scene files (an object
      keyed by which, so an attribute reads back as what it was), the world
      hash, attribute replication (protocol 18), the Luau types, and a
      sequence editor in the Properties panel.
- [x] The 2D and world UI shaders: gradient and stroke on the one pipeline,
      through `engine_ui.hlsli`, with one gradient table the two share.
- [x] Text strokes: stroked glyphs in the atlas (the coverage dilated by the
      join's kernel), and a ring for hollow lettering.
- [x] Rich text `<stroke>`.
- [x] `examples/25-gradients-and-strokes`; `ui_appearance_gate` (eighteen
      probes, `imgprobe`); unit tests for the draw list, the sequences and the
      scene file; conformance specs for the API; the manual page.

## Q2 — the editor, remade after VS Code

The owner, 2026-09-27: every frame of the editor looked at by a critical user
who knows VS Code well, and remade to be as close to it as possible in how it
works and how it looks -- responsive, practical for development, the smallest
details included. After Q1, and before anything else; defects still come first.

- [x] Home no longer carries the match controls (player count, dedicated
      server); they live on the Test tab alone. Play on Home still plays what
      Test says.
- [x] **An instrument to see it with**: `--editor-drive=FILE` feeds a script of
      pointer, key and text events into ImGui itself, frame by frame, and logs
      a marker a watcher photographs the window at (`PrintWindow`). The editor
      is driven and pictured with nobody's mouse moved -- the gap every
      milestone since E1 recorded ("no automated picture of the editor").
- [x] **The survey** (2026-09-27, photographs in the session's scratchpad):

  | Where | What a VS Code user meets | What VS Code does |
  |---|---|---|
  | Chrome | Four rows before the work: the OS title bar, a menu row, the ribbon tabs, the ribbon's buttons | One title bar that holds the menus and a command centre; everything else is the activity bar, the editor's own title actions and the palette |
  | Commands | `Ctrl+Shift+P` and `Ctrl+P` do nothing: no palette, no quick open | The palette runs every command by name; quick open finds any file |
  | Layout | No activity bar; Explorer, Content and Properties are loose docked windows, each with its own always-visible `X` | Activity bar -> side bar with collapsible sections; editor area; panel below; secondary side bar |
  | Status | No status bar; the scene's path sits at the right of the menu row and "Editing" in the toolbar | A status bar: what is open, its state, problems, notifications |
  | Explorer | Nineteen services listed flat, the scene buried among them | The workspace first; the rest grouped and collapsible; indent guides; hover actions |
  | Context menu | No "Insert object"; "Import..." first | New file / new folder first, then clipboard, then rename/delete |
  | Properties | Names cut ("OrthographicSi"); an enum reads "Enum.CameraP..."; two styles of section header | Full labels (wrapped or on their own line), the value's own name, one header style |
  | Menus | File has no Open Recent or Save All; Edit has no Cut/Copy/Paste/Find; "Window" and the "View" tab duplicate each other; Help is one item | File, Edit, Selection, View, Go, Run, Help, each full |
  | Tabs | A close button on every tab always; the active tab a filled block | The active tab lifted with a top accent line; close on hover; a dot when unsaved |
  | Panel | Console: a box inside a box, an unlabelled input line; "Reset" greyed at the far right | PROBLEMS / OUTPUT / DEBUG CONSOLE, the input labelled, actions in the panel's title |
  | Test tab | The dedicated-server checkbox is nearly invisible | Checkboxes with a visible box in both themes |
  | Colour | A teal-graphite palette of its own | Neutral greys, one blue accent, hairline borders |

- [x] **The remake**, in this order, each one photographed before and after:
  1. [x] The theme: VS Code's Dark Modern and Light Modern families (neutral
     greys, `#0078D4` accent, hairline borders, compact rows), fonts and
     metrics; fields and checkboxes on `#313131`, so a box is seen before it
     is hovered.
  2. [x] The status bar: run state (the bar turns blue while the game runs),
     the scene and its unsaved dot, error and warning counts, the selection,
     the tool or brush, the frame rate -- each one a button.
  3. [x] The command palette (`Ctrl+Shift+P`, `F1`) and quick open (`Ctrl+P`):
     every menu and toolbar action as a named command, "Insert: <class>" for
     each creatable class, "Color Theme" per theme, recently used first;
     `F5`, `Shift+F5`, `Ctrl+J`, `` Ctrl+` ``, `Ctrl+,`, `Ctrl+B`.
  4. [x] The activity bar (Explorer, Content, Run and Debug, Terrain, Blocks,
     Tiles, and the Manage gear); a click on the active view folds the whole
     side bar away and the next brings it back, as `Ctrl+B` does. Properties
     and the world tools on the secondary side bar (a terrain selected in the
     tree opens its panel, and in the side bar that would put the Explorer
     away under the click); Console and Stats in the panel. Layout revision 2
     rebuilds a saved layout once.
  5. [x] The menus rebuilt on VS Code's (File, Edit, Selection, View, Go, Run,
     Help), every item a palette command by id; the ribbon's tab row gone,
     its toolbar one row; the match's shape at the top of Run and Debug and in
     the Run menu.
  6. [x] Close buttons only on the active or hovered tab; "Insert Object"
     first on the Explorer's right-click; the Console filtered by level with
     counts, its actions as icons at the right, no box inside the box, its
     prompt labelled; Attributes and Tags headed like the property
     categories. **Decided against: the services grouped in the Explorer.**
     A virtual group row breaks the tree's exact row pitch and its
     selection, which index real instances, and hiding services moves rows
     the owner's hands know from the other platform. Revisit if he asks.
  7. [x] Notifications as cards in the bottom-right corner; a Welcome page
     (Help > Welcome: start, the project's scenes, the keys to know); search
     on the two long Preferences pages -- the script colours by name, the
     shortcuts by command or by keys -- and Preferences: Keyboard Shortcuts
     in the palette. General holds five settings, and a search there would
     find what the eye already sees.

## Q3 — then

Views ([`views-kickoff.md`](views-kickoff.md), ADR 0107): V0 and V1 built on
2026-09-27 -- more than one view a frame, `view://`, `CameraTexture` and
`examples/26-security-cameras`, the owner's camera game. V2 (`ViewportFrame`),
V3 (mirrors and portals) and V4 (`SubWorld`) next; **then the game-ready
plan** ([`game-ready-plan.md`](game-ready-plan.md), ADRs 0111 to 0123,
approved by the owner on 2026-09-27: saves, HTTPS, terrain materials and rules,
wind, foliage, water, the network service, encryption, editable primitives,
video and parallel scripts); then the AI panel ([`ai-kickoff.md`](ai-kickoff.md), ADR 0108).
