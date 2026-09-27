// `https://` from the platform, and redirects (ADR 0063, the game-ready plan's
// A1). Nothing here needs the public internet: the TLS server is on loopback,
// with a certificate made for the test (`tls_test_server.h`); a nightly job
// asks a public endpoint.
#include <doctest/doctest.h>
#include <string>

#include "engine/core/i18n.h"
#include "engine/net/http.h"
#include "loopback_server.h"
#include "tls_test_server.h"

using namespace engine;
using namespace engine::net;

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__)
// Compiled into a build with tests only (`ENG_NET_TEST_HOOKS`).
namespace engine::net::testing {
void trustExtraRoot(const std::string& pemFile);
}
#define ENG_TEST_CAN_TRUST_A_ROOT 1
#endif

namespace {

void seedCatalog()
{
    const auto result = core::engineCatalog().loadFromFile(ENG_TEST_CATALOG);
    REQUIRE_MESSAGE(result.ok, result.diagnostic);
}

[[nodiscard]] std::string resolved(std::string_view from, std::string_view location)
{
    std::string out;
    REQUIRE_FALSE(resolveRedirect(from, location, out).has_value());
    return out;
}

} // namespace

TEST_CASE("an https URL is parsed on port 443, and nothing is downgraded")
{
    seedCatalog();
    ParsedUrl url;
    REQUIRE_FALSE(parseUrl("https://example.test/a?b=1", url).has_value());
    CHECK(url.scheme == "https");
    CHECK(url.port == 443);
    CHECK(url.target == "/a?b=1");
}

TEST_CASE("a redirect's location is resolved the way a browser resolves it")
{
    seedCatalog();
    const std::string from = "http://127.0.0.1:8080/shop/cart?item=3";
    CHECK(resolved(from, "https://other.test/x") == "https://other.test/x");
    CHECK(resolved(from, "//cdn.test/y") == "http://cdn.test/y");
    CHECK(resolved(from, "/login") == "http://127.0.0.1:8080/login");
    CHECK(resolved(from, "?item=4") == "http://127.0.0.1:8080/shop/cart?item=4");
    CHECK(resolved(from, "checkout") == "http://127.0.0.1:8080/shop/checkout");
    // The default port is not written out.
    CHECK(resolved("https://a.test/x/y", "z") == "https://a.test/x/z");

    std::string out;
    const auto error = resolveRedirect(from, "", out);
    REQUIRE(error.has_value());
    CHECK(error->message.find("net.err.http_redirect_malformed") != std::string::npos);
}

TEST_CASE("a redirect is followed, and a POST answered with 303 becomes a GET")
{
    seedCatalog();
    testing::LoopbackServer server;
    std::string second;
    server.serveEach(2, [&second](testing::Connection& connection, int index) {
        const std::string head = connection.readUntil("\r\n\r\n");
        if (index == 0) {
            if (head.rfind("POST /form HTTP/1.1\r\n", 0) != 0)
                throw std::runtime_error("the first request is not the POST: " + head);
            (void)connection.read(5);
            connection.write("HTTP/1.1 303 See Other\r\nLocation: /done\r\nConnection: close\r\n\r\n");
        }
        else {
            second = head;
            connection.write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nthanks");
        }
        connection.close();
    });

    HttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/form";
    request.method = "POST";
    request.body = "a=b&c";
    request.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
    request.timeoutMs = 5000;
    HttpResponse response;
    const auto error = performHttp(request, response);
    server.join();
    CHECK(server.failure().empty());

    REQUIRE_FALSE(error.has_value());
    CHECK(response.statusCode == 200);
    CHECK(response.body == "thanks");
    CHECK(second.rfind("GET /done HTTP/1.1\r\n", 0) == 0);
    CHECK(second.find("Content-Type") == std::string::npos);
    CHECK(second.find("Content-Length") == std::string::npos);
}

TEST_CASE("a request that keeps being redirected stops after five")
{
    seedCatalog();
    testing::LoopbackServer server;
    server.serveEach(static_cast<int>(MaxRedirects) + 1, [](testing::Connection& connection, int) {
        (void)connection.readUntil("\r\n\r\n");
        connection.write("HTTP/1.1 302 Found\r\nLocation: /again\r\nConnection: close\r\n\r\n");
        connection.close();
    });

    HttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/start";
    request.timeoutMs = 5000;
    HttpResponse response;
    const auto error = performHttp(request, response);
    server.join();
    CHECK(server.failure().empty());
    REQUIRE(error.has_value());
    CHECK(error->message.find("net.err.http_too_many_redirects") != std::string::npos);
    CHECK_FALSE(response.ok);
}

TEST_CASE("a certificate this device does not trust is refused, and nothing is sent")
{
    seedCatalog();
    const testing::TlsIdentity identity;
    REQUIRE_MESSAGE(identity.ok(), "could not make a test certificate");

    testing::LoopbackServer server;
    std::string received;
    server.serve([&](testing::Connection& connection) {
        (void)testing::serveTls(identity, connection.raw(), "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nno",
                                &received);
    });

    HttpRequest request;
    request.url = "https://127.0.0.1:" + std::to_string(server.port()) + "/secret";
    request.headers.push_back({"Authorization", "Bearer do-not-send"});
    request.timeoutMs = 5000;
    HttpResponse response;
    const auto error = performHttp(request, response);
    server.join();

    REQUIRE(error.has_value());
    CHECK(error->message.find("net.err.https_untrusted") != std::string::npos);
    CHECK_FALSE(response.ok);
    // The request never reached the server.
    CHECK(received.find("Bearer") == std::string::npos);
}

#if ENG_TEST_CAN_TRUST_A_ROOT
TEST_CASE("a certificate from a root the device trusts is accepted, and a redirect from it to http is refused")
{
    seedCatalog();
    const testing::TlsIdentity identity;
    REQUIRE_MESSAGE(identity.ok(), "could not make a test certificate (is the openssl command installed?)");
    testing::trustExtraRoot(identity.certificateFile());

    SUBCASE("the request goes, and the answer comes back")
    {
        testing::LoopbackServer server;
        std::string received;
        server.serve([&](testing::Connection& connection) {
            (void)testing::serveTls(identity, connection.raw(),
                                    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nsecure",
                                    &received);
        });
        HttpRequest request;
        request.url = "https://127.0.0.1:" + std::to_string(server.port()) + "/hello";
        request.timeoutMs = 5000;
        HttpResponse response;
        const auto error = performHttp(request, response);
        server.join();
        REQUIRE_FALSE(error.has_value());
        CHECK(response.ok);
        CHECK(response.statusCode == 200);
        CHECK(response.body == "secure");
        CHECK(received.rfind("GET /hello HTTP/1.1\r\n", 0) == 0);
        CHECK(received.find("Host: 127.0.0.1:") != std::string::npos);
    }

    SUBCASE("a redirect from https to http is not followed")
    {
        testing::LoopbackServer server;
        server.serve([&](testing::Connection& connection) {
            (void)testing::serveTls(
                identity, connection.raw(),
                "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/leak\r\nConnection: close\r\n\r\n", nullptr);
        });
        HttpRequest request;
        request.url = "https://127.0.0.1:" + std::to_string(server.port()) + "/";
        request.timeoutMs = 5000;
        HttpResponse response;
        const auto error = performHttp(request, response);
        server.join();
        REQUIRE(error.has_value());
        CHECK(error->message.find("net.err.http_redirect_downgrade") != std::string::npos);
    }

    testing::trustExtraRoot({});
}
#endif
