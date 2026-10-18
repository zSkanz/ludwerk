# 0244 — A game runs in a headset through OpenXR, on SDL's GPU

- Status: accepted
- Date: 2026-10-17
- Builds on: ADR 0005 (the engine's RHI over SDL's GPU API), ADR 0037 (the
  RHI interface is frozen -- amended here, §7), ADR 0038 (visual fidelity)
- New dependencies: the OpenXR loader (Apache-2.0 OR MIT); a backport of
  SDL's OpenXR integration onto the pinned SDL (zlib), see §2

## The question

The maintainer opened virtual and augmented reality on 2026-10-17: a game made with
the engine should run in a headset -- first a Meta Quest 3 through Quest Link
or Air Link on a Windows PC, then standalone on the Quest -- and the same game
should run on a screen when there is no headset. Everything the engine draws
goes through its RHI on SDL's GPU API (D3D12 and Vulkan on Windows, Vulkan on
Linux and Android, Metal on Apple). A headset needs the frame drawn twice, from
two eyes the runtime places, into images the runtime owns, at the headset's
rate, on the GPU device the runtime chooses.

## Decision

### 1. OpenXR, and only OpenXR

OpenXR is the one API every runtime the maintainer's plan names speaks: Meta's for
Link and for the Quest itself, SteamVR, Android XR, and Monado on Linux. The
engine talks to no vendor's own API.

### 2. On SDL's own OpenXR integration, backported

SDL's GPU API gained an OpenXR integration in its development line (merged
2026-01-30, for SDL 3.6.0, which is not released: the newest release is
3.4.18, the one pinned). With it, SDL creates the GPU device on the adapter the
runtime asks for, with the instance and device extensions OpenXR needs, makes
the session (`SDL_CreateGPUXRSession`) and the swapchains
(`SDL_CreateGPUXRSwapchain`) as ordinary `SDL_GPUTexture`s, on D3D12, Vulkan
and Metal. **Driving OpenXR from the RHI without it is not possible on 3.4**:
a Vulkan device for OpenXR must be made through the runtime
(`XR_KHR_vulkan_enable2`) and a D3D12 one on the runtime's adapter, and SDL
3.4 has no way to be handed a device or to wrap an image it did not make.

So the integration is **backported onto 3.4.18 as a patch**
(`third_party/patches/sdl3/0007-gpu-openxr.patch`, R13): its 2,500 lines of
SDL code; the Khronos headers it carries come as they are. It is dropped when
a pinned SDL release carries the integration. SDL loads the OpenXR loader at
run time, so a game without XR never touches it.

**The loader** is Khronos's (`openxr_loader`, Apache-2.0 OR MIT), a pinned
release, vendored, built for Windows, Linux and Android, and shipped beside a
game that enables XR. On the Quest it finds Meta's runtime as it would any.

### 3. The frame: the headset's rate, the simulation's tick

With a headset, the frame loop is paced by the runtime: `xrWaitFrame` says
when to draw and for when (`predictedDisplayTime`), at the headset's rate --
72, 90 or 120 frames a second on a Quest 3. The simulation keeps its fixed 60
Hz tick and the frame draws between two ticks as it always has (`alpha`), so
a 90 Hz picture of a 60 Hz world is smooth and the world runs the same with or
without a headset (R10).

**The head is never interpolated.** The view of each eye comes from the
newest pose the runtime predicts for that frame's display time
(`xrLocateViews` with the time `xrWaitFrame` gave), read as late as the frame
allows before it is drawn -- never from the simulation's ticks and never
blended between two of them. Only the world and the player's body
interpolate between ticks; the head moves at the display's rate, so a 60 Hz
simulation seen at 90 Hz has a head that turns 90 times a second. A head
that followed the tick would lag by up to a tick and judder at every frame
that fell between two, which is the fastest way to make a player ill.

### 4. Two eyes: once with multiview on the Quest, twice where there is none

Each eye is a render pass into its swapchain image, with its own view and
projection (asymmetric, from the runtime's field of view). What does not
depend on the eye is done once a frame: culling against the union of the two
frusta, the shadow cascades (fitted to the head), animation, particles'
simulation, UI layout. The post chain runs per eye; the temporal effects that
keep history (TAA and the upscalers, ADRs 0158 and 0164) keep one history per eye, and the
default anti-aliasing in a headset is MSAA 4x, which needs none.

**On the Quest, the scene is drawn once into both eyes** (multiview,
`VK_KHR_multiview`): a mobile GPU drawing every draw call twice is the most
likely thing to miss 72 Hz, so multiview is part of the Quest build, not a
later step. The eyes are the two layers of one array texture -- which is what
OpenXR's swapchains already offer (`arraySize` 2) -- and each vertex shader
reads which eye it draws (`gl_ViewIndex`). SDL's GPU API has no multiview, so
**the SDL patch adds it**, on Vulkan first:

- a view mask on a render pass (`SDL_GPUColorTargetInfo` and the depth
  target name a layer count and `multiview = true`), made into Vulkan's
  `VkRenderPassMultiviewCreateInfo` (or the view mask of dynamic rendering);
- the `multiview` device feature asked for, and the device refused for XR
  without it;
- shaders compiled with the SPIR-V `MultiView` capability, which
  SDL_shadercross passes through from the engine's own shader sources (one
  `ENG_MULTIVIEW` variant of each forward and depth shader, the view and
  projection matrices an array of two indexed by the view).

