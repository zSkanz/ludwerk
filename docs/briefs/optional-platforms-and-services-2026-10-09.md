# Optional platforms, game services and gamepad identification

Last updated: 2026-10-09. Status: **platforms, input and six generic services implemented; failed checks corrected and local gates passed. External SDK discovery UI remains proposed.**.
Latest performance checkpoint: [implemented optimization batch](hordewake-profile-optimization-2026-10-09.md)
and [repeat physical measurements](hordewake-profile-optimization-results-2026-10-09.md).
The owner authorized a [second optimization batch](hordewake-profile-optimization-batch2-2026-10-09.md);
follow that handoff for current hierarchy/animation work and its validation status.
Latest active work: [batch 3 cold-loading investigation](hordewake-profile-optimization-batch3-2026-10-09.md).
Fresh normal Windows/Android/Xbox builds are in Downloads. Xbox v10 relaunch is
stable at the menu, but its first-start device-loss and cold-loading stall remain
open; follow the batch handoff for logs and exact validation limits.
The owner deferred external SDK UI and authorized the Xbox Developer Mode probe.
See [the hardware checkpoint](xbox-uwp-probe-2026-10-09.md): native UWP graphics,
pinned sandboxed Luau and physical controller work on the owner's Xbox. B reset
was fixed and confirmed by the owner. The subsequent full native UWP player now
runs Hordewake on Series S; the owner confirmed entering gameplay. Follow
[the current player handoff](xbox-native-player-port-2026-10-09.md) for the latest
build, hardware evidence and pending modal-input / first-use shader fixes.
PC GDK services remain a separate integration.
Owner explicitly authorized implementation, asked to preserve capabilities,
performance and existing coding style, then requested this Markdown handoff.

## Workspace and safeguards

- Engine: `D:/Projects/Ludwerk` (PowerShell on Windows).
- Game: `C:/Users/juanr/Documents/hordewake`. Current released game: 0.8.48;
  this engine task does not currently edit the game.
- Preserve pre-existing dirty changes. Before this task they included branding,
  editor SVGs, content_import.cpp/tests, assetc compiler.cpp/header, architecture,
  CHANGELOG, CLAUDE, PROGRESS and other assets. Do not reset the checkout.
- Read CLAUDE.md and MASTER_PROMPT.md; follow existing API schema/codegen, module
  layers, strict Luau/new solver, localization, pinned dependencies, deferred signals,
  sandbox and out-of-tree builds. Do not edit third_party contents.
- Developer instructions prohibit spawning agents unless explicitly authorized
  by the user or applicable AGENTS/skill. No subagents have been spawned.
- Do not commit/push/publish unless requested. No new engine release delivered yet.

## Owner-approved architecture

1. Extend the existing export system; do not create a second pipeline.
2. Windows and Linux precompiled players included by default. Android tooling
   optional. Microsoft GDK PC optional. Xbox console requires actual GDKX access,
   engine backend/lifecycle work and validation; never mark Windows as console support.
3. Installation on the machine and enablement in a project are independent.
   Writing/checking scripts must not require an SDK.
4. Generic platform/module registry, dependency ordering, pinned versions,
   downloads verified by hash, shared tools, install/verify/remove/update UI,
   reuse existing tools without deleting external tools. Disabled exports do not
   carry GDK native libraries. Restricted SDKs remain outside public source.
5. Start with XboxService. Do not duplicate MicrosoftService. AndroidService
   is not needed for APK export. Future GooglePlayService and StoreService should
   be added with actual functionality, not exposed as working integrations now.
6. **GetService continues to work when disabled**, without requiring FindService.
   Queries return false/nil. Actions return an explicit failure code, never success.
   Inactive async calls finish immediately. Development warnings are once per
   service/VM; editor diagnoses inactive references and offers enablement.
   Hide inactive services from new autocomplete suggestions and Explorer while
   recognizing existing references and their types.
7. Native SDK adapter isolated in an optional dynamically loaded provider. No
   SDK dependency, SDK threads or background requests in default exports.

## Current implementation (supersedes old intermediate checkpoints)

- ADR 0188 records optional modules/services. ADR 0189 records input preference.
- XboxService has IsAvailable, IsSignedIn, GetUserId, GetGamertag, SignInAsync,
  UnlockAchievementAsync. GetService works disabled. Queries return false/nil;
  actions fail immediately with IntegrationDisabled. Development warning once.
