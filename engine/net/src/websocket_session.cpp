#include "engine/net/websocket_session.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "engine/core/crypto.h"
#include "engine/core/i18n.h"
#include "engine/net/tcp.h"
#include "engine/net/websocket_protocol.h"
#include "http_internal.h"

#if defined(_WIN32) && !ENG_PLATFORM_UWP
#define ENG_WEBSOCKET_WINHTTP 1
#include <windows.h>
#include <winhttp.h>
#else
#define ENG_WEBSOCKET_WINHTTP 0
#endif

namespace engine::net {
namespace {

using core::I18nArg;

// How long a connection may take to open, and how long the thread waits for
// a message before it looks at what it has to send.
constexpr core::u32 ConnectMs = 10000;
constexpr core::u32 PollMs = 20;
// What a close says when the connection simply went (RFC 6455 §7.1.5).
constexpr core::u16 AbnormalClose = 1006;

} // namespace

struct WebSocketSession::Impl
{
    std::string url;
    std::vector<HttpHeader> headers;
    core::usize maxMessageBytes = 1u << 20;

    mutable std::mutex mutex;
    std::condition_variable wake;
    State state = State::Connecting;
    std::optional<core::EngineError> failure;
    std::deque<Message> inbox;
    std::deque<Message> outbox;
    bool closeAsked = false;
    core::u16 closeCode = AbnormalClose;
    std::string closeReason;
    std::atomic<bool> stopping{false};
    std::thread thread;

    void opened()
    {
        const std::lock_guard lock(mutex);
        state = State::Open;
    }

    void closed(core::u16 code, std::string reason, std::optional<core::EngineError> why = std::nullopt)
    {
        const std::lock_guard lock(mutex);
        if (state == State::Connecting && why.has_value())
            failure = std::move(why);
        if (state != State::Closed) {
            closeCode = code;
            closeReason = std::move(reason);
        }
        state = State::Closed;
    }

    void arrived(Message message)
    {
        const std::lock_guard lock(mutex);
        inbox.push_back(std::move(message));
    }

    // What is waiting to go, and whether a close was asked for.
    [[nodiscard]] bool takeOutbox(std::deque<Message>& out, bool& close, core::u16& code, std::string& reason)
    {
        const std::lock_guard lock(mutex);
        out.swap(outbox);
        close = closeAsked;
        code = closeCode;
        reason = closeReason;
        return !out.empty() || close;
    }

    void runSocket();
#if ENG_WEBSOCKET_WINHTTP
    void runWinHttp();
#endif
};

namespace {

// `ws://` or `wss://`, read as the HTTP client reads `http://` and `https://`.
[[nodiscard]] std::optional<core::EngineError> parseSocketUrl(const std::string& url, ParsedUrl& out, bool& secure)
{
    std::string asHttp;
    if (url.starts_with("wss://")) {
        secure = true;
        asHttp = "https://" + url.substr(6);
    }
    else if (url.starts_with("ws://")) {
        secure = false;
        asHttp = "http://" + url.substr(5);
    }
    else {
        const I18nArg args[] = {{"url", url}};
        return core::makeError(ENG_TR("net.err.websocket_url"), args);
    }
    return parseUrl(asHttp, out);
}

// One stream of bytes: a socket, or TLS over one.
class Stream
{
public:
    explicit Stream(TcpStream tcp) : m_tcp(std::move(tcp)) {}
    explicit Stream(std::unique_ptr<TlsStream> tls) : m_tls(std::move(tls)) {}

