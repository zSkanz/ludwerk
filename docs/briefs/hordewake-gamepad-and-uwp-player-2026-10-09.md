# Hordewake gamepad and UWP player continuation

Last updated: 2026-10-09. Hordewake controller implementation and Windows/Android
exports are delivered in 0.8.49. The full native UWP/Xbox player now runs the
real game on the owner's Series S; physical entry into a match is confirmed.

The owner subsequently authorized the real player port. Native D3D12 queue,
CoreWindow presentation and shipping DXIL shader execution now work on hardware;
resource ownership/transfers also have native tests. Continuation checkpoint:
[native player port](xbox-native-player-port-2026-10-09.md). Full runtime integration
and hardware fixes supersede the historical assessment below. Follow that
handoff for modal input fixes, v0.8.49.5 deployment and startup-loss investigation.

Owner authorization: complete remaining work; Hordewake should support a gamepad
on PC, Android and Xbox without choosing controls from the operating system.
Persist the implementation, evidence and outstanding work for another agent.

## Completed packaging checkpoint

D601 held-modal-button suppression and D602 independent stick filtering are in
both final exports. Their real-host regressions failed before the fixes and
passed afterward. Latest source analysis and editor menu/run checks passed.
Final sealed Windows menu and gameplay each ran 120 frames without errors.
APK/Windows archive CRCs, Android manifest and artifact/source hashes were
verified and refreshed. No temporary gameplay instrumentation remains.
Full UWP player and physical PC/Android controller validation remain pending.
Do not run exports against the shared Gradle cache concurrently.

## Baseline and ownership

Game checkout: `C:/Users/juanr/Documents/hordewake`. Both game and engine contain
substantial pre-existing dirty work. Preserve it; no reset/commit/push/publish.
New release version: project.toml and GameSettings.Version 0.8.49; Android
version_code 59. Existing releases in Downloads are preserved.

The previous UWP probe works on the owner's Xbox, including physical gamepad,
B reset, reconnect and saved-position cold relaunch. It is not the engine player.
Previous full gates: Windows 138/138, Linux 145/145, conformance 1642/1642 on both,
hot reload 3/3, Android APK/AAB tests 2/2. These predate the game changes below.
External SDK path/detection UI remains deferred by the owner.

## Implemented

- [x] Scoped InputContext/InputAction/InputBinding gameplay actions in the new
  `src/client/Modules/Controls.module.luau`; keyboard bindings remain supported.
- [x] Keyboard and left-stick movement use separate actions; filter the stick
  before combining it with keyboard/touch, independent of preferred input (D602).
- [x] Left-stick analogue movement; radial 0.18 deadzone rescaled to the remaining
  range, normalized diagonals, no normalization of partial deflection to full speed.
- [x] Right-stick camera; radians per second times frame dt and player sensitivity,
  pitch clamps and large-frame cap. Mouse/touch camera paths remain available.
- [x] Lower face button jump, right face button slide, left face button interact;
  upper face button or Back/View toggles statistics; Start/Menu pauses; right
  face button closes pause/stats and returns through menu/lobby; shoulder buttons
  cycle spectated heroes. No gamepad-to-keyboard event injection.
- [x] Held confirm/back/interact buttons cannot become gameplay on entering or
  resuming a scene: suppress held Bool actions until actual release, including
  raw bindings hidden by UI consumption. Movement resumes without that suppression
  (D601; real-host test failed before and passed after the fix).
- [x] Gameplay stops reading movement/camera/actions behind pause/cards/summary/stats.
  AutoSelect is disabled and SelectedObject cleared during gameplay so movement
  and jump cannot navigate/activate HUD buttons. Modal opening clears stale focus.
- [x] Existing UIService navigates and activates buttons/cards. Gold focus images
  match the game's style using SelectionImageObject; chosen hero/tier styling is
  preserved after focus leaves. Tabs have visible focus too.
