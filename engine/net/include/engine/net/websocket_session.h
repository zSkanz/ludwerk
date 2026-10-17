// **A WebSocket for a game's scripts** (ADR 0223 §2): `net.websocket`.
//
// Connected on a thread of its own, so a script that yields while it connects
// and a socket that waits for a message hold nothing of the frame. Text and
// binary messages both ways, each at most `maxMessageBytes`; `ws://` over the
// engine's own client, `wss://` with the platform's TLS as `net.request` has
// it (ADR 0063) -- WinHTTP on Windows, the system's OpenSSL on Linux. Where the
// platform has no TLS a raw stream can use, `wss://` fails and says so.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/error.h"
#include "engine/core/types.h"
#include "engine/net/http.h"

namespace engine::net {

class WebSocketSession
{
public:
    struct Message
    {
        bool binary = false;
        std::string bytes;
    };

    enum class State : core::u8
    {
        Connecting,
        Open,
        Closed,
    };

    // Starts connecting to `url` -- `ws://` or `wss://` -- at once.
    [[nodiscard]] static std::unique_ptr<WebSocketSession> open(std::string url, std::vector<HttpHeader> headers,
                                                                core::usize maxMessageBytes);
    ~WebSocketSession();

    WebSocketSession(const WebSocketSession&) = delete;
    WebSocketSession& operator=(const WebSocketSession&) = delete;

    [[nodiscard]] State state() const;
    // Why it never opened, once it is closed without having been open.
    [[nodiscard]] std::optional<core::EngineError> failure() const;
    // How it closed: the peer's code and words, or 1006 and nothing when the
    // connection simply went.
    [[nodiscard]] core::u16 closeCode() const;
    [[nodiscard]] std::string closeReason() const;

    // Queued for the thread; false once closed.
    bool send(Message message);
    // Closes it, telling the peer `code` and `reason`.
    void close(core::u16 code, std::string reason);
    // The messages that arrived since the last call, oldest first.
    void take(std::vector<Message>& out);

private:
    WebSocketSession();
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace engine::net