- Schema Class.Integration metadata feeds generated C++/Luau/API docs. Disabled
  optional services are hidden from Explorer and new service suggestions; AST
  diagnostics warn on direct game:GetService references without rejecting types.
- ProjectConfig -> EngineOptions -> WorldHostOptions uses generic enabled IDs
  and integrations.<id>.configuration strings. The original xboxScid/scid proposal
  was replaced; Xbox's configuration value is its SCID. Unsafe IDs are rejected.
- The SDK-free native ABI v1 uses safe absolute-path DLL loading on Windows
  (DLL_LOAD_DIR/DEFAULT_DIRS), portable SDL fallback elsewhere. Optional providers
  are lazy; a default player links no GDK libraries. Native load/lifecycle tests pass.
- Optional GDK adapter in platforms/xbox compiles against pinned SDK. Actual
  XUser sign-in and XSAPI achievement completion use async polling/cancellation;
  global SDK refcount and default XSAPI queue protect lifecycle. No real title
  authentication or achievement call was exercised without configured credentials.
- CLI modules registry/manager integrates the existing exporter. Windows/Linux
  prebuilt players are base; Android tooling/GDK opt-in. Machine install is separate
  from project enablement. Downloads have pinned hashes, required files have an
  integrity inventory, removal requires exact version/ownership and managed path.
- Editor Platforms and Services window reuses CLI process polling: SDK install,
  repair, verify, owned remove, project toggle/config and prebuilt ZIP path/SHA256.
  Native provider readiness is separate from SDK status. Reopen project to apply
  runtime enablement. UI compiles; visual pixels have not been inspected.
- scripts/package-gdk.ps1 builds a separate prebuilt module ZIP with ABI/version
  metadata and file hashes. Installs without a C++ compiler or SDK. No remote
  release artifact has been published. Base engine packaging remains SDK-free.
- Export preflight requires the enabled module/provider, configuration and authored
  MicrosoftGame.config with matching executable. It deploys only engine_xbox.dll,
  xgameruntime.dll, libHttpClient.dll and GDK-LICENSE.md. No SDK headers/libs copied.
- Integrity verification now checks an installed native runtime even when the SDK
  is also installed; that last enhancement was analyzed/formatted, needs gate coverage.

## InputService implementation

- Existing public API was LastInputDeviceType/InputDeviceChanged. PreferredInput
  is an alias using the EXISTING Enum.InputDeviceType (KeyboardMouse/Gamepad/Touch).
  Reuse InputDeviceChanged; no redundant PreferredInputChanged event.
- PreferredGamepadType uses engine Enum.GamepadType Unknown/Xbox/PlayStation/
  Nintendo/Generic; PreferredGamepadId uses SDL instance ID with zero = none.
- SDL_GetRealGamepadTypeForID maps physical family inside platform; no SDL enums
  in Luau and no rigid association with glyph layout or operating system.
- Connection does not select preference. Button down/axis above existing threshold
  selects actual pad. Keyboard/mouse activity changes category, preserving records.
  Real mouse motion now selects KeyboardMouse; zero delta does not.
- Per-pad button/axis state prevents unplugging one pad from clearing another's
  held input. Focus loss clears per-pad input too. Strongest axis and OR buttons
  preserve the existing aggregate actions model; no player assignment introduced.
- Added type/ID and connection/disconnection events using existing deferred signals.
  InputDeviceChanged was declared but not wired previously; it now fires.
- No glyph assets or GetInputIcon method added. Existing binding data is retained.
- Updated schema/generated types/reference/dump and manual/input/rebinding.md.

## Verified results and pending checks

PASSED before generic-services changes:
- Full Windows gate: 566.3 s; full Linux gate: 764.5 s. Conformance 1642/1642.
- Luau gate after input schema: 247.1 s, CLI 66/66, 382 files no diagnostics.
- Android build/export gate after input: 311.9 s, debug/release tests 2/2, APK/AAB.
- Input C++ executable: 45 cases, 853 assertions passed.
- Module tests: 4/4. SDK install/verify, intentional extracted-header corruption
  rejected, repair restored verification; owned removal passed in isolated root.
- Prebuilt ZIP install/verify passed with no sdk/ directory. Enabled Windows export
  copied exactly the declared native files and MicrosoftGame.config.