- [x] One-press gamepad card confirmation, including compact phone layout; touch
  retains its deliberate preview/second-press interaction and existing purchase
  affordability/grace/serial protections.
- [x] Phone touch UI follows preferred input; initialize its layout even when a
  controller is preferred at boot. Clear fingers, held stick and touch button
  state on controller use so switching back cannot resume stale input.
  With no controllers connected the touch UI returns immediately, even before a
  new touch updates preference. UiKit.Touch() remains a screen/layout question;
  UsingTouch() is input preference plus hardware availability.
- [x] Localized controller reference and interaction/statistics prompts in English
  and pt-BR. Physical-position captions avoid assuming platform or glyph legends.
- [x] Engine automatic UI navigation stays in the highest selectable display
  layer; equal-order screens share navigation, decorative/hidden overlays do not
  steal it. Explicit selection links remain available (ADR 0192).
- [x] CLI check batches analysis commands under Windows' spawn-length limit.
  Checking this checkout exposed the failure with 839 files; regression tests
  preserve prefix/definitions/order and account for every file.
- [x] Windows and Android players rebuilt and playable exports produced.
- [x] Full UWP dependency boundary audited and recorded below.
- [ ] Physical controller gameplay validation on PC and Android. No adb device
  is connected in this session. Xbox owner validation belongs to the probe only.
- [ ] Full UWP player/RHI port; real engine scene then Hordewake on Xbox.

## Validation for this continuation

- Windows native UI: 158/158 cases, 71636 assertions. Native input: 47/47 cases,
  928 assertions. Linux rebuilt the same targets and passed the same counts.
  New tests cover modal focus boundaries, peer screens, disabled/hidden overlays,
  and releasing gamepad actions plus both sticks on disconnect or focus loss.
- CLI tests: 67/67, including the large-game analysis spawn regression.
- Documentation gate passed (latest run: 119.8 s); changed native C++ uses pinned clang-format 18.
- luau-lsp 1.69.0 with `--flag:LuauSolverV2=true`: all game `src` passed, using
  refreshed generated engine definitions. Editor checker: menu 97 scripts and
  run 96 scripts, zero errors and zero warnings.
- Changed gameplay files pass the project's pinned StyLua settings. A whole-src
  formatting run still reports pre-existing formatting in ClientLoader,
  CharacterSettings, UnlockSettings and server WorldController. These are not
  functional/type errors and unrelated source was preserved.
- Full generic CLI check now gets past the Windows argument-length failure but
  stops on unused RunService variables in archived `marketing/steam-0.8.36`
  capture copies. Shipping source checks above pass; do not report the entire
  checkout CLI check as green or silently edit those archived captures.
- `tools/check_gamepad.py --host <dev engine-host>` passes in the real game:
  drift/deadzone, analogue range/diagonals, actual hero movement through Input
  Actions, camera, pause/resume, release, scoped context cleanup and visible focus.
  Uses explicit virtual channels, **not physical hardware emulation**. Tests use
  isolated save directories. Temporary instrumentation is restored byte-for-byte
  in a finally block.
- `tools/checkup_game.py --host <dev engine-host>` passes desktop and compact
  cards, including direct controller confirmation, purchase guards, duplicate
  choice prevention, scope/profiler lifetime, music/mute cleanup, and 600-enemy
  plus boss stress. Screenshots/logs: game `prints/gamepad` and `prints/checkup`.
- Headless gameplay opened/rendered 180 frames without errors. Windows packaged
  player opened/rendered menu and gameplay from its sealed pack (120 frames each). Focus screenshot inspected
  after replacing the engine's default blue outline with the game's gold.
- Windows/Android export tools compile game scripts, pack assets, stamp metadata
  and verify icons/signatures. APK is a local development build, not a store
  publication/release signing claim. Physical Android controller test not run.

Logs: `%TEMP%/hordewake-gamepad-{native-tests,linux-tests,cli-tests,lsp,
editor-check,editor-run-check,smoke,export-windows,export-android,docs}.log`.
Current artifact details are recorded beside the Downloads files after final
packaging. Never overwrite an older release or include private signing keys.

