// `net.websocket`'s socket (ADR 0223 §2) against a loopback server: the
// caller's headers in the handshake, text and binary both ways, and a close
// from the server with its code and words. On Windows the socket is WinHTTP's,
// so this proves the platform's client and the protocol agree.
#include <chrono>
#include <doctest/doctest.h>
#include <string>
#include <thread>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/net/websocket_protocol.h"
#include "engine/net/websocket_session.h"
#include "loopback_server.h"

using engine::core::u8;
using engine::core::usize;
using engine::net::WebSocketSession;
using engine::net::testing::Connection;
using engine::net::testing::LoopbackServer;
namespace ws = engine::net::ws;

namespace {

std::string answerHandshake(Connection& connection)
{
    const std::string request = connection.readUntil("\r\n\r\n");
    const std::string marker = "Sec-WebSocket-Key: ";
    const usize at = request.find(marker);
    REQUIRE(at != std::string::npos);
    const usize end = request.find("\r\n", at);
    const std::string key = request.substr(at + marker.size(), end - at - marker.size());
    connection.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: " +
                     ws::computeAccept(key) + "\r\n\r\n");
    return request;
}

std::vector<u8> serverFrame(ws::Opcode opcode, std::string_view payload)
{
    std::vector<u8> out;
    out.push_back(static_cast<u8>(0x80u | static_cast<u8>(opcode)));
    out.push_back(static_cast<u8>(payload.size()));
    for (const char c : payload)
        out.push_back(static_cast<u8>(c));
    return out;
}

std::string readExactly(Connection& connection, usize count)
{
    const std::vector<u8> bytes = connection.read(count);
    return std::string(bytes.begin(), bytes.end());
}

// One client frame read whole, unmasked.
std::pair<ws::Opcode, std::string> clientFrame(Connection& connection)
{
    const std::string head = readExactly(connection, 2);
    const auto opcode = static_cast<ws::Opcode>(static_cast<u8>(head[0]) & 0x0F);
    const usize length = static_cast<u8>(head[1]) & 0x7F;
    REQUIRE(length < 126);
    const std::string mask = readExactly(connection, 4);
    std::string payload = readExactly(connection, length);
    for (usize at = 0; at < payload.size(); ++at)
        payload[at] = static_cast<char>(payload[at] ^ mask[at % 4]);
    return {opcode, payload};
}

template <typename Done>
bool waitFor(Done done)
{
    for (int at = 0; at < 500; ++at) {
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

TEST_CASE("a script's WebSocket opens with its headers, carries text and binary, and closes with the server's word")
{
    REQUIRE(engine::core::engineCatalog().loadFromFile(ENG_TEST_CATALOG));
    LoopbackServer server;
    std::string request;
    std::pair<ws::Opcode, std::string> fromClientText;
    std::pair<ws::Opcode, std::string> fromClientBinary;
    server.serve([&](Connection& connection) {
        request = answerHandshake(connection);
        connection.write(serverFrame(ws::Opcode::Text, "welcome"));
        connection.write(serverFrame(ws::Opcode::Binary, std::string("\x01\x02\x00\x03", 4)));
        fromClientText = clientFrame(connection);
        fromClientBinary = clientFrame(connection);
        connection.write(serverFrame(ws::Opcode::Close, std::string("\x03\xe9", 2) + "bye"));
    });

    std::unique_ptr<WebSocketSession> socket = WebSocketSession::open(
        "ws://127.0.0.1:" + std::to_string(server.port()) + "/chat", {{"Authorization", "Bearer abc"}}, 1u << 20);
    REQUIRE(waitFor([&] { return socket->state() != WebSocketSession::State::Connecting; }));
    const std::optional<engine::core::EngineError> failed = socket->failure();
    if (failed.has_value())
        FAIL(failed->message);

    std::vector<WebSocketSession::Message> arrived;
    REQUIRE(waitFor([&] {
        socket->take(arrived);
        return arrived.size() >= 2;
    }));
    CHECK_FALSE(arrived[0].binary);
    CHECK(arrived[0].bytes == "welcome");
    CHECK(arrived[1].binary);
    CHECK(arrived[1].bytes == std::string("\x01\x02\x00\x03", 4));

    CHECK(socket->send({false, "hello"}));
    CHECK(socket->send({true, std::string("\xff\x00", 2)}));
    REQUIRE(waitFor([&] { return socket->state() == WebSocketSession::State::Closed; }));
    server.join();
    CHECK(server.failure().empty());

    CHECK(request.starts_with("GET /chat HTTP/1.1\r\n"));
    CHECK(request.find("Authorization: Bearer abc\r\n") != std::string::npos);
    CHECK(fromClientText.first == ws::Opcode::Text);
    CHECK(fromClientText.second == "hello");
    CHECK(fromClientBinary.first == ws::Opcode::Binary);
    CHECK(fromClientBinary.second == std::string("\xff\x00", 2));
    CHECK(socket->closeCode() == 1001);
    CHECK(socket->closeReason() == "bye");
    // Closed is closed.
    CHECK_FALSE(socket->send({false, "late"}));
}

TEST_CASE("a WebSocket where nothing listens, or to a URL that is not one, says why and never opens")
{
    REQUIRE(engine::core::engineCatalog().loadFromFile(ENG_TEST_CATALOG));
    std::unique_ptr<WebSocketSession> wrong = WebSocketSession::open("http://example.com/", {}, 1024);
    REQUIRE(waitFor([&] { return wrong->state() == WebSocketSession::State::Closed; }));
    REQUIRE(wrong->failure().has_value());
    CHECK(wrong->failure()->key.hash == ENG_TR("net.err.websocket_url").hash);

    std::unique_ptr<WebSocketSession> nobody = WebSocketSession::open("ws://127.0.0.1:1/", {}, 1024);
    REQUIRE(waitFor([&] { return nobody->state() == WebSocketSession::State::Closed; }));
    CHECK(nobody->failure().has_value());
}
