--!strict
--- Enums (api-design.md §2.3, the Enum row).
---
--- The enums the surface actually uses, milestone by milestone: an enum nobody
--- can pass anywhere is a name squatting on a meaning we have not designed yet.
--- The input and UI work adds the four the Input Action System needs, and `KeyCode` is the largest
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
            Records = { "ADR 0053" },
            Doc = "How a `Model` is loaded and unloaded by world streaming.",
            Items = {
                {
                    Name = "Nonatomic",
                    Value = 0,
                    Doc = "Each descendant is streamed separately, by its own position. This is the default and "
                        .. "suits loose scenery.",
                },
                {
                    Name = "Atomic",
                    Value = 1,
                    Doc = "The model is streamed as one unit: it belongs to a single cell however far its parts "
                        .. "spread, and it loads and unloads whole. Use it for a building, or for a machine made "
                        .. "of moving parts.",
                },
                {
                    Name = "Persistent",
                    Value = 2,
                    Doc = "The model is never streamed. It exists before the first tick and is never unloaded. "
                        .. "Use it for a spawn point, a checkpoint or anything a script keeps a reference to.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "PartShape",
            Doc = "The built-in solid that a `Part` is drawn and collides as.",
            Items = {
                { Name = "Block", Value = 0, Doc = "A box." },
                { Name = "Ball", Value = 1, Doc = "A sphere." },
                { Name = "Cylinder", Value = 2, Doc = "A cylinder that stands along the Y axis." },
                {
                    Name = "Capsule",
                    Value = 3,
                    Doc = "A cylinder with a hemisphere at each end, along the Y axis.",
                },
                {
                    Name = "Wedge",
                    Value = 4,
                    Doc = "A ramp: a box cut diagonally, with one sloped face.",
                },
            },
        },
        {
            Name = "AlphaMode",
            Doc = "How a `Material` uses the alpha channel of its base color. The three modes are those of glTF.",
            Items = {
                {
                    Name = "Opaque",
                    Value = 0,
                    Doc = "Alpha is ignored. This is the cheapest mode and suits most surfaces.",
                },
                {
                    Name = "Mask",
                    Value = 1,
                    Doc = "Each pixel is either drawn or not, by comparing its alpha with `AlphaCutoff`. Use it "
                        .. "for leaves and fences. It needs no sorting and writes depth, like an opaque surface.",
                },
                {
                    Name = "Blend",
                    Value = 2,
                    Doc = "The surface is blended with what is behind it, by its alpha. Use it for glass and "
                        .. "smoke. Blended surfaces are sorted and cost more to draw.",
                },
            },
        },
        {
            Name = "CollisionFidelity",
            Doc = "How closely the collision shape of a `MeshPart` follows its mesh. A closer shape costs more to "
                .. "simulate.",
            Items = {
                {
                    Name = "Default",
                    Value = 0,
                    Doc = "The engine chooses the shape. It currently uses `Hull`.",
                },
                { Name = "Hull", Value = 1, Doc = "One convex hull around the whole mesh." },
                {
                    Name = "Box",
                    Value = 2,
                    Doc = "The bounding box of the mesh. This is the cheapest shape and the least accurate.",
                },
                {
                    Name = "Precise",
                    Value = 3,
                    Doc = "The triangles of the mesh. It is not implemented yet: a part set to `Precise` collides "
                        .. "as `Hull`.",
                },
            },
        },
        {
            Name = "RaycastFilterType",
            Doc = "Whether the filter list of a `RaycastParams` names what a query can hit or what it ignores.",
            Items = {
                {
                    Name = "Exclude",
                    Value = 0,
                    Doc = "The query hits everything except the listed instances and their descendants.",
                },
                {
                    Name = "Include",
                    Value = 1,
                    Doc = "The query hits only the listed instances and their descendants.",
                },
            },
        },
        {
            Name = "CharacterState",
            Doc = "What a `CharacterBody` is doing.",
            Items = {
                { Name = "Grounded", Value = 0, Doc = "Standing on ground it can walk on." },
                {
                    Name = "Airborne",
                    Value = 1,
                    Doc = "Not supported by walkable ground: jumping, falling, or on ground too steep to walk on.",
                },
                {
                    Name = "Swimming",
                    Value = 2,
                    Doc = "Its middle is below the surface of a `Water` or inside a fluid block. Gravity does not "
                        .. "affect it, and it moves on all three axes.",
                },
                { Name = "Flying", Value = 3, Doc = "`CharacterBody.Flying` is on." },
            },
        },
        {
            Name = "RotationOrder",
            Doc = "The order in which `CFrame.fromEuler` applies its three angles and `CFrame:ToEuler` returns "
                .. "them. The rotations are intrinsic: each is about the axes produced by the previous one, read "
                .. "from left to right.",
            Items = {
                { Name = "XYZ", Value = 0, Doc = "X, then Y, then Z." },
                { Name = "XZY", Value = 1, Doc = "X, then Z, then Y." },
                {
                    Name = "YXZ",
                    Value = 2,
                    Doc = "Y (yaw), then X (pitch), then Z (roll). This is the default when `order` is omitted.",
                },
                { Name = "YZX", Value = 3, Doc = "Y, then Z, then X." },
                { Name = "ZXY", Value = 4, Doc = "Z, then X, then Y." },
                { Name = "ZYX", Value = 5, Doc = "Z, then Y, then X." },
            },
        },
        {
            Name = "LogLevel",
            Doc = "The severity of a log message, in ascending order. `Value` orders the levels, so a handler can "
                .. "filter with `level.Value >= Enum.LogLevel.Warning.Value`. `DebugService.MessageOut` passes "
                .. "one with every message.",
            Items = {
                {
                    Name = "Trace",
                    Value = 0,
                    Doc = "Very detailed output, per frame or per call. It is not shown in a normal run.",
                },
                {
                    Name = "Debug",
                    Value = 1,
                    Doc = "Diagnostic detail for whoever is working on the code that logs it.",
                },
                {
                    Name = "Info",
                    Value = 2,
                    Doc = "Ordinary information. `print` logs at this level.",
                },
                {
                    Name = "Warning",
                    Value = 3,
                    Doc = "Something is wrong, and the run continues. `warn` logs at this level.",
                },
                {
                    Name = "Error",
                    Value = 4,
                    Doc = "A failure that the code could not handle, including an error raised in a signal handler.",
                },
            },
        },
        {
            Name = "Dormancy",
            Records = { "ADR 0234" },
            Doc = "Whether a part may sleep where no player is near, set by `BasePart.Dormancy`.",
            Items = {
                {
                    Name = "Auto",
                    Value = 0,
                    Doc = "It sleeps on a dedicated server when no player's focus is within the game's "
                        .. "`dormant_radius`: a `CharacterBody` does not walk, and what it was asked waits.",
                },
                {
                    Name = "Never",
                    Value = 1,
                    Doc = "It stays awake wherever it is: a train, a projectile, a boss that roams.",
                },
            },
        },
        {
            Name = "RunContext",
            Records = { "ADR 0138" },
            Doc = "Where a `Script` runs, set by `Script.RunContext`. Inside a script service, the service "
                .. "decides instead. A single-player or hosting machine is both the server and a client, and runs "
                .. "each script once whatever its context.",
            Items = {
                {
                    Name = "Client",
                    Value = 0,
                    Doc = "On every machine that has a player: a client, a single-player game and a host. Never "
                        .. "on a dedicated server, whose build does not include client scripts.",
                },
                {
                    Name = "Server",
                    Value = 1,
                    Doc = "On the authority: a dedicated server, a single-player game and a host. Never on a "
                        .. "client that joined, whose build does not include server scripts.",
                },
                {
                    Name = "Shared",
                    Value = 2,
                    Doc = "On every machine, once each: the server and every client. This is the default outside "
                        .. "the script services.",
                },
            },
        },
        {
            Name = "KeyCode",
            Doc = "Every input an `InputBinding` can use: keyboard keys, mouse buttons and movement, and the "
                .. "buttons, sticks and triggers of a standard gamepad. Keyboard keys are named by their label on "
                .. "a US layout.\n\n"
                .. "Some items are digital (a key or a button) and some are analog (an axis, a stick or a "
                .. "trigger). A `Bool` action bound to an analog input counts as pressed beyond half of its range.",
            Items = {
                {
                    Name = "Unknown",
                    Value = 0,
                    Doc = "No input. It is the value of an `InputBinding` that has not been assigned, and of a "
                        .. "binding whose key does not exist on the current platform.",
                },
                { Name = "Escape", Value = 1, Doc = "The Escape key." },
                { Name = "F1", Value = 2, Doc = "The F1 key." },
                { Name = "F2", Value = 3, Doc = "The F2 key." },
                { Name = "F3", Value = 4, Doc = "The F3 key." },
                { Name = "F4", Value = 5, Doc = "The F4 key." },
                { Name = "F5", Value = 6, Doc = "The F5 key." },
                { Name = "F6", Value = 7, Doc = "The F6 key." },
                { Name = "F7", Value = 8, Doc = "The F7 key." },
                { Name = "F8", Value = 9, Doc = "The F8 key." },
                { Name = "F9", Value = 10, Doc = "The F9 key." },
                { Name = "F10", Value = 11, Doc = "The F10 key." },
                { Name = "F11", Value = 12, Doc = "The F11 key." },
                { Name = "F12", Value = 13, Doc = "The F12 key." },
                { Name = "A", Value = 14, Doc = "The A key." },
                { Name = "B", Value = 15, Doc = "The B key." },
                { Name = "C", Value = 16, Doc = "The C key." },
                { Name = "D", Value = 17, Doc = "The D key." },
                { Name = "E", Value = 18, Doc = "The E key." },
                { Name = "F", Value = 19, Doc = "The F key." },
                { Name = "G", Value = 20, Doc = "The G key." },
                { Name = "H", Value = 21, Doc = "The H key." },
                { Name = "I", Value = 22, Doc = "The I key." },
                { Name = "J", Value = 23, Doc = "The J key." },
                { Name = "K", Value = 24, Doc = "The K key." },
                { Name = "L", Value = 25, Doc = "The L key." },
                { Name = "M", Value = 26, Doc = "The M key." },
                { Name = "N", Value = 27, Doc = "The N key." },
                { Name = "O", Value = 28, Doc = "The O key." },
                { Name = "P", Value = 29, Doc = "The P key." },
                { Name = "Q", Value = 30, Doc = "The Q key." },
                { Name = "R", Value = 31, Doc = "The R key." },
                { Name = "S", Value = 32, Doc = "The S key." },
                { Name = "T", Value = 33, Doc = "The T key." },
                { Name = "U", Value = 34, Doc = "The U key." },
                { Name = "V", Value = 35, Doc = "The V key." },
                { Name = "W", Value = 36, Doc = "The W key." },
                { Name = "X", Value = 37, Doc = "The X key." },
                { Name = "Y", Value = 38, Doc = "The Y key." },
                { Name = "Z", Value = 39, Doc = "The Z key." },
                { Name = "Digit0", Value = 40, Doc = "The 0 key in the number row." },
                { Name = "Digit1", Value = 41, Doc = "The 1 key in the number row." },
                { Name = "Digit2", Value = 42, Doc = "The 2 key in the number row." },
                { Name = "Digit3", Value = 43, Doc = "The 3 key in the number row." },
                { Name = "Digit4", Value = 44, Doc = "The 4 key in the number row." },
                { Name = "Digit5", Value = 45, Doc = "The 5 key in the number row." },
                { Name = "Digit6", Value = 46, Doc = "The 6 key in the number row." },
                { Name = "Digit7", Value = 47, Doc = "The 7 key in the number row." },
                { Name = "Digit8", Value = 48, Doc = "The 8 key in the number row." },
                { Name = "Digit9", Value = 49, Doc = "The 9 key in the number row." },
                { Name = "Space", Value = 50, Doc = "The space bar." },
                { Name = "Return", Value = 51, Doc = "The Return or Enter key of the main keyboard." },
                { Name = "Tab", Value = 52, Doc = "The Tab key." },
                { Name = "Backspace", Value = 53, Doc = "The Backspace key." },
                { Name = "LeftShift", Value = 54, Doc = "The left Shift key." },
                { Name = "RightShift", Value = 55, Doc = "The right Shift key." },
                { Name = "LeftControl", Value = 56, Doc = "The left Ctrl key." },
                { Name = "RightControl", Value = 57, Doc = "The right Ctrl key." },
                { Name = "LeftAlt", Value = 58, Doc = "The left Alt key." },
                { Name = "RightAlt", Value = 59, Doc = "The right Alt key." },
                { Name = "Left", Value = 60, Doc = "The left arrow key." },
                { Name = "Right", Value = 61, Doc = "The right arrow key." },
                { Name = "Up", Value = 62, Doc = "The up arrow key." },
                { Name = "Down", Value = 63, Doc = "The down arrow key." },
                {
                    Name = "Home",
                    Value = 64,
                    Doc = "The Home key.",
                },
                { Name = "End", Value = 65, Doc = "The End key." },
                { Name = "Delete", Value = 66, Doc = "The Delete key." },
                { Name = "MouseLeft", Value = 67, Doc = "The left mouse button." },
                { Name = "MouseMiddle", Value = 68, Doc = "The middle mouse button, usually the wheel pressed in." },
                { Name = "MouseRight", Value = 69, Doc = "The right mouse button." },
                { Name = "MouseX1", Value = 70, Doc = "The first extra mouse button, usually Back." },
                { Name = "MouseX2", Value = 71, Doc = "The second extra mouse button, usually Forward." },
                {
                    Name = "MouseMovement",
                    Value = 72,
                    Doc = "The movement of the pointer in the current tick, as a `Vector2` in pixels. It is "
                        .. "analog: bind it to a `Direction2D` action, for example to look around.",
                },
                {
                    Name = "MouseWheel",
                    Value = 73,
                    Doc = "The movement of the mouse wheel in the current tick, in notches; positive is away from "
                        .. "the user. It is analog: bind it to a `Direction1D` action.",
                },
                {
                    Name = "ButtonSouth",
                    Value = 74,
                    Doc = "The bottom face button of a gamepad: A on an Xbox controller, B on a Nintendo "
                        .. "controller, Cross on a PlayStation controller. Face buttons are named by position, "
                        .. "because their labels differ between controllers.",
                },
                {
                    Name = "ButtonEast",
                    Value = 75,
                    Doc = "The right face button of a gamepad.",
                },
                {
                    Name = "ButtonWest",
                    Value = 76,
                    Doc = "The left face button of a gamepad.",
                },
                {
                    Name = "ButtonNorth",
                    Value = 77,
                    Doc = "The top face button of a gamepad.",
                },
                {
                    Name = "ButtonBack",
                    Value = 78,
                    Doc = "The Back, View or Select button of a gamepad, left of center.",
                },
                { Name = "ButtonGuide", Value = 79, Doc = "The Guide or Home button of a gamepad, at the center." },
                {
                    Name = "ButtonStart",
                    Value = 80,
                    Doc = "The Start, Menu or Options button of a gamepad, right of center.",
                },
                {
                    Name = "ButtonLeftStick",
                    Value = 81,
                    Doc = "The left stick pressed in, as a button. For the direction of the stick, use "
                        .. "`LeftThumbstick`.",
                },
                {
                    Name = "ButtonRightStick",
                    Value = 82,
                    Doc = "The right stick pressed in, as a button. For the direction of the stick, use "
                        .. "`RightThumbstick`.",
                },
                {
                    Name = "ButtonLeftShoulder",
                    Value = 83,
                    Doc = "The left shoulder button of a gamepad, above the trigger.",
                },
                {
                    Name = "ButtonRightShoulder",
                    Value = 84,
                    Doc = "The right shoulder button of a gamepad, above the trigger.",
                },
                { Name = "DpadUp", Value = 85, Doc = "Up on the D-pad of a gamepad." },
                { Name = "DpadDown", Value = 86, Doc = "Down on the D-pad of a gamepad." },
                { Name = "DpadLeft", Value = 87, Doc = "Left on the D-pad of a gamepad." },
                { Name = "DpadRight", Value = 88, Doc = "Right on the D-pad of a gamepad." },
                {
                    Name = "LeftStickX",
                    Value = 89,
                    Doc = "The horizontal axis of the left stick, from -1 (left) to 1 (right).",
                },
                { Name = "LeftStickY", Value = 90, Doc = "The vertical axis of the left stick, from -1 to 1." },
                {
                    Name = "RightStickX",
                    Value = 91,
                    Doc = "The horizontal axis of the right stick, from -1 (left) to 1 (right).",
                },
                { Name = "RightStickY", Value = 92, Doc = "The vertical axis of the right stick, from -1 to 1." },
                {
                    Name = "LeftTrigger",
                    Value = 93,
                    Doc = "The left trigger of a gamepad, from 0 (released) to 1 (fully pulled).",
                },
                {
                    Name = "RightTrigger",
                    Value = 94,
                    Doc = "The right trigger of a gamepad, from 0 (released) to 1 (fully pulled).",
                },
                {
                    Name = "LeftThumbstick",
                    Value = 95,
                    Doc = "The left stick as a `Vector2`, with both axes together. Bind it to a `Direction2D` "
                        .. "action for movement. The separate axes are `LeftStickX` and `LeftStickY`.",
                },
                {
                    Name = "RightThumbstick",
                    Value = 96,
                    Doc = "The right stick as a `Vector2`, with both axes together. The separate axes are "
                        .. "`RightStickX` and `RightStickY`.",
                },
                {
                    Name = "Virtual1",
                    Value = 97,
                    Doc = "A virtual input, set by `InputService:SetVirtualState` instead of by hardware. It is "
                        .. "bound, prioritized and recorded in a replay like any other key code. Use virtual "
                        .. "inputs for on-screen buttons and thumbsticks.\n\n"
                        .. "A virtual input holds a number, not a press. A button sets 1 and 0, and a slider sets "
                        .. "any value in between. A `Bool` action bound to it counts as pressed beyond 0.5.\n\n"
                        .. "There are sixteen, `Virtual1` to `Virtual16`. The first four are also the axes of the "
                        .. "two virtual sticks, `VirtualStick1` and `VirtualStick2`.",
                },
                {
                    Name = "Virtual2",
                    Value = 98,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. It is also the Y axis of `VirtualStick1`. See `Virtual1`.",
                },
                {
                    Name = "Virtual3",
                    Value = 99,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. It is also the X axis of `VirtualStick2`. See `Virtual1`.",
                },
                {
                    Name = "Virtual4",
                    Value = 100,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. It is also the Y axis of `VirtualStick2`. See `Virtual1`.",
                },
                {
                    Name = "VirtualStick1",
                    Value = 101,
                    Doc = "`Virtual1` and `Virtual2` together as a `Vector2`. Bind it to a `Direction2D` action "
                        .. "for an on-screen thumbstick.",
                },
                {
                    Name = "VirtualStick2",
                    Value = 102,
                    Doc = "`Virtual3` and `Virtual4` together as a `Vector2`. Bind it to a `Direction2D` action "
                        .. "for a second on-screen thumbstick.",
                },
                {
                    Name = "Minus",
                    Value = 103,
                    Doc = "The minus key (`-`) of the main keyboard.",
                },
                { Name = "Equals", Value = 104, Doc = "The equals key (`=`) of the main keyboard." },
                { Name = "LeftBracket", Value = 105, Doc = "The left bracket key (`[`)." },
                { Name = "RightBracket", Value = 106, Doc = "The right bracket key (`]`)." },
                { Name = "Backslash", Value = 107, Doc = "The backslash key." },
                { Name = "Semicolon", Value = 108, Doc = "The semicolon key (`;`)." },
                { Name = "Quote", Value = 109, Doc = "The quote key (`'`)." },
                { Name = "Backquote", Value = 110, Doc = "The backquote key, left of 1 on a US layout." },
                { Name = "Comma", Value = 111, Doc = "The comma key (`,`)." },
                { Name = "Period", Value = 112, Doc = "The period key (`.`)." },
                { Name = "Slash", Value = 113, Doc = "The slash key (`/`)." },
                { Name = "CapsLock", Value = 114, Doc = "The Caps Lock key." },
                { Name = "Insert", Value = 115, Doc = "The Insert key." },
                { Name = "PageUp", Value = 116, Doc = "The Page Up key." },
                { Name = "PageDown", Value = 117, Doc = "The Page Down key." },
                { Name = "NumLock", Value = 118, Doc = "The Num Lock key." },
                { Name = "Keypad0", Value = 119, Doc = "The 0 key of the numeric keypad." },
                { Name = "Keypad1", Value = 120, Doc = "The 1 key of the numeric keypad." },
                { Name = "Keypad2", Value = 121, Doc = "The 2 key of the numeric keypad." },
                { Name = "Keypad3", Value = 122, Doc = "The 3 key of the numeric keypad." },
                { Name = "Keypad4", Value = 123, Doc = "The 4 key of the numeric keypad." },
                { Name = "Keypad5", Value = 124, Doc = "The 5 key of the numeric keypad." },
                { Name = "Keypad6", Value = 125, Doc = "The 6 key of the numeric keypad." },
                { Name = "Keypad7", Value = 126, Doc = "The 7 key of the numeric keypad." },
                { Name = "Keypad8", Value = 127, Doc = "The 8 key of the numeric keypad." },
                { Name = "Keypad9", Value = 128, Doc = "The 9 key of the numeric keypad." },
                { Name = "KeypadPeriod", Value = 129, Doc = "The period key of the numeric keypad." },
                { Name = "KeypadDivide", Value = 130, Doc = "The divide key (`/`) of the numeric keypad." },
                { Name = "KeypadMultiply", Value = 131, Doc = "The multiply key (`*`) of the numeric keypad." },
                { Name = "KeypadMinus", Value = 132, Doc = "The minus key of the numeric keypad." },
                { Name = "KeypadPlus", Value = 133, Doc = "The plus key of the numeric keypad." },
                { Name = "KeypadEnter", Value = 134, Doc = "The Enter key of the numeric keypad." },
                {
                    Name = "KeypadEquals",
                    Value = 135,
                    Doc = "The equals key of the numeric keypad, on keyboards that have one.",
                },
                {
                    Name = "Virtual5",
                    Value = 136,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. See `Virtual1`.",
                },
                {
                    Name = "Virtual6",
                    Value = 137,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. See `Virtual1`.",
                },
                {
                    Name = "Virtual7",
                    Value = 138,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. See `Virtual1`.",
                },
                {
                    Name = "Virtual8",
                    Value = 139,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. See `Virtual1`.",
                },
                {
                    Name = "Virtual9",
                    Value = 140,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. See `Virtual1`.",
                },
                {
                    Name = "Virtual10",
                    Value = 141,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. See `Virtual1`.",
                },
                {
                    Name = "Virtual11",
                    Value = 142,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. See `Virtual1`.",
                },
                {
                    Name = "Virtual12",
                    Value = 143,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. See `Virtual1`.",
                },
                {
                    Name = "Virtual13",
                    Value = 144,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. See `Virtual1`.",
                },
                {
                    Name = "Virtual14",
                    Value = 145,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. See `Virtual1`.",
                },
                {
                    Name = "Virtual15",
                    Value = 146,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. See `Virtual1`.",
                },
                {
                    Name = "Virtual16",
                    Value = 147,
                    Doc = "A virtual input, set by `InputService:SetVirtualState`. See `Virtual1`.",
                },
                {
                    Name = "SwipeUp",
                    Value = 148,
                    Doc = "A swipe up, as a key. It is pressed for one tick when a finger, or the mouse with its "
                        .. "left button held, has traveled `InputService.SwipeThreshold` up the screen. Bind it "
                        .. "to an `InputAction` like any key.\n\n"
                        .. "The swipe is detected while the finger is still down. One long drag in one direction "
                        .. "is one swipe, and a change of direction starts the next. A press that the interface "
                        .. "consumed starts no swipe.",
                },
                { Name = "SwipeDown", Value = 149, Doc = "A swipe down, as a key. See `SwipeUp`." },
                { Name = "SwipeLeft", Value = 150, Doc = "A swipe to the left, as a key. See `SwipeUp`." },
                { Name = "SwipeRight", Value = 151, Doc = "A swipe to the right, as a key. See `SwipeUp`." },
                { Name = "XRButtonA", Value = 152, Doc = "The right controller's A button." },
                { Name = "XRButtonB", Value = 153, Doc = "The right controller's B button." },
                { Name = "XRButtonX", Value = 154, Doc = "The left controller's X button." },
                { Name = "XRButtonY", Value = 155, Doc = "The left controller's Y button." },
                { Name = "XRMenu", Value = 156, Doc = "The menu button, on the left controller." },
                { Name = "XRTriggerLeft", Value = 157, Doc = "The left controller's trigger, from 0 to 1." },
                { Name = "XRTriggerRight", Value = 158, Doc = "The right controller's trigger, from 0 to 1." },
                { Name = "XRGripLeft", Value = 159, Doc = "The left controller's grip, from 0 to 1." },
                { Name = "XRGripRight", Value = 160, Doc = "The right controller's grip, from 0 to 1." },
                {
                    Name = "XRThumbstickLeftX",
                    Value = 161,
                    Doc = "The left controller's thumbstick, across, from -1 (left) to 1.",
                },
                {
                    Name = "XRThumbstickLeftY",
                    Value = 162,
                    Doc = "The left controller's thumbstick, up and down, from -1 (down) to 1.",
                },
                {
                    Name = "XRThumbstickRightX",
                    Value = 163,
                    Doc = "The right controller's thumbstick, across, from -1 (left) to 1.",
                },
                {
                    Name = "XRThumbstickRightY",
                    Value = 164,
                    Doc = "The right controller's thumbstick, up and down, from -1 (down) to 1.",
                },
                {
                    Name = "XRThumbstickLeft",
                    Value = 165,
                    Doc = "The left controller's thumbstick, both axes, for a `Vector2` action.",
                },
                {
                    Name = "XRThumbstickRight",
                    Value = 166,
                    Doc = "The right controller's thumbstick, both axes, for a `Vector2` action.",
                },
                { Name = "XRThumbstickLeftClick", Value = 167, Doc = "The left controller's thumbstick, pressed in." },
                {
                    Name = "XRThumbstickRightClick",
                    Value = 168,
                    Doc = "The right controller's thumbstick, pressed in.",
                },
                {
                    Name = "XRPinchLeft",
                    Value = 169,
                    Doc = "The left hand's thumb and index finger pinched, with hand tracking and no controller.",
                },
                {
                    Name = "XRPinchRight",
                    Value = 170,
                    Doc = "The right hand's thumb and index finger pinched, with hand tracking and no controller.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "InputActionType",
            Records = { "ADR 0029" },
            Doc = "The kind of value an `InputAction` produces, which is what `GetState` returns. Every binding "
                .. "of an action produces this type, whatever device it reads.",
            Items = {
                {
                    Name = "Bool",
                    Value = 0,
                    Doc = "Pressed or not. `GetState` returns a boolean. Only this type fires `Pressed` and "
                        .. "`Released`.",
                },
                {
                    Name = "Direction1D",
                    Value = 1,
                    Doc = "One signed axis. `GetState` returns a number: -1 to 1 from a pair of keys, or the "
                        .. "analog value of a trigger or a wheel.",
                },
                {
                    Name = "Direction2D",
                    Value = 2,
                    Doc = "Two signed axes. `GetState` returns a `Vector2`, from a stick, from pointer movement "
                        .. "or from the four direction keys of a binding.",
                },
                {
                    Name = "Direction3D",
                    Value = 3,
                    Doc = "Three signed axes. `GetState` returns a `vector`. No binding can drive three axes yet, "
                        .. "so the value is always the zero vector.",
                },
                {
                    Name = "ViewportPosition",
                    Value = 4,
                    Doc = "The position of the pointer, not its movement. `GetState` returns a `Vector2` in "
                        .. "window pixels, measured from the top left.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "GamepadType",
            Doc = "The family of a physical gamepad. It is independent of the operating system and of the button "
                .. "layout.",
            Items = {
                {
                    Name = "Unknown",
                    Value = 0,
                    Doc = "No gamepad is active, or the family of the device cannot be identified.",
                },
                { Name = "Xbox", Value = 1, Doc = "An Xbox controller." },
                { Name = "PlayStation", Value = 2, Doc = "A PlayStation controller." },
                { Name = "Nintendo", Value = 3, Doc = "A Nintendo controller." },
                { Name = "Generic", Value = 4, Doc = "A standard gamepad that is not from one of the named families." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "InputDeviceType",
            Doc = "A kind of input device. `InputService.LastInputDeviceType` reports the kind the player used "
                .. "most recently, so that a HUD can switch its prompts. `InputBinding.DeviceType` reports the "
                .. "kind a binding belongs to, which `GetPreferredBinding` uses.",
            Items = {
                { Name = "KeyboardMouse", Value = 0, Doc = "A keyboard or a mouse." },
                { Name = "Gamepad", Value = 1, Doc = "A gamepad." },
                {
                    Name = "Touch",
                    Value = 2,
                    Doc = "A touchscreen. A finger on the screen makes this the last device used, so a game can "
                        .. "show on-screen controls only when they are needed. A binding to a virtual key code, "
                        .. "such as `Enum.KeyCode.Virtual1`, also reports this type.",
                },
                {
                    Name = "XR",
                    Value = 3,
                    Doc = "A headset's controllers, or the hands it tracks. Its key codes are the ones whose "
                        .. "names begin with `XR`.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "UserInputType",
            Records = { "ADR 0041" },
            Doc = "The kind of input an `InputObject` comes from, in the input events of `InputService`. It is "
                .. "coarser than `KeyCode`: read it when the handler only needs the kind of input, and read "
                .. "`KeyCode` for the specific key.",
            Items = {
                {
                    Name = "None",
                    Value = 0,
                    Doc = "No input. It is the value of an `InputObject` that no event produced.",
                },
                { Name = "Keyboard", Value = 1, Doc = "A keyboard key. `KeyCode` gives the key." },
                { Name = "MouseButton1", Value = 2, Doc = "The primary mouse button, normally the left one." },
                { Name = "MouseButton2", Value = 3, Doc = "The secondary mouse button, normally the right one." },
                { Name = "MouseButton3", Value = 4, Doc = "The middle mouse button, usually the wheel pressed in." },
                {
                    Name = "MouseMovement",
                    Value = 5,
                    Doc = "The pointer moved. It fires `InputChanged` only, never `InputBegan` or `InputEnded`.",
                },
                {
                    Name = "MouseWheel",
                    Value = 6,
                    Doc = "The mouse wheel turned. It fires `InputChanged`, with the amount in `Delta.z`; one "
                        .. "notch is 1.",
                },
                {
                    Name = "Gamepad",
                    Value = 7,
                    Doc = "A gamepad button or stick. `KeyCode` gives which. A stick's movement fires "
                        .. "`InputChanged`, with the axis value in `Position`.",
                },
                {
                    Name = "Touch",
                    Value = 8,
                    Doc = "A finger on a touchscreen. It fires `InputBegan` when the finger touches, "
                        .. "`InputChanged` as it moves and `InputEnded` when it lifts, with `Position` in window "
                        .. "pixels and `TouchId` identifying the finger. Even a very short tap begins and ends.\n\n"
                        .. "These events fire whether or not the interface used the touch. Use them for a game's "
                        .. "own on-screen controls, which drive actions through `InputService:SetVirtualState`.",
                },
                {
                    Name = "XRLeft",
                    Value = 9,
                    Doc = "The left controller of a headset, or the left hand it tracks. `KeyCode` gives which "
                        .. "button, trigger or stick. `HapticService` vibrates this controller under this type.",
                },
                {
                    Name = "XRRight",
                    Value = 10,
                    Doc = "The right controller of a headset, or the right hand it tracks. See `XRLeft`.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "InputRate",
            Records = { "ADR 0039" },
            Doc = "Whether the actions of an `InputContext` are dispatched on simulation ticks or on rendered "
                .. "frames. It is set per context and is independent of `Priority`.",
            Items = {
                {
                    Name = "Simulation",
                    Value = 0,
                    Doc = "On simulation ticks, at the fixed tick rate. This is the default. It is deterministic "
                        .. "and recorded in a replay; use it for everything that affects gameplay.",
                },
                {
                    Name = "Render",
                    Value = 1,
                    Doc = "On rendered frames, with a variable time step. Use it for camera look and UI, where a "
                        .. "tick of delay is visible. Input read here depends on the frame rate and is not "
                        .. "recorded in a replay, so do not base gameplay decisions on it.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "EasingStyle",
            Doc = "The shape of the curve a tween follows. The styles are the standard easing functions used by "
                .. "animation tools.",
            Items = {
                {
                    Name = "Linear",
                    Value = 0,
                    Doc = "No easing: constant speed. The easing direction has no effect.",
                },
                { Name = "Sine", Value = 1, Doc = "A gentle curve based on a sine wave." },
                { Name = "Quad", Value = 2, Doc = "A curve based on the square of the progress. A mild acceleration." },
                { Name = "Cubic", Value = 3, Doc = "A curve based on the cube of the progress. Stronger than `Quad`." },
                {
                    Name = "Quart",
                    Value = 4,
                    Doc = "A curve based on the fourth power of the progress. Stronger than `Cubic`.",
                },
                {
                    Name = "Quint",
                    Value = 5,
                    Doc = "A curve based on the fifth power of the progress. Stronger than `Quart`.",
                },
                {
                    Name = "Exponential",
                    Value = 6,
                    Doc = "An exponential curve: very slow at one end and very fast at the other.",
                },
                { Name = "Circular", Value = 7, Doc = "A curve shaped like a quarter of a circle." },
                {
                    Name = "Back",
                    Value = 8,
                    Doc = "Overshoots the end and comes back. The value goes outside the range between start and "
                        .. "goal, so the tweened property must accept that.",
                },
                { Name = "Bounce", Value = 9, Doc = "Settles by bouncing, like a dropped object." },
                {
                    Name = "Elastic",
                    Value = 10,
                    Doc = "Oscillates past the end before settling, like a spring. Like `Back`, the value goes "
                        .. "outside the range between start and goal.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "EasingDirection",
            Doc = "Which end of a tween the easing applies to.",
            Items = {
                { Name = "In", Value = 0, Doc = "Eased at the start and abrupt at the end." },
                {
                    Name = "Out",
                    Value = 1,
                    Doc = "Abrupt at the start and eased at the end. This is the usual choice.",
                },
                {
                    Name = "InOut",
                    Value = 2,
                    Doc = "Eased at both ends: the `In` curve over the first half, mirrored over the second.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "PlaybackState",
            Doc = "The state of a `Tween`. It distinguishes a tween that finished from one that was stopped early.",
            Items = {
                { Name = "Begin", Value = 0, Doc = "Created and not yet played." },
                {
                    Name = "Delayed",
                    Value = 1,
                    Doc = "Playing, but still within its `TweenInfo.DelayTime`, so it is not changing the "
                        .. "property yet.",
                },
                { Name = "Playing", Value = 2, Doc = "Playing and changing the property." },
                {
                    Name = "Paused",
                    Value = 3,
                    Doc = "Paused at its current position. `Play` resumes from there.",
                },
                {
                    Name = "Completed",
                    Value = 4,
                    Doc = "Reached its end. The property has exactly its goal value.",
                },
                {
                    Name = "Canceled",
                    Aliases = { "Cancelled" },
                    Value = 5,
                    Doc = "Stopped before its end by `Cancel`. The property keeps the value it had reached.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "PromiseState",
            Records = { "ADR 0094" },
            Doc = "The state of a `Promise`. A promise settles once and does not change afterward.",
            Items = {
                { Name = "Started", Value = 0, Doc = "Not settled yet: neither resolved nor rejected." },
                { Name = "Resolved", Value = 1, Doc = "Resolved, with the values that `Await` returns." },
                {
                    Name = "Rejected",
                    Value = 2,
                    Doc = "Rejected, with the values that its rejection handlers receive.",
                },
                {
                    Name = "Canceled",
                    Aliases = { "Cancelled" },
                    Value = 3,
                    Doc = "Canceled before it settled. Its executor was stopped and its handlers do not run.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "FillDirection",
            Doc = "The direction in which a layout arranges its items.",
            Items = {
                { Name = "Horizontal", Value = 0, Doc = "Side by side, in a row." },
                { Name = "Vertical", Value = 1, Doc = "One below another, in a column." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "HorizontalAlignment",
            Doc = "An alignment along the X axis. `TextLabel.TextXAlignment` aligns text with it, and a layout's "
                .. "`HorizontalAlignment` aligns children with it.",
            Items = {
                { Name = "Left", Value = 0, Doc = "Aligned to the left edge." },
                { Name = "Center", Value = 1, Doc = "Centered horizontally." },
                { Name = "Right", Value = 2, Doc = "Aligned to the right edge." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "TextOverflow",
            Records = { "ADR 0168" },
            Doc = "What a `TextLabel` does with text that does not fit in it.",
            Items = {
                {
                    Name = "Overflow",
                    Value = 0,
                    Doc = "The text is drawn beyond the edges of the label.",
                },
                {
                    Name = "Clip",
                    Value = 1,
                    Doc = "The text is cut at the edges of the label, even through the middle of a letter.",
                },
                {
                    Name = "Ellipsis",
                    Value = 2,
                    Doc = "The text is shortened at a whole character and ended with an ellipsis, so that it fits.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "VerticalAlignment",
            Doc = "An alignment along the Y axis. `TextLabel.TextYAlignment` aligns text with it, and a layout's "
                .. "`VerticalAlignment` aligns children with it.",
            Items = {
                { Name = "Top", Value = 0, Doc = "Aligned to the top edge." },
                { Name = "Center", Value = 1, Doc = "Centered vertically." },
                { Name = "Bottom", Value = 2, Doc = "Aligned to the bottom edge." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "SortOrder",
            Doc = "The order in which a layout arranges its children.",
            Items = {
                {
                    Name = "Name",
                    Value = 0,
                    Doc = "By `Name`, compared character by character. The order does not depend on the order the "
                        .. "children were created in.",
                },
                {
                    Name = "LayoutOrder",
                    Value = 1,
                    Doc = "By each child's `LayoutOrder`, and by tree order for equal values. This is the usual "
                        .. "choice, because a list can be reordered without renaming anything.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "AutomaticSize",
            Doc = "The axes on which a `UIObject` sizes itself to fit its contents, ignoring `Size` on those "
                .. "axes. Use it for text and lists, so that a label is never smaller than its text.",
            Items = {
                { Name = "None", Value = 0, Doc = "Neither axis: `Size` is used as set." },
                { Name = "X", Value = 1, Doc = "The width fits the contents." },
                { Name = "Y", Value = 2, Doc = "The height fits the contents." },
                { Name = "XY", Value = 3, Doc = "Both the width and the height fit the contents." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ScaleType",
            Doc = "How an `ImageLabel` fits its image into its rectangle.",
            Items = {
                {
                    Name = "Stretch",
                    Value = 0,
                    Doc = "The whole image fills the whole rectangle. Its aspect ratio is not kept.",
                },
                {
                    Name = "Slice",
                    Value = 1,
                    Doc = "Nine-slice scaling. The four corners are drawn at their own size, the four edges "
                        .. "stretch along one axis and the center stretches along both. `SliceCenter` sets where "
                        .. "the image is divided. Use it so a panel or a button keeps its rounded corners at any "
                        .. "size.",
                },
                {
                    Name = "Tile",
                    Value = 2,
                    Doc = "The image is repeated at its own size until the rectangle is full.",
                },
                {
                    Name = "Fit",
                    Value = 3,
                    Doc = "The whole image is shown as large as fits in the rectangle with its aspect ratio kept, "
                        .. "centered. The background shows where the image does not reach. Use it for an icon.",
                },
                {
                    Name = "Crop",
                    Value = 4,
                    Doc = "The image covers the whole rectangle with its aspect ratio kept, and what extends "
                        .. "beyond is cut off equally on both sides. Use it for a picture that fills a card.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "NetworkTopology",
            Records = { "ADR 0070" },
            Doc = "The network role of this process, reported by `NetworkService.Topology`.",
            Items = {
                {
                    Name = "Solo",
                    Value = 0,
                    Doc = "Single-player: nothing is networked, and this process is the authority.",
                },
                {
                    Name = "Host",
                    Value = 1,
                    Doc = "The authority of a match, with a player of its own in the same process.",
                },
                {
                    Name = "Dedicated",
                    Value = 2,
                    Doc = "The authority of a match with no player of its own: a server with no window.",
                },
                {
                    Name = "Replica",
                    Value = 3,
                    Doc = "A client connected to another machine's authority. It shows the world it is sent.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ParticleShape",
            Doc = "The shape a particle is drawn as when its emitter has no texture.",
            Items = {
                {
                    Name = "Soft",
                    Value = 0,
                    Doc = "A round shape that fades out toward its edge. Use it for smoke, dust, steam or a glow.",
                },
                {
                    Name = "Disc",
                    Value = 1,
                    Doc = "A round dot with a sharp edge. Use it for sparks, bubbles or confetti.",
                },
                {
                    Name = "Square",
                    Value = 2,
                    Doc = "A square that faces the camera. Use it for pixels, debris or square confetti.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "Face",
            Doc = "One face of a part, named relative to the part's own axes.",
            Items = {
                { Name = "Front", Value = 0, Doc = "The face the part's `LookVector` points out of." },
                { Name = "Back", Value = 1, Doc = "The face opposite the front." },
                { Name = "Top", Value = 2, Doc = "The face the part's `UpVector` points out of." },
                { Name = "Bottom", Value = 3, Doc = "The face opposite the top." },
                { Name = "Right", Value = 4, Doc = "The face the part's `RightVector` points out of." },
                { Name = "Left", Value = 5, Doc = "The face opposite the right." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "BlockOpacity",
            Doc = "How much of what is behind a block of `VoxelService` shows through it.",
            Items = {
                {
                    Name = "Opaque",
                    Value = 0,
                    Doc = "Nothing shows through. A face against the block is hidden, and the block darkens the "
                        .. "corners it touches.",
                },
                {
                    Name = "Cutout",
                    Value = 1,
                    Doc = "The block has holes where its image is transparent, as for leaves, a fence or a grate. "
                        .. "Each pixel is either drawn or not. The faces between two cutout blocks are drawn, "
                        .. "which makes foliage look full.",
                },
                {
                    Name = "Translucent",
                    Value = 2,
                    Doc = "The block is blended with what is behind it, as for glass, water or ice. No face is "
                        .. "drawn between two blocks of the same type, so a lake has a single surface.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "Shape2D",
            Doc = "The outline a `Part2D` is drawn with and collides as.",
            Items = {
                { Name = "Box", Value = 0, Doc = "A rectangle of the part's `Size`." },
                {
                    Name = "Circle",
                    Value = 1,
                    Doc = "A circle whose diameter is the smaller of the part's two sizes.",
                },
                {
                    Name = "Capsule",
                    Value = 2,
                    Doc = "A rectangle with rounded ends at the top and bottom. Use it for a character: its "
                        .. "rounded base slides over the seams between tiles instead of catching on them.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "TextureFilter",
            Doc = "How an image is sampled when it is drawn larger or smaller than its own size.",
            Items = {
                {
                    Name = "Linear",
                    Value = 0,
                    Doc = "Blended between neighboring pixels of the image. The result is smooth; use it for painted art.",
                },
                {
                    Name = "Nearest",
                    Value = 1,
                    Doc = "The nearest pixel of the image, with no blending. Pixels stay sharp at any zoom; use "
                        .. "it for pixel art.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "CameraProjection",
            Doc = "How a `Camera` projects the world onto the screen.",
            Items = {
                {
                    Name = "Perspective",
                    Value = 0,
                    Doc = "Distant objects look smaller, according to `FieldOfView`. Use it for 3D.",
                },
                {
                    Name = "Orthographic",
                    Value = 1,
                    Doc = "Objects keep the same size at any distance. The view extends `OrthographicSize` meters "
                        .. "from its center to its top edge. Use it for 2D: the camera looks along its -Z axis, "
                        .. "so a 2D world on the XY plane faces it.",
                },
            } :: { Schema.EnumItem },
        },
        -- Appended, never inserted: an enum's id is its place in this list, and ids
        -- travel on the wire and in snapshots.
        {
            Name = "ScreenOrientation",
            Doc = "The orientations in which a game can be held on a device that rotates, set by "
                .. "`UIService.ScreenOrientation`. The two landscape orientations are named for the side the top "
                .. "of the device is on.",
            Items = {
                {
                    Name = "LandscapeLeft",
                    Value = 0,
                    Doc = "Landscape, with the top of the device on the left. The screen does not rotate when the "
                        .. "device is turned over.",
                },
                {
                    Name = "LandscapeRight",
                    Value = 1,
                    Doc = "Landscape, with the top of the device on the right. The screen does not rotate when "
                        .. "the device is turned over.",
                },
                {
                    Name = "LandscapeSensor",
                    Value = 2,
                    Doc = "Either landscape orientation, following the device as it is turned over. This is the "
                        .. "default.",
                },
                {
                    Name = "Portrait",
                    Value = 3,
                    Doc = "Portrait, with the device upright.",
                },
                {
                    Name = "Sensor",
                    Value = 4,
                    Doc = "Any orientation, following the device and the player's rotation lock.",
                },
            } :: { Schema.EnumItem },
        },
        -- ADR 0106. At the END: an enum's id is its position, and it travels.
        {
            Name = "NetworkState",
            Records = { "ADR 0106" },
            Doc = "The connection state of this machine, reported by `NetworkService.State`. `Join`, `Host` and "
                .. "`Disconnect` change it.",
            Items = {
                {
                    Name = "Offline",
                    Value = 0,
                    Doc = "Single-player: no other machine is connected, and this machine is the authority.",
                },
                {
                    Name = "Connecting",
                    Value = 1,
                    Doc = "`Join` was called, and the server has not answered yet.",
                },
                {
                    Name = "Connected",
                    Value = 2,
                    Doc = "In a match on another machine's authority: this machine is a client.",
                },
                {
                    Name = "Hosting",
                    Value = 3,
                    Doc = "This machine is the authority, has a player of its own, and others can join.",
                },
                {
                    Name = "Serving",
                    Value = 4,
                    Doc = "A dedicated server, started with `--serve`: it is the authority and has no player.",
                },
                {
                    Name = "Reconnecting",
                    Value = 5,
                    Doc = "The connection to the server was lost, and this machine is trying to connect again. "
                        .. "The world stays as it was; a game can show a message and stop predicting.",
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
                    Doc = "A straight gradient across the object, in the direction `Rotation` gives. `Offset` "
                        .. "moves its center.",
                },
                {
                    Name = "Radial",
                    Value = 1,
                    Doc = "A gradient outward from the center of the object, plus `Offset`, to a radius of (width "
                        .. "+ height) / 4. `Rotation` has no effect.",
                },
                {
                    Name = "Conical",
                    Value = 2,
                    Doc = "A gradient that sweeps one full turn clockwise around the center of the object, plus "
                        .. "`Offset`, starting in the direction `Rotation` gives.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "GradientTileMode",
            Doc = "What a `UIGradient` draws beyond the ends of its sequence.",
            Items = {
                {
                    Name = "Clamp",
                    Value = 0,
                    Doc = "The colors at the two ends continue outward.",
                },
                {
                    Name = "Repeat",
                    Value = 1,
                    Doc = "The sequence repeats from its start. A seam shows where its two ends differ.",
                },
                {
                    Name = "Mirror",
                    Value = 2,
                    Doc = "The sequence repeats alternately backward and forward, so no seam shows.",
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
                    Doc = "The text of a text object, and the border of any other object.",
                },
                {
                    Name = "Border",
                    Value = 1,
                    Doc = "The border of the object, even for a text object.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "LineJoinMode",
            Doc = "How a `UIStroke` draws a corner.",
            Items = {
                {
                    Name = "Round",
                    Value = 0,
                    Doc = "Rounded, as an arc the width of the stroke.",
                },
                {
                    Name = "Bevel",
                    Value = 1,
                    Doc = "Cut off flat across the corner.",
                },
                {
                    Name = "Miter",
                    Value = 2,
                    Doc = "Extended to a sharp point.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "StrokeSizingMode",
            Doc = "The unit of a `UIStroke`'s `Thickness`.",
            Items = {
                {
                    Name = "FixedSize",
                    Value = 0,
                    Doc = "Pixels.",
                },
                {
                    Name = "ScaledSize",
                    Value = 1,
                    Doc = "A fraction of the parent's shorter side, or of the font size on text, so that the "
                        .. "stroke grows with what it outlines.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "BorderStrokePosition",
            Doc = "Where a border `UIStroke` lies relative to the edge of its object.",
            Items = {
                {
                    Name = "Outer",
                    Value = 0,
                    Doc = "Entirely outside the object.",
                },
                {
                    Name = "Center",
                    Value = 1,
                    Doc = "Centered on the edge: half outside and half inside.",
                },
                {
                    Name = "Inner",
                    Value = 2,
                    Doc = "Entirely inside the object.",
                },
            } :: { Schema.EnumItem },
        },
        -- ADR 0107. At the END.
        {
            Name = "ViewQuality",
            Doc = "How completely a `CameraTexture` or a `SubWorld` view is drawn.",
            Items = {
                {
                    Name = "Full",
                    Value = 0,
                    Doc = "Everything the main view draws, including shadows and post-processing effects.",
                },
                {
                    Name = "Simple",
                    Value = 1,
                    Doc = "No shadows and no post-processing effects. It suits a monitor feed or a minimap.",
                },
            } :: { Schema.EnumItem },
        },
        -- ADR 0125. At the END.
        {
            Name = "SceneLoadStatus",
            Doc = "The stage of a `SceneLoad`.",
            Items = {
                {
                    Name = "Preparing",
                    Value = 0,
                    Doc = "The scene's file is being read and its meshes are being loaded.",
                },
                { Name = "Ready", Value = 1, Doc = "The scene is prepared and waiting for `Activate`." },
                { Name = "Activating", Value = 2, Doc = "The switch has started: the old scene is closing." },
                { Name = "Done", Value = 3, Doc = "The scene is open." },
                { Name = "Failed", Value = 4, Doc = "The scene could not be prepared. `Error` gives the reason." },
                {
                    Name = "Canceled",
                    Aliases = { "Cancelled" },
                    Value = 5,
                    Doc = "The load was canceled, by `Cancel` or by another load.",
                },
            } :: { Schema.EnumItem },
        },
        -- ADR 0131. At the END.
        {
            Name = "AssetFetchStatus",
            Doc = "The outcome of one item of `ContentProvider:PreloadAsync`.",
            Items = {
                { Name = "Success", Value = 0, Doc = "Every asset the item uses is loaded." },
                { Name = "Failure", Value = 1, Doc = "An asset the item uses could not be found or read." },
            } :: { Schema.EnumItem },
        },
        -- ADR 0126. At the END.
        {
            Name = "TerrainPaintMode",
            Records = { "ADR 0114" },
            Doc = "What a stroke of `Terrain:PaintBall` does with the two materials a voxel holds, one over the other.",
            Items = {
                { Name = "Replace", Value = 0, Doc = "The ground becomes the material, replacing what was there." },
                {
                    Name = "Blend",
                    Value = 1,
                    Doc = "More of the material shows over what is there, by the strength of the stroke.",
                },
                { Name = "Under", Value = 2, Doc = "The material is put under the material painted over it." },
                { Name = "Erase", Value = 3, Doc = "Less of the upper material shows, revealing the one under it." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "WaterShape",
            Records = { "ADR 0118" },
            Doc = "The extent of a `Water`.",
            Items = {
                { Name = "Ocean", Value = 0, Doc = "Everywhere, with no edge." },
                {
                    Name = "Box",
                    Value = 1,
                    Doc = "A rectangle. It is the earlier name of `Pool` and behaves the same.",
                },
                {
                    Name = "Spline",
                    Value = 2,
                    Doc = "A level river in straight segments from one `WaterPoint` child to the next. Every "
                        .. "point is at the water's `SurfaceLevel`, whatever its own height. For a river that "
                        .. "curves and descends, use `River`.",
                },
                {
                    Name = "Lake",
                    Value = 3,
                    Records = { "ADR 0146" },
                    Doc = "The area inside the closed curve through its `WaterPoint` children, level at "
                        .. "`SurfaceLevel`. It needs at least three points.",
                },
                {
                    Name = "River",
                    Value = 4,
                    Records = { "ADR 0146" },
                    Doc = "A ribbon along the curve through its `WaterPoint` children, from the first to the "
                        .. "last. The height of each point is the height of the surface there, so a river can "
                        .. "descend. Each point's `Width` and `Depth` set the river's width and depth there.",
                },
                { Name = "Pool", Value = 5, Doc = "A rectangle of `Size`, centered on `Position`." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "RenderPriority",
            Records = { "ADR 0136" },
            Doc = "Reference priorities for `RunService:BindToRenderStep`, which takes a number. Use an item's "
                .. "`Value` plus or minus an offset: `Enum.RenderPriority.Camera.Value + 1` runs just after the "
                .. "camera.",
            Items = {
                { Name = "First", Value = 0, Doc = "Before everything else in the frame." },
                { Name = "Input", Value = 100, Doc = "Where the input that the frame reads becomes intents." },
                {
                    Name = "Camera",
                    Value = 200,
                    Doc = "Where the camera is positioned. The built-in camera rig runs here.",
                },
                {
                    Name = "Character",
                    Value = 300,
                    Doc = "After the camera, for anything that follows where the camera looks.",
                },
                { Name = "Last", Value = 2000, Doc = "After everything else, just before `PreRender` fires." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "FocusLossReason",
            Records = { "ADR 0139" },
            Doc = "Why a `TextInput` lost keyboard focus. `FocusLost` passes it along with `submitted`.",
            Items = {
                {
                    Name = "Submitted",
                    Value = 0,
                    Doc = "The player pressed Return in a field that releases focus on submit.",
                },
                {
                    Name = "Moved",
                    Value = 1,
                    Doc = "The player pressed somewhere else, or pressed Tab to go to the next field.",
                },
                {
                    Name = "Canceled",
                    Aliases = { "Cancelled" },
                    Value = 2,
                    Doc = "The player pressed Escape. With `RevertOnEscape` on, the text is restored to what it "
                        .. "was when the field gained focus.",
                },
                {
                    Name = "Script",
                    Value = 3,
                    Doc = "A script called `ReleaseFocus`, another field called `CaptureFocus`, or the field left "
                        .. "the screen.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "TextInputKeyboard",
            Records = { "ADR 0139" },
            Doc = "The on-screen keyboard a phone shows for a `TextInput`. A desktop input method also reads it "
                .. "as a hint. A masked field always shows the password keyboard.",
            Items = {
                { Name = "Default", Value = 0, Doc = "Letters, with the system's suggestions." },
                { Name = "Number", Value = 1, Doc = "Digits." },
                { Name = "Decimal", Value = 2, Doc = "Digits and a decimal point." },
                { Name = "Phone", Value = 3, Doc = "A telephone keypad." },
                { Name = "Email", Value = 4, Doc = "Letters with `@` within easy reach, for an email address." },
                { Name = "Url", Value = 5, Doc = "Letters with `/` and `.` within easy reach, for a web address." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "SwipeDirection",
            Doc = "The direction of a swipe, as `InputService.TouchSwiped` reports it.",
            Items = {
                { Name = "Up", Value = 0, Doc = "Toward the top of the window." },
                { Name = "Down", Value = 1, Doc = "Toward the bottom of the window." },
                { Name = "Left", Value = 2, Doc = "Toward the left of the window." },
                { Name = "Right", Value = 3, Doc = "Toward the right of the window." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "RunOutcome",
            Doc = "How a run of the game ended on this machine, as `RunService:GetLastRun().Outcome` reports it.",
            Items = {
                {
                    Name = "None",
                    Value = 0,
                    Doc = "There was no previous run, or its record was cleared.",
                },
                { Name = "Clean", Value = 1, Doc = "The run was asked to end and ended normally." },
                {
                    Name = "Crashed",
                    Value = 2,
                    Doc = "The run ended in a fault or an uncaught exception. Its report holds the crash note.",
                },
                {
                    Name = "Unfinished",
                    Value = 3,
                    Doc = "The run ended without being asked to and without a fault of its own: the system, the "
                        .. "player or a power loss ended it. On a phone, this is the usual end of a game left in "
                        .. "the background.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "Platform",
            Doc = "The operating system the game is running on, reported by `RunService.Platform`. Use it for the "
                .. "few things that depend on the system, such as store rules or a file path shown to the player. "
                .. "To decide what to show on screen, check what the machine has instead, with "
                .. "`InputService.TouchAvailable` and `UIService.ViewportSize`.",
            Items = {
                { Name = "Windows", Value = 0, Doc = "Windows." },
                { Name = "Linux", Value = 1, Doc = "Linux." },
                { Name = "MacOS", Value = 2, Doc = "macOS." },
                { Name = "Android", Value = 3, Doc = "Android." },
                { Name = "IOS", Value = 4, Doc = "iOS." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ActuatorType",
            Doc = "What drives a hinge, a prismatic joint or a ball socket.",
            Items = {
                { Name = "None", Value = 0, Doc = "Nothing drives the joint; it only constrains the parts." },
                { Name = "Motor", Value = 1, Doc = "The joint moves at a target speed." },
                { Name = "Servo", Value = 2, Doc = "The joint moves to a target position and holds it." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ActuatorRelativeTo",
            Doc = "The frame in which a mover's vector is given.",
            Items = {
                {
                    Name = "Attachment0",
                    Value = 0,
                    Doc = "The frame of the first attachment, which follows its part as the part turns.",
                },
                { Name = "Attachment1", Value = 1, Doc = "The frame of the second attachment." },
                { Name = "World", Value = 2, Doc = "The world's axes." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "VehicleTransmission",
            Doc = "How a `VehicleController`'s gearbox chooses its gear.",
            Items = {
                { Name = "Automatic", Value = 0, Doc = "It shifts by itself, by the engine's turns a minute." },
                { Name = "Manual", Value = 1, Doc = "It stays in the gear the controller's `Gear` asks for." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "VelocityConstraintMode",
            Doc = "How much of the velocity a `LinearVelocity` controls.",
            Items = {
                { Name = "Line", Value = 0, Doc = "Only the speed along a line." },
                { Name = "Plane", Value = 1, Doc = "Only the velocity within a plane." },
                { Name = "Vector", Value = 2, Doc = "The whole velocity." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "PositionAlignmentMode",
            Doc = "The target an `AlignPosition` pulls toward.",
            Items = {
                { Name = "OneAttachment", Value = 0, Doc = "The constraint's own `Position`." },
                { Name = "TwoAttachment", Value = 1, Doc = "The constraint's `Attachment1`." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "OrientationAlignmentMode",
            Doc = "The target an `AlignOrientation` turns toward.",
            Items = {
                { Name = "OneAttachment", Value = 0, Doc = "The constraint's own `CFrame`." },
                { Name = "TwoAttachment", Value = 1, Doc = "The constraint's `Attachment1`." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "XRHand",
            Records = { "ADR 0245" },
            Doc = "A hand, or the controller it holds, for `XRService`.",
            Items = {
                { Name = "Left", Value = 0, Doc = "The left hand." },
                { Name = "Right", Value = 1, Doc = "The right hand." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "VibrationMotor",
            Doc = "One of the vibration motors of a controller, for `HapticService`.",
            Items = {
                { Name = "Large", Value = 0, Doc = "The heavy, slow motor, which gives a rumble." },
                { Name = "Small", Value = 1, Doc = "The light, fast motor, which gives a buzz." },
                {
                    Name = "LeftTrigger",
                    Value = 2,
                    Doc = "The motor in the left trigger, on a controller that has one.",
                },
                {
                    Name = "RightTrigger",
                    Value = 3,
                    Doc = "The motor in the right trigger, on a controller that has one.",
                },
                {
                    Name = "LeftHand",
                    Value = 4,
                    Doc = "The left of two controllers held one in each hand. No supported device has one yet, so "
                        .. "it always reports as unsupported.",
                },
                {
                    Name = "RightHand",
                    Value = 5,
                    Doc = "The right of two controllers held one in each hand. No supported device has one yet, so it always reports as unsupported.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "HighlightDepthMode",
            Doc = "Where a `Highlight` is visible.",
            Items = {
                {
                    Name = "AlwaysOnTop",
                    Value = 0,
                    Doc = "Through anything in front of the shape, as for an ally behind a wall.",
                },
                {
                    Name = "Occluded",
                    Value = 1,
                    Doc = "Only where the shape itself is visible. The part of it behind a wall is not highlighted.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "TextureMode",
            Doc = "How the texture of a `Beam` or a `Trail` is laid along it.",
            Items = {
                {
                    Name = "Stretch",
                    Value = 0,
                    Doc = "Once over the whole length, whatever the length.",
                },
                {
                    Name = "Wrap",
                    Value = 1,
                    Doc = "Repeated every `TextureLength` meters, measured from the start. On a trail, the "
                        .. "texture slides along as the trail moves.",
                },
                {
                    Name = "Static",
                    Value = 2,
                    Doc = "Repeated every `TextureLength` meters and fixed where it was laid. On a trail, the "
                        .. "texture stays over the ground it was drawn over, like a tire mark. On a beam, the "
                        .. "texture does not scroll.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "StartCorner",
            Doc = "The corner from which a `UIGridLayout` starts filling.",
            Items = {
                {
                    Name = "TopLeft",
                    Value = 0,
                    Doc = "From left to right, then from top to bottom.",
                },
                {
                    Name = "TopRight",
                    Value = 1,
                    Doc = "From right to left, then from top to bottom.",
                },
                {
                    Name = "BottomLeft",
                    Value = 2,
                    Doc = "From left to right, then from bottom to top.",
                },
                {
                    Name = "BottomRight",
                    Value = 3,
                    Doc = "From right to left, then from bottom to top.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "UIFlexAlignment",
            Doc = "What a `UIListLayout` does with the space its children leave over along one axis.",
            Items = {
                {
                    Name = "None",
                    Value = 0,
                    Doc = "Nothing. The children stay where the alignment puts them.",
                },
                {
                    Name = "Fill",
                    Value = 1,
                    Doc = "The children grow by equal amounts until the space is used.",
                },
                {
                    Name = "SpaceAround",
                    Value = 2,
                    Doc = "The space is placed around each child: a full share between two children and half a "
                        .. "share at each end.",
                },
                {
                    Name = "SpaceBetween",
                    Value = 3,
                    Doc = "The space is placed between the children, with none at the ends.",
                },
                {
                    Name = "SpaceEvenly",
                    Value = 4,
                    Doc = "The space is placed in equal shares between the children and at both ends.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "UIFlexMode",
            Doc = "How one child of a `UIListLayout` takes up leftover space or gives up space when there is too "
                .. "little.",
            Items = {
                {
                    Name = "None",
                    Value = 0,
                    Doc = "The child keeps its size.",
                },
                {
                    Name = "Grow",
                    Value = 1,
                    Doc = "The child grows into leftover space and never shrinks.",
                },
                {
                    Name = "Shrink",
                    Value = 2,
                    Doc = "The child shrinks when the line is too long and never grows.",
                },
                {
                    Name = "Fill",
                    Value = 3,
                    Doc = "The child both grows and shrinks.",
                },
                {
                    Name = "Custom",
                    Value = 4,
                    Doc = "The child grows by its `GrowRatio` and shrinks by its `ShrinkRatio`.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ItemLineAlignment",
            Doc = "Where a child of a `UIListLayout` is placed across its line.",
            Items = {
                {
                    Name = "Automatic",
                    Value = 0,
                    Doc = "On a `UIFlexItem`, the layout's setting is used. On the layout, the layout's own "
                        .. "alignment across the line is used.",
                },
                {
                    Name = "Start",
                    Value = 1,
                    Doc = "At the start of the line's cross axis: the top of a row, or the left of a column.",
                },
                {
                    Name = "Center",
                    Value = 2,
                    Doc = "In the middle of the line.",
                },
                {
                    Name = "End",
                    Value = 3,
                    Doc = "At the end of the line's cross axis: the bottom of a row, or the right of a column.",
                },
                {
                    Name = "Stretch",
                    Value = 4,
                    Doc = "Stretched across the line: as tall as its row, or as wide as its column.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "AspectType",
            Doc = "How a `UIAspectRatioConstraint` sizes its object.",
            Items = {
                {
                    Name = "FitWithinMaxSize",
                    Value = 0,
                    Doc = "The object becomes the largest rectangle of the ratio that fits inside its own size.",
                },
                {
                    Name = "ScaleWithParentSize",
                    Value = 1,
                    Doc = "The dominant axis takes the parent's full extent, and the other axis follows from the "
                        .. "ratio, even if it exceeds the parent.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "DominantAxis",
            Doc = "The axis a `UIAspectRatioConstraint` takes from its parent when it scales with the parent's size.",
            Items = {
                {
                    Name = "Width",
                    Value = 0,
                    Doc = "The object is as wide as the parent, and its height follows from the ratio.",
                },
                {
                    Name = "Height",
                    Value = 1,
                    Doc = "The object is as tall as the parent, and its width follows from the ratio.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "UIDragDetectorDragStyle",
            Records = { "ADR 0128" },
            Doc = "How a `UIDragDetector` turns the pointer's movement into its object's movement.",
            Items = {
                {
                    Name = "TranslatePlane",
                    Value = 0,
                    Doc = "The object follows the pointer on both axes.",
                },
                {
                    Name = "TranslateLine",
                    Value = 1,
                    Doc = "The object moves only along `DragAxis`, like the thumb of a slider.",
                },
                {
                    Name = "Rotate",
                    Value = 2,
                    Doc = "The object rotates about its center as the pointer moves around it, like a dial.",
                },
                {
                    Name = "Scriptable",
                    Value = 3,
                    Doc = "The object does not move. The detector's events report the pointer's position, and a "
                        .. "script decides what to do.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "UIDragDetectorResponseStyle",
            Doc = "What a `UIDragDetector` changes when its object is dragged.",
            Items = {
                {
                    Name = "Offset",
                    Value = 0,
                    Doc = "The offset of the object's `Position`.",
                },
                {
                    Name = "Scale",
                    Value = 1,
                    Doc = "The scale of the object's `Position`, as a fraction of its parent.",
                },
                {
                    Name = "CustomOffset",
                    Value = 2,
                    Doc = "Nothing. `DragUDim2` and `DragRotation` report the drag as an offset, and a script "
                        .. "applies it.",
                },
                {
                    Name = "CustomScale",
                    Value = 3,
                    Doc = "Nothing. `DragUDim2` reports the drag as a fraction of the parent, and a script applies it.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "GraphicsQuality",
            Records = { "ADR 0147" },
            Doc = "An overall quality level of `GraphicsService`: one of four presets, or `Custom` or `Auto`.",
            Items = {
                {
                    Name = "Low",
                    Value = 0,
                    Doc = "The lowest preset: three quarters of the resolution, short soft shadows, and no bloom "
                        .. "or ambient occlusion.",
                },
                {
                    Name = "Medium",
                    Value = 1,
                    Doc = "The preset a handheld device starts with.",
                },
                {
                    Name = "High",
                    Value = 2,
                    Doc = "The preset a desktop starts with.",
                },
                {
                    Name = "Ultra",
                    Value = 3,
                    Doc = "The highest preset.",
                },
                {
                    Name = "Custom",
                    Value = 4,
                    Doc = "The value `QualityLevel` reads when any quality setting differs from its preset. It "
                        .. "cannot be set.",
                },
                {
                    Name = "Auto",
                    Value = 5,
                    Doc = "The preset that was measured as suitable for this machine.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "GraphicsLevel",
            Doc = "The level of one group of graphics settings.",
            Items = {
                {
                    Name = "Low",
                    Value = 0,
                    Doc = "The lowest level.",
                },
                {
                    Name = "Medium",
                    Value = 1,
                    Doc = "Between `Low` and `High`.",
                },
                {
                    Name = "High",
                    Value = 2,
                    Doc = "The default level.",
                },
                {
                    Name = "Ultra",
                    Value = 3,
                    Doc = "The highest level for play.",
                },
                {
                    Name = "Cinematic",
                    Value = 4,
                    Doc = "A level meant for recorded footage instead of play. It is currently the same as `Ultra`.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "GraphicsGroup",
            Doc = "The groups of graphics settings that an options menu typically shows. A group is set to a "
                .. "level as a whole.",
            Items = {
                {
                    Name = "ViewDistance",
                    Value = 0,
                    Doc = "How far objects are drawn, and the detail of distant terrain.",
                },
                {
                    Name = "AntiAliasing",
                    Value = 1,
                    Doc = "How edges are smoothed.",
                },
                {
                    Name = "PostProcessing",
                    Value = 2,
                    Doc = "Bloom, depth of field, sun rays, automatic exposure and motion blur.",
                },
                {
                    Name = "Shadows",
                    Value = 3,
                    Doc = "The resolution, cascades and distance of shadows, and contact shadows.",
                },
                {
                    Name = "GlobalIllumination",
                    Value = 4,
                    Doc = "Ambient occlusion and bounced light.",
                },
                {
                    Name = "Reflections",
                    Value = 5,
                    Doc = "What shiny surfaces reflect: reflection probes and screen-space reflections.",
                },
                {
                    Name = "Textures",
                    Value = 6,
                    Doc = "The size, filtering and streaming of textures.",
                },
                {
                    Name = "Effects",
                    Value = 7,
                    Doc = "Particles.",
                },
                {
                    Name = "Foliage",
                    Value = 8,
                    Doc = "The density of foliage.",
                },
                {
                    Name = "Shading",
                    Value = 9,
                    Doc = "The number of lights per frame, and shading detail.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ShadowQuality",
            Doc = "The quality of the sun's shadows: their resolution and their number of cascades.",
            Items = {
                {
                    Name = "Off",
                    Value = 0,
                    Doc = "The sun casts no shadows.",
                },
                {
                    Name = "Low",
                    Value = 1,
                    Doc = "512 texels and two cascades.",
                },
                {
                    Name = "Medium",
                    Value = 2,
                    Doc = "1024 texels and three cascades.",
                },
                {
                    Name = "High",
                    Value = 3,
                    Doc = "2048 texels and four cascades.",
                },
                {
                    Name = "Ultra",
                    Value = 4,
                    Doc = "2048 texels and four cascades, over a longer distance.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "AntiAliasingMode",
            Records = { "ADR 0158" },
            Doc = "The method used to smooth edges.",
            Items = {
                {
                    Name = "Off",
                    Value = 0,
                    Doc = "No anti-aliasing.",
                },
                {
                    Name = "FXAA",
                    Value = 1,
                    Doc = "Fast approximate anti-aliasing: one pass over the finished picture that finds edges by "
                        .. "contrast and softens them. It is the cheapest method and slightly blurs fine texture "
                        .. "detail.",
                },
                {
                    Name = "SMAA",
                    Value = 2,
                    Doc = "Subpixel morphological anti-aliasing: three passes over the finished picture that find "
                        .. "edges by their shape and blend them by coverage. It is sharper than FXAA and costs "
                        .. "slightly more.",
                },
                {
                    Name = "TAA",
                    Value = 3,
                    Doc = "Temporal anti-aliasing: each frame is drawn with a subpixel offset and blended with "
                        .. "the previous frames. It removes the shimmer of thin details in motion, such as "
                        .. "fences, wires and distant grass, which a single frame cannot.\n\n"
                        .. "It applies to the main camera only. A `ViewportFrame`, a sub-world and a picture made "
                        .. "only of sprites use SMAA instead. Pixel art among 3D surfaces is never offset or "
                        .. "blended.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "UpscalingMode",
            Records = { "ADR 0158" },
            Doc = "The method used to scale the world up to the window's resolution when "
                .. "`GraphicsService.RenderScale` is below 1.",
            Items = {
                {
                    Name = "None",
                    Value = 0,
                    Doc = "Plain filtering, as when an image is stretched.",
                },
                {
                    Name = "FSR1",
                    Value = 1,
                    Doc = "AMD FidelityFX Super Resolution 1: an upscale that preserves edges, followed by sharpening.",
                },
                {
                    Name = "FSR2",
                    Value = 2,
                    Records = { "ADR 0164" },
                    Doc = "AMD FidelityFX Super Resolution 2: a temporal upscale that builds each pixel of the "
                        .. "window from several frames, each drawn with a subpixel offset. It also does the "
                        .. "anti-aliasing: `AntiAliasing` is ignored on frames it upscales, and at a "
                        .. "`RenderScale` of 1 it acts as anti-aliasing only.\n\n"
                        .. "AMD's names for the scales are quality (0.67), balanced (0.59), performance (0.5) and "
                        .. "ultra performance (0.33). It applies to the main view of a perspective camera, on a "
                        .. "device with compute shaders; everything else is upscaled with FSR 1.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "TextureQuality",
            Doc = "How much of each texture is loaded into memory.",
            Items = {
                {
                    Name = "Low",
                    Value = 0,
                    Doc = "A quarter of each side.",
                },
                {
                    Name = "Medium",
                    Value = 1,
                    Doc = "Half of each side.",
                },
                {
                    Name = "High",
                    Value = 2,
                    Doc = "The full size.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "WindowMode",
            Doc = "How the game's window uses the display.",
            Items = {
                {
                    Name = "Windowed",
                    Value = 0,
                    Doc = "A window with a border.",
                },
                {
                    Name = "Borderless",
                    Value = 1,
                    Doc = "A window with no border that covers the whole display. Switching to other windows "
                        .. "stays fast.",
                },
                {
                    Name = "Fullscreen",
                    Value = 2,
                    Doc = "Exclusive use of the whole display.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "SettingSource",
            Doc = "Where the value of a graphics setting comes from, as `GraphicsService:GetSource` reports it.",
            Items = {
                {
                    Name = "CommandLine",
                    Value = 0,
                    Doc = "A command-line flag the game was started with. A script or the player cannot override it.",
                },
                {
                    Name = "Player",
                    Value = 1,
                    Doc = "The player's saved choice.",
                },
                {
                    Name = "Script",
                    Value = 2,
                    Doc = "A value a script set in this session, which has not been saved.",
                },
                {
                    Name = "Project",
                    Value = 3,
                    Doc = "The game's `project.toml`.",
                },
                {
                    Name = "Preset",
                    Value = 4,
                    Doc = "The current quality level.",
                },
                {
                    Name = "Engine",
                    Value = 5,
                    Doc = "The engine's default; nothing else set the value.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ScrollingDirection",
            Doc = "The axes along which a `ScrollFrame` scrolls.",
            Items = {
                { Name = "X", Value = 1, Doc = "Horizontally only, as for a carousel." },
                { Name = "Y", Value = 2, Doc = "Vertically only, as for a list." },
                {
                    Name = "XY",
                    Value = 3,
                    Doc = "Both ways, as for a map or a canvas larger than its frame in both directions.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ElasticBehavior",
            Doc = "Whether the content of a `ScrollFrame` stretches when dragged beyond its end and springs back. "
                .. "The stretch shows the player that the end was reached.",
            Items = {
                {
                    Name = "WhenScrollable",
                    Value = 0,
                    Doc = "Only on an axis where the canvas is longer than the frame. A list too short to scroll "
                        .. "does not move.",
                },
                {
                    Name = "Always",
                    Value = 1,
                    Doc = "On every axis the frame scrolls along, even when the content fits.",
                },
                { Name = "Never", Value = 2, Doc = "Never. Scrolling stops hard at each end." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "AnimationCullingMode",
            Doc = "How often the meshes of an `AnimationPlayer` are posed.",
            Items = {
                {
                    Name = "Automatic",
                    Value = 0,
                    Doc = "As often as the meshes are visible. They are posed every tick up close, every second, "
                        .. "fourth or eighth tick as they get smaller on screen, and not at all where neither the "
                        .. "camera nor a shadow shows them. Clips keep advancing in every case, so a character is "
                        .. "in the right pose as soon as it is seen.",
                },
                {
                    Name = "AlwaysAnimate",
                    Value = 1,
                    Doc = "Every tick, visible or not. Use it for a main character, or a character whose pose a "
                        .. "script reads.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "Retargeting",
            Records = { "ADR 0199" },
            Doc = "How an `AnimationPlayer` adapts a clip made for one skeleton to another.",
            Items = {
                {
                    Name = "Automatic",
                    Value = 0,
                    Doc = "When both skeletons are humanoid, joints are matched by their role, such as hips, "
                        .. "spine, arms and legs, which is found from common joint naming schemes. A clip made "
                        .. "for one character then works on another with different proportions and joint names: "
                        .. "each joint rotates by the same amount from its own rest pose, the hips move in "
                        .. "proportion to the length of the legs, and every bone keeps its own length.\n\n"
                        .. "Otherwise, and always for two files that share one skeleton, joints are matched by name.",
                },
                {
                    Name = "ByName",
                    Value = 1,
                    Doc = "Joints are matched by name only, and the clip's transforms are applied unchanged. Use "
                        .. "it for clothing on the body it was made for, or for a creature with its own clips. A "
                        .. "joint that one side does not have is skipped silently.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "IKControlType",
            Records = { "ADR 0198" },
            Doc = "What an `IKControl` does to its joints.",
            Items = {
                {
                    Name = "TwoBone",
                    Value = 0,
                    Doc = "A limb of two bones, such as an arm or a leg, is bent so that its end reaches the target.",
                },
                {
                    Name = "LookAt",
                    Value = 1,
                    Doc = "A joint is turned to face the target, and the joints above it share the turn.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ParticleCollision",
            Records = { "ADR 0160" },
            Doc = "What the particles of a `ParticleEmitter` collide with.",
            Items = {
                { Name = "None", Value = 0, Doc = "Nothing. Particles pass through the world." },
                {
                    Name = "SceneDepth",
                    Value = 1,
                    Doc = "The surfaces that are drawn. On the GPU, the depth of the picture is used: every "
                        .. "surface on screen, but nothing off screen or hidden behind another surface. On the "
                        .. "CPU, a ray is cast against the parts in the particle's way.",
                },
                {
                    Name = "Terrain",
                    Value = 2,
                    Doc = "The terrain, whether it is in view or not. Most sparks, rain and debris land on it.",
                },
                { Name = "Both", Value = 3, Doc = "The terrain and the surfaces that are drawn." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ParticleCollisionResponse",
            Records = { "ADR 0160" },
            Doc = "What a particle does when it hits something.",
            Items = {
                {
                    Name = "Bounce",
                    Value = 0,
                    Doc = "It bounces, keeping the share of its speed that `Bounce` sets and losing speed along "
                        .. "the surface by `Friction`. It comes to rest when it barely rises.",
                },
                { Name = "Stick", Value = 1, Doc = "It stays where it landed for the rest of its life." },
                { Name = "Kill", Value = 2, Doc = "It is removed, like rain on a roof or a spark on water." },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ParticleSimulation",
            Records = { "ADR 0160" },
            Doc = "Whether the particles of a `ParticleEmitter` are simulated on the CPU or on the GPU.",
            Items = {
                {
                    Name = "Cpu",
                    Value = 0,
                    Doc = "On the CPU: a few thousand particles per emitter, sorted from back to front together "
                        .. "with all other particles. It works on every machine.",
                },
                {
                    Name = "Gpu",
                    Value = 1,
                    Doc = "On the GPU: tens of thousands of particles per emitter for a few draw calls, emitted, "
                        .. "moved and collided in a compute shader. They are not sorted among themselves, which "
                        .. "is not visible with additive particles.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "DecalBlendMode",
            Records = { "ADR 0160" },
            Doc = "How a `Decal` is combined with the surface it lands on.",
            Items = {
                {
                    Name = "Multiply",
                    Value = 0,
                    Doc = "The image multiplies the surface: it darkens and tints and cannot brighten. Use it for "
                        .. "a stain, a shadow or a scorch mark.",
                },
                {
                    Name = "Alpha",
                    Value = 1,
                    Doc = "The image is laid over the surface by its alpha, and lit like the surface. Use it for "
                        .. "a painted sign or a marking that must be readable on any ground.",
                },
                {
                    Name = "Additive",
                    Value = 2,
                    Doc = "The image's light is added to the surface, unlit. Use it for a glow, an aura or a "
                        .. "magic circle.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ParticleFlipbookMode",
            Records = { "ADR 0160" },
            Doc = "How a particle plays the frames of its emitter's flipbook.",
            Items = {
                {
                    Name = "Loop",
                    Value = 0,
                    Doc = "The frames play in order at `FlipbookFramerate`, repeating. Use it for fire, a flag or "
                        .. "a glint.",
                },
                {
                    Name = "OverLife",
                    Value = 1,
                    Doc = "The frames play once over the particle's life, and the last is held at the end. Use it "
                        .. "for a puff of smoke or an explosion.",
                },
                {
                    Name = "Random",
                    Value = 2,
                    Doc = "Each particle shows one frame, chosen at random when it is emitted. Use it for debris, "
                        .. "leaves or a mix of shapes from one image.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "SwarmAgentRemoval",
            Records = { "ADR 0162" },
            Doc = "Why a machine no longer has an agent of a `Swarm`, as `AgentRemoved` reports it.",
            Items = {
                { Name = "Removed", Value = 0, Doc = "The authority removed the agent." },
                {
                    Name = "OutOfReach",
                    Value = 1,
                    Doc = "On a client: the agent left the client's replication radius. It still exists on the "
                        .. "authority and is added again if it returns.",
                },
            } :: { Schema.EnumItem },
        },
        -- ADR 0178. At the END: an enum's id is its position, and it travels.
        {
            Name = "RelayState",
            Records = { "ADR 0178" },
            Doc = "The state of a hosted match with its relay, reported by `NetworkService.RelayState`.",
            Items = {
                {
                    Name = "None",
                    Value = 0,
                    Doc = "No relay was requested, or this machine is not hosting. The match can be joined by address.",
                },
                {
                    Name = "Connecting",
                    Value = 1,
                    Doc = "The relay was asked to register the match and has not answered yet.",
                },
                {
                    Name = "Ready",
                    Value = 2,
                    Doc = "The relay has registered the match, and others can join with `JoinCode`.",
                },
                {
                    Name = "Unreachable",
                    Value = 3,
                    Doc = "The relay does not answer, or refused. The engine keeps trying, and the match can "
                        .. "still be joined by address.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "SoundCategory",
            Records = { "ADR 0200" },
            Doc = "The kind of sound a `Sound` is. It selects which of the player's three volume settings "
                .. "applies, and music is lowered while a voice plays.",
            Items = {
                {
                    Name = "Effects",
                    Value = 0,
                    Doc = "Everything that is not music or voice: a footstep, a door, a shot, the wind.",
                },
                {
                    Name = "Music",
                    Value = 1,
                    Doc = "Music. It is lowered while a `Voice` sound is audible (see `AudioService.MusicUnderVoice`).",
                },
                {
                    Name = "Voice",
                    Value = 2,
                    Doc = "Speech: a line of dialogue, a narrator or a spoken shout.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "SubtitleMode",
            Records = { "ADR 0200" },
            Doc = "When captions are shown, set by `DialogueService.Subtitles`.",
            Items = {
                {
                    Name = "Auto",
                    Value = 0,
                    Doc = "Shown when the player does not hear the line in the language they read. That is the "
                        .. "case when the voice language differs from the text language, the line fell back to "
                        .. "the default language's recording, or the line has no recording.",
                },
                {
                    Name = "On",
                    Value = 1,
                    Doc = "Always shown.",
                },
                {
                    Name = "Off",
                    Value = 2,
                    Doc = "Never shown.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "LipSyncMode",
            Records = { "ADR 0200" },
            Doc = "The method a `LipSync` uses to move a mouth.",
            Items = {
                {
                    Name = "Auto",
                    Value = 0,
                    Doc = "The best method available. It is the line's viseme track if the language being heard "
                        .. "has one; otherwise the sound's frequency bands, if the face map has targets for them; "
                        .. "otherwise the sound's loudness.",
                },
                {
                    Name = "Bands",
                    Value = 1,
                    Doc = "The sound's frequency bands, even if the line has a viseme track. They tell an open "
                        .. "mouth from a wide or a round one.",
                },
                {
                    Name = "Loudness",
                    Value = 2,
                    Doc = "The sound's loudness only, which opens a single morph target.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "IrradianceMode",
            Records = { "ADR 0203" },
            Doc = "What light an `IrradianceVolume` computes, and how it is combined with the ambient light.",
            Items = {
                {
                    Name = "Full",
                    Value = 0,
                    Doc = "All the indirect light a surface receives: the sky through every opening, and all "
                        .. "bounced light. Inside the volume it replaces the sky's light and `Lighting.Ambient`.",
                },
                {
                    Name = "BounceOnly",
                    Value = 1,
                    Doc = "Bounced light only: light reflected from surfaces that the sun and other lights "
                        .. "illuminate, and from emissive surfaces. It is added to the sky's light and "
                        .. "`Lighting.Ambient`, which apply as they do outside the volume. Use it where the "
                        .. "ambient light is set by hand or by the time of day and should stay as set.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "ScreenSplit",
            Records = { "ADR 0205" },
            Doc = "How the window is divided between the local players on one machine.",
            Items = {
                {
                    Name = "None",
                    Value = 0,
                    Doc = "One picture, through `Workspace.CurrentCamera`.",
                },
                {
                    Name = "Columns",
                    Value = 1,
                    Doc = "One view for each local player that has a camera, side by side.",
                },
                {
                    Name = "Rows",
                    Value = 2,
                    Doc = "One view for each local player that has a camera, stacked one above another.",
                },
                {
                    Name = "Grid",
                    Value = 3,
                    Doc = "Two views side by side, or three or four views in the quarters of the window.",
                },
                {
                    Name = "Dynamic",
                    Value = 4,
                    Doc = "One picture while two players are close together, and two views divided by a line when "
                        .. "they move apart.",
                },
            } :: { Schema.EnumItem },
        },
        {
            Name = "SequenceControl",
            Records = { "ADR 0204" },
            Doc = "What a playing `SequencePlayer` does with the local players' input.",
            Items = {
                {
                    Name = "Keep",
                    Value = 0,
                    Doc = "Nothing. The players keep control during the sequence.",
                },
                {
                    Name = "Suspend",
                    Value = 1,
                    Doc = "The players' movement and actions read as released until the sequence ends.",
                },
            } :: { Schema.EnumItem },
        },
    },
}

return Definition
