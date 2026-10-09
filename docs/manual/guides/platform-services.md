# Platform services

Use the generic services for shared gameplay code. Use XboxService for native
Xbox-specific identifiers and features. Both layers share one provider session
and the existing async scheduler. NetworkService, scenes and local SaveService
keep their existing responsibilities.

All six generic services exist without an SDK or enabled integration. Writing
and checking scripts does not initialize a native SDK. Native providers load on
first use only when enabled. Connecting a controller does not choose a platform
account provider.

GetService creates each generic service singleton on its first request. Unused
services do not allocate objects in the game tree. FindService returns nil before
that request; there is no need to call it before using GetService. Hardware family
and gamepad connection IDs are excluded from the deterministic world hash; the
existing input category and gameplay changes remain covered.

## What works in the initial Xbox provider

| Service | Real initial capability |
| --- | --- |
| IdentityService | Identity queries and native sign-in |
| AchievementService | Completion of mapped achievements |
| StoreService | Unavailable |
| CloudSaveService | Unavailable |
| LeaderboardService | Unavailable |
| SocialService | Unavailable |

Progress reads/updates are also unavailable initially. The other contracts are
prepared for real providers; they do not return mock products, saves, rankings
or friends. Use `IsAvailable` for the service's primary capability and `Supports`
for individual optional features. The API reference names each primary capability.

```luau
local identity = game:GetService("IdentityService")
if identity:IsAvailable() then
    local ok, reason = identity:SignInAsync()
    if ok then
        local user = identity:GetLocalUser()
        if user then
            print(user.Id, user.DisplayName)
        end
    else
        warn(reason)
    end
end

local achievements = game:GetService("AchievementService")
if achievements:Supports("Unlock") then
    local ok, reason = achievements:UnlockAsync("FIRST_WIN")
    if not ok then
        warn(reason)
    end
end
```

User IDs are opaque and provider-namespaced, such as `xbox:...`; do not parse them
as a universal XUID or number. `Platform` names the provider, independently of
the operating system. Social queries require a user from their selected provider.

## Project settings

Choose providers and edit mappings in **Platforms and Services**. `Auto` selects
an enabled, available provider declaring the requested operation, in stable ID
order. Explicit selections never silently fall back to a different provider.
Only registered integrations appear in the selector. Reopen the project to apply
changes to the runtime. The equivalent project configuration is:

```toml
[integrations]
enabled = ["xbox"]

[integrations.xbox]
configuration = "YOUR_TITLE_SCID"

[platform_services.identity]
provider = "Auto"

[platform_services.achievements]
provider = "xbox"

[platform_services.achievements.xbox.ids]
FIRST_WIN = "YOUR_NATIVE_ACHIEVEMENT_ID"

[platform_services.store.xbox.ids]
DLC_01 = "YOUR_NATIVE_PRODUCT_ID"

[platform_services.leaderboards.xbox.ids]
HIGH_SCORE = "YOUR_NATIVE_BOARD_ID"
```

Store/leaderboard mappings prepare project IDs; they do not enable those native
capabilities. Other selection keys are `store`, `cloud_save`, `leaderboards` and
`social`. Cloud slots and namespaced social user IDs are not product ID mappings.
Never put account credentials or secret keys in these public project settings.

## Results and failures

Actions return `(success, errorCode?)`. Data queries return `(value?, errorCode?)`.
A boolean query can return `false, nil` as a successful negative answer;
`nil, errorCode` means it could not answer. An unavailable async operation finishes
immediately. Accepted operations use the engine's existing coroutine scheduler.
One pending request per provider returns Busy to concurrent callers across both
generic and native services.

| Error | Meaning |
| --- | --- |
| IntegrationUnavailable | No enabled/registered provider for this service |
| ProviderUnavailable | Selected native backend could not be used |
| NotSupported | Available provider does not declare this operation |
| UnknownAchievement / UnknownId | Internal ID has no provider mapping |
| InvalidArgument / InvalidUser | Invalid data or user from another provider |
| NotSignedIn / PermissionDenied | Authentication or permission failure |
| Conflict / QuotaExceeded / NetworkError | Explicit provider/storage failure |
| ProviderError | Native failure; development log retains native details |
| InvalidProviderResponse | Malformed, oversized or incorrectly typed native result |

Normal scene changes retain platform authentication. Ending the application/runtime
run releases its provider. Cloud saves do not create local fallback files or
silently resolve conflicts. Ranking queries accept integer counts 1 through 100;
cloud writes accept up to 1 MiB of textual/encoded data, subject to lower native
quotas. Native JSON responses are bounded to 1 MiB and validated before Luau delivery.

## Adding a real provider

Implement the SDK-free GameIntegration contract in an optional native module and
register its project integration as the existing module system does. Generic
bindings contain no Xbox-name branches. Keep SDK headers and redistributables
outside the default engine. Do not register test doubles as developer providers.

The original ABI v1 remains valid. The optional
`engineGameIntegrationCapabilities` entry declares operations and supplies JSON
responses for data queries. Old DLLs keep their XboxService contract and declare
no generic capabilities until updated. The Xbox extension currently declares
only Identity, SignIn and UnlockAchievement.

New operations receive JSON with `id` (resolved native ID) and `logicalId`, when
applicable, plus `data`, `score`, `count` or `progress` for their arguments.
Legacy SignIn/UnlockAchievement retain their v1 argument convention. Response
records follow the generated Luau types; product IDs stay logical, user IDs stay
namespaced. Providers must return stable generic errors and explicit unsupported
capabilities. Adding another SDK should require the adapter, module metadata and
mappings, without rewriting the six generic service APIs.

See [shipping](manual:guides/shipping) for optional module packaging and
Microsoft title configuration requirements.