- Disabled Windows export (rebuilt player) carried no integrations directory;
  launched and printed OPTIONAL_INTEGRATION_DISABLED_OK; explicit log checked.
- Real GDK DLL safely loaded through ctypes; ABI v1/unconfigured create/destroy.

HISTORICAL FAILURES (resolved; retained for diagnosis):
- Initial Input Luau signal test wrote readonly _G; corrected to DataModel attributes.
  The corrected test passed in the full 185/185 script run, then later 190/190 runs.
- An intermediate Linux compile read a header during formatting and reported
  transient NUL bytes. Current files contain no NULs. Do not format during builds.
  The final Linux gate is compiling stable sources with warnings as errors.
- The first generic identity Auto selector had a dangling string_view; tests
  exposed it and it was fixed. Full script suite then passed.
- The first final Luau gate rejected a raw Auto identifier inside an ImGui preview
  expression. Localized preview moved to a variable; direct i18n lint now passes.
  Final Luau rerun passed (359.9 s).
- Real title/account, Microsoft submission/MakePkg, Xbox console, hardware gamepad
  checks, macOS and visual UI pixels remain explicitly UNVERIFIED.

## New authorized scope: generic platform services

Read [architecture/phase results](generic-platform-services-2026-10-09.md) and
[verbatim requirements](generic-platform-services-owner-request-2026-10-09.md).
ADR 0190 records the accepted design. No fake Steam/PlayStation/Nintendo providers.

IMPLEMENTED:
- IdentityService, AchievementService, StoreService, CloudSaveService,
  LeaderboardService and SocialService schemas, bindings and generated reference.
- Reuse the existing native map, Busy state, coroutine waiter/resumeScheduled.
  Specific XboxService and generic services share the SAME authentication session.
- Native ABI v1 retained; optional engineGameIntegrationCapabilities extension
  declares operations/JSON responses. Old DLLs remain valid for XboxService.
- Xbox REAL capabilities: Identity, SignIn, UnlockAchievement. All other generic
  features, including progress, are unavailable until real GDK implementations exist.
- Identity is readonly, provider-namespaced, platform-neutral data; no universal XUID.
- Achievement/product/leaderboard IDs resolve per project/provider. Auto or explicit
  provider selection is configured through platform_services.<service>.provider.
- Cloud slots remain independent of local saves. Social IDs preserve provider
  namespaces; failures never imply permission. JSON responses are bounded and
  shape-checked, including false-versus-error and optional null fields.
- Platform manager now has six provider selectors (Auto/actual registry entries)
  and per-provider achievement/store/leaderboard ID map add/remove controls.
- API generation: 148 classes, 87 enums, 29 datatypes, 1942 API members; 285 bound methods.
- Guide: docs/manual/guides/platform-services.md, linked from shipping.
- Normal scene load retains the VM/provider. Actual two-scene survival/destruction
  test in world_host_tests.cpp passed in the final Windows app gate.
- Configuration tests cover Auto/explicit providers and independent provider maps.

PHASE RESULTS (GDK-free Windows test binaries):
- Identity: script 185/185, platform 56/56 passed.
- Achievements: script 186/186 passed; Store: script 187/187 passed.
- Cloud: script 188/188 passed; Leaderboard: 189/189 passed; Social: 190/190 passed.
- Tests use INTERNAL native test providers only, absent from the module catalog.
- Rebuilt real GDK provider and prebuilt ZIP. Safe ctypes ABI v1+capability extension
  load/lifecycle passed; asserted only the three implemented operations supported.
- Reinstalled/verified updated native ZIP in isolated tools root, with no SDK folder.

FIRST INTEGRATED ATTEMPT (historical; superseded by correction results below):
- %TEMP%/ludwerk-generic-final-windows.log (full engine/tests/player/export gate).
- %TEMP%/ludwerk-generic-final-linux.log (full Tier-2 gate).
- %TEMP%/ludwerk-generic-final-android.log (player cross-build and exports).
- %TEMP%/ludwerk-generic-final-luau-fixed.log (full lint/generation/CLI gate).
- This attempt exposed determinism/navigation failures; correction results below
  supersede these original logs.
  Repeat disabled final exported-player smoke with all six services, enabled export
  payload inventory, and affected editor build after the localized preview fix.
  Run format check on changed owned files without touching unrelated dirty work.

## SDK and local paths

