--!strict
--- Instance classes (api-design.md §2.2).
---
--- The kernel's surface only: `Instance` and the classes a script can build a tree
--- out of. Classes whose behaviour belongs to a later milestone arrive with it,
--- because a property that accepts a write and changes nothing is worse than a
--- missing one -- it type-checks.

local Schema = require("../schema")

--- The attribute value domain (§2.2). Written once because it widens as the
--- remaining datatypes ship, and three copies would widen at three different
--- times. It widened once, with the input and UI work, when `Vector2`, `UDim`, `UDim2` and `Rect`
--- were declared -- which is the whole of what §2.2's "as those datatypes ship"
--- was waiting for.
local AttributeValue =
    "string | number | boolean | vector | CFrame | Color3 | Vector2 | UDim | UDim2 | Rect | ColorSequence | NumberSequence"
local OptionalAttributeValue = "(" .. AttributeValue .. ")?"
local AttributeMap = "{ [string]: " .. AttributeValue .. " }"

--- Where a class does its job (`Schema.Class.Parents`), in the groups most of
--- them share. The two storages are in every one: a template waiting in
--- `ReplicatedStorage` to be cloned is exactly where anything may sit.
local function under(...: string): { string }
    local places = { ... }
    table.insert(places, "ReplicatedStorage")
    table.insert(places, "ServerStorage")
    return places
end
--- Something with a place in the 3D world: the world, a model, or a part
--- (a part inside a part moves with it).
local InTheWorld = under("Workspace", "Model", "BasePart")
--- Something a `ViewportFrame` can show as well as the world can (ADR 0107):
--- a part, a model, and the camera that frames them. Inside a frame it is
--- drawn there and nowhere else, and nothing simulates it.
local InTheWorldOrAFrame = under("Workspace", "Model", "BasePart", "ViewportFrame")
--- Something a part or a point on one gives off or wears.
local OnAPart = under("BasePart", "Attachment")
--- A canvas that floats over something in the world: a part, a point on one,
--- or a part on the 2D plane.
local OverAPart = under("BasePart", "Attachment", "Part2D")
--- An element of a UI tree, under one of its three roots or another element.
local InAUi = under("ScreenGui", "BillboardGui", "SurfaceGui", "UIObject")

--- The list carries its own annotation so that `Schema.Class` is the expected
--- type of every entry. Written inline under `Definition`, the new solver infers
--- the element type from the first class and then rejects every later one that
--- omits an optional field -- which is most of them.
local Classes: { Schema.Class } = {
    {
        Name = "Instance",
        Module = "scene",
        Tags = { "Abstract", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "The base class of every object in the tree. An instance has a name, a parent, children, attributes "
            .. "and tags.",
        Properties = {
            {
                Name = "Name",
                Type = "string",
                Doc = "The name of this instance. Siblings can share a name. Renaming an instance to a name that "
                    .. "a `WaitForChild` call is waiting for resumes that call, as parenting a new child would.",
            },
            {
                Name = "Parent",
                Type = "Instance?",
                Doc = "The instance this one is a child of, or nil when it has no parent. Setting it adds this "
                    .. "instance after its new siblings. Setting it after `Destroy` raises an error.",
            },
            {
                Name = "ClassName",
                Type = "string",
                ReadOnly = true,
                Doc = "The name of this instance's class. `typeof` returns `Instance` for every class, so use "
                    .. "`ClassName` or `IsA` to tell classes apart.",
            },
            {
                Name = "Local",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                -- Who made it, which no operation of the world's produced: two
                -- machines in one match answer differently for their own.
                HostFact = true,
                Records = { "ADR 0186" },
                Doc = "Whether this instance exists only on this machine and is never sent to another. It is true "
                    .. "for an instance that a client-side script made with `Instance.new` or `Clone`, and for "
                    .. "everything under it. A client-side script is one under `ClientScriptService`, in the "
                    .. "client half of `GlobalScriptService`, or with a `RunContext` other than `Server`; this "
                    .. "applies on a host as well as on a machine that joined.\n\n"
                    .. "Instances made by server-side scripts or loaded from a scene are not local, and "
                    .. "replicate. The value records who made the instance, not who writes to it later: a "
                    .. "client-side script on a host that sets a property of a replicated instance changes it for "
                    .. "every player.",
            },
        },
        Methods = {
            {
                Name = "FindFirstChild",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "Instance?" },
                Doc = "Returns the first child with the given name, or nil. Only direct children are searched; to "
                    .. "search deeper, loop over `GetDescendants`.",
            },
            {
                Name = "FindFirstChildOfClass",
                Parameters = { { Name = "className", Type = "string" } },
                Returns = { "Instance?" },
                Doc = "Returns the first child whose `ClassName` is exactly `className`, or nil. Subclasses do "
                    .. "not match: asking for `BasePart` never finds a `Part`.",
            },
            {
                Name = "FindFirstChildWhichIsA",
                Parameters = { { Name = "className", Type = "string" } },
                Returns = { "Instance?" },
                Doc = "Returns the first child that is of the class `className` or of a class derived from it, or "
                    .. "nil. Abstract base classes such as `BasePart` are accepted.",
            },
            {
                Name = "FindFirstAncestor",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "Instance?" },
                Doc = "Returns the nearest ancestor with the given name, or nil. The search starts at `Parent`; "
                    .. "the instance itself is never returned.",
            },
            {
                Name = "FindFirstAncestorOfClass",
                Parameters = { { Name = "className", Type = "string" } },
                Returns = { "Instance?" },
                Doc = "Returns the nearest ancestor whose `ClassName` is exactly `className`, or nil. The search "
                    .. "starts at `Parent`; the instance itself is never returned.",
            },
            {
                Name = "GetFullName",
                Parameters = {},
                Returns = { "string" },
                Doc = "Returns the path to this instance: the names of its ancestors and its own name, joined by "
                    .. "dots, such as `Workspace.Tree.Trunk`. The path starts below the data model. It is meant "
                    .. "for logs; siblings can share a name, so a path does not always identify one instance.",
            },
            {
                Name = "GetChildren",
                Parameters = {},
                Returns = { "{Instance}" },
                Doc = "Returns a new array of this instance's children, in the order they were parented. The "
                    .. "array is a copy, so it is safe to destroy children while looping over it.",
            },
            {
                Name = "GetDescendants",
                Parameters = {},
                Returns = { "{Instance}" },
                Doc = "Returns a new array of every descendant, depth first: each child, followed by that child's "
                    .. "own descendants.",
            },
            -- Parks the caller without the `Async` suffix §9 otherwise
            -- requires. The rule now carries a `Wait*` exception rather than
            -- the name carrying a suffix: `Async` exists to warn that a call
            -- parks when its name does not say so, and this one says so.
            {
                Name = "WaitForChild",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "timeout", Type = "number?", Default = "nil" },
                },
                Returns = { "Instance?" },
                Yields = true,
                Doc = "Waits until a child with the given name exists, and returns it. Returns immediately if one "
                    .. "already exists. With `timeout`, returns nil when the time runs out. Without one, it waits "
                    .. "indefinitely and logs a warning after five seconds of game time.",
            },
            {
                Name = "IsA",
                Parameters = { { Name = "className", Type = "string" } },
                Returns = { "boolean" },
                Doc = "Returns whether this instance is of the given class or of a class derived from it. A name "
                    .. "that is not a class returns false instead of raising an error.",
            },
            {
                Name = "IsAncestorOf",
                Parameters = { { Name = "descendant", Type = "Instance" } },
                Returns = { "boolean" },
                Doc = "Returns whether `descendant` is anywhere below this instance. An instance is not its own "
                    .. "ancestor. Returns false if either instance has been destroyed.",
            },
            {
                Name = "IsDescendantOf",
                Parameters = { { Name = "ancestor", Type = "Instance" } },
                Returns = { "boolean" },
                Doc = "Returns whether this instance is anywhere below `ancestor`. An instance is not its own "
                    .. "descendant. Returns false if either instance has been destroyed.",
            },
            {
                Name = "GetStamp",
                Parameters = {},
                Returns = { "string?" },
                Records = { "ADR 0155" },
                Doc = "Returns the content path of the stamp this instance was placed from, such as "
                    .. "`stamps/fighter.stamp.json`, or nil. Only a linked copy of a stamp returns a path; a copy "
                    .. "placed unlinked returns nil. A copy's parameters are stored as its attributes.",
            },
            {
                Name = "Clone",
                Parameters = {},
                Returns = { "Instance" },
                Doc = "Returns a copy of this instance and everything under it, with no parent. Properties, "
                    .. "attributes and tags are copied at every depth. A reference to an instance inside the "
                    .. "copied tree points to the copy; a reference to anything outside it still points to the "
                    .. "original.",
            },
            {
                Name = "Destroy",
                Parameters = {},
                Returns = {},
                Doc = "Removes this instance and all of its descendants permanently. `Parent` is set to nil and "
                    .. "locked, tags are cleared, and `Destroying` fires. The instance remains readable until the "
                    .. "`Destroying` handlers have run. After that, `Parent` reads nil and `IsDescendantOf` and "
                    .. "`IsAncestorOf` return false, but reading `Name` or `ClassName` or calling `IsA` raises an "
                    .. "error.",
            },
            {
                Name = "GetAttribute",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { OptionalAttributeValue },
                Doc = "Returns the value of the named attribute, or nil if it has never been set. Attribute names "
                    .. "are case-sensitive.",
            },
            {
                Name = "SetAttribute",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "value", Type = OptionalAttributeValue },
                },
                Returns = {},
                Doc = "Sets the named attribute, or removes it when `value` is nil. Raises an error for a table, "
                    .. "an instance or a function, and for an empty name; the previous value is kept.",
            },
            {
                Name = "GetAttributes",
                Parameters = {},
                Returns = { AttributeMap },
                Doc = "Returns a new table of this instance's attributes, keyed by name.",
            },
            {
                Name = "GetAttributeChangedSignal",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "Signal<()>" },
                Doc = "Returns a signal that fires when the named attribute changes. The attribute does not have "
                    .. "to exist yet.",
            },
            {
                Name = "GetPropertyChangedSignal",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "Signal<()>" },
                Doc = "Returns a signal that fires when the named property changes. The signal passes no "
                    .. "arguments; read the property for its new value. Each call returns the same signal. Raises "
                    .. "an error if the class has no such property.",
            },
            {
                Name = "AddTag",
                Parameters = { { Name = "tag", Type = "string" } },
                Returns = {},
                Doc = "Adds a tag to this instance. Does nothing if the instance already has it. Tags do not "
                    .. "depend on the tree: an instance with no parent keeps its tags and is still found by them.",
            },
            {
                Name = "RemoveTag",
                Parameters = { { Name = "tag", Type = "string" } },
                Returns = {},
                Doc = "Removes a tag from this instance. Does nothing if the instance does not have it.",
            },
            {
                Name = "HasTag",
                Parameters = { { Name = "tag", Type = "string" } },
                Returns = { "boolean" },
                Doc = "Returns whether this instance has the given tag.",
            },
            {
                Name = "GetTags",
                Parameters = {},
                Returns = { "{string}" },
                Doc = "Returns a new array of this instance's tags.",
            },
        },
        Events = {
            {
                Name = "ChildAdded",
                Parameters = { { Name = "child", Type = "Instance" } },
                Doc = "Fires after an instance becomes a child of this one. Passes the child.",
            },
            {
                Name = "ChildRemoved",
                Parameters = { { Name = "child", Type = "Instance" } },
                Doc = "Fires after an instance stops being a child of this one. Passes the child.",
            },
            {
                Name = "DescendantAdded",
                Parameters = { { Name = "descendant", Type = "Instance" } },
                Doc = "Fires after an instance is added anywhere below this one. Passes the descendant.",
            },
            {
                Name = "DescendantRemoving",
                Parameters = { { Name = "descendant", Type = "Instance" } },
                Doc = "Fires when an instance is removed from anywhere below this one. Passes the descendant. "
                    .. "When an instance moves from one parent to another, the removal fires before the addition.",
            },
            {
                Name = "AncestryChanged",
                Parameters = {
                    { Name = "instance", Type = "Instance" },
                    { Name = "newParent", Type = "Instance?" },
                },
                Doc = "Fires when this instance or one of its ancestors is given a new parent. Passes the "
                    .. "instance that moved and its new parent, which is nil if it was unparented. It fires on "
                    .. "the moved instance first, then on each of its descendants.",
            },
            {
                Name = "AttributeChanged",
                Parameters = { { Name = "name", Type = "string" } },
                Doc = "Fires when any attribute of this instance changes. Passes the attribute's name.",
            },
            {
                Name = "Destroying",
                Parameters = {},
                Doc = "Fires once when `Destroy` is called on this instance. Handlers run after the instance has "
                    .. "already been removed from the tree.",
            },
        },
    },
    {
        Name = "PVInstance",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Abstract", "NotCreatable" } :: { Schema.ClassTag },
        Storage = "PV",
        Doc = "The base class of everything that has a position and an orientation in the world: `BasePart`, "
            .. "`Model` and `Camera`. It adds a pivot, the point the object is moved and rotated about.",
        Properties = {
            {
                Name = "PivotOffset",
                Type = "CFrame",
                Doc = "Where the pivot is, relative to the object. The default, the identity, puts it at the "
                    .. "object's center. Set it to a hinge edge and `PivotTo` rotates the object about that edge.",
            },
        },
        Methods = {
            {
                Name = "GetPivot",
                Parameters = {},
                Returns = { "CFrame" },
                Doc = "Returns the object's pivot in world space.",
            },
            {
                Name = "PivotTo",
                Parameters = { { Name = "target", Type = "CFrame" } },
                Returns = {},
                Doc = "Moves the object so that its pivot is at `target`. A `Model` moves every part under it and "
                    .. "keeps their arrangement; a `BasePart` or a `Camera` moves itself.",
            },
        },
    },
    {
        Name = "Folder",
        Extends = "Instance",
        Module = "scene",
        Doc = "A container for grouping instances. It has no behavior of its own. Each subdirectory of a script "
            .. "folder under `src/` becomes a `Folder` when the project loads.",
    },
    {
        Name = "Model",
        Extends = "PVInstance",
        Module = "scene",
        Parents = InTheWorldOrAFrame,
        Storage = "Model",
        Doc = "A group of parts that is moved, scaled and measured as one object.",
        Properties = {
            {
                Name = "PrimaryPart",
                Type = "BasePart?",
                Doc = "The part that the model's pivot follows. When the model is cloned, the copy's "
                    .. "`PrimaryPart` refers to the copied part.",
            },
            {
                Name = "StreamingMode",
                Type = "Enum.StreamingMode",
                Default = "Enum.StreamingMode.Nonatomic",
                Records = { "ADR 0053" },
                Doc = "How this model is loaded and unloaded when the world streams. `Nonatomic`, the default, "
                    .. "loads each part separately by its own position. `Atomic` loads and unloads the whole "
                    .. "model as one unit. `Persistent` keeps the model loaded at all times.\n\n"
                    .. "The value is read when the world is divided for streaming. Changing it at run time has no "
                    .. "effect until the world is built again.",
            },
            {
                Name = "Scale",
                Type = "number",
                Default = "1",
                Doc = "The size of this model relative to the size it was built at. Setting it resizes every part "
                    .. "under the model and moves each part about the model's pivot, so the model grows or "
                    .. "shrinks in place. Must be greater than 0.\n\n"
                    .. "The value is absolute: setting 2 twice leaves the model at twice its built size, and "
                    .. "setting 1 restores it. Parts added to the model later are not scaled, so parent a part "
                    .. "before you scale. A scaled model costs the same to draw and simulate as an unscaled one.",
            },
        },
        Methods = {
            {
                Name = "GetExtentsSize",
                Parameters = {},
                Returns = { "vector" },
                Doc = "Returns the size, in meters, of the smallest box that contains every part in this model.",
            },
        },
    },
    {
        Name = "BaseScript",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Abstract" } :: { Schema.ClassTag },
        -- `Script` and `ModuleScript` inherit this rather than declaring their
        -- own: `World::create` calls every hook down the ancestry, so a second
        -- one would attach a second `ScriptComponent`.
        Storage = "Script",
        Doc = "The abstract base class of instances that hold Luau code. The code is stored in the `Source` "
            .. "property, so a script is created, copied and saved like any other instance.",
        Properties = {
            {
                Name = "Source",
                Type = "string",
                Code = true,
                ScriptReadOnly = true,
                Default = '""',
                Records = { "ADR 0137" },
                Doc = "The Luau code this script runs. It is stored on the instance, so the script is saved with "
                    .. "the scene or stamp that contains it. A running script can read `Source` but cannot write "
                    .. "it; only the editor can. To create a script at run time, `Clone` an existing one or use "
                    .. "`Instance.stamp`.",
            },
        },
    },
    {
        Name = "Script",
        Extends = "BaseScript",
        Module = "scene",
        Records = { "ADR 0137", "ADR 0138", "ADR 0058" },
        Doc = "Luau code that runs. A script runs while it is enabled, is in the world and not under a storage "
            .. "service, and its side (server or client) runs on this machine.\n\n"
            .. "Where a script is parented decides where it runs. A script under `ServerScriptService` or "
            .. "`GlobalScriptService.Server` runs only on the machine that is the authority. A script under "
            .. "`ClientScriptService` or `GlobalScriptService.Client` runs only on a machine with a player, never "
            .. "on a dedicated server. Anywhere else, `RunContext` decides.\n\n"
            .. "In the editor, scripts start when you press Play. Opening a project loads every script, so you "
            .. "can see and edit its `Source`, but starts none of them. Stop shuts the script VM down, so every "
            .. "Play starts from the same state. Outside the editor, scripts start when the game starts.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                -- The engine is the subscriber here. A write to this starts or
                -- stops a script (ADR 0059), and there is nothing for that to
                -- subscribe through -- the mask is per instance and set by a
                -- `GetPropertyChangedSignal`, so a rule that only worked once
                -- somebody had connected to a signal would usually not work.
                Observed = true,
                Records = { "ADR 0059" },
                Doc = "Whether this script runs. A script that is disabled when the game starts never starts.\n\n"
                    .. "Setting it to true starts the script from the top of its code, even if it has run before. "
                    .. "Setting it to false stops the script's threads from being resumed: deferred and delayed "
                    .. "tasks and signal handlers no longer run, and a thread that is running continues until it "
                    .. "next yields. The change takes effect at the next deferred update.\n\n"
                    .. "Disabling a script does not disconnect its connections and does not undo or remove "
                    .. "anything it made. Disconnect them yourself if you need to. Scripts enabled in the same "
                    .. "tick start in tree order.",
            },
            {
                Name = "RunContext",
                Type = "Enum.RunContext",
                ScriptReadOnly = true,
                Default = "Enum.RunContext.Shared",
                Records = { "ADR 0138" },
                Doc = "Which side runs this script when it is outside the script services: `Server` on the "
                    .. "authority, `Client` on a machine with a player, or `Shared` on both. A `Shared` script "
                    .. "runs once per machine, so a solo game runs it once. Inside `ServerScriptService`, "
                    .. "`ClientScriptService` and the `Server` and `Client` folders of `GlobalScriptService`, the "
                    .. "service decides, and moving a script into one sets this property to that side.\n\n"
                    .. "An exported game leaves out the code of every script whose side never runs on that "
                    .. "machine. The instance stays, with an empty `Source`, so a stamp can hold a server half "
                    .. "and a client half.\n\nOnly the editor can set it; a running script can only read it.",
            },
        },
    },
    {
        Name = "ModuleScript",
        Extends = "BaseScript",
        Module = "scene",
        Doc = "Luau code that is loaded with `require` instead of running on its own. The first `require(module)` "
            .. "runs the code once; every later `require` of the same instance returns the same value. Use it for "
            .. "code that several scripts share.",
    },
    {
        -- The structural half arrived with the kernel and the physical half with the physics work, when
        -- there was a simulation to mean something by it. The surface is a
        -- material the part WEARS (ADR 0090): no colour of its own, only the
        -- parameters its material lets it override.
        Name = "BasePart",
        Extends = "PVInstance",
        Module = "scene",
        Tags = { "Abstract" } :: { Schema.ClassTag },
        -- `Part` inherits this rather than declaring its own: `World::create`
        -- calls every hook down the ancestry, so a second one would attach a
        -- second `PartComponent`.
        Storage = "Part",
        Doc = "The abstract base class of every solid object in the world. A part has a position, an orientation, "
            .. "a size, a material and physical properties.",
        Properties = {
            {
                Name = "CFrame",
                Type = "CFrame",
                Doc = "The position and orientation of this part in world space. It is stored in double "
                    .. "precision; `Position` and `Orientation` are read from it.",
            },
            {
                Name = "Position",
                Type = "vector",
                Doc = "The position of this part in world space, in meters. It is `CFrame`'s position rounded to "
                    .. "single precision; more than a few kilometers from the origin, do your math on `CFrame` "
                    .. "instead.",
            },
            {
                Name = "Orientation",
                Type = "vector",
                Doc = "The rotation of this part in world space, as angles in degrees about the Y, X and Z axes, "
                    .. "applied in that order.",
            },
            {
                Name = "Size",
                Type = "vector",
                Doc = "The size of this part in meters, along its own X, Y and Z axes.",
            },
            {
                Name = "Material",
                Type = "Material?",
                Records = { "ADR 0090" },
                Doc = "The material this part is drawn with: a material loaded with `Material.load`, a copy made "
                    .. "with `material:Clone()`, or nil. With nil, the part uses the engine's default material, "
                    .. "which is white, non-metallic and fairly rough.\n\n"
                    .. "A part has no color of its own. Its look comes from its material, and a part can change "
                    .. "only the parameters that the material declares, with `SetMaterialParameter`. The default "
                    .. "material declares `Color` and `Transparency`.\n\n"
                    .. "Reading this property returns the material itself, not a copy; call `material:Clone()` to "
                    .. "get one you can change. A `MeshPart` with no material is drawn as its model file "
                    .. "describes, tinted by a `Color` parameter if you set one.",
            },
            {
                Name = "MaterialParameters",
                Type = "MaterialParameters",
                Doc = "The material parameters this part overrides, as a table keyed by name. Each read returns a "
                    .. "new table that holds only the overridden values. The possible names are `Color`, "
                    .. "`Transparency`, `Emissive`, `Metalness`, `Roughness`, `NormalScale` and `AlphaCutoff`.\n\n"
                    .. "An override that the current material does not declare is kept but has no effect, so a "
                    .. "part looks the same after its material is changed and changed back. Prefer "
                    .. "`SetMaterialParameter`, which checks the name against the material. Assigning this "
                    .. "property replaces the whole set and checks only that each entry is a known parameter with "
                    .. "a value of the right type.",
            },
            {
                Name = "CastShadow",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this part casts a shadow, from the sun and from every light. Turn it off for "
                    .. "effects that should not darken the scene, such as a muzzle flash, a trail or a marker on "
                    .. "the ground. The part is still drawn and lit, and still receives shadows. A transparent "
                    .. "part casts no shadow regardless of this value.",
            },
            {
                Name = "Fade",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Transient = true,
                -- One screen's choice, not the world's: two machines in one
                -- match hold different values of it and the same world, so it
                -- is in no hash (ADR 0184 §2).
                Presentation = true,
                Doc = "How much this machine fades the part out of its own picture, from 0 to 1. At 0 the part is "
                    .. "drawn as `Transparency` says; at 1 it is not drawn. It is never replicated and never "
                    .. "saved, so use it to change what one player sees of a part that every player has, such as "
                    .. "a tree between the camera and the character. The part keeps casting the shadow that its "
                    .. "`Transparency` gives it.",
            },
            {
                Name = "ReceivesDecals",
                Type = "boolean",
                Default = "true",
                Doc = "Whether a `Decal` is drawn on this part. A decal projects onto everything inside its box; "
                    .. "turn this off for parts that should stay clean, such as a character standing on a scorch "
                    .. "mark. The decal still paints the surfaces around the part. Terrain always receives decals.",
            },
            {
                Name = "Anchored",
                Type = "boolean",
                Default = "false",
                Doc = "Whether this part is fixed in place. An anchored part is not moved by gravity, forces or "
                    .. "collisions, but other parts still collide with it. Setting it keeps the part's position "
                    .. "and orientation.",
            },
            {
                Name = "CanCollide",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this part blocks other parts. With it off, other parts pass through, but `Touched` "
                    .. "still fires, which is how a trigger volume, a checkpoint or a pickup works.",
            },
            {
                Name = "CanTouch",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this part takes part in touch events. With it off, neither `Touched` nor "
                    .. "`TouchEnded` fires for this part or for a part that touches it. Collisions are still "
                    .. "resolved. Turn it off for scenery that no script listens to.",
            },
            {
                Name = "CanQuery",
                Type = "boolean",
                Default = "true",
                Doc = "Whether raycasts, shapecasts and overlap queries can find this part. It is independent of "
                    .. "`CanCollide`: a part can block movement and still be ignored by a camera ray.",
            },
            {
                Name = "Buoyant",
                Type = "boolean",
                Default = "true",
                Records = { "ADR 0118" },
                Doc = "Whether a `Water` pushes this part up. An unanchored part in water is lifted by the water "
                    .. "it displaces, at several points across its shape, so a hull pitches and rolls; it is also "
                    .. "slowed by the water's `Viscosity`. With it off, water ignores the part.",
            },
            {
                Name = "CollisionGroup",
                Type = "string",
                Default = '"Default"',
                Doc = "The name of the collision group that decides which parts this one collides with. Register "
                    .. "groups with `PhysicsService:RegisterCollisionGroup`. Setting a name that is not "
                    .. "registered raises an error.",
            },
            {
                Name = "Friction",
                Type = "number",
                Default = "0.3",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much this surface resists sliding, from 0 (no friction) to 1 (very grippy). A contact "
                    .. "uses a combination of both surfaces' values, so one slippery part is enough to make a "
                    .. "pair slide.",
            },
            {
                Name = "Restitution",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much this surface bounces, from 0 (no bounce) to 1 (a perfect bounce). A contact "
                    .. "combines both surfaces' values, as it does for `Friction`.",
            },
            {
                Name = "Density",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The mass of this part per cubic meter, in kilograms. The part's `Mass` is its density "
                    .. "times the volume of its shape.",
            },
            {
                Name = "Mass",
                Type = "number",
                ReadOnly = true,
                Doc = "The mass of this part in kilograms: `Density` times the volume of its shape. Change "
                    .. "`Density` or `Size` to change it.",
            },
            {
                Name = "AssemblyMass",
                Type = "number",
                ReadOnly = true,
                Doc = "The total mass, in kilograms, of this part and every part rigidly joined to it by a `Weld` "
                    .. "or a `FixedConstraint`.",
            },
            {
                Name = "LinearDamping",
                Type = "number",
                Default = "0.05",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How quickly this part slows down when nothing touches it, as the share of its speed that "
                    .. "it loses each second. At 0 it never slows; 1 feels like moving through thick air.",
            },
            {
                Name = "AngularDamping",
                Type = "number",
                Default = "0.05",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How quickly this part stops spinning when nothing touches it, as the share of its spin "
                    .. "that it loses each second.",
            },
            {
                Name = "ContactDetails",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `Collided` fires for this part. Off by default, because reporting every contact "
                    .. "has a cost.",
            },
            {
                Name = "LinearVelocity",
                Type = "vector",
                Doc = "The velocity of this part in meters per second, as of the last simulation tick. Setting it "
                    .. "sets the velocity for the next tick. To push the part instead of setting its velocity, "
                    .. "use `ApplyImpulse`.",
            },
            {
                Name = "AngularVelocity",
                Type = "vector",
                Doc = "The angular velocity of this part in radians per second about each world axis, as of the "
                    .. "last simulation tick. Setting it sets the spin for the next tick.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "SetMaterialParameter",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "value", Type = "Color3 | number | boolean | Vector2 | vector | { number }" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Overrides one parameter of this part's material, for this part only. Raises an error if "
                    .. "the material does not declare the parameter. `Color` and `Emissive` take a `Color3`; the "
                    .. "other built-in parameters take a number.\n\n"
                    .. "A parameter of the material's surface shader can be set if the material lists it in "
                    .. "`instanceParameters`. It takes a number, a boolean, a `Vector2`, a `vector`, a `Color3`, "
                    .. "or a table of four numbers, but not a texture. Surface shader parameters are not "
                    .. "replicated: set them on the machine that draws the part.",
            },
            {
                Name = "GetMaterialParameter",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "(Color3 | number | Vector2 | vector | { number })?" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the value this part is drawn with for one material parameter: the part's override "
                    .. "if it has one, otherwise the material's own value. Returns nil for a surface shader "
                    .. "parameter that neither sets; the shader's default is then used.",
            },
            {
                Name = "ClearMaterialParameter",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes this part's override of one material parameter, so the part uses the material's "
                    .. "value again. Does nothing if the parameter is not overridden.",
            },
            {
                Name = "SetNetworkOwner",
                Parameters = { { Name = "player", Type = "Player?" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0099" },
                Doc = "Gives one player's machine the simulation of this part. That machine simulates the part "
                    .. "and sends its position, so what the player pushes moves immediately on their screen. Pass "
                    .. "nil to return the part to the authority.\n\n"
                    .. "Only the authority can call it, and only on an unanchored part. When a player leaves, the "
                    .. "parts they owned return to the authority. The owner's results are trusted: validate them, "
                    .. "or do not hand the part over, if cheating matters.",
            },
            {
                Name = "GetNetworkOwner",
                Parameters = {},
                Returns = { "Player?" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns the player whose machine simulates this part, or nil if the authority does. On a "
                    .. "client it returns the local `Player` for a part that client owns, and nil for every other "
                    .. "part.",
            },
            {
                Name = "ApplyImpulse",
                Parameters = { { Name = "impulse", Type = "vector" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Applies an instant push at the part's center of mass, in kilogram meters per second. It "
                    .. "takes effect at the next simulation tick. An anchored part ignores it.",
            },
            {
                Name = "Fracture",
                Parameters = {
                    {
                        Name = "options",
                        Type = "{ Pieces: number?, Seed: number?, Strength: number? }?",
                        Default = "nil",
                    },
                },
                Returns = { "Model" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0215" },
                Doc = "Breaks the part into pieces and returns them in a `Model` where the part was. The part is "
                    .. "destroyed. Each piece is a part like the original, held to the pieces beside it by a "
                    .. "`FixedConstraint` that breaks under `Strength` newtons, so the pieces stand together until "
                    .. "something hits them hard enough. The pieces are unanchored. A part that was anchored keeps "
                    .. "its footing: an anchored `Footing` part under its bottom, which does not collide, holds the "
                    .. "pieces along the bottom by the same joints, so a wall breaks where it is hit rather than "
                    .. "falling over whole.\n\n"
                    .. "`options.Pieces` is how many, from 1 to 64 (8 by default). `options.Seed` chooses the "
                    .. "cut: the same seed and the same part give the same pieces on every machine. "
                    .. "`options.Strength` is what each joint holds; by default it is twice the part's weight.\n\n"
                    .. "The pieces are simulation: in a match the server's, sent to every player as parts, so "
                    .. "use as few as the moment needs. Dust and small chips are better as a `ParticleEmitter` "
                    .. "burst when a joint breaks: drawn on each machine and sent as nothing but the burst.\n\n"
                    .. "Only a `Part` whose `Shape` is `Block` can be broken.",
            },
            {
                Name = "BindToPredictedTouch",
                Parameters = { { Name = "fn", Type = "(character: BasePart, step: PredictedStep) -> ()" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0157" },
                Doc = "Runs `fn(character, step)` inside the simulation step when a player's character that this "
                    .. "machine simulates starts touching this part. Use it for a jump pad, a booster or a "
                    .. "trampoline, so the effect is predicted on the player's machine and needs no correction.\n\n"
                    .. "On a client the function runs again for each tick that is replayed after a correction, as "
                    .. "a function bound with `RunService:BindToPredictedStep` does. A part has one such "
                    .. "function; binding again replaces it. The function must not yield.",
            },
            {
                Name = "UnbindFromPredictedTouch",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes the function bound with `BindToPredictedTouch`. Does nothing if none is bound.",
            },
            {
                Name = "GetRenderCFrame",
                Parameters = {},
                Returns = { "CFrame" },
                ThreadSafety = "ReadParallel",
                Records = { "ADR 0136" },
                Doc = "Returns where this part is drawn in the current frame, which is between its positions at "
                    .. "the last two simulation ticks. Use it for anything that must line up with the picture, "
                    .. "such as a camera that follows the part.\n\n"
                    .. "It differs from `CFrame` only inside a render step: a function bound with "
                    .. "`RunService:BindToRenderStep`, a `PreRender` handler, or an action whose context has "
                    .. "`Rate = Render`. Everywhere else, including on a server, it returns `CFrame`, so "
                    .. "simulation code never depends on frame timing.",
            },
            {
                Name = "ApplyImpulseAtPosition",
                Parameters = { { Name = "impulse", Type = "vector" }, { Name = "position", Type = "vector" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0118" },
                Doc = "Applies an instant push at a point in world space, in kilogram meters per second. A push "
                    .. "away from the center of mass also turns the part. It takes effect at the next simulation "
                    .. "tick.",
            },
            {
                Name = "ApplyAngularImpulse",
                Parameters = { { Name = "impulse", Type = "vector" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Applies an instant twist about the axis the vector points along, in kilogram square meters "
                    .. "per second. It changes the part's spin without moving it. It takes effect at the next "
                    .. "simulation tick.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Touched",
                Parameters = { { Name = "otherPart", Type = "BasePart" } },
                Records = { "ADR 0015" },
                Doc = "Fires when another part starts touching this one. Passes the other part. A pair that stays "
                    .. "in contact fires once, not every tick. Like every signal, it is deferred: handlers run at "
                    .. "the next resumption point, not inside the physics step.",
            },
            {
                Name = "Collided",
                Parameters = {
                    { Name = "otherPart", Type = "BasePart" },
                    { Name = "position", Type = "vector" },
                    { Name = "normal", Type = "vector" },
                    { Name = "speed", Type = "number" },
                },
                Doc = "Fires together with `Touched` when `ContactDetails` is on, with details of the contact. "
                    .. "Passes the other part, the contact point in world space, the direction from this part "
                    .. "into the other, and the speed at which the two were approaching along that direction, in "
                    .. "meters per second, before the collision was resolved.",
            },
            {
                Name = "TouchEnded",
                Parameters = { { Name = "otherPart", Type = "BasePart" } },
                Doc = "Fires when another part stops touching this one. Passes the other part. It does not fire "
                    .. "for contacts that end because a part was destroyed.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "Part",
        Extends = "BasePart",
        Module = "scene",
        Parents = InTheWorldOrAFrame,
        Doc = "A part whose shape is one of the built-in solids: a block, a ball, a cylinder, a capsule or a wedge.",
        Properties = {
            {
                Name = "Shape",
                Type = "Enum.PartShape",
                Doc = "Which built-in solid this part is. The shape sets both what is drawn and what collides.\n\n"
                    .. "`Size` is the full extent of the shape. A `Ball` is a sphere whose diameter is the "
                    .. "largest of the three sizes. A `Cylinder` and a `Capsule` stand along the Y axis: their "
                    .. "diameter is the larger of X and Z, and their height, including the caps, is Y. A "
                    .. "capsule's drawn shape matches its collision shape exactly when `Size.y` is twice "
                    .. "`Size.x`.\n\n"
                    .. "A `Wedge` is a ramp whose tall face points toward -Z. It collides as its whole box, not "
                    .. "as a ramp.",
            },
        },
    },
    {
        Name = "MeshPart",
        Extends = "BasePart",
        Module = "render",
        Parents = InTheWorldOrAFrame,
        Storage = "MeshPart",
        Doc = "A part whose shape is a mesh imported from a model file. One `MeshPart` is one mesh; a model made "
            .. "of several pieces is several `MeshPart`s.",
        Properties = {
            {
                Name = "MeshContent",
                Type = "Content",
                ContentKind = "Mesh",
                Doc = "The model file this part draws, as an `asset://` path to a glTF file. Setting it replaces "
                    .. "the mesh. If the file fails to load, the part keeps its previous mesh and the error is "
                    .. "logged.",
            },
            {
                Name = "CollisionFidelity",
                Type = "Enum.CollisionFidelity",
                Doc = "How closely the collision shape follows the mesh. `Box` collides as the mesh's bounding "
                    .. "box. Every other value collides as the mesh's convex hull.\n\n"
                    .. "Until the mesh has loaded, the part collides as its bounding box. A mesh too thin or too "
                    .. "small to form a solid also collides as its bounding box. The property reads back the "
                    .. "value you set.",
            },
            {
                Name = "MeshSize",
                Type = "vector",
                Default = "vector.create(1, 1, 1)",
                Doc = "The size of the mesh as it was authored, in meters. The part is scaled by `Size` divided "
                    .. "by `MeshSize`, so setting `Size` to twice `MeshSize` draws and collides the mesh at twice "
                    .. "its authored size.\n\n"
                    .. "The importer sets this value and it is saved with the scene. You can set it yourself to "
                    .. "stretch the mesh. Each component must be greater than 0.",
            },
            {
                Name = "PoseFrom",
                Type = "MeshPart?",
                Default = "nil",
                Records = { "ADR 0201" },
                Doc = "Another skinned `MeshPart` whose pose this one copies. Set it on a piece of armor, hair, a "
                    .. "glove or a held item so that the piece moves with the body it is worn on.\n\n"
                    .. "Every joint that both meshes have follows the leader's joint exactly, whatever moved it: "
                    .. "a clip, an animation graph, an `IKControl`, a `SpringBone` or a ragdoll. Joints are "
                    .. "matched by name. A joint that only this mesh has stays at rest relative to its parent, "
                    .. "unless an animation playing on the leader has a track for it.\n\n"
                    .. "A mesh that follows a leader has no physics body of its own: it does not collide, and "
                    .. "raycasts pass through it. It keeps its own `SpringBone` chains and morph targets. It "
                    .. "follows nothing if it names itself, if the leaders form a loop, or if the leader has no "
                    .. "skeleton. To attach an item that shares no joints with the body, parent it under a `Bone` "
                    .. "of the body instead.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetMorphTargets",
                Parameters = {},
                Returns = { "{ string }" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the names of this mesh's morph targets (shape keys), in the order the file lists "
                    .. "them. Returns an empty array if the mesh has none, or if its file has not loaded yet. To "
                    .. "be sure the mesh is loaded, call `ContentProvider:PreloadAsync` first.",
            },
            {
                Name = "SetMorphWeight",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "weight", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets the weight of one morph target of this mesh. 0 is the mesh at rest and 1 is the full "
                    .. "shape. Values outside that range are allowed, and several targets add together.\n\n"
                    .. "The weight applies only on the machine where the script runs. It is not replicated and "
                    .. "not recorded in a replay, so set it in a client script. It overrides what a playing clip "
                    .. "sets for the same target until you call `ClearMorphWeight`.\n\n"
                    .. "Raises an error for a name the mesh does not have, once the mesh has loaded. A weight set "
                    .. "before the mesh loads is kept and applied when it arrives.",
            },
            {
                Name = "GetMorphWeight",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the weight this part is drawn with for one morph target, on this machine. That is "
                    .. "the value a script set, or else the value from the clips playing on the mesh, or else the "
                    .. "weight the file gives at rest. Returns 0 for a name the mesh does not have.",
            },
            {
                Name = "ClearMorphWeight",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes the weight that `SetMorphWeight` set for one morph target. From the next frame, "
                    .. "the target follows the playing clips again, or the weight the file gives at rest.",
            },
        },
    },
    {
        Name = "Bone",
        Extends = "Attachment",
        Module = "render",
        Parents = under("BasePart", "Bone"),
        -- No `Storage`: it inherits `Attachment`'s. `World::create` calls every
        -- hook down the ancestry, so declaring one here would attach a second
        -- `AttachmentComponent` -- the same reason `Script` does not declare one
        -- beside `BaseScript`'s.
        Doc = "An attachment that follows one joint of the skinned `MeshPart` it is parented to. Use it to weld a "
            .. "sword to a hand, hang a light from a moving arm, or read where a foot is: it moves with the "
            .. "animation.\n\n"
            .. "Bones are not created automatically. Create one for each joint you need and set its `JointName`. "
            .. "A bone whose `JointName` does not match a joint in the rig follows the `MeshPart` itself.",
        Properties = {
            {
                Name = "JointName",
                Type = "string",
                Doc = "The name of the joint this bone follows, as it is named in the model file of the parent "
                    .. "`MeshPart`.",
            },
            {
                Name = "JointIndex",
                Type = "number",
                ReadOnly = true,
                Default = "-1",
                Doc = "The index of the joint that `JointName` matched, or -1 if the rig has no joint of that "
                    .. "name. It is looked up again when the mesh reloads.",
            },
            {
                Name = "Transform",
                Type = "CFrame",
                Doc = "An offset added to the joint's animated pose, in the joint's own space. Use it for "
                    .. "procedural animation on top of a clip: bend an arm toward a target, turn a head, kick a "
                    .. "weapon back on a shot. The identity, the default, leaves the pose unchanged.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "IKControl",
        Extends = "Instance",
        Module = "render",
        Parents = under("MeshPart"),
        Storage = "IKControl",
        Records = { "ADR 0198" },
        Doc = "Bends a limb so its end reaches a target, or turns a head to look at one, on top of the animation "
            .. "that is playing. Use it to put a hand on a weapon's grip or a foot on a pedal, or to make eyes "
            .. "follow something. Parent it to the skinned `MeshPart`, set `EndJoint` to the joint at the end of "
            .. "the limb (the hand, the foot or the head), and set `Target`.\n\n"
            .. "It is visual only. Each machine solves it every frame for the pose it draws; it does not change "
            .. "the pose that simulation code reads, so a `Bone` on the hand still reports where the clip put it. "
            .. "To turn a joint in a way that game logic can read, use `Bone.Transform`. Anything drawn with the "
            .. "limb follows it, such as a part attached to one of its bones.\n\n"
            .. "A limb reaches no farther than its length. In a multiplayer match, the control and its properties "
            .. "replicate with the character, and each machine does the bending itself.",
        Properties = {
            {
                Name = "Type",
                Type = "Enum.IKControlType",
                Default = "Enum.IKControlType.TwoBone",
                Doc = "What the control does. `TwoBone` treats the end joint and the two joints above it as a "
                    .. "limb and moves the end to the target. `LookAt` turns the end joint to face the target; "
                    .. "the joints above it share the turn.",
            },
            {
                Name = "EndJoint",
                Type = "string",
                Doc = "The name of the joint at the end of the limb, as it is named in the model file: a hand, a "
                    .. "foot or a head. The control does nothing if the rig has no joint of that name.",
            },
            {
                Name = "Target",
                Type = "(BasePart | Attachment)?",
                Doc = "The part or attachment to reach for or look at. The control uses the position where the "
                    .. "target is drawn in the current frame. With no target, the control does nothing.",
            },
            {
                Name = "TargetOffset",
                Type = "CFrame",
                Doc = "An offset from the target, in the target's own space. Use it to aim at a point on the "
                    .. "target, such as a grip a hand's width from the middle of a weapon.",
            },
            {
                Name = "Pole",
                Type = "(BasePart | Attachment)?",
                Doc = "For `TwoBone`: the part or attachment the elbow or knee points toward. With none, the limb "
                    .. "bends the way the animation already bends it.",
            },
            {
                Name = "AlignRotation",
                Type = "boolean",
                Doc = "For `TwoBone`: whether the end joint also takes the target's rotation, so a palm lies flat "
                    .. "on a grip or a sole lies flat on a pedal.",
            },
            {
                Name = "ChainLength",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.ik_chain_length",
                Doc = "For `LookAt`: how many joints above the end joint share the turn, from 0 to 8. With 0 only "
                    .. "the head turns; with 2, the head, the neck and one spine joint each take an equal share.",
            },
            {
                Name = "MaxAngle",
                Type = "number",
                Default = "70",
                ErrorKey = "scene.err.number_cone_degrees",
                Doc = "For `LookAt`: the largest angle, in degrees, that the look can turn away from straight "
                    .. "ahead. Beyond it, the look stops at the limit.",
            },
            {
                Name = "Weight",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How strongly the control is applied, from 0 to 1. At 0 only the animation shows. Tween it "
                    .. "to blend the control in and out smoothly.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the control is applied. A disabled control costs nothing.",
            },
            {
                Name = "Smoothing",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The time, in seconds, over which a sudden change is eased, such as a target that jumps or "
                    .. "the control being enabled. At 0 the control follows immediately.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "FootPlacement",
        Extends = "Instance",
        Module = "render",
        Parents = under("MeshPart"),
        Storage = "FootPlacement",
        Records = { "ADR 0198" },
        Doc = "Places a character's feet on the ground under them, such as a stair, a slope or a rock, and lowers "
            .. "the hips to match. Walk animations are made for flat ground; this adapts them to uneven ground.\n\n"
            .. "Parent it to the skinned `MeshPart` and set the two ankle joints and the hips joint. Every frame, "
            .. "a ray cast down from each foot finds the ground. The foot is moved onto it, keeping the height "
            .. "the animation had lifted it to, and the knee bends to fit.\n\n"
            .. "It is visual only: each machine computes it for the pose it draws, and simulation code does not "
            .. "see it. It applies only to characters near the camera, at a distance set by the graphics "
            .. "settings, and not at all at the lowest quality level or for the agents of a `Swarm`. It costs two "
            .. "raycasts per character per frame.",
        Properties = {
            {
                Name = "LeftFoot",
                Type = "string",
                Doc = "The name of the left ankle joint, as it is named in the model file.",
            },
            {
                Name = "RightFoot",
                Type = "string",
                Doc = "The name of the right ankle joint, as it is named in the model file.",
            },
            {
                Name = "Hips",
                Type = "string",
                Doc = "The name of the joint both legs hang from. It is lowered when one foot is below the ground "
                    .. "level the animation assumes.",
            },
            {
                Name = "FootHeight",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance from the ankle joint down to the sole, in meters. The ankle is kept this far "
                    .. "above the ground.",
            },
            {
                Name = "StepHeight",
                Type = "number",
                Default = "0.4",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The largest distance, in meters, that a foot is moved up or down from where the animation "
                    .. "put it. Ground farther away than that, such as a ledge or a drop, is ignored.",
            },
            {
                Name = "AlignToSlope",
                Type = "boolean",
                Default = "true",
                Doc = "Whether a planted foot is rotated to lie flat on the ground under it.",
            },
            {
                Name = "Weight",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How strongly the placement is applied, from 0 to 1.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the placement is applied.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "LipSync",
        Extends = "Instance",
        Module = "render",
        Parents = under("MeshPart"),
        Storage = "LipSync",
        Records = { "ADR 0200" },
        Doc = "Moves a character's mouth to match speech. Parent it to the `MeshPart` that has the face's morph "
            .. "targets and set `Source`. While a `Sound` in the `Voice` category plays under the source, the "
            .. "face follows it; no script is needed.\n\n"
            .. "There are three methods, and each is the fallback of the one before. If the recording has a "
            .. "viseme track beside it (`intro_01.visemes.json` beside `intro_01.ogg`, one per language), the "
            .. "track is played. Otherwise the sound is analyzed in three frequency bands to tell an open mouth "
            .. "from a wide or a round one. A face with only a jaw target opens it by how loud the sound is.\n\n"
            .. "A face map file beside the model (`hero.face.json` beside `hero.glb`) says which morph targets "
            .. "each viseme, band and loudness level moves. Without one, a target named `JawOpen` or `MouthOpen` "
            .. "is opened by loudness.\n\n"
            .. "It is visual only and local to each machine, which plays the track for the voice language it "
            .. "hears. Its weights are applied over those a clip sets and under those a script sets with "
            .. "`SetMorphWeight`.",
        Properties = {
            {
                Name = "Source",
                Type = "Instance?",
                Default = "nil",
                Doc = "The instance whose voice moves the mouth: a `Voice` sound that is this instance, or is "
                    .. "under it, drives the face. With nil, the source is the parent of the `MeshPart`.",
            },
            {
                Name = "Mode",
                Type = "Enum.LipSyncMode",
                Default = "Enum.LipSyncMode.Auto",
                Doc = "Which method is used. `Auto`, the default, plays a viseme track when the line has one and "
                    .. "otherwise falls back to analyzing the sound.",
            },
            {
                Name = "Map",
                Type = "Content",
                ContentKind = "FaceMap",
                Doc = "The face map file, as an `asset://` path ending in `.face.json`. It says which morph "
                    .. "targets each viseme, frequency band and loudness level moves. When empty, the file beside "
                    .. "the model is used: the model's path with its extension replaced by `.face.json`.",
            },
            {
                Name = "Smoothing",
                Type = "number",
                Default = "0.06",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "The time, in seconds, a morph target takes to reach a new weight. At 0 the mouth snaps "
                    .. "between shapes; 0.1 is a slow mouth.",
            },
            {
                Name = "Weight",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.lip_sync_weight",
                Doc = "How strongly the mouth moves, from 0 to 2. Use 0.5 for a mumble and more than 1 for a shout.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the mouth is moved.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "SpringBone",
        Extends = "Instance",
        Module = "render",
        Parents = under("MeshPart"),
        Storage = "SpringBone",
        Records = { "ADR 0194" },
        Doc = "Makes a chain of joints swing and settle behind the body that carries it, for a cape, a tail, hair "
            .. "or a tassel. Parent it to the skinned `MeshPart` and set `RootJoint` to the joint the chain hangs "
            .. "from. The joints below it then move with the body, on top of the animation that is playing.\n\n"
            .. "It is visual only. Each machine runs it at the frame rate after the animation is applied; it is "
            .. "not replicated or recorded in a replay, and a server with no window does not run it. Game logic "
            .. "should not read where the chain is.\n\n"
            .. "A cape is typically three or four columns of four or five joints, skinned like any others. One "
            .. "`SpringBone` on the joint above all the columns moves every column.\n\n"
            .. "Each joint keeps its distance from its parent, is pulled back toward its animated pose by "
            .. "`Stiffness`, and never bends more than `LimitAngle` from that pose. Add `SpringCollider`s under "
            .. "the same mesh to keep the chain out of the body.",
        Properties = {
            {
                Name = "RootJoint",
                Type = "string",
                Doc = "The name of the joint the chain hangs from, as it is named in the model file. That joint "
                    .. "stays where the animation puts it; every joint below it is moved. Nothing moves if the "
                    .. "rig has no joint of that name.",
            },
            {
                Name = "JointPattern",
                Type = "string",
                Doc = "A name pattern for the top joint of each chain, where `*` matches any run of characters. "
                    .. "Every joint the pattern matches starts its own chain: `Cape_*0` matches the top of each "
                    .. "column of a cape whose joints are named `Cape_L0`, `Cape_L1`, and so on. A matched joint "
                    .. "that is below another matched joint belongs to that joint's chain. Use it with "
                    .. "`RootJoint` or in place of it.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the chain is moved. When it is off, the joints return to their animated pose from "
                    .. "the next frame.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "0.25",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How strongly each joint is pulled back toward its animated pose, from 0 to 1. At 0 the "
                    .. "chain hangs like a rope; at 1 it does not move.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0.2",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much of its motion a joint loses each step, from 0 to 1. A low value swings for a long "
                    .. "time; a high value settles quickly and looks heavy.",
            },
            {
                Name = "GravityScale",
                Type = "number",
                Default = "1",
                Doc = "How much of the world's gravity affects the chain. At 0 the chain floats; a negative value "
                    .. "makes it rise.",
            },
            {
                Name = "Inertia",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much the chain lags behind the body's motion, from 0 to 1. At 1 a cape streams out "
                    .. "behind a running character; at 0 it moves with the body as if there were no air.",
            },
            {
                Name = "LimitAngle",
                Type = "number",
                Default = "70",
                ErrorKey = "scene.err.number_cone_degrees",
                Doc = "The largest angle, in degrees, that a joint can bend away from its animated direction.",
            },
            {
                Name = "Radius",
                Type = "number",
                Default = "0.05",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The thickness of the chain where it meets a `SpringCollider`, in the mesh's units.",
            },
            {
                Name = "WindInfluence",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much the world's wind pushes the chain. At 0 the chain ignores wind.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "SpringCollider",
        Extends = "Instance",
        Module = "render",
        Parents = under("MeshPart"),
        Storage = "SpringCollider",
        Records = { "ADR 0194" },
        Doc = "A sphere or capsule attached to a joint that the `SpringBone` chains and the `Cloth` of the same "
            .. "`MeshPart` cannot pass through. Use a few to keep a cape or hair out of the body: one for the back "
            .. "and one for each leg is usually enough. A `Cloth` on a mesh that wears another's pose is also kept "
            .. "out of the colliders of the mesh it follows.\n\n"
            .. "It affects only `SpringBone` chains and `Cloth`. It is not a physics shape, and nothing else in the "
            .. "world collides with it.",
        Properties = {
            {
                Name = "JointName",
                Type = "string",
                Doc = "The name of the joint the collider follows, as it is named in the model file. If the rig "
                    .. "has no joint of that name, the collider follows the `MeshPart` itself.",
            },
            {
                Name = "Radius",
                Type = "number",
                Default = "0.25",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The radius of the sphere, or of the capsule's ends.",
            },
            {
                Name = "Length",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The length of the capsule along the joint's up axis, starting at `Offset`. At 0 the "
                    .. "collider is a sphere.",
            },
            {
                Name = "Offset",
                Type = "vector",
                Doc = "The position of the sphere's center, or of the capsule's lower end, in the joint's own space.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Cloth",
        Extends = "Instance",
        Module = "render",
        Parents = under("MeshPart"),
        Storage = "Cloth",
        Records = { "ADR 0214" },
        Doc = "Makes the `MeshPart` it is parented to hang, sway and fold like cloth: a cape, a flag, a skirt or a "
            .. "banner. Each vertex of the mesh is simulated, pulled by gravity and the world's wind, kept at its "
            .. "distance from its neighbors, and kept out of the `SpringCollider`s of the mesh and of the parts "
            .. "anchored near it.\n\n"
            .. "It is visual only. Each machine runs it at the frame rate after the animation is applied; it is not "
            .. "replicated or recorded in a replay, and a server with no window does not run it. Game logic should "
            .. "not read where the cloth is.\n\n"
            .. "The mesh must be skinned. Its vertices that are weighted at least half to a joint `PinJoints` "
            .. "matches are pinned: they stay where the animation puts them, and the rest hang from them. Every "
            .. "other vertex hangs free, never further than `MaxDistance` from where the animation would put it. "
            .. "A cape is modeled hanging straight down, with its top row weighted to a joint at the shoulders and "
            .. "the rest weighted as it would be without cloth.\n\n"
            .. "A mesh of more than 8,192 vertices is drawn as the animation has it. Cloth further from the camera "
            .. "than the quality level allows, and at the Low quality level, is also drawn as the animation has it.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the cloth is simulated. When it is off, the mesh is drawn as the animation has it.",
            },
            {
                Name = "PinJoints",
                Type = "string",
                Doc = "A name pattern for the joints the cloth hangs from, where `*` matches any run of characters. "
                    .. "A vertex weighted at least half to joints it matches is pinned to where the animation puts "
                    .. "it. With no pinned vertex, the cloth hangs only by `MaxDistance`.",
            },
            {
                Name = "MaxDistance",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How far, in meters, a vertex may move from where the animation puts it. A cape that should "
                    .. "never fly over the head has a small one; at 0 the cloth does not move.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "0.9",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much the cloth resists stretching, from 0 to 1. At 1 it keeps the length it was modeled "
                    .. "with, as canvas does; lower values stretch like knitted cloth.",
            },
            {
                Name = "BendStiffness",
                Type = "number",
                Default = "0.2",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much the cloth resists folding, from 0 to 1. Silk is near 0 and leather near 1.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much of its motion the cloth loses each second, from 0 to 1. A low value flutters for a "
                    .. "long time; a high value settles quickly and looks heavy.",
            },
            {
                Name = "GravityScale",
                Type = "number",
                Default = "1",
                Doc = "How much of the world's gravity affects the cloth. At 0 the cloth floats.",
            },
            {
                Name = "WindInfluence",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much the world's wind pushes the cloth. At 0 the cloth ignores wind.",
            },
            {
                Name = "Thickness",
                Type = "number",
                Default = "0.02",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How far, in meters, the cloth stays from what it rests on.",
            },
            {
                Name = "CollidesWithWorld",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the cloth is kept out of the anchored parts near it, such as a wall it is pressed "
                    .. "against or the floor it drags on. It never pushes them.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Attachment",
        Extends = "Instance",
        Module = "scene",
        Parents = under("BasePart"),
        Storage = "Attachment",
        Doc = "A named point on a part, with a position and an orientation. Use it as a socket to weld something "
            .. "to, as the anchor of a constraint, or as the place where an effect is emitted. It has no physics "
            .. "body.",
        Properties = {
            {
                Name = "CFrame",
                Type = "CFrame",
                Doc = "The position and orientation of the attachment relative to the part it is parented to.",
            },
            {
                Name = "WorldCFrame",
                Type = "CFrame",
                ReadOnly = true,
                Doc = "The position and orientation of the attachment in world space. It is updated every tick as "
                    .. "the part moves.",
            },
        },
        Methods = {
            {
                Name = "GetRenderCFrame",
                Parameters = {},
                Returns = { "CFrame" },
                ThreadSafety = "ReadParallel",
                Records = { "ADR 0136" },
                Doc = "Returns where this attachment is drawn in the current frame, which is between its "
                    .. "positions at the last two simulation ticks. Use it for anything that must line up with "
                    .. "the picture, such as a camera that follows the attachment.\n\n"
                    .. "It differs from `WorldCFrame` only inside a render step: a function bound with "
                    .. "`RunService:BindToRenderStep`, a `PreRender` handler, or an action whose context has "
                    .. "`Rate = Render`. Everywhere else, including on a server, it returns `WorldCFrame`.",
            },
        } :: { Schema.Method },
    },
    {
        -- `NotCreatable` and abstract: a constraint's TYPE is its class, because
        -- a hinge that could become a slider is two joints wearing one name.
        Name = "Constraint",
        Extends = "Instance",
        Module = "scene",
        Parents = InTheWorld,
        Storage = "Constraint",
        Tags = { "Abstract", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "The abstract base class of physics joints. A constraint connects two parts through two "
            .. "`Attachment`s, and the physics solver works out how both parts move: a door swings under its own "
            .. "weight, a ragdoll falls.\n\n"
            .. "A constraint is not a `Weld`. A weld moves its second part with its first every tick, without the "
            .. "solver; use it to attach one thing to another. Use a constraint for parts that move relative to "
            .. "each other.\n\n"
            .. "The two attachments set where the joint is on each part. A constraint cannot be connected to a "
            .. "`CharacterBody`, which is moved by its controller and not by the solver.",
        Properties = {
            {
                Name = "Attachment0",
                Type = "Attachment?",
                Doc = "The attachment at one end of the joint. The part it is on is one of the two bodies the "
                    .. "joint connects.",
            },
            {
                Name = "Attachment1",
                Type = "Attachment?",
                Doc = "The attachment at the other end of the joint. When the joint is created, the two "
                    .. "attachments mark the same place on the two parts.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the joint holds. A disabled constraint stays in the world and holds nothing. It "
                    .. "keeps its place in the solver's order, so enabling it again behaves as it did before; "
                    .. "destroying and recreating a joint does not guarantee that.",
            },
            {
                Name = "CollideConnected",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the two connected parts collide with each other. Turn it off for parts that "
                    .. "overlap at the joint, such as the upper and lower arm of a ragdoll, or they push each "
                    .. "other apart every step.",
            },
            {
                Name = "Visible",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the constraint is drawn. In the editor and in the debug overlay, every constraint "
                    .. "is drawn as a shape that shows what it is. In a running game, only a rope, a rod and a "
                    .. "spring are drawn, as a thin line with their `Color` and `Thickness`.",
            },
            {
                Name = "BreakForce",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The force, in newtons, above which the joint breaks. A joint that breaks is disabled and "
                    .. "fires `Broken`. At 0 the joint never breaks.",
            },
            {
                Name = "BreakTorque",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The torque, in newton meters, above which the joint breaks. At 0 the joint never breaks.",
            },
        },
        Methods = {
            {
                Name = "GetForce",
                Parameters = {},
                Returns = { "number" },
                Doc = "Returns the force, in newtons, that the joint applied during the last simulation tick to "
                    .. "hold its two parts together. This is the value `BreakForce` is compared with. Returns 0 "
                    .. "for a constraint that only drives motion, such as `AlignPosition`; use `GetMotorForce` "
                    .. "for those.",
            },
            {
                Name = "GetTorque",
                Parameters = {},
                Returns = { "number" },
                Doc = "Returns the torque, in newton meters, that the joint applied during the last simulation "
                    .. "tick to resist turning. Returns 0 for a joint that does not resist turning, such as a "
                    .. "ball joint or a rope.",
            },
            {
                Name = "GetMotorForce",
                Parameters = {},
                Returns = { "number" },
                Doc = "Returns the force, in newtons, that the constraint's motor applied during the last "
                    .. "simulation tick. It applies to the actuator of a `PrismaticConstraint`, an "
                    .. "`AlignPosition` and a `LinearVelocity`. It is separate from `GetForce`, which is the load "
                    .. "the joint carried. Returns 0 for a constraint with no linear motor.",
            },
            {
                Name = "GetMotorTorque",
                Parameters = {},
                Returns = { "number" },
                Doc = "Returns the torque, in newton meters, that the constraint's motor applied during the last "
                    .. "simulation tick. It applies to the actuator of a `HingeConstraint` or a "
                    .. "`BallSocketConstraint`, an `AlignOrientation` and an `AngularVelocity`. A servo that "
                    .. "reads its `ServoMaxTorque` here is at its limit. Returns 0 for a constraint with no "
                    .. "rotary motor.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Broken",
                Parameters = {},
                Doc = "Fires when the joint breaks because it carried more than `BreakForce` or `BreakTorque`. "
                    .. "The joint is disabled, not destroyed; set `Enabled` to true to restore it.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "BallSocketConstraint",
        Extends = "Constraint",
        Module = "scene",
        -- A hook of its own, which does NOT add a second component: it stamps
        -- the kind onto the one `Constraint` already added. `World::create`
        -- walks the ancestry root-first, so the base has run by the time this
        -- does -- which is what lets the type be the class rather than a
        -- property somebody could write a hinge into.
        Storage = "BallSocketConstraint",
        Doc = "A joint that lets two parts rotate freely about one point, optionally limited to a cone and a "
            .. "twist. Without limits it is the end of a rope or a pendulum. With limits it is a shoulder or a "
            .. "hip, as in a ragdoll. The limits are enforced by the solver and are never exceeded.",
        Properties = {
            {
                Name = "LimitsEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `UpperAngle` and `TwistLimit` apply. When it is off, the joint rotates freely.",
            },
            {
                Name = "UpperAngle",
                Type = "number",
                Default = "180",
                Doc = "The half-angle of the cone the joint can swing within, in degrees, measured from the X "
                    .. "axis of `Attachment0`.",
            },
            {
                Name = "TwistLimit",
                Type = "number",
                Default = "180",
                Doc = "How far the joint can twist about its X axis, in degrees, in either direction.",
            },
            {
                Name = "ActuatorType",
                Type = "Enum.ActuatorType",
                Default = "Enum.ActuatorType.None",
                Doc = "Whether the joint drives itself. `None` does nothing; `Servo` holds the orientation set in "
                    .. "`TargetOrientation`. `Motor` is treated as `None`, because a ball joint has no single "
                    .. "axis to spin about.",
            },
            {
                Name = "TargetOrientation",
                Type = "CFrame",
                Default = "CFrame.identity",
                Doc = "For `Servo`: the orientation of `Attachment1` relative to `Attachment0` that the joint "
                    .. "holds. Only the rotation is used. The identity is the pose the two parts were joined in. "
                    .. "With the joint's X axis along a limb, a rotation about X twists the limb, and a rotation "
                    .. "about Y or Z swings it.",
            },
            {
                Name = "ServoMaxTorque",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Servo`: the largest torque the joint can apply, in newton meters, about each of its "
                    .. "axes. A load that needs more than this pulls the joint away from its target.",
            },
            {
                Name = "AngularResponsiveness",
                Type = "number",
                Default = "45",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "For `Servo`: how quickly the joint turns toward its target, as a frequency in radians per "
                    .. "second. Used only when `Stiffness` is 0. The joint settles without overshooting. The "
                    .. "value is tuned for the two parts the joint connects; if more parts hang from the joint, "
                    .. "set `Stiffness` instead.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Servo`: the strength of the spring that pulls the joint to its target, in newton "
                    .. "meters per radian. At 0, `AngularResponsiveness` is used instead. Above 0, this and "
                    .. "`Damping` set the response. For a load with a moment of inertia of `I` kilogram square "
                    .. "meters, `w * w * I` gives a response of `w` radians per second.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Servo`: how strongly the joint resists turning, in newton meter seconds per radian. "
                    .. "Used together with `Stiffness`. Critical damping, which reaches the target without "
                    .. "overshooting, is `2 * math.sqrt(Stiffness * I)` for a moment of inertia of `I`. Less "
                    .. "overshoots and oscillates; more arrives slowly.",
            },
        },
    },
    {
        Name = "HingeConstraint",
        Extends = "Constraint",
        Module = "scene",
        -- A hook of its own, which does NOT add a second component: it stamps
        -- the kind onto the one `Constraint` already added. `World::create`
        -- walks the ancestry root-first, so the base has run by the time this
        -- does -- which is what lets the type be the class rather than a
        -- property somebody could write a hinge into.
        Storage = "HingeConstraint",
        Doc = "A joint that lets two parts rotate about one axis, such as a door, a lid or an elbow. The axis is "
            .. "the X axis of the attachments, so rotating an attachment aims the hinge.",
        Properties = {
            {
                Name = "LimitsEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `LowerAngle` and `UpperAngle` apply. When it is off, the hinge turns freely.",
            },
            {
                Name = "LowerAngle",
                Type = "number",
                Default = "-180",
                Doc = "The lower limit of the hinge's rotation, in degrees.",
            },
            {
                Name = "UpperAngle",
                Type = "number",
                Default = "180",
                Doc = "The upper limit of the hinge's rotation, in degrees. If `LowerAngle` is greater than "
                    .. "`UpperAngle`, the hinge is not limited.",
            },
            {
                Name = "ActuatorType",
                Type = "Enum.ActuatorType",
                Default = "Enum.ActuatorType.None",
                Doc = "Whether the hinge drives itself. `None` does nothing, `Motor` turns at a constant speed, "
                    .. "and `Servo` turns to an angle and holds it.",
            },
            {
                Name = "AngularVelocity",
                Type = "number",
                Default = "0",
                Doc = "For `Motor`: the speed the hinge turns at, in radians per second about its X axis. A "
                    .. "negative value turns the other way.",
            },
            {
                Name = "MotorMaxTorque",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Motor`: the largest torque the motor can apply, in newton meters. A load that needs "
                    .. "more than this stalls the motor.",
            },
            {
                Name = "MotorMaxAcceleration",
                Type = "number",
                Default = "100000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Motor`: how quickly the motor's speed can change, in radians per second squared. A "
                    .. "low value makes the motor wind up slowly, like a fan.",
            },
            {
                Name = "TargetAngle",
                Type = "number",
                Default = "0",
                Doc = "For `Servo`: the angle the hinge turns to, in degrees, measured the same way as the "
                    .. "limits. Speeds, by contrast, are in radians per second.",
            },
            {
                Name = "AngularSpeed",
                Type = "number",
                Default = "100000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Servo`: the highest speed the hinge turns at on its way to the target, in radians per "
                    .. "second. Near the target, the spring takes over.",
            },
            {
                Name = "ServoMaxTorque",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Servo`: the largest torque the servo can apply to reach its target and hold it, in "
                    .. "newton meters. A load that needs more than this pulls the hinge away from its target.",
            },
            {
                Name = "AngularResponsiveness",
                Type = "number",
                Default = "45",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "For `Servo`: how quickly the hinge closes the last of the distance to its target, as a "
                    .. "frequency in radians per second. Used only when `Stiffness` is 0. The hinge arrives "
                    .. "without overshooting. The value is tuned for the two parts the hinge connects; if more "
                    .. "parts hang from the joint, set `Stiffness` instead.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Servo`: the strength of the spring that pulls the hinge to its target, in newton "
                    .. "meters per radian. At 0, `AngularResponsiveness` is used instead. Above 0, this and "
                    .. "`Damping` set the response. For a load with a moment of inertia of `I` kilogram square "
                    .. "meters, `w * w * I` gives a response of `w` radians per second.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Servo`: how strongly the hinge resists turning, in newton meter seconds per radian. "
                    .. "Used together with `Stiffness`. Critical damping, which reaches the target without "
                    .. "overshooting, is `2 * math.sqrt(Stiffness * I)` for a moment of inertia of `I`. Less "
                    .. "overshoots and oscillates; more arrives slowly.",
            },
        },
    },
    {
        Name = "FixedConstraint",
        Extends = "Constraint",
        Module = "scene",
        -- A hook of its own, which does NOT add a second component: it stamps
        -- the kind onto the one `Constraint` already added. `World::create`
        -- walks the ancestry root-first, so the base has run by the time this
        -- does -- which is what lets the type be the class rather than a
        -- property somebody could write a hinge into.
        Storage = "FixedConstraint",
        Doc = "A joint that holds two parts rigidly together, so that the solver treats them as one object. "
            .. "Unlike a `Weld`, both parts take part in the simulation: their masses add, and a force on either "
            .. "part moves both. Use a `Weld` to attach something to a character, and a `FixedConstraint` to "
            .. "build an object out of several parts.",
        Properties = {},
    },
    {
        Name = "PrismaticConstraint",
        Extends = "Constraint",
        Module = "scene",
        Storage = "PrismaticConstraint",
        Doc = "A joint that lets two parts slide along one line and nothing else, such as a lift, a drawer or a "
            .. "piston. The line is the X axis of the attachments, so rotating an attachment aims the rail. With "
            .. "an actuator the joint moves by itself: a `Motor` holds a speed, and a `Servo` moves to a position "
            .. "and stays there.",
        Properties = {
            {
                Name = "LimitsEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `LowerLimit` and `UpperLimit` apply. When it is off, the joint slides without limit.",
            },
            {
                Name = "LowerLimit",
                Type = "number",
                Default = "0",
                Doc = "The lower limit of the slide, in meters from the position where the parts were joined.",
            },
            {
                Name = "UpperLimit",
                Type = "number",
                Default = "5",
                Doc = "The upper limit of the slide, in meters from the position where the parts were joined. If "
                    .. "`LowerLimit` is greater than `UpperLimit`, the slide is not limited.",
            },
            {
                Name = "ActuatorType",
                Type = "Enum.ActuatorType",
                Default = "Enum.ActuatorType.None",
                Doc = "Whether the joint drives itself. `None` does nothing, `Motor` moves at a constant speed, "
                    .. "and `Servo` moves to a position and holds it.",
            },
            {
                Name = "Velocity",
                Type = "number",
                Default = "0",
                Doc = "For `Motor`: the speed the joint moves at, in meters per second along the rail.",
            },
            {
                Name = "MotorMaxForce",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Motor`: the largest force the motor can apply, in newtons.",
            },
            {
                Name = "MotorMaxAcceleration",
                Type = "number",
                Default = "100000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Motor`: how quickly the motor's speed can change, in meters per second squared.",
            },
            {
                Name = "TargetPosition",
                Type = "number",
                Default = "0",
                Doc = "For `Servo`: the position the joint moves to, in meters along the rail from where the "
                    .. "parts were joined.",
            },
            {
                Name = "Speed",
                Type = "number",
                Default = "100000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Servo`: the highest speed the joint moves at on its way to the target, in meters per "
                    .. "second. Near the target, the spring takes over.",
            },
            {
                Name = "ServoMaxForce",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Servo`: the largest force the servo can apply to reach its target and hold it, in "
                    .. "newtons. A load that needs more than this is not moved.",
            },
            {
                Name = "LinearResponsiveness",
                Type = "number",
                Default = "45",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "For `Servo`: how quickly the joint closes the last of the distance to its target, as a "
                    .. "frequency in radians per second. Used only when `Stiffness` is 0. The joint arrives "
                    .. "without overshooting. The value is tuned for the two parts on the rail.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Servo`: the strength of the spring that pulls the joint to its target, in newtons per "
                    .. "meter. At 0, `LinearResponsiveness` is used instead. Above 0, this and `Damping` set the "
                    .. "response. For a load of `m` kilograms, `w * w * m` gives a response of `w` radians per "
                    .. "second.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "For `Servo`: how strongly the joint resists moving, in newton seconds per meter. Used "
                    .. "together with `Stiffness`. Critical damping, which reaches the target without "
                    .. "overshooting, is `2 * math.sqrt(Stiffness * m)` for a mass of `m` kilograms. Less "
                    .. "overshoots and oscillates; more arrives slowly.",
            },
        },
    },
    {
        Name = "RopeConstraint",
        Extends = "Constraint",
        Module = "scene",
        Storage = "RopeConstraint",
        Doc = "A joint that keeps two attachments no farther apart than `Length`, such as a rope, a chain or a "
            .. "leash. While it is slack, it does nothing. With the winch enabled, `Length` moves toward "
            .. "`WinchTarget` at `WinchSpeed`, and stalls if that needs more force than `WinchForce`.",
        Properties = {
            {
                Name = "Length",
                Type = "number",
                Default = "5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The largest distance between the two ends, in meters. The winch changes it.",
            },
            {
                Name = "WinchEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the winch is moving `Length` toward `WinchTarget`.",
            },
            {
                Name = "WinchTarget",
                Type = "number",
                Default = "5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The length the winch moves toward, in meters.",
            },
            {
                Name = "WinchSpeed",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The speed at which the winch takes rope in or lets it out, in meters per second.",
            },
            {
                Name = "WinchForce",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The largest tension the winch can pull against, in newtons. A heavier load is not moved.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(0.6, 0.6, 0.6)",
                Doc = "The color of the line the rope is drawn as.",
            },
            {
                Name = "Thickness",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The thickness of the line the rope is drawn as, in meters.",
            },
        },
    },
    {
        Name = "RodConstraint",
        Extends = "Constraint",
        Module = "scene",
        Storage = "RodConstraint",
        Doc = "A joint that holds two attachments exactly `Length` apart while letting each end rotate freely, "
            .. "such as a rod, a strut or a link in a mechanism.",
        Properties = {
            {
                Name = "Length",
                Type = "number",
                Default = "5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance at which the two ends are held, in meters.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(0.6, 0.6, 0.6)",
                Doc = "The color of the line the rod is drawn as.",
            },
            {
                Name = "Thickness",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The thickness of the line the rod is drawn as, in meters.",
            },
        },
    },
    {
        Name = "VehicleController",
        Extends = "Instance",
        Module = "scene",
        Parents = under("BasePart"),
        Storage = "VehicleController",
        Records = { "ADR 0213" },
        Doc = "Makes the part it is under a vehicle's chassis, held up by the `VehicleWheel`s under it and driven "
            .. "by an engine through a gearbox. Each wheel is a suspension and a tire. "
            .. "Set `Throttle`, `Steer`, `Brake` and `HandBrake` each frame, from a script or an input action, "
            .. "to drive it.\n\n"
            .. "The chassis must be unanchored. A wheel is not a part of the simulation: it is a cast from the "
            .. "chassis down to the ground, so it rolls over a curb a wheel part would bounce off. Give each "
            .. "wheel a `WheelPart` to see it turn and roll. Forward is the chassis' `LookVector`.\n\n"
            .. "It runs on the server, in the simulation, and is replicated as the chassis is.",
        Properties = {
            {
                Name = "MaxTorque",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The most torque the engine gives, in newton meters. At 0 it is what the chassis' mass needs to drive briskly without spinning its driven wheels.",
            },
            {
                Name = "MinRPM",
                Type = "number",
                Default = "1000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The engine's turns a minute when it idles.",
            },
            {
                Name = "MaxRPM",
                Type = "number",
                Default = "6000",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The most turns a minute the engine reaches. It is held under this.",
            },
            {
                Name = "Transmission",
                Type = "Enum.VehicleTransmission",
                Default = "Enum.VehicleTransmission.Automatic",
                Doc = "Whether the gearbox shifts by itself, at `ShiftUpRPM` and `ShiftDownRPM`, or stays in the `Gear` asked for.",
            },
            {
                Name = "ShiftUpRPM",
                Type = "number",
                Default = "4000",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "With an automatic gearbox: the engine's turns a minute at which it shifts up.",
            },
            {
                Name = "ShiftDownRPM",
                Type = "number",
                Default = "2000",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "With an automatic gearbox: the engine's turns a minute at which it shifts down.",
            },
            {
                Name = "FinalDriveRatio",
                Type = "number",
                Default = "3.42",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The ratio from the gearbox to the wheels. A higher ratio accelerates harder and reaches a lower top speed.",
            },
            {
                Name = "MaxLeanAngle",
                Type = "number",
                Default = "180",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How far the chassis can lean, in degrees, before the vehicle holds it upright. At 180 it can roll over.",
            },
            {
                Name = "Throttle",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_minus_one_to_one",
                Doc = "How hard the engine is asked to drive, from -1 (full reverse) to 1 (full forward). With an automatic gearbox, a negative value at rest selects reverse.",
            },
            {
                Name = "Steer",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_minus_one_to_one",
                Doc = "How far the steering wheels are turned, from -1 (full left) to 1 (full right).",
            },
            {
                Name = "Brake",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How hard the brakes are pressed, from 0 to 1.",
            },
            {
                Name = "HandBrake",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How hard the hand brake is pulled, from 0 to 1. It holds only the wheels that have a `MaxHandBrakeTorque`.",
            },
            {
                Name = "Gear",
                Type = "number",
                Default = "1",
                Doc = "With a manual gearbox: the gear asked for, -1 for reverse and 0 for neutral.",
            },
            {
                Name = "Driver",
                Type = "Player?",
                Doc = "The player who drives, or nil. While a player drives, their intents named by `DriveIntent`, "
                    .. "`BrakeIntent` and `HandBrakeIntent` set `Throttle`, `Steer`, `Brake` and `HandBrake` "
                    .. "every simulation tick, and no script needs to. In a match, the server sets it, and the "
                    .. "driver's own machine predicts the vehicle as it predicts their character, so the car "
                    .. "answers the wheel at once.",
            },
            {
                Name = "DriveIntent",
                Type = "string",
                Default = '"Drive"',
                Doc = "The name of the driver's `Direction2D` input action that drives: up and down set `Throttle`, "
                    .. "left and right set `Steer`.",
            },
            {
                Name = "BrakeIntent",
                Type = "string",
                Default = '"Brake"',
                Doc = "The name of the driver's input action that brakes: a button, or the first axis of a "
                    .. "direction.",
            },
            {
                Name = "HandBrakeIntent",
                Type = "string",
                Default = '"HandBrake"',
                Doc = "The name of the driver's input action that pulls the hand brake.",
            },
            {
                Name = "Speed",
                Type = "number",
                ReadOnly = true,
                Doc = "The chassis' speed along its forward direction at the last simulation tick, in meters per second; negative when it is going backward.",
            },
            {
                Name = "RPM",
                Type = "number",
                ReadOnly = true,
                Doc = "The engine's turns a minute at the last simulation tick.",
            },
            {
                Name = "CurrentGear",
                Type = "number",
                ReadOnly = true,
                Doc = "The gear the gearbox was in at the last simulation tick: -1 reverse, 0 neutral, then the forward gears from 1.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "VehicleWheel",
        Extends = "Instance",
        Module = "scene",
        Parents = under("VehicleController"),
        Storage = "VehicleWheel",
        Records = { "ADR 0213" },
        Doc = "One wheel of a vehicle, under its `VehicleController`. It hangs from `Attachment`, an attachment "
            .. "of the chassis, on a suspension that pushes the chassis up where the wheel meets the ground; "
            .. "its tire then pushes the chassis along and across by how much it slips.\n\n"
            .. "A wheel that steers has a `MaxSteerAngle`; one the engine turns is `Driven`. Driven wheels are "
            .. "paired in the order they are under the controller, left then right, so give a car's driven "
            .. "wheels left first.",
        Properties = {
            {
                Name = "Attachment",
                Type = "Attachment?",
                Doc = "The attachment of the chassis the suspension hangs from: the top of its travel. A wheel with no attachment, or with one on another part, is not part of the vehicle.",
            },
            {
                Name = "WheelPart",
                Type = "BasePart?",
                Doc = "A part the engine places where the wheel is, turned by its steering and rolling, after each simulation tick. It should not collide: the wheel itself is not a part.",
            },
            {
                Name = "Radius",
                Type = "number",
                Default = "0.35",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The wheel's radius, in meters.",
            },
            {
                Name = "Width",
                Type = "number",
                Default = "0.25",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The wheel's width, in meters.",
            },
            {
                Name = "SuspensionMinLength",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The shortest the suspension can be, in meters from the attachment to the wheel's center.",
            },
            {
                Name = "SuspensionMaxLength",
                Type = "number",
                Default = "0.35",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The longest the suspension can be, in meters from the attachment to the wheel's center.",
            },
            {
                Name = "SuspensionFrequency",
                Type = "number",
                Default = "1.5",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "How stiff the suspension is, as the times a second it would bounce. 1 is soft, 2 is firm, 3 is a race car.",
            },
            {
                Name = "SuspensionDamping",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much of a bounce the suspension takes away, from 0 (it bounces on) to 1 (it stops in one swing).",
            },
            {
                Name = "MaxSteerAngle",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How far the wheel turns at full `Steer`, in degrees. At 0 it does not steer.",
            },
            {
                Name = "Driven",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the engine turns this wheel.",
            },
            {
                Name = "MaxBrakeTorque",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The brake's torque at full `Brake`, in newton meters. At 0 it is enough for the chassis' mass.",
            },
            {
                Name = "MaxHandBrakeTorque",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The hand brake's torque at full `HandBrake`, in newton meters. At 0 the hand brake does not hold this wheel.",
            },
            {
                Name = "Grip",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How well the tire holds the ground, as a scale of the ground's own friction: below 1 it slides sooner, above 1 later.",
            },
            {
                Name = "Grounded",
                Type = "boolean",
                ReadOnly = true,
                Doc = "Whether the wheel touched the ground at the last simulation tick.",
            },
            {
                Name = "Compression",
                Type = "number",
                ReadOnly = true,
                Doc = "How far the suspension was pushed in at the last simulation tick, from 0 (fully out) to 1 (fully in).",
            },
            {
                Name = "Slip",
                Type = "number",
                ReadOnly = true,
                Doc = "How much the tire slid sideways at the last simulation tick, as the angle between where it points and where it goes, in degrees.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "SpringConstraint",
        Extends = "Constraint",
        Module = "scene",
        Storage = "SpringConstraint",
        Doc = "A spring between two attachments. It pulls them together when they are farther apart than "
            .. "`FreeLength` and pushes them apart when they are closer, with a force that grows with the "
            .. "distance. Use it for a suspension, a bouncy bridge or a door that closes by itself.",
        Properties = {
            {
                Name = "FreeLength",
                Type = "number",
                Default = "5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The length at which the spring applies no force, in meters.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "100",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The force the spring applies for each meter it is away from `FreeLength`, in newtons per meter.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The force that resists the spring's change of length, in newton seconds per meter. At 0 "
                    .. "the spring bounces indefinitely; a high value makes it settle slowly.",
            },
            {
                Name = "LimitsEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `MinLength` and `MaxLength` are hard limits on the spring's length.",
            },
            {
                Name = "MinLength",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The shortest the spring can be, in meters.",
            },
            {
                Name = "MaxLength",
                Type = "number",
                Default = "5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The longest the spring can be, in meters.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(0.6, 0.6, 0.6)",
                Doc = "The color of the line the spring is drawn as.",
            },
            {
                Name = "Thickness",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The thickness of the line the spring is drawn as, in meters.",
            },
        },
    },
    {
        Name = "LinearVelocity",
        Extends = "Constraint",
        Module = "scene",
        Storage = "LinearVelocity",
        Doc = "Keeps the part that `Attachment0` is on moving at a set velocity, using up to `MaxForce`. Use it "
            .. "for a crate on a conveyor, a gliding platform or a projectile that does not slow down. In `Line` "
            .. "and `Plane` modes it controls only the speed along a line or within a plane and leaves the rest "
            .. "of the motion free, so a puck held at a speed along a rail still falls.",
        Properties = {
            {
                Name = "VelocityConstraintMode",
                Type = "Enum.VelocityConstraintMode",
                Default = "Enum.VelocityConstraintMode.Vector",
                Doc = "Whether the constraint controls the whole velocity (`Vector`), only the speed along a line "
                    .. "(`Line`), or only the velocity within a plane (`Plane`).",
            },
            {
                Name = "VectorVelocity",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "For `Vector` mode: the velocity to hold, in meters per second, in the frame that "
                    .. "`RelativeTo` names.",
            },
            {
                Name = "LineDirection",
                Type = "vector",
                Default = "vector.create(1, 0, 0)",
                Doc = "For `Line` mode: the direction of the line. Its length is ignored.",
            },
            {
                Name = "LineVelocity",
                Type = "number",
                Default = "0",
                Doc = "For `Line` mode: the speed to hold along the line, in meters per second.",
            },
            {
                Name = "PrimaryTangentAxis",
                Type = "vector",
                Default = "vector.create(1, 0, 0)",
                Doc = "For `Plane` mode: the first of the two directions that define the plane.",
            },
            {
                Name = "SecondaryTangentAxis",
                Type = "vector",
                Default = "vector.create(0, 0, 1)",
                Doc = "For `Plane` mode: the second of the two directions that define the plane.",
            },
            {
                Name = "PlaneVelocity",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "For `Plane` mode: the speed to hold along each of the two plane directions, in meters per "
                    .. "second.",
            },
            {
                Name = "MaxForce",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The largest force the constraint can apply, in newtons, along each axis it controls: the "
                    .. "line, the two plane directions, or the three axes of the frame.",
            },
            {
                Name = "RelativeTo",
                Type = "Enum.ActuatorRelativeTo",
                Default = "Enum.ActuatorRelativeTo.World",
                Doc = "The frame the velocities are given in: the world, or an attachment. Relative to an "
                    .. 'attachment, "forward" follows the part as it turns.',
            },
        },
    },
    {
        Name = "AngularVelocity",
        Extends = "Constraint",
        Module = "scene",
        Storage = "AngularVelocity",
        Doc = "Keeps the part that `Attachment0` is on spinning at a set rate, using up to `MaxTorque`. Use it "
            .. "for a fan, a turning coin or a planet. It is solved together with the joints the part is in, so a "
            .. "hinged door turns about its hinge. For a wheel on an axle, a `HingeConstraint` with a motor is "
            .. "usually the better choice.",
        Properties = {
            {
                Name = "AngularVelocity",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "The spin to hold, in radians per second about each axis of the frame that `RelativeTo` names.",
            },
            {
                Name = "MaxTorque",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The largest torque the constraint can apply, in newton meters, about each axis of the part.",
            },
            {
                Name = "RelativeTo",
                Type = "Enum.ActuatorRelativeTo",
                Default = "Enum.ActuatorRelativeTo.World",
                Doc = "The frame the spin is given in: the world, or an attachment.",
            },
            {
                Name = "ReactionTorqueEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether an equal and opposite torque is applied to the part that `Attachment1` is on, like "
                    .. "a motor twisting its mount. With it on, the spin is measured between the two parts.",
            },
        },
    },
    {
        Name = "AlignPosition",
        Extends = "Constraint",
        Module = "scene",
        Storage = "AlignPosition",
        Doc = "Pulls `Attachment0` toward a position and holds it there. Use it for a pet that hovers at a "
            .. "shoulder, a platform that returns to its post or an object being carried. The target is "
            .. "`Position`, or `Attachment1` when `Mode` is `TwoAttachment`.\n\n"
            .. "It pulls with up to `MaxForce` and no faster than `MaxVelocity`. A load heavier than the force "
            .. "can hold sags below the target.",
        Properties = {
            {
                Name = "Mode",
                Type = "Enum.PositionAlignmentMode",
                Default = "Enum.PositionAlignmentMode.OneAttachment",
                Doc = "Whether the target is `Position` (`OneAttachment`) or `Attachment1` (`TwoAttachment`).",
            },
            {
                Name = "Position",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "For `OneAttachment`: the position in world space to pull toward.",
            },
            {
                Name = "MaxForce",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The largest force the constraint can apply, in newtons, along each world axis. With "
                    .. "`ReactionForceEnabled`, the axes are those of `Attachment1`.",
            },
            {
                Name = "MaxVelocity",
                Type = "number",
                Default = "100000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The highest speed at which the attachment is moved toward the target, in meters per "
                    .. "second. Used only when `Stiffness` is 0. Near the target, the spring takes over.",
            },
            {
                Name = "Responsiveness",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "How quickly the constraint closes the distance to the target, as a frequency in radians "
                    .. "per second. Used only when `Stiffness` is 0. A low value drifts in; a high value snaps. "
                    .. "The part arrives without overshooting. The value suits a single part; for a part with "
                    .. "others jointed to it, set `Stiffness` instead.",
            },
            {
                Name = "RigidityEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the constraint moves the part to the target as fast as the simulation allows, "
                    .. "ignoring the force and speed limits.",
            },
            {
                Name = "ApplyAtCenterOfMass",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the force is applied at the part's center of mass instead of at the attachment. "
                    .. "When it is off, a pull at a corner also turns the part.",
            },
            {
                Name = "ReactionForceEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "For `TwoAttachment`: whether an equal and opposite force is applied to the part that "
                    .. "`Attachment1` is on. When it is off, the target is not disturbed by what follows it.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The strength of the spring that pulls the part to the target, in newtons per meter. At 0, "
                    .. "`Responsiveness` is used instead. Above 0, this and `Damping` set the response. For a "
                    .. "load of `m` kilograms, `w * w * m` gives a response of `w` radians per second.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How strongly the constraint resists motion, in newton seconds per meter. Used together "
                    .. "with `Stiffness`. Critical damping, which reaches the target without overshooting, is `2 "
                    .. "* math.sqrt(Stiffness * m)` for a mass of `m` kilograms. Less overshoots and oscillates; "
                    .. "more arrives slowly.",
            },
        },
    },
    {
        Name = "AlignOrientation",
        Extends = "Constraint",
        Module = "scene",
        Storage = "AlignOrientation",
        Doc = "Turns `Attachment0` toward an orientation and holds it there. Use it to keep something upright, to "
            .. "make a turret face its target or to make a pet look where its owner looks. The target is the "
            .. "rotation of `CFrame`, or of `Attachment1` when `Mode` is `TwoAttachment`.\n\n"
            .. "It does not spin a part continuously; for that, use an `AngularVelocity` or a hinge with a motor.",
        Properties = {
            {
                Name = "Mode",
                Type = "Enum.OrientationAlignmentMode",
                Default = "Enum.OrientationAlignmentMode.OneAttachment",
                Doc = "Whether the target is `CFrame` (`OneAttachment`) or `Attachment1` (`TwoAttachment`).",
            },
            {
                Name = "CFrame",
                Type = "CFrame",
                Default = "CFrame.identity",
                Doc = "For `OneAttachment`: the orientation in world space to turn toward. Only the rotation is used.",
            },
            {
                Name = "MaxTorque",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The largest torque the constraint can apply, in newton meters, about each axis of the part.",
            },
            {
                Name = "MaxAngularVelocity",
                Type = "number",
                Default = "100000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The highest speed at which the part is turned toward the target, in radians per second. "
                    .. "Used only when `Stiffness` is 0. Near the target, the spring takes over.",
            },
            {
                Name = "Responsiveness",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "How quickly the constraint turns the part toward its target, as a frequency in radians per "
                    .. "second. Used only when `Stiffness` is 0. The part arrives without overshooting. The value "
                    .. "suits a single part; for a part with others jointed to it, such as the hips of a "
                    .. "character, set `Stiffness` instead.",
            },
            {
                Name = "RigidityEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the constraint turns the part to the target as fast as the simulation allows, "
                    .. "ignoring the torque and speed limits.",
            },
            {
                Name = "ReactionTorqueEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "For `TwoAttachment`: whether an equal and opposite torque is applied to the part that "
                    .. "`Attachment1` is on. An arm aligned to a chest then also turns the chest.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The strength of the spring that turns the part to the target, in newton meters per radian. "
                    .. "At 0, `Responsiveness` is used instead. Above 0, this and `Damping` set the response. For "
                    .. "a load with a moment of inertia of `I` kilogram square meters, `w * w * I` gives a "
                    .. "response of `w` radians per second.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How strongly the constraint resists turning, in newton meter seconds per radian. Used "
                    .. "together with `Stiffness`. Critical damping, which reaches the target without "
                    .. "overshooting, is `2 * math.sqrt(Stiffness * I)` for a moment of inertia of `I`. Less "
                    .. "overshoots and oscillates; more arrives slowly.",
            },
        },
    },
    {
        Name = "VectorForce",
        Extends = "Constraint",
        Module = "scene",
        Storage = "VectorForce",
        Doc = "Applies a constant force to the part that `Attachment0` is on. Use it for a thruster, for wind on "
            .. "one object or for lift.",
        Properties = {
            {
                Name = "Force",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "The force, in newtons, in the frame that `RelativeTo` names.",
            },
            {
                Name = "RelativeTo",
                Type = "Enum.ActuatorRelativeTo",
                Default = "Enum.ActuatorRelativeTo.Attachment0",
                Doc = "The frame the force is given in. `Attachment0`, the default, makes a thruster: the force "
                    .. "follows the part as it turns.",
            },
            {
                Name = "ApplyAtCenterOfMass",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the force is applied at the part's center of mass instead of at the attachment. "
                    .. "When it is off, a force away from the center also turns the part.",
            },
        },
    },
    {
        Name = "Torque",
        Extends = "Constraint",
        Module = "scene",
        Storage = "Torque",
        Doc = "Applies a constant torque to the part that `Attachment0` is on.",
        Properties = {
            {
                Name = "Torque",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "The torque, in newton meters about each axis of the frame that `RelativeTo` names.",
            },
            {
                Name = "RelativeTo",
                Type = "Enum.ActuatorRelativeTo",
                Default = "Enum.ActuatorRelativeTo.Attachment0",
                Doc = "The frame the torque is given in: the world, or an attachment.",
            },
        },
    },
    {
        Name = "NoCollisionConstraint",
        Extends = "Instance",
        Module = "scene",
        Parents = InTheWorld,
        Storage = "NoCollisionConstraint",
        Doc = "Stops two specific parts from colliding with each other. Both still collide with everything else. "
            .. "Use it where a collision group is too broad, since a group applies to every part in it: an object "
            .. "and the character carrying it, or a door and its frame.",
        Properties = {
            {
                Name = "Part0",
                Type = "BasePart?",
                Doc = "The first of the two parts.",
            },
            {
                Name = "Part1",
                Type = "BasePart?",
                Doc = "The second of the two parts.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the two parts pass through each other. When it is off, they collide again.",
            },
        },
    },
    {
        Name = "Ragdoll",
        Extends = "Instance",
        Module = "scene",
        Parents = under("MeshPart"),
        Storage = "Ragdoll",
        Doc = "Makes a character's pose follow the physics simulation instead of an animation. Parent it to the "
            .. "skinned `MeshPart`, put the limb parts under it, and enable it.\n\n"
            .. "A ragdoll is made of ordinary instances: a `Part` for each limb, a `Bone` on each part that names "
            .. "the joint the part drives, and constraints between the parts. `Ragdoll` only tells the engine to "
            .. "take the pose from them. Every `Bone` under the ragdoll that names a joint and sits on a part "
            .. "drives that joint.\n\n"
            .. "Joints that no bone drives keep their place relative to their parent, so a partial ragdoll works: "
            .. "simulate a dozen joints and the fingers follow the wrist.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the pose comes from the simulation. Turning it off returns the character to the "
                    .. "animation that is playing, from the next tick.",
            },
            {
                Name = "Blend",
                Type = "number",
                Default = "1",
                Doc = "How much of the pose comes from the simulation, from 0 (none) to 1 (all).\n\n"
                    .. "The blend is between two poses: each driven joint is drawn between where the animation "
                    .. "puts it and where its limb part is. It does not weaken the simulation; the limb parts "
                    .. "fall the same way at any value. Ramp it from 0 to 1 over a few tenths of a second to make "
                    .. "a character go down without a visible snap, and back to 0 to make it get up.",
            },
        },
        Methods = {
            {
                Name = "Build",
                Parameters = { { Name = "profile", Type = "{ any }" } },
                Returns = { "number" },
                Doc = "Creates the limbs of the ragdoll from a profile and returns how many it made. For each "
                    .. "joint the profile names, it creates a `Part`, a `Bone` on the part that names the joint, "
                    .. "and a constraint to the limb above. The limbs are created at the rig's current pose.\n\n"
                    .. "Everything it creates is an ordinary instance that you can select, move, adjust and "
                    .. "delete. Run it once in the editor and save the scene, and the ragdoll is part of the "
                    .. "scene with no script needed.\n\n"
                    .. 'A profile is an ordered list of limbs. `require("@engine/ragdoll").Humanoid` is a '
                    .. "built-in profile that fits common humanoid rigs. Each entry is a table:\n\n"
                    .. "- `Joint`: the joint's name, or a list of names tried in order.\n"
                    .. "- `Parent`: the 1-based index of the limb this one hangs from; omit it for the root. It "
                    .. "must refer to an earlier entry.\n"
                    .. "- `Radius`, `LeafLength`: the capsule's radius, and the length of a limb that has nothing "
                    .. "below it. Otherwise a limb's length is the distance to the joint below it.\n"
                    .. '- `Kind`: `"BallSocket"`, `"Hinge"` or `"Fixed"`.\n'
                    .. "- `Swing`, `Twist`: for a ball socket, the cone half-angle and the twist range, in "
                    .. "degrees.\n"
                    .. "- `LimitLow`, `LimitHigh`: for a hinge, the range in degrees.\n\n"
                    .. "A limb whose joint the rig does not have is skipped, and its children hang from the "
                    .. "nearest limb that was built. Raises an error if the ragdoll is not under a skinned "
                    .. "`MeshPart`, if it already has limbs, if the profile is malformed, or if the rig has none "
                    .. "of the profile's joints.",
            },
        },
    },
    {
        Name = "Weld",
        Extends = "Instance",
        Module = "scene",
        Parents = InTheWorld,
        Storage = "Weld",
        Doc = "Holds one part at a fixed offset from another. `Part1` is no longer simulated on its own; every "
            .. "tick it is moved to follow `Part0`. Use it to attach something to a character or to another "
            .. "moving part.\n\n"
            .. "A weld does not make the physics solver treat the two parts as one object; use a "
            .. "`FixedConstraint` for that. A weld is refused if its two parts are already connected by other "
            .. "welds, directly or through a chain.",
        Properties = {
            {
                Name = "Part0",
                Type = "Instance?",
                Doc = "The part or attachment that `Part1` follows. It can be simulated, anchored or a "
                    .. "`CharacterBody`. Use a `Bone` here to attach an object to a joint of an animated "
                    .. "character, such as a sword to a hand. Only a `BasePart` or an `Attachment` is accepted.",
            },
            {
                Name = "Part1",
                Type = "BasePart?",
                Doc = "The part that follows `Part0`. While the weld is enabled, only the weld moves it: gravity "
                    .. "does not affect it, and a `CFrame` you set is overwritten at the next tick. It must be a "
                    .. "part, not an attachment.",
            },
            {
                Name = "C0",
                Type = "CFrame",
                Doc = "The offset of the weld on `Part0`. The weld keeps `Part0.CFrame * C0` equal to "
                    .. "`Part1.CFrame * C1`.",
            },
            {
                Name = "C1",
                Type = "CFrame",
                Doc = "The offset of the weld on `Part1`. The weld keeps `Part0.CFrame * C0` equal to "
                    .. "`Part1.CFrame * C1`.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the weld holds. Disabling it returns `Part1` to the simulation where it is, with "
                    .. "no velocity.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Terrain",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Workspace"),
        Storage = "Terrain",
        Doc = "A sculpted landscape that collides: ground that you dig into and build up, instead of a floor made "
            .. "of parts.\n\n"
            .. "Terrain is a grid of voxels. Space is divided into cubes `VoxelSize` on a side. Each voxel holds "
            .. "a material and an occupancy, which is how full of that material it is, from 0 to 1. The surface "
            .. "lies where the occupancy crosses one half, interpolated between neighboring voxels, so the ground "
            .. "is smooth and not stepped. Caves, arches, overhangs and flat ground are all the same data.\n\n"
            .. "Materials are numbered. 0 is air, and `n` is the terrain's layer `n`, a material asset (see "
            .. "`GetLayers`). A new terrain has no layers and draws plain gray ground until `SetLayers` gives it "
            .. "some.\n\n"
            .. "A `Workspace` has one terrain, `workspace.Terrain`. You can create another, but the workspace "
            .. "does not refer to it.\n\n"
            .. "Every editing method writes voxels, and voxels are part of the world: an edit can be undone in "
            .. "the editor and is saved with the project.",
        Properties = {
            {
                Name = "VoxelSize",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.terrain_not_empty",
                Doc = "The size of one voxel, in meters, from 0.1 to 64. One meter resolves a doorway or a path. "
                    .. "Two meters resolves a hillside and uses an eighth of the memory.\n\n"
                    .. "It can be set only while the terrain is empty. Call `Clear` first to change it on a "
                    .. "sculpted terrain.",
            },
            {
                Name = "CellSize",
                Type = "number",
                ReadOnly = true,
                Default = "64",
                Inert = true,
                Doc = "The width of one streamed cell of terrain, in meters. It is the spacing of the world's "
                    .. "streaming grid.\n\n"
                    .. "A terrain saved with a scene that spans 256 cells or more is divided into cells of this "
                    .. "size, rounded to whole columns of 32-voxel chunks. Each cell is loaded and unloaded "
                    .. "around the focuses of `StreamingService`, at its terrain radii. A cell that was changed "
                    .. "during a run is written to the run's cache on disk when it is unloaded, and loads again "
                    .. "with the change.",
            },
            {
                Name = "Position",
                Type = "vector",
                Default = "vector.zero",
                Doc = "The position of the terrain's origin in the world, in meters. Every voxel, collider and "
                    .. "edit is offset by it, so moving a sculpted terrain is cheap. A terrain can be moved but "
                    .. "not rotated.",
            },
            {
                Name = "MinHeight",
                Type = "number",
                Default = "-256",
                Doc = "The lowest height, in meters, at which a voxel can be written.\n\n"
                    .. "Ground created where there was none, by `WriteHeights`, by the editor's Generate Flat "
                    .. "Ground or by raising empty terrain, is a slab 32 meters deep below its lowest surface, "
                    .. "rounded down to a whole chunk and never below this height.",
            },
            {
                Name = "MaxHeight",
                Type = "number",
                Default = "256",
                Doc = "The highest height, in meters, at which a voxel can be written.",
            },
            {
                Name = "CellCount",
                Type = "number",
                ReadOnly = true,
                Doc = "The number of 32 by 32 by 32 voxel chunks that currently hold any ground. Air is not "
                    .. "stored, so the count is 0 for an empty terrain.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "IsMeshed",
                Parameters = {
                    { Name = "center", Type = "vector" },
                    { Name = "radius", Type = "number" },
                },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0159" },
                Doc = "Returns whether the ground within `radius` of `center` is drawn in its final form. That "
                    .. "means every piece the view needs there has its mesh built and uploaded at the detail it "
                    .. "will be drawn at, and no further refinement is pending.\n\n"
                    .. "Ground that a script has just written exists at once and is meshed a few frames later; "
                    .. "this method reports the second. The answer is as of the last frame drawn. It is always "
                    .. "true where nothing is drawn, such as on a server or in a run with no camera.",
            },
            {
                Name = "WaitForMeshAsync",
                Parameters = {
                    { Name = "center", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "timeout", Type = "number?", Default = "30" },
                },
                Returns = { "boolean" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Records = { "ADR 0159" },
                Doc = "Yields until `IsMeshed(center, radius)` is true and returns true, or returns false after "
                    .. "`timeout` seconds of simulation time. Use it in a loading screen after building a "
                    .. "terrain, before letting the player in. Together with `SceneService:HoldLoading`, it keeps "
                    .. "the engine's loading screen up until the ground is ready.",
            },
            {
                Name = "FillBall",
                Parameters = {
                    { Name = "center", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "material", Type = "number" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Adds a ball of ground, or removes one when `material` is 0. Returns the number of voxels "
                    .. "it changed.\n\n"
                    .. "Adding never removes ground, and removing never adds it. A voxel ends up as full as the "
                    .. "fuller of itself and the ball when adding, or as empty as the ball leaves it when "
                    .. "removing. An explosion crater is `FillBall(hit.Position, 4, 0)`.\n\n"
                    .. "Raises an error for a position or size that is not a finite number, or for a negative "
                    .. "radius. This applies to every `Terrain` method that edits or reads. A return value of 0 "
                    .. "means the ground was already as requested, or the edit was entirely outside `MinHeight` "
                    .. "to `MaxHeight`.",
            },
            {
                Name = "FillBlock",
                Parameters = {
                    { Name = "center", Type = "vector" },
                    { Name = "size", Type = "vector" },
                    { Name = "material", Type = "number" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Adds an axis-aligned box of ground `size` across, or removes one when `material` is 0. "
                    .. "Returns the number of voxels it changed.",
            },
            {
                Name = "FillCylinder",
                Parameters = {
                    { Name = "center", Type = "vector" },
                    { Name = "height", Type = "number" },
                    { Name = "radius", Type = "number" },
                    { Name = "material", Type = "number" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Adds an upright cylinder of ground `height` tall, or removes one when `material` is 0. "
                    .. "Returns the number of voxels it changed. A well is `FillCylinder(top - vector.create(0, "
                    .. "10, 0), 20, 1.5, 0)`.",
            },
            {
                Name = "RaiseBall",
                Parameters = {
                    { Name = "center", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "amount", Type = "number" },
                    { Name = "material", Type = "number?" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Raises the ground around `center` by `amount` meters at the middle, falling smoothly to "
                    .. "nothing at `radius`. A negative `amount` lowers it. Returns the number of voxels it "
                    .. "changed.\n\n"
                    .. "It moves the existing surface instead of adding a ball. The ground within `radius` above "
                    .. "and below `center` is pushed up, so a hill rises with no overhang at its rim, and a "
                    .. "tunnel farther down stays where it is. Use it to shape hills and valleys; use `FillBall` "
                    .. "for a boulder or a tunnel.\n\n"
                    .. "Where a column within reach has no ground and `material` is given, raising creates new "
                    .. "ground of that material from the height of `center` upward.",
            },
            {
                Name = "GrowBall",
                Parameters = {
                    { Name = "center", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "amount", Type = "number" },
                    { Name = "material", Type = "number?" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Moves the surface near `center` outward along its own slope by `amount` meters at the "
                    .. "middle, falling smoothly to nothing at `radius`. A negative `amount` moves it inward. "
                    .. "Returns the number of voxels it changed.\n\n"
                    .. "This is what the editor's Raise and Lower tools do: a field rises, a cliff moves forward, "
                    .. "and an overhang grows downward. New ground is made of `material`, or of the neighboring "
                    .. "ground's material when none is given.",
            },
            {
                Name = "SmoothBall",
                Parameters = {
                    { Name = "center", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "strength", Type = "number?" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Smooths the ground in a ball. Each point of the surface moves toward the average of the "
                    .. "surface around it by `strength` (0 to 1, default 0.5), less toward the rim. Returns the "
                    .. "number of voxels it changed.\n\n"
                    .. "Flat ground does not move, and a slab is never cut through. Material removed from a bump "
                    .. "is spread around it, so the volume of ground stays nearly constant over repeated strokes. "
                    .. "The average is taken over a width of half the brush, at most four voxels.",
            },
            {
                Name = "FlattenBall",
                Parameters = {
                    { Name = "center", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "height", Type = "number" },
                    { Name = "strength", Type = "number?" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Pulls the ground in a ball toward a level plane at `height`, removing what is above the "
                    .. "plane and filling what is below it. `strength` (0 to 1, default 1) sets how far, less "
                    .. "toward the rim. Returns the number of voxels it changed.",
            },
            {
                Name = "PaintBall",
                Parameters = {
                    { Name = "center", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "material", Type = "number" },
                    {
                        Name = "options",
                        Type = "{ Mode: Enum.TerrainPaintMode?, Strength: number?, Falloff: number? }?",
                        Default = "nil",
                    },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0114" },
                Doc = "Changes the material of the ground in a ball without moving the surface. Returns the "
                    .. "number of voxels it changed.\n\n"
                    .. "A voxel holds two materials, one over the other, and how much of the upper one shows. "
                    .. "`options.Mode` chooses what the stroke does with them:\n\n"
                    .. "- `Replace`, the default, makes the ground `material` outright.\n"
                    .. "- `Blend` shows more of `material` over what is there, by `Strength` (0 to 1, default 1). "
                    .. "`Falloff` is the share of the radius over which the stroke fades toward the rim: 0, the "
                    .. "default, is a hard edge, 0.5 fades over the outer half, and 1 fades from the center.\n"
                    .. "- `Under` puts `material` under what was painted over it.\n"
                    .. "- `Erase` shows less of the upper material, revealing the one under it.\n\n"
                    .. "A `material` of 0 raises an error, except in `Erase` mode, which takes no material.",
            },
            {
                Name = "ReplaceMaterial",
                Parameters = {
                    { Name = "minCorner", Type = "vector" },
                    { Name = "maxCorner", Type = "vector" },
                    { Name = "from", Type = "number" },
                    { Name = "to", Type = "number" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Changes every voxel of material `from` to material `to` in the box between two corners. "
                    .. "Returns the number of voxels it changed. Raises an error if either material is 0: this "
                    .. "method changes what the ground is made of, not whether it is there.",
            },
            {
                Name = "ReadVoxels",
                Parameters = {
                    { Name = "minCorner", Type = "vector" },
                    { Name = "maxCorner", Type = "vector" },
                },
                Returns = { "{ number }", "{ number }", "vector" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the voxels in the box between two corners, snapped outward to the grid. The "
                    .. "results are their materials, their occupancies (0 to 1) and the number of voxels along "
                    .. "each axis.\n\n"
                    .. "The two tables are flat. The voxel at `x, y, z`, counted from 0 at the low corner, is at "
                    .. "index `1 + x + size.X * (y + size.Y * z)`. One call reads at most 256 voxels along each "
                    .. "axis; read a larger region in pieces.",
            },
            {
                Name = "WriteVoxels",
                Parameters = {
                    { Name = "corner", Type = "vector" },
                    { Name = "size", Type = "vector" },
                    { Name = "materials", Type = "{ number }" },
                    { Name = "occupancies", Type = "{ number }" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Writes a box of voxels in one call, in the layout that `ReadVoxels` returns. `corner` is a "
                    .. "point in the first voxel, `size` is the number of voxels along each axis, and each table "
                    .. "holds one entry per voxel. Returns the number of voxels it changed.\n\n"
                    .. "A material of 0 or an occupancy of 0 is air. Writing what `ReadVoxels` returned restores "
                    .. "the terrain exactly. Use it to copy a piece of terrain, to generate terrain voxel by "
                    .. "voxel, or to undo a script's own change.",
            },
            {
                Name = "WorldToCell",
                Parameters = {
                    { Name = "position", Type = "vector" },
                },
                Returns = { "vector" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the voxel that a world position falls in, as its integer index along each axis.",
            },
            {
                Name = "CellCenterToWorld",
                Parameters = {
                    { Name = "cell", Type = "vector" },
                },
                Returns = { "vector" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the world position of the center of a voxel, given its index along each axis.",
            },
            {
                Name = "HeightAt",
                Parameters = {
                    { Name = "x", Type = "number" },
                    { Name = "z", Type = "number" },
                },
                Returns = { "number?" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the height of the top of the ground at a position, in meters, or nil where there "
                    .. "is no ground.\n\n"
                    .. "The result is the highest surface in the column, even above a cave. That is where to "
                    .. "plant a tree or where a player lands from above. For the first surface along a direction, "
                    .. "use `Workspace:Raycast`.\n\n"
                    .. "It also returns nil where the ground has not streamed in yet. In a large world, wait for "
                    .. "the area with `StreamingService:LoadAreaAsync(position, radius)` first.",
            },
            {
                Name = "WriteHeights",
                Parameters = {
                    { Name = "corner", Type = "vector" },
                    { Name = "columns", Type = "number" },
                    { Name = "heights", Type = "{ number }" },
                    { Name = "material", Type = "(number | { number })?" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Writes a heightmap in one call: one height per column of voxels, `columns` to a row, row "
                    .. "after row along +Z, starting at the column that `corner` falls in. Only the X and Z of "
                    .. "`corner` are used. Returns the number of voxels it changed.\n\n"
                    .. "Use it for ground that comes from data, such as generated noise or an image, where one "
                    .. "brush call per column would be far too slow. Heights are in meters in world space and are "
                    .. "clamped between `MinHeight` and `MaxHeight`. A height that is not a number is skipped.\n\n"
                    .. "`material` (default 1) is what new ground is made of: one id for all of it, or a table of "
                    .. "ids parallel to `heights`, in which 0 leaves that column unchanged.\n\n"
                    .. "The top of each column moves to its height: ground is added from the old top upward or "
                    .. "removed from it downward, and a cave below the top is kept. A column with no ground at "
                    .. "all is created as a slab 32 meters deep below the lowest height in the table.",
            },
            {
                Name = "Clear",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes every voxel. The terrain is then empty and `VoxelSize` can be set again.",
            },
            {
                Name = "GetLayers",
                Parameters = {},
                Returns = { "{ string }" },
                ThreadSafety = "Safe",
                Doc = "Returns the terrain's layers: a list of material URNs in which entry `n` is the material "
                    .. "that voxels of material `n` are drawn with. A new terrain has none.\n\n"
                    .. "A scene saved by a version of the engine in which terrains had built-in layers returns "
                    .. "those eight, `engine://terrain/grass` to `engine://terrain/ice`.",
            },
            {
                Name = "SetLayers",
                Parameters = { { Name = "layers", Type = "{ string }" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets the terrain's layers, in the form that `GetLayers` returns: up to 255 material URNs, "
                    .. "entry `n` for material `n`. Voxels keep their numbers, so replacing entry 3 changes every "
                    .. "voxel of material 3 at once. To paint with a new material, add it at the end. Layers are "
                    .. "saved with the scene.",
            },
            {
                Name = "GetRules",
                Parameters = {},
                Returns = {
                    "{ { Enabled: boolean, Material: number, SlopeMin: number, SlopeMax: number, HeightMin: number, HeightMax: number, Blend: number, Noise: number, AppliesTo: { number } } }",
                },
                ThreadSafety = "Safe",
                Doc = "Returns the rules that choose the ground's material by slope and height, in the order they "
                    .. "are applied. A new terrain has none.\n\n"
                    .. "Each rule draws `Material` where the ground is between `SlopeMin` and `SlopeMax` degrees "
                    .. "and between `HeightMin` and `HeightMax` meters. `AppliesTo` lists the layers it draws "
                    .. "over; an empty list means every layer except its own. `Blend` is the width of its edge, "
                    .. "in degrees or meters, and `Noise` is how irregular the edge is.\n\n"
                    .. "Rules change what is drawn, not the voxels: the voxels keep their materials until "
                    .. "`ApplyRules`. A raycast reports the material that is drawn.",
            },
            {
                Name = "SetRules",
                Parameters = {
                    {
                        Name = "rules",
                        -- `read`: a rule from `GetRules` has every field, and one
                        -- written by hand only some; read-only fields are
                        -- covariant, so both are accepted.
                        Type = "{ { read Enabled: boolean?, read Material: number?, read SlopeMin: number?, "
                            .. "read SlopeMax: number?, read HeightMin: number?, read HeightMax: number?, "
                            .. "read Blend: number?, read Noise: number?, read AppliesTo: { number }? } }",
                    },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets the terrain's rules, in the form that `GetRules` returns: up to 16, applied in order. "
                    .. "A field left out takes its default: `Enabled` true, slopes 0 to 90, no height limits, "
                    .. "`Blend` 10 and `Noise` 0.12. An empty list removes all rules. Rules are saved with the "
                    .. "scene.",
            },
            {
                Name = "ApplyRules",
                Parameters = {
                    { Name = "minCorner", Type = "vector" },
                    { Name = "maxCorner", Type = "vector" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Writes the materials that the rules draw into the surface voxels in the box between two "
                    .. "corners. The rules can then be removed with no visible change. Returns the number of "
                    .. "voxels it changed.",
            },
            {
                Name = "Compact",
                Parameters = {},
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Does nothing and returns 0. Terrain storage is always compact. The method is kept so that "
                    .. "scripts that call it still run.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "WeldConstraint",
        Extends = "Instance",
        Module = "scene",
        Parents = InTheWorld,
        Storage = "WeldConstraint",
        Doc = "Holds one part at the offset from another that the two have when the weld is enabled. It behaves "
            .. "like a `Weld`, but the offset is captured from where the parts are instead of being set through "
            .. "`C0` and `C1`. Use it to join two parts that are already in the right place.",
        Properties = {
            {
                Name = "Part0",
                Type = "BasePart?",
                Doc = "The part that `Part1` follows.",
            },
            {
                Name = "Part1",
                Type = "BasePart?",
                Doc = "The part that follows `Part0`.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the weld holds. Each time it changes from false to true, the offset between the "
                    .. "parts is captured again. To weld a part somewhere else, disable the weld, move the part "
                    .. "and enable it again.",
            },
            {
                Name = "Active",
                Type = "boolean",
                ReadOnly = true,
                Doc = "Whether the weld is currently holding: it is enabled, both parts are set, and both are in "
                    .. "the world. A weld with one part missing is not an error; it is inactive until the other "
                    .. "part is set.",
            },
        } :: { Schema.Property },
    },
    {
        -- Module `scene` rather than `physics`, and that is layering rather
        -- than taste: `physics` is L2 and `scene` is L3, so the physics module
        -- cannot register a class into the scene registry at all. The class is
        -- storage; the meaning is applied by the glue above both, exactly as
        -- `MeshPart` holds a URN that `render` interprets.
        Name = "CharacterBody",
        Extends = "BasePart",
        Module = "scene",
        Storage = "CharacterBody",
        Doc = "A capsule that walks: a player's character, or anything that should climb a ramp and step over a "
            .. "curb instead of tumbling. It is a character controller, not a rigid body. It moves at the "
            .. "velocity your script gives it through `Move` and never tips over.\n\n"
            .. "The whole character is this one instance.\n\n"
            .. "Two characters block each other, and neither pushes the other. Walking into a character that is "
            .. "standing still stops you and leaves it in place. To shove or knock back another character, move "
            .. "it from a script. Whether two characters collide is decided by `CollisionGroup`, as for any two "
            .. "parts.",
        Properties = {
            {
                Name = "WalkSpeed",
                Type = "number",
                Default = "16",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The speed at which `Move` moves the character along the ground, in meters per second. The "
                    .. "direction given to `Move` is multiplied by this, so a direction of length 0.5 walks at "
                    .. "half speed.",
            },
            {
                Name = "JumpSpeed",
                Type = "number",
                Default = "8",
                Doc = "The upward speed that `Jump` gives the character, in meters per second. The height of the "
                    .. "jump depends on `Workspace.Gravity`. A negative value launches the character downward.",
            },
            {
                Name = "GravityScale",
                Type = "number",
                Default = "1",
                Doc = "How much of `Workspace.Gravity` affects the character. 1 is all of it, 0.5 gives a floaty "
                    .. "jump, and 0 is none. With a negative value the character falls upward.",
            },
            {
                Name = "SwimSpeed",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The speed at which `Move` moves the character through a fluid, in meters per second.",
            },
            {
                Name = "FlySpeed",
                Type = "number",
                Default = "16",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The speed at which `Move` moves the character while `Flying` is on, in meters per second.",
            },
            {
                Name = "Flying",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the character flies: gravity does not affect it, and `Move` uses all three axes at "
                    .. "`FlySpeed`. Use it for a creative mode, a ghost or a jetpack.",
            },
            {
                Name = "MaxSlopeAngle",
                Type = "number",
                Default = "46",
                ErrorKey = "scene.err.number_slope_degrees",
                Doc = "The steepest ground the character can stand and walk on, in degrees from horizontal. On "
                    .. "steeper ground it slides down.",
            },
            {
                Name = "AutoStepHeight",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The height of the tallest ledge the character steps over instead of being stopped by, in "
                    .. "meters. At 0, a curb stops it.",
            },
            {
                Name = "Grounded",
                Type = "boolean",
                ReadOnly = true,
                Doc = "Whether the character was standing on walkable ground at the last simulation tick. It is "
                    .. "true exactly when `State` is `Grounded`.",
            },
            {
                Name = "FloorMaterial",
                Type = "Material?",
                ReadOnly = true,
                Doc = "The material the character is standing on: the material of the part under it, or the "
                    .. "terrain layer drawn under its feet. It is what `RaycastResult.Material` reports for a ray "
                    .. "cast straight down.\n\n"
                    .. "It is nil in the air, in water and on a part with no material. Read its `FootstepSound` "
                    .. "for a footstep, and its `Friction` or `Tags` to treat ice or mud differently.",
            },
            {
                Name = "State",
                Type = "Enum.CharacterState",
                ReadOnly = true,
                Doc = "What the character is doing.\n\n"
                    .. "- `Grounded`: standing on walkable ground.\n"
                    .. "- `Airborne`: not supported. Ground too steep to walk on also reads as `Airborne`.\n"
                    .. "- `Swimming`: its middle is below the surface of a `Water` or inside a fluid block. The "
                    .. "character enters and leaves this state by itself.\n- `Flying`: `Flying` is on.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Move",
                Parameters = { { Name = "direction", Type = "vector" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets the direction the character moves in during the next simulation tick, in world space. "
                    .. "Call it every tick while the character should move; a character that is not told to move "
                    .. "stops.\n\n"
                    .. "On foot, only the horizontal part of the direction is used; gravity and `Jump` control "
                    .. "the vertical motion. The direction is multiplied by `WalkSpeed` and is not normalized, so "
                    .. "a shorter vector walks more slowly. While swimming or flying, all three axes are used, at "
                    .. "`SwimSpeed` or `FlySpeed`.\n\n"
                    .. "A character can be pushed like a part. `ApplyImpulse` changes its velocity by the impulse "
                    .. "divided by its `Mass`, and `LinearVelocity` can be set, for a knockback or a launch pad. "
                    .. "The push fades, quickly on the ground and slowly in the air, and the walk is added to it.",
            },
            {
                Name = "Jump",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Launches the character upward at `JumpSpeed` at the next simulation tick.\n\n"
                    .. "It does not check whether the character is on the ground, so that a double jump or a wall "
                    .. "jump can be written with the same call. For an ordinary jump, check first: `if "
                    .. "character.Grounded then character:Jump() end`. Calling it every frame makes the character "
                    .. "fly.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Landed",
                Parameters = { { Name = "groundPart", Type = "BasePart?" } },
                Doc = "Fires when the character becomes `Grounded` after being `Airborne`. Passes the part it "
                    .. "landed on, or nil when it landed on terrain.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "Camera",
        Extends = "PVInstance",
        Module = "render",
        Parents = under("Workspace", "ViewportFrame"),
        Storage = "Camera",
        Doc = "The viewpoint the world is drawn from. A camera is an ordinary instance that a script creates and "
            .. "moves; the engine never moves it for you.",
        Properties = {
            {
                Name = "CFrame",
                Type = "CFrame",
                Records = { "ADR 0136" },
                Doc = "The position of the camera and the direction it looks in. The camera looks along the "
                    .. "`LookVector` of this value, which is -Z.\n\n"
                    .. "Set inside a render step (a function bound with `RunService:BindToRenderStep`, a "
                    .. "`PreRender` handler, or an action whose context has `Rate = Render`), the value is drawn "
                    .. "exactly as set in the current frame, and it becomes the value simulation code reads when "
                    .. "the next tick starts. Set anywhere else, it takes effect at once and is drawn "
                    .. "interpolated between ticks.\n\n"
                    .. "Read inside a render step, it returns the value that is being drawn. Read anywhere else, "
                    .. "it returns the value simulation code sees.",
            },
            {
                Name = "ViewportSize",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                -- The size of the window or panel drawn into: a fact about the
                -- host, like `UIService.DisplayScale`, kept out of the hash.
                HostFact = true,
                Doc = "The size of the area the world is drawn into, in pixels: the window in a game, or the "
                    .. "Viewport panel in the editor. It changes when the window is resized or a phone is "
                    .. "rotated. Use it to turn a screen position into a fraction, or to tell a tall screen from "
                    .. "a wide one.",
            },
            {
                Name = "FieldOfView",
                Type = "number",
                ErrorKey = "scene.err.number_cone_degrees",
                Doc = "The vertical field of view, in degrees. It is clamped to more than 0 and less than 180.",
            },
            {
                Name = "NearPlane",
                Type = "number",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The distance to the near clip plane, in meters. Geometry closer than this is not drawn. A "
                    .. "very small value reduces depth precision across the whole view, so prefer raising it to "
                    .. "lowering it. It applies only to a perspective camera.",
            },
            {
                Name = "FarPlane",
                Type = "number",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The distance to the far clip plane, in meters. Geometry beyond it is not drawn.",
            },
            {
                Name = "Projection",
                Type = "Enum.CameraProjection",
                Doc = "Whether the camera is `Perspective`, for a 3D view, or `Orthographic`, for a 2D view. An "
                    .. "orthographic camera ignores `FieldOfView` and shows `OrthographicSize` meters above and "
                    .. "below the center of the view, at any distance.",
            },
            {
                Name = "OrthographicSize",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "Half the height of what an orthographic camera shows, in meters. The width follows from "
                    .. "the shape of the window. Change it to zoom a 2D view.\n\n"
                    .. "An orthographic camera draws everything from `FarPlane` behind it to `FarPlane` in front "
                    .. "of it. Nothing shrinks with distance, so a top-down camera that stands lower than the top "
                    .. "of a tall object still shows all of the object.",
            },
            {
                Name = "ClipPlane",
                Type = "CFrame",
                Records = { "ADR 0107" },
                Doc = "A plane that hides everything behind it from this camera, used while `ClipPlaneEnabled` is "
                    .. "on. The plane passes through the position of this `CFrame`; what is in front of its "
                    .. "`LookVector` is drawn, and what is behind is not.\n\n"
                    .. "A mirror's camera stands behind the glass and uses the plane of the glass, so it draws "
                    .. "the room and not the wall the mirror hangs on. `@engine/views` sets it for you. The "
                    .. "camera must be behind the plane; if it is in front, the plane has no effect.",
            },
            {
                Name = "ClipPlaneEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `ClipPlane` is applied.",
            },
        },
        Methods = {
            {
                Name = "GetRenderCFrame",
                Parameters = {},
                Returns = { "CFrame" },
                ThreadSafety = "ReadParallel",
                Records = { "ADR 0136" },
                Doc = "Returns where this camera is drawn in the current frame, which is between its positions at "
                    .. "the last two simulation ticks. Use it for a camera that follows this one.\n\n"
                    .. "It differs from `CFrame` only inside a render step: a function bound with "
                    .. "`RunService:BindToRenderStep`, a `PreRender` handler, or an action whose context has "
                    .. "`Rate = Render`. Everywhere else, including on a server, it returns `CFrame`.",
            },
            {
                Name = "WorldToViewportPoint",
                Parameters = { { Name = "position", Type = "vector" } },
                Returns = { "Vector2", "number", "boolean" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns where a point in the world appears on screen: its position in pixels, its depth in "
                    .. "meters in front of the camera, and whether it is on screen.\n\n"
                    .. "The position is measured from the top left of the area the world is drawn into, in the "
                    .. "same units as `UIObject.AbsolutePosition` and `InputService:GetPointerPosition`. It is "
                    .. "valid beyond the edges of the screen, so an arrow at the border can point toward an "
                    .. "off-screen target. With a depth of 0 or less, the point is beside or behind a perspective "
                    .. "camera and the position is meaningless.\n\n"
                    .. "It works for both projections. A `ScreenGui` that places elements by this position should "
                    .. "have `ScreenInsets` off, or they are offset by the screen's cutout.",
            },
            {
                Name = "ViewportPointToRay",
                Parameters = { { Name = "point", Type = "Vector2" } },
                Returns = { "vector", "vector" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns the ray that a pixel looks along, as an origin and a direction of length 1. Pass "
                    .. "it to `Workspace:Raycast` to find what is under the pointer. For a perspective camera the "
                    .. "ray starts at the camera and passes through the pixel; for an orthographic camera it "
                    .. "starts at the pixel's own position and points straight ahead.",
            },
            {
                Name = "ViewportPointToWorld2D",
                Parameters = { { Name = "point", Type = "Vector2" } },
                Returns = { "Vector2?" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns the position on the 2D plane that a pixel points at. Use it in a 2D game to "
                    .. "compare a tap or a click with `Part2D.Position`. Returns nil when the camera looks along "
                    .. "the plane or away from it.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "PointLight",
        Extends = "Instance",
        Module = "render",
        Parents = OnAPart,
        Storage = "PointLight",
        Records = { "ADR 0095" },
        Doc = "A light that shines equally in every direction. Parented to a `BasePart` or an `Attachment`, it "
            .. "shines from there and moves with it. Parented to anything else, it shines from its own `CFrame`.",
        Properties = {
            {
                Name = "CFrame",
                Type = "CFrame",
                Records = { "ADR 0095" },
                Doc = "The position of the light. When the light is parented to a `BasePart` or an `Attachment`, "
                    .. "this is relative to that parent, and the identity puts the light at the parent. Otherwise "
                    .. "it is in world space.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Doc = "The color of the light, multiplied by `Brightness`. It is not clamped, so a channel above "
                    .. "1 gives an over-bright tint.",
            },
            {
                Name = "Brightness",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much light is emitted. At 0 the light emits nothing.",
            },
            {
                Name = "Range",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The radius, in meters, beyond which the light has no effect. A smaller range is cheaper to "
                    .. "draw.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the light shines. A disabled light does not count toward the limit on lights drawn "
                    .. "at once; a light with a `Brightness` of 0 does.",
            },
            {
                Name = "Shadows",
                Type = "boolean",
                Doc = "Whether the light casts shadows.\n\n"
                    .. "A point light's shadow uses six of the sixteen tiles in the shadow atlas, one for each "
                    .. "face of a cube around the light. A `SpotLight` uses one. Two point lights and four spot "
                    .. "lights fill the atlas.\n\n"
                    .. "When more lights ask for shadows than there are tiles, the lights that are nearest and "
                    .. "largest on screen cast them and the others do not. The choice is stable from frame to "
                    .. "frame, so shadows do not flicker.",
            },
        },
    },
    {
        Name = "ReflectionProbe",
        Extends = "Instance",
        Module = "render",
        Parents = under("BasePart"),
        Storage = "ReflectionProbe",
        Records = { "ADR 0202" },
        Doc = "Captures what a room looks like from one point, so that surfaces in the room reflect it and are "
            .. "lit by it instead of by the sky. Parent it to a part: the part's box is the area the probe "
            .. "applies to, and the walls its reflections are corrected against. Under anything other than a part "
            .. "it does nothing.\n\n"
            .. "For a room, use an invisible part the size of the room, with `Transparency` at 1 and `CanCollide` "
            .. "and `CanQuery` off, and put the probe under it. A surface uses the probe whose box contains the "
            .. "point just in front of it, so the floor and walls of a room use that room's probe, and each side "
            .. "of a thin wall between two rooms reflects its own room.\n\n"
            .. "The probe captures six images of every anchored object from its position. Inside the box, a shiny "
            .. "surface reflects the room and a matte surface is lit by it. Outside every probe's box, surfaces "
            .. "use the sky.\n\n"
            .. "It is visual only: each machine captures its own, and nothing a script reads depends on it. No "
            .. "probes are captured when `GraphicsService.Reflections` is `Low`.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the probe applies. A disabled probe does not count toward the number of probes a "
                    .. "machine keeps, and surfaces in its box use the sky.",
            },
            {
                Name = "Offset",
                Type = "vector",
                Default = "vector.zero",
                Doc = "The position the probe captures from, relative to the center of the box, in the box's own "
                    .. "space and in meters. Move it when the center of the room is inside an obstacle such as a "
                    .. "pillar.",
            },
            {
                Name = "Blend",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance, in meters, over which the probe fades out beyond the faces of its box, where "
                    .. "no other probe applies. Inside the box it applies fully. Two probes whose boxes meet "
                    .. "blend across the boundary, so a doorway between two rooms shows no seam.",
            },
            {
                Name = "Intensity",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "A multiplier on the light the probe provides.",
            },
            {
                Name = "Priority",
                Type = "number",
                Default = "0",
                Doc = "Which probe applies where two boxes overlap: the one with the higher number. An integer. "
                    .. "Give a small probe around a mirror a higher priority than the probe of the room it is in.",
            },
            {
                Name = "BoxProjection",
                Type = "boolean",
                Default = "true",
                Doc = "Whether reflections are corrected for the walls of the box, so that a floor reflects a "
                    .. "wall where the wall actually is. Turn it off for a probe outdoors, whose box is not a room.",
            },
            {
                Name = "Dynamic",
                Type = "boolean",
                Doc = "Whether the probe is captured again when the lighting changes, such as when the sun moves "
                    .. "or a light inside its box is switched on. It captures one image per frame. When it is "
                    .. "off, the probe is captured once and again on `Refresh`.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Refresh",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Requests a new capture. Call it after the room changes: a wall falls, a door opens, a room "
                    .. "is built. A probe captured while another is already in place sees that probe's light, so "
                    .. "calling it twice adds a second bounce of light.",
            },
            {
                Name = "IsReady",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the probe's most recently requested capture is finished. It is false from "
                    .. "`Refresh` until the new capture is done. It is true at once on a machine that keeps no "
                    .. "probes, and for a probe beyond the number the machine keeps. To wait: `if not "
                    .. "probe:IsReady() then probe.Completed:Wait() end`.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Completed",
                Parameters = {},
                Doc = "Fires when the probe's capture finishes on this machine, each time `IsReady` changes from "
                    .. "false to true. Each machine fires it at its own time.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "IrradianceVolume",
        Extends = "Instance",
        Module = "render",
        Parents = under("BasePart"),
        Storage = "IrradianceVolume",
        Records = { "ADR 0203" },
        Doc = "Adds bounced light to an area: the light a surface receives from other surfaces instead of "
            .. "directly from a light, such as from a sunlit wall or a red carpet. Parent it to a part: the "
            .. "part's box is the volume. The bounced light is computed at points `Spacing` apart inside the box "
            .. "and applied to everything drawn there, moving or not.\n\n"
            .. "Use one volume per room. A surface uses the volume whose box contains the point just in front of "
            .. "it, so each side of a wall between two rooms is lit by its own room's volume. One volume "
            .. "stretched over two rooms lets light from the bright one leak into the dark one.\n\n"
            .. "The light is computed on the CPU by casting rays against anchored objects, behind the loading "
            .. "screen when a scene opens. It is visual only: each machine computes its own. A machine whose "
            .. "`GraphicsService.GlobalIllumination` is below `High` computes none.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the volume applies.",
            },
            {
                Name = "Spacing",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.irradiance_spacing",
                Doc = "The distance between the volume's points, in meters, along each side of the box. The "
                    .. "minimum is 0.5.\n\n"
                    .. "A box that needs more than 4,096 points, or more than 32 along a side, is computed as up "
                    .. "to sixteen tiles that blend where they meet; the eight nearest the camera are drawn. A "
                    .. "box that would need more tiles than that is given a coarser spacing, and a message is "
                    .. "logged once.",
            },
            {
                Name = "Blend",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance, in meters, over which the volume fades out beyond the faces of its box, "
                    .. "where no other volume applies.",
            },
            {
                Name = "Intensity",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "A multiplier on the volume's light. At 1, the default, the room is as bright as its "
                    .. "lights, openings and surfaces make it. Other values are an artistic adjustment.",
            },
            {
                Name = "Tint",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "A color the volume's light is multiplied by. It can change every frame at no cost; nothing "
                    .. "is computed again.",
            },
            {
                Name = "Mode",
                Type = "Enum.IrradianceMode",
                Default = "Enum.IrradianceMode.Full",
                Doc = "What light the volume provides. `Full` provides the light of the sky and all bounced "
                    .. "light, replacing the sky's light and `Lighting.Ambient`. `BounceOnly` provides only "
                    .. "bounced light, added to them. Changing it computes the volume again.",
            },
            {
                Name = "Bounces",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.irradiance_bounces",
                Doc = "How many times light bounces, from 1 to 3. Each bounce costs as much to compute as the first.",
            },
            {
                Name = "Baked",
                Type = "Content",
                ContentKind = "Irradiance",
                Records = { "ADR 0207" },
                Doc = "A file that holds this volume's computed light, as an `asset://` path ending in "
                    .. "`.irradiance`. Write it with the editor's Bake command on the volume, or with "
                    .. "`engine-host <project> --bake-irradiance`. A machine that computes volumes loads the file "
                    .. "instead of casting rays, so the room is lit from the first frame.\n\n"
                    .. "A machine that computes none (`GlobalIllumination` below `High`, such as a phone) reads "
                    .. "one color from the file, the average of the whole volume, and lights the box with it in "
                    .. "place of the flat ambient light. That gives the room its overall tone at no cost per "
                    .. "frame.\n\n"
                    .. "The file is valid only for the volume it was baked for. If the box, `Spacing`, `Bounces` "
                    .. "or `Mode` differ, the file is rejected with one logged message and the volume is computed "
                    .. "as if it had no file. With `Dynamic` on, the file provides the first light and the volume "
                    .. "is computed again as the lighting changes. `Refresh` computes it by rays. The file is not "
                    .. "used with `FollowsCamera`.\n\n"
                    .. "Bake again after changing the room. The editor and development runs log a message once "
                    .. "when the objects in the box differ from those it was baked with.",
            },
            {
                Name = "FollowsCamera",
                Type = "boolean",
                Records = { "ADR 0206" },
                Doc = "Whether the volume moves with the camera, to provide bounced light outdoors, where no box "
                    .. "can enclose the area. The part then gives only the volume's size; its position is "
                    .. "ignored.\n\n"
                    .. "The volume is centered on the camera on a grid of its own cells and follows it a few "
                    .. "cells at a time. Points that are in both the old and the new position keep their light, "
                    .. "so a move costs only the new points. A room's own volume still applies inside the room; "
                    .. "this one lights what no other volume covers.",
            },
            {
                Name = "Levels",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.irradiance_bounces",
                Doc = "With `FollowsCamera`: the number of nested levels, from 1 to 3. Each level after the first "
                    .. "is twice as large around the same center, with points twice as far apart, so light is "
                    .. "detailed near the camera and coarse far from it. A part of 40 by 16 by 40 at a `Spacing` "
                    .. "of 2 with three levels lights 80 meters in every direction.",
            },
            {
                Name = "Dynamic",
                Type = "boolean",
                Doc = "Whether the volume is computed again when the lighting changes, such as when the sun moves "
                    .. "or a light that reaches its box is switched on. The work is spread over several frames, "
                    .. "and the picture changes from the old light to the new once the new is complete. When it "
                    .. "is off, the volume is computed once and again on `Refresh`.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Refresh",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Requests that the volume be computed again. Call it after the room changes: a wall falls "
                    .. "or a room is built.",
            },
            {
                Name = "GetProgress",
                Parameters = {},
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Returns how much of the computation is done, from 0 to 1. Use it for a loading screen. It "
                    .. "is 1 when the volume's own light is being drawn, and also on a machine that computes "
                    .. "none. It stays 1 while a `Dynamic` volume is being computed again after the lighting "
                    .. "changed.",
            },
            {
                Name = "IsReady",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether the volume's own light is being drawn, for the most recent request. It is "
                    .. "false from `Refresh`, or from a change to the box, `Spacing`, `Bounces` or `Mode`, until "
                    .. "the volume is computed again. It is true at once on a machine that computes none. To "
                    .. "wait: `if not volume:IsReady() then volume.Completed:Wait() end`.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Completed",
                Parameters = {},
                Doc = "Fires when the volume finishes computing on this machine, each time `IsReady` changes from "
                    .. "false to true. Each machine fires it at its own time, so game rules must not depend on it.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "SequencePlayer",
        Extends = "Instance",
        Module = "render",
        Storage = "SequencePlayer",
        Records = { "ADR 0204" },
        Doc = "Plays a sequence: a cutscene stored as a file of tracks over time. A sequence can have a camera "
            .. "that cuts and moves, clips on characters, spoken lines, sounds, keyed properties on any instance, "
            .. "morph targets, and events for scripts. Create one anywhere, set `Sequence` to the file, add a "
            .. "`SequenceBinding` child for each name the file expects, and call `Play`.\n\n"
            .. "A sequence runs on simulation time. `TimePosition` advances each tick, so every key and event "
            .. "lands on the same tick in a replay or a headless run. Keyed properties, clips, lines, sounds and "
            .. "events are applied in the tick. The camera and the morph targets are visual only and are computed "
            .. "every frame on each machine.\n\n"
            .. "The sequence controls a keyed property from the first key of its track to the last. Before the "
            .. "first key the sequence does not touch the property. After the last key the property keeps its "
            .. "value and the game can set it again.\n\n"
            .. "While a sequence plays with `TakesCamera` on, the main view is drawn through the camera track. "
            .. "`Workspace.CurrentCamera` is not changed, the game's camera script keeps running, and the view "
            .. "returns to the game's camera when the sequence ends.\n\n"
            .. "In a multiplayer match, a sequence that the authority plays is seen on every machine. Only the "
            .. "fact that it is playing and its start time are replicated; each machine reads the file from its "
            .. "own build. A client that joins late sees the sequence from its current position. A sequence that "
            .. "a client creates plays only on that client.",
        Properties = {
            {
                Name = "Sequence",
                Type = "Content",
                ContentKind = "Sequence",
                Doc = "The sequence file, as an `asset://` path ending in `.sequence.json`. It is read when first "
                    .. "played and again whenever the file changes.",
            },
            {
                Name = "Playing",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                Doc = "Whether the sequence is advancing. Change it with `Play` and `Pause`.",
            },
            {
                Name = "TimePosition",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Transient = true,
                Doc = "The current position in the sequence, in seconds. Setting it seeks: every track is put in "
                    .. "the state it has at that time, and no event fires. The sequence stays playing or paused "
                    .. "as it was. A value beyond `Length` is clamped to `Length`.",
            },
            {
                Name = "Speed",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "How fast the sequence's time runs relative to simulation time. Lines and sounds always "
                    .. "play at their own speed.",
            },
            {
                Name = "Looped",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the sequence starts again when it reaches the end. A looped sequence does not fire "
                    .. "`Finished` on its own; `Skip` still ends it.",
            },
            {
                Name = "TakesCamera",
                Type = "boolean",
                Default = "true",
                Doc = "Whether each machine's main view is drawn through the camera track while the sequence "
                    .. "plays. When it is off, the camera track is ignored.",
            },
            {
                Name = "Controls",
                Type = "Enum.SequenceControl",
                Default = "Enum.SequenceControl.Keep",
                Doc = "What happens to player input while the sequence plays. `Suspend` holds the local players' "
                    .. "movement and actions at rest on this machine and restores them when the sequence ends. "
                    .. "Input events still reach scripts, so a game can bind a key to `Skip`.",
            },
            {
                Name = "Length",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                Doc = "The length of the sequence, in seconds. It is available as soon as `Sequence` is set, and "
                    .. "is 0 if the file does not exist.\n\n"
                    .. "The length is the end of the last key or the last line, or the length the file states if "
                    .. "that is greater. A line lasts as long as its longest language, so the length is the same "
                    .. "in every language.",
            },
            {
                Name = "Held",
                Type = "string",
                ReadOnly = true,
                Transient = true,
                Doc = "The name of the hold the sequence is waiting at, or an empty string. A key on a `hold` "
                    .. "track stops the sequence's time until `Release` is called, to wait for a choice or a "
                    .. "button press. What the sequence already started continues, such as a playing clip, and "
                    .. "nothing new starts.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Play",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Plays the sequence from its current position: from the start the first time or after it "
                    .. "ended, and otherwise from where `Pause` left it.",
            },
            {
                Name = "Pause",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops the sequence's time at its current position. The camera stays on the sequence, lines "
                    .. "that are being spoken finish, and `Play` resumes it.",
            },
            {
                Name = "Stop",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Ends the sequence and leaves the world as it is at that moment. Properties keep the values "
                    .. "the sequence gave them, clips fade out, the lines and sounds it started end, characters "
                    .. "it was carrying are set down, the camera is returned, and its time goes back to 0. No "
                    .. "event fires, including `Finished`.",
            },
            {
                Name = "Skip",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Ends the sequence and leaves the world as the sequence would have left it at its end. "
                    .. "Every keyed property and morph target takes the value of its last key, and every event "
                    .. "between the current position and the end fires in order with `skipped` true. The lines "
                    .. "and sounds it started end, clips fade out, characters are set down where their last keys "
                    .. "put them, and the camera is returned. Then `Finished` fires with `skipped` true.",
            },
            {
                Name = "Release",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Continues from the hold the sequence is waiting at. Does nothing if it is not held. `Play` "
                    .. "does the same. In a multiplayer match only the server can call it, like `Play`: a hold is "
                    .. "held and released on every machine.",
            },
            {
                Name = "GetMarkers",
                Parameters = {},
                Returns = { "{ { Name: string, Time: number } }" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the sequence's markers, in order of time: the keys of its `marker` track, which "
                    .. "usually mark where each shot begins. Returns an empty list if the file has none or does "
                    .. "not exist.",
            },
            {
                Name = "PlayFrom",
                Parameters = { { Name = "marker", Type = "string" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Seeks to a marker by name and plays from there. The seek works like setting "
                    .. "`TimePosition`: every track is put in the state it has at that time, and no event fires. "
                    .. "Returns false and does nothing if the file has no marker of that name.",
            },
            {
                Name = "SkipToNext",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Skips to the next marker after the current position and plays on from there. The world is "
                    .. "left as the sequence would have left it at that time: keyed properties take the values "
                    .. "their keys give them, events in between fire with `skipped` true, and lines and sounds in "
                    .. "between are not played. Holds on the way are passed. Returns false and does nothing if "
                    .. "there is no later marker; use `Skip` to end the sequence.",
            },
            {
                Name = "LoadSequence",
                Parameters = { { Name = "sequence", Type = "{ [string]: any }" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Loads a sequence from a table instead of a file. The table has the same shape as a "
                    .. '`.sequence.json` file: `{ format = "sequence", version = 1, bindings = {...}, tracks = '
                    .. "{...} }`. `Sequence` is set to a generated name, and `Play` plays the table. Use it for a "
                    .. "scene that the game builds as it runs. Raises an error that names the place if the table "
                    .. "is not a valid sequence.\n\n"
                    .. "The sequence exists only on the machine that loads it. Loaded on a server, it changes the "
                    .. "world and every client sees the change, but the visual tracks (camera, fade, bars, morph "
                    .. "targets) are not drawn on clients, which do not have the table. For a scene that every "
                    .. 'machine should see in full, use a file, write the parts that vary as `"$Name"` and set '
                    .. "them as attributes.",
            },
            {
                Name = "BindCamera",
                Parameters = {
                    {
                        Name = "fn",
                        Type = "(cframe: CFrame, fieldOfView: number, time: number) -> (CFrame?, ...number)",
                    },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Binds a function that can adjust the camera on every frame in which this sequence has the "
                    .. "view. Use it for what a file cannot know, such as a wall the scene happens to be next to.\n\n"
                    .. "The function receives the `CFrame` the sequence computed, the field of view in degrees "
                    .. "and the sequence's time in seconds. It returns a `CFrame` to draw instead, then "
                    .. "optionally a field of view; return nil for either to keep the sequence's value. It runs "
                    .. "in a render step, so `GetRenderCFrame` returns drawn positions inside it, and it must not "
                    .. "yield.\n\n"
                    .. "A player has one such function; binding again replaces it. It applies only on the machine "
                    .. "that binds it.",
            },
            {
                Name = "UnbindCamera",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes the function bound with `BindCamera`. Does nothing if none is bound.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Finished",
                Parameters = { { Name = "skipped", Type = "boolean" } },
                Doc = "Fires when the sequence ends: it reached its end, or `Skip` was called, in which case "
                    .. "`skipped` is true. `Stop` does not fire it.",
            },
            {
                Name = "HoldReached",
                Parameters = { { Name = "name", Type = "string" } },
                Doc = "Fires when the sequence reaches a key on a `hold` track and starts waiting for `Release`. "
                    .. "Passes the hold's name. It fires on every machine.",
            },
            {
                Name = "MarkerReached",
                Parameters = { { Name = "name", Type = "string" } },
                Doc = "Fires when the sequence reaches a marker, either by playing through it or by `SkipToNext` "
                    .. "landing on it. Passes the marker's name.",
            },
            {
                Name = "EventReached",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "data", Type = "any" },
                    { Name = "skipped", Type = "boolean" },
                },
                Doc = "Fires when the sequence reaches a key on an event track. Passes the event's `name` and its "
                    .. "`data` as written in the file: a number, a boolean, a string, a list of numbers, or nil.\n\n"
                    .. "`skipped` is true for an event fired by `Skip` on the way to the end. A handler that "
                    .. "changes the game, such as opening a door, should still act; one that only plays an effect "
                    .. "can ignore it. Seeking fires no events.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "SequenceBinding",
        Extends = "Instance",
        Module = "render",
        Parents = { "SequencePlayer" },
        Storage = "SequenceBinding",
        Records = { "ADR 0204" },
        Doc = "Tells a `SequencePlayer` which instance one of its sequence's names refers to. A sequence refers "
            .. "to its subjects by name, such as `Anna` or `Lamp`. A binding under the player whose `Name` is "
            .. "that name sets the instance, so one sequence can be played on different characters.\n\n"
            .. "The tracks of a name with no binding, or with a binding whose `Target` is nil, do nothing, and "
            .. "the player logs a message once.",
        Properties = {
            {
                Name = "Target",
                Type = "Instance?",
                Default = "nil",
                Doc = "The instance the name refers to. A track targets this instance, or a descendant given by a "
                    .. "path from it, such as `Lamp/PointLight`.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Decal",
        Extends = "Instance",
        Module = "render",
        Parents = under("BasePart"),
        Storage = "Decal",
        Doc = "An image projected onto every surface inside a box: a scorch mark, a footprint, a poster or a "
            .. "crack. It lands on parts, terrain and blocks alike, and needs no parent. To place one where a "
            .. "raycast hit, set its `CFrame` to `CFrame.lookAt(hit.Position, hit.Position + hit.Normal)` and "
            .. "give it a size. Parented to a `BasePart`, its `CFrame` is relative to the part and it moves with "
            .. "the part.\n\n"
            .. "By default a decal multiplies the color of the surface after lighting, so the surface keeps its "
            .. "light and shadow. It darkens and tints and cannot make a surface brighter; a white pixel changes "
            .. "nothing. `BlendMode` selects other behavior.",
        Properties = {
            {
                Name = "CFrame",
                Type = "CFrame",
                Doc = "The position and orientation of the box, relative to the parent part if there is one. The "
                    .. "image lies along the box's right and up axes and is projected along its look direction.",
            },
            {
                Name = "Size",
                Type = "vector",
                Default = "vector.create(2, 2, 1)",
                Doc = "The width, height and depth of the box, in meters. Only surfaces inside the box receive "
                    .. "the image, so a shallow box marks one wall and a deep one wraps around a corner.",
            },
            {
                Name = "Texture",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image to project. Its alpha channel sets how much of it is applied. With no texture, "
                    .. "the decal applies `Color` alone.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "A color the image is multiplied by.",
            },
            {
                Name = "Transparency",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How transparent the decal is, from 0 (the image as it is) to 1 (invisible).",
            },
            {
                Name = "BlendMode",
                Type = "Enum.DecalBlendMode",
                Default = "Enum.DecalBlendMode.Multiply",
                Doc = "How the image is combined with the surface: multiplied into it, laid over it, or added to it.",
            },
            {
                Name = "Emissive",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How brightly the image glows on its own, in addition to the light that falls on it. 0 is "
                    .. "no glow; a value above 1 is bright enough to bloom. It applies to the `Alpha` and "
                    .. "`Additive` blend modes only.",
            },
        },
    },
    {
        Name = "CameraTexture",
        Extends = "Instance",
        Module = "render",
        Parents = under("Workspace", "Model", "BasePart", "Camera"),
        Storage = "CameraTexture",
        Records = { "ADR 0107" },
        Doc = "Draws what a camera sees into a texture. The picture is available as `view://` followed by "
            .. "`ViewName`, and anything that takes a texture can show it: an `ImageLabel` on a `SurfaceGui`, a "
            .. "`Decal` or a material's map. For a wall of security monitors, use one per camera.\n\n"
            .. "The number of views drawn per frame is limited by `[render] max_views_per_frame` in the project "
            .. "settings (4 by default). The view with the oldest picture is drawn first, so twelve monitors take "
            .. "turns. Views are drawn before the main view and show the current frame; a camera that sees its "
            .. "own texture sees the previous frame's picture.\n\n"
            .. "It is visual only: the simulation does not see it, and a dedicated server draws nothing. It is "
            .. "not replicated. In a multiplayer match, create the cameras and their textures in a client script "
            .. "on the machine that shows them.",
        Properties = {
            {
                Name = "Camera",
                Type = "Camera?",
                Doc = "The camera to draw from. With nil, nothing is drawn and the texture keeps its last picture.",
            },
            {
                Name = "ViewName",
                Type = "string",
                Doc = "The name of the texture: it is available as `view://` followed by this name. If two have "
                    .. "the same name, the one created first draws and a message is logged for the other. The "
                    .. "texture is black until the first picture is drawn.",
            },
            {
                Name = "Resolution",
                Type = "Vector2",
                Default = "Vector2.new(512, 288)",
                ErrorKey = "scene.err.size2_positive",
                Doc = "The size of the texture, in pixels. Each side is limited to `[render] max_view_resolution` "
                    .. "(1024 by default); a larger size is scaled down with its aspect ratio kept.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the view is drawn. A disabled view keeps its last picture and costs nothing.",
            },
            {
                Name = "UpdateInterval",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.whole_number_at_least_one",
                Doc = "The number of frames between pictures: at 1 the view is drawn every frame, and at 4 every "
                    .. "fourth frame, for a quarter of the cost.",
            },
            {
                Name = "Quality",
                Type = "Enum.ViewQuality",
                Default = "Enum.ViewQuality.Simple",
                Doc = "How the view is drawn. `Simple` draws without shadows or post-processing effects, which "
                    .. "suits a monitor feed. `Full` draws the way the main view does.",
            },
        },
    },
    {
        Name = "SubWorld",
        Extends = "Instance",
        Module = "render",
        Parents = under("Workspace", "Model", "BasePart"),
        Storage = "SubWorld",
        Records = { "ADR 0107" },
        Doc = "Runs another scene alongside this one and draws it into a texture. Use it for a playable arcade "
            .. "cabinet, a game on a computer inside the game, or a snow globe with its own weather.\n\n"
            .. "`Load` starts the scene that `Scene` names in a world of its own, with its own instances, "
            .. "scripts, physics and clock. Its current camera draws into `view://` followed by `ViewName`, which "
            .. "an `ImageLabel` on a `SurfaceGui` can show.\n\n"
            .. "The two worlds are isolated. A script in one cannot reach an instance in the other. They "
            .. "communicate through `Send` and `Received` on this side, and `SceneService:SendToHost` and "
            .. "`SceneService.HostMessageReceived` inside. This side chooses what input the sub-world receives "
            .. "with `SetInputState`.\n\n"
            .. "A sub-world runs its scene's `ServerScriptService` and `ClientScriptService` as a single-player "
            .. "game, and never the project's `GlobalScriptService`. It ticks once for each tick of this world. "
            .. "At most `[render] max_sub_worlds` run at once (2 by default), and a sub-world cannot contain "
            .. "another. It is not replicated: in a multiplayer match, each machine that loads one runs its own.",
        Properties = {
            {
                Name = "Scene",
                Type = "string",
                Doc = "The scene file to run, relative to the content folder, such as `scenes/arcade.scene.json`. "
                    .. "It is read when `Load` starts the scene; a later change takes effect at the next `Load`.",
            },
            {
                Name = "ViewName",
                Type = "string",
                Doc = "The name of the texture the sub-world is drawn into: it is available as `view://` followed "
                    .. "by this name. With an empty name nothing is drawn, and the world still runs.",
            },
            {
                Name = "Resolution",
                Type = "Vector2",
                Default = "Vector2.new(512, 288)",
                ErrorKey = "scene.err.size2_positive",
                Doc = "The size of the texture, in pixels, with the same limit as `CameraTexture.Resolution`.",
            },
            {
                Name = "UpdateInterval",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.whole_number_at_least_one",
                Doc = "The number of frames between pictures: at 1 the view is drawn every frame. The world ticks "
                    .. "every tick whatever this value is.",
            },
            {
                Name = "Quality",
                Type = "Enum.ViewQuality",
                Default = "Enum.ViewQuality.Simple",
                Doc = "How the view is drawn. `Simple` draws without shadows or post-processing effects. `Full` "
                    .. "draws the way the main view does.",
            },
            {
                Name = "Running",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the sub-world ticks. When it is off, the world is paused and its picture stays as "
                    .. "it was.",
            },
        },
        Methods = {
            {
                Name = "Load",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Starts the scene. It returns at once: the world starts after the current tick, and "
                    .. "`Loaded` fires when its scripts have started. Does nothing if the sub-world is already "
                    .. "loaded. Raises an error if `[render] max_sub_worlds` are already running, or if it is "
                    .. "called inside a sub-world.",
            },
            {
                Name = "Unload",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops the sub-world and discards its world, after the current tick. Its picture stays "
                    .. "until the next `Load` draws over it. Destroying the `SubWorld` does the same.",
            },
            {
                Name = "IsLoaded",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns whether the sub-world's world exists, running or paused. It is true from `Loaded` "
                    .. "until `Unload`.",
            },
            {
                Name = "SetInputState",
                Parameters = {
                    { Name = "action", Type = "string" },
                    { Name = "value", Type = "boolean | number | Vector2 | vector" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Holds the sub-world's `InputAction` named `action` at `value` from its next tick until it "
                    .. "is set again. `value` is true or false for a `Bool` action, or a number, a `Vector2` or a "
                    .. "`vector` for a direction. No other input reaches the sub-world: the keyboard, mouse and "
                    .. "gamepads belong to this world, which decides what to pass on.",
            },
            {
                Name = "Send",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends values into the sub-world, where `SceneService.HostMessageReceived` fires with them "
                    .. "on its next tick. It accepts the values a `RemoteEvent` accepts, except instances, "
                    .. "because the two worlds share none. The values are dropped if the sub-world is not loaded.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Loaded",
                Parameters = {},
                Doc = "Fires when the sub-world's world is running and its scripts have started.",
            },
            {
                Name = "Received",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Doc = "Fires with the values the sub-world passed to `SceneService:SendToHost`, one tick after it "
                    .. "sent them.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "ParticleEmitter",
        Extends = "Instance",
        Module = "render",
        Parents = OnAPart,
        Storage = "ParticleEmitter",
        Doc = "Emits particles for effects such as sparks, smoke, dust and magic. Parent it to a `BasePart` or an "
            .. "`Attachment`: particles appear at its position and move along its up direction.\n\n"
            .. "Particles are visual only. They collide with nothing, a script cannot find one, and they are "
            .. "simulated every frame instead of every tick. In return, a scene can hold thousands of them in one "
            .. "draw call.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the emitter emits continuously. `Emit` works whether it is on or off.",
            },
            {
                Name = "Rate",
                Type = "number",
                Default = "20",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The number of particles emitted per second while the emitter is enabled.",
            },
            {
                Name = "Lifetime",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How long each particle lives, in seconds.",
            },
            {
                Name = "Speed",
                Type = "number",
                Default = "4",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The speed of a particle when it is emitted, in meters per second, along the emitter's up "
                    .. "direction.",
            },
            {
                Name = "SpreadAngle",
                Type = "number",
                Default = "15",
                ErrorKey = "scene.err.number_spread_degrees",
                Doc = "The largest angle, in degrees, between a particle's direction and the emitter's up "
                    .. "direction. 0 is a jet and 180 is a sphere.",
            },
            {
                Name = "Acceleration",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "A constant acceleration applied to every particle, in meters per second squared, in world "
                    .. "space. `vector.create(0, -9.81, 0)` is gravity; a small upward value suits rising heat.",
            },
            {
                Name = "Drag",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The fraction of its velocity a particle loses each second. Use it for smoke, which slows; "
                    .. "leave it at 0 for sparks.",
            },
            {
                Name = "WindAffectsDrift",
                Type = "boolean",
                Default = "false",
                Presentation = true,
                Doc = "Whether the workspace's wind carries the particles. The wind at each particle's position "
                    .. "is added to its motion. Off by default.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "The color of a particle when it is emitted.",
            },
            {
                Name = "ColorEnd",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "The color of a particle at the end of its life. The color changes gradually from `Color` "
                    .. "to this.",
            },
            {
                Name = "Size",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The width of a particle when it is emitted, in meters.",
            },
            {
                Name = "SizeEnd",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The width of a particle at the end of its life, in meters.",
            },
            {
                Name = "Transparency",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How transparent a particle is when it is emitted, from 0 (opaque) to 1 (invisible).",
            },
            {
                Name = "TransparencyEnd",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How transparent a particle is at the end of its life, from 0 (opaque) to 1 (invisible). "
                    .. "With the default, each particle fades out as it dies.",
            },
            {
                Name = "LightEmission",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much a particle adds its light to what is behind it instead of covering it, from 0 to "
                    .. "1. At 0 it is blended over the background. At 1 it is added to it and never darkens "
                    .. "anything, which suits fire, sparks and magic.",
            },
            {
                Name = "Brightness",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "A multiplier on the particle's color. It is not clamped; a value above 1 makes a particle "
                    .. "bright enough to bloom.",
            },
            {
                Name = "Shape",
                Type = "Enum.ParticleShape",
                Default = "Enum.ParticleShape.Soft",
                Doc = "The shape a particle is drawn as when the emitter has no `Texture`.",
            },
            {
                Name = "Texture",
                Type = "Content",
                ContentKind = "Texture",
                Records = { "ADR 0160" },
                Doc = "The image each particle is drawn as, facing the camera. Its color is multiplied by the "
                    .. "particle's color, and its alpha channel gives the particle's outline. With no texture, "
                    .. "`Shape` is drawn. With `FlipbookColumns` or `FlipbookRows` above 1, the image is a grid "
                    .. "of animation frames.",
            },
            {
                Name = "FlipbookColumns",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.whole_number_at_least_one",
                Doc = "The number of frames across `Texture`, from 1 to 16. Frames are read left to right, then "
                    .. "top to bottom.",
            },
            {
                Name = "FlipbookRows",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.whole_number_at_least_one",
                Doc = "The number of rows of frames in `Texture`, from 1 to 16.",
            },
            {
                Name = "FlipbookFramerate",
                Type = "number",
                Default = "12",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The speed of the flipbook animation, in frames per second, when `FlipbookMode` is `Loop`.",
            },
            {
                Name = "FlipbookMode",
                Type = "Enum.ParticleFlipbookMode",
                Default = "Enum.ParticleFlipbookMode.Loop",
                Doc = "How a particle plays the frames of the flipbook: in a loop, once over its life, or as one "
                    .. "still frame chosen for that particle.",
            },
            {
                Name = "Rotation",
                Type = "number",
                Default = "0",
                Doc = "The angle, in degrees, a particle is rotated by when it is emitted, in the plane facing "
                    .. "the camera.",
            },
            {
                Name = "RotationSpread",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The largest random angle, in degrees, added to or subtracted from `Rotation` for each "
                    .. "particle. At 180, particles are emitted at any angle.",
            },
            {
                Name = "RotationSpeed",
                Type = "number",
                Default = "0",
                Doc = "How fast a particle rotates, in degrees per second. A negative value rotates the other way.",
            },
            {
                Name = "RotationSpeedSpread",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The largest random amount, in degrees per second, added to or subtracted from "
                    .. "`RotationSpeed` for each particle.",
            },
            {
                Name = "ColorOverLife",
                Type = "ColorSequence",
                Default = "ColorSequence.new(Color3.new(1, 1, 1))",
                Doc = "A color over a particle's life, from 0 when it is emitted to 1 when it dies. It is "
                    .. "multiplied by the color that `Color` and `ColorEnd` give. White changes nothing.",
            },
            {
                Name = "SizeOverLife",
                Type = "NumberSequence",
                Default = "NumberSequence.new(1)",
                Doc = "A scale over a particle's life, multiplied by the size that `Size` and `SizeEnd` give. For "
                    .. "a puff that swells and shrinks, use a curve that rises and falls. A value of 1 changes "
                    .. "nothing.",
            },
            {
                Name = "TransparencyOverLife",
                Type = "NumberSequence",
                Default = "NumberSequence.new(0)",
                Doc = "A transparency over a particle's life, combined with the one that `Transparency` and "
                    .. "`TransparencyEnd` give, like one transparent layer over another. For a fade in and out, "
                    .. "use a curve from 1 down to 0 and back to 1. A value of 0 changes nothing.",
            },
            {
                Name = "Collision",
                Type = "Enum.ParticleCollision",
                Default = "Enum.ParticleCollision.None",
                Doc = "What the particles collide with: nothing, the surfaces on screen, the terrain, or both.",
            },
            {
                Name = "CollisionResponse",
                Type = "Enum.ParticleCollisionResponse",
                Default = "Enum.ParticleCollisionResponse.Bounce",
                Doc = "What a particle does when it hits something: bounce, stick or die.",
            },
            {
                Name = "Bounce",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "The fraction of its speed into a surface that a particle keeps when it bounces, from 0 "
                    .. "(none) to 1 (all). A particle that bounces too slowly to be seen comes to rest.",
            },
            {
                Name = "Friction",
                Type = "number",
                Default = "0.2",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "The fraction of its speed along a surface that a particle loses each time it touches, from "
                    .. "0 (it slides forever) to 1 (it stops where it lands).",
            },
            {
                Name = "CollisionRadius",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance from a particle's center at which it collides, in meters. At 0, half the "
                    .. "particle's size is used.",
            },
            {
                Name = "Simulation",
                Type = "Enum.ParticleSimulation",
                Default = "Enum.ParticleSimulation.Cpu",
                Doc = "Whether the particles are simulated on the CPU or on the GPU. The GPU handles a hundred "
                    .. "thousand particles where the CPU handles a few thousand. A machine without compute "
                    .. "shaders always simulates on the CPU, and logs a message once.",
            },
        },
        Methods = {
            {
                Name = "Emit",
                Parameters = { { Name = "count", Type = "number" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Emits `count` particles at once, whether or not the emitter is enabled. Use it for a burst "
                    .. "such as an explosion. One call emits at most 1,000.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "Highlight",
        Extends = "Instance",
        Module = "render",
        Parents = InTheWorld,
        Storage = "Highlight",
        Doc = "Draws an outline and a tint over a part or a model, to mark what is selected, what can be picked "
            .. "up, or an ally behind a wall. It is drawn over the finished frame in the colors you set, "
            .. "unaffected by lighting. It is visual only: nothing collides with it, and raycasts do not hit it.\n\n"
            .. "At most `[render] max_highlights` are drawn per frame (32 by default). Beyond that, the nearest "
            .. "are drawn.",
        Properties = {
            {
                Name = "Adornee",
                Type = "(BasePart | Model)?",
                Presentation = true,
                Doc = "The part or model to highlight. A model highlights every part in it. With nil, the "
                    .. "highlight's parent is highlighted.",
            },
            {
                Name = "FillColor",
                Type = "Color3",
                Default = "Color3.new(1, 0.2, 0.2)",
                Presentation = true,
                Doc = "The color of the tint drawn over the whole shape.",
            },
            {
                Name = "FillTransparency",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How transparent the fill is, from 0 (it hides what it covers) to 1 (only the outline shows).",
            },
            {
                Name = "OutlineColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Presentation = true,
                Doc = "The color of the line around the edge of the shape.",
            },
            {
                Name = "OutlineTransparency",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How transparent the outline is, from 0 (solid) to 1 (no outline).",
            },
            {
                Name = "DepthMode",
                Type = "Enum.HighlightDepthMode",
                Default = "Enum.HighlightDepthMode.AlwaysOnTop",
                Presentation = true,
                Doc = "Whether the highlight shows through objects in front of the shape, or only where the shape "
                    .. "itself is visible.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether the highlight is drawn.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Beam",
        Extends = "Instance",
        Module = "render",
        Parents = under("Workspace", "Model", "BasePart", "Attachment"),
        Storage = "Beam",
        Doc = "Draws a band of color or a texture between two attachments: a laser, a tractor beam, a rope of "
            .. "light, or lightning with a scrolling texture. It can be straight, or a curve that leaves each "
            .. "attachment along that attachment's X axis.\n\n"
            .. "It is visual only: it collides with nothing, and it is drawn every frame where its attachments "
            .. "are drawn.",
        Properties = {
            {
                Name = "Attachment0",
                Type = "Attachment?",
                Presentation = true,
                Doc = "The attachment where the beam starts.",
            },
            {
                Name = "Attachment1",
                Type = "Attachment?",
                Presentation = true,
                Doc = "The attachment where the beam ends.",
            },
            {
                Name = "Color",
                Type = "ColorSequence",
                Default = "ColorSequence.new(Color3.new(1, 1, 1))",
                Presentation = true,
                Doc = "The color of the beam along its length, from `Attachment0` at 0 to `Attachment1` at 1. It "
                    .. "is multiplied by the texture.",
            },
            {
                Name = "Transparency",
                Type = "NumberSequence",
                Default = "NumberSequence.new(0)",
                Presentation = true,
                Doc = "How transparent the beam is along its length, from 0 (opaque) to 1 (invisible).",
            },
            {
                Name = "Width0",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "The width of the beam at `Attachment0`, in meters.",
            },
            {
                Name = "Width1",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "The width of the beam at `Attachment1`, in meters.",
            },
            {
                Name = "CurveSize0",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "How far the beam travels from `Attachment0` along that attachment's X axis before it bends "
                    .. "toward the other end, in meters. With 0 at both ends the beam is straight. A negative "
                    .. "value leaves in the opposite direction.",
            },
            {
                Name = "CurveSize1",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "How far before `Attachment1` the beam is already traveling along that attachment's X axis, "
                    .. "in meters.",
            },
            {
                Name = "Segments",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.whole_number_at_least_one",
                Presentation = true,
                Doc = "The number of straight pieces a curved beam is drawn with, from 1 to 64. A straight beam "
                    .. "needs only 1.",
            },
            {
                Name = "Texture",
                Type = "Content",
                ContentKind = "Texture",
                Presentation = true,
                Doc = "The image drawn along the beam, with U along its length and V across it. With no texture, "
                    .. "the beam is a plain band of `Color`.",
            },
            {
                Name = "TextureLength",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "The length of one repeat of the texture, in meters, in the `Wrap` and `Static` texture modes.",
            },
            {
                Name = "TextureMode",
                Type = "Enum.TextureMode",
                Default = "Enum.TextureMode.Stretch",
                Presentation = true,
                Doc = "How the texture is laid along the beam.",
            },
            {
                Name = "TextureSpeed",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "How fast the texture scrolls from `Attachment0` toward `Attachment1`: in repeats per "
                    .. "second in `Stretch` mode, and in meters per second in `Wrap` mode. In `Static` mode it "
                    .. "does not scroll.",
            },
            {
                Name = "FaceCamera",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether the band turns about its own length to face the camera. When it is off, the band "
                    .. "lies in the plane of its attachments' Y axes, as a flat ribbon with two sides.",
            },
            {
                Name = "LightEmission",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much the beam adds its color to what is behind it instead of covering it, from 0 (an "
                    .. "ordinary surface) to 1 (pure light).",
            },
            {
                Name = "LightInfluence",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much the scene's lighting affects the beam's color, from 0 (always its own color) to 1 "
                    .. "(lit like a surface).",
            },
            {
                Name = "ZOffset",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "The distance, in meters, the beam is drawn toward the camera. Use it so a beam that lies "
                    .. "on a wall is not cut off by the wall.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether the beam is drawn.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Trail",
        Extends = "Instance",
        Module = "render",
        Parents = under("Workspace", "Model", "BasePart", "Attachment"),
        Storage = "Trail",
        Doc = "Draws the ribbon that two attachments leave behind as they move: the swing of a sword, a tire "
            .. "mark, or the wake of something fast. Each piece of the trail lasts `Lifetime` seconds and fades "
            .. "as `Color`, `Transparency` and `WidthScale` set.\n\n"
            .. "It is visual only: it is drawn every frame, it is not simulated, and a script cannot read where "
            .. "it is.",
        Properties = {
            {
                Name = "Attachment0",
                Type = "Attachment?",
                Presentation = true,
                Doc = "The attachment at one edge of the ribbon.",
            },
            {
                Name = "Attachment1",
                Type = "Attachment?",
                Presentation = true,
                Doc = "The attachment at the other edge of the ribbon.",
            },
            {
                Name = "Lifetime",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "How long a piece of the trail lasts, in seconds, from the moment the attachments were there.",
            },
            {
                Name = "MinLength",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "The distance the attachments must move before the trail gains a new piece, in meters. A "
                    .. "smaller value is smoother and costs more.",
            },
            {
                Name = "MaxLength",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "The greatest length of the trail, in meters. Older pieces beyond it are removed. At 0, "
                    .. "only `Lifetime` limits the trail.",
            },
            {
                Name = "Color",
                Type = "ColorSequence",
                Default = "ColorSequence.new(Color3.new(1, 1, 1))",
                Presentation = true,
                Doc = "The color of a piece over its life, from 0 when it is new to 1 at `Lifetime`. It is "
                    .. "multiplied by the texture.",
            },
            {
                Name = "Transparency",
                Type = "NumberSequence",
                Default = "NumberSequence.new(0)",
                Presentation = true,
                Doc = "How transparent a piece is over its life, from 0 (opaque) to 1 (invisible).",
            },
            {
                Name = "WidthScale",
                Type = "NumberSequence",
                Default = "NumberSequence.new(1)",
                Presentation = true,
                Doc = "The width of a piece over its life, as a fraction of the distance between the attachments.",
            },
            {
                Name = "Texture",
                Type = "Content",
                ContentKind = "Texture",
                Presentation = true,
                Doc = "The image drawn along the trail, with U along its length and V across it. With no texture, "
                    .. "the trail is a plain band of `Color`.",
            },
            {
                Name = "TextureLength",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "The length of one repeat of the texture, in meters, in the `Wrap` and `Static` texture modes.",
            },
            {
                Name = "TextureMode",
                Type = "Enum.TextureMode",
                Default = "Enum.TextureMode.Stretch",
                Presentation = true,
                Doc = "How the texture is laid along the trail.",
            },
            {
                Name = "FaceCamera",
                Type = "boolean",
                Default = "false",
                Presentation = true,
                Doc = "Whether the ribbon turns about its center line to face the camera, as wide as the distance "
                    .. "between the attachments. When it is off, the ribbon is the surface the two attachments "
                    .. "swept.",
            },
            {
                Name = "LightEmission",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much the trail adds its color to what is behind it instead of covering it, from 0 (an "
                    .. "ordinary surface) to 1 (pure light).",
            },
            {
                Name = "LightInfluence",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much the scene's lighting affects the trail's color, from 0 (always its own color) to "
                    .. "1 (lit like a surface).",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether the trail grows. When it is off, the existing trail fades out over its `Lifetime`.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Clear",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes the whole trail at once. Call it when the object teleports, so the ribbon does not "
                    .. "stretch from the old position to the new one.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "SpotLight",
        Extends = "Instance",
        Module = "render",
        Parents = OnAPart,
        Storage = "SpotLight",
        Records = { "ADR 0095" },
        Doc = "A light that shines in a cone around its forward direction. Parented to a `BasePart` or an "
            .. "`Attachment`, it shines from there, turned by its own `CFrame`, and moves with it. Parented to "
            .. "anything else, it shines from its own `CFrame`.",
        Properties = {
            {
                Name = "CFrame",
                Type = "CFrame",
                Records = { "ADR 0095" },
                Doc = "The position and direction of the light. When the light is parented to a `BasePart` or an "
                    .. "`Attachment`, this is relative to that parent, and the identity puts the light at the "
                    .. "parent. Otherwise it is in world space.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Doc = "The color of the light, multiplied by `Brightness`.",
            },
            {
                Name = "Brightness",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much light is emitted. At 0 the light emits nothing.",
            },
            {
                Name = "Range",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance, in meters, beyond which the light has no effect.",
            },
            {
                Name = "Angle",
                Type = "number",
                ErrorKey = "scene.err.number_cone_degrees",
                Doc = "The full width of the cone, in degrees.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the light shines. A disabled light does not count toward the limit on lights drawn "
                    .. "at once; a light with a `Brightness` of 0 does.",
            },
            {
                Name = "Shadows",
                Type = "boolean",
                Doc = "Whether the light casts shadows.\n\n"
                    .. "A spot light's shadow uses one of the sixteen tiles in the shadow atlas. A `PointLight` "
                    .. "uses six, and both kinds share the same atlas.\n\n"
                    .. "When more lights ask for shadows than there are tiles, the lights that are nearest and "
                    .. "largest on screen cast them and the others do not. The choice is stable from frame to "
                    .. "frame, so shadows do not flicker.",
            },
        },
    },
    {
        Name = "PostEffect",
        Extends = "Instance",
        Module = "render",
        Parents = under("Lighting", "Camera"),
        Tags = { "Abstract" } :: { Schema.ClassTag },
        -- Every effect class stores its own numbers beside this one's `Enabled`:
        -- `World::create` calls every hook down the ancestry, so the base's
        -- component and the class's are two pools, never one attached twice.
        Storage = "PostEffect",
        Records = { "ADR 0096" },
        Doc = "The base class of the effects that change the finished picture instead of the world in it. Where "
            .. "an effect is parented decides who sees it. Directly under `Lighting`, it belongs to the world: it "
            .. "is saved with the scene and everyone sees it. Directly under `Workspace.CurrentCamera`, it "
            .. "applies to whoever looks through that camera. Anywhere else it does nothing, and the editor shows "
            .. "a warning on the instance.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the effect is applied. A disabled effect is not the same as no effect: a disabled "
                    .. "`BloomEffect` still controls bloom, and turns it off.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "BloomEffect",
        Extends = "PostEffect",
        Module = "render",
        Storage = "BloomEffect",
        Doc = "Controls the glow that bright light spreads onto what is around it. The engine applies bloom "
            .. "without this instance, with the same result as this class's defaults. Adding one overrides the "
            .. "bloom, and one with `Enabled` off turns bloom off, which suits a 2D game whose white sprites "
            .. "should not glow.\n\n"
            .. "When several apply, the first enabled one is used: the children of `Lighting` are considered "
            .. "first, then the camera's.",
        Properties = {
            {
                Name = "Intensity",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The strength of the glow, as a multiple of the engine's default. 1 is the default look, 2 "
                    .. "is twice the glow, and 0 is none.",
            },
            {
                Name = "Size",
                Type = "number",
                Default = "24",
                ErrorKey = "scene.err.number_bloom_size",
                Doc = "How far the glow spreads, from 0 to 56. 24 is the engine's default. 48 spreads the same "
                    .. "light about twice as far, and fainter.",
            },
            {
                Name = "Threshold",
                Type = "number",
                Default = "1.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How bright a pixel must be, after exposure, before it glows. Exposure brings the average "
                    .. "of a frame to about 0.45, so the default blooms pixels a little more than twice as bright "
                    .. "as the average. At 0, everything glows a little.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "ColorCorrectionEffect",
        Extends = "PostEffect",
        Module = "render",
        Storage = "ColorCorrectionEffect",
        Doc = "Adjusts the color of the whole picture: its brightness, contrast, saturation and tint. It is "
            .. "applied to the exposed image before tone mapping, so a highlight it brightens still rolls off "
            .. "smoothly instead of clipping.\n\n"
            .. "Every enabled one is applied, in order: the children of `Lighting` first, then the camera's. Each "
            .. "applies its tint, then its saturation, then its contrast, then its brightness.",
        Properties = {
            {
                Name = "Brightness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_minus_one_to_one",
                Doc = "An amount of light added to every pixel, from -1 to 1. The average of a frame is about "
                    .. "0.45 after exposure, so 0.1 is a gentle lift and -0.2 is clearly darker. The darkest "
                    .. "pixels stop at black.",
            },
            {
                Name = "Contrast",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_minus_one_to_one",
                Doc = "How far every pixel is pushed away from the frame's average brightness, from -1 to 1. At 0 "
                    .. "nothing changes, at 0.5 the difference grows by half, and at -1 the picture is flattened "
                    .. "to a single gray.",
            },
            {
                Name = "Saturation",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_minus_one_to_one",
                Doc = "How vivid colors are, from -1 to 1. -1 is black and white, 0 changes nothing, and 1 "
                    .. "doubles the distance of each color from gray.",
            },
            {
                Name = "TintColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "A color the picture is multiplied by. White changes nothing; a warm white warms the picture.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "BlurEffect",
        Extends = "PostEffect",
        Module = "render",
        Storage = "BlurEffect",
        Doc = "Blurs the whole picture of the world, for example behind a pause menu. The interface is drawn "
            .. "after it and stays sharp. When several apply, their sizes combine as the square root of the sum "
            .. "of their squares: two blurs of 8 make one of about 11.3, not 16.",
        Properties = {
            {
                Name = "Size",
                Type = "number",
                Default = "24",
                ErrorKey = "scene.err.number_blur_size",
                Doc = "How far the light of a pixel is spread, in pixels of a picture 1,080 lines tall. It scales "
                    .. "with the window, so a blur looks the same at any resolution. 0 is no blur and costs "
                    .. "nothing. 100 is the maximum and leaves no detail.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "DepthOfFieldEffect",
        Extends = "PostEffect",
        Module = "render",
        Storage = "DepthOfFieldEffect",
        Doc = "Blurs the picture by distance, as a camera lens does: a band of the world is in focus, and what is "
            .. "nearer or farther is blurred. The sky counts as infinitely far away.\n\n"
            .. "When several apply, the first enabled one is used: the children of `Lighting` are considered "
            .. "first, then the camera's. It is not drawn on a machine whose graphics settings turn depth of "
            .. "field off.",
        Properties = {
            {
                Name = "FocusDistance",
                Type = "number",
                Default = "25",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance from the camera that is sharpest, in meters.",
            },
            {
                Name = "InFocusRadius",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance on either side of `FocusDistance` that stays fully sharp, in meters. The blur "
                    .. "begins beyond it.",
            },
            {
                Name = "NearIntensity",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much what is nearer than the sharp band is blurred, from 0 (not at all) to 1.",
            },
            {
                Name = "FarIntensity",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much what is farther than the sharp band is blurred, from 0 (not at all) to 1.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "SunRaysEffect",
        Extends = "PostEffect",
        Module = "render",
        Storage = "SunRaysEffect",
        Doc = "Draws shafts of light that stream from the sun past whatever is in front of it. The shafts come "
            .. "from the open sky around the sun, so a tree or a wall interrupts them. They fade as the sun "
            .. "leaves the view or sets.\n\n"
            .. "When several apply, the first enabled one is used: the children of `Lighting` are considered "
            .. "first, then the camera's. It is not drawn on a machine whose graphics settings turn sun rays off.",
        Properties = {
            {
                Name = "Intensity",
                Type = "number",
                Default = "0.25",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "The brightness of the shafts, from 0 to 1.",
            },
            {
                Name = "Spread",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How far the shafts extend from the sun, from 0 (a halo) to 1 (across the screen).",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Atmosphere",
        Extends = "Instance",
        Module = "render",
        Parents = under("Lighting"),
        Storage = "Atmosphere",
        Doc = "The air between the camera and everything it sees. Distant objects fade into it, it thins with "
            .. "height, the sun lights it and makes it glow, and it tints the sky toward the horizon so that the "
            .. "ground and the sky meet.\n\n"
            .. "It applies only directly under `Lighting`, and only the first one there is used. While one "
            .. "applies, `Lighting.FogStart`, `FogEnd` and `FogColor` are kept but not used.",
        Properties = {
            {
                Name = "Density",
                Type = "number",
                Default = "0.35",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How thick the air is, from 0 to 1. At 0 the air is perfectly clear. At the default, half "
                    .. "of an object 280 meters away still shows through, at the height of `Offset`. At 1, half "
                    .. "is hidden within 35 meters.",
            },
            {
                Name = "Offset",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Doc = "The height, in meters, at which the air has exactly its `Density`. The air is thicker "
                    .. "below this height and thinner above it, at the rate `Decay` sets. Raising it lifts the "
                    .. "haze up the sides of a valley.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.fromRGB(196, 210, 230)",
                Doc = "The color of the air when it is lit. Distant objects fade toward it, and the sky turns "
                    .. "toward it at the horizon. Its brightness still depends on the height of the sun, so the "
                    .. "same color is dark at night.",
            },
            {
                Name = "Decay",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How quickly the air thins with height, from 0 to 1. At 0 it is equally thick at every "
                    .. "height. At the default it halves every 100 meters of height, and at 1 every 10 meters.",
            },
            {
                Name = "Glare",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_ten",
                Doc = "The strength of the bright area the air forms around the sun, from 0 to 10. It brightens "
                    .. "the sky near the sun and distant objects seen in its direction.",
            },
            {
                Name = "Haze",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_ten",
                Doc = "Extra thickness near the horizon, from 0 to 10. It blends the sky's horizon into the color "
                    .. "of the air and hides far distances more than `Density` alone.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Sky",
        Extends = "Instance",
        Module = "render",
        Parents = under("Lighting"),
        Storage = "Sky",
        Doc = "Sets what the sky shows: six images around the world, the appearance of the sun and the moon, "
            .. "stars and clouds. It applies only directly under `Lighting`, and only the first one there is "
            .. "used. Surfaces also reflect the images and are lit by them.\n\n"
            .. "The direction of the sun always comes from `Lighting.ClockTime` and `GeographicLatitude`, "
            .. "whatever the images show, so shadows match the sun that is drawn. Use images with no sun painted "
            .. "in, or turn `CelestialBodiesShown` off.",
        Properties = {
            {
                Name = "SkyboxBack",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image toward +Z, before `SkyboxOrientation` rotates the set. If none of the six images "
                    .. "is set, the engine draws its own sky from the time of day.",
            },
            {
                Name = "SkyboxDown",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image directly below.",
            },
            {
                Name = "SkyboxFront",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image toward -Z, the direction a camera with no rotation looks in.",
            },
            {
                Name = "SkyboxLeft",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image toward -X.",
            },
            {
                Name = "SkyboxRight",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image toward +X.",
            },
            {
                Name = "SkyboxUp",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image directly above.",
            },
            {
                Name = "SkyboxOrientation",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "A rotation applied to the six images together, in degrees about X, Y and Z. Use it to move "
                    .. "something painted into the images, such as a mountain range, to where the world needs it.",
            },
            {
                Name = "SunTexture",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "An image drawn as the sun's disc, lit by the sun's color. With none, a round disc with a "
                    .. "soft edge is drawn.",
            },
            {
                Name = "MoonTexture",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "An image drawn as the moon's disc at night. With none, a plain pale disc is drawn.",
            },
            {
                Name = "SunAngularSize",
                Type = "number",
                Default = "2.3",
                ErrorKey = "scene.err.number_angular_size",
                Doc = "The apparent width of the sun, in degrees, greater than 0 and at most 60. The real sun is "
                    .. "about half a degree wide; the default is larger so that a sunset is visible in a short "
                    .. "game day.",
            },
            {
                Name = "MoonAngularSize",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_angular_size",
                Doc = "The apparent width of the moon, in degrees, greater than 0 and at most 60.",
            },
            {
                Name = "StarCount",
                Type = "number",
                Default = "3000",
                ErrorKey = "scene.err.number_star_count",
                Doc = "The number of stars over the whole sky at night, from 0 to 10,000. They appear as the sun "
                    .. "sets, and they are the same stars in the same places every night.",
            },
            {
                Name = "CelestialBodiesShown",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the sun, the moon and the stars are drawn. When it is off, only the images and the "
                    .. "clouds are drawn; the light still comes from the sun's direction.",
            },
            {
                Name = "CloudCover",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much of the sky the clouds cover, from 0 (none) to 1 (overcast). Clouds drift on "
                    .. "simulation time, so they stop while the game is paused and move identically in a replay.",
            },
            {
                Name = "CloudDensity",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How thick each cloud is, from 0 (wisps that the sky shows through) to 1 (solid, with dark "
                    .. "undersides).",
            },
            {
                Name = "CloudColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "The color of the clouds where the sun lights them.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "InputContext",
        Extends = "Instance",
        Module = "input",
        Storage = "InputContext",
        Records = { "ADR 0029" },
        Doc = "A group of `InputAction` children that are enabled and disabled together. To open a menu, enable "
            .. "the menu's context and disable the gameplay context, instead of checking a condition in every "
            .. "handler. It works wherever it is parented; by convention, put it under the script that uses it.",
        Properties = {
            {
                Name = "Player",
                Type = "Player?",
                Doc = "The local player whose character input receives this context's actions. With nil, the "
                    .. "primary local player is used. Only a local player that is present is accepted. If the "
                    .. "player is removed, the context's input does not move to the primary player.",
            },
            {
                Name = "GamepadId",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The id of the gamepad this context reads. At 0, the input of all gamepads is combined. "
                    .. "With a positive id, only that gamepad is read, and a disconnected one reads as neutral. "
                    .. "Keyboard, mouse and touch bindings are not affected. A context with `Sink` on consumes "
                    .. "input only from the gamepad it reads.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this context and its actions are active. A disabled context consumes no input, so "
                    .. "a lower-priority context bound to the same key starts receiving it in the same tick.",
            },
            {
                Name = "Priority",
                Type = "number",
                Default = "0",
                Records = { "ADR 0039" },
                Doc = "The order in which contexts receive input, highest first. It decides only which context is "
                    .. "offered an input first; `Rate` sets when actions are dispatched. Contexts with equal "
                    .. "priority are ordered the same way on every run.",
            },
            {
                Name = "Sink",
                Type = "boolean",
                Default = "false",
                Doc = "Whether this context consumes the inputs its bindings use, hiding them from every "
                    .. "lower-priority context. A modal dialog should sink; a HUD should not. Only the bound "
                    .. "inputs are consumed: a sinking context that binds Escape hides Escape and leaves W "
                    .. "available.",
            },
            {
                Name = "Rate",
                Type = "Enum.InputRate",
                Default = "Enum.InputRate.Simulation",
                Records = { "ADR 0039" },
                Doc = "Whether the context's actions are dispatched on simulation ticks or on rendered frames. "
                    .. "`Simulation`, the default, is deterministic and is recorded in a replay; use it for "
                    .. "gameplay. `Render` is for camera look and UI; an action read there depends on the frame "
                    .. "rate.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "InputAction",
        Extends = "Instance",
        Module = "input",
        Parents = under("InputContext"),
        Storage = "InputAction",
        Records = { "ADR 0029" },
        Doc = "A named action the player can take, separate from the input that triggers it. Game code connects "
            .. "to the action, and `InputBinding` children set which inputs drive it. Rebinding a control is then "
            .. "a property change, and a prompt can ask the action which key to show.\n\n"
            .. "Parent it to an `InputContext`. An action outside a context does nothing.",
        Properties = {
            {
                Name = "Type",
                Type = "Enum.InputActionType",
                Default = "Enum.InputActionType.Bool",
                Doc = "The kind of value the action produces, which is what `GetState` returns. Every binding of "
                    .. "the action produces this type, whether it is a key, a stick or an on-screen button.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the action is active. A disabled action consumes no input, even in a context with "
                    .. "`Sink` on, so lower-priority contexts receive it.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetState",
                Parameters = {},
                Returns = { "boolean | number | Vector2 | vector" },
                Doc = "Returns the action's value for the current tick. The type depends on the action's `Type`: "
                    .. "a boolean for `Bool`, a number for `Direction1D`, a `Vector2` for `Direction2D` and "
                    .. "`ViewportPosition`, and a `vector` for `Direction3D`. The value does not change within a "
                    .. "tick, and a replay returns the same values that were recorded.",
            },
            {
                Name = "GetPreferredBinding",
                Parameters = { { Name = "deviceType", Type = "Enum.InputDeviceType?", Default = "nil" } },
                Returns = { "InputBinding?" },
                Doc = 'Returns the binding a prompt should show for this action, as in "Press [E]". With a device '
                    .. "type, it returns the first child binding for that kind of device. Without one, it uses "
                    .. "`InputService.LastInputDeviceType`, so the prompt follows the device the player last "
                    .. "used. Returns nil if the action has no binding for that device.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Pressed",
                Parameters = {},
                Doc = "Fires on the tick a `Bool` action becomes true. It does not fire for other action types.",
            },
            {
                Name = "Released",
                Parameters = {},
                Doc = "Fires on the tick a `Bool` action becomes false. It also fires if the action or its "
                    .. "context is disabled while the action is held.",
            },
            {
                Name = "StateChanged",
                Parameters = {},
                Records = { "ADR 0039" },
                Doc = "Fires on any tick in which the action's value changed, for every action type. It passes no "
                    .. "arguments; call `GetState` in the handler to read the new value.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "InputBinding",
        Extends = "Instance",
        Module = "input",
        Parents = under("InputAction"),
        Storage = "InputBinding",
        Doc = "One physical input that drives its parent `InputAction`. An action can have several bindings, and "
            .. "any of them triggers it: a key, a gamepad stick and an on-screen button can all drive the same "
            .. "action.\n\n"
            .. "Which properties are used depends on the parent's `Type`. A `Bool` action uses `KeyCode`. A "
            .. "`Direction2D` action uses either a `KeyCode` that names a 2D input such as `LeftThumbstick`, or "
            .. "the four keys `Up`, `Down`, `Left` and `Right`. A binding whose properties do not fit its "
            .. "parent's type does nothing; it is not an error.",
        Properties = {
            {
                Name = "KeyCode",
                Type = "Enum.KeyCode",
                Default = "Enum.KeyCode.Unknown",
                Records = { "ADR 0039" },
                Doc = "The input this binding uses. `Unknown` means the binding is not bound.",
            },
            {
                Name = "Up",
                Type = "Enum.KeyCode",
                Default = "Enum.KeyCode.Unknown",
                Doc = "The key for +Y on a `Direction1D` or `Direction2D` action. The four direction keys let a "
                    .. "keyboard drive an axis: W, A, S and D are one binding, not four.",
            },
            {
                Name = "Down",
                Type = "Enum.KeyCode",
                Default = "Enum.KeyCode.Unknown",
                Doc = "The key for -Y, used with `Up`.",
            },
            {
                Name = "Left",
                Type = "Enum.KeyCode",
                Default = "Enum.KeyCode.Unknown",
                Doc = "The key for -X, used with `Right`.",
            },
            {
                Name = "Right",
                Type = "Enum.KeyCode",
                Default = "Enum.KeyCode.Unknown",
                Doc = "The key for +X, used with `Left`.",
            },
            {
                Name = "Scale",
                Type = "number",
                Default = "1",
                Doc = "A multiplier on the binding's value before it reaches the action. Use a small value for "
                    .. 'mouse look and 1 for a stick. A negative value inverts the axis, as an "invert Y" setting '
                    .. "does.",
            },
            {
                Name = "DisplayName",
                Type = "string",
                Default = '""',
                Doc = "The text a prompt shows for this binding. It can be a localization key, which lets the "
                    .. "prompt be translated. When it is empty, a prompt should use the name of the key.\n\n"
                    .. "The engine does not use this value. A game reads it through `GetPreferredBinding` to draw "
                    .. "its own prompts.",
            },
            {
                Name = "Image",
                Type = "Content",
                ContentKind = "Texture",
                Default = '""',
                Doc = "An icon for this binding, as an `asset://` path. When it is empty, a prompt should show "
                    .. "`DisplayName` instead.\n\n"
                    .. "The engine does not use this value or load the image. A game reads it through "
                    .. "`GetPreferredBinding` and shows it in its own prompt, for example in an `ImageLabel`.",
            },
            {
                Name = "DeviceType",
                Type = "Enum.InputDeviceType",
                ReadOnly = true,
                Records = { "ADR 0039" },
                Doc = "The kind of device this binding belongs to, derived from `KeyCode`. Every input that is "
                    .. "not a gamepad input reports `KeyboardMouse`, including `Unknown`.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "ScreenGui",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIService"),
        Storage = "ScreenGui",
        Doc = "The root of a tree of UI drawn on the screen. Parent it to `UIService`. Everything under it is "
            .. "laid out against the window and drawn over the world.\n\n"
            .. "Layout is computed per `ScreenGui`, and only when something under it changes. A screen where "
            .. "nothing changed costs no layout work.",
        Properties = {
            {
                Name = "Player",
                Type = "Player?",
                Default = "nil",
                Presentation = true,
                Records = { "ADR 0205" },
                Doc = "The local player this interface belongs to. With a player set, the interface is laid out "
                    .. "in that player's view when the window is split, it has its own selection "
                    .. "(`SelectedObject`), and only that player's gamepad (`Player.GamepadId`) navigates it. "
                    .. "Four players can each pick a card at the same time.\n\n"
                    .. "With nil, the interface belongs to everyone: it covers the whole window and is navigated "
                    .. "by `UIService.GamepadId`. When the player leaves, the interface is hidden until it is "
                    .. "given another player or nil.",
            },
            {
                Name = "SelectedObject",
                Type = "UIObject?",
                Default = "nil",
                Transient = true,
                -- Where a hand has the interface, like the focus: not the world's.
                HostFact = true,
                Doc = "The object this player's gamepad has selected in this interface. It is used only when "
                    .. "`Player` is set; otherwise `UIService.SelectedObject` is used.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this tree is laid out and drawn. A disabled `ScreenGui` costs nothing.",
            },
            {
                Name = "DisplayOrder",
                Type = "number",
                Default = "0",
                Doc = "Which `ScreenGui` is drawn on top where two overlap: the one with the higher value. "
                    .. "`ZIndex` orders objects within a tree, and this orders the trees.",
            },
            {
                Name = "ReferenceHeight",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The window height this interface was designed for, or 0 to work in pixels. When it is set, "
                    .. "every offset, text size, corner radius and stroke under it is measured in units of a "
                    .. "window that tall, and the whole tree is scaled to the actual window. A layout designed at "
                    .. "720 then takes up the same share of a phone screen with twice the pixels, instead of "
                    .. "half.\n\n"
                    .. "The width follows from the window's aspect ratio, so anchor to edges with `Scale` and "
                    .. "size with `Offset`. `AbsolutePosition` and `AbsoluteSize` stay in window pixels. 720 "
                    .. "suits a layout designed on a desktop.",
            },
            {
                Name = "ScreenInsets",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this tree is laid out inside `UIService.SafeAreaInsets` instead of against the "
                    .. "whole window. On by default, so that a HUD is not hidden by a screen cutout. Turn it off "
                    .. "for a background that should fill the screen.",
            },
            {
                Name = "KeepOnSceneLoad",
                Type = "boolean",
                Default = "false",
                Records = { "ADR 0106" },
                Doc = "Whether this screen is kept when the game changes scene, instead of being removed with the "
                    .. "scene it is in. A loading screen shown before `SceneService:LoadScene` stays up while the "
                    .. "scene changes, and a script removes it on `SceneLoaded`.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "BillboardGui",
        Extends = "Instance",
        Module = "ui",
        Parents = OverAPart,
        Storage = "BillboardGui",
        Doc = "A tree of UI placed in the world and turned to face the camera: a name over a head, a health bar "
            .. "over a crate or a marker on an objective. Its children are laid out as in a `ScreenGui`, against "
            .. "a canvas of `Size`. It is drawn in the world, behind whatever is in front of it and under the "
            .. "scene's lighting, not over the picture.\n\n"
            .. "It is placed over `Adornee`, or over its parent when the parent is a part and `Adornee` is nil. "
            .. "Its buttons work like those on screen: the pointer's ray finds them in the world, and a solid "
            .. "object in front blocks them unless `AlwaysOnTop` is on. A screen element at the same pixel takes "
            .. "the press first.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the billboard is laid out and drawn.",
            },
            {
                Name = "Adornee",
                Type = "(BasePart | Part2D)?",
                Doc = "The part the billboard is placed over: a part in the world, or a `Part2D` on the 2D plane. "
                    .. "With nil, its parent is used if the parent is one of those. A billboard with neither is "
                    .. "not drawn.",
            },
            {
                Name = "Size",
                Type = "UDim2",
                Default = "UDim2.new(0, 200, 0, 50)",
                Doc = "The size of the billboard. The scale is in meters and the offset is in pixels. A scale "
                    .. "keeps its size in the world, so the billboard shrinks with distance. An offset keeps its "
                    .. "size on screen, so a name tag is as readable from far away as from nearby. Children are "
                    .. "laid out against the size in pixels in both cases.",
            },
            {
                Name = "WorldOffset",
                Type = "vector",
                Default = "vector.zero",
                Doc = "The position of the billboard's center relative to the center of the adornee, in meters "
                    .. "along the world's axes. `vector.create(0, 3, 0)` is three meters above the part, however "
                    .. "the part is rotated.",
            },
            {
                Name = "AlwaysOnTop",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the billboard shows through walls. When it is off, objects in front of it hide it.",
            },
            {
                Name = "MaxDistance",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance from the camera, in meters, beyond which the billboard is not drawn. 0 means "
                    .. "no limit.",
            },
            {
                Name = "Brightness",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "A multiplier on the billboard's colors in the scene's lighting. Above 1 it glows and blooms.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "SurfaceGui",
        Extends = "Instance",
        Module = "ui",
        Parents = under("BasePart"),
        Storage = "SurfaceGui",
        Doc = "A tree of UI drawn on one face of a part: a screen on a wall, a sign, a scoreboard or a label on a "
            .. "crate. Its children are laid out as in a `ScreenGui`, against a canvas the size of the face at "
            .. "`PixelsPerMeter`. It moves, turns and is hidden with the part.\n\n"
            .. "It covers `Adornee`, or its parent when the parent is a part and `Adornee` is nil. Its buttons "
            .. "work like those on screen, from in front of the face: the pointer's ray finds them in the world, "
            .. "and a solid object in front blocks them unless `AlwaysOnTop` is on.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the interface is laid out and drawn.",
            },
            {
                Name = "Adornee",
                Type = "BasePart?",
                Doc = "The part the interface covers. With nil, its parent is used if the parent is a part.",
            },
            {
                Name = "Face",
                Type = "Enum.Face",
                Default = "Enum.Face.Front",
                Doc = "The face of the part the interface covers. `Front` is the face the part's `LookVector` "
                    .. "points out of. On every side face the interface is the right way up.",
            },
            {
                Name = "PixelsPerMeter",
                Aliases = { "PixelsPerMetre" },
                Type = "number",
                Default = "50",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The number of canvas pixels per meter of the face. Offsets and `TextSize` under the "
                    .. "interface are measured in these pixels. At 50, text of size 14 is about 0.28 meters tall "
                    .. "on a wall; a higher value gives finer text on the same face.",
            },
            {
                Name = "AlwaysOnTop",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the interface shows through objects in front of the face.",
            },
            {
                Name = "Brightness",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "A multiplier on the interface's colors in the scene's lighting. Above 1 it glows, like a "
                    .. "lit screen.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIObject",
        Extends = "Instance",
        Module = "ui",
        Parents = InAUi,
        Tags = { "Abstract", "NotCreatable" } :: { Schema.ClassTag },
        Storage = "UIObject",
        Doc = "The base class of everything that occupies a rectangle in a UI tree. Its properties set where the "
            .. "rectangle is, how it is drawn, and how it responds to the pointer and to selection.\n\n"
            .. "Positions and sizes are `UDim2` values: on each axis, a fraction of the parent plus an offset in "
            .. "pixels. That lets one layout work at every resolution.",
        Properties = {
            {
                Name = "Position",
                Type = "UDim2",
                Default = "UDim2.new()",
                Doc = "The position of this object's `AnchorPoint` inside its parent.",
            },
            {
                Name = "Size",
                Type = "UDim2",
                Default = "UDim2.new()",
                Doc = "The size of the object: a fraction of its parent plus an offset. It is ignored on an axis "
                    .. "that `AutomaticSize` covers.",
            },
            {
                Name = "AnchorPoint",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "The point of this object that `Position` places, as a fraction of the object's own size. "
                    .. "(0, 0) is its top left corner and (0.5, 0.5) is its center.",
            },
            {
                Name = "Rotation",
                Type = "number",
                Default = "0",
                Doc = "The rotation of the object, in degrees clockwise about its anchor point. It rotates how "
                    .. "the object is drawn, not its layout: a rotated object still occupies its unrotated "
                    .. "rectangle.\n\n"
                    .. "Descendants rotate with it, and a descendant's own `Rotation` is added. A dial rotated 30 "
                    .. "degrees with a needle rotated 90 draws the needle at 120.\n\n"
                    .. "Rotation does not affect three things. Layout ignores it. Hit testing uses the unrotated "
                    .. "rectangle. `ClipsDescendants` clips to an upright rectangle, so a rotated child of a "
                    .. "clipping parent is clipped by the parent's unrotated box.",
            },
            {
                Name = "BackgroundColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "The color of the fill behind the object's content.",
            },
            {
                Name = "BackgroundTransparency",
                Type = "number",
                Default = "0",
                Doc = "How transparent the background is, from 0 (opaque) to 1 (invisible). An object with an "
                    .. "invisible background is still laid out; `Active` decides whether it receives the pointer.",
            },
            {
                Name = "Active",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the object receives the pointer: whether a press lands on it and whether the "
                    .. "pointer counts as over it. A press on an active object belongs to the interface, which a "
                    .. "game sees as `uiConsumed` in `InputService.InputBegan`. A press on an inactive object "
                    .. "passes to whatever is under it and to the game.\n\n"
                    .. "Until you set it, the value depends on the class. A button, a `TextInput` and a "
                    .. "`ScrollFrame` are active. A `TextLabel` and an `ImageLabel` are not, so a press on the "
                    .. "icon or caption inside a button reaches the button. A `Frame` is active when it draws a "
                    .. "background (`BackgroundTransparency` below 1), so a backdrop behind a dialog blocks what "
                    .. "is under it, and a transparent frame used only for layout lets presses through.\n\n"
                    .. "Once you set it, your value is used: true on a transparent frame makes an invisible "
                    .. "blocker, and false on a button makes it decorative. No object receives the pointer while "
                    .. "`InputService.PointerLocked` is on.",
            },
            {
                Name = "Visible",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this object and its descendants are laid out, drawn and able to receive the "
                    .. "pointer. An invisible object does none of the three.",
            },
            {
                Name = "ZIndex",
                Type = "number",
                Default = "0",
                Doc = "The drawing order within the `ScreenGui`: higher values are drawn later, on top. Objects "
                    .. "with equal values are drawn in tree order. It does not affect layout, and it is one "
                    .. "ordering across the whole tree, not one per parent.",
            },
            {
                Name = "LayoutOrder",
                Type = "number",
                Default = "0",
                Doc = "The position of this child in a `UIListLayout` whose `SortOrder` is `LayoutOrder`. It is "
                    .. "ignored otherwise.",
            },
            {
                Name = "AutomaticSize",
                Type = "Enum.AutomaticSize",
                Default = "Enum.AutomaticSize.None",
                Doc = "The axes on which the object sizes itself to fit its contents. On those axes, `Size` is "
                    .. "ignored.",
            },
            {
                Name = "ClipsDescendants",
                Type = "boolean",
                Default = "false",
                Doc = "Whether descendants are clipped to this object's rectangle. A scrolling list needs it on; "
                    .. "an object with a drop shadow outside its rectangle needs it off.",
            },
            {
                Name = "AbsolutePosition",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                Doc = "The position of the object's top left corner after layout, in window pixels.",
            },
            {
                Name = "AbsoluteSize",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                Doc = "The size of the object after layout, in window pixels.",
            },
            {
                Name = "Selectable",
                Type = "boolean",
                Default = "false",
                Records = { "ADR 0128" },
                Doc = "Whether a gamepad or the arrow keys can select this object. Until you set it, the value "
                    .. "depends on the class: a button and a `TextInput` are selectable, and nothing else is. The "
                    .. "selected object is `UIService.SelectedObject`, and the gamepad's confirm button fires its "
                    .. "`Activated`.",
            },
            {
                Name = "NextSelectionUp",
                Type = "UIObject?",
                Doc = "The object that is selected when the selection moves up from this one. With nil, the "
                    .. "nearest selectable object above it is chosen by position.",
            },
            {
                Name = "NextSelectionDown",
                Type = "UIObject?",
                Doc = "The object that is selected when the selection moves down from this one. With nil, the "
                    .. "nearest selectable object below it is chosen by position.",
            },
            {
                Name = "NextSelectionLeft",
                Type = "UIObject?",
                Doc = "The object that is selected when the selection moves left from this one. With nil, the "
                    .. "nearest selectable object to its left is chosen by position.",
            },
            {
                Name = "NextSelectionRight",
                Type = "UIObject?",
                Doc = "The object that is selected when the selection moves right from this one. With nil, the "
                    .. "nearest selectable object to its right is chosen by position.",
            },
            {
                Name = "SelectionImageObject",
                Type = "UIObject?",
                Doc = "An object drawn over this one while it is selected, in place of the default outline. Its "
                    .. "background, image, corners, gradient and strokes are drawn at this object's rectangle. "
                    .. "Its children are not drawn.",
            },
        } :: { Schema.Property },
        Events = {
            {
                Name = "Activated",
                Parameters = {},
                Doc = "Fires when the object is activated: a pointer press and release both land on it, or the "
                    .. "gamepad's confirm button is pressed while it is selected. A mouse click, a tap and a "
                    .. "gamepad button all fire it.",
            },
            {
                Name = "PointerEntered",
                Parameters = {},
                Doc = "Fires when the pointer moves onto the object. The pointer is a mouse cursor or a finger.",
            },
            {
                Name = "PointerExited",
                Parameters = {},
                Doc = "Fires when the pointer moves off the object.",
            },
            {
                Name = "SelectionGained",
                Parameters = {},
                Doc = "Fires when the object becomes `UIService.SelectedObject`.",
            },
            {
                Name = "SelectionLost",
                Parameters = {},
                Doc = "Fires when the object stops being `UIService.SelectedObject`.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "Frame",
        Extends = "UIObject",
        Module = "ui",
        Doc = "A plain rectangle: a background and a container for other UI objects. Most interfaces are built "
            .. "mainly from frames.",
    },
    {
        Name = "ViewportFrame",
        Extends = "UIObject",
        Module = "ui",
        Storage = "ViewportFrame",
        Records = { "ADR 0107" },
        Doc = "A UI object that draws the parts and models parented inside it: an item turning in an inventory "
            .. "slot, a character preview or a 3D icon. Its contents are drawn only here and are not simulated. "
            .. "There is no physics and no collision, so move them from a script by setting `CFrame`.\n\n"
            .. "It is drawn again only when something inside it, its camera or its size changes, so forty still "
            .. "items cost forty pictures once. It is lit by a simple light of its own, with no sky. With "
            .. "`BackgroundTransparency` at 1, only the instances show. It also works inside a `SurfaceGui`.",
        Properties = {
            {
                Name = "CurrentCamera",
                Type = "Camera?",
                Doc = "A `Camera` inside the frame to look through. With nil, the frame's contents are framed "
                    .. "automatically, from the front and slightly above.",
            },
            {
                Name = "Ambient",
                Type = "Color3",
                Default = "Color3.fromRGB(140, 140, 150)",
                Doc = "The color of the light that reaches the contents from every direction.",
            },
            {
                Name = "LightColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "The color of the frame's directional light.",
            },
            {
                Name = "LightDirection",
                Type = "vector",
                Default = "vector.create(-1, -1, -1)",
                Doc = "The direction in which the frame's directional light travels. By default it points down "
                    .. "and away from the viewer.",
            },
        },
    },
    {
        Name = "TextLabel",
        Extends = "UIObject",
        Module = "ui",
        Storage = "TextLabel",
        Doc = "A UI object that shows text. Set `RichText` to use markup inside the text.\n\n"
            .. "Text is laid out left to right, one character after another, with no kerning and no "
            .. "complex-script shaping. Scripts that need shaping, such as Arabic, Devanagari and Thai, are not "
            .. "displayed correctly.",
        Properties = {
            {
                Name = "Text",
                Type = "string",
                Default = '""',
                Doc = "The text to show, in UTF-8. A character the font does not have is drawn as the font's "
                    .. "missing-glyph box.",
            },
            {
                Name = "TextColor",
                Type = "Color3",
                Default = "Color3.new(0, 0, 0)",
                Doc = "The color of the text.",
            },
            {
                Name = "TextTransparency",
                Type = "number",
                Default = "0",
                Doc = "How transparent the text is, from 0 (opaque) to 1 (invisible). It is separate from "
                    .. "`BackgroundTransparency`, so a label can fade its text and keep its background, or the "
                    .. "reverse. A value outside 0 to 1 is kept as set and drawn as the nearer limit.",
            },
            {
                Name = "TextSize",
                Type = "number",
                Default = "14",
                Doc = "The size of the text, in pixels. Glyphs are drawn at the nearest whole size; the layout "
                    .. "uses the exact value.",
            },
            {
                Name = "Font",
                Type = "Content",
                ContentKind = "Font",
                Default = '""',
                Doc = "The font, as an `asset://` path to a TrueType file. An empty value, or the name `Inter`, "
                    .. "uses the engine's default font, Inter.\n\n"
                    .. "A font that cannot be found falls back to the default, and a message is logged once. If "
                    .. "the project has no font file at all, a built-in font is used that has only ASCII "
                    .. "characters and one weight.",
            },
            {
                Name = "TextXAlignment",
                Type = "Enum.HorizontalAlignment",
                Default = "Enum.HorizontalAlignment.Center",
                Doc = "The horizontal position of the text in the object: `Left`, `Center` or `Right`.",
            },
            {
                Name = "TextYAlignment",
                Type = "Enum.VerticalAlignment",
                Default = "Enum.VerticalAlignment.Center",
                Doc = "The vertical position of the text in the object: `Top`, `Center` or `Bottom`.",
            },
            {
                Name = "TextWrapped",
                Type = "boolean",
                Default = "false",
                Doc = "Whether lines break at the object's width. Lines break at spaces. A word is broken only if "
                    .. "it is wider than the object.",
            },
            {
                Name = "TextOverflow",
                Type = "Enum.TextOverflow",
                Default = "Enum.TextOverflow.Overflow",
                Records = { "ADR 0168" },
                Doc = "What happens to text that does not fit in the object, such as a long player name in a "
                    .. "table row.\n\n"
                    .. "- `Overflow` draws the text beyond the object's edges.\n"
                    .. "- `Clip` cuts the text at the object's edges.\n"
                    .. "- `Ellipsis` shortens the text and ends it with an ellipsis. The text is measured in the "
                    .. "label's font at the size it is drawn, so as much as fits is kept. On a single line, each "
                    .. "line wider than the object is shortened. With `TextWrapped`, the lines that fit in the "
                    .. "object's height are kept and the last one is shortened.\n\n"
                    .. "`Text` itself is never changed. Nothing is cut when `TextScaled` is on, or on an axis "
                    .. "that `AutomaticSize` covers. With `RichText`, `Ellipsis` behaves like `Clip`. A "
                    .. "`TextInput` scrolls its text and ignores this property.",
            },
            {
                Name = "RichText",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `Text` is read as markup, so that color, size and weight can change within one "
                    .. "label.\n\n"
                    .. "The tags are `<b>` bold, `<i>` italic, `<u>` underline, `<s>` strikethrough, `<font "
                    .. 'color="#rrggbb" size="20" transparency="0.5">` with any of its three attributes, and '
                    .. "`<br/>` for a line break. `color` also accepts `rgb(r, g, b)`. Tags can be nested, and "
                    .. "each must be closed. Write `&lt;`, `&gt;`, `&amp;`, `&quot;` and `&apos;` for the literal "
                    .. "characters.\n\n"
                    .. "A tag that is not recognized is shown as text. Bold and italic are synthesized from the "
                    .. "label's one font file. A `TextInput` always shows its text without markup.",
            },
            {
                Name = "TextScaled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the text is drawn at the largest size that fits the object, ignoring `TextSize`.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "TextButton",
        Extends = "TextLabel",
        Module = "ui",
        Doc = "A `TextLabel` meant to be pressed. It adds no properties of its own: `Activated`, `PointerEntered` "
            .. "and `PointerExited` come from `UIObject`.",
    },
    {
        Name = "TextInput",
        Extends = "TextLabel",
        Module = "ui",
        Storage = "TextInput",
        Records = { "ADR 0139" },
        Doc = "A text field the player can type in, with one line or several: a chat box, a name or a server "
            .. "address.\n\n"
            .. "It supports the usual editing keys: selection with Shift and with the mouse, movement and "
            .. "deletion by word with Ctrl, Ctrl+A, copy, cut and paste through the system clipboard, undo and "
            .. "redo, a double press to select a word and a triple press to select a line. It treats an accented "
            .. "letter or an emoji as one character and shows an input method's composition at the caret. While "
            .. "it has focus, it takes the keyboard, so typing does not trigger game actions.\n\n"
            .. "Setting `Text` from a script moves the caret to the end and clears the undo history.",
        Properties = {
            {
                Name = "PlaceholderText",
                Type = "string",
                Default = '""',
                Doc = "The text shown while `Text` is empty and the field does not have focus.",
            },
            {
                Name = "PlaceholderColor",
                Type = "Color3",
                Default = "Color3.fromRGB(178, 178, 178)",
                Doc = "The color of the placeholder text.",
            },
            {
                Name = "MultiLine",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the field holds several lines. With it on, Return starts a new line and "
                    .. "Ctrl+Return submits, the text wraps at the field's width and scrolls to keep the caret "
                    .. "visible, and Up and Down move between lines. A single-line field turns a pasted line "
                    .. "break into a space.",
            },
            {
                Name = "Editable",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the player can change the text. A field that is not editable still takes focus, "
                    .. "and its text can be selected and copied. Use that to show a code or an address to copy.",
            },
            {
                Name = "MaxLength",
                Type = "number",
                Default = "0",
                Doc = "The largest number of characters the field holds; 0 means no limit. An accented letter or "
                    .. "an emoji counts as one character. Text that does not fit is left out, and `InputRejected` "
                    .. "fires with it.",
            },
            {
                Name = "Masked",
                Type = "boolean",
                Default = "false",
                Doc = "Whether each character is drawn as `MaskCharacter`, for a password. Copy and cut are "
                    .. "disabled; paste still works. A phone shows its password keyboard.",
            },
            {
                Name = "MaskCharacter",
                Type = "string",
                Default = '"\u{2022}"',
                Doc = "The character each character of a masked field is drawn as. Only the first character of "
                    .. "this string is used.",
            },
            {
                Name = "ClearTextOnFocus",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the field is emptied each time it gains focus.",
            },
            {
                Name = "SelectAllOnFocus",
                Type = "boolean",
                Default = "false",
                Doc = "Whether all the text is selected when the field gains focus, so that typing replaces it.",
            },
            {
                Name = "ReleaseFocusOnSubmit",
                Type = "boolean",
                Default = "true",
                Doc = "Whether Return releases focus as well as submitting. Turn it off for a chat box, where the "
                    .. "player types one message after another. `Submitted` fires in both cases.",
            },
            {
                Name = "RevertOnEscape",
                Type = "boolean",
                Default = "false",
                Doc = "Whether Escape restores the text the field had when it gained focus, as it releases focus.",
            },
            {
                Name = "KeyboardType",
                Type = "Enum.TextInputKeyboard",
                Default = "Enum.TextInputKeyboard.Default",
                Doc = "The on-screen keyboard a phone shows when the field gains focus.",
            },
            {
                Name = "CursorPosition",
                Type = "number",
                Default = "-1",
                -- Where a player's caret is, not something a scene says.
                Transient = true,
                Doc = "The position of the caret, as a 1-based byte position like those of a Luau string; `#Text "
                    .. "+ 1` is after the last character. It is -1 while the field does not have focus. Setting "
                    .. "it moves the caret to the nearest character boundary.",
            },
            {
                Name = "SelectionStart",
                Type = "number",
                Default = "-1",
                -- Where a player's caret is, not something a scene says.
                Transient = true,
                Doc = "The other end of the selection, as a position like `CursorPosition`, or -1 when nothing is "
                    .. "selected. Setting it selects from that position to the caret.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "CaptureFocus",
                Parameters = {},
                Returns = {},
                Doc = "Gives the field keyboard focus, as a press on it does. A field that had focus loses it "
                    .. "with `Enum.FocusLossReason.Script`. It takes effect at the next frame.",
            },
            {
                Name = "ReleaseFocus",
                Parameters = { { Name = "submitted", Type = "boolean?" } },
                Returns = {},
                Doc = "Releases keyboard focus. `FocusLost` fires with `submitted`, which is false if omitted, "
                    .. "and `Enum.FocusLossReason.Script`. It takes effect at the next frame.",
            },
            {
                Name = "IsFocused",
                Parameters = {},
                Returns = { "boolean" },
                Doc = "Returns whether the field has keyboard focus.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Focused",
                Parameters = {},
                Doc = "Fires when the field gains keyboard focus.",
            },
            {
                Name = "FocusLost",
                Parameters = {
                    { Name = "submitted", Type = "boolean" },
                    { Name = "reason", Type = "Enum.FocusLossReason" },
                },
                Doc = "Fires when the field loses keyboard focus. `submitted` is true when the player pressed "
                    .. "Return and false when focus was lost another way, such as a press elsewhere. `reason` "
                    .. "says which of the four ways it was.",
            },
            {
                Name = "Submitted",
                Parameters = { { Name = "text", Type = "string" } },
                Doc = "Fires when the player presses Return, with the text at that moment. It fires whether or "
                    .. "not the field then releases focus.",
            },
            {
                Name = "TextChanged",
                Parameters = { { Name = "text", Type = "string" } },
                Doc = "Fires when the player changes the text by typing, pasting, cutting or undoing, with the "
                    .. 'new text. It does not fire when a script sets `Text`; `GetPropertyChangedSignal("Text")` '
                    .. "fires for both.",
            },
            {
                Name = "InputRejected",
                Parameters = { { Name = "rejected", Type = "string" } },
                Doc = "Fires with the text that `MaxLength` kept out of an edit.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "ImageLabel",
        Extends = "UIObject",
        Module = "ui",
        Storage = "ImageLabel",
        Doc = "A UI object that shows an image.",
        Properties = {
            {
                Name = "Image",
                Type = "Content",
                ContentKind = "Texture",
                Default = '""',
                Doc = "The image to show, as an `asset://` path. With an empty value no image is drawn; the "
                    .. "background still is. An image that does not exist, or that has not finished loading, is "
                    .. "drawn as a plain rectangle of `ImageColor`.",
            },
            {
                Name = "ImageColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "A color the image is multiplied by, so that one white image can be drawn in any color. "
                    .. "White changes nothing.",
            },
            {
                Name = "ImageTransparency",
                Type = "number",
                Default = "0",
                Doc = "How transparent the image is, from 0 (opaque) to 1 (invisible). It is separate from "
                    .. "`BackgroundTransparency`, so an icon can fade while its background stays, or the reverse. "
                    .. "A value outside 0 to 1 is kept as set and drawn as the nearer limit.",
            },
            {
                Name = "ScaleType",
                Type = "Enum.ScaleType",
                Default = "Enum.ScaleType.Stretch",
                Doc = "How the image fits the object. With `Tile`, an image that would repeat more than about "
                    .. "four thousand times is stretched instead.",
            },
            {
                Name = "SliceCenter",
                Type = "Rect",
                Default = "Rect.new(Vector2.zero, Vector2.zero)",
                Doc = "For `ScaleType.Slice`: the rectangle that divides the image into nine slices, in pixels of "
                    .. "the image. The four corners are drawn at their own size whatever the size of the object, "
                    .. "so a panel keeps its rounded corners.\n\n"
                    .. "If the object is narrower than the two corners together, the center is removed and the "
                    .. "corners do not overlap. An empty or inverted rectangle is used as given.",
            },
            {
                Name = "ImageRectOffset",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "The top left corner of the part of the image to show, in pixels of the image. Use it with "
                    .. "`ImageRectSize` to show one frame of a sprite sheet or one icon of an atlas.",
            },
            {
                Name = "ImageRectSize",
                Type = "Vector2",
                Default = "Vector2.zero",
                Records = { "ADR 0107" },
                Doc = "The size of the part of the image to show, in pixels of the image, starting at "
                    .. "`ImageRectOffset`. Zero shows the whole image. A negative size mirrors the image along "
                    .. "that axis.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "ImageButton",
        Extends = "ImageLabel",
        Module = "ui",
        Doc = "An `ImageLabel` meant to be pressed. It adds no properties of its own: `Activated`, "
            .. "`PointerEntered` and `PointerExited` come from `UIObject`.",
    },
    {
        Name = "ScrollFrame",
        Extends = "UIObject",
        Module = "ui",
        Storage = "ScrollFrame",
        Doc = "A `Frame` whose contents can be larger than it is, and that scrolls to show them. It always clips "
            .. "its descendants, whatever `ClipsDescendants` is set to.",
        Properties = {
            {
                Name = "CanvasSize",
                Type = "UDim2",
                Default = "UDim2.new()",
                Doc = "The size of the scrollable area. Children are laid out against this size instead of the "
                    .. "frame's own size.",
            },
            {
                Name = "CanvasPosition",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "How far the canvas is scrolled, in pixels. At the next layout it is clamped to the range "
                    .. "the canvas allows.",
            },
            {
                Name = "ScrollBarThickness",
                Type = "number",
                Default = "12",
                Doc = "The thickness of the scroll bar, in pixels. At 0 no bar is drawn and the frame still "
                    .. "scrolls, which suits a touch screen.\n\n"
                    .. "A bar appears only on an axis that can scroll. The length of the thumb shows how much of "
                    .. "the canvas is visible, and it is never shorter than the bar is thick. The thumb can be "
                    .. "dragged.",
            },
            {
                Name = "ScrollBarImageColor",
                Type = "Color3",
                Default = "Color3.new(0.5, 0.5, 0.5)",
                Doc = "The color of the scroll bar. The thumb is drawn in this color, and the track under it in "
                    .. "the same color at a quarter of the opacity.",
            },
            {
                Name = "ScrollBarImageTransparency",
                Type = "number",
                Default = "0",
                Doc = "How transparent the scroll bar is, from 0 (opaque) to 1 (invisible; the frame still "
                    .. "scrolls). It is separate from `BackgroundTransparency`.",
            },
            {
                Name = "ScrollingEnabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the player can scroll the frame, with the wheel, a drag, a fling, the bar or by "
                    .. "moving the selection out of view. A script can still scroll it by setting `CanvasPosition`.",
            },
            {
                Name = "ScrollingDirection",
                Type = "Enum.ScrollingDirection",
                Default = "Enum.ScrollingDirection.XY",
                Doc = "The axes along which the player can scroll. When a scroll frame is inside another, a drag "
                    .. "goes to the one that scrolls in the direction the drag started, and stays with it until "
                    .. "the pointer is released. A vertical list inside a horizontal carousel scrolls on a "
                    .. "vertical drag and leaves a horizontal drag to the carousel.",
            },
            {
                Name = "ElasticBehavior",
                Type = "Enum.ElasticBehavior",
                Default = "Enum.ElasticBehavior.WhenScrollable",
                Doc = "Whether the content stretches when dragged beyond the end and springs back on release, and "
                    .. "whether a fling that reaches the end bounces. The stretch is only drawn: `CanvasPosition` "
                    .. "never leaves the range of the canvas.",
            },
            {
                Name = "AutomaticCanvasSize",
                Type = "Enum.AutomaticSize",
                Default = "Enum.AutomaticSize.None",
                Doc = "The axes on which the canvas grows to hold its contents. The contents are measured as the "
                    .. "extent of a `UIListLayout` or `UIGridLayout`, or else as the farthest point any child "
                    .. "reaches, plus the frame's `UIPadding`. On those axes the canvas is the larger of "
                    .. "`CanvasSize` and the contents, so a list that a script adds rows to scrolls without the "
                    .. "script measuring it.",
            },
            {
                Name = "AbsoluteCanvasSize",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                -- What the interface found, not what the world is.
                HostFact = true,
                Doc = "The size of the canvas after layout, in pixels. It is never smaller than the frame, and it "
                    .. "includes the effect of `AutomaticCanvasSize`.",
            },
            {
                Name = "AbsoluteWindowSize",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                HostFact = true,
                Doc = "The size of the visible part of the canvas after layout, in pixels, which is the frame's "
                    .. "own size. `AbsoluteCanvasSize - AbsoluteWindowSize` is how far the frame can scroll.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIListLayout",
        Extends = "Instance",
        Module = "ui",
        Parents = InAUi,
        Storage = "UIListLayout",
        Doc = "Arranges the `UIObject` children of its parent in a row or a column. Parent it next to the objects "
            .. "it arranges, not above them.\n\n"
            .. "A child arranged by a layout keeps its `Size`; its `Position` is ignored.",
        Properties = {
            {
                Name = "FillDirection",
                Type = "Enum.FillDirection",
                Default = "Enum.FillDirection.Vertical",
                Doc = "Whether the children are arranged in a row (`Horizontal`) or a column (`Vertical`).",
            },
            {
                Name = "Padding",
                Type = "UDim",
                Default = "UDim.new()",
                Doc = "The gap between children. It does not add space around them; use `UIPadding` for that. Its "
                    .. "scale is a fraction of the parent's size along the fill direction.",
            },
            {
                Name = "HorizontalAlignment",
                Type = "Enum.HorizontalAlignment",
                Default = "Enum.HorizontalAlignment.Left",
                Doc = "The horizontal position of the children within the parent.",
            },
            {
                Name = "VerticalAlignment",
                Type = "Enum.VerticalAlignment",
                Default = "Enum.VerticalAlignment.Top",
                Doc = "The vertical position of the children within the parent.",
            },
            {
                Name = "SortOrder",
                Type = "Enum.SortOrder",
                Default = "Enum.SortOrder.LayoutOrder",
                Doc = "The order the children are arranged in.",
            },
            {
                Name = "Wraps",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the children continue on a new line when they run out of room, instead of overflowing.",
            },
            {
                Name = "HorizontalFlex",
                Type = "Enum.UIFlexAlignment",
                Default = "Enum.UIFlexAlignment.None",
                Doc = "What happens to leftover horizontal space: nothing, the children grow to fill it, or it is "
                    .. "distributed between them.",
            },
            {
                Name = "VerticalFlex",
                Type = "Enum.UIFlexAlignment",
                Default = "Enum.UIFlexAlignment.None",
                Doc = "What happens to leftover vertical space: nothing, the children grow to fill it, or it is "
                    .. "distributed between them.",
            },
            {
                Name = "ItemLineAlignment",
                Type = "Enum.ItemLineAlignment",
                Default = "Enum.ItemLineAlignment.Automatic",
                Doc = "The position of each child across its line. `Stretch` makes every child as tall as its row "
                    .. "or as wide as its column. A child's own `UIFlexItem` can override it.",
            },
            {
                Name = "AbsoluteContentSize",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                -- What the interface found, not what the world is.
                HostFact = true,
                Doc = "The space the children take after layout, in pixels. Use it to set the `CanvasSize` of a "
                    .. "`ScrollFrame`.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIGridLayout",
        Extends = "Instance",
        Module = "ui",
        Parents = InAUi,
        Storage = "UIGridLayout",
        Records = { "ADR 0128" },
        Doc = "Arranges the `UIObject` children of its parent in a grid of equal cells, for an inventory, a shop "
            .. "or a level select. Parent it next to the objects it arranges.\n\n"
            .. "The `Position` and `Size` of a child are ignored: every cell is `CellSize`. Cells fill a line "
            .. "until the next one would not fit, or until `FillDirectionMaxCells` is reached, and then start a "
            .. "new line.",
        Properties = {
            {
                Name = "CellSize",
                Type = "UDim2",
                Default = "UDim2.new(0, 100, 0, 100)",
                Doc = "The size of every cell. Its scale is a fraction of the parent, so `UDim2.fromScale(0.25, "
                    .. "0.25)` gives four cells across at any window size.",
            },
            {
                Name = "CellPadding",
                Type = "UDim2",
                Default = "UDim2.new(0, 5, 0, 5)",
                Doc = "The horizontal and vertical gap between cells. It does not add space around the grid; use "
                    .. "`UIPadding` for that.",
            },
            {
                Name = "FillDirection",
                Type = "Enum.FillDirection",
                Default = "Enum.FillDirection.Horizontal",
                Doc = "Whether cells fill rows (`Horizontal`) or columns (`Vertical`).",
            },
            {
                Name = "FillDirectionMaxCells",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.whole_number_at_least_zero",
                Doc = "The largest number of cells in one line. 0 means as many as fit. A line never holds more "
                    .. "cells than fit.",
            },
            {
                Name = "StartCorner",
                Type = "Enum.StartCorner",
                Default = "Enum.StartCorner.TopLeft",
                Doc = "The corner the first cell is placed in, which also sets the direction the lines run in.",
            },
            {
                Name = "SortOrder",
                Type = "Enum.SortOrder",
                Default = "Enum.SortOrder.LayoutOrder",
                Doc = "The order the children are arranged in.",
            },
            {
                Name = "HorizontalAlignment",
                Type = "Enum.HorizontalAlignment",
                Default = "Enum.HorizontalAlignment.Left",
                Doc = "The horizontal position of the whole grid within the parent.",
            },
            {
                Name = "VerticalAlignment",
                Type = "Enum.VerticalAlignment",
                Default = "Enum.VerticalAlignment.Top",
                Doc = "The vertical position of the whole grid within the parent.",
            },
            {
                Name = "AbsoluteContentSize",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                -- What the interface found, not what the world is.
                HostFact = true,
                Doc = "The space the cells take after layout, in pixels. Use it to set the `CanvasSize` of a "
                    .. "`ScrollFrame`.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIPageLayout",
        Extends = "Instance",
        Module = "ui",
        Parents = InAUi,
        Storage = "UIPageLayout",
        Records = { "ADR 0128" },
        Doc = "Arranges the `UIObject` children of its parent side by side as pages and shows one at a time, for "
            .. "a tutorial, a set of tabs or a carousel. The other pages lie beside the shown one, outside the "
            .. "parent's rectangle; turn `ClipsDescendants` on to hide them.\n\n"
            .. "A page keeps its `Size`; its `Position` is ignored. Change page with `Next`, `Previous`, `JumpTo` "
            .. "and `JumpToIndex`. A swipe, the mouse wheel and a gamepad's shoulder buttons also change page "
            .. "unless they are turned off. `CurrentPage` changes at once, and the pages slide for `TweenTime` "
            .. "seconds when `Animated` is on.",
        Properties = {
            {
                Name = "Animated",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the pages slide when the page changes. When it is off, the new page appears at once.",
            },
            {
                Name = "Circular",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the pages wrap around: the page after the last is the first, and the page before "
                    .. "the first is the last.",
            },
            {
                Name = "EasingStyle",
                Type = "Enum.EasingStyle",
                Default = "Enum.EasingStyle.Quad",
                Doc = "The easing curve of the slide.",
            },
            {
                Name = "EasingDirection",
                Type = "Enum.EasingDirection",
                Default = "Enum.EasingDirection.Out",
                Doc = "Which end of the slide the easing applies to.",
            },
            {
                Name = "TweenTime",
                Type = "number",
                Default = "0.3",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The duration of the slide, in seconds.",
            },
            {
                Name = "Padding",
                Type = "UDim",
                Default = "UDim.new()",
                Doc = "The gap between pages. Its scale is a fraction of the parent's size along the fill direction.",
            },
            {
                Name = "FillDirection",
                Type = "Enum.FillDirection",
                Default = "Enum.FillDirection.Horizontal",
                Doc = "Whether the pages are arranged, and slide, horizontally or vertically.",
            },
            {
                Name = "SortOrder",
                Type = "Enum.SortOrder",
                Default = "Enum.SortOrder.LayoutOrder",
                Doc = "The order the pages are arranged in.",
            },
            {
                Name = "HorizontalAlignment",
                Type = "Enum.HorizontalAlignment",
                Default = "Enum.HorizontalAlignment.Center",
                Doc = "The horizontal position of the shown page within the parent.",
            },
            {
                Name = "VerticalAlignment",
                Type = "Enum.VerticalAlignment",
                Default = "Enum.VerticalAlignment.Center",
                Doc = "The vertical position of the shown page within the parent.",
            },
            {
                Name = "ScrollWheelInputEnabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the mouse wheel over the pages changes page.",
            },
            {
                Name = "TouchInputEnabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether a swipe across the pages changes page, with a finger or with a mouse drag.",
            },
            {
                Name = "GamepadInputEnabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether a gamepad's shoulder buttons change page.",
            },
            {
                Name = "CurrentPage",
                Type = "UIObject?",
                ReadOnly = true,
                Transient = true,
                -- What the interface found, not what the world is.
                HostFact = true,
                Doc = "The page that is shown, or the page being slid to. It is nil when there are no pages.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Next",
                Parameters = {},
                Returns = {},
                Doc = "Changes to the page after `CurrentPage`. Does nothing at the last page unless `Circular` is on.",
            },
            {
                Name = "Previous",
                Parameters = {},
                Returns = {},
                Doc = "Changes to the page before `CurrentPage`. Does nothing at the first page unless `Circular` "
                    .. "is on.",
            },
            {
                Name = "JumpTo",
                Parameters = { { Name = "page", Type = "UIObject" } },
                Returns = {},
                Doc = "Changes to `page`. Raises an error if the object is not one of the pages.",
            },
            {
                Name = "JumpToIndex",
                Parameters = { { Name = "index", Type = "number" } },
                Returns = {},
                Doc = "Changes to the page at `index`, counted from 0 in the layout's order. An index beyond "
                    .. "either end stops at that end, or wraps around when `Circular` is on.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "PageEnter",
                Parameters = { { Name = "page", Type = "UIObject" } },
                Doc = "Fires when `page` becomes `CurrentPage`, as the change to it starts.",
            },
            {
                Name = "PageLeave",
                Parameters = { { Name = "page", Type = "UIObject" } },
                Doc = "Fires when `page` stops being `CurrentPage`.",
            },
            {
                Name = "Stopped",
                Parameters = { { Name = "page", Type = "UIObject" } },
                Doc = "Fires when the slide ends with `page` shown. When `Animated` is off, it fires at once.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "UIFlexItem",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UIFlexItem",
        Records = { "ADR 0128" },
        Doc = "Sets how its parent behaves as a child of a `UIListLayout`. It controls whether the parent grows "
            .. "into leftover space, whether it shrinks when the line is too long, and where it sits across the "
            .. "line. Parent it to the object it affects.",
        Properties = {
            {
                Name = "FlexMode",
                Type = "Enum.UIFlexMode",
                Default = "Enum.UIFlexMode.None",
                Doc = "Whether the object grows, shrinks, or both.",
            },
            {
                Name = "GrowRatio",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "When `FlexMode` is `Custom`: the object's share of the leftover space, relative to the "
                    .. "other children that grow. An object at 2 next to one at 1 takes two thirds.",
            },
            {
                Name = "ShrinkRatio",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "When `FlexMode` is `Custom`: the object's share of the space the line is short by, "
                    .. "relative to the other children that shrink.",
            },
            {
                Name = "ItemLineAlignment",
                Type = "Enum.ItemLineAlignment",
                Default = "Enum.ItemLineAlignment.Automatic",
                Doc = "The position of the object across its line. `Automatic` uses the layout's `ItemLineAlignment`.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIScale",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UIScale",
        Records = { "ADR 0128" },
        Doc = "Scales its parent and everything inside it, both what is drawn and where a press lands. Use it for "
            .. "a window that pops open or a HUD the player can enlarge.\n\n"
            .. "The parent takes up `Scale` times its size, and a layout it is in makes room for that. Everything "
            .. "inside it is laid out as if unscaled and then scaled with it, including offsets, text, corners "
            .. "and strokes. It can be tweened.",
        Properties = {
            {
                Name = "Scale",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The factor the parent and its descendants are multiplied by.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIAspectRatioConstraint",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UIAspectRatioConstraint",
        Records = { "ADR 0128" },
        Doc = "Keeps its parent at a fixed aspect ratio, such as a square slot or a 16:9 picture, whatever the "
            .. "size of the window. It is applied after the parent's own `Size` and before its children are laid "
            .. "out.",
        Properties = {
            {
                Name = "AspectRatio",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_positive",
                Doc = "The width divided by the height. 1 is a square, and 2 is twice as wide as it is tall.",
            },
            {
                Name = "AspectType",
                Type = "Enum.AspectType",
                Default = "Enum.AspectType.FitWithinMaxSize",
                Doc = "Whether the ratio is fitted within the object's own size or scaled from its parent's size.",
            },
            {
                Name = "DominantAxis",
                Type = "Enum.DominantAxis",
                Default = "Enum.DominantAxis.Width",
                Doc = "With `ScaleWithParentSize`: the axis that takes the parent's full extent.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UISizeConstraint",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UISizeConstraint",
        Records = { "ADR 0128" },
        Doc = "Keeps the size of its parent between a minimum and a maximum, such as a panel that is a third of "
            .. "the window but never less than 240 units wide. It is applied after the parent's own `Size` and "
            .. "before its children are laid out.",
        Properties = {
            {
                Name = "MinSize",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "The smallest size the object can have, in the object's units.",
            },
            {
                Name = "MaxSize",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "The largest size the object can have. 0 on an axis, the default, means no limit. "
                    .. "`math.huge` also means no limit and reads back as 0.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UITextSizeConstraint",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UITextSizeConstraint",
        Records = { "ADR 0128" },
        Doc = "Keeps the text size of a label with `TextScaled` on between a minimum and a maximum, so the text "
            .. "is never too small to read or too large. It has no effect on a label whose text is not scaled.",
        Properties = {
            {
                Name = "MinTextSize",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The smallest size the text can have, in the object's units.",
            },
            {
                Name = "MaxTextSize",
                Type = "number",
                Default = "100",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The largest size the text can have, in the object's units.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIDragDetector",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UIDragDetector",
        Records = { "ADR 0128" },
        Doc = "Lets the player drag its parent with the pointer: a window by its title bar, the thumb of a slider "
            .. "or a dial. A press on the parent or anything inside it, followed by a movement, drags it, with a "
            .. "mouse or a finger. A press with no movement is still an ordinary press.\n\n"
            .. "`DragStyle` sets how the object moves, and `ResponseStyle` sets what is changed. The detector "
            .. "sets the parent's `Position` or `Rotation`, so it cannot move a parent that a layout arranges.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether a press can start a drag.",
            },
            {
                Name = "DragStyle",
                Type = "Enum.UIDragDetectorDragStyle",
                Default = "Enum.UIDragDetectorDragStyle.TranslatePlane",
                Doc = "How the pointer's movement is turned into the object's movement.",
            },
            {
                Name = "ResponseStyle",
                Type = "Enum.UIDragDetectorResponseStyle",
                Default = "Enum.UIDragDetectorResponseStyle.Offset",
                Doc = "What the detector changes as the object is dragged.",
            },
            {
                Name = "DragAxis",
                Type = "Vector2",
                Default = "Vector2.new(1, 0)",
                Doc = "The direction a `TranslateLine` drag moves along, with Y pointing down. `(1, 0)` is "
                    .. "horizontal and `(0, 1)` is vertical.",
            },
            {
                Name = "BoundingUI",
                Type = "UIObject?",
                Doc = "An object the dragged object must stay inside. With nil there is no bound.",
            },
            {
                Name = "MinDragTranslation",
                Type = "UDim2",
                Default = "UDim2.new()",
                Doc = "The lower limit on how far the object can be moved from where the drag began. It is not "
                    .. "applied while it equals `MaxDragTranslation`, as it does by default.",
            },
            {
                Name = "MaxDragTranslation",
                Type = "UDim2",
                Default = "UDim2.new()",
                Doc = "The upper limit on how far the object can be moved from where the drag began. It is not "
                    .. "applied while it equals `MinDragTranslation`.",
            },
            {
                Name = "MinDragAngle",
                Type = "number",
                Default = "0",
                Doc = "The lower limit on how far a `Rotate` drag can turn the object, in degrees. It is not "
                    .. "applied while it equals `MaxDragAngle`.",
            },
            {
                Name = "MaxDragAngle",
                Type = "number",
                Default = "0",
                Doc = "The upper limit on how far a `Rotate` drag can turn the object, in degrees. It is not "
                    .. "applied while it equals `MinDragAngle`.",
            },
            {
                Name = "DragUDim2",
                Type = "UDim2",
                ReadOnly = true,
                Transient = true,
                -- What the interface found, not what the world is.
                HostFact = true,
                Doc = "How far the current drag has moved the object from where it began, in the terms "
                    .. "`ResponseStyle` uses.",
            },
            {
                Name = "DragRotation",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                -- What the interface found, not what the world is.
                HostFact = true,
                Doc = "How far the current `Rotate` drag has turned the object from where it began, in degrees.",
            },
        } :: { Schema.Property },
        Events = {
            {
                Name = "DragStart",
                Parameters = { { Name = "position", Type = "Vector2" } },
                Doc = "Fires when a press on the object starts to move it. `position` is the pointer's position, "
                    .. "in pixels.",
            },
            {
                Name = "DragContinue",
                Parameters = { { Name = "position", Type = "Vector2" } },
                Doc = "Fires when the pointer moves during a drag.",
            },
            {
                Name = "DragEnd",
                Parameters = { { Name = "position", Type = "Vector2" } },
                Doc = "Fires when the drag ends: the press is released, or the object or the detector is removed.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "CanvasGroup",
        Extends = "Frame",
        Module = "ui",
        Storage = "CanvasGroup",
        Records = { "ADR 0128" },
        Doc = "A `Frame` whose descendants are drawn together into one image, which is then shown as a whole. Use "
            .. "it to fade a window as one piece, instead of having its parts show through each other. "
            .. "`GroupTransparency` and `GroupColor` apply to the whole image.\n\n"
            .. "The image is drawn again only when something inside the group changes. A group always clips its "
            .. "descendants to its rectangle.",
        Properties = {
            {
                Name = "GroupTransparency",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How transparent the whole group is, from 0 (opaque) to 1 (invisible).",
            },
            {
                Name = "GroupColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "A color the whole image is multiplied by. White changes nothing.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIPadding",
        Extends = "Instance",
        Module = "ui",
        Parents = InAUi,
        Storage = "UIPadding",
        Doc = "Adds space inside each edge of its parent, around the parent's content. Parent it next to the "
            .. "content it affects.\n\n"
            .. "The content is the parent's children and the parent's own text. The text of a `TextLabel` or "
            .. "`TextButton`, and the text, caret and placeholder of a `TextInput`, are positioned, wrapped, cut "
            .. "and scaled inside the padding.",
        Properties = {
            {
                Name = "PaddingTop",
                Type = "UDim",
                Default = "UDim.new()",
                Doc = "The space inside the top edge. Its scale is a fraction of the parent's height.",
            },
            {
                Name = "PaddingBottom",
                Type = "UDim",
                Default = "UDim.new()",
                Doc = "The space inside the bottom edge. Its scale is a fraction of the parent's height.",
            },
            {
                Name = "PaddingLeft",
                Type = "UDim",
                Default = "UDim.new()",
                Doc = "The space inside the left edge. Its scale is a fraction of the parent's width.",
            },
            {
                Name = "PaddingRight",
                Type = "UDim",
                Default = "UDim.new()",
                Doc = "The space inside the right edge. Its scale is a fraction of the parent's width.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UICorner",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UICorner",
        Doc = "Rounds the corners of its parent. It changes only how the parent is drawn: layout and hit testing "
            .. "still use the full rectangle.",
        Properties = {
            {
                Name = "CornerRadius",
                Type = "UDim",
                Default = "UDim.new(0, 8)",
                Doc = "The radius of the corners. Its scale is a fraction of the parent's shorter side, so "
                    .. "`UDim.new(0.5, 0)` gives a pill shape at any size.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIGradient",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject", "UIStroke"),
        Storage = "UIGradient",
        Records = { "ADR 0110" },
        Doc = "Applies a gradient of color and transparency to its parent. Everything the parent draws, its "
            .. "background, image and text, is multiplied by `Color` and faded by `Transparency` along the "
            .. "gradient. Under a `UIStroke`, it colors the stroke instead.\n\n"
            .. "It changes only how the parent is drawn, not its layout or hit testing. Only the first gradient "
            .. "under a parent is used.",
        Properties = {
            {
                Name = "Color",
                Type = "ColorSequence",
                Default = "ColorSequence.new(Color3.new(1, 1, 1))",
                Doc = "The colors along the gradient, multiplied by the parent's own colors. White changes nothing.",
            },
            {
                Name = "Transparency",
                Type = "NumberSequence",
                Default = "NumberSequence.new(0)",
                Doc = "The transparency along the gradient. 0 leaves the parent as opaque as it was, and 1 hides it.",
            },
            {
                Name = "Offset",
                Type = "Vector2",
                Default = "Vector2.new(0, 0)",
                Doc = "An offset of the gradient's center, as a fraction of the parent's size: (1, 0) moves it "
                    .. "one full width to the right. Negative values are allowed.",
            },
            {
                Name = "Rotation",
                Type = "number",
                Default = "0",
                Doc = "The rotation of the gradient, in degrees clockwise. At 0, a linear gradient runs from left "
                    .. "to right and a conical one starts pointing right. A radial gradient ignores it.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the gradient is applied.",
            },
            {
                Name = "Type",
                Type = "Enum.GradientType",
                Default = "Enum.GradientType.Linear",
                Doc = "The shape of the gradient: a line across the parent, a circle outward from its center, or "
                    .. "a sweep around its center.",
            },
            {
                Name = "TileMode",
                Type = "Enum.GradientTileMode",
                Default = "Enum.GradientTileMode.Clamp",
                Doc = "What fills the area beyond the ends of the gradient: the end colors, the gradient "
                    .. "repeated, or the gradient mirrored. It applies when `Scale` is below 1 or `Offset` moves "
                    .. "the gradient.",
            },
            {
                Name = "Scale",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_positive",
                Doc = "How much of the parent one run of the gradient covers. At 1 it runs once across; at 0.25 "
                    .. "it covers a quarter, and `TileMode` fills the rest. For a radial gradient it scales the "
                    .. "radius, and for a conical one the sweep, up to one full turn.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIStroke",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UIStroke",
        Records = { "ADR 0110" },
        Doc = "Draws an outline on the text or the border of its parent. On a text object it outlines the text, "
            .. "unless `ApplyStrokeMode` is `Border`. On any other object it outlines the border. A `UIGradient` "
            .. "under the stroke colors it.\n\n"
            .. "An object can have several border strokes, ordered by `ZIndex`, and a text object can have one "
            .. "text stroke. A stroke does not change the layout, so a stroke drawn outside an object can overlap "
            .. "its neighbors.",
        Properties = {
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(0, 0, 0)",
                Doc = "The color of the stroke.",
            },
            {
                Name = "Thickness",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The thickness of the stroke, in pixels. With `StrokeSizingMode.ScaledSize`, it is a "
                    .. "fraction of the parent's shorter side, or of the font size on text. Animating the "
                    .. "thickness of a text stroke is expensive, because each size needs a new outline.",
            },
            {
                Name = "Transparency",
                Type = "number",
                Default = "0",
                Doc = "How transparent the stroke is, from 0 (opaque) to 1 (invisible). It is separate from the "
                    .. "parent's transparency: a stroke on text whose `TextTransparency` is 1 gives hollow "
                    .. "lettering.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the stroke is drawn.",
            },
            {
                Name = "ApplyStrokeMode",
                Type = "Enum.ApplyStrokeMode",
                Default = "Enum.ApplyStrokeMode.Contextual",
                Doc = "What the stroke outlines. `Contextual` outlines the text of a text object and the border "
                    .. "of any other object. `Border` always outlines the border.",
            },
            {
                Name = "LineJoinMode",
                Type = "Enum.LineJoinMode",
                Default = "Enum.LineJoinMode.Round",
                Doc = "How the stroke turns a corner: rounded, beveled or mitered. If the parent has a "
                    .. "`UICorner`, a border stroke is always rounded.",
            },
            {
                Name = "StrokeSizingMode",
                Type = "Enum.StrokeSizingMode",
                Default = "Enum.StrokeSizingMode.FixedSize",
                Doc = "Whether `Thickness` is in pixels or a fraction of the parent.",
            },
            {
                Name = "BorderStrokePosition",
                Type = "Enum.BorderStrokePosition",
                Default = "Enum.BorderStrokePosition.Outer",
                Doc = "Where a border stroke lies relative to the border: entirely outside it, centered on it, or "
                    .. "entirely inside it.",
            },
            {
                Name = "BorderOffset",
                Type = "UDim",
                Default = "UDim.new(0, 0)",
                Doc = "A distance by which a border stroke is moved outward, or inward when negative. Its scale "
                    .. "is a fraction of the parent's shorter side. Text strokes ignore it.",
            },
            {
                Name = "ZIndex",
                Type = "number",
                Default = "1",
                Doc = "The drawing order among the strokes of one object: lower values are drawn behind higher "
                    .. "ones, and equal values are drawn in child order.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "AudioGroup",
        Extends = "Instance",
        Module = "audio",
        Storage = "AudioGroup",
        Doc = "A mixing bus. The volume of every `Sound` whose `Group` is this group is multiplied by the group's "
            .. "`Volume`, so one property turns down all the music or all the effects.",
        Properties = {
            {
                Name = "Volume",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "A multiplier on the volume of the group's sounds. 0 is silent, 1 is unchanged, and a value "
                    .. "above 1 is louder and can clip. Groups do not nest: a group parented to another group is "
                    .. "not affected by it.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "SoundEffect",
        Extends = "Instance",
        Module = "audio",
        Tags = { "Abstract", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0131" },
        Doc = "The base class of audio effects, which process a sound before it reaches the speakers. Parented to "
            .. "a `Sound`, an effect processes that sound. Parented to an `AudioGroup`, it processes everything "
            .. "in the group, mixed together. Several effects under one parent are applied in order of "
            .. "`Priority`.\n\n"
            .. "An effect changes only what is heard: nothing a script reads changes, and a sound lasts as long "
            .. "with an effect as without. An effect on a `Sound` stops when the sound stops. An effect on an "
            .. "`AudioGroup` keeps ringing after its sounds stop, which is what a reverb needs.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether the effect is applied.",
            },
            {
                Name = "Priority",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "The order of the effect among the effects under one parent: lower values are applied "
                    .. "first, and equal values are applied in the order the effects were created.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "ReverbSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "ReverbSoundEffect",
        Doc = "Adds the reverberation of a room, such as a hall, a cave or a bathroom.",
        Properties = {
            {
                Name = "RoomSize",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The size of the room, from 0 to 1. A larger room reverberates longer.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How quickly high frequencies fade in the room, from 0 to 1. Use a low value for a stone "
                    .. "hall and a high one for a carpeted room.",
            },
            {
                Name = "WetLevel",
                Type = "number",
                Default = "0.3",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The level of the reverberation, from 0 to 1.",
            },
            {
                Name = "DryLevel",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The level of the original sound, from 0 to 1.",
            },
            {
                Name = "Width",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How widely the reverberation is spread between the left and right channels.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "EchoSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "EchoSoundEffect",
        Doc = "Repeats the sound after a delay, more quietly each time, like a canyon or a tunnel.",
        Properties = {
            {
                Name = "Delay",
                Type = "number",
                Default = "0.3",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The time before each echo, in seconds, from 0.01 to 2.",
            },
            {
                Name = "Feedback",
                Type = "number",
                Default = "0.4",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How much of each echo is echoed again, from 0 to 0.95. At 0 there is a single echo; higher "
                    .. "values give a longer series.",
            },
            {
                Name = "WetLevel",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The level of the echoes, from 0 to 1.",
            },
            {
                Name = "DryLevel",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The level of the original sound, from 0 to 1.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "EqualizerSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "EqualizerSoundEffect",
        Doc = "Raises or lowers three frequency bands: low, middle and high. Cutting the lows and the highs "
            .. "sounds like a radio; keeping only the lows sounds like a voice through a wall.",
        Properties = {
            {
                Name = "LowGain",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The gain of the low band, in decibels, from -80 to 12.",
            },
            {
                Name = "MidGain",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The gain of the middle band, in decibels, from -80 to 12.",
            },
            {
                Name = "HighGain",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The gain of the high band, in decibels, from -80 to 12.",
            },
            {
                Name = "MidLow",
                Type = "number",
                Default = "400",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The frequency where the middle band begins, in hertz. The low band is below it.",
            },
            {
                Name = "MidHigh",
                Type = "number",
                Default = "4000",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The frequency where the middle band ends, in hertz. The high band is above it.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "LowPassSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "LowPassSoundEffect",
        Doc = "Removes high frequencies and keeps low ones, for a sound heard under water, behind a door or while "
            .. "the game is paused.",
        Properties = {
            {
                Name = "Cutoff",
                Type = "number",
                Default = "2000",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The frequency above which sound is removed, in hertz.",
            },
            {
                Name = "Resonance",
                Type = "number",
                Default = "0.707",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How much frequencies near the cutoff are boosted, from 0.1 to 10. 0.707 is flat; higher "
                    .. "values ring.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "HighPassSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "HighPassSoundEffect",
        Doc = "Removes low frequencies and keeps high ones, for a telephone or a small speaker.",
        Properties = {
            {
                Name = "Cutoff",
                Type = "number",
                Default = "500",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The frequency below which sound is removed, in hertz.",
            },
            {
                Name = "Resonance",
                Type = "number",
                Default = "0.707",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How much frequencies near the cutoff are boosted, from 0.1 to 10. 0.707 is flat; higher "
                    .. "values ring.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "DistortionSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "DistortionSoundEffect",
        Doc = "Overdrives the sound until it breaks up, for a megaphone, a blown speaker or an electric guitar.",
        Properties = {
            {
                Name = "Level",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The amount of distortion, from 0 (none) to 1.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "CompressorSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "CompressorSoundEffect",
        Doc = "Reduces the loud parts of a sound so that the whole can be made louder. Use it to keep speech "
            .. "audible over music, or to stop an explosion from drowning out everything else.",
        Properties = {
            {
                Name = "Threshold",
                Type = "number",
                Default = "-20",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The level above which the sound is reduced, in decibels, from -60 to 0.",
            },
            {
                Name = "Ratio",
                Type = "number",
                Default = "4",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How much the sound above the threshold is reduced. At 4, a sound four decibels over the "
                    .. "threshold comes out one decibel over.",
            },
            {
                Name = "Attack",
                Type = "number",
                Default = "0.01",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How quickly the compressor reacts to a loud part, in seconds.",
            },
            {
                Name = "Release",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How quickly the compressor stops reducing after a loud part, in seconds.",
            },
            {
                Name = "MakeupGain",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The gain added to the whole sound after compression, in decibels, from 0 to 24.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "ChorusSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "ChorusSoundEffect",
        Doc = "Adds a copy of the sound that drifts slightly out of time and tune with it, which makes one voice "
            .. "sound like several.",
        Properties = {
            {
                Name = "Rate",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The speed at which the copy drifts back and forth, in hertz.",
            },
            {
                Name = "Depth",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How far the copy drifts, from 0 to 1.",
            },
            {
                Name = "Mix",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How much of the output is the copy, from 0 to 1.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "PitchShiftSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "PitchShiftSoundEffect",
        Doc = "Raises or lowers the pitch of a sound without changing its length, which `PlaybackSpeed` cannot "
            .. "do. Use it for a tiny creature, a giant or a disguised voice.",
        Properties = {
            {
                Name = "Octave",
                Type = "number",
                Default = "1.25",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The factor the pitch is multiplied by, from 0.5 (an octave down) to 2 (an octave up). 1 "
                    .. "changes nothing.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Sound",
        Extends = "Instance",
        Module = "audio",
        Storage = "Sound",
        Doc = "Plays an audio file. Parented to a `BasePart`, the sound is positional: it gets quieter with "
            .. "distance and is heard from the side it is on. Parented to anything else, it plays at the same "
            .. "volume everywhere.\n\n"
            .. "A positional sound is mixed to mono and panned left or right by its direction from the listener. "
            .. "Height is not conveyed, and a source directly ahead sounds the same as one directly behind. A "
            .. "sound that is not positional keeps its own stereo image.\n\n"
            .. "A sound runs on simulation time. `TimePosition` advances by the tick length times `PlaybackSpeed` "
            .. "every tick, and `Ended` fires from that position. A replay reproduces both exactly, and a run "
            .. "with no audio device fires `Ended` on the same tick as a run with speakers. The length of the "
            .. "sound comes from its file.",
        Properties = {
            {
                Name = "Content",
                Type = "Content",
                ContentKind = "Audio",
                Default = '""',
                Doc = "The audio file, as an `asset://` path. WAV, MP3, FLAC and Ogg Vorbis are supported. A file "
                    .. "of up to ten seconds is decoded once and kept in memory. A longer file is streamed, so a "
                    .. "long music track uses about as much memory as the size of its file.\n\n"
                    .. "The length of the sound is the length of the file, and `Ended` fires when `TimePosition` "
                    .. "reaches it.\n\n"
                    .. "A path to a file that does not exist plays a one-second placeholder tone instead of "
                    .. 'silence, so that a missing file is noticed. `DebugService:GetStat("AudioClipsMissing")` '
                    .. "returns how many sounds are missing their file.",
            },
            {
                Name = "Playing",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the sound is advancing. Setting it resumes or pauses the sound at `TimePosition`; "
                    .. "it never rewinds. Use `Play` to start from the beginning and `Stop` to stop and rewind.",
            },
            {
                Name = "Looped",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the sound starts again when it reaches the end. A looped sound never fires `Ended`.",
            },
            {
                Name = "Volume",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "A multiplier on the loudness of the sound, combined with its group's volume and with the "
                    .. "falloff over distance. The default is 0.5, which leaves headroom when several sounds play "
                    .. "at once.",
            },
            {
                Name = "PlaybackSpeed",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "How fast the sound plays, which also changes its pitch. 2 is an octave higher and half as "
                    .. "long. It must be greater than 0; use `Playing` to pause.",
            },
            {
                Name = "TimePosition",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Transient = true,
                Doc = "The current position in the sound, in seconds, from 0 to `TimeLength`. Set it to seek. A "
                    .. "value below 0 raises an error. A value beyond the end is clamped to the end, or wrapped "
                    .. "if the sound is `Looped`.",
            },
            {
                Name = "TimeLength",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                Records = { "ADR 0200" },
                Doc = "The length of `Content`, in seconds. It is the length of the file, or one second for the "
                    .. "placeholder tone of a missing file. It is 0 until the length has been read, which happens "
                    .. "on the first tick after `Content` is set.\n\n"
                    .. "A sound recorded in several languages is as long as its longest recording. The value is "
                    .. "the same on every machine whatever language each one plays, so `Ended` fires on the same "
                    .. "tick everywhere. A shorter recording is followed by silence. A sound with a `Caption` and "
                    .. "no file in any language lasts as long as its caption is shown.",
            },
            {
                Name = "RollOffMinDistance",
                Type = "number",
                Default = "8",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance from the listener within which a positional sound plays at its full `Volume`, "
                    .. "in meters. Beyond it the sound gets quieter. A sound that is not positional ignores it.",
            },
            {
                Name = "RollOffMaxDistance",
                Type = "number",
                Default = "80",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance from the listener at which a positional sound becomes silent, in meters. "
                    .. "Between `RollOffMinDistance` and this distance, the volume falls off linearly.",
            },
            {
                Name = "Group",
                Type = "AudioGroup?",
                Default = "nil",
                Doc = "The `AudioGroup` whose volume this sound's volume is multiplied by, or nil for none.",
            },
            {
                Name = "Category",
                Type = "Enum.SoundCategory",
                Default = "Enum.SoundCategory.Effects",
                Records = { "ADR 0200" },
                Doc = "The kind of sound: `Effects`, `Music` or `Voice`. It selects which of the player's volume "
                    .. "settings applies (`AudioService.EffectsVolume`, `MusicVolume` or `VoiceVolume`). Music is "
                    .. "automatically lowered while a voice plays. The category is replicated with the sound, "
                    .. "unlike `Group`, which is local to each machine.",
            },
            {
                Name = "Loudness",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                -- What one machine's copy of the recording holds at one moment.
                HostFact = true,
                Records = { "ADR 0200" },
                Doc = "How loud the recording is at the current moment on this machine, from 0 to 1. It is the "
                    .. "level of the recording over about the last twentieth of a second at `TimePosition`; a "
                    .. "full-scale tone reads 1. Use it to open a character's mouth while a line plays.\n\n"
                    .. "It is the level of the recording itself. No volume setting and no distance affects it. It "
                    .. "is measured on the language this machine plays, so it differs between machines; do not "
                    .. "use it in game rules. It is 0 while the sound is not playing, during the silence after a "
                    .. "shorter language, and for a sound with no file.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetBands",
                Parameters = {},
                Returns = { "number", "number", "number" },
                Records = { "ADR 0200" },
                Doc = "Returns the level that `Loudness` measures, split into three frequency bands, each from 0 "
                    .. "to 1. The bands are low (80 to 500 Hz), middle (500 to 2,500 Hz) and high (2,500 to "
                    .. "10,000 Hz). A full-scale tone inside one band reads 1 in that band and 0 in the others. "
                    .. "Use it to tell an open mouth from a wide or a round one, or to drive a visualizer.\n\n"
                    .. "The bands are computed only when requested, and once per tick however many times they are "
                    .. "requested. Like `Loudness`, the result differs between machines.",
            },
            {
                Name = "Play",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Records = { "ADR 0200" },
                Doc = "Plays the sound from the start, or from `TimePosition` if a script set it since the sound "
                    .. "last started. Calling it on a sound that is already playing restarts it. To continue "
                    .. "after `Pause`, use `Resume`.\n\n"
                    .. "The language of the sound is chosen when it starts. `Content` is looked for under "
                    .. "`l10n/<VoiceLocale>/`, then under the language without its region, then at the path as "
                    .. "written. A sound that is playing finishes in the language it started in.",
            },
            {
                Name = "Resume",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Continues the sound from `TimePosition`, where `Pause` left it. It is the same as setting "
                    .. "`Playing` to true.",
            },
            {
                Name = "Pause",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops the sound at its current position. `Resume` continues from there, and `Play` starts "
                    .. "again from the beginning. It is the same as setting `Playing` to false.",
            },
            {
                Name = "Stop",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops the sound and rewinds it to the start. `Ended` does not fire.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Ended",
                Parameters = {},
                Doc = "Fires on the tick a sound that is not `Looped` reaches its end. The sound stops and "
                    .. "`TimePosition` returns to 0. It fires on the same tick in a replay, in a headless run and "
                    .. "on every machine.",
            },
            {
                Name = "Loaded",
                Parameters = {},
                Doc = "Fires when the sound's `Content` is ready to play. It currently fires immediately.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "Caption",
        Extends = "Instance",
        Module = "audio",
        Parents = under("Sound"),
        Storage = "Caption",
        Records = { "ADR 0200" },
        Doc = 'The text of what a sound says: a line of dialogue, or a description such as "[a door creaks]". '
            .. "Parent it to its `Sound`. While that sound plays and can be heard on a machine, the caption is "
            .. "current there: `DialogueService:GetCaptions` lists it, and `CaptionStarted` and `CaptionEnded` "
            .. "fire for it. A sound on a part can be heard within `RollOffMaxDistance` of the listener; the "
            .. "player's volume settings are not considered.\n\n"
            .. "`Text` and `Speaker` are localization keys, not text. They are looked up in the game's catalogs "
            .. "with `LocalizationService:Translate`, so a caption is shown in each player's own language, which "
            .. "can differ from the language that player hears.\n\n"
            .. "A sound that has a caption but no file in any language is silent and lasts as long as its "
            .. "caption: `Seconds`, or a reading time for the text. Dialogue can therefore be written and played "
            .. "as text before it is recorded. In a multiplayer match, a caption replicates with its sound.",
        Properties = {
            {
                Name = "Text",
                Type = "string",
                Default = '""',
                Doc = "The localization key of the text that is said. If no catalog has the key, the key itself "
                    .. "is shown.",
            },
            {
                Name = "Speaker",
                Type = "string",
                Default = '""',
                Doc = "The localization key of the speaker's name. Leave it empty for a sound with no speaker.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "The color the speaker's name is shown in.",
            },
            {
                Name = "Seconds",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How long the sound lasts when it has no file in any language, in seconds. At 0 a reading "
                    .. "time is used: 1.5 seconds plus 0.06 seconds for each character of the text in the "
                    .. "project's default language, up to 12 seconds, the same on every machine. It is ignored "
                    .. "for a sound that has a file.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "AnimationPlayer",
        Extends = "Instance",
        Module = "render",
        Parents = under("Model"),
        Storage = "AnimationPlayer",
        Records = { "ADR 0197" },
        Doc = "Plays animation clips on skinned meshes.\n\n"
            .. "Parent it to the `Model` of a character, and it animates every skinned `MeshPart` under that "
            .. "model. A character is often several meshes that share a skeleton, such as a body, a shirt and "
            .. "trousers, and one clip moves all of them. Only one of the meshes needs to contain the animation; "
            .. "the clip is taken from the first one that has it, in tree order. Parented directly to a "
            .. "`MeshPart`, it animates only that mesh.\n\n"
            .. "Joints are matched between the meshes by name, not by index. A joint that a mesh does not have is "
            .. "skipped.\n\n"
            .. "Clips are sampled on simulation time, so the position of a clip at a given tick is the same in a "
            .. "replay as it was live. Clips play in place: there is no root motion, and the character's body "
            .. "moves the character.\n\n"
            .. "With a `Graph`, the player blends clips by itself. An animation graph defines which clips play in "
            .. "which state, how a walk blends into a run, when a jump starts and how long each fade lasts; the "
            .. "game only supplies values such as the character's speed. Tracks loaded from a script keep working "
            .. "alongside the graph. For reaching limbs and feet on uneven ground, add `IKControl` and "
            .. "`FootPlacement`.\n\n"
            .. "In a multiplayer match, the player replicates with its character, including `Graph` and "
            .. "`Retargeting`. Nothing is sent per tick: each machine runs its own copy of the graph from values "
            .. "that already replicate, such as the body's speed, whether it is on the ground, and its attributes.",
        Properties = {
            {
                Name = "Graph",
                Type = "Content",
                ContentKind = "AnimationGraph",
                Doc = "The animation graph the player runs, as an `asset://` path ending in `.animgraph.json`. "
                    .. "When it is empty, the player plays only the tracks a script loads. The graph starts in "
                    .. "the first state of each layer once its file has loaded.",
            },
            {
                Name = "Retargeting",
                Type = "Enum.Retargeting",
                Default = "Enum.Retargeting.Automatic",
                Records = { "ADR 0199" },
                Doc = "How a clip from another model file is adapted to this skeleton.",
            },
            {
                Name = "CullingMode",
                Type = "Enum.AnimationCullingMode",
                Default = "Enum.AnimationCullingMode.Automatic",
                Doc = "How often the meshes are posed.\n\n"
                    .. "- `Automatic` poses them as often as they are visible: every tick up close, every second, "
                    .. "fourth or eighth tick as they get smaller on screen, and not at all where neither the "
                    .. "camera nor a shadow shows them. Clips keep advancing in every case, and a joint that a "
                    .. "script or a `Bone` reads is posed on demand. A world with no viewer, such as a server or "
                    .. "a replay, poses every tick.\n- `AlwaysAnimate` poses them every tick, visible or not.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "LoadAnimation",
                Parameters = { { Name = "content", Type = "Content" } },
                Returns = { "AnimationTrack" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a track for one clip. Load a track once and keep it.\n\n"
                    .. "A string with no `#` is the name of a clip in the player's own mesh, as in "
                    .. '`player:LoadAnimation("Walk")`. To play a clip from another file, write the file\'s path, '
                    .. "then `#`, then the clip's name. A clip from another file is retargeted to this skeleton "
                    .. "by joint name, and a joint this skeleton does not have is skipped. One walk cycle can "
                    .. "therefore be shared by every character.\n\n"
                    .. "The method always returns a track. If the clip does not exist, or its file has not "
                    .. "loaded, the track is empty and plays nothing.",
            },
            {
                Name = "SetParameter",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "value", Type = "number | boolean" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets one of the graph's parameters on this machine. A number or a boolean keeps its value "
                    .. "until it is set again. A trigger is fired by any value and stays true until a transition "
                    .. "consumes it or the tick ends.\n\n"
                    .. "A parameter that the graph reads from the world (`from` in its file) is overridden on "
                    .. "this machine until `ClearParameter` is called. Other machines do not see the value. For "
                    .. "something every machine must show, set an attribute that the graph reads, or let it "
                    .. "follow from the body's motion.\n\n"
                    .. "Raises an error for a name the graph does not declare, once the graph has loaded. A value "
                    .. "set before the graph loads is kept and applied when it does.",
            },
            {
                Name = "GetParameter",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "(number | boolean)?" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the value of a graph parameter on this machine for the current tick: the value a "
                    .. "script set, or else the value read from the world, or else its default. For a trigger, it "
                    .. "returns whether the trigger is fired. Returns nil if no graph is loaded or the graph does "
                    .. "not declare the name.",
            },
            {
                Name = "ClearParameter",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes the value that `SetParameter` set. The parameter goes back to the value the graph "
                    .. "reads from the world, or to its default.",
            },
            {
                Name = "GetState",
                Parameters = { { Name = "layer", Type = "string?", Default = "nil" } },
                Returns = { "string" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the name of the state a layer is in. During a fade, that is the state being faded "
                    .. "into. With no layer name, the first layer is used. Returns an empty string if no graph is "
                    .. "loaded or the graph has no such layer.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "StateChanged",
                Parameters = {
                    { Name = "layer", Type = "string" },
                    { Name = "from", Type = "string" },
                    { Name = "to", Type = "string" },
                },
                Doc = "Fires when a layer of the graph changes from one state to another on this machine.",
            },
            {
                Name = "EventReached",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "layer", Type = "string" },
                },
                Doc = "Fires when a clip passes a moment that the graph names under `events` in its file, such as "
                    .. "a footstep or the active frames of an attack.\n\n"
                    .. "It fires only for the state a layer is in, not for one fading out. In a blend, it fires "
                    .. "only for the clip with the greatest weight, so a walk blended with a run does not produce "
                    .. "two footsteps. A tick that a client predicts again does not fire it again. Each machine "
                    .. "fires its own; decide what a hit does on the authority.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "Part2D",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Workspace", "Model", "Part2D"),
        Storage = "Part2D",
        Doc = "A sprite with a physics body on the 2D plane; the 2D counterpart of a `Part`. It lies on the "
            .. "world's XY plane facing -Z, so an orthographic `Camera` looking along -Z sees it straight on. "
            .. "`ZIndex` orders sprites that overlap.\n\n"
            .. "It is drawn as its `Image`, or as a flat `Color` if it has none. 2D physics simulates it unless "
            .. "it is `Anchored`, in which case it does not move.",
        Properties = {
            { Name = "Position", Type = "Vector2", Doc = "The position of the part's center, in meters." },
            {
                Name = "Rotation",
                Type = "number",
                Doc = "The rotation of the part, in degrees counterclockwise. A part with `FixedRotation` on "
                    .. "keeps this value.",
            },
            {
                Name = "Size",
                Type = "Vector2",
                Default = "Vector2.new(1, 1)",
                ErrorKey = "scene.err.size2_positive",
                Doc = "The width and height of the part, in meters, for both the image and the collider.",
            },
            { Name = "Shape", Type = "Enum.Shape2D", Doc = "The outline the part is drawn with and collides as." },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "A color the image is multiplied by. With no image, the part is drawn in this color.",
            },
            {
                Name = "Transparency",
                Type = "number",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How transparent the part is, from 0 (as drawn) to 1 (invisible). A transparent part still "
                    .. "collides.",
            },
            {
                Name = "ZIndex",
                Type = "number",
                Doc = "Which of two overlapping sprites is drawn in front: the one with the higher value. Sprites "
                    .. "with equal values are drawn in tree order.",
            },
            {
                Name = "FlipX",
                Type = "boolean",
                Doc = "Whether the image is mirrored left to right, for a character that faces the other way.",
            },
            { Name = "FlipY", Type = "boolean", Doc = "Whether the image is mirrored top to bottom." },
            {
                Name = "Image",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image the part is drawn as, stretched over `Size`. Transparent pixels of the image "
                    .. "stay transparent.",
            },
            {
                Name = "ImageRectOffset",
                Type = "Vector2",
                Doc = "The top left corner of the part of the image to draw, in pixels. Use it to show one frame "
                    .. "of a sprite sheet.",
            },
            {
                Name = "ImageRectSize",
                Type = "Vector2",
                Doc = "The size of the part of the image to draw, in pixels. Zero on both axes draws the whole image.",
            },
            {
                Name = "Filter",
                Type = "Enum.TextureFilter",
                Doc = "How the image is sampled when it is scaled: `Linear` for painted art, `Nearest` for pixel art.",
            },
            {
                Name = "ExactColor",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the sprite is drawn in exactly the colors of its image, bypassing exposure, tone "
                    .. "mapping, color correction and bloom. On by default: a `Color` of `Color3.fromRGB(200, 80, "
                    .. "40)` appears as exactly that color whatever `Lighting` is set to.\n\n"
                    .. "When it is off, the sprite is lit and processed like any part, which suits a sprite "
                    .. "placed in a 3D scene. In both cases it is sorted by depth with the rest of the scene.",
            },
            {
                Name = "Anchored",
                Type = "boolean",
                Doc = "Whether the part is fixed in place. An anchored part is not moved by physics, and other "
                    .. "parts rest on it. A script can still move it.",
            },
            {
                Name = "CanCollide",
                Type = "boolean",
                Default = "true",
                Doc = "Whether other parts collide with this one. A part that does not collide is only drawn.",
            },
            {
                Name = "Sensor",
                Type = "boolean",
                Doc = "Whether the part detects overlaps instead of colliding. A sensor fires `Touched` for what "
                    .. "enters it and pushes nothing. Use it for a coin, a checkpoint or a trigger.",
            },
            {
                Name = "Density",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The density of the part, in kilograms per square meter. Together with `Size`, it sets the "
                    .. "part's mass.",
            },
            {
                Name = "Friction",
                Type = "number",
                Default = "0.3",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much the part grips a surface it slides on. 0 is like ice.",
            },
            {
                Name = "Elasticity",
                Type = "number",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "The fraction of its speed the part keeps when it bounces, from 0 (no bounce) to 1 (it "
                    .. "bounces forever).",
            },
            {
                Name = "FixedRotation",
                Type = "boolean",
                Doc = "Whether the part is prevented from rotating, whatever hits it. Use it for a character that "
                    .. "should stay upright.",
            },
            {
                Name = "GravityScale",
                Type = "number",
                Default = "1",
                Doc = "How much of `Workspace.Gravity` affects the part. At 0 it floats, at 2 it falls twice as "
                    .. "hard, and with a negative value it falls upward.",
            },
            {
                Name = "Velocity",
                Type = "Vector2",
                Doc = "The velocity of the part, in meters per second. The simulation updates it every tick, and "
                    .. "a script can set it: to jump, set its Y.",
            },
            {
                Name = "AngularVelocity",
                Type = "number",
                Doc = "How fast the part rotates, in degrees per second, counterclockwise.",
            },
            {
                Name = "CollisionGroup",
                Type = "string",
                Default = '"Default"',
                Doc = "The collision group that decides which other parts this one collides with, as defined in "
                    .. "`PhysicsService`.",
            },
        },
        Methods = {
            {
                Name = "ApplyImpulse",
                Parameters = { { Name = "impulse", Type = "Vector2" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Applies an instant push at the part's center, in kilogram meters per second. It takes "
                    .. "effect at the next simulation tick. An anchored part ignores it.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Touched",
                Parameters = { { Name = "otherPart", Type = "Instance" } },
                Doc = "Fires once when something starts touching the part: another `Part2D`, or the solid tiles "
                    .. "of a `Tilemap2D`. For a `Sensor`, it fires when something starts overlapping it.",
            },
            {
                Name = "TouchEnded",
                Parameters = { { Name = "otherPart", Type = "Instance" } },
                Doc = "Fires when something that was touching the part stops touching it.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "Tilemap2D",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Workspace", "Model"),
        Storage = "Tilemap2D",
        Doc = "A grid of tiles drawn from one tileset image, on the 2D plane. Use it to build a level by painting "
            .. "tiles. Tile 0 is empty, and tile `n` is the `n`-th tile of the tileset, counted along its rows "
            .. "from the top left.\n\n"
            .. "When `Collides` is on, every tile that is not empty is solid. The tiles collide as one body with "
            .. "merged edges, so a character runs across a floor of tiles without catching on the seams.",
        Properties = {
            {
                Name = "Position",
                Type = "Vector2",
                Doc = "The position of the bottom left corner of cell (0, 0), in meters.",
            },
            {
                Name = "CellSize",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The length of the side of one cell, in meters.",
            },
            {
                Name = "Tileset",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image the tiles are taken from.",
            },
            {
                Name = "TileSize",
                Type = "Vector2",
                Default = "Vector2.new(16, 16)",
                ErrorKey = "scene.err.size2_positive",
                Doc = "The size of one tile in the tileset, in pixels.",
            },
            {
                Name = "ZIndex",
                Type = "number",
                Doc = "The drawing order of the tilemap among overlapping sprites, like `Part2D.ZIndex`.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "A color every tile is multiplied by.",
            },
            {
                Name = "Filter",
                Type = "Enum.TextureFilter",
                Default = "Enum.TextureFilter.Nearest",
                Doc = "How the tiles are sampled when they are scaled. `Nearest`, the default, suits pixel art.",
            },
            {
                Name = "ExactColor",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the tiles are drawn in exactly the colors of the tileset, like "
                    .. "`Part2D.ExactColor`. When it is off, the tilemap is lit and processed with the rest of "
                    .. "the scene.",
            },
            {
                Name = "Collides",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the tiles are solid. Turn it off for a background layer.",
            },
            {
                Name = "Friction",
                Type = "number",
                Default = "0.3",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much the tiles grip what slides on them.",
            },
            {
                Name = "CollisionGroup",
                Type = "string",
                Default = '"Default"',
                Doc = "The collision group of the tiles.",
            },
        },
        Methods = {
            {
                Name = "SetCell",
                Parameters = {
                    { Name = "x", Type = "number" },
                    { Name = "y", Type = "number" },
                    { Name = "tile", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets the tile in cell (`x`, `y`) to `tile`; 0 empties the cell. Cell coordinates are "
                    .. "integers, and Y increases upward, as in the world.",
            },
            {
                Name = "GetCell",
                Parameters = { { Name = "x", Type = "number" }, { Name = "y", Type = "number" } },
                Returns = { "number" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns the tile in cell (`x`, `y`), or 0 if the cell is empty.",
            },
            {
                Name = "FillRect",
                Parameters = {
                    { Name = "x0", Type = "number" },
                    { Name = "y0", Type = "number" },
                    { Name = "x1", Type = "number" },
                    { Name = "y1", Type = "number" },
                    { Name = "tile", Type = "number" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets every cell in the rectangle from (`x0`, `y0`) to (`x1`, `y1`), both corners included, "
                    .. "to `tile`.",
            },
            {
                Name = "Clear",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets every cell to empty.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "Constraint2D",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Workspace", "Model", "Part2D"),
        Storage = "Constraint2D",
        Tags = { "Abstract", "NotCreatable" } :: { Schema.ClassTag },
        Records = { "ADR 0102" },
        Doc = "The base class of the joints between two `Part2D`s. Use joints to build a rope bridge, a hinged "
            .. "door or a swinging lamp.\n\n"
            .. "A 2D joint connects two parts at a point on each. Each point is given in meters from the center "
            .. "of its part, in the part's own frame. A joint that cannot hold does nothing and is not an error: "
            .. "one with a part missing or not in the world, or one between two anchored parts.",
        Properties = {
            {
                Name = "Part0",
                Type = "Part2D?",
                Doc = "The first of the two parts the joint connects.",
            },
            {
                Name = "Part1",
                Type = "Part2D?",
                Doc = "The second of the two parts the joint connects.",
            },
            {
                Name = "Anchor0",
                Type = "Vector2",
                Doc = "The position of the joint on `Part0`, in meters from the part's center, in the part's own "
                    .. "frame. It turns with the part.",
            },
            {
                Name = "Anchor1",
                Type = "Vector2",
                Doc = "The position of the joint on `Part1`, in meters from the part's center, in the part's own "
                    .. "frame. It turns with the part.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the joint holds. A disabled joint stays in the world and does nothing.",
            },
            {
                Name = "CollideConnected",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the two parts still collide with each other. Turn it off for the links of a chain "
                    .. "or a limb, where the parts overlap at the joint.",
            },
        },
    },
    {
        Name = "HingeConstraint2D",
        Extends = "Constraint2D",
        Module = "scene",
        -- A hook of its own that stamps the kind onto the one `Constraint2D`
        -- component, as the 3D constraints do: the type is the class.
        Storage = "HingeConstraint2D",
        Doc = "Joins two parts at a point that both can rotate about: a door, a wheel, a pendulum or a lever. "
            .. "With `Anchor0` and `Anchor1` at the same position in the world, the parts stay together there and "
            .. "rotate freely, unless limits or a motor are enabled.",
        Properties = {
            {
                Name = "LimitsEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `LowerAngle` and `UpperAngle` limit the rotation.",
            },
            {
                Name = "LowerAngle",
                Type = "number",
                Default = "-90",
                Doc = "How far `Part1` can rotate clockwise, in degrees, from the relative angle the parts had "
                    .. "when the joint was created.",
            },
            {
                Name = "UpperAngle",
                Type = "number",
                Default = "90",
                Doc = "How far `Part1` can rotate counterclockwise, in degrees, from the relative angle the parts "
                    .. "had when the joint was created.",
            },
            {
                Name = "MotorEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the hinge drives itself toward `MotorSpeed`, for a wheel, a fan or a windmill.",
            },
            {
                Name = "MotorSpeed",
                Type = "number",
                Default = "0",
                Doc = "The speed the motor drives toward, in degrees per second. Positive is counterclockwise.",
            },
            {
                Name = "MotorMaxTorque",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The largest torque the motor can apply, in newton meters. A load that needs more stops the "
                    .. "motor.",
            },
        },
    },
    {
        Name = "SpringConstraint2D",
        Extends = "Constraint2D",
        Module = "scene",
        Storage = "SpringConstraint2D",
        Doc = "Holds two points at a distance, with a spring: a spring, a bungee cord or a suspension. With a "
            .. "`Stiffness` of 0 it is a rigid rod of `Length`, which suits the links of a rope bridge.",
        Properties = {
            {
                Name = "Length",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The rest length of the spring, in meters.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "4",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How quickly the spring returns to its rest length, as a frequency in hertz. At 0 the joint "
                    .. "is rigid.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The damping ratio of the spring. At 0 it oscillates forever; at 1 it settles without "
                    .. "overshooting.",
            },
            {
                Name = "MinLength",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The shortest the spring can be compressed to, in meters.",
            },
            {
                Name = "MaxLength",
                Type = "number",
                Default = "1000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The longest the spring can be stretched to, in meters.",
            },
        },
    },
    {
        Name = "WeldConstraint2D",
        Extends = "Constraint2D",
        Module = "scene",
        Storage = "WeldConstraint2D",
        Doc = "Holds two parts rigidly together, at the relative position they had when the weld was created. Use "
            .. "it for a sword in a hand, the lid of a crate or the body of a vehicle on its frame.",
    },
    {
        Name = "SpriteAnimator",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Part2D"),
        Storage = "SpriteAnimator",
        Records = { "ADR 0102" },
        Doc = "Plays the frames of a sprite sheet on its parent `Part2D`, on simulation time, so a walk cycle "
            .. "needs no script. On each tick that it plays, it sets the parent's `ImageRectOffset` and "
            .. "`ImageRectSize` to the current frame.\n\n"
            .. "Frames are counted along the rows of the sheet, starting at `SheetOffset`, with `Columns` frames "
            .. "per row. An animation that is not looped stops at its end and sets `Playing` to false. To be "
            .. 'notified, connect to `GetPropertyChangedSignal("Playing")`.',
        Properties = {
            {
                Name = "FrameSize",
                Type = "Vector2",
                Default = "Vector2.new(16, 16)",
                ErrorKey = "scene.err.size2_positive",
                Doc = "The size of one frame in the sheet, in pixels.",
            },
            {
                Name = "Columns",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.whole_number_at_least_one",
                Doc = "The number of frames in one row of the sheet.",
            },
            {
                Name = "SheetOffset",
                Type = "Vector2",
                Doc = "The position of frame 0, in pixels from the top left of the sheet. Use it to keep several "
                    .. "animations in one sheet, one below another.",
            },
            {
                Name = "FirstFrame",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.whole_number_at_least_zero",
                Doc = "The frame the animation starts on, counted from `SheetOffset`.",
            },
            {
                Name = "FrameCount",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.whole_number_at_least_one",
                Doc = "The number of frames the animation plays.",
            },
            {
                Name = "FramesPerSecond",
                Type = "number",
                Default = "8",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The speed of the animation, in frames per second of simulation time.",
            },
            {
                Name = "Looped",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the animation starts again at its end. When it is off, the animation stops on its "
                    .. "last frame.",
            },
            {
                Name = "Playing",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the animation is advancing. Setting it to true starts from `FirstFrame` if the "
                    .. "animation had stopped at its end, and continues from the current frame if it was paused.",
            },
            {
                Name = "Frame",
                Type = "number",
                ReadOnly = true,
                Doc = "The current frame, counted from 0 at `FirstFrame`.",
            },
        },
    },
    {
        Name = "NavigationArea",
        Extends = "Instance",
        Module = "scene",
        Storage = "NavigationArea",
        Parents = under("BasePart"),
        Records = { "ADR 0098" },
        Doc = "Marks the walkable ground inside its parent part's box with a `Label`. "
            .. "`NavigationService:SetAreaCost` sets the cost of walking through each label, for water that "
            .. "agents should go around, mud that slows them, or a closed door. The part only defines a volume; "
            .. "it does not need to collide or be anchored. Ground that no area marks has a cost of 1.",
        Properties = {
            {
                Name = "Label",
                Type = "string",
                Default = '"Area"',
                Doc = "The name the area's cost is set by. Areas with the same label share a cost.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "NavigationLink",
        Extends = "Instance",
        Module = "scene",
        Storage = "NavigationLink",
        Records = { "ADR 0098" },
        Doc = "Connects two points that the walkable ground does not join: a gap to jump, a ladder or a drop. A "
            .. "path can use the link. The third value `FindPath` returns gives the link's `Label` at the "
            .. "waypoint where the link begins, so the script that follows the path knows to jump there instead "
            .. "of walking. Both points are in world space, and each must be near walkable ground.",
        Properties = {
            {
                Name = "From",
                Type = "vector",
                Doc = "The position where the link starts, in world space.",
            },
            {
                Name = "To",
                Type = "vector",
                Doc = "The position where the link ends, in world space.",
            },
            {
                Name = "Bidirectional",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the link can also be taken from `To` back to `From`. A ladder can; a drop cannot.",
            },
            {
                Name = "Label",
                Type = "string",
                Default = '"Jump"',
                Doc = "A name for the kind of crossing, reported in the path.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Swarm",
        Extends = "Instance",
        Module = "scene",
        Storage = "Swarm",
        Parents = under("Workspace", "Model", "Folder"),
        Records = { "ADR 0156" },
        Doc = "Steers a large crowd of agents on open ground, such as a horde of hundreds. The agents walk toward "
            .. "`Target`, push each other apart, follow the height of the terrain and go around the obstacles the "
            .. "swarm was given. When the way is blocked, they climb over the agent in front and stand on the "
            .. "agents under them.\n\n"
            .. "An agent is not an instance. It is an entry the swarm keeps, with a radius, a height and a speed. "
            .. "It moves one body, the `BasePart` given to `AddAgent`, which the swarm places every tick facing "
            .. "`Target`. Scripts add, remove, push and query agents in bulk, and no Luau code runs per agent.\n\n"
            .. "The swarm updates every simulation tick, after `PreSimulation`, and is deterministic. An agent "
            .. "far from `Target` is updated less often: every second tick beyond `NearDistance` and every fourth "
            .. "beyond `FarDistance`, moving by the time it skipped.\n\n"
            .. "For agents that follow paths on a navigation mesh, use `NavigationAgent`.",
        Properties = {
            {
                Name = "Target",
                Type = "vector",
                Doc = "The position every agent walks toward, unless `SetTargets` has given other targets.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the swarm updates. When it is off, its agents stay where they are.",
            },
            {
                Name = "CellSize",
                Type = "number",
                Default = "1.5",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The side of the cells of the grid in which agents find their neighbors, in meters. Set it "
                    .. "to about the diameter of the widest agent.",
            },
            {
                Name = "MaxNeighbors",
                Aliases = { "MaxNeighbours" },
                Type = "number",
                Default = "14",
                Doc = "The largest number of neighbors an agent considers each tick. It limits the cost of a "
                    .. "tightly packed crowd.",
            },
            {
                Name = "Gravity",
                Type = "number",
                Default = "30",
                Doc = "The acceleration at which an agent falls, in meters per second squared.",
            },
            {
                Name = "ClimbSpeed",
                Type = "number",
                Default = "6",
                Doc = "The speed at which an agent climbs over the agent in its way, in meters per second.",
            },
            {
                Name = "PileHeight",
                Type = "number",
                Default = "0",
                Doc = "How high the crowd can pile on itself, in meters above the ground under an agent. 0 means "
                    .. "no limit.\n\n"
                    .. "Above this height, an agent does not climb the agent in its way, and an agent standing on "
                    .. "one that tall is pushed aside and comes down. A limit of two or three agents' heights "
                    .. "looks like a wave breaking over an obstacle. With no limit, a crowd around a stationary "
                    .. "obstacle builds a tower over it.",
            },
            {
                Name = "StopDistance",
                Type = "number",
                Default = "0.55",
                Doc = "The distance from `Target`, beyond the agent's own radius, at which an agent stops walking "
                    .. "toward it.",
            },
            {
                Name = "NearDistance",
                Type = "number",
                Default = "24",
                Doc = "The distance from `Target`, in meters, beyond which an agent is updated every second tick.",
            },
            {
                Name = "FarDistance",
                Type = "number",
                Default = "48",
                Doc = "The distance from `Target`, in meters, beyond which an agent is updated every fourth tick.",
            },
            {
                Name = "Replicates",
                Type = "boolean",
                Default = "false",
                Records = { "ADR 0162" },
                Doc = "Whether the swarm's agents are replicated to the other machines in a multiplayer match. "
                    .. "When it is on, each client has this swarm with the same agent numbers, read-only. On a "
                    .. "client, `GetAgents`, `GetPositions`, `GetAgentPosition`, `GetAgentTag` and `QueryRadius` "
                    .. "return results, `AgentAdded`, `AgentRemoved` and `AgentTagChanged` fire, and "
                    .. "`SetAgentBody` gives an agent a local body to move.\n\n"
                    .. "An agent's state is sent to a client when the client's copy would otherwise be drawn "
                    .. "wrong, and the client moves the agent itself in between. A client's agent is therefore "
                    .. "close to the authority's position but not exactly at it; decide hits on the authority. A "
                    .. "client has only the agents within `ReplicationRadius` of its player's focus.\n\n"
                    .. "When it is off, the swarm exists only on the machine that runs it.",
            },
            {
                Name = "ReplicationRadius",
                Type = "number",
                Default = "80",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The distance from a player's focus, in meters, within which agents are sent to that "
                    .. "player's machine. The focus is the player's `Character`, or `Player.ReplicationFocus`. An "
                    .. "agent that leaves the radius is removed on that machine (`AgentRemoved` with "
                    .. "`OutOfReach`) and added again when it returns.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "AddAgent",
                Parameters = {
                    { Name = "body", Type = "BasePart" },
                    {
                        Name = "settings",
                        Type = "{ Radius: number?, Height: number?, Speed: number?, Floats: boolean?, "
                            .. "FloatHeight: number?, Climbs: boolean? }?",
                    },
                },
                Returns = { "number" },
                Doc = "Adds an agent at the position of `body` and returns its number. The options are `Radius` "
                    .. "(default 0.5), `Height` (1) and `Speed` (4, in meters per second). `Floats` keeps the "
                    .. "agent `FloatHeight` (1) above the ground, where the crowd does not push it. `Climbs` "
                    .. "(true) lets it climb over the agent in its way.\n\n"
                    .. "The number identifies the agent while it exists and is reused for a later agent after "
                    .. "this one is removed. For an agent with no body, use `AddAgentAt`.",
            },
            {
                Name = "AddAgentAt",
                Parameters = {
                    { Name = "position", Type = "vector" },
                    {
                        Name = "settings",
                        Type = "{ Radius: number?, Height: number?, Speed: number?, Floats: boolean?, "
                            .. "FloatHeight: number?, Climbs: boolean? }?",
                    },
                },
                Returns = { "number" },
                Doc = "Adds an agent that has no body, with its feet at `position`, and returns its number. The "
                    .. "swarm simulates it and nothing is drawn. Use it on a server that simulates a horde for "
                    .. "clients that draw it themselves: read the agents with `GetPositions` and send what the "
                    .. "clients need. The options are those of `AddAgent`.",
            },
            {
                Name = "SetTargets",
                Parameters = { { Name = "targets", Type = "{ vector }" } },
                Returns = {},
                Doc = "Sets several positions for the agents to walk toward, such as every player in a match. "
                    .. "Each agent walks toward the nearest one, and its update rate (`NearDistance`, "
                    .. "`FarDistance`) depends on its distance to that one. Call it again whenever the positions "
                    .. "change. An empty table goes back to using `Target` alone.",
            },
            {
                Name = "RemoveAgent",
                Parameters = { { Name = "agent", Type = "number" } },
                Returns = {},
                Doc = "Removes an agent from the swarm. Its body stays where it is and is no longer moved.",
            },
            {
                Name = "SetAgentSpeed",
                Parameters = { { Name = "agent", Type = "number" }, { Name = "speed", Type = "number" } },
                Returns = {},
                Doc = "Sets how fast an agent walks, in meters per second. At 0 the agent stays in place, and "
                    .. "other agents still push it.",
            },
            {
                Name = "SetAgentPosition",
                Parameters = { { Name = "agent", Type = "number" }, { Name = "position", Type = "vector" } },
                Returns = {},
                Doc = "Moves an agent to a position. It falls to the ground from there.",
            },
            {
                Name = "Push",
                Parameters = { { Name = "agent", Type = "number" }, { Name = "velocity", Type = "vector" } },
                Returns = {},
                Doc = "Knocks an agent back with a velocity along the ground, in meters per second, that fades "
                    .. "quickly.",
            },
            {
                Name = "GetAgentPosition",
                Parameters = { { Name = "agent", Type = "number" } },
                Returns = { "vector?" },
                Doc = "Returns the position of an agent, or nil if there is no such agent.",
            },
            {
                Name = "GetAgents",
                Parameters = { { Name = "into", Type = "{ number }?" } },
                Returns = { "{ number }" },
                Doc = "Returns the number of every live agent, in slot order. If `into` is given, that table is "
                    .. "cleared, filled and returned, so a script that calls this every tick allocates nothing.",
            },
            {
                Name = "GetPositions",
                Parameters = { { Name = "into", Type = "{ vector }?" } },
                Returns = { "{ vector }" },
                Doc = "Returns the position of every live agent, in the order `GetAgents` returns them. If `into` "
                    .. "is given, that table is cleared, filled and returned.",
            },
            {
                Name = "QueryRadius",
                Parameters = {
                    { Name = "center", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "into", Type = "{ number }?" },
                    { Name = "flat", Type = "boolean?" },
                },
                Returns = { "{ number }" },
                Doc = "Returns every live agent whose feet are within `radius` of `center`, in slot order. Use it "
                    .. "to find what a weapon hits, or which agents have reached `Target`. Its cost depends on "
                    .. "the number of agents near `center`, not on the size of the swarm.\n\n"
                    .. "If `into` is given, that table is cleared, filled and returned. With `flat`, distance is "
                    .. "measured along the ground only, so every agent of a pile above the circle is included.",
            },
            {
                Name = "AddObstacle",
                Parameters = { { Name = "position", Type = "vector" }, { Name = "radius", Type = "number" } },
                Returns = {},
                Doc = "Adds an obstacle that agents walk around, such as a tree or a rock, as a circle on the ground.",
            },
            {
                Name = "ClearObstacles",
                Parameters = {},
                Returns = {},
                Doc = "Removes every obstacle.",
            },
            {
                Name = "SetAgentTag",
                Parameters = { { Name = "agent", Type = "number" }, { Name = "tag", Type = "number" } },
                Returns = {},
                Records = { "ADR 0162" },
                Doc = "Sets an agent's tag: an integer from 0 to 65535 whose meaning the game defines, for "
                    .. "example a kind in some bits and flags in others. Only the authority can call it.\n\n"
                    .. "The tag replicates to every machine that has the agent. Those machines read it with "
                    .. "`GetAgentTag` and receive `AgentTagChanged`; a client typically chooses a mesh and a pose "
                    .. 'from it. The tag an agent has when it is removed is sent with the removal, so a "killed" '
                    .. "flag set just before `RemoveAgent` reaches every machine's `AgentRemoved`.",
            },
            {
                Name = "GetAgentTag",
                Parameters = { { Name = "agent", Type = "number" } },
                Returns = { "number" },
                Doc = "Returns an agent's tag. Returns 0 for an agent that was never given one, or that does not "
                    .. "exist.",
            },
            {
                Name = "SetAgentBody",
                Parameters = { { Name = "agent", Type = "number" }, { Name = "body", Type = "BasePart?" } },
                Returns = {},
                Doc = "Gives an agent a part on this machine to move, or nil to release it. Every tick the swarm "
                    .. "sets the part's `CFrame` to the agent's position, facing its target, and changes nothing "
                    .. "else about the part.\n\n"
                    .. "On the authority, it replaces the body given to `AddAgent`. On a client, it is how a "
                    .. "replicated horde is drawn: create a body when `AgentAdded` fires, pass it here, and take "
                    .. "it back when `AgentRemoved` fires. The part is drawn interpolated between ticks like any "
                    .. "moving part.",
            },
        },
        Events = {
            {
                Name = "AgentAdded",
                Parameters = {
                    { Name = "agent", Type = "number" },
                    { Name = "tag", Type = "number" },
                    { Name = "size", Type = "vector" },
                },
                Doc = "Fires when this machine gains an agent. On the authority that is after `AddAgent`; on a "
                    .. "client it is when an agent arrives, either newly created or entering the client's "
                    .. "replication radius. `size` is the agent's width, height and width again. The number "
                    .. "identifies the agent on every machine until `AgentRemoved` fires for it, and can be "
                    .. "reused after that.",
            },
            {
                Name = "AgentRemoved",
                Parameters = {
                    { Name = "agent", Type = "number" },
                    { Name = "tag", Type = "number" },
                    { Name = "position", Type = "vector" },
                    { Name = "reason", Type = "Enum.SwarmAgentRemoval" },
                },
                Doc = "Fires when this machine loses an agent. Passes the agent's last tag and position, and the "
                    .. "reason: the authority removed it, or, on a client, it left the client's replication "
                    .. "radius. Play a death effect for the first reason and nothing for the second.",
            },
            {
                Name = "AgentTagChanged",
                Parameters = { { Name = "agent", Type = "number" }, { Name = "tag", Type = "number" } },
                Doc = "Fires when the tag of an agent that this machine has changes.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "NavigationAgent",
        Extends = "Instance",
        Module = "scene",
        Storage = "NavigationAgent",
        Parents = under("BasePart"),
        Records = { "ADR 0098" },
        Doc = "Walks its parent part to a point across the walkable ground, avoiding other agents on the way. The "
            .. "engine moves the part on simulation time: set `Target`, and `Reached` fires when the part "
            .. "arrives.\n\n"
            .. "For a single character, a script that follows the waypoints from `FindPath` is enough. Use agents "
            .. "for many characters that must not walk through each other.",
        Properties = {
            {
                Name = "Target",
                Type = "vector",
                Doc = "The position the agent walks to. Setting it starts the walk.",
            },
            {
                Name = "Active",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the agent is walking. Setting `Target` turns it on, and arriving turns it off. "
                    .. "While it is off, the agent stands still.",
            },
            {
                Name = "MaxSpeed",
                Type = "number",
                Default = "8",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The agent's top speed, in meters per second.",
            },
            {
                Name = "AgentType",
                Type = "string",
                Default = '""',
                Doc = "The name of the agent type, as defined with `NavigationService:DefineAgent`. When it is "
                    .. "empty, the service's default type is used.",
            },
        } :: { Schema.Property },
        Events = {
            {
                Name = "Reached",
                Parameters = {},
                Doc = "Fires once when the agent arrives at `Target`.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "FoliageLayer",
        Extends = "Instance",
        Module = "render",
        Parents = under("Terrain"),
        Storage = "FoliageLayer",
        Records = { "ADR 0116" },
        Doc = "Covers terrain with foliage such as grass, flowers, stones and bushes. Its `FoliageMesh` children "
            .. "set what grows, and its properties set where: by the ground's material, slope and height, and "
            .. "never under a roof.\n\n"
            .. "It is visual only. Nothing collides with it, raycasts pass through it, and it is not replicated; "
            .. "every machine generates the same field from the same terrain and `Seed`.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether the layer grows. When it is off, nothing is placed or drawn.",
            },
            {
                Name = "Density",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "The number of instances per square meter of ground where every rule allows growth. The "
                    .. "material's multiplier, the painted mask and the `[render] foliage_density` setting scale it.",
            },
            {
                Name = "SlopeMin",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_slope_degrees",
                Presentation = true,
                Doc = "The flattest ground the layer grows on, in degrees from level.",
            },
            {
                Name = "SlopeMax",
                Type = "number",
                Default = "35",
                ErrorKey = "scene.err.number_slope_degrees",
                Presentation = true,
                Doc = "The steepest ground the layer grows on, in degrees from level.",
            },
            {
                Name = "HeightMin",
                Type = "number",
                Default = "-10000",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "The lowest height the layer grows at, in meters in world space.",
            },
            {
                Name = "HeightMax",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "The highest height the layer grows at, in meters in world space. Use it for a tree line.",
            },
            {
                Name = "Clumping",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much the foliage gathers into patches, from 0 (even cover) to 1 (tufts with bare "
                    .. "ground between them).",
            },
            {
                Name = "MinSpacing",
                Type = "number",
                Default = "0.25",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "The smallest distance between two instances, in meters.",
            },
            {
                Name = "DrawDistance",
                Type = "number",
                Default = "120",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "The distance from the camera, in meters, within which the layer is drawn.",
            },
            {
                Name = "FadeDistance",
                Type = "number",
                Default = "20",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "The distance, in meters, before `DrawDistance` over which the foliage thins out, so that "
                    .. "the edge of the field is not a visible line.",
            },
            {
                Name = "Seed",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "A number that selects one of the possible arrangements the rules allow. The same seed "
                    .. "gives the same field on every machine.",
            },
            {
                Name = "ReceivesDecals",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether a `Decal` also marks the foliage. When it is on, a decal on the ground also "
                    .. "appears on the foliage standing in it, whichever way each blade faces. When it is off, "
                    .. "decals skip the foliage, as they skip a part with `BasePart.ReceivesDecals` off.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetMaterials",
                Parameters = {},
                Returns = { "{ { Material: number, Density: number } }" },
                ThreadSafety = "Safe",
                Doc = "Returns the terrain materials the layer grows on, each with a multiplier on `Density`. "
                    .. "Material `n` is the terrain's layer `n` (see `Terrain:GetLayers`). An empty list means "
                    .. "every material, at a multiplier of 1.",
            },
            {
                Name = "SetMaterials",
                Parameters = {
                    { Name = "materials", Type = "{ { read Material: number, read Density: number? } }" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets the terrain materials the layer grows on, in the form that `GetMaterials` returns: up "
                    .. "to 32, with `Density` defaulting to 1. An empty list grows on every material. The list is "
                    .. "saved with the scene.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "FoliageMesh",
        Extends = "Instance",
        Module = "render",
        Parents = under("FoliageLayer"),
        Storage = "FoliageMesh",
        Records = { "ADR 0116" },
        Doc = "One mesh that a `FoliageLayer` grows. It sets the mesh's share of the layer, its size, how it sits "
            .. "on the ground and how the wind moves it.",
        Properties = {
            {
                Name = "Mesh",
                Type = "Content",
                Default = '""',
                ContentKind = "Mesh",
                Presentation = true,
                Doc = "The mesh to grow. Any mesh asset in the project can be used.",
            },
            {
                Name = "Weight",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "The mesh's share of the layer's instances, relative to the layer's other meshes. Two "
                    .. "meshes with weights 1 and 3 grow in a ratio of one to three.",
            },
            {
                Name = "ScaleMin",
                Type = "number",
                Default = "0.8",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "The smallest scale of an instance, as a multiple of the mesh's size.",
            },
            {
                Name = "ScaleMax",
                Type = "number",
                Default = "1.2",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "The largest scale of an instance, as a multiple of the mesh's size.",
            },
            {
                Name = "RandomRotation",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether each instance is rotated by a random angle about its up axis, so that the field "
                    .. "does not look like rows of identical copies.",
            },
            {
                Name = "AlignToNormal",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much an instance leans with the slope of the ground, from 0 (upright) to 1 "
                    .. "(perpendicular to the ground).",
            },
            {
                Name = "Sink",
                Type = "number",
                Default = "0.05",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "How far below the ground the base of an instance is placed, in meters, so that a stem on a "
                    .. "slope does not float at one edge.",
            },
            {
                Name = "WindResponse",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much the wind moves the mesh, from 0 (still) to 1 (full sway).",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "How strongly the mesh resists the wind. A reed is about 0.5 and a bush about 3.",
            },
            {
                Name = "CastShadow",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether the mesh casts a shadow. Foliage casts shadows only near the camera, within "
                    .. "`[render] foliage_shadow_distance`.",
            },
            {
                Name = "Material",
                Type = "Material?",
                Presentation = true,
                Doc = "The material the mesh is drawn with, or empty to use the mesh's own. The material's maps "
                    .. "and values are used. Its surface shader is not run on foliage yet: the built-in sway "
                    .. "bends the mesh from its base.",
            },
        } :: { Schema.Property },
    },
    -- ADR 0126: a part pressed, prompted and pulled without code.
    {
        Name = "ClickDetector",
        Extends = "Instance",
        Module = "scene",
        Parents = under("BasePart", "Model"),
        Storage = "ClickDetector",
        Records = { "ADR 0126" },
        Doc = "Lets a player click the part or model it is parented to. Every tick the engine casts a ray from "
            .. "the pointer, a mouse cursor or a tap, into the world. The nearest part that has a detector, on "
            .. "itself or on a model that contains it, is the one pointed at. A part with `CanQuery` off is "
            .. "ignored.\n\n"
            .. "In a multiplayer match, a click is sent to the authority, which checks the distance from the "
            .. "player's character and fires `MouseClick` with the player. The machine that clicked also fires "
            .. "it, so local scripts react at once. Hover events fire only on the local machine.",
        Properties = {
            {
                Name = "MaxActivationDistance",
                Type = "number",
                Default = "32",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The largest distance from the player's character, in meters, at which a click counts. With "
                    .. "no character, it is measured from the camera.",
            },
            {
                Name = "CursorIcon",
                Type = "Content",
                Default = '""',
                ContentKind = "Texture",
                Inert = true,
                Doc = "The image the pointer shows over the detector. The value is saved and replicated but is "
                    .. "not used yet: the pointer does not change.",
            },
        } :: { Schema.Property },
        Events = {
            {
                Name = "MouseClick",
                Parameters = { { Name = "player", Type = "Player" } },
                Doc = "Fires when the primary button is pressed, or a tap lands, on the detector within range.",
            },
            {
                Name = "RightMouseClick",
                Parameters = { { Name = "player", Type = "Player" } },
                Doc = "Fires when the secondary button is pressed on the detector within range.",
            },
            {
                Name = "MouseHoverEnter",
                Parameters = { { Name = "player", Type = "Player" } },
                Doc = "Fires when the pointer moves onto the detector within range. It fires only on the local "
                    .. "machine.",
            },
            {
                Name = "MouseHoverLeave",
                Parameters = { { Name = "player", Type = "Player" } },
                Doc = "Fires when the pointer leaves the detector or the detector goes out of range. It fires "
                    .. "only on the local machine.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "Water",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Workspace"),
        Storage = "Water",
        Records = { "ADR 0118" },
        Doc = "A body of water: a sea, a pool, a lake or a river. Its `WaterWave` children define the waves, and "
            .. "the same waves are used for the simulation and for the picture, so objects float on the surface "
            .. "that is drawn.\n\n"
            .. "An unanchored part in the water is held up by the water it displaces, sampled at points over its "
            .. "shape, so a hull pitches and rolls. `Viscosity` slows the part and `Current` carries it. "
            .. "`GetHeightAt` returns the height of the surface for a game's own use.",
        Properties = {
            {
                Name = "Shape",
                Type = "Enum.WaterShape",
                Default = "Enum.WaterShape.Ocean",
                Doc = "The extent of the water. `Ocean` is everywhere, `Pool` is a rectangle, `River` runs along "
                    .. "its `WaterPoint` children, and `Lake` fills the closed curve through them.",
            },
            {
                Name = "SurfaceLevel",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Doc = "The height of the still surface, in meters. The waves rise and fall around it.",
            },
            {
                Name = "Position",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "The center of a `Pool`, on the horizontal plane. Its height is set by `SurfaceLevel`.",
            },
            {
                Name = "Size",
                Type = "vector",
                Default = "vector.create(64, 8, 64)",
                ErrorKey = "scene.err.water_size",
                Doc = "The dimensions of the water, in meters. For a `Pool`: its width, its depth below the "
                    .. "surface and its length. For a `River`: X is the width and Y the depth at any point that "
                    .. "does not set its own. For a `Lake`: Y is the depth, and the outline comes from its "
                    .. "points. Y must be greater than 0; X and Z can be 0.",
            },
            {
                Name = "Density",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The density of the water, in the same units as `BasePart.Density`. A part less dense than "
                    .. "the water floats, and a part of half the water's density floats half submerged.",
            },
            {
                Name = "Viscosity",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much the water slows what moves through it. 0 is no drag, 1 suits a sea, and higher "
                    .. "values suit a swamp.",
            },
            {
                Name = "Current",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "The velocity at which the water flows, in meters per second. It carries floating objects. "
                    .. "A `River` adds `FlowSpeed` along its course.",
            },
            {
                Name = "FlowSpeed",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Doc = "How fast a river flows along its points, from the first to the last, in meters per second, "
                    .. "where it is level. Where it descends it flows faster, by four times its slope: twice as "
                    .. "fast on a slope of one in four.",
            },
            {
                Name = "BankWidth",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The width of the bank that `Carve` cuts, in meters. The bed slopes from the surface at the "
                    .. "water's edge down to the water's depth over this distance. At 0 the bank is a vertical wall.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetHeightAt",
                Parameters = { { Name = "position", Type = "vector" } },
                Returns = { "number?" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns the current height of the surface above a position, including waves, or nil if "
                    .. "this water does not cover the position. It matches the surface that is drawn.",
            },
            {
                Name = "GetNormalAt",
                Parameters = { { Name = "position", Type = "vector" } },
                Returns = { "vector?" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns the direction the surface currently faces above a position, or nil if this water "
                    .. "does not cover the position.",
            },
            {
                Name = "Carve",
                Parameters = {},
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0146" },
                Doc = "Cuts a bed for this water into the terrain under it and returns the number of columns it "
                    .. "lowered. Every column of ground the water covers is lowered to the bed, which slopes from "
                    .. "the surface at the water's edge down to the water's depth over `BankWidth`. Ground that "
                    .. "is already lower is not changed. A river's bed follows the river downhill.\n\n"
                    .. "An `Ocean` carves nothing. Carving only removes ground, so after moving the water, call "
                    .. "it again to cut the new bed.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "WaterWave",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Water"),
        Storage = "WaterWave",
        Records = { "ADR 0118" },
        Doc = "One wave of the `Water` it is parented to. The first eight waves, in order, form the surface. A "
            .. "wave travels at the speed that deep water gives a wave of its length.",
        Properties = {
            {
                Name = "Wavelength",
                Type = "number",
                Default = "40",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The distance from one crest to the next, in meters.",
            },
            {
                Name = "Amplitude",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The height of a crest above the still surface, in meters.",
            },
            {
                Name = "Direction",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Doc = "The direction the wave travels in, in degrees about the vertical axis. 0 is along +X and "
                    .. "90 is along +Z.",
            },
            {
                Name = "Steepness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "The shape of the wave, from 0 (a rounded swell) to 1 (sharp crests and flat troughs).",
            },
            {
                Name = "Phase",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Doc = "The point in its cycle at which the wave starts, in radians. Use it so that two similar "
                    .. "waves do not rise together.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "WaterPoint",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Water"),
        Storage = "WaterPoint",
        Records = { "ADR 0146" },
        Doc = "One of the points that a `River` runs through or that the outline of a `Lake` passes through, in "
            .. "order. The water follows a smooth curve that passes through every point without looping.",
        Properties = {
            {
                Name = "Position",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "The position the water passes through. For a `River`, its height is the height of the "
                    .. "surface there, so a river whose points descend flows downhill. A `Lake` and a `Spline` "
                    .. "are level at the water's `SurfaceLevel` and use only the horizontal position.",
            },
            {
                Name = "Width",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The width of a river at this point, in meters. It changes smoothly toward the next point's "
                    .. "width. At 0, the water's `Size.X` is used.",
            },
            {
                Name = "Depth",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The depth of a river at this point, in meters. At 0, the water's `Size.Y` is used.",
            },
            {
                Name = "Sharp",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the curve has a corner at this point instead of being rounded.",
            },
        } :: { Schema.Property },
    },
}

local Definition: Schema.Definition = {
    Classes = Classes,
}

return Definition
