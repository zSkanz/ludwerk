// A blocking HTTP/1.1 client (ADR 0012, api-design.md §7 `@std/net.request`).
//
// **`https://` comes from the platform** (ADR 0063, built in the game-ready
// plan's A1): WinHTTP on Windows, `NSURLSession` on macOS and iOS, the system's
// OpenSSL on Linux -- loaded when first asked for, so a machine without it
// fails the one request rather than the whole program -- and the Java stack on
// Android. Each verifies the certificate against the operating system's own
// trust store, and there is no switch that turns verification off: a player
// build cannot be told to trust a stranger. The TLS and the HTTP are one API on
// three of the four, so a platform's backend makes the whole request.
//
// `http://` stays on the socket client below, which is what a backend on
// localhost or a LAN is (ADR 0012), with `Connection: close`.
//
// **Redirects are followed here, for every backend**: at most `MaxRedirects`,
// never from `https` to `http` -- a downgrade would put what the caller sent
// over TLS on the wire in the clear -- and a `303` (or a `301`/`302` answering
// a `POST`) becomes a `GET` without a body, as every browser does. A redirect
// to another host does not carry `Authorization` or `Cookie`.
//
// No chunked REQUEST bodies. Chunked responses ARE decoded, because a server
// chooses that and a client cannot refuse it.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "engine/core/error.h"
#include "engine/core/types.h"

namespace engine::net {

using core::u16;
using core::u32;

// How many redirects one request follows before it is an error.
inline constexpr u32 MaxRedirects = 5;

struct HttpHeader
{
    std::string name;
    std::string value;
};

struct HttpRequest
{
    std::string url;
    std::string method = "GET";
    std::vector<HttpHeader> headers;
    std::string body;

    // Whole-request, not per-read. A server that sends a byte a second would
    // otherwise hold a coroutine forever without ever timing out.
    u32 timeoutMs = 10'000;

    // A response larger than this is an error rather than a truncation. The
    // engine has no streaming response type in v1, so the whole body lands in
    // memory and an unbounded one is a script handing a stranger the process.
    core::usize maxBodyBytes = 8u * 1024u * 1024u;
};

struct HttpResponse
{
    // The transport succeeded and a response was parsed. Says nothing about the
    // status code: a 404 is `ok` with `statusCode == 404`, which is Lute's
    // shape and the right one -- "the server answered" and "the server agreed"
    // are different questions.
    bool ok = false;
    u16 statusCode = 0;
    std::string statusMessage;
    std::vector<HttpHeader> headers;
    std::string body;
};

// Blocking. Runs on whatever thread calls it, which is never the VM's thread --
// `@std/net` parks the coroutine and hands this to a worker (`async_net.h`).
[[nodiscard]] std::optional<core::EngineError> performHttp(const HttpRequest& request, HttpResponse& response);

// Split out so the parsing is testable without a socket, which is what lets the
// awkward cases -- a chunked body, a header with no space after the colon, a
// status line with no reason phrase -- be tested at all.
struct ParsedUrl
{
    std::string scheme;
    std::string host;
    u16 port = 80;
    // Path plus query, ready to go on the request line. Never empty: a URL with
    // no path is `/`.
    std::string target;
};

[[nodiscard]] std::optional<core::EngineError> parseUrl(std::string_view url, ParsedUrl& out);

// Where a redirect from `from` to `location` goes: an absolute URL as it is, a
// scheme-relative one (`//host/path`) on `from`'s scheme, and a path against
// `from`'s host -- rooted (`/path`), a query (`?q`) on its path, or relative to
// its directory.
[[nodiscard]] std::optional<core::EngineError> resolveRedirect(std::string_view from, std::string_view location,
                                                               std::string& out);

// Parses a complete response, headers and body. `raw` must hold the entire
// response; this does no reading of its own.
[[nodiscard]] std::optional<core::EngineError> parseHttpResponse(std::string_view raw, core::usize maxBodyBytes,
                                                                 HttpResponse& out);

} // namespace engine::net