- SDK pin 2604.5.7903, native root native/260405/windows, archive 141346554 bytes.
- Archive SHA256: 6679f5ae3f1ed873676f9ad363eed4fd754ffcf795628490e916ca8a9058a4e3.
- Research SDK outside repo: C:/Users/juanr/AppData/Local/engine/toolchains/gdk/2604.5.7903/
- NuGet does NOT contain MakePkg. No fabricated title IDs/credentials.
- Manager shared root ENG_TOOLS_ROOT or ~/.android/engine, then modules/ID/version.
- Isolated module validation root: %LOCALAPPDATA%/engine/platform-module-validation.
- Export fixture: %LOCALAPPDATA%/engine/platform-export-validation; currently disabled.
- Builds: %LOCALAPPDATA%/Ludwerk/build/win-msvc-dev, win-msvc-player, win-msvc-gdk.
- Lute .rokit/tool-storage/luau-lang/lute/1.0.0/lute.exe; LSP
  johnnymorganz/luau-lsp/1.69.0; StyLua johnnymorganz/stylua/2.5.2.
- Use clang-format-18 in engine-tier2 Docker image; do not format unrelated dirty files.
- Generate api/generator/{check,gen_cpp,gen_dts,gen_dump,gen_reference}.luau.
- Docker Desktop running; engine-tier2-build volume used by localgate Linux.

## Earlier Hordewake requests: preserve context, re-audit before claiming completion

These requests were made earlier in this conversation. They are not evidence
that each was implemented; consult the game repository's own ledger and delivered
builds before reopening or claiming any item complete.

- Compare the engine with Godot by graphics, features, physics and support.
- Improve game graphics and inspect hero/mob models, hair and meshes while
  preserving the existing aesthetic; professional visual quality is the goal.
- Inspect environment props for incomplete/one-sided geometry (jar screenshot).
- Check bugs, performance and opportunities to improve without losing quality.
- Check mob animation intersections, especially the goblin axe entering its head;
  inspect hero animations too.
- Distinguish relic, weapon and other reward-card categories clearly after the
  tomes-to-relics migration.
- Create Zeus with lightning, matching models/animations and balance, initially
  unlocked. Later instruction supersedes availability: disable Zeus without
  deleting him; retain the implementation for future use.
- Improve lobby character-selection layout. Inherited checkpoint reports a
  delivered Hordewake 0.8.48 Windows/APK, but this handoff has not reverified files.
- User requested updated Windows and Android game builds in Downloads. Do not
  present a new engine integration build as a new game release.

## Resume procedure

Read CLAUDE.md, MASTER_PROMPT.md, PROGRESS.md, ADRs 0188/0189 and both briefs.
Inspect git diff before editing. Do not reuse closed tool session IDs. Preserve
pre-existing changes and do not edit third_party. Continue authorized work; the
Markdown checkpoint is not a pause request. Update tests/results/remaining scope
after each milestone. Communicate in Portuguese; do not claim zero performance
impact or real SDK/account support from synthetic tests. No commit/push/publication
has been authorized. No subagents have been spawned.

## Latest verified status — 2026-10-09

Final logs inspected again after the owner asked whether everything is tested.
- Windows final gate FAILED: determinism (411 s gate, 357.80 s tests).
- Linux final gate FAILED: determinism (830.8 s gate, 521.84 s tests).
- Luau final gate FAILED: documentation navigation does not declare guides/platform-services.
- Android final gate PASSED: APK/AAB export, 2/2 tests (146.1 s).
- Documentation lint PASSED (216.5 s). Formatting PASSED (56.5 s).
- Do not claim release-ready, all tests green, real-account authentication, or hardware gamepad validation.
- Next: investigate both determinism failures, register the manual page, rerun affected gates, then finish exported-player smoke checks described above.

## Correction work — 2026-10-09

Owner authorized fixing the failed checks. Two independent determinism causes:
1. New InputService alias/device properties entered the property hash. Marked
   PreferredInput as Presentation (LastInputDeviceType already hashes category),
   physical family/connection ID as HostFact. New regression test confirms hardware
   metadata is excluded while the input category remains covered.
2. Six generic service singletons were eagerly created at boot, shifting instance
   IDs and the game tree even when unused. Added declarative Class.LazyService
   schema/registry metadata; all six instantiate on GetService and retain disabled
   behavior. Default eager services retain their order. Tests cover lazy creation,
   singleton identity and eager-service counts. No replay traces changed.
