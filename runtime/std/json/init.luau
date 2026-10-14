--!strict
--[[
	`@std/json` — the TYPE side of a module whose body is C++.

	**This file is never executed by the engine.** `@std/json` is registered
	natively (`engine/script/src/json_module.cpp`), as `@std/net` is, and this
	is where a strict-mode script learns its shape. What keeps the two from
	drifting is `tests/conformance/std/json.spec.luau`, which asserts the
	surface from inside the running VM.

	Naming is Lute's, not ADR 0034's, and deliberately: `@std/*` exists to be
	the Lute-compatible surface (ADR 0030), so that a module shared with a
	backend encodes and decodes with one `require` on both runtimes.

	**What differs from Lute's own**, none of it in a name:

	- An object's members are written in the order of their names, so the same
	  table is the same text on every run and every machine.
	- A number is written so that reading it back gives the same number.
	- What JSON cannot say raises: a number that is not finite, a key that is
	  not a string, a function or an instance, a table that holds itself.
	- An object read by `deserialize` holds its members and nothing else --
	  nothing a `for` over it would meet beside them.
]]

--- A JSON object: a table with string keys.
export type Object = { [string]: Value }
--- A JSON array.
export type Array = { Value }
--- Any JSON value. `null` is `json.null`, which is not `nil`.
export type Value = nil | number | string | boolean | Array | Object

export type Json = {
    --- What stands for JSON's `null`: in what `deserialize` returns, and to
    --- write one with `serialize`. Not `nil` — a table cannot hold `nil` — and
    --- equal only to itself: compare with `value == json.null`.
    null: any,

    --- The value as JSON text. `prettyPrint` indents it, four spaces a level.
    ---
    --- A table with members and no first element is an object; any other table
    --- is an array, the empty one included — `json.object({})` is how to say
    --- an empty object. Raises for what JSON has no way to say.
    serialize: (value: Value, prettyPrint: boolean?) -> string,

    --- The value the text says. Raises where the text is not JSON, naming the
    --- byte it stops making sense at. A `null` in it is `json.null`.
    deserialize: (text: string) -> Value,

    --- A copy of `props` that `serialize` writes as an object even when it is
    --- empty, and `asObject` answers for.
    object: (props: { [string]: Value }) -> Object,

    --- `value` when it is an object `deserialize` read or `object` made; `nil`
    --- otherwise.
    asObject: (value: Value) -> Object?,

    --- `value` when it is a table that is not such an object; `nil` otherwise.
    asArray: (value: Value) -> Array?,
}

return (error("@std/json is provided by the engine; this file is the type declaration and is never run") :: any) :: Json
