// **A relay on a socket** (ADR 0178): `rendezvous::RelayServer` given a UDP
// port and a thread to answer on. The relay program is this and a loop that
// prints what it carries; a test is this on a port of the loopback.
#pragma once

#include <memory>
#include <optional>

#include "engine/core/error.h"
#include "engine/core/types.h"
#include "engine/net/rendezvous.h"

namespace engine::net {

class RelayService
{
public:
    RelayService();
    ~RelayService();
    RelayService(const RelayService&) = delete;
    RelayService& operator=(const RelayService&) = delete;

    // Binds `port` on every address of the machine -- zero for any free one,
    // which `port()` then says -- and answers from a thread of its own until
    // `stop`.
    [[nodiscard]] std::optional<core::EngineError> start(core::u16 port, const rendezvous::RelayLimits& limits);
    [[nodiscard]] std::optional<core::EngineError> start(core::u16 port) { return start(port, {}); }
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] core::u16 port() const noexcept;
    [[nodiscard]] rendezvous::RelayStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace engine::net
