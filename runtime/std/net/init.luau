--!strict
--[[
	`@std/net` — the TYPE side of a module whose body is C++.

	**This file is never executed by the engine.** `scriptRequire` matches an
	engine-provided module before it ever reaches the loader (`modules.cpp`), and
	`@std/net` is registered natively, so what runs is `engine/script/src/net_module.cpp`.
	What this file is for is `luau-analyze`: the `.luaurc` alias points `@std` at
	this directory, so this is where a strict-mode script learns the shape.

	Which means the two CAN drift, and the guard against that is the conformance
	suite rather than good intentions: `tests/conformance/std/net.spec.luau`
	asserts the surface from inside the running VM, so a field that exists here
	and not there fails a gate rather than a script.

	The `error` below is the other half. If the engine ever stops matching this
	name natively, a script gets a loud message naming the cause instead of a
	table of nils.

	Naming is Lute's, not ADR 0034's, and deliberately: `@std/*` exists to be the
	Lute-compatible surface (ADR 0030) so that backend and utility code runs
	unchanged on both runtimes. See the header of `net_module.cpp`.
]]

--- What `net.request` accepts.
---
--- `timeout` is in SECONDS, like every other duration a script writes in this
--- engine (`task.wait`, `TweenInfo`).
export type RequestOptions = {
    --- `http://` or `https://`. HTTPS is the platform's own (WinHTTP, the
    --- system's OpenSSL, `NSURLSession`, Android's stack), and the certificate
    --- is always checked against the device's trust store. Redirects are
    --- followed, at most five, and never from `https` to `http`.
    url: string,
    method: string?,
    headers: { [string]: string }?,
    body: string?,
    timeout: number?,
}

--- What `net.request` returns.
---
--- `ok` is about the TRANSPORT and not about the status code: a 404 is a server
--- answering, so it arrives with `ok = true` and `statusCode = 404`. A call that
--- never reached a server has `ok = false` and the reason in `statusMessage`.
export type Response = {
    ok: boolean,
    statusCode: number,
    statusMessage: string,
    headers: { [string]: string },
    body: string,
}

--- What `net.websocket` accepts.
export type WebSocketOptions = {
    --- Sent with the opening handshake: an `Authorization`, a protocol name.
    headers: { [string]: string }?,
}

--- **An open WebSocket** (ADR 0223 §2). A message is text as a `string` and
--- binary as a `buffer`, at most 1 MiB, both ways.
export type WebSocket = {
    --- Fires with each message that arrives, at a frame safe point.
    Received: Signal<string | buffer>,
    --- Fires once, with the close code and the words, when it closes for any
    --- reason: 1006 and nothing when the connection simply went.
    Closed: Signal<number, string>,
    --- Queues a message; false once the socket is closed.
    Send: (self: WebSocket, message: string | buffer) -> boolean,
    --- Closes it, telling the server `code` (1000) and `reason`.
    Close: (self: WebSocket, code: number?, reason: string?) -> (),
}

export type Net = {
    --- Performs an HTTP/1.1 request. **Yields** — the calling coroutine is
    --- parked and resumed at a frame safe point when the answer arrives, so a
    --- slow server costs the caller time and costs the frame nothing.
    ---
    --- Never raises for a network condition: a refused connection, a bad URL and
    --- a timeout all come back as `ok = false`. It raises only for a malformed
    --- CALL — a missing `url`, a field of the wrong type — because that is a bug
    --- in the script rather than a fact about the world.
    request: (options: RequestOptions) -> Response,
    --- Opens a WebSocket to `url` -- `ws://` or `wss://`, TLS the platform's
    --- as `request`'s is -- and **yields** until it is open, then returns it.
    --- Raises with the reason when it cannot be opened.
    ---
    --- On the server by default: a client's game opens one only with
    --- `websocket_client = true` under `[permissions]` in `project.toml`.
    --- At most eight are open at once.
    websocket: (url: string, options: WebSocketOptions?) -> WebSocket,
}

return (error("@std/net is provided by the engine; this file is the type declaration and is never run") :: any) :: Net
