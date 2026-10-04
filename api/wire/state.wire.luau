--!strict
--- What a replica is told about the world (ADR 0069).
---
--- **Every concrete class that exists is a class this file decides about**, and
--- the answers are `Classes`, `Excluded` and `ExcludedByTag`. Absence is not an
--- answer: a class nobody thought about and a class deliberately left out look
--- identical in a schema, and `wirecheck` refuses a tree where one is missing.
---
--- **The ids are permanent.** Not "stable", not "please do not change" -- an id
--- that means one thing to a server and another to a replica produces a symptom
--- nobody can debug from the outside: a position that decodes as a colour, a
--- health that decodes as a rotation. Removing a field means moving it to
--- `Retired`, which claims its id for ever.

local Schema = require("./schema")

-- Every list below carries its element type, and that is load-bearing rather
-- than decoration -- `api/defs/services.api.luau` explains it at length. Without
-- the annotation the new solver infers a list's element type from the entries
-- themselves, which widens every string literal to `string`: `Delivery` stops
-- being one of three words and becomes any word at all. Naming each list is an
-- assertion rather than a cast, so a wrong field name still fails analysis.

--- **Not a `Class` entry for `Instance`.** Every class extends `Instance`, so a
--- row for it would make every class in the engine decided by inheritance, and
--- the one check this file exists for would pass on an empty schema.
local Common: { Schema.Field } = {
    {
        Name = "Name",
        Id = 1,
        Encoding = "NameAtom",
        Source = "Property",
        Doc = "Scripts find things by name, so a replica whose names differ is a replica where "
            .. "`FindFirstChild` answers differently -- which is a divergence in the code rather than "
            .. "in the picture.",
    },
    {
        Name = "Parent",
        Id = 2,
        Encoding = "InstanceRef",
        Source = "Property",
        Doc = "**A network id, never a path.** A path is a string that goes stale the moment anything "
            .. "is renamed, and it is O(depth) to resolve on a replica that may not even hold the "
            .. "ancestors -- interest management gives it a subset by design.",
    },
}

local BasePartFields: { Schema.Field } = {
    {
        Name = "CFrame",
        Id = 1,
        Encoding = "CFrameD",
        Source = "Component",
        Component = "parts",
        Doc = "Where the part is. **Read from the component and not through a property**, because "
            .. "`PhysicsSync` writes it there directly under a comment naming itself the QUIET write "
            .. "-- a schema that could only name properties could not replicate motion at all "
            .. "(ADR 0069).",
    },
    {
        Name = "Size",
        Id = 2,
        Encoding = "Vector3",
        Source = "Component",
        Component = "parts",
        Doc = "Rarely changes and is cheap to diff, but a part that resized on the authority and not "
            .. "on the replica is a collision mismatch a player experiences as being stopped by "
            .. "nothing.",
    },
    {
        Name = "Anchored",
        Id = 5,
        Encoding = "Bool",
        Source = "Component",
        Component = "rigidBodies",
        Doc = "Decides whether the replica's own body is kinematic-driven or asleep, so a stale value "
            .. "costs a frame of the wrong physics. **In `rigidBodies` and not in `parts`**, which "
            .. "is where the schema first said it was: a part's simulation state is a separate "
            .. "component from its shape, because `scene` must not learn what a body is.",
    },
    {
        Name = "CanCollide",
        Id = 6,
        Encoding = "Bool",
        Source = "Component",
        Component = "rigidBodies",
        Doc = "A door that opened on the authority and not on the replica is a player walking into a "
            .. "wall that is not there.",
    }, -- **The surface, which the wire never carried** (ADR 0090). A part's
    -- `Color` and `Transparency` went out and its `Material` never did, so a
    -- replica drew every part bare; a part wears a material now, and all of
    -- what it wears travels.
    {
        Name = "Material",
        Id = 7,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "parts",
        Doc = "The material asset the part wears, by URN, or none for the engine default. A name rather "
            .. "than the material itself: the file is content both machines have, as a mesh is.",
    },
    {
        Name = "MaterialParameters",
        Id = 8,
        Encoding = "MaterialOverrides",
        Source = "Component",
        Component = "parts",
        Doc = "What the part overrides -- the tint and fade a game signals state with, which is gameplay "
            .. "rather than decoration.",
    },
    {
        Name = "MaterialClone",
        Id = 9,
        Encoding = "U32",
        Source = "Component",
        Component = "parts",
        Doc = "Which runtime copy of the material the part wears, by the authority's own number, or zero "
            .. "for the asset itself. Parts that share a copy share it on the replica too.",
    },
    {
        Name = "MaterialCloneValues",
        Id = 10,
        Encoding = "MaterialValues",
        Source = "Component",
        Component = "parts",
        Doc = "What that copy changed. **Carried with each part that wears it** rather than as an object "
            .. "of its own, so a copy is never named before it has arrived; zero when the part wears none, "
            .. "so taking one off and putting it back is a change the diff sees.",
    },
    {
        Name = "MaterialCloneColorMap",
        Id = 11,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "parts",
        Doc = "A map the copy changed, by URN; the three below are the others.",
    },
    {
        Name = "MaterialCloneNormalMap",
        Id = 12,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "parts",
        Doc = "The copy's normal map, when it changed it.",
    },
    {
        Name = "MaterialCloneMetallicRoughnessMap",
        Id = 13,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "parts",
        Doc = "The copy's metallic-roughness map, when it changed it.",
    },
    {
        Name = "MaterialCloneEmissiveMap",
        Id = 14,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "parts",
        Doc = "The copy's emissive map, when it changed it.",
    },
    {
        Name = "LinearVelocity",
        Id = 15,
        Encoding = "Vector3",
        Source = "Component",
        Component = "rigidBodies",
        Doc = "How fast a loose part moves (protocol 23, ADR 0133). A replica that predicts the part -- "
            .. "one near its character -- starts it and corrects it from the authority's motion, not "
            .. "only its place: a crate restored without its velocity steps again as a crate at rest.",
    },
    {
        Name = "AngularVelocity",
        Id = 16,
        Encoding = "Vector3",
        Source = "Component",
        Component = "rigidBodies",
        Doc = "How fast it turns, for the same reason (protocol 23).",
    },
    {
        Name = "CastShadow",
        Id = 17,
        Encoding = "Bool",
        Source = "Component",
        Component = "parts",
        Doc = "Whether the part casts a shadow (protocol 38). A look, like what it wears: a replica that "
            .. "did not have it drew a shadow under every effect the authority had asked to cast none.",
    },
    {
        Name = "ReceivesDecals",
        Id = 19,
        Encoding = "Bool",
        Source = "Component",
        Component = "parts",
        Doc = "Whether a decal paints the part (protocol 39). A look, like `CastShadow`: a replica that did "
            .. "not have it drew the scorch mark over the character the authority kept clean.",
    },
    {
        Name = "CollisionGroup",
        Id = 18,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "rigidBodies",
        Doc = "Which group the part collides as (protocol 38, D545). A replica predicts its own character "
            .. "against the parts it holds: one whose group it was never told stopped a character the "
            .. "authority's walked through. The table of groups travels in `CollisionGroups`.",
    },
}

--- What a `Part` is beyond a base part (protocol 30, D424): its shape. On
--- `Part` and not on `BasePart`, because `Shape` is `Part`'s property -- a mesh
--- part's shape is its mesh's and a character's its controller's, and a field
--- that travels is one the world's hash sees change.
local PartFields: { Schema.Field } = {
    {
        Name = "Shape",
        Id = 1,
        Encoding = "I32",
        Source = "Component",
        Component = "parts",
        Doc = "Which primitive solid the part is. **It never travelled**: a ball or a cylinder a script "
            .. "made on the authority was a block on every replica -- drawn as one, and collided with as "
            .. "one, so a body predicted on a ball the replica believed was a box was corrected every tick.",
    },
}

--- What a `MeshPart` is beyond a part (protocol 30, D424): which mesh, how it
--- collides, and what the mesh measures -- without which `Size` means nothing.
local MeshPartFields: { Schema.Field } = {
    {
        Name = "MeshContent",
        Id = 1,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "meshParts",
        Doc = "The mesh, by URN: the file is content both machines have. It never travelled, so a mesh "
            .. "part a script made on the authority drew nothing on a replica.",
    },
    {
        Name = "CollisionFidelity",
        Id = 2,
        Encoding = "I32",
        Source = "Component",
        Component = "meshParts",
        Doc = "What it collides as: a replica that predicts against a box where the authority has a hull "
            .. "is corrected whenever the two differ.",
    },
    {
        Name = "MeshSize",
        Id = 3,
        Encoding = "Vector3",
        Source = "Component",
        Component = "meshParts",
        Doc = "What the mesh measures at its own scale, which is what `Size` is a multiple of.",
    },
}

--- **Retired**: a part has no colour or transparency of its own since ADR 0090;
--- the two are its material's parameters and travel in `MaterialParameters`.
--- The ids stay claimed for ever.
local BasePartRetired: { Schema.RetiredField } = {
    {
        Name = "Color",
        Id = 3,
        Since = "2.0.0",
        Reason = "a part's colour is its material's parameter (ADR 0090); see MaterialParameters",
    },
    {
        Name = "Transparency",
        Id = 4,
        Since = "2.0.0",
        Reason = "a part's see-through is its material's parameter (ADR 0090); see MaterialParameters",
    },
}

local CharacterBodyFields: { Schema.Field } = {
    {
        Name = "VerticalVelocity",
        Id = 1,
        Encoding = "F32",
        Source = "Component",
        Component = "characterBodies",
        Doc = "**Sent because a position alone cannot be interpolated**, and vertical because that is "
            .. "the axis a controller integrates itself: a `CharacterVirtual` is not a body the "
            .. "solver knows about, so gravity accumulates here. Two positions a tick apart say where "
            .. "a body was; this says where it is going, which is what lets a replica draw a smooth "
            .. "frame between two snapshots instead of stepping.",
    },
    {
        Name = "Grounded",
        Id = 2,
        Encoding = "Bool",
        Source = "Component",
        Component = "characterBodies",
        Doc = "Drives what a game shows -- a jump, a fall, a landing -- and is the authority's answer "
            .. "rather than something a replica can derive: whether a character is on the ground "
            .. "depends on collision the replica does not solve.",
    },
    {
        Name = "State",
        Id = 3,
        Encoding = "I32",
        Source = "Component",
        Component = "characterBodies",
        Doc = "`Enum.CharacterState`, raw, exactly as the component stores it. It is what an animation "
            .. "picks a clip from, and a replica that guessed it from velocity would guess wrong at "
            .. "every transition -- which is where a player looks.",
    },
    -- **The four a prediction steps with** (D205). A
    -- replica predicts its own character with ITS copy of these, and a server
    -- script that set `WalkSpeed = 24` left every replica walking at 16 and
    -- corrected every snapshot: a rollback the player felt as a stutter.
    {
        Name = "WalkSpeed",
        Id = 4,
        Encoding = "F32",
        Source = "Component",
        Component = "characterBodies",
        Doc = "How fast the authority walks this character, and so how fast a replica must predict it "
            .. "-- a prediction at any other speed is corrected every snapshot.",
    },
    {
        Name = "JumpSpeed",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "characterBodies",
        Doc = "The upward speed a jump starts at, for the same reason as `WalkSpeed`.",
    },
    {
        Name = "MaxSlopeAngle",
        Id = 6,
        Encoding = "F32",
        Source = "Component",
        Component = "characterBodies",
        Doc = "Which slopes the controller climbs. A replica that disagreed would predict walking up "
            .. "a ramp the authority slides down.",
    },
    {
        Name = "AutoStepHeight",
        Id = 7,
        Encoding = "F32",
        Source = "Component",
        Component = "characterBodies",
        Doc = "Which ledges the controller steps up, for the same reason as `MaxSlopeAngle`.",
    },
    {
        Name = "PushVelocity",
        Id = 8,
        Encoding = "Vector3",
        Source = "Component",
        Component = "characterBodies",
        Doc = "What an impulse or a written velocity gave the character that is not its walk (protocol "
            .. "32): a knock sideways, fading. Sent for the reason `VerticalVelocity` is -- a replay that "
            .. "started from a position alone would walk a knocked-back character home every snapshot.",
    },
    {
        Name = "GravityScale",
        Id = 9,
        Encoding = "F32",
        Source = "Component",
        Component = "characterBodies",
        Doc = "A setting the character is stepped with, as `WalkSpeed` is: a replica that predicted with "
            .. "its own would be corrected every snapshot.",
    },
    {
        Name = "SwimSpeed",
        Id = 10,
        Encoding = "F32",
        Source = "Component",
        Component = "characterBodies",
        Doc = "The same.",
    },
    {
        Name = "FlySpeed",
        Id = 11,
        Encoding = "F32",
        Source = "Component",
        Component = "characterBodies",
        Doc = "The same.",
    },
    {
        Name = "Flying",
        Id = 12,
        Encoding = "Bool",
        Source = "Component",
        Component = "characterBodies",
        Doc = "The same: whether it has weight is how it is stepped.",
    },
}

--- **`moveDirection` is deliberately absent**, and it is worth saying why here
--- rather than in `Excluded`: it is a COMMAND, cleared by the mirror once
--- consumed, and it travels the other way. A client says what it did on the
--- `Intent` channel; an authority pushing a move direction back would be an
--- authority pressing a player's keys.

local ModelFields: { Schema.Field } = {
    {
        Name = "Scale",
        Id = 1,
        Encoding = "F32",
        Source = "Component",
        Component = "models",
        Doc = "One number that moves every descendant, so sending it beats sending the descendants it " .. "moved.",
    },
}

local BloomEffectFields: { Schema.Field } = {
    {
        Name = "Enabled",
        Id = 1,
        Encoding = "Bool",
        Source = "Component",
        Component = "postEffects",
        Doc = "An effect switched off from a script is off on every screen that sees the world.",
    },
    {
        Name = "Intensity",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "bloomEffects",
        Doc = "How strong the glow is.",
    },
    {
        Name = "Size",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "bloomEffects",
        Doc = "How far it reaches.",
    },
    {
        Name = "Threshold",
        Id = 4,
        Encoding = "F32",
        Source = "Component",
        Component = "bloomEffects",
        Doc = "How bright a pixel must be to glow.",
    },
}

