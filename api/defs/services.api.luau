--!strict
--- Services and the DataModel (api-design.md §2.1, §1.2).
---
--- The kernel's subset. §2.1 lists the complete v1 set; the ones missing here are
--- missing because the systems behind them are later milestones.
---
--- One thing the schema cannot carry: `GetService` is declared with its general
--- `(name: string) -> Instance` overload alone. §1.2's string-singleton
--- overloads are the typed half of the same call and `Schema.Method` has no
--- overload list, so the general form is what is written down and the typed
--- forms are owed to the `.d.luau` generator; see the method's own note.
---
--- `RunService.Heartbeat` and `DebugService.MessageOut` are ordinary `Events`.
--- Neither name is a past-tense fact or a `Pre*`/`Post*` phase and both are
--- frozen public surface, which is what the schema's `EventNameExceptions` set
--- is for -- the rule carries the exception rather than the names carrying a
--- suffix.
---
--- Thread safety is annotated conservatively: `Safe` only where the value is
--- immutable, `ReadParallel` where the engine settles it before a tick and code
--- merely reads it, and `Unsafe` everywhere else. v1 runs serially (§3), so
--- nothing observes these yet -- and widening `Unsafe` to `ReadParallel` later
--- is a compatible change while the reverse is not.

local Schema = require("../schema")

