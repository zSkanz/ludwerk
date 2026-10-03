# Orbit — LuauG editor icon system

Created 2026-09-23. This is the active style for the default editor theme:
251 unique drawings serving 272 logical IDs. The application logo and branding
are a separate identity. Earlier PNG masters and briefs in the parent directory
are retained as historical sources; this specification supersedes their style.

## Exact design prompt

> Create the LuauG editor icon family in a style named Orbit. Design clean,
> compact geometric symbols with a calm, modern appearance. Use a 24 by 24
> coordinate grid, a consistent 1.8-unit stroke, round line caps and round joins.
> Prefer simple outlines with generous open interiors and recognizable silhouettes.
> Reserve solid areas for playback controls, small dots and cursor symbols.
> Center each subject optically and normally keep it within coordinates 2 to 22;
> allow small optical overshoots. Make the primary meaning legible at 16 pixels
> and preserve the silhouette at 13 pixels. Use one white ink on true transparency;
> colors are assigned by the editor, never painted into the source. No gradients,
> shadows, texture, bevels, lettering, decorative background tiles or tiny details.
> Related states share their base geometry: locked/unlocked, visible/hidden,
> script/module script. Distinguish classes by silhouette and meaningful interior
> marks. Use a conventional isometric wireframe only for spatial objects.
> For the stamp overlay use a solid circular base and the same circle with a
> transparent central hole. Deliver editable SVG geometry and 256 by 256 RGBA
> PNG masks, with pure white RGB even in transparent pixels. Check every icon
> on both light and dark backgrounds at 32, 24, 16 and 13 pixels.
>
> Subject: {logical ID and its meaning}. Preserve the established metaphor when
> adjusting an existing icon. Change only the requested characteristics and keep
> all shared family constants identical.

This prompt was the design specification for code-authored geometry, not a claim
of an image-model generation. No image model or external icon library was used.
The exact subject geometry is recorded in `draw_icon()` in
[`tools/repo/draw_icons.py`](../../../tools/repo/draw_icons.py).

## Reproduce or change

Run from the repository root:

```sh
python tools/repo/draw_icons.py
# Existing entry point also works:
python icons/bake.py
```

Requires the existing art-tool dependency Pillow. No engine dependency is added.
The generator reads the current theme manifest, keeps all IDs, aliases, roles,
palette and overlay settings intact, and emits:

- SVG sources under this directory, grouped by class, action, content and overlay.
- Runtime masks under `icons/default/`, using the existing file paths.
- `preview-dark.png` and `preview-light.png` with runtime-style BOX downsampling.
- `index.html`, a local gallery with a light/dark switch.

Edit the geometry in the generator, then regenerate. SVGs are portable editable
exports; direct SVG edits must be reflected in the generator before the next bake.
Stroke and grid constants apply to the entire family. Do not normalize individual
icons by cropping: that changes stroke weight. PNGs are rendered at 768 pixels
and reduced to 256 with Lanczos antialiasing; RGB stays white throughout.

## Review

Inspect both preview sheets. Check pairs at small sizes, inspect alpha bounds,
verify every manifest path and intentional alias, and confirm the overlay has
the same outer silhouette as its base. The gallery is monochrome; the PNG sheets
apply the actual role colors from the theme. The engine must be rebuilt/copied
and restarted to refresh an already loaded icon atlas.

## Coverage extension

Added the built-in network, player, particle, terrain, voxel, 3D GUI and
constraint classes. Abstract/base classes reuse the corresponding family
silhouette. Dedicated selection, terrain operation and clipboard symbols
replace unrelated action icons in the editor.

Subject prompt extension: use connected nodes for networking, a particle plume
for emitters, mountain contours for terrain and stepped cells for voxels. Use
a cursor for selection, arrows over a ground line for raise/dig, an even line
for flatten, a brush for paint, and conventional clipboard/scissors/document
symbols. Preserve Orbit's 24-unit grid, rounded strokes and white alpha masks.

Validation: all 71 classes in the generated engine reflection descriptors
resolve to a declared icon. All 167 IDs resolve to existing 256 x 256 RGBA
white masks with nonempty alpha. Dark/light sheets cover 32/24/16/13 px.