local ColorCorrectionEffectFields: { Schema.Field } = {
    {
        Name = "Enabled",
        Id = 1,
        Encoding = "Bool",
        Source = "Component",
        Component = "postEffects",
        Doc = "An effect switched off from a script is off on every screen that sees the world.",
    },
    {
        Name = "Brightness",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "colorCorrectionEffects",
        Doc = "A grade is how the world looks, so every replica wears it.",
    },
    {
        Name = "Contrast",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "colorCorrectionEffects",
        Doc = "The same.",
    },
    {
        Name = "Saturation",
        Id = 4,
        Encoding = "F32",
        Source = "Component",
        Component = "colorCorrectionEffects",
        Doc = "The same -- a flash to grey when a player is hit is a grade a script sets.",
    },
    {
        Name = "TintColor",
        Id = 5,
        Encoding = "Color3",
        Source = "Component",
        Component = "colorCorrectionEffects",
        Doc = "The same.",
    },
}

local BlurEffectFields: { Schema.Field } = {
    {
        Name = "Enabled",
        Id = 1,
        Encoding = "Bool",
        Source = "Component",
        Component = "postEffects",
        Doc = "An effect switched off from a script is off on every screen that sees the world.",
    },
    {
        Name = "Size",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "blurEffects",
        Doc = "How far the world picture is softened.",
    },
}

local DepthOfFieldEffectFields: { Schema.Field } = {
    {
        Name = "Enabled",
        Id = 1,
        Encoding = "Bool",
        Source = "Component",
        Component = "postEffects",
        Doc = "An effect switched off from a script is off on every screen that sees the world.",
    },
    {
        Name = "FocusDistance",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "depthOfFieldEffects",
        Doc = "Where the world's lens is focused.",
    },
    {
        Name = "InFocusRadius",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "depthOfFieldEffects",
        Doc = "How deep the sharp band is.",
    },
    {
        Name = "NearIntensity",
        Id = 4,
        Encoding = "F32",
        Source = "Component",
        Component = "depthOfFieldEffects",
        Doc = "How soft the near side is.",
    },
    {
        Name = "FarIntensity",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "depthOfFieldEffects",
        Doc = "How soft the far side is.",
    },
}

local SunRaysEffectFields: { Schema.Field } = {
    {
        Name = "Enabled",
        Id = 1,
        Encoding = "Bool",
        Source = "Component",
        Component = "postEffects",
        Doc = "An effect switched off from a script is off on every screen that sees the world.",
    },
    {
        Name = "Intensity",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "sunRaysEffects",
        Doc = "How bright the shafts are.",
    },
    {
        Name = "Spread",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "sunRaysEffects",
        Doc = "How far they reach.",
    },
}

local AtmosphereFields: { Schema.Field } = {
    {
        Name = "Density",
        Id = 1,
        Encoding = "F32",
        Source = "Component",
        Component = "atmospheres",
        Doc = "The air is the world's weather: a fog rolling in on the authority rolls in everywhere.",
    },
    {
        Name = "Offset",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "atmospheres",
        Doc = "Where it is thickest.",
    },
    {
        Name = "Color",
        Id = 3,
        Encoding = "Color3",
        Source = "Component",
        Component = "atmospheres",
        Doc = "Its colour.",
    },
    {
        Name = "Decay",
        Id = 4,
        Encoding = "F32",
        Source = "Component",
        Component = "atmospheres",
        Doc = "How it thins with height.",
    },
    {
        Name = "Glare",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "atmospheres",
        Doc = "Its glow towards the sun.",
    },
    {
        Name = "Haze",
        Id = 6,
        Encoding = "F32",
        Source = "Component",
        Component = "atmospheres",
        Doc = "Its thickness at the horizon.",
    },
}

local SkyFields: { Schema.Field } = {
    {
        Name = "SkyboxBack",
        Id = 1,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "skies",
        Doc = "The pictures, by content URN through the snapshot's string table; the images ship with the game.",
    },
    {
        Name = "SkyboxDown",
        Id = 2,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "skies",
        Doc = "The same.",
    },
    {
        Name = "SkyboxFront",
        Id = 3,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "skies",
        Doc = "The same.",
    },
    {
        Name = "SkyboxLeft",
        Id = 4,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "skies",
        Doc = "The same.",
    },
    {
        Name = "SkyboxRight",
        Id = 5,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "skies",
        Doc = "The same.",
    },
    {
        Name = "SkyboxUp",
        Id = 6,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "skies",
        Doc = "The same.",
    },
    {
        Name = "SkyboxOrientation",
        Id = 7,
        Encoding = "Vector3",
        Source = "Component",
        Component = "skies",
        Doc = "How the pictures are turned.",
    },
    {
        Name = "SunTexture",
        Id = 8,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "skies",
        Doc = "The sun's picture.",
    },
    {
        Name = "MoonTexture",
        Id = 9,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "skies",
        Doc = "The moon's picture.",
    },
    {
        Name = "SunAngularSize",
        Id = 10,
        Encoding = "F32",
        Source = "Component",
        Component = "skies",
        Doc = "How large the sun looks.",
    },
    {
        Name = "MoonAngularSize",
        Id = 11,
        Encoding = "F32",
        Source = "Component",
        Component = "skies",
        Doc = "How large the moon looks.",
    },
    {
        Name = "StarCount",
        Id = 12,
        Encoding = "F32",
        Source = "Component",
        Component = "skies",
        Doc = "How many stars.",
    },
    {
        Name = "CelestialBodiesShown",
        Id = 13,
        Encoding = "Bool",
        Source = "Component",
        Component = "skies",
        Doc = "Whether the sun, moon and stars are drawn.",
    },
    {
        Name = "CloudCover",
        Id = 14,
        Encoding = "F32",
        Source = "Component",
        Component = "skies",
        Doc = "How much of the sky is cloud -- weather, and the world's.",
    },
    {
        Name = "CloudDensity",
        Id = 15,
        Encoding = "F32",
        Source = "Component",
        Component = "skies",
        Doc = "How thick the clouds are.",
    },
    {
        Name = "CloudColor",
        Id = 16,
        Encoding = "Color3",
        Source = "Component",
        Component = "skies",
        Doc = "Their colour.",
    },
}

local NoFields: { Schema.Field } = {}

local LightingFields: { Schema.Field } = {
    {
        Name = "ClockTime",
        Id = 1,
        Encoding = "F32",
        Source = "Component",
        Component = "lighting",
        Doc = "Night on the authority is night on every replica: the sun is where the game says it is.",
    },
    {
        Name = "GeographicLatitude",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "lighting",
        Doc = "Where the sun's path runs, which with the clock is where every shadow falls.",
    },
    {
        Name = "Ambient",
        Id = 3,
        Encoding = "Color3",
        Source = "Component",
        Component = "lighting",
        Doc = "The light in the shadows, which a game changes to say it is getting dark.",
    },
    {
        Name = "Brightness",
        Id = 4,
        Encoding = "F32",
        Source = "Component",
        Component = "lighting",
        Doc = "How strong the sun is.",
    },
    {
        Name = "FogColor",
        Id = 5,
        Encoding = "Color3",
        Source = "Component",
        Component = "lighting",
        Doc = "What the distance fades into.",
    },
    {
        Name = "FogStart",
        Id = 6,
        Encoding = "F32",
        Source = "Component",
        Component = "lighting",
        Doc = "Where it starts to fade.",
    },
    {
        Name = "FogEnd",
        Id = 7,
        Encoding = "F32",
        Source = "Component",
        Component = "lighting",
        Doc = "Where it is gone -- a storm rolling in is this number coming closer.",
    },
    {
        Name = "ExposureCompensation",
        Id = 8,
        Encoding = "F32",
        Source = "Component",
        Component = "lighting",
        Doc = "A flash, a blinding moment, a cave entrance a game brightens.",
    },
    {
        Name = "EnvironmentDiffuseScale",
        Id = 9,
        Encoding = "F32",
        Source = "Component",
        Component = "lighting",
        Doc = "How much the sky lights the world (ADR 0096).",
    },
    {
        Name = "EnvironmentSpecularScale",
        Id = 10,
        Encoding = "F32",
        Source = "Component",
        Component = "lighting",
        Doc = "How much it is reflected.",
    },
    {
        Name = "ShadowSoftness",
        Id = 11,
        Encoding = "F32",
        Source = "Component",
        Component = "lighting",
        Doc = "How soft the sun's shadows are.",
    },
    {
        Name = "GlobalShadows",
        Id = 12,
        Encoding = "Bool",
        Source = "Component",
        Component = "lighting",
        Doc = "Whether the sun casts at all.",
    },
    {
        Name = "AutoExposure",
        Id = 13,
        Encoding = "Bool",
        Source = "Component",
        Component = "lighting",
        Doc = "Whether the world's exposure follows the frame.",
    },
    {
        Name = "ExposureMin",
        Id = 14,
        Encoding = "F32",
        Source = "Component",
        Component = "lighting",
        Doc = "The most the automatic exposure darkens, in EV stops (protocol 31).",
    },
    {
        Name = "ExposureMax",
        Id = 15,
        Encoding = "F32",
        Source = "Component",
        Component = "lighting",
        Doc = "The most the automatic exposure brightens, in EV stops (protocol 31).",
    },
}

local DecalFields: { Schema.Field } = {
    {
        Name = "CFrame",
        Id = 1,
        Encoding = "CFrameD",
        Source = "Component",
        Component = "decals",
        Doc = "Where the mark is -- relative to the part it is on, so a mark on a moving crate moves with it.",
    },
    {
        Name = "Size",
        Id = 2,
        Encoding = "Vector3",
        Source = "Component",
        Component = "decals",
        Doc = "How much of the world the mark covers.",
    },
    {
        Name = "Texture",
        Id = 3,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "decals",
        Doc = "The image, by content URN, through the snapshot's string table as every name travels.",
    },
    {
        Name = "Color",
        Id = 4,
        Encoding = "Color3",
        Source = "Component",
        Component = "decals",
        Doc = "What the image is multiplied by.",
    },
    {
        Name = "Transparency",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "decals",
        Doc = "A mark fading out is a game saying something.",
    },
}

local ParticleEmitterFields: { Schema.Field } = {
    {
        Name = "Enabled",
        Id = 1,
        Encoding = "Bool",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "A stream a replica keeps drawing after the authority stopped it is smoke from a fire somebody put out.",
    },
    {
        Name = "Rate",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "How dense the stream is, which a game changes to say how much is burning.",
    },
    {
        Name = "Lifetime",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "How far a stream reaches, with Speed.",
    },
    {
        Name = "Speed",
        Id = 4,
        Encoding = "F32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "How far a stream reaches, with Lifetime.",
    },
    {
        Name = "SpreadAngle",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "A jet and a fountain are one number apart.",
    },
    {
        Name = "Acceleration",
        Id = 6,
        Encoding = "Vector3",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "Whether smoke rises or sparks fall.",
    },
    {
        Name = "Drag",
        Id = 7,
        Encoding = "F32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "How soon a puff stops.",
    },
    {
        Name = "Color",
        Id = 8,
        Encoding = "Color3",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "What it looks like, which is the whole of what it is.",
    },
    {
        Name = "ColorEnd",
        Id = 9,
        Encoding = "Color3",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "What it fades to.",
    },
    {
        Name = "Size",
        Id = 10,
        Encoding = "F32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "How big a particle is born.",
    },
    {
        Name = "SizeEnd",
        Id = 11,
        Encoding = "F32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "How big it dies.",
    },
    {
        Name = "Transparency",
        Id = 12,
        Encoding = "F32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "How solid it is born.",
    },
    {
        Name = "TransparencyEnd",
        Id = 13,
        Encoding = "F32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "How solid it dies.",
    },
    {
        Name = "LightEmission",
        Id = 14,
        Encoding = "F32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "Whether it glows or blends.",
    },
    {
        Name = "Brightness",
        Id = 15,
        Encoding = "F32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "How hard it glows.",
    },
    {
        Name = "Shape",
        Id = 16,
        Encoding = "I32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "What one particle looks like.",
    },
    {
        Name = "Emitted",
        Id = 17,
        Encoding = "U32",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "**How bursts reach a replica**: a running total of what `Emit` asked for, so a replica spawns the difference it has not seen -- the low 32 bits, because a difference is all it ever takes, and it wraps safely.",
    },
    {
        Name = "WindAffectsDrift",
        Id = 18,
        Encoding = "Bool",
        Source = "Component",
        Component = "particleEmitters",
        Doc = "Whether the smoke leans with the wind (ADR 0115, protocol 19).",
    },
}

--- The workspace's own properties that travel (ADR 0115, protocol 19): the
--- wind, so every player sees the same one.
local WorkspaceFields: { Schema.Field } = {
    {
        Name = "GlobalWind",
        Id = 1,
        Encoding = "Vector3",
        Source = "Component",
        Component = "workspaces",
        Doc = "The wind the authority set is the wind every replica draws.",
    },
    {
        Name = "WindGusts",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "workspaces",
        Doc = "How gusty, so a gust crosses every player's field the same.",
    },
    {
        Name = "WindTurbulence",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "workspaces",
        Doc = "How turbulent.",
    },
    {
        Name = "Gravity",
        Id = 4,
        Encoding = "Vector3",
        Source = "Component",
        Component = "workspaces",
        Doc = "**What a replica simulates its own bodies under** (protocol 34): a part it owns or predicts, "
            .. "a vehicle it drives. Left behind, a game that changed gravity on the authority had every "
            .. "owner's parts fall at the old rate and corrected a round trip later.",
    },
}

