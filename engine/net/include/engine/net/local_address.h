// **This machine's own addresses on its networks** (G41): what a host shows its
// friends on the same Wi-Fi to type.
//
// A host fact, read when asked and never replicated: two machines' answers are
// about two machines. IPv4 only, because the address a person reads aloud and
// types is the dotted one.
#pragma once

#include <string>
#include <vector>

#include "engine/core/types.h"

namespace engine::net {

// One IPv4 address on one interface, as the operating system reported it.
struct InterfaceAddress
{
    // Dotted, `192.168.1.20`.
    std::string address;
    // The interface's own name (`wlan0`, `Ethernet`) and, where the system has
    // one, its description (`Intel(R) Wi-Fi 6E`): what tells a virtual adapter
    // from a real one.
    std::string interfaceName;
    std::string description;
    // Up and carrying traffic.
    bool up = true;
    bool loopback = false;
    // The interface the default route leaves by: the one a friend's packets
    // come in on.
    bool defaultRoute = false;
};

// **The addresses worth showing, best first**, from what the system listed:
//
//   - only up, non-loopback interfaces, and never a link-local address
//     (169.254/16, which is what an interface with no DHCP answer gives itself);
//   - no virtual adapter that can be told apart by name -- Hyper-V's and
//     WSL's vEthernet, Docker's, VirtualBox's, VMware's, a bridge -- since
//     nobody on the Wi-Fi can reach those;
//   - the private ranges first (192.168/16, 10/8, 172.16/12), and among them
//     the interface with the default route first; then any other, in the
//     order listed;
//   - each address once.
//
// Pure, so the order is a test rather than a hope.
[[nodiscard]] std::vector<std::string> orderLocalAddresses(std::vector<InterfaceAddress> listed);

// What `orderLocalAddresses` makes of this machine's interfaces right now.
// Empty offline, or where the system will not say.
[[nodiscard]] std::vector<std::string> localAddresses();

} // namespace engine::net
