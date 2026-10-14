// `https://` on Windows: WinHTTP (ADR 0063).
//
// WinHTTP is the TLS and the HTTP in one API, over Schannel, verifying against
// the machine's certificate store. One session serves the process, and WinHTTP
// keeps connections to a host alive inside it between requests. Redirects are
// turned off here because `performHttp` follows them itself, by one rule for
// every platform.

#include <array>
#include <string>
#include <string_view>
#include <windows.h>
#include <winhttp.h>

#include "engine/core/i18n.h"
#include "http_internal.h"

namespace engine::net {
namespace {

using core::I18nArg;

[[nodiscard]] std::wstring widen(std::string_view text)
{
    if (text.empty())
        return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

[[nodiscard]] std::string narrow(std::wstring_view text)
{
    if (text.empty())
        return {};
    const int length =
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

// Closes a WinHTTP handle when it goes out of scope.
struct Handle
{
    HINTERNET value = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle()
    {
        if (value != nullptr)
            ::WinHttpCloseHandle(value);
    }
};

// The process's session, made once. Never closed: it lives as long as a
// request might be made, which is the life of the process.
[[nodiscard]] HINTERNET session()
{
    static const HINTERNET made = ::WinHttpOpen(L"engine", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                                WINHTTP_NO_PROXY_BYPASS, 0);
    return made;
}

// What went wrong, named: a certificate this machine does not trust is not the
// same failure as a host that does not answer.
[[nodiscard]] core::EngineError failure(DWORD code, const std::string& url)
{
    switch (code) {
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE: {
        const I18nArg args[] = {{"url", url}};
        return core::makeError(ENG_TR("net.err.https_untrusted"), args);
    }
    case ERROR_WINHTTP_TIMEOUT:
        return core::makeError(ENG_TR("net.err.http_response_timeout"));
    default: {
        const I18nArg args[] = {{"url", url}, {"code", static_cast<core::i64>(code)}};
        return core::makeError(ENG_TR("net.err.https_failed"), args);
    }
    }
}

[[nodiscard]] std::wstring queryText(HINTERNET request, DWORD what)
{
    DWORD bytes = 0;
    (void)::WinHttpQueryHeaders(request, what, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
                                WINHTTP_NO_HEADER_INDEX);
    if (bytes == 0)
        return {};
    std::wstring out(bytes / sizeof(wchar_t), L'\0');
    if (!::WinHttpQueryHeaders(request, what, WINHTTP_HEADER_NAME_BY_INDEX, out.data(), &bytes,
                               WINHTTP_NO_HEADER_INDEX))
        return {};
    out.resize(bytes / sizeof(wchar_t));
    return out;
}

} // namespace

std::optional<core::EngineError> performHttps(const HttpRequest& request, const ParsedUrl& url, HttpResponse& response)
{
    if (session() == nullptr)
        return failure(::GetLastError(), request.url);

    std::wstring headers;
    for (const HttpHeader& header : request.headers) {
        if (auto error = checkHeader(header); error.has_value())
            return error;
        // The host is the URL's, for the reason the socket client gives.
        if (_stricmp(header.name.c_str(), "host") == 0)
            continue;
        headers.append(widen(header.name)).append(L": ").append(widen(header.value)).append(L"\r\n");
    }

    const Handle connection(::WinHttpConnect(session(), widen(url.host).c_str(), url.port, 0));
    if (connection.value == nullptr)
        return failure(::GetLastError(), request.url);
    const Handle exchange(::WinHttpOpenRequest(connection.value, widen(request.method).c_str(),
                                               widen(url.target).c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (exchange.value == nullptr)
        return failure(::GetLastError(), request.url);

    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    (void)::WinHttpSetOption(exchange.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
    const Deadline deadline = deadlineAfter(request.timeoutMs);
    const int timeout = static_cast<int>(request.timeoutMs);
    (void)::WinHttpSetTimeouts(exchange.value, timeout, timeout, timeout, timeout);

    const auto bodyLength = static_cast<DWORD>(request.body.size());
    if (!::WinHttpSendRequest(exchange.value, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                              headers.empty() ? 0 : static_cast<DWORD>(-1L),
                              request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data()),
                              bodyLength, bodyLength, 0) ||
        !::WinHttpReceiveResponse(exchange.value, nullptr))
        return failure(::GetLastError(), request.url);

    DWORD status = 0;
    DWORD statusBytes = sizeof(status);
    if (!::WinHttpQueryHeaders(exchange.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX))
        return failure(::GetLastError(), request.url);
    response.statusCode = static_cast<u16>(status);
    response.statusMessage = narrow(queryText(exchange.value, WINHTTP_QUERY_STATUS_TEXT));

    // The headers as the server sent them, a line each; the first is the
    // status line.
    const std::string raw = narrow(queryText(exchange.value, WINHTTP_QUERY_RAW_HEADERS_CRLF));
    std::string_view fields = raw;
    if (const std::size_t first = fields.find("\r\n"); first != std::string_view::npos)
        fields.remove_prefix(first + 2);
    while (!fields.empty()) {
        const std::size_t end = fields.find("\r\n");
        const std::string_view line = fields.substr(0, end);
        if (const std::size_t colon = line.find(':'); colon != std::string_view::npos) {
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ')
                value.remove_prefix(1);
            response.headers.push_back({std::string(line.substr(0, colon)), std::string(value)});
        }
        if (end == std::string_view::npos)
            break;
        fields.remove_prefix(end + 2);
    }

    // The body, decoded from chunks by WinHTTP, and bounded as the socket
    // client's is.
    std::array<char, 8192> chunk{};
    while (true) {
        // WinHTTP's timeouts are a read's; the request's is checked between
        // them, so a stall costs at most one more read past it.
        if (msLeft(deadline) == 0)
            return core::makeError(ENG_TR("net.err.http_response_timeout"));
        DWORD read = 0;
        if (!::WinHttpReadData(exchange.value, chunk.data(), static_cast<DWORD>(chunk.size()), &read))
            return failure(::GetLastError(), request.url);
        if (read == 0)
            break;
        if (response.body.size() + read > request.maxBodyBytes) {
            const I18nArg args[] = {{"limit", static_cast<core::i64>(request.maxBodyBytes)}};
            return core::makeError(ENG_TR("net.err.http_body_too_large"), args);
        }
        response.body.append(chunk.data(), read);
    }

    response.ok = true;
    return std::nullopt;
}

} // namespace engine::net