--- A sprite and a body on the 2D plane (the 2D layer, ADR 0088). Every field is
--- in its component, which the 2D mirror writes quietly as the 3D one writes a
--- part's.
local Part2DFields: { Schema.Field } = {
    {
        Name = "Position",
        Id = 1,
        Encoding = "Vector3",
        Source = "Component",
        Component = "parts2d",
        Doc = "Where it is, on the plane. **A `Vector2` travels as a `Vector3` with a zero z** (ADR 0088): "
            .. "twelve bytes the codec already has, rather than an encoding of its own for four fewer.",
    },
    {
        Name = "Rotation",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "parts2d",
        Doc = "Degrees, as authored. A turned crate on the authority is a turned crate everywhere.",
    },
    {
        Name = "Size",
        Id = 3,
        Encoding = "Vector3",
        Source = "Component",
        Component = "parts2d",
        Doc = "The picture's size and the collider's, for the reason a `BasePart`'s travels.",
    },
    {
        Name = "Velocity",
        Id = 4,
        Encoding = "Vector3",
        Source = "Component",
        Component = "parts2d",
        Doc = "Sent because a script on the replica reads it -- a run animation picks its frame from it -- "
            .. "and because it is what a later version extrapolates with.",
    },
    {
        Name = "AngularVelocity",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "parts2d",
        Doc = "The same argument as `Velocity`.",
    },
    {
        Name = "Color",
        Id = 6,
        Encoding = "Color3",
        Source = "Component",
        Component = "parts2d",
        Doc = "Games say things with colour; the same argument as a `BasePart`'s.",
    },
    {
        Name = "Transparency",
        Id = 7,
        Encoding = "F32",
        Source = "Component",
        Component = "parts2d",
        Doc = "A blinking hurt sprite is a gameplay fact.",
    },
    {
        Name = "Anchored",
        Id = 8,
        Encoding = "Bool",
        Source = "Component",
        Component = "parts2d",
        Doc = "Decides whether the replica's body is kinematic-driven or ground.",
    },
    {
        Name = "CanCollide",
        Id = 9,
        Encoding = "Bool",
        Source = "Component",
        Component = "parts2d",
        Doc = "A door that opened on the authority must be open on the replica.",
    },
    {
        Name = "Sensor",
        Id = 10,
        Encoding = "Bool",
        Source = "Component",
        Component = "parts2d",
        Doc = "Whether it pushes: a replica's own character must pass through a coin.",
    },
    {
        Name = "Shape",
        Id = 11,
        Encoding = "I32",
        Source = "Component",
        Component = "parts2d",
        Doc = "`Enum.Shape2D`, raw: what it is drawn and collides as.",
    },
    {
        Name = "ZIndex",
        Id = 12,
        Encoding = "I32",
        Source = "Component",
        Component = "parts2d",
        Doc = "Which sprite is in front; a game changes it to say something.",
    },
    {
        Name = "FlipX",
        Id = 13,
        Encoding = "Bool",
        Source = "Component",
        Component = "parts2d",
        Doc = "**The facing of a sprite is animation state**, and a replica that did not receive "
            .. "it would show every other player running backwards half the time.",
    },
    {
        Name = "FlipY",
        Id = 14,
        Encoding = "Bool",
        Source = "Component",
        Component = "parts2d",
        Doc = "The same as `FlipX`.",
    },
    {
        Name = "Image",
        Id = 15,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "parts2d",
        Doc = "The picture, by URN, through the snapshot's string table as a `Decal`'s is.",
    },
    {
        Name = "ImageRectOffset",
        Id = 16,
        Encoding = "Vector3",
        Source = "Component",
        Component = "parts2d",
        Doc = "**The frame of a sprite sheet**: a walk cycle is this changing, so it is the other half "
            .. "of a replicated sprite's animation. A `Vector2` as a `Vector3`, as `Position` is.",
    },
    {
        Name = "ImageRectSize",
        Id = 17,
        Encoding = "Vector3",
        Source = "Component",
        Component = "parts2d",
        Doc = "The frame's size, beside its offset.",
    },
    {
        Name = "Filter",
        Id = 18,
        Encoding = "I32",
        Source = "Component",
        Component = "parts2d",
        Doc = "`Enum.TextureFilter`, raw: pixel art stays crisp on every screen.",
    },
    {
        Name = "ExactColor",
        Id = 19,
        Encoding = "Bool",
        Source = "Component",
        Component = "parts2d",
        Doc = "Whether it is drawn past the tone curve (ADR 0153, protocol 32): a sprite the server made "
            .. "lit is lit on every screen.",
    },
}

--- A tilemap on the 2D plane (ADR 0103, protocol 15). Its properties are fields
--- like any other class's; its cells are not a property and travel in
--- `TilemapBlocks`, a block at a time.
local Tilemap2DFields: { Schema.Field } = {
    {
        Name = "Position",
        Id = 1,
        Encoding = "Vector3",
        Source = "Component",
        Component = "tilemaps2d",
        Doc = "Where cell (0, 0) is, on the plane. A `Vector2` as a `Vector3`, as a `Part2D`'s is.",
    },
    {
        Name = "CellSize",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "tilemaps2d",
        Doc = "How big a cell is, in metres: the ground a replica stands on is this size.",
    },
    {
        Name = "Tileset",
        Id = 3,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "tilemaps2d",
        Doc = "The picture the tiles are cut from, by URN, through the string table.",
    },
    {
        Name = "TileSize",
        Id = 4,
        Encoding = "Vector3",
        Source = "Component",
        Component = "tilemaps2d",
        Doc = "One tile's size in the tileset, in pixels.",
    },
    {
        Name = "ZIndex",
        Id = 5,
        Encoding = "I32",
        Source = "Component",
        Component = "tilemaps2d",
        Doc = "Which of it and a sprite is in front.",
    },
    {
        Name = "Color",
        Id = 6,
        Encoding = "Color3",
        Source = "Component",
        Component = "tilemaps2d",
        Doc = "What the tiles are multiplied by.",
    },
    {
        Name = "Filter",
        Id = 7,
        Encoding = "I32",
        Source = "Component",
        Component = "tilemaps2d",
        Doc = "`Enum.TextureFilter`, raw.",
    },
    {
        Name = "Collides",
        Id = 8,
        Encoding = "Bool",
        Source = "Component",
        Component = "tilemaps2d",
        Doc = "Whether its solid tiles are ground: a replica's own character stands on them.",
    },
    {
        Name = "Friction",
        Id = 9,
        Encoding = "F32",
        Source = "Component",
        Component = "tilemaps2d",
        Doc = "How the ground grips, for the same reason.",
    },
    {
        Name = "ExactColor",
        Id = 10,
        Encoding = "Bool",
        Source = "Component",
        Component = "tilemaps2d",
        Doc = "As a `Part2D`'s (ADR 0153, protocol 32).",
    },
}

local ClickDetectorFields: { Schema.Field } = {
    {
        Name = "MaxActivationDistance",
        Id = 1,
        Encoding = "F32",
        Source = "Component",
        Component = "clickDetectors",
        Doc = "A replica decides what its pointer is on, so it needs the reach the authority checks against.",
    },
    {
        Name = "CursorIcon",
        Id = 2,
        Encoding = "NameAtom",
        Source = "Component",
        Component = "clickDetectors",
        Doc = "What the pointer shows over it, on the machine that points.",
    },
}

-- Water (ADR 0118, protocol 28): every machine draws the same surface and the
-- authority floats what is in it, so the shape and the waves travel whole.
local WaterFields: { Schema.Field } = {
    {
        Name = "Shape",
        Id = 1,
        Encoding = "I32",
        Source = "Component",
        Component = "waters",
        Doc = "Ocean, box or river.",
    },
    {
        Name = "SurfaceLevel",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "waters",
        Doc = "The still surface's height.",
    },
    {
        Name = "Position",
        Id = 3,
        Encoding = "Vector3",
        Source = "Component",
        Component = "waters",
        Doc = "A box's middle.",
    },
    {
        Name = "Size",
        Id = 4,
        Encoding = "Vector3",
        Source = "Component",
        Component = "waters",
        Doc = "A box's extent; a river's width and depth.",
    },
    {
        Name = "Density",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "waters",
        Doc = "What floats in it and how high.",
    },
    {
        Name = "Viscosity",
        Id = 6,
        Encoding = "F32",
        Source = "Component",
        Component = "waters",
        Doc = "How it drags.",
    },
    {
        Name = "Current",
        Id = 7,
        Encoding = "Vector3",
        Source = "Component",
        Component = "waters",
        Doc = "How it flows.",
    },
    {
        Name = "FlowSpeed",
        Id = 8,
        Encoding = "F32",
        Source = "Component",
        Component = "waters",
        Doc = "How fast a river flows.",
    },
}

local WaterWaveFields: { Schema.Field } = {
    {
        Name = "Wavelength",
        Id = 1,
        Encoding = "F32",
        Source = "Component",
        Component = "waterWaves",
        Doc = "Crest to crest.",
    },
    {
        Name = "Amplitude",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "waterWaves",
        Doc = "How high a crest rises.",
    },
    {
        Name = "Direction",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "waterWaves",
        Doc = "Which way it travels, in degrees.",
    },
    {
        Name = "Steepness",
        Id = 4,
        Encoding = "F32",
        Source = "Component",
        Component = "waterWaves",
        Doc = "How sharp its crests are.",
    },
    {
        Name = "Phase",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "waterWaves",
        Doc = "Where in its cycle it starts.",
    },
}

local WaterPointFields: { Schema.Field } = {
    {
        Name = "Position",
        Id = 1,
        Encoding = "Vector3",
        Source = "Component",
        Component = "waterPoints",
        Doc = "Where the river passes.",
    },
}

-- Everything a replica needs to begin a drag and, for a `Physical` one of a
-- part it is handed, to pull it: the shape, the limits, the force. Not
-- `ReferenceInstance`, which is the authority's -- a replica drags in the
-- world's frame, and a `Geometric` drag is moved by the authority anyway.
-- Everything a replica needs to show a prompt and trigger it: the texts, the
-- keys, the reach, the rules. `UIOffset` is not carried -- it moves a drawn
-- box by pixels, and a game that sets it sets it where the prompt is drawn.
--- **Joints on the wire** (protocol 34, NA34). They were excluded on the
--- argument that a replica's bodies are kinematic and solve nothing -- true of
--- what the authority simulates, and false of what a replica OWNS (ADR 0099) or
--- predicts (ADR 0133): an owner received a vehicle's parts and none of what
--- held them together, and the vehicle fell apart on the one machine that
--- drove it. A replica solves a joint when it simulates one of the bodies; any
--- other copy is held and ignored.
local AttachmentFields: { Schema.Field } = {
    {
        Name = "CFrame",
        Id = 1,
        Encoding = "CFrameD",
        Source = "Component",
        Component = "attachments",
        Doc = "Where on its part the frame is. A joint holds two of these together, so a replica "
            .. "that solves the joint (NA34) needs them where the authority has them.",
    },
}

local ConstraintFields: { Schema.Field } = {
    {
        Name = "Attachment0",
        Id = 1,
        Encoding = "InstanceRef",
        Source = "Component",
        Component = "constraints",
        Doc = "The first end. A network id; resolved on the replica to its own copy, and written "
            .. "once that copy exists.",
    },
    {
        Name = "Attachment1",
        Id = 2,
        Encoding = "InstanceRef",
        Source = "Component",
        Component = "constraints",
        Doc = "The second end. A network id; resolved on the replica to its own copy, and written "
            .. "once that copy exists.",
    },
    {
        Name = "Enabled",
        Id = 3,
        Encoding = "Bool",
        Source = "Component",
        Component = "constraints",
        Doc = "A joint switched off holds nothing, on every machine that solves it.",
    },
    {
        Name = "CollideConnected",
        Id = 4,
        Encoding = "Bool",
        Source = "Component",
        Component = "constraints",
        Doc = "Whether the two bodies it joins collide with each other.",
    },
    {
        Name = "Visible",
        Id = 5,
        Encoding = "Bool",
        Source = "Component",
        Component = "constraints",
        Doc = "Whether the engine draws it.",
    },
    {
        Name = "BreakForce",
        Id = 6,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The force past which it breaks; zero never breaks.",
    },
    {
        Name = "BreakTorque",
        Id = 7,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The same, for a twist.",
    },
}

local BallSocketFields: { Schema.Field } = {
    {
        Name = "LimitsEnabled",
        Id = 1,
        Encoding = "Bool",
        Source = "Component",
        Component = "constraints",
        Doc = "Whether its stops hold.",
    },
    {
        Name = "SwingLimit",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How far a ball joint swings from its axis.",
    },
    {
        Name = "TwistLimit",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How far a ball joint twists about its axis.",
    },
    {
        Name = "ActuatorType",
        Id = 4,
        Encoding = "I32",
        Source = "Component",
        Component = "constraints",
        Doc = "None, a motor or a servo: what drives the joint.",
    },
    {
        Name = "TargetOrientation",
        Id = 5,
        Encoding = "CFrameD",
        Source = "Component",
        Component = "constraints",
        Doc = "The pose a ball joint holds; the position half is zero.",
    },
    {
        Name = "ServoMaxForce",
        Id = 6,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How hard the servo may push.",
    },
    {
        Name = "Responsiveness",
        Id = 7,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How quickly a servo closes on its target.",
    },
    {
        Name = "Stiffness",
        Id = 8,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The joint as a spring, in its own terms.",
    },
    {
        Name = "Damping",
        Id = 9,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The same, for damping.",
    },
}

local HingeFields: { Schema.Field } = {
    {
        Name = "LimitsEnabled",
        Id = 1,
        Encoding = "Bool",
        Source = "Component",
        Component = "constraints",
        Doc = "Whether its stops hold.",
    },
    {
        Name = "LimitLow",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The lower stop: an angle or a distance, by the kind of joint.",
    },
    {
        Name = "LimitHigh",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The upper stop.",
    },
    {
        Name = "ActuatorType",
        Id = 4,
        Encoding = "I32",
        Source = "Component",
        Component = "constraints",
        Doc = "None, a motor or a servo: what drives the joint.",
    },
    {
        Name = "MotorVelocity",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The speed a motor holds: what turns a vehicle's wheels on the machine that owns it.",
    },
    {
        Name = "MotorMaxForce",
        Id = 6,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How hard the motor may push to hold it.",
    },
    {
        Name = "MotorMaxAcceleration",
        Id = 7,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How fast the motor may reach it.",
    },
    {
        Name = "ServoTarget",
        Id = 8,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The angle or place a servo goes to: a steering rack.",
    },
    {
        Name = "ServoSpeed",
        Id = 9,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How fast the servo goes there.",
    },
    {
        Name = "ServoMaxForce",
        Id = 10,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How hard the servo may push.",
    },
    {
        Name = "Responsiveness",
        Id = 11,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How quickly a servo closes on its target.",
    },
    {
        Name = "Stiffness",
        Id = 12,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The joint as a spring, in its own terms.",
    },
    {
        Name = "Damping",
        Id = 13,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The same, for damping.",
    },
}

local PrismaticFields: { Schema.Field } = {
    {
        Name = "LimitsEnabled",
        Id = 1,
        Encoding = "Bool",
        Source = "Component",
        Component = "constraints",
        Doc = "Whether its stops hold.",
    },
    {
        Name = "LimitLow",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The lower stop: an angle or a distance, by the kind of joint.",
    },
    {
        Name = "LimitHigh",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The upper stop.",
    },
    {
        Name = "ActuatorType",
        Id = 4,
        Encoding = "I32",
        Source = "Component",
        Component = "constraints",
        Doc = "None, a motor or a servo: what drives the joint.",
    },
    {
        Name = "MotorVelocity",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The speed a motor holds: what turns a vehicle's wheels on the machine that owns it.",
    },
    {
        Name = "MotorMaxForce",
        Id = 6,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How hard the motor may push to hold it.",
    },
    {
        Name = "MotorMaxAcceleration",
        Id = 7,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How fast the motor may reach it.",
    },
    {
        Name = "ServoTarget",
        Id = 8,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The angle or place a servo goes to: a steering rack.",
    },
    {
        Name = "ServoSpeed",
        Id = 9,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How fast the servo goes there.",
    },
    {
        Name = "ServoMaxForce",
        Id = 10,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How hard the servo may push.",
    },
    {
        Name = "Responsiveness",
        Id = 11,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How quickly a servo closes on its target.",
    },
    {
        Name = "Stiffness",
        Id = 12,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The joint as a spring, in its own terms.",
    },
    {
        Name = "Damping",
        Id = 13,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The same, for damping.",
    },
}