Further action subjects: Import uses an arrow entering a tray; NewFolder adds
a plus to the folder; Tools uses a wrench/tool silhouette; Information uses
a circled i. PlaceBlock, BreakBlock and ReplaceBlock share a cube silhouette
with plus, minus and replacement-arrow marks. These are wired to Content,
Tools, About and the Blocks panel, retaining text labels and tooltips.

Latest action-extension validation: PNG mask checks and Windows app/platform/
editor-shell suites passed. The Linux host build was blocked by unrelated
`-Wdouble-promotion` errors in scene_file.cpp (terrain cell settings); no full
Linux test pass is claimed for this extension.

2D layer extension: Part2D is a flat outlined shape with an origin marker and
an offset plane corner; Tilemap2D is a stepped arrangement of square tiles.
Use the spatial role, shared rounded strokes and existing alpha-mask format.
Both resolve directly from reflected class names in the Explorer and pickers.

2026-09-24: Tiles-panel extension. Erase uses a diagonal eraser silhouette;
View2D uses a flat frame with perpendicular axes. Keep Orbit geometry and masks.
Tiles tabs/menus use Tilemap2D; Paint reuses the brush.

Tiles-extension validation: all 128 manifest IDs have valid 256px RGBA masks;
preview inspected at 32/24/16/13 px. The Windows interface object compiled.
The complete build was blocked by undefined asI32 calls in replication's
extract.cpp, outside this icon change. The subsequent Linux host build and targeted
app/platform/editor-shell tests passed.

NavigationService extension (2026-09-24): draw a route from an outlined start
point through two waypoints to an arrow indicating the destination. Use the
motion role and preserve Orbit's 24-unit grid, stroke and alpha-mask rules.
The Explorer resolves it directly by the reflected class name.

Material-assets extension: keep the surface sphere for content.Material (alias
of the established material art). NewMaterial adds a plus to that silhouette;
MaterialVariant connects a parent surface to a derived surface. Use the spatial
role and the same rounded white strokes. Wire the two actions to the browser's
New Material and New Variant menus.

Refresh refinement: use two opposing open circular arcs with arrowheads
connected to their endpoints. Keep clear gaps between the arrows and an open
center so the refresh silhouette survives at 13/16 px. Do not reuse Rotate's
single arc or add disconnected arrowhead marks.


Script and material actions extension (2026-09-24) ? exact drawing prompt:
> Extend Orbit on its existing 24-unit grid, with 1.8-unit rounded strokes,
> white RGBA masks and no embedded text. Replace shows source text lines and
> a right arrow leading to replacement lines; ReplaceAll adds a second route
> into those lines. StepOver arches over a stopped point, StepInto points
> down toward that point, StepOut points upward away from it. Revert uses a
> return arc around a value line. Inherit connects a parent box to a child box
> with a downward elbow arrow. Keep every action neutral and distinguish the
> three debugger steps by silhouette. Inspect at 32, 24, 16 and 13 pixels on
> light and dark surfaces. Retain labels on replacement and debugger controls;
> compact close/revert/inherit controls have descriptive tooltips.

Seven new drawings: Replace, ReplaceAll, StepOver, StepInto, StepOut, Revert,
Inherit. Existing Play, Close, Save, Undo, Redo and Copy are reused. The theme
now resolves 139 IDs to 127 drawings. [Focused review](script-actions-review.png).


## Planned lighting classes ? 2026-09-24

Source: [accepted ADR 0096](../../../docs/decisions/0096-atmosphere-post-effects-and-a-sky-are-instances-under-lighting.md)
and its [implementation plan](../../../docs/briefs/atmosphere-post-kickoff.md).
Seven concrete classes plus the abstract PostEffect base; these assets reserve
names in the icon theme, not new classes in the runtime API. Explorer lookups
will resolve them by class name once those classes are implemented.

Exact authored-vector prompt:
> Extend the Orbit family with eight lighting icons on its 24-unit grid,
> using 1.8-unit rounded white strokes and transparent RGBA masks. Atmosphere
> is a horizon dome above three receding haze layers. Sky is a framed sky
> with a small sun and a cloud arc. BloomEffect is a four-point glow with
> separated diagonal rays. ColorCorrectionEffect is a circle split between
> a clear half and a hatched half. BlurEffect is a central circle with three
> short horizontal diffusion marks on each side. DepthOfFieldEffect is a
> sharp central circle inside four focus corners. SunRaysEffect is a sun
> in the upper left emitting three long divergent shafts. PostEffect is
> two offset image layers with a processing curve on the front. Use the
> existing light palette role; no baked-in color, gradients, letters or new
> dependencies. Keep each silhouette distinct from Lighting, PointLight and
> SpotLight. Review all eight at 32/24/16/13 pixels on light and dark surfaces.

