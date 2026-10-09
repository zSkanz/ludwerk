// UWP's maintained TLS/HTTP stack; desktop WinHTTP is not an AppContainer API.
#include <array>
#include <chrono>

#include "http_internal.h"
#pragma warning(push)
#pragma warning(disable : 4265)
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Filters.h>
#include <winrt/Windows.Web.Http.Headers.h>
#include <winrt/Windows.Web.Http.h>
#pragma warning(pop)

namespace engine::net {
namespace W = winrt::Windows;
namespace {
struct Apartment
{
    Apartment() { winrt::init_apartment(); }
    ~Apartment() { winrt::uninit_apartment(); }
};
template <typename T>
bool completed(const T& operation, Deadline deadline)
{
    if (operation.wait_for(std::chrono::milliseconds(msLeft(deadline))) == W::Foundation::AsyncStatus::Started) {
        operation.Cancel();
        return false;
    }
    return true;
}
} // namespace
std::optional<core::EngineError> performHttps(const HttpRequest& request, const ParsedUrl&, HttpResponse& response)
{
    try {
        Apartment apartment;
        const auto deadline = deadlineAfter(request.timeoutMs);
        W::Web::Http::Filters::HttpBaseProtocolFilter filter;
        filter.AllowAutoRedirect(false);
        filter.CookieUsageBehavior(W::Web::Http::Filters::HttpCookieUsageBehavior::NoCookies);
        W::Web::Http::HttpClient client(filter);
        W::Web::Http::HttpRequestMessage message(W::Web::Http::HttpMethod(winrt::to_hstring(request.method)),
                                                 W::Foundation::Uri(winrt::to_hstring(request.url)));
        if (!request.body.empty()) {
            const auto bytes = winrt::array_view<const core::u8>(
                reinterpret_cast<const core::u8*>(request.body.data()),
                reinterpret_cast<const core::u8*>(request.body.data()) + request.body.size());
            message.Content(W::Web::Http::HttpBufferContent(
                W::Security::Cryptography::CryptographicBuffer::CreateFromByteArray(bytes)));
        }
        for (const auto& header : request.headers) {
            if (auto error = checkHeader(header))
                return error;
            if (_stricmp(header.name.c_str(), "host") == 0)
                continue;
            const auto name = winrt::to_hstring(header.name), value = winrt::to_hstring(header.value);
            if (!message.Headers().TryAppendWithoutValidation(name, value)) {
                if (!message.Content())
                    message.Content(W::Web::Http::HttpBufferContent(W::Storage::Streams::Buffer(0u)));
                (void)message.Content().Headers().TryAppendWithoutValidation(name, value);
            }
        }
        const auto operation =
            client.SendRequestAsync(message, W::Web::Http::HttpCompletionOption::ResponseHeadersRead);
        if (!completed(operation, deadline))
            return core::makeError(ENG_TR("net.err.http_response_timeout"));
        const auto result = operation.GetResults();
        response.statusCode = static_cast<u16>(result.StatusCode());
        response.statusMessage = winrt::to_string(result.ReasonPhrase());
        for (const auto& field : result.Headers())
            response.headers.push_back({winrt::to_string(field.Key()), winrt::to_string(field.Value())});
        for (const auto& field : result.Content().Headers())
            response.headers.push_back({winrt::to_string(field.Key()), winrt::to_string(field.Value())});
        const auto opening = result.Content().ReadAsInputStreamAsync();
        if (!completed(opening, deadline))
            return core::makeError(ENG_TR("net.err.http_response_timeout"));
        const auto stream = opening.GetResults();
        while (true) {
            const auto read = stream.ReadAsync(W::Storage::Streams::Buffer(8192), 8192,
                                               W::Storage::Streams::InputStreamOptions::Partial);
            if (!completed(read, deadline))
                return core::makeError(ENG_TR("net.err.http_response_timeout"));
            const auto buffer = read.GetResults();
            if (!buffer.Length())
                break;
            if (response.body.size() > request.maxBodyBytes ||
                buffer.Length() > request.maxBodyBytes - response.body.size()) {
                const core::I18nArg args[] = {{"limit", static_cast<core::i64>(request.maxBodyBytes)}};
                return core::makeError(ENG_TR("net.err.http_body_too_large"), args);
            }
            std::array<core::u8, 8192> bytes{};
            W::Storage::Streams::DataReader::FromBuffer(buffer).ReadBytes(
                winrt::array_view<core::u8>(bytes.data(), bytes.data() + buffer.Length()));
            response.body.append(reinterpret_cast<const char*>(bytes.data()), buffer.Length());
        }
        response.ok = true;
        return std::nullopt;
    } catch (const winrt::hresult_error& error) {
        const auto code = static_cast<core::i64>(error.code().value);
        const core::I18nArg args[] = {{"url", request.url}, {"code", code}};
        return core::makeError(ENG_TR("net.err.https_failed"), args, winrt::to_string(error.message()));
    }
}
} // namespace engine::net