local RopeFields: { Schema.Field } = {
    {
        Name = "Length",
        Id = 1,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "Its free length. A winch moves a rope's on the authority every tick, and the replica "
            .. "is sent where it got to.",
    },
    {
        Name = "WinchEnabled",
        Id = 2,
        Encoding = "Bool",
        Source = "Component",
        Component = "constraints",
        Doc = "Whether a rope takes itself in or lets itself out.",
    },
    {
        Name = "WinchTarget",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The length a winch goes to.",
    },
    {
        Name = "WinchSpeed",
        Id = 4,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How fast.",
    },
    {
        Name = "WinchForce",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How hard it may pull.",
    },
    {
        Name = "Color",
        Id = 6,
        Encoding = "Color3",
        Source = "Component",
        Component = "constraints",
        Doc = "Its colour, where it is drawn.",
    },
    {
        Name = "Thickness",
        Id = 7,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How thick it is drawn.",
    },
}

local RodFields: { Schema.Field } = {
    {
        Name = "Length",
        Id = 1,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "Its free length. A winch moves a rope's on the authority every tick, and the replica "
            .. "is sent where it got to.",
    },
    {
        Name = "Color",
        Id = 2,
        Encoding = "Color3",
        Source = "Component",
        Component = "constraints",
        Doc = "Its colour, where it is drawn.",
    },
    {
        Name = "Thickness",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How thick it is drawn.",
    },
}

local SpringFields: { Schema.Field } = {
    {
        Name = "Length",
        Id = 1,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "Its free length. A winch moves a rope's on the authority every tick, and the replica "
            .. "is sent where it got to.",
    },
    {
        Name = "Stiffness",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The joint as a spring, in its own terms.",
    },
    {
        Name = "Damping",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "The same, for damping.",
    },
    {
        Name = "LimitsEnabled",
        Id = 4,
        Encoding = "Bool",
        Source = "Component",
        Component = "constraints",
        Doc = "Whether its stops hold.",
    },
    {
        Name = "MinLength",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "A spring's shortest.",
    },
    {
        Name = "MaxLength",
        Id = 6,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "A spring's longest.",
    },
    {
        Name = "Color",
        Id = 7,
        Encoding = "Color3",
        Source = "Component",
        Component = "constraints",
        Doc = "Its colour, where it is drawn.",
    },
    {
        Name = "Thickness",
        Id = 8,
        Encoding = "F32",
        Source = "Component",
        Component = "constraints",
        Doc = "How thick it is drawn.",
    },
}

local LinearVelocityFields: { Schema.Field } = {
    {
        Name = "Mode",
        Id = 1,
        Encoding = "I32",
        Source = "Component",
        Component = "movers",
        Doc = "Which of its ways it works: a vector, a line or a plane; one end or two.",
    },
    {
        Name = "Vector",
        Id = 2,
        Encoding = "Vector3",
        Source = "Component",
        Component = "movers",
        Doc = "The speed, the spin, the force or the torque it holds.",
    },
    {
        Name = "LineDirection",
        Id = 3,
        Encoding = "Vector3",
        Source = "Component",
        Component = "movers",
        Doc = "The line a speed is held along.",
    },
    {
        Name = "LineVelocity",
        Id = 4,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "The speed along it.",
    },
    {
        Name = "PrimaryTangentAxis",
        Id = 5,
        Encoding = "Vector3",
        Source = "Component",
        Component = "movers",
        Doc = "The plane a speed is held in, by its first axis.",
    },
    {
        Name = "SecondaryTangentAxis",
        Id = 6,
        Encoding = "Vector3",
        Source = "Component",
        Component = "movers",
        Doc = "And its second.",
    },
    {
        Name = "PlaneVelocity",
        Id = 7,
        Encoding = "Vector3",
        Source = "Component",
        Component = "movers",
        Doc = "The speed in that plane; the third number is zero.",
    },
    {
        Name = "MaxForce",
        Id = 8,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "How hard it may push.",
    },
    {
        Name = "RelativeTo",
        Id = 9,
        Encoding = "I32",
        Source = "Component",
        Component = "movers",
        Doc = "The frame its vector is written in.",
    },
}

local AngularVelocityFields: { Schema.Field } = {
    {
        Name = "Vector",
        Id = 1,
        Encoding = "Vector3",
        Source = "Component",
        Component = "movers",
        Doc = "The speed, the spin, the force or the torque it holds.",
    },
    {
        Name = "MaxTorque",
        Id = 2,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "How hard it may turn.",
    },
    {
        Name = "RelativeTo",
        Id = 3,
        Encoding = "I32",
        Source = "Component",
        Component = "movers",
        Doc = "The frame its vector is written in.",
    },
    {
        Name = "ReactionEnabled",
        Id = 4,
        Encoding = "Bool",
        Source = "Component",
        Component = "movers",
        Doc = "Whether what it does to one body it does, reversed, to the other.",
    },
}

local AlignPositionFields: { Schema.Field } = {
    {
        Name = "Mode",
        Id = 1,
        Encoding = "I32",
        Source = "Component",
        Component = "movers",
        Doc = "Which of its ways it works: a vector, a line or a plane; one end or two.",
    },
    {
        Name = "Position",
        Id = 2,
        Encoding = "Position",
        Source = "Component",
        Component = "movers",
        Doc = "The place it pulls to: a world coordinate.",
    },
    {
        Name = "MaxForce",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "How hard it may push.",
    },
    {
        Name = "MaxVelocity",
        Id = 4,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "How fast it may travel.",
    },
    {
        Name = "MoverResponsiveness",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "How quickly it closes on its target.",
    },
    {
        Name = "RigidityEnabled",
        Id = 6,
        Encoding = "Bool",
        Source = "Component",
        Component = "movers",
        Doc = "As rigid as a tick can show.",
    },
    {
        Name = "ApplyAtCenterOfMass",
        Id = 7,
        Encoding = "Bool",
        Source = "Component",
        Component = "movers",
        Doc = "Whether it pushes at the body's centre rather than at its attachment.",
    },
    {
        Name = "ReactionEnabled",
        Id = 8,
        Encoding = "Bool",
        Source = "Component",
        Component = "movers",
        Doc = "Whether what it does to one body it does, reversed, to the other.",
    },
    {
        Name = "MoverStiffness",
        Id = 9,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "It as a spring, in its own terms.",
    },
    {
        Name = "MoverDamping",
        Id = 10,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "The same, for damping.",
    },
}

local AlignOrientationFields: { Schema.Field } = {
    {
        Name = "Mode",
        Id = 1,
        Encoding = "I32",
        Source = "Component",
        Component = "movers",
        Doc = "Which of its ways it works: a vector, a line or a plane; one end or two.",
    },
    {
        Name = "Orientation",
        Id = 2,
        Encoding = "CFrameD",
        Source = "Component",
        Component = "movers",
        Doc = "The facing it turns to; the position half is zero.",
    },
    {
        Name = "MaxTorque",
        Id = 3,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "How hard it may turn.",
    },
    {
        Name = "MaxAngularVelocity",
        Id = 4,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "How fast it may turn.",
    },
    {
        Name = "MoverResponsiveness",
        Id = 5,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "How quickly it closes on its target.",
    },
    {
        Name = "RigidityEnabled",
        Id = 6,
        Encoding = "Bool",
        Source = "Component",
        Component = "movers",
        Doc = "As rigid as a tick can show.",
    },
    {
        Name = "ReactionEnabled",
        Id = 7,
        Encoding = "Bool",
        Source = "Component",
        Component = "movers",
        Doc = "Whether what it does to one body it does, reversed, to the other.",
    },
    {
        Name = "MoverStiffness",
        Id = 8,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "It as a spring, in its own terms.",
    },
    {
        Name = "MoverDamping",
        Id = 9,
        Encoding = "F32",
        Source = "Component",
        Component = "movers",
        Doc = "The same, for damping.",
    },
}

local VectorForceFields: { Schema.Field } = {
    {
        Name = "Vector",
        Id = 1,
        Encoding = "Vector3",
        Source = "Component",
        Component = "movers",
        Doc = "The speed, the spin, the force or the torque it holds.",
    },
    {
        Name = "RelativeTo",
        Id = 2,
        Encoding = "I32",
        Source = "Component",
        Component = "movers",
        Doc = "The frame its vector is written in.",
    },
    {
        Name = "ApplyAtCenterOfMass",
        Id = 3,
        Encoding = "Bool",
        Source = "Component",
        Component = "movers",
        Doc = "Whether it pushes at the body's centre rather than at its attachment.",
    },
}

local TorqueFields: { Schema.Field } = {
    {
        Name = "Vector",
        Id = 1,
        Encoding = "Vector3",
        Source = "Component",
        Component = "movers",
        Doc = "The speed, the spin, the force or the torque it holds.",
    },
    {
        Name = "RelativeTo",
        Id = 2,
        Encoding = "I32",
        Source = "Component",
        Component = "movers",
        Doc = "The frame its vector is written in.",
    },
}

local WeldFields: { Schema.Field } = {
    {
        Name = "Part0",
        Id = 1,
        Encoding = "InstanceRef",
        Source = "Component",
        Component = "welds",
        Doc = "What the weld holds to. A network id; resolved on the replica to its own copy, and "
            .. "written once that copy exists.",
    },
    {
        Name = "Part1",
        Id = 2,
        Encoding = "InstanceRef",
        Source = "Component",
        Component = "welds",
        Doc = "What the weld moves. A network id; resolved on the replica to its own copy, and "
            .. "written once that copy exists.",
    },
    {
        Name = "C0",
        Id = 3,
        Encoding = "CFrameD",
        Source = "Component",
        Component = "welds",
        Doc = "The joint's frame on the first.",
    },
    {
        Name = "C1",
        Id = 4,
        Encoding = "CFrameD",
        Source = "Component",
        Component = "welds",
        Doc = "The joint's frame on the second.",
    },
    {
        Name = "Enabled",
        Id = 5,
        Encoding = "Bool",
        Source = "Component",
        Component = "welds",
        Doc = "A weld switched off holds nothing.",
    },
}

--- A weld constraint's offset is not sent: it reads it off the world where it
--- is first solved, and a replica's two parts are drawn from the same snapshot,
--- so the offset it reads is the authority's.
local WeldConstraintFields: { Schema.Field } = {
    {
        Name = "Part0",
        Id = 1,
        Encoding = "InstanceRef",
        Source = "Component",
        Component = "welds",
        Doc = "What the weld holds to. A network id; resolved on the replica to its own copy, and "
            .. "written once that copy exists.",
    },
    {
        Name = "Part1",
        Id = 2,
        Encoding = "InstanceRef",
        Source = "Component",
        Component = "welds",
        Doc = "What the weld moves. A network id; resolved on the replica to its own copy, and written "
            .. "once that copy exists.",
    },
    {
        Name = "Enabled",
        Id = 3,
        Encoding = "Bool",
        Source = "Component",
        Component = "welds",
        Doc = "A weld switched off holds nothing.",
    },
}

local NoCollisionFields: { Schema.Field } = {
    {
        Name = "Part0",
        Id = 1,
        Encoding = "InstanceRef",
        Source = "Component",
        Component = "noCollisions",
        Doc = "One of the two. A network id; resolved on the replica to its own copy, and written "
            .. "once that copy exists.",
    },
    {
        Name = "Part1",
        Id = 2,
        Encoding = "InstanceRef",
        Source = "Component",
        Component = "noCollisions",
        Doc = "The other. A network id; resolved on the replica to its own copy, and written once "
            .. "that copy exists.",
    },
    {
        Name = "Enabled",
        Id = 3,
        Encoding = "Bool",
        Source = "Component",
        Component = "noCollisions",
        Doc = "Switched off, the two collide again.",
    },
}

