--!strict
--- Services and the DataModel (api-design.md §2.1, §1.2).
---
--- The M2 subset. §2.1 lists the complete v1 set; the ones missing here are
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
                Doc = "The version of the engine runtime executing this VM, as `project.toml` pins it. Read-only: "
                    .. "the runtime is chosen before the VM exists, so nothing a script does can change it.",
            },
            {
                Name = "LuauVersion",
                Type = "string",
                ReadOnly = true,
                ThreadSafety = "Safe",
                -- A fact about the build, not about the world.
                HostFact = true,
                Doc = "The version of the Luau language runtime this engine build embeds. Read-only, and fixed per "
                    .. "engine release: a language upgrade is an engine release rather than a silent change "
                    .. "underneath a running project.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetService",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "Instance" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the service of this name. Services are singletons and all of them exist from boot, so "
                    .. "this is a lookup rather than a creation and every call returns the same ordinary child of "
                    .. "`game`. A string literal gives an exactly typed result; a name computed at runtime gives "
                    .. "the general `Instance`, which is the honest cost of not knowing the name at analysis time. "
                    .. "An unknown name raises `scene.err.unknown_service`.",
            },
            {
                Name = "FindService",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "Instance?" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns the service of this name, or `nil` for a name that is not a service at all. "
                    .. "\n\n**It answers the same as `GetService` for every real service now**, and that is a "
                    .. 'narrowing worth stating: it used to mean "is this service in use", because a service was '
                    .. "created by its first `GetService`. All of them exist from boot, so there is no such "
                    .. "question left to ask. What is left is the difference in how they refuse an unknown name -- "
                    .. "this one returns `nil` where `GetService` raises -- which is the reason to keep it.",
            },
            {
                Name = "BindToClose",
                Parameters = { { Name = "callback", Type = "() -> ()" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Registers a function to run while the game is shutting down, for work that has to happen "
                    .. "before the process ends. The callbacks run against a capped timeout and the shutdown "
                    .. "proceeds when it expires, finished or not: a close handler is a chance to finish, never a "
                    .. "veto. The editor's Stop is the game closing too.\n\n"
                    .. "**For the game, not a level** (ADR 0124): the open scene's `scene:BindToClose` handlers "
                    .. "run first, then these. One registered by a scene's script is dropped when that scene "
                    .. "closes, without running -- with a warning in the editor -- because it asked for a close "
                    .. "its scene did not live to see; `scene:BindToClose` is the one for a level. Every call adds "
                    .. "one, in order, each in its own thread; one that errors does not stop the others.",
            },
            {
                Name = "SendMessage",
                Parameters = { { Name = "topic", Type = "string" }, { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends a message to the game (ADR 0124): every function bound to `topic` on `game` runs, "
                    .. "deferred, in send order, each in its own thread. **Send to the mailbox of whom you want to "
                    .. 'reach** -- a level tells the game\'s HUD `game:SendMessage("CoinCollected", 1)` -- and '
                    .. "listen on your own. The values are `Scene:SendMessage`'s: copied, never a function or a "
                    .. "live object. Local to this machine.",
            },
            {
                Name = "BindToMessage",
                Parameters = {
                    { Name = "topic", Type = "string" },
                    { Name = "callback", Type = "(...any) -> ()" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Runs `callback` with a message's values each time one is sent to `game` under `topic`. It "
                    .. "belongs to the script that bound it: a scene script's binding goes with its scene.",
            },
            {
                Name = "Shutdown",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Ends the run. The registered `BindToClose` callbacks are given their timeout and the process "
                    .. "then exits. This is how a game closes itself deliberately.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Loaded",
                Parameters = {},
                Doc = "Fires once, after every entry script has had its first resumption. Connecting at file scope "
                    .. "inside an entry script still works: the fire is deferred like every other, so a handler "
                    .. "registered while the world is still being built is registered in time. It carries no "
                    .. "arguments.",
            },
        } :: { Schema.Event },
        Doc = "The root of the instance tree, reached through the `game` global. **Every service is a child of it and "
            .. "every service exists from boot**, in the order this file declares them -- so the tree has the same "
            .. "shape before a line of script has run as it does after, which is what lets an editor show it. It is "
            .. "the services' parent rather than one of them.",
    },
    {
        Name = "Workspace",
        Extends = "Instance",
        Module = "scene",
        Storage = "Workspace",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        -- `CurrentCamera` arrived with the renderer at M4 and `Gravity` with
        -- the simulation at M5, as the note here always said they would.
        Doc = "The root of the 3D scene, reached through the `workspace` global as well as through "
            .. "`game:GetService`. Whatever is parented under it is in the world and whatever is not, is not.",
        Properties = {
            {
                Name = "Gravity",
                Type = "vector",
                Default = "vector.create(0, -9.81, 0)",
                Doc = "The acceleration applied to every unanchored part, in metres per second squared. SI and "
                    .. "signed, so the default points down; a game wanting moon gravity scales it rather than "
                    .. "reaching for a separate multiplier.",
            },
            {
                Name = "GlobalWind",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Presentation = true,
                Doc = "The wind's direction and speed, in metres per second. **Visual only**: it moves what is "
                    .. "drawn -- particles that drift with it, surface shaders that read it, foliage -- and pushes "
                    .. "no body. Zero, the default, is a still world. `GetWindAt` is the wind at a point, gusts "
                    .. "and all.",
            },
            {
                Name = "WindGusts",
                Type = "number",
                Default = "0",
                Presentation = true,
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How far the wind's speed rises and falls in gusts, from 0 (steady) to 1. Gusts travel "
                    .. "downwind at the wind's own speed, so one is seen crossing a field.",
            },
            {
                Name = "WindTurbulence",
                Type = "number",
                Default = "0",
                Presentation = true,
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How far the wind's direction wanders from place to place, from 0 (straight) to 1.",
            },
            {
                Name = "CurrentCamera",
                Type = "Camera?",
                Doc = "The camera the world is rendered from. Nil renders nothing rather than falling back to a "
                    .. "camera the engine invented, because a view nobody asked for is harder to debug than a black "
                    .. "frame that says why.",
            },
            {
                Name = "Terrain",
                Type = "Terrain?",
                ReadOnly = true,
                Doc = "This world's ground, or nil in a world that has none.\n\n"
                    .. "Read-only because it names rather than owns: a `Terrain` is an ordinary instance "
                    .. "parented under the workspace, and this is how you reach the one the engine treats "
                    .. "as the world's ground. Parent a second one and it is a perfectly good terrain that "
                    .. "this does not point at.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetWindAt",
                Parameters = { { Name = "position", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "The wind at a point now, in metres per second: `GlobalWind` with its gusts and turbulence, "
                    .. "at `RunService.SimTime`. It is the same function the renderer draws with, so a flag, a "
                    .. "sail or a sound put where this says the wind is agrees with the picture.",
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
                Doc = "The nearest thing a ray from `origin` hits, or nil. The direction is NOT normalised: its "
                    .. "length is how far the ray reaches, so `direction * 100` is a hundred-metre ray. A tie "
                    .. "between two surfaces at the same distance resolves the same way on every run, because a "
                    .. "query whose answer depends on traversal order is a replay divergence waiting for a body "
                    .. "count to change (R10).",
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
                Doc = "The nearest `Part2D` or `Tilemap2D` a ray on the 2D plane hits, or nil. As with `Raycast`, "
                    .. "the direction's length is the reach. Sensors are passed through, and so is a part with "
                    .. "`CanCollide` off. Two hits at the same distance resolve the same way on every run (R10).",
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
                Doc = "The same question with a sphere swept along the ray instead of a point. This is what a "
                    .. "camera boom or a character's ground check wants: a ray slips through a gap narrower than "
                    .. "the thing it is standing in for.",
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
                Doc = "Every part overlapping an oriented box, as a fresh array in a stable order -- one entry "
                    .. "per part however many of its surfaces are inside. `size` is the full extent, matching "
                    .. "`BasePart.Size`.",
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
                Doc = "Every part overlapping a ball, as `GetBodiesInBox` answers for a box: what an explosion "
                    .. "reaches, what is within arm's length.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "Lighting",
        Extends = "Instance",
        Module = "render",
        Storage = "Lighting",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Day/night and the environment every surface is lit against. The sun is service state rather than an "
            .. "instance, because there is exactly one of it and its direction is derived rather than authored.",
        Properties = {
            {
                Name = "ClockTime",
                Type = "number",
                Doc = "The hour of day, 0 to 24, wrapping rather than clamping so that adding a delta every tick "
                    .. "never has to check. It is the ONLY input to the sun's direction, which is what makes a "
                    .. "replay light the same way the live run did.",
            },
            {
                Name = "GeographicLatitude",
                Type = "number",
                ErrorKey = "scene.err.number_latitude_degrees",
                Doc = "Degrees north of the equator, which tilts the sun's arc across the sky. The second and last "
                    .. "input to SunDirection.",
            },
            {
                Name = "Ambient",
                Type = "Color3",
                Doc = "The flat light in ENCLOSED spaces -- a cave, a tunnel, under an overhang: surfaces that see "
                    .. "none of the sky. `OutdoorAmbient` is the light of those that see all of it, and a surface "
                    .. "that sees some takes a blend of the two by how much. A stand-in for bounced light, so a "
                    .. "cave is dim rather than black; set it darker for a darker cave. Not clamped, so it can be "
                    .. "pushed above one deliberately.",
            },
            {
                Name = "OutdoorAmbient",
                Type = "Color3",
                Doc = "The flat light on surfaces that see the open sky. What `Ambient` is for enclosed spaces; "
                    .. "the two start equal, so a world that sets neither is lit the same inside and out, and one "
                    .. "that wants dark caves darkens `Ambient` alone. A part is outdoors unless the terrain says "
                    .. "otherwise.",
            },
            {
                Name = "Brightness",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How strong the sun is, independent of its colour.",
            },
            {
                Name = "FogColor",
                Type = "Color3",
                Doc = "What distance fades towards between FogStart and FogEnd. **Kept and not used while an `Atmosphere` "
                    .. "is directly under `Lighting`** -- the air replaces the linear fog -- and it applies again when "
                    .. "the `Atmosphere` goes.",
            },
            {
                Name = "FogStart",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Distance in metres at which fog begins. Kept and not used while an `Atmosphere` is directly "
                    .. "under `Lighting`.",
            },
            {
                Name = "FogEnd",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Distance in metres at which fog is total. Equal to or below FogStart means no fog at all, "
                    .. "which is how fog is turned off without a separate flag.",
            },
            {
                Name = "ExposureCompensation",
                Type = "number",
                ErrorKey = "scene.err.number_exposure_stops",
                Doc = "Exposure in EV stops, on top of the automatic exposure the renderer measures from the frame "
                    .. "itself. Zero means whatever it measured; +1 is twice the light and -1 is half, which is the "
                    .. "unit a camera uses. It is a look rather than a limit, so it is not clamped.",
            },
            {
                Name = "ExposureMin",
                Type = "number",
                Default = "-2.7369655",
                ErrorKey = "scene.err.number_exposure_stops",
                Doc = "The most the automatic exposure may DARKEN the picture, in EV stops: how far it follows "
                    .. "a world that gets brighter. About two and three quarter stops down by default. Raise "
                    .. "it towards 0 and a bright place stays bright -- snow glares, a desert at noon is "
                    .. "hard to look at.",
            },
            {
                Name = "ExposureMax",
                Type = "number",
                Default = "1.5849625",
                ErrorKey = "scene.err.number_exposure_stops",
                Doc = "The most the automatic exposure may BRIGHTEN the picture, in EV stops: how far it "
                    .. "follows a world that gets darker. About a stop and a half by default, so a night is "
                    .. "darker than a day and still seen. **Lower it and night is dark**: at 0 the exposure "
                    .. "never brightens at all, and a world lit a third as bright LOOKS a third as bright -- "
                    .. "what a game with a day and a night that should feel like one sets. Set both to the "
                    .. "same number and the exposure is fixed there.",
            },
            {
                Name = "EnvironmentDiffuseScale",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much the sky lights the world's matte surfaces, from 0 to 1: 1 is all of its light, "
                    .. "0 none -- a surface is then lit by the sun, its lamps and `Ambient` alone. The sky "
                    .. "itself is drawn as it is either way.",
            },
            {
                Name = "EnvironmentSpecularScale",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much the sky is reflected in the world's shiny surfaces, from 0 to 1. 0 leaves metal "
                    .. "reflecting nothing but the sun's highlight, which is what an interior with the sky "
                    .. "shut out should look like.",
            },
            {
                Name = "ShadowSoftness",
                Type = "number",
                Default = "0.2",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How soft the edge of a sun shadow is, from 0 (as hard as the shadow map allows) to 1 "
                    .. "(a quarter of a metre of penumbra either side of the edge). The default is the edge the "
                    .. "engine drew before this property existed.",
            },
            {
                Name = "GlobalShadows",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the sun -- or the moon, at night -- casts shadows. Off, nothing shades anything "
                    .. "from it: the look of an overcast day, or of a stylised world with no shadows at all. "
                    .. "Lamps that cast shadows still do.",
            },
            {
                Name = "AutoExposure",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the picture's brightness follows what the camera sees, as an eye adapting from a "
                    .. "cave to daylight does. Off holds the exposure fixed, so a world lit darker LOOKS darker; "
                    .. "`ExposureCompensation` still applies either way. A machine whose graphics settings turn "
                    .. "automatic exposure off has it off whatever this says.",
            },
            {
                Name = "SunDirection",
                Type = "vector",
                ReadOnly = true,
                ThreadSafety = "ReadParallel",
                Doc = "The unit vector pointing from the world towards the sun, derived from ClockTime and "
                    .. "GeographicLatitude and nothing else -- no wall clock and no accumulated state, so the same "
                    .. "ClockTime always gives the same direction.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "ReplicatedStorage",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "What every machine has and nobody sees (ADR 0080): templates to clone, `RemoteEvent`s, "
            .. "anything a game keeps rather than shows. It is not the world -- nothing under it is drawn, "
            .. "collides or moves -- and it is saved with the scene, so what the editor puts here is here "
            .. "when the game starts.\n\n"
            .. "**Its contents reach every replica, whatever their distance.** A replica's copy is the "
            .. "authority's: what the scene put here on the replica is replaced by what the authority "
            .. 'sends, as `Workspace` is. Reach it with `game:GetService("ReplicatedStorage")`, and clone '
            .. "what you want in the world into `Workspace`.",
    },
    {
        Name = "SaveService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Where a game keeps what has to outlive a run (ADR 0111): progress, settings, a best time. "
            .. "It keeps named SLOTS, each a file on the player's own machine, under the folder the game's "
            .. "`[project]` name and company say -- `%APPDATA%\\<company>\\<name>\\saves` on Windows. That is "
            .. "the exported game's. Every run of the project itself -- the editor's Play, `ludwerk dev`, the "
            .. "host given the project's folder -- saves to `.engine/saves/` in the project instead, so a "
            .. "test run never touches a real player's.\n\n"
            .. "**The only way from a script to a disk**, and it names slots, never paths. It is not "
            .. "replicated: every machine keeps its own. What a slot held is a fact about this machine, not "
            .. "about the simulation, so a replay that loads one records the load as an input.",
        Properties = {
            {
                Name = "Version",
                Type = "number",
                Default = "1",
                Transient = true,
                HostFact = true,
                ErrorKey = "script.err.save_version",
                Doc = "The version of this game's save layout, a whole number from 1. Set it before the first "
                    .. "`GetSlotAsync`; a slot written by an older version is handed to `OnMigrate` first, "
                    .. "once, and saved at this version.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetSlotAsync",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "SaveSlot" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "The slot called `name`, read from disk -- yields until it is. A name is 1 to 64 "
                    .. "letters, digits, `_` and `-`; anything else is an error, because a slot is a file and "
                    .. "a name is never a path. A slot that does not exist yet is an empty one. One whose file "
                    .. "is damaged is read from the backup the previous save left, with a warning; when both "
                    .. "are, it is empty and `Recovered` is false.",
            },
            {
                Name = "ListSlots",
                Parameters = {},
                Returns = { "{ string }" },
                ThreadSafety = "Unsafe",
                Doc = "The names of the slots this game has saved, sorted.",
            },
            {
                Name = "DeleteSlot",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes a slot and its backup. A `SaveSlot` already handed out is emptied.",
            },
        } :: { Schema.Method },
        Callbacks = {
            {
                Name = "OnMigrate",
                Parameters = { { Name = "slot", Type = "SaveSlot" }, { Name = "fromVersion", Type = "number" } },
                Returns = {},
                Doc = "Brings a slot saved by an older `Version` up to this one, by editing it in place. Runs "
                    .. "once, before `GetSlotAsync` returns that slot, in a thread of its own; the slot is then "
                    .. "saved at the current version. An error in it is logged and leaves the slot as it was "
                    .. "read.",
            },
        } :: { Schema.Callback },
    },
    {
        Name = "GraphicsService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "The machine's graphics and display settings (ADR 0147): how good the picture is, how the window "
            .. "sits on the display and how fast frames are made. What an options menu is built on.\n\n"
            .. "**A setting's value is the first of these that says it**: a flag the game was started with, "
            .. "what a script wrote this session, the player's saved choice, the project's `project.toml`, "
            .. "the quality level in force, and the engine's own default. `GetSource` says which. A write "
            .. "takes effect at the next frame, and ten writes in one frame are applied once.\n\n"
            .. "**Nothing here is the world's.** The settings describe the machine the world is shown on: "
            .. "they are not saved with a scene, do not travel to other players, and the simulation is the "
            .. "same at every one of them. `SaveAsync` keeps them for this player, in their own folder; a "
            .. "game starts in what they chose. A dedicated server has no display: it reads the defaults, "
            .. "and a write there raises.",
        Properties = {
            {
                Name = "QualityLevel",
                Type = "Enum.GraphicsQuality",
                Default = "Enum.GraphicsQuality.High",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The quality level. Writing one is `ApplyPreset`: every quality setting becomes that "
                    .. "level's. It reads `Custom` when any of them is not the level's -- a script, the player or "
                    .. "the project said otherwise -- and `Custom` cannot be written.",
            },
            {
                Name = "RenderScale",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The fraction of the window's resolution the world is drawn at, 0.5 to 1. The interface is "
                    .. "drawn at the window's own, whatever this is.",
            },
            {
                Name = "ShadowQuality",
                Type = "Enum.ShadowQuality",
                Default = "Enum.ShadowQuality.High",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The sun's shadow, as a level. Writing one writes `ShadowResolution` and `ShadowCascades` "
                    .. "as that level has them; `Off` is no cascades.",
            },
            {
                Name = "ShadowResolution",
                Type = "number",
                Default = "2048",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "One shadow cascade's side in texels, 256 to 2048, rounded down to a power of two.",
            },
            {
                Name = "ShadowCascades",
                Type = "number",
                Default = "4",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How many cascades the sun casts into, 0 to 4. Zero is no sun shadow.",
            },
            {
                Name = "ShadowDistance",
                Type = "number",
                Default = "120",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How far from the camera the sun casts, in metres, 10 to 1000.",
            },
            {
                Name = "AntiAliasing",
                Type = "Enum.AntiAliasingMode",
                Default = "Enum.AntiAliasingMode.FXAA",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How edges are smoothed.",
            },
            {
                Name = "AmbientOcclusion",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether creases and corners are darkened.",
            },
            {
                Name = "ContactShadows",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether the sun's shadow is drawn right up to what casts it.",
            },
            {
                Name = "Bloom",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether bright things glow.",
            },
            {
                Name = "DepthOfField",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether a world's `DepthOfFieldEffect` is drawn. The machine wins over the world: off, a "
                    .. "scene that asks for focus is sharp.",
            },
            {
                Name = "SunRays",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether a world's `SunRaysEffect` is drawn, by the same rule.",
            },
            {
                Name = "AutoExposure",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether the picture's brightness follows the scene. Off, the exposure is held.",
            },
            {
                Name = "LightBudget",
                Type = "number",
                Default = "256",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How many lights one frame may carry.",
            },
            {
                Name = "TerrainDetail",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How fine distant ground is, as a scale on the level's own: 2 refines it at twice the "
                    .. "distance, 0.5 at half.",
            },
            {
                Name = "FoliageDensity",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How much of the foliage a `FoliageLayer` grows is drawn, 0 to 1, over the project's own "
                    .. "density.",
            },
            {
                Name = "ViewDistance",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "A scale on how far things are drawn, 0.25 to 4. **Kept, saved and reported, and not drawn "
                    .. "by yet**: it applies when that part of the renderer lands.",
            },
            {
                Name = "TextureQuality",
                Type = "Enum.TextureQuality",
                Default = "Enum.TextureQuality.High",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How much of each texture is kept. **Kept, saved and reported, and not drawn by yet**: it "
                    .. "applies when that part of the renderer lands.",
            },
            {
                Name = "AnisotropicFiltering",
                Type = "number",
                Default = "8",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How sharp a texture stays at a grazing angle, 1 to 16. **Kept, saved and reported, and not "
                    .. "drawn by yet**: it applies when that part of the renderer lands.",
            },
            {
                Name = "LODBias",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "A scale on the distance a mesh's levels of detail change at. **Kept, saved and reported, "
                    .. "and not drawn by yet**: it applies when that part of the renderer lands.",
            },
            {
                Name = "MaximumLODLevel",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The finest level of detail a mesh may be drawn at; 0 is its finest. **Kept, saved and "
                    .. "reported, and not drawn by yet**: it applies when that part of the renderer lands.",
            },
            {
                Name = "ParticleBudget",
                Type = "number",
                Default = "65536",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The most particles alive at once. **Kept, saved and reported, and not drawn by yet**: it "
                    .. "applies when that part of the renderer lands.",
            },
            {
                Name = "SoftParticles",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether particles fade where they meet a surface. **Kept, saved and reported, and not "
                    .. "drawn by yet**: it applies when that part of the renderer lands.",
            },
            {
                Name = "SkinWeights",
                Type = "number",
                Default = "4",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How many bones may move one vertex, 1 to 4. **Kept, saved and reported, and not drawn by "
                    .. "yet**: it applies when that part of the renderer lands.",
            },
            {
                Name = "TextureStreamingBudget",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The memory textures may keep resident, in mebibytes; 0 is no limit. **Kept, saved and "
                    .. "reported, and not drawn by yet**: it applies when that part of the renderer lands.",
            },
            {
                Name = "AsyncUploadBudget",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The milliseconds a frame may spend sending data to the GPU. **Kept, saved and reported, "
                    .. "and not drawn by yet**: it applies when that part of the renderer lands.",
            },
            {
                Name = "MotionBlur",
                Type = "boolean",
                Default = "false",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether motion is blurred. **Kept, saved and reported, and not drawn by yet**: it applies "
                    .. "when that part of the renderer lands.",
            },
            {
                Name = "FogQuality",
                Type = "Enum.GraphicsLevel",
                Default = "Enum.GraphicsLevel.High",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How fog is drawn. **Kept, saved and reported, and not drawn by yet**: it applies when that "
                    .. "part of the renderer lands.",
            },
            {
                Name = "GlobalIllumination",
                Type = "Enum.GraphicsLevel",
                Default = "Enum.GraphicsLevel.High",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Indirect light. **Kept, saved and reported, and not drawn by yet**: it applies when that "
                    .. "part of the renderer lands.",
            },
            {
                Name = "Reflections",
                Type = "Enum.GraphicsLevel",
                Default = "Enum.GraphicsLevel.High",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Reflections. **Kept, saved and reported, and not drawn by yet**: it applies when that part "
                    .. "of the renderer lands.",
            },
            {
                Name = "RenderResolutionCap",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The most pixels the world's shorter side is drawn at; 0 is no limit. A handheld's levels "
                    .. "set it: a phone's display has more pixels than an eye resolves or a battery pays for.",
            },
            {
                Name = "WindowMode",
                Type = "Enum.WindowMode",
                Default = "Enum.WindowMode.Windowed",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "How the window sits on the display. A handheld has the whole display whatever this says.",
            },
            {
                Name = "Resolution",
                Type = "Vector2",
                Default = "Vector2.zero",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The window's size in pixels when it is windowed, and the display mode when it is "
                    .. "fullscreen. Zero is \"the project's window size\", or the display's own when fullscreen.",
            },
            {
                Name = "Monitor",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Which display the window is on, as an index into `GetMonitors()`.",
            },
            {
                Name = "VSync",
                Type = "boolean",
                Default = "true",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "Whether a frame waits for the display. On by default; a handheld is always on.",
            },
            {
                Name = "MaxFrameRate",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The most frames a second; 0 is no cap. With `VSync` off it is the only limit, and with it "
                    .. "on it caps below the display's rate.",
            },
            {
                Name = "BackgroundFrameRate",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "The cap while the window is not in front or is minimised; 0 is no throttle.",
            },
            {
                Name = "Brightness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.graphics_setting",
                Transient = true,
                HostFact = true,
                Doc = "An offset on the picture's gamma, -1 to 1. **Kept, saved and reported, and not drawn by "
                    .. "yet**: it applies when that part of the renderer lands.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "ApplyPreset",
                Parameters = { { Name = "level", Type = "Enum.GraphicsQuality" } },
                Returns = {},
                Doc = "Makes every quality setting what `level` has, whatever a script, the player or the "
                    .. "project said before. `Auto` is the level this machine was measured for; `Custom` is "
                    .. "refused. The display settings -- the window, `VSync`, the frame caps -- are not part "
                    .. "of a level and stay.",
            },
            {
                Name = "ResetToDefaults",
                Parameters = {},
                Returns = {},
                Doc = "Forgets what a script wrote and what the player saved, so the project's defaults "
                    .. "stand. The player's file is not touched until `SaveAsync`.",
            },
            {
                Name = "GetSource",
                Parameters = { { Name = "setting", Type = "string" } },
                Returns = { "Enum.SettingSource" },
                Doc = "Who said the value of the setting named `setting` -- a property's name, as "
                    .. '`"VSync"`. A name that is no setting raises.',
            },
            {
                Name = "IsApplied",
                Parameters = { { Name = "setting", Type = "string" } },
                Returns = { "boolean" },
                Doc = "Whether anything draws by the setting named `setting` yet. A few are kept, saved and "
                    .. "reported ahead of the renderer that reads them, so a menu does not change shape; a "
                    .. "menu can leave those out, or grey them, by asking.",
            },
            {
                Name = "GetGroupLevel",
                Parameters = { { Name = "group", Type = "Enum.GraphicsGroup" } },
                Returns = { "Enum.GraphicsLevel?" },
                Doc = "The level a group is at: the one whose values every setting of the group has. Nil "
                    .. "when they are no one level's -- somebody set one by itself.",
            },
            {
                Name = "SetGroupLevel",
                Parameters = {
                    { Name = "group", Type = "Enum.GraphicsGroup" },
                    { Name = "level", Type = "Enum.GraphicsLevel" },
                },
                Returns = {},
                Doc = "Sets every setting of `group` to what `level` has.",
            },
            {
                Name = "GetSupportedResolutions",
                Parameters = {},
                Returns = { "{ Vector2 }" },
                Doc = "The fullscreen sizes the window's display offers, largest first. Empty where there is "
                    .. "no display to ask.",
            },
            {
                Name = "GetMonitors",
                Parameters = {},
                Returns = { "{ { Name: string, Size: Vector2, RefreshRate: number } }" },
                Doc = "The displays, in the order `Monitor` counts them.",
            },
            {
                Name = "GetRefreshRate",
                Parameters = {},
                Returns = { "number" },
                Doc = "The rate of the display the window is on, in hertz; 0 when it will not say.",
            },
            {
                Name = "SaveAsync",
                Parameters = {},
                Returns = { "boolean" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Keeps the player's choices -- what was saved before, under what a script has written "
                    .. "since -- in their own folder, as `settings.json`. Yields until the file is written, "
                    .. "at the end of the frame, and returns whether it was; from then on `GetSource` says "
                    .. "`Player` for them. False where there is nowhere to write: a game that turned "
                    .. "`[display] remember_player_settings` off, or a run with no folder of its own.",
            },
            {
                Name = "LoadAsync",
                Parameters = {},
                Returns = { "boolean" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Reads the player's saved choices again, and forgets what a script wrote since: what "
                    .. "a menu's Revert does. Yields until it is done, at the end of the frame, and returns "
                    .. "whether there was a file to read.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "QualityChanged",
                Parameters = {},
                Doc = "The quality level or any quality setting changed, whoever changed it. Once a frame at "
                    .. "most.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "HapticService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "What a player feels in their hands (ADR 0131): a gamepad's motors, and a phone's own vibration.\n\n"
            .. "**A level, not a pulse.** `SetMotor` leaves a motor running at what it was told until it is "
            .. "told something else -- zero stops it -- so an engine that rumbles while it runs is one call "
            .. "when it starts and one when it stops. `Vibrate` is the other shape: a phone shakes for a "
            .. "moment and stops by itself.\n\n"
            .. "**It stops when nobody is there to feel it**: every motor is still while the game's window "
            .. "is not the one in front, picks up where it was when it is again, and stops for good when "
            .. "the game closes. Nothing here is the simulation's: a dedicated server and a headless run "
            .. "have no hands, answer false to every question, and ignore every call.",
        Methods = {
            {
                Name = "IsVibrationSupported",
                Parameters = { { Name = "inputType", Type = "Enum.UserInputType" } },
                Returns = { "boolean" },
                Doc = "Whether that kind of device can vibrate at all, on this machine now: "
                    .. "`Enum.UserInputType.Gamepad` for a connected gamepad with motors, "
                    .. "`Enum.UserInputType.Touch` for the device itself -- a phone. False for anything else.",
            },
            {
                Name = "IsMotorSupported",
                Parameters = {
                    { Name = "inputType", Type = "Enum.UserInputType" },
                    { Name = "motor", Type = "Enum.VibrationMotor" },
                },
                Returns = { "boolean" },
                Doc = "Whether that one motor is there. Most gamepads have `Large` and `Small`; fewer have "
                    .. "motors in their triggers; a phone has none of these -- it has `Vibrate`.",
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
                Doc = "Runs one motor of every connected gamepad at `value`, 0 (still) to 1 (full), until it "
                    .. "is set again. A motor that is not there is ignored, so a game need not ask first.",
            },
            {
                Name = "Vibrate",
                Parameters = {
                    { Name = "strength", Type = "number" },
                    { Name = "seconds", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Shakes the device itself -- a phone -- at `strength`, 0 to 1, for `seconds`, up to "
                    .. "five. A machine with no vibrator of its own ignores it.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "CryptoService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "What a game keeps a secret with, and tells one player from another by (ADR 0151): chance that "
            .. "nobody can predict, two hashes, and a hash meant for passwords.\n\n"
            .. "**Not the simulation's chance.** `Random.new()` and `math.random` give the same numbers on "
            .. "every run, on purpose: a replay and a second machine must agree. A session token, a salt and "
            .. "an identity key must be the opposite, and come from here -- the operating system's own "
            .. "generator. What this service answers is a fact about this machine at this moment: a replay "
            .. "does not reproduce it and a client does not predict it, so use it where a server decides, or "
            .. "outside the simulation.\n\n"
            .. "**It does not hide what a `RemoteEvent` carries.** A password sent from a client travels as "
            .. "the connection carries it; hash what you STORE with `HashPasswordAsync`, and see the "
            .. "multiplayer guide for what the connection itself protects.",
        Methods = {
            {
                Name = "RandomBytes",
                Parameters = { { Name = "count", Type = "number" } },
                Returns = { "buffer" },
                ThreadSafety = "Safe",
                Doc = "A buffer of `count` bytes from the operating system's generator -- a key, a salt, a "
                    .. "token. A whole number from 0 to 1048576.",
            },
            {
                Name = "RandomInteger",
                Parameters = { { Name = "min", Type = "number" }, { Name = "max", Type = "number" } },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "A whole number from `min` to `max`, both included, every one as likely as the next, "
                    .. "that nobody can predict: a room code, a seed for a `Random` that must differ each "
                    .. "run. Both are whole numbers and `min` is no more than `max`.",
            },
            {
                Name = "UniqueId",
                Parameters = {},
                Returns = { "string" },
                ThreadSafety = "Safe",
                Doc = "A new identifier no other call will ever return, here or on any machine: a version-4 "
                    .. "UUID in lower case, `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`. 122 random bits, so it "
                    .. "serves as a session token as well as a name.",
            },
            {
                Name = "Sha256",
                Parameters = { { Name = "data", Type = "string | buffer" } },
                Returns = { "string" },
                ThreadSafety = "Safe",
                Doc = "The SHA-256 of `data`'s bytes, as 64 lower-case hexadecimal characters. A fingerprint: "
                    .. "the same bytes give the same answer everywhere. **Not for a password** -- it is fast, "
                    .. "and a fast hash of a password is guessed at billions of tries a second; that is what "
                    .. "`HashPasswordAsync` is for.",
            },
            {
                Name = "HmacSha256",
                Parameters = {
                    { Name = "key", Type = "string | buffer" },
                    { Name = "data", Type = "string | buffer" },
                },
                Returns = { "string" },
                ThreadSafety = "Safe",
                Doc = "The HMAC-SHA-256 of `data` under `key`, as 64 lower-case hexadecimal characters: a "
                    .. "signature only a holder of the key can make or check. What a server signs a token "
                    .. "with, and what most web services sign a request with. Compare one with `SecureEquals`.",
            },
            {
                Name = "SecureEquals",
                Parameters = {
                    { Name = "a", Type = "string | buffer" },
                    { Name = "b", Type = "string | buffer" },
                },
                Returns = { "boolean" },
                ThreadSafety = "Safe",
                Doc = "Whether `a` and `b` are the same bytes, taking the same time whether they differ at "
                    .. "the first byte or the last. What a token or a signature from outside is compared "
                    .. "with: `==` stops at the first difference, and how long it took tells a guesser how "
                    .. "much of the guess was right.",
            },
            {
                Name = "HashPasswordAsync",
                Parameters = { { Name = "password", Type = "string" } },
                Returns = { "string" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "A hash of `password` to store in its place -- yields while it is made, on a thread of "
                    .. "its own, so a server's tick does not wait for it. Argon2id, salted with bytes of its "
                    .. "own choosing and slow on purpose (tens of milliseconds, 64 MiB), so a stolen table "
                    .. "cannot be guessed quickly. The same password hashed twice gives two different "
                    .. "strings; check one with `VerifyPasswordAsync`, never with `==`.\n\n"
                    .. "The answer is a line of text that carries its own salt and cost "
                    .. "(`$argon2id$v=19$m=65536,t=2,p=1$...`, under 128 characters): store it whole. A "
                    .. "password is at most 1024 bytes. Requests are hashed one at a time, in the order they "
                    .. "came.",
            },
            {
                Name = "VerifyPasswordAsync",
                Parameters = { { Name = "password", Type = "string" }, { Name = "hash", Type = "string" } },
                Returns = { "boolean" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Whether `password` is the one `hash` was made from by `HashPasswordAsync` -- yields "
                    .. "while it is checked, as that does. False for a `hash` that is not one of its.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "TeamService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Where the sides of a game live (ADR 0099): every `Team` under it reaches every replica, "
            .. "whatever their distance, as `ReplicatedStorage`'s contents do. It is saved with the scene.",
        Methods = {
            {
                Name = "GetTeams",
                Parameters = {},
                Returns = { "{ Team }" },
                ThreadSafety = "ReadParallel",
                Doc = "Every team under this service, in child order.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "Team",
        Extends = "Instance",
        Module = "scene",
        Storage = "Team",
        Parents = { "TeamService", "ReplicatedStorage", "ServerStorage" },
        Doc = "A side (ADR 0099): a name -- its `Name` -- a colour, and the players whose `Player.Team` is "
            .. "it. It counts only under `TeamService`, which is what carries it to every replica.",
        Properties = {
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "The side's colour, for a game to paint name tags, scoreboards and shirts with.",
            },
            {
                Name = "AutoAssign",
                Type = "boolean",
                Default = "true",
                Doc = "Whether a player who joins may be put on this team. The engine picks the "
                    .. "`AutoAssign` team with the fewest players.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetPlayers",
                Parameters = {},
                Returns = { "{ Player }" },
                ThreadSafety = "ReadParallel",
                Doc = "Every player on this team, in the order they joined.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "ContentProvider",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Loads assets before anything shows them (ADR 0131): a level's meshes and pictures read and "
            .. "on the GPU while a loading screen is up, so the first frame of play has nothing popping in. "
            .. "The same path a scene prepared with `SceneService:LoadSceneAsync` warms by.",
        Properties = {
            {
                Name = "RequestQueueSize",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                Scripted = true,
                Doc = "How many of the assets `PreloadAsync` was asked for are still on their way.",
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
                Doc = "Loads every item and yields until each has arrived or failed. An item is a content "
                    .. "name, such as `asset://models/tree.gltf`, or an instance -- every asset it and its "
                    .. "descendants name. `callback` runs once per item as it finishes, with "
                    .. "`Enum.AssetFetchStatus.Success` or `Failure`. Meshes and pictures are loaded onto the "
                    .. "GPU; a sound is opened and its length read; anything else is found or not.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "ProximityPromptService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Every `ProximityPrompt` at once (ADR 0126): whether any show, how many at most, and a "
            .. "signal for any of them triggered.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "False hides every prompt and triggers none -- a cutscene, a menu.",
            },
            {
                Name = "MaxPromptsVisible",
                Type = "number",
                Default = "16",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The most prompts shown at once, nearest first.",
            },
        } :: { Schema.Property },
        Events = {
            {
                Name = "PromptTriggered",
                Parameters = { { Name = "prompt", Type = "ProximityPrompt" }, { Name = "player", Type = "Player" } },
                Doc = "Any prompt was triggered, with the player who triggered it.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "SceneService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Which scene the game is in, and the way to go to another (ADR 0106). A scene is a complete "
            .. "place -- its world, its lighting, its UI, its storages and its own script services -- and "
            .. "`LoadScene` swaps the whole of it for another, between two ticks. **`GlobalScriptService` "
            .. "stays**, with its scripts, their variables and their connections: that is what it is for.\n\n"
            .. "In a match the scene is the authority's: a host or a server changes it, and every client "
            .. "follows without dropping the connection.",
        Properties = {
            {
                Name = "CurrentScene",
                Type = "Scene",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                Scripted = true,
                Doc = "The scene open now (ADR 0124), as an object that stays with it: its `Path` is the "
                    .. "content-relative path of its file, such as `scenes/arena.scene.json` (empty for a world "
                    .. "no scene file made), and once the scene closes `IsOpen()` says so. The global `scene` is "
                    .. "the same scene while it is open.",
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
                Doc = "Unloads this scene and loads `path` (content-relative). **Returns at once**: the "
                    .. "change happens at the next safe point between ticks, after `SceneLoading` has run. "
                    .. "The old scene goes whole -- its world, its services' contents and settings, and its "
                    .. "scripts, whose threads stop -- and the new one opens from the engine's settings, "
                    .. "reads its file, mounts its own `src/scenes/<scene>/` code and starts its scripts.\n\n"
                    .. "`data` is handed to the new scene through `GetLoadData`: plain values only, the "
                    .. "same a `RemoteEvent` argument takes, and no instance, since the old scene's are "
                    .. "gone. A client connected to a match may not call it: the scene is the server's.",
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
                Doc = "Starts preparing the scene at `path` (content-relative) while this one keeps running, and "
                    .. "returns at once with a `SceneLoad` to watch (ADR 0125): its `Progress` for a loading "
                    .. "bar, `Ready` when it is prepared, `Activate()` to switch. `options.Activate` (true by "
                    .. "default) switches as soon as it is ready; false waits for `Activate()` -- a fade, a "
                    .. "button. `options.Data` is `LoadScene`'s `data`.\n\n"
                    .. "The switch itself is `LoadScene`'s, with the file already parsed and its meshes loaded. "
                    .. "One load at a time: a second call cancels the first. A client connected to a match may "
                    .. "not call it.",
            },
            {
                Name = "GetLoadData",
                Parameters = {},
                Returns = { "any" },
                ThreadSafety = "Unsafe",
                Doc = "The `data` the last `LoadScene` passed, decoded afresh, or nil.",
            },
            {
                Name = "SendToHost",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Inside a `SubWorld` (ADR 0107): sends plain values out to the game that runs this "
                    .. "one, where the `SubWorld`'s `Received` fires with them on its next tick -- a score, a "
                    .. "game over. The values a `RemoteEvent` takes, and never an instance. Anywhere else there "
                    .. "is no host, and it raises.",
            },
            {
                Name = "IsSubWorld",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "ReadParallel",
                Doc = "Whether this world runs inside another game's `SubWorld`.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "SceneLoading",
                Parameters = { { Name = "path", Type = "string" } },
                Doc = "The scene is about to change to `path`, and the old one is still here: the moment to "
                    .. "put a loading screen up (a `ScreenGui` with `KeepOnSceneLoad` stays across).",
            },
            {
                Name = "SceneLoaded",
                Parameters = { { Name = "path", Type = "string" } },
                Doc = "The new scene is in place and its scripts have started.",
            },
            {
                Name = "SceneLoadFailed",
                Parameters = { { Name = "path", Type = "string" }, { Name = "message", Type = "string" } },
                Doc = "A `LoadScene` that could not happen: the scene is not there, or does not read. The scene "
                    .. "that was open stays as it was -- its close handlers have not run -- and `message` says "
                    .. "why, as the log does.",
            },
            {
                Name = "HostMessageReceived",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Doc = "Inside a `SubWorld` (ADR 0107): the values the game running this one passed to the "
                    .. "`SubWorld`'s `Send`, on the tick after it sent them. Never fires anywhere else.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "ServerStorage",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "What only the authority has (ADR 0080): templates and data a client has no business "
            .. "holding. Like `ReplicatedStorage` it is not the world and it is saved with the scene; "
            .. "unlike it, **nothing here replicates**, and a replica empties it when it joins. Solo, this "
            .. "machine is the authority and has it all.",
    },
    {
        Name = "NetworkService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "What this process is in a networked game, and the way to join, host or leave one (ADR 0069, "
            .. "ADR 0106). **A script asks it a gameplay question -- do I decide this? --** and `if "
            .. "NetworkService.Authority then` is a branch that is present and TAKEN in a game nobody "
            .. "networked, because solo is an authority of one.\n\n"
            .. "`Join`, `Host` and `Disconnect` change the posture at run time, at the safe point between "
            .. "two ticks, and `State` says where it stands. **`Authority` can change during a run** -- false "
            .. "from a join, true again from a leave -- so code that runs for the whole game listens to "
            .. "`State` rather than reading `Authority` once. `--host`, `--serve` and `--join=` are the same "
            .. "calls made before the first tick, and `--serve` is the only way to be a dedicated server.",
        Properties = {
            {
                Name = "Authority",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                -- A fact about the process, not about the world: a trace recorded
                -- as a host must replay solo.
                HostFact = true,
                Doc = "Whether this process decides the world: true solo, hosting or serving; false on a "
                    .. "replica, which shows what it is sent.",
            },
            {
                Name = "Topology",
                Type = "Enum.NetworkTopology",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                HostFact = true,
                Doc = "Which posture this process runs in. Branch on `Authority` for gameplay; this is for a "
                    .. "menu that wants to say which one it is.",
            },
            {
                Name = "ServerTick",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                HostFact = true,
                Doc = "The authority's tick: its own on an authority, the newest one applied on a replica. "
                    .. "Zero solo. **A count of ticks and never a time**, because two machines agree on the "
                    .. "first and never on the second.",
            },
            {
                Name = "PeerCount",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                HostFact = true,
                Doc = "Connected peers, not counting this process: the replicas an authority is serving, "
                    .. "or 1 on a replica that is connected to its authority.",
            },
            {
                Name = "State",
                Type = "Enum.NetworkState",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                HostFact = true,
                Doc = "Where this machine stands: `Offline`, `Connecting`, `Connected`, `Hosting` or `Serving`.",
            },
            {
                Name = "LocalPlayer",
                Type = "Player?",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "Safe",
                Doc = "The player at this machine: there from boot solo, hosting or joining, and nil on a "
                    .. "dedicated server, which has nobody at it.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetPlayers",
                Parameters = {},
                Returns = { "{Player}" },
                ThreadSafety = "Unsafe",
                Doc = "Everybody taking part, in the order they joined. **The same call solo** -- one player "
                    .. "-- so a game that loops over its players is already a multiplayer game.",
            },
            {
                Name = "GetStats",
                Parameters = {},
                Returns = {
                    "{ Ping: number, Jitter: number, Loss: number, SnapshotsPerSecond: number, "
                        .. "CorrectionsPerSecond: number, LastCorrection: number, InputBufferDepth: number, "
                        .. "InputStarvations: number, PredictedParts: number, ResimulationsPerSecond: number, "
                        .. "ResimulatedTicksPerSecond: number, ResimulationTime: number }",
                },
                ThreadSafety = "Unsafe",
                Doc = "**How the connection is doing**, for a game that shows a connection-quality mark: the "
                    .. "round trip in milliseconds (`Ping`), how much it varies (`Jitter`), the share of packets "
                    .. "lost in percent (`Loss`), snapshots a second, corrections of this machine's own "
                    .. "character a second and how far the last one moved it in metres (`LastCorrection`), and "
                    .. "the authority's queue of this player's input -- how many ticks it holds "
                    .. "(`InputBufferDepth`) and how many times it ran dry (`InputStarvations`). On a client, "
                    .. "what it simulates itself: the loose parts near its character it predicts "
                    .. "(`PredictedParts`), the times a second a correction stepped them again and the ticks it "
                    .. "stepped, and what one took on average in milliseconds (`ResimulationTime`). On an "
                    .. "authority, the worst peer's link and the deepest queue. All zero solo.",
            },
            {
                Name = "Join",
                Parameters = { { Name = "address", Type = "string?", Default = "nil" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Connects to a server at `address` (`host` or `host:port`), or to `[network] server` from "
                    .. "`project.toml` with none. **Joining replaces this machine's scene with the server's**: what "
                    .. "it replicates arrives, and server code stops, since this machine no longer decides the "
                    .. "world. `Connected` or `JoinFailed` says how it went. A dedicated server may not join.",
            },
            {
                Name = "Host",
                Parameters = { { Name = "port", Type = "number?", Default = "nil" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Makes this machine the authority others join, on `port` (7777 with none), keeping the "
                    .. "scene it is in. A dedicated server may not call it.",
            },
            {
                Name = "Disconnect",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Leaves the match, or stops hosting, and goes back to solo in the scene it is in; the "
                    .. "game's code decides what next. Server code starts again, fresh, since this machine "
                    .. "decides the world again. `Disconnected` fires. A dedicated server may not call it.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Connected",
                Parameters = {},
                Doc = "The join succeeded: this machine is in the server's match.",
            },
            {
                Name = "JoinFailed",
                Parameters = { { Name = "reason", Type = "string" } },
                Doc = "The join did not succeed, and this machine is solo again. The reason is readable text.",
            },
            {
                Name = "Disconnected",
                Parameters = { { Name = "reason", Type = "string" } },
                Doc = "The connection ended -- the server left, the network dropped, or `Disconnect` was "
                    .. "called -- and this machine is solo again.",
            },
            {
                Name = "PlayerAdded",
                Parameters = { { Name = "player", Type = "Player" } },
                Doc = "Somebody joined. Deferred like every signal (ADR 0015), so a script that connects in its "
                    .. "file scope and then walks `GetPlayers()` sees each player exactly once.",
            },
            {
                Name = "PlayerRemoving",
                Parameters = { { Name = "player", Type = "Player" } },
                Doc = "Somebody is leaving: the player still resolves inside the handler, for the reason "
                    .. "`Destroying` does, so a game can save what it needs from them.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "RemoteEvent",
        Extends = "Instance",
        Module = "scene",
        Doc = 'A message a game sends between machines (ADR 0077): "I bought the sword" from a '
            .. 'client to the server, "the round starts" from the server to everyone. Create it on the '
            .. "authority under `Workspace` -- in a `Folder`, if you like -- and it reaches every replica "
            .. "like any instance; a replica finds it with `WaitForChild`.\n\n"
            .. "**A client says what it did, never what happened**: the authority learns who sent a "
            .. "message from the connection, not from anything in it, so one client cannot speak for "
            .. "another. What travels is values -- nil, booleans, numbers, strings, vectors, instances "
            .. "and tables of them, eight deep -- and an instance arrives as the receiver's own copy, or "
            .. "nil where the receiver does not have it. A function, a thread or a table that refers to "
            .. "itself is refused at the call, and so is a message larger than 64 KiB.\n\n"
            .. "**One script runs solo, hosting and networked.** On an authority, `FireServer` reaches "
            .. "its own `ServerReceived` from its local player, and a host's messages to its own player "
            .. "reach its own `ClientReceived`. Messages are reliable, and delivered as deferred signals "
            .. "at the start of the tick after they arrive.",
        Methods = {
            {
                Name = "FireServer",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends the arguments to the authority, where `ServerReceived` fires with this machine's "
                    .. "player first. On an authority with a player of its own -- solo or hosting -- it is "
                    .. "delivered there directly; a dedicated server has no player to send as, and refuses.",
            },
            {
                Name = "FireClient",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends the arguments to one player's machine, where `ClientReceived` fires. Only the "
                    .. "authority speaks to clients: a replica calling this is refused.",
            },
            {
                Name = "FireAllClients",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends the arguments to every player's machine, a host's own included. Only the "
                    .. "authority speaks to clients: a replica calling this is refused.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "ServerReceived",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "arguments", Type = "...any" } },
                Doc = "Fires on the authority when a player's machine calls `FireServer`, with the player "
                    .. "who sent it first. Trust nothing else in it: it is what a client says it did.",
            },
            {
                Name = "ClientReceived",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Doc = "Fires on a player's machine when the authority calls `FireClient` for this player or "
                    .. "`FireAllClients`.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "RemoteFunction",
        Extends = "Instance",
        Module = "scene",
        Doc = 'A question a client asks the server and waits for the answer to (ADR 0079): "what is in my '
            .. 'inventory?", "may I open this door?". Create it on the authority under `Workspace` and give '
            .. "it an `OnServerInvoke`; a client calls `InvokeServerAsync`, which yields until the server's "
            .. "answer arrives and returns it.\n\n"
            .. "What travels is what a `RemoteEvent` carries, both ways, under the same limits. **The client "
            .. "says what it wants; the server decides**: the player who asked is the handler's first "
            .. "argument, taken from the connection.\n\n"
            .. "**Only a client asks.** There is no way for the server to call a client and wait: a client "
            .. "that never answered would hold the server's script forever, and a `RemoteEvent` to that "
            .. "client says the same thing without waiting.",
        Methods = {
            {
                Name = "InvokeServerAsync",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Returns = { "...any" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Asks the authority, yields until it answers, and returns what its `OnServerInvoke` "
                    .. "returned. When the handler raised an error, or the server has no handler, this raises "
                    .. "too. On an authority with a player of its own -- solo or hosting -- the handler is "
                    .. "called there, as that player, at the start of the next tick; a dedicated server has "
                    .. "no player to ask as, and refuses.",
            },
        } :: { Schema.Method },
        Callbacks = {
            {
                Name = "OnServerInvoke",
                Parameters = { { Name = "player", Type = "Player" }, { Name = "arguments", Type = "...any" } },
                Returns = { "...any" },
                Doc = "Answers `InvokeServerAsync` on the authority, with the player who asked first, and "
                    .. "returns the answer. It runs in a thread of its own and may yield. Set it once, from "
                    .. "the server's script: a replica's is never called.",
            },
        } :: { Schema.Callback },
    },
    {
        Name = "Player",
        Extends = "Instance",
        Module = "scene",
        Storage = "Player",
        Tags = { "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Somebody taking part in this world (N1). Players live under `NetworkService`, and the engine "
            .. "makes and removes them: one at a solo or hosting machine, one per replica on an authority. "
            .. "**What a player DID reaches the authority as intent** -- the values of their input actions, "
            .. "never the results -- so the authority decides what happened and a client that could assert "
            .. "an outcome does not exist.",
        Properties = {
            {
                Name = "UserId",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The authority's number for this player: 1 for whoever sits at a solo or hosting "
                    .. "machine, 2 and up for replicas in the order they joined. Never reused within a "
                    .. "session, and not an account: a reconnect is a new number.",
            },
            {
                Name = "Team",
                Type = "Team?",
                Doc = "The side this player is on (ADR 0099), or `nil` for none. **The authority's game sets it**, "
                    .. "and every replica learns it with the roster. A player who joins is put on the "
                    .. "`AutoAssign` team with the fewest players -- ties to the first -- or on none when "
                    .. "there is no such team.",
            },
            {
                Name = "Character",
                Type = "BasePart?",
                Doc = "The part that is this player in the world -- a `CharacterBody`, a racer, whatever "
                    .. "the game moves for them; a hero on the 2D plane is `Character2D`. **The authority's "
                    .. "game sets it**, and every replica learns it: each machine's `Player` for someone "
                    .. "points at that machine's copy of their part.\n\n"
                    .. "It is what the engine knows about ownership, and it decides three things. A replica "
                    .. "PREDICTS its own player's character -- the local scripts move it at once, and the "
                    .. "authority's snapshots correct it rather than overwrite it -- and draws everyone "
                    .. "else's between snapshots. And the authority sends each replica only what is near "
                    .. "that replica's character, out to `StreamingService.LoadRadius`; a player with no "
                    .. "character is sent everything.",
            },
            {
                Name = "Character2D",
                Type = "Part2D?",
                Doc = "The `Part2D` that is this player on the plane: `Character` for a 2D game, and the "
                    .. "same one thing -- a player has one character, so setting either replaces the other, "
                    .. "and each reads nil while the other kind is set. Two properties so that each is "
                    .. "typed as what it holds: a 3D game reads a `BasePart` and a 2D game a `Part2D`, and "
                    .. "neither has to say which.\n\n"
                    .. "It decides the same things. The authority sends each replica what is near its "
                    .. "character. And its own player's machine does not draw it from the past: whatever "
                    .. "that machine's scripts did to the sprite is kept, the authority's snapshots correct "
                    .. "it by the difference, and a client that steps it with the code the server steps it "
                    .. "with is never corrected at all (the multiplayer guide shows the pattern). A client "
                    .. "that moves nothing has it at the newest snapshot.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetIntent",
                Parameters = { { Name = "action", Type = "string" } },
                Returns = { "boolean | number | Vector2 | vector" },
                ThreadSafety = "Unsafe",
                Doc = "What this player's input action of that name reads this tick, in the currency "
                    .. "`InputAction:GetState` uses: a boolean, a number, a `Vector2` or a `vector`. For the "
                    .. "player at this machine it is this machine's own action; for a remote one it is what "
                    .. "their machine sent. An action they never sent reads `false`, which is what an "
                    .. "unpressed button reads.",
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
                Name = "Platform",
                Type = "Enum.Platform",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "The operating system this machine runs: `Windows`, `Linux`, `MacOS`, `Android` or "
                    .. "`IOS`. On a dedicated server it is the server's. Known from a script's first line.",
            },
            {
                Name = "SimTime",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "ReadParallel",
                Doc = "The simulation clock, in seconds, at the current tick. It is constant for the whole tick -- "
                    .. "deferred drains included -- and advances by `PhysicsService.FixedTimestep` between ticks. "
                    .. "This is the clock simulation code reads: the wall clock is forbidden to it, there is no "
                    .. "`tick()`, and `os.clock` is for profiling and never for gameplay. It stops advancing while "
                    .. "the world is paused.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Pause",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Pauses the world. The simulation clock stops, so `SimTime` stops advancing and no "
                    .. "`task.wait` or `task.delay` timer comes due; rendering and the debug overlay keep running. "
                    .. "Idempotent: pausing a paused world is a no-op, not an error.",
            },
            {
                Name = "Resume",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Restarts a paused world, and its clock with it. Idempotent: resuming a running world is a "
                    .. "no-op, not an error.",
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
                Doc = "Runs `fn(dt)` **every drawn frame**, in `priority` order -- lowest first, and in the order "
                    .. "bound between equals -- after the input a frame reads at `Rate = Render` and before "
                    .. "`PreRender` fires, with the time since the last frame (ADR 0136). What a camera that "
                    .. "follows something is for: in it, `GetRenderCFrame` answers where things are drawn, and a "
                    .. "camera written is drawn exactly as written. `Enum.RenderPriority` holds the landmarks, as "
                    .. "numbers: `Enum.RenderPriority.Camera.Value + 1` runs just after the camera.\n\n"
                    .. "Binding a name already bound replaces it. Nothing runs without a window. A binding "
                    .. "belongs to the script that made it and goes with its scene (ADR 0124).",
            },
            {
                Name = "UnbindFromRenderStep",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops the render step bound under `name`. A name nothing is bound under is not an error.",
            },
            {
                Name = "IsPaused",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "ReadParallel",
                Doc = "Whether the world is currently paused. Since `Pause` and `Resume` are both idempotent, this "
                    .. "is how code tells the two states apart rather than by watching a call fail.",
            },
            {
                Name = "SaveSimulation",
                Parameters = {},
                Returns = { "buffer" },
                ThreadSafety = "Unsafe",
                Doc = "**The 3D simulation as it is now, for rolling back to** (ADR 0101): the solver's whole "
                    .. "state, and where every simulated part and character is and how it is moving. Not which "
                    .. "instances exist, not their other properties, not the 2D layer, and not your scripts' "
                    .. "variables -- a rollback game keeps its own state beside this one and restores both. "
                    .. "Deterministic: the same world saved on two machines is the same bytes.",
            },
            {
                Name = "RestoreSimulation",
                Parameters = { { Name = "state", Type = "buffer" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Puts back what `SaveSimulation` saved, and answers true. **Answers false and changes "
                    .. "nothing** when the world's simulated parts and characters are not the ones the state "
                    .. "was saved with -- one was created, destroyed or anchored since -- or when the buffer is "
                    .. "not a saved simulation.",
            },
            {
                Name = "StepSimulation",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Steps the 3D simulation one fixed tick now, as the tick would, without running a script "
                    .. "and **without `Touched` or `TouchEnded`**: a tick simulated again already fired its "
                    .. "touches. What re-simulating after `RestoreSimulation` is: set each tick's inputs -- a "
                    .. "character's `MoveDirection`, an impulse -- and step, once per tick to catch up.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "PreRender",
                Parameters = { { Name = "dt", Type = "number" } },
                Doc = "Fires at render rate, before the frame is drawn and after the render steps "
                    .. "(`BindToRenderStep`), carrying the variable time in seconds since the previous render. Its "
                    .. "handlers run in a render phase: `GetRenderCFrame` answers where things are drawn, and a "
                    .. "camera written is presented (ADR 0136). It never fires in a headless run: headless is the same scheduler minus "
                    .. "the render steps, and this is one of them. It stays connectable so that shared code need "
                    .. "not branch on it -- but a handler connected to it in a headless process is a handler that "
                    .. "will not run.",
            },
            {
                Name = "PreAnimation",
                Parameters = { { Name = "dt", Type = "number" } },
                Doc = "The first phase of a sim tick, before animation is evaluated, carrying the fixed tick "
                    .. "duration in seconds.",
            },
            {
                Name = "PreSimulation",
                Parameters = { { Name = "dt", Type = "number" } },
                Doc = "Fires each sim tick after animation and before the simulation step, carrying the fixed tick "
                    .. "duration in seconds.",
            },
            {
                Name = "PostSimulation",
                Parameters = { { Name = "dt", Type = "number" } },
                Doc = "Fires each sim tick immediately after the simulation step, carrying the fixed tick duration "
                    .. "in seconds.",
            },
            {
                Name = "Heartbeat",
                Parameters = { { Name = "dt", Type = "number" } },
                Doc = "The last phase of a sim tick, carrying the fixed tick duration in seconds. It fires at the "
                    .. "sim tick rate like the three phases before it, and it is the usual place for per-tick "
                    .. "gameplay work that wants to see the state the tick settled on.",
            },
        } :: { Schema.Event },
        Doc = "The frame loop: the phase signals per-frame work hangs off, and the clock that work reads. In frame "
            .. "order they are `PreRender` at render rate with a variable delta, then `PreAnimation`, "
            .. "`PreSimulation`, `PostSimulation` and `Heartbeat` at the fixed sim tick rate. That rate split is "
            .. "the point of the service: render-rate work and simulation work are different jobs and get different "
            .. "signals.",
    },
    -- ADR 0105: three script services. No members by design: the tree is the
    -- whole API, and WHERE a script is decides where it runs.
    {
        Name = "ServerScriptService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "This scene's server code (ADR 0105): the arena's rules, the lobby's countdown. A `Script` here "
            .. "runs only while this machine is the authority -- solo, a host or a dedicated server -- and "
            .. "**never reaches a player that joined**: it is not replicated, a replica empties its own copy "
            .. "when it joins, and a dedicated export leaves it out of every client package.\n\n"
            .. "At boot every `src/scenes/<scene>/server/**/*.luau` file becomes a `Script` under it, with "
            .. "subdirectories as `Folder`s. It belongs to the scene, and goes when the scene does.",
    },
    {
        Name = "ClientScriptService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "This scene's player code (ADR 0105): its HUD, its menu's buttons, a camera. A `Script` here "
            .. "runs on every machine a player sits at -- solo, a host, a client that joined -- and **never on "
            .. "a dedicated server**. It is not replicated: each client runs its own, from its own package.\n\n"
            .. "At boot every `src/scenes/<scene>/client/**/*.luau` file becomes a `Script` under it. It "
            .. "belongs to the scene, and goes when the scene does.",
    },
    {
        Name = "GlobalScriptService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "The game's code, for every scene (ADR 0105). It is never unloaded: a script here keeps its "
            .. "variables, its connections and its threads across every scene change, which is where the "
            .. "character a player picked, the connection and a lobby's state are kept.\n\n"
            .. "It holds three fixed folders, made by the engine and never renamed, moved or destroyed:\n\n"
            .. "- **`Server`**: runs while this machine is the authority, like `ServerScriptService`; from "
            .. "`src/server/`.\n"
            .. "- **`Client`**: runs where a player sits, never on a dedicated server, like "
            .. "`ClientScriptService`; from `src/client/`.\n"
            .. "- **`Shared`**: `ModuleScript`s both sides require; from `src/shared/`.\n\n"
            .. "What is authored under it that is not code is saved in `content/global.json`, not in a scene.",
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
                Doc = "Whether the world streams at all. Turning it off FREEZES the current set rather than "
                    .. "draining it: nothing new is scheduled and nothing resident is evicted, which is what "
                    .. "makes it usable as `hold the world still while I do something`. Evicting while refusing "
                    .. "to load would empty the world one ring at a time and leave nothing to come back to.",
            },
            {
                Name = "LoadRadius",
                Type = "number",
                Default = "1024",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_positive",
                Doc = "How far from a focus the engine tries to keep chunks resident, in metres. Best effort: a "
                    .. "chunk inside it is wanted, and one outside stays resident until it passes a WIDER ring, "
                    .. "because a single radius makes a focus standing on a boundary load and evict the same "
                    .. "chunk every frame -- and the symptom of that is not a wrong world, it is a stutter "
                    .. "nobody can find.",
            },
            {
                Name = "MinRadius",
                Type = "number",
                Default = "512",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_positive",
                Doc = "The must-have ring, in metres. Everything inside it is guaranteed resident before a focus "
                    .. "may advance into it, so the character walks into a world that exists rather than "
                    .. "through one that has not arrived. Read `PauseOutsideLoadedArea` for what happens when it "
                    .. "has not.",
            },
            {
                Name = "PauseOutsideLoadedArea",
                Type = "boolean",
                Default = "false",
                ThreadSafety = "ReadParallel",
                Doc = "Whether the simulation waits when the minimum ring is not yet resident. A familiar "
                    .. "streaming pause, and the alternative is a character falling through ground that has not "
                    .. "loaded. It is a pause and not a rollback: the tick simply does not advance.\n\n**In a "
                    .. "world that is not the authority it does nothing.** A copy of a shared world cannot stop "
                    .. "a simulation it does not own; what it does instead is hold its camera and say that it "
                    .. "is loading.",
            },
            {
                Name = "StructureMinRadius",
                Type = "number",
                Default = "0",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The must-have ring for cells of STRUCTURES, in metres. A mountain and a pebble stop "
                    .. "sharing a radius, which is the choice one grid forces and always resolves badly in one "
                    .. "direction.\n\n**Zero means follow `MinRadius`**, which is the default and is why a world "
                    .. "authored before this existed behaves exactly as it did: everything it holds is detail, "
                    .. "and detail is what `MinRadius` and `LoadRadius` have always meant.",
            },
            {
                Name = "StructureLoadRadius",
                Type = "number",
                Default = "0",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How far from a focus cells of STRUCTURES are kept, in metres. Best effort and hysteretic, "
                    .. "exactly as `LoadRadius` is. Zero means follow `LoadRadius`.",
            },
            {
                Name = "TerrainMinRadius",
                Type = "number",
                Default = "0",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The must-have ring for cells of TERRAIN FEATURES, in metres. The longest of the three: it "
                    .. "is what puts a horizon further out than the props standing on it. Zero means follow "
                    .. "`MinRadius`.\n\nIt is also the ring for a saved `Terrain` and block world streamed in "
                    .. "cells, and on a world's first load the simulation waits until the ground inside it has "
                    .. "arrived, so nothing standing on streamed ground falls through it.",
            },
            {
                Name = "TerrainLoadRadius",
                Type = "number",
                Default = "0",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How far from a focus cells of TERRAIN FEATURES are kept, in metres. Zero means follow "
                    .. "`LoadRadius`.\n\nRaising this is the cheapest distance there is, and it is cheap for a "
                    .. "reason worth knowing: a cell of terrain holds few large things, so a ring twice as wide "
                    .. "costs far less than the same ring would cost in props. A saved `Terrain` and block "
                    .. "world streamed in cells are kept out to this radius too.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "AddFocus",
                Parameters = { { Name = "instance", Type = "Instance" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Streams the world around this instance as well. A focus is normally the character or the "
                    .. "camera; several are legal and the engine keeps a chunk that any of them wants. An "
                    .. "instance with no position in the world is refused, because a focus that cannot be "
                    .. "located would silently anchor the world at the origin.",
            },
            {
                Name = "RemoveFocus",
                Parameters = { { Name = "instance", Type = "Instance" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops streaming around it. Removing the last focus does not unload the world: it stops "
                    .. "anything being wanted, and what is resident stays until something wants it gone.",
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
                Doc = "Yields until everything within `radius` of `position` is resident: the parts' cells and "
                    .. "the ground's. What a teleport calls before it moves the character, so the destination "
                    .. "exists when they arrive -- and what a script calls at the start, before it asks "
                    .. "`Terrain:HeightAt` where to put something, since ground that has not streamed in has no "
                    .. "height yet. Returns immediately when the area is already loaded, and raises rather than "
                    .. "hanging when the world has no chunks there at all.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "AreaLoaded",
                Parameters = {
                    { Name = "position", Type = "vector" },
                    { Name = "radius", Type = "number" },
                },
                Doc = "Fires when an area a script asked for has finished loading. Deferred like every signal.",
            },
            {
                Name = "InstanceStreamedOut",
                Parameters = { { Name = "instance", Type = "Instance" } },
                Doc = "Fires for each instance that left the world while a script still held a reference to it. "
                    .. "Streamed out means REPARENTED TO NIL rather than destroyed, so the handle a script holds "
                    .. "still resolves -- it is a husk, and reading it is legal. An instance nothing referenced "
                    .. "is simply destroyed and does not fire this.",
            },
        } :: { Schema.Event },
        Doc = "The streamed world's controls (api-design.md §2.1). A world too large to hold is divided into a "
            .. "uniform grid of chunks; this service decides which of them are resident, by distance to the foci "
            .. "a game names. Streaming is a system rather than scene-root state, which is why it is a service "
            .. "and not a property of `Workspace`. The per-frame cost is budgeted in TIME rather than in a count "
            .. "of chunks, because a chunk's cost varies with what is in it.",
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
                Doc = "Returns every instance currently carrying this tag, as a fresh array the caller owns. Tags "
                    .. "are pure instance state with no relationship to the tree, so an instance parented to `nil` "
                    .. "is returned like any other.",
            },
            {
                Name = "GetInstanceAddedSignal",
                Parameters = { { Name = "tag", Type = "string" } },
                Returns = { "Signal<Instance>" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the signal that fires with each instance that gains this tag. It is deferred like "
                    .. "every signal, so a handler sees the instance at the next resumption point rather than "
                    .. "inside the `AddTag` call that added it.",
            },
            {
                Name = "GetInstanceRemovedSignal",
                Parameters = { { Name = "tag", Type = "string" } },
                Returns = { "Signal<Instance>" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the signal that fires with each instance that loses this tag, whether through "
                    .. "`RemoveTag` or through `Destroy`, which strips every tag the instance carried. The fire is "
                    .. "deferred, and a destroyed handle still resolves for the drain in which it arrives, so a "
                    .. "handler can still read what it lost before the instance goes.",
            },
            {
                Name = "GetAllTags",
                Parameters = {},
                Returns = { "{string}" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns the set of tags currently carried by at least one instance, as a fresh array -- not "
                    .. "every name ever seen. It updates synchronously with `AddTag` and `RemoveTag`, so a tag "
                    .. "leaves it the moment its last carrier drops it, even though the signal reporting that drop "
                    .. "is deferred like everything else.",
            },
        } :: { Schema.Method },
        Doc = "Finds instances by tag. Tags themselves are free-form string labels on an instance and are added, "
            .. "removed and tested through `Instance` -- `AddTag`, `RemoveTag`, `HasTag`, `GetTags`. What lives "
            .. "here are the queries that span the whole world, and the signals that report a tag coming and going.",
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
                Doc = "Whether the debug overlay is drawn. It starts off, shipped builds included, so a game can "
                    .. "offer the overlay deliberately instead of needing a different build to get it.",
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
                Doc = "Draws a line between two world positions for one frame. Headless there is no renderer and "
                    .. "the call is a silent no-op rather than an error, so debug drawing left in shared code "
                    .. "cannot fail a headless test.",
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
                Doc = "Draws the wireframe of a box at this frame, with these dimensions in metres, for one frame. "
                    .. "A silent no-op in a headless run, like every gizmo.",
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
                Doc = "Draws the wireframe of a sphere at this world position, with this radius in metres, for one "
                    .. "frame. A silent no-op in a headless run, like every gizmo.",
            },
            {
                Name = "GetStat",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "number" },
                ThreadSafety = "ReadParallel",
                Doc = "Reads a named instrumentation counter -- `FPS`, `FrameTimeMs`, `PhysicsBodies`, "
                    .. "`InstanceCount`, `DrawCalls`, `MeshLodDraws`, `LuaMemoryKB` and the rest, plus whatever `SetCustomStat` has "
                    .. "published. An unregistered name raises `scene.err.unknown_stat` instead of returning zero: "
                    .. "a misspelt stat is a bug in the caller, and a debug surface that answers zero hides that "
                    .. "bug in the one place people are already confused.",
            },
            {
                Name = "SetCustomStat",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "value", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Publishes a game-defined number under this name, readable afterwards through `GetStat` and "
                    .. "shown on the stats panel. A value that is not a number raises like any other typed-argument "
                    .. "mismatch.",
            },
            {
                Name = "ShowPanel",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Opens one panel of the overlay by name. The built-ins are `Stats`, `Scene`, `Log`, "
                    .. "`Streaming`, `Physics` and `Terrain` -- the terrain's triangles and normals around the "
                    .. "camera, red where a triangle faces the wrong way; an unknown panel raises "
                    .. "`scene.err.unknown_stat`.",
            },
            {
                Name = "HidePanel",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Closes one panel of the overlay by name, taking the same names as `ShowPanel`. An unknown "
                    .. "panel raises `scene.err.unknown_stat`.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "MessageOut",
                Parameters = {
                    { Name = "message", Type = "string" },
                    { Name = "level", Type = "Enum.LogLevel" },
                },
                Doc = "Fires for every console message, carrying the text and its level. `print` and `warn` each "
                    .. "produce exactly one deferred fire with their text verbatim, at `Info` and `Warning`. Engine "
                    .. "messages arrive pre-formatted with their key in front, as in `[scene.err.parent_cycle] "
                    .. "...`, so a handler matching on a key substring sees engine output while one matching prose "
                    .. "sees script output. Contained handler errors and the re-entrancy cap's dropped-fire log come "
                    .. "through here too.",
            },
        } :: { Schema.Event },
        Doc = "The debug overlay and the engine's own instrumentation. The service is present in shipped builds "
            .. "with the overlay off unless a game turns it on, so the panels, the stats and the log stream are "
            .. "available to a shipped game that wants them. The gizmo calls are a development aid and draw nothing "
            .. "at all in a headless run.",
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
                Doc = "Stores a value under `key` so the world built by the next reload can read it back. The value "
                    .. "is converted to an engine-side copy at the moment of the call, because the VM holding the "
                    .. "original is what a reload destroys -- so nil, booleans, numbers, strings, buffers, and "
                    .. "tables of those are accepted, while a function, a thread, an Instance or a cyclic table "
                    .. "raises `script.err.unsavable_state`. Raising beats dropping: a reload that quietly loses "
                    .. "state is worse than one that says what it could not keep.",
            },
            {
                Name = "LoadState",
                Parameters = { { Name = "key", Type = "string" } },
                Returns = { "any" },
                ThreadSafety = "Unsafe",
                Doc = "Reads back what `SaveState` stored under `key` before the reload, or nil if nothing did. "
                    .. "The value is a fresh copy built in this VM; mutating it changes nothing that a later "
                    .. "reload will see, and only another `SaveState` does.",
            },
            {
                Name = "IsReload",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Safe",
                Doc = "Whether this world was built by a hot reload rather than by starting the engine. It is how "
                    .. "a script knows to look for what it left behind instead of building it again -- the "
                    .. "instances tagged `PreserveOnReload` are already in the tree by the time any entry script "
                    .. "runs.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "PreReload",
                Parameters = {},
                Doc = "Fires on the outgoing world when a reload has been requested and before anything is torn "
                    .. "down, which is the last moment `SaveState` can be called and have the value survive. Its "
                    .. "handlers are drained before the new world is built, so the reload waits for them.",
            },
            {
                Name = "PostReload",
                Parameters = {},
                Doc = "Fires on the world a reload just built, after every entry script has had its first "
                    .. "resumption and the preserved state is already in place. A handler connected at file scope "
                    .. "therefore sees a world that is finished rather than one still being assembled.",
            },
        } :: { Schema.Event },
        Doc = "The hot-reload loop as a script can see it (ADR 0024). Present in development builds only: a "
            .. "shipped game has no reload to observe, and a service that existed there and never fired would be "
            .. "a documented no-op. A reload rebuilds the world from source and keeps nothing by default -- what "
            .. "survives is what `SaveState` stored and what was tagged `PreserveOnReload`.",
    },
    {
        Name = "PhysicsService",
        Extends = "Instance",
        -- `scene` rather than a physics module, because in this release the one
        -- member it carries is scheduler state that physics merely names -- M2
        -- brief, Decision 6 as amended.
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Properties = {
            {
                Name = "FixedTimestep",
                Type = "number",
                Default = "1/60",
                ThreadSafety = "ReadParallel",
                ErrorKey = "scene.err.number_fixed_timestep",
                Doc = "The duration of one simulation tick, in seconds, and therefore the grid every timing "
                    .. "guarantee is expressed against: `task.wait(1)` is exactly 60 ticks at the default 1/60, on "
                    .. "every machine and every run. Express durations as multiples of it and the same code stays "
                    .. "correct at 30 Hz or at 240 Hz. Writable, and a write takes effect at the next "
                    .. "frame start rather than mid-tick: the accumulator, the timer wheel and the solver all "
                    .. "read it, and changing it between two of those reads inside one frame is a class of bug "
                    .. "worth designing out. Values outside 1/240 to 1/30 are refused.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "RegisterCollisionGroup",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Declares a collision group. Registering a name that already exists is a no-op rather than "
                    .. "an error, which is what lets a script register its groups at file scope and survive a hot "
                    .. "reload. The group collides with everything until told otherwise.",
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
                Doc = "Sets whether parts in two groups collide with each other. Symmetric: setting a against b "
                    .. "sets b against a, because a one-way collision is not a thing a solver can express.",
            },
            {
                Name = "GetRegisteredCollisionGroups",
                Parameters = {},
                Returns = { "{string}" },
                ThreadSafety = "Unsafe",
                Doc = "Every registered group name, in registration order, `Default` first. A fresh array on "
                    .. "every call, and ordered rather than hashed so that iterating it cannot leak a container's "
                    .. "own order into a simulation (R10).",
            },
        } :: { Schema.Method },
        Doc = "The simulation tick grid and the physics controls that do not belong on an individual part. It "
            .. "shipped before any physics did because `FixedTimestep` is a scheduler property that physics merely "
            .. "names, and everything about scheduling and timing is expressed against it.",
    },
    {
        Name = "InputService",
        Extends = "Instance",
        Module = "input",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "The host of the Input Action System (§2.4, ADR 0029) and the only place device-wide state is "
            .. "readable. There is no UserInputService, no ContextActionService and no Mouse: one input model, "
            .. "rebindable and promptable by default (divergence #3).",
        Properties = {
            {
                Name = "PointerLocked",
                Type = "boolean",
                Default = "false",
                Transient = true,
                Doc = "Whether the pointer is locked to the window and reporting motion instead of position -- "
                    .. "what a first-person camera wants. While it is locked, `GetPointerPosition` keeps "
                    .. "reporting the position the pointer had when it was locked, because there is no other "
                    .. "honest answer, and a `Direction2D` action bound to `MouseMovement` is the way to read "
                    .. "the motion.",
            },
            {
                Name = "PointerVisible",
                Type = "boolean",
                Default = "true",
                Transient = true,
                Doc = "Whether the system cursor is drawn. Independent of `PointerLocked`, because the two are "
                    .. "separate wishes: a strategy game hides the cursor during a cutscene without locking it.",
            },
            {
                Name = "LastInputDeviceType",
                Type = "Enum.InputDeviceType",
                ReadOnly = true,
                Doc = "The device family the player most recently used, which is what a HUD switches its "
                    .. "prompts on. It changes on a real input and not on a resting one: a gamepad stick "
                    .. "drifting inside its dead zone does not steal the prompts from a keyboard.",
            },
            {
                Name = "SwipeThreshold",
                Type = "number",
                Default = "6",
                Transient = true,
                HostFact = true,
                ErrorKey = "scene.err.number_positive",
                Doc = "How far a finger travels before it is a swipe, in MILLIMETRES of screen -- a length "
                    .. "under a thumb, the same on a phone and on a monitor, where a number of pixels would "
                    .. "be a nudge on one and a reach across on the other. Six by default; the engine turns "
                    .. "it into pixels from the display's density.",
            },
            {
                Name = "TouchAvailable",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "Whether this machine has a touchscreen. **Known from a script's first line**, before "
                    .. "anybody has touched anything -- which `LastInputDeviceType` cannot say -- so a game "
                    .. "can decide which HUD to build and how far to draw before its first frame. True on a "
                    .. "phone and on a laptop with a touchscreen alike.",
            },
            {
                Name = "KeyboardAvailable",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "Whether a keyboard is attached. False on a phone until somebody plugs one in or "
                    .. "pairs one, and then true.",
            },
            {
                Name = "GamepadAvailable",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "Whether at least one gamepad is connected now. It changes as one is plugged in or "
                    .. "taken away; `InputDeviceChanged` says when one is first USED.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetPointerPosition",
                Parameters = {},
                Returns = { "Vector2" },
                Doc = "The pointer in window pixels, origin at the top left, as of the current tick. A "
                    .. "snapshot like every other input read: two calls inside one tick agree.",
            },
            {
                Name = "IsKeyDown",
                Parameters = { { Name = "keyCode", Type = "Enum.KeyCode" } },
                Returns = { "boolean" },
                Doc = "Whether that key, button or trigger is held as of the current tick. The shortest thing "
                    .. "this API can be asked, and the reason there is no separate sugar module for "
                    .. "it.\n\nIt reads the device SNAPSHOT and ignores what the UI consumed, which is the "
                    .. "opposite of the events' second argument -- a poll asks what the hardware is doing and "
                    .. "an event asks what happened to the game. A caller who wants the UI-aware answer wants "
                    .. "an `InputAction`, which is also the one that can be rebound.\n\nAn analogue source "
                    .. "counts as down past half deflection, which is the same rule a `Bool` action applies to "
                    .. "one.",
            },
            {
                Name = "SetVirtualState",
                Parameters = {
                    { Name = "keyCode", Type = "Enum.KeyCode" },
                    { Name = "value", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Drives one of the `Virtual` key codes from something that is not hardware -- a HUD "
                    .. "button, an on-screen thumbstick, an accessibility control for a player who cannot hold "
                    .. "a key (§2.4).\n\n**It writes into the same device snapshot a keyboard writes into**, "
                    .. "which is what makes it one input model rather than two: the value binds through an "
                    .. "ordinary `InputBinding`, resolves in the ordinary order, is eaten by an ordinary "
                    .. "sinking context, and is carried by the recorded stream a replay hands back. A seam "
                    .. "that reached past the snapshot would be a HUD button a recorded stream could not "
                    .. "see.\n\n**It carries a value, not a press.** Write 1 and 0 for a button; write anything "
                    .. "between for a slider or the axis of a thumbstick. Bound to a `Bool` action the value "
                    .. "counts as pressed past half deflection, which is the rule every analogue source "
                    .. "follows.\n\nThe value STICKS until it is written again -- a button that is held is a "
                    .. "1 nobody has cleared -- and losing window focus clears it with everything else, so a "
                    .. "press cannot survive an alt-tab. Refuses any key code that is not one of the `Virtual` "
                    .. "family: writing to `Space` would be a script pretending to be a keyboard.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "InputBegan",
                Parameters = {
                    { Name = "input", Type = "InputObject" },
                    { Name = "uiConsumed", Type = "boolean" },
                },
                Doc = "Fired on the tick an input STARTS -- a key pressed, a button pushed, a trigger crossing "
                    .. "half deflection (§2.4, ADR 0041).\n\n**`uiConsumed` is the second argument for a "
                    .. "reason: read it.** It is true when the interface already took this input -- the "
                    .. "pointer was over an element, or a `TextInput` had focus -- and a handler that ignores "
                    .. "it is a handler that fires the gun when the player clicks a button and jumps when they "
                    .. "type a `w` into a chat box.\n\n**What this costs, stated where it is read: a key "
                    .. "handled here cannot be rebound.** It does not appear in a remapping screen, because "
                    .. "nothing in the tree says it exists. An `InputAction` does appear, binds a keyboard key "
                    .. "and a gamepad button to one name, and is what a shipped game should use -- this is the "
                    .. "direct, familiar option beside it, and it is the right one for a prototype, a debug "
                    .. "key, or anything a player will never remap.\n\nIt fires on the SIMULATION clock, from "
                    .. "the Input Action System's own dispatch, so a handler that writes to the world "
                    .. "replays exactly. Camera look at render rate is an `InputContext` with "
                    .. "`Rate = Enum.InputRate.Render`.",
            },
            {
                Name = "InputChanged",
                Parameters = {
                    { Name = "input", Type = "InputObject" },
                    { Name = "uiConsumed", Type = "boolean" },
                },
                Doc = "Fired on a tick an input MOVED without starting or ending: pointer motion, the wheel, a "
                    .. "stick or a trigger's deflection. `Position` and `Delta` carry it.\n\nOne fire per tick "
                    .. "per source at most, with the deltas ACCUMULATED since the last one -- several device "
                    .. "events land in a tick and a handler that saw only the last would lose most of a fast "
                    .. "flick. The same rebinding cost as `InputBegan` applies.",
            },
            {
                Name = "InputEnded",
                Parameters = {
                    { Name = "input", Type = "InputObject" },
                    { Name = "uiConsumed", Type = "boolean" },
                },
                Doc = "Fired on the tick an input STOPS. It fires for everything held when the window loses "
                    .. "focus, so a handler that pairs `InputBegan` with this one never leaks a press -- which "
                    .. "is the failure that leaves a character walking into a wall after an alt-tab.\n\n"
                    .. "`uiConsumed` carries what it carried when the input began, so a press that started on "
                    .. "a button is still marked consumed when it is released off one.",
            },
            {
                Name = "TouchSwiped",
                Parameters = {
                    { Name = "direction", Type = "Enum.SwipeDirection" },
                    { Name = "start", Type = "Vector2" },
                    { Name = "fingers", Type = "number" },
                },
                Doc = "A finger -- or the mouse with its left button down -- travelled `SwipeThreshold` "
                    .. "along one axis. Fired while it is still down, with where that stroke began in "
                    .. "window pixels and how many fingers are down. One long drag one way is one swipe; a "
                    .. "change of direction is the next, measured from where the finger then is. The same "
                    .. "swipe is `Enum.KeyCode.SwipeUp` and its three siblings for an `InputAction`. A "
                    .. "press the interface took (`UIObject.Active`) starts none of these gestures.",
            },
            {
                Name = "TouchTapped",
                Parameters = { { Name = "position", Type = "Vector2" } },
                Doc = "A finger came down and went up again where it landed, within a third of a second: "
                    .. "the place, in window pixels. A mouse click on the world is one too.",
            },
            {
                Name = "TouchLongPressed",
                Parameters = { { Name = "position", Type = "Vector2" } },
                Doc = "A finger has stayed where it landed for half a second. Fired once, while it is still "
                    .. "down; lifting it afterwards is not a tap.",
            },
            {
                Name = "TouchPinched",
                Parameters = { { Name = "scale", Type = "number" }, { Name = "centre", Type = "Vector2" } },
                Doc = "Two fingers moved apart or together: the distance between them over what it was when "
                    .. "the second landed -- 2 is twice as far apart -- and the point midway between them. "
                    .. "Fired each tick it changes; multiply a zoom the game kept from before the pinch.",
            },
            {
                Name = "TouchPanned",
                Parameters = { { Name = "delta", Type = "Vector2" }, { Name = "fingers", Type = "number" } },
                Doc = "What is down moved: how far this tick, in window pixels (down the window is +Y, as "
                    .. "`Position` is), as one motion however many fingers made it. What a camera that is "
                    .. "dragged follows, with one finger or two.",
            },
            {
                Name = "InputDeviceChanged",
                Parameters = { { Name = "deviceType", Type = "Enum.InputDeviceType" } },
                Doc = "Fired when `LastInputDeviceType` changes, so a prompt redraws once rather than polling.",
            },
            {
                Name = "WindowFocusChanged",
                Parameters = { { Name = "focused", Type = "boolean" } },
                Doc = "Fired when the game window gains or loses keyboard focus. Losing focus releases every "
                    .. "held input, so an alt-tab does not leave a character walking into a wall.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "TweenService",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Property animation (§2.1). It exists so that "
            .. '"move this over half a second, easing out" is one call rather than a `Heartbeat` handler '
            .. "with a timer in it -- and so that the engine, not the game, owns the arithmetic that makes "
            .. "two such animations agree.",
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
                Doc = "A tween that will move each named property of `instance` to its goal. Created stopped: "
                    .. "`Play` starts it.\n\nEvery goal is validated NOW rather than at the first write -- a "
                    .. "property the class does not have, a read-only one, or a value of the wrong type raises "
                    .. "here, where the caller is. The alternative is a tween that plays for half a second and "
                    .. "then reports a typo. Only properties the engine can interpolate are accepted: numbers, "
                    .. "vectors, CFrames, Color3s, Vector2s, UDims, UDim2s and Rects. A boolean or a string "
                    .. "goal raises, because there is no halfway between two of either.",
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
                Doc = "The eased value of `alpha` under that curve -- the same arithmetic a tween uses, "
                    .. "exposed so a game can ease something the tween system does not own. `alpha` is not "
                    .. "clamped, and several styles leave 0-1 on purpose (`Back`, `Elastic`).",
            },
        } :: { Schema.Method },
    },
    {
        Name = "UIService",
        Extends = "Instance",
        Module = "ui",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "The parent of every `ScreenGui` and the source of the two numbers a layout needs about the "
            .. "screen it is on (§2.1). It takes the PlayerGui role without taking the Player: there is no "
            .. "`Players` service in v1 and a UI does not need one.",
        Properties = {
            {
                Name = "SafeAreaInsets",
                Type = "Rect",
                ReadOnly = true,
                Transient = true,
                -- A fact about the window, not about the world.
                HostFact = true,
                Doc = "How far in from each window edge it is safe to draw, in pixels -- what a notch, a "
                    .. "rounded corner or a system gesture bar takes away. Zero on a desktop window, which is "
                    .. "why a HUD that ignores it looks fine until it does not.",
            },
            {
                Name = "ViewportSize",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                -- The size of what is drawn into: the host's, like the two below.
                HostFact = true,
                Doc = "The size of what the interface is drawn into, in pixels: the window in a game, the "
                    .. "Viewport panel in the editor. What `AbsolutePosition` and the pointer are measured "
                    .. "in, and what a `ScreenGui.ReferenceHeight` is scaled to.",
            },
            {
                Name = "DisplayScale",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                -- A fact about the display, not about the world. A trace recorded
                -- on a doubled screen must replay on an ordinary one.
                HostFact = true,
                Doc = "The window's pixel density relative to its logical size: 2 on a doubled display. UI "
                    .. "coordinates are in PIXELS, so this is what a game multiplies by when it wants a "
                    .. "measurement to mean the same physical size on two screens.",
            },
            {
                Name = "ScreenOrientation",
                Type = "Enum.ScreenOrientation",
                Default = "Enum.ScreenOrientation.LandscapeSensor",
                -- How the window is presented, which reaches nothing the
                -- simulation reads: kept out of the world hash, so adding it
                -- moved no recorded trace.
                HostFact = true,
                Doc = "Which ways up the game may be held on a phone or a tablet. **Takes effect at once**: "
                    .. "a script that sets it turns the screen then and there, and the value saved with the "
                    .. "scene is where the game starts. A layout reads the new `AbsoluteSize` on the next "
                    .. "frame. Nothing on a desktop, whose window does not turn.",
            },
            {
                Name = "SelectedObject",
                Type = "UIObject?",
                Transient = true,
                -- Where a hand has the interface, like the focus: not the world's.
                HostFact = true,
                Doc = "The object a gamepad or the arrow keys have selected (ADR 0128), or nothing. The "
                    .. "d-pad, the left stick and the arrows move it -- to `NextSelection*` where that is "
                    .. "set, to the nearest selectable object in that direction where it is not -- and "
                    .. "`ButtonA` or Enter fires its `Activated`. Write it to put the selection somewhere: "
                    .. "a menu that opens selects its first button. An object that is not `Selectable` is "
                    .. "refused.",
            },
            {
                Name = "AutoSelect",
                Type = "boolean",
                Default = "true",
                HostFact = true,
                Doc = "Whether the engine manages the selection when the game does not: with nothing "
                    .. "selected, the first move of a d-pad, a stick or an arrow selects the first "
                    .. "selectable object on the topmost screen, and a press of the pointer clears the "
                    .. "selection -- a player who picks up the mouse sees no outline. Off, the selection "
                    .. "is what a script last wrote.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetFocusedTextInput",
                Parameters = {},
                Returns = { "TextInput?" },
                Doc = "The `TextInput` that has the keyboard, or nil (ADR 0139): what a chat hotkey asks "
                    .. "before it opens the box.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "TextInputFocused",
                Parameters = { { Name = "input", Type = "TextInput" } },
                Doc = "A `TextInput` took the keyboard.",
            },
            {
                Name = "TextInputFocusReleased",
                Parameters = { { Name = "input", Type = "TextInput" } },
                Doc = "A `TextInput` let go of it.",
            },
            {
                Name = "SelectionChanged",
                Parameters = { { Name = "selected", Type = "UIObject?" } },
                Doc = "`SelectedObject` changed, to `selected` or to nothing.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "AudioService",
        Extends = "Instance",
        Module = "audio",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Mixing and the listener (§2.1). **The listener is `Workspace.CurrentCamera`** and is not "
            .. "settable: a game with two ideas about where the player is hearing from is a game with a bug, "
            .. "and the camera is already the one thing that answers that question.",
        Properties = {
            {
                Name = "MasterVolume",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Multiplied into every sound, after its own volume and its group's. The one a settings "
                    .. "screen writes.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "PlayLocal",
                Parameters = { { Name = "content", Type = "Content" } },
                Returns = { "Sound" },
                ThreadSafety = "Unsafe",
                Doc = "Creates a 2D `Sound` parented to this service and plays it. For the fire-and-forget "
                    .. "case -- a click, a pickup -- where naming an instance is all ceremony.\n\n**It does "
                    .. "not clean up after itself in this release**: the `Sound` stays a child of the service "
                    .. "once it has ended, so a caller firing one per frame accumulates them. Keep the "
                    .. "returned handle and `Destroy` it on `Ended` where that matters. The handle is also "
                    .. "what a caller sets the volume through, since this takes only the content.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "VoxelService",
        Extends = "Instance",
        Module = "scene",
        Storage = "Voxel",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "A world made of blocks: the service for block games -- mining, building, a world of cubes in "
            .. "chunks. **It is not `Terrain`.** Terrain is a sculpted landscape, a smooth surface at any "
            .. "angle; this is a grid of cubes, each one a block type, whose surface is axis-aligned faces.\n\n"
            .. "Blocks are addressed by integer block coordinates, as a `vector` of whole numbers: block "
            .. "`(0, 0, 0)` spans from the origin to `BlockSize` on every axis. Register the block types a "
            .. "game uses once, then place and break them by id. Id 0 is air.\n\n"
            .. "The world is drawn and collided by the engine: every chunk that holds blocks is meshed "
            .. "with its hidden faces removed and its coplanar faces merged, and shaded with ambient "
            .. "occlusion at every corner.",
        Properties = {
            {
                Name = "BlockSize",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.voxel_not_empty",
                Doc = "How large a block is, in metres. **Only settable while the world holds no blocks**: "
                    .. "resizing a built world would move every block, so it is refused by name rather than "
                    .. "done quietly. `Clear` first if that is really what you want.",
            },
            {
                Name = "ChunkCount",
                Type = "number",
                ReadOnly = true,
                Doc = "How many 16 x 16 x 16 chunks hold at least one block. A chunk emptied of blocks is "
                    .. "dropped, so this counts what the world actually stores.",
            },
            {
                Name = "BlockTypeCount",
                Type = "number",
                ReadOnly = true,
                Doc = "How many block types have been registered. Ids run from 1 to this number.",
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
                Doc = "Registers a block type and returns its id. Ids are handed out in registration order "
                    .. "from 1, which makes them a pure function of the script that registered them -- the "
                    .. "same script on every machine gets the same ids. Registering a name twice returns the "
                    .. "id it already has and updates its colours.\n\n"
                    .. "`color` is the block's top, and every face when it is the only colour given. "
                    .. "`sideColor` is the four sides and `bottomColor` the underside, which defaults to the "
                    .. "sides: a grass block is green on top and earth everywhere else.\n\n"
                    .. "A world holds up to 4,095 block types.",
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
                Doc = "Gives a block type images, by content URN -- `asset://textures/grass_top.png` -- for its top, "
                    .. "its four sides and its underside; the sides default to the top and the underside to the "
                    .. "sides, exactly as the colours do. The block's colours TINT its images, so a type "
                    .. "registered white shows them as drawn. An empty string removes an image.\n\n"
                    .. "Every image fills one block face and repeats block by block, sampled without "
                    .. "smoothing: a sixteen-pixel image stays sixteen crisp pixels however close the camera is. "
                    .. "The side image stands upright on every side and is never mirrored.",
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
                Doc = "Says how much of what is behind a block type shows through it. `Cutout` blocks have holes "
                    .. "where their image is transparent; `Translucent` ones blend, and `transparency` (0 to 1, "
                    .. "default 0.5) is how much they let through where their image does not say. See-through "
                    .. "blocks still collide -- glass is a wall -- and do not shade the corners around them.",
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
                Doc = "Makes a block type a fluid -- water, lava, oil -- or, with a `reach` of 0, an ordinary "
                    .. "block again.\n\n"
                    .. "A fluid block placed with `SetBlock` or `FillBlocks` is a SOURCE, and stays. From it the "
                    .. "fluid flows DOWN first, filling every block below it that is empty; where it cannot fall "
                    .. "it spreads SIDEWAYS, one level shallower per block, for `reach` blocks (1 to 7). Take the "
                    .. "source away and what it fed drains. It moves once every `ticksPerStep` simulation ticks "
                    .. "(default 5, a quarter of a second): a slower fluid is a larger number.\n\n"
                    .. "A fluid is drawn see-through whatever its opacity, as deep as it is full, and it does not "
                    .. "collide and does not stop `Raycast`: a character wades into a lake, and a pickaxe swung "
                    .. "at the lake bed hits the bed. `GetBlock` answers its type at every level; "
                    .. "`GetFluidDepth` says how full it is.",
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
                Doc = "Says what fluid `from` becomes where it touches fluid `touching`: lava meeting water "
                    .. "sets as stone, and the water stays water. A block of `from` that touches `touching` on "
                    .. "any of its six sides turns into `result` on the next step, a source included. Both "
                    .. "types must be fluids. A `result` of 0 removes the reaction; give the pair the other way "
                    .. "round a reaction of its own if the other fluid should change too.",
            },
            {
                Name = "GetFluidDepth",
                Parameters = {
                    { Name = "block", Type = "vector" },
                },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "How full a block is with fluid, from 0 to 1: 1 where the fluid is falling or has more of "
                    .. "itself above, a little under 1 at a source, and shallower with every block it has "
                    .. "spread. 0 for a block that is not a fluid.",
            },
            {
                Name = "GetBlockId",
                Parameters = {
                    { Name = "name", Type = "string" },
                },
                Returns = { "number?" },
                ThreadSafety = "Safe",
                Doc = "The id a block type was registered with, or nil for a name nobody registered.",
            },
            {
                Name = "SetBlock",
                Parameters = {
                    { Name = "block", Type = "vector" },
                    { Name = "id", Type = "number" },
                },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Places a block, or breaks one when `id` is 0. `block` is a block coordinate; its "
                    .. "components are rounded down. Returns whether anything changed.",
            },
            {
                Name = "GetBlock",
                Parameters = {
                    { Name = "block", Type = "vector" },
                },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "The id of the block at a block coordinate, 0 for air. For a fluid it is the fluid's "
                    .. "type at every level: how full the block is, is `GetFluidDepth`'s answer.",
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
                Doc = "Fills every block in the box between two block coordinates, both included, with one "
                    .. "id -- 0 empties it. Returns how many blocks changed. The way to build a floor, a "
                    .. "wall or a world's first ground in one call rather than thousands. A box of more "
                    .. "than 256 blocks cubed is refused: fill it in pieces.",
            },
            {
                Name = "Clear",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes every block. The registered block types stay.",
            },
            {
                Name = "WorldToBlock",
                Parameters = {
                    { Name = "position", Type = "vector" },
                },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "The block coordinate a world position falls in.",
            },
            {
                Name = "BlockToWorld",
                Parameters = {
                    { Name = "block", Type = "vector" },
                },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "The world position of a block's centre.",
            },
            {
                Name = "Raycast",
                Parameters = {
                    { Name = "origin", Type = "vector" },
                    { Name = "direction", Type = "vector" },
                },
                Returns = { "vector?", "vector?" },
                ThreadSafety = "Safe",
                Doc = "Walks a ray block by block and returns the first solid block it enters and the face "
                    .. "it entered through, as an outward normal -- or nil and nil if it hits nothing within "
                    .. "`direction`'s length. The block is what a pickaxe breaks; the block plus the normal "
                    .. "is where a placed block goes. A fluid is passed through, as if it were air.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "NavigationService",
        Extends = "Instance",
        Module = "scene",
        Storage = "Navigation",
        Tags = { "Service", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Where an agent can walk, and how it gets somewhere (ADR 0089). The walkable ground is every "
            .. "anchored, colliding part under `Workspace` and the `Terrain`'s surface, turned into a "
            .. "navigation mesh for one agent size -- the four `Agent` properties.\n\n"
            .. "**The mesh is built where it is asked for.** A query builds, in square tiles, the ground "
            .. "between its two ends, and rebuilds a tile when what stands in it moved, so a door that "
            .. "opened is walkable on the next tick. Ground no query has reached yet is not built, and a "
            .. "path through it stops short: `BuildRegion` builds an area ahead of time.\n\n"
            .. "Following a path is the game's: walk a `CharacterBody` towards each waypoint in turn.",
        Properties = {
            {
                Name = "AgentRadius",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The DEFAULT agent type's width, in metres from its middle: paths keep this far from walls. "
                    .. "A size of its own is `DefineAgent`, and an agent picks one by name.",
            },
            {
                Name = "AgentHeight",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The default agent type's height: a gap lower than this is not a way through.",
            },
            {
                Name = "AgentMaxClimb",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The highest step the default agent type walks up rather than around, in metres.",
            },
            {
                Name = "AgentMaxSlope",
                Type = "number",
                Default = "45",
                ErrorKey = "scene.err.nav_slope",
                Doc = "The steepest ground the default agent type walks on, in degrees from level.",
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
                Doc = "The waypoints from `from` to `to`, each a corner to walk to in a straight line, and "
                    .. "whether they reach the goal. **Nil when `from` is not on walkable ground at all.** A "
                    .. "path that stops short -- the goal is off the ground, cut off, or past ground not built "
                    .. "yet -- still comes back, with `false`: walk it and ask again from where it ends.\n\n"
                    .. "The third answer runs beside the waypoints: the `Label` of the `NavigationLink` that "
                    .. 'begins at each, or `""` where the way on is walking. `agent` names a type '
                    .. "`DefineAgent` made; empty is the service's own (ADR 0098).",
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
                Doc = "Names another agent size, with a walkable ground of its own (ADR 0098): a giant that "
                    .. "does not fit through a door the default agent walks through. Queries and a "
                    .. "`NavigationAgent` pick it by name. Defining a name again redefines it.",
            },
            {
                Name = "SetAreaCost",
                Parameters = {
                    { Name = "label", Type = "string" },
                    { Name = "cost", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Prices the ground a `NavigationArea` labels: a path through it counts its length times "
                    .. "`cost`. `math.huge` forbids it outright. Unpriced labels cost 1; re-pricing needs no "
                    .. "rebuild.",
            },
            {
                Name = "FindPath2D",
                Parameters = {
                    { Name = "from", Type = "Vector2" },
                    { Name = "to", Type = "Vector2" },
                },
                Returns = { "{ Vector2 }?", "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "A path on the 2D plane (ADR 0098): over the cells of every `Tilemap2D` -- a filled, "
                    .. "colliding tile is a wall -- with every anchored, colliding `Part2D` a wall too. "
                    .. "Diagonal steps never cut a wall's corner. The waypoints are where the path turns, and "
                    .. "the boolean whether it reaches `to`; nil when `from` is inside a wall.",
            },
            {
                Name = "NearestPoint",
                Parameters = {
                    { Name = "point", Type = "vector" },
                    { Name = "maxDistance", Type = "number?", Default = "4" },
                },
                Returns = { "vector?" },
                ThreadSafety = "Unsafe",
                Doc = "The nearest walkable point within `maxDistance` metres, or nil. What a spawn point, "
                    .. "a teleport and a click on the world need before they can ask for a path.",
            },
            {
                Name = "Raycast",
                Parameters = {
                    { Name = "from", Type = "vector" },
                    { Name = "to", Type = "vector" },
                },
                Returns = { "vector?" },
                ThreadSafety = "Unsafe",
                Doc = "Walks straight from `from` towards `to` over the walkable ground and answers where it "
                    .. "stops -- at `to`, or where a wall or an edge is in the way. Nil when `from` is not on "
                    .. "the ground. The cheap question to ask before `FindPath`: most steps are straight.",
            },
            {
                Name = "BuildRegion",
                Parameters = {
                    { Name = "minimum", Type = "vector" },
                    { Name = "maximum", Type = "vector" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Builds the walkable ground over a box now, so the first path through it costs no "
                    .. "building. Answers how many tiles it built; ground already built and unchanged costs "
                    .. "nothing.",
            },
        } :: { Schema.Method },
    },
}

local Definition: Schema.Definition = {
    Classes = Classes,
}

return Definition
