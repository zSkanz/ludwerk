# 0247 — The room around the player is seen through, and known

- Status: accepted
- Date: 2026-10-17
- Builds on: ADR 0244 (OpenXR), ADR 0245 (`XRService`, the play space),
  ADR 0238 (the data service, for anchors shared online)

## The question

Augmented reality on a headset -- the maintainer's is a Meta Quest 3 -- is the
player's real room seen through the headset's cameras, in colour, with the
game's world drawn into it: a creature walking on the real floor, a board on
the real table, a window in the real wall. For that a game needs the
passthrough on and blended, to know where the room's floor, walls, tables and
furniture are, and to pin things to places in the room that stay there across
sessions.

## Decision

### 1. Passthrough: what is behind the picture

`XRService.Passthrough = true` shows the room behind everything the game
draws: the frame is submitted with the runtime's alpha blend
(`XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` where the runtime has it, and
`XR_FB_passthrough` on the Quest), and **where the game draws nothing, the
room is seen** -- the sky is not drawn, and a pixel's alpha says how much of
the room shows through it. `Lighting.XRPassthroughOpacity` (0 to 1) fades
between the room and the game's world, for a portal or a transition. A
runtime with no passthrough answers `XRService:SupportsPassthrough()` false
and the property does nothing.

### 2. The room: planes and its mesh

Where the runtime knows the room -- the Quest 3's scene, which the player
captures once in its settings -- `XRService:GetRoomAsync()` returns it:

```luau
--!strict
local room = XRService:GetRoomAsync()     -- nil when the runtime has none or the player refused
for _, plane in room.Planes do            -- floor, ceiling, walls, tables, couches, ...
    print(plane.Kind, plane.CFrame, plane.Size)   -- Enum.XRPlaneKind; a rectangle in the play space
end
local mesh = room.Mesh                    -- the room's triangles, or nil
```

On the Quest this is OpenXR's scene (`XR_FB_scene`, `XR_FB_spatial_entity`,
and `XR_META_spatial_entity_mesh` for the mesh), asked for with the
`com.oculus.permission.USE_SCENE` permission, which the player grants. **The
room can be made solid**: `XRService:CreateRoomParts()` makes an anchored,
invisible, collidable part for each plane and a `MeshPart` for the mesh, under
`Workspace.XRRoom`, so physics, raycasts and navigation use the real room
like any scene.

### 3. Anchors: a place in the room that stays

`XRService:CreateAnchorAsync(cframe)` pins a place in the room
(`XR_FB_spatial_entity`); `anchor.CFrame` follows it as the runtime refines
it, and `anchor:SaveAsync()` keeps it on the headset, so `XRService:
LoadAnchorsAsync()` finds it in the next session, in the same room. Saved
anchors are the headset's; sharing one with another player in the same room
(`XR_META_spatial_entity_sharing`) is a later step, and needs the platform's
accounts.

### 4. Phone AR is not this

AR on a phone (ARCore) is a different runtime, not OpenXR; it would be a
provider under ADR 0188's rules if the maintainer asks for it, and is not decided
here.

## Proof

- The simulated headset with passthrough on: the frame is submitted with the
  alpha blend and the sky's pixels have an alpha of zero.
- A recorded room (a floor, four walls and a table, as the Quest's scene gives
  them) is returned by `GetRoomAsync` in the play space; `CreateRoomParts`
  makes parts a ball dropped on the table rests on.
- An anchor created, saved, and loaded in a second session of the simulated
  headset is at the same place.