local Classes: { Schema.Class } = {
    {
        Name = "BasePart",
        Doc = "The moving half of nearly every game, and therefore the field set that decides whether "
            .. "replication is affordable at all. `Part` and `MeshPart` inherit it.",
        Fields = BasePartFields,
        Retired = BasePartRetired,
    },
    {
        Name = "Part",
        Extends = "BasePart",
        Doc = "The primitive solid. Everything `BasePart` carries, and which solid it is.",
        Fields = PartFields,
    },
    {
        Name = "MeshPart",
        Extends = "BasePart",
        Doc = "A part whose geometry is a file's. Everything `BasePart` carries, and the three things "
            .. "that say which mesh it is and how large.",
        Fields = MeshPartFields,
    },
    {
        Name = "CharacterBody",
        Extends = "BasePart",
        Doc = "A player's body, and the one class where the authority's answer and the client's guess "
            .. "are most visibly different. **Carries `BasePart`'s six too**, by `Extends` -- the first "
            .. "version of this entry said so in its doc and not in its data, and a replica would have "
            .. "held a character that never moved.",
        Fields = CharacterBodyFields,
    },
    {
        Name = "Model",
        Doc = "Replicated for its scale alone: everything else about a model is its children, and "
            .. "they replicate as themselves.",
        Fields = ModelFields,
    },
    {
        Name = "Lighting",
        Service = true,
        -- ADR 0096: the world's look is instances under it, and they travel with
        -- it. A camera's do not -- `Camera` is excluded, and its subtree with it.
        Contents = true,
        Doc = "**The first service whose properties travel**: the time of day, the fog and the light. "
            .. "It is never spawned -- every world has one from boot, on both ends -- so it travels under "
            .. "a fixed id and a replica writes its own.",
        Fields = LightingFields,
    },
    {
        Name = "Decal",
        Doc = "A scorch mark, a footprint, a poster (F2). Replicated since the snapshot's string table "
            .. "carries every name-shaped field rather than `Name` alone, which is what its image URN "
            .. "needed.",
        Fields = DecalFields,
    },
    {
        Name = "ParticleEmitter",
        Doc = "Sparks, smoke and magic (F2). Its particles are a picture and never travel -- each "
            .. "machine simulates its own -- but how an emitter is set up is what a game changes to say "
            .. "something, and a burst a script asked for on the authority has to happen on a replica too.",
        Fields = ParticleEmitterFields,
    },
    {
        Name = "Folder",
        Doc = "**Replicated with no fields of its own, and that is the point.** A folder is "
            .. "structure: a `Parent` reference to one has to resolve on the replica, so the folder "
            .. "has to be there. Its `Name` and `Parent` come from the common set.",
        Fields = NoFields,
    },
    {
        Name = "RemoteEvent",
        Doc = "Replicated so a message has something both ends agree it is for (ADR 0077): its "
            .. "network id is the address a `RemoteToAuthority` or `RemoteToReplica` names. No fields "
            .. "of its own; a message is not state.",
        Fields = NoFields,
    },
    {
        Name = "UnreliableRemoteEvent",
        Doc = "Replicated for the reason `RemoteEvent` is (ADR 0161, protocol 38): its network id is "
            .. "the address an `UnreliableToAuthority` or `UnreliableToReplica` names. No fields of its "
            .. "own.",
        Fields = NoFields,
    },
    {
        Name = "ReplicatedStorage",
        Service = true,
        Contents = true,
        Doc = "A service with no fields whose contents travel (ADR 0080): every instance under it "
            .. "reaches every replica, whatever its position, parented to the replica's own "
            .. "`ReplicatedStorage`.",
        Fields = NoFields,
    },
    {
        Name = "RemoteFunction",
        Doc = "Replicated for the reason `RemoteEvent` is (ADR 0079): a question and its answer both "
            .. "name it. Its handler is a function on the authority and never travels.",
        Fields = NoFields,
    },
    {
        Name = "Part2D",
        Doc = "The 2D layer's moving half (ADR 0088, protocol 11): where a sprite is, how it is turned "
            .. "and which frame of its sheet it shows. A replica drives it kinematically, as it drives a "
            .. "`BasePart`, and draws a remote one between snapshots, its rotation the short way round "
            .. "(ADR 0103, protocol 15).",
        Fields = Part2DFields,
    },
    {
        Name = "Tilemap2D",
        Doc = "A level on the plane (ADR 0103, protocol 15): its properties as fields, and its cells in "
            .. "`TilemapBlocks` -- every block after the spawn, then each block an edit changed. What a "
            .. "wall broken or a floor built on the authority is on every replica.",
        Fields = Tilemap2DFields,
    },
    {
        Name = "ClickDetector",
        Doc = "A part a player clicks (ADR 0126, protocol 21). Each machine resolves its own pointer and tells "
            .. "the authority, so the detector has to be there.",
        Fields = ClickDetectorFields,
    },
    {
        Name = "Water",
        Doc = "Water (ADR 0118, protocol 28): its surface and what it does to what floats in it.",
        Fields = WaterFields,
    },
    {
        Name = "WaterWave",
        Doc = "One wave of a `Water` (ADR 0118, protocol 28).",
        Fields = WaterWaveFields,
    },
    {
        Name = "WaterPoint",
        Doc = "A point a river runs through (ADR 0118, protocol 28).",
        Fields = WaterPointFields,
    },
    {
        Name = "BloomEffect",
        Doc = "The world's look under `Lighting` (ADR 0096, protocol 13): the authority's is every replica's. A camera's stays on its own screen.",
        Fields = BloomEffectFields,
    },
    {
        Name = "ColorCorrectionEffect",
        Doc = "The world's look under `Lighting` (ADR 0096, protocol 13): the authority's is every replica's. A camera's stays on its own screen.",
        Fields = ColorCorrectionEffectFields,
    },
    {
        Name = "BlurEffect",
        Doc = "The world's look under `Lighting` (ADR 0096, protocol 13): the authority's is every replica's. A camera's stays on its own screen.",
        Fields = BlurEffectFields,
    },
    {
        Name = "Swarm",
        Doc = "A crowd the engine steers (ADR 0156), replicated so a replica has something to ask "
            .. "(ADR 0162, protocol 38). Its agents are rows, not instances: with `Replicates` they "
            .. "travel in `SwarmAgents` and `SwarmState`, and without it a replica's copy is empty.",
        Fields = {
            {
                Name = "Replicates",
                Id = 1,
                Encoding = "Bool",
                Source = "Component",
                Component = "swarms",
                Doc = "Whether the authority sends this swarm's agents: a replica's copy is read-only "
                    .. "either way.",
            },
            {
                Name = "ReplicationRadius",
                Id = 2,
                Encoding = "F32",
                Source = "Component",
                Component = "swarms",
                Doc = "How far from a player's focus an agent is sent, so a replica's script can read what "
                    .. "the authority decided.",
            },
        },
    },
    {
        Name = "DepthOfFieldEffect",
        Doc = "The world's look under `Lighting` (ADR 0096, protocol 13): the authority's is every replica's. A camera's stays on its own screen.",
        Fields = DepthOfFieldEffectFields,
    },
    {
        Name = "SunRaysEffect",
        Doc = "The world's look under `Lighting` (ADR 0096, protocol 13): the authority's is every replica's. A camera's stays on its own screen.",
        Fields = SunRaysEffectFields,
    },
    {
        Name = "Atmosphere",
        Doc = "The world's look under `Lighting` (ADR 0096, protocol 13): the authority's is every replica's. A camera's stays on its own screen.",
        Fields = AtmosphereFields,
    },
    {
        Name = "Sky",
        Doc = "The world's look under `Lighting` (ADR 0096, protocol 13): the authority's is every replica's. A camera's stays on its own screen.",
        Fields = SkyFields,
    },
    {
        Name = "Workspace",
        Service = true,
        Doc = "The wind (ADR 0115, protocol 19). **A service for its properties only**: its children "
            .. "already travel as the world, by where they are, so it does not carry its contents the way "
            .. "`ReplicatedStorage` does. After every other service in this list, so each of them keeps its "
            .. "id; what follows it is instances.",
        Fields = WorkspaceFields,
    },
    {
        Name = "Attachment",
        Doc = "A frame on a part, which a joint holds to another (NA34). A `Bone` is one too, and stays "
            .. "off the wire by its own exclusion: its pose is derived from state that travels.",
        Fields = AttachmentFields,
    },
    {
        Name = "Constraint",
        Doc = "What every joint between two attachments has (NA34): its two ends, whether it holds, and "
            .. "when it breaks. Each kind adds what it is, by `Extends`, and is spawned on the replica as "
            .. "the class it is.",
        Fields = ConstraintFields,
    },
    {
        Name = "BallSocketConstraint",
        Extends = "Constraint",
        Doc = "A ball joint: its cone and twist, and the pose a servo holds it in.",
        Fields = BallSocketFields,
    },
    {
        Name = "HingeConstraint",
        Extends = "Constraint",
        Doc = "A hinge: its stops, and the motor or servo that turns it -- a vehicle's wheel.",
        Fields = HingeFields,
    },
    {
        Name = "FixedConstraint",
        Extends = "Constraint",
        Doc = "Two parts held as one by the solver. Nothing of its own beyond a joint's.",
        Fields = NoFields,
    },
    {
        Name = "PrismaticConstraint",
        Extends = "Constraint",
        Doc = "A slider: its stops, and the motor or servo that moves it -- a lift, a suspension.",
        Fields = PrismaticFields,
    },
    {
        Name = "RopeConstraint",
        Extends = "Constraint",
        Doc = "A rope: its length and its winch, and how it is drawn.",
        Fields = RopeFields,
    },
    {
        Name = "RodConstraint",
        Extends = "Constraint",
        Doc = "A rod: its length, and how it is drawn.",
        Fields = RodFields,
    },
    {
        Name = "SpringConstraint",
        Extends = "Constraint",
        Doc = "A spring: its free length, how stiff, its stops, and how it is drawn.",
        Fields = SpringFields,
    },
    {
        Name = "LinearVelocity",
        Extends = "Constraint",
        Doc = "A mover (ADR 0127, NA34): the speed it holds. Applied only where its body is simulated -- "
            .. "on the authority, or on the replica that owns or predicts it.",
        Fields = LinearVelocityFields,
    },
    {
        Name = "AngularVelocity",
        Extends = "Constraint",
        Doc = "A mover: the spin it holds.",
        Fields = AngularVelocityFields,
    },
    {
        Name = "AlignPosition",
        Extends = "Constraint",
        Doc = "A mover: the place it pulls to.",
        Fields = AlignPositionFields,
    },
    {
        Name = "AlignOrientation",
        Extends = "Constraint",
        Doc = "A mover: the facing it turns to.",
        Fields = AlignOrientationFields,
    },
    {
        Name = "VectorForce",
        Extends = "Constraint",
        Doc = "A mover: the force it pushes with.",
        Fields = VectorForceFields,
    },
    {
        Name = "Torque",
        Extends = "Constraint",
        Doc = "A mover: the torque it turns with.",
        Fields = TorqueFields,
    },
    {
        Name = "Weld",
        Doc = "Two parts held as one (NA34). A replica that simulates either end resolves it as the "
            .. "authority does; any other is sent where the welded part ended up, and ignores it.",
        Fields = WeldFields,
    },
    {
        Name = "WeldConstraint",
        Doc = "A weld that reads its offset off the world, on a replica too; as `Weld`.",
        Fields = WeldConstraintFields,
    },
    {
        Name = "NoCollisionConstraint",
        Doc = "Two parts that pass through each other (NA34): a vehicle's wheel and its arch, on the "
            .. "machine that drives it as on the authority.",
        Fields = NoCollisionFields,
    },
}

--- Recorded by name, because a class nobody decided about and a class
--- deliberately left out look identical in a schema.
local Excluded: { Schema.Excluded } = {
    {
        Name = "FoliageLayer",
        Reason = "Foliage is drawn and never simulated (ADR 0116): placement is a pure function of the "
            .. "terrain, the layer and its seed, so every machine grows the same field from the scene it "
            .. "already has, and the wire carries none of it.",
    },
    {
        Name = "FoliageMesh",
        Reason = "Part of a `FoliageLayer`, and excluded for its reason.",
    },
    {
        Name = "Highlight",
        Reason = "A mark on the picture (ADR 0129). ADR 0129 has it travel as an instance and each machine "
            .. "draw its own; it does not yet. What it marks is named by reference, which the wire carries "
            .. "since protocol 34 (NA34); a box in the network's ledger.",
    },
    {
        Name = "Beam",
        Reason = "Drawn between two attachments (ADR 0129). `Attachment` is on the wire since protocol 34, "
            .. "but a sequence of colours or of widths is not an encoding it has. Excluded until it is; a box "
            .. "in the network's ledger.",
    },
    {
        Name = "Trail",
        Reason = "Excluded for `Beam`'s reasons.",
    },
    {
        Name = "Terrain",
        Reason = "Its bulk is not a property and a field is megabytes. A world's ground arrives with "
            .. "the world -- from the scene, or later from a streamed cell -- and a sculpt is rare, "
            .. "authored, and not a per-tick fact. ADR 0069 decides this by name so the next person "
            .. "does not have to work out whether it was considered.",
    },
    {
        Name = "Player",
        Reason = "Lives under `NetworkService`, which is not replicated, and each end makes its own: an "
            .. "authority one per peer, a replica one for itself and one for each player the `Players` "
            .. "roster names. What a replica knows of a player -- their number and their character -- "
            .. "is the roster, not a snapshot of the instance; their attributes travel on their own "
            .. "(ADR 0106), a side included.",
    },
    {
        Name = "DataModel",
        Reason = "The root. Every world has exactly one and it is created at boot on both sides, so "
            .. "there is nothing to spawn and nothing to reference by id.",
    },

    {
        Name = "Bone",
        Reason = "The same argument as `Attachment`, plus one: a bone's world transform is derived "
            .. "from an animated pose, and the pose is derived from state that does replicate.",
    },
    {
        Name = "Ragdoll",
        Reason = "A ragdoll is parts and constraints, and the parts replicate. The ragdoll instance "
            .. "is the recipe rather than the result.",
    },
    {
        Name = "Constraint2D",
        Reason = "The same as `BallSocketConstraint`, on the plane (ADR 0102): a replica's sprites are "
            .. "moved to where the authority's joints put them, and `Part2D.Position` is what arrives. "
            .. "The one entry covers its three classes.",
    },
    {
        Name = "SpriteAnimator",
        Reason = "What it does is write its sprite's `ImageRectOffset`, and that property replicates "
            .. "(ADR 0102). Sending the animator too would play the sheet twice on a replica.",
    },

    {
        Name = "Camera",
        Reason = "A replica's view is its own. Replicating it would take the camera away from the "
            .. "player, which is the one thing a client must decide.",
    },
    {
        Name = "Script",
        Reason = "Source code is not world state. A replica runs the game it was shipped, and a "
            .. "server that could push source to a client would be a server that can run code on it.",
    },
    {
        Name = "ModuleScript",
        Reason = "The same as `Script`.",
    },
    {
        Name = "Sound",
        Reason = "Playback position is local and drifts by design. What replicates is the world event "
            .. "that starts a sound, not the sound.",
    },
    {
        Name = "AudioGroup",
        Reason = "A mixer bus. What a player hears at what volume is theirs.",
    },
    {
        Name = "ReverbSoundEffect",
        Reason = "What the speakers do with a sound is each machine's, as the `Sound` and the `AudioGroup` it sits under are.",
    },
    {
        Name = "EchoSoundEffect",
        Reason = "Excluded for `ReverbSoundEffect`'s reason.",
    },
    {
        Name = "EqualizerSoundEffect",
        Reason = "Excluded for `ReverbSoundEffect`'s reason.",
    },
    {
        Name = "LowPassSoundEffect",
        Reason = "Excluded for `ReverbSoundEffect`'s reason.",
    },
    {
        Name = "HighPassSoundEffect",
        Reason = "Excluded for `ReverbSoundEffect`'s reason.",
    },
    {
        Name = "DistortionSoundEffect",
        Reason = "Excluded for `ReverbSoundEffect`'s reason.",
    },
    {
        Name = "CompressorSoundEffect",
        Reason = "Excluded for `ReverbSoundEffect`'s reason.",
    },
    {
        Name = "ChorusSoundEffect",
        Reason = "Excluded for `ReverbSoundEffect`'s reason.",
    },
    {
        Name = "PitchShiftSoundEffect",
        Reason = "Excluded for `ReverbSoundEffect`'s reason.",
    },
    {
        Name = "PointLight",
        Reason = "Lighting is drawn from world state a replica already has. A light that moved "
            .. "because its parent moved needs no message of its own.",
    },
    {
        Name = "SpotLight",
        Reason = "The same as `PointLight`.",
    },

    -- ADR 0096: the abstract base of the five effects, which travel by name.
    {
        Name = "PostEffect",
        Reason = "Abstract. Its five classes are replicated by name.",
    },
    {
        Name = "Material",
        Reason = "A material is an asset reference, and assets arrive with the build rather than over " .. "the wire.",
    },

    {
        Name = "InputAction",
        Reason = "Input is the client's to send as intent and never the authority's to push back. "
            .. "ADR 0041 already routes it through one dispatch; replicating it would give a server a "
            .. "second way to press a player's keys.",
    },
    {
        Name = "InputBinding",
        Reason = "A rebinding is a preference on one machine. The same as `InputAction`.",
    },
    {
        Name = "InputContext",
        Reason = "Which bindings are live is local UI state. The same as `InputAction`.",
    },

    {
        Name = "ScreenGui",
        Reason = "Screen-space UI is drawn from local state by local scripts. What a player sees on "
            .. "their own HUD is not a fact the authority owns.",
    },
    {
        Name = "BillboardGui",
        Reason = "World-space UI is built by each peer's own scripts, as the screen's is: everything under "
            .. "it is a UI class the wire does not carry, so sending the root alone would send an empty "
            .. "board. A name tag over another player's character is made on each client from what IS "
            .. "replicated -- the character and its name.",
    },
    { Name = "SurfaceGui", Reason = "World-space UI. The same as `BillboardGui`." },
    {
        Name = "NavigationArea",
        Reason = "What it says is a question only the authority's navigation asks (ADR 0098): a path is "
            .. "searched where the game's scripts run, and a replica that wants one asks with a remote. "
            .. "The part it labels is replicated; the label is not something a replica draws.",
    },
    { Name = "NavigationLink", Reason = "A way across for the authority's paths. The same as `NavigationArea`." },
    {
        Name = "NavigationAgent",
        Reason = "It walks its part on the authority's tick, and the part's `CFrame` is what arrives -- "
            .. "interpolated like any other part's. Sending the agent too would send a second account of "
            .. "one motion.",
    },
    { Name = "Frame", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "TextLabel", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "TextButton", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "TextInput", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "ImageLabel", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "ImageButton", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "ScrollFrame", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "UICorner", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "UIGradient", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "ViewportFrame", Reason = "Screen-space UI. The same as `ScreenGui`." },
    {
        Name = "CameraTexture",
        Reason = "Its camera does not replicate -- a replica's view is its own -- so a feed sent without "
            .. "one would show nothing, and one kept under its camera would not arrive at all. A feed is set "
            .. "up by the machine that draws it (ADR 0107's finding).",
    },
    {
        Name = "SubWorld",
        Reason = "A world of its own, run by the machine that loads it (ADR 0107 §3): what it holds is "
            .. "not this world's state, and a sub-world shared by players is a later decision.",
    },
    { Name = "UIStroke", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "UIListLayout", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "UIPadding", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "UIGridLayout", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "UIPageLayout", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "UIFlexItem", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "UIScale", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "UIAspectRatioConstraint", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "UISizeConstraint", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "UITextSizeConstraint", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "UIDragDetector", Reason = "Screen-space UI. The same as `ScreenGui`." },
    { Name = "CanvasGroup", Reason = "Screen-space UI. The same as `ScreenGui`." },

    {
        Name = "AnimationPlayer",
        Reason = "**The first candidate for protocol 2, and out of N1 on purpose.** What an animation "
            .. "needs is which clip, at what time, at what weight -- three fields whose meaning is a "
            .. "blend tree the wire would have to describe. N1 replicates where bodies are, which is "
            .. "what a player sees; making the limbs move the same way needs a design of its own and "
            .. "would double this milestone.",
    },
}

