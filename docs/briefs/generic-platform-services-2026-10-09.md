# Generic platform services: architecture review and implementation plan

Date: 2026-10-09. Status: six service phases implemented and tested individually; final integrated validation running.
Owner requirements are preserved verbatim in
[the request](generic-platform-services-owner-request-2026-10-09.md).
This extends [the optional module work](optional-platforms-and-services-2026-10-09.md).

## Current architecture examined before changing this layer

- Services and bindings: `api/defs/services.api.luau`, generated descriptors,
  `engine/script/src/services.cpp`, `optional_service.cpp`, `services.h` and
  `runtime.cpp`. Service names are schema driven. XboxService has six methods,
  retains optional integration metadata, and is accessible while disabled.
- Native seam: `engine/platform/game_integration.h` and `game_integration_abi.h`.
  ABI v1 has SDK-free identity, begin/poll and explicit failure codes. The
  optional `platforms/xbox/xbox_provider.cpp` implements sign-in and achievement
  completion through the real GDK. Store/cloud/leaderboard/social are not implemented.
- Async: optional operations use the existing Luau coroutine references and
  `resumeScheduled`, polled at the existing timer resumption point. Action
  results are `(boolean, string?)`; there is no reason to add promises or a
  second scheduler. Provider execution is serialized and reports Busy.
- Lifecycle: `WorldHost::loadScene` closes scene-owned scripts and reads the next
  scene inside the same World/ScriptRuntime. It does NOT destroy the VM or native
  provider. `restartRuntime` is a distinct editor/runtime reset. Providers must
  remain independent of scene objects; test this explicitly.
- Networking: NetworkService and scene session transport remain untouched.
- Configuration: strict shared TOML subset, ProjectConfig -> EngineOptions ->
  WorldHostOptions; enabled integration IDs and opaque provider configurations.
  Exporters and the platform manager already share the CLI module registry.
- Build: default GDK-free engine; optional native DLL built only with ENG_GDK_ROOT.
  SDK and prebuilt provider installs are separate, pinned and outside source.

## Smallest implementation that fits

1. Reuse the native provider and per-runtime registry for both generic services
   and XboxService. No provider-name branches inside generic methods.
2. Add operation capabilities and a typed request/response extension to the
   existing SDK-free native seam. Preserve ABI v1 and existing Xbox methods;
   optional extension discovery must not break older DLLs. Unsupported operations
   return NotSupported immediately, without a waiter or fake data.
3. Add schema-defined IdentityService and AchievementService first. Identity
   returns a provider-namespaced opaque ID and display name. Achievements resolve
   internal project IDs through provider mappings. Reuse authentication and
   achievement completion, not a second native session.
4. Expose StoreService, CloudSaveService, LeaderboardService and SocialService
   with explicit unavailable behavior until real capabilities exist. The provider
   contract must allow future real implementations without changing gameplay API.
5. Configuration selects Auto or a registered provider per generic service;
   deterministic Auto selection must use real registered capabilities. Store,
   achievement and leaderboard mappings are provider-scoped project data.
6. Extend the existing platform manager for those settings, including honest
   capability descriptions. No fake Steam/console providers and no secrets.
7. Regenerate Luau types/reference/dump and test real service dispatch with
   internal test-only providers. Compile/test each service incrementally.

## Validation to retain

Disabled and absent providers, capabilities, signed-in/out users, explicit async
failures, missing IDs, provider errors, shared identity with XboxService, scene
switch survival, default GDK-free builds and Windows/Linux/Android exports.
Real account/title calls cannot be claimed tested without configured credentials.

## Historical continuity (superseded by correction verification below)

The earlier platform Windows/Linux full gates passed before the InputService
extension. InputService Windows/Linux/Android/Luau checks were running at this checkpoint;
inspect their logs before treating them as green. Complete those results while
implementing this new authorized scope. Do not reset unrelated dirty files.

## Checkpoint: identity, achievements and store

- Identity source compiled; full script suite 185/185 and platform 56/56 passed.
  A dangling string_view in Auto selection was caught by tests and corrected.
- Achievement mapping/capability source compiled; script 186/186 and platform
  56/56 passed. Native HRESULT details map to ProviderError only for generic
  APIs; XboxService retains its existing specific failure code.
- Store source compiled; script suite passed, including absent capabilities,
  mapped requests, product shape validation and false ownership versus errors.
  No real Xbox store capability advertised or implemented.
- Cloud source and tests now being compiled (ludwerk-generic-cloud-check.log).
- JSON requests/responses reuse @std/json's codec; native response size/type checks
  protect Luau contracts. SDK ABI v1 retained; optional extension adds discovery.
- Configuration mapping test and actual host scene-survival test written; app
  compilation/test still pending after the remaining service phases.
- Input deferred signal test corrected; included in the 185/185 passing script run.
- Earlier Linux input build failed after reading a file during formatting (NUL
  bytes in the transient read); current file has zero NUL bytes. Rerun on stable
  sources, avoid formatting files during compilation. Docs gate passed 557.1 s.
- ADR 0190 records the generic architecture and current real capability limits.

## Checkpoint: all six contracts and editor settings implemented

- Cloud/leaderboard/social phases passed script suites 188/189/190 cases respectively.
- Platform provider selectors and provider-scoped ID mapping UI are written.
- GDK extension rebuilt; safe DLL lifecycle/capabilities passed; only identity/sign-in/
  completion are advertised. Updated prebuilt ZIP installed and verified without SDK.
- Manual guide documents all contracts, errors, ID mapping and current limits.
- Full final Windows/Linux/Android/Luau checks running; see main handoff log paths.
- Initial final Luau lint rejected Auto in an ImGui preview expression; fixed localized
  preview and direct i18n lint passed. Full rerun pending.
- Do not claim finished until integrated gates and scene-survival test pass.

## Correction verification — 2026-10-09

The final integrated run exposed eager generic service creation shifting instance
IDs, plus new input metadata/alias entering the world hash. Both were corrected
without rewriting replay traces: schema/registry LazyService metadata for generic
services, Presentation for the input alias and HostFact for physical metadata.
Tests assert lazy GetService singleton creation and hardware-independent hashes;
the existing category remains hashed. Manual page now registered in site nav.

Final-source Windows gate PASSED (716.3 s, 138/138 CTest, 1642 conformance,
hot reload 3/3, packaging 5/5). Android PASSED (472.4 s, APK/AAB 2/2);
Luau PASSED (359.9 s), docs PASSED (437.6 s), pinned format PASSED (804 files).
Shipping Windows player built and exported disabled runtime passed all six generic
service failures, Xbox disabled failure and input-default checks. Enabled export
payload inventory and file hashes passed, then fixture restored to disabled.
Linux: determinism, app and prolonged terrain flight passed; full suite 140/145
passed, remaining serial network/soak/perf tests running. Inspect main brief and
%TEMP%/ludwerk-corrections-linux.log for final results before claiming complete.

External SDK auto-detection/manual-location UI is a newly discussed extension,
not implemented by these corrections. Current managed installs and ENG_GDK_ROOT
remain the available paths. See the main brief's external SDK clarification.

## Final local validation

All correction gates passed. Linux full gate: 1246.3 s, 145/145 CTest,
1642/1642 conformance and 3/3 hot reload. Windows full gate: 716.3 s,
138/138 CTest, 1642/1642 conformance, 3/3 hot reload, 5/5 packaging.
Android, Luau, docs, pinned format and final exported-player/payload checks
also passed. No trace changes. See main brief Final correction result for logs,
package hash, external SDK discovery proposal and remaining external validations.
