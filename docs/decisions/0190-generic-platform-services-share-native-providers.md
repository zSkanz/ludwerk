# 0190 - Generic platform services share native providers

- Status: accepted
- Date: 2026-10-09
- Decided by: the owner, generic platform services implementation requirements.

## Decision

IdentityService, AchievementService, StoreService, CloudSaveService,
LeaderboardService and SocialService are built-in service contracts, accessible
with GetService even without integrations. XboxService remains the optional,
specific API. They reuse the same per-runtime native provider, busy state and
existing coroutine scheduler. NetworkService and local SaveService are unchanged.

The native ABI v1 table remains unchanged. A separate optional capabilities
symbol declares supported operations and JSON responses. Old DLLs remain usable
through XboxService without claiming new generic capabilities. New operations
use a bounded JSON request/response through the existing JSON codec; generic
records and enums contain no SDK handles, types or primary native error codes.

Project settings choose Auto or a registered provider ID for each service.
Auto selects deterministically among enabled, available providers declaring the
requested operation. Project achievement/product/leaderboard IDs map per provider;
missing mappings fail instead of forwarding a guessed native ID. Identity IDs
are opaque and namespaced by provider. Physical gamepad family and operating
system do not select an identity provider.

Actions return boolean/error; data queries return optional data/error. An absent
provider never parks a coroutine. Unsupported capabilities return NotSupported,
never fake data. Native diagnostic failures become ProviderError for generic
callers; development logs retain native details. Platform-specific APIs preserve
their existing diagnostic behavior.

Normal scene changes retain the application VM and provider, so identity survives
without SDK reinitialization. Runtime/application teardown owns native cleanup;
scene objects never own SDK state. A distinct editor/runtime reset ends that run.

## Initial capabilities

The six generic service classes declare LazyService in the API schema. GetService
creates each singleton on first request without requiring any SDK; creating the
singleton does not load its provider. Unused services allocate no scene objects
and cannot shift existing instance IDs or recorded determinism traces. Existing
eager services retain their startup order. LazyService is generic registry
metadata, not a list of service names in the boot path.

Xbox is the only real provider. Its implemented generic capabilities are identity,
sign-in and achievement completion. Progress queries/updates, store, cloud saves,
leaderboards and social operations are declared unavailable until their real GDK
adapters exist. Test providers belong only to test binaries and never appear in
the module catalog. No fictitious console or Steam support is registered.

## Verification

Test provider selection, authentication reuse, mappings, stable errors, malformed
responses, false-versus-error queries, deferred completion, unsupported features
and actual scene-load survival. Validate default GDK-free builds and optional DLL
compilation separately. Native title/account behavior requires configured titles.