--- **One reason, written once.** Thirteen copies of a sentence is a schema
--- nobody reads, and a schema nobody reads is one nobody notices a fourteenth
--- entry being wrong in.
local ExcludedByTag: { Schema.ExcludedTag } = {
    {
        Tag = "Service",
        Reason = "A service is a singleton every world creates at boot, on both sides, so there is "
            .. "nothing to spawn, despawn or reference by network id. Whether a service's PROPERTIES "
            .. "are world state is a separate, per-service question: a service whose properties travel is "
            .. "listed under `Classes` with `Service = true` (`Lighting` is), and every other one "
            .. "falls here.",
    },
}

--- **Numbers are permanent for the reason field ids are.** Channel 3 was
--- reserved so the numbering could not shift when network ownership arrived,
--- and it has (ADR 0099). `RemoteEvent` arrived without it: its messages ride
--- the Control channel, after the spawns they may name (ADR 0077).
local Channels: { Schema.Channel } = {
    {
        Name = "Control",
        Id = 0,
        Delivery = "Reliable",
        Doc = "Handshake, spawn, despawn, and the baseline acknowledgement. Everything whose loss "
            .. "cannot be recovered by the next tick's snapshot.",
    },
    {
        Name = "State",
        Id = 1,
        Delivery = "UnreliableSequenced",
        Doc = "The per-tick diff. Unreliable because a dropped snapshot is superseded by the next "
            .. "one, and sequenced because an old one arriving late is worse than none. **This is the "
            .. "channel `flagsFor`'s missing `ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT` would have "
            .. "broken**, silently, by putting a large snapshot on the reliable-fragmented path -- "
            .. "head-of-line blocking, which is exactly what the mode exists to avoid.",
    },
    {
        Name = "Intent",
        Id = 2,
        Delivery = "UnreliableSequenced",
        Doc = "What a client did, never what happened. The client says it moved and fired; the "
            .. "authority decides whether it moved and whether it hit.",
    },
    {
        Name = "Ownership",
        Id = 3,
        Delivery = "UnreliableSequenced",
        Doc = "Where an owner's parts are, from the owner (ADR 0099, protocol 14). Reserved as channel 3 "
            .. "since protocol 1 so its arrival shifted nothing. Unreliable and sequenced for the reason "
            .. "the State channel is: a lost state is superseded by the next tick's, and an old one "
            .. "arriving late would move a part backwards.",
    },
    {
        Name = "Remote",
        Id = 4,
        Delivery = "UnreliableSequenced",
        Doc = "What a script sends through an `UnreliableRemoteEvent` (ADR 0161, protocol 38): sent "
            .. "once and never again, so a lost one holds nothing up, and sequenced so one that arrives "
            .. "after a later one is dropped. Its own channel: the sequence it shares is only its own, "
            .. "and a game's messages cannot make a snapshot late.",
    },
    {
        Name = "Swarm",
        Id = 5,
        Delivery = "Unreliable",
        Doc = "Where a replicated `Swarm`'s agents are (ADR 0162, protocol 38): each message stands "
            .. "alone, a packet, and is never sent again -- an agent a replica draws wrong is sent again "
            .. "because it is wrong. **Not sequenced**: every agent in a message carries the order with "
            .. "it, in the message's tick, so one that arrives after a later one is still taken for the "
            .. "agents the later one did not name; sequenced, a link whose packets swap places lost a "
            .. "third of them to the rule.",
    },
}