3. Registered guides/platform-services in site navigation; all API generators and
   gen_site passed after fixes.

The first correction Windows run was canceled after it exposed the second cause;
its failed hash is not a final result. Updated source is now under validation:
- %TEMP%/ludwerk-corrections-windows-final.log (full build/tests/player/export).
- %TEMP%/ludwerk-corrections-linux.log (full Tier-2 build/tests).
- %TEMP%/ludwerk-corrections-luau.log (lint/generation/CLI).
Final exported-player fixture now checks all six generic disabled services plus
XboxService and default input preference. Run after fresh player is built.
### Correction verification checkpoint

- Windows original-trace determinism PASSED (13.17 s); no traces rewritten.
- Windows script, input and app CTest entries PASSED on final source. App includes
  actual scene-switch authentication/provider survival and one-time destruction.
- Documentation lint PASSED (437.6 s).
- Same pinned clang-format gate PASSED, 804 files, using existing Tier-2 image.
  Wrapper's Docker image refresh timed out at Docker Hub before lint ran; direct
  scripts/gates/clang-format.sh completed with exit 0. No style check bypassed.
- Full Luau first correction gate PASSED (331 s); final-source rerun still running.
- Fresh shipping Windows player built with exit 0.
- Exported disabled player PASSED with explicit log markers for Xbox, all six
  generic services and default gamepad preference. Exit 0, no error log entries,
  no integrations directory. Log: %TEMP%/ludwerk-corrections-disabled-player.log.
- Enabled Windows export inventory/hash checks PASSED: engine_xbox.dll,
  xgameruntime.dll, libHttpClient.dll, GDK-LICENSE.md and MicrosoftGame.config.
  Config uses a test-only fake SCID; native account calls were NOT attempted.
- Restored fixture source and output to disabled; confirmed no native payload.
- Pending full Windows/Linux final completion, Android exports and final Luau rerun.
## Owner clarification: external SDK installations — 2026-10-09

Owner asked how GDK/GDKX paths and open-source distribution should work.
Current verified implementation: optional managed public-GDK NuGet install outside
source; explicit ENG_GDK_ROOT for native adapter builds; machine-wide tools root;
project configuration has no local absolute SDK path. SDK headers/libraries are
not checked into platforms/xbox (own CMakeLists.txt and adapter source only).

Proposed extension NOT YET IMPLEMENTED in the panel/manager:
- Extend existing manager with generic external-installation descriptors/detectors,
  rather than a parallel ExternalSdkManager or a hardcoded GDKPath UI.
- Detect supported installations first; offer Detect Again, Locate SDK and Verify.
- Store selected SDK ID/path/version/source in local machine config, not project TOML.
- Validate supported version, required headers/libs and required build tools.
- Separate SDKDetected, ProviderReady and TitleConfigured; never claim native
  service readiness just because a folder exists.
- Never delete or repair files belonging to an externally located SDK; managed
  installs retain ownership/integrity rules.
- Distinguish public GDK PC and restricted console modules. Do not register fake
  PS/Nintendo/GDKX implementations or label Windows GDK support Xbox console-ready.
- Distribution of SDK/tools/runtime files is subject to their own license, not the
  engine's Apache license. No GDK artifact was published during this task; local
  prebuilt package tests are not evidence of permission to publish standalone DLLs.

Official sources checked for this answer:
https://github.com/microsoft/GDK
https://github.com/microsoft/GDK/blob/Main/LICENSE-EN-US.MD
This clarification does not replace the active task of fixing the failed gates.
### Final-source gates completed so far

- Windows FULL: PASSED, 716.3 s; 138/138 CTest entries (448.78 s), 1642/1642
  conformance cases, hot-reload 3/3 and packaging 5/5. Script 190/190,
  platform 56/56 and input regression included. Latest editor and player built.
- Android FULL: PASSED, 472.4 s; arm64 player, APK/AAB exports, 2/2 tests.
- Luau FULL final-source rerun: PASSED, 359.9 s.
- Docs: PASSED, 437.6 s. Format: same pinned gate PASSED, 804 files.
- Linux original-trace determinism: PASSED, 10.99 s; full suite still running.
- Both tests/determinism and tests/replay remain unchanged in git status.
## Final correction result — 2026-10-09 (supersedes pending checkpoints above)

