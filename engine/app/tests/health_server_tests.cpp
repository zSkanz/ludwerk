// **A world's health** (ADR 0220): what `/health` and `/metrics` answer, and
// the answer arriving over loopback TCP as a probe would ask it.
#include <array>
#include <doctest/doctest.h>
#include <string>

#include "engine/app/health_server.h"
#include "engine/net/tcp.h"

using namespace engine;

namespace {

// Test ports are 47xxx, never a game's.
constexpr core::u16 HealthTestPort = 47831;

std::string ask(core::u16 port, std::string_view request)
{
    net::TcpStream stream;
    REQUIRE_FALSE(stream.connect("127.0.0.1", port, 2000).has_value());
    REQUIRE_FALSE(
        stream.send(std::span<const core::u8>(reinterpret_cast<const core::u8*>(request.data()), request.size()))
            .has_value());
    std::string answer;
    std::array<core::u8, 2048> chunk{};
    while (true) {
        core::usize received = 0;
        if (stream.receive(chunk, received, 2000).has_value() || received == 0)
            break;
        answer.append(reinterpret_cast<const char*>(chunk.data()), received);
    }
    return answer;
}

} // namespace

TEST_CASE("health is ready or not, metrics are the world's numbers, and nothing else is answered")
{
    const std::string metrics = "{\"players\":3}";
    CHECK(app::answerHealth("GET /health HTTP/1.1", true, metrics).status == 200);
    CHECK(app::answerHealth("GET /health HTTP/1.1", true, metrics).body == "{\"ready\":true}");
    // **Starting or stopping is 503**, so a balancer sends nobody there.
    CHECK(app::answerHealth("GET /health HTTP/1.1", false, metrics).status == 503);
    CHECK(app::answerHealth("GET /health?verbose=1 HTTP/1.0", true, metrics).status == 200);
    CHECK(app::answerHealth("GET /metrics HTTP/1.1", false, metrics).body == metrics);
    CHECK(app::answerHealth("HEAD /metrics HTTP/1.1", true, metrics).status == 200);
    CHECK(app::answerHealth("POST /health HTTP/1.1", true, metrics).status == 405);
    CHECK(app::answerHealth("GET /console HTTP/1.1", true, metrics).status == 404);
    CHECK(app::answerHealth("nonsense", true, metrics).status == 404);
}

TEST_CASE("a probe over TCP is answered with what the frame loop last published")
{
    app::HealthServer server;
    REQUIRE_FALSE(server.start(app::HealthServer::Options{HealthTestPort, {}}).has_value());

    // Before the first frame has said anything, the world is not ready.
    std::string answer = ask(HealthTestPort, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(answer.starts_with("HTTP/1.1 503 "));

    server.publish(true, "{\"players\":7,\"state\":\"running\"}");
    answer = ask(HealthTestPort, "GET /health HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(answer.starts_with("HTTP/1.1 200 \r\n"));
    CHECK(answer.find("Content-Type: application/json") != std::string::npos);
    CHECK(answer.ends_with("{\"ready\":true}"));

    answer = ask(HealthTestPort, "GET /metrics HTTP/1.1\r\n\r\n");
    CHECK(answer.ends_with("{\"players\":7,\"state\":\"running\"}"));
    server.stop();
}
