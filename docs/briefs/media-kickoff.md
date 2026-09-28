# Creativity and media: the kickoff and the ledger

Block D of [`game-ready-plan.md`](game-ready-plan.md), approved by the owner on
2026-09-27. The order across blocks is in that file (D1 and D3 together, D2
after C1); this one is the order of work inside block D and where each piece
stands.

Decisions: [ADR 0121](../decisions/0121-a-script-can-write-an-image-a-sound-and-a-mesh.md)
(D1, D3), [ADR 0122](../decisions/0122-a-videoplayer-decodes-av1-and-opus-and-the-platforms-h264.md)
(D2).

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## Stage D1 — `EditableImage` and `AudioStream` (ADR 0121 §1-2)

- [ ] IDL: `EditableImage` (`new`, `WritePixels`, `ReadPixels`, `Clear`,
  `Size`, `TextureName`); only changed rows uploaded, once a frame; the texture
  name taken by every texture slot through the ADR 0107 registry.
- [ ] IDL: `AudioStream` (`new`, `Push`, `Buffered`, `Starved`), through the
  audio mix, 3D when parented to a part.
- [ ] Budgets and F3 lines.
- [ ] An example: a drawing board on a `SurfaceGui`, and a tone synthesised in
  Luau.
- [ ] Tests: pixels written read back and drawn (a screenshot); a pushed sine
  plays without a gap at a steady push rate.

## Stage D3 — `EditableMesh` (ADR 0121 §3)

- [ ] IDL: `EditableMesh` (vertices, triangles, normals, UVs, colours; by
  `buffer` and per element; `ComputeNormals`); `MeshPart:SetEditableMesh`.
- [ ] Only changed ranges uploaded, once a frame.
- [ ] `CollisionMode`: `None`, `Static` (Jolt triangle mesh, rebuilt at most
  once a tick, anchored only), `Dynamic` (convex hull; decomposition opt-in).
- [ ] Edits to a colliding mesh applied on the tick; in the trace.
- [ ] An example: a destructible wall, or terrain-like ground built in Luau.
- [ ] Tests: a static mesh collides after an edit; a dynamic hull falls and
  rests; the trace reproduces.

## Stage D2 — `VideoPlayer` (ADR 0122)

- [ ] Vendor **dav1d** and **libopus** (manifest rows, `vendor.luau`,
  `THIRD_PARTY_NOTICES.md`); the engine's own WebM demuxer.
- [ ] Platform decoders for MP4/H.264/AAC: Media Foundation, `AMediaCodec`,
  AVFoundation; Linux refuses MP4 by keyed error.
- [ ] IDL: `VideoPlayer` (`Source`, `Play`, `Pause`, `Stop`, `TimePosition`,
  `TimeLength`, `Looped`, `Playing`, `Volume`, `Buffering`, `Loaded`,
  `Ended`); its texture name in any slot; sound through the mix.
- [ ] Decoding on a job thread; frames uploaded directly; audio as the clock.
- [ ] From a URL with ranged requests (needs C1).
- [ ] `assetc video` converts to WebM/AV1/Opus.
- [ ] An example: a screen in the world playing a clip, and a cutscene.
- [ ] Tests: a short clip decodes the expected frame at a time (a screenshot);
  audio and video stay within a stated drift; a URL clip plays from a loopback
  server.

## Findings

(Filled as the work goes: what the ADRs assumed that reality corrected.)