DONE: determinism failures on Windows/Linux and unresolved manual navigation fixed.
No determinism or replay traces modified. No commits, pushes or publications made.

Verified final-source results:
- Windows full gate PASS, 716.3 s: 138/138 CTest, conformance 1642/1642,
  hot reload 3/3, Windows packaging 5/5. Dev editor and shipping player built.
- Linux full gate PASS, 1246.3 s: 145/145 CTest (511.85 s), conformance 1642/1642,
  hot reload 3/3. Original determinism traces passed on both compilers/platforms.
- Android full gate PASS, 472.4 s: arm64 builds, APK/AAB exports, 2/2 tests.
- Luau full final-source gate PASS, 359.9 s: 382 files no diagnostics, CLI 66/66,
  schema/types/generated C++/dump/reference/site freshness and i18n/layer checks.
- Docs PASS, 437.6 s. Same pinned format gate PASS, 804 files (cached image after
  Docker Hub timeout). git diff --check PASS after final documentation updates.
- Final Windows test binaries: script 190/190, input 46/46, platform 56/56.
- Actual scene-switch authentication/provider lifetime regression passed.
- Fresh exported Windows player disabled runtime smoke PASS with all three log
  markers; no error entries or native payload. Enabled export exact inventory
  and SHA256 checks PASS; fixture restored to disabled and rebuilt without GDK.

Local native package (not published):
C:/Users/juanr/AppData/Local/Ludwerk/build/optional-modules/microsoft-gdk-2604.5.7903-windows-x64.zip
SHA256: 168b78dffd3b64df283dee29591cf4d6f6f2967782a123f07dd10b2c76911f4f.

Remaining scope/limitations, not hidden by green local checks:
- External SDK auto-detect/manual-location UI described in the owner clarification
  is NOT IMPLEMENTED; current paths are managed installation and CMake ENG_GDK_ROOT.
- Real Microsoft account/title sign-in/achievement submission, Store submission,
  hardware gamepad identification and visual editor UI inspection unverified.
- Real provider exposes only identity/sign-in/achievement completion. Other generic
  operations explicitly unavailable; GDKX/console backends and macOS unverified.
- No new Hordewake release or Downloads deliverable was produced by this engine fix.

Next: if external SDK discovery is requested for implementation, extend the existing
module manager with machine-local installations/detectors and panel controls per
its clarification above. Preserve managed ownership and the tested default exports.
## Xbox retail Developer Mode assessment — 2026-10-09

Owner reports their console activated in Developer Mode and asks readiness.
Current integration is GDK Windows PC (_GAMING_DESKTOP), not a console player.
Export targets remain Windows/Linux/Android and desktop servers: no UWP APPX/MSIX
or GDKX console build/deploy pipeline. Hordewake was not modified by this task.
SDL3's vendored docs/README-windows.md:5 explicitly says UWP/WinRT unsupported;
docs/README-platforms.md lists it as dropped. Thus UWP is a substantial port and
feasibility effort, not a packaging checkbox. Vendored README-gdk.md documents
SDL GDKX support with licensed SDK access; this alone does not port engine RHI,
filesystem, lifecycle, networking or packaging. Current PC service provider cannot
be assumed compatible with UWP or Xbox console.
Suggested sequence: enable Device Portal and verify PC access; choose feasibility
prototype for retail UWP versus authorized GDKX native path; minimal renderer/input/
assets/lifecycle package first, Hordewake afterward. Never promise the existing EXE
runs on retail Dev Mode or that adding service calls supplies platform support.
Official references checked:
https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/device-portal-xbox
https://learn.microsoft.com/en-us/windows/uwp/xbox-apps/devkit-activation
https://learn.microsoft.com/en-us/gaming/gdk/docs/gdk-dev/get-started/get-started-home
https://github.com/libsdl-org/SDL/blob/main/docs/README-windows.md
No UWP/GDKX implementation was started by this assessment.


## Latest continuation - Hordewake controller support

The historical assessment above is superseded by the deployed UWP probe and
[Hordewake gamepad continuation](hordewake-gamepad-and-uwp-player-2026-10-09.md).
Hordewake 0.8.49 now uses device-independent gameplay actions and UI navigation,
with Windows and Android exports in Downloads. The full console player/RHI port
remains pending; do not infer console game readiness from the working probe.

## Native Xbox player continuation - 2026-10-09