    [[nodiscard]] std::optional<core::EngineError> send(std::span<const core::u8> data)
    {
        return m_tls != nullptr ? m_tls->send(data) : m_tcp.send(data);
    }
    [[nodiscard]] std::optional<core::EngineError> receive(std::span<core::u8> out, core::usize& received,
                                                           core::u32 timeoutMs)
    {
        return m_tls != nullptr ? m_tls->receive(out, received, timeoutMs) : m_tcp.receive(out, received, timeoutMs);
    }

private:
    TcpStream m_tcp;
    std::unique_ptr<TlsStream> m_tls;
};

[[nodiscard]] core::u32 maskKey()
{
    std::array<core::u8, 4> bytes{};
    core::secureRandom(bytes);
    return static_cast<core::u32>(bytes[0]) | static_cast<core::u32>(bytes[1]) << 8 |
           static_cast<core::u32>(bytes[2]) << 16 | static_cast<core::u32>(bytes[3]) << 24;
}

} // namespace

void WebSocketSession::Impl::runSocket()
{
    ParsedUrl parsed;
    bool secure = false;
    if (auto error = parseSocketUrl(url, parsed, secure); error.has_value()) {
        closed(AbnormalClose, {}, error);
        return;
    }
    std::optional<Stream> stream;
    if (secure) {
        std::optional<core::EngineError> error;
        std::unique_ptr<TlsStream> tls = openTlsStream(parsed.host, parsed.port, ConnectMs, error);
        if (tls == nullptr) {
            closed(AbnormalClose, {}, error);
            return;
        }
        stream.emplace(std::move(tls));
    }
    else {
        TcpStream tcp;
        if (auto error = tcp.connect(parsed.host, parsed.port, ConnectMs); error.has_value()) {
            closed(AbnormalClose, {}, error);
            return;
        }
        stream.emplace(std::move(tcp));
    }

    // **The opening handshake**, with the caller's headers in it.
    std::array<core::u8, 16> nonce{};
    core::secureRandom(nonce);
    ws::ClientHandshake handshake = ws::buildClientHandshake(parsed.host, parsed.port, parsed.target, nonce);
    std::string extra;
    for (const HttpHeader& header : headers) {
        if (auto error = checkHeader(header); error.has_value()) {
            closed(AbnormalClose, {}, error);
            return;
        }
        extra += header.name + ": " + header.value + "\r\n";
    }
    handshake.request.insert(handshake.request.size() - 2, extra);
    if (auto error = stream->send(std::span<const core::u8>(reinterpret_cast<const core::u8*>(handshake.request.data()),
                                                            handshake.request.size()));
        error.has_value()) {
        closed(AbnormalClose, {}, error);
        return;
    }
    std::string response;
    std::vector<core::u8> buffer;
    std::array<core::u8, 16 * 1024> chunk{};
    const auto connectDeadline = deadlineAfter(ConnectMs);
    while (true) {
        if (stopping) {
            closed(AbnormalClose, {});
            return;
        }
        core::usize received = 0;
        if (auto error = stream->receive(chunk, received, PollMs); error.has_value()) {
            closed(AbnormalClose, {}, error);
            return;
        }
        response.append(reinterpret_cast<const char*>(chunk.data()), received);
        if (const std::optional<core::usize> end = ws::findHeaderEnd(response); end.has_value()) {
            if (auto error = ws::validateServerHandshake(response.substr(0, *end), handshake.expectedAccept);
                error.has_value()) {
                closed(AbnormalClose, {}, error);
                return;
            }
            buffer.assign(response.begin() + static_cast<std::ptrdiff_t>(*end), response.end());
            break;
        }
        if (msLeft(connectDeadline) == 0 || response.size() > 64 * 1024) {
            closed(AbnormalClose, {}, core::makeError(ENG_TR("net.err.websocket_handshake")));
            return;
        }
    }
    opened();

    // **Open**: what arrives, read; what is waiting, sent.
    std::vector<core::u8> message;
    ws::Opcode messageKind = ws::Opcode::Text;
    while (true) {
        std::deque<Message> sending;
        bool closing = false;
        core::u16 code = 1000;
        std::string reason;
        if (takeOutbox(sending, closing, code, reason)) {
            for (const Message& one : sending) {
                std::vector<core::u8> frame;
                ws::encodeFrame(
                    frame, one.binary ? ws::Opcode::Binary : ws::Opcode::Text, true,
                    std::span<const core::u8>(reinterpret_cast<const core::u8*>(one.bytes.data()), one.bytes.size()),
                    maskKey());
                if (auto error = stream->send(frame); error.has_value()) {
                    closed(AbnormalClose, {});
                    return;
                }
            }
            if (closing || stopping) {
                std::vector<core::u8> payload{static_cast<core::u8>(code >> 8), static_cast<core::u8>(code & 0xff)};
                payload.insert(payload.end(), reason.begin(), reason.end());
                std::vector<core::u8> frame;
                ws::encodeFrame(frame, ws::Opcode::Close, true, payload, maskKey());
                (void)stream->send(frame);
                closed(code, reason);
                return;
            }
        }
        if (stopping) {
            closed(AbnormalClose, {});
            return;
        }
        // Whole frames out of what has come.
        while (true) {
            const ws::DecodeResult decoded = ws::decodeFrame(buffer, maxMessageBytes);
            if (decoded.status == ws::DecodeStatus::NeedMore)
                break;
            if (decoded.status == ws::DecodeStatus::Malformed || decoded.frame.masked) {
                closed(1002, {});
                return;
            }
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));
            const ws::Frame& frame = decoded.frame;
            switch (frame.opcode) {
            case ws::Opcode::Ping: {
                std::vector<core::u8> pong;
                ws::encodeFrame(pong, ws::Opcode::Pong, true, frame.payload, maskKey());
                (void)stream->send(pong);
                break;
            }
            case ws::Opcode::Pong:
                break;
            case ws::Opcode::Close: {
                const core::u16 said = frame.payload.size() >= 2
                                           ? static_cast<core::u16>(frame.payload[0] << 8 | frame.payload[1])
                                           : static_cast<core::u16>(1005);
                std::string words(frame.payload.size() > 2 ? reinterpret_cast<const char*>(frame.payload.data()) + 2
                                                           : "",
                                  frame.payload.size() > 2 ? frame.payload.size() - 2 : 0);
                std::vector<core::u8> reply;
                ws::encodeFrame(
                    reply, ws::Opcode::Close, true,
                    std::span<const core::u8>(frame.payload.data(), std::min<core::usize>(2, frame.payload.size())),
                    maskKey());
                (void)stream->send(reply);
                closed(said, std::move(words));
                return;
            }
            case ws::Opcode::Text:
            case ws::Opcode::Binary:
            case ws::Opcode::Continuation: {
                if (frame.opcode != ws::Opcode::Continuation) {
                    message.clear();
                    messageKind = frame.opcode;
                }
                message.insert(message.end(), frame.payload.begin(), frame.payload.end());
                if (message.size() > maxMessageBytes) {
                    closed(1009, {});
                    return;
                }
                if (frame.fin)
                    arrived(Message{messageKind == ws::Opcode::Binary, std::string(message.begin(), message.end())});
                break;
            }
            }
        }
        core::usize received = 0;
        if (auto error = stream->receive(chunk, received, PollMs); error.has_value()) {
            closed(AbnormalClose, {});
            return;
        }
        buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(received));
    }
}

