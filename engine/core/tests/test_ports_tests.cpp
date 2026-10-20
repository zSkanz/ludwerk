// The tests' ports in this checkout's block (`tests/support/engine_test_ports.h`):
// a port written in a test moves with the block, and nothing else does.
#include <doctest/doctest.h>
#include <string>

#include "engine_test_ports.h"

using engine::testing::port;
using engine::testing::ports;
using engine::testing::PortShift;

TEST_CASE("a test's port is moved by its checkout's block, and only a test's port")
{
    CHECK(PortShift % 1000 == 0);
    CHECK(PortShift <= 5000);
    CHECK(port(47000) == 47000 - PortShift);
    CHECK(port(47999) == 47999 - PortShift);
    CHECK(port(46999) == 46999);
    CHECK(port(48000) == 48000);
    CHECK(port(7777) == 7777);

    const auto moved = [](int written) { return std::to_string(written - PortShift); };
    CHECK(ports("port = 47981\nworld_ports = \"47982-47989\"\n") ==
          "port = " + moved(47981) + "\nworld_ports = \"" + moved(47982) + "-" + moved(47989) + "\"\n");
    CHECK(ports(R"(NetworkService:Join("127.0.0.1:47310"))") ==
          R"(NetworkService:Join("127.0.0.1:)" + moved(47310) + R"("))");
    // A number that only holds 47 and three digits is not one.
    CHECK(ports("147000 470000 4700 48000 7777") == "147000 470000 4700 48000 7777");
    CHECK(ports("").empty());
}