The owner authorized porting the actual player. Progress and exact remaining work
are recorded in [the native player handoff](xbox-native-player-port-2026-10-09.md).
Native D3D12 CoreWindow presentation with shipping Ludwerk DXIL and shared resource/
pipeline code is verified on the owner's Series S (renderer check 0.1.0.10,
120 frames, hardware adapter). The private native command encoder now uses the
existing RHI interface; local tests cover graphics, indirect execution, compute,
descriptor snapshots and safe teardown. Full Windows RHI: 26 cases / 989 assertions,
no failures/skips; AppContainer compilation passes `/W4 /WX`.

The full Hordewake player has now been built, packaged and deployed. The owner
confirmed entering a match with the physical controller. Game classification
unlocks the measured 5120 MiB budget; the 1080p high menu runs around 60 FPS.
The previous 35% estimate is obsolete. v0.8.49.5 now prewarms the essence shader
during loading and releases gameplay bindings under modals; owner confirmed cards
and pause work on the controller. Current follow-up is a periodic ~0.52 s
GPU/display wait, startup device loss around the App -> Game transition and
remaining lifecycle/save/reconnect checks. Follow the native handoff for evidence
and the VSync A/B (hitch persisted). Preserve Windows/Android 0.8.49 exports and probe saves.

Owner additionally confirms spontaneous hitches on PC and mobile. Investigate
the shared player/game path as well as native GPU waits; identical symptoms do
not yet prove an identical cause. The published PC/Android packs predate essence
pipeline prewarming. Shared long-wait diagnostics now distinguish limiter sleep,
present pacing, begin-frame, swapchain acquisition and submission, with requested
sleep reported separately. Windows application tests: 1042 cases / 38847
assertions passed (two skipped). See the native handoff for ongoing soak evidence.

Owner requests the stress benchmark specifically ON THE CONSOLE. PC was used
only to validate automation; those FPS numbers must not appear as Series S
results. Owner objected to mouse capture; all owned PC benchmark players were
closed immediately, fixture cursor lock disabled. No more PC benchmark windows.
Separate Hordewake Benchmark UWP APPX (identity Ludwerk.Hordewake.Benchmark,
0.8.49.1) is built/signed in Downloads/Hordewake-Benchmark-xbox and uploading.
It leaves the normal Hordewake installation and saves separate. Needs Game
classification before measured console run. Eight cases / metrics / workload
limitations are documented in the native-player handoff; results still pending.

Latest benchmark state and precise continuation steps:
[Series S benchmark handoff](hordewake-series-s-benchmark-2026-10-09.md).
Revision 2 is deployed, default Game preference enabled and console restarted.
Owner must sign in again after restart; no valid console benchmark results yet.

Latest performance work supersedes the earlier pending benchmark checkpoints: baseline eight cases completed; native queue overlap candidate in hardware measurement. See [render performance handoff](hordewake-render-performance-2026-10-09.md).

Native performance batch now complete: same Series S benchmark (High1080p)
gained up to43.4%; VSync ON550 mobs46.41->59.71 FPS and1000 mobs/12 weapons
39.38->55.83. Windows RHI28/1137 passed. Normal v0.8.49.7 installed and open at
~60FPS menu,5120MiB Game budget; APPX in Downloads/Hordewake-0.8.49-xbox.
All eight results, caveats and remaining cross-platform/loading work are recorded
in [performance handoff](hordewake-render-performance-2026-10-09.md) and
[measured comparison](hordewake-render-performance-results-2026-10-09.md).

Next optimization batch: shared particle texture palettes, not platform-specific.
Windows/Linux particles18/1601, Windows RHI29/1181, full AndroidARM64 player and
UWP compile. Functional headless SDL_GPU checks on D3D12/NVIDIA and Vulkan/Intel
render matching mixed textured/procedural particles with no PC window or mouse
capture. Clean Xbox benchmark v7 is collecting remaining cases; save under
results/particles-v7, preserve original v5 and native-overlap v6 evidence. See
[performance handoff](hordewake-render-performance-2026-10-09.md).

Shared particle batch completed all eight Series S cases: up to56.7% fewer total
calls, extreme OFF54.93->56.17FPS; modest FPS gain, OFF550 worst39.04ms recorded.
Normal v0.8.49.8 now installed/launched in Game mode. Same-quality source and
functional D3D12/Vulkan checks passed; Android player builds, hardware untested.
[Second-batch measurements](hordewake-particle-performance-results-2026-10-09.md).
# Latest cross-device profiling evidence (2026-10-09)

