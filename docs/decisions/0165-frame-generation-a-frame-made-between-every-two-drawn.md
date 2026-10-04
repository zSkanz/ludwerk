# 0165 — Frame generation: a frame made between every two drawn

- Status: accepted
- Date: 2026-10-04
- Decided by: the owner (2026-10-03, approving ADR 0158's batch: every
  upscaler and frame generator on the market that R6 allows); the design is
  the agent's, under the standing rule to decide as professional engines do
  and record it
- Builds on: ADR 0164 (FSR 2, whose item 11 left this out and said why),
  ADR 0158 (anti-aliasing and upscaling, whose second delivery this is the
  second half of), ADR 0116 (compute passes), ADR 0147 (the settings),
  ADR 0042 (vendoring narrowed by an include list)

## Context

A frame costs what the world costs to draw. Where that is 16 ms a window
shows sixty of them a second, and a display that could show a hundred and
twenty shows each one twice.

A frame generator makes the frame that would have been between two drawn
ones, from the two and from what the engine knows of how each pixel moved,
and the window shows it in between. Motion on the screen is twice as smooth
for a few milliseconds of GPU. Nothing is simulated or drawn for it, so what
the player does is seen no sooner -- later, in fact, by half a frame, since a
drawn frame waits for the one made before it to be shown first.

ADR 0164 left it out: it needs frames presented that were never rendered,
paced between the ones that were, with the interface drawn on each. That is
a decision about how the host presents, not about a pass.

## How mature engines do it

- **AMD's FSR 3**, the open one: an optical flow pass over the two finished
  pictures, and a frame interpolation pass that takes the game's motion
  vectors and depth with it, falls back to the optical flow where the game's
  vectors are not to be trusted, and paints what neither frame holds from a
  blurred pyramid. Its runtime replaces the swap chain with one of its own
  that presents the made frame and the drawn one from a pacing thread. The
  interface is given to it one of three ways: a callback that draws it on
  each frame, a texture of it alone, or the picture without it beside the
  picture with it.
- **Unreal and Unity** take FSR 3 or DLSS Frame Generation as a plugin under
  the upscaler's slot, off by default, a setting the player turns on; the
  interface is composed after the made frame and is not interpolated. Both
  vendors say the same of when: over a base of about sixty frames a second,
  with the display waited for or a variable-refresh display, and not as a way
  to make thirty feel like sixty.
- **Godot 4** has none.

All of them: a player's setting, off by default; the interface never
interpolated; the game's own motion and depth as the first source and optical
flow as the second.

## Decision

1. **`GraphicsService.FrameGeneration`**, a boolean, written
   `frame_generation = true` under `[display]` in a project's file and
   `--frame-generation` / `--no-frame-generation` on the command line. Off.

   **It is one of the display's settings, beside `VSync`, and not one of the
   level's.** It costs every input half a frame, and whether that is worth
   the smoothness is the game's to say and the player's, not a level of
   quality's: no level turns it on, choosing a level does not turn it off,
   `QualityLevel` does not read `Custom` for it, and a level from the command
   line, which takes a file's refinements of its own level away (D052),
   leaves it. It is how frames are shown, as the sync is -- which it holds
   on (item 6).

