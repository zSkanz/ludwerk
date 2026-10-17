#include "engine/app/admin_channel.h"

#include <array>
#include <chrono>
#include <utility>

#include "engine/core/i18n.h"

namespace engine::app {
namespace {

// How long a handshake message may take.
constexpr core::u32 HandshakeMs = 10000;

[[nodiscard]] std::optional<core::EngineError> writeFrame(net::TcpStream& stream, std::span<const core::u8> bytes)
{
    std::vector<core::u8> frame;
    frame.reserve(bytes.size() + 4);
    const auto length = static_cast<core::u32>(bytes.size());
    for (int at = 0; at < 4; ++at)
        frame.push_back(static_cast<core::u8>(length >> (8 * at)));
    frame.insert(frame.end(), bytes.begin(), bytes.end());
    return stream.send(frame);
}

// One whole frame out of `buffer`, reading more within `timeoutMs`.
[[nodiscard]] std::optional<core::EngineError> readFrame(net::TcpStream& stream, std::vector<core::u8>& buffer,
                                                         std::vector<core::u8>& out, bool& got, core::u32 timeoutMs)
{
    got = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        if (buffer.size() >= 4) {
            core::u32 length = 0;
            for (int at = 0; at < 4; ++at)
                length |= static_cast<core::u32>(buffer[static_cast<std::size_t>(at)]) << (8 * at);
            if (length > MaxAdminMessage + net::NoiseOverheadBytes + 64)
                return core::makeError(ENG_TR("net.err.closed"));
            if (buffer.size() >= 4u + length) {
                out.assign(buffer.begin() + 4, buffer.begin() + 4 + static_cast<std::ptrdiff_t>(length));
                buffer.erase(buffer.begin(), buffer.begin() + 4 + static_cast<std::ptrdiff_t>(length));
                got = true;
                return std::nullopt;
            }
        }
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0)
            return std::nullopt;
        std::array<core::u8, 16 * 1024> chunk{};
        core::usize received = 0;
        if (auto error = stream.receive(chunk, received, static_cast<core::u32>(left)); error.has_value())
            return error;
        buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(received));
    }
}

[[nodiscard]] std::optional<core::EngineError> handshakeFailed()
{
    return core::makeError(ENG_TR("net.err.admin_handshake"));
}

} // namespace

std::vector<core::u8> adminPrologue()
{
    const std::string_view text = "admin 1";
    return std::vector<core::u8>(text.begin(), text.end());
}

AdminChannel::AdminChannel(net::TcpStream stream, net::NoiseSender sender, net::NoiseReceiver receiver)
    : m_stream(std::move(stream)), m_sender(std::move(sender)), m_receiver(std::move(receiver))
{}

std::optional<AdminChannel> AdminChannel::accept(net::TcpStream stream, const core::X25519Pair& serverKey,
                                                 std::span<const core::u8> prologue, std::array<core::u8, 32>& adminKey,
                                                 std::optional<core::EngineError>& error)
{
    net::NoiseHandshake handshake(net::NoiseHandshake::Role::Responder, serverKey, prologue);
    std::vector<core::u8> buffer;
    std::vector<core::u8> frame;
    bool got = false;
    // Their first, our second, their third.
    if ((error = readFrame(stream, buffer, frame, got, HandshakeMs)).has_value() || !got || !handshake.read(frame)) {
        error = error.has_value() ? error : handshakeFailed();
        return std::nullopt;
    }
    const std::optional<std::vector<core::u8>> second = handshake.write();
    if (!second.has_value() || (error = writeFrame(stream, *second)).has_value()) {
        error = error.has_value() ? error : handshakeFailed();
        return std::nullopt;
    }
    if ((error = readFrame(stream, buffer, frame, got, HandshakeMs)).has_value() || !got || !handshake.read(frame) ||
        !handshake.done()) {
        error = error.has_value() ? error : handshakeFailed();
        return std::nullopt;
    }
    adminKey = handshake.remoteStatic();
    const net::NoiseHandshake::Keys keys = handshake.split();
    AdminChannel channel(std::move(stream), net::NoiseSender(keys.send), net::NoiseReceiver(keys.receive));
    channel.m_buffer = std::move(buffer);
    return channel;
}

std::optional<AdminChannel> AdminChannel::connect(std::string_view host, core::u16 port,
                                                  const core::X25519Pair& adminKey, std::span<const core::u8> prologue,
                                                  std::array<core::u8, 32>& serverKey,
                                                  std::optional<core::EngineError>& error)
{
    net::TcpStream stream;
    if ((error = stream.connect(host, port, HandshakeMs)).has_value())
        return std::nullopt;
    net::NoiseHandshake handshake(net::NoiseHandshake::Role::Initiator, adminKey, prologue);
    const std::optional<std::vector<core::u8>> first = handshake.write();
    if (!first.has_value() || (error = writeFrame(stream, *first)).has_value()) {
        error = error.has_value() ? error : handshakeFailed();
        return std::nullopt;
    }
    std::vector<core::u8> buffer;
    std::vector<core::u8> frame;
    bool got = false;
    if ((error = readFrame(stream, buffer, frame, got, HandshakeMs)).has_value() || !got || !handshake.read(frame)) {
        error = error.has_value() ? error : handshakeFailed();
        return std::nullopt;
    }
    serverKey = handshake.remoteStatic();
    const std::optional<std::vector<core::u8>> third = handshake.write();
    if (!third.has_value() || (error = writeFrame(stream, *third)).has_value() || !handshake.done()) {
        error = error.has_value() ? error : handshakeFailed();
        return std::nullopt;
    }
    const net::NoiseHandshake::Keys keys = handshake.split();
    AdminChannel channel(std::move(stream), net::NoiseSender(keys.send), net::NoiseReceiver(keys.receive));
    channel.m_buffer = std::move(buffer);
    return channel;
}

std::optional<core::EngineError> AdminChannel::send(AdminKind kind, std::string_view text)
{
    std::vector<core::u8> plain;
    plain.reserve(text.size() + 1);
    plain.push_back(static_cast<core::u8>(kind));
    plain.insert(plain.end(), text.begin(),
                 text.begin() + static_cast<std::ptrdiff_t>(std::min(text.size(), MaxAdminMessage)));
    std::vector<core::u8> sealed;
    m_sender.seal(plain, sealed);
    return writeFrame(m_stream, sealed);
}

std::optional<core::EngineError> AdminChannel::receive(AdminKind& kind, std::string& text, bool& got,
                                                       core::u32 timeoutMs)
{
    std::vector<core::u8> frame;
    if (auto error = readFrame(m_stream, m_buffer, frame, got, timeoutMs); error.has_value() || !got)
        return error;
    std::vector<core::u8> plain;
    // **Forged, changed or played back**: the end of the conversation.
    if (!m_receiver.open(frame, plain) || plain.empty()) {
        got = false;
        return core::makeError(ENG_TR("net.err.admin_handshake"));
    }
    kind = static_cast<AdminKind>(plain[0]);
    text.assign(plain.begin() + 1, plain.end());
    return std::nullopt;
}

} // namespace engine::app
