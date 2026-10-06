# 0180 — A phone's textures are ASTC

- Status: accepted
- Date: 2026-10-05
- Decided by: the agent, in the performance programme the owner put first,
  under the standing rule to decide as professional engines do and record it
- Builds on: ADR 0073 (compiled textures: one container, transcoded to what
  the device samples), the mobile audit's item G7

## Context

A compiled texture is carried once, as UASTC in a KTX2 container, and
transcoded when it is read into whatever the device samples. The transcoder
was told of four targets: BC7, BC1, BC3, and RGBA. A phone's GPU samples none
of the BC formats, so on Android every texture of every game was sent
uncompressed: four bytes a pixel where a desktop sends one, in memory a phone
has less of, read through a cache a block format fits four times better.

## How mature engines do it

ASTC is the block format of every current phone GPU (Adreno, Mali, PowerVR,
Apple's), required by Android's own baseline for Vulkan devices; Unreal and
Unity build ASTC for Android by default. Basis Universal's UASTC exists to be
transcoded to it: its four-by-four ASTC target is the same sixteen bytes a
block BC7 is, and near lossless from UASTC.

## Decision

1. **ASTC, four by four, is a format the transcoder targets**
   (`asset::TextureFormat::Astc4x4Rgba`) and the device names
   (`rhi::TextureFormat::Astc4x4RgbaUnorm` and its sRGB twin): colour or data,
   with or without alpha, at eight bits a pixel -- what BC7 is to a desktop.

2. **BC7 first, then ASTC, then the rest.** A device that samples BC7 is sent
   BC7 as before; one that does not and samples ASTC is sent ASTC; one that
   samples neither is sent what it was.

3. **The device says what it samples, once** (`Capabilities::astcTextures`,
   asked of the GPU for both the linear and the sRGB format), and the engine
   says it to the transcoder before the first texture is read
   (`asset::setDeviceSamplesAstc`). A fact about the process, and so every
   caller's default (`TranscodeOptions::allowAstc`) rather than a thing each
   loader passes down: a mesh's textures, the interface's and the sky's all go
   to the one device.

4. Nothing changes where BC7 is sampled: no picture on a desktop moves.

## What it does not do

- The larger block sizes (six by six and up: less than half the memory
  again). The transcoder writes four by four only; a larger block is a second
  encode at build time, and a build's size.
- The terrain's layer arrays, which the engine draws itself into RGBA8 on
  every platform (they are small: 512 across).
- HDR textures: the sky's stay as they were.
- A device that samples neither format still takes RGBA.

## Consequences

- On a phone a compiled texture takes a quarter of the memory it took, on the
  GPU and on the way there.
- Checked here by a round trip through the transcoder (UASTC to ASTC blocks of
  the right size, written); no GPU on the development machine samples ASTC, so
  the first picture drawn from one is the phone's.