local Messages: { Schema.Message } = {
    {
        Name = "Hello",
        Id = 1,
        Channel = "Control",
        Direction = "ToAuthority",
        Layout = {
            {
                Name = "protocol",
                Type = "u32",
                Doc = "The protocol version the replica speaks. Any other than the authority's is refused.",
            },
            {
                Name = "tokenHigh",
                Type = "u64",
                Doc = "The player token the authority gave on an earlier join, or zero the first time.",
            },
            { Name = "tokenLow", Type = "u64" },
        },
        Doc = "A replica announcing itself: protocol version, then the player token the authority "
            .. "gave it last time, or zeros the first time. **Version first, so a mismatch is refused "
            .. "before a single field is parsed** rather than after a decoder has already read a field "
            .. "id it does not have. The token is how a player who drops and comes back is the same "
            .. "player (ADR 0085).",
    },
    {
        Name = "Welcome",
        Id = 2,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            {
                Name = "protocol",
                Type = "u32",
                Doc = "The authority's version; a replica refuses a Welcome in another.",
            },
            { Name = "userId", Type = "u32", Doc = "The replica's own player number, 2 and up." },
            { Name = "tick", Type = "u64", Doc = "The authority's tick when it welcomed." },
            { Name = "tokenHigh", Type = "u64", Doc = "The high half of the player token (ADR 0085)." },
            { Name = "tokenLow", Type = "u64", Doc = "The low half." },
        },
        Doc = "Accepted: the peer's own `Player` id, the authority's tick, and the player token to "
            .. "send back on a reconnect -- the same id again (ADR 0085). A refusal is a disconnect "
            .. "with a keyed reason rather than a silence.",
    },
    {
        Name = "Spawn",
        Id = 3,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "count", Type = "u32" },
            {
                Name = "instances",
                Type = "repeat",
                Count = "count",
                Doc = "In ascending network id. A service is never spawned.",
                Each = {
                    { Name = "netId", Type = "u32" },
                    {
                        Name = "className",
                        Type = "text",
                        Doc = "Created by name; a class the replica does not have is skipped.",
                    },
                    {
                        Name = "origin",
                        Type = "text",
                        Doc = "Where it was authored (protocol 29, ADR 0138 §6): `scene:` and the tree a scene "
                            .. "read made it in, `stamp:` and the stamp `Instance.stamp` placed it from, or empty for "
                            .. "an instance made at run time. A replica puts the scripts its own package holds for "
                            .. "that place under the instance, since scripts never cross the wire.",
                    },
                    {
                        Name = "originIndex",
                        Type = "u32",
                        Doc = "Its place in `origin`'s tree, in preorder (protocol 29). Zero with no origin.",
                    },
                },
            },
        },
        Doc = "An instance entering interest: its network id, the class to create and where it was "
            .. "authored. Reliable, "
            .. "because an instance that is never created is a replica missing part of the world for "
            .. "good. **Its fields ride the snapshot**, as a whole record against a baseline that "
            .. "does not have it -- one decoding path for new and changed rather than two.",
    },
    {
        Name = "Despawn",
        Id = 4,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "destroyedCount", Type = "u32" },
            {
                Name = "destroyed",
                Type = "repeat",
                Count = "destroyedCount",
                Doc = "Instances the authority destroyed.",
                Each = {
                    { Name = "netId", Type = "u32" },
                },
            },
            { Name = "streamedOutCount", Type = "u32" },
            {
                Name = "streamedOut",
                Type = "repeat",
                Count = "streamedOutCount",
                Doc = "Instances that left this replica's interest and still exist on the authority.",
                Each = {
                    { Name = "netId", Type = "u32" },
                },
            },
        },
        Doc = "Instances a replica no longer has, in two lists: those the authority destroyed, then "
            .. "those that only left this peer's interest (protocol 6). **Leaving interest is the same "
            .. "event as a chunk being evicted**: one a script holds becomes a husk, reparented to nil, "
            .. "and is reported through the same `InstanceStreamedOut` a script already handles -- a "
            .. "second spelling of one thing would mean every game handles both.",
    },
    {
        Name = "Snapshot",
        Id = 5,
        Channel = "State",
        Direction = "ToReplica",
        Layout = {
            {
                Name = "tick",
                Type = "u64",
                Doc = "The authority tick this is the world at. Older than one applied: dropped.",
            },
            {
                Name = "baseTick",
                Type = "u64",
                Doc = "The acknowledged tick this is a diff against, or 0: every record is then whole.",
            },
            { Name = "checksum", Type = "u64", Doc = 'Over the replica\'s state once applied; see "The checksum".' },
            {
                Name = "intentTick",
                Type = "u64",
                Doc = "The `Intent` tick whose step this state holds (protocol 22): what the replica's "
                    .. "prediction for that tick is compared with. Zero before the first.",
            },
            {
                Name = "intentDepth",
                Type = "u8",
                Doc = "How many of this replica's intents the authority holds unapplied (protocol 22), for "
                    .. "its network overlay.",
            },
            {
                Name = "intentStarvations",
                Type = "u32",
                Doc = "How many ticks the authority has found none of this replica's intents to apply "
                    .. "(protocol 22), for its network overlay.",
            },
            {
                Name = "predictedCount",
                Type = "u16",
                Doc = "At most 64; 0 for a replica with no character, or one whose predicted steps write "
                    .. "no attribute.",
            },
            {
                Name = "predicted",
                Type = "repeat",
                Count = "predictedCount",
                Doc = "The replica's own character's predicted attributes (protocol 37, G37): every "
                    .. "attribute a predicted step has written on it, as it stands after the step of "
                    .. "`intentTick`. The replica checks its prediction against them and takes them; the "
                    .. "`Attributes` copy of them is not applied to its own character.",
                Each = {
                    { Name = "name", Type = "text" },
                    { Name = "value", Type = "attribute", Doc = 'See "Attribute values"; Nil for none.' },
                },
            },
            { Name = "atomCount", Type = "u32", Doc = "32 bits from protocol 33: 16 wrapped past 65 535 names." },
            {
                Name = "atoms",
                Type = "repeat",
                Count = "atomCount",
                Doc = "Every name the records mention, in ascending number.",
                Each = {
                    { Name = "atom", Type = "u32", Doc = "The authority's number for a name." },
                    { Name = "text", Type = "text" },
                },
            },
            { Name = "recordCount", Type = "u32" },
            {
                Name = "records",
                Type = "repeat",
                Count = "recordCount",
                Doc = "In ascending network id; an instance with nothing changed has none.",
                Each = {
                    { Name = "netId", Type = "u32" },
                    { Name = "schema", Type = "u8", Doc = 'The class\'s index in "Replicated classes".' },
                    {
                        Name = "flags",
                        Type = "u8",
                        Doc = "Bit 0: a whole record, every field in order. Otherwise only what changed.",
                    },
                    { Name = "fieldCount", Type = "u16" },
                    {
                        Name = "fields",
                        Type = "repeat",
                        Count = "fieldCount",
                        Each = {
                            { Name = "wireId", Type = "u16", Doc = 'See "Wire ids".' },
                            { Name = "value", Type = "value", Doc = "In the encoding of the field the wire id names." },
                        },
                    },
                },
            },
        },
        Doc = "The tick's diff against the peer's last acknowledged baseline, with a checksum over "
            .. "what was applied. **The checksum catches apply bugs and nothing else** -- interest "
            .. "gives a replica a strict subset of the world, so its hash is not the authority's and "
            .. "comparing them would be comparing two different worlds. One larger than 16 KiB is "
            .. "sent as `SnapshotPart`s instead.",
    },
    {
        Name = "Ack",
        Id = 6,
        Channel = "Control",
        Direction = "ToAuthority",
        Layout = {
            {
                Name = "tick",
                Type = "u64",
                Doc = "The snapshot tick the replica now holds, and may be diffed against.",
            },
        },
        Doc = "Which baseline the replica now holds. Reliable, because a lost acknowledgement makes "
            .. "the authority diff against a baseline the replica has moved past.",
    },
    {
        Name = "Intent",
        Id = 7,
        Channel = "Intent",
        Direction = "ToAuthority",
        Layout = {
            {
                Name = "timeEpoch",
                Type = "u32",
                Doc = "How many times the replica has dropped simulated time (protocol 36, D498): it moves "
                    .. "when a frame owed more ticks than it may catch up. An authority anchors a late "
                    .. "stream again only when this moved, or when the stream stays late; a burst held up "
                    .. "in transit is not the replica's clock.",
            },
            {
                Name = "tickCount",
                Type = "u8",
                Doc = "How many ticks this message carries, 1 to 4: the newest and the ones before it "
                    .. "(protocol 22), so one lost message loses no input.",
            },
            {
                Name = "ticks",
                Type = "repeat",
                Count = "tickCount",
                Doc = "Oldest first. A tick already applied or already queued is a copy, and dropped.",
                Each = {
                    { Name = "tick", Type = "u64", Doc = "The replica's tick." },
                    {
                        Name = "count",
                        Type = "u16",
                        Doc = "A button not held is left out (protocol 37): it reads as an action not sent.",
                    },
                    {
                        Name = "intents",
                        Type = "repeat",
                        Count = "count",
                        Each = {
                            {
                                Name = "action",
                                Type = "u16",
                                Doc = "The number an `IntentNames` gave the action's name on this connection "
                                    .. "(protocol 37). One whose name has not arrived waits for it.",
                            },
                            {
                                Name = "type",
                                Type = "u8",
                                Doc = "0 Bool, 1 Direction1D, 2 Direction2D, 3 Direction3D, 4 ViewportPosition.",
                            },
                            { Name = "x", Type = "f32" },
                            { Name = "y", Type = "f32" },
                            { Name = "z", Type = "f32" },
                            { Name = "pressed", Type = "u8" },
                        },
                    },
                },
            },
        },
        Doc = "A client's input, as actions rather than as results. The client says what it did; a "
            .. "client that could assert what happened is a client that always hits. The authority "
            .. "queues it by tick and applies one tick a tick, in order, a few behind the newest "
            .. "(protocol 22).",
    },
    {
        Name = "IntentNames",
        Id = 24,
        Channel = "Control",
        Direction = "ToAuthority",
        Layout = {
            { Name = "count", Type = "u16", Doc = "At most 256." },
            {
                Name = "names",
                Type = "repeat",
                Count = "count",
                Each = {
                    { Name = "id", Type = "u16", Doc = "Below 1024, numbered in the order first used." },
                    { Name = "name", Type = "text", Doc = "The action's name, 1 to 255 bytes." },
                },
            },
        },
        Doc = "The numbers a replica's `Intent`s will carry for its actions' names (protocol 37, G38): "
            .. "sent once a connection, reliably, before the first intent that uses one. A name once "
            .. "given stands for the connection. A name on every intent was most of what a tick of "
            .. "input cost, and an intent written by code may name many actions.",
    },
    {
        Name = "Players",
        Id = 8,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "count", Type = "u32" },
            {
                Name = "players",
                Type = "repeat",
                Count = "count",
                Doc = "In join order.",
                Each = {
                    { Name = "userId", Type = "u32" },
                    { Name = "character", Type = "u32", Doc = "The network id of their `Character`, or 0." },
                },
            },
        },
        Doc = "Who is taking part: every player's number and their character's network id, zero for "
            .. "none -- the team's column went with teams (protocol 35) -- sent whole whenever any of it "
            .. "changes and "
            .. "once on welcome. Whole rather than as joins and leaves, because a roster is a few "
            .. "numbers and a list a replica can only ever replace cannot drift from the authority's.",
    },
    {
        Name = "RemoteToAuthority",
        Id = 9,
        Channel = "Control",
        Direction = "ToAuthority",
        Layout = {
            { Name = "remote", Type = "u32", Doc = "The network id of the `RemoteEvent` or `RemoteFunction`." },
            {
                Name = "call",
                Type = "u32",
                Doc = "0 for an event; otherwise the call a `RemoteFunction` answer belongs to.",
            },
            {
                Name = "flags",
                Type = "u8",
                Doc = "Bit 0: an answer. Bit 1: the answer is an error. Always 0 toward the authority.",
            },
            { Name = "refCount", Type = "u16" },
            {
                Name = "refs",
                Type = "repeat",
                Count = "refCount",
                Doc = "The instances the arguments name, by position.",
                Each = {
                    { Name = "netId", Type = "u32", Doc = "0 for an instance the sender has no id for." },
                },
            },
            { Name = "size", Type = "u32", Doc = "The byte length of what follows, at most 65552." },
            { Name = "arguments", Type = "arguments", Doc = 'See "Arguments".' },
        },
        Doc = "`RemoteEvent:FireServer` (protocol 7): the event's network id, a call number and a "
            .. "flags byte (protocol 8), the network ids of the instances the arguments name, and the "
            .. "arguments as the script module encoded them. A call number that is not zero is "
            .. "`RemoteFunction:InvokeServerAsync` asking, and names a `RemoteFunction`; zero names a "
            .. "`RemoteEvent`. The sender is the connection's player, never anything the message says.",
    },
    {
        Name = "RemoteToReplica",
        Id = 10,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "remote", Type = "u32", Doc = "The network id of the `RemoteEvent` or `RemoteFunction`." },
            {
                Name = "call",
                Type = "u32",
                Doc = "0 for an event; otherwise the call a `RemoteFunction` answer belongs to.",
            },
            {
                Name = "flags",
                Type = "u8",
                Doc = "Bit 0: an answer to `call`. Bit 1: the answer is an error, one string argument.",
            },
            { Name = "refCount", Type = "u16" },
            {
                Name = "refs",
                Type = "repeat",
                Count = "refCount",
                Doc = "The instances the arguments name, by position.",
                Each = {
                    { Name = "netId", Type = "u32", Doc = "0 for an instance the sender has no id for." },
                },
            },
            { Name = "size", Type = "u32", Doc = "The byte length of what follows, at most 65552." },
            { Name = "arguments", Type = "arguments", Doc = 'See "Arguments".' },
        },
        Doc = "`RemoteEvent:FireClient` and `FireAllClients`, laid out as `RemoteToAuthority` is. On "
            .. "the Control channel, after the spawns of the same send, so an event created and fired "
            .. "in one tick is never named before it exists. With the reply flag set it is a "
            .. "`RemoteFunction`'s answer to the call number it carries, and with the failed flag the "
            .. "answer is the error the handler raised.",
    },
    {
        Name = "Ownership",
        Id = 11,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "count", Type = "u32" },
            {
                Name = "parts",
                Type = "repeat",
                Count = "count",
                Doc = "Every part this replica owns, in ascending network id.",
                Each = {
                    { Name = "netId", Type = "u32" },
                },
            },
        },
        Doc = "The network ids of every part this replica owns (ADR 0099, protocol 14), sent whole "
            .. "whenever the list changes. Whole, as the roster is: a list the replica can only replace "
            .. "cannot drift from the authority's. Reliable and after the spawns, so it never names a "
            .. "part the replica has not been sent.",
    },
    {
        Name = "OwnedState",
        Id = 12,
        Channel = "Ownership",
        Direction = "ToAuthority",
        Layout = {
            { Name = "tick", Type = "u64", Doc = "The owner's tick. Not newer than the last one taken: dropped." },
            { Name = "count", Type = "u16" },
            {
                Name = "parts",
                Type = "repeat",
                Count = "count",
                Each = {
                    { Name = "netId", Type = "u32", Doc = "A part the sender owns; any other is skipped." },
                    { Name = "cframe", Type = "CFrameD" },
                    { Name = "linearVelocity", Type = "Vector3", Doc = "Metres per second." },
                    { Name = "angularVelocity", Type = "Vector3", Doc = "Radians per second." },
                },
            },
        },
        Doc = "One tick of the parts a replica owns: for each, its network id, its transform and its "
            .. "linear and angular velocity, as the owner's solver left them. The authority takes a state "
            .. "only for a part the sender owns at the moment it arrives, and moves the part there.",
    },
    {
        Name = "TilemapBlocks",
        Id = 13,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "netId", Type = "u32", Doc = "The `Tilemap2D`. One the replica does not have is skipped." },
            { Name = "count", Type = "u16", Doc = "At most 64 blocks a message; a larger map takes several." },
            {
                Name = "blocks",
                Type = "repeat",
                Count = "count",
                Each = {
                    {
                        Name = "x",
                        Type = "u32",
                        Doc = "The block's column, two's complement: a block may be left of the origin.",
                    },
                    { Name = "y", Type = "u32", Doc = "Its row, the same way." },
                    {
                        Name = "cellCount",
                        Type = "u16",
                        Doc = "Always 256, 16 by 16; a block of any other size is refused.",
                    },
                    {
                        Name = "cells",
                        Type = "repeat",
                        Count = "cellCount",
                        Doc = "Row after row from the block's bottom left.",
                        Each = {
                            { Name = "tile", Type = "u16", Doc = "The tile, 0 for empty." },
                        },
                    },
                },
            },
        },
        Doc = "A tilemap's cells, a block of 16 by 16 at a time (ADR 0103, protocol 15). Every block it "
            .. "holds after its spawn, then every block an edit changed, added or emptied. **A block of all "
            .. "zeros is an empty one**, and the replica drops it. Whole blocks, so a block received twice is "
            .. "simply the truth; reliable and after the spawns, so it never names a tilemap the replica has "
            .. "not been sent.",
    },
    {
        Name = "SceneChange",
        Id = 14,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "path", Type = "text", Doc = "The scene, content-relative: `scenes/arena.scene.json`." },
            { Name = "dataLength", Type = "u32" },
            {
                Name = "data",
                Type = "repeat",
                Count = "dataLength",
                Doc = "What `LoadScene` handed the new scene, encoded as one `RemoteEvent` argument is.",
                Each = {
                    { Name = "byte", Type = "u8" },
                },
            },
            {
                Name = "load",
                Type = "u32",
                Doc = "How many scenes the authority has loaded (protocol 35, G18). A replica already in "
                    .. "`path` loads it again when this moved: the next match in the same scene is a scene change, "
                    .. "and was taken for the join's.",
            },
        },
        Doc = "The scene the authority is in (ADR 0106, protocol 16). Sent when it changes, and once to "
            .. "every replica that joins. The replica loads the same scene from its own package, without "
            .. "dropping the connection, and the new world then arrives by replication as it does at a "
            .. "join. Reliable and before the spawns of the same send, so the new scene's instances arrive "
            .. "into it rather than into the old one.",
    },
    {
        Name = "Attributes",
        Id = 15,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            {
                Name = "owner",
                Type = "u8",
                Doc = "Whose they are: 0 an instance by network id, 1 a `Player` by user id, 2 "
                    .. "`GlobalScriptService`, 3 its `Shared` folder.",
            },
            { Name = "id", Type = "u32", Doc = "The network id or the user id; 0 for owners 2 and 3." },
            { Name = "count", Type = "u16" },
            {
                Name = "attributes",
                Type = "repeat",
                Count = "count",
                Each = {
                    { Name = "name", Type = "text" },
                    { Name = "value", Type = "attribute", Doc = 'See "Attribute values".' },
                },
            },
            {
                Name = "tagCount",
                Type = "u16",
                Doc = "Its tags (protocol 36, G30), in name order, at most 1024.",
            },
            { Name = "tags", Type = "repeat", Count = "tagCount", Each = { { Name = "tag", Type = "text" } } },
        },
        Doc = "Every attribute and every tag one owner has now (ADR 0106, protocol 17; tags since 36), sent when any of them changed and "
            .. "once to a replica that is sent the owner. **Whole, not a diff**: the replica makes the owner's "
            .. "attributes exactly these, so a message received twice is simply the truth, and an attribute "
            .. "missing from it is gone. Reliable and after the spawns, so it never names an instance the "
            .. "replica has not been sent.",
    },
    {
        Name = "DetectorInput",
        Id = 16,
        Channel = "Control",
        Direction = "ToAuthority",
        Layout = {
            {
                Name = "detector",
                Type = "u32",
                Doc = "The network id of the `ClickDetector`.",
            },
            {
                Name = "kind",
                Type = "u8",
                Doc = "0 a click, 1 a right click (a prompt's kinds went with prompts, protocol 35).",
            },
        },
        Doc = "A player pressed something (ADR 0126, protocol 21). The replica fired it already, for its own "
            .. "scripts; the authority checks it -- the detector is live and enabled, and within reach of the "
            .. "sender's character -- and fires it there with the connection's player, never one the message "
            .. "names. Counted against the same per-tick limit as `RemoteToAuthority`.",
    },
    {
        Name = "TerrainChunks",
        Id = 17,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            {
                Name = "voxelSize",
                Type = "f32",
                Doc = "The field's voxel, metres. A replica whose terrain has another refuses the message.",
            },
            { Name = "minHeight", Type = "f32", Doc = "The world's floor, for a terrain the replica makes." },
            { Name = "maxHeight", Type = "f32", Doc = "And its ceiling." },
            { Name = "count", Type = "u16", Doc = "At most 64 chunks a message." },
            {
                Name = "chunks",
                Type = "repeat",
                Count = "count",
                Each = {
                    {
                        Name = "x",
                        Type = "u32",
                        Doc = "The chunk's key, two's complement, within a million of the origin.",
                    },
                    { Name = "y", Type = "u32" },
                    { Name = "z", Type = "u32" },
                    {
                        Name = "codeLength",
                        Type = "u32",
                        Doc = "0 is a chunk that is gone; at most 262144, its voxels and then its paint (protocol 27).",
                    },
                    {
                        Name = "code",
                        Type = "repeat",
                        Count = "codeLength",
                        Doc = "The chunk's voxels as a terrain cell codes them: runs of a u16 packed voxel and a "
                            .. "u16 count, covering the chunk's 32768 voxels in row order.",
                        Each = {
                            { Name = "byte", Type = "u8" },
                        },
                    },
                },
            },
        },
        Doc = "The workspace's terrain, a chunk at a time (ADR 0135, protocol 24). Every chunk that differs "
            .. "from the scene both ends loaded, to a replica that joins; then every chunk an edit changed. "
            .. "Whole chunks, so a chunk received twice is simply the truth; a replica with no terrain makes "
            .. "one. Reliable and after the spawns of the same send.",
    },
    {
        Name = "TerrainLook",
        Id = 18,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "originX", Type = "u64", Doc = "The terrain's `Position`, an f64's bits (protocol 25)." },
            { Name = "originY", Type = "u64" },
            { Name = "originZ", Type = "u64" },
            { Name = "layerCount", Type = "u16", Doc = "At most 255." },
            {
                Name = "layers",
                Type = "repeat",
                Count = "layerCount",
                Each = {
                    { Name = "urn", Type = "text", Doc = "The material of that id." },
                },
            },
            { Name = "ruleCount", Type = "u16", Doc = "At most 16." },
            {
                Name = "rules",
                Type = "repeat",
                Count = "ruleCount",
                Each = {
                    { Name = "enabled", Type = "u8" },
                    { Name = "material", Type = "u8" },
                    { Name = "slopeMin", Type = "f32" },
                    { Name = "slopeMax", Type = "f32" },
                    { Name = "heightMin", Type = "f32" },
                    { Name = "heightMax", Type = "f32" },
                    { Name = "blend", Type = "f32" },
                    { Name = "noise", Type = "f32" },
                    { Name = "appliesCount", Type = "u16", Doc = "At most 255." },
                    {
                        Name = "appliesTo",
                        Type = "repeat",
                        Count = "appliesCount",
                        Each = {
                            { Name = "layer", Type = "u8" },
                        },
                    },
                },
            },
        },
        Doc = "The workspace's terrain's place, layers and rules, whole (ADR 0135, protocol 24; its place since "
            .. "25): once to a replica that joins, and whenever any changed. The replica's become exactly these, "
            .. "and a replica with no terrain makes one.",
    },
    {
        Name = "VoxelChunks",
        Id = 19,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "blockSize", Type = "f32", Doc = "The block world's block, metres." },
            { Name = "count", Type = "u16", Doc = "At most 64 chunks a message." },
            {
                Name = "chunks",
                Type = "repeat",
                Count = "count",
                Each = {
                    {
                        Name = "x",
                        Type = "u32",
                        Doc = "The chunk's key, two's complement, within a million of the origin.",
                    },
                    { Name = "y", Type = "u32" },
                    { Name = "z", Type = "u32" },
                    { Name = "codeLength", Type = "u32", Doc = "0 is a chunk that is gone; at most 16384." },
                    {
                        Name = "code",
                        Type = "repeat",
                        Count = "codeLength",
                        Doc = "The chunk's blocks as a scene codes them: runs of a u16 block and a u16 count.",
                        Each = {
                            { Name = "byte", Type = "u8" },
                        },
                    },
                },
            },
        },
        Doc = "The block world's chunks (ADR 0135, protocol 24), on `TerrainChunks`' terms: every chunk that "
            .. "differs from the scene to a replica that joins, then every chunk that changed.",
    },
    {
        Name = "VoxelTypes",
        Id = 20,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "count", Type = "u16", Doc = "At most 65535; type `n` is block id `n`." },
            {
                Name = "types",
                Type = "repeat",
                Count = "count",
                Each = {
                    { Name = "name", Type = "text" },
                    { Name = "color", Type = "Color3", Doc = "The top, and every face of a one-colour block." },
                    { Name = "side", Type = "Color3" },
                    { Name = "bottom", Type = "Color3" },
                    { Name = "texture", Type = "text", Doc = "An image by content URN; empty for none." },
                    { Name = "sideTexture", Type = "text" },
                    { Name = "bottomTexture", Type = "text" },
                    { Name = "opacity", Type = "u8", Doc = "0 opaque, 1 cutout, 2 translucent." },
                    { Name = "transparency", Type = "f32" },
                    { Name = "fluidReach", Type = "u8", Doc = "0 for a block that is not a fluid." },
                    { Name = "fluidTicks", Type = "u32" },
                },
            },
            { Name = "reactionCount", Type = "u16" },
            {
                Name = "reactions",
                Type = "repeat",
                Count = "reactionCount",
                Each = {
                    { Name = "from", Type = "u16" },
                    { Name = "touching", Type = "u16" },
                    { Name = "result", Type = "u16" },
                },
            },
        },
        Doc = "The block world's types and fluid reactions, whole (ADR 0135, protocol 24): once to a replica "
            .. "that joins, and whenever they changed.",
    },
    {
        Name = "SnapshotPart",
        Id = 22,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "tick", Type = "u64", Doc = "The snapshot's tick." },
            { Name = "index", Type = "u16", Doc = "This part's place, from 0." },
            { Name = "count", Type = "u16", Doc = "How many parts the snapshot is in." },
            { Name = "byteCount", Type = "u32" },
            {
                Name = "bytes",
                Type = "repeat",
                Count = "byteCount",
                Each = { { Name = "byte", Type = "u8" } },
                Doc = "The next piece of a `Snapshot` message, from its type byte on.",
            },
        },
        Doc = "A piece of a `Snapshot` too large to go as one unreliable message (protocol 33, NA1): "
            .. "a world's first, every record whole, or one an instance-heavy tick made. Reliable and in "
            .. "order behind the spawns it names; the replica joins the pieces and applies them as the "
            .. "snapshot they were. The authority sends that peer no other snapshot until it is "
            .. "acknowledged.",
    },
    {
        Name = "Refused",
        Id = 23,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            {
                Name = "reason",
                Type = "u8",
                Doc = "1: the replica speaks another protocol. 2: the server is full.",
            },
        },
        Doc = "Why the authority will not take this replica (protocol 33, NA8), sent before it lets the "
            .. "connection go. Before it, a full server or one of another version answered nothing, and "
            .. "the player waited out the join's timeout with no reason.",
    },
    {
        Name = "CollisionGroups",
        Id = 30,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "count", Type = "u16", Doc = "At most 1024; `Default` is among them." },
            {
                Name = "groups",
                Type = "repeat",
                Count = "count",
                Each = {
                    { Name = "name", Type = "text" },
                },
            },
            { Name = "apartCount", Type = "u32" },
            {
                Name = "apart",
                Type = "repeat",
                Count = "apartCount",
                Each = {
                    { Name = "a", Type = "u16", Doc = "A group, by its place in `groups`." },
                    { Name = "b", Type = "u16", Doc = "Another, or the same: `a` at most `b`." },
                },
            },
        },
        Doc = "The authority's collision groups and the pairs of them that do not collide (protocol 38, "
            .. "D545), whole: once to a replica that joins, when a game has registered any, and whenever the "
            .. "table changed. A replica registers the names it has not and takes every pair among them "
            .. "from here -- collidable unless listed -- and keeps its own word on groups the authority "
            .. "never named. By name, never by number: each machine numbers its groups as it met them.",
    },
    {
        Name = "UnreliableToAuthority",
        Id = 25,
        Channel = "Remote",
        Direction = "ToAuthority",
        Layout = {
            {
                Name = "sequence",
                Type = "u16",
                Doc = "Counts up by one for every unreliable message this connection sends, and wraps. "
                    .. "One that is not newer than the last taken is dropped.",
            },
            { Name = "remote", Type = "u32", Doc = "The network id of the `UnreliableRemoteEvent`." },
            { Name = "refCount", Type = "u16" },
            {
                Name = "refs",
                Type = "repeat",
                Count = "refCount",
                Doc = "The instances the arguments name, by position.",
                Each = {
                    { Name = "netId", Type = "u32", Doc = "0 for an instance the sender has no id for." },
                },
            },
            { Name = "size", Type = "u32", Doc = "The byte length of what follows, at most 16384." },
            { Name = "arguments", Type = "arguments", Doc = 'See "Arguments".' },
        },
        Doc = "`UnreliableRemoteEvent:FireServer` (protocol 38, ADR 0161): a message sent once. The "
            .. "sender is the connection's player, never anything the message says, and it is taken "
            .. "from the same budgets a `RemoteToAuthority` is.",
    },
    {
        Name = "UnreliableToReplica",
        Id = 26,
        Channel = "Remote",
        Direction = "ToReplica",
        Layout = {
            {
                Name = "sequence",
                Type = "u16",
                Doc = "Counts up by one for every unreliable message the authority sends this replica, "
                    .. "and wraps. One that is not newer than the last taken is dropped.",
            },
            { Name = "remote", Type = "u32", Doc = "The network id of the `UnreliableRemoteEvent`." },
            { Name = "refCount", Type = "u16" },
            {
                Name = "refs",
                Type = "repeat",
                Count = "refCount",
                Doc = "The instances the arguments name, by position.",
                Each = {
                    { Name = "netId", Type = "u32", Doc = "0 for an instance the sender has no id for." },
                },
            },
            { Name = "size", Type = "u32", Doc = "The byte length of what follows, at most 16384." },
            { Name = "arguments", Type = "arguments", Doc = 'See "Arguments".' },
        },
        Doc = "`UnreliableRemoteEvent:FireClient` and `FireAllClients`, laid out as "
            .. "`UnreliableToAuthority` is. Never held: a replica that does not have the event yet is "
            .. "sent nothing.",
    },
    {
        Name = "SwarmAgents",
        Id = 27,
        Channel = "Control",
        Direction = "ToReplica",
        Layout = {
            { Name = "swarm", Type = "u32", Doc = "The network id of the `Swarm`." },
            { Name = "tick", Type = "u32", Doc = "The authority's tick this is true at, its low 32 bits." },
            {
                Name = "originX",
                Type = "u32",
                Doc = "Where every position below is measured from, in whole metres, as a signed number: "
                    .. "a point on a 16 m lattice near the replica's focus.",
            },
            { Name = "originY", Type = "u32" },
            { Name = "originZ", Type = "u32" },
            {
                Name = "membership",
                Type = "u16",
                Doc = "How many `SwarmAgents` messages this replica has been sent for this swarm, this one "
                    .. "counted. The replica says the last it took in (`SwarmAck`), and the authority sends "
                    .. "the position of an agent only once the message that brought it was taken in: a "
                    .. "number is used again, and a position must not reach the agent that had it before.",
            },
            {
                Name = "floor",
                Type = "f32",
                Doc = "The height, from the origin, that a lift is measured from where no terrain covers "
                    .. "the agent. Where one does, it is measured from the terrain's top there.",
            },
            { Name = "removedCount", Type = "u16" },
            {
                Name = "removed",
                Type = "repeat",
                Count = "removedCount",
                Doc = "The agents this replica no longer has, before the ones it gains: a number is used "
                    .. "again only after its agent went.",
                Each = {
                    { Name = "slot", Type = "u16", Doc = "The agent's number, less one." },
                    { Name = "reason", Type = "u8", Doc = "`Enum.SwarmAgentRemoval`: 0 removed, 1 out of reach." },
                    { Name = "tag", Type = "u16", Doc = "The tag it had last." },
                    { Name = "x", Type = "f32", Doc = "Where it was last, from the origin." },
                    { Name = "y", Type = "f32" },
                    { Name = "z", Type = "f32" },
                },
            },
            { Name = "addedCount", Type = "u16" },
            {
                Name = "added",
                Type = "repeat",
                Count = "addedCount",
                Each = {
                    { Name = "slot", Type = "u16", Doc = "The agent's number, less one." },
                    { Name = "tag", Type = "u16" },
                    { Name = "radius", Type = "u8", Doc = "In sixteenths of a metre." },
                    { Name = "height", Type = "u8", Doc = "In eighths of a metre." },
                    { Name = "x", Type = "f32", Doc = "Where it is, from the origin." },
                    { Name = "y", Type = "f32" },
                    { Name = "z", Type = "f32" },
                    { Name = "lift", Type = "f32", Doc = "How far above its ground: the terrain, or the floor." },
                    { Name = "yaw", Type = "u8", Doc = "The way it faces, a 256th of a turn." },
                    {
                        Name = "walk",
                        Type = "u8",
                        Doc = "How fast it walks that way, in quarters of a metre a second.",
                    },
                },
            },
        },
        Doc = "The agents of a replicated `Swarm` entering and leaving what one replica is sent "
            .. "(protocol 38, ADR 0162): reliable, in order, and ahead of the game's own reliable messages "
            .. "of the tick, so a number names one agent from its arrival to its removal on every "
            .. "machine. A removal carries the agent's last tag and place.",
    },
    {
        Name = "SwarmState",
        Id = 28,
        Channel = "Swarm",
        Direction = "ToReplica",
        Layout = {
            { Name = "swarm", Type = "u32", Doc = "The network id of the `Swarm`." },
            { Name = "tick", Type = "u32", Doc = "The authority's tick this is true at, its low 32 bits." },
            {
                Name = "sequence",
                Type = "u16",
                Doc = "This message's number for the replica, counted up and wrapping: what a `SwarmAck` " .. "names.",
            },
            { Name = "originX", Type = "u32", Doc = "As `SwarmAgents`'s." },
            { Name = "originY", Type = "u32" },
            { Name = "originZ", Type = "u32" },
            { Name = "floor", Type = "f32", Doc = "As `SwarmAgents`'s." },
            { Name = "byteCount", Type = "u16" },
            {
                Name = "agents",
                Type = "repeat",
                Count = "byteCount",
                Each = { { Name = "byte", Type = "u8" } },
                Doc = "The agents, in rising number. Each: the step from the number before it (from -1 "
                    .. "for the first) as a varint of seven bits a byte, low first; three bytes of x and z, "
                    .. "twelve bits each, in eighths of a metre from the origin less 256 m; a byte of yaw, a "
                    .. "256th of a turn; a byte whose low six bits are the walk in quarters of a metre a "
                    .. "second, bit 6 says a lift follows and bit 7 a tag. The lift is a byte of eighths of "
                    .. "a metre above its ground -- the terrain, or the floor -- or 255 and then a u16 of "
                    .. "them; the tag is a u16.",
            },
        },
        Doc = "Where some agents of a replicated `Swarm` are (protocol 38, ADR 0162), at most 1100 bytes "
            .. "so it is one packet. An agent is in it when the replica's drawing of it would otherwise "
            .. "be wrong by more than a threshold, when its tag changed, or when it has not been sent for "
            .. "a while. A replica ignores one older than what it holds for that agent, and says which "
            .. "messages arrived (`SwarmAck`): the agents of one that did not are sent again.",
    },
    {
        Name = "SwarmAck",
        Id = 29,
        Channel = "Swarm",
        Direction = "ToAuthority",
        Layout = {
            { Name = "count", Type = "u8" },
            {
                Name = "sequences",
                Type = "repeat",
                Count = "count",
                Doc = "The `SwarmState` messages this replica took in, newest last: each is said in "
                    .. "several of these, since one of them may be lost too.",
                Each = { { Name = "sequence", Type = "u16" } },
            },
            { Name = "swarmCount", Type = "u8" },
            {
                Name = "swarms",
                Type = "repeat",
                Count = "swarmCount",
                Doc = "For each swarm this replica mirrors, the last `SwarmAgents` it took in.",
                Each = {
                    { Name = "swarm", Type = "u32", Doc = "The network id of the `Swarm`." },
                    { Name = "membership", Type = "u16" },
                },
            },
        },
        Doc = "Which `SwarmState` messages arrived (protocol 38, ADR 0162). The authority sends an agent "
            .. "when a replica would draw it wrong, and takes what it sent as what the replica has; a "
            .. "message nobody acknowledges within the round trip was lost, and its agents are told "
            .. "again at once rather than when they are next due.",
    },
}

local Definition: Schema.Definition = {
    Common = Common,
    Classes = Classes,
    Excluded = Excluded,
    ExcludedByTag = ExcludedByTag,
    Channels = Channels,
    Messages = Messages,
}

return Definition
