--!strict
--- Instance classes (api-design.md §2.2).
---
--- The M2 surface only: `Instance` and the classes a script can build a tree
--- out of. Classes whose behaviour belongs to a later milestone arrive with it,
--- because a property that accepts a write and changes nothing is worse than a
--- missing one -- it type-checks.

local Schema = require("../schema")

--- The attribute value domain (§2.2). Written once because it widens as the
--- remaining datatypes ship, and three copies would widen at three different
--- times. It widened once, at M6, when `Vector2`, `UDim`, `UDim2` and `Rect`
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
        Doc = "The root of the class hierarchy: a node in the tree, carrying a name, a parent, attributes and tags.",
        Properties = {
            {
                Name = "Name",
                Type = "string",
                Doc = "This instance's name; siblings may share one, and renaming into an awaited name satisfies a WaitForChild waiter exactly as parenting a new child does.",
            },
            {
                Name = "Parent",
                Type = "Instance?",
                Doc = "The instance this one hangs under, nil when unparented; assigning it appends this instance last among its new siblings, assigning the current parent again changes nothing, and assigning after Destroy raises.",
            },
            {
                Name = "ClassName",
                Type = "string",
                ReadOnly = true,
                Doc = "The name of this instance's class; typeof answers Instance whatever the class, so this is what tells two classes apart.",
            },
        },
        Methods = {
            {
                Name = "FindFirstChild",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "Instance?" },
                Doc = "The first direct child with that name in document order, or nil; there is no recursive form in v1, so depth is a GetDescendants loop.",
            },
            {
                Name = "FindFirstChildOfClass",
                Parameters = { { Name = "className", Type = "string" } },
                Returns = { "Instance?" },
                Doc = "The first direct child whose ClassName is exactly className, or nil; asking for BasePart therefore never finds a Part.",
            },
            {
                Name = "FindFirstChildWhichIsA",
                Parameters = { { Name = "className", Type = "string" } },
                Returns = { "Instance?" },
                Doc = "The first direct child that IsA className, or nil; matching runs through the class hierarchy, so abstract base names are accepted.",
            },
            {
                Name = "FindFirstAncestor",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "Instance?" },
                Doc = "The nearest ancestor with that name, searching upward from Parent; the instance itself is never a candidate.",
            },
            {
                Name = "FindFirstAncestorOfClass",
                Parameters = { { Name = "className", Type = "string" } },
                Returns = { "Instance?" },
                Doc = "The nearest ancestor whose ClassName is exactly className, searching upward from Parent; the instance itself is never a candidate.",
            },
            {
                Name = "GetFullName",
                Parameters = {},
                Returns = { "string" },
                Doc = "The path to the instance, as the names of its ancestors and its own joined by dots: "
                    .. "`Workspace.Recursos.Tree.Trunk`. It starts below the data model, as a script's own path "
                    .. "from `game` does. For reading in a log -- two siblings may share a name, so it does not "
                    .. "say which of them this is.",
            },
            {
                Name = "GetChildren",
                Parameters = {},
                Returns = { "{Instance}" },
                Doc = "A fresh array of the direct children in child order, which is the order they were parented; the caller owns it, so destroying while iterating is safe.",
            },
            {
                Name = "GetDescendants",
                Parameters = {},
                Returns = { "{Instance}" },
                Doc = "A fresh array of every descendant in document order -- child order taken depth-first, preorder -- which is the order the Find family tie-breaks on.",
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
                Doc = "Parks the caller until a child of that name exists here and returns it, returning immediately when one already does; with a timeout, expiry returns nil, and without one the wait is unbounded and warns after five sim-seconds.",
            },
            {
                Name = "IsA",
                Parameters = { { Name = "className", Type = "string" } },
                Returns = { "boolean" },
                Doc = "Whether this instance is of that class or descends from it; a string naming no class answers false rather than raising, because the point is to test names you do not trust.",
            },
            {
                Name = "IsAncestorOf",
                Parameters = { { Name = "descendant", Type = "Instance" } },
                Returns = { "boolean" },
                Doc = "Whether descendant sits anywhere below this instance; read strictly, so an instance is not its own ancestor. False when either is destroyed, so a check needs no pcall.",
            },
            {
                Name = "IsDescendantOf",
                Parameters = { { Name = "ancestor", Type = "Instance" } },
                Returns = { "boolean" },
                Doc = "Whether this instance sits anywhere below ancestor; read strictly, so an instance is not its own descendant. False when either is destroyed, so a check needs no pcall.",
            },
            {
                Name = "Clone",
                Parameters = {},
                Returns = { "Instance" },
                Doc = "Deep-copies this subtree unparented -- children, properties, attributes and tags at every depth -- rewiring object references that point inside the copy and leaving those that point outside it on the originals.",
            },
            {
                Name = "Destroy",
                Parameters = {},
                Returns = {},
                Doc = "Removes this instance and its children from the tree synchronously, clears its tags, locks Parent to nil and defers Destroying; the handle stops resolving at the end of that drain. After that, Parent still reads nil and IsDescendantOf and IsAncestorOf answer false -- what defensive code asks -- while Name, ClassName and IsA raise, the instance's data being gone.",
            },
            {
                Name = "GetAttribute",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { OptionalAttributeValue },
                Doc = "The value of that attribute, or nil when it has never been set; attribute names are case-sensitive.",
            },
            {
                Name = "SetAttribute",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "value", Type = OptionalAttributeValue },
                },
                Returns = {},
                Doc = "Sets that attribute, or removes it when value is nil; a table, an Instance or a function raises and leaves any previous value in place, and an empty name raises.",
            },
            {
                Name = "GetAttributes",
                Parameters = {},
                Returns = { AttributeMap },
                Doc = "A fresh table of this instance's attributes, which the caller owns.",
            },
            {
                Name = "GetAttributeChangedSignal",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "Signal<()>" },
                Doc = "The signal fired when that attribute changes; any name is accepted, because an attribute that has never been set is a reasonable thing to wait for.",
            },
            {
                Name = "GetPropertyChangedSignal",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "Signal<()>" },
                Doc = "The signal fired when that property changes -- the same object on every call, carrying no value so its type stays independent of the property's; a name the class does not have raises.",
            },
            {
                Name = "AddTag",
                Parameters = { { Name = "tag", Type = "string" } },
                Returns = {},
                Doc = "Adds a tag to this instance, a no-op when it is already present; tags are pure instance state, so a nil-parented instance still carries and is found by them.",
            },
            {
                Name = "RemoveTag",
                Parameters = { { Name = "tag", Type = "string" } },
                Returns = {},
                Doc = "Removes a tag from this instance, a no-op when it is absent.",
            },
            {
                Name = "HasTag",
                Parameters = { { Name = "tag", Type = "string" } },
                Returns = { "boolean" },
                Doc = "Whether this instance currently carries that tag.",
            },
            {
                Name = "GetTags",
                Parameters = {},
                Returns = { "{string}" },
                Doc = "A fresh array of this instance's tags, which the caller owns.",
            },
        },
        Events = {
            {
                Name = "ChildAdded",
                Parameters = { { Name = "child", Type = "Instance" } },
                Doc = "Fired after an instance is parented to this one, carrying that child.",
            },
            {
                Name = "ChildRemoved",
                Parameters = { { Name = "child", Type = "Instance" } },
                Doc = "Fired after an instance stops being parented to this one, carrying that child.",
            },
            {
                Name = "DescendantAdded",
                Parameters = { { Name = "descendant", Type = "Instance" } },
                Doc = "Fired after an instance is added anywhere below this one, carrying that descendant.",
            },
            {
                Name = "DescendantRemoving",
                Parameters = { { Name = "descendant", Type = "Instance" } },
                Doc = "Fired as an instance leaves the subtree below this one, carrying that descendant; leaving is fully observed before arriving.",
            },
            {
                Name = "AncestryChanged",
                Parameters = {
                    { Name = "instance", Type = "Instance" },
                    { Name = "newParent", Type = "Instance?" },
                },
                Doc = "Fired on a moved instance and then on each of its descendants in document order, carrying the instance whose ancestry changed and its new parent, nil when it was unparented.",
            },
            {
                Name = "AttributeChanged",
                Parameters = { { Name = "name", Type = "string" } },
                Doc = "Fired when any attribute of this instance changes, carrying the attribute name.",
            },
            {
                Name = "Destroying",
                Parameters = {},
                Doc = "Fired once when Destroy is called, carrying nothing; the tree mutation has already happened and only the signal is deferred.",
            },
        },
    },
    {
        Name = "PVInstance",
        Extends = "Instance",
        Module = "scene",
        Tags = { "Abstract", "NotCreatable" } :: { Schema.ClassTag },
        Storage = "PV",
        Doc = "The base of everything with a position and orientation in the world: a pivot to move it by, and "
            .. "an offset saying where that pivot sits. BasePart, Model and Camera are the three.",
        Properties = {
            {
                Name = "PivotOffset",
                Type = "CFrame",
                Doc = "Where this object's pivot sits relative to the object itself; the identity puts it at the "
                    .. "centre. Set it to a hinge edge and PivotTo rotates about that edge.",
            },
        },
        Methods = {
            {
                Name = "GetPivot",
                Parameters = {},
                Returns = { "CFrame" },
                Doc = "This object's pivot in world space.",
            },
            {
                Name = "PivotTo",
                Parameters = { { Name = "target", Type = "CFrame" } },
                Returns = {},
                Doc = "Moves this object so its pivot lands on target. A Model moves every part under it and keeps "
                    .. "their relative layout; a BasePart or Camera moves itself.",
            },
        },
    },
    {
        Name = "Folder",
        Extends = "Instance",
        Module = "scene",
        Doc = "A node with no behaviour of its own, for grouping instances; mounting a script folder under `src/` also "
            .. "builds one per subdirectory.",
    },
    {
        Name = "Model",
        Extends = "PVInstance",
        Module = "scene",
        Parents = InTheWorldOrAFrame,
        Storage = "Model",
        Doc = "A group of parts handled as one object, with a pivot to move it by and an extents box to measure it with.",
        Properties = {
            {
                Name = "PrimaryPart",
                Type = "BasePart?",
                Doc = "The part this model's pivot follows; Clone rewires it to the copy, since it is the reference into a cloned subtree that catches people.",
            },
            {
                Name = "StreamingMode",
                Type = "Enum.StreamingMode",
                Default = "Enum.StreamingMode.Nonatomic",
                Doc = "What this model does when the world is partitioned into a streaming grid (ADR 0053). It "
                    .. "is the one thing a person says about a world they built by hand: the grid decides WHEN "
                    .. "something becomes eligible, and this decides WHAT comes with it.\n\n**The default "
                    .. "changes nothing.** `Nonatomic` places each part in the cell its own position falls in, "
                    .. "which is how a world of loose scenery already behaves. `Atomic` makes the model one "
                    .. "unit that lives in a single cell however far it spreads, and `Persistent` keeps it out "
                    .. "of the grid entirely so it is always there.\n\nIt is read when the world is "
                    .. "partitioned, not every frame: changing it at run time changes nothing until the world "
                    .. "is built again.",
            },
            {
                Name = "Scale",
                Type = "number",
                Default = "1",
                Doc = "How big this model is relative to the size it was built at. Setting it resizes every "
                    .. "part under the model and moves each one about the model's pivot, so the whole thing "
                    .. "grows in place rather than drifting away from where it stood.\n\n**It is "
                    .. "absolute, not cumulative.** Setting 2 twice leaves the model twice its built size, "
                    .. "not four times -- the write scales by the ratio between the new value and the old "
                    .. "one. Setting 1 puts it back exactly.\n\n**A descendant added afterwards is "
                    .. "not scaled.** The write fans out once over what is there and then it is done: "
                    .. "nothing is multiplied by this per frame, which is what makes a scaled model cost "
                    .. "the same to draw and simulate as an unscaled one. Parent the part first, then "
                    .. "scale.\n\nZero and negative values are refused. A part with no size is not "
                    .. "a small part, it is a shape the solver cannot build.",
            },
        },
        Methods = {
            {
                Name = "GetExtentsSize",
                Parameters = {},
                Returns = { "vector" },
                Doc = "The size in metres of the smallest box enclosing every part in this model.",
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
        Doc = "The abstract base of anything that carries Luau code. Its source is a PROPERTY, so a script is an "
            .. "ordinary instance: it is created like any other, it lives wherever you put it, and it is saved "
            .. "inside whatever holds it.",
        Properties = {
            {
                Name = "Source",
                Type = "string",
                Code = true,
                ScriptReadOnly = true,
                Default = '""',
                Doc = "The Luau this instance carries. It is data on the instance rather than a path to a file, "
                    .. "which is what makes a script something you can create, copy, put inside a prefab and save "
                    .. "in a scene like anything else.\n\n"
                    .. "**The editor writes it, and a script only reads it** (ADR 0137): a script that could "
                    .. "write another's `Source` and enable it would run text the sandbox never loaded. A script "
                    .. "made at run time takes its code from a copy -- `Clone` of one in the scene, or "
                    .. "`Instance.stamp`.",
            },
        },
    },
    {
        Name = "Script",
        Extends = "BaseScript",
        Module = "scene",
        Doc = "Luau that RUNS, exactly while it is live: enabled, in the world, under no storage, and its side "
            .. "running on this machine (ADR 0137)."
            .. "\n\n**Where it runs** (ADR 0138): under `ServerScriptService` or `GlobalScriptService.Server` "
            .. "only where this machine is the authority; under `ClientScriptService` or "
            .. "`GlobalScriptService.Client` only where a player sits -- never on a dedicated server. Anywhere else, "
            .. "`RunContext` says."
            .. '\n\n**In the editor, "when the world does" means when you press play** (ADR 0058). Opening a '
            .. "project mounts every script -- they are in the tree, `Source` is editable, a tab can open one -- and "
            .. "starts none of them, so a project shows what its scene holds and nothing a file scope built. Stop "
            .. "tears the VM down, which is what makes a second play the same as the first. Every other way of "
            .. "running this engine starts scripts at boot.",
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
                Doc = "**Whether this script's threads are resumed.** It decides nothing else "
                    .. "(ADR 0059).\n\nFalse at the moment scripts start means this one never "
                    .. "starts. Writing it "
                    .. "afterwards takes effect at the next deferred drain: **false to true STARTS it**, on its own "
                    .. "coroutine, file scope running now against the world as it is -- a start and not a resume, so "
                    .. "a script that ran, was disabled and is enabled again runs its file scope a second time. "
                    .. "**True to false stops resumption**, and that is all it does: a queued `task.defer`, a "
                    .. "`task.delay` coming due and a signal fire are all discarded when they come up, and a "
                    .. "thread already executing runs to its next yield, because a coroutine cannot be preempted."
                    .. "\n\n**Connections are NOT disconnected**, nothing it made is undone and nothing it "
                    .. "built is removed. They stop being invoked because nothing resumes the threads that would "
                    .. "run them; disconnect them yourself if that is what you need. Anything larger would answer "
                    .. "whether a connection dies with the thing its closure captured, which is a separate question "
                    .. "and not one a property setter may decide."
                    .. "\n\nSeveral scripts enabled by one tick start in document order.",
            },
            {
                Name = "RunContext",
                Type = "Enum.RunContext",
                ScriptReadOnly = true,
                Default = "Enum.RunContext.Shared",
                Doc = "**Where this script runs, outside the script services** (ADR 0138): `Server` on the "
                    .. "authority, `Client` where a player sits, `Shared` on both -- once per machine, so a solo "
                    .. "game runs a `Shared` script once. Inside `ServerScriptService`, `ClientScriptService` or "
                    .. "`GlobalScriptService`'s `Server` and `Client` folders **the service decides**, and moving a "
                    .. "script into one writes the service's side here: server code taken back out stays server "
                    .. "code.\n\nA package leaves out the code of every script whose side never runs there -- the "
                    .. "instance stays, with no `Source` -- so a stamp can carry a server half and a client half and "
                    .. "each machine gets its own.\n\n**The editor writes it, and a script only reads it**: the "
                    .. "side was fixed when the package was made.",
            },
        },
    },
    {
        Name = "ModuleScript",
        Extends = "BaseScript",
        Module = "scene",
        Doc = "Luau that is REQUIRED rather than run. It never starts on its own; `require(module)` evaluates it "
            .. "once and every later require of the same instance gives back the same value, which is what makes "
            .. "it the place shared code goes.",
    },
    {
        -- The structural half arrived at M2 and the physical half at M5, when
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
        Doc = "The abstract base of every solid object in the world: a transform, a size and a look.",
        Properties = {
            {
                Name = "CFrame",
                Type = "CFrame",
                Doc = "This part's transform in world space, and the f64 source of truth Position and Orientation are read back from.",
            },
            {
                Name = "Position",
                Type = "vector",
                Doc = "This part's world position, the f32 rounding of the CFrame translation; gameplay math past a few kilometres from the origin belongs on CFrame instead.",
            },
            {
                Name = "Orientation",
                Type = "vector",
                Doc = "This part's world rotation as intrinsic YXZ euler angles in degrees.",
            },
            {
                Name = "Size",
                Type = "vector",
                Doc = "This part's dimensions in metres along its own axes.",
            },
            {
                Name = "Material",
                Type = "Material?",
                Doc = "The material this part wears: a handle to a material asset (`Material.load`), a runtime "
                    .. "copy of one (`material:Clone()`), or nothing -- which is the engine default material, "
                    .. "white, dielectric and fairly rough, and exactly what a plain part has always looked "
                    .. "like.\n\n**A part has no colour of its own** (ADR 0090). Its look is its material's, "
                    .. "and what it may change about that is what the material lets it: the parameters the "
                    .. "material declares, set with `SetMaterialParameter`. The engine default declares `Color` "
                    .. "and `Transparency`, so a part wearing nothing is still tinted and faded.\n\n**Reading "
                    .. "it never copies.** It returns the handle the part wears; `material:Clone()` is how to get "
                    .. "one to change. In a scene file it is written as the asset's `Content`.\n\nA `MeshPart` "
                    .. "with no material draws what its own file described, and a `Color` it overrides tints "
                    .. "that -- which is what an unimported mesh has always looked like.",
            },
            {
                Name = "MaterialParameters",
                Type = "MaterialParameters",
                Doc = "The parameters this part overrides, by name -- a fresh table on every read, holding only "
                    .. "what is overridden. The names are the closed set a material may declare: `Color`, "
                    .. "`Transparency`, `Emissive`, `Metalness`, `Roughness`, `NormalScale` and `AlphaCutoff`."
                    .. "\n\n**An override the current material does not declare is kept and ignored**, so a "
                    .. "part that changes material and changes back looks as it did. `SetMaterialParameter` is "
                    .. "the checked way to write one; assigning this property replaces the whole set and checks "
                    .. "only that every entry is a parameter with a value of its kind.",
            },
            {
                Name = "Anchored",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the simulation moves this part. An anchored part is immovable scenery that other "
                    .. "parts collide with and gravity ignores -- not a very heavy part, a different kind of "
                    .. "body -- so writing this rebuilds it in the simulation and keeps its transform.",
            },
            {
                Name = "CanCollide",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this part physically stops anything. A part with this off still reports Touched, "
                    .. "which is what makes a checkpoint, a trigger volume or a pickup work: things pass through "
                    .. "it and it notices.",
            },
            {
                Name = "CanTouch",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this part takes part in touch reporting at all. **It gates the PAIR**: a part with "
                    .. "this off is silent, and so is whatever touches it -- because a signal naming a part that "
                    .. "said not to report touches would be that part reporting one on somebody else's handler. "
                    .. "That is the difference from CanQuery, which is about one part being found and is therefore "
                    .. "one part's decision. Off is what a world full of scenery wants: contacts are still solved, "
                    .. "and nothing is queued for scripts that were never going to listen.",
            },
            {
                Name = "CanQuery",
                Type = "boolean",
                Default = "true",
                Doc = "Whether raycasts, shapecasts and box queries can see this part. Independent of CanCollide, "
                    .. "because a part that blocks movement and a part a camera ray should ignore are different "
                    .. "questions.",
            },
            {
                Name = "Buoyant",
                Type = "boolean",
                Default = "true",
                Doc = "Whether a `Water` holds it up (ADR 0118): the engine pushes an unanchored part in water "
                    .. "up by the water it displaces, at points over its shape, so a hull pitches and rolls, and "
                    .. "drags it by the water's `Viscosity`. Off, the water ignores it -- a game floating it its "
                    .. "own way, or a stone.",
            },
            {
                Name = "CollisionGroup",
                Type = "string",
                Default = '"Default"',
                Doc = "The named group deciding which other parts this one collides with, registered through "
                    .. "`PhysicsService:RegisterCollisionGroup`. An unregistered name is an error rather than a "
                    .. "silent fallback to Default: the failure mode of a typo here is a wall players walk "
                    .. "through, which is expensive to find and cheap to refuse.",
            },
            {
                Name = "Friction",
                Type = "number",
                Default = "0.3",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much this surface resists sliding, 0 frictionless and 1 very grippy. The value the "
                    .. "solver uses for a contact combines both surfaces' -- one slippery part is enough to make "
                    .. "a pair slide.",
            },
            {
                Name = "Restitution",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much of an impact this surface gives back, 0 a dead stop and 1 a perfect bounce. "
                    .. "Combined across a contact pair the same way Friction is.",
            },
            {
                Name = "Density",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "Mass per cubic metre. Mass is this times the volume the Size and Shape describe, which "
                    .. "is why `Mass` can be read and not written: the two cannot be set to contradict each other.",
            },
            {
                Name = "Mass",
                Type = "number",
                ReadOnly = true,
                Doc = "What the part weighs, in kilograms: `Density` times the volume of its shape, as the "
                    .. "simulation worked it out. An impulse is this times a change of speed.",
            },
            {
                Name = "AssemblyMass",
                Type = "number",
                ReadOnly = true,
                Doc = "The mass of this part and of everything rigidly joined to it by welds and "
                    .. "`FixedConstraint`s: what has to be moved to move it.",
            },
            {
                Name = "LinearDamping",
                Type = "number",
                Default = "0.05",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The share of its speed a part loses each second with nothing touching it. Zero keeps "
                    .. "moving for ever; one is thick air.",
            },
            {
                Name = "AngularDamping",
                Type = "number",
                Default = "0.05",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The same for its spin.",
            },
            {
                Name = "ContactDetails",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `Collided` fires for this part. Off by default, so a world of scenery pays "
                    .. "nothing for a signal nobody listens to.",
            },
            {
                Name = "LinearVelocity",
                Type = "vector",
                Doc = "How fast this part is moving, in metres per second, as of the last simulation tick. "
                    .. "Writing it sets the speed the next tick starts from: a ball served, a thing stopped "
                    .. "dead. To push rather than to set, `ApplyImpulse`.",
            },
            {
                Name = "AngularVelocity",
                Type = "vector",
                Doc = "How fast this part is spinning, in radians per second about each world axis, as of the "
                    .. "last simulation tick. Writable, as `LinearVelocity` is.",
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
                Doc = "Overrides one parameter of the material this part wears, for this part alone. **Raises "
                    .. "for a parameter the material does not declare**: a material decides what a part may "
                    .. "change about it, and a tint written to one that did not allow it would be a surface "
                    .. "that silently ignores its script. `Color` and `Emissive` take a `Color3`; the rest take "
                    .. "a number. A parameter of the material's surface shader, when the material lists it in "
                    .. "`instanceParameters`, takes a number, a boolean, a `Vector2`, a `vector` or a `Color3`, or "
                    .. "four numbers in a table -- not a texture, which would make the part a material of its "
                    .. "own. **A surface shader parameter is not replicated**: set it where the part is drawn.",
            },
            {
                Name = "GetMaterialParameter",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "(Color3 | number | Vector2 | vector | { number })?" },
                ThreadSafety = "Unsafe",
                Doc = "The value this part draws with for one parameter: its override when it has one the "
                    .. "material declares, and the material's own value otherwise. `nil` for a surface shader "
                    .. "parameter neither sets, which draws with the shader's own default.",
            },
            {
                Name = "ClearMaterialParameter",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes this part's override of one parameter, so it draws with the material's value "
                    .. "again. Clearing one that is not overridden does nothing.",
            },
            {
                Name = "SetNetworkOwner",
                Parameters = { { Name = "player", Type = "Player?" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Hands this part to one player's machine (ADR 0099), which then simulates it and sends "
                    .. "where it is: what that player pushes moves at once rather than a round trip later. "
                    .. "`nil` hands it back to the authority. Only the authority may call it, and only on a part "
                    .. "that is not anchored; a player who leaves gives back everything they owned. **What the "
                    .. "owner sends is trusted** -- a game that must not trust it checks it, or does not hand "
                    .. "the part over.",
            },
            {
                Name = "GetNetworkOwner",
                Parameters = {},
                Returns = { "Player?" },
                ThreadSafety = "ReadParallel",
                Doc = "The player whose machine simulates this part, or `nil` for the authority. A replica "
                    .. "knows only about itself: it answers its own `Player` for a part it owns, and `nil` "
                    .. "otherwise.",
            },
            {
                Name = "ApplyImpulse",
                Parameters = { { Name = "impulse", Type = "vector" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Adds an instantaneous change of momentum at the part's centre of mass, in kilogram-metres "
                    .. "per second. Applied at the next simulation tick and ignored by an anchored part, which "
                    .. "has no momentum to change.",
            },
            {
                Name = "GetRenderCFrame",
                Parameters = {},
                Returns = { "CFrame" },
                ThreadSafety = "ReadParallel",
                Doc = "**Where this is drawn this frame** (ADR 0136): between the last two ticks, as the picture "
                    .. "shows it -- in a render phase, which is a `RunService:BindToRenderStep` function, a "
                    .. "`PreRender` handler or an action at `Rate = Render`. Anywhere else -- a simulation phase, "
                    .. "a run with no window, a server -- it is `CFrame`, because what a tick reads must not depend on "
                    .. "when a frame happened. A camera that follows this reads it.",
            },
            {
                Name = "ApplyImpulseAtPosition",
                Parameters = { { Name = "impulse", Type = "vector" }, { Name = "position", Type = "vector" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "The same push, at a point in the world: off the centre it also turns the part, as a shove "
                    .. "at a boat's bow swings it round (ADR 0118). Applied at the next tick.",
            },
            {
                Name = "ApplyAngularImpulse",
                Parameters = { { Name = "impulse", Type = "vector" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "An instantaneous change of angular momentum, about the axis it points along, in "
                    .. "kilogram-square-metres per second: a twist with no push. Applied at the next tick.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Touched",
                Parameters = { { Name = "otherPart", Type = "BasePart" } },
                Doc = "Fires once when another part begins touching this one, with the part it touched. Deferred "
                    .. "like every signal here (ADR 0015), so it arrives at the next resumption point rather than "
                    .. "inside the solver. A pair that stays in contact fires once, not once per tick -- including "
                    .. "across the moment the simulation puts both parts to sleep.",
            },
            {
                Name = "Collided",
                Parameters = {
                    { Name = "otherPart", Type = "BasePart" },
                    { Name = "position", Type = "vector" },
                    { Name = "normal", Type = "vector" },
                    { Name = "speed", Type = "number" },
                },
                Doc = "Fires with `Touched`, for a part whose `ContactDetails` is on, and says what the contact "
                    .. "was: where in the world, the direction from this part into the other, and how fast the "
                    .. "two were closing along it when they met, in metres a second -- before the simulation "
                    .. "answered. How hard a blow was is that speed times what was moving.",
            },
            {
                Name = "TouchEnded",
                Parameters = { { Name = "otherPart", Type = "BasePart" } },
                Doc = "Fires when a pair that was touching stops. Destroying a part does NOT fire it for that "
                    .. "part's contacts: the instance is gone, and a signal on an instance nobody can reach is a "
                    .. "signal nobody can handle.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "Part",
        Extends = "BasePart",
        Module = "scene",
        Parents = InTheWorldOrAFrame,
        Doc = "The primitive solid: a part whose geometry is one of a small set of shapes rather than an imported mesh.",
        Properties = {
            {
                Name = "Shape",
                Type = "Enum.PartShape",
                Doc = "Which primitive solid this part is. It drives BOTH the geometry that is drawn and the "
                    .. "collider that is simulated, which is the point: a `Ball` that rolled like a box would be "
                    .. "the kind of divergence nobody can see in a screenshot.\n\n**`Size` is the full extent, "
                    .. "and two shapes read it slightly differently so that the mesh agrees with the collider.** "
                    .. "A `Ball` is a sphere of the LARGEST half-extent, so a non-uniform `Size` gives a ball "
                    .. "that sticks out of its own box rather than an ellipsoid you cannot collide with. A "
                    .. "`Cylinder` and a `Capsule` stand along Y, take their diameter from the larger of X and Z, "
                    .. "and take their full height from Y -- caps included. A capsule's caps are hemispheres the "
                    .. "collider never stretches, so `Size.y == 2 * Size.x` is where the drawn shape and the "
                    .. "simulated one agree exactly; away from it the drawn ends are slightly oval.\n\nA `Wedge` "
                    .. "is a ramp with its tall face towards -Z, so an unrotated one is walked UP as you walk "
                    .. "towards -Z. Its collider is deliberately the whole box (§2.2's physics notes).",
            },
        },
    },
    {
        Name = "MeshPart",
        Extends = "BasePart",
        Module = "render",
        Parents = InTheWorldOrAFrame,
        Storage = "MeshPart",
        Doc = "A part whose geometry is an imported mesh rather than a primitive solid. One file is one mesh: a "
            .. "model made of several pieces is several MeshParts, which is the shape a prefab assumes.",
        Properties = {
            {
                Name = "MeshContent",
                Type = "Content",
                ContentKind = "Mesh",
                Doc = "The glTF file this part renders. Setting it replaces the geometry; a file that fails to "
                    .. "import leaves the previous mesh in place and reports why, because a part that silently "
                    .. "becomes invisible is harder to diagnose than one that says it could not load.",
            },
            {
                Name = "CollisionFidelity",
                Type = "Enum.CollisionFidelity",
                Doc = "How closely the collision shape follows the rendered geometry.\n\n`Box` names a "
                    .. "shape rather than an accuracy and is honoured exactly: a caller who asked for the "
                    .. "bounding box gets it even when the mesh's geometry is loaded. Every other value asks "
                    .. "for the geometry, and what it gets is a CONVEX HULL -- a hull is what a rigid-body "
                    .. "solver can use directly, and a concave triangle mesh is a different shape class with "
                    .. "different rules (it cannot be dynamic) that nothing in v1 asks for.\n\nA mesh "
                    .. "whose points have not arrived yet collides as its bounding box, which is the state of "
                    .. "every `MeshPart` in the frame before its file finishes loading: a body with no shape "
                    .. "for one frame is a body that falls through the floor. So does a mesh too degenerate "
                    .. "to make a solid from -- three points are a triangle, not a hull.\n\nThe "
                    .. "property reads back what was written rather than what it resolved to, because a "
                    .. "value that silently became another value is worse than one that says what it did.",
            },
            {
                Name = "MeshSize",
                Type = "vector",
                Default = "vector.create(1, 1, 1)",
                Doc = "What the mesh measures at its authored size, in metres. `Size` divided by this is what "
                    .. "the part is scaled by, so setting `Size` to twice `MeshSize` draws the mesh at twice "
                    .. "its authored size and collides with a hull to match -- which is what makes `Size` mean "
                    .. "the same thing here as it does on a `Part`.\n\n**The import writes it, and it "
                    .. "is saved with the scene.** It is not read back off the file: the mesh's real bounds "
                    .. "are known only where something loaded it, and a scene has to describe the same world "
                    .. "whether or not anything is rendering. A part whose `Size` and `MeshSize` are both one "
                    .. "draws exactly as it always has.\n\nWriting it yourself stretches the mesh, "
                    .. "which is a legitimate thing to want; zero and negative components are refused, since "
                    .. "they are a division rather than a size.",
            },
        } :: { Schema.Property },
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
        Doc = "An attachment that follows a joint of the `MeshPart` it is parented to. Weld a sword to a "
            .. "character's hand, hang a light off a lamp's swinging arm, or read where a foot is for a "
            .. "footstep -- and it moves with the animation, because it IS the joint.\n\n"
            .. "**Created on demand rather than one per joint.** A rig has tens of bones and a crowd has "
            .. "thousands; instantiating every one would put them all in the world hash and the change queue "
            .. "to answer a question about three of them. Make the ones you need.\n\nA "
            .. "bone whose `JointName` the rig does not have follows the mesh part itself, which puts it on "
            .. "the character rather than at the world origin -- the failure a renamed joint should produce "
            .. "is a sword in the wrong place, not a sword in another country.",
        Properties = {
            {
                Name = "JointName",
                Type = "string",
                Doc = "The joint in the parent `MeshPart`'s skeleton this follows, as the file names it.",
            },
            {
                Name = "JointIndex",
                Type = "number",
                ReadOnly = true,
                Default = "-1",
                Doc = "Where that name resolved to, or -1 for a name the rig does not have. Re-resolved when "
                    .. "the mesh reloads, so an index survives a file changing under it.",
            },
            {
                Name = "Transform",
                Type = "CFrame",
                Doc = "An offset applied on top of the animated pose, in the joint's own space.\n\n"
                    .. "This is the writable half of procedural animation: bend an arm towards a target, turn "
                    .. "a head to look at something, kick a gun up on a shot. It composes with whatever the "
                    .. "clip is doing rather than replacing it, and the identity is a bone nobody drives.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Attachment",
        Extends = "Instance",
        Module = "scene",
        Parents = under("BasePart"),
        Storage = "Attachment",
        Doc = "A named place on a part: a socket to weld something to, a joint frame for a constraint, a "
            .. "muzzle to spawn something at. It has no body and is not simulated -- what it costs is one "
            .. "transform per tick, and a world with none pays nothing.\n\nIt is not on "
            .. "`PVInstance`, deliberately: an attachment is a thing you make and name, and giving every part "
            .. 'an invisible one would make "where is this attached" ambiguous in exactly the cases it '
            .. "needs to be clear.",
        Properties = {
            {
                Name = "CFrame",
                Type = "CFrame",
                Doc = "Where this sits relative to the part it is parented to.",
            },
            {
                Name = "WorldCFrame",
                Type = "CFrame",
                ReadOnly = true,
                Doc = "Where that lands in the world, recomputed each tick.\n\nRead-only "
                    .. "because a world transform that could be assigned would be a second source of truth "
                    .. "about the same place: the part moves, and whatever was written here would be either "
                    .. "overwritten or wrong.",
            },
        },
        Methods = {
            {
                Name = "GetRenderCFrame",
                Parameters = {},
                Returns = { "CFrame" },
                ThreadSafety = "ReadParallel",
                Doc = "**Where this is drawn this frame** (ADR 0136): between the last two ticks, as the picture "
                    .. "shows it -- in a render phase, which is a `RunService:BindToRenderStep` function, a "
                    .. "`PreRender` handler or an action at `Rate = Render`. Anywhere else -- a simulation phase, "
                    .. "a run with no window, a server -- it is `WorldCFrame`, because what a tick reads must not depend on "
                    .. "when a frame happened. A camera that follows this reads it.",
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
        Doc = "The base of anything the SOLVER holds together. A constraint hands both bodies to the solver "
            .. "and lets it work out where they end up -- a door swings under its own weight, a ragdoll "
            .. "falls.\n\n**This is not a `Weld`.** A weld DRIVES its second part from its "
            .. "first every tick and the solver is never asked, which is what keeps a sword on a hand "
            .. "whatever else is happening. A constraint asks. Use a weld to attach, a constraint to "
            .. "articulate.\n\n**It joins two `Attachment`s, not two parts**, which is "
            .. "what makes the joint frame something you can see and move: where a door hinges is a place "
            .. "on the door and a place on the frame. A constraint cannot reach a `CharacterBody` -- that is "
            .. "swept rather than solved, so there is no body for a joint to hold.",
        Properties = {
            {
                Name = "Attachment0",
                Type = "Attachment?",
                Doc = "One end of the joint. The part it is on is the body the solver holds.",
            },
            {
                Name = "Attachment1",
                Type = "Attachment?",
                Doc = "The other end. The two frames are the same place when the constraint is built, which "
                    .. 'is what "this is where they are attached" means.',
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the joint holds. A disabled constraint stays in the world holding nothing, "
                    .. "rather than being destroyed and rebuilt -- rebuilding would move it to the end of the "
                    .. "solve order, and a ragdoll that switched itself off and on would simulate differently "
                    .. "afterwards.",
            },
            {
                Name = "CollideConnected",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the two parts still collide with each other.\n\n**False is "
                    .. "what a ragdoll needs.** An upper arm and a lower arm overlap at the elbow by "
                    .. "construction, and left colliding they shove each other apart every step -- a "
                    .. "character that vibrates rather than falls.",
            },
            {
                Name = "Visible",
                Type = "boolean",
                Default = "false",
                Doc = "Whether it is drawn. In the editor and under the debug overlay every constraint draws what "
                    .. "it is -- an arrow, a rail, a line. In a game only a rope, a rod and a spring are drawn, as a "
                    .. "thin line of their `Color` and `Thickness`.",
            },
            {
                Name = "BreakForce",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The force, in newtons, past which the joint gives: it is disabled, and `Broken` fires. Zero "
                    .. "never breaks.",
            },
            {
                Name = "BreakTorque",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The same for the turning it resists, in newton-metres. Zero never breaks.",
            },
        },
        Methods = {
            {
                Name = "GetForce",
                Parameters = {},
                Returns = { "number" },
                Doc = "The force the joint carried over the last simulation tick to hold its two parts "
                    .. "together, in newtons: what a `BreakForce` is a threshold on. Zero for a mover -- what it does is its "
                    .. "motor's, and `GetMotorForce` says it -- and for a joint that held nothing.",
            },
            {
                Name = "GetTorque",
                Parameters = {},
                Returns = { "number" },
                Doc = "The same for the turning it resisted, in newton-metres. Zero for a joint with nothing to "
                    .. "resist a turn with -- a ball joint, a rope.",
            },
            {
                Name = "GetMotorForce",
                Parameters = {},
                Returns = { "number" },
                Doc = "The force its own motor used over the last simulation tick, in newtons: a "
                    .. "`PrismaticConstraint`'s actuator, an `AlignPosition`, a `LinearVelocity`. Apart from "
                    .. "`GetForce` on purpose -- that is what the joint bore, which is what breaks it; this is how "
                    .. "hard it worked. Zero for anything with no motor along a line.",
            },
            {
                Name = "GetMotorTorque",
                Parameters = {},
                Returns = { "number" },
                Doc = "The torque its own motor used over the last simulation tick, in newton-metres: a "
                    .. "`HingeConstraint`'s or a `BallSocketConstraint`'s actuator, an `AlignOrientation`, an "
                    .. "`AngularVelocity`. A servo holding a weight level reads the weight times its arm here, and "
                    .. "at its `ServoMaxTorque` it is doing all it can. Zero for anything with no motor about an axis.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Broken",
                Parameters = {},
                Doc = "Fires when the joint carried more than its `BreakForce` or `BreakTorque` and gave. It "
                    .. "is disabled, not destroyed: enabling it again mends it.",
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
        Doc = "A joint free to rotate in every direction about one point, and optionally limited to a cone "
            .. "and a twist.\n\nUnlimited it is a rope's end or a pendulum. Limited it is a "
            .. "shoulder or a hip, which is exactly what a ragdoll is made of -- and the limited form is a "
            .. "different solver joint rather than a free one with corrections bolted on, so it is not "
            .. "exceeded and then pulled back, it is not exceeded.",
        Properties = {
            {
                Name = "LimitsEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the cone and twist below apply. Off is a free ball joint.",
            },
            {
                Name = "UpperAngle",
                Type = "number",
                Default = "180",
                Doc = "The half-angle of the swing cone in degrees, measured from the joint's own X axis.",
            },
            {
                Name = "TwistLimit",
                Type = "number",
                Default = "180",
                Doc = "How far it may twist about that axis, in degrees, either way.",
            },
            {
                Name = "ActuatorType",
                Type = "Enum.ActuatorType",
                Default = "Enum.ActuatorType.None",
                Doc = "What the joint does of itself: nothing, or hold a pose (`Servo`). A ball joint has no axis "
                    .. "to spin about, so `Motor` is read as `None`.",
            },
            {
                Name = "TargetOrientation",
                Type = "CFrame",
                Default = "CFrame.identity",
                Doc = "Servo: the orientation of `Attachment1` in `Attachment0`'s frame that the joint pulls to. "
                    .. "Only the rotation is read. The identity is the pose the two were joined in. With the joint's "
                    .. "X along a limb, a turn about X twists the limb and a turn about Y or Z swings it: about Y by "
                    .. "a positive angle swings its far end towards the frame's -Z, about Z towards its +Y.",
            },
            {
                Name = "ServoMaxTorque",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Servo: the most torque it may use, in newton-metres, about each of the joint's axes. A "
                    .. "shoulder that holds an arm out, and gives when the arm is pulled harder than this.",
            },
            {
                Name = "AngularResponsiveness",
                Type = "number",
                Default = "45",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "Servo: how eagerly it pulls when `Stiffness` is zero, as the natural frequency of the spring "
                    .. "it is, in radians a second. Critically damped, so it settles without ringing, and sized for "
                    .. "the two parts the joint holds -- about the socket, arm and all -- so the same number settles "
                    .. "a finger and a crane alike. What hangs further down a chain is not counted: a joint that "
                    .. "carries more than its own part takes a `Stiffness`.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The spring it pulls with, in newton-metres a radian: the torque for each radian it is away. "
                    .. "Zero leaves the pull to `AngularResponsiveness`; above zero this and `Damping` decide it, "
                    .. "and one number no longer has to say both how firm and how bouncy. For what it turns weighing "
                    .. "`I` kilogram metres squared about it, `w * w * I` settles at `w` radians a second.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "What resists its turning, in newton-metre-seconds a radian, read with `Stiffness`. Critical "
                    .. "damping -- it arrives without crossing -- is `2 * math.sqrt(Stiffness * I)` for an inertia "
                    .. "of `I` kilogram metres squared; less overshoots and rings, more arrives late and stays.",
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
        Doc = "A joint free to rotate about one axis: a door, a lid, an elbow. The axis is the joint frame's "
            .. "own X, so pointing the attachment points the hinge.",
        Properties = {
            {
                Name = "LimitsEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the range below applies. Off is a hinge that spins freely.",
            },
            {
                Name = "LowerAngle",
                Type = "number",
                Default = "-180",
                Doc = "How far it may turn one way, in degrees.",
            },
            {
                Name = "UpperAngle",
                Type = "number",
                Default = "180",
                Doc = "How far it may turn the other, in degrees. A lower above the upper is unlimited.",
            },
            {
                Name = "ActuatorType",
                Type = "Enum.ActuatorType",
                Default = "Enum.ActuatorType.None",
                Doc = "What the hinge does of itself: nothing, turn at a speed (`Motor`), or go to an angle and "
                    .. "hold it (`Servo`).",
            },
            {
                Name = "AngularVelocity",
                Type = "number",
                Default = "0",
                Doc = "Motor: the speed it turns at, in radians a second, about the hinge's own X. Negative turns "
                    .. "the other way.",
            },
            {
                Name = "MotorMaxTorque",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Motor: the most torque it may use, in newton-metres. A load heavier than this stalls it.",
            },
            {
                Name = "MotorMaxAcceleration",
                Type = "number",
                Default = "100000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Motor: how fast its speed may change, in radians a second squared. Low is a fan that winds "
                    .. "up.",
            },
            {
                Name = "TargetAngle",
                Type = "number",
                Default = "0",
                Doc = "Servo: the angle it goes to, in DEGREES, measured as the limits are. (Speeds are in radians "
                    .. "a second: `AngularVelocity`, `AngularSpeed`.)",
            },
            {
                Name = "AngularSpeed",
                Type = "number",
                Default = "100000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Servo: the fastest it turns on the way, in radians a second. Far from its angle it turns at "
                    .. "this speed; near, it is the spring.",
            },
            {
                Name = "ServoMaxTorque",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Servo: the most torque it may use to get there and to stay, in newton-metres. A load that "
                    .. "asks more sags to where its weight is this much, and -- a servo at its cap being a constant "
                    .. "torque -- swings about there until something else takes its speed away.",
            },
            {
                Name = "AngularResponsiveness",
                Type = "number",
                Default = "45",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "Servo: how eagerly it closes the last of the distance when `Stiffness` is zero, as the "
                    .. "natural frequency of the spring it is, in radians a second. Critically damped: it arrives "
                    .. "without crossing, sooner the higher this is. Sized for the two parts the hinge holds, about "
                    .. "the hinge -- an arm a metre long counts as a metre long -- and not for what hangs further "
                    .. "down a chain: a joint that carries more than its own part takes a `Stiffness`.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The spring it pulls with, in newton-metres a radian: the torque for each radian it is away. "
                    .. "Zero leaves the pull to `AngularResponsiveness`; above zero this and `Damping` decide it, "
                    .. "and one number no longer has to say both how firm and how bouncy. For what it turns weighing "
                    .. "`I` kilogram metres squared about it, `w * w * I` settles at `w` radians a second.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "What resists its turning, in newton-metre-seconds a radian, read with `Stiffness`. Critical "
                    .. "damping -- it arrives without crossing -- is `2 * math.sqrt(Stiffness * I)` for an inertia "
                    .. "of `I` kilogram metres squared; less overshoots and rings, more arrives late and stays.",
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
        Doc = "A joint with no freedom at all: two bodies the solver treats as one rigid assembly.\n\n"
            .. "**This is the one a `Weld` is not.** A weld moves its second part and asks nothing; this "
            .. "hands both to the solver, so a heavy crate fixed to a light one makes a heavy pair and a "
            .. "force applied to either moves both. Use a weld to attach something to a character, and this "
            .. "to build an object out of parts.",
        Properties = {},
    },
    {
        Name = "PrismaticConstraint",
        Extends = "Constraint",
        Module = "scene",
        Storage = "PrismaticConstraint",
        Doc = "A joint free to slide along one line and to do nothing else: a lift, a drawer, a piston. The "
            .. "line is the joint frame's own X, so pointing the attachment points the rail.\n\nWith an "
            .. "actuator it moves by itself: a `Motor` holds a speed, a `Servo` goes to a place along the "
            .. "rail and stays.",
        Properties = {
            {
                Name = "LimitsEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the range below applies. Off, it slides as far as it is pushed.",
            },
            {
                Name = "LowerLimit",
                Type = "number",
                Default = "0",
                Doc = "How far it may slide one way, in metres from where it was joined.",
            },
            {
                Name = "UpperLimit",
                Type = "number",
                Default = "5",
                Doc = "How far it may slide the other. A lower above the upper is unlimited.",
            },
            {
                Name = "ActuatorType",
                Type = "Enum.ActuatorType",
                Default = "Enum.ActuatorType.None",
                Doc = "What the rail does of itself: nothing, move at a speed (`Motor`), or go to a position and "
                    .. "hold it (`Servo`).",
            },
            {
                Name = "Velocity",
                Type = "number",
                Default = "0",
                Doc = "Motor: the speed it moves at, in metres a second along the rail.",
            },
            {
                Name = "MotorMaxForce",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Motor: the most force it may use, in newtons.",
            },
            {
                Name = "MotorMaxAcceleration",
                Type = "number",
                Default = "100000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Motor: how fast its speed may change, in metres a second squared.",
            },
            {
                Name = "TargetPosition",
                Type = "number",
                Default = "0",
                Doc = "Servo: where it goes to, in metres along the rail from where it was joined.",
            },
            {
                Name = "Speed",
                Type = "number",
                Default = "100000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Servo: the fastest it moves on the way, in metres a second. Far from its place it moves at "
                    .. "this speed; near, it is the spring.",
            },
            {
                Name = "ServoMaxForce",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Servo: the most force it may use to get there and to stay, in newtons. A lift that cannot "
                    .. "raise more than this does not.",
            },
            {
                Name = "LinearResponsiveness",
                Type = "number",
                Default = "45",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "Servo: how eagerly it closes the last of the distance when `Stiffness` is zero, as the "
                    .. "natural frequency of the spring it is, in radians a second. Critically damped, and sized for "
                    .. "the two parts on the rail.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The spring it pulls with, in newtons a metre: the force for each metre it is away. Zero "
                    .. "leaves the pull to `LinearResponsiveness`; above zero this and `Damping` decide it, and one "
                    .. "number no longer has to say both how firm and how bouncy. For what it moves weighing `m` "
                    .. "kilograms, `w * w * m` settles at `w` radians a second.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "What resists its moving, in newton-seconds a metre, read with `Stiffness`. Critical damping "
                    .. "-- it arrives without crossing -- is `2 * math.sqrt(Stiffness * m)` for a mass of `m` "
                    .. "kilograms; less overshoots and rings, more arrives late and stays.",
            },
        },
    },
    {
        Name = "RopeConstraint",
        Extends = "Constraint",
        Module = "scene",
        Storage = "RopeConstraint",
        Doc = "Two attachments that can be no further apart than `Length`, and as close as they like: a "
            .. "rope, a chain, a leash. Slack, it does nothing.\n\nWith the winch on, `Length` itself moves "
            .. "towards `WinchTarget` at `WinchSpeed` -- a crane taking in cable -- and stalls when that "
            .. "would need more than `WinchForce`.",
        Properties = {
            {
                Name = "Length",
                Type = "number",
                Default = "5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The most the two ends may be apart, in metres. The winch changes it.",
            },
            {
                Name = "WinchEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the winch is working `Length` towards `WinchTarget`.",
            },
            {
                Name = "WinchTarget",
                Type = "number",
                Default = "5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The length the winch works towards, in metres.",
            },
            {
                Name = "WinchSpeed",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How fast the winch takes rope in or lets it out, in metres a second.",
            },
            {
                Name = "WinchForce",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The most tension the winch can pull against, in newtons. A load heavier than this stays "
                    .. "where it is.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(0.6, 0.6, 0.6)",
                Doc = "The colour of the line it is drawn as.",
            },
            {
                Name = "Thickness",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How thick that line is, in metres.",
            },
        },
    },
    {
        Name = "RodConstraint",
        Extends = "Constraint",
        Module = "scene",
        Storage = "RodConstraint",
        Doc = "Two attachments held exactly `Length` apart, free to turn about either end: a rod, a strut, "
            .. "the link of a mechanism.",
        Properties = {
            {
                Name = "Length",
                Type = "number",
                Default = "5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How far apart the two ends are held, in metres.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(0.6, 0.6, 0.6)",
                Doc = "The colour of the line it is drawn as.",
            },
            {
                Name = "Thickness",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How thick that line is, in metres.",
            },
        },
    },
    {
        Name = "SpringConstraint",
        Extends = "Constraint",
        Module = "scene",
        Storage = "SpringConstraint",
        Doc = "A spring between two attachments: it pulls them together when they are further apart than "
            .. "`FreeLength` and pushes them apart when they are closer, harder the further they are from "
            .. "it. A suspension, a bouncy bridge, a door that closes by itself.",
        Properties = {
            {
                Name = "FreeLength",
                Type = "number",
                Default = "5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The length at which it neither pulls nor pushes, in metres.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "100",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The force for each metre it is away from `FreeLength`, in newtons a metre.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The force against each metre a second its length is changing, in newton-seconds a metre. "
                    .. "None and it bounces for ever; a lot and it creeps.",
            },
            {
                Name = "LimitsEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the two lengths below are hard stops.",
            },
            {
                Name = "MinLength",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The shortest it can be, in metres.",
            },
            {
                Name = "MaxLength",
                Type = "number",
                Default = "5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The longest it can be, in metres.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(0.6, 0.6, 0.6)",
                Doc = "The colour of the line it is drawn as.",
            },
            {
                Name = "Thickness",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How thick that line is, in metres.",
            },
        },
    },
    {
        Name = "LinearVelocity",
        Extends = "Constraint",
        Module = "scene",
        Storage = "LinearVelocity",
        Doc = "Holds the part its `Attachment0` is on at a velocity, with as much force as `MaxForce` "
            .. "allows: a conveyor's crate, a platform that glides, a thrown thing that does not slow. In "
            .. "`Line` and `Plane` modes it holds only the speed along a line or across a plane and leaves "
            .. "the rest to the world -- a puck held at a speed along a rail still falls.",
        Properties = {
            {
                Name = "VelocityConstraintMode",
                Type = "Enum.VelocityConstraintMode",
                Default = "Enum.VelocityConstraintMode.Vector",
                Doc = "Whether it holds the whole velocity, or only the part of it along a line or in a plane.",
            },
            {
                Name = "VectorVelocity",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "Vector mode: the velocity held, in metres a second, in the frame `RelativeTo` names.",
            },
            {
                Name = "LineDirection",
                Type = "vector",
                Default = "vector.create(1, 0, 0)",
                Doc = "Line mode: the line. Its length does not matter.",
            },
            {
                Name = "LineVelocity",
                Type = "number",
                Default = "0",
                Doc = "Line mode: the speed held along the line, in metres a second.",
            },
            {
                Name = "PrimaryTangentAxis",
                Type = "vector",
                Default = "vector.create(1, 0, 0)",
                Doc = "Plane mode: one direction in the plane.",
            },
            {
                Name = "SecondaryTangentAxis",
                Type = "vector",
                Default = "vector.create(0, 0, 1)",
                Doc = "Plane mode: the other.",
            },
            {
                Name = "PlaneVelocity",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "Plane mode: the speed held along each of the two, in metres a second.",
            },
            {
                Name = "MaxForce",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The most force it may use, in newtons, along each axis it holds a speed on: the line, the "
                    .. "plane's two, or the three of the frame the velocity is written in.",
            },
            {
                Name = "RelativeTo",
                Type = "Enum.ActuatorRelativeTo",
                Default = "Enum.ActuatorRelativeTo.World",
                Doc = "The frame the velocities are written in: the world's, or an attachment's -- so \"forwards\" "
                    .. "follows the part as it turns.",
            },
        },
    },
    {
        Name = "AngularVelocity",
        Extends = "Constraint",
        Module = "scene",
        Storage = "AngularVelocity",
        Doc = "Holds the part its `Attachment0` is on at a spin, with as much torque as `MaxTorque` allows: "
            .. "a fan, a coin that turns, a planet. It is a motor the simulation solves with the joints the "
            .. "part is in, so a hinged door it spins turns about its hinge. For a wheel on an axle a "
            .. "`HingeConstraint` with a motor is still the better tool.",
        Properties = {
            {
                Name = "AngularVelocity",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "The spin held, in radians a second about each axis of the frame `RelativeTo` names.",
            },
            {
                Name = "MaxTorque",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The most torque it may use, in newton-metres, about each axis of the part.",
            },
            {
                Name = "RelativeTo",
                Type = "Enum.ActuatorRelativeTo",
                Default = "Enum.ActuatorRelativeTo.World",
                Doc = "The frame the spin is written in.",
            },
            {
                Name = "ReactionTorqueEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the part `Attachment1` is on is turned the other way by as much: a motor that twists "
                    .. "what it is mounted on. On, the spin held is the one part's against the other's.",
            },
        },
    },
    {
        Name = "AlignPosition",
        Extends = "Constraint",
        Module = "scene",
        Storage = "AlignPosition",
        Doc = "Pulls `Attachment0` towards a place and holds it there: a pet that hovers at a shoulder, a "
            .. "platform that returns to its post, something carried. The place is `Position`, or "
            .. "`Attachment1` when the mode is `TwoAttachment`.\n\nIt does not carry the part there at a "
            .. "speed: it pulls, as hard as `MaxForce` allows and no faster than `MaxVelocity`, and a weight "
            .. "heavier than the force sags.",
        Properties = {
            {
                Name = "Mode",
                Type = "Enum.PositionAlignmentMode",
                Default = "Enum.PositionAlignmentMode.OneAttachment",
                Doc = "Whether the target is `Position` or `Attachment1`.",
            },
            {
                Name = "Position",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "OneAttachment: the place in the world it pulls to.",
            },
            {
                Name = "MaxForce",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The most force it may use, in newtons, along each axis of the world (or of `Attachment1`, "
                    .. "with `ReactionForceEnabled`).",
            },
            {
                Name = "MaxVelocity",
                Type = "number",
                Default = "100000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The fastest it may bring the attachment in, in metres a second, when `Stiffness` is zero. "
                    .. "Far from its place it travels at this speed; near, it is the spring.",
            },
            {
                Name = "Responsiveness",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "How eagerly it closes the distance when `Stiffness` is zero, as the natural frequency of the "
                    .. "spring it is, in radians a second: low drifts in, high snaps. Critically damped, and sized "
                    .. "for the one part it pulls: a part with others jointed to it takes a `Stiffness` sized for "
                    .. "all of them.",
            },
            {
                Name = "RigidityEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether it gets there as fast as the simulation can take it, with no cap on force or speed: "
                    .. "a spring as stiff as a tick can show.",
            },
            {
                Name = "ApplyAtCenterOfMass",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the pull is at where the part balances rather than at the attachment. Off, a pull at "
                    .. "a corner also turns the part.",
            },
            {
                Name = "ReactionForceEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "TwoAttachment: whether the part `Attachment1` is on is pulled back by as much. Off, the "
                    .. "target is not disturbed by what follows it; on, a hand pulling a crate is pulled by the "
                    .. "crate.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The spring it pulls with, in newtons a metre: the force for each metre it is away. Zero "
                    .. "leaves the pull to `Responsiveness`; above zero this and `Damping` decide it, and one number "
                    .. "no longer has to say both how firm and how bouncy. For what it moves weighing `m` kilograms, "
                    .. "`w * w * m` settles at `w` radians a second.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "What resists its moving, in newton-seconds a metre, read with `Stiffness`. Critical damping "
                    .. "-- it arrives without crossing -- is `2 * math.sqrt(Stiffness * m)` for a mass of `m` "
                    .. "kilograms; less overshoots and rings, more arrives late and stays.",
            },
        },
    },
    {
        Name = "AlignOrientation",
        Extends = "Constraint",
        Module = "scene",
        Storage = "AlignOrientation",
        Doc = "Turns `Attachment0` towards an orientation and holds it there: something that stays upright, "
            .. "a turret that faces its target, a pet that looks where its owner looks. The orientation is "
            .. "`CFrame`'s, or `Attachment1`'s when the mode is `TwoAttachment`.\n\nIt does not spin a part: "
            .. "for that, an `AngularVelocity` or a hinge with a motor.",
        Properties = {
            {
                Name = "Mode",
                Type = "Enum.OrientationAlignmentMode",
                Default = "Enum.OrientationAlignmentMode.OneAttachment",
                Doc = "Whether the target is `CFrame` or `Attachment1`.",
            },
            {
                Name = "CFrame",
                Type = "CFrame",
                Default = "CFrame.identity",
                Doc = "OneAttachment: the orientation in the world it turns to. Only the rotation is read.",
            },
            {
                Name = "MaxTorque",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The most torque it may use, in newton-metres, about each axis of the part.",
            },
            {
                Name = "MaxAngularVelocity",
                Type = "number",
                Default = "100000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The fastest it may turn the part, in radians a second, when `Stiffness` is zero. Far from "
                    .. "its facing it turns at this speed; near, it is the spring.",
            },
            {
                Name = "Responsiveness",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "How eagerly it closes the angle when `Stiffness` is zero, as the natural frequency of the "
                    .. "spring it is, in radians a second. Critically damped, and sized for the one part it turns "
                    .. "about where that part balances: a part with others jointed to it -- the hips of a body -- "
                    .. "takes a `Stiffness` sized for all of them.",
            },
            {
                Name = "RigidityEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether it gets there as fast as the simulation can take it, with no cap on torque or speed: "
                    .. "a spring as stiff as a tick can show.",
            },
            {
                Name = "ReactionTorqueEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "TwoAttachment: whether the part `Attachment1` is on is turned back by as much. An arm "
                    .. "aligned to a chest then turns the chest, as a real one does.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The spring it pulls with, in newton-metres a radian: the torque for each radian it is away. "
                    .. "Zero leaves the pull to `Responsiveness`; above zero this and `Damping` decide it, and one "
                    .. "number no longer has to say both how firm and how bouncy. For what it turns weighing `I` "
                    .. "kilogram metres squared about it, `w * w * I` settles at `w` radians a second.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "What resists its turning, in newton-metre-seconds a radian, read with `Stiffness`. Critical "
                    .. "damping -- it arrives without crossing -- is `2 * math.sqrt(Stiffness * I)` for an inertia "
                    .. "of `I` kilogram metres squared; less overshoots and rings, more arrives late and stays.",
            },
        },
    },
    {
        Name = "VectorForce",
        Extends = "Constraint",
        Module = "scene",
        Storage = "VectorForce",
        Doc = "A constant force on the part its `Attachment0` is on: a thruster, wind on one thing, lift.",
        Properties = {
            {
                Name = "Force",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "The force, in newtons, in the frame `RelativeTo` names.",
            },
            {
                Name = "RelativeTo",
                Type = "Enum.ActuatorRelativeTo",
                Default = "Enum.ActuatorRelativeTo.Attachment0",
                Doc = "The frame the force is written in. `Attachment0`, the default, is a thruster: it pushes "
                    .. "where the part points.",
            },
            {
                Name = "ApplyAtCenterOfMass",
                Type = "boolean",
                Default = "false",
                Doc = "Whether it pushes at where the part balances rather than at the attachment. Off, a force "
                    .. "off-centre also turns the part.",
            },
        },
    },
    {
        Name = "Torque",
        Extends = "Constraint",
        Module = "scene",
        Storage = "Torque",
        Doc = "A constant torque on the part its `Attachment0` is on: something that keeps being twisted.",
        Properties = {
            {
                Name = "Torque",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "The torque, in newton-metres about each axis of the frame `RelativeTo` names.",
            },
            {
                Name = "RelativeTo",
                Type = "Enum.ActuatorRelativeTo",
                Default = "Enum.ActuatorRelativeTo.Attachment0",
                Doc = "The frame the torque is written in.",
            },
        },
    },
    {
        Name = "NoCollisionConstraint",
        Extends = "Instance",
        Module = "scene",
        Parents = InTheWorld,
        Storage = "NoCollisionConstraint",
        Doc = "Two parts that pass through each other, and collide with everything else as they did. What "
            .. "a collision group cannot say: a group is every part in it against every part in another, and "
            .. "this is one part against one. A thing being carried and the one carrying it; a door and its "
            .. "frame.",
        Properties = {
            {
                Name = "Part0",
                Type = "BasePart?",
                Doc = "One of the two.",
            },
            {
                Name = "Part1",
                Type = "BasePart?",
                Doc = "The other.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the two pass through each other. Off, they collide again.",
            },
        },
    },
    {
        Name = "Ragdoll",
        Extends = "Instance",
        Module = "scene",
        Parents = under("MeshPart"),
        Storage = "Ragdoll",
        Doc = "Makes a character's pose come from the simulation instead of from a clip. Parent it to the "
            .. "`MeshPart` whose skeleton, put the limbs under it, and switch it on.\n\n"
            .. "**It owns nothing.** A ragdoll is parts, `Bone`s and constraints -- every one of them an "
            .. "instance you can see, select and move -- and this is the flag that says to drive the pose "
            .. "from them. A class that owned its own hidden bodies would be a second owner of things the "
            .. "physics mirror already creates from the tree, and two owners of one body is a rule "
            .. "broken.\n\nHow the pose is found: every `Bone` under this ragdoll that names "
            .. "a joint, sitting on a part. The part is where the simulation put that limb and the bone says "
            .. "which joint it is. Nothing else is declared, which is also why a partial ragdoll works -- "
            .. "simulate a dozen bones and the fingers ride along on the wrist, because the joints nobody "
            .. "drives keep their own place relative to their parent.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the pose comes from the simulation.\n\nSwitching it off hands "
                    .. "the character back to whatever clip is playing, from wherever it is standing -- the "
                    .. "overrides stop being written and the next tick's pose is the animation's again.",
            },
            {
                Name = "Blend",
                Type = "number",
                Default = "1",
                Doc = "How much of the pose comes from the simulation: 1 is all of it, 0 is none. Clamped.\n\n"
                    .. "**A POSE blend, not a solver one**, and the difference matters. This interpolates each "
                    .. "driven joint between where the clip put it and where the limb ended up, which is what a "
                    .. "stumble is: the character keeps running and sags. It does not make the simulation "
                    .. "weaker -- the parts fall exactly as hard either way -- so a limb at 0.5 is drawn "
                    .. "halfway to a body that is somewhere else entirely, and far from the pose it started in "
                    .. "that reads as rubber. Driving the SOLVER towards a pose instead is a motorised joint, "
                    .. "and it is a different feature.\n\nRamping this from 0 to 1 over a few tenths of a "
                    .. "second is how a character goes down without the frame it changes on being a visible "
                    .. "snap, and ramping it back is how one gets up.",
            },
        },
        Methods = {
            {
                Name = "Build",
                Parameters = { { Name = "profile", Type = "{ any }" } },
                Returns = { "number" },
                Doc = "Creates the limbs: one `Part` per joint the profile names, a `Bone` on each saying which "
                    .. "joint it is, and a constraint holding it to the limb above. Returns how many limbs it "
                    .. "made.\n\n**It creates ordinary instances and nothing else**, which is the whole shape of "
                    .. "this class: what `Build` saves you is the arithmetic -- where each limb goes, how long it "
                    .. "is, which way it points -- and everything it produces is a thing you can select, move, "
                    .. "retune and delete. Run it once in the editor and save the scene, and the ragdoll is part "
                    .. "of the prefab with no script involved.\n\nThe limbs are placed at the rig's CURRENT "
                    .. "pose, so a character that goes down mid-stride falls from where it was standing.\n\n"
                    .. 'A profile is an ordered list of limbs; `require("@engine/ragdoll").Humanoid` is a '
                    .. "built-in one that fits the common humanoid rigs. Each entry is a table:\n\n"
                    .. "- `Joint`: the joint's name, or a list of names tried in order -- the same shoulder is "
                    .. "`mixamorig:LeftArm` or `upper_arm.L` depending on who exported it.\n"
                    .. "- `Parent`: the 1-based index of the limb this hangs from, absent for the root. It must "
                    .. "point at a limb declared BEFORE this one.\n"
                    .. "- `Radius`, `LeafLength`: the capsule's radius, and the length to use for a limb with "
                    .. "nothing below it. A limb's length is otherwise the distance to the joint under it.\n"
                    .. '- `Kind`: `"BallSocket"`, `"Hinge"` or `"Fixed"`.\n'
                    .. "- `Swing`, `Twist`: a ball socket's cone half-angle and twist range, in degrees. Present "
                    .. "means it becomes a swing-twist, which is what a shoulder is.\n"
                    .. "- `LimitLow`, `LimitHigh`: a hinge's range in degrees.\n\n"
                    .. "**A limb the rig has no joint for is skipped, not refused**, and its children hang from "
                    .. "the nearest limb that was built -- so one humanoid profile works on a rig with no toes. "
                    .. "It errors when the ragdoll is not on a rigged MeshPart, when it already has limbs under "
                    .. "it, when the profile is malformed, or when the rig shares no joint with it at all.",
            },
        },
    },
    {
        Name = "Weld",
        Extends = "Instance",
        Module = "scene",
        Parents = InTheWorld,
        Storage = "Weld",
        Doc = "Holds one part at a fixed offset from another. **A transform weld, not a solver constraint**: "
            .. "Part1 stops being independently simulated and is driven from Part0 every tick, so welding a "
            .. "MeshPart to a CharacterBody keeps the two together without anything asking the solver's "
            .. "permission. Welding two dynamic parts so the SOLVER treats them as one rigid assembly is a "
            .. "different feature and is not this one. A weld whose two parts are already joined by another "
            .. "weld, directly or through a chain, is refused: welds form a graph and a cycle has no "
            .. "resolution order.",
        Properties = {
            {
                Name = "Part0",
                Type = "Instance?",
                Doc = "The anchor: the thing the other one follows. It may itself be simulated, anchored, or a "
                    .. "CharacterBody -- whatever moves it, Part1 goes with it.\n\n"
                    .. "**A `BasePart` or an `Attachment`.** Naming an attachment is what welds a sword to a "
                    .. "character's hand: the attachment is a `Bone`, the bone follows a joint, and the sword "
                    .. "follows the bone. Anything else is refused.",
            },
            {
                Name = "Part1",
                Type = "BasePart?",
                Doc = "The driven part. While the weld is active it is moved by the weld and by nothing else: "
                    .. "gravity does not reach it, and writing its CFrame is overwritten at the next tick.\n\n"
                    .. "**A part, never an attachment**, unlike `Part0`: a weld MOVES its driven end, and an "
                    .. "attachment has nothing of its own to move -- it is a place ON a part. Naming one here "
                    .. "would be a write that silently did nothing every tick.",
            },
            {
                Name = "C0",
                Type = "CFrame",
                Doc = "The attachment on Part0. The weld holds `Part0.CFrame * C0` equal to `Part1.CFrame * C1`, "
                    .. "which is the one sentence that says where both offsets go.",
            },
            {
                Name = "C1",
                Type = "CFrame",
                Doc = "The attachment on Part1, in the same relationship.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the weld holds. Disabling it hands Part1 back to the simulation where it stands, "
                    .. "with no velocity -- it is released rather than thrown.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Terrain",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Workspace"),
        Storage = "Terrain",
        Doc = "A sculpted, collidable landscape: ground you dig into rather than a floor made of parts.\n\n"
            .. "**It is a grid of voxels.** Space is cut into cubes `VoxelSize` on a side, and each one "
            .. "holds a material and an occupancy -- how full of that material it is, from 0 to 1. The "
            .. "surface is wherever the occupancy crosses one half, found by interpolating between "
            .. "neighbouring voxels, so a ball carved out of a hillside lands where you put it rather than "
            .. "on a grid line. Caves, arches, overhangs and flat ground are all the same data.\n\n"
            .. "Materials are numbered: 0 is air, and `n` is the terrain's layer `n` -- a material asset "
            .. "(`GetLayers`). A new terrain has none -- its materials are the project's own files -- and "
            .. "draws plain grey ground until `SetLayers` gives it some.\n\n"
            .. "There is one per `Workspace`, reached as `workspace.Terrain`, because a world has one "
            .. "ground. Creating a second is legal and it simply is not the one the workspace names.\n\n"
            .. "**Every verb here is a write to the voxels**, and the voxels are part of the world -- so a "
            .. "sculpt is undoable in the editor, it moves the world hash, and it saves with the project.",
        Properties = {
            {
                Name = "VoxelSize",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.terrain_not_empty",
                Doc = "How big one voxel is, in metres, from 0.1 to 64. One metre resolves a doorway and a "
                    .. "path; two metres resolves a hillside and costs an eighth as much.\n\n"
                    .. "**Only settable while the terrain is empty.** Changing it under a sculpted world "
                    .. "would mean resampling every voxel onto a different grid, which is a lossy operation "
                    .. "nobody asked for -- so it is refused, by name, rather than done quietly. Clear it "
                    .. "first if that is really what you want.",
            },
            {
                Name = "CellSize",
                Type = "number",
                ReadOnly = true,
                Default = "64",
                Inert = true,
                Doc = "How wide one streamed cell of terrain is, in metres. Read-only: it is the streaming "
                    .. "grid's own spacing, and a terrain that disagreed with it would have cells that "
                    .. "load and unload on different boundaries from everything else in the world.\n\n"
                    .. "**What a saved terrain streams by.** A terrain written with a scene and large "
                    .. "enough to span 256 cells or more is cut into cells of this size, rounded to "
                    .. "whole columns of 32-voxel chunks, and each one is loaded and dropped around "
                    .. "`StreamingService`'s foci on its terrain radii. A cell somebody changed is written "
                    .. "to the run's cache on disk when it is dropped, and comes back as it was changed.",
            },
            {
                Name = "Position",
                Type = "vector",
                Default = "vector.zero",
                Doc = "Where this terrain's own origin sits in the world, in metres.\n\n"
                    .. "**A terrain is an instance and it can be moved.** Every voxel, every collider and "
                    .. "every brush stroke is offset by this rather than baked into the grid, so moving a "
                    .. "sculpted world is one number rather than a rewrite of every voxel. It translates "
                    .. "and does not turn.",
            },
            {
                Name = "MinHeight",
                Type = "number",
                Default = "-256",
                Doc = "The world's floor, in metres: no voxel below it is ever written.\n\n"
                    .. "Ground laid where there was none -- by `WriteHeights`, Generate Flat Ground or a "
                    .. "first raise on empty terrain -- is a slab 32 metres deep under its lowest surface, "
                    .. "rounded down to a whole chunk, and never deeper than this.",
            },
            {
                Name = "MaxHeight",
                Type = "number",
                Default = "256",
                Doc = "The world's ceiling, in metres: no voxel above it is ever written.",
            },
            {
                Name = "CellCount",
                Type = "number",
                ReadOnly = true,
                Doc = "How many chunks of 32 by 32 by 32 voxels currently hold anything. Air is never "
                    .. "stored, so zero is a terrain nobody has sculpted, which is what `Clear` returns it to.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "FillBall",
                Parameters = {
                    { Name = "center", Type = "vector" },
                    { Name = "radius", Type = "number" },
                    { Name = "material", Type = "number" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Adds a ball of ground, or removes one when `material` is zero. Returns how many "
                    .. "voxels it changed.\n\n"
                    .. "Adding never takes ground away and removing never adds it: a voxel ends up as full "
                    .. "as the fuller of it and the ball (adding), or as empty as the ball leaves it "
                    .. "(removing). An explosion crater is `FillBall(hit.Position, 4, 0)`.\n\n"
                    .. "**A place or a size that is not a finite number is an error**, here and in every "
                    .. "Terrain edit and read, and so is a negative radius: zero is only ever ground that "
                    .. "already was what was asked for, or an edit wholly outside `MinHeight` to `MaxHeight`.",
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
                Doc = "The same, as an axis-aligned box `size` across. Returns how many voxels it changed.",
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
                Doc = "The same, as an upright cylinder `height` tall. Returns how many voxels it changed. "
                    .. "A well is `FillCylinder(top - vector.create(0, 10, 0), 20, 1.5, 0)`.",
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
                Doc = "Raises the ground around `center` by `amount` metres at the middle, falling smoothly "
                    .. "to nothing at `radius` -- or lowers it, when `amount` is negative. Returns how many "
                    .. "voxels it changed.\n\n"
                    .. "**It moves the surface rather than adding a ball**: the ground within `radius` "
                    .. "above and below `center` is pushed up, so a hill rises without an overhang at its "
                    .. "rim, and a tunnel further down stays where it is. This is the verb for shaping hills "
                    .. "and valleys; `FillBall` is the one for a boulder or a tunnel.\n\n"
                    .. "Where a column within reach has no ground at all and `material` is given, raising "
                    .. "lays new ground of it from `center`'s height up.",
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
                Doc = "Moves the surface near `center` outwards along its own slope by `amount` metres at the "
                    .. "middle, falling smoothly to nothing at `radius` -- or inwards, when `amount` is negative. "
                    .. "Returns how many voxels it changed.\n\n"
                    .. "The editor's Raise and Lower: a field rises, a cliff comes forward, an overhang grows "
                    .. "down, and ground worn away goes back the way it came. Where it grows new ground, it is "
                    .. "`material`, or the neighbouring ground's when that is not given.",
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
                Doc = "Softens the ground in a ball: each surface moves towards the average of the same "
                    .. "surface round it -- over a gaussian half the brush wide, at most four voxels -- by "
                    .. "`strength` (0 to 1, default 0.5), less towards the rim. Flat ground stays where it "
                    .. "is, a slab is never cut through, nothing sinks below the ground it smooths towards, "
                    .. "and what comes off a bump goes round it: the ground's volume is kept to within a "
                    .. "tenth over twenty stamps. Returns how many voxels it changed.",
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
                Doc = "Pulls the ground in a ball towards a level plane at `height` -- taking away what is "
                    .. "above it and filling what is below it -- by `strength` (0 to 1, default 1), less "
                    .. "towards the rim. Returns how many voxels it changed.",
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
                Doc = "Changes what the ground is MADE OF in a ball, without moving it. Returns how many "
                    .. "voxels it changed.\n\n"
                    .. "**A voxel holds two materials and how much the one over shows** (ADR 0114), and "
                    .. "`options.Mode` says what the stroke does with them: `Replace` (the default) makes the "
                    .. "ground `material` outright; `Blend` shows `material` more over what is there, by "
                    .. "`Strength` (0 to 1, default 1), fading out towards the rim over the share of the radius "
                    .. "`Falloff` says (0 a hard edge, 0.5 the outer half, 1 from the centre; default 0); `Under` "
                    .. "puts `material` under what was painted over; "
                    .. "`Erase` shows what was painted over less, revealing what is under.\n\n"
                    .. "A `material` of zero is refused rather than treated as erase, except by `Erase`, which "
                    .. "takes none. Zero means erase to `FillBall`, and picking the first entry of a material "
                    .. "list must not delete the ground you were about to paint.",
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
                Doc = "Every voxel of material `from` in the box between the two corners becomes `to`. "
                    .. "Returns how many it changed. Zero for either is refused: this changes what ground is "
                    .. "made of, never whether it is there.",
            },
            {
                Name = "ReadVoxels",
                Parameters = {
                    { Name = "minCorner", Type = "vector" },
                    { Name = "maxCorner", Type = "vector" },
                },
                Returns = { "{ number }", "{ number }", "vector" },
                ThreadSafety = "Unsafe",
                Doc = "Reads every voxel the box between the two corners touches, snapped outward to the "
                    .. "grid. Returns their materials, their occupancies (0 to 1) and how many there are on "
                    .. "each axis.\n\n"
                    .. "**The tables are flat**: the voxel at `x, y, z` (counting from 0 at the low corner) "
                    .. "is at index `1 + x + size.X * (y + size.Y * z)`. At most 256 cubed voxels at once; "
                    .. "read a larger region in pieces.",
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
                Doc = "Writes a box of voxels in one call, laid out as `ReadVoxels` returns them: `corner` "
                    .. "is a point in the first voxel, `size` how many voxels on each axis, and both tables "
                    .. "hold one entry per voxel. Returns how many it changed.\n\n"
                    .. "A material of zero, or an occupancy of zero, is air. What `ReadVoxels` reads, "
                    .. "`WriteVoxels` puts back exactly -- the verb for copying a piece of terrain, "
                    .. "generating one voxel by voxel, or undoing a script's own change.",
            },
            {
                Name = "WorldToCell",
                Parameters = {
                    { Name = "position", Type = "vector" },
                },
                Returns = { "vector" },
                ThreadSafety = "Unsafe",
                Doc = "Which voxel a world position falls in, as its whole-number index on each axis.",
            },
            {
                Name = "CellCenterToWorld",
                Parameters = {
                    { Name = "cell", Type = "vector" },
                },
                Returns = { "vector" },
                ThreadSafety = "Unsafe",
                Doc = "The world position of a voxel's centre, from its index on each axis.",
            },
            {
                Name = "HeightAt",
                Parameters = {
                    { Name = "x", Type = "number" },
                    { Name = "z", Type = "number" },
                },
                Returns = { "number?" },
                ThreadSafety = "Unsafe",
                Doc = "The height of the top of the ground here, in metres, or nil where there is none.\n\n"
                    .. "**The top, over caves as well**: the highest surface in the column, which is where "
                    .. "a tree is planted or a player lands from above. Cast a ray with `Workspace:Raycast` "
                    .. "when what you want is the first surface along a direction.\n\n**Nil too where the "
                    .. "ground has not streamed in yet**, which in a large world is everywhere at the first "
                    .. "line of a script: wait for it with `StreamingService:LoadAreaAsync(position, radius)`.",
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
                Doc = "Writes a heightmap in one call: one height per column of voxels, `columns` to a row, "
                    .. "row after row along +Z, starting at the column `corner` falls in (its X and Z; Y is "
                    .. "not read). Returns how many voxels it changed.\n\n"
                    .. "**This is the verb for ground that comes from somewhere** -- a generator's noise, an "
                    .. "image, another tool -- where a brush per column would be a quarter of a million "
                    .. "calls. Heights are in world metres and clamped between `MinHeight` and `MaxHeight`; "
                    .. "`material` (default 1) is what new ground is made of -- one id for all of it, or a "
                    .. "table of ids parallel to `heights`, where 0 leaves that column alone. Each column's top "
                    .. "moves to its "
                    .. "height -- ground added from the old top up, or taken from it down -- and a cave under "
                    .. "the top stays. A column with no ground at all is laid as a slab 32 metres deep under "
                    .. "the table's lowest height. A height that is not a number is skipped.",
            },
            {
                Name = "Clear",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes every voxel. The terrain is empty afterwards and `VoxelSize` becomes "
                    .. "settable again.",
            },
            {
                Name = "GetLayers",
                Parameters = {},
                Returns = { "{ string }" },
                ThreadSafety = "Safe",
                Doc = "What each material id is: a list of material URNs, where entry `n` is what voxels of "
                    .. "material `n` are drawn with. A new terrain has none; a scene saved before terrains "
                    .. "started empty reads the engine's eight, `engine://terrain/grass` to `engine://terrain/ice`.",
            },
            {
                Name = "SetLayers",
                Parameters = { { Name = "layers", Type = "{ string }" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets what each material id is, as `GetLayers` returns it: up to 255 material URNs, "
                    .. "entry `n` for material `n`. The voxels keep their numbers, so replacing entry 3 "
                    .. "repaints every rock at once; add a project material at the end to paint with it. "
                    .. "Saved with the scene.",
            },
            {
                Name = "GetRules",
                Parameters = {},
                Returns = {
                    "{ { Enabled: boolean, Material: number, SlopeMin: number, SlopeMax: number, HeightMin: number, HeightMax: number, Blend: number, Noise: number, AppliesTo: { number } } }",
                },
                ThreadSafety = "Safe",
                Doc = "The rules that paint the ground by slope and height, in the order they paint: each draws "
                    .. "`Material` where the ground is between `SlopeMin` and `SlopeMax` degrees and between "
                    .. "`HeightMin` and `HeightMax` metres, over the layers in `AppliesTo` (empty is every layer "
                    .. "but its own), with an edge `Blend` wide (degrees or metres) and `Noise` ragged. A new "
                    .. "terrain has none.\n\n"
                    .. "**Rules are drawn, not written**: the voxels keep their materials until `ApplyRules`. "
                    .. "What a raycast reports is what is drawn.",
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
                Doc = "Sets the rules, as `GetRules` returns them: up to 16, painted in order. A missing field "
                    .. "keeps its default -- `Enabled` true, the slopes 0 and 90, the heights open, `Blend` 10, "
                    .. "`Noise` 0.12. An empty list turns the automatic rock off. Saved with the scene.",
            },
            {
                Name = "ApplyRules",
                Parameters = {
                    { Name = "minCorner", Type = "vector" },
                    { Name = "maxCorner", Type = "vector" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Writes what the rules draw into the voxels at the surface between the two corners, so "
                    .. "the rules can be turned off afterwards with nothing changing on screen. Returns how "
                    .. "many voxels changed.",
            },
            {
                Name = "Compact",
                Parameters = {},
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Does nothing and returns 0.\n\n"
                    .. "It converted the old terrain's voxel columns back to heights. Every edit now leaves "
                    .. "the voxels as compact as they can be, so there is nothing left for it to do. It stays "
                    .. "so scripts that call it keep running.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "WeldConstraint",
        Extends = "Instance",
        Module = "scene",
        Parents = InTheWorld,
        Storage = "WeldConstraint",
        Doc = "The same joint, with the offset CAPTURED rather than authored. When it becomes active it "
            .. "records where the two parts are relative to each other and holds that; a Weld is told the "
            .. "relationship and a WeldConstraint reads it off the world. That difference is the whole reason "
            .. "both names exist: authoring an offset by hand for two parts already in the right place is "
            .. "arithmetic nobody should have to do, and capturing one when you meant to specify it is a "
            .. "joint that silently depends on where things happened to be.",
        Properties = {
            {
                Name = "Part0",
                Type = "BasePart?",
                Doc = "The anchor, as on Weld.",
            },
            {
                Name = "Part1",
                Type = "BasePart?",
                Doc = "The driven part, as on Weld.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the joint holds. Going from false to true captures the relative transform afresh, "
                    .. "which is how a part is re-welded somewhere else: move it, then enable.",
            },
            {
                Name = "Active",
                Type = "boolean",
                ReadOnly = true,
                Doc = "Whether it is currently holding: Enabled, with both parts set and both in the world. The "
                    .. "distinction matters because a constraint with one part missing is not an error -- it is "
                    .. "half-built, which is what every script that assigns the two properties on separate lines "
                    .. "briefly produces.",
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
        Doc = "A capsule that walks: the player, or anything that should climb a ramp and step over a kerb "
            .. "instead of tumbling. It is a controller rather than a rigid body -- it sweeps its own shape and "
            .. "moves at the velocity you give it, which is why it does not tip over and why gravity is yours to "
            .. "apply through Move. There is no Humanoid and no HumanoidRootPart: this is one instance (§2.7 "
            .. "divergence 13). "
            .. "Two characters BLOCK each other and neither pushes the other. Walking into somebody who is "
            .. "standing still stops you and leaves them where they were, however fast you were going and "
            .. "however long you keep walking -- shoving, knockback and crowd flow are game rules, and a game "
            .. "writes them by moving the other character itself. What holds the block is the rigid body inside "
            .. "the capsule, so CollisionGroup decides it like any other pair: two characters in a group set "
            .. "never to collide with itself walk straight through one another.",
        Properties = {
            {
                Name = "WalkSpeed",
                Type = "number",
                Default = "16",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How fast Move drives the character along the ground, in metres per second. The direction "
                    .. "passed to Move is scaled by this, so a half-length direction walks at half speed.",
            },
            {
                Name = "JumpSpeed",
                Type = "number",
                Default = "8",
                Doc = "The upward speed Jump imparts, in metres per second. How high that reaches depends on "
                    .. "`Workspace.Gravity`, which is the relationship a game tunes rather than a height. "
                    .. "Negative is a slam downwards.",
            },
            {
                Name = "GravityScale",
                Type = "number",
                Default = "1",
                Doc = "How much of `Workspace.Gravity` the character feels: 1 all of it, 0.5 a floaty jump, 0 "
                    .. "none. Negative falls upwards.",
            },
            {
                Name = "SwimSpeed",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How fast `Move` takes it through a fluid, in metres per second, as `WalkSpeed` does on "
                    .. "the ground.",
            },
            {
                Name = "FlySpeed",
                Type = "number",
                Default = "16",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How fast `Move` takes it while `Flying`, in metres per second.",
            },
            {
                Name = "Flying",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the character flies: no weight, and `Move` in three dimensions at `FlySpeed`. A "
                    .. "creative mode, a ghost, a jetpack held on.",
            },
            {
                Name = "MaxSlopeAngle",
                Type = "number",
                Default = "46",
                ErrorKey = "scene.err.number_slope_degrees",
                Doc = "The steepest ground, in degrees from horizontal, the character can stand and walk on. "
                    .. "Anything steeper is a wall it slides down rather than a floor it stands on.",
            },
            {
                Name = "AutoStepHeight",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The tallest ledge, in metres, the character walks over rather than into. This is the "
                    .. "single number that separates a character from a capsule: at zero, a kerb stops it.",
            },
            {
                Name = "Grounded",
                Type = "boolean",
                ReadOnly = true,
                Doc = "Whether the character is standing on something walkable as of the last simulation tick. "
                    .. "The same fact `State` reports, as the boolean a jump check actually wants.",
            },
            {
                Name = "FloorMaterial",
                Type = "Material?",
                ReadOnly = true,
                Doc = "What it is standing on: the material of the part under it, or the layer the terrain is "
                    .. "drawn as under its feet -- what `RaycastResult.Material` would say of a ray straight "
                    .. "down. Nil in the air, in water, and on a part wearing nothing. A footstep reads its "
                    .. "`FootstepSound`; ice and mud are its `Friction` or its `Tags`.",
            },
            {
                Name = "State",
                Type = "Enum.CharacterState",
                ReadOnly = true,
                Doc = "What the character is doing: `Grounded` or `Airborne` on foot -- ground too steep to "
                    .. "walk on reads as Airborne, because the question is whether it is supported -- `Swimming` "
                    .. "with its middle under a `Water`'s surface or inside a fluid block, which it enters and "
                    .. "leaves by itself, and `Flying` while `Flying` is set.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Move",
                Parameters = { { Name = "direction", Type = "vector" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets the direction to walk in for the next simulation tick, in world space. Only the "
                    .. "horizontal part is used -- vertical movement is gravity's and Jump's -- and the vector is "
                    .. "scaled by WalkSpeed rather than normalised, so a shorter one walks slower. Call it every "
                    .. "tick while moving; a character told nothing stops.\n\n"
                    .. "Swimming and flying, all three axes are used -- up is up -- at `SwimSpeed` and "
                    .. "`FlySpeed`.\n\n"
                    .. "A character is pushed as a part is: `ApplyImpulse` changes its speed by the impulse over "
                    .. "its `Mass`, and `LinearVelocity` can be written -- a knock back, a launch pad. What it was "
                    .. "pushed by fades, quickly on the ground and slowly in the air, and its walk is added on "
                    .. "top.",
            },
            {
                Name = "Jump",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Launches the character upward at JumpSpeed at the next simulation tick, wherever it is. "
                    .. "It does NOT check Grounded, deliberately: a mid-air check here would make a double "
                    .. "jump, a wall jump and a triple jump impossible to write as a jump at all. "
                    .. "`if character.Grounded then character:Jump() end` is the old behaviour in one "
                    .. "line, in the game, where a jump policy belongs -- and coyote time and jump buffering "
                    .. "become counters beside it. Calling it every frame is flying: that is the game's bug, "
                    .. "and it is the same in every engine that offers a mechanism rather than a policy. The "
                    .. "TICK stays engine-side: the velocity is applied at the next simulation step and never "
                    .. "inside the call, or a replay diverges.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Landed",
                Parameters = { { Name = "groundPart", Type = "BasePart?" } },
                Doc = "Fires when the character becomes Grounded after being Airborne, with the part it landed "
                    .. "on -- nil when that is terrain no instance owns. Deferred like every signal here.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "Camera",
        Extends = "PVInstance",
        Module = "render",
        Parents = under("Workspace", "ViewportFrame"),
        Storage = "Camera",
        Doc = "The viewpoint the world is rendered from. A camera is an ordinary instance a script owns and moves; "
            .. "the engine never takes it over, which is why there is no CameraType.",
        Properties = {
            {
                Name = "CFrame",
                Type = "CFrame",
                Doc = "Where the camera is and which way it looks. The look direction is the CFrame's LookVector, "
                    .. "which is -Z.\n\n"
                    .. "**Written in a render phase** -- a `RunService:BindToRenderStep` function, a `PreRender` "
                    .. "handler, an action at `Rate = Render` -- it is *presented*: drawn exactly as written this "
                    .. "frame, and it becomes the camera the simulation reads when the next tick starts. Written "
                    .. "in a simulation phase it is that camera at once, drawn between ticks. Read in a render "
                    .. "phase it answers what was presented; anywhere else, the simulation's (ADR 0136).",
            },
            {
                Name = "ViewportSize",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                -- The size of the window or panel drawn into: a fact about the
                -- host, like `UIService.DisplayScale`, kept out of the hash.
                HostFact = true,
                Doc = "The size of what the world is drawn into, in pixels: the window in a game, the "
                    .. "Viewport panel in the editor. What a script divides by to turn a screen position into a "
                    .. "fraction, or reads to know it is on a tall screen rather than a wide one -- it changes "
                    .. "when the window is resized or a phone is turned, and a layout that reads it every frame "
                    .. "follows.",
            },
            {
                Name = "FieldOfView",
                Type = "number",
                ErrorKey = "scene.err.number_cone_degrees",
                Doc = "The vertical field of view in degrees. Clamped to a sane open interval: zero and 180 both "
                    .. "produce a projection matrix that renders nothing.",
            },
            {
                Name = "NearPlane",
                Type = "number",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "Distance in metres to the near clip plane. Small values buy little and cost depth precision "
                    .. "everywhere, so this is worth raising rather than lowering. A perspective camera's only: an "
                    .. "orthographic one sees behind where it stands too (see `OrthographicSize`).",
            },
            {
                Name = "FarPlane",
                Type = "number",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "Distance in metres to the far clip plane; geometry beyond it is not drawn.",
            },
            {
                Name = "Projection",
                Type = "Enum.CameraProjection",
                Doc = "Perspective for a 3D view, Orthographic for a 2D one (the 2D layer, phase 3). An "
                    .. "orthographic camera ignores `FieldOfView` and shows `OrthographicSize` metres above and "
                    .. "below the middle of the view, whatever the distance.",
            },
            {
                Name = "OrthographicSize",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "Half the height of what an orthographic camera shows, in metres. Its width follows from "
                    .. "the window's shape. Zooming a 2D view is changing this.\n\n"
                    .. "**An orthographic camera sees its whole column**, from `FarPlane` behind where it stands to "
                    .. "`FarPlane` in front: nothing in its picture shrinks with distance, so a top-down camera "
                    .. "standing lower than the top of what it looks at still shows all of it.",
            },
            {
                Name = "ClipPlane",
                Type = "CFrame",
                Doc = "A plane the camera does not see behind (ADR 0107), used while `ClipPlaneEnabled` is "
                    .. "true: it passes through this CFrame's position, and what lies in front of its "
                    .. "`LookVector` is drawn while what lies behind is not. A mirror's camera, standing behind "
                    .. "the glass, sees the room and not the wall the mirror hangs on. `@engine/views` sets it for "
                    .. "you. The camera must be behind the plane; one in front of it draws as if there were none.",
            },
            {
                Name = "ClipPlaneEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `ClipPlane` is used.",
            },
        },
        Methods = {
            {
                Name = "GetRenderCFrame",
                Parameters = {},
                Returns = { "CFrame" },
                ThreadSafety = "ReadParallel",
                Doc = "**Where this is drawn this frame** (ADR 0136): between the last two ticks, as the picture "
                    .. "shows it -- in a render phase, which is a `RunService:BindToRenderStep` function, a "
                    .. "`PreRender` handler or an action at `Rate = Render`. Anywhere else -- a simulation phase, "
                    .. "a run with no window, a server -- it is the simulation's `CFrame`, because what a tick reads must not depend on "
                    .. "when a frame happened. A camera that follows this reads it.",
            },
            {
                Name = "WorldToViewportPoint",
                Parameters = { { Name = "position", Type = "vector" } },
                Returns = { "Vector2", "number", "boolean" },
                ThreadSafety = "ReadParallel",
                Doc = "Where a point in the world is in the picture: its place in pixels from the top-left of "
                    .. "what the world is drawn into -- the unit `UIObject.AbsolutePosition` and "
                    .. "`InputService:GetPointerPosition` use -- then how far in front of the camera it is, in "
                    .. "metres, then whether it is on the screen. The place is meaningful past the edges, so an "
                    .. "arrow at the border can point at it; with a depth of zero or less the point is beside "
                    .. "or behind a perspective camera and the place means nothing. For either projection. A "
                    .. "`ScreenGui` that places things by it wants `ScreenInsets = false`, or they are off by "
                    .. "the notch.",
            },
            {
                Name = "ViewportPointToRay",
                Parameters = { { Name = "point", Type = "Vector2" } },
                Returns = { "vector", "vector" },
                ThreadSafety = "ReadParallel",
                Doc = "The ray a pixel looks along, as an origin and a direction of length one: what to give "
                    .. "`Workspace:Raycast` to find what is under the pointer. From the camera through the "
                    .. "pixel for a perspective camera; straight ahead from the pixel's own place for an "
                    .. "orthographic one.",
            },
            {
                Name = "ViewportPointToWorld2D",
                Parameters = { { Name = "point", Type = "Vector2" } },
                Returns = { "Vector2?" },
                ThreadSafety = "ReadParallel",
                Doc = "Where a pixel is on the 2D plane: the place a tap or a click points at in a 2D game, "
                    .. "to compare with a `Part2D.Position`. Nil when the camera looks along the plane or "
                    .. "away from it.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "PointLight",
        Extends = "Instance",
        Module = "render",
        Parents = OnAPart,
        Storage = "PointLight",
        Doc = "A light radiating equally in every direction. Inside a BasePart or an Attachment it shines from "
            .. "there and moves with it; anywhere else it shines from its own `CFrame` (ADR 0095).",
        Properties = {
            {
                Name = "CFrame",
                Type = "CFrame",
                Doc = "Where the light is. **In the world when nothing holds it** -- a light dropped straight into "
                    .. "the Workspace shines from here, and turns with it -- and relative to the BasePart or "
                    .. "Attachment it sits in otherwise, which the identity leaves exactly where that is. The owner's "
                    .. "call (ADR 0095): a light can stand on its own.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Doc = "The light's colour, multiplied by Brightness. Not clamped, so a channel above one is a legal "
                    .. "over-bright tint.",
            },
            {
                Name = "Brightness",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much light this emits. Zero is off without the instance having to be destroyed.",
            },
            {
                Name = "Range",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The radius in metres beyond which this light contributes nothing, which is what lets the "
                    .. "renderer skip it for distant geometry.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this light contributes anything. Off is not the same as a Brightness of zero: a "
                    .. "disabled light is skipped before the renderer counts it against the light budget, so "
                    .. "turning a room's lights off gives the rest of the scene the slots back.",
            },
            {
                Name = "Shadows",
                Type = "boolean",
                Doc = "Whether this light casts shadows.\n\n**It costs SIX tiles of a sixteen-tile atlas**, because "
                    .. "a point light shines in every direction and a shadow map does not: it is six maps, one per "
                    .. "face of a cube around the light, where a SpotLight is one. Two point lights and four spots "
                    .. "fill the atlas; three point lights do not fit.\n\nWhen more lights ask than there are tiles, "
                    .. "the ones nearest and largest on screen are served and the rest simply do not cast. A light "
                    .. "that loses is not dimmed and does not flicker: the order is by apparent size with ties "
                    .. "broken by creation order, so it is the same answer on every frame and on every machine.",
            },
        },
    },
    {
        Name = "Decal",
        Extends = "Instance",
        Module = "render",
        Parents = under("BasePart"),
        Storage = "Decal",
        Doc = "An image projected onto whatever lies inside a box (F2): a scorch mark, a footprint, a poster, "
            .. "a crack. **No face to choose and no parent required** -- `CFrame.lookAt(hit.Position, "
            .. "hit.Position + hit.Normal)` from a `Raycast` and a size is the whole placement, and it lands on "
            .. "parts, terrain and blocks alike. Parented to a `BasePart` its `CFrame` is relative to the part, "
            .. "so it moves with it.\n\nA decal MULTIPLIES what it lands on, after lighting, which keeps the "
            .. "surface's own light and shadow on it exactly: it darkens and tints, and it cannot make a surface "
            .. "brighter than it was. A white pixel is no change.",
        Properties = {
            {
                Name = "CFrame",
                Type = "CFrame",
                Doc = "Where the box is -- relative to the parent part when there is one. The image lies in the "
                    .. "box's right and up, and is projected along its look direction.",
            },
            {
                Name = "Size",
                Type = "vector",
                Default = "vector.create(2, 2, 1)",
                Doc = "Width, height and depth of the box, in metres. Only what is inside it is painted, so a "
                    .. "shallow box paints one wall and a deep one reaches round a corner.",
            },
            {
                Name = "Texture",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image. Its alpha is how much of it lands; none paints `Color` alone.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "Multiplies the image.",
            },
            {
                Name = "Transparency",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "0 paints the image as it is; 1 paints nothing.",
            },
        },
    },
    {
        Name = "CameraTexture",
        Extends = "Instance",
        Module = "render",
        Parents = under("Workspace", "Model", "BasePart", "Camera"),
        Storage = "CameraTexture",
        Doc = "A camera drawing into a texture (ADR 0107): whatever `Camera` sees is drawn into `view://` "
            .. "followed by `ViewName`, and anything that takes a texture -- an `ImageLabel` on a `SurfaceGui`, "
            .. "a `Decal`, a material's map -- can show it. A wall of security monitors is one of these per "
            .. "camera.\n\n**A budget draws them**: at most `[render] max_views_per_frame` a frame (4 by "
            .. "default), the oldest picture first, so twelve monitors take turns instead of costing twelve "
            .. "views. Drawn before the main view, so a feed shows this frame's picture; a camera that sees its "
            .. "own texture sees the previous one. Rendering only: nothing here reaches the simulation, and a "
            .. "dedicated server draws nothing.\n\n**It does not replicate**, because a camera does not -- a "
            .. "replica's view is its own. A feed is set up by the machine that draws it: in a match, a "
            .. "client script makes its cameras and their textures.",
        Properties = {
            {
                Name = "Camera",
                Type = "Camera?",
                Doc = "The camera it draws from. Nil draws nothing and keeps the last picture.",
            },
            {
                Name = "ViewName",
                Type = "string",
                Doc = "The name it draws into: `view://` followed by this. Two with one name: the first made "
                    .. "draws, and the second says so in the log. Until the first picture, the texture is black -- "
                    .. "a screen that is off.",
            },
            {
                Name = "Resolution",
                Type = "Vector2",
                Default = "Vector2.new(512, 288)",
                ErrorKey = "scene.err.size2_positive",
                Doc = "The texture's size in pixels. Capped at `[render] max_view_resolution` on a side "
                    .. "(1024 by default); the cap keeps the shape.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Off keeps the last picture and costs nothing.",
            },
            {
                Name = "UpdateInterval",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.whole_number_at_least_one",
                Doc = "Draw on one frame in this many. A wall of monitors at 4 costs a quarter of what it would "
                    .. "at 1, and a security feed does not need more.",
            },
            {
                Name = "Quality",
                Type = "Enum.ViewQuality",
                Default = "Enum.ViewQuality.Simple",
                Doc = "`Simple` draws without shadows or the look's effects -- what a feed wants; `Full` draws "
                    .. "the way the main view does.",
            },
        },
    },
    {
        Name = "SubWorld",
        Extends = "Instance",
        Module = "render",
        Parents = under("Workspace", "Model", "BasePart"),
        Storage = "SubWorld",
        Doc = "A scene running beside this one (ADR 0107): an arcade cabinet you can play, a game on a "
            .. "computer inside the game, a snow globe with its own weather. `Load()` starts the scene named "
            .. "by `Scene` in a world of its own -- its own instances, scripts, physics and clock -- and its "
            .. "current camera draws into `view://` followed by `ViewName`, which an `ImageLabel` on a "
            .. "`SurfaceGui` shows like any other picture.\n\n"
            .. "**Nothing crosses except what you send.** A script here cannot reach an instance there, "
            .. "nor one there an instance here. The two talk through `Send` and `Received` (inside, "
            .. "`SceneService:SendToHost` and `SceneService.HostMessageReceived`), and this side decides what input "
            .. "reaches it with `SetInputState`.\n\n"
            .. "It runs the scene's own `ServerScriptService` and `ClientScriptService` as a game played "
            .. "alone, never the project's `GlobalScriptService`, and it ticks once for each tick of this "
            .. "world. At most `[render] max_sub_worlds` run at once (2 by default); a sub-world cannot hold "
            .. "another. **It does not replicate**: in a match, each machine that loads one runs its own.",
        Properties = {
            {
                Name = "Scene",
                Type = "string",
                Doc = "The scene file it runs, content-relative: `scenes/arcade.scene.json`. Read when "
                    .. "`Load()` starts it; a change afterwards waits for the next `Load()`.",
            },
            {
                Name = "ViewName",
                Type = "string",
                Doc = "The name its picture is drawn into: `view://` followed by this. Empty draws nothing, "
                    .. "and the world runs all the same.",
            },
            {
                Name = "Resolution",
                Type = "Vector2",
                Default = "Vector2.new(512, 288)",
                ErrorKey = "scene.err.size2_positive",
                Doc = "The picture's size in pixels, as a `CameraTexture`'s.",
            },
            {
                Name = "UpdateInterval",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.whole_number_at_least_one",
                Doc = "Draw on one frame in this many. The world ticks every tick regardless.",
            },
            {
                Name = "Quality",
                Type = "Enum.ViewQuality",
                Default = "Enum.ViewQuality.Simple",
                Doc = "`Simple` draws without shadows or the look's effects; `Full` draws the way the main "
                    .. "view does.",
            },
            {
                Name = "Running",
                Type = "boolean",
                Default = "true",
                Doc = "False pauses it: no ticks, and its picture stays as it was.",
            },
        },
        Methods = {
            {
                Name = "Load",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Starts the scene. **Returns at once**: the world boots after this tick, and "
                    .. "`Loaded` fires when its scripts have started. Loading one already loaded does nothing. "
                    .. "Raises when `[render] max_sub_worlds` are already running, and inside a sub-world.",
            },
            {
                Name = "Unload",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops it and throws its world away, after this tick. Its picture stays until the "
                    .. "next `Load()` draws over it. Destroying the `SubWorld` does the same.",
            },
            {
                Name = "IsLoaded",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "ReadParallel",
                Doc = "Whether its world is running (or paused by `Running`), which is true from `Loaded` "
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
                Doc = "Holds the sub-world's `InputAction` named `action` at `value` -- true or false for "
                    .. "a `Bool` action, a number, a `Vector2` or a `vector` for a direction -- from its next "
                    .. "tick until it is set again. Nothing else reaches its input: the keyboard, the mouse "
                    .. "and the pads are this world's, and passing them on is this world's decision.",
            },
            {
                Name = "Send",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends plain values into the sub-world, where `SceneService.HostMessageReceived` fires with "
                    .. "them on its next tick. The values a `RemoteEvent` takes, and never an instance -- "
                    .. "the two worlds have none in common. Dropped when it is not loaded.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Loaded",
                Parameters = {},
                Doc = "Its world is running and its scripts have started.",
            },
            {
                Name = "Received",
                Parameters = { { Name = "arguments", Type = "...any" } },
                Doc = "The values the sub-world passed to `SceneService:SendToHost`, one tick after it sent them.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "ParticleEmitter",
        Extends = "Instance",
        Module = "render",
        Parents = OnAPart,
        Storage = "ParticleEmitter",
        Doc = "Sparks, smoke, dust and magic (F2). Parent it to a `BasePart` or an `Attachment`: particles are "
            .. "born at its position and fly along its up direction. **They are a picture, not the world** -- "
            .. "they collide with nothing, a script cannot find one, and they are simulated on the frame rather "
            .. "than the tick -- which is what lets a scene hold thousands for the price of one draw.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether it emits continuously. `Emit` works either way: a burst is a separate thing from a stream.",
            },
            {
                Name = "Rate",
                Type = "number",
                Default = "20",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Particles a second while enabled.",
            },
            {
                Name = "Lifetime",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Seconds each particle lives before it is gone.",
            },
            {
                Name = "Speed",
                Type = "number",
                Default = "4",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Metres a second at birth, along the emitter's up direction.",
            },
            {
                Name = "SpreadAngle",
                Type = "number",
                Default = "15",
                ErrorKey = "scene.err.number_spread_degrees",
                Doc = "How far, in degrees, a particle's direction may stray from straight up: 0 is a jet, 180 a sphere.",
            },
            {
                Name = "Acceleration",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "Metres a second squared in world space, for the whole of each particle's life. `vector.create(0, -9.81, 0)` is gravity; a small upward one is heat.",
            },
            {
                Name = "Drag",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The fraction of its velocity a particle loses each second: smoke slows, sparks do not.",
            },
            {
                Name = "WindAffectsDrift",
                Type = "boolean",
                Default = "false",
                Presentation = true,
                Doc = "Whether the workspace's wind carries the particles: the wind where each one is is added "
                    .. "to how it moves. Off by default, so an effect made before the wind looks as it did.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "The colour at birth.",
            },
            {
                Name = "ColorEnd",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "The colour at death; life interpolates between the two.",
            },
            {
                Name = "Size",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The width in metres at birth.",
            },
            {
                Name = "SizeEnd",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The width in metres at death.",
            },
            {
                Name = "Transparency",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "0 opaque to 1 invisible, at birth.",
            },
            {
                Name = "TransparencyEnd",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "At death. The default fades each particle out as it dies.",
            },
            {
                Name = "LightEmission",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "0 blends a particle over what is behind it, 1 ADDS its light to it -- fire, sparks, magic -- and between is both. Additive particles never darken anything, which is the point.",
            },
            {
                Name = "Brightness",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "A multiplier on the colour, unclamped: above one is what makes a spark bloom.",
            },
            {
                Name = "Shape",
                Type = "Enum.ParticleShape",
                Default = "Enum.ParticleShape.Soft",
                Doc = "What one particle looks like.",
            },
        },
        Methods = {
            {
                Name = "Emit",
                Parameters = { { Name = "count", Type = "number" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Emits `count` particles at once, whether or not the emitter is enabled: an explosion is a "
                    .. "burst, not a stream. At most 1,000 a call.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "Highlight",
        Extends = "Instance",
        Module = "render",
        Parents = InTheWorld,
        Storage = "Highlight",
        Doc = "An outline and a tint over a part or a model: what is selected, what can be picked up, an "
            .. "ally seen through a wall. **It is a mark on the picture, not a thing in the world**: it is "
            .. "drawn over the finished frame in the colours it was given, whatever the light, and nothing "
            .. "can collide with it or find it with a ray. At most `[render] max_highlights` are drawn in a "
            .. "frame (32 unless the project says otherwise); past that the nearest win.",
        Properties = {
            {
                Name = "Adornee",
                Type = "(BasePart | Model)?",
                Presentation = true,
                Doc = "What is highlighted: a part, or a model and every part in it. Nothing means the highlight's "
                    .. "own parent.",
            },
            {
                Name = "FillColor",
                Type = "Color3",
                Default = "Color3.new(1, 0.2, 0.2)",
                Presentation = true,
                Doc = "The colour laid over the whole shape.",
            },
            {
                Name = "FillTransparency",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How see-through the fill is: 0 hides what it covers, 1 leaves only the outline.",
            },
            {
                Name = "OutlineColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Presentation = true,
                Doc = "The colour of the line round the shape's edge.",
            },
            {
                Name = "OutlineTransparency",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How see-through the outline is: 1 is a fill with no line.",
            },
            {
                Name = "DepthMode",
                Type = "Enum.HighlightDepthMode",
                Default = "Enum.HighlightDepthMode.AlwaysOnTop",
                Presentation = true,
                Doc = "Whether the highlight shows through what is in front of the shape, or only where the shape "
                    .. "itself is seen.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether it is drawn.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Beam",
        Extends = "Instance",
        Module = "render",
        Parents = under("Workspace", "Model", "BasePart", "Attachment"),
        Storage = "Beam",
        Doc = "A band of colour or a texture between two attachments: a laser, a tractor beam, a rope of "
            .. "light, lightning with a texture that runs. Straight, or a curve that leaves each attachment "
            .. "along its X. **A picture, not the world** -- it collides with nothing and is drawn on the "
            .. "frame, where its attachments are drawn.",
        Properties = {
            {
                Name = "Attachment0",
                Type = "Attachment?",
                Presentation = true,
                Doc = "Where it starts.",
            },
            {
                Name = "Attachment1",
                Type = "Attachment?",
                Presentation = true,
                Doc = "Where it ends.",
            },
            {
                Name = "Color",
                Type = "ColorSequence",
                Default = "ColorSequence.new(Color3.new(1, 1, 1))",
                Presentation = true,
                Doc = "Its colour along its length, from `Attachment0` at 0 to `Attachment1` at 1. Multiplies the "
                    .. "texture.",
            },
            {
                Name = "Transparency",
                Type = "NumberSequence",
                Default = "NumberSequence.new(0)",
                Presentation = true,
                Doc = "How see-through it is along its length: 0 solid, 1 gone.",
            },
            {
                Name = "Width0",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "How wide it is at `Attachment0`, in metres.",
            },
            {
                Name = "Width1",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "How wide it is at `Attachment1`, in metres.",
            },
            {
                Name = "CurveSize0",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "How far the beam leaves `Attachment0` along that attachment's own X before it bends towards "
                    .. "the other end, in metres. Zero at both ends is a straight line; negative leaves the other "
                    .. "way.",
            },
            {
                Name = "CurveSize1",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "How far before `Attachment1` the beam is already running along that attachment's own X, in "
                    .. "metres.",
            },
            {
                Name = "Segments",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.whole_number_at_least_one",
                Presentation = true,
                Doc = "How many straight pieces a curved beam is drawn as, 1 to 64. A straight beam needs one.",
            },
            {
                Name = "Texture",
                Type = "Content",
                ContentKind = "Texture",
                Presentation = true,
                Doc = "The image along it, its U along the beam and its V across. None is a plain band of `Color`.",
            },
            {
                Name = "TextureLength",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "How long one repeat of the texture is, in metres, in `Wrap` and `Static` modes.",
            },
            {
                Name = "TextureMode",
                Type = "Enum.TextureMode",
                Default = "Enum.TextureMode.Stretch",
                Presentation = true,
                Doc = "How the texture is laid along it.",
            },
            {
                Name = "TextureSpeed",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "How fast the texture runs from `Attachment0` towards `Attachment1`: in repeats a second in "
                    .. "`Stretch` mode, in metres a second in `Wrap`. `Static` does not run.",
            },
            {
                Name = "FaceCamera",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether the band turns about its own length to face the camera. Off, it lies in the plane of "
                    .. "its attachments' Y axes -- a flat ribbon with two sides.",
            },
            {
                Name = "LightEmission",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much it adds its colour to what is behind it instead of covering it: 0 an ordinary "
                    .. "surface, 1 light.",
            },
            {
                Name = "LightInfluence",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much the scene's light colours it: 1 is lit like a surface, 0 is its own colour at any "
                    .. "hour.",
            },
            {
                Name = "ZOffset",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "Metres towards the camera it is drawn at, so a beam lying on a wall is not cut by it.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether it is drawn.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Trail",
        Extends = "Instance",
        Module = "render",
        Parents = under("Workspace", "Model", "BasePart", "Attachment"),
        Storage = "Trail",
        Doc = "The ribbon two attachments leave behind as they move: a sword's swing, a tyre's mark, the "
            .. "wake of something fast. Each piece lasts `Lifetime` seconds and fades as its sequences say. "
            .. "**A picture, not the world** -- drawn on the frame, simulated by nothing, and a script "
            .. "cannot read where it is.",
        Properties = {
            {
                Name = "Attachment0",
                Type = "Attachment?",
                Presentation = true,
                Doc = "One edge of the ribbon.",
            },
            {
                Name = "Attachment1",
                Type = "Attachment?",
                Presentation = true,
                Doc = "The other edge.",
            },
            {
                Name = "Lifetime",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "How long a piece of the trail lasts, in seconds, from when the attachments were there.",
            },
            {
                Name = "MinLength",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "How far the attachments must move before the trail gains a piece, in metres. Smaller is "
                    .. "smoother and costs more.",
            },
            {
                Name = "MaxLength",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "The longest the trail may be, in metres; what is older is dropped. Zero is no limit but "
                    .. "`Lifetime`.",
            },
            {
                Name = "Color",
                Type = "ColorSequence",
                Default = "ColorSequence.new(Color3.new(1, 1, 1))",
                Presentation = true,
                Doc = "Its colour over a piece's life, from new at 0 to `Lifetime` at 1. Multiplies the texture.",
            },
            {
                Name = "Transparency",
                Type = "NumberSequence",
                Default = "NumberSequence.new(0)",
                Presentation = true,
                Doc = "How see-through a piece is over its life: 0 solid, 1 gone.",
            },
            {
                Name = "WidthScale",
                Type = "NumberSequence",
                Default = "NumberSequence.new(1)",
                Presentation = true,
                Doc = "How wide a piece is over its life, as a fraction of the distance between the attachments.",
            },
            {
                Name = "Texture",
                Type = "Content",
                ContentKind = "Texture",
                Presentation = true,
                Doc = "The image along it, its U along the trail and its V across. None is a plain band of `Color`.",
            },
            {
                Name = "TextureLength",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "How long one repeat of the texture is, in metres, in `Wrap` and `Static` modes.",
            },
            {
                Name = "TextureMode",
                Type = "Enum.TextureMode",
                Default = "Enum.TextureMode.Stretch",
                Presentation = true,
                Doc = "How the texture is laid along it.",
            },
            {
                Name = "FaceCamera",
                Type = "boolean",
                Default = "false",
                Presentation = true,
                Doc = "Whether the ribbon is turned to face the camera about its own middle line, as wide as the "
                    .. "attachments are apart. Off, it is the surface the two attachments swept.",
            },
            {
                Name = "LightEmission",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much it adds its colour to what is behind it instead of covering it: 0 an ordinary "
                    .. "surface, 1 light.",
            },
            {
                Name = "LightInfluence",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much the scene's light colours it: 1 is lit like a surface, 0 is its own colour at any "
                    .. "hour.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether the trail grows. Off, what is already there fades out over its `Lifetime`.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Clear",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Drops the whole trail at once: what something that teleports calls, so the ribbon does "
                    .. "not stretch from where it was to where it is.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "SpotLight",
        Extends = "Instance",
        Module = "render",
        Parents = OnAPart,
        Storage = "SpotLight",
        Doc = "A light confined to a cone about its forward direction -- its holder's, turned by its own "
            .. "`CFrame`, or its own alone when nothing holds it (ADR 0095). A sibling of PointLight rather than "
            .. "a subclass of it: the two share their placement and a few properties, and no behaviour.",
        Properties = {
            {
                Name = "CFrame",
                Type = "CFrame",
                Doc = "Where the light is. **In the world when nothing holds it** -- a light dropped straight into "
                    .. "the Workspace shines from here, and turns with it -- and relative to the BasePart or "
                    .. "Attachment it sits in otherwise, which the identity leaves exactly where that is. The owner's "
                    .. "call (ADR 0095): a light can stand on its own.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Doc = "The light's colour, multiplied by Brightness.",
            },
            {
                Name = "Brightness",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much light this emits.",
            },
            {
                Name = "Range",
                Type = "number",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance in metres beyond which this light contributes nothing.",
            },
            {
                Name = "Angle",
                Type = "number",
                ErrorKey = "scene.err.number_cone_degrees",
                Doc = "The full width of the cone in degrees.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this light contributes anything. Off is not the same as a Brightness of zero: a "
                    .. "disabled light is skipped before the renderer counts it against the light budget, so "
                    .. "turning a room's lights off gives the rest of the scene the slots back.",
            },
            {
                Name = "Shadows",
                Type = "boolean",
                Doc = "Whether this light casts shadows.\n\n**It costs one tile of a sixteen-tile atlas**, where a "
                    .. "PointLight costs six, so the two are budgeted against each other rather than counted "
                    .. "separately.\n\nWhen more lights ask than there are tiles, the ones nearest and largest on "
                    .. "screen are served and the rest simply do not cast. A light that loses is not dimmed and does "
                    .. "not flicker: the order is by apparent size with ties broken by creation order, so it is the "
                    .. "same answer on every frame and on every machine.",
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
        Doc = "The abstract base of the effects that change the finished picture rather than the world in it "
            .. "(ADR 0096). **Where one is decides whose it is**: directly under `Lighting` it belongs to the "
            .. "world -- saved with the scene and seen by everybody -- and directly under "
            .. "`Workspace.CurrentCamera` it belongs to whoever looks through that camera. Anywhere else it does "
            .. "nothing, and the editor says so on the instance.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this effect takes part. Off is not the same as deleting it: a `BloomEffect` that "
                    .. "is off still governs bloom, and governs it to nothing.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "BloomEffect",
        Extends = "PostEffect",
        Module = "render",
        Storage = "BloomEffect",
        Doc = "The glow that bright light spills onto what is around it. **The engine blooms without one**, "
            .. "exactly as this class's defaults do; adding one takes the glow over, and one with `Enabled` "
            .. "off turns it off -- what a 2D game whose white sprites should not shine wants. When several "
            .. "apply, the first enabled one in order wins: `Lighting`'s children first, then the camera's.",
        Properties = {
            {
                Name = "Intensity",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How strongly the glow is added, as a multiple of the engine's own: 1 is the look every "
                    .. "world has without this instance, 2 is twice the glow, 0 is none.",
            },
            {
                Name = "Size",
                Type = "number",
                Default = "24",
                ErrorKey = "scene.err.number_bloom_size",
                Doc = "How far the glow reaches, from 0 to 56. 24 is the engine's own reach; 48 spreads the "
                    .. "same light about twice as far, and fainter for it.",
            },
            {
                Name = "Threshold",
                Type = "number",
                Default = "1.1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How bright a pixel must be, after exposure, before it glows. Exposure maps a frame's "
                    .. "average to about 0.45, so the default blooms what is a little over twice as bright as "
                    .. "the average; 0 makes everything glow a little.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "ColorCorrectionEffect",
        Extends = "PostEffect",
        Module = "render",
        Storage = "ColorCorrectionEffect",
        Doc = "A grade over the whole picture: brighter or darker, flatter or punchier, greyer or more vivid, "
            .. "and tinted. It works on the exposed light, before the tone curve compresses it, so a "
            .. "highlight it brightens still rolls off rather than clipping. **Every enabled one applies**, "
            .. "one after another in order -- `Lighting`'s children first, then the camera's -- and each "
            .. "applies its tint, then its saturation, then its contrast, then its brightness.",
        Properties = {
            {
                Name = "Brightness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_minus_one_to_one",
                Doc = "Light added to every pixel, from -1 to 1. A frame's average sits near 0.45 after "
                    .. "exposure, so 0.1 is a gentle lift and -0.2 a clear darkening; the darkest pixels "
                    .. "stop at black.",
            },
            {
                Name = "Contrast",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_minus_one_to_one",
                Doc = "How far every pixel is pushed from the frame's average brightness, from -1 to 1. 0 "
                    .. "leaves it, 0.5 spreads it half as far again, and -1 flattens the picture to one grey.",
            },
            {
                Name = "Saturation",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_minus_one_to_one",
                Doc = "How vivid colours are, from -1 to 1. -1 is black and white, 0 leaves them, 1 is twice "
                    .. "as far from grey.",
            },
            {
                Name = "TintColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "Multiplies the picture. White changes nothing; a warm white warms it.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "BlurEffect",
        Extends = "PostEffect",
        Module = "render",
        Storage = "BlurEffect",
        Doc = "Softens the whole world picture, as behind a pause menu. The interface is drawn after it and "
            .. "stays sharp. **Several combine as blurs do in optics**: the sizes add by their squares, so two "
            .. "blurs of 8 are one of about 11.3, not one of 16.",
        Properties = {
            {
                Name = "Size",
                Type = "number",
                Default = "24",
                ErrorKey = "scene.err.number_blur_size",
                Doc = "How far a pixel's light is spread, in pixels of a picture 1,080 lines tall -- most of it "
                    .. "lands within this distance. It scales with the window, so a blur looks the same at any "
                    .. "size. 0 is no blur, and costs nothing; 100 is the most, a picture with no detail left.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "DepthOfFieldEffect",
        Extends = "PostEffect",
        Module = "render",
        Storage = "DepthOfFieldEffect",
        Doc = "Focus by distance, as a camera lens has it: a band of the world in focus, and what is nearer or "
            .. "further softening away from it. The sky counts as infinitely far. When several apply, the "
            .. "first enabled one wins, in the order `Lighting`'s children and then the camera's. A machine "
            .. "whose graphics settings turn depth of field off draws without it.",
        Properties = {
            {
                Name = "FocusDistance",
                Type = "number",
                Default = "25",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The distance from the camera, in metres, that is sharpest.",
            },
            {
                Name = "InFocusRadius",
                Type = "number",
                Default = "10",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How many metres either side of `FocusDistance` stay fully sharp before the softening "
                    .. "begins.",
            },
            {
                Name = "NearIntensity",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How soft what is nearer than the sharp band becomes, from 0 (not at all) to 1.",
            },
            {
                Name = "FarIntensity",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How soft what is further than the sharp band becomes, from 0 (not at all) to 1.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "SunRaysEffect",
        Extends = "PostEffect",
        Module = "render",
        Storage = "SunRaysEffect",
        Doc = "Shafts of light streaming from the sun past whatever stands in front of it. They come from the "
            .. "open sky around the sun, so a tree or a wall cuts them, and they fade away as the sun leaves "
            .. "the view or sets. When several apply, the first enabled one wins, in the order `Lighting`'s "
            .. "children and then the camera's. A machine whose graphics settings turn sun rays off draws "
            .. "without them.",
        Properties = {
            {
                Name = "Intensity",
                Type = "number",
                Default = "0.25",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How bright the shafts are, from 0 to 1.",
            },
            {
                Name = "Spread",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How far the shafts reach from the sun, from 0 (a halo) to 1 (across the screen).",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Atmosphere",
        Extends = "Instance",
        Module = "render",
        Parents = under("Lighting"),
        Storage = "Atmosphere",
        Doc = "The air between the camera and everything it sees. Distance fades into it and it thins with "
            .. "height, it is lit by the sun and glows around it, and it tints the sky towards the horizon so "
            .. "the ground and the sky meet. **It counts only directly under `Lighting`**, and only the first "
            .. "one there. With one, `Lighting.FogStart`, `FogEnd` and `FogColor` are kept but not used.",
        Properties = {
            {
                Name = "Density",
                Type = "number",
                Default = "0.35",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How thick the air is, from 0 to 1. 0 is perfectly clear; at the default, half of what "
                    .. "stands 280 metres away still shows through at the height of `Offset`; at 1, half is "
                    .. "gone within 35 metres.",
            },
            {
                Name = "Offset",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Doc = "The height, in metres, at which the air has exactly its `Density`. It is thicker below "
                    .. "and thinner above, by `Decay`, so raising it lifts the haze up a valley's sides.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.fromRGB(196, 210, 230)",
                Doc = "The colour of the air, lit. Distant things fade towards it, and the sky turns towards it "
                    .. "at the horizon. The sun's height still decides how bright it is: the same colour is dark "
                    .. "at night.",
            },
            {
                Name = "Decay",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How quickly the air thins with height, from 0 to 1. 0 keeps it equally thick at every "
                    .. "altitude; at the default it halves every 100 metres of height; at 1, every 10.",
            },
            {
                Name = "Glare",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_ten",
                Doc = "The bright lobe the air makes around the sun, from 0 to 10. It brightens the sky near "
                    .. "the sun and the distance seen towards it.",
            },
            {
                Name = "Haze",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_ten",
                Doc = "Extra thickness near the horizon, from 0 to 10: it washes the sky's horizon band into "
                    .. "the air's colour and buries far distance deeper than `Density` alone.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Sky",
        Extends = "Instance",
        Module = "render",
        Parents = under("Lighting"),
        Storage = "Sky",
        Doc = "What the sky shows: six images around the world, the sun's and the moon's look, stars and "
            .. "clouds. **It counts only directly under `Lighting`**, and only the first one there. The images "
            .. "are also what surfaces reflect and are lit by. **The sun stays on the clock**: its direction "
            .. "comes from `Lighting.ClockTime` and `GeographicLatitude` whatever the images show, so "
            .. "shadows and the drawn sun agree -- choose images without a sun painted in, or turn "
            .. "`CelestialBodiesShown` off.",
        Properties = {
            {
                Name = "SkyboxBack",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image towards +Z, before `SkyboxOrientation` turns the set. None of the six means "
                    .. "the engine's own sky, drawn from the time of day.",
            },
            {
                Name = "SkyboxDown",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image straight down.",
            },
            {
                Name = "SkyboxFront",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image towards -Z, which a camera with no rotation looks at.",
            },
            {
                Name = "SkyboxLeft",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image towards -X.",
            },
            {
                Name = "SkyboxRight",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image towards +X.",
            },
            {
                Name = "SkyboxUp",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image straight up.",
            },
            {
                Name = "SkyboxOrientation",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "Turns the six images together, in degrees about X, Y and Z -- a mountain range painted "
                    .. "into the images is moved to where the world wants it.",
            },
            {
                Name = "SunTexture",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "An image drawn as the sun's disc, lit by the sun's colour. None draws a round, soft-edged "
                    .. "disc.",
            },
            {
                Name = "MoonTexture",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "An image drawn as the moon's disc at night. None draws a plain pale disc.",
            },
            {
                Name = "SunAngularSize",
                Type = "number",
                Default = "2.3",
                ErrorKey = "scene.err.number_angular_size",
                Doc = "How wide the sun looks, in degrees across, above 0 and at most 60. The real sun is about "
                    .. "half a degree; the default is larger so a sunset reads in a short day.",
            },
            {
                Name = "MoonAngularSize",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_angular_size",
                Doc = "How wide the moon looks, in degrees across, above 0 and at most 60.",
            },
            {
                Name = "StarCount",
                Type = "number",
                Default = "3000",
                ErrorKey = "scene.err.number_star_count",
                Doc = "How many stars cover the whole sky at night, from 0 to 10,000. They come out as the "
                    .. "sun goes down and are the same stars in the same places every night.",
            },
            {
                Name = "CelestialBodiesShown",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the sun, the moon and the stars are drawn. Off leaves only the images and the "
                    .. "clouds -- the light still comes from where the clock puts the sun.",
            },
            {
                Name = "CloudCover",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much of the sky the clouds cover, from 0 (none) to 1 (overcast). They drift with the "
                    .. "game's own clock, so a paused game's clouds stand still and a replay's move the same way.",
            },
            {
                Name = "CloudDensity",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How thick each cloud is, from 0 (wisps the sky shows through) to 1 (solid, with dark "
                    .. "undersides).",
            },
            {
                Name = "CloudColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "The clouds' colour where the sun lights them.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "InputContext",
        Extends = "Instance",
        Module = "input",
        Storage = "InputContext",
        Doc = "A group of `InputAction` children that are live together (§2.4, ADR 0029). A menu opening is one "
            .. "context enabled and another disabled, rather than a pile of conditionals inside every handler. "
            .. "Parent it anywhere -- the convention is under the Script that owns it -- because a context is "
            .. "reached by being enabled, not by where it sits.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this context and its actions take part in resolution at all. A disabled context "
                    .. "consumes nothing, so a lower-priority one bound to the same key starts firing the tick "
                    .. "this goes false -- which is exactly what closing a menu should do.",
            },
            {
                Name = "Priority",
                Type = "number",
                Default = "0",
                Doc = "Resolution order, highest first. It orders FALLTHROUGH and nothing else -- in particular "
                    .. "it does not decide which clock the context is dispatched on, which is `Rate` (ADR 0039). "
                    .. "Ties are broken by a stable order the engine reproduces on every run (R10), so two "
                    .. "contexts at the same priority never swap places between one run and the next.",
            },
            {
                Name = "Sink",
                Type = "boolean",
                Default = "false",
                Doc = "Whether this context CONSUMES the inputs its bindings name, hiding them from every "
                    .. "lower-priority context. A modal dialog sinks; a HUD does not. Sinking is per input and "
                    .. "not per context: a sinking context that binds Escape hides Escape and leaves W alone.",
            },
            {
                Name = "Rate",
                Type = "Enum.InputRate",
                Default = "Enum.InputRate.Simulation",
                Doc = "Which clock this context is dispatched on (ADR 0039). `Simulation` by default, because "
                    .. "that is the safe one: an action nobody thought about fires where determinism holds and "
                    .. "where the "
                    .. "input replay can see it. `Render` is for camera look and for UI, and an action read from "
                    .. "a Render context is frame-rate-dependent by construction.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "InputAction",
        Extends = "Instance",
        Module = "input",
        Parents = under("InputContext"),
        Storage = "InputAction",
        Doc = "A named thing the player can do, decoupled from the input that does it (§2.4, ADR 0029). Game "
            .. "code connects to the action; `InputBinding` children say what drives it. That indirection is "
            .. "the whole point: rebinding is a property write, and a prompt can ask the action what key to "
            .. "draw.\n\nParent it to an `InputContext`. An action outside one is inert -- resolution walks "
            .. "contexts, so an action nothing groups is an action nothing dispatches.",
        Properties = {
            {
                Name = "Type",
                Type = "Enum.InputActionType",
                Default = "Enum.InputActionType.Bool",
                Doc = "What this action produces, and therefore what `GetState` returns. It belongs to the "
                    .. "action rather than to the binding, because one action may be driven by a key, a stick "
                    .. "and an on-screen button at once and all three have to answer in the same currency.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this action resolves. A disabled action consumes nothing even inside a sinking "
                    .. "context, so disabling one hands its input back to whatever is underneath.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetState",
                Parameters = {},
                Returns = { "boolean | number | Vector2 | vector" },
                Doc = "This action's value as of the current tick -- a boolean for `Bool`, a number for "
                    .. "`Direction1D`, a `Vector2` for `Direction2D` and `ViewportPosition`, and a `vector` for "
                    .. "`Direction3D`. It is a SNAPSHOT: two calls inside one tick agree, and a recorded input "
                    .. "stream hands the same answers back with no hardware attached, which is what makes the "
                    .. "replay a replay of input rather than of a bot.",
            },
            {
                Name = "GetPreferredBinding",
                Parameters = { { Name = "deviceType", Type = "Enum.InputDeviceType?", Default = "nil" } },
                Returns = { "InputBinding?" },
                Doc = 'The binding a prompt should draw for this action -- "Press [E]". With a device type, '
                    .. "the first child binding belonging to that family in document order; without one, the "
                    .. "family of `InputService.LastInputDeviceType`, so a prompt follows whatever the player "
                    .. "last touched. nil when the action has no binding for that device.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Pressed",
                Parameters = {},
                Doc = "Fired on the tick a `Bool` action becomes true. Never fires for any other action type: "
                    .. "an axis has no moment of being pressed.",
            },
            {
                Name = "Released",
                Parameters = {},
                Doc = "Fired on the tick a `Bool` action becomes false, including when the action or its "
                    .. "context is disabled while it was held -- a held key that stops being delivered has been "
                    .. "released as far as the game is concerned, and leaving it stuck true is how a character "
                    .. "keeps running after a menu opens.",
            },
            {
                Name = "StateChanged",
                Parameters = {},
                Doc = "Fired on any tick this action's value changed, whatever its type. It carries NO "
                    .. "arguments and the handler reads `GetState()` -- the same arrangement `Destroying` and "
                    .. "`GetPropertyChangedSignal` use, and for the same reason (ADR 0039): the queue between "
                    .. "the engine and the VM carries POD facts, an action's value is one of four types, and a "
                    .. "signal whose argument had to be rebuilt at drain time would be a fire that captured "
                    .. "nothing. Input dispatches once per tick, so the value read in the handler is the value "
                    .. "that caused the fire.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "InputBinding",
        Extends = "Instance",
        Module = "input",
        Parents = under("InputAction"),
        Storage = "InputBinding",
        Doc = "One physical input that drives its parent `InputAction` (§2.4). An action may have several, and "
            .. "they are ORed: a key, a gamepad stick and a UI button on one action are three ways to do one "
            .. "thing.\n\nWhich fields matter depends on the parent's `Type`. A `Bool` action reads `KeyCode`; "
            .. "a `Direction2D` action reads either `KeyCode` naming a 2D source such as `LeftThumbstick`, or "
            .. "the four composite keys. A binding whose fields do not suit its parent is inert rather than an "
            .. "error -- an action's type is a property somebody can change after the binding was built.",
        Properties = {
            {
                Name = "KeyCode",
                Type = "Enum.KeyCode",
                Default = "Enum.KeyCode.Unknown",
                Doc = "The input this binding names. `Unknown` means unbound, which is why the enum carries "
                    .. "that item: an unset binding is expressible without the property being nullable, and the "
                    .. "engine's value domain stays a closed set (ADR 0039).",
            },
            {
                Name = "Up",
                Type = "Enum.KeyCode",
                Default = "Enum.KeyCode.Unknown",
                Doc = "The composite key for +Y, on a `Direction1D` or `Direction2D` action. The four "
                    .. "composites are how a keyboard drives an axis: W/A/S/D is one binding, not four.",
            },
            {
                Name = "Down",
                Type = "Enum.KeyCode",
                Default = "Enum.KeyCode.Unknown",
                Doc = "The composite key for -Y.",
            },
            {
                Name = "Left",
                Type = "Enum.KeyCode",
                Default = "Enum.KeyCode.Unknown",
                Doc = "The composite key for -X.",
            },
            {
                Name = "Right",
                Type = "Enum.KeyCode",
                Default = "Enum.KeyCode.Unknown",
                Doc = "The composite key for +X.",
            },
            {
                Name = "Scale",
                Type = "number",
                Default = "1",
                Doc = "What this binding's value is multiplied by before it reaches the action. Mouse look "
                    .. "wants a small one and a stick wants 1; a negative one inverts an axis, which is what a "
                    .. 'settings screen\'s "invert Y" writes.',
            },
            {
                Name = "DisplayName",
                Type = "string",
                Default = '""',
                Doc = "What a prompt shows for this binding. A localization key is allowed and is the intended "
                    .. 'use (§6): a binding that says "ui.prompt.interact" translates and one that says '
                    .. '"Interact" does not. Empty means the engine has nothing to offer and the prompt '
                    .. "should fall back to the key's own name.\n\n**The engine never acts on this and never "
                    .. "will**, which is what makes it different from an `Inert` property: it is data a GAME "
                    .. "reads back through `GetPreferredBinding` to draw its own prompt. There is no prompt "
                    .. "widget in v1 and there is not meant to be one -- a rebinding screen is a game's "
                    .. "screen.",
            },
            {
                Name = "Image",
                Type = "Content",
                ContentKind = "Texture",
                Default = '""',
                Doc = "A glyph for this binding, as an `asset://` URI. Empty means none -- a prompt with no "
                    .. "glyph falls back to `DisplayName`, which is why the two are set together or not at "
                    .. "all.\n\n**The engine never acts on this and never will**, for exactly the reason "
                    .. "`DisplayName` gives: it is the same datum in a picture instead of a word, read back "
                    .. "by a GAME through `GetPreferredBinding` to draw its own prompt. Nothing in the engine "
                    .. "draws a prompt, so nothing in the engine loads this texture -- an `ImageLabel` the "
                    .. "game points at it does.",
            },
            {
                Name = "DeviceType",
                Type = "Enum.InputDeviceType",
                ReadOnly = true,
                Doc = "Which device family this binding belongs to, DERIVED from `KeyCode` (ADR 0039). It is "
                    .. "read-only because a settable one could disagree with the key beside it -- "
                    .. "`DeviceType = Gamepad` on a binding whose KeyCode is W is a statement the engine would "
                    .. "then have to choose whether to believe. Anything that is not a gamepad input reports "
                    .. "`KeyboardMouse`, `Unknown` included.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "ScreenGui",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIService"),
        Storage = "ScreenGui",
        Doc = "The root of one screen-space UI tree, parented to `UIService` (§2.2). Everything under it is "
            .. "laid out against the window and drawn over the world.\n\nIt is the unit of layout: a write "
            .. "that changes a layout marks the nearest ScreenGui dirty, and a screen nothing changed does not "
            .. "run the solver at all. That is a design constraint rather than an optimisation -- the "
            .. 'benchmark asserts ZERO solver invocations on an idle frame, because "about zero '
            .. 'microseconds" is a measurement of the clock.',
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this tree is laid out and drawn at all. A disabled ScreenGui costs one boolean "
                    .. "read per frame; it does not cost a layout.",
            },
            {
                Name = "DisplayOrder",
                Type = "number",
                Default = "0",
                Doc = "Which ScreenGui draws on top when two overlap, highest last. `ZIndex` orders WITHIN a "
                    .. "tree and this orders BETWEEN trees, so a HUD and a modal never have to agree on a "
                    .. "shared numbering.",
            },
            {
                Name = "ReferenceHeight",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The height of the window this tree was laid out for, or 0 for pixels. With one set, "
                    .. "every offset, text size, corner and stroke under it is in units of a window that "
                    .. "tall, and the engine scales the whole tree to the window it has -- so a layout drawn "
                    .. "for a 720-line monitor takes the same share of a phone with twice the lines, where in "
                    .. "pixels it would be half its size. The width follows: the window is as many units "
                    .. "wide as its shape gives that height, so anchor to edges with `Scale` and size with "
                    .. "`Offset`. `AbsolutePosition` and `AbsoluteSize` stay in window pixels. 720 suits a "
                    .. "layout drawn on a desktop.",
            },
            {
                Name = "ScreenInsets",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this tree lays out inside `UIService.SafeAreaInsets` rather than against the "
                    .. "whole window. On by default: a full-bleed background is the exception and a HUD "
                    .. "clipped by a notch is the failure.",
            },
            {
                Name = "KeepOnSceneLoad",
                Type = "boolean",
                Default = "false",
                Doc = "Whether this screen goes with the game to the next scene (ADR 0106) rather than with "
                    .. "the scene it is in. A loading screen drawn before `SceneService:LoadScene` stays up "
                    .. "while the scene changes under it, and a script takes it down on `SceneLoaded`.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "BillboardGui",
        Extends = "Instance",
        Module = "ui",
        Parents = OverAPart,
        Storage = "BillboardGui",
        Doc = "A UI tree hung in the world and turned to face the camera (F3): a name over a head, a health "
            .. "bar over a crate, a marker on an objective. Its children are laid out exactly as a "
            .. "`ScreenGui`'s are, against a canvas of `Size`, and drawn IN the world -- behind what is in "
            .. "front of it, in the same light as the picture around it -- rather than over it.\n\n"
            .. "It hangs over `Adornee`, or over its parent when that is a part and `Adornee` is empty. "
            .. "Its buttons are pressed like the screen's: the pointer's ray finds them in the world, "
            .. "and something solid in front hides them unless `AlwaysOnTop` is set. A screen element "
            .. "over the same pixel wins.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether it is laid out and drawn at all.",
            },
            {
                Name = "Adornee",
                Type = "(BasePart | Part2D)?",
                Doc = "The part it hangs over: a part in the world, or a `Part2D` on the plane -- a name over "
                    .. "a sprite, a health bar over a monster. Empty means its parent, when its parent is "
                    .. "one of those; a billboard with neither is not drawn.",
            },
            {
                Name = "Size",
                Type = "UDim2",
                Default = "UDim2.new(0, 200, 0, 50)",
                Doc = "How big it is. **The scale is in metres and the offset in pixels**, and the two mean "
                    .. "different things: a scale keeps its size in the world, so it shrinks with distance "
                    .. "like anything else there; an offset keeps its size on the screen, so a name tag "
                    .. "is as readable across the map as up close. The children lay out against the pixel "
                    .. "size either way.",
            },
            {
                Name = "WorldOffset",
                Type = "vector",
                Default = "vector.zero",
                Doc = "Where its centre sits relative to the adornee's centre, in metres and in the world's "
                    .. "own axes -- `vector.create(0, 3, 0)` is three metres above it however the part is "
                    .. "turned.",
            },
            {
                Name = "AlwaysOnTop",
                Type = "boolean",
                Default = "false",
                Doc = "Whether it shows through walls. Off, it is hidden by whatever is in front of it, like "
                    .. "anything in the world.",
            },
            {
                Name = "MaxDistance",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How far from the camera it stops being drawn, in metres. Zero means no limit.",
            },
            {
                Name = "Brightness",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How bright its colours are in the world's light, which is brighter than the screen's: "
                    .. "above one, it glows and blooms.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "SurfaceGui",
        Extends = "Instance",
        Module = "ui",
        Parents = under("BasePart"),
        Storage = "SurfaceGui",
        Doc = "A UI tree drawn onto one face of a part (F3): a screen on a wall, a sign, a scoreboard, a "
            .. "label on a crate. Its children are laid out exactly as a `ScreenGui`'s are, against a "
            .. "canvas the size of the face at `PixelsPerMetre`, and it moves, turns and is hidden with the "
            .. "part.\n\n"
            .. "It covers `Adornee`, or its parent when that is a part and `Adornee` is empty. Its "
            .. "buttons are pressed like the screen's, from in front of the face: the pointer's ray finds "
            .. "them in the world, and something solid in front hides them unless `AlwaysOnTop` is set.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether it is laid out and drawn at all.",
            },
            {
                Name = "Adornee",
                Type = "BasePart?",
                Doc = "The part it covers. Empty means its parent, when its parent is a part.",
            },
            {
                Name = "Face",
                Type = "Enum.Face",
                Default = "Enum.Face.Front",
                Doc = "Which face of the part it covers. `Front` is the one the part's `LookVector` comes out "
                    .. "of, and every side face is read the right way up.",
            },
            {
                Name = "PixelsPerMetre",
                Type = "number",
                Default = "50",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How many of the canvas's pixels fit in a metre of the face, which is what a child's "
                    .. "offset and a `TextSize` are measured in. Fifty makes 14-point text about a hand high "
                    .. "on a wall; more is finer text on the same face.",
            },
            {
                Name = "AlwaysOnTop",
                Type = "boolean",
                Default = "false",
                Doc = "Whether it shows through what is in front of the face.",
            },
            {
                Name = "Brightness",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How bright its colours are in the world's light: above one, a screen that glows.",
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
        Doc = "Anything that occupies a rectangle on screen (§2.2). Every property here is one the layout "
            .. "solver or the 2D pass reads, which is what makes it the base rather than a convenience.\n\n"
            .. "Coordinates are `UDim2`: a fraction of the parent plus a pixel offset, on each axis. That "
            .. "pair is what lets one layout be correct at every resolution, and it is why the reference "
            .. "images are recorded at two of them.",
        Properties = {
            {
                Name = "Position",
                Type = "UDim2",
                Default = "UDim2.new()",
                Doc = "Where this object's `AnchorPoint` sits inside its parent.",
            },
            {
                Name = "Size",
                Type = "UDim2",
                Default = "UDim2.new()",
                Doc = "How big it is, relative to its parent plus an offset. Overridden on an axis that "
                    .. "`AutomaticSize` covers.",
            },
            {
                Name = "AnchorPoint",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "Which point of THIS object `Position` places, in fractions of its own size. (0,0) is "
                    .. "its top-left corner and (0.5,0.5) its middle -- which is what makes centring "
                    .. "something one property rather than an arithmetic apology.",
            },
            {
                Name = "Rotation",
                Type = "number",
                Default = "0",
                Doc = "Degrees clockwise about the anchor point. It rotates the DRAWING and not the layout: a "
                    .. "rotated element still occupies its unrotated rectangle, which is what stops one "
                    .. "spinning icon reflowing a list every frame.\n\n**Descendants turn with it**, and a "
                    .. "descendant's own `Rotation` composes on top -- a dial rotated 30 degrees with a "
                    .. "needle rotated 90 draws the needle at 120, about the dial's anchor and then its "
                    .. "own.\n\nThree things the turn does not reach, all for the reason `UICorner` gives: "
                    .. "layout, which never sees it; the hit test, which stays the unrotated rectangle, "
                    .. "because a button you could see in one place and click in another is worse than one "
                    .. "that does not turn; and `ClipsDescendants`, whose rectangle stays axis-aligned, so a "
                    .. "turned child of a clipping parent is clipped by the parent's upright box.",
            },
            {
                Name = "BackgroundColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "The fill behind the object's own content. No `3` suffix (§2.5, divergence #11).",
            },
            {
                Name = "BackgroundTransparency",
                Type = "number",
                Default = "0",
                Doc = "0 opaque, 1 invisible. A fully transparent background still lays out; whether it takes "
                    .. "the pointer is `Active`'s to say.",
            },
            {
                Name = "Active",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this object takes the pointer: whether a press lands on it, whether the pointer "
                    .. "is over it, and whether a press there is the interface's -- the `uiConsumed` a game "
                    .. "reads from `InputService.InputBegan` -- or goes on to the game and to whatever is "
                    .. "under it.\n\n"
                    .. "**Until you write it, it is what the object is.** A button, a `TextInput` and a "
                    .. "`ScrollFrame` take the pointer. A `TextLabel` and an `ImageLabel` do not, so a press "
                    .. "on the icon or the caption inside a button is a press on the button. A `Frame` "
                    .. "takes it when it draws a background -- `BackgroundTransparency` under 1 -- so a veil "
                    .. "behind a dialog blocks what is under it, and a clear frame that only holds a layout "
                    .. "lets every press through. Write it and what you wrote stands: `true` on a clear "
                    .. "frame makes an invisible blocker, `false` on a button makes it something to look at.\n\n"
                    .. "**Never while `InputService.PointerLocked`**: a locked pointer is the game's, and nothing "
                    .. "takes it.",
            },
            {
                Name = "Visible",
                Type = "boolean",
                Default = "true",
                Doc = "Whether this object and its descendants are laid out, drawn and hit-tested. All three "
                    .. "together: an invisible element that still swallowed clicks is the defect this "
                    .. "sentence exists to rule out.",
            },
            {
                Name = "ZIndex",
                Type = "number",
                Default = "0",
                Doc = "Draw order within this ScreenGui, highest last, ties broken by document order. It does "
                    .. "not affect layout and does not nest: one flat ordering across the tree, because a "
                    .. "per-parent stacking context is the part of CSS nobody can hold in their head.",
            },
            {
                Name = "LayoutOrder",
                Type = "number",
                Default = "0",
                Doc = "Where this child sits in a `UIListLayout` whose `SortOrder` is `LayoutOrder`. Ignored "
                    .. "otherwise.",
            },
            {
                Name = "AutomaticSize",
                Type = "Enum.AutomaticSize",
                Default = "Enum.AutomaticSize.None",
                Doc = "Which axes size themselves to fit the contents, overriding that half of `Size`.",
            },
            {
                Name = "ClipsDescendants",
                Type = "boolean",
                Default = "false",
                Doc = "Whether descendants are clipped to this object's rectangle. What a scrolling list needs "
                    .. "and what a drop shadow must not have.",
            },
            {
                Name = "AbsolutePosition",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                Doc = "Where the solver put this object's top-left corner, in window pixels. Read-only "
                    .. "because it is an OUTPUT: writing it would be arguing with the layout rather than "
                    .. "changing it. Valid after the layout that produced it, which is the frame the "
                    .. "property was read in.",
            },
            {
                Name = "AbsoluteSize",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                Doc = "The size the solver gave it, in window pixels.",
            },
            {
                Name = "Selectable",
                Type = "boolean",
                Default = "false",
                Doc = "Whether a gamepad or the arrow keys can select this object (ADR 0128). **Until you "
                    .. "write it, it is what the object is**: a button and a `TextInput` are, and nothing "
                    .. "else is. The selected object is `UIService.SelectedObject`; the button that "
                    .. "activates fires its `Activated`.",
            },
            {
                Name = "NextSelectionUp",
                Type = "UIObject?",
                Doc = "What is selected when the selection moves up from this object. Nothing means the nearest "
                    .. "selectable object above it, found from where they are.",
            },
            {
                Name = "NextSelectionDown",
                Type = "UIObject?",
                Doc = "The same, moving down.",
            },
            {
                Name = "NextSelectionLeft",
                Type = "UIObject?",
                Doc = "The same, moving left.",
            },
            {
                Name = "NextSelectionRight",
                Type = "UIObject?",
                Doc = "The same, moving right.",
            },
            {
                Name = "SelectionImageObject",
                Type = "UIObject?",
                Doc = "What is drawn over this object while it is selected, in place of the default outline: its "
                    .. "background, picture, corners, gradient and strokes, at this object's box. Its children are "
                    .. "not drawn.",
            },
        } :: { Schema.Property },
        Events = {
            {
                Name = "Activated",
                Parameters = {},
                Doc = "Fired when a pointer press and release both land on this object. Device-neutral by "
                    .. "design (§2.5, divergence #17): a click, a tap and a bound gamepad button all produce "
                    .. "it, which is why there is no `MouseButton1Click`.",
            },
            {
                Name = "PointerEntered",
                Parameters = {},
                Doc = "Fired when the pointer moves onto this object. Replaces `MouseEnter`, and is named for "
                    .. "a pointer rather than a mouse because a finger is one too.",
            },
            {
                Name = "PointerExited",
                Parameters = {},
                Doc = "Fired when the pointer moves off it.",
            },
            {
                Name = "SelectionGained",
                Parameters = {},
                Doc = "This object became `UIService.SelectedObject`.",
            },
            {
                Name = "SelectionLost",
                Parameters = {},
                Doc = "It stopped being it.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "Frame",
        Extends = "UIObject",
        Module = "ui",
        Doc = "A rectangle and nothing else: a background, a border of children, and the layout it imposes on "
            .. "them. The element every UI is mostly made of.",
    },
    {
        Name = "ViewportFrame",
        Extends = "UIObject",
        Module = "ui",
        Storage = "ViewportFrame",
        Doc = "A UI element that draws the parts and models inside it (ADR 0107): an item turning in an "
            .. "inventory slot, a character preview, a 3D icon. **What is inside is its world and nothing "
            .. "else's**: drawn here and nowhere else, and not simulated -- no physics, no collisions -- so a "
            .. "script moves it by setting `CFrame`.\n\nDrawn only when something inside it, its camera or "
            .. "its size changes, so forty still items cost forty pictures once. Lit by its own simple light, "
            .. "with no sky: with `BackgroundTransparency = 1` only the instances show. It works in a "
            .. "`SurfaceGui` too.",
        Properties = {
            {
                Name = "CurrentCamera",
                Type = "Camera?",
                Doc = "A `Camera` inside the frame to look through. Nil frames what is inside automatically, "
                    .. "from the front and a little above.",
            },
            {
                Name = "Ambient",
                Type = "Color3",
                Default = "Color3.fromRGB(140, 140, 150)",
                Doc = "The light from every direction.",
            },
            {
                Name = "LightColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "The colour of its one light.",
            },
            {
                Name = "LightDirection",
                Type = "vector",
                Default = "vector.create(-1, -1, -1)",
                Doc = "Which way its one light travels -- down and away from the viewer by default.",
            },
        },
    },
    {
        Name = "TextLabel",
        Extends = "UIObject",
        Module = "ui",
        Storage = "TextLabel",
        Doc = "Text in a box (§2.2). Markup inside one label is `RichText`'s. There is no complex-script "
            .. "shaping and no kerning -- codepoints are laid out left to right by their advances. That is a "
            .. "known gap for Arabic, Devanagari and Thai, and the seam a text shaper would sit behind is "
            .. "deliberately left open.",
        Properties = {
            {
                Name = "Text",
                Type = "string",
                Default = '""',
                Doc = "What it says. UTF-8, and a codepoint the font has no glyph for draws the font's "
                    .. "`.notdef` box rather than nothing: a label that silently drops characters is the "
                    .. "failure mode that ships.",
            },
            {
                Name = "TextColor",
                Type = "Color3",
                Default = "Color3.new(0, 0, 0)",
                Doc = "No `3` suffix (§2.5, divergence #11).",
            },
            {
                Name = "TextTransparency",
                Type = "number",
                Default = "0",
                Doc = "How see-through the text is: 0 solid, 1 not drawn at all. The text's own, apart from "
                    .. "`BackgroundTransparency`, so a label can fade its words and keep its box, or the "
                    .. "other way round. Values outside 0 to 1 are kept as written and drawn as the nearer end.",
            },
            {
                Name = "TextSize",
                Type = "number",
                Default = "14",
                Doc = "The em size in pixels. An atlas is rasterized per font and integer size, so a "
                    .. "fractional one is rounded for rasterization and the layout still uses what was asked "
                    .. "for.",
            },
            {
                Name = "Font",
                Type = "Content",
                ContentKind = "Font",
                Default = '""',
                Doc = "The typeface, as an `asset://` URI to a TrueType file. Empty is the engine's own "
                    .. "default face -- **Inter**, OFL 1.1, vendored and staged beside the binary -- and so is "
                    .. "the literal name `Inter`.\n\nA name that cannot be resolved falls back to the default and "
                    .. "says so ONCE rather than once a frame: a label that vanished because its font name had "
                    .. "a typo is a bug report about the label. A build whose content directory has no font at "
                    .. "all falls back further, to a built-in vector face that is ASCII only, one weight and no "
                    .. "kerning -- text still draws, which is the point of having a fallback.\n\nGlyphs are "
                    .. "rasterised on demand and cached by face, size and codepoint, so a `TextSize` a tween is "
                    .. "animating costs one entry per quarter-pixel rather than one per frame.",
            },
            {
                Name = "TextXAlignment",
                Type = "Enum.HorizontalAlignment",
                Default = "Enum.HorizontalAlignment.Center",
                Doc = "Where the text sits across the box: `Left`, `Center` or `Right`. Named for the text, "
                    .. "because a layout's `HorizontalAlignment` places children and this places words; the "
                    .. "values are the same enum a layout uses.",
            },
            {
                Name = "TextYAlignment",
                Type = "Enum.VerticalAlignment",
                Default = "Enum.VerticalAlignment.Center",
                Doc = "Where the text sits down the box: `Top`, `Center` or `Bottom`.",
            },
            {
                Name = "TextWrapped",
                Type = "boolean",
                Default = "false",
                Doc = "Whether lines break at the box's width. Breaks at spaces, and mid-word only for a word "
                    .. "wider than the box -- a word cut in half at a random letter is worse than one that "
                    .. "overhangs.",
            },
            {
                Name = "RichText",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `Text` is read as markup, so one label can change colour, size and weight "
                    .. "part-way through (F3).\n\n"
                    .. "The tags: `<b>` bold, `<i>` italic, `<u>` underlined, `<s>` struck through, "
                    .. '`<font color="#rrggbb" size="20" transparency="0.5">` (any of the three, '
                    .. "`color` also as `rgb(r, g, b)`), and `<br/>` for a line break. Tags nest and each closes "
                    .. "its own. Write `&lt;`, `&gt;`, `&amp;`, `&quot;` and `&apos;` for the characters "
                    .. "themselves.\n\n"
                    .. "**A tag that is not understood is drawn as text**, so a typo shows as itself rather "
                    .. "than vanishing. Bold is drawn from the label's one face rather than a bold file, and "
                    .. "italic by leaning it. A `TextInput` shows its text plain whatever this says: its "
                    .. "caret counts characters as typed, tags and all.",
            },
            {
                Name = "TextScaled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `TextSize` is ignored and the text is rasterized at whatever size fills the "
                    .. "box. Re-rasterized rather than scaled: there is no distance field in v1, and a "
                    .. 'stretched bitmap is what "scaled text" usually means and looks like.',
            },
        } :: { Schema.Property },
    },
    {
        Name = "TextButton",
        Extends = "TextLabel",
        Module = "ui",
        Doc = "A `TextLabel` that is meant to be pressed. It adds no property: `Activated`, `PointerEntered` "
            .. "and `PointerExited` are on `UIObject` because anything can be pressed, and what makes this a "
            .. "button is that it is the class a reader recognizes as one.",
    },
    {
        Name = "TextInput",
        Extends = "TextLabel",
        Module = "ui",
        Storage = "TextInput",
        Doc = "An editable field (ADR 0139), on one line or on several: a chat box, a name, a server's "
            .. "address. It edits as every text field does -- selection with Shift and with the mouse, "
            .. "Ctrl+arrows and Ctrl+Backspace by words, Ctrl+A, Ctrl+C, Ctrl+X and Ctrl+V through the system's "
            .. "clipboard, Ctrl+Z and Ctrl+Y, a double press for a word and a triple for the line -- steps over "
            .. "whole characters as a reader sees them, and shows an input method's composition at the caret. "
            .. "While it has focus it takes the keyboard, so a player typing `w` does not walk.\n\n**A "
            .. "script's write to `Text` puts the caret at the end** and clears the undo history: what it "
            .. "would undo to is not the player's.",
        Properties = {
            {
                Name = "PlaceholderText",
                Type = "string",
                Default = '""',
                Doc = "Drawn in place of `Text` while it is empty and the field is unfocused.",
            },
            {
                Name = "PlaceholderColor",
                Type = "Color3",
                Default = "Color3.fromRGB(178, 178, 178)",
                Doc = "The placeholder's colour.",
            },
            {
                Name = "MultiLine",
                Type = "boolean",
                Default = "false",
                Doc = "Several lines: Return makes a new one and Ctrl+Return submits, the text wraps at the "
                    .. "field's width and scrolls to keep the caret in view, and Up and Down move between lines. "
                    .. "A single-line field takes a pasted line break as a space.",
            },
            {
                Name = "Editable",
                Type = "boolean",
                Default = "true",
                Doc = "False: read-only. It still takes focus, and its text can still be selected and copied -- "
                    .. "a code or an address shown to be copied.",
            },
            {
                Name = "MaxLength",
                Type = "number",
                Default = "0",
                Doc = "The most characters it holds; 0 is no limit. Characters as a reader sees them, so an "
                    .. "accented letter or an emoji is one. What does not fit is kept out and `InputRejected` "
                    .. "says what it was.",
            },
            {
                Name = "Masked",
                Type = "boolean",
                Default = "false",
                Doc = "Drawn as `MaskCharacter`s: a password. Copying and cutting give nothing away; pasting "
                    .. "still works. A phone raises its password keyboard.",
            },
            {
                Name = "MaskCharacter",
                Type = "string",
                Default = '"\u{2022}"',
                Doc = "What each character of a masked field is drawn as: its first character.",
            },
            {
                Name = "ClearTextOnFocus",
                Type = "boolean",
                Default = "false",
                Doc = "Focusing it empties it: a field that is typed afresh each time.",
            },
            {
                Name = "SelectAllOnFocus",
                Type = "boolean",
                Default = "false",
                Doc = "Focusing it selects all of it, so typing replaces what is there.",
            },
            {
                Name = "ReleaseFocusOnSubmit",
                Type = "boolean",
                Default = "true",
                Doc = "Return submits and lets go of the keyboard. False keeps it: a chat box, where the next "
                    .. "message follows. `Submitted` fires either way.",
            },
            {
                Name = "RevertOnEscape",
                Type = "boolean",
                Default = "false",
                Doc = "Escape puts back the text it had when it was focused, as it lets go.",
            },
            {
                Name = "KeyboardType",
                Type = "Enum.TextInputKeyboard",
                Default = "Enum.TextInputKeyboard.Default",
                Doc = "The on-screen keyboard a phone raises when it is focused.",
            },
            {
                Name = "CursorPosition",
                Type = "number",
                Default = "-1",
                -- Where a player's caret is, not something a scene says.
                Transient = true,
                Doc = "Where the caret is: 1-based, in bytes, as a Luau string's positions are -- `#Text + 1` "
                    .. "after the last character. -1 while it is not focused. Writing it moves the caret, snapped "
                    .. "to a character.",
            },
            {
                Name = "SelectionStart",
                Type = "number",
                Default = "-1",
                -- Where a player's caret is, not something a scene says.
                Transient = true,
                Doc = "The other end of the selection, in the same positions as `CursorPosition`; -1 when "
                    .. "nothing is selected. Writing it selects from there to the caret.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "CaptureFocus",
                Parameters = {},
                Returns = {},
                Doc = "Takes the keyboard, as a press on it would: another field loses it with "
                    .. "`Enum.FocusLossReason.Script`. Takes effect at the next frame.",
            },
            {
                Name = "ReleaseFocus",
                Parameters = { { Name = "submitted", Type = "boolean?" } },
                Returns = {},
                Doc = "Lets go of the keyboard; `FocusLost` fires with `submitted` (false when omitted) and "
                    .. "`Enum.FocusLossReason.Script`. Takes effect at the next frame.",
            },
            {
                Name = "IsFocused",
                Parameters = {},
                Returns = { "boolean" },
                Doc = "Whether it has the keyboard now.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Focused",
                Parameters = {},
                Doc = "Fired when this field starts receiving keystrokes. Focus is taken by a press and "
                    .. "released by a press elsewhere; there is deliberately no settable `Focused` property "
                    .. "beside this, because two fields could then both believe they had it.",
            },
            {
                Name = "FocusLost",
                Parameters = {
                    { Name = "submitted", Type = "boolean" },
                    { Name = "reason", Type = "Enum.FocusLossReason" },
                },
                Doc = "Fired when it stops. `submitted` is true when the field was left by pressing Return "
                    .. 'rather than by clicking away -- the difference between "the player finished" and '
                    .. '"the player went somewhere else"; `reason` says which of the four ways it was.',
            },
            {
                Name = "Submitted",
                Parameters = { { Name = "text", Type = "string" } },
                Doc = "Return was pressed, with the text as it was -- whether or not the field lets go "
                    .. "(`ReleaseFocusOnSubmit`).",
            },
            {
                Name = "TextChanged",
                Parameters = { { Name = "text", Type = "string" } },
                Doc = "The player changed the text -- typing, a paste, a cut, an undo -- with the text now. A "
                    .. 'script\'s own write to `Text` does not fire it; `GetPropertyChangedSignal("Text")` '
                    .. "fires for both.",
            },
            {
                Name = "InputRejected",
                Parameters = { { Name = "rejected", Type = "string" } },
                Doc = "What `MaxLength` kept out of an edit.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "ImageLabel",
        Extends = "UIObject",
        Module = "ui",
        Storage = "ImageLabel",
        Doc = "A picture in a box (§2.2).",
        Properties = {
            {
                Name = "Image",
                Type = "Content",
                ContentKind = "Texture",
                Default = '""',
                Doc = "The picture, as an `asset://` URI. A compiled texture out of a pack or an ordinary "
                    .. "PNG out of a source tree; both work, because dev mode is the mode people spend their "
                    .. "time in.\n\nEmpty draws nothing but still fills the background. A URI that names "
                    .. "nothing, and a picture that has been asked for but has not finished loading, both draw "
                    .. "as a flat `ImageColor` rectangle -- the same thing, because from a frame's point of "
                    .. "view they are.",
            },
            {
                Name = "ImageColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "Multiplied into the image, so one white glyph sheet can be drawn in any colour. White "
                    .. "leaves it alone.",
            },
            {
                Name = "ImageTransparency",
                Type = "number",
                Default = "0",
                Doc = "How see-through the picture is: 0 solid, 1 not drawn at all. The picture's own, apart "
                    .. "from `BackgroundTransparency`, as `TextTransparency` is a label's words' -- an icon "
                    .. "fades and its box stays, or the other way round. Values outside 0 to 1 are kept as "
                    .. "written and drawn as the nearer end.",
            },
            {
                Name = "ScaleType",
                Type = "Enum.ScaleType",
                Default = "Enum.ScaleType.Stretch",
                Doc = "How the picture fits the box.\n\n`Tile` is BOUNDED: past about four thousand "
                    .. "tiles the picture stretches instead, because a one-pixel image tiled over a full-screen "
                    .. "frame is nearly a million quads and the frame that built them would be the hitch. A "
                    .. "visibly wrong picture is one somebody can find; a frame that stops responding is not.",
            },
            {
                Name = "SliceCenter",
                Type = "Rect",
                Default = "Rect.new(Vector2.zero, Vector2.zero)",
                Doc = "For `ScaleType.Slice`, where the nine-slice cuts are, in SOURCE pixels. The four "
                    .. "corners are drawn at their own size whatever the box is, which is how a panel keeps "
                    .. "its rounded corners.\n\nA box narrower than its own two corners collapses the "
                    .. "middle rather than drawing the corners on top of each other.\n\nAn inverted or empty rectangle is kept as given rather than "
                    .. "corrected: a slice quietly fixed up is a panel that draws wrong with no way to find out "
                    .. "why.",
            },
            {
                Name = "ImageRectOffset",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "Where the part of the picture to show starts, in source pixels from its top-left "
                    .. "corner: one frame of a sprite sheet, one icon of an atlas. Read with `ImageRectSize`.",
            },
            {
                Name = "ImageRectSize",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "How much of the picture to show, in source pixels from `ImageRectOffset`. Zero is the "
                    .. "whole picture. **A negative size reads the rectangle backwards** -- mirrored across "
                    .. "that axis -- which is what a mirror's picture needs (ADR 0107).",
            },
        } :: { Schema.Property },
    },
    {
        Name = "ImageButton",
        Extends = "ImageLabel",
        Module = "ui",
        Doc = "An `ImageLabel` that is meant to be pressed, and the same argument `TextButton` carries: it "
            .. "adds nothing, and it is the class a reader recognizes.",
    },
    {
        Name = "ScrollFrame",
        Extends = "UIObject",
        Module = "ui",
        Storage = "ScrollFrame",
        Doc = "A `Frame` whose contents can be larger than it is (§2.2). It clips its descendants whatever "
            .. "`ClipsDescendants` says -- a scrolling region that did not clip would not be one.",
        Properties = {
            {
                Name = "CanvasSize",
                Type = "UDim2",
                Default = "UDim2.new()",
                Doc = "How big the scrollable area is. Children lay out against this rather than against the "
                    .. "frame's own size.",
            },
            {
                Name = "CanvasPosition",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "How far the canvas is scrolled, in pixels. Clamped to what the canvas allows on the "
                    .. "next layout, so a write past the end settles at the end rather than showing "
                    .. "emptiness.",
            },
            {
                Name = "ScrollBarThickness",
                Type = "number",
                Default = "12",
                Doc = "In pixels; zero draws no bar and still scrolls, which is what a touch surface wants."
                    .. "\n\nA bar appears only on an axis that can actually move: a canvas that fits its "
                    .. "region gets none, because a bar for an axis with nowhere to go is a bar that lies. The "
                    .. "thumb is as long a fraction of the track as the view is of the canvas, and never "
                    .. "shorter than the bar is wide -- a thumb of two pixels in a very long canvas is a thumb "
                    .. "nobody can grab.\n\nBoth parts take their colour from the frame's own "
                    .. "`BackgroundColor`, at a quarter and three quarters alpha. v1 has no theme, and a "
                    .. "hard-coded grey would be wrong on half of them.\n\nThe bar is drawn OUTSIDE "
                    .. "the region's own clip, or it would scroll away with the content it reports on.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIListLayout",
        Extends = "Instance",
        Module = "ui",
        Parents = InAUi,
        Storage = "UIListLayout",
        Doc = "Stacks its parent's `UIObject` children in a line (§2.2). A modifier rather than a container: "
            .. "it is parented BESIDE the things it arranges, so adding one to a Frame does not reparent "
            .. "anything.\n\nA child laid out by one keeps its `Size` and loses its `Position` -- the layout "
            .. "decides where each one goes, which is what a layout is.",
        Properties = {
            {
                Name = "FillDirection",
                Type = "Enum.FillDirection",
                Default = "Enum.FillDirection.Vertical",
                Doc = "Which way the line runs.",
            },
            {
                Name = "Padding",
                Type = "UDim",
                Default = "UDim.new()",
                Doc = "The gap BETWEEN children, not around them -- `UIPadding` is what puts space around the "
                    .. "outside. Scale is a fraction of the parent along the fill direction.",
            },
            {
                Name = "HorizontalAlignment",
                Type = "Enum.HorizontalAlignment",
                Default = "Enum.HorizontalAlignment.Left",
                Doc = "Where the line sits across the parent.",
            },
            {
                Name = "VerticalAlignment",
                Type = "Enum.VerticalAlignment",
                Default = "Enum.VerticalAlignment.Top",
                Doc = "Where it sits down the parent.",
            },
            {
                Name = "SortOrder",
                Type = "Enum.SortOrder",
                Default = "Enum.SortOrder.LayoutOrder",
                Doc = "What order the children go in.",
            },
            {
                Name = "Wraps",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the line breaks into a second one when it runs out of room, rather than "
                    .. "overflowing.",
            },
            {
                Name = "HorizontalFlex",
                Type = "Enum.UIFlexAlignment",
                Default = "Enum.UIFlexAlignment.None",
                Doc = "What happens to room left over across the parent: nothing, the children grow into it, or "
                    .. "it is shared out as space between them.",
            },
            {
                Name = "VerticalFlex",
                Type = "Enum.UIFlexAlignment",
                Default = "Enum.UIFlexAlignment.None",
                Doc = "The same, down the parent.",
            },
            {
                Name = "ItemLineAlignment",
                Type = "Enum.ItemLineAlignment",
                Default = "Enum.ItemLineAlignment.Automatic",
                Doc = "Where each child sits across its line; `Stretch` makes them all as tall as a row or as "
                    .. "wide as a column. A child's own `UIFlexItem` can say otherwise.",
            },
            {
                Name = "AbsoluteContentSize",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                -- What the interface found, not what the world is.
                HostFact = true,
                Doc = "How much room the children take, in pixels, as the last layout found it. What a "
                    .. "`ScrollFrame`'s `CanvasSize` is set from.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIGridLayout",
        Extends = "Instance",
        Module = "ui",
        Parents = InAUi,
        Storage = "UIGridLayout",
        Doc = "Arranges its parent's `UIObject` children in a grid of equal cells (ADR 0128): an "
            .. "inventory, a shop, a level select. A modifier, like `UIListLayout`: parented beside what "
            .. "it arranges.\n\n"
            .. "A child laid out by one loses its `Position` and its `Size` -- every cell is `CellSize`. "
            .. "The cells fill a line until the next would not fit, or until `FillDirectionMaxCells`, and "
            .. "then start another.",
        Properties = {
            {
                Name = "CellSize",
                Type = "UDim2",
                Default = "UDim2.new(0, 100, 0, 100)",
                Doc = "The size of every cell. Scale is a fraction of the parent, so `UDim2.fromScale(0.25, "
                    .. "0.25)` is four across at any window size.",
            },
            {
                Name = "CellPadding",
                Type = "UDim2",
                Default = "UDim2.new(0, 5, 0, 5)",
                Doc = "The gap between cells, across and down. Between, not around: `UIPadding` puts space around "
                    .. "the outside.",
            },
            {
                Name = "FillDirection",
                Type = "Enum.FillDirection",
                Default = "Enum.FillDirection.Horizontal",
                Doc = "Which way a line runs: `Horizontal` fills rows, `Vertical` fills columns.",
            },
            {
                Name = "FillDirectionMaxCells",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.whole_number_at_least_zero",
                Doc = "The most cells in one line; zero means as many as fit. A line never holds more than fit, "
                    .. "whatever this says.",
            },
            {
                Name = "StartCorner",
                Type = "Enum.StartCorner",
                Default = "Enum.StartCorner.TopLeft",
                Doc = "The corner the first cell goes in, and so which way the lines run.",
            },
            {
                Name = "SortOrder",
                Type = "Enum.SortOrder",
                Default = "Enum.SortOrder.LayoutOrder",
                Doc = "What order the children go in.",
            },
            {
                Name = "HorizontalAlignment",
                Type = "Enum.HorizontalAlignment",
                Default = "Enum.HorizontalAlignment.Left",
                Doc = "Where the whole grid sits across the parent.",
            },
            {
                Name = "VerticalAlignment",
                Type = "Enum.VerticalAlignment",
                Default = "Enum.VerticalAlignment.Top",
                Doc = "Where it sits down the parent.",
            },
            {
                Name = "AbsoluteContentSize",
                Type = "Vector2",
                ReadOnly = true,
                Transient = true,
                -- What the interface found, not what the world is.
                HostFact = true,
                Doc = "How much room the cells take, in pixels, as the last layout found it. What a "
                    .. "`ScrollFrame`'s `CanvasSize` is set from.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIPageLayout",
        Extends = "Instance",
        Module = "ui",
        Parents = InAUi,
        Storage = "UIPageLayout",
        Doc = "Lays its parent's `UIObject` children side by side as pages and shows one (ADR 0128): a "
            .. "tutorial, a set of tabs, a carousel. The others are beside it, out of the parent's box -- "
            .. "turn `ClipsDescendants` on, or they show.\n\n"
            .. "A page keeps its `Size` and loses its `Position`. `Next`, `Previous`, `JumpTo` and "
            .. "`JumpToIndex` turn the page, and so do a swipe, the wheel and a gamepad's shoulder buttons "
            .. "unless they are turned off. `CurrentPage` changes at once; the pages slide for `TweenTime` "
            .. "when `Animated`.",
        Properties = {
            {
                Name = "Animated",
                Type = "boolean",
                Default = "true",
                Doc = "Whether a page turn slides. Off, the new page is there at once.",
            },
            {
                Name = "Circular",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the page after the last is the first, and the one before the first the last.",
            },
            {
                Name = "EasingStyle",
                Type = "Enum.EasingStyle",
                Default = "Enum.EasingStyle.Quad",
                Doc = "The shape of the slide.",
            },
            {
                Name = "EasingDirection",
                Type = "Enum.EasingDirection",
                Default = "Enum.EasingDirection.Out",
                Doc = "Which end of the slide is eased.",
            },
            {
                Name = "TweenTime",
                Type = "number",
                Default = "0.3",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How long a slide takes, in seconds.",
            },
            {
                Name = "Padding",
                Type = "UDim",
                Default = "UDim.new()",
                Doc = "The gap between pages. Scale is a fraction of the parent along the fill direction.",
            },
            {
                Name = "FillDirection",
                Type = "Enum.FillDirection",
                Default = "Enum.FillDirection.Horizontal",
                Doc = "Which way the pages run, and so which way they slide.",
            },
            {
                Name = "SortOrder",
                Type = "Enum.SortOrder",
                Default = "Enum.SortOrder.LayoutOrder",
                Doc = "What order the children go in.",
            },
            {
                Name = "HorizontalAlignment",
                Type = "Enum.HorizontalAlignment",
                Default = "Enum.HorizontalAlignment.Center",
                Doc = "Where the page that is shown sits across the parent.",
            },
            {
                Name = "VerticalAlignment",
                Type = "Enum.VerticalAlignment",
                Default = "Enum.VerticalAlignment.Center",
                Doc = "Where it sits down the parent.",
            },
            {
                Name = "ScrollWheelInputEnabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the wheel over the pages turns them.",
            },
            {
                Name = "TouchInputEnabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether a swipe across the pages turns them -- a finger's, or a mouse dragged.",
            },
            {
                Name = "GamepadInputEnabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether a gamepad's shoulder buttons turn them.",
            },
            {
                Name = "CurrentPage",
                Type = "UIObject?",
                ReadOnly = true,
                Transient = true,
                -- What the interface found, not what the world is.
                HostFact = true,
                Doc = "The page that is shown, or the one being slid to. Nothing when there are no pages.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Next",
                Parameters = {},
                Returns = {},
                Doc = "Turns to the page after `CurrentPage`. At the last page it does nothing, unless "
                    .. "`Circular`.",
            },
            {
                Name = "Previous",
                Parameters = {},
                Returns = {},
                Doc = "Turns to the page before `CurrentPage`. At the first it does nothing, unless `Circular`.",
            },
            {
                Name = "JumpTo",
                Parameters = { { Name = "page", Type = "UIObject" } },
                Returns = {},
                Doc = "Turns to `page`. An object that is not one of the pages is refused.",
            },
            {
                Name = "JumpToIndex",
                Parameters = { { Name = "index", Type = "number" } },
                Returns = {},
                Doc = "Turns to the page at `index`, counted from 0 in the layout's order. Past either end it "
                    .. "stops at that end, or goes round when `Circular`.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "PageEnter",
                Parameters = { { Name = "page", Type = "UIObject" } },
                Doc = "`page` became `CurrentPage`: it is being turned to.",
            },
            {
                Name = "PageLeave",
                Parameters = { { Name = "page", Type = "UIObject" } },
                Doc = "`page` stopped being `CurrentPage`.",
            },
            {
                Name = "Stopped",
                Parameters = { { Name = "page", Type = "UIObject" } },
                Doc = "The slide ended with `page` shown. At once when the layout is not `Animated`.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "UIFlexItem",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UIFlexItem",
        Doc = "Says how its parent behaves as a child of a `UIListLayout` (ADR 0128): whether it grows "
            .. "into room that is left over, whether it shrinks when the line is too long, and where it "
            .. "sits across the line. Parented to the child it is about, not beside it.",
        Properties = {
            {
                Name = "FlexMode",
                Type = "Enum.UIFlexMode",
                Default = "Enum.UIFlexMode.None",
                Doc = "Whether it grows, shrinks, or both.",
            },
            {
                Name = "GrowRatio",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Its share of the room left over, against the other children that grow, when `FlexMode` is "
                    .. "`Custom`: one at 2 beside one at 1 takes two thirds.",
            },
            {
                Name = "ShrinkRatio",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Its share of what the line is short by, against the others that shrink, when `FlexMode` is "
                    .. "`Custom`.",
            },
            {
                Name = "ItemLineAlignment",
                Type = "Enum.ItemLineAlignment",
                Default = "Enum.ItemLineAlignment.Automatic",
                Doc = "Where it sits across its line; `Automatic` is as the layout says.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIScale",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UIScale",
        Doc = "Scales its parent and everything inside it (ADR 0128): what is drawn, and where a press "
            .. "lands. A window that pops open, a HUD a player can make larger.\n\n"
            .. "The element takes `Scale` times its size -- a list it is in makes room for that -- and "
            .. "everything inside it is laid out as if nothing had changed and then scaled with it: "
            .. "offsets, text, corners and strokes alike. A tween can drive it.",
        Properties = {
            {
                Name = "Scale",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "What the element and its descendants are multiplied by.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIAspectRatioConstraint",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UIAspectRatioConstraint",
        Doc = "Keeps its parent a fixed shape (ADR 0128): a square slot, a 16:9 picture, whatever the "
            .. "window does. Applied after the element's own `Size` and before its children are laid out.",
        Properties = {
            {
                Name = "AspectRatio",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_positive",
                Doc = "Width over height: 1 is a square, 2 is twice as wide as tall.",
            },
            {
                Name = "AspectType",
                Type = "Enum.AspectType",
                Default = "Enum.AspectType.FitWithinMaxSize",
                Doc = "What it is measured against: the element's own size, or its parent's.",
            },
            {
                Name = "DominantAxis",
                Type = "Enum.DominantAxis",
                Default = "Enum.DominantAxis.Width",
                Doc = "Which axis takes the parent's extent under `ScaleWithParentSize`.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UISizeConstraint",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UISizeConstraint",
        Doc = "Keeps its parent between two sizes (ADR 0128): a panel that is a third of the window and "
            .. "never under 240 units wide. A clamp, applied after the element's own `Size` and before its "
            .. "children are laid out.",
        Properties = {
            {
                Name = "MinSize",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "The least it may be, in the element's units.",
            },
            {
                Name = "MaxSize",
                Type = "Vector2",
                Default = "Vector2.zero",
                Doc = "The most it may be. **Zero on an axis is no limit**, which is what a new one has; "
                    .. "`math.huge` is taken as the same and reads back as zero.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UITextSizeConstraint",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UITextSizeConstraint",
        Doc = "Keeps scaled text between two sizes (ADR 0128): a `TextScaled` label that never falls "
            .. "under what can be read nor grows past what looks right. Nothing on a label whose text is "
            .. "not scaled -- that one is `TextSize`, as written.",
        Properties = {
            {
                Name = "MinTextSize",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The least the text may be, in the element's units.",
            },
            {
                Name = "MaxTextSize",
                Type = "number",
                Default = "100",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The most it may be.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIDragDetector",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UIDragDetector",
        Doc = "Makes its parent something a pointer drags (ADR 0128): a window by its title bar, a "
            .. "slider's thumb, a dial. A press on the parent or anything inside it that is then moved "
            .. "drags it, by mouse or by finger; a press that does not move is still a press.\n\n"
            .. "`DragStyle` says how it moves and `ResponseStyle` what is written. The detector writes the "
            .. "parent's `Position` (or `Rotation`), so a parent inside a layout, which has no position of "
            .. "its own, is not moved.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether a press starts a drag.",
            },
            {
                Name = "DragStyle",
                Type = "Enum.UIDragDetectorDragStyle",
                Default = "Enum.UIDragDetectorDragStyle.TranslatePlane",
                Doc = "How the pointer's motion becomes the element's.",
            },
            {
                Name = "ResponseStyle",
                Type = "Enum.UIDragDetectorResponseStyle",
                Default = "Enum.UIDragDetectorResponseStyle.Offset",
                Doc = "What is written as it is dragged.",
            },
            {
                Name = "DragAxis",
                Type = "Vector2",
                Default = "Vector2.new(1, 0)",
                Doc = "The line a `TranslateLine` drag slides along, y down: `(1, 0)` is across, `(0, 1)` is "
                    .. "down.",
            },
            {
                Name = "BoundingUI",
                Type = "UIObject?",
                Doc = "An element the dragged one stays inside. Nothing is no bound.",
            },
            {
                Name = "MinDragTranslation",
                Type = "UDim2",
                Default = "UDim2.new()",
                Doc = "The least the element may be moved from where the drag began. Not applied while it equals "
                    .. "`MaxDragTranslation`, which is how a new detector has no limit.",
            },
            {
                Name = "MaxDragTranslation",
                Type = "UDim2",
                Default = "UDim2.new()",
                Doc = "The most it may be moved.",
            },
            {
                Name = "MinDragAngle",
                Type = "number",
                Default = "0",
                Doc = "The least a `Rotate` drag may turn it, in degrees. Not applied while it equals "
                    .. "`MaxDragAngle`.",
            },
            {
                Name = "MaxDragAngle",
                Type = "number",
                Default = "0",
                Doc = "The most it may turn it.",
            },
            {
                Name = "DragUDim2",
                Type = "UDim2",
                ReadOnly = true,
                Transient = true,
                -- What the interface found, not what the world is.
                HostFact = true,
                Doc = "How far the drag has moved the element from where it began, as `ResponseStyle` measures "
                    .. "it.",
            },
            {
                Name = "DragRotation",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                -- What the interface found, not what the world is.
                HostFact = true,
                Doc = "How far a `Rotate` drag has turned it from where it began, in degrees.",
            },
        } :: { Schema.Property },
        Events = {
            {
                Name = "DragStart",
                Parameters = { { Name = "position", Type = "Vector2" } },
                Doc = "A press on the element began to move it. `position` is the pointer's, in pixels.",
            },
            {
                Name = "DragContinue",
                Parameters = { { Name = "position", Type = "Vector2" } },
                Doc = "The pointer moved while dragging.",
            },
            {
                Name = "DragEnd",
                Parameters = { { Name = "position", Type = "Vector2" } },
                Doc = "The press was released, or the element or the detector went away.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "CanvasGroup",
        Extends = "Frame",
        Module = "ui",
        Storage = "CanvasGroup",
        Doc = "A `Frame` whose descendants are drawn together and then shown as one picture (ADR 0128): a "
            .. "window that fades as a whole, instead of its parts fading through each other. "
            .. "`GroupTransparency` and `GroupColor` apply to the picture.\n\n"
            .. "The picture is redrawn only when something inside it changes, and it clips: nothing inside "
            .. "a group is drawn outside its box.",
        Properties = {
            {
                Name = "GroupTransparency",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How see-through the group is, as one: 0 is solid and 1 is not drawn.",
            },
            {
                Name = "GroupColor",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "A colour the whole picture is multiplied by. White changes nothing.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIPadding",
        Extends = "Instance",
        Module = "ui",
        Parents = InAUi,
        Storage = "UIPadding",
        Doc = "Insets its parent's content on each side (§2.2). Like `UIListLayout`, a modifier parented "
            .. "beside what it affects.",
        Properties = {
            {
                Name = "PaddingTop",
                Type = "UDim",
                Default = "UDim.new()",
                Doc = "Scale is a fraction of the parent's own height.",
            },
            {
                Name = "PaddingBottom",
                Type = "UDim",
                Default = "UDim.new()",
                Doc = "Scale is a fraction of the parent's own height.",
            },
            {
                Name = "PaddingLeft",
                Type = "UDim",
                Default = "UDim.new()",
                Doc = "Scale is a fraction of the parent's own width.",
            },
            {
                Name = "PaddingRight",
                Type = "UDim",
                Default = "UDim.new()",
                Doc = "Scale is a fraction of the parent's own width.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UICorner",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UICorner",
        Doc = "Rounds its parent's corners (§2.2). It changes the DRAWING and not the layout or the hit test: "
            .. "a rounded button is still a rectangle to the solver and to the pointer, which is what keeps "
            .. "a corner radius from being a geometry problem.",
        Properties = {
            {
                Name = "CornerRadius",
                Type = "UDim",
                Default = "UDim.new(0, 8)",
                Doc = "Scale is a fraction of the parent's SHORTER side, so a pill shape is "
                    .. "`UDim.new(0.5, 0)` whatever the box's proportions.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIGradient",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject", "UIStroke"),
        Storage = "UIGradient",
        Doc = "Colours and fades its parent (ADR 0110): what the parent draws -- its background, its picture, its "
            .. "text -- is multiplied by `Color` and faded by `Transparency` along the gradient. Under a "
            .. "`UIStroke` it colours the stroke instead. Like `UICorner`, it changes the drawing and not the "
            .. "layout or the hit test; the first one under a parent is the one that counts.",
        Properties = {
            {
                Name = "Color",
                Type = "ColorSequence",
                Default = "ColorSequence.new(Color3.new(1, 1, 1))",
                Doc = "The colours along the gradient, multiplied into the parent's own. White changes nothing.",
            },
            {
                Name = "Transparency",
                Type = "NumberSequence",
                Default = "NumberSequence.new(0)",
                Doc = "The see-through along the gradient: 0 leaves the parent as opaque as it was, 1 hides it.",
            },
            {
                Name = "Offset",
                Type = "Vector2",
                Default = "Vector2.new(0, 0)",
                Doc = "Moves the gradient's centre by fractions of the parent's size: (1, 0) is a whole width to the "
                    .. "right. Negative values are legal.",
            },
            {
                Name = "Rotation",
                Type = "number",
                Default = "0",
                Doc = "Degrees, clockwise. A linear gradient at 0 runs left to right; a conical one starts pointing "
                    .. "right. A radial gradient ignores it.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Off draws the parent as though it had no gradient.",
            },
            {
                Name = "Type",
                Type = "Enum.GradientType",
                Default = "Enum.GradientType.Linear",
                Doc = "The gradient's shape: a line across the parent, a circle out from its centre, or a sweep "
                    .. "round it.",
            },
            {
                Name = "TileMode",
                Type = "Enum.GradientTileMode",
                Default = "Enum.GradientTileMode.Clamp",
                Doc = "What fills the space past the sequence's end when `Scale` is below 1 or `Offset` moves it: "
                    .. "the end colours, the sequence again, or the sequence mirrored.",
            },
            {
                Name = "Scale",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_positive",
                Doc = "How much of the parent one run of the sequence spans: 1 is once across; 0.25 is a quarter, "
                    .. "the rest filled per `TileMode`. For a radial gradient it scales the radius, for a conical "
                    .. "one the sweep (never more than a turn).",
            },
        } :: { Schema.Property },
    },
    {
        Name = "UIStroke",
        Extends = "Instance",
        Module = "ui",
        Parents = under("UIObject"),
        Storage = "UIStroke",
        Doc = "An outline on its parent's text or border (ADR 0110). On a text object it outlines the text unless "
            .. "`ApplyStrokeMode` says `Border`; on anything else, the border. A `UIGradient` under it colours "
            .. "the stroke. Several border strokes may share an element, ordered by `ZIndex`; a text object takes "
            .. "one text stroke. A stroke never changes the layout, so an outer one can overlap a neighbour.",
        Properties = {
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(0, 0, 0)",
                Doc = "The stroke's colour.",
            },
            {
                Name = "Thickness",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Pixels, or with `StrokeSizingMode.ScaledSize` a fraction of the parent's shorter side (of "
                    .. "the font size, on text). Animating it on text makes a new outline for every size it passes "
                    .. "through, so prefer a fixed one there.",
            },
            {
                Name = "Transparency",
                Type = "number",
                Default = "0",
                Doc = "The stroke's own see-through, apart from its parent's: a stroke on text whose "
                    .. "`TextTransparency` is 1 is hollow lettering.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Off draws nothing.",
            },
            {
                Name = "ApplyStrokeMode",
                Type = "Enum.ApplyStrokeMode",
                Default = "Enum.ApplyStrokeMode.Contextual",
                Doc = "`Contextual` outlines the text of a text object and the border of anything else; `Border` "
                    .. "always outlines the border.",
            },
            {
                Name = "LineJoinMode",
                Type = "Enum.LineJoinMode",
                Default = "Enum.LineJoinMode.Round",
                Doc = "How a corner is turned: rounded, cut off, or brought to a point. A `UICorner` on the parent "
                    .. "makes a border stroke round whatever this says.",
            },
            {
                Name = "StrokeSizingMode",
                Type = "Enum.StrokeSizingMode",
                Default = "Enum.StrokeSizingMode.FixedSize",
                Doc = "Whether `Thickness` is pixels or a fraction of the parent.",
            },
            {
                Name = "BorderStrokePosition",
                Type = "Enum.BorderStrokePosition",
                Default = "Enum.BorderStrokePosition.Outer",
                Doc = "Where on the border a border stroke lies: wholly outside it, centred on it, or wholly inside.",
            },
            {
                Name = "BorderOffset",
                Type = "UDim",
                Default = "UDim.new(0, 0)",
                Doc = "Moves a border stroke outwards (inwards when negative). Scale is a fraction of the parent's "
                    .. "shorter side. Text strokes ignore it.",
            },
            {
                Name = "ZIndex",
                Type = "number",
                Default = "1",
                Doc = "The order among the element's strokes: lower draws behind higher, and equal ones draw in "
                    .. "child order.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "AudioGroup",
        Extends = "Instance",
        Module = "audio",
        Storage = "AudioGroup",
        Doc = "A mixing bus (§2.1, §2.2). A `Sound` whose `Group` names one has its volume multiplied by the "
            .. 'group\'s, which is how "turn the music down" is one property rather than a loop over every '
            .. "music track.\n\nNamed `AudioGroup` rather than `SoundGroup` because "
            .. "the service is `AudioService`, and one prefix across a family beats two.",
        Properties = {
            {
                Name = "Volume",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "A multiplier, not a level in decibels: 0 is silent, 1 is unchanged, and above 1 is "
                    .. "louder and will clip if the material was already near full scale. Nesting is not "
                    .. "supported -- a group parented to a group is just an instance parented to an instance, "
                    .. "and only a `Sound`'s own `Group` is read.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "SoundEffect",
        Extends = "Instance",
        Module = "audio",
        Tags = { "Abstract", "NotCreatable" } :: { Schema.ClassTag },
        Doc = "Something done to a sound on its way to the speakers (ADR 0131). Parent one to a `Sound` and that "
            .. "sound goes through it; parent one to an `AudioGroup` and everything in the group does, mixed "
            .. "together first. Several under one parent are applied in the order of their `Priority`.\n\n"
            .. "**What comes out of the speakers is downstream of the simulation**: an effect changes nothing a "
            .. "script can read, and a sound is as long with one as without. An effect on a `Sound` stops with "
            .. "the sound; one on an `AudioGroup` rings on after it -- which is where a reverb belongs.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether the sound goes through it.",
            },
            {
                Name = "Priority",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "Its place among the effects under one parent: lower is applied first. Equal ones are "
                    .. "applied in the order they were made.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "ReverbSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "ReverbSoundEffect",
        Doc = "The sound of a room: what is played comes back from the walls. A hall, a cave, a bathroom.",
        Properties = {
            {
                Name = "RoomSize",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How large the room is, 0 to 1: larger rings for longer.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How quickly the high notes die in it, 0 to 1: a stone hall is low, a carpeted room high.",
            },
            {
                Name = "WetLevel",
                Type = "number",
                Default = "0.3",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How loud the room's answer is, 0 to 1.",
            },
            {
                Name = "DryLevel",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How loud the sound itself stays, 0 to 1.",
            },
            {
                Name = "Width",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How far apart the room's answer is spread across the two speakers.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "EchoSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "EchoSoundEffect",
        Doc = "What is played, again after a moment, and again more quietly: a canyon, a tunnel, a dub " .. "delay.",
        Properties = {
            {
                Name = "Delay",
                Type = "number",
                Default = "0.3",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How long until the echo, in seconds, 0.01 to 2.",
            },
            {
                Name = "Feedback",
                Type = "number",
                Default = "0.4",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How much of each echo is echoed again, 0 to 0.95: zero is one echo, more is a trail of them.",
            },
            {
                Name = "WetLevel",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How loud the echoes are, 0 to 1.",
            },
            {
                Name = "DryLevel",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How loud the sound itself stays, 0 to 1.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "EqualizerSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "EqualizerSoundEffect",
        Doc = "Three tone controls: the low notes, the middle and the high, each turned up or down. A radio "
            .. "is no lows and no highs; a voice through a wall is lows alone.",
        Properties = {
            {
                Name = "LowGain",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The low band's level, in decibels, -80 to 12.",
            },
            {
                Name = "MidGain",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The middle band's level, in decibels, -80 to 12.",
            },
            {
                Name = "HighGain",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The high band's level, in decibels, -80 to 12.",
            },
            {
                Name = "MidLow",
                Type = "number",
                Default = "400",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "Where the middle band begins, in hertz. Below it is the low band.",
            },
            {
                Name = "MidHigh",
                Type = "number",
                Default = "4000",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "Where the middle band ends, in hertz. Above it is the high band.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "LowPassSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "LowPassSoundEffect",
        Doc = "Lets the low notes through and takes the high ones away: under water, behind a door, a "
            .. "paused game.",
        Properties = {
            {
                Name = "Cutoff",
                Type = "number",
                Default = "2000",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The frequency above which sound is taken away, in hertz.",
            },
            {
                Name = "Resonance",
                Type = "number",
                Default = "0.707",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How much the notes right at the cutoff are raised, 0.1 to 10: 0.707 is flat, more whistles.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "HighPassSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "HighPassSoundEffect",
        Doc = "Lets the high notes through and takes the low ones away: a telephone, a tinny speaker.",
        Properties = {
            {
                Name = "Cutoff",
                Type = "number",
                Default = "500",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The frequency below which sound is taken away, in hertz.",
            },
            {
                Name = "Resonance",
                Type = "number",
                Default = "0.707",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How much the notes right at the cutoff are raised, 0.1 to 10: 0.707 is flat, more whistles.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "DistortionSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "DistortionSoundEffect",
        Doc = "Pushes the sound until it breaks up: a megaphone, a blown speaker, an electric guitar.",
        Properties = {
            {
                Name = "Level",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How hard it is pushed, 0 (untouched) to 1.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "CompressorSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "CompressorSoundEffect",
        Doc = "Turns the loud parts down so the whole can be turned up: speech that stays audible under "
            .. "music, an explosion that does not drown the room.",
        Properties = {
            {
                Name = "Threshold",
                Type = "number",
                Default = "-20",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "The level above which it turns down, in decibels, -60 to 0.",
            },
            {
                Name = "Ratio",
                Type = "number",
                Default = "4",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How much it turns down what is over the threshold: at 4, four decibels over come out as one.",
            },
            {
                Name = "Attack",
                Type = "number",
                Default = "0.01",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How quickly it reacts to a loud part, in seconds.",
            },
            {
                Name = "Release",
                Type = "number",
                Default = "0.1",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How quickly it lets go after one, in seconds.",
            },
            {
                Name = "MakeupGain",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "What is added back to the whole afterwards, in decibels, 0 to 24.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "ChorusSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "ChorusSoundEffect",
        Doc = "A copy of the sound that drifts a little out of time and tune with it: one voice as several, "
            .. "a shimmer.",
        Properties = {
            {
                Name = "Rate",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How fast the copy drifts back and forth, in hertz.",
            },
            {
                Name = "Depth",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How far it drifts, 0 to 1.",
            },
            {
                Name = "Mix",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "How much of what is heard is the copy, 0 to 1.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "PitchShiftSoundEffect",
        Extends = "SoundEffect",
        Module = "audio",
        Parents = under("Sound", "AudioGroup"),
        Storage = "PitchShiftSoundEffect",
        Doc = "Raises or lowers the pitch without changing how long the sound lasts -- which "
            .. "`PlaybackSpeed` cannot: a chipmunk, a giant, a voice disguised.",
        Properties = {
            {
                Name = "Octave",
                Type = "number",
                Default = "1.25",
                ErrorKey = "scene.err.number_out_of_range",
                Presentation = true,
                Doc = "What the pitch is multiplied by, 0.5 (an octave down) to 2 (an octave up). 1 changes "
                    .. "nothing.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Sound",
        Extends = "Instance",
        Module = "audio",
        Storage = "Sound",
        Doc = "One sound, playing or not (§2.2). **Parent it to a `BasePart` and it is positional**; parent it "
            .. "anywhere else and it is 2D. That is the whole of the 3D switch, and it is a property of where "
            .. "the instance sits rather than a flag that could disagree with it.\n\n**Positional means two "
            .. "things**: it gets quieter with distance, and it comes out of the side it is on. The side is an "
            .. "azimuth in the listener's own frame, so turning around swaps it -- elevation is not expressed, "
            .. "because two channels cannot, and a source directly ahead and one directly behind are both centre "
            .. "for the same reason. A positional sound is folded to mono before it is placed; a 2D one keeps its "
            .. "own stereo image, which is all it has.\n\n**Its timeline is the "
            .. "SimClock's, not the mixer's.** `TimePosition` advances by "
            .. "`FixedTimestep * PlaybackSpeed` per tick and `Ended` fires from that timeline, so a replay "
            .. "reproduces both exactly and a headless run with no audio device produces the same `Ended` on "
            .. "the same tick as a run with speakers. What the speakers do is downstream of the simulation and "
            .. "never an input to it -- otherwise a script reading `TimePosition` would be reading the wall "
            .. "clock through a side door (R10).\n\n**How long it is, is the FILE's.** `TimePosition` "
            .. "advances on the simulation's clock and stops at the length of what `Content` decoded to, so "
            .. "the two facts a sound is made of come from the two places that own them.",
        Properties = {
            {
                Name = "Content",
                Type = "Content",
                ContentKind = "Audio",
                Default = '""',
                Doc = "The audio, as an `asset://` URI. WAV, MP3, FLAC and Ogg Vorbis. A file of up to "
                    .. "ten seconds is decoded once and held; a longer one STREAMS -- its encoded bytes "
                    .. "are kept and each voice decodes just ahead of the speakers, so three minutes of "
                    .. "music costs the file's size rather than about 70 MB. Either way a sound does not "
                    .. "re-read its file sixty times a second.\n\nA URI that "
                    .. "names nothing still PLAYS, as a generated tone whose pitch comes from a hash of the "
                    .. "id. A sound that went silent because a file was missing is a bug report about the "
                    .. "sound; a placeholder that is audibly a placeholder is one about the file. "
                    .. '`DebugService:GetStat("AudioClipsMissing")` is how a script asks which it '
                    .. "got.\n\n**The file's length is the sound's length**, read from its header "
                    .. "rather than by decoding it. `Ended` fires when "
                    .. "`TimePosition` reaches it, so a two-minute track plays for two minutes; a "
                    .. "content that names nothing is one second long, because that is how long the "
                    .. "placeholder tone is.\n\nThe mixer keeps its own cursor and takes "
                    .. "`TimePosition` whenever the two part company -- a seek, a rewind, a sound "
                    .. "stopped and started. It has to: the timeline moves in whole ticks and a "
                    .. "device moves in buffers, so a cursor re-read every frame stutters between "
                    .. "them. Nothing in the simulation reads that cursor, which is what keeps a "
                    .. "replay exact (R10).",
            },
            {
                Name = "Playing",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the timeline is advancing. Writing it is the same as calling `Play` or `Stop`, "
                    .. "and reading it is how a script asks without keeping its own flag.",
            },
            {
                Name = "Looped",
                Type = "boolean",
                Default = "false",
                Doc = "Whether reaching the end wraps to the start instead of stopping. A looped sound never "
                    .. "fires `Ended`, because it never does.",
            },
            {
                Name = "Volume",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "A multiplier, combined with the group's and with the listener distance. The default is "
                    .. "half rather than full because a game with several sounds at once and every one at 1 is "
                    .. "a game that clips, and the first thing anybody does is turn them all down.",
            },
            {
                Name = "PlaybackSpeed",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "How fast the timeline runs, and therefore the pitch. 2 is an octave up and half the "
                    .. "duration. Zero is refused rather than treated as a pause -- `Playing` is what pauses, "
                    .. "and a speed of zero would be a sound that never ends.",
            },
            {
                Name = "TimePosition",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Transient = true,
                Doc = "Where the timeline is, in seconds, from 0 to `TimeLength`. Writable, which is how a "
                    .. "script seeks: a write below zero is refused, and one past the end is clamped to it -- "
                    .. "or wrapped, for a `Looped` sound -- exactly as arriving there would.",
            },
            {
                Name = "TimeLength",
                Type = "number",
                ReadOnly = true,
                Transient = true,
                Doc = "How long `Content` is, in seconds: the file's own length, read from its header, or the "
                    .. "placeholder tone's one second when it names nothing. 0 until the length has been read: "
                    .. "the world's first tick after `Content` is set reads it, and the editor reads it for the "
                    .. "sound it shows.",
            },
            {
                Name = "RollOffMinDistance",
                Type = "number",
                Default = "8",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How far the listener can be before a positional sound starts getting quieter. Inside "
                    .. "it the sound is at full `Volume`; ignored entirely for a 2D sound.",
            },
            {
                Name = "RollOffMaxDistance",
                Type = "number",
                Default = "80",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How far the listener can be before a positional sound is silent. Between the two "
                    .. "distances it fades linearly -- linear rather than inverse-square because a game's "
                    .. "audible range is a design decision rather than a physical one, and an inverse square "
                    .. "makes the far half of it inaudible.",
            },
            {
                Name = "Group",
                Type = "AudioGroup?",
                Default = "nil",
                Doc = "The bus this sound's volume is multiplied by, or nil for none.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Play",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Plays it from the start -- or from `TimePosition`, when a script set it since the "
                    .. "sound last started. Playing one that is already playing starts it again. `Resume` "
                    .. "is what carries on from where `Pause` left it.",
            },
            {
                Name = "Resume",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Carries on from `TimePosition`, which is where `Pause` left it. The same as "
                    .. "`Playing = true`.",
            },
            {
                Name = "Pause",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops the timeline where it is. `Resume` carries on from there; `Play` starts "
                    .. "again from the start. The same as `Playing = false`.",
            },
            {
                Name = "Stop",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops it and rewinds to the start. `Ended` does NOT fire: it is a past-tense fact about "
                    .. "reaching the end, and code that awards something when a jingle finishes must not be "
                    .. "fooled by one that was cut off.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Ended",
                Parameters = {},
                Doc = "Fired on the tick the timeline reaches the end of a sound that is not `Looped`, which "
                    .. "stops it and rewinds `TimePosition` to 0 -- so playing it again plays it again. From "
                    .. "the SIM timeline rather than from the mixer, so it lands on the same tick in a replay, "
                    .. "in a headless run, and on a machine whose audio buffer is four times the size.",
            },
            {
                Name = "Loaded",
                Parameters = {},
                Doc = "Fired when this sound's `Content` has been decoded and is ready to play. **Fires "
                    .. "immediately in v1**, because a sound has nothing to wait for yet -- "
                    .. "declared now so that code written today does not have to change when it does.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "AnimationPlayer",
        Extends = "Instance",
        Module = "render",
        Parents = under("Model"),
        Doc = "Plays a skinned mesh's animation clips (§2.2).\n\n**Parent it to the `Model` "
            .. "whose character it is, and it drives every skinned `MeshPart` under that model.** A "
            .. "character is a body, a shirt and a pair of trousers -- several meshes wearing the same "
            .. "skeleton -- and one clip has to move all of them. Only one of the pieces needs to carry "
            .. "the animation; the clip is taken from the first that does, in tree order.\n\n"
            .. "**The joints are matched by NAME across the pieces, never by index.** Two files exported "
            .. "separately wear the same skeleton in the sense that matters -- the same joints, named "
            .. "the same -- and in no other: an exporter is free to order them differently, and a clip "
            .. "applied through the wrong index twists a sleeve in a way that looks like a broken "
            .. "animation rather than a mismatched rig. A joint one piece does not have is skipped, so a "
            .. "shirt with no fingers keeps its own sleeve.\n\nParenting it straight to a "
            .. "`MeshPart` still works and drives exactly that mesh, which is what a character made of "
            .. "one piece wants.\n\nIt stores nothing: the tracks are the "
            .. "state, and each one is a handle a script holds rather than a child in the tree. Sampling "
            .. "happens at `PreAnimation` on the SimClock, so a clip's position at a given tick is the same "
            .. "in a replay as it was live.\n\nv1 is clip playback and linear blending -- no state machines, "
            .. "no IK, and no root motion.",
        Methods = {
            {
                Name = "LoadAnimation",
                Parameters = { { Name = "content", Type = "Content" } },
                Returns = { "AnimationTrack" },
                ThreadSafety = "Unsafe",
                Doc = "A track for one clip. Everything after a `#` is the clip's NAME inside the file, and a "
                    .. "string with no `#` is a clip name in this player's own mesh -- so the common case is "
                    .. '`player:LoadAnimation("Walk")`.\n\n'
                    .. "**A path before the `#` names the file the CLIP is in, and it need not be the "
                    .. "one this player's skeleton came from.** One walk cycle authored once and played "
                    .. "by every character is the reason a clip is addressable at all, and a clip from "
                    .. "elsewhere is retargeted onto this rig by joint NAME. A joint the target does not "
                    .. "have is skipped rather than guessed, so a clip for a horse played on a person "
                    .. "moves the joints they have in common and no others -- and a file nothing has "
                    .. "loaded gives the same empty track a clip name the file lacks does.\n\n**Load a track "
                    .. "once and keep it.** It always returns a track, even for a clip that is not there -- a "
                    .. "mesh that has not finished loading would otherwise make an ordinary frame a nil index "
                    .. "-- and every call is a handle the VM holds until the world goes away.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "Part2D",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Workspace", "Model", "Part2D"),
        Storage = "Part2D",
        Doc = "A sprite and a body in one, on the 2D plane (the 2D layer, phase 3): what a `Part` is to a 3D "
            .. "world. It lies on the world's XY plane facing -Z, so an orthographic `Camera` looking down -Z "
            .. "sees it as it is; `ZIndex` orders sprites that overlap. It is drawn as its `Image` -- or a flat "
            .. "`Color` without one -- and simulated by 2D physics unless `Anchored`, which makes it ground.",
        Properties = {
            { Name = "Position", Type = "Vector2", Doc = "Where its middle is, in metres." },
            {
                Name = "Rotation",
                Type = "number",
                Doc = "Degrees, counter-clockwise. A part with `FixedRotation` keeps whatever this is.",
            },
            {
                Name = "Size",
                Type = "Vector2",
                Default = "Vector2.new(1, 1)",
                ErrorKey = "scene.err.size2_positive",
                Doc = "Width and height in metres: the picture's and the collider's.",
            },
            { Name = "Shape", Type = "Enum.Shape2D", Doc = "The outline it is drawn and collides as." },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "Multiplies the image; with no image, the colour it is drawn in.",
            },
            {
                Name = "Transparency",
                Type = "number",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "0 is as drawn, 1 is invisible. A transparent part still collides.",
            },
            {
                Name = "ZIndex",
                Type = "number",
                Doc = "Which of two overlapping sprites is in front: the higher. Ties keep the tree's order.",
            },
            {
                Name = "FlipX",
                Type = "boolean",
                Doc = "Mirrors the picture left to right: a character facing the other way.",
            },
            { Name = "FlipY", Type = "boolean", Doc = "Mirrors the picture top to bottom." },
            {
                Name = "Image",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The picture, stretched over `Size`. Its transparent pixels are transparent.",
            },
            {
                Name = "ImageRectOffset",
                Type = "Vector2",
                Doc = "The top-left of the part of the image drawn, in pixels: one frame of a sprite sheet.",
            },
            {
                Name = "ImageRectSize",
                Type = "Vector2",
                Doc = "The size of that part in pixels. Zero on both axes draws the whole image.",
            },
            {
                Name = "Filter",
                Type = "Enum.TextureFilter",
                Doc = "Linear for painted art, Nearest for pixel art.",
            },
            {
                Name = "ExactColor",
                Type = "boolean",
                Default = "true",
                Doc = "**The colours on the screen are the colours in the picture.** A sprite is art somebody "
                    .. "chose every colour of, so by default it is drawn past everything the scene does to light: "
                    .. "the exposure, the tone curve, colour correction and bloom leave it alone, and a `Color` of "
                    .. "`Color3.fromRGB(200, 80, 40)` is that pixel, whatever `Lighting` holds.\n\n"
                    .. "Off, the sprite is one more thing in the lit picture -- exposed, tone-mapped, graded and "
                    .. "glowing like a part beside it -- which is what a sprite standing in a 3D scene wants. "
                    .. "Either way it keeps its place: a part in front hides it, and glass or a particle in front "
                    .. "is drawn over it.",
            },
            {
                Name = "Anchored",
                Type = "boolean",
                Doc = "Ground: it does not move, and what lands on it stays. A script may still move it.",
            },
            {
                Name = "CanCollide",
                Type = "boolean",
                Default = "true",
                Doc = "Whether other parts bump into it. A part that cannot collide is only a picture.",
            },
            {
                Name = "Sensor",
                Type = "boolean",
                Doc = "Reports what enters it through `Touched` and pushes nothing: a coin, a checkpoint, a "
                    .. "trigger.",
            },
            {
                Name = "Density",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "Kilograms per square metre: with `Size`, how heavy it is.",
            },
            {
                Name = "Friction",
                Type = "number",
                Default = "0.3",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much it grips what it slides on. Zero is ice.",
            },
            {
                Name = "Elasticity",
                Type = "number",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "How much of its speed a bounce gives back: 0 lands dead, 1 bounces for ever.",
            },
            {
                Name = "FixedRotation",
                Type = "boolean",
                Doc = "Never turns, whatever hits it: a character that stays upright.",
            },
            {
                Name = "GravityScale",
                Type = "number",
                Default = "1",
                Doc = "How strongly `Workspace.Gravity` pulls it: 0 floats, 2 falls twice as hard, and a "
                    .. "negative number falls up.",
            },
            {
                Name = "Velocity",
                Type = "Vector2",
                Doc = "Metres per second. Written by the simulation every tick, and a script may set it: a jump "
                    .. "is setting its y.",
            },
            {
                Name = "AngularVelocity",
                Type = "number",
                Doc = "Degrees per second, counter-clockwise.",
            },
            {
                Name = "CollisionGroup",
                Type = "string",
                Default = '"Default"',
                Doc = "The group deciding which other parts it collides with, as `PhysicsService` names them.",
            },
        },
        Methods = {
            {
                Name = "ApplyImpulse",
                Parameters = { { Name = "impulse", Type = "Vector2" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "A sudden push, in kilogram-metres per second, at its middle. Applied at the next tick; an "
                    .. "anchored part ignores it.",
            },
        } :: { Schema.Method },
        Events = {
            {
                Name = "Touched",
                Parameters = { { Name = "otherPart", Type = "Instance" } },
                Doc = "Fires once when something begins touching it -- another `Part2D`, or a `Tilemap2D`'s "
                    .. "solid tiles -- or, for a `Sensor`, begins overlapping it. Deferred like every signal.",
            },
            {
                Name = "TouchEnded",
                Parameters = { { Name = "otherPart", Type = "Instance" } },
                Doc = "Fires when something that was touching it stops.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "Tilemap2D",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Workspace", "Model"),
        Storage = "Tilemap2D",
        Doc = "A grid of tiles from one tileset image, on the 2D plane (the 2D layer, phase 3): a level you "
            .. "paint. Tile 0 is empty; tile `n` is the `n`-th tile of the tileset, counted along its rows "
            .. "from the top left. Every tile that is not empty is solid when `Collides` is on, as one body "
            .. "whose edges are merged, so a character runs across a floor of tiles without catching on their "
            .. "seams.",
        Properties = {
            {
                Name = "Position",
                Type = "Vector2",
                Doc = "Where the bottom-left corner of cell (0, 0) is, in metres.",
            },
            {
                Name = "CellSize",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The side of one cell, in metres.",
            },
            {
                Name = "Tileset",
                Type = "Content",
                ContentKind = "Texture",
                Doc = "The image the tiles are cut from.",
            },
            {
                Name = "TileSize",
                Type = "Vector2",
                Default = "Vector2.new(16, 16)",
                ErrorKey = "scene.err.size2_positive",
                Doc = "One tile's size in the tileset, in pixels.",
            },
            {
                Name = "ZIndex",
                Type = "number",
                Doc = "Its place among overlapping sprites, as a `Part2D`'s.",
            },
            {
                Name = "Color",
                Type = "Color3",
                Default = "Color3.new(1, 1, 1)",
                Doc = "Multiplies every tile.",
            },
            {
                Name = "Filter",
                Type = "Enum.TextureFilter",
                Default = "Enum.TextureFilter.Nearest",
                Doc = "Nearest by default, because tiles are nearly always pixel art.",
            },
            {
                Name = "ExactColor",
                Type = "boolean",
                Default = "true",
                Doc = "The tiles' colours on the screen are the colours in the tileset, as `Part2D.ExactColor` "
                    .. "says. Off, the level is lit, exposed and graded with the rest of the scene.",
            },
            {
                Name = "Collides",
                Type = "boolean",
                Default = "true",
                Doc = "Whether its tiles are solid. Off, it is scenery: a background layer.",
            },
            {
                Name = "Friction",
                Type = "number",
                Default = "0.3",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much its tiles grip what slides on them.",
            },
            {
                Name = "CollisionGroup",
                Type = "string",
                Default = '"Default"',
                Doc = "The group its tiles collide as.",
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
                Doc = "Puts tile `tile` in cell (`x`, `y`); 0 empties it. Cells are whole numbers, and y grows "
                    .. "upwards, as the world's does.",
            },
            {
                Name = "GetCell",
                Parameters = { { Name = "x", Type = "number" }, { Name = "y", Type = "number" } },
                Returns = { "number" },
                ThreadSafety = "ReadParallel",
                Doc = "The tile in cell (`x`, `y`), or 0.",
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
                Doc = "Puts `tile` in every cell of the rectangle from (`x0`, `y0`) to (`x1`, `y1`), both "
                    .. "corners included.",
            },
            {
                Name = "Clear",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Empties every cell.",
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
        Doc = "The base of a joint between two `Part2D`s (ADR 0102): the 2D solver holds them together, "
            .. "and a rope bridge, a door on a hinge or a swinging lamp is what comes of it.\n\n"
            .. "**Two parts and a point on each**, not two attachments: a 2D joint is a point, and each "
            .. "part's is in that part's own metres from its middle. A joint that cannot hold -- a part that "
            .. "is missing or is not in the world, or two anchored parts -- holds nothing and is not an "
            .. "error.",
        Properties = {
            {
                Name = "Part0",
                Type = "Part2D?",
                Doc = "One of the two parts it holds.",
            },
            {
                Name = "Part1",
                Type = "Part2D?",
                Doc = "The other.",
            },
            {
                Name = "Anchor0",
                Type = "Vector2",
                Doc = "Where the joint is on `Part0`, in metres from its middle, in its own frame, so it turns "
                    .. "with the part.",
            },
            {
                Name = "Anchor1",
                Type = "Vector2",
                Doc = "Where the joint is on `Part1`, the same way.",
            },
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Doc = "Whether the joint holds. A disabled joint stays in the world holding nothing.",
            },
            {
                Name = "CollideConnected",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the two parts still collide with each other. False is what a chain of links "
                    .. "or a limb wants, where the parts overlap at the joint by construction.",
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
        Doc = "A point both parts turn about: a door, a wheel, a pendulum, a lever. With `Anchor0` and "
            .. "`Anchor1` at the same place in the world, the parts stay together there and turn freely, "
            .. "limited or driven as below.",
        Properties = {
            {
                Name = "LimitsEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether `LowerAngle` and `UpperAngle` apply. Off turns freely.",
            },
            {
                Name = "LowerAngle",
                Type = "number",
                Default = "-90",
                Doc = "How far `Part1` may turn clockwise, in degrees, from how the two stood when the joint "
                    .. "was made.",
            },
            {
                Name = "UpperAngle",
                Type = "number",
                Default = "90",
                Doc = "How far it may turn counter-clockwise, the same way.",
            },
            {
                Name = "MotorEnabled",
                Type = "boolean",
                Default = "false",
                Doc = "Whether the hinge drives itself towards `MotorSpeed`: a wheel, a fan, a windmill.",
            },
            {
                Name = "MotorSpeed",
                Type = "number",
                Default = "0",
                Doc = "The speed the motor drives to, in degrees per second, counter-clockwise positive.",
            },
            {
                Name = "MotorMaxTorque",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The most torque the motor applies, in newton metres. What stops it is anything that "
                    .. "needs more.",
            },
        },
    },
    {
        Name = "SpringConstraint2D",
        Extends = "Constraint2D",
        Module = "scene",
        Storage = "SpringConstraint2D",
        Doc = "A distance between two points, held softly: a spring, a bungee, a suspension. At a "
            .. "`Stiffness` of 0 it is a rigid rod of `Length`, which is what a rope bridge's links are.",
        Properties = {
            {
                Name = "Length",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "The rest length, in metres.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "4",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How quickly it springs back, in hertz. 0 makes it rigid.",
            },
            {
                Name = "Damping",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The damping ratio: 0 bounces for ever, 1 settles without overshooting.",
            },
            {
                Name = "MinLength",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The shortest it may be squeezed to, in metres, however hard it is pushed.",
            },
            {
                Name = "MaxLength",
                Type = "number",
                Default = "1000",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "The longest it may be stretched to, in metres: the end of a bungee.",
            },
        },
    },
    {
        Name = "WeldConstraint2D",
        Extends = "Constraint2D",
        Module = "scene",
        Storage = "WeldConstraint2D",
        Doc = "The two parts held rigidly together, in the placement they had when the weld was made: "
            .. "a sword in a hand, a crate's lid, a vehicle's body on its frame.",
    },
    {
        Name = "SpriteAnimator",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Part2D"),
        Storage = "SpriteAnimator",
        Doc = "Plays frames of its parent `Part2D`'s sprite sheet on the simulation clock (ADR 0102), so a "
            .. "walk cycle is data instead of script. Each tick it is playing, it writes the parent's "
            .. "`ImageRectOffset` and `ImageRectSize` for the frame it is on; the picture, the undo and the "
            .. "wire all see that as they see any write.\n\n"
            .. "Frames are counted along the sheet's rows from `SheetOffset`, `Columns` to a row. A "
            .. "non-looped animation that reaches its end stops and writes `Playing = false`, which is its "
            .. 'end event: listen with `GetPropertyChangedSignal("Playing")`.',
        Properties = {
            {
                Name = "FrameSize",
                Type = "Vector2",
                Default = "Vector2.new(16, 16)",
                ErrorKey = "scene.err.size2_positive",
                Doc = "One frame's size in the sheet, in pixels.",
            },
            {
                Name = "Columns",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.whole_number_at_least_one",
                Doc = "How many frames make one row of the sheet.",
            },
            {
                Name = "SheetOffset",
                Type = "Vector2",
                Doc = "Where frame 0 starts, in pixels from the sheet's top left, so one sheet can hold several "
                    .. "animations one under another.",
            },
            {
                Name = "FirstFrame",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.whole_number_at_least_zero",
                Doc = "The frame this animation starts on, counted from `SheetOffset`.",
            },
            {
                Name = "FrameCount",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.whole_number_at_least_one",
                Doc = "How many frames it plays.",
            },
            {
                Name = "FramesPerSecond",
                Type = "number",
                Default = "8",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "How fast, on the simulation clock.",
            },
            {
                Name = "Looped",
                Type = "boolean",
                Default = "true",
                Doc = "Whether it starts over at the end. Off stops on the last frame.",
            },
            {
                Name = "Playing",
                Type = "boolean",
                Default = "false",
                Doc = "Whether it advances. Setting it true starts from `FirstFrame` when it had stopped at the "
                    .. "end, and carries on from where it was when it had been paused.",
            },
            {
                Name = "Frame",
                Type = "number",
                ReadOnly = true,
                Doc = "The frame it is on, from 0 at `FirstFrame`.",
            },
        },
    },
    {
        Name = "NavigationArea",
        Extends = "Instance",
        Module = "scene",
        Storage = "NavigationArea",
        Parents = under("BasePart"),
        Doc = "Marks the walkable ground inside its part's box with a `Label` (ADR 0098), which "
            .. "`NavigationService:SetAreaCost` prices: water an agent would rather go round, mud that is slow, "
            .. "a door that is shut. The part need not collide or be anchored -- it is a volume, not a floor. "
            .. "Ground no area marks costs 1.",
        Properties = {
            {
                Name = "Label",
                Type = "string",
                Default = '"Area"',
                Doc = "The name its cost is set by. Areas that share a label share a cost.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "NavigationLink",
        Extends = "Instance",
        Module = "scene",
        Storage = "NavigationLink",
        Doc = "A way between two points the walkable ground does not join (ADR 0098): a gap to jump, a "
            .. "ladder, a drop. A path may use it; `FindPath`'s third answer names its `Label` at the waypoint "
            .. "where it begins, so the script walking the path knows to jump there rather than walk. Both "
            .. "points are in the world, and each must be near walkable ground.",
        Properties = {
            {
                Name = "From",
                Type = "vector",
                Doc = "Where it starts, in the world.",
            },
            {
                Name = "To",
                Type = "vector",
                Doc = "Where it ends, in the world.",
            },
            {
                Name = "Bidirectional",
                Type = "boolean",
                Default = "true",
                Doc = "Whether it can be taken back from `To` to `From`: a ladder can, a drop cannot.",
            },
            {
                Name = "Label",
                Type = "string",
                Default = '"Jump"',
                Doc = "What crossing it is, as the path reports it.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "NavigationAgent",
        Extends = "Instance",
        Module = "scene",
        Storage = "NavigationAgent",
        Parents = under("BasePart"),
        Doc = "Walks its part to a point over the walkable ground, keeping clear of the other agents as it "
            .. "goes -- a crowd (ADR 0098). The engine moves the part on the simulation clock: set `Target`, "
            .. "and `Reached` fires when it arrives. For one character a script walking `FindPath`'s waypoints "
            .. "is enough; this is for many that must not walk through each other.",
        Properties = {
            {
                Name = "Target",
                Type = "vector",
                Doc = "Where it is going. Writing it starts the walk; while `Active` is false it stands still.",
            },
            {
                Name = "Active",
                Type = "boolean",
                Default = "false",
                Doc = "Whether it is walking. Writing `Target` sets it; arriving clears it.",
            },
            {
                Name = "MaxSpeed",
                Type = "number",
                Default = "8",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "Its fastest, in metres per second.",
            },
            {
                Name = "AgentType",
                Type = "string",
                Default = '""',
                Doc = "Which agent it is, as `NavigationService:DefineAgent` named it; empty for the service's own.",
            },
        } :: { Schema.Property },
        Events = {
            {
                Name = "Reached",
                Parameters = {},
                Doc = "Fired once when it arrives at `Target`.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "FoliageLayer",
        Extends = "Instance",
        Module = "render",
        Parents = under("Terrain"),
        Storage = "FoliageLayer",
        Doc = "Where foliage grows over a terrain (ADR 0116): grass, flowers, stones, bushes -- drawn and "
            .. "never simulated. Its `FoliageMesh` children say what grows; its rules say where, by the "
            .. "ground's material, slope and height, never under a roof. **Visual only**: nothing collides "
            .. "with it, a raycast passes through it, it is not replicated and it is not in the world "
            .. "hash -- every machine grows the same field from the same terrain and seed.",
        Properties = {
            {
                Name = "Enabled",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether it grows at all. Off hides every instance of it, and nothing is placed.",
            },
            {
                Name = "Density",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "How many instances a square metre of ground holds where every rule allows it, before the "
                    .. "material's multiplier, the painted mask and `[render] foliage_density` scale it.",
            },
            {
                Name = "SlopeMin",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_slope_degrees",
                Presentation = true,
                Doc = "The flattest ground it grows on, in degrees from level.",
            },
            {
                Name = "SlopeMax",
                Type = "number",
                Default = "35",
                ErrorKey = "scene.err.number_slope_degrees",
                Presentation = true,
                Doc = "The steepest ground it grows on, in degrees: grass thins out before a cliff does.",
            },
            {
                Name = "HeightMin",
                Type = "number",
                Default = "-10000",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "The lowest ground it grows on, in metres of world height.",
            },
            {
                Name = "HeightMax",
                Type = "number",
                Default = "10000",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "The highest ground it grows on, in metres: a tree line.",
            },
            {
                Name = "Clumping",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much it gathers into patches, from 0 to 1: 0 is even cover, 1 is tufts with bare "
                    .. "ground between.",
            },
            {
                Name = "MinSpacing",
                Type = "number",
                Default = "0.25",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "The least distance between two instances, in metres.",
            },
            {
                Name = "DrawDistance",
                Type = "number",
                Default = "120",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "How far from the camera it is drawn, in metres. Past it nothing of the layer costs a "
                    .. "draw.",
            },
            {
                Name = "FadeDistance",
                Type = "number",
                Default = "20",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "Over how many metres before `DrawDistance` it thins away, so the edge of the field is "
                    .. "not a line.",
            },
            {
                Name = "Seed",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Presentation = true,
                Doc = "Which of the endless arrangements the same rules give. The same seed grows the same "
                    .. "field on every machine.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetMaterials",
                Parameters = {},
                Returns = { "{ { Material: number, Density: number } }" },
                ThreadSafety = "Safe",
                Doc = "The terrain materials it grows on, each with a multiplier of `Density`: material `n` is the "
                    .. "terrain's layer `n` (`Terrain:GetLayers`). Empty is every material at 1.",
            },
            {
                Name = "SetMaterials",
                Parameters = {
                    { Name = "materials", Type = "{ { read Material: number, read Density: number? } }" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets the materials it grows on, as `GetMaterials` returns them: up to 32, `Density` 1 when "
                    .. "left out. An empty list grows on every material. Saved with the scene.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "FoliageMesh",
        Extends = "Instance",
        Module = "render",
        Parents = under("FoliageLayer"),
        Storage = "FoliageMesh",
        Doc = "One mesh a `FoliageLayer` grows (ADR 0116), with its share of the layer, its size, how it "
            .. "sits on the ground and how the wind moves it.",
        Properties = {
            {
                Name = "Mesh",
                Type = "Content",
                Default = '""',
                ContentKind = "Mesh",
                Presentation = true,
                Doc = "The mesh that grows: any mesh asset of the project.",
            },
            {
                Name = "Weight",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "Its share of the layer's instances against the other meshes of the layer: two meshes of "
                    .. "weight 1 and 3 grow one to three.",
            },
            {
                Name = "ScaleMin",
                Type = "number",
                Default = "0.8",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "The smallest an instance is, as a multiple of the mesh.",
            },
            {
                Name = "ScaleMax",
                Type = "number",
                Default = "1.2",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "The largest an instance is, as a multiple of the mesh.",
            },
            {
                Name = "RandomRotation",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether each instance turns about its up axis at random, so a field is not rows of the "
                    .. "same leaf.",
            },
            {
                Name = "AlignToNormal",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much it leans with the ground, from 0 upright to 1 along the slope.",
            },
            {
                Name = "Sink",
                Type = "number",
                Default = "0.05",
                ErrorKey = "scene.err.number_at_least_zero",
                Presentation = true,
                Doc = "How far below the ground its base goes, in metres, so a stem on a slope is not floating "
                    .. "at one edge.",
            },
            {
                Name = "WindResponse",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_zero_to_one",
                Presentation = true,
                Doc = "How much the wind moves it, from 0 still to 1 full sway.",
            },
            {
                Name = "Stiffness",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Presentation = true,
                Doc = "How hard it resists the wind: a reed 0.5, a bush 3.",
            },
            {
                Name = "CastShadow",
                Type = "boolean",
                Default = "true",
                Presentation = true,
                Doc = "Whether it casts a shadow, near the camera: `[render] foliage_shadow_distance` is how near.",
            },
            {
                Name = "Material",
                Type = "Material?",
                Presentation = true,
                Doc = "What it is drawn with: a material asset, or nothing for the mesh's own. A material whose "
                    .. "surface shader has a `surfaceVertex` moves it as it likes, with the wind and the "
                    .. "instance's random value; otherwise the built-in sway bends it from its base.",
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
        Doc = "Makes the part or model it is in something a player clicks (ADR 0126). The engine casts the "
            .. "pointer -- the mouse, or a tap -- into the world once a tick; the nearest part with a detector "
            .. "in it, or in a model around it, is the one pointed at, and a part with `CanQuery` false is "
            .. "passed through. **In a match** the click is sent to the authority, which checks the distance "
            .. "from the player's character and fires `MouseClick` there with the player; the machine that "
            .. "clicked fires it too, so its own scripts react at once. Hovering is this machine's alone.",
        Properties = {
            {
                Name = "MaxActivationDistance",
                Type = "number",
                Default = "32",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How far from the player's character, in metres, a click still counts -- from the camera "
                    .. "when there is no character.",
            },
            {
                Name = "CursorIcon",
                Type = "Content",
                Default = '""',
                ContentKind = "Texture",
                Inert = true,
                Doc = "The picture the pointer shows over it. Empty keeps the pointer as it is. Stored, "
                    .. "replicated and not drawn yet: the pointer keeps its look (the toolkit's F1 ledger).",
            },
        } :: { Schema.Property },
        Events = {
            {
                Name = "MouseClick",
                Parameters = { { Name = "player", Type = "Player" } },
                Doc = "The primary button, or a tap, went down on it within reach.",
            },
            {
                Name = "RightMouseClick",
                Parameters = { { Name = "player", Type = "Player" } },
                Doc = "The secondary button went down on it within reach.",
            },
            {
                Name = "MouseHoverEnter",
                Parameters = { { Name = "player", Type = "Player" } },
                Doc = "The pointer came onto it, within reach. On this machine only.",
            },
            {
                Name = "MouseHoverLeave",
                Parameters = { { Name = "player", Type = "Player" } },
                Doc = "The pointer left it, or it went out of reach. On this machine only.",
            },
        } :: { Schema.Event },
    },
    {
        Name = "Water",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Workspace"),
        Storage = "Water",
        Doc = "Water (ADR 0118): a sea, a lake or a river, drawn and floated from **one wave definition** -- "
            .. "its `WaterWave` children -- evaluated the same on the CPU for the simulation and on the GPU "
            .. "for the picture. An unanchored part in it is held up by the water it displaces, at points "
            .. "over its shape, so a hull pitches and rolls; `Viscosity` drags it, and `Current` carries it. "
            .. "`GetHeightAt` is where the surface is, for a game's own use.",
        Properties = {
            {
                Name = "Shape",
                Type = "Enum.WaterShape",
                Default = "Enum.WaterShape.Ocean",
                Doc = "Everywhere (`Ocean`), a rectangle (`Pool`), along its `WaterPoint` children (`River`), or "
                    .. "inside the closed curve through them (`Lake`).",
            },
            {
                Name = "SurfaceLevel",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Doc = "The height of the still surface, in metres; the waves rise and fall about it.",
            },
            {
                Name = "Position",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "The middle of a `Box`, across the ground; its height is `SurfaceLevel`'s.",
            },
            {
                Name = "Size",
                Type = "vector",
                Default = "vector.create(64, 8, 64)",
                ErrorKey = "scene.err.water_size",
                Doc = "A `Pool`'s width, depth under the surface, and length. A `River`'s width is its X and "
                    .. "its depth its Y, where a point does not say its own; a `Lake`'s depth is its Y, and its "
                    .. "outline is its points -- it has no width or length to give. Y must be greater than zero; "
                    .. "X and Z may be zero.",
            },
            {
                Name = "Density",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "Mass per cubic metre, in the units of `BasePart.Density`: a part less dense than the "
                    .. "water floats, and one of half its density floats half under.",
            },
            {
                Name = "Viscosity",
                Type = "number",
                Default = "1",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How much it drags what moves through it: 0 none, 1 a sea's, more a swamp's.",
            },
            {
                Name = "Current",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "Metres a second the water flows, carrying what floats in it. A `River` adds `FlowSpeed` "
                    .. "along its course.",
            },
            {
                Name = "FlowSpeed",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Doc = "How fast a river flows along its points, first to last, in metres a second -- where it "
                    .. "is level. Where it drops it runs faster, by four times its slope: twice as fast down "
                    .. "one in four.",
            },
            {
                Name = "BankWidth",
                Type = "number",
                Default = "2",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How wide the bank `Carve` cuts is, in metres: the bed slopes from the water's edge, where "
                    .. "it is at the surface, down to the water's depth this far in. Zero is a wall.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "GetHeightAt",
                Parameters = { { Name = "position", Type = "vector" } },
                Returns = { "number?" },
                ThreadSafety = "ReadParallel",
                Doc = "The height of the surface above the position's column now, waves and all -- the height "
                    .. "the picture draws there -- or nil where this water is not.",
            },
            {
                Name = "GetNormalAt",
                Parameters = { { Name = "position", Type = "vector" } },
                Returns = { "vector?" },
                ThreadSafety = "ReadParallel",
                Doc = "Which way the surface faces above the position's column now, or nil where this water "
                    .. "is not.",
            },
            {
                Name = "Carve",
                Parameters = {},
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Cuts this water's bed into the terrain under it (ADR 0146): every column of ground it "
                    .. "covers is lowered to the bed -- the surface at the water's edge, sloping to its depth "
                    .. "`BankWidth` in -- and ground already lower is left alone. A river's bed follows it "
                    .. "down. Answers how many columns it lowered; a sea carves nothing. Call it again after "
                    .. "moving the water: it only ever digs.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "WaterWave",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Water"),
        Storage = "WaterWave",
        Doc = "One wave of the `Water` it is in (ADR 0118): the first eight, in order, make the surface. "
            .. "It travels at the speed deep water gives its length.",
        Properties = {
            {
                Name = "Wavelength",
                Type = "number",
                Default = "40",
                ErrorKey = "scene.err.number_above_zero",
                Doc = "Metres from one crest to the next.",
            },
            {
                Name = "Amplitude",
                Type = "number",
                Default = "0.5",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "Metres a crest rises above the still surface.",
            },
            {
                Name = "Direction",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Doc = "Which way it travels, in degrees about the vertical: 0 along +X, 90 along +Z.",
            },
            {
                Name = "Steepness",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_zero_to_one",
                Doc = "0 a rounded swell, 1 sharp crests and flat troughs.",
            },
            {
                Name = "Phase",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_finite",
                Doc = "Where in its cycle it starts, in radians, so two alike waves need not rise together.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "WaterPoint",
        Extends = "Instance",
        Module = "scene",
        Parents = under("Water"),
        Storage = "WaterPoint",
        Doc = "A point a `River` runs through, or a `Lake`'s outline passes (ADR 0146), in order. The water "
            .. "follows a smooth curve through its points -- a centripetal Catmull-Rom spline, which passes "
            .. "through every one and makes no loop.",
        Properties = {
            {
                Name = "Position",
                Type = "vector",
                Default = "vector.create(0, 0, 0)",
                Doc = "Where the water passes. For a `River` its height is the surface's height there, so a "
                    .. "river whose points descend runs downhill; a `Lake` and a `Spline` are level at the "
                    .. "water's `SurfaceLevel` and read only where the point is across the ground.",
            },
            {
                Name = "Width",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How wide a river is here, in metres, changing smoothly to the next point's. Zero is the "
                    .. "water's `Size.X`.",
            },
            {
                Name = "Depth",
                Type = "number",
                Default = "0",
                ErrorKey = "scene.err.number_at_least_zero",
                Doc = "How deep a river is here, in metres. Zero is the water's `Size.Y`.",
            },
            {
                Name = "Sharp",
                Type = "boolean",
                Default = "false",
                Doc = "A corner here, where the curve would round it.",
            },
        } :: { Schema.Property },
    },
}

local Definition: Schema.Definition = {
    Classes = Classes,
}

return Definition
