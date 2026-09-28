# 0122 — A `VideoPlayer` decodes AV1 and Opus, and the platform's H.264

- Status: accepted (to be built; see `docs/briefs/media-kickoff.md`, D2)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, approving the plan in
  `docs/briefs/game-ready-plan.md` and **approving the dependencies dav1d and
  libopus** (R5).
- Builds on: [0121](0121-a-script-can-write-an-image-a-sound-and-a-mesh.md)
  (`EditableImage`, `AudioStream`),
  [0119](0119-networkservice-opens-connections-and-a-project-declares-what-it-may-do.md)
  (streamed and ranged requests), [0063](0063-https-stays-refused-and-tls-comes-from-the-platform.md)
  (the platform's own facilities, preferred for what they are good at).

## Context

A game plays a cutscene, a trailer on a screen in the world, or a stream from a
URL. The engine has no video. The usual answer, FFmpeg, is LGPL or GPL — R6
rules it out — and H.264 carries patent licensing an engine should not take on
for its users.

## Decision

1. **`VideoPlayer`**: `Source` (a content asset or an `http(s)://` URL),
   `Play()`, `Pause()`, `Stop()`, `TimePosition`, `TimeLength`, `Looped`,
   `Playing`, `Volume`, `Ended` signal, `Loaded` signal. Its picture is a
   texture name any texture slot takes (a UI image, a `SurfaceGui`, a
   material), and its sound plays through the audio mix, 3D when parented to a
   part.
2. **The portable format is WebM with AV1 video and Opus audio**, decoded by
   **dav1d** (BSD-2) and **libopus** (BSD-3), both vendored at their latest
   release tags when this is built (R5 satisfied by this record). A small WebM
   demuxer is the engine's own.
3. **MP4 with H.264 and AAC through the platform's decoder**: Media Foundation
   on Windows, `AMediaCodec` on Android, AVFoundation on macOS. On Linux, where
   there is no universal system decoder, MP4 is refused with a keyed error that
   names WebM; the engine carries no H.264 decoder of its own.
4. **Frames go to the GPU without a copy through Luau**: decoding runs on a job
   thread; frames are uploaded to the video's texture directly, and audio is
   pushed to its stream, with the audio clock as the master.
5. **From a URL**, the player reads with ranged requests (ADR 0119) and buffers
   ahead; `Buffering` says when it waits.
6. **`assetc video`** converts a video to the portable format at import, so an
   author can drop in an MP4 and ship WebM.

## Consequences

- Cutscenes, screens in the world and streams, with no copyleft and no patent
  licence in the engine.
- Two more vendored libraries.