- On the Quest 3, standalone (the maintainer's): the room seen in colour, a ball
  bouncing on the real table, an anchor that is still there after the app
  restarts.

## Consequences

- New API: `XRService.Passthrough`, `SupportsPassthrough`,
  `GetRoomAsync`, `CreateRoomParts`, `CreateAnchorAsync`,
  `LoadAnchorsAsync`, `XRAnchor` (`CFrame`, `SaveAsync`, `Destroy`),
  `Enum.XRPlaneKind`, `Lighting.XRPassthroughOpacity`; the Quest's scene
  permission in the APK's manifest when a game asks for the room.
- A game for AR draws no sky; a game for VR is unchanged.

## Not decided here

Sharing anchors between players in one room, phone AR, and lighting the
game's objects from the room's real light (the runtime offers no estimate
on the Quest today).

## As built, first step (2026-10-18)

- **Passthrough** (§1): `XRService.Passthrough`, `SupportsPassthrough` and
  `Lighting.XRPassthroughOpacity`. The frame is submitted with
  `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` where the runtime lists it, and
  otherwise over a `XR_FB_passthrough` layer -- the Quest's -- made the first
  time it is asked for and paused and resumed after; the picture's layer
  blends by its alpha, unpremultiplied. The Quest's manifest says the app may
  use passthrough.
- **The alpha is the eye's depth**, not the renderer's: the copy of an eye
  into the runtime's image reads the depth the eye was drawn with, so where
  nothing was drawn (the sky, never cleared nearer) the picture is clear, and
  where something was its alpha is one less the opacity. Nothing else of the
  renderer carries alpha, and nothing else had to change: the sky is drawn
  and hidden, which costs its pixels and nothing more.
- Proved under the simulated headset with a GPU (`xr_simulated_passthrough`):
  the left eye's picture is clear over the horizon and solid on the floor,
  read by `imgprobe`, which now reads alpha.
- **Not yet**: the room's planes and mesh (§2), and anchors (§3).

## As built, the room's planes (2026-10-18)

- `XRService:GetRoomAsync()` returns `{ Planes = { { Kind, CFrame, Size } } }`,
  each plane a rectangle in the play space a script reads: its `CFrame` is
  its middle, the rectangle across its x and y, facing along its z into the
  room. `Enum.XRPlaneKind` names the floor, the ceiling, a wall, a table, a
  couch, a door, a window, or anything else.
- On the Quest it is the scene the player captured, through
  `XR_FB_spatial_entity`, its query and `XR_FB_scene`: every space with a
  rectangle, located in the play space, and its kind from its first
  semantic label. The player is asked first, at run time, for
  `com.oculus.permission.USE_SCENE`, which the Quest's manifest now lists.
  A refusal, or a runtime without the scene, answers nil.
- `XRService:CreateRoomParts()` makes a slab, 10 cm thick, behind each plane
  in `Workspace.XRRoom`: anchored, collidable, not drawn and casting no
  shadow, placed where the play space is in the world when it is called.
  The slab sits behind the plane so a thin plane cannot be passed through
  by a fast fall.
- The simulated headset answers with a recorded room, a floor three metres
  by three, a ceiling at 2.5, four walls and a table whose top is 0.75 up.
  `xr_room` proves it: the seven planes come back, and a ball dropped on the
  table rests on its top.
- **Not yet**: the room's mesh (`room.Mesh`, `XR_META_spatial_entity_mesh`),
  and anchors (§3).

## As built, anchors (2026-10-18)

- `XRService:CreateAnchorAsync(cframe)` returns an `XRAnchor`: `CFrame`,
  where it is now in the play space; `Tracked`, whether the headset knows;
  `Id`, the same in every session; `SaveAsync`; and `Destroy`, which lets
  the headset's anchor go. `XRService:LoadAnchorsAsync()` returns every
  anchor the game saved. An anchor is this machine's alone, and the wire
  excludes the class by name.
- On the Quest it is `XR_FB_spatial_entity`'s anchor, made storable as soon
  as it is made, so that saving it is one step; saved in the headset's own
  storage with `XR_FB_spatial_entity_storage`; and loaded by a query of that
  storage for every locatable space, each made locatable as it is found.
  The manifest lists `com.oculus.permission.USE_ANCHOR_API`.
- Loading an anchor a script already holds returns the same `XRAnchor`, and
  the headset's second handle for it is let go of.
- The simulated headset keeps saved anchors in the file
  `--xr-simulated-store` names. `xr_anchors` runs it twice: the first run
  pins an anchor and saves it, and the second loads it at the same place
  with the same id.
- **Not yet**: the room's mesh, and sharing an anchor between players.
