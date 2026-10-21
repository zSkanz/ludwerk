# 0257 — A material's pipelines are made off the frame and kept on disk

- Status: accepted
- Date: 2026-10-20
- Builds on: ADR 0091 (surface shaders), ADR 0125 (scenes prepared in the
  background), ADR 0176 (a pipeline that took long is said)

## The question

A surface shader is a material of the game's own, and the graphics card makes
its pipelines -- nine for each shader: the lit pass, its prepassed and
instanced forms, the blended pass, the shadow and depth passes -- the first
time something wearing it is drawn. That is the driver compiling the shader
for the card, and on a cold cache it is tenths of a second for a large one:
measured on an RTX 4070 Ti SUPER, 576 ms for one game's portal material and
469 ms for another, on the frame the material first came on screen in play.
The frame waited for all of it.

A game could name the shader in `ContentProvider:PreloadAsync` behind a
loading screen, but that made the same pipelines on the same thread: the
loading screen stopped instead.

## Decision

1. **The pipelines are made on a job.** The device holds the pipeline's slot
   and its description on its own thread, a job has the driver make the
   pipeline -- and its twins at an eye's pass, for a headset -- and the device
   takes what the job made into its tables when the renderer next asks
   (`IDevice::createGraphicsPipelineOffFrame`, `pipelineMade`). The tables
   every bind reads are never written from a worker. Direct3D 12, Vulkan and
   Metal all make a pipeline without state shared between calls: SDL's
   Direct3D 12 backend builds each pipeline's root signature for it, and its
   Vulkan backend locks the one cache a pipeline reads. A backend that cannot
   -- the recording and null devices, and a device with no job workers --
   makes it at once, as before.
2. **A part waits unseen, then is drawn plainly.** Until its material's
   pipelines are made, a part is drawn in no pass -- not its shadow, not its
   depth -- for up to `[graphics] material_wait_ms`, 250 by default. A
   wrong material for a few frames is a grey portal or a grey hero, which
   reads as broken; a part that appears a few frames late reads as nothing.
   Past the wait it is drawn with the built-in surface, and the log says once
   which shader and how to have it ready. `0` draws with the built-in surface
   at once.
3. **`PreloadAsync` waits for them.** A surface shader named in it is
   "loaded" once its pipelines are made, so a loading screen is a real
   pre-warm that stops nothing.
4. **Headless, they are made on the frame.** A headless run is evidence --
   a capture, a golden, a measured frame -- and what a frame shows has to be
   the same every run. `--pipelines=off-frame|on-frame` says otherwise.
5. **On disk, on Vulkan.** SDL's Vulkan backend made every pipeline with no
   `VkPipelineCache`. Patch 0011 gives it one, seeded from a file at device
   creation and written back when the device is destroyed, under the user's
   cache folder and keyed by the device and its driver version, so a second
   run -- the second session of a phone or a headset -- compiles nothing
   it compiled before. On Direct3D 12 the driver keeps its own compiled
   pipelines on disk across runs, as every desktop driver does; the API's
   own `ID3D12PipelineLibrary` is left for a measured need.

## Consequences

- A material first seen in play costs that frame nothing; its parts appear
  once their pipelines are made, a few frames later on a warm cache.
- A game that wants a material ready from its first frame names it in
  `PreloadAsync` behind its loading screen, which the manual says where it
  explains surface shaders.
- The built-in passes' pipelines are still made at start, behind the curtain
  the boot already draws; this is about what a game brings.