#if ENG_WEBSOCKET_WINHTTP
namespace {

[[nodiscard]] std::wstring wide(std::string_view text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

struct HandleCloser
{
    HINTERNET handle = nullptr;
    ~HandleCloser()
    {
        if (handle != nullptr)
            WinHttpCloseHandle(handle);
    }
};

} // namespace

void WebSocketSession::Impl::runWinHttp()
{
    ParsedUrl parsed;
    bool secure = false;
    if (auto error = parseSocketUrl(url, parsed, secure); error.has_value()) {
        closed(AbnormalClose, {}, error);
        return;
    }
    const auto failed = [&](DWORD code) {
        const I18nArg args[] = {{"url", url}, {"code", static_cast<core::i64>(code)}};
        closed(AbnormalClose, {}, core::makeError(ENG_TR("net.err.websocket_failed"), args));
    };
    HandleCloser session{
        WinHttpOpen(L"engine", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (session.handle == nullptr)
        return failed(GetLastError());
    (void)WinHttpSetTimeouts(session.handle, static_cast<int>(ConnectMs), static_cast<int>(ConnectMs),
                             static_cast<int>(ConnectMs), static_cast<int>(ConnectMs));
    HandleCloser connection{WinHttpConnect(session.handle, wide(parsed.host).c_str(), parsed.port, 0)};
    if (connection.handle == nullptr)
        return failed(GetLastError());
    HandleCloser request{WinHttpOpenRequest(connection.handle, L"GET", wide(parsed.target).c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            secure ? WINHTTP_FLAG_SECURE : 0)};
    if (request.handle == nullptr)
        return failed(GetLastError());
    if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
        return failed(GetLastError());
    std::wstring extra;
    for (const HttpHeader& header : headers) {
        if (auto error = checkHeader(header); error.has_value()) {
            closed(AbnormalClose, {}, error);
            return;
        }
        extra += wide(header.name) + L": " + wide(header.value) + L"\r\n";
    }
    if (!WinHttpSendRequest(request.handle, extra.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : extra.c_str(),
                            extra.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr))
        return failed(GetLastError());
    HandleCloser socket{WinHttpWebSocketCompleteUpgrade(request.handle, 0)};
    if (socket.handle == nullptr)
        return failed(GetLastError());
    opened();

    // **Two threads, as WinHTTP allows**: this one receives, blocking; the
    // other sends what is queued and asks for the close.
    std::thread sender([this, handle = socket.handle] {
        while (true) {
            std::deque<Message> sending;
            bool closing = false;
            core::u16 code = 1000;
            std::string reason;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(200),
                              [&] { return !outbox.empty() || closeAsked || stopping || state == State::Closed; });
                if (state == State::Closed && outbox.empty())
                    return;
            }
            (void)takeOutbox(sending, closing, code, reason);
            for (Message& one : sending) {
                if (WinHttpWebSocketSend(handle,
                                         one.binary ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE
                                                    : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                         one.bytes.data(), static_cast<DWORD>(one.bytes.size())) != NO_ERROR)
                    return;
            }
            if (closing || stopping) {
                (void)WinHttpWebSocketClose(handle, code, reason.empty() ? nullptr : reason.data(),
                                            static_cast<DWORD>(reason.size()));
                return;
            }
        }
    });
    std::vector<char> buffer(16 * 1024);
    std::string message;
    while (true) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD result =
            WinHttpWebSocketReceive(socket.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &type);
        if (result != NO_ERROR) {
            closed(AbnormalClose, {});
            break;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT status = AbnormalClose;
            std::array<char, 123> words{};
            DWORD length = 0;
            (void)WinHttpWebSocketQueryCloseStatus(socket.handle, &status, words.data(),
                                                   static_cast<DWORD>(words.size()), &length);
            closed(status, std::string(words.data(), length));
            break;
        }
        message.append(buffer.data(), read);
        if (message.size() > maxMessageBytes) {
            closed(1009, {});
            break;
        }
        if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            arrived(Message{type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, std::move(message)});
            message.clear();
        }
    }
    wake.notify_all();
    sender.join();
}
#endif