2. **AMD's source, not AMD's runtime**, as FSR 2's is (ADR 0164, item 3).
   The optical flow and frame interpolation techniques of the FidelityFX SDK
   1.1.4 are vendored as their shader headers alone (MIT;
   `third_party/fidelityfx_sdk`, narrowed by the include list): the last
   release in which frame generation is source. The runtime -- its Direct3D
   12 and Vulkan backends, and the swap chain that replaces the platform's --
   is not used: the engine presents through its RHI on every platform. What
   the runtime does besides, the order of the passes and the constants each
   is given, is written in the renderer after `ffx_opticalflow.cpp` and
   `ffx_frameinterpolation.cpp`.

   The two callbacks headers are derived from AMD's by a script
   (`tools/repo/fsr3_callbacks.py`), every change listed at the head of the
   file it writes (`shaders/include/engine_fsr3_of_callbacks.hlsli`,
   `engine_fsr3_fi_callbacks.hlsli`). The vendored copy is never edited. The
   changes that are decisions, beside FSR 2's sampler a texture:

   - **Integer images are thirty-two bits in one channel.** A luminance of
     eight bits, a pair of sixteen-bit motion components: AMD keeps each in
     the narrowest format, and those are stored to only with a Vulkan
     feature the RHI does not ask for. The luminance pyramids are
     `R32Uint`; a motion vector of the optical flow is its two components
     packed into one (`engPackFlow`). The price is memory, below.
   - **An image a pass only loads is said so.** An image of integers has no
     sampler. `TextureUsage::ComputeStorageRead` and
     `ICmdList::bindComputeStorageTextures` are the RHI's third kind of
     texture binding in a compute pass, after the sampled and the written.
   - **No wave intrinsics.** The search pass of the optical flow sums and
     takes minima over a wave of the GPU's lanes, whose width is the
     device's. The same reductions are done over the thread group through
     shared memory, sixty-four threads whatever the device: one answer on
     every GPU, and nothing asked of the shading language that Vulkan and
     Metal say differently.
   - **A pyramid is built a level a pass.** AMD's single-pass downsampler
     writes every mip of one image at once. A pass of the RHI writes
     eight textures, and a mip that is read while another of the same image
     is written is two states of one resource. Each level is written to an
     image of its own, which the next level reads, and to its mip of the
     pyramid the later passes sample.
   - **Inpainting reads one image and writes another.** AMD's reads and
     writes the output in place.
   - **The two depths are set to the furthest by the pass that empties the
     fields**, not cleared from outside: a clear to a float's bits is not
     one every backend does to an image of integers the same way.
   - **The atomics are atomics, and where there are none there is no frame
     generation.** The game's motion as seen from the frame between is kept
     by an atomic maximum over a vector packed under its priority, and the
     depths by an atomic minimum. FSR 2 on Metal does its one minimum
     without the atomic and misses a disocclusion for a frame (ADR 0164).
     Here a lost write is a wrong vector for a pixel, drawn. **On Metal the
     setting is refused**, said once
     (`render.warn.frame_generation_metal`), and frames are shown as drawn.
     Its shaders are still compiled there, so the build is one build.

3. **What it is made from.** The two finished pictures of the world -- after
   the tonemap, the anti-aliasing and the upscale, in the window's format,
   **without the interface** -- and the depth and the velocity of the newer
   one, at the resolution the world was rendered at. The velocity is the
   target TAA and FSR 2 read, written on a frame-generated frame whatever
   smooths it. It runs under any `AntiAliasing` and any `Upscaling`.

   The host draws the world into a picture of its own where it would have
   drawn the window, two of them by turns, so the last frame's is still
   there when this one's is done.

4. **The interface is drawn on every frame shown and is never
   interpolated.** The made frame and the drawn frame are each copied to the
   window and the interface drawn over them: text does not smear, and a
   health bar is where the game put it. On the made frame it is the
   interface as the frame after it has it.

5. **The made frame is shown first and the drawn one half a frame later.**
   With a window, the frame's commands end by showing the made frame; the
   drawn one is shown when the next frame's drawing begins, with the
   interface it had. Without a window there is nothing to pace: both are
   shown one after the other, and each is a frame to `--screenshot-every`.

6. **The display is waited for while frames are generated, whatever `VSync`
   says.** The two frames are sent one behind the other on the one queue a
   device has, and it is the display's wait that gives each its turn. With
   VSync off the made frame would be on the screen for no time at all, and a
   sleep of half a frame on the thread that draws is half the frames gone.
   The setting is held on while `FrameGeneration` is on and put back when it
   is turned off. `MaxFrameRate` still caps the frames DRAWN.

7. **Where no frame is made, the frame is shown as drawn**:
   - a device without compute shaders, or without the memory for the images
     (`render.warn.frame_generation_unavailable`, once), and Metal;
   - a camera without perspective, a picture of sprites alone and a picture
     with nothing behind it -- FSR 2's reasons;
   - the first frame, and the frame after the camera was cut (it moved
     further than anything walks in a frame): there is nothing to be between;
   - behind the loading curtain;
   - in the editor, whose world is a panel, and under the debug overlay,
     which is drawn once a frame.

   When a run ends the log says how many were made
   (`engine.frame.info.made`).

8. **`--debug-view=motion`** draws each pixel's motion in place of the
   picture: red and green along x and y, mid grey for none, a channel's
   whole range for sixteen pixels either way, and blue where the pixel moved
   at all. A test instrument, like the other views. It is how D548 was found
   and what its gate reads.

## Consequences

- **Latency.** A drawn frame is seen half a frame later than without, plus
  the time the made frame takes. At sixty drawn frames a second that is
  about nine milliseconds. The manual says so, and says what the vendors
  say: turn it on over sixty, not under.

