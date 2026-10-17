// **A server's console from another machine** (ADR 0219): a sealed
// conversation over TCP between a dedicated server and one administrator.
//
// Each side is a key. The handshake is the match's own Noise (ADR 0224) --
// the server learns the administrator's key and finds it in `admins.toml`, the
// administrator learns the server's and checks it -- and every message after it
// is sealed with a counter, so nothing on the path reads, changes or plays one
// back. A frame is a length, then the bytes.
#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/crypto.h"
#include "engine/core/error.h"
#include "engine/net/noise.h"
#include "engine/net/tcp.h"

namespace engine::app {

// What a message is.
enum class AdminKind : core::u8
{
    // A console line, administrator to server.
    Line = 1,
    // What it answered, and an answer that is a failure.
    Answer = 2,
    Failed = 3,
    // A line of the server's log, while the administrator follows it.
    LogLine = 4,
    // Follow the log from now on.
    Follow = 5,
};

// The longest message either way: an answer is cut there, with a note.
inline constexpr core::usize MaxAdminMessage = 1024 * 1024;
// And the longest line an administrator sends.
inline constexpr core::usize MaxAdminLine = 4 * 1024;

// What every administrator's handshake binds: never a match's, so neither
// key is ever taken for the other's conversation.
[[nodiscard]] std::vector<core::u8> adminPrologue();

class AdminChannel
{
public:
    // **As the server**: the handshake with whoever connected, their key out.
    [[nodiscard]] static std::optional<AdminChannel> accept(net::TcpStream stream, const core::X25519Pair& serverKey,
                                                            std::span<const core::u8> prologue,
                                                            std::array<core::u8, 32>& adminKey,
                                                            std::optional<core::EngineError>& error);
    // **As the administrator**: the server's key out, for the caller to check
    // before anything is sent.
    [[nodiscard]] static std::optional<AdminChannel>
    connect(std::string_view host, core::u16 port, const core::X25519Pair& adminKey, std::span<const core::u8> prologue,
            std::array<core::u8, 32>& serverKey, std::optional<core::EngineError>& error);

    [[nodiscard]] std::optional<core::EngineError> send(AdminKind kind, std::string_view text);
    // One message within `timeoutMs`, `got` false when none came.
    [[nodiscard]] std::optional<core::EngineError> receive(AdminKind& kind, std::string& text, bool& got,
                                                           core::u32 timeoutMs);

private:
    AdminChannel(net::TcpStream stream, net::NoiseSender sender, net::NoiseReceiver receiver);

    net::TcpStream m_stream;
    net::NoiseSender m_sender;
    net::NoiseReceiver m_receiver;
    std::vector<core::u8> m_buffer;
};

} // namespace engine::app