-- The annotations below -- this local, and the `:: { Schema.X }` on every
-- nested list -- are load-bearing, not decoration. Without one at a given
-- level, the new solver infers that array's element type from the entries
-- themselves and then demands every entry carry the same optional fields, so
-- one method with a `Default` makes every other method missing it an error.
-- Each is an assertion rather than a cast: a wrong field name or a `Tags` entry
-- outside `ClassTag` still fails analysis.
local Classes: { Schema.Class } = {
    {
        Name = "SocialService",
        LazyService = true,
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Queries the platform's social features: friends, presence, blocking and communication permission. "
            .. "User ids are specific to the provider. The Xbox provider does not support social features yet. "
            .. "For multiplayer sessions, use `NetworkService`.",
        Methods = {
            {
                Name = "IsAvailable",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the active provider supports friend queries.",
            },
            {
                Name = "Supports",
                Parameters = { { Name = "feature", Type = "string" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the active provider supports a feature: `GetFriends`, `GetPresence`, "
                    .. "`IsBlocked` or `CanCommunicate`. Returns false for an unknown feature name.",
            },
            {
                Name = "GetFriendsAsync",
                Parameters = {},
                Returns = { "{ { Id: string, DisplayName: string } }?", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Returns the friends the current user's privacy settings allow the game to see, or nil and "
                    .. "an error code. Each friend's `Id` is specific to the provider.",
            },
            {
                Name = "GetPresenceAsync",
                Parameters = { { Name = "userId", Type = "string" } },
                Returns = { "{ IsOnline: boolean, Status: string? }?", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Returns the basic presence of a user from the active provider, or nil and an error code "
                    .. "for a privacy, sign-in or network failure.",
            },
            {
                Name = "IsBlockedAsync",
                Parameters = { { Name = "userId", Type = "string" } },
                Returns = { "boolean?", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Returns whether the provider reports that the user is blocked. Returns nil and an error "
                    .. "code if the query failed or is not supported; nil does not mean that communication is "
                    .. "allowed.",
            },
            {
                Name = "CanCommunicateWithAsync",
                Parameters = { { Name = "userId", Type = "string" } },
                Returns = { "boolean?", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Returns whether the platform allows communication with a user. Returns nil and an error "
                    .. "code if the answer is unknown or the query is not supported; treat nil as not allowed. It "
                    .. "does not open a connection or create a session.",
            },
        } :: { Schema.Method },
    },

    {
        Name = "LeaderboardService",
        LazyService = true,
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Reads and writes the platform's leaderboards. Boards are referred to by project ids that the "
            .. "project maps to each provider. The Xbox provider does not support leaderboards yet, and there is "
            .. "no local leaderboard.",
        Methods = {
            {
                Name = "IsAvailable",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the active provider supports reading top scores.",
            },
            {
                Name = "Supports",
                Parameters = { { Name = "feature", Type = "string" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the active provider supports a feature: `SubmitScore`, `GetTop` or "
                    .. "`GetAroundPlayer`. Returns false for an unknown feature name.",
            },
            {
                Name = "SubmitScoreAsync",
                Parameters = { { Name = "id", Type = "string" }, { Name = "score", Type = "number" } },
                Returns = { "boolean", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Submits a score to a board. The score must be a finite whole number that a Luau number "
                    .. "represents exactly. The provider decides whether the score replaces an earlier one and "
                    .. "how the board is ordered.",
            },
            {
                Name = "GetTopAsync",
                Parameters = { { Name = "id", Type = "string" }, { Name = "count", Type = "number" } },
                Returns = { "{ { UserId: string, DisplayName: string, Rank: number, Score: number } }?", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Returns up to `count` entries from the top of a board, or nil and an error code. `count` "
                    .. "is an integer from 1 to 100. Each entry's `UserId` is specific to the provider, and "
                    .. "`Rank` starts at 1.",
            },
            {
                Name = "GetAroundPlayerAsync",
                Parameters = { { Name = "id", Type = "string" }, { Name = "count", Type = "number" } },
                Returns = { "{ { UserId: string, DisplayName: string, Rank: number, Score: number } }?", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Returns up to `count` entries around the current user's position on a board, or nil and an "
                    .. "error code. A user with no score on the board is an error.",
            },
        } :: { Schema.Method },
    },

    {
        Name = "CloudSaveService",
        LazyService = true,
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Reads and writes the current user's save slots in the platform's cloud. It is separate from "
            .. "`SaveService`, which saves on the local machine. There is no offline mode and no local fallback: "
            .. "an operation that cannot reach the cloud fails. The Xbox provider does not support cloud saves yet.",
        Methods = {
            {
                Name = "IsAvailable",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the active provider supports reading cloud saves.",
            },
            {
                Name = "Supports",
                Parameters = { { Name = "feature", Type = "string" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the active provider supports a feature: `Read`, `Write`, `Delete` or "
                    .. "`Exists`. Returns false for an unknown feature name.",
            },
            {
                Name = "WriteAsync",
                Parameters = { { Name = "slot", Type = "string" }, { Name = "data", Type = "string" } },
                Returns = { "boolean", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Writes save data, as a string, to one of the user's slots. The limit is 1 MiB, and a "
                    .. "provider can set a lower one. It fails with an error code such as `Conflict`, "
                    .. "`NotSignedIn`, `NetworkError` or `QuotaExceeded`; it never overwrites silently.",
            },
            {
                Name = "ReadAsync",
                Parameters = { { Name = "slot", Type = "string" } },
                Returns = { "string?", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Returns the contents of one of the user's slots, or nil and an error code such as "
                    .. "`NotFound`, `Conflict` or `NotSupported`.",
            },
            {
                Name = "DeleteAsync",
                Parameters = { { Name = "slot", Type = "string" } },
                Returns = { "boolean", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Deletes one of the user's cloud slots. Local saves are not affected.",
            },
            {
                Name = "ExistsAsync",
                Parameters = { { Name = "slot", Type = "string" } },
                Returns = { "boolean?", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Returns whether one of the user's cloud slots exists. False means the slot does not exist; "
                    .. "nil and an error code mean the query failed.",
            },
        } :: { Schema.Method },
    },

    {
        Name = "StoreService",
        LazyService = true,
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Reads the platform store's products and the user's entitlements, and starts purchases. Products "
            .. "are referred to by project ids that the project maps to each provider. The Xbox provider does not "
            .. "support the store yet: `IsAvailable` returns false and every call fails immediately.",
        Methods = {
            {
                Name = "IsAvailable",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the active provider supports product queries.",
            },
            {
                Name = "Supports",
                Parameters = { { Name = "feature", Type = "string" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the active provider supports a feature: `GetProduct`, `Purchase`, `Owns` "
                    .. "or `RefreshEntitlements`. Returns false for an unknown feature name.",
            },
            {
                Name = "GetProductAsync",
                Parameters = { { Name = "id", Type = "string" } },
                Returns = {
                    "{ Id: string, DisplayName: string, Description: string, Price: string?, Currency: string? }?",
                    "string?",
                },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Returns information about a product by its project id, or nil and an error code. `Price` "
                    .. "is localized text for display, not a number to calculate with.",
            },
            {
                Name = "PurchaseAsync",
                Parameters = { { Name = "id", Type = "string" } },
                Returns = { "boolean", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Opens the platform's purchase flow for a product. It never reports success without a "
                    .. "store. After it completes, confirm ownership with `OwnsAsync`.",
            },
            {
                Name = "OwnsAsync",
                Parameters = { { Name = "id", Type = "string" } },
                Returns = { "boolean?", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Returns whether the current user owns a product. False means the user does not own it; nil "
                    .. "and an error code mean the query failed.",
            },
            {
                Name = "RefreshEntitlementsAsync",
                Parameters = {},
                Returns = { "boolean", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Asks the provider to refresh the user's entitlements, if it supports that.",
            },
        } :: { Schema.Method },
    },

    {
        Name = "AchievementService",
        LazyService = true,
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Unlocks and reads the platform's achievements. Achievements are referred to by project ids that "
            .. "the project maps to each provider. The Xbox provider supports unlocking only. An unsupported "
            .. "feature always returns an error, never success.",
        Methods = {
            {
                Name = "IsAvailable",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the active provider supports unlocking achievements.",
            },
            {
                Name = "Supports",
                Parameters = { { Name = "feature", Type = "string" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the active provider supports a feature: `Unlock`, `GetProgress` or "
                    .. "`SetProgress`. Returns false for an unknown feature name.",
            },
            {
                Name = "UnlockAsync",
                Parameters = { { Name = "id", Type = "string" } },
                Returns = { "boolean", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Unlocks an achievement by its project id. The error code `UnknownAchievement` means the "
                    .. "project does not map the id for the active provider.",
            },
            {
                Name = "GetProgressAsync",
                Parameters = { { Name = "id", Type = "string" } },
                Returns = { "number?", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Returns the progress of an achievement, from 0 to 1, or nil and an error code. Check "
                    .. '`Supports("GetProgress")` first; the Xbox provider returns `NotSupported`.',
            },
            {
                Name = "SetProgressAsync",
                Parameters = { { Name = "id", Type = "string" }, { Name = "progress", Type = "number" } },
                Returns = { "boolean", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = 'Reports the progress of an achievement, from 0 to 1. Check `Supports("SetProgress")` '
                    .. "first; the Xbox provider returns `NotSupported`. The engine keeps no local copy of the "
                    .. "progress.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "IdentityService",
        LazyService = true,
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Signs the player in to the platform and reads who they are. The service always exists, with or "
            .. "without a platform SDK installed. Xbox is the only provider at present. Without a provider, "
            .. "sign-in fails; it is never simulated.",
        Methods = {
            {
                Name = "IsAvailable",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the active provider supports identity and is available.",
            },
            {
                Name = "IsSignedIn",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether a local user is signed in to the platform.",
            },
            {
                Name = "GetLocalUser",
                Parameters = {},
                Returns = { "{ Id: string, DisplayName: string, IsSignedIn: boolean, Platform: string }?" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a read-only snapshot of the signed-in user, or nil. `Id` is an opaque string "
                    .. "specific to the provider. `Platform` is the name of the identity provider, not of the "
                    .. "operating system.",
            },
            {
                Name = "SignInAsync",
                Parameters = {},
                Returns = { "boolean", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Starts the provider's sign-in flow. Returns whether it succeeded, and an error code if it "
                    .. "did not. `IntegrationUnavailable`, `ProviderUnavailable` and `NotSupported` are returned "
                    .. "immediately. It shares its session with the platform's own service, such as "
                    .. "`XboxService`: while one is signing in, the other returns `Busy`.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "XboxService",
        Extends = "Instance",
        Module = "scene",
        Integration = "xbox",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0188" },
        Doc = "Xbox identity and achievements on PC, through an optional provider. The service always exists. "
            .. "Without an enabled provider, queries return nothing and actions return an error code. No SDK is "
            .. "needed to write or check scripts that use it.",
        Methods = {
            {
                Name = "IsAvailable",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether an enabled Xbox provider is available. It is false when the SDK is not "
                    .. "installed.",
            },
            {
                Name = "IsSignedIn",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether a player is signed in.",
            },
            {
                Name = "GetUserId",
                Parameters = {},
                Returns = { "string?" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the player's id as a decimal string, or nil. The id is a string because it does "
                    .. "not fit in a number.",
            },
            {
                Name = "GetGamertag",
                Parameters = {},
                Returns = { "string?" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the display name of the signed-in player, or nil.",
            },
            {
                Name = "SignInAsync",
                Parameters = {},
                Returns = { "boolean", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Signs the player in through the Xbox provider. Returns whether it succeeded, and an error "
                    .. "code if it did not. With the integration disabled, it returns false and "
                    .. "`IntegrationDisabled` immediately.",
            },
            {
                Name = "UnlockAchievementAsync",
                Parameters = { { Name = "achievementId", Type = "string" } },
                Returns = { "boolean", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Unlocks a configured achievement for the signed-in player. It never reports success unless "
                    .. "the platform confirmed it.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "DataModel",
        Extends = "Instance",
        Module = "scene",
        -- Not a `Service` itself: it is the parent every service is a child of,
        -- and it is reached through the `game` global rather than through
        -- `GetService`.
        Tags = { "NotCreatable" } :: { Schema.ClassTag },
        Properties = {
            {
                Name = "EngineVersion",
                Type = "string",
                ReadOnly = true,
                ThreadSafety = "Safe",
                -- A fact about the build, not about the world.
                HostFact = true,
                Doc = "The version of the engine that is running, as set in `project.toml`.",
            },
            {
                Name = "LuauVersion",
                Type = "string",
                ReadOnly = true,
                ThreadSafety = "Safe",
                -- A fact about the build, not about the world.
                HostFact = true,
                Doc = "The version of the Luau language runtime built into this engine. It is fixed for each "
                    .. "engine release.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetService",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "Instance" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the service with the given name. Every service exists from startup, and every call "
                    .. "returns the same instance, an ordinary child of `game`. With a string literal, the result "
                    .. "has the service's exact type; with a name computed at run time, it is typed as "
                    .. "`Instance`. Raises an error for a name that is not a service.",
            },
            {
                Name = "FindService",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "Instance?" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns the service with the given name, or nil if the name is not a service. It differs "
                    .. "from `GetService` only in returning nil instead of raising an error.",
            },
            {
                Name = "BindToClose",
                Parameters = { { Name = "callback", Type = "() -> ()" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0124" },
                Doc = "Registers a function to run while the game shuts down, for work that must finish before "
                    .. "the process ends, such as saving. The functions have a limited time; when it runs out, "
                    .. "shutdown continues whether they have finished or not. Stopping the game in the editor "
                    .. "also runs them.\n\n"
                    .. "These functions are for the whole game. The open scene's `scene:BindToClose` functions "
                    .. "run first, then these, in the order they were registered, each in its own thread. An "
                    .. "error in one does not stop the others. A function registered by a scene's script is "
                    .. "dropped without running when that scene closes; use `scene:BindToClose` for a scene.",
            },
            {
                Name = "SendMessage",
                Parameters = { { Name = "topic", Type = "string" }, { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0124" },
                Doc = "Sends a message to the game: every function bound to `topic` on `game` with "
                    .. "`BindToMessage` runs with the message's values. The functions run deferred, in the order "
                    .. "the messages were sent, each in its own thread. Use it for a scene to tell game-wide code "
                    .. 'something, as in `game:SendMessage("CoinCollected", 1)`.\n\n'
                    .. "The values are copied, as in `Scene:SendMessage`, and cannot include functions or "
                    .. "instances. Messages stay on this machine.",
            },
            {
                Name = "BindToMessage",
                Parameters = {
                    { Name = "topic", Type = "string" },
                    { Name = "callback", Type = "(...any) -> ()" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Binds a function that runs with a message's values each time a message is sent to `game` "
                    .. "under `topic`. The binding belongs to the script that made it: one made by a scene's "
                    .. "script is removed with its scene.",
            },
            {
                Name = "Shutdown",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Ends the game. The functions registered with `BindToClose` get their time to run, and then "
                    .. "the process exits.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Loaded",
                Parameters = {},
                Doc = "Fires once, after every entry script has run for the first time. A handler connected at "
                    .. "the top level of an entry script still receives it. It passes no arguments.",
            },
        } :: { Schema.Event },
        Doc = "The root of the instance tree, available as the global `game`. Every service is a child of it, and "
            .. "every service exists from startup, in a fixed order.",
    },
    {
        Name = "Workspace",
        Extends = "Instance",
        Module = "scene",
        Storage = "Workspace",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        -- `CurrentCamera` arrived with the renderer with the first renderer and `Gravity` with
        -- the simulation with the physics work, as the note here always said they would.
        Doc = "The root of the 3D world, available as the global `workspace` and through `game:GetService`. An "
            .. "instance is in the world when it is a descendant of the workspace.",
        Properties = {
            {
                Name = "Gravity",
                Type = "vector",
                Default = "vector.create(0, -9.81, 0)",
                Doc = "The acceleration applied to every unanchored part, in meters per second squared. It is "
                    .. "signed: the default is negative and points down.",
            },
            {
                Name = "GlobalWind",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Presentation = true,
                Doc = "The direction and speed of the wind, in meters per second. It is visual only: it moves "
                    .. "particles that drift with it, foliage, and surface shaders that read it, and it does not "
                    .. "push physics bodies. Zero, the default, is no wind. `GetWindAt` returns the wind at a "
                    .. "point, including gusts.",
            },
            {
                Name = "WindGusts",
                Type = "number",
                Default = "0",
                Presentation = true,
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much the wind's speed rises and falls in gusts, from 0 (steady) to 1. Gusts travel "
                    .. "downwind at the wind's speed, so one can be seen crossing a field.",
            },
            {
                Name = "WindTurbulence",
                Type = "number",
                Default = "0",
                Presentation = true,
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much the wind's direction varies from place to place, from 0 (uniform) to 1.",
            },
            {
                Name = "ScreenSplit",
                Type = "Enum.ScreenSplit",
                Default = "Enum.ScreenSplit.None",
                Presentation = true,
                Records = { "ADR 0205" },
                Doc = "How the window is divided between the local players on this machine.\n\n"
                    .. "- `None` draws one picture through `CurrentCamera`.\n"
                    .. "- `Columns`, `Rows` and `Grid` give every local player that has a `Camera` its own view, "
                    .. "in the order the players joined: side by side, stacked, or in quarters.\n"
                    .. "- `Dynamic` draws one picture while two players are close together, and splits it along a "
                    .. "line when they move apart (see `SplitBlend` and `SplitAngle`).\n\n"
                    .. "Each view is drawn at the resolution of its part of the window, with everything a full "
                    .. "picture has. The views share the world, its animation and its reflections, and each has "
                    .. "its own culling, shadows, exposure and anti-aliasing.\n\n"
                    .. "While a sequence that takes the camera plays, the window shows one picture; the views "
                    .. "return when it ends. The setting is local to this machine and is not replicated.",
            },
            {
                Name = "SplitBlend",
                Type = "number",
                Default = "0",
                Presentation = true,
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "With `ScreenSplit` set to `Dynamic`: how far the picture is split, from 0 to 1. At 0 the "
                    .. "window shows one picture through `CurrentCamera`. Above 0 the views of the first two "
                    .. "local players are joined along a line through the center of the window. The line is "
                    .. "`SplitLineThickness` thick at 1 and thinner below it. A game sets this value every frame; "
                    .. "`@engine/splitscreen` does it for a top-down game.",
            },
            {
                Name = "SplitAngle",
                Type = "number",
                Default = "0",
                Presentation = true,
                Doc = "With `ScreenSplit` set to `Dynamic`: the direction on screen from the first player to the "
                    .. "second, in degrees. 0 puts the second player to the right of the first, and 90 puts it "
                    .. "above. The dividing line runs perpendicular to this direction, and each player's view is "
                    .. "on that player's side.",
            },
            {
                Name = "SplitLineThickness",
                Type = "number",
                Default = "4",
                Presentation = true,
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The thickness of the line between two views, in pixels of a window 1,080 pixels tall. 0 "
                    .. "draws no line.",
            },
            {
                Name = "SplitLineColor",
                Type = "Color3",
                Default = "Color3.new(0, 0, 0)",
                Presentation = true,
                Doc = "The color of the line between two views.",
            },
            {
                Name = "CurrentCamera",
                Type = "Camera?",
                Doc = "The camera the world is drawn from. With nil, nothing is drawn.",
            },
            {
                Name = "Terrain",
                Type = "Terrain?",
                ReadOnly = true,
                Doc = "The world's terrain, or nil if the world has none. A `Terrain` is an ordinary instance "
                    .. "under the workspace, and this property refers to the one the engine uses as the world's "
                    .. "ground.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetNearestPlayerDistance",
                Parameters = { { Name = "position", Type = "vector" } },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0234" },
                Doc = "Returns the distance in meters from `position` to the nearest player's focus, or "
                    .. "`math.huge` when no player has one. A focus is their `ReplicationFocus`, or their "
                    .. "character, as the physics step last took them. Use it to decide how much a far thing needs to do; "
                    .. "`@engine/npclod` calls an NPC's code by it.",
            },
            {
                Name = "GetWindAt",
                Parameters = { { Name = "position", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns the wind at a point at the current simulation time, in meters per second: "
                    .. "`GlobalWind` with its gusts and turbulence. The renderer uses the same function, so a "
                    .. "flag, a sail or a sound driven by this value matches what is drawn.",
            },
            {
                Name = "Raycast",
                Parameters = {
                    { Name = "origin", Type = "vector" },
                    { Name = "direction", Type = "vector" },
                    { Name = "params", Type = "RaycastParams?", Default = "nil" },
                },
                Returns = { "RaycastResult?" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the nearest thing that a ray from `origin` hits, or nil. The length of `direction` "
                    .. "is the length of the ray, so `direction * 100` casts a ray 100 meters long. When two "
                    .. "surfaces are hit at the same distance, the result is the same on every run.",
            },
            {
                Name = "Raycast2D",
                Parameters = {
                    { Name = "origin", Type = "Vector2" },
                    { Name = "direction", Type = "Vector2" },
                    { Name = "params", Type = "RaycastParams?", Default = "nil" },
                },
                Returns = { "RaycastResult2D?" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the nearest `Part2D` or `Tilemap2D` that a ray on the 2D plane hits, or nil. The "
                    .. "length of `direction` is the length of the ray. Sensors and parts with `CanCollide` off "
                    .. "are ignored. When two hits are at the same distance, the result is the same on every run.",
            },
            {
                Name = "Spherecast",
                Parameters = {
                    { Name = "origin", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "direction", Type = "vector" },
                    { Name = "params", Type = "RaycastParams?", Default = "nil" },
                },
                Returns = { "RaycastResult?" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the nearest thing that a sphere moved along a ray hits, or nil. Use it instead of "
                    .. "`Raycast` for a camera boom or a character's ground check, where a thin ray would pass "
                    .. "through a gap that the object itself does not fit through.",
            },
            {
                Name = "RaycastAsSeenBy",
                Parameters = {
                    { Name = "player", Type = "Player" },
                    { Name = "origin", Type = "vector" },
                    { Name = "direction", Type = "vector" },
                    { Name = "params", Type = "RaycastParams?", Default = "nil" },
                },
                Returns = { "RaycastResult?" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0235" },
                Doc = "Returns what a ray hits in the world as `player`'s machine was drawing it. Every other "
                    .. "player's `Character`, and every part with `BasePart.LagCompensated`, is placed where that "
                    .. "machine drew it, at the moment it was showing; everything else is as it is now. Check a shot with it on the server, and a "
                    .. "player with real latency hits the moving target they aimed at.\n\n"
                    .. "The rewind is at most `rewind_cap_ms` under `[network]` in `project.toml`, 200 by "
                    .. "default: a slower shooter's shot is checked against the world that long ago, so a "
                    .. "target who reached cover more than that long ago is safe. The shooter's own character "
                    .. "is where it is now. Only the server calls it; on a client it raises.",
            },
            {
                Name = "SpherecastAsSeenBy",
                Parameters = {
                    { Name = "player", Type = "Player" },
                    { Name = "origin", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "direction", Type = "vector" },
                    { Name = "params", Type = "RaycastParams?", Default = "nil" },
                },
                Returns = { "RaycastResult?" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0235" },
                Doc = "`RaycastAsSeenBy` for a sphere moved along the ray: a thick projectile, or a melee "
                    .. "swing, checked against what its player saw.",
            },
            {
                Name = "GetBodiesInBox",
                Parameters = {
                    { Name = "cframe", Type = "CFrame" },
                    { Name = "size", Type = "vector" },
                    { Name = "params", Type = "RaycastParams?", Default = "nil" },
                },
                Returns = { "{BasePart}" },
                ThreadSafety = "Unsafe",
                Doc = "Returns every part that overlaps an oriented box, as a new array in a stable order, with "
                    .. "one entry per part. `size` is the full extent of the box, as in `BasePart.Size`.",
            },
            {
                Name = "GetBodiesInSphere",
                Parameters = {
                    { Name = "position", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "params", Type = "RaycastParams?", Default = "nil" },
                },
                Returns = { "{BasePart}" },
                ThreadSafety = "Unsafe",
                Doc = "Returns every part that overlaps a sphere, as a new array in a stable order, with one "
                    .. "entry per part. Use it to find what an explosion reaches or what is within arm's length.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "Lighting",
        Extends = "Instance",
        Module = "render",
        Storage = "Lighting",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Controls the time of day, the sun and the ambient light of the world. The sun is not an instance: "
            .. "its direction is computed from `ClockTime` and `GeographicLatitude`.",
        Properties = {
            {
                Name = "ClockTime",
                Type = "number",
                Doc = "The time of day, in hours from 0 to 24. A value outside the range wraps around, so a "
                    .. "script can add to it every tick. Together with `GeographicLatitude`, it sets the "
                    .. "direction of the sun.",
            },
            {
                Name = "GeographicLatitude",
                Type = "number",
                ErrorKey = "scene.err.number_latitude_degrees",
                Doc = "The latitude of the world, in degrees north of the equator. It tilts the path of the sun "
                    .. "across the sky.",
            },
            {
                Name = "Ambient",
                Type = "Color3",
                Doc = "The uniform light on surfaces in enclosed spaces, such as a cave, a tunnel or the "
                    .. "underside of an overhang, which see none of the sky. A surface that sees part of the sky "
                    .. "is lit by a blend of this and `OutdoorAmbient`. It stands in for bounced light, so a cave "
                    .. "is dim instead of black; lower it for a darker cave. It is not clamped and can exceed 1.",
            },
            {
                Name = "OutdoorAmbient",
                Type = "Color3",
                Doc = "The uniform light on surfaces that see the open sky. It is equal to `Ambient` by default, "
                    .. "so a world that sets neither is lit the same indoors and outdoors. For dark caves, lower "
                    .. "`Ambient` alone. A part is outdoors unless terrain covers it.",
            },
            {
                Name = "Brightness",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The strength of the sun, independent of its color.",
            },
            {
                Name = "FogColor",
                Type = "Color3",
                Doc = "The color that distant objects fade toward between `FogStart` and `FogEnd`. It is kept but "
                    .. "not used while an `Atmosphere` is directly under `Lighting`.",
            },
            {
                Name = "FogStart",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance at which fog begins, in meters. It is kept but not used while an `Atmosphere` "
                    .. "is directly under `Lighting`.",
            },
            {
                Name = "FogEnd",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance at which fog hides everything, in meters. A value equal to or below "
                    .. "`FogStart` turns fog off.",
            },
            {
                Name = "ExposureCompensation",
                Type = "number",
                ErrorKey = "scene.err.number_exposure_stops",
                Doc = "An adjustment to the exposure, in EV stops, added to the automatic exposure. At 0 the "
                    .. "automatic exposure is used as measured; +1 doubles the light and -1 halves it. It is not "
                    .. "clamped.",
            },
            {
                Name = "ExposureMin",
                Type = "number",
                Default = "-2.7369655",
                ErrorKey = "scene.err.number_exposure_stops",
                Doc = "The most the automatic exposure can darken the picture, in EV stops, as the world gets "
                    .. "brighter. The default is about -2.75. Raise it toward 0 to keep bright places bright, so "
                    .. "that snow glares and a desert at noon is hard to look at.",
            },
            {
                Name = "ExposureMax",
                Type = "number",
                Default = "1.5849625",
                ErrorKey = "scene.err.number_exposure_stops",
                Doc = "The most the automatic exposure can brighten the picture, in EV stops, as the world gets "
                    .. "darker. The default is about 1.5, so a night is darker than a day but still visible.\n\n"
                    .. "Lower it for a darker night. At 0 the exposure never brightens, and a world lit a third "
                    .. "as brightly looks a third as bright. Set `ExposureMin` and `ExposureMax` to the same "
                    .. "value to fix the exposure there.",
            },
            {
                Name = "EnvironmentDiffuseScale",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much the sky lights matte surfaces, from 0 to 1. At 0 a surface is lit only by the "
                    .. "sun, by lights and by `Ambient`. The sky itself is drawn the same at any value.",
            },
            {
                Name = "EnvironmentSpecularScale",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much shiny surfaces reflect the sky, from 0 to 1. At 0 metal reflects only the sun's "
                    .. "highlight, which suits an interior that the sky does not reach.",
            },
            {
                Name = "ShadowSoftness",
                Type = "number",
                Default = "0.2",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How soft the edges of the sun's shadows are, from 0 (as sharp as the shadow map allows) to "
                    .. "1 (a penumbra of a quarter of a meter on each side of the edge).",
            },
            {
                Name = "GlobalShadows",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the sun, or the moon at night, casts shadows. Turn it off for the look of an "
                    .. "overcast day or for a stylized world without shadows. Lights that cast shadows still do.",
            },
            {
                Name = "AutoExposure",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the brightness of the picture adapts to what the camera sees, as an eye does when "
                    .. "it moves from a cave into daylight. When it is off, the exposure is fixed, so a darker "
                    .. "world looks darker. `ExposureCompensation` applies in both cases. A machine whose "
                    .. "graphics settings turn automatic exposure off ignores this property.",
            },
            {
                Name = "XRPassthroughOpacity",
                Type = "number",
                Default = "0",
                Presentation = true,
                ErrorKey = "scene.err.number_zero_to_one",
                Records = { "ADR 0247" },
                Doc = "In a headset showing the player's room (`XRService.Passthrough`), how much the room shows "
                    .. "through what the game draws, from 0 to 1. At 0 the game's world is solid and the room is "
                    .. "seen only where nothing is drawn; at 1 only the room is seen. Tween it for a portal or a "
                    .. "fade between the room and the game.",
            },
            {
                Name = "SunDirection",
                Type = "vector",
                ReadOnly = true,
                ThreadSafety = "ReadParallel",
                Doc = "The unit vector that points from the world toward the sun. It is computed only from "
                    .. "`ClockTime` and `GeographicLatitude`, so the same values always give the same direction.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "ReplicatedStorage",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0080" },
        Doc = "A container for instances that every machine has but that are not part of the world: templates to "
            .. "clone, `RemoteEvent`s and other data. Nothing under it is drawn, collides or is simulated. It is "
            .. "saved with the scene, so what the editor puts there is there when the game starts.\n\n"
            .. "In a multiplayer match, its contents replicate to every client at any distance. A client's copy "
            .. 'is replaced by what the authority sends. Get it with `game:GetService("ReplicatedStorage")`, and '
            .. "clone what should appear in the world into `Workspace`.",
    },
    {
        Name = "SaveService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0111" },
        Doc = "Saves data that must outlast a run of the game, such as progress, settings or a best time. Data is "
            .. "kept in named slots. Each slot is a file on the player's own machine, in a folder named after the "
            .. "company and name in the `[project]` settings: on Windows, `%APPDATA%\\<company>\\<name>\\saves`.\n\n"
            .. "That folder is used by the exported game. A run of the project itself, from the editor's Play, "
            .. "from `ludwerk dev` or from the host given the project's folder, saves to `.engine/saves/` inside "
            .. "the project instead, so testing never changes a real player's saves.\n\n"
            .. "This service is the only way a script can write to disk, and it takes slot names, never paths. "
            .. "Saves are not replicated: each machine keeps its own.",
        Properties = {
            {
                Name = "Version",
                Type = "number",
                Default = "1",
                Transient = true,
                HostFact = true,
                ErrorKey = "script.err.save_version",
                Doc = "The version of the game's save format, an integer starting at 1. Set it before the first "
                    .. "call to `GetSlotAsync`. A slot saved by an older version is passed to `OnMigrate` once "
                    .. "and then saved at this version.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetSlotAsync",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "SaveSlot" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Returns the slot called `name`, yielding until it has been read from disk. A name is 1 to "
                    .. "64 letters, digits, `_` and `-`; any other name raises an error.\n\n"
                    .. "A slot that does not exist yet is returned empty. If the slot's file is damaged, the "
                    .. "backup left by the previous save is read instead and a warning is logged. If both are "
                    .. "damaged, the slot is returned empty and its `Recovered` is false.",
            },
            {
                Name = "ListSlots",
                Parameters = {},
                Returns = { "{ string }" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the names of the slots the game has saved, sorted.",
            },
            {
                Name = "DeleteSlot",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Deletes a slot and its backup. A `SaveSlot` object already obtained for it is emptied.",
            },
        } :: { Schema.Method },
        Callbacks = {
            {
                Name = "OnMigrate",
                Parameters = { { Name = "slot", Type = "SaveSlot" }, { Name = "fromVersion", Type = "number" } },
                Returns = {},
                Doc = "Called to update a slot that was saved by an older `Version`. The callback edits the slot "
                    .. "in place. It runs once, in its own thread, before `GetSlotAsync` returns the slot, and "
                    .. "the slot is then saved at the current version. If it raises an error, the error is logged "
                    .. "and the slot is left as it was read.",
            },
        } :: { Schema.Callback },
    },
    {
        Name = "DataService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        LazyService = true,
        Properties = {
            {
                Name = "Version",
                Type = "number",
                Default = "1",
                Transient = true,
                HostFact = true,
                ErrorKey = "script.err.data_version",
                Doc = "The version of the format of the game's records, a whole number from 1. Set it before the "
                    .. "first call to `LoadAsync`. A record written at an older version is passed to `OnMigrate` "
                    .. "once and then written at this version.",
            },
        } :: { Schema.Property },
        Records = { "ADR 0238" },
        Doc = "Keeps each player's record in an online game's database, on the server. The database is the "
            .. "manager's (`servers.toml`), which every world of the game shares; a server run without a manager "
            .. "keeps its own, `data.db` in its saves folder.\n\n"
            .. "A record holds what a `SaveSlot` holds. It is held by one world at a time: the world a player is "
            .. "in. It is written every 30 seconds while it changes, when the player leaves or travels, and "
            .. "when the world stops, so a world that crashes loses at most the last 30 seconds of it.",
        Methods = {
            {
                Name = "LoadAsync",
                Parameters = { { Name = "player", Type = "Player" } },
                Returns = { "DataRecord" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Returns the player's record, empty for a player who has none yet, and holds it for this "
                    .. "world until the player leaves or `Release` is called. Where another world holds it, waits "
                    .. "for that world to write it and let it go, for up to ten seconds, and then raises an error "
                    .. "that begins with `DataBusy`. Raises an error on a player's machine, and in a game that is "
                    .. "not a server. A record already held by this world is returned at once.",
            },
            {
                Name = "TransactAsync",
                Parameters = {
                    {
                        Name = "moves",
                        Type = "{ { Move: number?, Pay: number?, Currency: string?, From: (Player | string)?, "
                            .. "To: (Player | string)? } }",
                    },
                },
                Returns = { "boolean", "string?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0238" },
                Doc = "Moves items and money between owners: all of the moves, or none. A move is `Move`, an "
                    .. "item's id, or `Pay`, a whole amount of `Currency`, from `From` to `To`. An owner is a "
                    .. "`Player` or a name the game gives what no player owns, such as a guild's bank. For money, "
                    .. "a nil owner is the game itself: what it pays comes from nowhere, and what is paid to it is "
                    .. "gone. Returns true, or false and why: `ItemNotThere` where an item is not where a move says "
                    .. "it is, `NotEnough` where a balance would fall below nought. It is on the disk before it "
                    .. "returns, and each move is logged with the transaction and the world.",
            },
            {
                Name = "CreateItemAsync",
                Parameters = {
                    { Name = "owner", Type = "Player | string" },
                    { Name = "kind", Type = "string" },
                    { Name = "fields", Type = "{ [string]: any }?", Default = "nil" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0238" },
                Doc = "Makes an item of `kind` for `owner` and returns its id. Its `fields` are what a save slot "
                    .. "can hold. No id is ever given twice, even after the item is destroyed.",
            },
            {
                Name = "DestroyItemAsync",
                Parameters = { { Name = "id", Type = "number" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0238" },
                Doc = "Destroys an item. Returns false where there is no item with that id.",
            },
            {
                Name = "GetItemsAsync",
                Parameters = { { Name = "owner", Type = "Player | string" } },
                Returns = { "{ { Id: number, Kind: string, Fields: { [string]: any } } }" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0238" },
                Doc = "Returns the items `owner` has, oldest first.",
            },
            {
                Name = "GetBalanceAsync",
                Parameters = { { Name = "owner", Type = "Player | string" }, { Name = "currency", Type = "string" } },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0238" },
                Doc = "Returns how much of `currency` `owner` has: nought where they have never had any.",
            },
            {
                Name = "GetAsync",
                Parameters = { { Name = "scope", Type = "string" }, { Name = "key", Type = "string" } },
                Returns = { "any", "number" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0238" },
                Doc = "Returns a value no player owns, such as an auction's listings or a world event's progress, "
                    .. "and its version. Nil and 0 for a value never written.",
            },
            {
                Name = "SetAsync",
                Parameters = {
                    { Name = "scope", Type = "string" },
                    { Name = "key", Type = "string" },
                    { Name = "value", Type = "any" },
                    { Name = "version", Type = "number" },
                },
                Returns = { "boolean", "number" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0238" },
                Doc = "Writes a shared value, but only while its version is still `version`, the one `GetAsync` "
                    .. "returned. Returns whether it was written, and the version it has now: a world that lost "
                    .. "to another reads it again. The value is what a save slot can hold, of at most 256 KiB "
                    .. "written; nil clears it.",
            },
            {
                Name = "UpdateAsync",
                Parameters = {
                    { Name = "scope", Type = "string" },
                    { Name = "key", Type = "string" },
                    { Name = "transform", Type = "(any) -> any" },
                },
                Returns = { "any" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0238" },
                Doc = "Calls `transform` with a shared value as it is, and writes what it returns, unless another "
                    .. "world wrote first. Then `transform` is called again with what that world wrote. Returns "
                    .. "the value written. The function must not yield, and may run more than once. No lock is "
                    .. "held while it runs.",
            },
        } :: { Schema.Method },
        Callbacks = {
            {
                Name = "OnMigrate",
                Parameters = { { Name = "record", Type = "DataRecord" }, { Name = "fromVersion", Type = "number" } },
                Returns = {},
                Doc = "Called to update a record written at an older `Version`. The callback edits the record "
                    .. "in place. It runs once, in its own thread, before `LoadAsync` returns the record, and the "
                    .. "record is then written at the current version. If it raises an error, the error is logged "
                    .. "and the record is left as it was read.",
            },
        } :: { Schema.Callback },
    },
    {
        Name = "LocalizationService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0154" },
        Doc = "Returns the game's text by key, in the player's language. A project has one catalog per locale, "
            .. "such as `i18n/en.json` and `i18n/pt-BR.json`, each a flat JSON object that maps keys to text. A "
            .. 'script asks for a key: `Translate("menu.play")`.\n\n'
            .. "A key is looked up in the player's locale, then in the project's default locale, then in the "
            .. "engine's own text. If none has it, the key itself is returned and a warning is logged once.\n\n"
            .. "Text properties are plain strings and are not translated automatically. A label shows the string "
            .. "it was given; a game that changes language at run time sets its labels again when `LocaleChanged` "
            .. "fires.\n\n"
            .. "The language is a player setting, like the graphics settings. It is not saved with a scene and is "
            .. "not replicated. On a dedicated server, which has no player, `Translate` returns the text of the "
            .. "default locale, and setting `Locale` raises an error.",
        Properties = {
            {
                Name = "Locale",
                Type = "string",
                Default = '"en"',
                ErrorKey = "scene.err.locale_unknown",
                Transient = true,
                -- Which language a person reads: a fact about the player.
                HostFact = true,
                Doc = "The locale that text is returned in, such as `en` or `pt-BR`. Setting it records the "
                    .. "player's choice: the choice is kept for the next run, and `LocaleChanged` fires.\n\n"
                    .. "The initial value is the first of these that exists: the player's saved choice, the "
                    .. "language of the store or launcher that started the game if a platform provider reports "
                    .. "one, the system language, `[project] default_locale`, and `en`. Each is matched to a "
                    .. "catalog the project has.\n\n"
                    .. "A locale the project has no catalog for is matched to a catalog of the same language, so "
                    .. "`pt` uses `pt-BR`. If there is none, setting it raises an error.",
            },
            {
                Name = "VoiceLocale",
                Type = "string",
                Default = '"en"',
                ErrorKey = "scene.err.voice_locale_unknown",
                Transient = true,
                -- Which language a person hears: a fact about the player.
                HostFact = true,
                -- It reads as the language in force and is written as the
                -- player's choice: choosing the one already heard is kept.
                Choice = true,
                Records = { "ADR 0200" },
                Doc = "The language that voices are played in, which can differ from the language of the text. "
                    .. "When a `Sound` is played, its `Content` is looked for under `l10n/<VoiceLocale>/`. "
                    .. "Reading this property returns the language in effect, which is always one of "
                    .. "`GetVoiceLocales`.\n\n"
                    .. "Set to an empty string, the default, it follows the text language: `Locale` matched to a "
                    .. "language this machine has voices for, or else the first system language it has voices "
                    .. "for, or else the project's default. `VoiceFollowsLocale` reports whether it is following.\n\n"
                    .. "Setting a language records the player's choice, which is kept for the next run. A "
                    .. "language this machine cannot play is matched to one of the same language, so `pt` uses "
                    .. "`pt-BR`; if there is none, setting it raises an error. A change takes effect at the next "
                    .. "`Play`: a sound that is playing finishes in the language it started in.",
            },
            {
                Name = "VoiceFollowsLocale",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "Whether the voice language follows the text language. It is true until a language is set "
                    .. "on `VoiceLocale`, and true again after `VoiceLocale` is set to an empty string. A "
                    .. 'settings menu can use it to show "same as text" as the current choice.',
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetLocales",
                Parameters = {},
                Returns = { "{ string }" },
                Doc = "Returns the locales the project has a catalog for, in order. Use it to fill a language menu.",
            },
            {
                Name = "GetVoiceLocales",
                Parameters = {},
                Returns = { "{ string }" },
                Records = { "ADR 0200" },
                Doc = "Returns the languages this machine can play voices in, with the project's default first "
                    .. "and the others sorted. The others are the languages under `l10n/` that the game shipped "
                    .. "with or that an installed language pack added. A menu built from the list never offers a "
                    .. "voice language that is not installed.",
            },
            {
                Name = "GetSystemLocales",
                Parameters = {},
                Returns = { "{ string }" },
                Records = { "ADR 0200" },
                Doc = "Returns the system's preferred languages, most preferred first, as the platform reports "
                    .. 'them, whether or not the game supports them. Use it to show "your system\'s language" in a '
                    .. "menu. Returns an empty list on a dedicated server.",
            },
            {
                Name = "Translate",
                Parameters = {
                    { Name = "key", Type = "string" },
                    { Name = "arguments", Type = "{ [string]: any }?" },
                },
                Returns = { "string" },
                Doc = "Returns the text for `key` in the current `Locale`. Each `{name}` in the text is replaced "
                    .. "by `arguments.name`, which can be a string, a number or a boolean. If no catalog has the "
                    .. "key, the key itself is returned and a warning is logged once.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "LocaleChanged",
                Parameters = { { Name = "locale", Type = "string" } },
                Doc = "Fires when `Locale` changes. During development, it also fires when a catalog file is "
                    .. "edited and reloaded. Set the text of the game's labels in its handler.",
            },
            {
                Name = "VoiceLocaleChanged",
                Parameters = { { Name = "locale", Type = "string" } },
                Records = { "ADR 0200" },
                Doc = "Fires when the language that voices are played in changes, with the new `locale`. That "
                    .. "happens when a script sets `VoiceLocale`, or when the text language changes and the voice "
                    .. "language follows it.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "GraphicsService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0147" },
        Doc = "The graphics and display settings of this machine: the quality of the picture, the window and the "
            .. "frame rate. Use it to build an options menu.\n\n"
            .. "The value of a setting is taken from the first of these that provides one: a command-line flag "
            .. "the game was started with, a value a script set in this session, the player's saved choice, the "
            .. "project's `project.toml`, the current quality level, and the engine's default. `GetSource` "
            .. "returns which one. A change takes effect at the next frame, and several changes in one frame are "
            .. "applied together.\n\n"
            .. "These settings describe the machine, not the world. They are not saved with a scene, are not "
            .. "replicated, and do not affect the simulation. `SaveAsync` saves them for this player, and the "
            .. "game starts with the saved values. A dedicated server has no display: it reads the defaults, and "
            .. "setting a value there raises an error.",
        Properties = {
            {
                Name = "QualityLevel",
                Type = "Enum.GraphicsQuality",
                Default = "Enum.GraphicsQuality.High",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The overall quality level. Setting it applies that level to every quality setting, like "
                    .. "`ApplyPreset`. It reads `Custom` when any quality setting differs from the level's value. "
                    .. "`Custom` cannot be set.",
            },
            {
                Name = "RenderScale",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The fraction of the window's resolution the world is drawn at, from one third to 1. The "
                    .. "interface is always drawn at the window's full resolution.",
            },
            {
                Name = "ShadowQuality",
                Type = "Enum.ShadowQuality",
                Default = "Enum.ShadowQuality.High",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The quality of the sun's shadows, as a level. Setting it sets `ShadowResolution` and "
                    .. "`ShadowCascades` to that level's values. `Off` uses no cascades.",
            },
            {
                Name = "ShadowResolution",
                Type = "number",
                Default = "2048",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The side of one shadow cascade, in texels, from 256 to 2048. It is rounded down to a power "
                    .. "of two.",
            },
            {
                Name = "ShadowCascades",
                Type = "number",
                Default = "4",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The number of cascades in the sun's shadow, from 0 to 4. At 0 the sun casts no shadows.",
            },
            {
                Name = "ShadowDistance",
                Type = "number",
                Default = "120",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The distance from the camera within which the sun casts shadows, in meters, from 10 to 1000.",
            },
            {
                Name = "AntiAliasing",
                Type = "Enum.AntiAliasingMode",
                Default = "Enum.AntiAliasingMode.SMAA",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0158" },
                Doc = "The method used to smooth edges.",
            },
            {
                Name = "Upscaling",
                Type = "Enum.UpscalingMode",
                Default = "Enum.UpscalingMode.None",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0158", "ADR 0164" },
                Doc = "The method used to scale the world up to the window's resolution when `RenderScale` is "
                    .. "below 1. At a scale of 1, `FSR1` does nothing and `FSR2` acts only as anti-aliasing.",
            },
            {
                Name = "Sharpness",
                Type = "number",
                Default = "0.2",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0158" },
                Doc = "How much the picture is sharpened, from 0 to 1. Sharpening is applied after an FSR 1 "
                    .. "upscale, after temporal anti-aliasing, and as part of FSR 2.",
            },
            {
                Name = "AmbientOcclusion",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether creases and corners are darkened, where little ambient light reaches.",
            },
            {
                Name = "ContactShadows",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether the sun's shadows reach right up to the object that casts them.",
            },
            {
                Name = "Bloom",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether bright areas glow.",
            },
            {
                Name = "DepthOfField",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether a world's `DepthOfFieldEffect` is drawn. When it is off, the picture is sharp even "
                    .. "in a scene that has the effect.",
            },
            {
                Name = "SunRays",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether a world's `SunRaysEffect` is drawn. When it is off, no shafts are drawn even in a "
                    .. "scene that has the effect.",
            },
            {
                Name = "AutoExposure",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether the brightness of the picture adapts to the scene. When it is off, the exposure is "
                    .. "fixed.",
            },
            {
                Name = "LightBudget",
                Type = "number",
                Default = "256",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The largest number of lights drawn in one frame.",
            },
            {
                Name = "TerrainDetail",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "A multiplier on the distance at which distant terrain is drawn in finer detail, relative "
                    .. "to the quality level's own value. At 2 the detail extends twice as far, and at 0.5 half "
                    .. "as far.",
            },
            {
                Name = "FoliageDensity",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The fraction of the foliage of every `FoliageLayer` that is drawn, from 0 to 1. It "
                    .. "multiplies the project's own foliage density.",
            },
            {
                Name = "FoliageDecals",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether decals are drawn on the foliage standing in them (see "
                    .. "`FoliageLayer.ReceivesDecals`). When it is off, a decal appears on the ground under the "
                    .. "foliage but not on the foliage, which is cheaper. It is off at the `Low` quality level, "
                    .. "and below `High` on a handheld device.",
            },
            {
                Name = "AnimationDetail",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "A multiplier, from 0.25 to 8, on how often small skinned meshes are posed.\n\n"
                    .. "At 1, a character is posed every tick while it is at least 12% of the picture's height, "
                    .. "every second tick down to 6%, every fourth down to 3%, and every eighth below that. At 2 "
                    .. "the thresholds are 6%, 3% and 1.5%, and at 4 they are halved again. Raise it for a game "
                    .. "whose camera is far from a crowd, since a crowd posed every fourth tick moves in visible "
                    .. "steps. Higher values cost more.",
            },
            {
                Name = "ViewDistance",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "A multiplier on the distance at which objects are drawn, from 0.25 to 4. This setting is "
                    .. "saved and reported but has no effect yet.",
            },
            {
                Name = "TextureQuality",
                Type = "Enum.TextureQuality",
                Default = "Enum.TextureQuality.High",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How much of each texture is loaded. `High` loads the full size, `Medium` half the size (a "
                    .. "quarter of the memory), and `Low` a quarter of the size (a sixteenth of the memory).\n\n"
                    .. "The initial value depends on the machine's memory, whatever the quality level: `Low` "
                    .. "below 3 GB, `Medium` below 5 GB and `High` above. A game or a player can set another "
                    .. "value.\n\n"
                    .. "It applies to compiled content: material maps, the images in model files, decals and "
                    .. "particles. Sprites, tilesets, block faces and interface images are always drawn at their "
                    .. "own size, and no texture is reduced below 64 pixels. A texture that is already loaded "
                    .. "keeps its size until it is loaded again, so set this before loading the scene that should "
                    .. "use it.",
            },
            {
                Name = "AnisotropicFiltering",
                Type = "number",
                Default = "8",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How sharp textures stay when seen at a shallow angle, from 1 to 16. This setting is saved "
                    .. "and reported but has no effect yet.",
            },
            {
                Name = "LODBias",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "A multiplier on the distance at which a mesh changes level of detail. This setting is "
                    .. "saved and reported but has no effect yet.",
            },
            {
                Name = "MaximumLODLevel",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The finest level of detail a mesh can be drawn at; 0 is the mesh's finest. This setting is "
                    .. "saved and reported but has no effect yet.",
            },
            {
                Name = "ParticleBudget",
                Type = "number",
                Default = "65536",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The largest number of particles alive at once. This setting is saved and reported but has "
                    .. "no effect yet.",
            },
            {
                Name = "SoftParticles",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether particles fade where they meet a surface. This setting is saved and reported but "
                    .. "has no effect yet.",
            },
            {
                Name = "SkinWeights",
                Type = "number",
                Default = "4",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The number of bones that can move one vertex, from 1 to 4. This setting is saved and "
                    .. "reported but has no effect yet.",
            },
            {
                Name = "TextureStreamingBudget",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The memory that textures can keep loaded, in mebibytes; 0 is no limit. This setting is "
                    .. "saved and reported but has no effect yet.",
            },
            {
                Name = "AsyncUploadBudget",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The time a frame can spend uploading data to the GPU, in milliseconds. This setting is "
                    .. "saved and reported but has no effect yet.",
            },
            {
                Name = "MotionBlur",
                Type = "boolean",
                Default = "false",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether motion is blurred. This setting is saved and reported but has no effect yet.",
            },
            {
                Name = "FogQuality",
                Type = "Enum.GraphicsLevel",
                Default = "Enum.GraphicsLevel.High",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How fog is drawn. This setting is saved and reported but has no effect yet.",
            },
            {
                Name = "GlobalIllumination",
                Type = "Enum.GraphicsLevel",
                Default = "Enum.GraphicsLevel.High",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0203" },
                Doc = "Whether bounced light is computed. At `High` and above, each `IrradianceVolume` is "
                    .. "computed and used. Below `High`, a surface inside a volume is lit as it is outside one: "
                    .. "by the ambient light, or by the light of a `ReflectionProbe`. On a phone, every quality "
                    .. "level starts below `High`.",
            },
            {
                Name = "GlobalIlluminationBudget",
                Type = "number",
                Default = "1.5",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0203" },
                Doc = "The time per frame that computing bounced light can take during play, in milliseconds, "
                    .. "from 0.25 to 16. The default is 1.5 unless a quality level or the game sets another.\n\n"
                    .. "The budget is spent on computing each `IrradianceVolume` and on updating `Dynamic` "
                    .. "volumes when the lighting changes, starting with the volume nearest the camera. With a "
                    .. "lower value, the light follows a moving sun more slowly. A volume is always shown lit by "
                    .. "one consistent moment, never partly updated. Behind a loading screen, the whole frame is "
                    .. "used regardless of this value.",
            },
            {
                Name = "Reflections",
                Type = "Enum.GraphicsLevel",
                Default = "Enum.GraphicsLevel.High",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0202" },
                Doc = "What shiny surfaces reflect. At `Low` they reflect the sky, and no `ReflectionProbe` is "
                    .. "captured. `Medium` keeps eight probes at 64 texels per side, `High` keeps sixteen at 128, "
                    .. "and `Ultra` keeps thirty-two. A phone keeps half as many, and none below `High`. When a "
                    .. "scene has more probes than are kept, the ones farthest from the camera are not used, and "
                    .. "surfaces there reflect the sky.",
            },
            {
                Name = "ScreenSpaceReflections",
                Type = "Enum.GraphicsLevel",
                Default = "Enum.GraphicsLevel.Low",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0209" },
                Doc = "Whether smooth surfaces reflect what is on screen. It shows a character standing on a "
                    .. "polished floor, an object that moved after a `ReflectionProbe` was captured, or the far "
                    .. "bank of a lake.\n\n"
                    .. "`Low` and `Medium` are off. `High` traces each pixel's reflection ray for 24 steps, and "
                    .. "`Ultra` for 48. Where the screen holds no answer, the surface falls back to what "
                    .. "`Reflections` provides. Only the `Ultra` quality preset turns this setting on, and on a "
                    .. "phone it starts off at every level.",
            },
            {
                Name = "ScreenSpaceBounce",
                Type = "Enum.GraphicsLevel",
                Default = "Enum.GraphicsLevel.Low",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0209" },
                Doc = "Whether light bounces off what is on screen. It adds fine detail that an "
                    .. "`IrradianceVolume` is too coarse for, such as the red of a wall on the floor beside it.\n\n"
                    .. "`Low` and `Medium` are off. `High` casts four rays per pixel at half resolution, and "
                    .. "`Ultra` eight, accumulated over a few frames. The result replaces the matching share of "
                    .. "the volume's light or the ambient light, so no light is counted twice. Only the `Ultra` "
                    .. "quality preset turns this setting on, and on a phone it starts off at every level.",
            },
            {
                Name = "RenderResolutionCap",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The largest number of pixels along the shorter side that the world is drawn at; 0 means no "
                    .. "limit. The quality levels of a handheld device set it, because a phone's display has more "
                    .. "pixels than are worth drawing.",
            },
            {
                Name = "WindowMode",
                Type = "Enum.WindowMode",
                Default = "Enum.WindowMode.Windowed",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether the game is windowed, borderless or fullscreen. A handheld device always uses the "
                    .. "whole display.",
            },
            {
                Name = "Resolution",
                Type = "Vector2",
                Default = "Vector2.zero",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The size of the window in pixels when windowed, or the display mode when fullscreen. Zero "
                    .. "means the project's window size when windowed, and the display's native mode when "
                    .. "fullscreen.",
            },
            {
                Name = "Monitor",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The display the window is on, as an index into the list that `GetMonitors` returns.",
            },
            {
                Name = "VSync",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether each frame waits for the display's refresh. On by default, and always on for a "
                    .. "handheld device.",
            },
            {
                Name = "MaxFrameRate",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The highest frame rate, in frames per second; 0 means no limit. With `VSync` off it is the "
                    .. "only limit. With `VSync` on, it can only limit the rate below the display's refresh rate.",
            },
            {
                Name = "BackgroundFrameRate",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The highest frame rate while the window is in the background or minimized, in frames per "
                    .. "second; 0 means no limit.",
            },
            {
                Name = "FrameGeneration",
                Type = "boolean",
                Default = "false",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0165" },
                Doc = "Whether an extra frame is generated between every two drawn frames, using AMD FSR 3 frame "
                    .. "generation. Twice as many frames are shown for the same rendering cost. Each drawn frame "
                    .. "is shown half a frame later, which adds input latency; leave it off for a game where "
                    .. "reaction time matters.\n\n"
                    .. "The interface is drawn on every frame shown and is never interpolated. It applies to the "
                    .. "main view of a perspective camera, on a device with compute shaders, and not on Metal; "
                    .. "elsewhere frames are shown as drawn. While it is on, frames wait for the display's "
                    .. "refresh whatever `VSync` is set to. No quality level changes this setting.",
            },
            {
                Name = "Brightness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "An offset on the gamma of the picture, from -1 to 1. This setting is saved and reported "
                    .. "but has no effect yet.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "ApplyPreset",
                Parameters = { { Name = "level", Type = "Enum.GraphicsQuality" } },
                Returns = {},
                Doc = "Sets every quality setting to the value that `level` gives it, replacing whatever a "
                    .. "script, the player or the project set. `Auto` selects the level that was measured for "
                    .. "this machine. `Custom` raises an error. The display settings, such as the window, `VSync` "
                    .. "and the frame rate limits, are not part of a level and are not changed.",
            },
            {
                Name = "ResetToDefaults",
                Parameters = {},
                Returns = {},
                Doc = "Discards the values a script set and the values the player saved, so that the project's "
                    .. "defaults apply. The player's saved file is not changed until `SaveAsync` is called.",
            },
            {
                Name = "GetSource",
                Parameters = { { Name = "setting", Type = "string" } },
                Returns = { "Enum.SettingSource" },
                Doc = "Returns where the value of a setting comes from. `setting` is the name of a property, such "
                    .. 'as `"VSync"`. Raises an error for a name that is not a setting.',
            },
            {
                Name = "IsApplied",
                Parameters = { { Name = "setting", Type = "string" } },
                Returns = { "boolean" },
                Doc = "Returns whether a setting has an effect yet. A few settings are saved and reported before "
                    .. "the renderer uses them; use this method to hide or disable them in a menu.",
            },
            {
                Name = "GetGroupLevel",
                Parameters = { { Name = "group", Type = "Enum.GraphicsGroup" } },
                Returns = { "Enum.GraphicsLevel?" },
                Doc = "Returns the level of a group of settings: the level whose values every setting in the "
                    .. "group has. Returns nil if the settings do not all match one level.",
            },
            {
                Name = "SetGroupLevel",
                Parameters = {
                    { Name = "group", Type = "Enum.GraphicsGroup" },
                    { Name = "level", Type = "Enum.GraphicsLevel" },
                },
                Returns = {},
                Doc = "Sets every setting in `group` to the value that `level` gives it.",
            },
            {
                Name = "GetSupportedResolutions",
                Parameters = {},
                Returns = { "{ Vector2 }" },
                Doc = "Returns the fullscreen resolutions that the window's display supports, largest first. "
                    .. "Returns an empty list where there is no display.",
            },
            {
                Name = "GetMonitors",
                Parameters = {},
                Returns = { "{ { Name: string, Size: Vector2, RefreshRate: number } }" },
                Doc = "Returns the displays, in the order that `Monitor` indexes them.",
            },
            {
                Name = "GetRefreshRate",
                Parameters = {},
                Returns = { "number" },
                Doc = "Returns the refresh rate of the display the window is on, in hertz, or 0 if it is unknown.",
            },
            {
                Name = "SaveAsync",
                Parameters = {},
                Returns = { "boolean" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Saves the player's settings to `settings.json` in the player's own folder: the previously "
                    .. "saved values, updated with what a script has set since. Yields until the file is written, "
                    .. "at the end of the frame, and returns whether it was. After that, `GetSource` returns "
                    .. "`Player` for those settings.\n\n"
                    .. "Returns false where there is nowhere to write: in a game that turned `[display] "
                    .. "remember_player_settings` off, or in a run that has no folder of its own.",
            },
            {
                Name = "LoadAsync",
                Parameters = {},
                Returns = { "boolean" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Reloads the player's saved settings and discards what a script has set since. Use it for a "
                    .. "menu's Revert button. Yields until it is done, at the end of the frame, and returns "
                    .. "whether there was a file to read.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "QualityChanged",
                Parameters = {},
                Doc = "Fires when the quality level or any quality setting changes, whatever changed it. It fires "
                    .. "at most once per frame.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "XRService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0245" },
        Doc = "Where the player is in a headset: the head and the two hands. A controller's buttons, triggers "
            .. "and sticks are not here: they are `Enum.KeyCode` values whose names begin with `XR`, bound to "
            .. "`InputAction`s beside a gamepad's, and a controller vibrates through `HapticService` with "
            .. "`Enum.UserInputType.XRLeft` or `XRRight`.\n\n"
            .. "Poses are in the play space: meters from the floor where the player stood when the session "
            .. "began, y up. `Workspace.CurrentCamera` places the play space in the world: the camera's "
            .. "`CFrame` is the play space's origin, and the head moves the view from there. A game moves "
            .. "its player by moving the camera, never the head.\n\n"
            .. "With no headset, `Enabled` is false and every pose is the identity: a game that plays both "
            .. "ways branches on `Enabled` once, where it places its camera. A script reads the poses of the "
            .. "tick it runs in; the picture is drawn from later ones, at the time each frame is shown.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "Whether a headset is drawing this game. False on the screen, on a server and in a "
                    .. "headless run without `--xr=simulated`.",
            },
            {
                Name = "Foveation",
                Type = "Enum.XRFoveation",
                Default = "Enum.XRFoveation.Off",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0250" },
                Doc = "How much less each eye is shaded away from where it looks, on a GPU with a fragment density map: `[xr] foveation`. Nothing on a GPU without one. Read, what the session runs with: the project's `[xr]` value until a script sets it. Set, it is applied at the next frame.",
            },
            {
                Name = "Msaa",
                Type = "number",
                Default = "1",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0250" },
                Doc = "Samples a pixel each eye is drawn with: 1 (none), 2, 4 or 8, `[xr] msaa`. Where a frame needs its picture or its depth partway through, or the headset reads the eye's depth, it is drawn with one. A change makes its targets again: a hitch at the switch. Read, what the session runs with: the project's `[xr]` value until a script sets it. Set, it is applied at the next frame.",
            },
            {
                Name = "RefreshRate",
                Type = "number",
                Default = "0",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0250" },
                Doc = "The display's rate in hertz the game asks for, the nearest the headset offers: `[xr] refresh_rate`. 0 is the headset's own. Read, what the session runs with: the project's `[xr]` value until a script sets it. Set, it is applied at the next frame.",
            },
            {
                Name = "RenderScale",
                Type = "number",
                Default = "1",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0250" },
                Doc = "The fraction of its pixels each eye is drawn at, 0.5 to 1, brought up to them: `[xr] render_scale`. A change makes its targets again: a hitch at the switch. Read, what the session runs with: the project's `[xr]` value until a script sets it. Set, it is applied at the next frame.",
            },
            {
                Name = "ScreenEffects",
                Type = "boolean",
                Default = "false",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0250" },
                Doc = "Whether occlusion, contact shadows and the screen's reflections are drawn in the eyes: `[xr] screen_effects`. Read, what the session runs with: the project's `[xr]` value until a script sets it. Set, it is applied at the next frame.",
            },
            {
                Name = "ShadowCascades",
                Type = "number",
                Default = "0",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0250" },
                Doc = "How many cascades the sun's shadows are, 1 to 4, or 0 for the quality level's: `[xr] shadow_cascades`. Read, what the session runs with: the project's `[xr]` value until a script sets it. Set, it is applied at the next frame.",
            },
            {
                Name = "ShadowDistance",
                Type = "number",
                Default = "0",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0250" },
                Doc = "How far the sun's shadows reach in the eyes, in meters, or 0 for the quality level's: `[xr] shadow_distance`. Read, what the session runs with: the project's `[xr]` value until a script sets it. Set, it is applied at the next frame.",
            },
            {
                Name = "ShadowFirstSplit",
                Type = "number",
                Default = "2",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0250" },
                Doc = "How many meters the sun's first cascade covers in the eyes: `[xr] shadow_first_split`. Read, what the session runs with: the project's `[xr]` value until a script sets it. Set, it is applied at the next frame.",
            },
            {
                Name = "ShadowResolution",
                Type = "number",
                Default = "0",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0250" },
                Doc = "Texels a cascade of the sun's shadows, 256 to 2048, or 0 for the quality level's: `[xr] shadow_resolution`. A change makes the shadow map again: a hitch at the switch. Read, what the session runs with: the project's `[xr]` value until a script sets it. Set, it is applied at the next frame.",
            },
            {
                Name = "Passthrough",
                Type = "boolean",
                Default = "false",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0247" },
                Doc = "Whether the player's real room is seen through the headset behind what the game draws. "
                    .. "The room shows wherever the game draws nothing, and the sky is not drawn. "
                    .. "`Lighting.XRPassthroughOpacity` fades the game's world over it. Nothing on a headset whose "
                    .. "runtime has no passthrough (`SupportsPassthrough`).",
            },
            {
                Name = "PointerHand",
                Type = "Enum.XRHand",
                Default = "Enum.XRHand.Right",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0246" },
                Doc = "Which hand points at interfaces in a headset. Its ray finds the panels the screens become "
                    .. "and every `SurfaceGui` and `BillboardGui`, and its trigger, or a pinch of its fingers, is a "
                    .. "click where the ray meets one.",
            },
            {
                Name = "PointerVisible",
                Type = "boolean",
                Default = "true",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0246" },
                Doc = "Whether the pointing hand's ray is drawn: a short beam ending in a dot where it meets an "
                    .. "interface. It points, and clicks, either way.",
            },
            {
                Name = "PointerLength",
                Type = "number",
                Default = "5",
                Transient = true,
                HostFact = true,
                ErrorKey = "scene.err.number_positive",
                Records = { "ADR 0246" },
                Doc = "How far the pointing hand reaches, in meters, and how long its beam is when it meets "
                    .. "nothing.",
            },
            {
                Name = "Space",
                Type = "Enum.XRSpace",
                Default = "Enum.XRSpace.Standing",
                Transient = true,
                HostFact = true,
                Doc = "Where the poses are measured from: the floor (`Standing`) or the head where the session "
                    .. "began (`Seated`). A game that is played sitting down sets `Seated` once, at the start. "
                    .. "The picture uses the same space, so the camera stands for the floor when standing and "
                    .. "for the player's eyes when seated.",
            },
            {
                Name = "SessionState",
                Type = "Enum.XRSessionState",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "What the headset is doing with this game: `Focused` while the player plays, `Visible` "
                    .. "while the headset's own menu is in front, and `None` with no headset. "
                    .. "`SessionStateChanged` fires when it changes; a game pauses on anything but `Focused`.",
            },
        },
        Methods = {
            {
                Name = "GetHeadCFrame",
                Parameters = {},
                Returns = { "CFrame" },
                ThreadSafety = "Unsafe",
                Doc = "Where the head is in the play space, and how it is turned: between the eyes, looking "
                    .. "down its -Z. The identity with no headset.",
            },
            {
                Name = "GetHandCFrame",
                Parameters = { { Name = "hand", Type = "Enum.XRHand" } },
                Returns = { "CFrame" },
                ThreadSafety = "Unsafe",
                Doc = "Where a hand holds its controller in the play space, and how it is turned. The identity "
                    .. "with no headset, and while the hand is not tracked (`IsHandTracked`).",
            },
            {
                Name = "IsHandTracked",
                Parameters = { { Name = "hand", Type = "Enum.XRHand" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Whether the headset knows where this hand is now: false when its controller is put "
                    .. "down out of sight, or asleep.",
            },
            {
                Name = "SupportsPassthrough",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0247" },
                Doc = "Returns whether this headset can show the player's room behind the game (`Passthrough`). "
                    .. "False with no headset.",
            },
            {
                Name = "GetRoomAsync",
                Parameters = {},
                Returns = { "{ Planes: { { Kind: Enum.XRPlaneKind, CFrame: CFrame, Size: vector } } }?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0247" },
                Doc = "Returns the player's room as the headset knows it: its floor, ceiling, walls, tables and "
                    .. "the rest, each a rectangle in the play space. A plane's `CFrame` is its middle, the "
                    .. "rectangle across its x and y and facing along its z, into the room; `Size` is its width "
                    .. "and height. It waits for the headset, which may first ask the player to let the game see "
                    .. "the room. Nil with no headset, where the headset knows no room, or when the player says no.",
            },
            {
                Name = "CreateAnchorAsync",
                Parameters = { { Name = "cframe", Type = "CFrame" } },
                Returns = { "XRAnchor?" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0247" },
                Doc = "Pins a place in the player's room, given in the play space, and returns its anchor. It "
                    .. "waits for the headset. Nil with no headset, or where the headset has no anchors.",
            },
            {
                Name = "LoadAnchorsAsync",
                Parameters = {},
                Returns = { "{ XRAnchor }" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0247" },
                Doc = "Returns every anchor this game saved on the headset with `XRAnchor:SaveAsync`. An anchor "
                    .. "saved in another room is returned too, and is not `Tracked` until the player is back in "
                    .. "it. It waits for the headset. Empty with no headset.",
            },
            {
                Name = "CreateRoomParts",
                Parameters = {},
                Returns = { "Folder?" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0247" },
                Doc = "Makes the room `GetRoomAsync` last returned solid, and returns the `Folder` named `XRRoom` "
                    .. "it puts in the `Workspace`. It holds an anchored part for each plane, not drawn and casting "
                    .. "no shadow. Physics, raycasts and navigation then use the real room. Each part is a slab "
                    .. "behind its plane, so its face toward the room is where the plane is. The parts are placed "
                    .. "where the play space is in the world now, the current camera's `CFrame`. A folder made "
                    .. "before is replaced. Nil before a room was returned.",
            },
            {
                Name = "GetHandJoints",
                Parameters = { { Name = "hand", Type = "Enum.XRHand" } },
                Returns = { "{ CFrame }?" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the 26 joints of a hand tracked as a hand, with no controller in it, in the play "
                    .. "space, or nil. The order is OpenXR's: the palm, the wrist, then each finger from the thumb "
                    .. "out, from its root to its tip. A hand tracked this way is also held at its palm "
                    .. "(`GetHandCFrame`), and a pinch of its thumb and index finger is the `XRPinchLeft` or "
                    .. "`XRPinchRight` key. `[xr] hands = false` in `project.toml` turns hands off.",
            },
            {
                Name = "Recenter",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Makes where the head is now the play space's origin, facing where it faces. When "
                    .. "`Space` is `Standing` the origin stays at the floor's height. The poses after it, and the "
                    .. "picture, are measured from there. Nothing with no headset.",
            },
            {
                Name = "GetPlayAreaBounds",
                Parameters = {},
                Returns = { "vector?" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the size of the play area the player set up on the floor, or nil where the "
                    .. "headset knows none. The width is x and the depth is z, in meters, centered on the "
                    .. "standing space's origin.",
            },
            {
                Name = "SimulatePose",
                Parameters = {
                    { Name = "pose", Type = "Enum.XRPose" },
                    { Name = "cframe", Type = "CFrame" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Puts the simulated headset's head or a hand where `cframe` says, from the next frame. "
                    .. "The pose is in the play space. A test uses it to move the player's head and hands from a "
                    .. "script. An index fingertip puts that hand down as a hand tracked with no controller, its "
                    .. "finger pointing along `cframe`'s look vector. Only with `--xr=simulated`; anywhere else "
                    .. "it raises an error.",
            },
            {
                Name = "SimulateInput",
                Parameters = {
                    { Name = "keyCode", Type = "Enum.KeyCode" },
                    { Name = "value", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0246" },
                Doc = "Holds one of the simulated headset's controls at `value` from the next frame: 1 for a "
                    .. "button down and 0 up, a trigger from 0 to 1, a stick's axis from -1 to 1. The key code is "
                    .. "one whose name begins with `XR`. Only with `--xr=simulated`; anywhere else it raises an "
                    .. "error.",
            },
            {
                Name = "GetPointerRay",
                Parameters = { { Name = "hand", Type = "Enum.XRHand" } },
                Returns = { "vector?", "vector?" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0246" },
                Doc = "Returns where a hand points from and the way it points, in the world, as a unit vector: "
                    .. "the ray a game casts for its own pointing. Nil and nil while the hand is not tracked, and "
                    .. "with no headset.",
            },
        },
        Events = {
            {
                Name = "UserCFrameChanged",
                Parameters = {
                    { Name = "pose", Type = "Enum.XRPose" },
                    { Name = "cframe", Type = "CFrame" },
                },
                Doc = "Fires when the head or a hand moves, once a frame for each that moved, with where it is "
                    .. "now in the play space.",
            },
            {
                Name = "SessionStateChanged",
                Parameters = { { Name = "state", Type = "Enum.XRSessionState" } },
                Doc = "Fires when `SessionState` changes.",
            },
        },
    },
    {
        Name = "HapticService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0131" },
        Doc = "Controls vibration: the motors of a gamepad and the vibration of a phone.\n\n"
            .. "`SetMotor` runs a motor at a level until it is set again, and 0 stops it. An engine that rumbles "
            .. "while it runs needs one call when it starts and one when it stops. `Vibrate` makes a phone "
            .. "vibrate for a short time and stop by itself.\n\n"
            .. "A headset's controller vibrates under `Enum.UserInputType.XRLeft` or `XRRight`. It has "
            .. "one motor, which `SetMotor` runs whichever motor is named.\n\n"
            .. "Every motor stops while the game's window is not in front, resumes when it is again, and stops "
            .. "when the game closes. On a dedicated server or in a headless run, every query returns false and "
            .. "every call is ignored.",
        Methods = {
            {
                Name = "IsVibrationSupported",
                Parameters = { { Name = "inputType", Type = "Enum.UserInputType" } },
                Returns = { "boolean" },
                Doc = "Returns whether a kind of device can vibrate on this machine at present. Pass "
                    .. "`Enum.UserInputType.Gamepad` for a connected gamepad with motors, or "
                    .. "`Enum.UserInputType.Touch` for the device itself, such as a phone. Returns false for any "
                    .. "other value.",
            },
            {
                Name = "IsMotorSupported",
                Parameters = {
                    { Name = "inputType", Type = "Enum.UserInputType" },
                    { Name = "motor", Type = "Enum.VibrationMotor" },
                },
                Returns = { "boolean" },
                Doc = "Returns whether a particular motor exists. Most gamepads have `Large` and `Small`, and a "
                    .. "few have motors in their triggers. A phone has none of these; use `Vibrate` for it.",
            },
            {
                Name = "SetMotor",
                Parameters = {
                    { Name = "inputType", Type = "Enum.UserInputType" },
                    { Name = "motor", Type = "Enum.VibrationMotor" },
                    { Name = "value", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Runs one motor of every connected gamepad at `value`, from 0 (stopped) to 1 (full "
                    .. "strength), until it is set again. A motor that does not exist is ignored, so there is no "
                    .. "need to check first.",
            },
            {
                Name = "Vibrate",
                Parameters = {
                    { Name = "strength", Type = "number" },
                    { Name = "seconds", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Vibrates the device itself, such as a phone, at `strength` (0 to 1) for `seconds`, up to "
                    .. "5. It is ignored on a machine that cannot vibrate.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "CrossWorldService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0223" },
        Doc = "Sends a game's events between its worlds: a guild's chat, a world boss that appears for "
            .. "everyone, an operator's announcement. A world publishes on a topic, and every world that "
            .. "subscribes to it hears it, the publisher included, in the order the manager received them.\n\n"
            .. "Under a manager (`servers.toml`) a message crosses worlds; a world on its own hears its own, so "
            .. "the same code runs tested on one machine. Delivery is at most once and not guaranteed: a world "
            .. "that is restarting misses what was published meanwhile, and a message older than 10 seconds "
            .. "when its turn comes is dropped. Never movement and never a part: what crosses is an event, and "
            .. "the world it lands in decides what it does. Server side only.",
        Methods = {
            {
                Name = "Publish",
                Parameters = { { Name = "topic", Type = "string" }, { Name = "message", Type = "any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends `message` to every world subscribed to `topic`. The message is anything `SaveService` "
                    .. "can store, at most 4 KiB written, and the topic at most 80 bytes. A world may publish 200 "
                    .. "messages a second; past that this raises.",
            },
            {
                Name = "Subscribe",
                Parameters = {
                    { Name = "topic", Type = "string" },
                    { Name = "callback", Type = "(message: any, origin: string) -> ()" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Calls `callback` with each message published on `topic`, and the name of the world that "
                    .. "published it. That name is empty for this world when no manager runs it. A world may "
                    .. "subscribe to 1000 topics. Each call runs on a thread of its own, between ticks.",
            },
            {
                Name = "Unsubscribe",
                Parameters = { { Name = "topic", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops every callback this world gave for `topic`.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "InstanceService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0221" },
        Doc = "Asks the manager for a copy of a scene for one group, such as a dungeon, an arena or a "
            .. "house, and runs the copy it made.\n\n"
            .. "A world a manager started (`servers.toml`) calls `CreateInstanceAsync` with a template's name "
            .. "from its `[[instance]]` rows, and sends the group there with `NetworkService:TravelGroup`. "
            .. "Only the players it was made for may enter, and one who drops comes back to it while it "
            .. "lives. It closes when it has been empty for its `idle_close_seconds`. Anywhere but under a "
            .. "manager, its methods raise.",
        Properties = {
            {
                Name = "InstanceId",
                Type = "string",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                Doc = "In an instance, its `Id`; empty in a world that is not one.",
            },
        },
        Methods = {
            {
                Name = "CreateInstanceAsync",
                Parameters = {
                    { Name = "template", Type = "string" },
                    { Name = "options", Type = "{ Players: {Player}?, Data: any? }?", Default = "nil" },
                },
                Returns = { "string" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Makes an instance of `template` for `options.Players`, nobody else, and returns its `Id` "
                    .. "once it is ready, to pass to `Travel` or `TravelGroup`. `options.Data` is what the "
                    .. "instance's scene reads with `SceneService:GetLoadData`, as a `RemoteEvent` would carry "
                    .. "it, with no instances in it. Raises with the reason when the template has as many "
                    .. "instances running as it may, the machine has too little memory free, or the instance "
                    .. "failed to start.",
            },
            {
                Name = "AllowPlayer",
                Parameters = { { Name = "key", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "In an instance: lets the player with this `PublicKey` in too, for a friend who comes late.",
            },
            {
                Name = "DisallowPlayer",
                Parameters = { { Name = "key", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "In an instance: no longer lets that player in. Somebody already in stays.",
            },
            {
                Name = "Close",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "In an instance: closes it now. `Closing` fires, and the players still in it go back to "
                    .. "the front door.",
            },
            {
                Name = "KeepOpen",
                Parameters = { { Name = "seconds", Type = "number" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "In an instance: keeps it open for at least this many seconds from now, empty or not.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Closing",
                Parameters = {},
                Doc = "Fires in an instance before it stops, for its script to save what it keeps. It stops "
                    .. "five seconds later.",
            },
            {
                Name = "InstanceClosed",
                Parameters = { { Name = "id", Type = "string" }, { Name = "reason", Type = "string" } },
                Doc = "Fires in the world that made an instance when that instance is gone, and says why.\n\n"
                    .. '`reason` is `"Closed"` when it was empty for its while or its script called `Close`, and '
                    .. '`"InstanceFailed"` when it stopped without closing. What the world handed out for the '
                    .. "run is settled here: its players may come back one by one, or not at all.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "ConsoleService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0218" },
        Doc = "The console of a server that runs with no window, and the game's own commands in it.\n\n"
            .. "A dedicated server reads lines from its standard input: `help`, `status`, `players`, `kick`, "
            .. "`ban`, `unban`, `say`, `save` and `stop` are the engine's, and a game adds its own with "
            .. "`Register`. A line runs between two ticks, never during one, so a command that changes the "
            .. "world is an input like a player's. Its answer goes to the standard output and to the log.\n\n"
            .. "Only the server has a console. A client, or a script on the client's side of a machine that "
            .. "hosts, that calls one of these methods gets an error.",
        Methods = {
            {
                Name = "Register",
                Parameters = {
                    { Name = "name", Type = "string" },
                    {
                        Name = "spec",
                        Type = "{ Description: string?, Parameters: { { Name: string, Type: string, "
                            .. "Optional: boolean? } }?, Permission: number? }",
                    },
                    {
                        Name = "callback",
                        Type = "(caller: { Local: boolean, Name: string, Key: string, Permission: number }, ...any) -> any",
                    },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Adds the command `name` to the console. `name` is one lowercase word, and none of the "
                    .. "engine's own. Registering a name again replaces it, as a hot reload does.\n\n"
                    .. "`spec.Parameters` lists what the command takes, in order: each has a `Name` and a "
                    .. '`Type` (`"string"`, `"number"`, `"boolean"` or `"Player"`), and the `Optional` ones '
                    .. "come last. A word left over at the end goes to the last `string` parameter, so "
                    .. "`announce Doors open at noon` gives the whole sentence. A `Player` is written as their "
                    .. "name, their `UserId` or the start of their `PublicKey`. A line that does not fit is "
                    .. "answered with the command's usage, and `callback` is not called.\n\n"
                    .. "`spec.Permission`, 0 to 3, is who may run it: 2 when absent. The local console is 3. "
                    .. "`spec.Description` is shown by `help`; it may be a key of the game's own catalog.\n\n"
                    .. "`callback` runs on a thread of its own, on the server, between ticks. Its first argument "
                    .. "is who ran it, then the parameters, `nil` for an optional one left out. What it returns "
                    .. "is shown to whoever ran it; an error it raises is shown the same way and does not stop "
                    .. "the server. It may yield, and the console takes the next line meanwhile.",
            },
            {
                Name = "Unregister",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Takes the command `name` out of the console. Nothing happens if there is none.",
            },
            {
                Name = "ExecuteAsync",
                Parameters = { { Name = "line", Type = "string" } },
                Returns = { "string" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Doc = "Runs `line` as the server's own console would, with every permission, and returns what "
                    .. "the command answered. Raises when the command failed. Yields until the command has "
                    .. "finished: a line runs between ticks.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "CryptoService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0151" },
        Doc = "Provides cryptographic functions: unpredictable random values, two hashes, and a hash for "
            .. "passwords.\n\n"
            .. "Its random values are not the simulation's. `Random.new()` and `math.random` are deterministic, "
            .. "so that a replay and every machine agree. A session token, a salt or an identity key must be "
            .. "unpredictable instead, and must come from this service, which uses the operating system's "
            .. "generator. Its results differ on every machine and every run, so a replay does not reproduce them "
            .. "and a client cannot predict them. Use it on the server, or outside the simulation.\n\n"
            .. "The service does not protect what a `RemoteEvent` carries. A password sent from a client travels "
            .. "with whatever protection the connection has. Hash what you store with `HashPasswordAsync`, and "
            .. "read the multiplayer guide for what the connection protects.",
        Methods = {
            {
                Name = "RandomBytes",
                Parameters = { { Name = "count", Type = "number" } },
                Returns = { "buffer" },
                ThreadSafety = "Safe",
                Doc = "Returns a buffer of `count` bytes from the operating system's random generator, for a key, "
                    .. "a salt or a token. `count` is an integer from 0 to 1048576.",
            },
            {
                Name = "RandomInteger",
                Parameters = { { Name = "min", Type = "number" }, { Name = "max", Type = "number" } },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "Returns an unpredictable integer from `min` to `max`, both included, with every value "
                    .. "equally likely. Use it for a room code, or for the seed of a `Random` that must differ on "
                    .. "each run. Both arguments must be integers, with `min` no greater than `max`.",
            },
            {
                Name = "UniqueId",
                Parameters = {},
                Returns = { "string" },
                ThreadSafety = "Safe",
                Doc = "Returns a new identifier that no other call returns, on this or any machine: a version 4 "
                    .. "UUID in lowercase, of the form `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`. It has 122 random "
                    .. "bits, so it can also be used as a session token.",
            },
            {
                Name = "Sha256",
                Parameters = { { Name = "data", Type = "string | buffer" } },
                Returns = { "string" },
                ThreadSafety = "Safe",
                Doc = "Returns the SHA-256 hash of the bytes of `data`, as 64 lowercase hexadecimal characters. "
                    .. "The same bytes give the same result everywhere. Do not use it for passwords: it is fast, "
                    .. "so hashed passwords can be guessed quickly. Use `HashPasswordAsync` for those.",
            },
            {
                Name = "HmacSha256",
                Parameters = {
                    { Name = "key", Type = "string | buffer" },
                    { Name = "data", Type = "string | buffer" },
                },
                Returns = { "string" },
                ThreadSafety = "Safe",
                Doc = "Returns the HMAC-SHA-256 of `data` with `key`, as 64 lowercase hexadecimal characters. It "
                    .. "is a signature that only a holder of the key can create or check. Use it to sign a token "
                    .. "on a server, or to sign a request to a web service. Compare signatures with `SecureEquals`.",
            },
            {
                Name = "SecureEquals",
                Parameters = {
                    { Name = "a", Type = "string | buffer" },
                    { Name = "b", Type = "string | buffer" },
                },
                Returns = { "boolean" },
                ThreadSafety = "Safe",
                Doc = "Returns whether `a` and `b` contain the same bytes, in a time that does not depend on "
                    .. "where they differ. Use it to compare a token or a signature received from outside. "
                    .. "Comparing with `==` stops at the first difference, and the time it takes reveals how much "
                    .. "of a guess was right.",
            },
            {
                Name = "HashPasswordAsync",
                Parameters = { { Name = "password", Type = "string" } },
                Returns = { "string" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Returns a hash of `password` to store instead of the password. It yields while the hash is "
                    .. "computed on another thread, so a server's tick does not wait for it.\n\n"
                    .. "The hash is Argon2id with a random salt, and is deliberately slow (tens of milliseconds "
                    .. "and 64 MiB of memory), so that a stolen table of hashes cannot be guessed quickly. "
                    .. "Hashing the same password twice gives two different strings. Check a password with "
                    .. "`VerifyPasswordAsync`, never with `==`.\n\n"
                    .. "The result is a string of fewer than 128 characters that contains its own salt and cost "
                    .. "settings, such as `$argon2id$v=19$m=65536,t=2,p=1$...`; store all of it. A password can "
                    .. "be at most 1024 bytes. Requests are processed one at a time, in order.",
            },
            {
                Name = "VerifyPasswordAsync",
                Parameters = { { Name = "password", Type = "string" }, { Name = "hash", Type = "string" } },
                Returns = { "boolean" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Returns whether `password` is the password that `hash` was made from by "
                    .. "`HashPasswordAsync`. It yields while the check runs. Returns false if `hash` is not a "
                    .. "hash that `HashPasswordAsync` produced.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "ContentProvider",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0131" },
        Doc = "Loads assets before they are shown. Use it behind a loading screen to load a level's meshes and "
            .. "images and upload them to the GPU, so that nothing appears late in the first frames of play. "
            .. "`SceneService:LoadSceneAsync` prepares a scene the same way.",
        Properties = {
            {
                Name = "RequestQueueSize",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                Scripted = true,
                Doc = "The number of assets requested with `PreloadAsync` that have not finished loading.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "PreloadAsync",
                Parameters = {
                    { Name = "items", Type = "{ string | Instance }" },
                    {
                        Name = "callback",
                        Type = "((item: string | Instance, status: Enum.AssetFetchStatus) -> ())?",
                        Default = "nil",
                    },
                },
                Returns = {},
                Yields = true,
                ThreadSafety = "Unsafe",
                Records = { "ADR 0155" },
                Doc = "Loads every item in a list and yields until each has loaded or failed. An item is a "
                    .. "content path, such as `asset://models/tree.gltf`, or an instance, which stands for every "
                    .. "asset that it and its descendants use. `callback` runs once for each item as it finishes, "
                    .. "with `Enum.AssetFetchStatus.Success` or `Failure`.\n\n"
                    .. "Meshes and images are uploaded to the GPU. A `.surface.hlsl` path compiles the surface "
                    .. "shader's pipelines, so they are not created during play when the shader first appears. A "
                    .. "sound is opened and its length is read. A name with no `://` is a stamp, named as for "
                    .. "`Instance.stamp`: its file is read and kept, and every asset it uses is loaded with it.\n\n"
                    .. "Preloaded assets are held for the scene in which they were requested and for the next "
                    .. "scene, which is what a loading screen needs. After that they are unloaded like anything "
                    .. "else a previous scene used, unless a scene still shows them. To hold assets for the whole "
                    .. "game, use `Keep`.",
            },
            {
                Name = "Keep",
                Parameters = {
                    { Name = "items", Type = "{ string | Instance }" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Holds every item in a list, named as for `PreloadAsync`, until `Release` is called. "
                    .. "Leaving a scene unloads the meshes and images that only that scene showed, but never a "
                    .. "kept one. Use it for what the game shows repeatedly across scenes, such as the player's "
                    .. "character or common enemies.\n\n"
                    .. "It requests the items and returns without waiting; call `PreloadAsync` with the same list "
                    .. "to wait for them. Kept assets stay in GPU memory, so keep only what is needed.",
            },
            {
                Name = "Release",
                Parameters = {
                    { Name = "items", Type = "{ string | Instance }" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops holding every item that was passed to `Keep`. Nothing is unloaded at once: an asset "
                    .. "that no scene shows is unloaded the next time the game leaves a scene.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "SceneService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0106" },
        Doc = "Reports which scene the game is in and changes to another. A scene is a complete place: its world, "
            .. "its lighting, its UI, its storage services and its own script services. `LoadScene` replaces all "
            .. "of it with another scene between two ticks. `GlobalScriptService` is kept across the change, with "
            .. "its scripts, their variables and their connections.\n\n"
            .. "In a multiplayer match, the authority controls the scene: a host or a server changes it, and "
            .. "every client follows without disconnecting.",
        Properties = {
            {
                Name = "CurrentScene",
                Type = "Scene",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                Scripted = true,
                Records = { "ADR 0124" },
                Doc = "The scene that is open, as an object that stays valid for that scene. Its `Path` is the "
                    .. "path of the scene file relative to the content folder, such as `scenes/arena.scene.json`, "
                    .. "or an empty string for a world that no scene file created. After the scene closes, its "
                    .. "`IsOpen` returns false. The global `scene` is the same object while the scene is open.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "LoadScene",
                Parameters = {
                    { Name = "path", Type = "string" },
                    { Name = "data", Type = "any?", Default = "nil" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Unloads the current scene and loads the one at `path`, which is relative to the content "
                    .. "folder. It returns at once; the change happens at the next safe point between ticks, "
                    .. "after `SceneLoading` has fired.\n\n"
                    .. "The old scene is removed entirely: its world, the contents and settings of its services, "
                    .. "and its scripts, whose threads stop. The new scene starts from the engine's default "
                    .. "settings, reads its file, loads its own code from `src/scenes/<scene>/` and starts its "
                    .. "scripts.\n\n"
                    .. "`data` is passed to the new scene, which reads it with `GetLoadData`. It can hold only "
                    .. "plain values, the same as a `RemoteEvent` argument, and no instances. A client connected "
                    .. "to a match cannot call this method.",
            },
            {
                Name = "LoadSceneAsync",
                Parameters = {
                    { Name = "path", Type = "string" },
                    { Name = "options", Type = "{ Activate: boolean?, Data: any? }?", Default = "nil" },
                },
                Returns = { "SceneLoad" },
                ReturnsHandle = true,
                ThreadSafety = "Unsafe",
                Records = { "ADR 0125" },
                Doc = "Starts preparing the scene at `path` while the current scene keeps running, and returns at "
                    .. "once with a `SceneLoad` object. Use its `Progress` for a loading bar, its `Ready` event "
                    .. "to know when the scene is prepared, and its `Activate` method to switch.\n\n"
                    .. "`options.Activate`, true by default, switches as soon as the scene is ready. Set it to "
                    .. "false to wait for a call to `Activate`, for example after a fade or a button press. "
                    .. "`options.Data` is the `data` of `LoadScene`.\n\n"
                    .. "The switch works like `LoadScene`, with the file already parsed and its meshes already "
                    .. "loaded. Only one load runs at a time: a second call cancels the first. A client connected "
                    .. "to a match cannot call this method.",
            },
            {
                Name = "GetLoadData",
                Parameters = {},
                Returns = { "any" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the `data` that the last `LoadScene` passed, as a fresh copy, or nil.",
            },
            {
                Name = "SendToHost",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0107" },
                Doc = "Inside a `SubWorld`: sends values out to the game that runs this world. The `Received` "
                    .. "event of the `SubWorld` fires there with them on its next tick. Use it to report a score "
                    .. "or a game over. It accepts the values a `RemoteEvent` accepts, except instances. Raises "
                    .. "an error if this world is not inside a `SubWorld`.",
            },
            {
                Name = "IsSubWorld",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns whether this world is running inside another game's `SubWorld`.",
            },
            {
                Name = "HoldLoading",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0159" },
                Doc = "Keeps the loading screen up over a scene until `ReleaseLoading` has been called the same "
                    .. "number of times.\n\n"
                    .. "The engine shows a loading screen while a scene loads: at the first scene and at every "
                    .. "`LoadScene`. It removes the screen once the scene's meshes and images are loaded and the "
                    .. "terrain around the camera is meshed. A scene whose scripts build its world, such as "
                    .. "generated terrain or parts created from code, should hold the screen until the world is "
                    .. "built.\n\n"
                    .. "A `ScreenGui` with `KeepOnSceneLoad` on, or one of the new scene's own, is drawn over the "
                    .. "engine's loading screen, so a game can show its own. A hold that is never released ends "
                    .. "after one minute, with a warning in the log.",
            },
            {
                Name = "ReleaseLoading",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Ends one `HoldLoading`. When none remain, the loading screen is removed as soon as the "
                    .. "scene has loaded. Extra calls are ignored.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "SceneLoading",
                Parameters = { { Name = "path", Type = "string" } },
                Doc = "Fires when the scene is about to change to `path`, while the old scene still exists. Show "
                    .. "a loading screen here; a `ScreenGui` with `KeepOnSceneLoad` on stays up across the change.",
            },
            {
                Name = "SceneLoaded",
                Parameters = { { Name = "path", Type = "string" } },
                Doc = "Fires when the new scene is in place and its scripts have started.",
            },
            {
                Name = "SceneLoadFailed",
                Parameters = { { Name = "path", Type = "string" }, { Name = "message", Type = "string" } },
                Doc = "Fires when a `LoadScene` fails because the scene file does not exist or cannot be read. "
                    .. "The scene that was open stays as it was, and its close handlers do not run. `message` "
                    .. "gives the reason, which is also logged.",
            },
            {
                Name = "HostMessageReceived",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Records = { "ADR 0107" },
                Doc = "Inside a `SubWorld`: fires with the values that the game running this world passed to "
                    .. "`SubWorld:Send`, one tick after they were sent. It never fires in a world that is not "
                    .. "inside a `SubWorld`.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "ServerStorage",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0080" },
        Doc = "A container for instances that only the authority has, such as templates and data that clients "
            .. "must not receive. Like `ReplicatedStorage`, nothing under it is in the world, and it is saved "
            .. "with the scene. Unlike it, nothing here replicates, and a client's copy is emptied when the "
            .. "client joins. In a single-player game, this machine is the authority and has everything.",
    },
    {
        Name = "NetworkService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0069", "ADR 0106" },
        Doc = "Reports the role of this process in a networked game, and joins, hosts and leaves matches.\n\n"
            .. "Use `Authority` to decide whether this machine makes gameplay decisions: `if "
            .. "NetworkService.Authority then`. A single-player game is also an authority, so the same code works "
            .. "with and without networking.\n\n"
            .. "`Join`, `Host` and `Disconnect` change the role at run time, at a safe point between two ticks, "
            .. "and `State` reports the current stage. `Authority` can change during a run: it becomes false "
            .. "after joining and true again after leaving. Code that runs for the whole game should listen for "
            .. "changes to `State` instead of reading `Authority` once.\n\n"
            .. "The command-line flags `--host`, `--serve` and `--join=` make the same calls before the first "
            .. "tick. `--serve` is the only way to run a dedicated server.",
        Properties = {
            {
                Name = "Channel",
                Type = "number",
                ReadOnly = true,
                Records = { "ADR 0230" },
                HostFact = true,
                Doc = "Which channel of its world this server is: 1 for a world without channels, and for the "
                    .. "first of a world with them. A `[[world]]` with `channels = true` in `servers.toml` is "
                    .. "copied into channels 2, 3 and on as the open ones fill, each a server of its own.",
            },
            {
                Name = "Authority",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                -- A fact about the process, not about the world: a trace recorded
                -- as a host must replay solo.
                HostFact = true,
                Doc = "Whether this process decides the state of the world. It is true in single-player, when "
                    .. "hosting and on a dedicated server, and false on a client, which shows the state it is sent.",
            },
            {
                Name = "Topology",
                Type = "Enum.NetworkTopology",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                HostFact = true,
                Doc = "The network role this process runs in. For gameplay decisions, use `Authority`; this "
                    .. "property is for showing the role, for example in a menu.",
            },
            {
                Name = "ServerTick",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                HostFact = true,
                Doc = "The authority's tick number. On the authority it is its own tick; on a client it is the "
                    .. "newest tick the client has applied; in single-player it is 0. It counts ticks and is not "
                    .. "a time.",
            },
            {
                Name = "PeerCount",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                HostFact = true,
                Doc = "The number of connected peers, not counting this process. On an authority it is the number "
                    .. "of clients; on a connected client it is 1.",
            },
            {
                Name = "State",
                Type = "Enum.NetworkState",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                HostFact = true,
                Doc = "The connection state of this machine: `Offline`, `Connecting`, `Connected`, `Hosting`, "
                    .. "`Serving` or `Reconnecting`.",
            },
            {
                Name = "Port",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                HostFact = true,
                Doc = "The port of this machine's match: the port it hosts or serves on, or the port it joined. "
                    .. "It is 0 when there is no match. A host can show it with `GetLocalAddresses` for others to "
                    .. "join.",
            },
            {
                Name = "RelayState",
                Type = "Enum.RelayState",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                HostFact = true,
                Records = { "ADR 0178" },
                Doc = "The state of this machine's match with its relay. It is `None` when there is no relay or "
                    .. "no hosted match, `Connecting` until the relay answers, `Ready` once the relay has "
                    .. "registered the match, and `Unreachable` while the relay does not answer. "
                    .. "`RelayStateChanged` fires when it changes.",
            },
            {
                Name = "JoinCode",
                Type = "string",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                HostFact = true,
                Records = { "ADR 0178" },
                Doc = "The code other players use to join this machine's match through a relay. It is eight "
                    .. "letters and digits, without 0, 1, I or O, which are easily confused. The relay resolves "
                    .. "it to this host wherever the host is, including behind a home router or on a mobile "
                    .. "network.\n\n"
                    .. "It is empty until `RelayState` is `Ready`, and on a machine that is not hosting through a "
                    .. "relay. It stays the same for as long as the match is hosted, even if the relay restarts, "
                    .. "and changes the next time `Host` is called.",
            },
            {
                Name = "LocalPlayer",
                Type = "Player?",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                Doc = "The player at this machine. It exists from startup in single-player, when hosting and when "
                    .. "joining. It is nil on a dedicated server.",
            },
            {
                Name = "MaxPlayers",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.network_max_players",
                Transient = true,
                HostFact = true,
                Records = { "ADR 0167" },
                Doc = "The largest number of players in this machine's match, including the host's own player. It "
                    .. "is the limit on the length of `GetPlayers()`. The host sets it, to an integer of at least "
                    .. "1, before or after calling `Host`.\n\n"
                    .. "A player who tries to join a full match is refused, and their `JoinFailed` reports that "
                    .. "the match is full. Lowering the value removes no one: players already in the match stay "
                    .. "until they leave or are removed with `Player:Kick`.\n\n"
                    .. "The value cannot exceed the capacity the match was opened with, which is 32 other players "
                    .. "or the `--max-players` flag; that capacity is what it reads while hosting if it was never "
                    .. "set. On a machine that joined a match it reads 0, and setting it raises an error.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetLocalPlayers",
                Parameters = {},
                Returns = { "{Player}" },
                Doc = "Returns the local players on this machine in the order they joined, with the primary "
                    .. "player first. Players that are being removed are not included.",
            },
            {
                Name = "AddLocalPlayer",
                Parameters = {},
                Returns = { "Player?" },
                ThreadSafety = "Unsafe",
                Doc = "Adds a guest player to an offline game on this machine and fires `PlayerAdded`. Returns "
                    .. "nil on a dedicated server or while connected to a match. A guest's id is valid for the "
                    .. "session only and is not a platform account. The game decides how many local players it "
                    .. "allows, and assigns each one's input with `InputContext.Player`. Local guests cannot join "
                    .. "online matches.",
            },
            {
                Name = "RemoveLocalPlayer",
                Parameters = { { Name = "player", Type = "Player" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Removes a guest player from an offline game, firing `PlayerRemoving`. Returns whether the "
                    .. "removal was started. It does not remove the primary local player or a remote player.",
            },
            {
                Name = "GetPlayers",
                Parameters = {},
                Returns = { "{Player}" },
                ThreadSafety = "Unsafe",
                Doc = "Returns every player in the game, in the order they joined. In single-player it returns "
                    .. "the one local player, so code that loops over players works with and without networking.",
            },
            {
                Name = "GetLocalAddresses",
                Parameters = {},
                Returns = { "{string}" },
                ThreadSafety = "Unsafe",
                Doc = "Returns this machine's IPv4 addresses on its local networks, best first. A host can show "
                    .. "them for players on the same network to type.\n\n"
                    .. "Only interfaces that are up are listed. Loopback and link-local (169.254) addresses are "
                    .. "left out, as are virtual adapters that can be identified, such as those of a hypervisor "
                    .. "or a container. Private ranges come first, and among them the interface of the default "
                    .. "route. The list does not depend on a match, is not replicated, and is empty when the "
                    .. "machine has no network.",
            },
            {
                Name = "PingRelayAsync",
                Parameters = {
                    { Name = "relay", Type = "string?", Default = "nil" },
                    { Name = "timeout", Type = "number?", Default = "nil" },
                },
                Returns = {
                    "{ Ping: number, Matches: number, Relayed: number, BytesPerSecond: number, Uptime: number }?",
                },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0178" },
                Doc = "Measures the round trip to a relay and returns what the relay reports, or nil if it did "
                    .. "not answer. Use it before hosting or joining to choose among relays in several regions.\n\n"
                    .. "`relay` is `host:port`; if omitted, the project's `[network] relay` is used. The method "
                    .. "yields until the relay has answered three requests sent a tenth of a second apart, or "
                    .. "until `timeout` seconds have passed (1.5 by default).\n\n"
                    .. "The result has `Ping`, the shortest of the three round trips in milliseconds; `Matches`, "
                    .. "the matches registered with the relay; `Relayed`, the players it is relaying; "
                    .. "`BytesPerSecond`, its traffic; and `Uptime`, in seconds. It does not need a match and "
                    .. "does not disturb one, and several relays can be pinged at once from separate tasks.",
            },
            {
                Name = "DefineAction",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "rule", Type = "{ PerSecond: number, Burst: number? }" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Says how often each player may do one of the game's own actions: `PerSecond` of them a "
                    .. "second, up to `Burst` at once (1 when not given). Said again for the same name, it "
                    .. "replaces the first. `CheckAction` then counts each player's.",
            },
            {
                Name = "CheckAction",
                Parameters = {
                    { Name = "player", Type = "Player" },
                    { Name = "name", Type = "string" },
                },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Whether this player may do the action now, by the rate `DefineAction` gave it, counted "
                    .. "by the simulation's clock. True takes one; false is logged as a suspicion "
                    .. "(`action_over_rate`), and what to do about it is the game's. Raises an error for an "
                    .. "action never defined.",
            },
            {
                Name = "ReportSuspicion",
                Parameters = {
                    { Name = "player", Type = "Player" },
                    { Name = "kind", Type = "string" },
                    { Name = "detail", Type = "string?" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Writes a line in the suspicion log, beside those the engine writes when a player goes "
                    .. "past a limit. The server console's `suspects` and `suspect` read it, and `/metrics` "
                    .. "counts it by `kind`. Nothing is done to the player by it.",
            },
            {
                Name = "IsMuted",
                Parameters = { { Name = "player", Type = "Player" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Whether the server console has muted this player (`mute`), by their account where the "
                    .. "game named one, else by their key. What a mute stops, such as chat or voice, is the "
                    .. "game's to decide.",
            },
            {
                Name = "GetStats",
                Parameters = {},
                Returns = {
                    "{ Ping: number, Jitter: number, Loss: number, SnapshotsPerSecond: number, "
                        .. "CorrectionsPerSecond: number, Corrections: number, LastCorrection: number, "
                        .. "PredictedDrift: number, PredictedDriftPart: BasePart?, "
                        .. "InputBufferDepth: number, "
                        .. "InputStarvations: number, InputReanchors: number, InterpolationDelay: number, "
                        .. "PredictedParts: number, ResimulationsPerSecond: number, "
                        .. "ResimulatedTicksPerSecond: number, ResimulationTime: number, "
                        .. "UnreliableSent: number, UnreliableReceived: number, UnreliableDropped: number, "
                        .. "BytesSent: number, BytesReceived: number, WireBytesSent: number, "
                        .. "WireBytesReceived: number, Deferred: number, BudgetUsed: number, SwarmBytes: number, "
                        .. "SnapshotBytes: number, AttributeBytes: number, RemoteBytes: number, "
                        .. "UnreliableBytes: number, InputBytes: number, CrossWorldDropped: number, Path: string, "
                        .. "LocalPlayers: { { UserId: number, Corrections: number, CorrectionsPerSecond: number, "
                        .. "LastCorrection: number } } }",
                },
                ThreadSafety = "Unsafe",
                Doc = "Returns statistics about the connection, as a table. Use them to show a connection quality "
                    .. "indicator. Every number is 0 in single-player. On an authority, the link numbers are "
                    .. "those of the worst client and the deepest input queue.\n\n"
                    .. "Link quality:\n\n"
                    .. "- `Ping`: the round trip, in milliseconds.\n"
                    .. "- `Jitter`: how much the round trip varies.\n"
                    .. "- `Loss`: the share of packets lost, in percent.\n"
                    .. "- The number of snapshots per second.\n"
                    .. "- `Corrections`: how often this machine's own character was corrected, per second and in "
                    .. "total since joining. `LastCorrection` is how far the last correction moved it, in meters.\n"
                    .. "- `PredictedDrift`: how far the parts this machine predicts were from the authority's at the "
                    .. "last answer, in meters, whether or not that was a correction. These are what its character "
                    .. "pushes and the vehicle it drives. A driven vehicle is left a few centimeters of drift and "
                    .. "pulled back gently, so this stays small but is rarely zero. `PredictedDriftPart` is the "
                    .. "part that was furthest, or nil.\n"
                    .. "- `InputBufferDepth`: how many ticks of this player's input the authority has queued. "
                    .. "`InputStarvations` counts the times the queue ran empty, and `InputReanchors` the times "
                    .. "it was restarted because the player's clock jumped, as after a long frame.\n"
                    .. "- `InterpolationDelay`: how far in the past other players are drawn, in milliseconds. It "
                    .. "is two snapshot intervals plus the link's jitter, and adapts to the link.\n"
                    .. "- On a client, `PredictedParts` is the number of loose parts near its character that it "
                    .. "predicts. The table also reports how often a correction made the client simulate them "
                    .. "again, how many ticks it simulated again, and `ResimulationTime`, the average time that "
                    .. "took in milliseconds.\n\n"
                    .. "Traffic: `BytesSent` and `BytesReceived` are totals since the session began. Five more "
                    .. "fields break the total down, sent and received together: `SnapshotBytes` for world state, "
                    .. "`AttributeBytes` for attributes, `RemoteBytes` for `RemoteEvent`s and `RemoteFunction`s, "
                    .. "`UnreliableBytes` for `UnreliableRemoteEvent`s, and `InputBytes` for a client's input and "
                    .. "the state of the parts it owns. `SwarmBytes` is the positions of replicated swarms. The "
                    .. "remainder is instances being created and removed, terrain and the handshake. To get a "
                    .. "rate, read a field twice, one second apart, and subtract.\n\n"
                    .. "`CrossWorldDropped` counts `CrossWorldService` messages this world dropped unheard: "
                    .. "too many waiting, or older than 10 seconds when their turn came.\n\n"
                    .. "`WireBytesSent` and `WireBytesReceived` are what the network itself carried: the same "
                    .. "messages with the transport's headers, acknowledgements, resends and encryption added. "
                    .. "They are what a player's connection pays for, and are larger than `BytesSent` and "
                    .. "`BytesReceived`.\n\n"
                    .. "On an authority, `Deferred` counts the updates its players' budgets held back in the last "
                    .. "second (`Player.ReplicationBudget`), and `BudgetUsed` is how much of those budgets that "
                    .. "second's snapshots spent, from 0 to 1; above 1 when what may not wait, such as a player "
                    .. "joining, spent more. Both are 0 on a client.\n\n"
                    .. '`Path` says how the other end is reached: `"lan"` on the same network, `"direct"` across '
                    .. 'the internet, or `"relayed"` through a relay, which adds the relay\'s distance to the '
                    .. "round trip. On a host it is the longest path among its players. It is empty when nobody "
                    .. "is connected.\n\n"
                    .. "`LocalPlayers` has one entry for each local player on a machine that joined a match: the "
                    .. "primary player first, then the guests. Each entry has the player's `UserId` and the three "
                    .. "correction numbers for that player's character alone. It is empty on an authority and in "
                    .. "single-player.",
            },
            {
                Name = "Join",
                Parameters = {
                    { Name = "address", Type = "string?", Default = "nil" },
                    {
                        Name = "options",
                        Type = "{ Relay: string?, Direct: boolean?, Ticket: string? }?",
                        Default = "nil",
                    },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0178" },
                Doc = "Connects to a server at `address`, written as `host` or `host:port`. With no address, it "
                    .. "connects to `[network] server` from `project.toml`. Joining replaces this machine's scene "
                    .. "with the server's, and server scripts stop, because this machine is no longer the "
                    .. "authority. `Connected` or `JoinFailed` fires with the outcome. A dedicated server cannot "
                    .. "join.\n\n"
                    .. "`address` can also be a join code, when a relay is known from `options.Relay` or "
                    .. "`[network] relay`. A code is accepted as a player types it: in either case, with spaces "
                    .. "or a dash. The relay is asked for the host, and the best available path is used "
                    .. "automatically: the same network, a direct connection across the internet, or the relay. "
                    .. "`GetStats().Path` reports which. Set `options.Direct` to false to always go through the "
                    .. "relay. `relay://host:port/CODE` gives the relay and the code in one string. To connect to "
                    .. "a host whose name could be read as a code, include its port.\n\n"
                    .. "`options.Ticket` is what this player presents to be let in: a ticket the game's backend "
                    .. "signed when they logged in. It travels inside the encrypted connection, and the server's "
                    .. "`OnJoinRequest` reads it.",
            },
            {
                Name = "Host",
                Parameters = {
                    { Name = "port", Type = "number?", Default = "nil" },
                    { Name = "options", Type = "{ Relay: string? }?", Default = "nil" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0178" },
                Doc = "Makes this machine the authority of a match that others can join, on `port` (7777 by "
                    .. "default), keeping the current scene. A dedicated server cannot call it.\n\n"
                    .. 'With a relay, from `options.Relay = "host:port"` or `[network] relay`, the match is '
                    .. "registered with the relay, and players anywhere can join it with `JoinCode`, with no port "
                    .. "opened on a router. Hosting does not wait for the relay or depend on it: `RelayState` "
                    .. "reports the relay's state, and the match can always be joined by address.",
            },
            {
                Name = "Disconnect",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Leaves the match, or stops hosting, and returns to single-player in the current scene. "
                    .. "Server scripts start again from the beginning, because this machine is the authority "
                    .. "again. `Disconnected` fires. A dedicated server cannot call it.",
            },
            {
                Name = "Travel",
                Parameters = {
                    { Name = "player", Type = "Player" },
                    { Name = "target", Type = "string" },
                    { Name = "data", Type = "any?", Default = "nil" },
                },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Sends `player` to another world of the game: `target` is a world's name from "
                    .. "`servers.toml`, or an instance's `Id`. Their machine leaves this world and joins that "
                    .. "one, which fires `TravelArrived` with `data` and the name of this world. `data` is anything "
                    .. "`SaveService` can store, at most 16 KiB. Their character does not travel; the target "
                    .. "spawns one.\n\n"
                    .. "Only a world a manager started can send players elsewhere: anywhere else, and for a "
                    .. "world the manager does not have, this raises at once."
                    .. "\n\n"
                    .. "Returns whether `player` was sent. A player who left while the script waited, for "
                    .. "`CreateInstanceAsync` say, is not an error: they are gone, nothing is sent, and it "
                    .. "returns false.",
            },
            {
                Name = "SwitchChannel",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "channel", Type = "number" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0230" },
                Doc = "Sends `player` to channel `channel` of the world this server is, for a channel menu: as "
                    .. "`Travel` does, and with `TravelArrived` there. A channel that is not open, or full, "
                    .. "leaves them where they are, and a warning says why. Only a world a manager started can "
                    .. "call it."
                    .. " Returns whether `player` was asked to go: false for a player who has left.",
            },
            {
                Name = "TravelToPlayer",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "key", Type = "string" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0230" },
                Doc = "Sends `player` to the world, and the channel of it, where the player whose `PublicKey` is "
                    .. "`key` is now: a friend, a party's leader. When that world is full, or the other player is "
                    .. "in none, they stay where they are, and a warning says why."
                    .. " Returns whether `player` was asked to go: false for a player who has left.",
            },
            {
                Name = "GetChannelsAsync",
                Parameters = {},
                Returns = { "{ { Channel: number, Players: number, MaxPlayers: number } }" },
                ThreadSafety = "Unsafe",
                Yields = true,
                Records = { "ADR 0230" },
                Doc = "Returns the open channels of the world this server is, each with how many players it "
                    .. "holds and may hold, as the manager last heard. It waits for the manager's answer. For a "
                    .. "world without channels, the one entry is this world.",
            },
            {
                Name = "TravelGroup",
                Parameters = {
                    { Name = "players", Type = "{Player}" },
                    { Name = "target", Type = "string" },
                    { Name = "data", Type = "any?", Default = "nil" },
                },
                Returns = { "{Player}" },
                ThreadSafety = "Unsafe",
                Doc = "Sends several players to the same world together, as `Travel` sends one. The target "
                    .. "fires `TravelArrived` for each, in the order given."
                    .. "\n\n"
                    .. "Returns the players sent, in the order given. Any who left while the script waited are "
                    .. "skipped, not an error, and the rest still go together; when none is left, it returns an "
                    .. "empty table and sends nobody.",
            },
            {
                Name = "VerifyTicket",
                Parameters = { { Name = "ticket", Type = "string" } },
                Returns = { "{ [string]: any }?" },
                ThreadSafety = "Unsafe",
                Doc = "Checks a ticket the game's backend signed, on the server and with no call out, and returns "
                    .. "its claims, or nil. The ticket is a standard JWT signed with Ed25519 (`EdDSA`), which any "
                    .. "backend's JWT library issues. A ticket is accepted when its signature checks against "
                    .. "`ticket_public_key` under `[network]` in `project.toml`, its `aud` is this game's "
                    .. "`[project] id`, it lives at most 90 seconds between `iat` and `exp` and has not expired, "
                    .. "and its `jti` has not been seen before by this server. Clocks may differ by 30 seconds "
                    .. "each way.\n\n"
                    .. "The algorithm is never the ticket's to choose: one that says anything but `EdDSA`, "
                    .. "including `none`, is refused before its signature is looked at, and a key the ticket "
                    .. "names is ignored. What was wrong is written to the server's log, never told to the "
                    .. "player. The claims are the ticket's top-level strings, numbers and booleans: `sub`, the "
                    .. "account, and whatever else the backend put in.",
            },
            {
                Name = "ForgetServerKey",
                Parameters = { { Name = "address", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Forgets the key this machine saved for the server at `address`, written as for `Join`. "
                    .. "The first time a machine joins a server by its address, it saves the server's key, "
                    .. "and a later join to the same address with another key fails with `JoinFailed`: "
                    .. "someone may be in the way. A server whose owner reinstalled it has a new key too. A "
                    .. "game can offer the player a way to trust it again: call this, then `Join`.\n\n"
                    .. "Joining by code saves nothing, so there is nothing to forget.",
            },
        } :: { Schema.Method },
        Callbacks = {
            {
                Name = "OnJoinRequest",
                Parameters = {
                    {
                        Name = "joiner",
                        Type = "{ PublicKey: string, Address: string, Ticket: string, AccountId: string? }",
                    },
                },
                Returns = { "boolean", "string?" },
                Doc = "Called on the server for everybody who joins, before they are a player, to say whether "
                    .. "they may. Return true to let them in, or false and the words they are shown. `joiner` "
                    .. "has their `PublicKey`, their `Address` and the `Ticket` their game presented, empty for "
                    .. "none; set `joiner.AccountId` to the account they are, usually the `sub` of "
                    .. "`VerifyTicket`'s claims.\n\n"
                    .. "It runs on a thread of its own and may yield, to ask the game's backend; after 10 seconds "
                    .. "it is a refusal, and so is an error in it. While it runs the joiner has no `Player` and "
                    .. "is sent nothing. An account already playing is dropped when the same account joins "
                    .. "again, and an account banned with the console's `ban` is refused after this has named "
                    .. "it. With no `OnJoinRequest`, everybody is let in. Set it once, in a server script: a "
                    .. "second assignment replaces the first, and a client cannot set it.",
            },
        } :: { Schema.Callback },
        Events = {
            {
                Name = "Connected",
                Parameters = {},
                Doc = "Fires when a join succeeds and this machine is in the server's match.",
            },
            {
                Name = "JoinFailed",
                Parameters = { { Name = "reason", Type = "string" } },
                Doc = "Fires when a join fails. This machine stays in single-player, in the state it was in "
                    .. "before `Join` was called, so the menu that called `Join` is still there to handle the "
                    .. "event.\n\n"
                    .. "The reason is passed as readable text: the server did not answer, the server is full, it "
                    .. "runs a different version, or it is taking no more players from this machine's network "
                    .. "address. Players behind one router or one mobile network share an address, and a server "
                    .. "allows 64 of them by default (`[network] max_connections_per_address` in `project.toml`, or "
                    .. "`--max-per-address` for a dedicated server; 0 is no limit). With a join code, it can also be that the relay did not "
                    .. "answer, no match has that code, the host has blocked this machine, or no connection could "
                    .. "be established in time.",
            },
            {
                Name = "RelayStateChanged",
                Parameters = {},
                Records = { "ADR 0178" },
                Doc = "Fires when `RelayState` or `JoinCode` changes: the relay registered the match, stopped "
                    .. "answering, or started answering again. Read the two properties for the new values.",
            },
            {
                Name = "HostFailed",
                Parameters = { { Name = "reason", Type = "string" } },
                Doc = "Fires when `Host` cannot open its port, for example because another program is using it. "
                    .. "This machine stays as it was. The reason is passed as readable text.",
            },
            {
                Name = "Disconnected",
                Parameters = { { Name = "reason", Type = "string" } },
                Doc = "Fires when the connection ends and this machine is back in single-player. The server left, "
                    .. "the network failed, or `Disconnect` was called.",
            },
            {
                Name = "ChannelMerging",
                Parameters = { { Name = "seconds", Type = "number" } },
                Records = { "ADR 0230" },
                Doc = "Fires on a channel the manager is about to merge into another, because it has held under a "
                    .. "quarter of its players for a while. In `seconds` its players are moved, all together, to "
                    .. "the fullest channel with room. A game can say so first.",
            },
            {
                Name = "Queued",
                Parameters = {
                    { Name = "position", Type = "number" },
                    { Name = "estimateSeconds", Type = "number?" },
                },
                Records = { "ADR 0232" },
                Doc = "Fires on a client waiting in line at a full game's front door, with its place and an "
                    .. "estimate. The place counts from 1. The estimate is about how many seconds until its "
                    .. "turn, measured by how fast the line moved in the last five minutes, and nil before "
                    .. "anybody has left it. It fires when the machine joins the line and every five seconds "
                    .. "after; `Connected` fires as usual when it is let in. `@engine/travelscreen` shows it.",
            },
            {
                Name = "TravelArrived",
                Parameters = {
                    { Name = "player", Type = "Player" },
                    { Name = "data", Type = "any" },
                    { Name = "origin", Type = "string" },
                    { Name = "reason", Type = "string" },
                },
                Doc = "Fires on the server of the world a player traveled to, after `PlayerAdded`, with the "
                    .. "`data` the world they left sent and that world's name. A player who came through the "
                    .. "front door fires it too, with nil and an empty origin. `reason` is empty, or "
                    .. '`"InstanceFailed"` for a player sent back because the instance they were in stopped '
                    .. "without closing: the game can say so, and give back what the run cost.",
            },
            {
                Name = "TravelStarted",
                Parameters = { { Name = "world", Type = "string" } },
                Doc = "Fires on a client when it is sent to another world, by the front door or by the world it "
                    .. "is in, before it leaves, with that world's name. Show a loading screen here and take it "
                    .. "down on `Connected`, which fires when the next world has taken this machine; "
                    .. "`@engine/travelscreen` is one, mounted in one line.",
            },
            {
                Name = "Announced",
                Parameters = { { Name = "text", Type = "string" } },
                Doc = "Fires on every machine of the match when the server's operator says something to "
                    .. "everybody, with the console's `say` or as `stop` counts down. The engine writes it to "
                    .. "the log and draws nothing: a game shows it its own way, or mounts `@engine/announcements`, "
                    .. "which shows each one across the top of the screen for a few seconds.",
            },
            {
                Name = "PlayerAdded",
                Parameters = { { Name = "player", Type = "Player" } },
                Records = { "ADR 0015" },
                Doc = "Fires when a player joins. A script that connects at its top level and then loops over "
                    .. "`GetPlayers()` handles each player exactly once.",
            },
            {
                Name = "PlayerRemoving",
                Parameters = { { Name = "player", Type = "Player" } },
                Doc = "Fires when a player is leaving. The player object is still valid inside the handler, so a "
                    .. "game can save that player's data.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "RemoteEvent",
        Extends = "Instance",
        Module = "scene",
        Records = { "ADR 0077" },
        Doc = 'Sends messages between machines in a multiplayer game: "I bought the sword" from a client to the '
            .. 'server, or "the round starts" from the server to every client. Create it on the authority, for '
            .. "example in a `Folder` under `Workspace`. It replicates to every client like any instance, and a "
            .. "client finds it with `WaitForChild`.\n\n"
            .. "The authority identifies the sender of a message from the connection, not from the message's "
            .. "contents, so a client cannot send a message as another client. A client should report what it "
            .. "did; the authority decides what happened.\n\n"
            .. "Arguments can be nil, booleans, numbers, strings, buffers, vectors, `Color3`, `CFrame`, "
            .. "`Vector2`, `UDim`, `UDim2`, enum items, instances, and tables of these up to eight levels deep. "
            .. "An instance arrives as the receiver's own copy of it, or as nil if the receiver does not have it. "
            .. "A call raises an error for a function, a thread or a table that contains itself, and for a "
            .. "message larger than 64 KiB.\n\n"
            .. "The same script works in single-player, when hosting and when connected. On an authority, "
            .. "`FireServer` fires that machine's own `ServerReceived` with its local player, and a host's "
            .. "messages to its own player fire its own `ClientReceived`. Messages are reliable. Their events "
            .. "fire at the start of the tick after they arrive.",
        Methods = {
            {
                Name = "SetRateLimit",
                Parameters = {
                    { Name = "perSecond", Type = "number" },
                    { Name = "burst", Type = "number?" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets how often each client may call this remote on the authority, in calls a second. "
                    .. "Up to `burst` may come at once (twice `perSecond`, and at least 480, when not "
                    .. "given). Calls past it are dropped before any script sees them and are logged as a "
                    .. "suspicion. Nought lifts the limit, for a game's own heavy channel. Without it a remote "
                    .. "takes 240 calls a second with a burst of 480, and calls of up to 16 KiB; the attributes "
                    .. "`RateLimit`, `Burst` and `MaxBytes` hold the same numbers.",
            },
            {
                Name = "FireServerFor",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends a message to the authority on behalf of one of this machine's local players, on a "
                    .. "machine that is itself the authority. `ServerReceived` fires with that player. Raises an "
                    .. "error on a client, and for a remote player or a guest that has been removed. `FireServer` "
                    .. "always sends as the primary local player.",
            },
            {
                Name = "FireServer",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends the arguments to the authority, where `ServerReceived` fires with this machine's "
                    .. "player followed by the arguments. On an authority that has a player of its own, in "
                    .. "single-player or when hosting, the event fires locally. Raises an error on a dedicated "
                    .. "server, which has no player.",
            },
            {
                Name = "FireClient",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends the arguments to one player's machine, where `ClientReceived` fires. Only the "
                    .. "authority can call it; it raises an error on a client.",
            },
            {
                Name = "FireAllClients",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends the arguments to every player's machine, including a host's own. Only the authority "
                    .. "can call it; it raises an error on a client.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "LocalClientReceived",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "arguments", Type = "...any" } },
                Doc = "Fires on a machine with local guests when the authority sends a message to one of its "
                    .. "guest players. Passes that player, followed by the arguments. Messages to the primary "
                    .. "player and to all clients fire `ClientReceived` instead.",
            },
            {
                Name = "ServerReceived",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "arguments", Type = "...any" } },
                Doc = "Fires on the authority when a player's machine calls `FireServer`. Passes the player who "
                    .. "sent the message, followed by the arguments. The sender is reliable, because it comes "
                    .. "from the connection; treat the arguments as claims by the client and validate them.",
            },
            {
                Name = "ClientReceived",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Doc = "Fires on a player's machine when the authority calls `FireClient` for that player or "
                    .. "`FireAllClients`.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "UnreliableRemoteEvent",
        Extends = "Instance",
        Module = "scene",
        Records = { "ADR 0161" },
        Doc = "A `RemoteEvent` for data that is sent many times per second and replaced each time. Use it for the "
            .. "positions of a horde, the direction a player aims in or a boss's health, and never for anything "
            .. "that must arrive.\n\n"
            .. "A message is sent once and can be lost. It is not sent again, so a lost message delays nothing "
            .. "else, whereas a lost reliable message makes every later reliable message wait for it.\n\n"
            .. "The newest message wins. A message that arrives after a later one is dropped instead of being "
            .. "delivered late, across all of the game's unreliable events, so a script never receives a message "
            .. "older than one it already received.\n\n"
            .. "A message to a machine that does not have the event yet, or where no handler is connected, is "
            .. "dropped.\n\n"
            .. "The arguments are limited to 16 KiB; a larger message raises an error at the call. A message "
            .. "larger than about 1 KB is sent in several packets and is lost entirely if any of them is, so "
            .. "large messages are lost more often. Send small messages, or several that are each useful alone.\n\n"
            .. "In every other respect it behaves like `RemoteEvent`: the values it accepts, who can call which "
            .. "method, and how the authority identifies the sender.",
        Methods = {
            {
                Name = "SetRateLimit",
                Parameters = {
                    { Name = "perSecond", Type = "number" },
                    { Name = "burst", Type = "number?" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets how often each client may call this remote on the authority, in calls a second. "
                    .. "Up to `burst` may come at once (twice `perSecond`, and at least 480, when not "
                    .. "given). Calls past it are dropped before any script sees them and are logged as a "
                    .. "suspicion. Nought lifts the limit, for a game's own heavy channel. Without it a remote "
                    .. "takes 240 calls a second with a burst of 480, and calls of up to 16 KiB; the attributes "
                    .. "`RateLimit`, `Burst` and `MaxBytes` hold the same numbers.",
            },
            {
                Name = "FireServerFor",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends a message to the authority on behalf of one of this machine's local players, on a "
                    .. "machine that is itself the authority. `ServerReceived` fires with that player. Raises an "
                    .. "error on a client, and for a remote player or a guest that has been removed. `FireServer` "
                    .. "always sends as the primary local player.",
            },
            {
                Name = "FireServer",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends the arguments to the authority once. If the message arrives, `ServerReceived` fires "
                    .. "there with this machine's player followed by the arguments. On an authority that has a "
                    .. "player of its own, the event fires locally. Raises an error on a dedicated server, which "
                    .. "has no player.",
            },
            {
                Name = "FireClient",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends the arguments to one player's machine once. If the message arrives, `ClientReceived` "
                    .. "fires there. Only the authority can call it; it raises an error on a client.",
            },
            {
                Name = "FireAllClients",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends the arguments to every player's machine once, including a host's own. Only the "
                    .. "authority can call it; it raises an error on a client.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "LocalClientReceived",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "arguments", Type = "...any" } },
                Doc = "Fires on a machine with local guests when the authority sends a message to one of its "
                    .. "guest players. Passes that player, followed by the arguments. Messages to the primary "
                    .. "player and to all clients fire `ClientReceived` instead.",
            },
            {
                Name = "ServerReceived",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "arguments", Type = "...any" } },
                Doc = "Fires on the authority when a message that a player's machine sent with `FireServer` "
                    .. "arrives. Passes the player who sent it, followed by the arguments. Treat the arguments as "
                    .. "claims by the client and validate them.",
            },
            {
                Name = "ClientReceived",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Doc = "Fires on a player's machine when a message arrives that the authority sent with "
                    .. "`FireClient` for that player or with `FireAllClients`.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "RemoteFunction",
        Extends = "Instance",
        Module = "scene",
        Records = { "ADR 0079" },
        Doc = 'Lets a client ask the server a question and wait for the answer, such as "what is in my '
            .. 'inventory?" or "may I open this door?". Create it on the authority, for example under '
            .. "`Workspace`, and set its `OnServerInvoke`. A client calls `InvokeServerAsync`, which yields until "
            .. "the server's answer arrives and returns it.\n\n"
            .. "Arguments and results can be the same values a `RemoteEvent` carries, with the same limits. The "
            .. "player who asked is passed to the handler as its first argument, taken from the connection.\n\n"
            .. "Only a client can ask. The server cannot call a client and wait, because a client that never "
            .. "answered would block the server's script forever. To tell a client something, use a `RemoteEvent`.",
        Methods = {
            {
                Name = "SetRateLimit",
                Parameters = {
                    { Name = "perSecond", Type = "number" },
                    { Name = "burst", Type = "number?" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets how often each client may call this remote on the authority, in calls a second. "
                    .. "Up to `burst` may come at once (twice `perSecond`, and at least 480, when not "
                    .. "given). Calls past it are dropped before any script sees them and are logged as a "
                    .. "suspicion. Nought lifts the limit, for a game's own heavy channel. Without it a remote "
                    .. "takes 240 calls a second with a burst of 480, and calls of up to 16 KiB; the attributes "
                    .. "`RateLimit`, `Burst` and `MaxBytes` hold the same numbers.",
            },
            {
                Name = "InvokeServerAsync",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Returns = { "...any" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Sends a request to the authority, yields until it answers, and returns what its "
                    .. "`OnServerInvoke` returned. Raises an error if the handler raised one, or if the server "
                    .. "has no handler.\n\n"
                    .. "On an authority that has a player of its own, in single-player or when hosting, the "
                    .. "handler is called locally as that player at the start of the next tick. Raises an error "
                    .. "on a dedicated server, which has no player.",
            },
        } :: { Schema.Method },
        Callbacks = {
            {
                Name = "OnServerInvoke",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "arguments", Type = "...any" } },
                Returns = { "...any" },
                Doc = "Called on the authority to answer `InvokeServerAsync`. It receives the player who asked, "
                    .. "followed by the arguments, and returns the answer. It runs in its own thread and can "
                    .. "yield. Set it once, in a server script; a callback set on a client is never called.",
            },
        } :: { Schema.Callback },
    },
    {
        Name = "Player",
        Extends = "Instance",
        Module = "scene",
        Storage = "Player",
        Tags = { "NotCreatable" } :: { Schema.ClassTag },
        Doc = "A participant in the game. Players are children of `NetworkService`, and the engine creates and "
            .. "removes them: one on a single-player or hosting machine, and one per client on an authority.\n\n"
            .. "A player's input reaches the authority as intents: the values of their input actions, not the "
            .. "results. The authority decides what happened, so a client cannot dictate an outcome.",
        Properties = {
            {
                Name = "Camera",
                Type = "Camera?",
                Default = "nil",
                Presentation = true,
                Records = { "ADR 0205" },
                Doc = "The camera a local player looks through. When `Workspace.ScreenSplit` is set, every local "
                    .. "player that has a camera gets a view in the window; with nil, the player has no view of "
                    .. "its own. It is local to this machine, is not replicated, and has no meaning for a player "
                    .. "that is not local.",
            },
            {
                Name = "GamepadId",
                Type = "number",
                Default = "0",
                Presentation = true,
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The id of the gamepad a local player uses, as numbered by `InputService:GetGamepads`; 0 "
                    .. "means none is assigned. It selects which gamepad navigates that player's interfaces (see "
                    .. "`ScreenGui.Player`). It does not affect input actions: an `InputContext` has its own "
                    .. "`GamepadId`. The game sets it when a gamepad joins.",
            },
            {
                Name = "UserId",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The number the authority gives this player. It is 1 for the player on a single-player or "
                    .. "hosting machine, and 2 and up for clients in the order they joined. It is never reused "
                    .. "within a session, and a player who reconnects while the server runs gets the same one. "
                    .. "It is not kept when the server restarts: what lasts is `PublicKey`, and an account id a "
                    .. "game gives its players (`NetworkService.OnJoinRequest`).",
            },
            {
                Name = "AccountId",
                Type = "string?",
                ReadOnly = true,
                Presentation = true,
                ThreadSafety = "Safe",
                Doc = "The account the game knows this player as, on the server: what its "
                    .. "`NetworkService.OnJoinRequest` set when they joined, or nil. Two joins with the same account "
                    .. "are one player: the second drops the first.",
            },
            {
                Name = "PublicKey",
                Type = "string",
                ReadOnly = true,
                Presentation = true,
                ThreadSafety = "Safe",
                Doc = "The key this player's game proved when it joined, in hexadecimal. It is the same on "
                    .. "every connection from the same copy of the game, and after the server restarts. On the server "
                    .. "every player has one; on a client only its own players do. Empty where the match is not "
                    .. "encrypted. Saved data and a ban are kept by it; a player who deletes their game's key "
                    .. "file is a new player, so a game with accounts keys what lasts by its own account id.",
            },
            {
                Name = "Character",
                Type = "BasePart?",
                Doc = "The part that represents this player in the world: a `CharacterBody`, a vehicle or "
                    .. "whatever the game moves for them. For a character on the 2D plane, use `Character2D`. The "
                    .. "game sets it on the authority, and it replicates: on each machine, the `Player` refers to "
                    .. "that machine's copy of the part.\n\n"
                    .. "The engine uses it for three things. A client predicts its own player's character: its "
                    .. "local scripts move it immediately, and the authority's updates correct it instead of "
                    .. "overwriting it. A client draws every other player's character interpolated between "
                    .. "updates. And the authority sends each client only the instances near that client's "
                    .. "character, within `StreamingService.LoadRadius`; a client whose player has no character "
                    .. "is sent everything.",
            },
            {
                Name = "ReplicationFocus",
                Type = "BasePart?",
                Records = { "ADR 0162" },
                Doc = "The part used in place of this player's character to decide what is sent to their machine. "
                    .. "For example, set it to a friend's character while a defeated player spectates. With nil, "
                    .. "the default, the player's own `Character` is used.\n\n"
                    .. "Only the authority sets it. It controls which parts are streamed to the player and which "
                    .. "agents of a replicated `Swarm` are sent. It does not change which character the client "
                    .. "predicts.",
            },
            {
                Name = "ReplicationBudget",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Records = { "ADR 0229" },
                Presentation = true,
                Doc = "The most of the world's state, in kilobytes (1,024 bytes) a second, this player's machine "
                    .. "is sent. 0, the default, is the game's `client_budget_kb_per_second` under `[network]` in "
                    .. "`project.toml`, which is 48 unless the game says otherwise.\n\n"
                    .. "When more changes than that, the authority sends what matters most to this player first: "
                    .. "their own character and what they own, always; then what is nearest them, times "
                    .. "`BasePart.ReplicationPriority`, times `SetReplicationPriority`. What waits keeps "
                    .. "growing in priority until it is sent, and a changed part waits no more than about a "
                    .. "second while the budget can carry each once a second. Messages a script sends "
                    .. "(`RemoteEvent`) and what a player is sent as they join are not counted.\n\n"
                    .. "Only the authority reads it. A player on a weak network, or a spectator, can be given "
                    .. "less.",
            },
            {
                Name = "Character2D",
                Type = "Part2D?",
                Doc = "The `Part2D` that represents this player on the 2D plane; the 2D counterpart of "
                    .. "`Character`. A player has one character: setting either property clears the other, and "
                    .. "each reads nil while the other is set. There are two properties so that each has the "
                    .. "right type.\n\n"
                    .. "It is used the same way as `Character`. The authority sends each client the instances "
                    .. "near its character. On the player's own machine, the character is not drawn from past "
                    .. "updates: what local scripts did to it is kept, and the authority's updates correct it by "
                    .. "the difference. A client that moves it with the same code the server uses is never "
                    .. "corrected; the multiplayer guide shows the pattern. On a client that does not move it, it "
                    .. "is at the position of the newest update.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetIntent",
                Parameters = { { Name = "action", Type = "string" } },
                Returns = { "boolean | number | Vector2 | vector" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the value of this player's input action of the given name for the current tick. "
                    .. "The type is the one `InputAction:GetState` returns: a boolean, a number, a `Vector2` or a "
                    .. "`vector`. For the local player it is this machine's own action; for a remote player it is "
                    .. "the value their machine sent. An action the player never sent reads false.",
            },
            {
                Name = "GetNetworkPing",
                Parameters = {},
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "Returns this player's network round trip, in milliseconds, for a scoreboard. On the "
                    .. "server it is the round trip to that player's machine, which a machine's other players "
                    .. "share. On a client it is this machine's own, for its own players. It is 0 for the "
                    .. "host's own player, in a single-player game, for another machine's player on a client, "
                    .. "and for a player who has left; it never raises.",
            },
            {
                Name = "AddPhase",
                Parameters = { { Name = "phase", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0231" },
                Doc = "Puts this player in `phase`: they see, and collide with, what has that `Phase`. Only the "
                    .. "server calls it; the player's own machine is told, and reads it with `HasPhase` and "
                    .. "`GetPhases`. A world names at most 64 phases, and the 65th raises.",
            },
            {
                Name = "RemovePhase",
                Parameters = { { Name = "phase", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0231" },
                Doc = "Takes this player out of `phase`: what has that `Phase` leaves their machine within a "
                    .. "snapshot. Only the server calls it.",
            },
            {
                Name = "HasPhase",
                Parameters = { { Name = "phase", Type = "string" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0231" },
                Doc = "Whether this player is in `phase`.",
            },
            {
                Name = "GetPhases",
                Parameters = {},
                Returns = { "{string}" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0231" },
                Doc = "The phases this player is in, in no particular order.",
            },
            {
                Name = "CanSee",
                Parameters = { { Name = "instance", Type = "Instance" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0231" },
                Doc = "Whether this player is in the phase of `instance` and of everything above it: what "
                    .. "decides whether it is sent to them. For a game's own hit and interaction code. It says "
                    .. "nothing of distance.",
            },
            {
                Name = "SetReplicationPriority",
                Parameters = { { Name = "instance", Type = "Instance" }, { Name = "priority", Type = "number" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0229" },
                Doc = "How much `instance`'s changes come first for this player alone, on top of its "
                    .. "`BasePart.ReplicationPriority`. Use 4 for a party member's character, and 0.5 for what "
                    .. "this player hardly needs. 1 forgets what was set. It matters only when the player's budget "
                    .. "(`ReplicationBudget`) cannot carry every change at once.\n\n"
                    .. "Only the authority calls it; on a client it raises an error. A player keeps at most 256 "
                    .. "of these; the instance being destroyed forgets it.",
            },
            {
                Name = "Kick",
                Parameters = { { Name = "reason", Type = "string?", Default = "nil" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0167" },
                Doc = "Removes this player from the match. Only the authority can call it, and only on a player "
                    .. "who joined: it raises an error on a client, and when a host calls it on its own player.\n\n"
                    .. "The player's machine returns to single-player, and its `NetworkService.Disconnected` "
                    .. "fires with `reason`: text the game provides, up to 512 bytes, or the engine's default "
                    .. "text. That machine does not reconnect by itself. On the authority, `PlayerRemoving` fires "
                    .. "at the end of the frame, as for any player who leaves. Nothing stops the player from "
                    .. "joining again; to ban a player, keep a list and remove them when they join.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "RunService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Properties = {
            {
                Name = "TimeDilation",
                Type = "number",
                ReadOnly = true,
                Records = { "ADR 0233" },
                -- How often ticks run, never what one does: no world's trace
                -- moves with it.
                HostFact = true,
                Doc = "How fast this world's clock runs against the wall's: 1 at full speed, less while an "
                    .. "overloaded server is slowed (`[server] time_dilation` in `project.toml`). The tick is the "
                    .. "same, so `dt` and what a tick does do not change; fewer ticks run a second, and every "
                    .. "machine in the match runs at the same rate. `task.wait`, animations and sounds driven by "
                    .. "the simulation slow with it; the interface, the camera and `os.time` do not. "
                    .. "`TimeDilationChanged` fires when it changes.",
            },
            {
                Name = "Platform",
                Type = "Enum.Platform",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "The operating system this machine runs: `Windows`, `Linux`, `MacOS`, `Android` or `IOS`. "
                    .. "On a dedicated server, it is the server's operating system.",
            },
            {
                Name = "SimTime",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "ReadParallel",
                Doc = "The simulation time at the current tick, in seconds. It is constant during a tick and "
                    .. "advances by `PhysicsService.FixedTimestep` between ticks. It stops while the world is "
                    .. "paused.\n\n"
                    .. "Simulation code should use this clock. `os.clock` measures real time and is only for "
                    .. "profiling.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Pause",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Pauses the world. The simulation clock stops: `SimTime` stops advancing, and no "
                    .. "`task.wait` or `task.delay` timer completes. Rendering and the debug overlay keep "
                    .. "running. Pausing a world that is already paused does nothing.",
            },
            {
                Name = "Resume",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Resumes a paused world and its clock. Resuming a world that is running does nothing.",
            },
            {
                Name = "BindToRenderStep",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "priority", Type = "number" },
                    { Name = "fn", Type = "(dt: number) -> ()" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0136", "ADR 0124" },
                Doc = "Binds a function that runs as `fn(dt)` on every drawn frame, where `dt` is the time since "
                    .. "the last frame. Use it for a camera that follows something: inside the function, "
                    .. "`GetRenderCFrame` returns where things are drawn, and a camera set there is drawn exactly "
                    .. "as set.\n\n"
                    .. "Bound functions run in order of `priority`, lowest first, and in the order they were "
                    .. "bound when priorities are equal. They run after the input that the frame reads for "
                    .. "contexts with `Rate = Render`, and before `PreRender` fires. `Enum.RenderPriority` "
                    .. "provides reference values: `Enum.RenderPriority.Camera.Value + 1` runs just after the "
                    .. "camera.\n\n"
                    .. "Binding a name that is already bound replaces the function. Nothing runs in a run without "
                    .. "a window. A binding belongs to the script that made it and is removed with that script's "
                    .. "scene.",
            },
            {
                Name = "BindToIntent",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "fn", Type = "(intent: IntentWriter) -> ()" },
                    { Name = "player", Type = "Player?", Default = "nil" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Binds a function that runs as `fn(intent)` on every simulation tick, just after this "
                    .. "machine's input has become its player's intents. Use it for input that is not an "
                    .. "`InputAction`, such as a turn computed from the mouse, a touch gesture or a bot.\n\n"
                    .. "`intent:Set(action, value)` sets an intent by name to a boolean, a number, a `Vector2` or "
                    .. "a `vector`. Every reader of `Player:GetIntent` sees the value, on this machine and, in a "
                    .. "match, on the authority. A value set this way overrides an `InputAction` of the same "
                    .. "name. The `intent` object is valid only during the call.\n\n"
                    .. "The optional `player` argument selects a local guest; with nil, the primary local player "
                    .. "is used. A binding for a guest that has been removed is skipped. Functions run in the "
                    .. "order they were bound. Binding a name that is already bound replaces the function, and a "
                    .. "binding belongs to the script that made it and is removed with that script's scene.",
            },
            {
                Name = "UnbindFromIntent",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes the function bound with `BindToIntent` under `name`. Does nothing if no function "
                    .. "is bound under that name.",
            },
            {
                Name = "BindToPredictedStep",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "fn", Type = "(step: PredictedStep) -> ()" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0157" },
                Doc = "Binds a function that runs as `fn(step)` inside the simulation step, before physics is "
                    .. "solved. It runs once for each player character this machine simulates: every character on "
                    .. "the authority, and the client's own on a client. Use it for movement that a script "
                    .. "controls and a client should predict without correction at any latency, such as a dash, a "
                    .. "double jump, a dodge or a jump pad.\n\n"
                    .. "What the function does to the character is part of the step: calling `Move` or `Jump`, or "
                    .. "setting a velocity, an impulse, a position or an attribute. When a client is corrected "
                    .. "and simulates its character again, the function runs again for each replayed tick, with "
                    .. "the input of that tick, and `step.Replaying` is true.\n\n"
                    .. "Read the player's input from `step` with `GetIntent` and `Pressed`, and measure time with "
                    .. "`step.Tick`, the player's own tick number, which is the same on the client and the "
                    .. "authority. An attribute the function sets on `step.Character` is predicted state: it is "
                    .. "saved every tick, restored before a tick is replayed, and corrected by the authority's "
                    .. "value. Use `step.Random` for random numbers, never an unseeded `Random`.\n\n"
                    .. "The function must not yield; `task.wait` inside it raises an error. Functions run in the "
                    .. "order they were bound. Binding a name that is already bound replaces the function, and a "
                    .. "binding belongs to the script that made it and is removed with that script's scene. In a "
                    .. "single-player game the function runs once per tick and is never replayed.",
            },
            {
                Name = "UnbindFromPredictedStep",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes the function bound with `BindToPredictedStep` under `name`. Does nothing if no "
                    .. "function is bound under that name.",
            },
            {
                Name = "UnbindFromRenderStep",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes the function bound with `BindToRenderStep` under `name`. Does nothing if no "
                    .. "function is bound under that name.",
            },
            {
                Name = "IsPaused",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns whether the world is paused.",
            },
            {
                Name = "GetLastRun",
                Parameters = {},
                Returns = {
                    "{ Outcome: Enum.RunOutcome, ScriptErrors: number, FirstScriptError: string?, Report: string?, "
                        .. "Dump: string? }",
                },
                ThreadSafety = "ReadParallel",
                Records = { "ADR 0187" },
                Doc = "Returns how the previous run of the game ended on this machine, and where its report is. "
                    .. "Call it at startup to offer the player a way to send a crash report: the engine uploads "
                    .. "nothing and shows nothing by itself.\n\n"
                    .. "`Outcome` is `Clean` when the run was asked to end and did, `Crashed` when it ended in a "
                    .. "fault, and `None` when there was no previous run. `Unfinished` means it ended without a "
                    .. "fault and without being asked to, which is normal on a phone, where the system closes "
                    .. "games in the background. `ScriptErrors` is the number of script errors that nothing "
                    .. "caught, and `FirstScriptError` is the first of them.\n\n"
                    .. "`Report` is the path of a text file written for a run that crashed, was left unfinished "
                    .. "or had script errors. The file identifies the game and the engine and holds the crash "
                    .. "note and the end of that run's log. It is nil if the run left nothing to report. `Dump` "
                    .. "is the crash dump next to it, on a system that writes one.\n\n"
                    .. "A typical check is `if last.Outcome == Enum.RunOutcome.Crashed or last.ScriptErrors > 0 "
                    .. "then`. On a phone, the file is in the game's own folder, `Android/data/<package>/files`, "
                    .. "which a cable or the system's file browser can open without a permission. The result "
                    .. "describes this machine and is the same on a host and on a client.",
            },
            {
                Name = "SaveSimulation",
                Parameters = {},
                Returns = { "buffer" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0101" },
                Doc = "Returns the current state of the 3D simulation as a buffer, to roll back to later. It "
                    .. "holds the physics solver's state and the position and motion of every simulated part and "
                    .. "character.\n\n"
                    .. "It does not hold which instances exist, their other properties, the 2D layer or script "
                    .. "variables. A rollback game keeps its own state alongside and restores both. The result is "
                    .. "deterministic: the same world saved on two machines gives the same bytes.",
            },
            {
                Name = "RestoreSimulation",
                Parameters = { { Name = "state", Type = "buffer" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Restores a state that `SaveSimulation` returned, and returns true. Returns false and "
                    .. "changes nothing if the buffer is not a saved simulation, or if the world's simulated "
                    .. "parts and characters differ from those the state was saved with because one was created, "
                    .. "destroyed or anchored since.",
            },
            {
                Name = "StepSimulation",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Advances the 3D simulation by one fixed tick immediately, without running scripts and "
                    .. "without firing `Touched` or `TouchEnded`. Use it to simulate again after "
                    .. "`RestoreSimulation`: for each tick to catch up, set that tick's inputs, such as a "
                    .. "character's `MoveDirection` or an impulse, and call it once.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "TimeDilationChanged",
                Parameters = {},
                Records = { "ADR 0233" },
                Doc = "Fires when `TimeDilation` changes: an overloaded server slowing its world, or bringing it "
                    .. "back to full speed. A game can say so to its players.",
            },
            {
                Name = "PreRender",
                Parameters = { { Name = "dt", Type = "number" } },
                Records = { "ADR 0136" },
                Doc = "Fires every frame before the frame is drawn, after the functions bound with "
                    .. "`BindToRenderStep`. Passes the time since the previous frame, in seconds. Its handlers "
                    .. "run in a render step: `GetRenderCFrame` returns where things are drawn, and a camera set "
                    .. "there is drawn exactly as set. It never fires in a headless run.",
            },
            {
                Name = "PreAnimation",
                Parameters = { { Name = "dt", Type = "number" } },
                Doc = "Fires at the start of each simulation tick, before animation is evaluated. Passes the "
                    .. "fixed tick length, in seconds.",
            },
            {
                Name = "PreSimulation",
                Parameters = { { Name = "dt", Type = "number" } },
                Doc = "Fires each simulation tick, after animation and before the physics step. Passes the fixed "
                    .. "tick length, in seconds.",
            },
            {
                Name = "PostSimulation",
                Parameters = { { Name = "dt", Type = "number" } },
                Doc = "Fires each simulation tick, immediately after the physics step. Passes the fixed tick "
                    .. "length, in seconds.",
            },
            {
                Name = "Heartbeat",
                Parameters = { { Name = "dt", Type = "number" } },
                Doc = "Fires at the end of each simulation tick. Passes the fixed tick length, in seconds. It is "
                    .. "the usual place for gameplay code that runs every tick and needs the state the tick ended "
                    .. "in.",
            },
        } :: { Schema.Event },
        Doc = "Provides the events of the frame loop and the simulation clock. Each frame, `PreRender` fires at "
            .. "the frame rate with a variable time step. Each simulation tick, `PreAnimation`, `PreSimulation`, "
            .. "`PostSimulation` and `Heartbeat` fire, in that order, at the fixed tick rate. Use `PreRender` for "
            .. "work tied to the picture and the others for simulation work.",
    },
    -- ADR 0105: three script services. No members by design: the tree is the
    -- whole API, and WHERE a script is decides where it runs.
    {
        Name = "ServerScriptService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0105" },
        Doc = "Holds the server scripts of the current scene, such as an arena's rules or a lobby's countdown. A "
            .. "`Script` here runs only while this machine is the authority: in single-player, on a host or on a "
            .. "dedicated server. It never reaches a client. It is not replicated, a client's own copy is emptied "
            .. "when the client joins, and an export for clients leaves it out.\n\n"
            .. "At startup, every file matching `src/scenes/<scene>/server/**/*.luau` becomes a `Script` under "
            .. "it, with subfolders as `Folder`s. It belongs to the scene and is unloaded with it.",
    },
    {
        Name = "ClientScriptService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0105" },
        Doc = "Holds the client scripts of the current scene, such as its HUD, its menu or a camera. A `Script` "
            .. "here runs on every machine that has a player: in single-player, on a host and on a client. It "
            .. "never runs on a dedicated server. It is not replicated: each client runs the copy in its own "
            .. "build.\n\n"
            .. "At startup, every file matching `src/scenes/<scene>/client/**/*.luau` becomes a `Script` under "
            .. "it. It belongs to the scene and is unloaded with it.",
    },
    {
        Name = "GlobalScriptService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0105" },
        Doc = "Holds the scripts of the whole game, which run in every scene. It is never unloaded: a script here "
            .. "keeps its variables, connections and threads across scene changes. Use it for state that outlasts "
            .. "a scene, such as the character a player picked, the network connection or a lobby.\n\n"
            .. "It has three fixed folders, which the engine creates and which cannot be renamed, moved or "
            .. "destroyed:\n\n"
            .. "- `Server` runs while this machine is the authority, like `ServerScriptService`. Its scripts come "
            .. "from `src/server/`.\n"
            .. "- `Client` runs on machines that have a player and never on a dedicated server, like "
            .. "`ClientScriptService`. Its scripts come from `src/client/`.\n"
            .. "- `Shared` holds `ModuleScript`s that both sides require. They come from `src/shared/`.\n\n"
            .. "Instances under it that are not code are saved in `content/global.json`, not in a scene.",
    },
    {
        Name = "StreamingService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                ThreadSafety = "ReadParallel",
                Doc = "Whether the world streams. Turning it off freezes the current state: nothing new is loaded "
                    .. "and nothing loaded is unloaded. Use it to hold the world still during an operation.",
            },
            {
                Name = "LoadRadius",
                Type = "number",
                Default = "1024",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_positive",
                Doc = "The distance from a focus within which the engine tries to keep cells loaded, in meters. A "
                    .. "cell inside it is loaded when time allows. A loaded cell is unloaded only beyond a "
                    .. "somewhat larger distance, so a focus at a boundary does not load and unload the same cell "
                    .. "repeatedly.",
            },
            {
                Name = "MinRadius",
                Type = "number",
                Default = "512",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_positive",
                Doc = "The distance from a focus within which cells must be loaded, in meters. Everything inside "
                    .. "it is loaded before a focus moves into it, so a character never walks into an area that "
                    .. "has not loaded. See `PauseOutsideLoadedArea` for what happens when the area is not ready.",
            },
            {
                Name = "PauseOutsideLoadedArea",
                Type = "boolean",
                Default = "false",
                ThreadSafety = "ReadParallel",
                Doc = "Whether the simulation waits while the area within `MinRadius` is not loaded. With it off, "
                    .. "a character can fall through ground that has not loaded. While waiting, ticks do not "
                    .. "advance; nothing is rolled back.\n\n"
                    .. "It has no effect on a client, which cannot pause a simulation it does not control. A "
                    .. "client holds its camera and shows that it is loading instead.",
            },
            {
                Name = "StructureMinRadius",
                Type = "number",
                Default = "0",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance within which cells of structures must be loaded, in meters. It lets large "
                    .. "structures use a larger radius than small detail. At 0, the default, `MinRadius` is used.",
            },
            {
                Name = "StructureLoadRadius",
                Type = "number",
                Default = "0",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance from a focus within which cells of structures are kept loaded, in meters. It "
                    .. "works like `LoadRadius`. At 0, `LoadRadius` is used.",
            },
            {
                Name = "TerrainMinRadius",
                Type = "number",
                Default = "0",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance within which cells of terrain features must be loaded, in meters. It is "
                    .. "usually the largest of the three, so the horizon reaches farther than the objects on it. "
                    .. "At 0, `MinRadius` is used.\n\n"
                    .. "It also applies to a saved `Terrain` and a block world that are streamed in cells. When a "
                    .. "world first loads, the simulation waits until the ground within this radius has loaded, "
                    .. "so nothing falls through it.",
            },
            {
                Name = "TerrainLoadRadius",
                Type = "number",
                Default = "0",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance from a focus within which cells of terrain features are kept loaded, in "
                    .. "meters. At 0, `LoadRadius` is used. A saved `Terrain` and a block world that are streamed "
                    .. "in cells are also kept to this radius.\n\n"
                    .. "A terrain cell holds a few large objects, so raising this radius costs much less than "
                    .. "raising the radius for detail.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "AddFocus",
                Parameters = { { Name = "instance", Type = "Instance" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Adds an instance as a focus, so that the world also streams around it. A focus is usually "
                    .. "the character or the camera. There can be several, and a cell is kept loaded if any focus "
                    .. "needs it. Raises an error for an instance that has no position in the world.",
            },
            {
                Name = "RemoveFocus",
                Parameters = { { Name = "instance", Type = "Instance" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes an instance as a focus. Removing the last focus does not unload the world: what is "
                    .. "loaded stays loaded.",
            },
            {
                Name = "LoadAreaAsync",
                Parameters = {
                    { Name = "position", Type = "vector" },
                    { Name = "radius", Type = "number" },
                },
                Returns = {},
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Yields until everything within `radius` of `position` is loaded, both the cells of parts "
                    .. "and the terrain. Call it before teleporting a character, so that the destination exists "
                    .. "on arrival. Also call it at startup before `Terrain:HeightAt`, which returns nil for "
                    .. "terrain that has not loaded.\n\n"
                    .. "It returns immediately if the area is already loaded. Raises an error if the world has no "
                    .. "cells there.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "AreaLoaded",
                Parameters = {
                    { Name = "position", Type = "vector" },
                    { Name = "radius", Type = "number" },
                },
                Doc = "Fires when an area requested by a script has finished loading.",
            },
            {
                Name = "InstanceStreamedOut",
                Parameters = { { Name = "instance", Type = "Instance" } },
                Doc = "Fires for each instance that is streamed out while a script still holds a reference to it. "
                    .. "Such an instance is not destroyed: its parent becomes nil, so the reference stays valid "
                    .. "and the instance can still be read. An instance that no script refers to is destroyed, "
                    .. "and this event does not fire for it.",
            },
        } :: { Schema.Event },
        Doc = "Controls world streaming. A world too large to keep loaded is divided into a grid of cells, and "
            .. "this service loads and unloads cells by their distance from the focuses a game sets, usually the "
            .. "character or the camera. The work done each frame is limited by time, not by a number of cells.",
    },
    {
        Name = "TagService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Methods = {
            {
                Name = "GetTagged",
                Parameters = { { Name = "tag", Type = "string" } },
                Returns = { "{Instance}" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns every instance that has the given tag, as a new array. An instance whose parent is "
                    .. "nil is included.",
            },
            {
                Name = "GetInstanceAddedSignal",
                Parameters = { { Name = "tag", Type = "string" } },
                Returns = { "Signal<Instance>" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the signal that fires when an instance gains the given tag. Passes the instance. "
                    .. "Like every signal, it is deferred: a handler runs at the next resumption point, not "
                    .. "inside the call to `AddTag`.",
            },
            {
                Name = "GetInstanceRemovedSignal",
                Parameters = { { Name = "tag", Type = "string" } },
                Returns = { "Signal<Instance>" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the signal that fires when an instance loses the given tag, through `RemoveTag` or "
                    .. "through `Destroy`, which removes all of an instance's tags. Passes the instance. A "
                    .. "destroyed instance can still be read inside the handler.",
            },
            {
                Name = "GetAllTags",
                Parameters = {},
                Returns = { "{string}" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns every tag that at least one instance currently has, as a new array. The list "
                    .. "updates immediately when `AddTag` or `RemoveTag` is called: a tag leaves it as soon as "
                    .. "the last instance loses it.",
            },
        } :: { Schema.Method },
        Doc = "Finds instances by tag. A tag is a string label on an instance. Tags are added, removed and tested "
            .. "on the instance, with `AddTag`, `RemoveTag`, `HasTag` and `GetTags`. This service has the queries "
            .. "that cover the whole game, and the signals for a tag being added or removed.",
    },
    {
        Name = "DebugService",
        Extends = "Instance",
        Module = "scene",
        -- Not `DevOnly`: §2.1 keeps the service in shipped builds with the
        -- overlay off. Only the gizmo calls are a development aid, and they are
        -- no-ops rather than absent.
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Properties = {
            {
                Name = "OverlayVisible",
                Type = "boolean",
                Default = "false",
                ThreadSafety = "Unsafe",
                Transient = true,
                Doc = "Whether the debug overlay is drawn. It is off by default, including in exported games.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "DrawLine",
                Parameters = {
                    { Name = "a", Type = "vector" },
                    { Name = "b", Type = "vector" },
                    { Name = "color", Type = "Color3?", Default = "nil" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Draws a line between two positions in world space for one frame. Does nothing in a "
                    .. "headless run.",
            },
            {
                Name = "DrawBox",
                Parameters = {
                    { Name = "cframe", Type = "CFrame" },
                    { Name = "size", Type = "vector" },
                    { Name = "color", Type = "Color3?", Default = "nil" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Draws the wireframe of a box at a `CFrame`, with a size in meters, for one frame. Does "
                    .. "nothing in a headless run.",
            },
            {
                Name = "DrawSphere",
                Parameters = {
                    { Name = "position", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "color", Type = "Color3?", Default = "nil" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Draws the wireframe of a sphere at a position in world space, with a radius in meters, for "
                    .. "one frame. Does nothing in a headless run.",
            },
            {
                Name = "CaptureProfile",
                Parameters = {
                    { Name = "seconds", Type = "number" },
                    { Name = "warmupSeconds", Type = "number?", Default = "0" },
                },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Requests a capture of the main thread's CPU scopes, starting and ending at frame "
                    .. "boundaries, with durations in seconds of real time. The measurement must be longer than 0 "
                    .. "and at most 120 seconds, and the warm-up from 0 to 120 seconds. Returns false while "
                    .. "another capture is pending, or for a duration that is out of range or not finite. It is a "
                    .. "diagnostic tool; do not use its results in the simulation.",
            },
            {
                Name = "GetProfileReport",
                Parameters = {},
                Returns = { "any" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the report of the capture requested with `CaptureProfile`, or nil until the "
                    .. "capture completes. The report is a table with `frames`, `seconds`, `ticksPerFrame` and "
                    .. "`scopes`. Each scope has `name`, `depth`, `meanMs`, `bestMs`, `medianMs`, `p95Ms`, "
                    .. "`p99Ms`, `worstMs`, `selfMedianMs`, `calls` and `perCallMs`.\n\n"
                    .. "The times are a frame's: what the scope spent in one frame, over the frames captured. "
                    .. "`calls` is how many times it ran in a frame, at the median, and `perCallMs` is one run "
                    .. "of it. `ticksPerFrame` is how many simulation ticks a frame ran, at the median: 1 for a "
                    .. "world keeping up. A server catching up runs several in a frame, and a tick's scopes "
                    .. "then show several ticks' time in each frame; read `perCallMs` for one tick.\n\n"
                    .. "A scope's times include its child scopes and any waiting, and they are CPU times, not GPU "
                    .. "times. At most the latest 16,384 frames are kept. The report stays available until the "
                    .. "next capture is requested.",
            },
            {
                Name = "GetStat",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "number" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns the value of a named statistic. The built-in statistics include `FPS`, "
                    .. "`FrameTimeMs`, `PhysicsBodies`, `InstanceCount`, `DrawCalls`, `MeshLodDraws` and "
                    .. "`LuaMemoryKB`, and `SetCustomStat` defines more. Raises an error for a name that is not a "
                    .. "statistic.",
            },
            {
                Name = "SetCustomStat",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "value", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Defines or updates a statistic of the game's own under a name. It can then be read with "
                    .. "`GetStat` and is shown in the Stats panel. Raises an error if the value is not a number.",
            },
            {
                Name = "ShowPanel",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Opens one panel of the debug overlay by name. The built-in panels are `Stats`, `Scene`, "
                    .. "`Log`, `Streaming`, `Physics` and `Terrain`. The `Terrain` panel shows the terrain's "
                    .. "triangles and normals around the camera, in red where a triangle faces the wrong way. "
                    .. "Raises an error for an unknown panel name.",
            },
            {
                Name = "HidePanel",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Closes one panel of the debug overlay by name, with the same names as `ShowPanel`. Raises "
                    .. "an error for an unknown panel name.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "MessageOut",
                Parameters = {
                    { Name = "message", Type = "string" },
                    { Name = "level", Type = "Enum.LogLevel" },
                },
                Doc = "Fires for every console message. Passes the text and its level. Each `print` fires it once "
                    .. "with its text at `Info`, and each `warn` once at `Warning`. Engine messages start with "
                    .. "their key in square brackets, as in `[scene.err.parent_cycle] ...`, so a handler can "
                    .. "match a key to find engine messages. Errors raised in signal handlers are also reported "
                    .. "here.",
            },
        } :: { Schema.Event },
        Doc = "Controls the debug overlay and gives access to the engine's statistics and log. It is available in "
            .. "exported games, with the overlay off unless the game turns it on. The gizmo methods are a "
            .. "development aid and draw nothing in a headless run.",
    },
    {
        Name = "HotReloadService",
        Extends = "Instance",
        Module = "scene",
        -- `DevOnly`: api-design.md §2.1 keeps this service out of shipped
        -- builds entirely, which is the one honest answer for a service whose
        -- whole subject is a development loop.
        Tags = { "Service", "NotCreatable", "DevOnly" } :: { Schema.ClassTag },
        Methods = {
            {
                Name = "SaveState",
                Parameters = {
                    { Name = "key", Type = "string" },
                    { Name = "value", Type = "any" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stores a value under `key` so that the world built by the next reload can read it. The "
                    .. "value is copied when the method is called. It can be nil, a boolean, a number, a string, "
                    .. "a buffer, or a table of those. Raises an error for a function, a thread, an instance or a "
                    .. "table that contains itself.",
            },
            {
                Name = "LoadState",
                Parameters = { { Name = "key", Type = "string" } },
                Returns = { "any" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the value that `SaveState` stored under `key` before the reload, or nil if there "
                    .. "is none. The result is a new copy: changing it does not change what is stored.",
            },
            {
                Name = "IsReload",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Safe",
                Doc = "Returns whether this world was built by a hot reload instead of by starting the engine. "
                    .. "Use it to reuse what the previous world left instead of building it again. Instances "
                    .. "tagged `PreserveOnReload` are already in the tree when the first entry script runs.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "PreReload",
                Parameters = {},
                Doc = "Fires in the old world when a reload has been requested, before anything is removed. It is "
                    .. "the last moment at which `SaveState` can be called. The reload waits for its handlers to "
                    .. "finish.",
            },
            {
                Name = "PostReload",
                Parameters = {},
                Doc = "Fires in the world a reload has built, after every entry script has run for the first time "
                    .. "and the preserved state is in place.",
            },
        } :: { Schema.Event },
        Records = { "ADR 0024" },
        Doc = "Gives scripts access to hot reload during development. It exists only in development builds, not "
            .. "in exported games. A reload rebuilds the world from source and keeps nothing by default: only "
            .. "values stored with `SaveState` and instances tagged `PreserveOnReload` survive.",
    },
    {
        Name = "PhysicsService",
        Extends = "Instance",
        -- `scene` rather than a physics module, because in this release the one
        -- member it carries is scheduler state that physics merely names.
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Properties = {
            {
                Name = "FixedTimestep",
                Type = "number",
                Default = "1/60",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_fixed_timestep",
                Doc = "The length of one simulation tick, in seconds. The default is 1/60. Every timer counts in "
                    .. "ticks: `task.wait(1)` is exactly 60 ticks at the default, on every machine and in every "
                    .. "run. Code that expresses durations as multiples of this value works at any tick rate.\n\n"
                    .. "A new value takes effect at the start of the next frame. Setting a value outside 1/240 to "
                    .. "1/30 raises an error.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "RegisterCollisionGroup",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Registers a collision group. A new group collides with every group until "
                    .. "`CollisionGroupSetCollidable` says otherwise. Registering a name that already exists does "
                    .. "nothing, so a script can register its groups every time it starts.",
            },
            {
                Name = "CollisionGroupSetCollidable",
                Parameters = {
                    { Name = "a", Type = "string" },
                    { Name = "b", Type = "string" },
                    { Name = "collidable", Type = "boolean" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets whether parts in two groups collide with each other. The setting is symmetric: "
                    .. "setting it for a and b also sets it for b and a.",
            },
            {
                Name = "GetRegisteredCollisionGroups",
                Parameters = {},
                Returns = { "{string}" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the names of every registered group as a new array, in the order they were "
                    .. "registered, with `Default` first.",
            },
        } :: { Schema.Method },
        Doc = "Holds the simulation's tick length and the physics settings that do not belong to a single part, "
            .. "such as collision groups.",
    },
    {
        Name = "InputService",
        Extends = "Instance",
        Module = "input",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0029" },
        Doc = "Reads the state of the input devices and hosts the input action system. For gameplay input, use "
            .. "`InputAction`, which can be rebound and can show prompts; this service is for device state and "
            .. "for direct key and pointer reads.",
        Properties = {
            {
                Name = "PointerLocked",
                Type = "boolean",
                Default = "false",
                Transient = true,
                Doc = "Whether the pointer is locked to the window and reports movement instead of position, as a "
                    .. "first-person camera needs. While it is locked, `GetPointerPosition` returns the position "
                    .. "the pointer had when it was locked. To read the movement, bind a `Direction2D` action to "
                    .. "`MouseMovement`.\n\n"
                    .. "While the pointer is locked, the interface ignores it: nothing is hovered or pressed, and "
                    .. "no `Active` object receives it. The keyboard and a gamepad can still select UI objects. "
                    .. "To let the player click the interface, unlock the pointer first, as a menu does.",
            },
            {
                Name = "PointerVisible",
                Type = "boolean",
                Default = "true",
                Transient = true,
                Doc = "Whether the system cursor is drawn. It is independent of `PointerLocked`: a strategy game "
                    .. "can hide the cursor during a cutscene without locking it.",
            },
            {
                Name = "PreferredInput",
                Type = "Enum.InputDeviceType",
                ReadOnly = true,
                Transient = true,
                -- Presentation alias; LastInputDeviceType already hashes the category.
                Presentation = true,
                Doc = "The kind of input the player is using: `KeyboardMouse`, `Gamepad` or `Touch`. It has the "
                    .. "same value as `LastInputDeviceType`, and `InputDeviceChanged` fires when it changes. For "
                    .. "the family of the gamepad, use `PreferredGamepadType`.",
            },
            {
                Name = "PreferredGamepadType",
                Type = "Enum.GamepadType",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "The family of the gamepad the player last used. Connecting a gamepad does not select it; "
                    .. "the player must use it. The value is kept when the player switches to keyboard or touch, "
                    .. "and becomes `Unknown` when that gamepad disconnects. Use it to choose button icons.",
            },
            {
                Name = "PreferredGamepadId",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "The connection id of the gamepad the player last used, or 0 if none is selected. A gamepad "
                    .. "that reconnects gets a new id. The value is kept when the player switches to keyboard or "
                    .. "touch. It is not a player index.",
            },
            {
                Name = "LastInputDeviceType",
                Type = "Enum.InputDeviceType",
                ReadOnly = true,
                Doc = "The kind of device the player most recently used. Use it to switch a HUD's button prompts. "
                    .. "It changes only on deliberate input: a gamepad stick resting inside its dead zone does "
                    .. "not change it.",
            },
            {
                Name = "SwipeThreshold",
                Type = "number",
                Default = "6",
                Transient = true,
                HostFact = true,
                ErrorKey = "scene.err.number_positive",
                Doc = "How far a finger must travel before the movement counts as a swipe, in millimeters on the "
                    .. "screen. The default is 6. The engine converts it to pixels from the display's pixel "
                    .. "density, so it is the same physical distance on a phone and on a monitor.",
            },
            {
                Name = "TouchAvailable",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "Whether this machine has a touchscreen. It is known from startup, before any touch, so a "
                    .. "game can choose which HUD to build before its first frame. It is true on a phone and on a "
                    .. "laptop with a touchscreen.",
            },
            {
                Name = "KeyboardAvailable",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "Whether a keyboard is attached. On a phone it is false until a keyboard is connected or paired.",
            },
            {
                Name = "GamepadAvailable",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "Whether at least one gamepad is connected. It changes as gamepads are connected and "
                    .. "disconnected. `InputDeviceChanged` fires when one is first used.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetGamepads",
                Parameters = {},
                Returns = { "{number}" },
                Doc = "Returns the ids of the connected gamepads, in ascending order. `GamepadConnected` and "
                    .. "`GamepadDisconnected` fire when the list changes. Connecting a gamepad does not change "
                    .. "`PreferredInput` or assign the gamepad to a local player.",
            },
            {
                Name = "IsGamepadKeyDown",
                Parameters = { { Name = "gamepadId", Type = "number" }, { Name = "keyCode", Type = "Enum.KeyCode" } },
                Returns = { "boolean" },
                Doc = "Returns whether a key of one gamepad, given by its id, is held. It reads the device "
                    .. "directly, whether or not the interface consumed the input. Returns false for a gamepad "
                    .. "that is not connected and for a key that is not a gamepad key. An analog input counts as "
                    .. "held beyond half of its range.",
            },
            {
                Name = "GetPointerPosition",
                Parameters = {},
                Returns = { "Vector2" },
                Doc = "Returns the position of the pointer in window pixels, measured from the top left, for the "
                    .. "current tick. The value does not change within a tick.",
            },
            {
                Name = "IsKeyDown",
                Parameters = { { Name = "keyCode", Type = "Enum.KeyCode" } },
                Returns = { "boolean" },
                Doc = "Returns whether a key, button or trigger is held, for the current tick. An analog input "
                    .. "counts as held beyond half of its range.\n\n"
                    .. "It reads the device directly and ignores whether the interface consumed the input. For "
                    .. "input that respects the interface and can be rebound, use an `InputAction`.",
            },
            {
                Name = "SetVirtualState",
                Parameters = {
                    { Name = "keyCode", Type = "Enum.KeyCode" },
                    { Name = "value", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets the value of one of the `Virtual` key codes from something that is not hardware. Use "
                    .. "it for an on-screen button, an on-screen thumbstick or an accessibility control.\n\n"
                    .. "The value is treated like input from a device. It drives actions through an ordinary "
                    .. "`InputBinding`, obeys context priority and `Sink`, and is recorded in a replay.\n\n"
                    .. "The value is a number, not a press. Use 1 and 0 for a button, and values in between for a "
                    .. "slider or a thumbstick axis. A `Bool` action bound to it counts as pressed beyond 0.5.\n\n"
                    .. "The value stays until it is set again; it is cleared when the window loses focus. Raises "
                    .. "an error for a key code that is not one of the `Virtual` codes.",
            },
            {
                Name = "SetClipboard",
                Parameters = { { Name = "text", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0177" },
                Doc = "Copies text to the player's clipboard, such as a room code, a seed or a link. The "
                    .. "clipboard cannot be read: there is no method that returns its contents.\n\n"
                    .. "The text goes to the clipboard of the machine that runs the script, so call it from a "
                    .. "client script. It does nothing on a server or in a headless run. The limit is 64 KiB; "
                    .. "longer text is cut at a character boundary. If it is called several times in one frame, "
                    .. "the last call is kept.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "InputBegan",
                Parameters = {
                    { Name = "input", Type = "InputObject" },
                    { Name = "uiConsumed", Type = "boolean" },
                },
                Records = { "ADR 0041" },
                Doc = "Fires on the tick an input starts: a key or button is pressed, or a trigger passes half of "
                    .. "its range.\n\n"
                    .. "The second argument, `uiConsumed`, is true when the interface already used the input, "
                    .. "because the pointer was over a UI object or a `TextInput` had focus. Check it, or the "
                    .. "game reacts to clicks on buttons and to text typed into a chat box.\n\n"
                    .. "A key handled through this event cannot be rebound by the player and does not appear in a "
                    .. "settings menu. For gameplay input, an `InputAction` is usually the better choice; this "
                    .. "event suits prototypes, debug keys and input that players never remap.\n\n"
                    .. "It fires on simulation ticks, so a handler that changes the world is reproduced exactly "
                    .. "in a replay. For camera look at the frame rate, use an `InputContext` with `Rate` set to "
                    .. "`Enum.InputRate.Render`.",
            },
            {
                Name = "InputChanged",
                Parameters = {
                    { Name = "input", Type = "InputObject" },
                    { Name = "uiConsumed", Type = "boolean" },
                },
                Doc = "Fires on a tick in which an input changed without starting or ending: the pointer moved, "
                    .. "the wheel turned, or a stick or trigger changed position. `Position` and `Delta` hold the "
                    .. "change.\n\n"
                    .. "It fires at most once per tick for each source, with the deltas accumulated since the "
                    .. "previous event, so fast movement is not lost. As with `InputBegan`, input handled here "
                    .. "cannot be rebound by the player.",
            },
            {
                Name = "InputEnded",
                Parameters = {
                    { Name = "input", Type = "InputObject" },
                    { Name = "uiConsumed", Type = "boolean" },
                },
                Doc = "Fires on the tick an input stops. It fires for every held input when the window loses "
                    .. "focus, so a handler that pairs it with `InputBegan` never leaves an input stuck.\n\n"
                    .. "`uiConsumed` has the value it had when the input began, so a press that started on a "
                    .. "button is still marked as consumed when it is released elsewhere.",
            },
            {
                Name = "TouchSwiped",
                Parameters = {
                    { Name = "direction", Type = "Enum.SwipeDirection" },
                    { Name = "start", Type = "Vector2" },
                    { Name = "fingers", Type = "number" },
                },
                Doc = "Fires when a finger, or the mouse with its left button held, travels `SwipeThreshold` "
                    .. "along one axis. It fires while the finger is still down, with the position where the "
                    .. "stroke began, in window pixels, and the number of fingers down.\n\n"
                    .. "One long drag in one direction is one swipe. A change of direction starts the next, "
                    .. "measured from the finger's position at that moment. The same swipe also drives "
                    .. "`Enum.KeyCode.SwipeUp` and its three siblings for an `InputAction`. A press that the "
                    .. "interface consumed (see `UIObject.Active`) starts no touch gesture.",
            },
            {
                Name = "TouchTapped",
                Parameters = { { Name = "position", Type = "Vector2" } },
                Doc = "Fires when a finger touches and lifts at the same place within a third of a second. Passes "
                    .. "the position, in window pixels. A mouse click on the world also fires it.",
            },
            {
                Name = "TouchLongPressed",
                Parameters = { { Name = "position", Type = "Vector2" } },
                Doc = "Fires once when a finger has stayed where it touched for half a second, while it is still "
                    .. "down. Lifting the finger afterward does not count as a tap.",
            },
            {
                Name = "TouchPinched",
                Parameters = { { Name = "scale", Type = "number" }, { Name = "center", Type = "Vector2" } },
                Doc = "Fires on each tick in which two fingers move apart or together. Passes the distance "
                    .. "between them relative to the distance when the second finger touched, so 2 means twice as "
                    .. "far apart, and the point midway between them. To zoom, multiply the zoom the game had "
                    .. "before the pinch by this ratio.",
            },
            {
                Name = "TouchPanned",
                Parameters = { { Name = "delta", Type = "Vector2" }, { Name = "fingers", Type = "number" } },
                Doc = "Fires when the fingers that are down move. Passes how far they moved in this tick, in "
                    .. "window pixels, as a single motion however many fingers there are; +Y is down the window, "
                    .. "as for `Position`. Use it for a camera that is dragged with one finger or two.",
            },
            {
                Name = "PreferredGamepadTypeChanged",
                Parameters = { { Name = "gamepadType", Type = "Enum.GamepadType" } },
                Doc = "Fires when `PreferredGamepadType` changes. It is independent of `InputDeviceChanged`.",
            },
            {
                Name = "PreferredGamepadIdChanged",
                Parameters = { { Name = "gamepadId", Type = "number" } },
                Doc = "Fires when `PreferredGamepadId` changes, including when the player switches between two "
                    .. "gamepads of the same family.",
            },
            {
                Name = "GamepadConnected",
                Parameters = {
                    { Name = "gamepadId", Type = "number" },
                    { Name = "gamepadType", Type = "Enum.GamepadType" },
                },
                Doc = "Fires when a gamepad connects. Connecting does not change `PreferredInput` or the "
                    .. "preferred gamepad.",
            },
            {
                Name = "GamepadDisconnected",
                Parameters = {
                    { Name = "gamepadId", Type = "number" },
                    { Name = "gamepadType", Type = "Enum.GamepadType" },
                },
                Doc = "Fires when a gamepad disconnects. Passes the id it had and its family. Other connected "
                    .. "gamepads are not affected.",
            },
            {
                Name = "InputDeviceChanged",
                Parameters = { { Name = "deviceType", Type = "Enum.InputDeviceType" } },
                Doc = "Fires when `LastInputDeviceType` changes. Use it to redraw button prompts.",
            },
            {
                Name = "WindowFocusChanged",
                Parameters = { { Name = "focused", Type = "boolean" } },
                Doc = "Fires when the game window gains or loses keyboard focus. Losing focus releases every held "
                    .. "input.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "TweenService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Animates properties over time. Use it to move, fade or resize something over a duration with "
            .. "easing, instead of writing a timer in a `Heartbeat` handler.",
        Methods = {
            {
                Name = "Create",
                Parameters = {
                    { Name = "instance", Type = "Instance" },
                    { Name = "tweenInfo", Type = "TweenInfo" },
                    { Name = "goals", Type = "{ [string]: any }" },
                },
                Returns = { "Tween" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a tween that moves each named property of `instance` to its goal value. The tween "
                    .. "is created stopped; call `Play` to start it.\n\n"
                    .. "Every goal is checked when the tween is created. Raises an error for a property the class "
                    .. "does not have, a read-only property or a value of the wrong type. Only properties that "
                    .. "can be interpolated are accepted: numbers, vectors, `CFrame`, `Color3`, `Vector2`, "
                    .. "`UDim`, `UDim2` and `Rect`. A boolean or string goal raises an error.",
            },
            {
                Name = "GetValue",
                Parameters = {
                    { Name = "alpha", Type = "number" },
                    { Name = "easingStyle", Type = "Enum.EasingStyle" },
                    { Name = "easingDirection", Type = "Enum.EasingDirection" },
                },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "Returns `alpha` eased by an easing style and direction, with the same calculation a tween "
                    .. "uses. Use it to ease a value that is not a property. `alpha` is not clamped, and some "
                    .. "styles, such as `Back` and `Elastic`, return values outside 0 to 1.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "UIService",
        Extends = "Instance",
        Module = "ui",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "The parent of every `ScreenGui`. It also provides the properties a layout needs about the screen, "
            .. "and the gamepad selection.",
        Properties = {
            {
                Name = "SafeAreaInsets",
                Type = "Rect",
                ReadOnly = true,
                Transient = true,
                -- A fact about the window, not about the world.
                HostFact = true,
                Doc = "The distance from each edge of the window within which drawing is not safe, in pixels, "
                    .. "because of a camera cutout, rounded corners or a system gesture bar. It is zero for a "
                    .. "desktop window.",
            },
            {
                Name = "ViewportSize",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                -- The size of what is drawn into: the host's, like the two below.
                HostFact = true,
                Doc = "The size of the area the interface is drawn into, in pixels: the window in a game, or the "
                    .. "Viewport panel in the editor. `AbsolutePosition` and the pointer position are measured in "
                    .. "it, and a `ScreenGui` with `ReferenceHeight` is scaled to it.",
            },
            {
                Name = "DisplayScale",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                -- A fact about the display, not about the world. A trace recorded
                -- on a doubled screen must replay on an ordinary one.
                HostFact = true,
                Doc = "The ratio of the window's pixels to its logical size: 2 on a display at 200% scaling. UI "
                    .. "coordinates are in pixels, so multiply by this value to give something the same physical "
                    .. "size on different screens.",
            },
            {
                Name = "ScreenOrientation",
                Type = "Enum.ScreenOrientation",
                Default = "Enum.ScreenOrientation.LandscapeSensor",
                -- How the window is presented, which reaches nothing the
                -- simulation reads: kept out of the world hash, so adding it
                -- moved no recorded trace.
                HostFact = true,
                Doc = "The orientations in which the game can be held on a phone or tablet. Setting it takes "
                    .. "effect immediately and rotates the screen if needed. The value saved with the scene is "
                    .. "the one the game starts with. A layout sees the new `AbsoluteSize` on the next frame. It "
                    .. "has no effect on a desktop.",
            },
            {
                Name = "SelectedObject",
                Type = "UIObject?",
                Transient = true,
                -- Where a hand has the interface, like the focus: not the world's.
                HostFact = true,
                Records = { "ADR 0128" },
                Doc = "The UI object selected by a gamepad or the arrow keys, or nil. The D-pad, the left stick "
                    .. "and the arrow keys move the selection: to the object's `NextSelectionUp`, `Down`, `Left` "
                    .. "or `Right` if set, and otherwise to the nearest selectable object in that direction. "
                    .. "`ButtonA` or Enter fires the selected object's `Activated`.\n\n"
                    .. "Set it to move the selection, for example to the first button when a menu opens. Raises "
                    .. "an error for an object that is not `Selectable`.",
            },
            {
                Name = "GamepadId",
                Type = "number",
                Default = "0",
                Transient = true,
                HostFact = true,
                Doc = "The id of the gamepad that can navigate the interface and activate `SelectedObject`. At 0, "
                    .. "every gamepad can. With a positive id, only that gamepad navigates menus; the others "
                    .. "still reach `InputService` and their `InputContext`s. Keyboard and pointer navigation are "
                    .. "not affected. Changing the id releases any held navigation input.",
            },
            {
                Name = "AutoSelect",
                Type = "boolean",
                Default = "true",
                HostFact = true,
                Doc = "Whether the engine manages the selection when the game does not. When it is on and nothing "
                    .. "is selected, the first press of a D-pad, a stick or an arrow key selects the first "
                    .. "selectable object on the topmost screen, and a pointer press clears the selection, so a "
                    .. "player who uses the mouse sees no selection outline. When it is off, the selection "
                    .. "changes only when a script sets it.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetFocusedTextInput",
                Parameters = {},
                Returns = { "TextInput?" },
                Records = { "ADR 0139" },
                Doc = "Returns the `TextInput` that has keyboard focus, or nil. A chat hotkey can check it before "
                    .. "opening the chat box.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "TextInputFocused",
                Parameters = { { Name = "input", Type = "TextInput" } },
                Doc = "Fires when a `TextInput` gains keyboard focus.",
            },
            {
                Name = "TextInputFocusReleased",
                Parameters = { { Name = "input", Type = "TextInput" } },
                Doc = "Fires when a `TextInput` loses keyboard focus.",
            },
            {
                Name = "SelectionChanged",
                Parameters = { { Name = "selected", Type = "UIObject?" } },
                Doc = "Fires when `SelectedObject` changes. Passes the newly selected object, or nil.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "AudioService",
        Extends = "Instance",
        Module = "audio",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Controls audio mixing: the master volume, the player's volume settings and one-shot sounds. The "
            .. "listener is always `Workspace.CurrentCamera`.",
        Properties = {
            {
                Name = "MasterVolume",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "A multiplier on every sound, applied after the sound's own volume and its group's. It "
                    .. "belongs to the game: use it for a fade or a cutscene. It is not saved. For the player's "
                    .. "volume setting, use `PlayerVolume`.",
            },
            {
                Name = "PlayerVolume",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_out_of_range",
                Transient = true,
                -- How loud a person wants their game: a fact about the player.
                HostFact = true,
                Records = { "ADR 0200" },
                Doc = "The player's master volume, from 0 to 1, applied to every sound on this machine. It is a "
                    .. "player setting, like the graphics settings: a value a script sets is kept for the next "
                    .. "run, it is not saved with a scene, and it is not replicated. Setting it on a dedicated "
                    .. "server raises an error.",
            },
            {
                Name = "MusicVolume",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_out_of_range",
                Transient = true,
                HostFact = true,
                Doc = "The player's volume for every `Sound` whose `Category` is `Music`, from 0 to 1. It is a "
                    .. "player setting, kept like `PlayerVolume`.",
            },
            {
                Name = "EffectsVolume",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_out_of_range",
                Transient = true,
                HostFact = true,
                Doc = "The player's volume for every `Sound` whose `Category` is `Effects`, from 0 to 1. It is a "
                    .. "player setting, kept like `PlayerVolume`.",
            },
            {
                Name = "VoiceVolume",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_out_of_range",
                Transient = true,
                HostFact = true,
                Doc = "The player's volume for every `Sound` whose `Category` is `Voice`, from 0 to 1. It is a "
                    .. "player setting, kept like `PlayerVolume`.",
            },
            {
                Name = "MusicUnderVoice",
                Type = "number",
                Default = "0.4",
                ErrorKey = "scene.err.number_out_of_range",
                Records = { "ADR 0200" },
                Doc = "The factor music is multiplied by while a voice plays, from 0 to 1. At 1, music is not "
                    .. "lowered.\n\n"
                    .. "While any `Voice` sound is audible on a machine, every `Music` sound there fades down to "
                    .. "this factor over 0.15 seconds, and back up over 0.6 seconds when the voice stops. A voice "
                    .. "is audible when it is playing, in range and not muted by the player. The value belongs to "
                    .. "the game, like `MasterVolume`. The fade happens in each machine's mixer, and scripts "
                    .. "cannot read it.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "PlayLocal",
                Parameters = { { Name = "content", Type = "Content" } },
                Returns = { "Sound" },
                ThreadSafety = "Unsafe",
                Doc = "Creates a `Sound` that is not positional, parents it to this service, plays it and returns "
                    .. "it. Use it for a one-shot sound such as a click or a pickup.\n\n"
                    .. "The sound is destroyed shortly after it ends, after handlers of its `Ended` event have "
                    .. "run. Use the returned sound to set its volume or category. A sound that the caller stops "
                    .. "or sets to loop is not destroyed automatically.",
            },
            {
                Name = "GetBands",
                Parameters = { { Name = "category", Type = "Enum.SoundCategory" } },
                Returns = { "number", "number", "number" },
                Records = { "ADR 0200" },
                Doc = "Returns the low, middle and high levels of every audible sound of one category on this "
                    .. "machine, mixed together, as three numbers from 0 to 1. It is `Sound:GetBands` for a whole "
                    .. "category. Use it to drive a visualizer from the music.\n\n"
                    .. "The mix includes each sound's volume, its group's volume, the master volume and distance, "
                    .. "but not the player's volume settings. It is computed only when requested.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "DialogueService",
        LazyService = true,
        Extends = "Instance",
        Module = "audio",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0200" },
        Doc = "Plays lines of dialogue and reports the captions that are current.\n\n"
            .. "A line is a sound with a caption, defined in a file: `content/dialogue/<name>.lines.json` holds a "
            .. "table of speakers and a table of lines by id. A line's full id is `<name>.<id>`. Unless the entry "
            .. "says otherwise, its text is the localization key of the same name and its sound is "
            .. "`asset://voice/<name>/<id>.ogg`. `Say` plays a line.\n\n"
            .. "Each player hears a line in their voice language and reads it in their text language. The sound "
            .. "file is chosen where it is played, from `LocalizationService.VoiceLocale`, and the caption's text "
            .. "is a localization key. A line with no recording is silent and lasts as long as its caption, so "
            .. "dialogue can be played as text before it is recorded.\n\n"
            .. "Captions and subtitle settings belong to the player's machine. They are not saved with a scene "
            .. "and are not replicated, and a dedicated server has none.",
        Properties = {
            {
                Name = "Subtitles",
                Type = "Enum.SubtitleMode",
                Default = "Enum.SubtitleMode.Auto",
                Transient = true,
                -- How a person wants to read what is said: a fact about the player.
                HostFact = true,
                Doc = "When captions are shown: `Auto`, `On` or `Off`. It is a player setting: a value a script "
                    .. "sets is kept for the next run. Setting it on a dedicated server raises an error. "
                    .. "`ShowsCaption` applies it to one caption.",
            },
            {
                Name = "SubtitleScale",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_out_of_range",
                Transient = true,
                HostFact = true,
                Doc = "The size of caption text, from 0.75 to 2. It is a player setting, kept like `Subtitles`.",
            },
            {
                Name = "SubtitleBackground",
                Type = "number",
                Default = "0.6",
                ErrorKey = "scene.err.number_out_of_range",
                Transient = true,
                HostFact = true,
                Doc = "The opacity of the background behind captions, from 0 (none) to 1. It is a player setting, "
                    .. "kept like `Subtitles`.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Say",
                Parameters = {
                    { Name = "line", Type = "string" },
                    { Name = "at", Type = "Instance?" },
                },
                Returns = { "Sound" },
                ThreadSafety = "Unsafe",
                Doc = "Plays a line of dialogue and returns its sound. `line` is `<name>.<id>`, the file and the "
                    .. "entry. Raises an error if no file has the line.\n\n"
                    .. "It creates a `Voice` sound with a `Caption` from the line's entry and parents it to `at`, "
                    .. "or to `Workspace` if `at` is nil. If `at` is a part, the line is heard from there. The "
                    .. "sound is destroyed shortly after it ends. Use the returned sound to wait for `Ended` or "
                    .. "to stop the line.\n\n"
                    .. "Called on a server, the sound replicates like any instance: every player hears the line "
                    .. "in their own voice language and reads it in their own text language. Called on a client, "
                    .. "the line plays only on that machine.",
            },
            {
                Name = "GetCaptions",
                Parameters = {},
                Returns = { "{ Caption }" },
                Doc = "Returns the captions that are current on this machine, in the order their sounds started: "
                    .. "every `Caption` whose `Sound` is playing and can be heard here. Returns an empty list on "
                    .. "a dedicated server.",
            },
            {
                Name = "ShowsCaption",
                Parameters = { { Name = "caption", Type = "Caption" } },
                Returns = { "boolean" },
                Doc = "Returns whether a caption should be shown, according to `Subtitles`. It is always true "
                    .. "with `On` and always false with `Off`. With `Auto`, it is true when the player does not "
                    .. "hear the line in the language they read: the voice language differs from the text "
                    .. "language, the sound fell back to the default language's recording while the text is in "
                    .. "another language, or the sound has no recording.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "CaptionStarted",
                Parameters = { { Name = "caption", Type = "Caption" } },
                Doc = "Fires when a caption becomes current on this machine: its sound started or came into range.",
            },
            {
                Name = "CaptionEnded",
                Parameters = { { Name = "caption", Type = "Caption" } },
                Doc = "Fires when a caption stops being current on this machine: its sound ended, was stopped, "
                    .. "went out of range or was destroyed.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "VoxelService",
        Extends = "Instance",
        Module = "scene",
        Storage = "Voxel",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "A world made of blocks, for games about mining and building with cubes. It is separate from "
            .. "`Terrain`, which is a smooth sculpted landscape: this is a grid of cubes, each of one block type, "
            .. "with flat axis-aligned faces.\n\n"
            .. "A block is addressed by integer block coordinates, as a `vector`. Block `(0, 0, 0)` extends from "
            .. "the origin to `BlockSize` along every axis. Register the game's block types once, then place and "
            .. "remove blocks by id. Id 0 is air.\n\n"
            .. "The engine draws the blocks and makes them solid. Each chunk is meshed with hidden faces removed "
            .. "and coplanar faces merged, and shaded with ambient occlusion at the corners.",
        Properties = {
            {
                Name = "BlockSize",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.voxel_not_empty",
                Doc = "The size of one block, in meters. It can be set only while the world has no blocks; "
                    .. "otherwise setting it raises an error. Call `Clear` first to change it in a built world.",
            },
            {
                Name = "ChunkCount",
                Type = "number",
                ReadOnly = true,
                Doc = "The number of 16 by 16 by 16 chunks that hold at least one block. A chunk with no blocks "
                    .. "is not stored.",
            },
            {
                Name = "BlockTypeCount",
                Type = "number",
                ReadOnly = true,
                Doc = "The number of registered block types. Their ids run from 1 to this number.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "RegisterBlock",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "color", Type = "Color3" },
                    { Name = "sideColor", Type = "Color3?" },
                    { Name = "bottomColor", Type = "Color3?" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Registers a block type and returns its id. Ids are assigned from 1 in the order of "
                    .. "registration, so the same script gets the same ids on every machine. Registering a name "
                    .. "again returns its existing id and updates its colors. A world can have up to 4,095 block "
                    .. "types.\n\n"
                    .. "`color` is the color of the top face, and of every face if no other color is given. "
                    .. "`sideColor` is the color of the four sides, and `bottomColor` the color of the bottom, "
                    .. "which defaults to the side color. A grass block, for example, is green on top and brown "
                    .. "elsewhere.",
            },
            {
                Name = "SetBlockTextures",
                Parameters = {
                    { Name = "id", Type = "number" },
                    { Name = "texture", Type = "string" },
                    { Name = "sideTexture", Type = "string?" },
                    { Name = "bottomTexture", Type = "string?" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets the images of a block type, as content paths such as "
                    .. "`asset://textures/grass_top.png`, for its top, its four sides and its bottom. The sides "
                    .. "default to the top image and the bottom to the side image. The block's colors tint its "
                    .. "images, so register the type in white to show the images unchanged. An empty string "
                    .. "removes an image.\n\n"
                    .. "Each image fills one face of a block and repeats on every block. Images are sampled "
                    .. "without filtering, so pixel art stays sharp at any distance. The side image is upright on "
                    .. "every side and is never mirrored.",
            },
            {
                Name = "SetBlockOpacity",
                Parameters = {
                    { Name = "id", Type = "number" },
                    { Name = "opacity", Type = "Enum.BlockOpacity" },
                    { Name = "transparency", Type = "number?" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets how a block type lets what is behind it show through. `Cutout` blocks have holes "
                    .. "where their image is transparent. `Translucent` blocks blend with what is behind them; "
                    .. "`transparency` (0 to 1, default 0.5) sets how much shows through where the image does "
                    .. "not. See-through blocks still collide, and they do not darken the corners of the blocks "
                    .. "around them.",
            },
            {
                Name = "SetBlockFluid",
                Parameters = {
                    { Name = "id", Type = "number" },
                    { Name = "reach", Type = "number" },
                    { Name = "ticksPerStep", Type = "number?" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Makes a block type a fluid, such as water, lava or oil. With a `reach` of 0, it makes the "
                    .. "type an ordinary block again.\n\n"
                    .. "A fluid block placed with `SetBlock` or `FillBlocks` is a source and stays in place. From "
                    .. "it the fluid flows down first, filling every empty block below. Where it cannot fall, it "
                    .. "spreads sideways for `reach` blocks (1 to 7), one level shallower with each block. When "
                    .. "the source is removed, the fluid it fed drains away. The fluid moves once every "
                    .. "`ticksPerStep` simulation ticks (default 5); a larger number makes a slower fluid.\n\n"
                    .. "A fluid is drawn translucent whatever its opacity setting, as deep as it is full. It does "
                    .. "not collide and does not block `Raycast`, so a character can walk into a lake and a tool "
                    .. "can reach the lake bed. `GetBlock` returns the fluid's type at every level, and "
                    .. "`GetFluidDepth` returns how full a block is.",
            },
            {
                Name = "SetFluidReaction",
                Parameters = {
                    { Name = "from", Type = "number" },
                    { Name = "touching", Type = "number" },
                    { Name = "result", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets what the fluid `from` turns into where it touches the fluid `touching`. For example, "
                    .. "lava that touches water turns to stone, and the water stays. A block of `from`, including "
                    .. "a source, that touches `touching` on any of its six sides turns into `result` on the next "
                    .. "step.\n\n"
                    .. "Both types must be fluids. A `result` of 0 removes the reaction. To make the other fluid "
                    .. "change too, set a second reaction with the two types swapped.",
            },
            {
                Name = "GetFluidDepth",
                Parameters = {
                    { Name = "block", Type = "vector" },
                },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "Returns how full a block is with fluid, from 0 to 1. It is 1 where the fluid is falling or "
                    .. "has more of the same fluid above it, slightly below 1 at a source, and lower with each "
                    .. "block the fluid has spread. Returns 0 for a block that is not a fluid.",
            },
            {
                Name = "GetBlockId",
                Parameters = {
                    { Name = "name", Type = "string" },
                },
                Returns = { "number?" },
                ThreadSafety = "Safe",
                Doc = "Returns the id a block type was registered with, or nil if no type has that name.",
            },
            {
                Name = "SetBlock",
                Parameters = {
                    { Name = "block", Type = "vector" },
                    { Name = "id", Type = "number" },
                },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Places a block, or removes one when `id` is 0. `block` is a block coordinate, whose "
                    .. "components are rounded down. Returns whether anything changed.",
            },
            {
                Name = "GetBlock",
                Parameters = {
                    { Name = "block", Type = "vector" },
                },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "Returns the id of the block at a block coordinate, or 0 for air. A fluid block returns the "
                    .. "fluid's type at every level; use `GetFluidDepth` for how full it is.",
            },
            {
                Name = "FillBlocks",
                Parameters = {
                    { Name = "from", Type = "vector" },
                    { Name = "to", Type = "vector" },
                    { Name = "id", Type = "number" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Fills every block in the box between two block coordinates, both included, with one block "
                    .. "id; 0 empties the box. Returns the number of blocks that changed. Use it to build a "
                    .. "floor, a wall or the first ground of a world in one call. Raises an error for a box of "
                    .. "more than 256 blocks along an axis; fill a larger box in pieces.",
            },
            {
                Name = "Clear",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes every block. The registered block types are kept.",
            },
            {
                Name = "WorldToBlock",
                Parameters = {
                    { Name = "position", Type = "vector" },
                },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns the block coordinate that a world position falls in.",
            },
            {
                Name = "BlockToWorld",
                Parameters = {
                    { Name = "block", Type = "vector" },
                },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns the world position of the center of a block.",
            },
            {
                Name = "Raycast",
                Parameters = {
                    { Name = "origin", Type = "vector" },
                    { Name = "direction", Type = "vector" },
                },
                Returns = { "vector?", "vector?" },
                ThreadSafety = "Safe",
                Doc = "Casts a ray through the blocks and returns the first solid block it enters and the face it "
                    .. "entered through, as an outward normal. Returns nil and nil if it hits nothing within the "
                    .. "length of `direction`. The block is the one to break; the block plus the normal is where "
                    .. "to place a new block. Fluids do not stop the ray.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "NavigationService",
        Extends = "Instance",
        Module = "scene",
        Storage = "Navigation",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0089" },
        Doc = "Finds where agents can walk and computes paths. The walkable ground is every anchored part under "
            .. "`Workspace` that collides, plus the surface of the `Terrain`. A navigation mesh is built from it "
            .. "for an agent of a given size, set by the four `Agent` properties.\n\n"
            .. "The mesh is built on demand. A query builds the ground between its two ends in square tiles, and "
            .. "a tile is rebuilt when something in it moves, so a door that opens becomes walkable on the next "
            .. "tick. Ground that no query has reached is not built, and a path through it stops short. Use "
            .. "`BuildRegion` to build an area in advance.\n\n"
            .. "The service returns paths; following one is the game's job. For example, move a `CharacterBody` "
            .. "toward each waypoint in turn.",
        Properties = {
            {
                Name = "AgentRadius",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The radius of the default agent type, in meters. Paths stay this far from walls. To use "
                    .. "other sizes, define agent types with `DefineAgent`.",
            },
            {
                Name = "AgentHeight",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The height of the default agent type, in meters. A gap lower than this is not passable.",
            },
            {
                Name = "AgentMaxClimb",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The highest step the default agent type can walk up, in meters. A higher step is an obstacle.",
            },
            {
                Name = "AgentMaxSlope",
                Type = "number",
                Default = "45",
                ErrorKey = "scene.err.nav_slope",
                Doc = "The steepest ground the default agent type can walk on, in degrees from level.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "FindPath",
                Parameters = {
                    { Name = "from", Type = "vector" },
                    { Name = "to", Type = "vector" },
                    { Name = "agent", Type = "string?", Default = '""' },
                },
                Returns = { "{ vector }?", "boolean", "{ string }" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0098" },
                Doc = "Returns a path from `from` to `to`: a list of waypoints to walk between in straight lines, "
                    .. "and whether the path reaches the goal. Returns nil if `from` is not on walkable ground.\n\n"
                    .. "A path that stops short is still returned, with false as the second value. That happens "
                    .. "when the goal is off the walkable ground, is cut off, or lies beyond ground that has not "
                    .. "been built yet. Walk the path and query again from where it ends.\n\n"
                    .. "The third value is a list parallel to the waypoints: the `Label` of the `NavigationLink` "
                    .. "that starts at each waypoint, or an empty string where the next segment is walked. "
                    .. "`agent` is the name of a type defined with `DefineAgent`; an empty string uses the "
                    .. "default type.",
            },
            {
                Name = "DefineAgent",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "radius", Type = "number" },
                    { Name = "height", Type = "number" },
                    { Name = "maxClimb", Type = "number?", Default = "0.5" },
                    { Name = "maxSlope", Type = "number?", Default = "45" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0098" },
                Doc = "Defines a named agent type with its own size, and so its own walkable ground. For example, "
                    .. "a giant does not fit through a door that the default agent uses. Queries and "
                    .. "`NavigationAgent` select a type by name. Defining a name again replaces the type.",
            },
            {
                Name = "SetAreaCost",
                Parameters = {
                    { Name = "label", Type = "string" },
                    { Name = "cost", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets the cost of walking on ground that a `NavigationArea` marks with a label. A path "
                    .. "through such ground counts its length multiplied by `cost`. `math.huge` makes the ground "
                    .. "impassable. A label with no cost set costs 1. Changing a cost does not rebuild the mesh.",
            },
            {
                Name = "FindPath2D",
                Parameters = {
                    { Name = "from", Type = "Vector2" },
                    { Name = "to", Type = "Vector2" },
                },
                Returns = { "{ Vector2 }?", "boolean" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0098" },
                Doc = "Returns a path on the 2D plane: the points where the path turns, and whether it reaches "
                    .. "`to`. Returns nil if `from` is inside a wall.\n\n"
                    .. "The path runs over the cells of every `Tilemap2D`. A tile that is filled and collides is "
                    .. "a wall, and so is every anchored `Part2D` that collides. Diagonal steps never cut across "
                    .. "the corner of a wall.",
            },
            {
                Name = "NearestPoint",
                Parameters = {
                    { Name = "point", Type = "vector" },
                    { Name = "maxDistance", Type = "number?", Default = "4" },
                },
                Returns = { "vector?" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the walkable point nearest to a position, within `maxDistance` meters, or nil. Use "
                    .. "it to place a spawn point or a teleport destination on walkable ground, or to turn a "
                    .. "click on the world into a path goal.",
            },
            {
                Name = "Raycast",
                Parameters = {
                    { Name = "from", Type = "vector" },
                    { Name = "to", Type = "vector" },
                },
                Returns = { "vector?" },
                ThreadSafety = "Unsafe",
                Doc = "Walks in a straight line from `from` toward `to` over the walkable ground and returns "
                    .. "where it stops: at `to`, or where a wall or an edge blocks the way. Returns nil if `from` "
                    .. "is not on walkable ground. It is much cheaper than `FindPath`; try it first, since most "
                    .. "moves are straight.",
            },
            {
                Name = "BuildRegion",
                Parameters = {
                    { Name = "minimum", Type = "vector" },
                    { Name = "maximum", Type = "vector" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Builds the navigation mesh over a box immediately, so that the first path through it does "
                    .. "not wait for it. Returns the number of tiles it built. Ground that is already built and "
                    .. "unchanged costs nothing.",
            },
        } :: { Schema.Method },
    },
}

local Definition: Schema.Definition = {
    Classes = Classes,
}

return Definition
