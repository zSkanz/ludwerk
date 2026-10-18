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