The owner's requested one-minute, 1500-enemy / twelve-weapon test ran on the
physical Windows PC, Xbox Series S (Game mode), and wireless-ADB Android phone.
FPS averages: 106.06 / 52.25 / 38.13 respectively. Full mean/best/worst/p99 CPU
scope tables and reproducibility caveats:
[results](hordewake-cross-device-profile-results-2026-10-09.md),
[handoff](hordewake-cross-device-profile-2026-10-09.md).
No direct GPU execution timestamps were collected. Android resolution and
presentation policy differ; these are actual-platform profiles, not equal-work
hardware rankings. This does not prove the earlier intermittent hitch is gone.



## Latest PC optimization and Release delivery (2026-10-09)

Current owner constraints: Xbox is off; phone access was withdrawn. Continue
PC-only/windowless, without pointer capture or physical device queries/deploys.
Batch 9 completed several optimization cycles in shared extraction, animation,
SDL/native GPU submission and game horde code, then delivered a proper Windows
Release/player folder and ZIP. Matched dev CPU frame medians improved 4.95%
SDL_GPU / 6.38% native D3D12. Valid exported Release stress medians: 5.1851 /
5.4728 ms on this PC, one run each; not physical device measurements. Tests,
quality caveats, discarded incomplete-source Release runs, artifacts and next
large targets are recorded in the
[batch 9 handoff](hordewake-profile-optimization-batch9-2026-10-09.md).
Do not infer that all intermittent stalls are fixed.


## Play-to-lobby loading investigation (2026-10-09)

Owner asks why fast loading is not yet achieved. Automated PC/dev Play baseline
finds 0.817 s terrain mesh wait within 1.127 s until the game card is destroyed;
repeated camp construction and fixed fade/timer padding are confirmed. No
production code changed in this audit; previous optimized9 Release remains
latest. Read [loading audit](hordewake-lobby-loading-audit-2026-10-09.md) before
implementing reuse/preload: includes measurement limits and architectural scope.
PC only; Xbox off and phone access remains withdrawn.


## Hordewake loading batch 10 and local co-op (2026-10-09)

Loading reuse implemented and exported as Downloads/Hordewake-0.8.49-loading10-windows
and matching ZIP. Three exported Release runs each: Play-to-card-removal median
0.888 to 0.561 s (-36.9%), PC/headless only. Bounded packed CPU terrain geometry
reuse, cached camp samples, readiness-based removal of padding and shorter
menu/lobby fade. Windows/Linux terrain tests 23 cases / 11813 assertions each,
Windows 7 captures, strict game types, both normal Windows backend startup smokes
pass. Read hordewake-lobby-loading-audit-2026-10-09.md for evidence/limits.

Owner additionally authorized full local co-op up to four controllers, shared
higher camera with adaptive framing, simultaneous independent card hands (wait
for everyone), full lobby/pause/reconnect/end flows. Not implemented yet; source
audit found missing per-controller raw input/action filtering and singular local
player routing. Read hordewake-local-coop-2026-10-09.md. Xbox remains off, phone
permission withdrawn, no pointer capture or interactive PC launches.


### Hordewake local co-op Xbox export checkpoint (2026-10-09)
First integrated offline 2-4 player co-op Release exported, signed and installed
as Ludwerk.Hordewake.Dev 0.8.49.35. Xbox startup verified in Game budget (5120 MiB),
menu around 60 FPS. Synthetic two/four-player PC flows, engine tests on Windows
and Linux and strict shipping Luau source analysis passed. Physical controller
feedback pending; full console port is not yet declared complete. Cold Xbox
startup stalls/readiness warning and guest stats-sheet ownership remain open.
Full handoff: docs/briefs/hordewake-local-coop-2026-10-09.md.
Artifact: C:/Users/juanr/Downloads/Hordewake-0.8.49-coop-xbox/.
Phone remains off limits; PC game testing remains windowless/no pointer capture.


Xbox co-op follow-up: owner confirmed independent movement with two controllers.
Lobby now identifies local players over figures and on hero portraits; local
readiness uses actual ready state. Strict source analysis and reviewed four-seat
windowless capture passed. Xbox package updated to 0.8.49.36. See co-op brief.
