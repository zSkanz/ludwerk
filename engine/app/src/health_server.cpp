#include "engine/app/health_server.h"

#include <array>

#include "engine/app/admin_server.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/net/tcp.h"

namespace engine::app {

struct HealthListener
{
    net::TcpListener listener;
};

namespace {

// How long a request may take to arrive, and how much of it is read.
constexpr core::u32 RequestMs = 2000;
constexpr core::usize MostRequestBytes = 4096;

} // namespace

HealthAnswer answerHealth(std::string_view requestLine, bool ready, const std::string& metrics)
{
    // `GET /path HTTP/1.1`: the method, then the path up to a query.
    const std::size_t space = requestLine.find(' ');
    if (space == std::string_view::npos)
        return HealthAnswer{404, "{}"};
    const std::string_view method = requestLine.substr(0, space);
    std::string_view path = requestLine.substr(space + 1);
    path = path.substr(0, path.find(' '));
    path = path.substr(0, path.find('?'));
    if (method != "GET" && method != "HEAD")
        return HealthAnswer{405, "{}"};
    // **Ready is what a balancer asks**: 503 while it starts or stops, so
    // nobody is sent to a world that would refuse them.
    if (path == "/health")
        return HealthAnswer{static_cast<core::u16>(ready ? 200 : 503),
                            ready ? "{\"ready\":true}" : "{\"ready\":false}"};
    if (path == "/metrics")
        return HealthAnswer{200, metrics};
    return HealthAnswer{404, "{}"};
}

HealthServer::HealthServer() = default;

HealthServer::~HealthServer()
{
    stop();
}

std::optional<core::EngineError> HealthServer::start(Options options)
{
    m_options = std::move(options);
    auto listener = std::make_unique<HealthListener>();
    if (auto error = listener->listener.open(m_options.port, !m_options.allow.empty()); error.has_value())
        return error;
    m_listener = std::move(listener);
    const std::array<core::I18nArg, 1> args{core::I18nArg{"port", static_cast<core::i64>(m_options.port)}};
    core::log(core::LogLevel::Info, ENG_TR("health.info.listening"), args);
    m_acceptor = std::thread([this] { acceptLoop(); });
    return std::nullopt;
}

void HealthServer::stop()
{
    if (m_stopping.exchange(true))
        return;
    if (m_acceptor.joinable())
        m_acceptor.join();
    if (m_listener != nullptr)
        m_listener->listener.close();
}

void HealthServer::publish(bool ready, std::string metrics)
{
    const std::lock_guard lock(m_mutex);
    m_ready = ready;
    m_metrics = std::move(metrics);
}

void HealthServer::acceptLoop()
{
    while (!m_stopping) {
        std::string address;
        std::optional<net::TcpStream> stream = m_listener->listener.accept(200, address);
        if (!stream.has_value())
            continue;
        // **Closed before a byte of it is read**, from where it may not come.
        if (!addressAllowed(address, m_options.allow)) {
            stream->close();
            continue;
        }
        std::string request;
        std::array<core::u8, 1024> chunk{};
        while (request.find("\r\n") == std::string::npos && request.size() < MostRequestBytes) {
            core::usize received = 0;
            if (stream->receive(chunk, received, RequestMs).has_value() || received == 0)
                break;
            request.append(reinterpret_cast<const char*>(chunk.data()), received);
        }
        const std::size_t end = request.find("\r\n");
        if (end == std::string::npos) {
            stream->close();
            continue;
        }
        HealthAnswer answer;
        {
            const std::lock_guard lock(m_mutex);
            answer = answerHealth(std::string_view{request}.substr(0, end), m_ready, m_metrics);
        }
        const bool head = request.starts_with("HEAD ");
        // The status line with its code and no reason phrase, which HTTP/1.1
        // allows and every client reads by the code: the words are for a person
        // and nobody reads them here.
        constexpr std::string_view Line = "\r\n";
        std::string response = "HTTP/1.1 " + std::to_string(answer.status) + " ";
        for (const std::string_view header :
             {std::string_view{"Content-Type: application/json"}, std::string_view{"Cache-Control: no-store"},
              std::string_view{"Connection: close"}}) {
            response += Line;
            response += header;
        }
        response += Line;
        response += "Content-Length: " + std::to_string(answer.body.size());
        response += Line;
        response += Line;
        if (!head)
            response += answer.body;
        (void)stream->send(
            std::span<const core::u8>(reinterpret_cast<const core::u8*>(response.data()), response.size()));
        stream->close();
    }
}

} // namespace engine::app
