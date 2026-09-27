// What `http.cpp` shares with the platforms' HTTPS backends (ADR 0063).
#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>

#include "engine/core/error.h"
#include "engine/net/http.h"

namespace engine::net {

// The request line, headers and body of one HTTP/1.1 request, with
// `Connection: close`; refused if a header would inject another request.
[[nodiscard]] std::optional<core::EngineError> buildRequestWire(const HttpRequest& request, const ParsedUrl& url,
                                                                std::string& wire);

// A header that would inject a second request -- a newline in its name or
// value -- refused, and named.
[[nodiscard]] std::optional<core::EngineError> checkHeader(const HttpHeader& header);

// Reads a response that ends when the peer closes, bounded by `maxBodyBytes`.
[[nodiscard]] std::optional<core::EngineError> readUntilClosed(
    const std::function<std::optional<core::EngineError>(std::span<core::u8> chunk, core::usize& received)>& receive,
    core::usize maxBodyBytes, std::string& raw);

// One `https://` exchange on this platform's own TLS stack, following no
// redirect (`performHttp` does). Verifies the certificate against the
// operating system's trust store, always. `net.err.http_tls_unsupported` where
// the platform has none.
[[nodiscard]] std::optional<core::EngineError> performHttps(const HttpRequest& request, const ParsedUrl& url,
                                                            HttpResponse& response);

} // namespace engine::net
