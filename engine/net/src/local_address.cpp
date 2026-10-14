#include "engine/net/local_address.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
// ws2tcpip.h and iphlpapi.h must follow winsock2.h; the order is not stylistic.
#include <ws2tcpip.h>
// And iphlpapi.h after both.
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#if defined(__ANDROID__)
#include <dlfcn.h>
#endif
#endif

namespace engine::net {

namespace {

[[nodiscard]] std::string lowered(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// The four numbers of a dotted address, or nothing for one that is not.
[[nodiscard]] bool octets(std::string_view address, std::array<core::u32, 4>& out)
{
    core::usize part = 0;
    core::u32 value = 0;
    bool digit = false;
    for (const char c : address) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + static_cast<core::u32>(c - '0');
            if (value > 255)
                return false;
            digit = true;
        }
        else if (c == '.' && digit && part < 3) {
            out[part++] = value;
            value = 0;
            digit = false;
        }
        else {
            return false;
        }
    }
    if (!digit || part != 3)
        return false;
    out[3] = value;
    return true;
}

[[nodiscard]] bool isPrivate(const std::array<core::u32, 4>& o) noexcept
{
    return o[0] == 10 || (o[0] == 192 && o[1] == 168) || (o[0] == 172 && o[1] >= 16 && o[1] <= 31);
}

// **A virtual adapter, by the names the common ones go by**: a hypervisor's
// switch, a container's bridge. Nobody on the Wi-Fi can reach an address on
// one, so offering it would send a friend to type a number that goes nowhere.
[[nodiscard]] bool looksVirtual(const InterfaceAddress& entry)
{
    static constexpr std::string_view Marks[] = {"vethernet", "hyper-v", "wsl",   "docker", "virtualbox",
                                                 "vmware",    "vmnet",   "virbr", "veth",   "br-"};
    const std::string name = lowered(entry.interfaceName);
    const std::string description = lowered(entry.description);
    for (const std::string_view mark : Marks) {
        if (name.find(mark) != std::string::npos || description.find(mark) != std::string::npos)
            return true;
    }
    return false;
}

// A wireless or the first wired interface, by name: what a phone's or a
// laptop's own network is when the system did not say where its default route
// goes.
[[nodiscard]] bool looksPrimary(const InterfaceAddress& entry)
{
    const std::string name = lowered(entry.interfaceName);
    return name.rfind("wlan", 0) == 0 || name == "en0" || name.rfind("wi-fi", 0) == 0;
}

} // namespace

std::vector<std::string> orderLocalAddresses(std::vector<InterfaceAddress> listed)
{
    struct Ranked
    {
        std::string address;
        int rank = 0;
        core::usize order = 0;
    };
    std::vector<Ranked> kept;
    for (core::usize index = 0; index < listed.size(); ++index) {
        InterfaceAddress& entry = listed[index];
        std::array<core::u32, 4> o{};
        if (!entry.up || entry.loopback || !octets(entry.address, o))
            continue;
        if (o[0] == 127 || (o[0] == 169 && o[1] == 254) || o[0] == 0)
            continue;
        if (looksVirtual(entry))
            continue;
        // Lower is better: private before public, then the default route's
        // interface, then a primary-looking one.
        int rank = isPrivate(o) ? 0 : 4;
        if (!entry.defaultRoute)
            rank += 2;
        if (!looksPrimary(entry))
            rank += 1;
        kept.push_back(Ranked{std::move(entry.address), rank, index});
    }
    std::stable_sort(kept.begin(), kept.end(), [](const Ranked& a, const Ranked& b) { return a.rank < b.rank; });

    std::vector<std::string> out;
    for (Ranked& entry : kept) {
        if (std::find(out.begin(), out.end(), entry.address) == out.end())
            out.push_back(std::move(entry.address));
    }
    return out;
}

#if defined(_WIN32)

namespace {

[[nodiscard]] std::string narrow(const wchar_t* text)
{
    if (text == nullptr)
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};
    std::string out(static_cast<core::usize>(length - 1), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    return out;
}

} // namespace

std::vector<std::string> localAddresses()
{
    // **Every adapter, its state and its gateways**, IPv4 only. The gateway is
    // what says which one the default route leaves by.
    constexpr ULONG Flags =
        GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, Flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()),
                                      &size);
    }
    if (result != NO_ERROR)
        return {};

    std::vector<InterfaceAddress> listed;
    for (auto* adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); adapter != nullptr;
         adapter = adapter->Next) {
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            const sockaddr* raw = unicast->Address.lpSockaddr;
            if (raw == nullptr || raw->sa_family != AF_INET)
                continue;
            std::array<char, INET_ADDRSTRLEN> text{};
            const auto* inet = reinterpret_cast<const sockaddr_in*>(raw);
            if (inet_ntop(AF_INET, &inet->sin_addr, text.data(), text.size()) == nullptr)
                continue;
            listed.push_back(InterfaceAddress{
                .address = text.data(),
                .interfaceName = narrow(adapter->FriendlyName),
                .description = narrow(adapter->Description),
                .up = adapter->OperStatus == IfOperStatusUp,
                .loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK,
                .defaultRoute = adapter->FirstGatewayAddress != nullptr,
            });
        }
    }
    return orderLocalAddresses(std::move(listed));
}

#else

namespace {

// The interface the default route leaves by, from the kernel's routing table
// where it can be read (Linux; a phone may refuse it, and then the order falls
// back to the interface's name).
[[nodiscard]] std::string defaultRouteInterface()
{
    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line); // the header
    while (std::getline(routes, line)) {
        std::istringstream fields(line);
        std::string name;
        std::string destination;
        if (fields >> name >> destination && destination == "00000000")
            return name;
    }
    return {};
}

} // namespace

// **`getifaddrs` is in Android's libc from API 24**, past the level this
// build targets, so a phone has it looked up where it runs -- every phone this
// is played on has it -- and an older one answers that it does not know.
#if defined(__ANDROID__)
using GetIfAddrs = int (*)(ifaddrs**);
using FreeIfAddrs = void (*)(ifaddrs*);
#endif

std::vector<std::string> localAddresses()
{
#if defined(__ANDROID__)
    const auto getAddresses = reinterpret_cast<GetIfAddrs>(dlsym(RTLD_DEFAULT, "getifaddrs"));
    const auto freeAddresses = reinterpret_cast<FreeIfAddrs>(dlsym(RTLD_DEFAULT, "freeifaddrs"));
    if (getAddresses == nullptr || freeAddresses == nullptr)
        return {};
#else
    const auto getAddresses = &getifaddrs;
    const auto freeAddresses = &freeifaddrs;
#endif
    ifaddrs* list = nullptr;
    if (getAddresses(&list) != 0)
        return {};
    const std::string routed = defaultRouteInterface();
    std::vector<InterfaceAddress> listed;
    for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET)
            continue;
        char text[INET_ADDRSTRLEN] = {};
        const auto* inet = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
        if (inet_ntop(AF_INET, &inet->sin_addr, text, sizeof text) == nullptr)
            continue;
        const std::string name = entry->ifa_name != nullptr ? entry->ifa_name : "";
        listed.push_back(InterfaceAddress{
            .address = text,
            .interfaceName = name,
            .description = {},
            .up = (entry->ifa_flags & IFF_UP) != 0 && (entry->ifa_flags & IFF_RUNNING) != 0,
            .loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0,
            .defaultRoute = !routed.empty() && name == routed,
        });
    }
    freeAddresses(list);
    return orderLocalAddresses(std::move(listed));
}

#endif

} // namespace engine::net
