// `https://` on Linux: the system's OpenSSL (ADR 0063).
//
// **Loaded when first asked for, not linked.** A distribution keeps its
// OpenSSL patched and its trust store current, and a binary linked against one
// soname would not start on a machine that has the other; loading it at the
// first `https://` request means a machine without it fails that request, by
// name, and nothing else. Only the dozen functions a client needs are looked
// up, declared here with opaque pointers, so no OpenSSL header is needed to
// build either.
//
// The certificate is verified against the system's default paths and the name
// against the host, always. The HTTP is the socket client's own, over the TLS
// stream, with `Connection: close`.

#include <cerrno>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>

#include "engine/core/i18n.h"
#include "engine/net/tcp.h"
#include "http_internal.h"

namespace engine::net {
namespace {

using core::I18nArg;

// Values from OpenSSL's public headers, which are part of its stable ABI.
constexpr int SslVerifyPeer = 1;
constexpr int SslCtrlSetTlsextHostname = 55;
constexpr long TlsextNametypeHostName = 0;
constexpr int SslCtrlSetMinProtoVersion = 123;
constexpr long Tls12Version = 0x0303;
constexpr int SslErrorZeroReturn = 6;
constexpr int SslErrorSyscall = 5;
constexpr long X509VerifyOk = 0;

struct OpenSsl
{
    bool loaded = false;
    const void* (*clientMethod)() = nullptr;
    void* (*ctxNew)(const void*) = nullptr;
    int (*ctxDefaultVerifyPaths)(void*) = nullptr;
    int (*ctxLoadVerifyLocations)(void*, const char*, const char*) = nullptr;
    void (*ctxSetVerify)(void*, int, void*) = nullptr;
    long (*ctxCtrl)(void*, int, long, void*) = nullptr;
    void* (*sslNew)(void*) = nullptr;
    void (*sslFree)(void*) = nullptr;
    int (*setFd)(void*, int) = nullptr;
    long (*sslCtrl)(void*, int, long, void*) = nullptr;
    int (*set1Host)(void*, const char*) = nullptr;
    int (*connect)(void*) = nullptr;
    int (*read)(void*, void*, int) = nullptr;
    int (*write)(void*, const void*, int) = nullptr;
    int (*getError)(const void*, int) = nullptr;
    long (*verifyResult)(const void*) = nullptr;
    int (*shutdown)(void*) = nullptr;
};

template <typename Function>
bool bind(void* library, const char* name, Function& out)
{
    out = reinterpret_cast<Function>(::dlsym(library, name));
    return out != nullptr;
}

[[nodiscard]] OpenSsl load()
{
    OpenSsl ssl;
    void* library = ::dlopen("libssl.so.3", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
        library = ::dlopen("libssl.so", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
        return ssl;
    ssl.loaded = bind(library, "TLS_client_method", ssl.clientMethod) && bind(library, "SSL_CTX_new", ssl.ctxNew) &&
                 bind(library, "SSL_CTX_set_default_verify_paths", ssl.ctxDefaultVerifyPaths) &&
                 bind(library, "SSL_CTX_load_verify_locations", ssl.ctxLoadVerifyLocations) &&
                 bind(library, "SSL_CTX_set_verify", ssl.ctxSetVerify) && bind(library, "SSL_CTX_ctrl", ssl.ctxCtrl) &&
                 bind(library, "SSL_new", ssl.sslNew) && bind(library, "SSL_free", ssl.sslFree) &&
                 bind(library, "SSL_set_fd", ssl.setFd) && bind(library, "SSL_ctrl", ssl.sslCtrl) &&
                 bind(library, "SSL_set1_host", ssl.set1Host) && bind(library, "SSL_connect", ssl.connect) &&
                 bind(library, "SSL_read", ssl.read) && bind(library, "SSL_write", ssl.write) &&
                 bind(library, "SSL_get_error", ssl.getError) &&
                 bind(library, "SSL_get_verify_result", ssl.verifyResult) &&
                 bind(library, "SSL_shutdown", ssl.shutdown);
    return ssl;
}

[[nodiscard]] const OpenSsl& openSsl()
{
    static const OpenSsl loaded = load();
    return loaded;
}

// One context for the process, made at the first request: loading the trust
// store is the expensive part, and a context is safe to share once made.
std::mutex g_contextLock;
void* g_context = nullptr;
std::string g_extraRoot;

[[nodiscard]] void* context()
{
    const OpenSsl& ssl = openSsl();
    const std::scoped_lock lock(g_contextLock);
    if (g_context != nullptr)
        return g_context;
    void* made = ssl.ctxNew(ssl.clientMethod());
    if (made == nullptr)
        return nullptr;
    (void)ssl.ctxCtrl(made, SslCtrlSetMinProtoVersion, Tls12Version, nullptr);
    (void)ssl.ctxDefaultVerifyPaths(made);
    if (!g_extraRoot.empty())
        (void)ssl.ctxLoadVerifyLocations(made, g_extraRoot.c_str(), nullptr);
    ssl.ctxSetVerify(made, SslVerifyPeer, nullptr);
    g_context = made;
    return g_context;
}

// Frees a connection when it goes out of scope.
struct Connection
{
    const OpenSsl& ssl;
    void* value = nullptr;
    ~Connection()
    {
        if (value != nullptr) {
            (void)ssl.shutdown(value);
            ssl.sslFree(value);
        }
    }
};

void setTimeouts(int socket, u32 timeoutMs)
{
    timeval timeout{};
    timeout.tv_sec = static_cast<time_t>(timeoutMs / 1000);
    timeout.tv_usec = static_cast<suseconds_t>((timeoutMs % 1000) * 1000);
    (void)::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

} // namespace

#if ENG_NET_TEST_HOOKS
namespace testing {
void trustExtraRoot(const std::string& pemFile)
{
    const std::scoped_lock lock(g_contextLock);
    g_extraRoot = pemFile;
    // The next request makes a context that trusts it. The old one is kept
    // rather than freed: a request on another thread may still hold it.
    g_context = nullptr;
}
} // namespace testing
#endif

std::optional<core::EngineError> performHttps(const HttpRequest& request, const ParsedUrl& url, HttpResponse& response)
{
    const OpenSsl& ssl = openSsl();
    if (!ssl.loaded) {
        const I18nArg args[] = {{"url", request.url}};
        return core::makeError(ENG_TR("net.err.https_no_openssl"), args);
    }
    void* shared = context();
    if (shared == nullptr) {
        const I18nArg args[] = {{"url", request.url}, {"code", static_cast<core::i64>(0)}};
        return core::makeError(ENG_TR("net.err.https_failed"), args);
    }

    std::string wire;
    if (auto error = buildRequestWire(request, url, wire); error.has_value())
        return error;

    const Deadline deadline = deadlineAfter(request.timeoutMs);
    TcpStream stream;
    if (auto error = stream.connect(url.host, url.port, request.timeoutMs); error.has_value())
        return error;
    const int socket = static_cast<int>(stream.nativeHandle());
    setTimeouts(socket, request.timeoutMs);

    Connection connection{ssl, ssl.sslNew(shared)};
    if (connection.value == nullptr || ssl.setFd(connection.value, socket) != 1) {
        const I18nArg args[] = {{"url", request.url}, {"code", static_cast<core::i64>(0)}};
        return core::makeError(ENG_TR("net.err.https_failed"), args);
    }
    // The name it asks for, and the name the certificate must carry.
    (void)ssl.sslCtrl(connection.value, SslCtrlSetTlsextHostname, TlsextNametypeHostName,
                      const_cast<char*>(url.host.c_str()));
    (void)ssl.set1Host(connection.value, url.host.c_str());

    if (const int shook = ssl.connect(connection.value); shook != 1) {
        const long verified = ssl.verifyResult(connection.value);
        const I18nArg args[] = {{"url", request.url},
                                {"code", static_cast<core::i64>(ssl.getError(connection.value, shook))}};
        return verified != X509VerifyOk ? core::makeError(ENG_TR("net.err.https_untrusted"), args)
                                        : core::makeError(ENG_TR("net.err.https_failed"), args);
    }

    for (std::size_t sent = 0; sent < wire.size();) {
        const int wrote = ssl.write(connection.value, wire.data() + sent, static_cast<int>(wire.size() - sent));
        if (wrote <= 0) {
            const I18nArg args[] = {{"url", request.url},
                                    {"code", static_cast<core::i64>(ssl.getError(connection.value, wrote))}};
            return core::makeError(ENG_TR("net.err.https_failed"), args);
        }
        sent += static_cast<std::size_t>(wrote);
    }

    std::string raw;
    const auto receive = [&](std::span<u8> chunk, core::usize& received,
                             u32 waitMs) -> std::optional<core::EngineError> {
        setTimeouts(socket, waitMs);
        const int got = ssl.read(connection.value, chunk.data(), static_cast<int>(chunk.size()));
        if (got > 0) {
            received = static_cast<core::usize>(got);
            return std::nullopt;
        }
        const int why = ssl.getError(connection.value, got);
        // A close, announced or not: many servers end a `Connection: close`
        // response by closing the socket without a TLS goodbye.
        if (why == SslErrorZeroReturn || (why == SslErrorSyscall && got == 0))
            return core::makeError(ENG_TR("net.err.closed"));
        if (why == SslErrorSyscall && (errno == EAGAIN || errno == EWOULDBLOCK))
            return core::makeError(ENG_TR("net.err.http_response_timeout"));
        const I18nArg args[] = {{"url", request.url}, {"code", static_cast<core::i64>(why)}};
        return core::makeError(ENG_TR("net.err.https_failed"), args);
    };
    if (auto error = readUntilClosed(receive, request.maxBodyBytes, raw, deadline); error.has_value())
        return error;
    return parseHttpResponse(raw, request.maxBodyBytes, response);
}

namespace {

// One TLS connection over a socket, kept for a protocol's own bytes.
class OpenSslStream final : public TlsStream
{
public:
    OpenSslStream(TcpStream stream, void* connection) : m_stream(std::move(stream)), m_connection(connection) {}
    ~OpenSslStream() override
    {
        const OpenSsl& ssl = openSsl();
        (void)ssl.shutdown(m_connection);
        ssl.sslFree(m_connection);
    }

    [[nodiscard]] std::optional<core::EngineError> send(std::span<const core::u8> data) override
    {
        const OpenSsl& ssl = openSsl();
        setTimeouts(static_cast<int>(m_stream.nativeHandle()), 10000);
        for (std::size_t sent = 0; sent < data.size();) {
            const int wrote = ssl.write(m_connection, data.data() + sent, static_cast<int>(data.size() - sent));
            if (wrote <= 0)
                return core::makeError(ENG_TR("net.err.closed"));
            sent += static_cast<std::size_t>(wrote);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<core::EngineError> receive(std::span<core::u8> out, core::usize& received,
                                                           core::u32 timeoutMs) override
    {
        const OpenSsl& ssl = openSsl();
        received = 0;
        setTimeouts(static_cast<int>(m_stream.nativeHandle()), timeoutMs == 0 ? 1 : timeoutMs);
        const int got = ssl.read(m_connection, out.data(), static_cast<int>(out.size()));
        if (got > 0) {
            received = static_cast<core::usize>(got);
            return std::nullopt;
        }
        const int why = ssl.getError(m_connection, got);
        if (why == SslErrorSyscall && got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return std::nullopt;
        // A read that times out inside TLS reports "want read".
        constexpr int SslErrorWantRead = 2;
        if (why == SslErrorWantRead)
            return std::nullopt;
        return core::makeError(ENG_TR("net.err.closed"));
    }

private:
    TcpStream m_stream;
    void* m_connection = nullptr;
};

} // namespace

std::unique_ptr<TlsStream> openTlsStream(std::string_view host, core::u16 port, core::u32 timeoutMs,
                                         std::optional<core::EngineError>& error)
{
    const OpenSsl& ssl = openSsl();
    const std::string name(host);
    if (!ssl.loaded) {
        const I18nArg args[] = {{"url", name}};
        error = core::makeError(ENG_TR("net.err.https_no_openssl"), args);
        return nullptr;
    }
    void* shared = context();
    TcpStream stream;
    if (auto failed = stream.connect(host, port, timeoutMs); failed.has_value()) {
        error = failed;
        return nullptr;
    }
    if (shared == nullptr) {
        const I18nArg args[] = {{"url", name}, {"code", static_cast<core::i64>(0)}};
        error = core::makeError(ENG_TR("net.err.https_failed"), args);
        return nullptr;
    }
    const int socket = static_cast<int>(stream.nativeHandle());
    setTimeouts(socket, timeoutMs);
    void* connection = ssl.sslNew(shared);
    if (connection == nullptr || ssl.setFd(connection, socket) != 1) {
        if (connection != nullptr)
            ssl.sslFree(connection);
        const I18nArg args[] = {{"url", name}, {"code", static_cast<core::i64>(0)}};
        error = core::makeError(ENG_TR("net.err.https_failed"), args);
        return nullptr;
    }
    (void)ssl.sslCtrl(connection, SslCtrlSetTlsextHostname, TlsextNametypeHostName, const_cast<char*>(name.c_str()));
    (void)ssl.set1Host(connection, name.c_str());
    if (const int shook = ssl.connect(connection); shook != 1) {
        const long verified = ssl.verifyResult(connection);
        const I18nArg args[] = {{"url", name}, {"code", static_cast<core::i64>(ssl.getError(connection, shook))}};
        error = verified != X509VerifyOk ? core::makeError(ENG_TR("net.err.https_untrusted"), args)
                                         : core::makeError(ENG_TR("net.err.https_failed"), args);
        ssl.sslFree(connection);
        return nullptr;
    }
    return std::make_unique<OpenSslStream>(std::move(stream), connection);
}

} // namespace engine::net