[Focused lighting review](lighting-review.png). Reproduce with
`python tools/repo/draw_icons.py`, then `lute tools/repo/genicons.luau`.
Lighting-extension total: 147 IDs, 135 drawings, including these eight class IDs.


## Navigation extension ? 2026-09-25

Source: [ADR 0098](../../../docs/decisions/0098-navigation-agent-types-areas-links-crowds-and-the-plane.md).
NavigationArea, NavigationLink and NavigationAgent are registered by their
planned class names; their implementation remains independent of the artwork.
Explorer resolves the class IDs through its existing name-based lookup.

Exact authored-vector prompt:
> Extend Orbit with NavigationArea, NavigationLink and NavigationAgent. Keep
> the 24-unit grid, 1.8-unit rounded strokes, white transparent RGBA masks and
> the existing motion palette used by NavigationService. NavigationArea is
> a diamond-shaped ground region subdivided by crossed diagonals, with a
> lower contour suggesting its volume. NavigationLink is two outlined endpoint
> circles joined visually by an overhead jump arc with an attached arrowhead.
> NavigationAgent is a walking figure with a separate destination arrow in
> the upper right. Preserve open negative space and distinct silhouettes;
> no letters, gradients, textures or new dependencies. Compare with the
> existing NavigationService at 32, 24, 16 and 13 pixels on both themes.

[Navigation review](navigation-review.png) includes the existing service for
comparison and is regenerated by `python tools/repo/draw_icons.py`.
Regenerate C++ IDs with `lute tools/repo/genicons.luau`.
Total after navigation: 150 IDs and 138 drawings.

## Teams extension -- 2026-09-25

Source: [ADR 0099](../../../docs/decisions/0099-teams-and-network-ownership.md).

Exact authored-vector prompt:
> Extend Orbit with Team and TeamService in the system palette that Player and
> NetworkService use. Team is a flag on a pole: a vertical staff with a
> swallow-tailed pennant at its top. TeamService is two Player silhouettes side
> by side, heads and shoulders overlapping slightly, standing on one base line.
> Keep the 24-unit grid, 1.8-unit rounded strokes and white transparent masks;
> compare with Player at 32, 24, 16 and 13 pixels on both themes.

Regenerate with `python tools/repo/draw_icons.py`, then `lute tools/repo/genicons.luau`.
Total after teams: 152 IDs and 140 drawings.

[Teams review](teams-review.png) compares Player, Team and TeamService at
32/24/16/13 pixels on both themes; regenerated by the same artwork script.


## Scripts, shaders and 2D ? 2026-09-26

Seven dedicated drawings replace four aliases and prepare three service IDs
from [ADR 0105](../../../docs/decisions/0105-server-code-lives-in-serverscriptservice-and-a-dedicated-client-carries-none.md).
The three script services are artwork registrations, not API implementations.
Existing NewShader and content.Shader callers automatically use the new art;
class-name lookup resolves the new class drawings without UI code changes.

Exact authored-vector prompt:
> Extend Orbit on the existing 24-unit grid with 1.8-unit rounded white strokes
> and transparent RGBA masks. SpringConstraint2D is a zigzag spring between
> two fixed vertical endpoints, using the physics role. SpriteAnimator is two
> offset flat frames with a filled play triangle, using the motion role.
> Shader is a hexagonal surface with inward code chevrons; NewShader repeats
> that motif smaller with a distinct plus at the lower right. Use the script
> role for both. ServerScriptService is a server enclosure with a status row
> and code chevrons. ClientScriptService is a monitor and stand containing
> code chevrons. GlobalScriptService is a globe above code chevrons. Keep all
> three in the script role, distinguished by silhouette rather than color.
> No text, gradients, shadows, textures or new dependencies. Review the seven
> drawings at 32, 24, 16 and 13 pixels on light and dark surfaces.

