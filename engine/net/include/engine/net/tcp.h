// A blocking TCP client stream with deadlines (architecture.md §2 `net_api`).
//
// Client only. Nothing in v1 listens, and the one caller -- the dev-server
// control connection (ADR 0035) -- dials 127.0.0.1. A listener is what would
// put a port on the developer's machine, which is the thing that decision
// exists to avoid.
//
// No socket type appears in this header. `IDevice` earns its opacity the same
// way (R17): a caller that cannot see a `SOCKET` cannot write code that only
// compiles on one platform.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "engine/core/error.h"
#include "engine/core/types.h"

namespace engine::net {

using core::u16;
using core::u32;
using core::u8;
using core::usize;

class TcpStream
{
public:
    TcpStream();
    ~TcpStream();

    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&&) noexcept;
    TcpStream& operator=(TcpStream&&) noexcept;

    // Resolves `host`, connects, and gives up after `timeoutMs`. The timeout is
    // the point: the dev server may not be listening yet, and the OS default
    // for a refused-then-retried connect is tens of seconds of nothing.
    [[nodiscard]] std::optional<core::EngineError> connect(std::string_view host, u16 port, u32 timeoutMs);

    // Writes all of `data` or fails. A partial write is not reported as
    // success: the caller is framing a protocol, and half a frame is worse
    // than a closed connection.
    [[nodiscard]] std::optional<core::EngineError> send(std::span<const u8> data);

    // Reads whatever has arrived, up to `out.size()`.
    //
    // `received == 0` with no error means `timeoutMs` elapsed and nothing came.
    // An orderly close by the peer is `net.err.closed` -- an error, not zero --
    // because a reader that cannot tell "nothing yet" from "never again" spins
    // forever on a dead socket.
    [[nodiscard]] std::optional<core::EngineError> receive(std::span<u8> out, usize& received, u32 timeoutMs);

    void close();
    [[nodiscard]] bool isOpen() const noexcept;

    // The operating system's socket, or -1 when closed: what a TLS stack that
    // takes a descriptor is handed (ADR 0063). The stream still owns it.
    [[nodiscard]] std::intptr_t nativeHandle() const noexcept;

private:
    friend class TcpListener;
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// **A TCP port a dedicated server listens on for its administrators** (ADR
// 0219) -- the one listener in the engine besides a match's own socket, off
// unless asked for. On the machine's own address unless told otherwise.
class TcpListener
{
public:
    TcpListener();
    ~TcpListener();

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // `anyAddress` false listens on 127.0.0.1 alone.
    [[nodiscard]] std::optional<core::EngineError> open(u16 port, bool anyAddress);
    // A connection, if one arrives within `timeoutMs`, and the IPv4 address it
    // came from; nothing otherwise.
    [[nodiscard]] std::optional<TcpStream> accept(u32 timeoutMs, std::string& peerAddress);
    void close();
    [[nodiscard]] u16 port() const noexcept { return m_port; }

private:
    std::intptr_t m_handle = -1;
    u16 m_port = 0;
};

} // namespace engine::net