## UWP full-player boundary and next steps

This task does not turn a PC EXE or the existing GDK PC provider into an Xbox
application. The deployed probe is isolated before the normal CMake graph and
reuses Luau/math/i18n only, with its own D3D11 circle and direct WinRT input.

Concrete dependencies found in the current engine:

1. `engine/platform/CMakeLists.txt` always links SDL3-static; window/events,
   clipboard, filesystem/process and crash helpers contain desktop assumptions.
   Vendored SDL3 documents that UWP/WinRT support was dropped. Do not merely set
   WindowsStore on the normal target or claim SDL3 provides the missing backend.
2. `engine/rhi` has SDL GPU as its only graphical backend (plus Null/capture).
   The probe's D3D11 drawing is not an implementation of IDevice/ICmdList. A real
   backend must cover the game's rendering passes, shader formats, compute,
   resource bindings, uploads, barriers, textures, readback and swapchain lifecycle.
3. `engine/app` owns the SDL window/event loop, UI navigation and graphics host;
   player profile removing ImGui does not remove SDL. CoreWindow host integration
   and suspend/resume ownership must be provided without forking gameplay logic.
4. Assets/caches/saves need Package.InstalledLocation and ApplicationData.LocalFolder
   plumbing; audio, networking capabilities, jobs and legal AppContainer APIs need
   verification. The probe's save/log support validates only that isolated host.
5. Gamepad platform events should come from Windows.Gaming.Input into the existing
   InputSystem/UiNavigation, carrying stable connection IDs, face positions,
   analogue axes, disconnect/focus release and preferred-device metadata. Consume
   UWP system BackRequested appropriately: B must not close the application.
6. Only after a real engine scene works on hardware should an optional UWP
   player/export module produce a validated APPX with the actual game's pack,
   declare capabilities/dependencies and use the existing Device Portal workflow.

Recommended sequence: implement/test a real CoreWindow-capable RHI surface and
backend; decouple the SDL host boundary; bridge input and lifecycle; validate
asset/audio/network/saves in an engine scene; package Hordewake; run physical
menu -> lobby -> run -> cards -> pause -> reconnect -> suspend/resume checks.
Keep the default Windows/Linux/Android build and PC GDK integration independent.
Do not add mock Xbox providers, bundle restricted SDKs or rename the probe to
Hordewake. Console URL/access evidence remains in the separate probe checkpoint.


## Packaging follow-up found during final artifact inspection

D599: successive APK exports grew from 92159067 to 172281507 bytes although
ZIP-listed compressed payload was only 92078476 bytes in 428 entries. Inspection
located an 80084522-byte unlisted gap left by incremental APK packaging. The CLI
now removes only its cached final APK before Gradle runs, forcing fresh packaging
without discarding Java/resource caches. Real Android export tests passed 2/2 (debug, release, AAB). Repeated game exports
are 92161468 and 92161473 bytes, a five-byte difference; ZIP overhead is 82992
bytes and all archive CRCs passed. The flat Downloads APK has been replaced
with the fixed artifact. D599 is closed.
D598 records the analysis spawn fix; D600 records modal navigation.


## Final Downloads artifacts

- `C:\Users\juanr\Downloads\Hordewake-0.8.49.apk`: 92163008 bytes; SHA-256 `c8922168383b6e6c57616af27abed0f76b14291d9d708acc12a29c96ffa01a5b`.
- `C:\Users\juanr\Downloads\Hordewake-0.8.49-windows.zip`: 98439572 bytes; SHA-256 `e32251b49e6440f815920fce25dc5aa1c411a0f2ec9deb5206d6bbf9c7eb27c3`.
- `C:/Users/juanr/Downloads/Hordewake-0.8.49-validation.json`: hashes, package metadata, tests and limits.
- Android manifest read back: versionName 0.8.49, versionCode 59.