[Focused review](scripts-shaders-2d-review.png) is reproduced by
`python tools/repo/draw_icons.py`; regenerate IDs with
`lute tools/repo/genicons.luau`. Total after scripts/shaders/2D: 162 IDs, 147 drawings.


## Scenes, views and AI ? 2026-09-26

Sources: ADRs [0106](../../../docs/decisions/0106-a-scene-is-a-place-and-the-game-changes-scenes-at-run-time.md),
[0107](../../../docs/decisions/0107-a-camera-draws-into-a-texture-a-frame-draws-its-own-instances-and-a-scene-runs-beside-another.md)
and [0108](../../../docs/decisions/0108-the-engine-is-driven-by-ai-through-one-tool-registry-an-mcp-server-and-an-ai-panel.md).
SceneService already exists and resolves its icon by the existing class-name
lookup. CameraTexture, ViewportFrame and SubWorld reserve their planned class
IDs. `action.AI` / `icons::ActionAI` is ready for the planned panel's menu and
button; registering this art does not implement the panel or its features.

Exact authored-vector prompt:
> Extend Orbit with five distinct symbols on the 24-unit grid, using 1.8-unit
> rounded white strokes and transparent masks. SceneService shows two scene
> cards connected by transition arrows; the first has a clapperboard header.
> CameraTexture shows a camera and lens sending an image to a separate small
> rectangular surface. ViewportFrame is a UI window with a header and a cube
> inside. SubWorld is a cube enclosed in a circular world boundary. AI is a
> conversation bubble with one central four-point sparkle. Use system for
> SceneService, spatial for CameraTexture and SubWorld, ui for ViewportFrame,
> and neutral for the AI panel action. No letters, gradients, shadows or extra
> dependencies. Inspect distinct silhouettes at 32, 24, 16 and 13 pixels on
> light and dark backgrounds.

[Focused review](scenes-views-ai-review.png) is regenerated by
`python tools/repo/draw_icons.py`; IDs by `lute tools/repo/genicons.luau`.
Total after scenes/views/AI: **167 IDs and 152 drawings**.

## Game-ready classes and UI redraws — 2026-09-27

Ten dedicated drawings reserve the planned subjects from ADRs 0111, 0116,
0118, 0121, 0122 and 0123: SaveService, SaveSlot, FoliageLayer, FoliageMesh,
Water, EditableImage, AudioStream, EditableMesh, VideoPlayer and Actor.
These registrations do not implement the corresponding APIs. SaveSlot and
editable resources have reserved class IDs for future editor representations.

UIGradient and UIStroke were the only class artwork additions in the subsequent
git history after the original imported Orbit set. Both are redrawn at the
owner's request. UIGradient uses a swatch with progressively sparser coverage;
UIStroke uses a pen finishing an open contour, avoiding UIPadding's nested boxes.

Authored-vector subjects: an archive tray and downward arrow for SaveService;
an individual archive drawer for SaveSlot; two shoots on a ground contour for
FoliageLayer and a veined leaf for FoliageMesh; three flowing wave lines for
Water; a film frame and play triangle for VideoPlayer; bracketed parallel lanes
for Actor; an image surface and pen for EditableImage; waveform and output arrow
for AudioStream; a triangular surface with vertex handles for EditableMesh.
Preserve the Orbit grid, rounded strokes, white masks and existing palette roles.

[Focused review](game-ready-ui-review.png) includes all twelve drawings on light
and dark backgrounds at 32, 24, 16 and 13 pixels. Reproduced by the existing
generator. Manifest paths, RGBA masks, SVG exports, current API class coverage,
distinct artwork and generated C++ registry freshness were checked.

Current total: **183 IDs and 168 drawings**.

### Tools redraw

At the owner's request, `action.Tools` now uses one closed open-jaw wrench
contour. The earlier crossing polyline is removed; the handle and jaw remain
separate readable features at small sizes. Existing neutral tint and ID stay.
[Tools review](tools-review.png) is generated at 64/32/24/16/13 px on both themes.

### Whole-set readability review

The owner requested a review of confusing or unattractive icons. Eight drawings
were revised after inspecting the full set:

| Subject | Problem | Revised geometry |
|---|---|---|
| Bone | Overlapping circles cut into each other and obscure the subject | One continuous bone contour with lobed ends |
| NetworkService | Spokes cross the center node | Three endpoints joined by a clean branching connection |
| CameraTexture | Camera, tiny output and arrow crowd one square | Lens on a surface with a separate rear edge |
| SceneService | Two cards and two arrows compete at small sizes | Stacked scene cards with one transition arrow |
| GlobalScriptService | Globe meridians and code marks become dense | Simplified globe with code chevrons in an open lower-right quadrant |
| TextInput | The T overlaps the field boundary | Two text lines beside an insertion caret |
| UIGradient | The first redraw resembles a bar chart | Bordered swatch transitioning from a continuous line to sparse dots |
| Rotate | Arrowhead does not meet the arc cleanly | Arrowhead attached to the arc endpoint |

These subjects supersede the corresponding earlier geometry descriptions.
No IDs, aliases or palette roles changed. SVGs, PNG masks and all affected
review sheets are regenerated from the same geometry. See
[readability review](readability-review.png) for 32/24/16/13 px on both themes.

The GlobalScriptService circle-and-chevrons revision was subsequently rejected
by the owner. Its replacement restores a recognizable globe: an equator,
one elliptical meridian, and an open lower-right quadrant holding a separate
code mark. Reviewed beside the server/client script services at all four sizes
in [the script services sheet](scripts-shaders-2d-review.png).


## Keyboard, shape previews and filled folders ? 2026-09-27

Replaced the provisional Keyboard, ShapeBall and ShapeCylinder art with
reproducible Orbit geometry. Keyboard has a rounded enclosure, two rows of
keys and a space bar. ShapeBall uses a circular silhouette with curved surface
contours; ShapeCylinder uses an elliptical top, straight sides and a curved
base. ShapeBlock intentionally continues to alias class.Part.

FolderFilled now shares the exact outline of class.Folder, with solid ink to
indicate a folder containing files. This is an intentional filled-state
exception to the outline convention. All four drawings are authored in
`tools/repo/draw_icons.py`, exported as SVG and 256px white RGBA masks.
Existing IDs, paths and palette roles are unchanged.

[Focused review](shapes-keyboard-folders-review.png) compares the six subjects
(including the reused block and empty folder) at 32/24/16/13 px on the current
neutral dark and white editor surfaces. Total: **188 IDs, 172 drawings**.


## Planned toolkit ? ADRs 0126?0132

At the owner's request, 50 logical IDs reserve artwork for the complete planned
Toolkit: 45 class/configuration IDs and five modeling actions. Union and Negate
share their corresponding class drawings, yielding 48 new vector masters.
These are artwork registrations only; they do not implement classes, UI actions
or the modeling dependency. TextChatMessage remains a value, not a class icon.

Exact authored-vector brief: extend Orbit's 24-unit grid, 1.8-unit rounded
strokes and white transparency masks. Use a clicking pointer and proximity
rings for interaction; arrows, targets and attachment endpoints for physics;
rectangular layouts, bounds and a text caret family for UI; an outlined target,
tapered beam and flowing ribbon for visual effects. Boolean modeling uses joined
or cut contours, with a wedge and lattice beam for the new shapes. Audio effects
use waveforms, frequency response curves, echoes and level controls. Chat uses
speech bubbles distinguished by channel, command and configuration marks;
preloading uses an asset entering a tray, haptics a vibrating device. No letters,
textures, gradients, shadows or new palette colors. Configurations use the
system role, haptics the input role; all action icons remain neutral.

`TOOLKIT` and `toolkit_icon()` in the existing generator are the inventory and
shared geometry. All exports and seven light/dark review sheets are reproducible
with `python tools/repo/draw_icons.py`; regenerate C++ IDs with
`lute tools/repo/genicons.luau`.

[Review the entire toolkit](toolkit-review.html), inspected at 32/24/16/13 px.
Current total: **238 IDs and 220 drawings**.


## Water children and terrain sampling ? 2026-09-29

Three dedicated Orbit drawings: `class.WaterPoint` uses a flowing spline with
an outlined control point; `class.WaterWave` uses a single sinusoid and a travel
arrow, distinct from Water's three ripples; `action.Eyedropper` uses a diagonal
pipette with a tapered tip, guard and bulb. Preserve the 24-unit grid and rounded
1.8-unit strokes, white transparency masks and existing spatial/neutral roles.

