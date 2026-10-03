#include <algorithm>
#include <doctest/doctest.h>
#include <string>
#include <vector>

#include "engine/net/local_address.h"

using engine::net::InterfaceAddress;
using engine::net::localAddresses;
using engine::net::orderLocalAddresses;

namespace {

[[nodiscard]] InterfaceAddress on(std::string address, std::string name, bool routed = false)
{
    InterfaceAddress entry;
    entry.address = std::move(address);
    entry.interfaceName = std::move(name);
    entry.defaultRoute = routed;
    return entry;
}

} // namespace

TEST_CASE("G41: the addresses a host shows are the private ones first, its routed interface's first of those")
{
    std::vector<InterfaceAddress> listed;
    listed.push_back(on("8.8.4.4", "Ethernet 2"));               // public, routed below
    listed.push_back(on("10.0.0.7", "Ethernet"));                // private, not routed
    listed.push_back(on("192.168.1.20", "Wi-Fi", true));         // private, the default route
    listed.push_back(on("172.20.5.1", "vEthernet (WSL)", true)); // virtual
    listed.push_back(on("127.0.0.1", "Loopback Pseudo-Interface 1"));
    listed.push_back(on("169.254.3.9", "Ethernet 3")); // no DHCP answer
    InterfaceAddress down = on("192.168.7.7", "Ethernet 4");
    down.up = false;
    listed.push_back(down);
    InterfaceAddress loop = on("10.9.9.9", "lo");
    loop.loopback = true;
    listed.push_back(loop);
    InterfaceAddress docker = on("172.17.0.1", "docker0");
    listed.push_back(docker);
    InterfaceAddress hyperV = on("172.31.0.1", "Ethernet 5");
    hyperV.description = "Hyper-V Virtual Ethernet Adapter";
    listed.push_back(hyperV);
    listed.push_back(on("192.168.1.20", "Wi-Fi", true)); // listed twice

    CHECK(orderLocalAddresses(listed) == std::vector<std::string>{"192.168.1.20", "10.0.0.7", "8.8.4.4"});
}

TEST_CASE("G41: with no default route known, a phone's Wi-Fi comes first")
{
    std::vector<InterfaceAddress> listed;
    listed.push_back(on("10.20.30.40", "rmnet_data0"));
    listed.push_back(on("192.168.0.12", "wlan0"));
    CHECK(orderLocalAddresses(listed) == std::vector<std::string>{"192.168.0.12", "10.20.30.40"});
}

TEST_CASE("G41: this machine's addresses carry no loopback and no link-local one")
{
    const std::vector<std::string> found = localAddresses();
    if (found.empty())
        MESSAGE("no network on this machine: nothing to show, which is the contract offline");
    for (const std::string& address : found) {
        CHECK(address.rfind("127.", 0) != 0);
        CHECK(address.rfind("169.254.", 0) != 0);
        CHECK(std::count(address.begin(), address.end(), '.') == 3);
    }
}
