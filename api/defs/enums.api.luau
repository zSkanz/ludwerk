--!strict
--- Enums (api-design.md §2.3, the Enum row).
---
--- The enums the surface actually uses, milestone by milestone: an enum nobody
--- can pass anywhere is a name squatting on a meaning we have not designed yet.
--- M6 adds the four the Input Action System needs, and `KeyCode` is the largest
--- enum in the surface by an order of magnitude -- it is the whole of what a
--- binding may name, and its item NAMES are the `platform` device layer's
--- enumerator names on purpose, so that the two tables can be checked against
--- each other rather than kept in step by hand.
---
--- Item values are contract rather than bookkeeping: they are what
--- serialization writes, what an api-dump diff compares, and -- for `LogLevel`
--- -- what a severity comparison reads. Every enum below numbers its items
--- sequentially from 0 in declaration order, and declaration order is §2.3's
--- own listing order, so the document and this file can be diffed by eye.
--- Nothing is spaced out to leave room for later insertions: reserving value
--- space for an item nobody has designed is the same squatting the paragraph
--- above refuses, differing only in that it squats on a number instead of a
--- name. A level or a shape added later is a surface decision either way, and
--- it will be made with the whole list in front of it.

local Schema = require("../schema")

local Definition: Schema.Definition = {
    Enums = {
        {
            Name = "StreamingMode",
            Doc = "How a `Model` behaves when the streaming system reaches it. The grid decides WHEN something "
                .. "becomes eligible and this decides WHAT comes with it, which is the whole of what a person "
                .. "has to say about a world they built by hand (ADR 0053).",
            Items = {
                {
                    Name = "Nonatomic",
                    Value = 0,
                    Doc = "Its descendants are placed in cells one at a time, each by its own position, and they "
                        .. "arrive and leave independently. The default, and what a world of loose scenery "
                        .. "wants: a distant hillside's parts can go one at a time.",
                },
                {
                    Name = "Atomic",
                    Value = 1,
                    Doc = "The model is ONE unit: it goes in a single cell however far its parts spread, and it "
                        .. "materialises and evicts whole. A house arrives as a house rather than as forty parts "
                        .. "appearing in an order nobody chose, and a machine assembled from moving parts is "
                        .. "never half a mechanism.",
                },
                {
                    Name = "Persistent",
                    Value = 2,
                    Doc = "Never enters the grid at all. It stays in the scene, it exists before the first tick, "
                        .. "and no eviction reaches it however far the focus walks. The spawn, the checkpoint, "
                        .. "and anything a script holds a long-lived reference to.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "PartShape",
            Doc = "The solid a `Part` renders and collides as; `MeshPart` carries geometry instead (§2.2).",
            Items = {
                { Name = "Block", Value = 0 },
                { Name = "Ball", Value = 1 },
                { Name = "Cylinder", Value = 2 },
                {
                    Name = "Capsule",
                    Value = 3,
                    Doc = "A cylinder closed by a hemisphere at each end.",
                },
                {
                    Name = "Wedge",
                    Value = 4,
                    Doc = "A box with one face sloped away to an edge: a right-triangular prism.",
                },
            },
        },
        {
            Name = "AlphaMode",
            Doc = "How a `Material` reads the alpha channel of its colour. glTF's three modes, kept because they "
                .. "are what a file says and translating them into something else here would only move the "
                .. "switch.",
            Items = {
                {
                    Name = "Opaque",
                    Value = 0,
                    Doc = "Alpha is ignored entirely. The cheapest, and what almost everything is.",
                },
                {
                    Name = "Mask",
                    Value = 1,
                    Doc = "Alpha is a coverage TEST against `AlphaCutoff`, not a blend: a pixel is drawn or it is "
                        .. "not. Leaves and chain-link fences want this -- it stays in the opaque pass, so it "
                        .. "writes depth and needs no sorting.",
                },
                {
                    Name = "Blend",
                    Value = 2,
                    Doc = "Alpha blends. Glass and smoke want this, and it costs what every transparent surface "
                        .. "costs: a sorted pass and no depth writes.",
                },
            },
        },
        {
            Name = "CollisionFidelity",
            Doc = "How closely a `MeshPart`'s collision shape follows the geometry it renders (§2.2). The pairing "
                .. "is a trade: the closer the shape, the more the solver pays for every contact against it.",
            Items = {
                {
                    Name = "Default",
                    Value = 0,
                    Doc = "The engine chooses. In this release that is `Hull`, and the property reports `Default` "
                        .. "rather than what it resolved to, because the point of the item is that the choice is "
                        .. "the engine's to change.",
                },
                { Name = "Hull", Value = 1, Doc = "One convex hull around the whole mesh." },
                {
                    Name = "Box",
                    Value = 2,
                    Doc = "The mesh's bounding box, which is the cheapest shape there is and the least faithful.",
                },
                {
                    Name = "Precise",
                    Value = 3,
                    Doc = "The triangles themselves. Accepted and not yet implemented: this release collides "
                        .. "against a hull and says so through this property reading back `Precise` while "
                        .. "behaving as `Hull`. A triangle-mesh collider is work the asset pipeline has yet to do.",
                },
            },
        },
        {
            Name = "RaycastFilterType",
            Doc = "Whether a `RaycastParams` filter list names what a cast may hit or what it must ignore (§2.3).",
            Items = {
                { Name = "Exclude", Value = 0, Doc = "Everything except the listed instances and their descendants." },
                { Name = "Include", Value = 1, Doc = "Only the listed instances and their descendants." },
            },
        },
        {
            Name = "CharacterState",
            Doc = "What a `CharacterBody` is doing (§2.2). On foot there are two answers and not three: "
                .. "ground too steep to walk on reads as `Airborne`, because the question a script asks is "
                .. "whether it may jump.",
            Items = {
                { Name = "Grounded", Value = 0 },
                { Name = "Airborne", Value = 1 },
                {
                    Name = "Swimming",
                    Value = 2,
                    Doc = "Its middle is under a `Water`'s surface or inside a fluid block: no weight, and it "
                        .. "moves in three dimensions.",
                },
                { Name = "Flying", Value = 3, Doc = "`CharacterBody.Flying` is set." },
            },
        },
        {
            Name = "RotationOrder",
            Doc = "The order `CFrame.fromEuler` applies its three angles, and `CFrame:ToEuler` recovers them: "
                .. "intrinsic rotations, each about the axes the previous one produced, read left to right. "
                .. "All six permutations are declared.",
            Items = {
                { Name = "XYZ", Value = 0 },
                { Name = "XZY", Value = 1 },
                {
                    Name = "YXZ",
                    Value = 2,
                    Doc = "Yaw about Y, then pitch about X, then roll about Z -- what an omitted `order` "
                        .. "parameter means, `ToEuler` included.",
                },
                { Name = "YZX", Value = 3 },
                { Name = "ZXY", Value = 4 },
                { Name = "ZYX", Value = 5 },
            },
        },
        {
            Name = "LogLevel",
            Doc = "The severity of a log line, ascending: `Value` orders the levels, so a handler filters with "
                .. "`level.Value >= Enum.LogLevel.Warning.Value`. `DebugService.MessageOut` carries one with "
                .. "every message (§2.1).",
            Items = {
                {
                    Name = "Trace",
                    Value = 0,
                    Doc = "Per-frame or per-call detail, silent in any ordinary run.",
                },
                {
                    Name = "Debug",
                    Value = 1,
                    Doc = "Diagnostic detail for whoever is working on the code that emits it.",
                },
                {
                    Name = "Info",
                    Value = 2,
                    Doc = "Ordinary progress; `print` reports at this level.",
                },
                {
                    Name = "Warning",
                    Value = 3,
                    Doc = "Something is wrong and the run continues; `warn` reports at this level.",
                },
                {
                    Name = "Error",
                    Value = 4,
                    Doc = "A failure the emitting code could not handle, a contained handler error among them (§3.1).",
                },
            },
        },
        {
            Name = "RunContext",
            Doc = "**Where a `Script` runs** (ADR 0138): `Script.RunContext`. Inside a script service the "
                .. "service decides, and anywhere else the property does. A solo or hosting machine is the "
                .. "server and a player at once, and runs each script once whatever its side.",
            Items = {
                {
                    Name = "Client",
                    Value = 0,
                    Doc = "Where a player sits: a joined client, solo and a host -- never a dedicated server. A "
                        .. "dedicated server's package carries no `Client` script's code.",
                },
                {
                    Name = "Server",
                    Value = 1,
                    Doc = "The authority: a dedicated server, solo and a host -- never a joined client. A "
                        .. "client's package carries no `Server` script's code.",
                },
                {
                    Name = "Shared",
                    Value = 2,
                    Doc = "Every machine, once each: the server and every client. The default outside the script "
                        .. "services.",
                },
            },
        },
        {
            Name = "KeyCode",
            Doc = "Every physical input an `InputBinding` can name: keyboard keys by their US-layout legend, "
                .. "mouse buttons and analogue motion, and the standard gamepad layout's buttons, axes and "
                .. "sticks. One enum rather than three, because a binding names ONE input and the question a "
                .. 'binding asks is never "which device" -- that is `InputBinding.DeviceType`, which says which '
                .. "device a PROMPT should be drawn for.\n\nItems are analogue or digital, and mixing them is "
                .. 'the one mistake this enum makes easy: a Bool action bound to `LeftStickX` is asking "is '
                .. 'this axis pressed", which has no answer. The engine treats an analogue source on a Bool '
                .. "action as pressed past half deflection rather than refusing, because refusing at bind time "
                .. "would mean a rebinding UI could hand the player an unusable choice.",
            Items = {
                {
                    Name = "Unknown",
                    Value = 0,
                    Doc = "No input at all. What an `InputBinding` carries before anything is assigned to it, and what a "
                        .. "binding whose key the running platform does not have reads back as.",
                },
                { Name = "Escape", Value = 1 },
                { Name = "F1", Value = 2 },
                { Name = "F2", Value = 3 },
                { Name = "F3", Value = 4 },
                { Name = "F4", Value = 5 },
                { Name = "F5", Value = 6 },
                { Name = "F6", Value = 7 },
                { Name = "F7", Value = 8 },
                { Name = "F8", Value = 9 },
                { Name = "F9", Value = 10 },
                { Name = "F10", Value = 11 },
                { Name = "F11", Value = 12 },
                { Name = "F12", Value = 13 },
                { Name = "A", Value = 14 },
                { Name = "B", Value = 15 },
                { Name = "C", Value = 16 },
                { Name = "D", Value = 17 },
                { Name = "E", Value = 18 },
                { Name = "F", Value = 19 },
                { Name = "G", Value = 20 },
                { Name = "H", Value = 21 },
                { Name = "I", Value = 22 },
                { Name = "J", Value = 23 },
                { Name = "K", Value = 24 },
                { Name = "L", Value = 25 },
                { Name = "M", Value = 26 },
                { Name = "N", Value = 27 },
                { Name = "O", Value = 28 },
                { Name = "P", Value = 29 },
                { Name = "Q", Value = 30 },
                { Name = "R", Value = 31 },
                { Name = "S", Value = 32 },
                { Name = "T", Value = 33 },
                { Name = "U", Value = 34 },
                { Name = "V", Value = 35 },
                { Name = "W", Value = 36 },
                { Name = "X", Value = 37 },
                { Name = "Y", Value = 38 },
                { Name = "Z", Value = 39 },
                { Name = "Digit0", Value = 40 },
                { Name = "Digit1", Value = 41 },
                { Name = "Digit2", Value = 42 },
                { Name = "Digit3", Value = 43 },
                { Name = "Digit4", Value = 44 },
                { Name = "Digit5", Value = 45 },
                { Name = "Digit6", Value = 46 },
                { Name = "Digit7", Value = 47 },
                { Name = "Digit8", Value = 48 },
                { Name = "Digit9", Value = 49 },
                { Name = "Space", Value = 50 },
                { Name = "Return", Value = 51 },
                { Name = "Tab", Value = 52 },
                { Name = "Backspace", Value = 53 },
                { Name = "LeftShift", Value = 54 },
                { Name = "RightShift", Value = 55 },
                { Name = "LeftControl", Value = 56 },
                { Name = "RightControl", Value = 57 },
                { Name = "LeftAlt", Value = 58 },
                { Name = "RightAlt", Value = 59 },
                { Name = "Left", Value = 60 },
                { Name = "Right", Value = 61 },
                { Name = "Up", Value = 62 },
                { Name = "Down", Value = 63 },
                {
                    Name = "Home",
                    Value = 64,
                    Doc = "**In the keyboard block rather than appended**, which is what shifted every mouse "
                        .. "and gamepad item below it by three. The resolver reaches a keyboard code by "
                        .. "SUBTRACTION over a contiguous block and a `static_assert` holds this list and "
                        .. "`platform::Key` to the same length, so appending was not open. Renumbering is "
                        .. "safe because a scene stores an enum item by NAME -- which `scene_file.cpp` says "
                        .. "it does precisely so that renumbering survives.",
                },
                { Name = "End", Value = 65 },
                { Name = "Delete", Value = 66 },
                { Name = "MouseLeft", Value = 67 },
                { Name = "MouseMiddle", Value = 68 },
                { Name = "MouseRight", Value = 69 },
                { Name = "MouseX1", Value = 70 },
                { Name = "MouseX2", Value = 71 },
                {
                    Name = "MouseMovement",
                    Value = 72,
                    Doc = "The pointer's motion this tick, as a Vector2 in pixels. An analogue source: bind it to a "
                        .. "Direction2D action for a look control, never to a Bool one.",
                },
                {
                    Name = "MouseWheel",
                    Value = 73,
                    Doc = "The wheel's motion this tick, in notches, positive away from the user. Analogue, like "
                        .. "MouseMovement -- a Direction1D source rather than a button.",
                },
                {
                    Name = "ButtonSouth",
                    Value = 74,
                    Doc = "The bottom face button: A on an Xbox pad, B on a Nintendo one, Cross on a PlayStation one. Named "
                        .. "by POSITION because the legend moves with the hardware and a saved binding must not.",
                },
                {
                    Name = "ButtonEast",
                    Value = 75,
                    Doc = "The right face button.",
                },
                {
                    Name = "ButtonWest",
                    Value = 76,
                    Doc = "The left face button.",
                },
                {
                    Name = "ButtonNorth",
                    Value = 77,
                    Doc = "The top face button.",
                },
                { Name = "ButtonBack", Value = 78 },
                { Name = "ButtonGuide", Value = 79 },
                { Name = "ButtonStart", Value = 80 },
                {
                    Name = "ButtonLeftStick",
                    Value = 81,
                    Doc = "The left stick pressed IN. The stick's direction is LeftThumbstick.",
                },
                {
                    Name = "ButtonRightStick",
                    Value = 82,
                    Doc = "The right stick pressed IN. The stick's direction is RightThumbstick.",
                },
                { Name = "ButtonLeftShoulder", Value = 83 },
                { Name = "ButtonRightShoulder", Value = 84 },
                { Name = "DpadUp", Value = 85 },
                { Name = "DpadDown", Value = 86 },
                { Name = "DpadLeft", Value = 87 },
                { Name = "DpadRight", Value = 88 },
                { Name = "LeftStickX", Value = 89 },
                { Name = "LeftStickY", Value = 90 },
                { Name = "RightStickX", Value = 91 },
                { Name = "RightStickY", Value = 92 },
                {
                    Name = "LeftTrigger",
                    Value = 93,
                    Doc = "The left trigger, 0 released to 1 fully pulled. Analogue over the positive range only, which is "
                        .. "why a released trigger reads 0 rather than -1.",
                },
                {
                    Name = "RightTrigger",
                    Value = 94,
                    Doc = "The right trigger, 0 released to 1 fully pulled.",
                },
                {
                    Name = "LeftThumbstick",
                    Value = 95,
                    Doc = "The left stick as a Vector2, both axes at once -- the source a Direction2D action binds for "
                        .. "movement. The individual axes are LeftStickX and LeftStickY.",
                },
                {
                    Name = "RightThumbstick",
                    Value = 96,
                    Doc = "The right stick as a Vector2.",
                },
                {
                    Name = "Virtual1",
                    Value = 97,
                    Doc = "A source that is not hardware, written by `InputService:SetVirtualState` (§2.4). It is "
                        .. "a KeyCode like any other, so it binds, sinks, resolves and REPLAYS like any other -- "
                        .. "which is the whole point: a HUD button that fed an action through a side door would "
                        .. "be a second input model, and the recorded stream would not contain it.\n\n**It "
                        .. "carries a VALUE and not a press.** A button writes 1 and 0; a slider writes anything "
                        .. "between. Bound to a `Bool` action it counts as pressed past half deflection, which "
                        .. "is the rule every analogue source here follows.\n\nSixteen of them, `Virtual1` to "
                        .. "`Virtual16`: the first four are also the halves of the two virtual sticks, and the "
                        .. "rest are for the buttons a phone's HUD grows -- an action game's skill bar ran out "
                        .. "of four on its first screen (D443). They are numbered rather than named for a "
                        .. "purpose, because the purpose is the game's.",
                },
                { Name = "Virtual2", Value = 98 },
                { Name = "Virtual3", Value = 99 },
                { Name = "Virtual4", Value = 100 },
                {
                    Name = "VirtualStick1",
                    Value = 101,
                    Doc = "`Virtual1` and `Virtual2` as one Vector2 -- the source a `Direction2D` action binds "
                        .. "for an on-screen thumbstick. The same relationship `LeftThumbstick` has to "
                        .. "`LeftStickX` and `LeftStickY`, and the reason the virtual seam carries a value: "
                        .. "design an on-screen BUTTON and the thumbstick does not fit later.",
                },
                {
                    Name = "VirtualStick2",
                    Value = 102,
                    Doc = "`Virtual3` and `Virtual4` as one Vector2.",
                },
                {
                    Name = "Minus",
                    Value = 103,
                    Doc = "**The rest of the keyboard, appended** (D210): punctuation, the navigation keys "
                        .. "and the keypad, by their US-layout legend. At the END of the enum rather than in "
                        .. "the keyboard block, so no item a game already compares by value moves; the "
                        .. "resolver reaches them as a second keyboard block.",
                },
                { Name = "Equals", Value = 104 },
                { Name = "LeftBracket", Value = 105 },
                { Name = "RightBracket", Value = 106 },
                { Name = "Backslash", Value = 107 },
                { Name = "Semicolon", Value = 108 },
                { Name = "Quote", Value = 109 },
                { Name = "Backquote", Value = 110 },
                { Name = "Comma", Value = 111 },
                { Name = "Period", Value = 112 },
                { Name = "Slash", Value = 113 },
                { Name = "CapsLock", Value = 114 },
                { Name = "Insert", Value = 115 },
                { Name = "PageUp", Value = 116 },
                { Name = "PageDown", Value = 117 },
                { Name = "NumLock", Value = 118 },
                { Name = "Keypad0", Value = 119 },
                { Name = "Keypad1", Value = 120 },
                { Name = "Keypad2", Value = 121 },
                { Name = "Keypad3", Value = 122 },
                { Name = "Keypad4", Value = 123 },
                { Name = "Keypad5", Value = 124 },
                { Name = "Keypad6", Value = 125 },
                { Name = "Keypad7", Value = 126 },
                { Name = "Keypad8", Value = 127 },
                { Name = "Keypad9", Value = 128 },
                { Name = "KeypadPeriod", Value = 129 },
                { Name = "KeypadDivide", Value = 130 },
                { Name = "KeypadMultiply", Value = 131 },
                { Name = "KeypadMinus", Value = 132 },
                { Name = "KeypadPlus", Value = 133 },
                { Name = "KeypadEnter", Value = 134 },
                { Name = "KeypadEquals", Value = 135 },
                {
                    Name = "Virtual5",
                    Value = 136,
                    Doc = "**The rest of the virtual keys, appended** (D443): `Virtual5` to `Virtual16`, at "
                        .. "the END of the enum so that no item a game already compares by value moves. Each is "
                        .. "what `Virtual1` is; none is half of a stick.",
                },
                { Name = "Virtual6", Value = 137 },
                { Name = "Virtual7", Value = 138 },
                { Name = "Virtual8", Value = 139 },
                { Name = "Virtual9", Value = 140 },
                { Name = "Virtual10", Value = 141 },
                { Name = "Virtual11", Value = 142 },
                { Name = "Virtual12", Value = 143 },
                { Name = "Virtual13", Value = 144 },
                { Name = "Virtual14", Value = 145 },
                { Name = "Virtual15", Value = 146 },
                { Name = "Virtual16", Value = 147 },
                {
                    Name = "SwipeUp",
                    Value = 148,
                    Doc = "**A swipe, as a key** (D462): a finger -- or the mouse with its left button down "
                        .. "-- that has travelled `InputService.SwipeThreshold` up the screen. Bound to an "
                        .. "`InputAction` like any key, it is a press that lasts one tick, so a puzzle moves "
                        .. "a piece by `SwipeLeft` beside the arrow key and has nothing to recognise itself. "
                        .. "It is said while the finger is still down, as soon as it has gone far enough; one "
                        .. "long drag one way is one swipe, and a change of direction is the next. A press "
                        .. "the interface took starts none.",
                },
                { Name = "SwipeDown", Value = 149 },
                { Name = "SwipeLeft", Value = 150 },
                { Name = "SwipeRight", Value = 151 },
            } :: { Schema.EnumItem },
        },
        {
            Name = "InputActionType",
            Doc = "What an `InputAction` produces, and therefore what `GetState` returns (§2.4, ADR 0029). The "
                .. "type is the action's, not the binding's: one action of one type may be driven by a key, a "
                .. "stick and an on-screen button at once, and every one of them has to answer in the same "
                .. "currency.",
            Items = {
                {
                    Name = "Bool",
                    Value = 0,
                    Doc = "Pressed or not. `GetState` returns a boolean, and this is the only type that fires "
                        .. "`Pressed` and `Released`.",
                },
                {
                    Name = "Direction1D",
                    Value = 1,
                    Doc = "One signed axis. `GetState` returns a number, -1 to 1 from a pair of keys and the "
                        .. "analogue value from a trigger or a wheel.",
                },
                {
                    Name = "Direction2D",
                    Value = 2,
                    Doc = "Two signed axes. `GetState` returns a `Vector2`, from a stick, from pointer motion, or "
                        .. "from the four composite keys on a binding.",
                },
                {
                    Name = "Direction3D",
                    Value = 3,
                    Doc = "Three signed axes, returning a `vector`. **Declared and not driveable in v1**: no "
                        .. "binding names three axes, and inventing one would be designing the "
                        .. "binding surface backwards from an enum item. `GetState` returns the zero vector. The "
                        .. "item exists because the enum is designed once and an item added later is the harder "
                        .. "break -- the same reasoning `Enum.RunContext` carries.",
                },
                {
                    Name = "ViewportPosition",
                    Value = 4,
                    Doc = "Where the pointer IS, rather than how it moved. `GetState` returns a `Vector2` in "
                        .. "window pixels with the origin at the top left.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "InputDeviceType",
            Doc = "A family of input hardware. Used for two things and neither of them is dispatch: "
                .. "`InputService.LastInputDeviceType` says what the player touched most recently, so a HUD can "
                .. "switch its prompts, and `InputBinding.DeviceType` says which family a binding is the "
                .. "preferred one for, which is what `GetPreferredBinding` answers with.",
            Items = {
                { Name = "KeyboardMouse", Value = 0 },
                { Name = "Gamepad", Value = 1 },
                {
                    Name = "Touch",
                    Value = 2,
                    Doc = "What a VIRTUAL binding reports -- one keyed to `Enum.KeyCode.Virtual1` and its "
                        .. "siblings, which is how a HUD button or an on-screen thumbstick drives an action "
                        .. "(§2.4) -- and what a finger on a touchscreen makes the last device used, so a game "
                        .. "that shows on-screen controls only on a phone can ask `InputService.LastInputDeviceType`.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "UserInputType",
            Doc = "What KIND of input an `InputObject` came from, on `InputService`'s raw events (§2.4, "
                .. "ADR 0041). It is coarser than `KeyCode` on purpose: a handler that only cares whether "
                .. "something was typed at all reads this and stops, and one that cares which key reads "
                .. "`KeyCode` beside it.\n\nThe mouse buttons are separate items rather than one `Mouse` "
                .. "item plus a `KeyCode`, because the overwhelmingly common test is `== MouseButton1` and "
                .. "spelling that as two comparisons would be worse for no gain.",
            Items = {
                {
                    Name = "None",
                    Value = 0,
                    Doc = "What a default-constructed `InputObject` carries. Nothing produces it: an event that "
                        .. "fired came from something.",
                },
                { Name = "Keyboard", Value = 1 },
                { Name = "MouseButton1", Value = 2, Doc = "The primary button -- left on a right-handed mouse." },
                { Name = "MouseButton2", Value = 3, Doc = "The secondary button." },
                { Name = "MouseButton3", Value = 4, Doc = "The middle button, usually the wheel pressed in." },
                {
                    Name = "MouseMovement",
                    Value = 5,
                    Doc = "The pointer moved. Fires `InputChanged` and never `InputBegan` or `InputEnded`: "
                        .. "motion has no beginning and no end.",
                },
                {
                    Name = "MouseWheel",
                    Value = 6,
                    Doc = "The wheel turned. `InputChanged`, with the amount in `Delta.z` -- one notch is 1.",
                },
                {
                    Name = "Gamepad",
                    Value = 7,
                    Doc = "A pad button or stick. `KeyCode` says which, and a stick's deflection arrives as "
                        .. "`InputChanged` with the axis value in `Position`.",
                },
                {
                    Name = "Touch",
                    Value = 8,
                    Doc = "A finger on a touchscreen: `InputBegan` when it lands, `InputChanged` as it moves, "
                        .. "`InputEnded` when it lifts, with `Position` in window pixels and `TouchId` saying "
                        .. "which finger. A tap shorter than a frame still begins and ends. Never consumed by "
                        .. "the interface, which follows one pointer: a game's own on-screen controls are what "
                        .. "these are for, and they drive actions through `InputService:SetVirtualState`.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "InputRate",
            Doc = "Which clock an `InputContext` is dispatched on (ADR 0039). It is a property of the context "
                .. "rather than of the action, and it is NOT derived from `Priority`: priority orders "
                .. "fallthrough, and overloading it would make a low-priority render-rate context "
                .. "inexpressible -- and would change an action's determinism class the day somebody re-tuned a "
                .. "number for an unrelated reason.",
            Items = {
                {
                    Name = "Simulation",
                    Value = 0,
                    Doc = "The sim tick, with the fixed timestep. The default, because it is the safe one: an "
                        .. "action nobody thought about fires where determinism holds and where a replay can "
                        .. "see it. Everything a game DECIDES with should be here.",
                },
                {
                    Name = "Render",
                    Value = 1,
                    Doc = "The render frame, with a variable dt. For camera look and for UI, where a tick of "
                        .. "latency is visible. A gameplay decision taken from a render-rate action is "
                        .. "frame-rate-dependent by construction, and it is not recorded by the input replay -- "
                        .. "a render frame is not a unit the replay has.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "EasingStyle",
            Doc = "The shape of a tween's progress curve (§2.1). The set is the one tutorials and design tools "
                .. "already speak, so a curve named in one transfers to the other; the formulas are the "
                .. "published ones and `tests/fixtures/easing.luau` pins them as literal numbers rather than as "
                .. "a re-derivation.",
            Items = {
                {
                    Name = "Linear",
                    Value = 0,
                    Doc = "No easing at all: alpha is the progress. The only style whose direction changes "
                        .. "nothing.",
                },
                { Name = "Sine", Value = 1 },
                { Name = "Quad", Value = 2 },
                { Name = "Cubic", Value = 3 },
                { Name = "Quart", Value = 4 },
                { Name = "Quint", Value = 5 },
                { Name = "Exponential", Value = 6 },
                { Name = "Circular", Value = 7 },
                {
                    Name = "Back",
                    Value = 8,
                    Doc = "Overshoots the far end and comes back. Values outside 0-1 are the point of it, so a "
                        .. "property tweened with this one has to tolerate them.",
                },
                { Name = "Bounce", Value = 9, Doc = "Settles by bouncing, like something dropped." },
                {
                    Name = "Elastic",
                    Value = 10,
                    Doc = "Oscillates past both ends before settling. Like `Back`, it leaves 0-1.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "EasingDirection",
            Doc = "Which end of a tween the easing applies to (§2.1).",
            Items = {
                { Name = "In", Value = 0, Doc = "Eased at the start, abrupt at the end." },
                { Name = "Out", Value = 1, Doc = "Abrupt at the start, eased at the end. The usual choice." },
                {
                    Name = "InOut",
                    Value = 2,
                    Doc = "Eased at both ends: the `In` curve over the first half, mirrored over the second.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "PlaybackState",
            Doc = "Where a `Tween` is in its life (§2.1). A state rather than a pair of booleans, because "
                .. '"finished" and "stopped early" are different facts and code that acts on one usually '
                .. "must not act on the other.",
            Items = {
                { Name = "Begin", Value = 0, Doc = "Created and never played." },
                {
                    Name = "Delayed",
                    Value = 1,
                    Doc = "Playing, inside its `TweenInfo.DelayTime`, and not yet writing anything.",
                },
                { Name = "Playing", Value = 2 },
                {
                    Name = "Paused",
                    Value = 3,
                    Doc = "Stopped where it was. `Play` resumes from there rather than restarting.",
                },
                {
                    Name = "Completed",
                    Value = 4,
                    Doc = "Reached its end. The goal values have been written exactly, not approached: a tween "
                        .. "that stopped at 0.9999 of the way would leave a property somebody has to explain.",
                },
                {
                    Name = "Cancelled",
                    Value = 5,
                    Doc = "Stopped before its end by `Cancel`. The property keeps whatever value it had reached "
                        .. "-- cancelling is not undoing.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "PromiseState",
            Doc = "Where a `Promise` is in its life (ADR 0094). Settled once, and never again.",
            Items = {
                { Name = "Started", Value = 0, Doc = "Neither resolved nor rejected yet." },
                { Name = "Resolved", Value = 1, Doc = "Resolved, with the values `Await` returns." },
                { Name = "Rejected", Value = 2, Doc = "Rejected, with the values its handlers receive." },
                {
                    Name = "Cancelled",
                    Value = 3,
                    Doc = "Cancelled before it settled: its executor stopped and its handlers will not run.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "FillDirection",
            Doc = "Which way a `UIListLayout` stacks its siblings (§2.2).",
            Items = {
                { Name = "Horizontal", Value = 0 },
                { Name = "Vertical", Value = 1 },
            } :: { Schema.EnumItem },
        },
        {
            Name = "HorizontalAlignment",
            Doc = "Alignment along the X axis. One set of values for two jobs: a `TextLabel`'s "
                .. "`TextXAlignment` places its words with it and a `UIListLayout`'s `HorizontalAlignment` "
                .. "places its children with it.",
            Items = {
                { Name = "Left", Value = 0 },
                { Name = "Center", Value = 1 },
                { Name = "Right", Value = 2 },
            } :: { Schema.EnumItem },
        },
        {
            Name = "VerticalAlignment",
            Doc = "Alignment along the Y axis: a `TextLabel`'s `TextYAlignment` and a layout's "
                .. "`VerticalAlignment`.",
            Items = {
                { Name = "Top", Value = 0 },
                { Name = "Center", Value = 1 },
                { Name = "Bottom", Value = 2 },
            } :: { Schema.EnumItem },
        },
        {
            Name = "SortOrder",
            Doc = "What order a `UIListLayout` lays its children out in (§2.2).",
            Items = {
                {
                    Name = "Name",
                    Value = 0,
                    Doc = "By `Name`, ordinal by code point. Stable across runs, which document order also is "
                        .. "-- the difference is that this one survives the children being rebuilt in a "
                        .. "different order.",
                },
                {
                    Name = "LayoutOrder",
                    Value = 1,
                    Doc = "By each child's `LayoutOrder`, ties broken by document order. The usual choice, and "
                        .. "the only one that lets a list be reordered without renaming anything.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "AutomaticSize",
            Doc = "Which axes a `UIObject` sizes to fit its contents on, overriding that half of `Size` "
                .. "(§2.2). Text and lists are what need it: a label whose box is smaller than its words is "
                .. "the commonest UI defect there is.",
            Items = {
                { Name = "None", Value = 0 },
                { Name = "X", Value = 1 },
                { Name = "Y", Value = 2 },
                { Name = "XY", Value = 3 },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ScaleType",
            Doc = "How an `ImageLabel` fits its image into its box (§2.2).",
            Items = {
                {
                    Name = "Stretch",
                    Value = 0,
                    Doc = "The whole image into the whole box, aspect ratio and all.",
                },
                {
                    Name = "Slice",
                    Value = 1,
                    Doc = "Nine-slice: the four corners are drawn at their own size, the four edges stretch "
                        .. "along one axis and the middle stretches along both. `SliceCenter` says where the "
                        .. "cuts are. This is how a panel or a button keeps its rounded corners at any size.",
                },
                {
                    Name = "Tile",
                    Value = 2,
                    Doc = "Repeated at its own size until the box is full.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "NetworkTopology",
            Doc = "Which of the four postures this process runs in (ADR 0070). Decided at start from the "
                .. "command line and never changed, so a script reads it and never sets it.",
            Items = {
                {
                    Name = "Solo",
                    Value = 0,
                    Doc = "Nothing is networked. This process decides everything, which is why "
                        .. "`NetworkService.Authority` is true here too.",
                },
                {
                    Name = "Host",
                    Value = 1,
                    Doc = "The authority, and a player of its own game in the same process.",
                },
                {
                    Name = "Dedicated",
                    Value = 2,
                    Doc = "The authority with no player of its own: a server with no window.",
                },
                {
                    Name = "Replica",
                    Value = 3,
                    Doc = "A client of somebody else's authority. It shows the world it is sent and decides "
                        .. "none of it.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ParticleShape",
            Doc = "What one particle looks like before its colour is applied. Drawn in the shader rather "
                .. "than sampled from a texture, so a puff of smoke needs no file.",
            Items = {
                {
                    Name = "Soft",
                    Value = 0,
                    Doc = "A round puff that fades to nothing at its edge: smoke, dust, steam, a glow.",
                },
                {
                    Name = "Disc",
                    Value = 1,
                    Doc = "A round dot with a crisp edge: sparks, bubbles, confetti.",
                },
                {
                    Name = "Square",
                    Value = 2,
                    Doc = "A square facing the camera: pixels, debris, blocky confetti.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "Face",
            Doc = "One face of a part, for what is drawn on it (F3). Named from the part's own point of view.",
            Items = {
                { Name = "Front", Value = 0, Doc = "The face its `LookVector` comes out of." },
                { Name = "Back", Value = 1, Doc = "Opposite the front." },
                { Name = "Top", Value = 2, Doc = "The face its `UpVector` comes out of." },
                { Name = "Bottom", Value = 3, Doc = "Opposite the top." },
                { Name = "Right", Value = 4, Doc = "The face its `RightVector` comes out of." },
                { Name = "Left", Value = 5, Doc = "Opposite the right." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "BlockOpacity",
            Doc = "How much of what is behind a block shows through it (V1).",
            Items = {
                {
                    Name = "Opaque",
                    Value = 0,
                    Doc = "Nothing. A face against it is hidden, and it shades the corners it touches.",
                },
                {
                    Name = "Cutout",
                    Value = 1,
                    Doc = "Holes where its image is transparent: leaves, a fence, a grate. Each pixel is there or "
                        .. "not, and the faces between two cutout blocks are drawn, which is what makes a tree full.",
                },
                {
                    Name = "Translucent",
                    Value = 2,
                    Doc = "Blended over what is behind it: glass, water, ice. No face between two blocks of the "
                        .. "same type, so a lake is one surface.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "Shape2D",
            Doc = "The outline a `Part2D` is drawn and collides as (the 2D layer, phase 3).",
            Items = {
                { Name = "Box", Value = 0, Doc = "A rectangle the part's `Size`." },
                {
                    Name = "Circle",
                    Value = 1,
                    Doc = "A disc as wide as the smaller of the two sides, so a stretched `Size` gives a round "
                        .. "wheel rather than an egg, and the picture and the collider agree.",
                },
                {
                    Name = "Capsule",
                    Value = 2,
                    Doc = "A rectangle with round ends at the top and bottom: what a character stands on, because "
                        .. "its rounded foot slides over the seam between two tiles instead of catching.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "TextureFilter",
            Doc = "How a picture is sampled when it is drawn larger or smaller than it is.",
            Items = {
                { Name = "Linear", Value = 0, Doc = "Blended between texels: smooth, for painted art." },
                {
                    Name = "Nearest",
                    Value = 1,
                    Doc = "The nearest texel, unblended: sharp pixels, for pixel art at any zoom.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "CameraProjection",
            Doc = "How a `Camera` turns the world into a picture.",
            Items = {
                {
                    Name = "Perspective",
                    Value = 0,
                    Doc = "Far things smaller, by `FieldOfView`: the 3D camera.",
                },
                {
                    Name = "Orthographic",
                    Value = 1,
                    Doc = "Everything its own size wherever it is, `OrthographicSize` metres from the middle of "
                        .. "the view to its top edge: the 2D camera. It looks down the camera's -Z, so a 2D world "
                        .. "on the XY plane faces it.",
                },
            } :: { Schema.EnumItem },
        },
        -- Appended, never inserted: an enum's id is its place in this list, and ids
        -- travel on the wire and in snapshots.
        {
            Name = "ScreenOrientation",
            Doc = "Which ways up a game may be held, on a device that turns (`UIService.ScreenOrientation`). "
                .. "The two landscapes are named for the side the device's top ends up on.",
            Items = {
                {
                    Name = "LandscapeLeft",
                    Value = 0,
                    Doc = "Landscape, the top of the device on the left. Turning it over does not follow.",
                },
                {
                    Name = "LandscapeRight",
                    Value = 1,
                    Doc = "Landscape, the top of the device on the right.",
                },
                {
                    Name = "LandscapeSensor",
                    Value = 2,
                    Doc = "Either landscape, following the device as it is turned over. The default: most "
                        .. "games are drawn for a wide screen.",
                },
                {
                    Name = "Portrait",
                    Value = 3,
                    Doc = "Upright.",
                },
                {
                    Name = "Sensor",
                    Value = 4,
                    Doc = "Any way up, following the device -- and the player's own rotation lock.",
                },
            } :: { Schema.EnumItem },
        },
        -- ADR 0106. At the END: an enum's id is its position, and it travels.
        {
            Name = "NetworkState",
            Doc = "Where this machine stands in a networked game (ADR 0106): what `NetworkService.State` says, "
                .. "and what `Join`, `Host` and `Disconnect` move it between.",
            Items = {
                {
                    Name = "Offline",
                    Value = 0,
                    Doc = "Solo: nobody else, and this machine decides the world.",
                },
                {
                    Name = "Connecting",
                    Value = 1,
                    Doc = "`Join` was called and the server has not answered yet.",
                },
                {
                    Name = "Connected",
                    Value = 2,
                    Doc = "In a match another machine decides: this one is a client.",
                },
                {
                    Name = "Hosting",
                    Value = 3,
                    Doc = "This machine decides the world, a player sits at it, and others may join.",
                },
                {
                    Name = "Serving",
                    Value = 4,
                    Doc = "A dedicated server (`--serve`): it decides the world and nobody sits at it.",
                },
                {
                    Name = "Reconnecting",
                    Value = 5,
                    Doc = "The connection to the server was lost and this machine is dialling it again. The "
                        .. "world stands as it was; a game can say so and stop predicting.",
                },
            } :: { Schema.EnumItem },
        },
        -- ADR 0110, the six a gradient and a stroke are described with. At the END.
        {
            Name = "GradientType",
            Doc = "The shape of a `UIGradient`.",
            Items = {
                {
                    Name = "Linear",
                    Value = 0,
                    Doc = "Across the element, in the direction `Rotation` points; `Offset` moves where it is centred.",
                },
                {
                    Name = "Radial",
                    Value = 1,
                    Doc = "Out from the element's centre (plus `Offset`) to a radius of (width + height) / 4. "
                        .. "`Rotation` does nothing.",
                },
                {
                    Name = "Conical",
                    Value = 2,
                    Doc = "Clockwise round the element's centre (plus `Offset`), a whole turn, starting where "
                        .. "`Rotation` points.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "GradientTileMode",
            Doc = "What a `UIGradient` draws past the end of its sequence.",
            Items = {
                {
                    Name = "Clamp",
                    Value = 0,
                    Doc = "The colours at the two ends carry on, filling everything before and after.",
                },
                {
                    Name = "Repeat",
                    Value = 1,
                    Doc = "The sequence again from its start: a seam wherever its two ends differ.",
                },
                {
                    Name = "Mirror",
                    Value = 2,
                    Doc = "The sequence backwards, then forwards, and so on: no seam.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ApplyStrokeMode",
            Doc = "What a `UIStroke` outlines.",
            Items = {
                {
                    Name = "Contextual",
                    Value = 0,
                    Doc = "The text of a text object; the border of anything else.",
                },
                {
                    Name = "Border",
                    Value = 1,
                    Doc = "The element's border, even on a text object.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "LineJoinMode",
            Doc = "How a `UIStroke` turns a corner.",
            Items = {
                {
                    Name = "Round",
                    Value = 0,
                    Doc = "Rounded: the corner is an arc as wide as the stroke.",
                },
                {
                    Name = "Bevel",
                    Value = 1,
                    Doc = "Cut off square across the corner.",
                },
                {
                    Name = "Miter",
                    Value = 2,
                    Doc = "Brought to a point.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "StrokeSizingMode",
            Doc = "What a `UIStroke`'s `Thickness` is measured in.",
            Items = {
                {
                    Name = "FixedSize",
                    Value = 0,
                    Doc = "Pixels.",
                },
                {
                    Name = "ScaledSize",
                    Value = 1,
                    Doc = "A fraction of the parent's shorter side -- or, on text, of the font size -- so the stroke "
                        .. "grows with what it outlines.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "BorderStrokePosition",
            Doc = "Where a border `UIStroke` lies against its element's edge.",
            Items = {
                {
                    Name = "Outer",
                    Value = 0,
                    Doc = "Wholly outside the element.",
                },
                {
                    Name = "Center",
                    Value = 1,
                    Doc = "Half outside and half inside.",
                },
                {
                    Name = "Inner",
                    Value = 2,
                    Doc = "Wholly inside the element.",
                },
            } :: { Schema.EnumItem },
        },
        -- ADR 0107. At the END.
        {
            Name = "ViewQuality",
            Doc = "How much of the renderer a `CameraTexture` asks for.",
            Items = {
                {
                    Name = "Full",
                    Value = 0,
                    Doc = "Everything the main view draws: shadows and the look's effects.",
                },
                {
                    Name = "Simple",
                    Value = 1,
                    Doc = "No shadows and no effects -- a feed, a monitor, a minimap.",
                },
            } :: { Schema.EnumItem },
        },
        -- ADR 0125. At the END.
        {
            Name = "SceneLoadStatus",
            Doc = "Where a `SceneLoad` is.",
            Items = {
                { Name = "Preparing", Value = 0, Doc = "Its file is being read and its meshes loaded." },
                { Name = "Ready", Value = 1, Doc = "Prepared, waiting for `Activate()`." },
                { Name = "Activating", Value = 2, Doc = "The switch has begun: the old scene is closing." },
                { Name = "Done", Value = 3, Doc = "The scene is open." },
                { Name = "Failed", Value = 4, Doc = "It could not be prepared; `Error` says why." },
                { Name = "Cancelled", Value = 5, Doc = "`Cancel()`, or another load, dropped it." },
            } :: { Schema.EnumItem },
        },
        -- ADR 0131. At the END.
        {
            Name = "AssetFetchStatus",
            Doc = "How an item of `ContentProvider:PreloadAsync` ended.",
            Items = {
                { Name = "Success", Value = 0, Doc = "Everything it names is loaded." },
                { Name = "Failure", Value = 1, Doc = "Something it names could not be found or read." },
            } :: { Schema.EnumItem },
        },
        -- ADR 0126. At the END.
        {
            Name = "ProximityPromptExclusivity",
            Doc = "Which of several `ProximityPrompt`s in reach show.",
            Items = {
                { Name = "OnePerButton", Value = 0, Doc = "The nearest of those on the same key." },
                { Name = "OneGlobally", Value = 1, Doc = "The nearest of all that are `OneGlobally`." },
                { Name = "AlwaysShow", Value = 2, Doc = "This one whenever it is in reach." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ProximityPromptStyle",
            Doc = "Who draws a `ProximityPrompt`.",
            Items = {
                { Name = "Default", Value = 0, Doc = "The engine: the key, the texts and a hold ring." },
                { Name = "Custom", Value = 1, Doc = "The game, on `PromptShown` and `PromptHidden`." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ProximityPromptInputType",
            Doc = "The input a shown `ProximityPrompt` is drawn for.",
            Items = {
                { Name = "Keyboard", Value = 0, Doc = "A key." },
                { Name = "Gamepad", Value = 1, Doc = "A gamepad button." },
                { Name = "Touch", Value = 2, Doc = "A tap on the prompt." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "DragDetectorDragStyle",
            Doc = "How a `DragDetector` turns the pointer's motion into its part's (ADR 0126).",
            Items = {
                { Name = "TranslateLine", Value = 0, Doc = "Along `Axis`, through where it was grabbed." },
                { Name = "TranslatePlane", Value = 1, Doc = "Across the plane `Axis` is the normal of." },
                { Name = "TranslateViewPlane", Value = 2, Doc = "Across the plane facing the camera." },
                { Name = "RotateAxis", Value = 3, Doc = "Turned about `Axis`, through its pivot." },
                { Name = "RotateTrackball", Value = 4, Doc = "Turned any way about its pivot, as a ball." },
                { Name = "Scriptable", Value = 5, Doc = "Not moved: the events say where the pointer is." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "DragDetectorResponseStyle",
            Doc = "What a `DragDetector` does with where its part is dragged to.",
            Items = {
                { Name = "Geometric", Value = 0, Doc = "Puts it there." },
                {
                    Name = "Physical",
                    Value = 1,
                    Doc = "Pulls an unanchored part there with a force under `MaxForce`; an anchored one is put there.",
                },
                { Name = "Custom", Value = 2, Doc = "Nothing: the events report, and the game moves it." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "TerrainPaintMode",
            Doc = "What a stroke of `Terrain:PaintBall` does with the two materials a voxel holds (ADR 0114).",
            Items = {
                { Name = "Replace", Value = 0, Doc = "The ground becomes the material, whatever showed before." },
                { Name = "Blend", Value = 1, Doc = "The material shows more over what is there, by the strength." },
                { Name = "Under", Value = 2, Doc = "The material goes under what was painted over it." },
                { Name = "Erase", Value = 3, Doc = "What was painted over shows less, revealing what is under." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "WaterShape",
            Doc = "Where a `Water` is (ADR 0118).",
            Items = {
                { Name = "Ocean", Value = 0, Doc = "Everywhere, without end: the sea." },
                { Name = "Box", Value = 1, Doc = "The older name of `Pool`: the same rectangle." },
                {
                    Name = "Spline",
                    Value = 2,
                    Doc = "The older river: straight from one `WaterPoint` child to the next, and level -- every "
                        .. "point at the water's `SurfaceLevel`, whatever its own height.",
                },
                {
                    Name = "Lake",
                    Value = 3,
                    Doc = "The area inside the closed curve through its `WaterPoint` children, level at "
                        .. "`SurfaceLevel` (ADR 0146). It needs three points.",
                },
                {
                    Name = "River",
                    Value = 4,
                    Doc = "A ribbon along the curve through its `WaterPoint` children, first to last (ADR 0146). "
                        .. "Each point's height is the surface's there, so a river descends; its `Width` and "
                        .. "`Depth` are the river's there.",
                },
                { Name = "Pool", Value = 5, Doc = "A rectangle: `Size` across, centred on `Position`." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "RenderPriority",
            Doc = "Where a render step runs in the frame (ADR 0136): `RunService:BindToRenderStep` takes a "
                .. "number, and these are the landmarks -- `Enum.RenderPriority.Camera.Value + 1` runs just after "
                .. "the camera.",
            Items = {
                { Name = "First", Value = 0, Doc = "Before everything else in the frame." },
                { Name = "Input", Value = 100, Doc = "Where input a frame reads is turned into intent." },
                { Name = "Camera", Value = 200, Doc = "Where the camera is placed: the default rig runs here." },
                { Name = "Character", Value = 300, Doc = "After the camera: what follows where it looks." },
                { Name = "Last", Value = 2000, Doc = "After everything else, before `PreRender` fires." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "FocusLossReason",
            Doc = "Why a `TextInput` stopped taking the keyboard (ADR 0139): what `FocusLost` says beside "
                .. "`submitted`.",
            Items = {
                { Name = "Submitted", Value = 0, Doc = "Return, in a field that lets go on submit." },
                { Name = "Moved", Value = 1, Doc = "A press somewhere else, or Tab to the next field." },
                {
                    Name = "Cancelled",
                    Value = 2,
                    Doc = "Escape; with `RevertOnEscape` the text is what it was when focus came.",
                },
                {
                    Name = "Script",
                    Value = 3,
                    Doc = "`ReleaseFocus`, another field's `CaptureFocus`, or the field leaving the screen.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "TextInputKeyboard",
            Doc = "The on-screen keyboard a phone raises for a `TextInput` (ADR 0139), and the hint a "
                .. "desktop input method reads. A masked field raises a password keyboard whatever this says.",
            Items = {
                { Name = "Default", Value = 0, Doc = "Letters, with the system's suggestions." },
                { Name = "Number", Value = 1, Doc = "Digits." },
                { Name = "Decimal", Value = 2, Doc = "Digits and a decimal point." },
                { Name = "Phone", Value = 3, Doc = "A phone's dial pad." },
                { Name = "Email", Value = 4, Doc = "An address, with `@` at hand." },
                { Name = "Url", Value = 5, Doc = "A web address, with `/` and `.` at hand." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "SwipeDirection",
            Doc = "Which way a swipe went, as `InputService.TouchSwiped` says it. Up is towards the top of "
                .. "the window.",
            Items = {
                { Name = "Up", Value = 0 },
                { Name = "Down", Value = 1 },
                { Name = "Left", Value = 2 },
                { Name = "Right", Value = 3 },
            } :: { Schema.EnumItem },
        },
        {
            Name = "Platform",
            Doc = "The operating system a game is running on: `RunService.Platform`. For the few things "
                .. "that differ by system -- a store's rules, a path a player is told about. What a game "
                .. "shows on screen should follow what the machine HAS (`InputService.TouchAvailable`, "
                .. "`UIService.ViewportSize`), not what it is called: a tablet with a keyboard and a "
                .. "laptop with a touchscreen are both real.",
            Items = {
                { Name = "Windows", Value = 0 },
                { Name = "Linux", Value = 1 },
                { Name = "MacOS", Value = 2 },
                { Name = "Android", Value = 3 },
                { Name = "IOS", Value = 4 },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ActuatorType",
            Doc = "What a hinge, a rail or a ball joint does of itself.",
            Items = {
                { Name = "None", Value = 0, Doc = "Nothing: it only holds." },
                { Name = "Motor", Value = 1, Doc = "It moves at a speed." },
                { Name = "Servo", Value = 2, Doc = "It goes to a target and holds it." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ActuatorRelativeTo",
            Doc = "The frame a mover's vector is written in.",
            Items = {
                { Name = "Attachment0", Value = 0, Doc = "The first attachment's: it follows the part as it turns." },
                { Name = "Attachment1", Value = 1, Doc = "The second attachment's." },
                { Name = "World", Value = 2, Doc = "The world's." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "VelocityConstraintMode",
            Doc = "How much of a velocity a `LinearVelocity` holds.",
            Items = {
                { Name = "Line", Value = 0, Doc = "Only the speed along a line." },
                { Name = "Plane", Value = 1, Doc = "Only the speed across a plane." },
                { Name = "Vector", Value = 2, Doc = "All of it." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "PositionAlignmentMode",
            Doc = "What an `AlignPosition` pulls to.",
            Items = {
                { Name = "OneAttachment", Value = 0, Doc = "Its own `Position`." },
                { Name = "TwoAttachment", Value = 1, Doc = "Its `Attachment1`." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "OrientationAlignmentMode",
            Doc = "What an `AlignOrientation` turns to.",
            Items = {
                { Name = "OneAttachment", Value = 0, Doc = "Its own `CFrame`." },
                { Name = "TwoAttachment", Value = 1, Doc = "Its `Attachment1`." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "VibrationMotor",
            Doc = "One of the things in a controller that can shake (`HapticService`).",
            Items = {
                { Name = "Large", Value = 0, Doc = "The heavy, slow motor: a rumble." },
                { Name = "Small", Value = 1, Doc = "The light, fast one: a buzz." },
                { Name = "LeftTrigger", Value = 2, Doc = "The motor in the left trigger, where there is one." },
                { Name = "RightTrigger", Value = 3, Doc = "The motor in the right trigger, where there is one." },
                {
                    Name = "LeftHand",
                    Value = 4,
                    Doc = "The left of two controllers held one in each hand. No device this engine runs on "
                        .. "has one yet: it answers unsupported.",
                },
                { Name = "RightHand", Value = 5, Doc = "The right of the two, likewise." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "HighlightDepthMode",
            Doc = "Where a `Highlight` shows.",
            Items = {
                {
                    Name = "AlwaysOnTop",
                    Value = 0,
                    Doc = "Through whatever is in front of the shape: an ally behind a wall.",
                },
                {
                    Name = "Occluded",
                    Value = 1,
                    Doc = "Only where the shape itself is seen: what is behind a wall is not marked.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "TextureMode",
            Doc = "How a `Beam`'s or a `Trail`'s texture is laid along it.",
            Items = {
                {
                    Name = "Stretch",
                    Value = 0,
                    Doc = "Once over the whole length, however long that is.",
                },
                {
                    Name = "Wrap",
                    Value = 1,
                    Doc = "Repeated every `TextureLength` metres, measured from the start: on a trail the "
                        .. "picture slides along as the trail moves.",
                },
                {
                    Name = "Static",
                    Value = 2,
                    Doc = "Repeated every `TextureLength` metres and fixed where it was laid: a trail's "
                        .. "picture stays on the ground it was drawn over, as a tyre mark does. A beam's "
                        .. "does not run.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "StartCorner",
            Doc = "Which corner a `UIGridLayout` starts filling from.",
            Items = {
                {
                    Name = "TopLeft",
                    Value = 0,
                    Doc = "Left to right, top to bottom. How a page of text is read.",
                },
                {
                    Name = "TopRight",
                    Value = 1,
                    Doc = "Right to left, top to bottom.",
                },
                {
                    Name = "BottomLeft",
                    Value = 2,
                    Doc = "Left to right, bottom to top.",
                },
                {
                    Name = "BottomRight",
                    Value = 3,
                    Doc = "Right to left, bottom to top.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "UIFlexAlignment",
            Doc = "What a `UIListLayout` does with the room its children leave over, along one axis.",
            Items = {
                {
                    Name = "None",
                    Value = 0,
                    Doc = "Nothing: the children sit where the alignment puts them.",
                },
                {
                    Name = "Fill",
                    Value = 1,
                    Doc = "The children grow by equal amounts until the room is used.",
                },
                {
                    Name = "SpaceAround",
                    Value = 2,
                    Doc = "The room goes around each child: a whole share between two, half a share at each end.",
                },
                {
                    Name = "SpaceBetween",
                    Value = 3,
                    Doc = "The room goes between the children, and none at the ends.",
                },
                {
                    Name = "SpaceEvenly",
                    Value = 4,
                    Doc = "The room goes in equal shares between the children and at both ends.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "UIFlexMode",
            Doc = "How one child of a `UIListLayout` takes room that is left over, or gives up room that is "
                .. "short.",
            Items = {
                {
                    Name = "None",
                    Value = 0,
                    Doc = "It keeps its size.",
                },
                {
                    Name = "Grow",
                    Value = 1,
                    Doc = "It grows into room that is left over, and never shrinks.",
                },
                {
                    Name = "Shrink",
                    Value = 2,
                    Doc = "It shrinks when the line is too long, and never grows.",
                },
                {
                    Name = "Fill",
                    Value = 3,
                    Doc = "Both: it grows and it shrinks.",
                },
                {
                    Name = "Custom",
                    Value = 4,
                    Doc = "By `GrowRatio` and `ShrinkRatio`, each as it is written.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ItemLineAlignment",
            Doc = "Where a child of a `UIListLayout` sits across its line.",
            Items = {
                {
                    Name = "Automatic",
                    Value = 0,
                    Doc = "On a `UIFlexItem`: as the layout says. On the layout: by its own alignment across the "
                        .. "line.",
                },
                {
                    Name = "Start",
                    Value = 1,
                    Doc = "At the line's near edge: the top of a row, the left of a column.",
                },
                {
                    Name = "Center",
                    Value = 2,
                    Doc = "In the middle of the line.",
                },
                {
                    Name = "End",
                    Value = 3,
                    Doc = "At the line's far edge.",
                },
                {
                    Name = "Stretch",
                    Value = 4,
                    Doc = "As tall as a row, or as wide as a column.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "AspectType",
            Doc = "What a `UIAspectRatioConstraint` measures its element against.",
            Items = {
                {
                    Name = "FitWithinMaxSize",
                    Value = 0,
                    Doc = "The largest box of that shape inside the element's own size.",
                },
                {
                    Name = "ScaleWithParentSize",
                    Value = 1,
                    Doc = "The dominant axis takes the parent's whole extent and the other follows from the shape, "
                        .. "whatever that makes it.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "DominantAxis",
            Doc = "Which axis a `UIAspectRatioConstraint` keeps when it scales with its parent.",
            Items = {
                {
                    Name = "Width",
                    Value = 0,
                    Doc = "As wide as the parent; the height follows.",
                },
                {
                    Name = "Height",
                    Value = 1,
                    Doc = "As tall as the parent; the width follows.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "UIDragDetectorDragStyle",
            Doc = "How a `UIDragDetector` turns the pointer's motion into its element's (ADR 0128).",
            Items = {
                {
                    Name = "TranslatePlane",
                    Value = 0,
                    Doc = "The element follows the pointer on both axes.",
                },
                {
                    Name = "TranslateLine",
                    Value = 1,
                    Doc = "The element slides along `DragAxis` and nowhere else: a slider's thumb.",
                },
                {
                    Name = "Rotate",
                    Value = 2,
                    Doc = "The element turns about its own middle as the pointer goes round it: a dial.",
                },
                {
                    Name = "Scriptable",
                    Value = 3,
                    Doc = "Nothing moves: the signals say where the pointer is, and a script does the rest.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "UIDragDetectorResponseStyle",
            Doc = "What a `UIDragDetector` writes when its element is dragged.",
            Items = {
                {
                    Name = "Offset",
                    Value = 0,
                    Doc = "The offset of the element's `Position`, in its own units.",
                },
                {
                    Name = "Scale",
                    Value = 1,
                    Doc = "The scale of the element's `Position`, as a fraction of its parent.",
                },
                {
                    Name = "CustomOffset",
                    Value = 2,
                    Doc = "Nothing: `DragUDim2` and `DragRotation` say how far, as an offset, and a script applies "
                        .. "it.",
                },
                {
                    Name = "CustomScale",
                    Value = 3,
                    Doc = "Nothing: the same, with `DragUDim2` as a fraction of the parent.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "GraphicsQuality",
            Doc = "A quality level of `GraphicsService` (ADR 0147): a preset, or one of the two readings that "
                .. "are not one.",
            Items = {
                {
                    Name = "Low",
                    Value = 0,
                    Doc = "The least: three quarters of the resolution, short soft shadows, no bloom or occlusion.",
                },
                {
                    Name = "Medium",
                    Value = 1,
                    Doc = "What a handheld starts at.",
                },
                {
                    Name = "High",
                    Value = 2,
                    Doc = "What a desktop starts at, and what every reference picture of the engine is drawn at.",
                },
                {
                    Name = "Ultra",
                    Value = 3,
                    Doc = "The most the renderer has.",
                },
                {
                    Name = "Custom",
                    Value = 4,
                    Doc = "What `QualityLevel` reads as when any quality setting is not the preset's. Never written.",
                },
                {
                    Name = "Auto",
                    Value = 5,
                    Doc = "The preset this machine was measured for.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "GraphicsLevel",
            Doc = "How far one group of graphics settings is turned up.",
            Items = {
                {
                    Name = "Low",
                    Value = 0,
                    Doc = "The least.",
                },
                {
                    Name = "Medium",
                    Value = 1,
                    Doc = "A step up.",
                },
                {
                    Name = "High",
                    Value = 2,
                    Doc = "The default.",
                },
                {
                    Name = "Ultra",
                    Value = 3,
                    Doc = "The most.",
                },
                {
                    Name = "Cinematic",
                    Value = 4,
                    Doc = "For a recorded shot rather than a played frame. `Ultra`, until something is finer.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "GraphicsGroup",
            Doc = "The coarse groups a player's options menu shows, each set to a level at once.",
            Items = {
                {
                    Name = "ViewDistance",
                    Value = 0,
                    Doc = "How far things are drawn and how fine distant ground is.",
                },
                {
                    Name = "AntiAliasing",
                    Value = 1,
                    Doc = "How edges are smoothed.",
                },
                {
                    Name = "PostProcessing",
                    Value = 2,
                    Doc = "Bloom, depth of field, sun rays, automatic exposure, motion blur.",
                },
                {
                    Name = "Shadows",
                    Value = 3,
                    Doc = "Their resolution, cascades, distance and the contact shadows.",
                },
                {
                    Name = "GlobalIllumination",
                    Value = 4,
                    Doc = "Ambient occlusion now; indirect light when it lands.",
                },
                {
                    Name = "Reflections",
                    Value = 5,
                    Doc = "Reflection quality, when reflections land.",
                },
                {
                    Name = "Textures",
                    Value = 6,
                    Doc = "Texture size, filtering and streaming.",
                },
                {
                    Name = "Effects",
                    Value = 7,
                    Doc = "Particles.",
                },
                {
                    Name = "Foliage",
                    Value = 8,
                    Doc = "How dense grown foliage is.",
                },
                {
                    Name = "Shading",
                    Value = 9,
                    Doc = "How many lights a frame carries, and shading detail.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ShadowQuality",
            Doc = "How good the sun's shadow is: its resolution and how many cascades it is cast into.",
            Items = {
                {
                    Name = "Off",
                    Value = 0,
                    Doc = "No sun shadow.",
                },
                {
                    Name = "Low",
                    Value = 1,
                    Doc = "512 texels, two cascades.",
                },
                {
                    Name = "Medium",
                    Value = 2,
                    Doc = "1024 texels, three cascades.",
                },
                {
                    Name = "High",
                    Value = 3,
                    Doc = "2048 texels, four cascades.",
                },
                {
                    Name = "Ultra",
                    Value = 4,
                    Doc = "The same map, cast further.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "AntiAliasingMode",
            Doc = "How edges are smoothed.",
            Items = {
                {
                    Name = "Off",
                    Value = 0,
                    Doc = "Not at all.",
                },
                {
                    Name = "FXAA",
                    Value = 1,
                    Doc = "A pass over the finished picture.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "TextureQuality",
            Doc = "How much of a texture is kept in memory.",
            Items = {
                {
                    Name = "Low",
                    Value = 0,
                    Doc = "A quarter of each side.",
                },
                {
                    Name = "Medium",
                    Value = 1,
                    Doc = "Half.",
                },
                {
                    Name = "High",
                    Value = 2,
                    Doc = "All of it.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "WindowMode",
            Doc = "How a game's window sits on the display.",
            Items = {
                {
                    Name = "Windowed",
                    Value = 0,
                    Doc = "A window, with its border.",
                },
                {
                    Name = "Borderless",
                    Value = 1,
                    Doc = "The whole display, as a window with no border: other windows are a key away.",
                },
                {
                    Name = "Fullscreen",
                    Value = 2,
                    Doc = "The whole display, the game's alone.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "SettingSource",
            Doc = "Who said a graphics setting's value (`GraphicsService:GetSource`).",
            Items = {
                {
                    Name = "CommandLine",
                    Value = 0,
                    Doc = "A flag the game was started with. Nothing a script or a player says changes it.",
                },
                {
                    Name = "Player",
                    Value = 1,
                    Doc = "The player's saved choice.",
                },
                {
                    Name = "Script",
                    Value = 2,
                    Doc = "A script wrote it this session, and it has not been saved.",
                },
                {
                    Name = "Project",
                    Value = 3,
                    Doc = "The game's own `project.toml`.",
                },
                {
                    Name = "Preset",
                    Value = 4,
                    Doc = "The quality level in force.",
                },
                {
                    Name = "Engine",
                    Value = 5,
                    Doc = "Nobody: the engine's default.",
                },
            } :: { Schema.EnumItem },
        },
    },
}

return Definition