WaterPoint and WaterWave resolve through the existing class-name lookup.
Eyedropper is registered for terrain sampling; this artwork change does not add
or change a toolbar control. SVG, PNG and C++ registry exports are generated by
the existing tools. [Focused review](water-terrain-review.png) shows all three
beside Water at 32/24/16/13 px on both current editor surfaces.

Current total: **241 IDs and 223 drawings**.


## Graphics settings and water tools ? 2026-10-01

Five dedicated drawings for ADRs 0146 and 0147: GraphicsService is a monitor
with adjustment sliders; WaterRiver is a pair of curved banks; WaterLake is an
organic closed shoreline with a ripple; WaterPool is a rectangular basin with
a ladder and ripple; WaterOcean is a horizon over two wave rows. Maintain
Orbit's 24-unit grid, rounded 1.8-unit strokes and white transparency masks.
GraphicsService uses the system role; the water actions remain neutral.

The River, Lake, Pool and Ocean buttons in the Water panel now use these four
action IDs. GraphicsService is a reserved class icon for the planned service.
[Focused review](graphics-water-review.png) shows all five at 32/24/16/13 px on
both current editor surfaces. The three existing script-side badges are included
in the current inventory: **249 IDs, 231 drawings**.


## CryptoService and SoundEffect ? 2026-10-02

CryptoService uses a shield containing a key, in the system role. SoundEffect,
the abstract base of the audio effects, uses a waveform inside processing
brackets, in the audio role. Both are dedicated drawings on the Orbit 24-unit
grid with 1.8-unit rounded strokes, white RGBA masks and no baked-in colors.
They resolve through the existing class-name lookup; no editor code is needed.

[Focused review](crypto-sound-review.png) compares both new drawings with
AudioService and EqualizerSoundEffect at 32/24/16/13 px on light and dark surfaces.
Sources, runtime masks and review sheets are reproduced by the existing icon
generator. Current total: **251 IDs and 233 drawings**.


## Editor actions and localization ? 2026-10-03

Fourteen new drawings: LocalizationService (globe and speech bubble), NewStamp
(cube and plus), StampVariant (cube and derived diamond), PlaceLinked (cube and
chain), ApplyOverrides (checked layered cards), SelectCopies (two selected
objects), LocalAxes (three spatial axes), WorldAxes (global axes), Pivot (offset
rotation point), SelectionCenter (centered cross in selection bounds),
SimulationStep (play up to a bar), Console (terminal), Stats (bar chart), and
WaterTools (droplet containing a ripple). Preserve Orbit geometry and palette.

The owner clarified that Water's left-menu symbol was too generic, not absent.
WaterTools now represents that panel in its activity button, tab, menus and
command palette. The class.Water waves remain the Explorer class icon. Water's
four tool actions and its panel now use generated constants rather than strings.
New editor actions are wired to their corresponding controls; open, revert and
replace stamp actions reuse existing drawings. The stand-alone PlaceLinked
symbol replaces use of the small overlay badge as a menu icon.

[Focused review](editor-localization-review.png) shows the new family alongside
Water and WaterPoint at 32/24/16/13 px, light and dark. Current inventory:
**265 IDs and 247 drawings**. Runtime themes must be restaged and the editor
restarted after rebuilding to refresh the cached atlas.


The follow-up interface sweep adds ThemeDark (crescent), Snap (magnet),
GenerateTerrain (hills and plus) and TerrainMaterials (layered surfaces).
ThemeLight reuses PointLight's sun, Ungroup reuses Separate, and
ViewportSettings reuses GraphicsService's monitor/sliders, all with neutral
roles. These seven IDs replace generic icons in the launcher, theme palette,
snapping controls, ungroup commands, viewport settings and terrain tools.
Keyboard shortcuts now reuse the existing Keyboard icon. This editor batch
adds 21 IDs and 18 drawings in total: **272 IDs, 251 drawings**.

Validation: all API classes have theme entries; all masks and SVGs load; editor
constants resolve. MSVC syntax checking of debug_overlay.cpp passed. The full
Windows gate could not regenerate its Ninja build files (permission denied),
so no successful executable rebuild is claimed for this artwork delivery.