WebSocketSession::WebSocketSession() : m_impl(std::make_unique<Impl>())
{}

WebSocketSession::~WebSocketSession()
{
    m_impl->stopping = true;
    {
        const std::lock_guard lock(m_impl->mutex);
        m_impl->closeAsked = true;
    }
    m_impl->wake.notify_all();
    if (m_impl->thread.joinable())
        m_impl->thread.join();
}

std::unique_ptr<WebSocketSession> WebSocketSession::open(std::string url, std::vector<HttpHeader> headers,
                                                         core::usize maxMessageBytes)
{
    std::unique_ptr<WebSocketSession> session(new WebSocketSession());
    Impl& impl = *session->m_impl;
    impl.url = std::move(url);
    impl.headers = std::move(headers);
    impl.maxMessageBytes = maxMessageBytes;
    impl.thread = std::thread([&impl] {
#if ENG_WEBSOCKET_WINHTTP
        impl.runWinHttp();
#else
        impl.runSocket();
#endif
    });
    return session;
}

WebSocketSession::State WebSocketSession::state() const
{
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->state;
}

std::optional<core::EngineError> WebSocketSession::failure() const
{
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->failure;
}

core::u16 WebSocketSession::closeCode() const
{
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->closeCode;
}

std::string WebSocketSession::closeReason() const
{
    const std::lock_guard lock(m_impl->mutex);
    return m_impl->closeReason;
}

bool WebSocketSession::send(Message message)
{
    {
        const std::lock_guard lock(m_impl->mutex);
        if (m_impl->state == State::Closed || m_impl->closeAsked)
            return false;
        m_impl->outbox.push_back(std::move(message));
    }
    m_impl->wake.notify_all();
    return true;
}

void WebSocketSession::close(core::u16 code, std::string reason)
{
    {
        const std::lock_guard lock(m_impl->mutex);
        if (m_impl->closeAsked)
            return;
        m_impl->closeAsked = true;
        m_impl->closeCode = code;
        m_impl->closeReason = std::move(reason);
    }
    m_impl->wake.notify_all();
}

void WebSocketSession::take(std::vector<Message>& out)
{
    const std::lock_guard lock(m_impl->mutex);
    while (!m_impl->inbox.empty()) {
        out.push_back(std::move(m_impl->inbox.front()));
        m_impl->inbox.pop_front();
    }
}

} // namespace engine::net
