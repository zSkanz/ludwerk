// What `http.cpp` shares with the platforms' HTTPS backends (ADR 0063).
#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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

// **When an exchange must be over**. `HttpRequest::timeoutMs`
// is whole-request, and a read timeout alone let a server that sends a byte
// inside each one hold a request for as long as it liked. Each exchange of a
// redirect chain has its own, and the chain is bounded in hops.
using Deadline = std::chrono::steady_clock::time_point;

[[nodiscard]] inline Deadline deadlineAfter(core::u32 ms)
{
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}

// Milliseconds left before `deadline`; zero once it has passed.
[[nodiscard]] inline core::u32 msLeft(Deadline deadline)
{
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    return left <= 0 ? 0u : static_cast<core::u32>(std::min<long long>(left, 0xFFFFFFFFll));
}

// Reads a response that ends when the peer closes, bounded by `maxBodyBytes`
// and by `deadline`: each read is given what is left of it.
[[nodiscard]] std::optional<core::EngineError>
readUntilClosed(const std::function<std::optional<core::EngineError>(std::span<core::u8> chunk, core::usize& received,
                                                                     core::u32 waitMs)>& receive,
                core::usize maxBodyBytes, std::string& raw, Deadline deadline);

// One `https://` exchange on this platform's own TLS stack, following no
// redirect (`performHttp` does). Verifies the certificate against the
// operating system's trust store, always. `net.err.http_tls_unsupported` where
// the platform has none.
[[nodiscard]] std::optional<core::EngineError> performHttps(const HttpRequest& request, const ParsedUrl& url,
                                                            HttpResponse& response);

// **A TLS stream of bytes**, for a protocol of its own over it -- a WebSocket
// (ADR 0223 §2) -- on this platform's TLS stack, verified as `performHttps`
// verifies. Where the platform offers none a raw stream can use, opening one
// says so (`net.err.websocket_tls_unsupported`).
class TlsStream
{
public:
    virtual ~TlsStream() = default;
    [[nodiscard]] virtual std::optional<core::EngineError> send(std::span<const core::u8> data) = 0;
    // Nothing received and no error: `timeoutMs` passed.
    [[nodiscard]] virtual std::optional<core::EngineError> receive(std::span<core::u8> out, core::usize& received,
                                                                   core::u32 timeoutMs) = 0;
};

[[nodiscard]] std::unique_ptr<TlsStream> openTlsStream(std::string_view host, core::u16 port, core::u32 timeoutMs,
                                                       std::optional<core::EngineError>& error);

} // namespace engine::net