- **Memory.** Thirty-two bytes a pixel of the window and forty-five a pixel
  rendered, the host's two pictures included:

  | Window | At a scale of 1 | With FSR 2 at 0.67 |
  |---|---|---|
  | 1920 x 1080 | 161 MB | 109 MB |
  | 2560 x 1440 | 286 MB | 194 MB |
  | 3840 x 2160 | 643 MB | 436 MB |

  A seventh of it is the luminance pyramids, at thirty-two bits where AMD's
  are eight. The images are released the frame the setting is turned off.

- **Time**, on a GeForce RTX 4070 Ti SUPER through Direct3D 12, the shipping
  build, what making a frame adds to a drawn one:

  | Window | At a scale of 1 | With FSR 2 at 0.67 |
  |---|---|---|
  | 1920 x 1080 | 1.2 ms | 0.9 |
  | 2560 x 1440 | 2.0 ms | 1.4 |
  | 3840 x 2160 | 4.3 ms | 2.9 |

  The least of three runs' medians, the frame with it less the same frame
  without, both shown without a window: the eighteen passes, the velocity
  where nothing else wrote it, the two pictures copied to what is shown and
  the interface drawn a second time. A device whose compute is its CPU
  -- CI's -- pays seconds.

- **What it gets wrong.** The pixels a moving thing uncovers are in neither
  frame where the frame between needs them; they are painted from a blur of
  what is round them, and at the leading and trailing edge of something fast
  that is a soft fringe a pixel or two wide for one frame. Something that
  blends -- a flame, glass -- has the motion of what is behind it and is
  carried by the optical flow alone. A shadow moves with the surface it is
  on, not with what casts it.

- **D548, found here and not of it.** The made frames had thin diagonal
  streaks across a moving box. They were not the generator's: the velocity
  the engine handed it was the wall's on half the box's face. `motion.hlsl`
  placed a vertex by one matrix, the camera's times the model's, multiplied
  on the CPU; the depth it is tested against was written by a pass that
  multiplies the vertex by the model and then by the camera. Two roundings
  of one depth, and the moving thing lost to itself wherever its own was the
  further by a bit. TAA and FSR 2 had been reprojecting those pixels by the
  background's motion since ADR 0158. The motion pass is now given the two
  matrices and multiplies as the depth pass does.

- **The wire is untouched**, and so is every golden: the setting is off
  unless asked for, and a frame without it issues the commands it always did.

- Tests:
  - `frame_generation_gate`: a scene whose every motion the game knows -- a
    sliding camera, a box crossing, a bar turning -- drawn with frame
    generation at two steps of time a frame, and again without at one, so
    each made frame has a frame DRAWN at its instant to be held against.
    Over ten made frames the grossly wrong pixels must be fewer than three
    fifths of those of the nearer drawn neighbour, which is what the window
    would have shown; and each frame drawn in the first run must be the
    frame drawn at that step in the second. Measured here: 3569
    against 10 657, summed over ten frames of 230 400 pixels. On Windows it
    then runs ninety frames with a window, which must show at least sixty
    made ones: a window's made frames take another path.
  - `motion_whole_gate` (D548): a slab crossing a wall under a sliding
    camera, in `--debug-view=motion`; every pixel of the middle of the slab
    is the slab's motion. Before the fix 13 087 of 33 864 were the wall's.
  - the RHI's: the third usage is apart from the other two, and the textures
    a pass loads are recorded by the capture device, each by its handle.
  - settings: `frame_generation` from a project's `[display]` and not from
    its `[graphics]`, the flag over it, no level turning it on or reading
    `Custom` for it, and `GraphicsService.FrameGeneration` from a script.
  - by hand, and not a gate: `FrameGeneration`, `Upscaling`, `RenderScale`
    and `AntiAliasing` changed every few frames under Direct3D's debug
    layer, with and without a window, without a message from it.

## Not decided here

- **Pacing on a display that is not waited for**, and on one with a variable
  refresh rate: a thread of its own that presents, as AMD's swap chain has.
  The RHI has one queue and presents from the thread that draws.
- **Less latency**: a late read of the input, or the vendors' own
  (Anti-Lag, Reflex), which are not source.
- **Metal.** Its shading language has atomics on an image from version 3.1;
  the day the shader toolchain writes that, the refusal is one line to drop.
- **HDR output.** The pictures it reads are the window's, eight bits.
- **FSR 3's own upscaler**, which the SDK also carries: FSR 2 stays the
  temporal upscaler (ADR 0164).
- **DLSS Frame Generation and XeSS-FG** stay out, against R6.
