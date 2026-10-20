--!strict
--- Value types (api-design.md §2.3).
---
--- Constructors and namespace constants are camelCase; members reached through
--- a value are PascalCase (ADR 0034). The schema's validator enforces both, so
--- the split is not something a definition can get wrong quietly.
---
--- The kernel's set (`Vector3`, `CFrame`, `Color3`, `Random`, `Signal`,
--- `Connection`), the physics work's `RaycastParams` and `RaycastResult`, and the input and UI work's
--- screen-space four: `Vector2`, `UDim`, `UDim2` and `Rect`. `TweenInfo` lands
--- with `TweenService`, the only thing that reads one. `Content` is an alias of
--- `string` and is emitted by the definition generator rather than declared
--- here.
---
--- `Module` is `script` for every entry: a value type is bound in the VM by the
--- `script` module, not registered into the scene
--- like an Instance class -- nothing here has an `InstanceId`.
---
--- `Constructors` holds every namespace-level *function*, not only the ones
--- that construct: `vector.magnitude` and `vector.clamp` sit beside
--- `Vector3.new` because they are reached the same way, and the casing rule
--- keys on how a name is reached rather than on what it does.

local Schema = require("../schema")

--- Annotated on its own rather than inline in `Definition`, because the new
--- solver infers an array literal's element type from the elements it can see
--- and then holds every later entry to it -- so the first datatype that omits
--- an optional field makes every other one an error. The annotation is what
--- lets `Schema.Datatype` flow down into the literal instead.
---
--- The same thing happens one level down, and for a specific reason: an
--- expected type does not propagate through an *optional* field, so
--- `Constructors: { Method }?` leaves its literal to infer itself while
--- `Parameters: { Parameter }` inherits fine. The `:: { Schema.Method }` and
--- `:: { Schema.Property }` assertions on the four member lists supply what the
--- `?` swallows. They are not casts that hide anything -- a misspelt field or a
--- wrong value type still fails exactly where it is written; verified against
--- both a typo and a bad type before being relied on.
local Datatypes: { Schema.Datatype } = {
    {
        Name = "Vector3",
        Module = "script",
        Records = { "ADR 0013", "ADR 0014" },
        Doc = "A vector of three 32-bit floats. It is Luau's native `vector` type, not an object: `typeof` "
            .. 'returns `"vector"`, and arithmetic allocates no memory. Its components are the lowercase fields '
            .. "`x`, `y` and `z`.\n\n"
            .. "The `Vector3` global is the `vector` library plus `new`, so every function is available under "
            .. "both names, and `Vector3.zero == vector.zero`.\n\n"
            .. "The operators are `+`, `-` (binary and unary), `*` and `/` (by a number on either side, or by "
            .. "another vector component by component), `//`, `%` and `==`.\n\n"
            .. "A 32-bit float is exact to the millimeter up to about 8 km from the origin, and to about 16 mm at "
            .. "131 km. For positions in a very large world, work with `CFrame`, which stores its position in 64 "
            .. "bits.",
        Constructors = {
            {
                Name = "new",
                Parameters = {
                    { Name = "x", Type = "number?", Default = "0" },
                    { Name = "y", Type = "number?", Default = "0" },
                    { Name = "z", Type = "number?", Default = "0" },
                },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns a vector with the given components. A missing component is 0. The call allocates "
                    .. "no memory.",
            },
            {
                Name = "create",
                Parameters = {
                    { Name = "x", Type = "number" },
                    { Name = "y", Type = "number" },
                    { Name = "z", Type = "number?", Default = "0" },
                },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns a vector with the given components. It is the constructor of Luau's standard "
                    .. "`vector` library, for code shared with other Luau runtimes. Unlike `new`, it requires `x` "
                    .. "and `y`.",
            },
            {
                Name = "magnitude",
                Parameters = { { Name = "v", Type = "vector" } },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "Returns the length of a vector. It is faster than reading the `Magnitude` field.",
            },
            {
                Name = "normalize",
                Parameters = { { Name = "v", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns a vector scaled to a length of 1. Normalizing a zero vector returns NaN "
                    .. "components; it does not raise an error.",
            },
            {
                Name = "dot",
                Parameters = { { Name = "a", Type = "vector" }, { Name = "b", Type = "vector" } },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "Returns the dot product of two vectors.",
            },
            {
                Name = "cross",
                Parameters = { { Name = "a", Type = "vector" }, { Name = "b", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns the cross product of two vectors. It is right-handed: for the axes of any "
                    .. "`CFrame`, `cross(right, up) == -look`.",
            },
            {
                Name = "angle",
                Parameters = {
                    { Name = "a", Type = "vector" },
                    { Name = "b", Type = "vector" },
                    { Name = "axis", Type = "vector?", Default = "nil" },
                },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "Returns the angle between two vectors, in radians, from 0 to π. With `axis`, the angle is "
                    .. "signed about that axis by the right-hand rule and ranges from -π to π.",
            },
            {
                Name = "lerp",
                Parameters = {
                    { Name = "a", Type = "vector" },
                    { Name = "b", Type = "vector" },
                    { Name = "alpha", Type = "number" },
                },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns the linear interpolation from `a` to `b`, component by component. `alpha` is not "
                    .. "clamped, so a value outside 0 to 1 extrapolates.",
            },
            {
                Name = "floor",
                Parameters = { { Name = "v", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns a vector with each component rounded down.",
            },
            {
                Name = "ceil",
                Parameters = { { Name = "v", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns a vector with each component rounded up.",
            },
            {
                Name = "abs",
                Parameters = { { Name = "v", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns a vector with the absolute value of each component.",
            },
            {
                Name = "sign",
                Parameters = { { Name = "v", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns a vector with -1, 0 or 1 for each component, according to its sign.",
            },
            {
                Name = "clamp",
                Parameters = {
                    { Name = "v", Type = "vector" },
                    { Name = "min", Type = "vector" },
                    { Name = "max", Type = "vector" },
                },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns a vector with each component clamped between the matching components of `min` and "
                    .. "`max`. Raises an error if a component of `min` is greater than that of `max`.",
            },
            {
                Name = "min",
                Parameters = { { Name = "a", Type = "vector" }, { Name = "rest", Type = "...vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns a vector with the smallest value of each component among the arguments. The result "
                    .. "is a corner of their bounding box, not the shortest argument.",
            },
            {
                Name = "max",
                Parameters = { { Name = "a", Type = "vector" }, { Name = "rest", Type = "...vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns a vector with the largest value of each component among the arguments. The result "
                    .. "is the corner of their bounding box opposite the one `min` returns.",
            },
        } :: { Schema.Method },
        Constants = {
            {
                Name = "zero",
                Type = "vector",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The vector `(0, 0, 0)`.",
            },
            {
                Name = "one",
                Type = "vector",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The vector `(1, 1, 1)`.",
            },
        } :: { Schema.Property },
        Properties = {
            {
                Name = "x",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The X component. The component names are lowercase because they are fields of Luau's "
                    .. "native vector type. `v.X` also reads the component at run time, but only the lowercase "
                    .. "name is declared, so the type checker rejects the uppercase one.",
            },
            {
                Name = "y",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The Y component. Y is up.",
            },
            {
                Name = "z",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The Z component. Forward is -Z, so a direction that points forward has a negative `z`.",
            },
            {
                Name = "Magnitude",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The length of the vector. `vector.magnitude(v)` returns the same value and is faster.",
            },
            {
                Name = "Unit",
                Type = "vector",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The vector scaled to a length of 1. `vector.normalize(v)` returns the same value and is faster.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Dot",
                Parameters = { { Name = "other", Type = "vector" } },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "Returns the dot product with `other`. It is the method form of `vector.dot`.",
            },
            {
                Name = "Cross",
                Parameters = { { Name = "other", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns the cross product with `other`, in that order. It is the method form of "
                    .. "`vector.cross`. The order matters: `a:Cross(b) == -b:Cross(a)`.",
            },
            {
                Name = "Lerp",
                Parameters = { { Name = "goal", Type = "vector" }, { Name = "alpha", Type = "number" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Returns the linear interpolation toward `goal`. It is the method form of `vector.lerp`. "
                    .. "`alpha` is not clamped.",
            },
            {
                Name = "Angle",
                Parameters = {
                    { Name = "other", Type = "vector" },
                    { Name = "axis", Type = "vector?", Default = "nil" },
                },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "Returns the angle to `other`, in radians. It is the method form of `vector.angle`, "
                    .. "including the optional `axis`.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "CFrame",
        Module = "script",
        Records = { "ADR 0014" },
        Doc = "A position and an orientation: a rotation plus a translation. Use it to place and orient anything "
            .. "in the world.\n\n"
            .. "The translation is stored in 64-bit floats, so a `CFrame` stays precise far from the origin. "
            .. "`Position` is that translation rounded to 32 bits. In a very large world, compute with `CFrame` "
            .. "values instead of converting to positions and back.\n\n"
            .. "The world is right-handed, with Y up and -Z forward, as in glTF.\n\n"
            .. "`CFrame * CFrame` combines two transforms. `CFrame * vector` transforms a point, including the "
            .. "translation. `==` compares every component exactly.",
        Constructors = {
            {
                Name = "new",
                Parameters = {
                    { Name = "x", Type = "number?", Default = "0" },
                    { Name = "y", Type = "number?", Default = "0" },
                    { Name = "z", Type = "number?", Default = "0" },
                },
                Returns = { "CFrame" },
                ThreadSafety = "Safe",
                Doc = "Returns a `CFrame` at the given position with no rotation. A missing component is 0, so "
                    .. "`CFrame.new()` is the identity. The components are stored as 64-bit floats.",
            },
            {
                Name = "new",
                Parameters = { { Name = "position", Type = "vector" } },
                Returns = { "CFrame" },
                ThreadSafety = "Safe",
                Doc = "Returns a `CFrame` at `position` with no rotation. A vector has 32-bit components; pass "
                    .. "three numbers where the extra precision matters.",
            },
            {
                Name = "lookAt",
                Parameters = {
                    { Name = "position", Type = "vector" },
                    { Name = "target", Type = "vector" },
                    { Name = "up", Type = "vector?", Default = "Vector3.new(0, 1, 0)" },
                },
                Returns = { "CFrame" },
                ThreadSafety = "Safe",
                Doc = "Returns a `CFrame` at `position` whose `LookVector` points toward `target`. `up` sets "
                    .. "which way is up and defaults to `(0, 1, 0)`. If `target` equals `position`, or `up` is "
                    .. "parallel to the direction, the result has no rotation.",
            },
            {
                Name = "fromEuler",
                Parameters = {
                    { Name = "rx", Type = "number" },
                    { Name = "ry", Type = "number" },
                    { Name = "rz", Type = "number" },
                    { Name = "order", Type = "Enum.RotationOrder?", Default = "Enum.RotationOrder.YXZ" },
                },
                Returns = { "CFrame" },
                ThreadSafety = "Safe",
                Doc = "Returns a rotation from three angles, in radians, applied in the order that `order` gives. "
                    .. "Each rotation is about the axes produced by the previous one (intrinsic rotations), and "
                    .. "positive angles follow the right-hand rule.",
            },
            {
                Name = "fromAxisAngle",
                Parameters = { { Name = "axis", Type = "vector" }, { Name = "angle", Type = "number" } },
                Returns = { "CFrame" },
                ThreadSafety = "Safe",
                Doc = "Returns a rotation of `angle` radians about `axis`, by the right-hand rule, at the origin. "
                    .. "The length of the axis is ignored.",
            },
            {
                Name = "fromQuaternion",
                Parameters = {
                    { Name = "position", Type = "vector" },
                    { Name = "qx", Type = "number" },
                    { Name = "qy", Type = "number" },
                    { Name = "qz", Type = "number" },
                    { Name = "qw", Type = "number" },
                },
                Returns = { "CFrame" },
                ThreadSafety = "Safe",
                Doc = "Returns a `CFrame` at `position` with the rotation of the quaternion `(qx, qy, qz, qw)`, "
                    .. "with w last. The quaternion is normalized.",
            },
            {
                Name = "fromMatrix",
                Parameters = {
                    { Name = "position", Type = "vector" },
                    { Name = "right", Type = "vector" },
                    { Name = "up", Type = "vector" },
                    { Name = "back", Type = "vector" },
                },
                Returns = { "CFrame" },
                ThreadSafety = "Safe",
                Doc = "Returns a `CFrame` from a position and three axes. The third axis is the back direction, "
                    .. "so `LookVector` is `-back`. The axes are stored as given, even if they are not unit "
                    .. "length or not perpendicular; call `Orthonormalize` to correct them.",
            },
        } :: { Schema.Method },
        Constants = {
            {
                Name = "identity",
                Type = "CFrame",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The `CFrame` at the origin with no rotation. `cf * CFrame.identity == cf`.",
            },
        } :: { Schema.Property },
        Properties = {
            {
                Name = "Position",
                Type = "vector",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The translation, as a vector. The value is rounded from 64 to 32 bits, so it loses "
                    .. "precision far from the origin.",
            },
            {
                Name = "Rotation",
                Type = "CFrame",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The same orientation with the translation set to zero.",
            },
            {
                Name = "RightVector",
                Type = "vector",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The direction of the +X axis.",
            },
            {
                Name = "UpVector",
                Type = "vector",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The direction of the +Y axis.",
            },
            {
                Name = "LookVector",
                Type = "vector",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The forward direction, which is the -Z axis. Because forward is -Z, "
                    .. "`RightVector:Cross(UpVector)` is `-LookVector`.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Inverse",
                Parameters = {},
                Returns = { "CFrame" },
                ThreadSafety = "Safe",
                Doc = "Returns the transform that reverses this one: `cf * cf:Inverse()` is the identity, up to "
                    .. "rounding.",
            },
            {
                Name = "Lerp",
                Parameters = { { Name = "goal", Type = "CFrame" }, { Name = "alpha", Type = "number" } },
                Returns = { "CFrame" },
                ThreadSafety = "Safe",
                Doc = "Returns the interpolation toward `goal`. The position is interpolated linearly and the "
                    .. "rotation spherically, so the rotation turns at a constant rate. `alpha` is not clamped, "
                    .. "so a value outside 0 to 1 extrapolates.",
            },
            {
                Name = "Orthonormalize",
                Parameters = {},
                Returns = { "CFrame" },
                ThreadSafety = "Safe",
                Doc = "Returns the `CFrame` with its axes made unit length and perpendicular, and its position "
                    .. "unchanged. `LookVector` keeps its direction, `UpVector` is made perpendicular to it, and "
                    .. "`RightVector` follows from the two. Use it on a `CFrame` built with `fromMatrix`, or "
                    .. "after many multiplications have accumulated rounding error.",
            },
            {
                Name = "ToWorldSpace",
                Parameters = { { Name = "cf", Type = "CFrame" } },
                Returns = { "CFrame" },
                ThreadSafety = "Safe",
                Doc = "Converts `cf` from this `CFrame`'s local space to world space. It returns `self * cf`.",
            },
            {
                Name = "ToObjectSpace",
                Parameters = { { Name = "cf", Type = "CFrame" } },
                Returns = { "CFrame" },
                ThreadSafety = "Safe",
                Doc = "Converts the world-space `cf` to this `CFrame`'s local space. It returns `self:Inverse() * cf`.",
            },
            {
                Name = "PointToWorldSpace",
                Parameters = { { Name = "point", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Converts a position from this `CFrame`'s local space to world space. It returns `self * point`.",
            },
            {
                Name = "PointToObjectSpace",
                Parameters = { { Name = "point", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Converts a world-space position to this `CFrame`'s local space. It returns `self:Inverse() "
                    .. "* point`.",
            },
            {
                Name = "VectorToWorldSpace",
                Parameters = { { Name = "direction", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Converts a direction from this `CFrame`'s local space to world space, applying the "
                    .. "rotation only. It returns `self.Rotation * direction`. Use it for directions such as "
                    .. "surface normals; `PointToWorldSpace` would also add the position.",
            },
            {
                Name = "VectorToObjectSpace",
                Parameters = { { Name = "direction", Type = "vector" } },
                Returns = { "vector" },
                ThreadSafety = "Safe",
                Doc = "Converts a world-space direction to this `CFrame`'s local space, applying the rotation "
                    .. "only. It returns `self.Rotation:Inverse() * direction`.",
            },
            {
                Name = "ToEuler",
                Parameters = {
                    { Name = "order", Type = "Enum.RotationOrder?", Default = "Enum.RotationOrder.YXZ" },
                },
                Returns = { "number", "number", "number" },
                ThreadSafety = "Safe",
                Doc = "Returns the rotation as three angles `(rx, ry, rz)`, in radians, in the parameter order of "
                    .. "`fromEuler`. It is the inverse of `fromEuler` for the same `order`. Euler angles are not "
                    .. "unique, so converting to angles and back gives an equivalent rotation, not necessarily "
                    .. "the same three numbers.",
            },
            {
                Name = "ToAxisAngle",
                Parameters = {},
                Returns = { "vector", "number" },
                ThreadSafety = "Safe",
                Doc = "Returns the rotation as a unit axis and an angle in radians. It is the inverse of "
                    .. "`fromAxisAngle`.",
            },
            {
                Name = "ToQuaternion",
                Parameters = {},
                Returns = { "number", "number", "number", "number" },
                ThreadSafety = "Safe",
                Doc = "Returns the rotation as a quaternion `(qx, qy, qz, qw)`, with w last. It is the inverse of "
                    .. "`fromQuaternion`.",
            },
        } :: { Schema.Method },
        Operators = {
            {
                Name = "__mul",
                Parameters = { { Name = "other", Type = "CFrame" } },
                Returns = { "CFrame" },
                Doc = "Combines two transforms. `a * b` applies `b` first and then `a`.",
            },
            {
                Name = "__mul",
                Parameters = { { Name = "point", Type = "vector" } },
                Returns = { "vector" },
                Doc = "Transforms a point, including the translation. For a direction, use `VectorToWorldSpace`.",
            },
            {
                Name = "__eq",
                Parameters = { { Name = "other", Type = "CFrame" } },
                Returns = { "boolean" },
                Doc = "Compares two `CFrame` values exactly, component by component, with no tolerance.",
            },
        } :: { Schema.Operator },
    },
    {
        Name = "Color3",
        Module = "script",
        Doc = "A color with red, green and blue channels, each a 32-bit float nominally from 0 to 1. The channels "
            .. "are not clamped: values above 1 are valid and are used for bright emissive colors and tints. `==` "
            .. "compares every channel exactly.",
        Constructors = {
            {
                Name = "new",
                Parameters = {
                    { Name = "r", Type = "number?", Default = "0" },
                    { Name = "g", Type = "number?", Default = "0" },
                    { Name = "b", Type = "number?", Default = "0" },
                },
                Returns = { "Color3" },
                ThreadSafety = "Safe",
                Doc = "Returns a color from three channels in the range 0 to 1. Each defaults to 0, so "
                    .. "`Color3.new()` is black. Values outside the range are kept.",
            },
            {
                Name = "fromRGB",
                Parameters = {
                    { Name = "r", Type = "number" },
                    { Name = "g", Type = "number" },
                    { Name = "b", Type = "number" },
                },
                Returns = { "Color3" },
                ThreadSafety = "Safe",
                Doc = "Returns a color from three channels in the range 0 to 255, as color pickers show them. "
                    .. "Each channel is divided by 255.",
            },
            {
                Name = "fromHSV",
                Parameters = {
                    { Name = "h", Type = "number" },
                    { Name = "s", Type = "number" },
                    { Name = "v", Type = "number" },
                },
                Returns = { "Color3" },
                ThreadSafety = "Safe",
                Doc = "Returns a color from hue, saturation and value, each from 0 to 1. The hue is also 0 to 1, "
                    .. "not degrees: `fromHSV(1/3, 1, 1)` is green.",
            },
            {
                Name = "fromHex",
                Parameters = { { Name = "hex", Type = "string" } },
                Returns = { "Color3" },
                ThreadSafety = "Safe",
                Doc = "Returns a color from a hexadecimal string. It accepts `#rrggbb`, `rrggbb`, `#rgb` and "
                    .. "`rgb`, in upper or lower case. Raises an error for any other string.",
            },
        } :: { Schema.Method },
        Properties = {
            {
                Name = "R",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The red channel, nominally 0 to 1 and not clamped.",
            },
            {
                Name = "G",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The green channel, nominally 0 to 1 and not clamped.",
            },
            {
                Name = "B",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The blue channel, nominally 0 to 1 and not clamped.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Lerp",
                Parameters = { { Name = "goal", Type = "Color3" }, { Name = "alpha", Type = "number" } },
                Returns = { "Color3" },
                ThreadSafety = "Safe",
                Doc = "Returns the linear interpolation toward `goal`, channel by channel in RGB. `alpha` is not "
                    .. "clamped.",
            },
            {
                Name = "ToHSV",
                Parameters = {},
                Returns = { "number", "number", "number" },
                ThreadSafety = "Safe",
                Doc = "Returns the color as hue, saturation and value, each from 0 to 1. It is the inverse of "
                    .. "`fromHSV`. The hue of a gray is 0.",
            },
            {
                Name = "ToHex",
                Parameters = {},
                Returns = { "string" },
                ThreadSafety = "Safe",
                Doc = "Returns the color as six lowercase hexadecimal digits with no leading `#`, such as "
                    .. '`"ff8800"`. Each channel is clamped to the range 0 to 255, so a channel above 1 is not '
                    .. "preserved.",
            },
        } :: { Schema.Method },
        Operators = {
            {
                Name = "__eq",
                Parameters = { { Name = "other", Type = "Color3" } },
                Returns = { "boolean" },
                Doc = "Compares two colors exactly, channel by channel. Two colors that would look the same after "
                    .. "clamping are still unequal if their stored values differ.",
            },
        } :: { Schema.Operator },
    },
    {
        Name = "Vector2",
        Module = "script",
        Records = { "ADR 0013" },
        Doc = "A vector of two 32-bit floats, used mostly for screen positions and sizes. It supports `+`, `-`, "
            .. "unary `-`, `*` and `/` by a number on either side or by another `Vector2` component by component, "
            .. "and an exact `==`.",
        Constructors = {
            {
                Name = "new",
                Parameters = {
                    { Name = "x", Type = "number?", Default = "0" },
                    { Name = "y", Type = "number?", Default = "0" },
                },
                Returns = { "Vector2" },
                ThreadSafety = "Safe",
                Doc = "Returns a vector with the given components. Each defaults to 0.",
            },
        } :: { Schema.Method },
        Constants = {
            {
                Name = "zero",
                Type = "Vector2",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The vector `(0, 0)`.",
            },
            {
                Name = "one",
                Type = "Vector2",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The vector `(1, 1)`.",
            },
        } :: { Schema.Property },
        Properties = {
            {
                Name = "X",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The horizontal component. It is uppercase, unlike `Vector3.x`.",
            },
            {
                Name = "Y",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The vertical component. In screen coordinates, Y increases downward.",
            },
            {
                Name = "Magnitude",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The length of the vector.",
            },
            {
                Name = "Unit",
                Type = "Vector2",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The vector scaled to a length of 1. The unit of a zero vector is the zero vector.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Dot",
                Parameters = { { Name = "other", Type = "Vector2" } },
                Returns = { "number" },
                ThreadSafety = "Safe",
                Doc = "Returns the dot product with another vector.",
            },
            {
                Name = "Lerp",
                Parameters = {
                    { Name = "goal", Type = "Vector2" },
                    { Name = "alpha", Type = "number" },
                },
                Returns = { "Vector2" },
                ThreadSafety = "Safe",
                Doc = "Returns the linear interpolation toward `goal`. `alpha` is not clamped, so a value outside "
                    .. "0 to 1 extrapolates.",
            },
        } :: { Schema.Method },
        Operators = {
            {
                Name = "__add",
                Parameters = { { Name = "other", Type = "Vector2" } },
                Returns = { "Vector2" },
                Doc = "Adds two vectors component by component.",
            },
            {
                Name = "__sub",
                Parameters = { { Name = "other", Type = "Vector2" } },
                Returns = { "Vector2" },
                Doc = "Subtracts one vector from another component by component.",
            },
            {
                Name = "__unm",
                Parameters = {},
                Returns = { "Vector2" },
                Doc = "Negates the vector.",
            },
            {
                Name = "__mul",
                Parameters = { { Name = "other", Type = "Vector2" } },
                Returns = { "Vector2" },
                Doc = "Multiplies two vectors component by component.",
            },
            {
                Name = "__mul",
                Parameters = { { Name = "scalar", Type = "number" } },
                Returns = { "Vector2" },
                Doc = "Multiplies a vector by a number. The number can be on either side.",
            },
            {
                Name = "__div",
                Parameters = { { Name = "other", Type = "Vector2" } },
                Returns = { "Vector2" },
                Doc = "Divides one vector by another component by component.",
            },
            {
                Name = "__div",
                Parameters = { { Name = "scalar", Type = "number" } },
                Returns = { "Vector2" },
                Doc = "Divides a vector by a number, or a number by a vector, component by component.",
            },
            {
                Name = "__eq",
                Parameters = { { Name = "other", Type = "Vector2" } },
                Returns = { "boolean" },
                Doc = "Compares two vectors exactly, component by component, with no tolerance.",
            },
        } :: { Schema.Operator },
    },
    {
        Name = "UDim",
        Module = "script",
        Doc = "One axis of a UI position or size: a fraction of the parent (`Scale`) plus a number of pixels "
            .. '(`Offset`). "Half the parent minus eight pixels" is `UDim.new(0.5, -8)`, and it stays correct '
            .. "when the window is resized. It supports `+`, `-` and `==`.",
        Constructors = {
            {
                Name = "new",
                Parameters = {
                    { Name = "scale", Type = "number?", Default = "0" },
                    { Name = "offset", Type = "number?", Default = "0" },
                },
                Returns = { "UDim" },
                ThreadSafety = "Safe",
                Doc = "Returns a `UDim` with the given scale and offset. Each defaults to 0.",
            },
        } :: { Schema.Method },
        Properties = {
            {
                Name = "Scale",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The fraction of the parent's size along the axis. 1 is the whole parent. It is not "
                    .. "clamped, so values above 1 or below 0 are valid.",
            },
            {
                Name = "Offset",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The number of pixels added after the scale is applied.",
            },
        } :: { Schema.Property },
        Operators = {
            {
                Name = "__add",
                Parameters = { { Name = "other", Type = "UDim" } },
                Returns = { "UDim" },
                Doc = "Adds the scales and the offsets.",
            },
            {
                Name = "__sub",
                Parameters = { { Name = "other", Type = "UDim" } },
                Returns = { "UDim" },
                Doc = "Subtracts the scales and the offsets.",
            },
            {
                Name = "__eq",
                Parameters = { { Name = "other", Type = "UDim" } },
                Returns = { "boolean" },
                Doc = "Compares the scale and the offset exactly.",
            },
        } :: { Schema.Operator },
    },
    {
        Name = "UDim2",
        Module = "script",
        Doc = "A UI position or size on two axes: one `UDim` for X and one for Y. The `Position` and `Size` of "
            .. "every `UIObject` are `UDim2` values.",
        Constructors = {
            {
                Name = "new",
                Parameters = {
                    { Name = "xs", Type = "number?", Default = "0" },
                    { Name = "xo", Type = "number?", Default = "0" },
                    { Name = "ys", Type = "number?", Default = "0" },
                    { Name = "yo", Type = "number?", Default = "0" },
                },
                Returns = { "UDim2" },
                ThreadSafety = "Safe",
                Doc = "Returns a `UDim2` from four numbers: the X scale, the X offset, the Y scale and the Y offset.",
            },
            {
                Name = "fromScale",
                Parameters = {
                    { Name = "xs", Type = "number?", Default = "0" },
                    { Name = "ys", Type = "number?", Default = "0" },
                },
                Returns = { "UDim2" },
                ThreadSafety = "Safe",
                Doc = "Returns a `UDim2` with the given scales and zero offsets, for a size or position relative "
                    .. "to the parent.",
            },
            {
                Name = "fromOffset",
                Parameters = {
                    { Name = "xo", Type = "number?", Default = "0" },
                    { Name = "yo", Type = "number?", Default = "0" },
                },
                Returns = { "UDim2" },
                ThreadSafety = "Safe",
                Doc = "Returns a `UDim2` with the given offsets in pixels and zero scales, for a fixed size or "
                    .. "position.",
            },
        } :: { Schema.Method },
        Properties = {
            {
                Name = "X",
                Type = "UDim",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The `UDim` of the horizontal axis.",
            },
            {
                Name = "Y",
                Type = "UDim",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The `UDim` of the vertical axis.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Lerp",
                Parameters = {
                    { Name = "goal", Type = "UDim2" },
                    { Name = "alpha", Type = "number" },
                },
                Returns = { "UDim2" },
                ThreadSafety = "Safe",
                Doc = "Returns the interpolation toward `goal`, with each of the four numbers interpolated "
                    .. "independently. A tween of a `Position` or `Size` computes the same values.",
            },
        } :: { Schema.Method },
        Operators = {
            {
                Name = "__add",
                Parameters = { { Name = "other", Type = "UDim2" } },
                Returns = { "UDim2" },
                Doc = "Adds both axes.",
            },
            {
                Name = "__sub",
                Parameters = { { Name = "other", Type = "UDim2" } },
                Returns = { "UDim2" },
                Doc = "Subtracts both axes.",
            },
            {
                Name = "__eq",
                Parameters = { { Name = "other", Type = "UDim2" } },
                Returns = { "boolean" },
                Doc = "Compares all four numbers exactly.",
            },
        } :: { Schema.Operator },
    },
    {
        Name = "Rect",
        Module = "script",
        Doc = "A rectangle on screen, given by two corners in pixels. The corners are not sorted or checked, so a "
            .. "rectangle can be inverted.",
        Constructors = {
            {
                Name = "new",
                Parameters = {
                    { Name = "min", Type = "Vector2" },
                    { Name = "max", Type = "Vector2" },
                },
                Returns = { "Rect" },
                ThreadSafety = "Safe",
                Doc = "Returns a rectangle from its two corners.",
            },
        } :: { Schema.Method },
        Properties = {
            {
                Name = "Min",
                Type = "Vector2",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The first corner.",
            },
            {
                Name = "Max",
                Type = "Vector2",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The second corner.",
            },
            {
                Name = "Width",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The width, `Max.X - Min.X`. It is negative for an inverted rectangle.",
            },
            {
                Name = "Height",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The height, `Max.Y - Min.Y`. It is negative for an inverted rectangle.",
            },
        } :: { Schema.Property },
        Operators = {
            {
                Name = "__eq",
                Parameters = { { Name = "other", Type = "Rect" } },
                Returns = { "boolean" },
                Doc = "Compares both corners exactly.",
            },
        } :: { Schema.Operator },
    },
    -- ADR 0110: the two sequences a `UIGradient` is made of, and their stops.
    {
        Name = "ColorSequenceKeypoint",
        Module = "script",
        Doc = "One stop of a `ColorSequence`: a color at a time from 0 to 1. It is immutable, and `==` compares "
            .. "exactly.",
        Constructors = {
            {
                Name = "new",
                Parameters = {
                    { Name = "time", Type = "number" },
                    { Name = "color", Type = "Color3" },
                },
                Returns = { "ColorSequenceKeypoint" },
                ThreadSafety = "Safe",
                Doc = "Returns a stop at `time`, which must be from 0 to 1.",
            },
        } :: { Schema.Method },
        Properties = {
            {
                Name = "Time",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The position of the stop along the sequence, from 0 to 1.",
            },
            {
                Name = "Value",
                Type = "Color3",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The color at that position.",
            },
        } :: { Schema.Property },
        Operators = {
            {
                Name = "__eq",
                Parameters = { { Name = "other", Type = "ColorSequenceKeypoint" } },
                Returns = { "boolean" },
                Doc = "Compares the time and the color exactly.",
            },
        } :: { Schema.Operator },
    },
    {
        Name = "ColorSequence",
        Module = "script",
        Records = { "ADR 0110" },
        Doc = "A gradient of colors over the range 0 to 1, made of 2 to 20 stops. The first stop is at 0 and the "
            .. "last at 1, and the color between two stops is interpolated linearly. `UIGradient.Color` is one. "
            .. "It is immutable, and `==` compares every stop.",
        Constructors = {
            {
                Name = "new",
                Parameters = { { Name = "color", Type = "Color3" } },
                Returns = { "ColorSequence" },
                ThreadSafety = "Safe",
                Doc = "Returns a sequence of one color from start to end.",
            },
            {
                Name = "new",
                Parameters = {
                    { Name = "from", Type = "Color3" },
                    { Name = "to", Type = "Color3" },
                },
                Returns = { "ColorSequence" },
                ThreadSafety = "Safe",
                Doc = "Returns a sequence that blends from `from` at 0 to `to` at 1.",
            },
            {
                Name = "new",
                Parameters = { { Name = "keypoints", Type = "{ ColorSequenceKeypoint }" } },
                Returns = { "ColorSequence" },
                ThreadSafety = "Safe",
                Doc = "Returns a sequence from a list of 2 to 20 stops. Their times must increase, the first must "
                    .. "be exactly 0, and the last exactly 1. Two stops at the same time make a hard edge. Raises "
                    .. "an error for any other list.",
            },
        } :: { Schema.Method },
        Properties = {
            {
                Name = "Keypoints",
                Type = "{ ColorSequenceKeypoint }",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The stops, in order of time. Each read returns a new table.",
            },
        } :: { Schema.Property },
        Operators = {
            {
                Name = "__eq",
                Parameters = { { Name = "other", Type = "ColorSequence" } },
                Returns = { "boolean" },
                Doc = "Compares every stop exactly.",
            },
        } :: { Schema.Operator },
    },
    {
        Name = "NumberSequenceKeypoint",
        Module = "script",
        Doc = "One stop of a `NumberSequence`: a number at a time from 0 to 1, with an envelope around it. It is "
            .. "immutable, and `==` compares exactly.",
        Constructors = {
            {
                Name = "new",
                Parameters = {
                    { Name = "time", Type = "number" },
                    { Name = "value", Type = "number" },
                    { Name = "envelope", Type = "number?", Default = "0" },
                },
                Returns = { "NumberSequenceKeypoint" },
                ThreadSafety = "Safe",
                Doc = "Returns a stop at `time`, which must be from 0 to 1.",
            },
        } :: { Schema.Method },
        Properties = {
            {
                Name = "Time",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The position of the stop along the sequence, from 0 to 1.",
            },
            {
                Name = "Value",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The number at that position.",
            },
            {
                Name = "Envelope",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "How far a value taken from the sequence can vary from `Value` at this stop. It is stored "
                    .. "with the stop; UI gradients do not use it.",
            },
        } :: { Schema.Property },
        Operators = {
            {
                Name = "__eq",
                Parameters = { { Name = "other", Type = "NumberSequenceKeypoint" } },
                Returns = { "boolean" },
                Doc = "Compares all three numbers exactly.",
            },
        } :: { Schema.Operator },
    },
    {
        Name = "NumberSequence",
        Module = "script",
        Records = { "ADR 0110" },
        Doc = "A series of numbers over the range 0 to 1, made of 2 to 20 stops. The first stop is at 0 and the "
            .. "last at 1, and the value between two stops is interpolated linearly. `UIGradient.Transparency` is "
            .. "one. It is immutable, and `==` compares every stop.",
        Constructors = {
            {
                Name = "new",
                Parameters = { { Name = "value", Type = "number" } },
                Returns = { "NumberSequence" },
                ThreadSafety = "Safe",
                Doc = "Returns a sequence of one number from start to end.",
            },
            {
                Name = "new",
                Parameters = {
                    { Name = "from", Type = "number" },
                    { Name = "to", Type = "number" },
                },
                Returns = { "NumberSequence" },
                ThreadSafety = "Safe",
                Doc = "Returns a sequence that blends from `from` at 0 to `to` at 1.",
            },
            {
                Name = "new",
                Parameters = { { Name = "keypoints", Type = "{ NumberSequenceKeypoint }" } },
                Returns = { "NumberSequence" },
                ThreadSafety = "Safe",
                Doc = "Returns a sequence from a list of 2 to 20 stops. Their times must increase, the first must "
                    .. "be exactly 0, and the last exactly 1. Raises an error for any other list.",
            },
        } :: { Schema.Method },
        Properties = {
            {
                Name = "Keypoints",
                Type = "{ NumberSequenceKeypoint }",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The stops, in order of time. Each read returns a new table.",
            },
        } :: { Schema.Property },
        Operators = {
            {
                Name = "__eq",
                Parameters = { { Name = "other", Type = "NumberSequence" } },
                Returns = { "boolean" },
                Doc = "Compares every stop exactly.",
            },
        } :: { Schema.Operator },
    },
    {
        Name = "Random",
        Module = "script",
        Records = { "ADR 0025" },
        Doc = "A seeded pseudorandom number generator. Use it for every random number in simulation code. The "
            .. "same seed gives the same sequence on the same engine build and platform, which is what makes a "
            .. "replay reproduce a run: a replay stores seeds, not results.",
        Constructors = {
            {
                Name = "new",
                Parameters = { { Name = "seed", Type = "number?", Default = "nil" } },
                Returns = { "Random" },
                ThreadSafety = "Safe",
                Doc = "Returns a generator seeded with `seed`, truncated to an integer. With no seed, a seed is "
                    .. "taken from the system and the sequence is different on every run. Do not use an unseeded "
                    .. "generator in simulation code; `ludwerk check` reports it. Use one only for effects that "
                    .. "do not affect gameplay.",
            },
        } :: { Schema.Method },
        Methods = {
            {
                Name = "NextNumber",
                Parameters = {
                    { Name = "min", Type = "number?", Default = "0" },
                    { Name = "max", Type = "number?", Default = "1" },
                },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the next number from `min` up to but not including `max`. The defaults are 0 and "
                    .. "1. Raises an error if `min` is greater than `max`.",
            },
            {
                Name = "NextInteger",
                Parameters = { { Name = "min", Type = "number" }, { Name = "max", Type = "number" } },
                Returns = { "number" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the next integer from `min` to `max`, both included. Raises an error if `min` is "
                    .. "greater than `max`, or if either is not an integer.",
            },
            {
                Name = "NextUnitVector",
                Parameters = {},
                Returns = { "vector" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a random direction of length 1, uniformly distributed over the sphere.",
            },
            {
                Name = "Clone",
                Parameters = {},
                Returns = { "Random" },
                ThreadSafety = "ReadParallel",
                Doc = "Returns a copy of the generator at the same point in its sequence. The copy and the "
                    .. "original then continue independently. Use it to give a subsystem its own deterministic "
                    .. "stream.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "RaycastParams",
        Module = "script",
        Doc = "Sets what a raycast or other spatial query can hit. Every query on `Workspace` accepts one; "
            .. "without it, the query can hit everything that has `CanQuery` on. It is created from a table of "
            .. "options.",
        Constructors = {
            {
                Name = "new",
                Parameters = {
                    {
                        Name = "options",
                        Type = "{ Filter: { Instance }?, FilterType: Enum.RaycastFilterType?, CollisionGroup: string? }?",
                        Default = "nil",
                    },
                },
                Returns = { "RaycastParams" },
                ThreadSafety = "Safe",
                Doc = "Returns params from a table of options. `Filter` defaults to an empty list, `FilterType` "
                    .. "to `Exclude` and `CollisionGroup` to an empty string, which means every group. An empty "
                    .. "`Exclude` filter hits everything, and an empty `Include` filter hits nothing.",
            },
        } :: { Schema.Method },
        Properties = {
            {
                Name = "Filter",
                Type = "{ Instance }",
                ReadOnly = true,
                Doc = "The instances the filter lists. Each read returns a new array. The descendants of a listed "
                    .. "instance are included, so listing a `Model` covers its parts.",
            },
            {
                Name = "FilterType",
                Type = "Enum.RaycastFilterType",
                ReadOnly = true,
                Doc = "Whether `Filter` lists the only instances that can be hit (`Include`) or the instances to "
                    .. "ignore (`Exclude`).",
            },
            {
                Name = "CollisionGroup",
                Type = "string",
                ReadOnly = true,
                Doc = "The collision group a hit must belong to, or an empty string for any group. Restricting a "
                    .. "query to one group is cheaper than hitting everything and filtering the results.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "RaycastResult",
        Module = "script",
        Doc = "The result of a raycast: what was hit and where. `Workspace:Raycast` and `Workspace:Spherecast` "
            .. "return one, or nil when nothing was hit.",
        Properties = {
            {
                Name = "Instance",
                Type = "BasePart",
                ReadOnly = true,
                Doc = "The part that was hit.",
            },
            {
                Name = "Position",
                Type = "vector",
                ReadOnly = true,
                Doc = "The position of the hit, in world space.",
            },
            {
                Name = "Normal",
                Type = "vector",
                ReadOnly = true,
                Doc = "The surface normal at the hit, pointing out of the surface.",
            },
            {
                Name = "Distance",
                Type = "number",
                ReadOnly = true,
                Doc = "The distance from the origin of the ray to the hit, in meters.",
            },
            {
                Name = "Material",
                Type = "Material?",
                ReadOnly = true,
                Doc = "The material of the surface at the hit: the part's material, or nil if the part has none. "
                    .. "On a `Terrain`, it is the layer drawn at that point, including painted materials and "
                    .. "rules, so snow that a rule draws over rock above a certain height is reported as snow. "
                    .. "Read its `Friction`, `FootstepSound` and `Tags` to choose a footstep, a skid or an impact "
                    .. "effect.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "RaycastResult2D",
        Module = "script",
        Doc = "The result of `Workspace:Raycast2D`: what was hit on the 2D plane and where. The method returns "
            .. "nil when nothing was hit.",
        Properties = {
            {
                Name = "Instance",
                Type = "Instance",
                ReadOnly = true,
                Doc = "The `Part2D` or `Tilemap2D` that was hit.",
            },
            {
                Name = "Position",
                Type = "Vector2",
                ReadOnly = true,
                Doc = "The position of the hit, in meters on the 2D plane.",
            },
            {
                Name = "Normal",
                Type = "Vector2",
                ReadOnly = true,
                Doc = "The surface normal at the hit, pointing out of the surface.",
            },
            {
                Name = "Distance",
                Type = "number",
                ReadOnly = true,
                Doc = "The distance from the origin of the ray to the hit, in meters.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "Signal",
        Module = "script",
        Records = { "ADR 0015" },
        Doc = "An event that handlers can connect to. Its type is written `Signal<T...>`, where `T...` is the "
            .. "list of values that handlers receive and `Wait` returns. A script creates its own with "
            .. "`Signal.new`.\n\n"
            .. "Signals are deferred: firing one queues its handlers, and they run at the next resumption point, "
            .. "in the order the signals were fired. Within one fire, handlers run in the order they were "
            .. "connected.\n\n"
            .. "Each handler runs in its own coroutine. An error in one handler does not stop the other handlers "
            .. "or the script that fired the signal.",
        Constructors = {
            {
                Name = "new",
                Parameters = {},
                Returns = { "Signal<T...>" },
                ThreadSafety = "Safe",
                Doc = "Returns a new signal owned by the script. The argument types are inferred from the calls "
                    .. "to `Fire` and `Connect`; annotate the type, as in `Signal<string>` or `Signal<()>`, where "
                    .. "they cannot be inferred.",
            },
        } :: { Schema.Method },
        Methods = {
            {
                Name = "Connect",
                Parameters = { { Name = "handler", Type = "(T...) -> ()" } },
                Returns = { "Connection" },
                ThreadSafety = "Unsafe",
                Doc = "Connects `handler` to the signal and returns a `Connection` that disconnects it. The "
                    .. "handlers of one fire run in the order they were connected. Do not rely on the order of "
                    .. "handlers between different signals.",
            },
            {
                Name = "Once",
                Parameters = { { Name = "handler", Type = "(T...) -> ()" } },
                Returns = { "Connection" },
                ThreadSafety = "Unsafe",
                Doc = "Connects `handler` for one call only: it is disconnected as it is called. If the signal "
                    .. "fires twice before the handlers run, the handler still runs once. A handler that raises "
                    .. "an error is still disconnected.",
            },
            {
                Name = "Wait",
                Parameters = {},
                Returns = { "T..." },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Yields the calling coroutine until the signal next fires, and returns the arguments of "
                    .. "that fire. It counts as a connection made at the moment of the call, so it resumes in "
                    .. "connection order among the handlers.",
            },
            {
                Name = "Fire",
                Parameters = { { Name = "values", Type = "T..." } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Queues a call to every connected handler with the given arguments. Nothing runs until the "
                    .. "next resumption point. Tables are passed by reference, so a change made to one before the "
                    .. "handlers run is visible to them.",
            },
            {
                Name = "Destroy",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Closes the signal. Fires that are already queued call no handlers, and every `Connection` "
                    .. "reports `Connected` as false. Only a signal that a script created can be destroyed; an "
                    .. "instance's signals are closed by `Instance:Destroy`.",
            },
            {
                Name = "DisconnectAll",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Disconnects every handler and keeps the signal usable: it can be connected to and fired "
                    .. "again. It works only on a signal that a script created, not on an instance's event.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "Connection",
        Module = "script",
        Doc = "The handle that `Signal:Connect` and `Signal:Once` return. Use it to disconnect the handler. It "
            .. "cannot be created directly.",
        Properties = {
            {
                Name = "Connected",
                Type = "boolean",
                ReadOnly = true,
                Transient = true,
                ThreadSafety = "ReadParallel",
                Doc = "Whether the handler is still connected. It is false after `Disconnect`, after the signal "
                    .. "is destroyed, and after a `Once` handler has run.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Disconnect",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Disconnects the handler. Calling it again does nothing. A fire that was queued before the "
                    .. "call can still run the handler once.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "Collector",
        Module = "script",
        Records = { "ADR 0094" },
        Doc = "Collects things to clean up together, such as connections, instances, threads and functions. "
            .. "`Cleanup` cleans each one in the way its type requires, newest first.",
        Constructors = {
            {
                Name = "new",
                Parameters = {},
                Returns = { "Collector" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a new, empty `Collector`.",
            },
        } :: { Schema.Method },
        Methods = {
            {
                Name = "Add",
                Parameters = {
                    { Name = "object", Type = "any" },
                    { Name = "method", Type = "(string | boolean)?", Default = "nil" },
                    { Name = "key", Type = "any?", Default = "nil" },
                },
                Returns = { "any" },
                ThreadSafety = "Unsafe",
                Doc = "Adds `object` to be cleaned up later, and returns it. How it is cleaned depends on its "
                    .. "type: a function is called, a thread is canceled, a `Connection` is disconnected, a "
                    .. "`Promise` is canceled, and an `Instance`, a `Signal` or any object with a `Destroy` "
                    .. "method is destroyed.\n\n"
                    .. "`method` names a method to call instead, or true to call the object itself. `key` gives "
                    .. "the object a name for `Remove` and `Get`. Adding under a key that is already in use "
                    .. "cleans the object that was there first.",
            },
            {
                Name = "AddPromise",
                Parameters = { { Name = "promise", Type = "Promise" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Adds a promise to be canceled on cleanup, and returns it. If the promise settles by itself "
                    .. "first, it is removed from the collector.",
            },
            {
                Name = "Remove",
                Parameters = { { Name = "key", Type = "any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Cleans the object stored under `key` immediately and removes it from the collector.",
            },
            {
                Name = "RemoveNoClean",
                Parameters = { { Name = "key", Type = "any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes the object stored under `key` from the collector without cleaning it.",
            },
            {
                Name = "Get",
                Parameters = { { Name = "key", Type = "any" } },
                Returns = { "any" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the object stored under `key`, or nil.",
            },
            {
                Name = "Cleanup",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Cleans every object, newest first. The collector stays usable afterward.",
            },
            {
                Name = "Destroy",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Cleans every object. Adding to the collector afterward raises an error.",
            },
            {
                Name = "LinkToInstance",
                Parameters = {
                    { Name = "instance", Type = "Instance" },
                    { Name = "allowMultiple", Type = "boolean?", Default = "nil" },
                },
                Returns = { "Connection" },
                ThreadSafety = "Unsafe",
                Doc = "Cleans the collector when `instance` is destroyed, and returns the connection that does "
                    .. "so. A new link replaces the previous one unless `allowMultiple` is true.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "Promise",
        Module = "script",
        Records = { "ADR 0094" },
        Doc = "A value that becomes available later. A promise is resolved with values or rejected with an error, "
            .. "and code attaches handlers for either outcome.\n\n"
            .. "The executor of a new promise runs immediately, in its own thread. Resolving a promise with "
            .. "another promise adopts that promise's outcome. A handler that raises an error rejects the promise "
            .. "it feeds. Canceling a promise cancels every promise that depends on it. A rejection that no "
            .. "handler receives is reported as a warning. Delays and timeouts use simulation time.",
        Constructors = {
            {
                Name = "new",
                Parameters = {
                    {
                        Name = "executor",
                        Type = "(resolve: (...any) -> (), reject: (...any) -> (), onCancel: (hook: (() -> ())?) -> boolean) -> ()",
                    },
                },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that runs `executor(resolve, reject, onCancel)` immediately, in its own "
                    .. "thread.",
            },
            {
                Name = "defer",
                Parameters = {
                    {
                        Name = "executor",
                        Type = "(resolve: (...any) -> (), reject: (...any) -> (), onCancel: (hook: (() -> ())?) -> boolean) -> ()",
                    },
                },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise like `new`, except that the executor runs at the next resumption point "
                    .. "instead of immediately.",
            },
            {
                Name = "resolve",
                Parameters = { { Name = "values", Type = "...any" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that is already resolved with the given values.",
            },
            {
                Name = "reject",
                Parameters = { { Name = "values", Type = "...any" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that is already rejected with the given values.",
            },
            {
                Name = "try",
                Parameters = { { Name = "fn", Type = "(...any) -> ...any" }, { Name = "values", Type = "...any" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise for the call `fn(...)`. What the function returns resolves the promise, "
                    .. "and an error it raises rejects it.",
            },
            {
                Name = "delay",
                Parameters = { { Name = "seconds", Type = "number" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that resolves after `seconds` of simulation time, with `seconds` as its "
                    .. "value.",
            },
            {
                Name = "all",
                Parameters = { { Name = "promises", Type = "{ Promise }" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that resolves with every value, in order, when all the promises resolve. "
                    .. "It rejects with the first rejection and cancels the others.",
            },
            {
                Name = "allSettled",
                Parameters = { { Name = "promises", Type = "{ Promise }" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that resolves when every promise has settled, with the final status of each.",
            },
            {
                Name = "race",
                Parameters = { { Name = "promises", Type = "{ Promise }" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that settles the way the first of the promises to settle does, and "
                    .. "cancels the others.",
            },
            {
                Name = "any",
                Parameters = { { Name = "promises", Type = "{ Promise }" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that resolves with the value of the first promise to resolve. It rejects "
                    .. "if all of them reject.",
            },
            {
                Name = "some",
                Parameters = { { Name = "promises", Type = "{ Promise }" }, { Name = "count", Type = "number" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that resolves with the values of the first `count` promises to resolve. "
                    .. "It rejects as soon as that many can no longer resolve.",
            },
            {
                Name = "fromEvent",
                Parameters = {
                    { Name = "event", Type = "Signal<...any>" },
                    { Name = "predicate", Type = "((...any) -> boolean)?", Default = "nil" },
                },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that resolves with the arguments of the next fire of a signal that "
                    .. "`predicate` accepts, and then disconnects from the signal.",
            },
            {
                Name = "retry",
                Parameters = {
                    { Name = "fn", Type = "(...any) -> ...any" },
                    { Name = "times", Type = "number" },
                    { Name = "values", Type = "...any" },
                },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise for `fn(...)`, like `try`, that calls the function again up to `times` "
                    .. "more times while it rejects.",
            },
            {
                Name = "retryWithDelay",
                Parameters = {
                    { Name = "fn", Type = "(...any) -> ...any" },
                    { Name = "times", Type = "number" },
                    { Name = "seconds", Type = "number" },
                    { Name = "values", Type = "...any" },
                },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise like `retry`, with `seconds` of simulation time before each new attempt.",
            },
            {
                Name = "is",
                Parameters = { { Name = "value", Type = "any" } },
                Returns = { "boolean" },
                ThreadSafety = "Unsafe",
                Doc = "Returns whether `value` is a `Promise`.",
            },
            {
                Name = "promisify",
                Parameters = { { Name = "fn", Type = "(...any) -> ...any" } },
                Returns = { "(...any) -> Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a function that calls `fn` and returns a promise for its result.",
            },
        } :: { Schema.Method },
        Methods = {
            {
                Name = "AndThen",
                Parameters = {
                    { Name = "onResolved", Type = "((...any) -> ...any)?", Default = "nil" },
                    { Name = "onRejected", Type = "((...any) -> ...any)?", Default = "nil" },
                },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise for the result of `onResolved`, or of `onRejected` if this promise "
                    .. "rejects. A handler that raises an error rejects the new promise. If a handler returns a "
                    .. "promise, the new promise waits for it.",
            },
            {
                Name = "Catch",
                Parameters = { { Name = "onRejected", Type = "(...any) -> ...any" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise for the result of `onRejected` if this promise rejects. It is the same "
                    .. "as `AndThen(nil, onRejected)`.",
            },
            {
                Name = "Finally",
                Parameters = { { Name = "fn", Type = "(status: EnumPromiseState) -> ...any" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Calls `fn` with the final status when this promise settles in any way. Returns a promise "
                    .. "that settles the way this one did.",
            },
            {
                Name = "Tap",
                Parameters = { { Name = "fn", Type = "(...any) -> ...any" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Calls `fn` with the resolved values and returns a promise that resolves with those same "
                    .. "values, whatever `fn` returns.",
            },
            {
                Name = "AndThenCall",
                Parameters = { { Name = "fn", Type = "(...any) -> ...any" }, { Name = "values", Type = "...any" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that, when this one resolves, calls `fn` with the given arguments "
                    .. "instead of the resolved values.",
            },
            {
                Name = "AndThenReturn",
                Parameters = { { Name = "values", Type = "...any" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that, when this one resolves, resolves with the given values instead.",
            },
            {
                Name = "Await",
                Parameters = {},
                Returns = { "boolean", "...any" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Yields until the promise settles. Returns true and the resolved values, or false and the "
                    .. "rejection. A canceled promise returns false.",
            },
            {
                Name = "AwaitStatus",
                Parameters = {},
                Returns = { "EnumPromiseState", "...any" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Yields until the promise settles, like `Await`, and returns the status in place of the boolean.",
            },
            {
                Name = "ExpectAsync",
                Parameters = {},
                Returns = { "...any" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Yields until the promise settles and returns the resolved values. Raises an error with the "
                    .. "rejection if the promise rejects.",
            },
            {
                Name = "Now",
                Parameters = { { Name = "rejection", Type = "any?", Default = "nil" } },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that passes this one's values on if it is already resolved, and "
                    .. "otherwise rejects immediately with `rejection`.",
            },
            {
                Name = "Cancel",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Cancels the promise. The executor stops, its cancel hook runs, and every promise that "
                    .. "depends on this one is canceled. The promise it depends on is also canceled once nothing "
                    .. "else depends on it.",
            },
            {
                Name = "GetStatus",
                Parameters = {},
                Returns = { "EnumPromiseState" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the status of the promise: `Enum.PromiseState.Started`, `Resolved`, `Rejected` or "
                    .. "`Canceled`.",
            },
            {
                Name = "Timeout",
                Parameters = {
                    { Name = "seconds", Type = "number" },
                    { Name = "rejection", Type = "any?", Default = "nil" },
                },
                Returns = { "Promise" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a promise that rejects with `rejection` if this one does not settle within "
                    .. "`seconds` of simulation time. This promise is canceled when the time runs out.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "TweenInfo",
        Module = "script",
        Records = { "ADR 0039" },
        Doc = "Describes how a tween moves: its duration, its easing curve, how many times it repeats and its "
            .. "delay. It is immutable.\n\n"
            .. "Times are in seconds of simulation time, the same seconds `task.wait` counts, so a tween lasts a "
            .. "whole number of ticks and a replay reproduces it exactly.",
        Constructors = {
            {
                Name = "new",
                Parameters = {
                    { Name = "time", Type = "number?", Default = "1" },
                    { Name = "easingStyle", Type = "Enum.EasingStyle?", Default = "Enum.EasingStyle.Quad" },
                    {
                        Name = "easingDirection",
                        Type = "Enum.EasingDirection?",
                        Default = "Enum.EasingDirection.Out",
                    },
                    { Name = "repeatCount", Type = "number?", Default = "0" },
                    { Name = "reverses", Type = "boolean?", Default = "false" },
                    { Name = "delayTime", Type = "number?", Default = "0" },
                },
                Returns = { "TweenInfo" },
                ThreadSafety = "Safe",
                Doc = "Returns a `TweenInfo`. The defaults are one second, `Quad`, `Out`, no repeat, no reverse "
                    .. "and no delay. Raises an error for a negative `time` or `delayTime`. A negative "
                    .. "`repeatCount` repeats forever.",
            },
        } :: { Schema.Method },
        Properties = {
            {
                Name = "Time",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The duration of one pass, in seconds, not counting the delay, the repeats or the return "
                    .. "pass of a reversing tween.",
            },
            {
                Name = "EasingStyle",
                Type = "Enum.EasingStyle",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The easing curve.",
            },
            {
                Name = "EasingDirection",
                Type = "Enum.EasingDirection",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "Which end of the pass the easing applies to.",
            },
            {
                Name = "RepeatCount",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The number of extra passes after the first. 0 plays once and 2 plays three times. A "
                    .. "negative number repeats until the tween is canceled.",
            },
            {
                Name = "Reverses",
                Type = "boolean",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "Whether each pass is followed by the same pass backward. A reversing tween takes twice "
                    .. "`Time` per repeat and ends where it started.",
            },
            {
                Name = "DelayTime",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The time before the first pass and before each repeat, in seconds. During the delay, the "
                    .. "property is not changed.",
            },
        } :: { Schema.Property },
        Operators = {
            {
                Name = "__eq",
                Parameters = { { Name = "other", Type = "TweenInfo" } },
                Returns = { "boolean" },
                Doc = "Compares two `TweenInfo` values exactly, field by field.",
            },
        } :: { Schema.Operator },
    },
    {
        Name = "Tween",
        Module = "script",
        Doc = "A property animation, returned by `TweenService:Create`. It is a value, not an instance: it has no "
            .. "parent and is not in the tree.\n\n"
            .. "A tween sets properties the way a script does. Each change fires `GetPropertyChangedSignal`, and "
            .. "a value that is out of range is refused, as for an assignment. It advances on simulation ticks, "
            .. "so it is reproducible.\n\n"
            .. "A tween does not keep its instance alive. If the instance is destroyed, the tween stops and "
            .. "reports `Canceled`.",
        Properties = {
            {
                Name = "Instance",
                Type = "Instance?",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The instance the tween animates, or nil after that instance is destroyed.",
            },
            {
                Name = "TweenInfo",
                Type = "TweenInfo",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The `TweenInfo` the tween was created with.",
            },
            {
                Name = "PlaybackState",
                Type = "Enum.PlaybackState",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The current state of the tween.",
            },
            {
                Name = "Completed",
                Type = "Signal<EnumPlaybackState>",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "A signal that fires once when the tween ends. Passes the final state: `Completed` if the "
                    .. "tween reached its end, or `Canceled` if it was stopped early. Check the state before "
                    .. "acting on a finished animation.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Play",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Starts the tween, or resumes a paused tween from where it stopped. Does nothing if the "
                    .. "tween is already playing. To restart an animation, create a new tween.",
            },
            {
                Name = "Pause",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Pauses the tween and leaves the property at its current value. `Play` resumes it.",
            },
            {
                Name = "Cancel",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops the tween before its end. The property keeps the value it had reached. `Completed` "
                    .. "fires with `Canceled`.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "DataRecord",
        Module = "script",
        Records = { "ADR 0238" },
        Doc = "A player's record in an online game's database, returned by `DataService:LoadAsync` and held by "
            .. "this world. It holds values under string keys, as a `SaveSlot` does, and raises the same errors "
            .. "for a value it cannot hold. Once it is let go, by `Release` or as its player leaves, every member "
            .. "but `Released` raises an error.",
        Properties = {
            {
                Name = "Released",
                Type = "boolean",
                ReadOnly = true,
                Doc = "Whether this world has let the record go.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Get",
                Parameters = { { Name = "key", Type = "string" } },
                Returns = { "any" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a copy of the value stored under `key`, or nil.",
            },
            {
                Name = "Set",
                Parameters = { { Name = "key", Type = "string" }, { Name = "value", Type = "any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stores a copy of `value` under `key`. A value of nil removes the key. Raises an error for "
                    .. "a value that cannot be saved, and for one that would make the record larger than 4 MB.",
            },
            {
                Name = "Update",
                Parameters = { { Name = "key", Type = "string" }, { Name = "transform", Type = "(any) -> any" } },
                Returns = { "any" },
                ThreadSafety = "Unsafe",
                Doc = "Calls `transform` with the value stored under `key`, stores what it returns, and returns "
                    .. "that value. The function must not yield.",
            },
            {
                Name = "Remove",
                Parameters = { { Name = "key", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes `key` and its value.",
            },
            {
                Name = "GetKeys",
                Parameters = {},
                Returns = { "{ string }" },
                ThreadSafety = "Unsafe",
                Doc = "Returns every key, sorted.",
            },
            {
                Name = "Save",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Writes the record now, without waiting for the next write behind. It does not wait for "
                    .. "the database.",
            },
            {
                Name = "Release",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Writes the record and lets it go, so another world can load it. Doing it again does "
                    .. "nothing.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "SaveSlot",
        Module = "script",
        Records = { "ADR 0111" },
        Doc = "One named save of a game, returned by `SaveService:GetSlotAsync`. It holds values under string "
            .. "keys.\n\n"
            .. "A value can be anything an attribute can hold: a string, a number, a boolean, a vector, a "
            .. "`CFrame`, a `Color3`, a `Vector2`, a `UDim`, a `UDim2`, a `Rect` or a sequence. It can also be a "
            .. "table of such values, nested, with string keys or array indices. `Set` raises an error for a "
            .. "function, a thread, an instance or a table that contains itself.\n\n"
            .. "Changes are written to disk automatically: at most once per second while something has changed, "
            .. "when the game closes, and when the app goes to the background, because a phone can close a game "
            .. "in the background without warning. `SaveAsync` writes immediately and waits. `Get` returns a "
            .. "copy; changing the copy changes nothing until it is passed to `Set`.",
        Properties = {
            {
                Name = "Name",
                Type = "string",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The name of the slot.",
            },
            {
                Name = "Recovered",
                Type = "boolean",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "Whether the slot's data was read successfully. It is false when the slot's file and its "
                    .. "backup were both damaged and the slot started empty, so that the game can tell the "
                    .. "player. It is true otherwise, including for a slot that did not exist yet.",
            },
            {
                Name = "Changed",
                Type = "Signal<string>",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "A signal that fires when `Set`, `Update` or `Remove` changes a key. Passes the key.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Get",
                Parameters = { { Name = "key", Type = "string" } },
                Returns = { "any" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a copy of the value stored under `key`, or nil.",
            },
            {
                Name = "Set",
                Parameters = { { Name = "key", Type = "string" }, { Name = "value", Type = "any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stores a copy of `value` under `key`. A value of nil removes the key. Raises an error for "
                    .. "a value that cannot be saved, and for a value that would make the slot larger than the "
                    .. "project's `[save] max_slot_bytes`.",
            },
            {
                Name = "Update",
                Parameters = { { Name = "key", Type = "string" }, { Name = "transform", Type = "(any) -> any" } },
                Returns = { "any" },
                ThreadSafety = "Unsafe",
                Doc = "Calls `transform` with the value stored under `key`, stores what it returns, and returns "
                    .. "that value. The function must not yield.",
            },
            {
                Name = "Remove",
                Parameters = { { Name = "key", Type = "string" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Removes `key` and its value.",
            },
            {
                Name = "GetKeys",
                Parameters = {},
                Returns = { "{ string }" },
                ThreadSafety = "Unsafe",
                Doc = "Returns every key, sorted.",
            },
            {
                Name = "SaveAsync",
                Parameters = {},
                Returns = {},
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Writes the slot to disk immediately and yields until it is written. Raises an error if the "
                    .. "write fails.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "Scene",
        Module = "script",
        Records = { "ADR 0124" },
        Doc = "A scene of the game, such as a level, a menu or an arena. A scene stays open until "
            .. "`SceneService:LoadScene` changes to another.\n\n"
            .. "The global `scene` is available in every script, like `game`, and always refers to the scene that "
            .. "is open when it is read. `SceneService.CurrentScene` returns the same scene as an object that "
            .. "keeps referring to that scene.\n\n"
            .. "A `Scene` object kept after its scene closes is closed: `IsOpen` returns false, `BindToClose` and "
            .. "`BindToMessage` raise an error, and a message sent to it is dropped. Inside a `SubWorld`, `scene` "
            .. "is the sub-world's scene.",
        Properties = {
            {
                Name = "Name",
                Type = "string",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The name of the scene file without its folder or the `.scene.json` extension: `arena` for "
                    .. "`scenes/arena.scene.json`. It is empty for a world that no scene file created.",
            },
            {
                Name = "Path",
                Type = "string",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The path of the scene file relative to the content folder, such as `scenes/arena.scene.json`.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "IsOpen",
                Parameters = {},
                Returns = { "boolean" },
                ThreadSafety = "Safe",
                Doc = "Returns whether this scene is the one that is open. It is false after the scene has "
                    .. "closed, and for a scene that is still being prepared (see `SceneLoad.Scene`) until it "
                    .. "opens. `scene:IsOpen()` is always true.",
            },
            {
                Name = "BindToClose",
                Parameters = { { Name = "callback", Type = "() -> ()" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Registers a function to run when this scene closes: when `LoadScene` changes to another "
                    .. "scene, or when the game closes while this scene is open. Use it for what belongs to a "
                    .. "level, such as saving its progress. Raises an error on a scene that has closed.\n\n"
                    .. "The scene change waits for the functions. Each runs in its own thread and can yield, for "
                    .. "example in `SaveSlot:SaveAsync`, for up to the project's `[scene] close_grace_seconds` (5 "
                    .. "by default). Then saves are written and the scene closes. Functions run in the order they "
                    .. "were registered, and an error in one does not stop the others.\n\n"
                    .. "`game:BindToClose` is for the whole game. A function that a scene's script registers "
                    .. "there is dropped when its scene closes, with a warning in the editor.",
            },
            {
                Name = "SendMessage",
                Parameters = { { Name = "topic", Type = "string" }, { Name = "arguments", Type = "...any" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sends a message to this scene: every function bound to `topic` on it with `BindToMessage` "
                    .. "runs with the message's values. The functions run deferred, in the order the messages "
                    .. "were sent, each in its own thread. For example, a global script can tell the level "
                    .. '`scene:SendMessage("OpenGate", "north")`.\n\n'
                    .. "Values arrive the way an attribute or a save stores them: a table as a copy without its "
                    .. "metatable, a `buffer` as a copy, and an instance as the same instance. Raises an error "
                    .. "that names the place for a function, a thread, a table that contains itself, or a live "
                    .. "object such as a `Signal`, a `Connection` or a `Tween`.\n\n"
                    .. "A message to a closed scene is dropped. A message to a scene that is still being prepared "
                    .. "waits until its scripts have started. Messages never leave this machine; to send between "
                    .. "machines, use a `RemoteEvent`.",
            },
            {
                Name = "BindToMessage",
                Parameters = {
                    { Name = "topic", Type = "string" },
                    { Name = "callback", Type = "(...any) -> ()" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Binds a function that runs with a message's values each time a message is sent to this "
                    .. "scene under `topic`. The binding belongs to the script that made it and is removed with "
                    .. "that script's scene. A message with no bound function is dropped, with a warning in the "
                    .. "editor. Raises an error on a scene that has closed.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "SceneLoad",
        Module = "script",
        Records = { "ADR 0125" },
        Doc = "A scene that is being prepared while the current scene keeps running, returned by "
            .. "`SceneService:LoadSceneAsync`. Its file is read and parsed on another thread, and the meshes it "
            .. "uses are loaded.\n\n"
            .. "The switch happens when the game calls `Activate`, or immediately for a load started with "
            .. "`Activate = true`. It happens between two ticks, like `LoadScene`: `SceneLoading` fires, the old "
            .. "scene's `scene:BindToClose` functions run and are waited for, and the prepared scene opens.\n\n"
            .. "Only one load runs at a time: another `LoadSceneAsync` or a `LoadScene` cancels this one.",
        Properties = {
            {
                Name = "Scene",
                Type = "Scene",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The scene that is being prepared. A message sent to it waits until the scene opens and its "
                    .. "scripts have started, and is delivered before `SceneLoaded` fires.",
            },
            {
                Name = "Path",
                Type = "string",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The path of the scene file relative to the content folder.",
            },
            {
                Name = "Progress",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The progress of the load, from 0 to 1. It never decreases. The first half is reading and "
                    .. "parsing the file, and the second half is loading the meshes. It is 1 when `Ready` fires.",
            },
            {
                Name = "Status",
                Type = "Enum.SceneLoadStatus",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The stage of the load: `Preparing`, `Ready`, `Activating` and then `Done`, or `Failed` or "
                    .. "`Canceled`.",
            },
            {
                Name = "Error",
                Type = "string?",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The reason the load failed, when `Status` is `Failed`. It is nil otherwise.",
            },
            {
                Name = "Ready",
                Type = "Signal<>",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "A signal that fires once, when the scene is prepared and waiting to be activated.",
            },
            {
                Name = "Finished",
                Type = "Signal<boolean>",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "A signal that fires once, when the load ends. Passes true if the scene opened, and false "
                    .. "if the load failed or was canceled.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Activate",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Switches to the scene immediately if it is ready, or as soon as it is. Raises an error on "
                    .. "a load that failed or was canceled.",
            },
            {
                Name = "Cancel",
                Parameters = {},
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Discards the prepared scene. `Finished` fires with false. Does nothing after the switch "
                    .. "has started.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "InputObject",
        Module = "script",
        Records = { "ADR 0041" },
        Doc = "One input, as passed by `InputService.InputBegan`, `InputChanged` and `InputEnded`. It is a "
            .. "read-only snapshot of what happened on one tick, not a live object.\n\n"
            .. "These events are raised by the engine on simulation ticks, after the interface has consumed what "
            .. "it uses. In a replay they come from the recorded input, so a replay reproduces every input a game "
            .. "reads.",
        Properties = {
            {
                Name = "GamepadId",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The id of the gamepad that produced the input. Presses on different gamepads at the same "
                    .. "time keep their own ids. It is 0 for other devices. A gamepad that reconnects can get a "
                    .. "new id.",
            },
            {
                Name = "UserInputType",
                Type = "Enum.UserInputType",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The kind of input, for a handler that does not need to know which key it was.",
            },
            {
                Name = "KeyCode",
                Type = "Enum.KeyCode",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The key, button or axis. It is `Unknown` for pointer movement and the mouse wheel, which "
                    .. "are not keys.",
            },
            {
                Name = "Position",
                Type = "vector",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Records = { "ADR 0013" },
                Doc = "The position of the pointer, in window pixels measured from the top left, with the "
                    .. "accumulated wheel movement in `z`. For a gamepad axis it is the position of the axis "
                    .. "instead: `x` and `y` for a stick, and `x` for a trigger.",
            },
            {
                Name = "Delta",
                Type = "vector",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "How much the input moved since the last tick, in the same units as `Position`. `z` is the "
                    .. "movement of the mouse wheel. It is zero for an input that is not a movement.",
            },
            {
                Name = "TouchId",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The finger of a `Touch` input. The number is the same from the finger's `InputBegan` to "
                    .. "its `InputEnded`, so a handler can follow several fingers at once. Numbers start at 1 and "
                    .. "are reused after a finger lifts. It is 0 for an input that is not a touch.",
            },
        } :: { Schema.Property },
    },
    {
        Name = "IntentWriter",
        Module = "script",
        Doc = "The object a function bound with `RunService:BindToIntent` uses to set the current tick's intents. "
            .. "It is valid only during that call.",
        Methods = {
            {
                Name = "Set",
                Parameters = {
                    { Name = "action", Type = "string" },
                    { Name = "value", Type = "boolean | number | Vector2 | vector" },
                },
                Returns = {},
                Doc = "Sets the current tick's intent named `action`. A boolean is a button that is pressed or "
                    .. "not, a number is one axis, a `Vector2` is two axes and a `vector` is three. Raises an "
                    .. "error for a value that is not finite.",
            },
        },
    },
    {
        Name = "PredictedStep",
        Module = "script",
        Records = { "ADR 0157" },
        Doc = "One tick of one character, passed to a function bound with `RunService:BindToPredictedStep` or "
            .. "`BasePart:BindToPredictedTouch`. It is valid only during that call.",
        Properties = {
            {
                Name = "Character",
                Type = "BasePart",
                ReadOnly = true,
                Doc = "The character being simulated: the `Character` of its player on this machine.",
            },
            { Name = "Player", Type = "Player", ReadOnly = true, Doc = "The player the character belongs to." },
            {
                Name = "Tick",
                Type = "number",
                ReadOnly = true,
                Doc = "The player's own tick number: the number their machine gave to the tick of input this step "
                    .. "uses. It is the same on the authority and on the player's client, so use it to time an "
                    .. "effect such as a dash that lasts twenty ticks.",
            },
            { Name = "DeltaTime", Type = "number", ReadOnly = true, Doc = "The length of the step, in seconds." },
            {
                Name = "Replaying",
                Type = "boolean",
                ReadOnly = true,
                Doc = "Whether this step is being simulated again after a correction, on a client. Check it "
                    .. "before doing something that must happen only once, such as playing a sound or emitting "
                    .. "particles.",
            },
            {
                Name = "Random",
                Type = "Random",
                ReadOnly = true,
                Doc = "A random number generator seeded from the player and `Tick`. It gives the same numbers "
                    .. "when the step is first simulated and when it is replayed, on the client and on the "
                    .. "authority. Each read returns a generator at the start of its sequence, so store it in a "
                    .. "local variable.",
            },
        },
        Methods = {
            {
                Name = "GetIntent",
                Parameters = { { Name = "action", Type = "string" } },
                Returns = { "boolean | number | Vector2 | vector" },
                Doc = "Returns the player's intent named `action` for this tick, as `Player:GetIntent` does. In a "
                    .. "replayed step, it returns the value the tick originally had.",
            },
            {
                Name = "Pressed",
                Parameters = { { Name = "action", Type = "string" } },
                Returns = { "boolean" },
                Doc = "Returns whether the button `action` was pressed on this tick: it is held now and was not "
                    .. "held on the previous step.",
            },
        },
    },
    {
        Name = "WorldStep",
        Module = "script",
        Records = { "ADR 0254" },
        Doc = "One tick of the world, passed to a function bound with `RunService:BindToWorldStep`. It is valid "
            .. "only during that call.",
        Properties = {
            {
                Name = "Tick",
                Type = "number",
                ReadOnly = true,
                Doc = "The authority's tick number for this step. On a client it is the tick at which the "
                    .. "authority will simulate the tick being predicted, so a part placed from it is where the "
                    .. "authority will have it.",
            },
            { Name = "DeltaTime", Type = "number", ReadOnly = true, Doc = "The length of the step, in seconds." },
            {
                Name = "Replaying",
                Type = "boolean",
                ReadOnly = true,
                Doc = "Whether this step is being simulated again after a correction, on a client. Check it "
                    .. "before doing something that must happen only once, such as playing a sound.",
            },
            {
                Name = "Predicted",
                Type = "boolean",
                ReadOnly = true,
                Doc = "Whether this machine is a client simulating a tick ahead of its authority. It is false on "
                    .. "the authority and in a single-player game.",
            },
            {
                Name = "Random",
                Type = "Random",
                ReadOnly = true,
                Doc = "A random number generator seeded from `Tick`. It gives the same numbers on every machine "
                    .. "and in every replay of the tick. Each read returns a generator at the start of its "
                    .. "sequence, so store it in a local variable.",
            },
        },
    },
    {
        Name = "AnimationTrack",
        Module = "script",
        Doc = "One clip playing on one `AnimationPlayer`, returned by `LoadAnimation`. It is a handle, not an "
            .. "instance: it has no parent and is not in the tree. Its properties can be set, and every holder of "
            .. "the handle sees the change.\n\n"
            .. "A track runs on simulation time, scaled by `Speed`, so a clip is at the same position on the same "
            .. "tick in a replay as it was live.",
        Properties = {
            {
                Name = "Playing",
                Type = "boolean",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "Whether the track is advancing. It becomes false when a track that is not `Looped` reaches "
                    .. "the end of its clip. Such a track keeps showing its last frame until it is stopped or "
                    .. "played again.",
            },
            {
                Name = "Looped",
                Type = "boolean",
                ThreadSafety = "Safe",
                Doc = "Whether the track starts again when it reaches the end of its clip. A looped track never "
                    .. "fires `Ended`.",
            },
            {
                Name = "Speed",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "How fast the clip plays. 2 is twice as fast and half as long. 0 holds the current pose.",
            },
            {
                Name = "Weight",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "How much this track contributes when several tracks animate the same skeleton. Setting it "
                    .. "takes effect immediately; to fade a track in or out, pass a fade time to `Play` or "
                    .. "`Stop`.\n\n"
                    .. "Tracks are blended as a weighted average for each joint. A joint that no playing clip "
                    .. "animates keeps its rest pose.",
            },
            {
                Name = "Length",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The duration of the clip, in seconds, or 0 if the clip was not found.",
            },
            {
                Name = "TimePosition",
                Type = "number",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The current position in the clip, in seconds.",
            },
            {
                Name = "Ended",
                Type = "Signal<>",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "A signal that fires on the tick a track that is not `Looped` reaches the end of its clip. "
                    .. "`Stop` does not fire it, so code that starts the next animation from this signal is not "
                    .. "triggered by an interrupted one.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Play",
                Parameters = { { Name = "fadeTime", Type = "number?" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Plays the clip from the beginning, every time it is called, so an animation triggered "
                    .. "twice plays twice.\n\n"
                    .. "`fadeTime` fades the track's weight in from 0 to its `Weight` over that many seconds, "
                    .. "which blends it smoothly with what is already playing.",
            },
            {
                Name = "Stop",
                Parameters = { { Name = "fadeTime", Type = "number?" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Stops the track. With `fadeTime`, its weight fades out over that many seconds and the "
                    .. "track stops when the weight reaches 0. Without it, the track stops immediately. `Ended` "
                    .. "does not fire in either case.",
            },
        } :: { Schema.Method },
    },
    {
        Name = "Material",
        Module = "script",
        Records = { "ADR 0090" },
        Doc = "The appearance of a surface: its color, its texture maps, and how rough and metallic it is. It "
            .. "also sets how the surface behaves in physics and what tags it has. A material is not an instance. "
            .. "A project stores each material as a `.material.json` file under `content/`, and a part uses one "
            .. "through `BasePart.Material`.\n\n"
            .. "There are two kinds of material handle. `Material.load` returns the shared handle of a material "
            .. "file, which is read-only: setting a property on it raises an error, because the change would "
            .. "affect every part that uses the file. `Clone` returns a copy that exists only at run time, can be "
            .. "changed, and is never saved.\n\n"
            .. "Nothing is cloned automatically. Reading `part.Material` returns the handle the part uses, and "
            .. "changing a clone changes every part that uses that clone.",
        Constructors = {
            {
                Name = "load",
                Parameters = { { Name = "content", Type = "Content" } },
                Returns = { "Material" },
                ThreadSafety = "Unsafe",
                Doc = "Returns the shared, read-only handle of a material file, as in "
                    .. '`Material.load("asset://materials/brick.material.json")`. Raises an error if the path is '
                    .. "not a material.",
            },
        } :: { Schema.Method },
        Properties = {
            {
                Name = "Source",
                Type = "Content",
                ReadOnly = true,
                ThreadSafety = "Safe",
                Doc = "The path of the material file this handle comes from. For a clone, including a clone of a "
                    .. "clone, it is the file the original was loaded from.",
            },
            {
                Name = "Color",
                Type = "Color3",
                ThreadSafety = "Safe",
                Doc = "The base color of the surface. It is multiplied by `ColorMap` if there is one.",
            },
            {
                Name = "Transparency",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "How transparent the surface is, from 0 (opaque) to 1 (invisible).",
            },
            {
                Name = "ColorMap",
                Type = "Content",
                ThreadSafety = "Safe",
                Doc = "The base color image, multiplied by `Color`. It holds color data in sRGB, unlike the "
                    .. "normal and metallic-roughness maps, which hold linear data.",
            },
            {
                Name = "NormalMap",
                Type = "Content",
                ThreadSafety = "Safe",
                Doc = "A normal map in tangent space. It holds linear data, not color.",
            },
            {
                Name = "MetallicRoughnessMap",
                Type = "Content",
                ThreadSafety = "Safe",
                Doc = "An image with occlusion in its red channel, roughness in green and metalness in blue, as "
                    .. "in glTF. It holds linear data, not color.",
            },
            {
                Name = "Emissive",
                Type = "Color3",
                ThreadSafety = "Safe",
                Doc = "The color of the light the surface gives off by itself. It makes the surface bright but "
                    .. "does not light other objects; use a `PointLight` for that.",
            },
            {
                Name = "EmissiveMap",
                Type = "Content",
                ThreadSafety = "Safe",
                Doc = "An image of the light the surface gives off, multiplied by `Emissive`. It holds color data.",
            },
            {
                Name = "Metalness",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "How metallic the surface is, from 0 (not a metal, such as plastic, stone or wood) to 1 "
                    .. "(bare metal).",
            },
            {
                Name = "Roughness",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "How rough the surface is, from 0 (a mirror) to 1 (fully matte). The default is 0.7.",
            },
            {
                Name = "NormalScale",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "The strength of the normal map. 1 uses the map as authored, and 0 gives a flat surface.",
            },
            {
                Name = "AlphaMode",
                Type = "Enum.AlphaMode",
                ThreadSafety = "Safe",
                Doc = "How the alpha of the base color is used. `Opaque` ignores it, `Mask` draws only the pixels "
                    .. "whose alpha is at least `AlphaCutoff`, and `Blend` blends the surface with what is behind "
                    .. "it.",
            },
            {
                Name = "AlphaCutoff",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "The alpha threshold for `Mask` mode. It is used only in that mode.",
            },
            {
                Name = "DoubleSided",
                Type = "boolean",
                ThreadSafety = "Safe",
                Doc = "Whether back faces are drawn. Turn it on for thin surfaces such as leaves and cloth.",
            },
            {
                Name = "TileSize",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "The size of one repeat of the textures on a part's faces, in meters. A face shows as many "
                    .. "repeats as fit along it, so the texture has the same scale on every face of a part. At 0, "
                    .. "each texture is stretched over the whole face. A `MeshPart` uses the UVs from its file "
                    .. "instead. A terrain layer also repeats at this size.",
            },
            {
                Name = "HeightMap",
                Type = "string",
                ThreadSafety = "Safe",
                Doc = "A height image for a terrain layer, in which white is high. Where two layers meet, the "
                    .. "higher one shows through. Only terrain uses it.",
            },
            {
                Name = "Triplanar",
                Type = "boolean",
                ThreadSafety = "Safe",
                Doc = "Whether a terrain layer's textures are projected along three axes, so that they do not "
                    .. "stretch on a cliff. On by default. Only terrain uses it.",
            },
            {
                Name = "BlendSharpness",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "How sharp the edge is where this terrain layer meets another, from 0 (a wide fade) to 1 (a "
                    .. "sharp line that follows the height map). Only terrain uses it.",
            },
            {
                Name = "TilingVariation",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "How much a large-scale pattern, tens of meters across, varies the color of a terrain "
                    .. "layer, from 0 (none) to 1. It hides the repetition of the texture. The default is 0.5. "
                    .. "Only terrain uses it.",
            },
            {
                Name = "TilingFarScale",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "The scale of a second, larger sample of a terrain layer's textures that is blended in with "
                    .. "distance, from 1 (none) to 32. The default is 6. Only terrain uses it.",
            },
            {
                Name = "HexTiling",
                Type = "boolean",
                ThreadSafety = "Safe",
                Doc = "Whether a terrain layer's textures are placed in hexagonal cells, each randomly offset and "
                    .. "rotated, with blended borders. It hides repetition, which shows most in photographic "
                    .. "textures. It samples each texture three times instead of once. Off by default. Only "
                    .. "terrain uses it.",
            },
            {
                Name = "Friction",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "How much a body that touches this surface resists sliding. A part uses its material's "
                    .. "friction unless the part's own `Friction` is set, and terrain uses the friction of the "
                    .. "material drawn at each point. The default is 0.3.",
            },
            {
                Name = "Restitution",
                Type = "number",
                ThreadSafety = "Safe",
                Doc = "The fraction of its speed a body keeps when it bounces off this surface, from 0 (no "
                    .. "bounce) to 1. The default is 0.",
            },
            {
                Name = "FootstepSound",
                Type = "Content",
                ThreadSafety = "Safe",
                Doc = "The sound of a footstep on this surface, as a content path, or an empty string. The engine "
                    .. "does not play it; a game reads it through `CharacterBody.FloorMaterial` and plays it in "
                    .. "time with the character's steps.",
            },
            {
                Name = "Tags",
                Type = "{ string }",
                ThreadSafety = "Safe",
                Doc = 'Labels a game can read from a surface, such as "slippery", "metal" or "lava", in the order '
                    .. "the material lists them. Each read returns a new table.",
            },
        } :: { Schema.Property },
        Methods = {
            {
                Name = "Clone",
                Parameters = {},
                Returns = { "Material" },
                ThreadSafety = "Unsafe",
                Doc = "Returns a copy of the material that can be changed. The copy has the same properties and "
                    .. "the same `Source`, exists only at run time and is never saved. Assign it with "
                    .. "`part.Material = copy` and then change it: every part that uses the copy changes, and "
                    .. "nothing else does.",
            },
            {
                Name = "GetShaderParameter",
                Parameters = { { Name = "name", Type = "string" } },
                Returns = { "(number | Vector2 | vector | { number } | string)?" },
                ThreadSafety = "Unsafe",
                Records = { "ADR 0091" },
                Doc = "Returns the value this material sets for a parameter of its surface shader, or nil if it "
                    .. "leaves the parameter at the shader's default. The value is a number, a `Vector2`, a "
                    .. "`vector` (also used for a color), a table of four numbers, or the path of a texture.",
            },
            {
                Name = "SetShaderParameter",
                Parameters = {
                    { Name = "name", Type = "string" },
                    { Name = "value", Type = "number | boolean | Vector2 | vector | Color3 | { number } | string" },
                },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Sets a parameter of the material's surface shader. It works only on a clone, because a "
                    .. "loaded material is shared and read-only. A string value is the path of a texture. Every "
                    .. "part that uses the clone is drawn with the new value from the next frame. The value is "
                    .. "not replicated: set it on the machine that draws the part.",
            },
        } :: { Schema.Method },
    },
}

local Definition: Schema.Definition = {
    Datatypes = Datatypes,
}

return Definition
