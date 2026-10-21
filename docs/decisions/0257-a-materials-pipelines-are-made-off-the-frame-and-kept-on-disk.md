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

   **As built, not on the job system.** A job of the general pool waited
   behind whatever the world had queued there -- streaming, foliage, a
   texture's transcode -- and its compile of half a second held a worker the
   world needed. The device has threads of its own for its pipelines, first
   asked first made: `[graphics] pipeline_threads`, one on a machine of four
   hardware threads or fewer and two above, four at most
   (`DeviceDesc::pipelineThreads`). The keeping of item 5 runs there too.
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
   `VkPipelineCache`. Patch 0011 gives it one, seeded from a file in the
   user's folder at device creation, so a second run -- the second session
   of a phone or a headset -- compiles nothing it compiled before. **Kept as
   the run goes** (patch 0012, `SDL_SaveGPUPipelineCache`), because a phone,
   a headset or a player with a task manager ends a game without a word and
   a device destroyed is the one moment that never comes there: five seconds
   after the last new pipeline, on a pipeline thread; at once when the app goes to the
   background or loses focus, and when a headset stops running. Written
   beside the file and moved over it, so a kill mid-write leaves the last
   whole copy. The file carries a header of its own -- a mark, its size and a
   CRC -- and is read only whole and only when Vulkan's own header names this
   device and this driver's cache; anything else starts empty, and the log
   says how many bytes were read. On Direct3D 12 the driver keeps its own compiled
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