On D3D12 (PC through Link) the eyes are drawn twice: a desktop GPU has the
headroom, and D3D12's view instancing is added only if the measurement on the
example scene shows it is needed. Either way the measurement on the Quest is
part of this work's proof (§Proof): 72 Hz held in the example scene, with
the GPU's time per frame reported.

### 5. What the runtime does, it does

The engine submits each eye's depth with its colour
(`XR_KHR_composition_layer_depth`), so the runtime's reprojection has it.
Foveated rendering (`XR_FB_foveation` on the Quest) and a render scale the
runtime recommends are used where offered; a game's own `RenderScale` applies
on top. A window on the desktop shows the left eye (the "mirror"), or nothing
(`[xr] mirror = false`).

### 6. Turning it on

`[xr] enabled = true` in `project.toml` makes a game try a headset at start
and run on the screen when there is none; `--xr` and `--no-xr` on the command
line override it. A game that is only VR says `[xr] required = true`, and
without a headset it says so and does not start.

### 7. The RHI, amended

ADR 0037 froze the RHI's interface. XR adds to it, and changes nothing that
is there: a device may be created for a runtime (`DeviceDesc::xr`), a frame
may begin when the runtime says (`beginExternalFrame`), and a render target
may be an image of a runtime's swapchain (`TextureHandle` from
`xrSwapchainImage`). A backend without XR (bgfx on the mobile path of ADR
0005, if it is ever built) answers that it has none.

## Proof

- **Quest 3 through Link on Windows** (the maintainer's to see; delivered as a
  package): the XR example at the headset's rate with no dropped frame in a
  minute's walk, on D3D12 and on Vulkan.
- The simulated headset (ADR 0248) in CI: a frame drawn into two swapchains,
  the two eyes' pictures differing by the eye separation, depth submitted;
  and drawn with multiview on Vulkan (lavapipe has the feature), the same
  pictures as two passes.
- **The head at the display's rate**: the simulated headset turning its head
  at a steady rate, presented at 90 Hz over a 60 Hz simulation: every frame's
  view is the pose of its own display time (no two frames with the same head,
  no frame with a head between two ticks'), while a part moving with the
  world steps by ticks and is drawn interpolated.
- **On the Quest 3, standalone, with multiview**: the example scene holds
  72 Hz for a minute, with the GPU's time per frame -- from the engine's own
  GPU timings by pass (ADR 0171) in the device's log, and from the headset's
  performance overlay, which the maintainer reads.
- The same game with no headset runs on the screen, and its determinism
  traces are unchanged by `[xr] enabled`.
- A game without `[xr]` loads no loader and links nothing new.

## Consequences

- New: `[xr]` in `project.toml`; `--xr`, `--no-xr`; the SDL patch; the
  OpenXR loader under `third_party/`.
- **The RHI interface changes** (ADR 0037 is amended, §7): a frame may be
  paced from outside, and drawn into a target an outside runtime owns.
- Two passes per eye cost the scene's draw calls twice until multiview; the
  measurement on the Quest decides when that is next.

## Not decided here

How the head and hands reach a game (ADR 0245), UI in a headset (ADR 0246),
passthrough and the world around the player (ADR 0247), the standalone
Quest build, multiplayer and the editor (ADR 0248).

## As built, first step (2026-10-17)

- **The device for a headset** (§2, §7): `DeviceDesc::xr` asks SDL for the
  device on the runtime's adapter; where that fails -- no runtime, no
  headset -- the device is made as before and `IDevice::headset()` is null.
  `rhi::IHeadset` is the frame (`beginFrame`, `endFrame`), the eyes' views
  (position, orientation, four tangents) and the eyes' images as
  `TextureHandle`s; no OpenXR type crosses it (R17).
- **Each eye is a part of the window** (ADR 0205's view): drawn into a
  picture of the eye's size with its own history, from the head's view moved
  to the eye (`render::eyeCamera`, an off-axis projection,
  `core::perspectiveOffAxis`), then copied into the runtime's image by
  `xr_eye.hlsl`, which decodes the picture first when the image is sRGB --
  the runtime reads one as linear light. The frame is gathered from the
  head, as wide as both eyes and a tenth more; the window shows the head's
  view.
- **The simulated headset** (ADR 0248 §4) came first, because it is what
  tests: `--xr=simulated`, headless or not, and with `--screenshot` each
  eye's picture beside the window's (`xr_simulated_eyes`).
- **Not yet**: the depth submitted with each eye (§5), the window showing
  the left eye and `[xr] mirror` / `required`, multiview (§4), the Android
  build. A minimised window draws no eyes. The first session on a real
  runtime is the maintainer's: a test never starts the machine's own.
