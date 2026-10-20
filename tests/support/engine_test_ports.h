// `port` and `ports` -- a test's port, in this checkout's block.
//
// Every port a test writes is in 47000-47999; `cmake/engine_test_ports.cmake`
// gives each checkout on a machine a block of its own and moves them all down
// by the same thousand, so two checkouts' suites run at once without taking
// each other's ports. A test writes the port as it always has and asks for it
// here: `port(47310)` for a number, `ports(text)` for a project's file or a
// script that names ports -- a `servers.toml`, a `Join("127.0.0.1:47981")`.
#pragma once

#include <string>
#include <string_view>

#include "engine/core/types.h"

#ifndef ENG_TEST_PORT_SHIFT
#define ENG_TEST_PORT_SHIFT 0
#endif

namespace engine::testing {

inline constexpr core::u16 PortShift = ENG_TEST_PORT_SHIFT;

[[nodiscard]] constexpr core::u16 port(core::u16 written) noexcept
{
    return written >= 47000 && written <= 47999 ? static_cast<core::u16>(written - PortShift) : written;
}

// Every number of five digits from 47000 to 47999 in `text`, standing alone,
// moved as `port` moves it.
[[nodiscard]] inline std::string ports(std::string_view text)
{
    std::string out(text);
    if (PortShift == 0)
        return out;
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    for (std::size_t at = 0; at + 5 <= out.size(); ++at) {
        if (out[at] != '4' || out[at + 1] != '7' || (at > 0 && digit(out[at - 1])))
            continue;
        if (!digit(out[at + 2]) || !digit(out[at + 3]) || !digit(out[at + 4]))
            continue;
        if (at + 5 < out.size() && digit(out[at + 5]))
            continue;
        const auto written = static_cast<core::u16>(std::stoi(out.substr(at, 5)));
        out.replace(at, 5, std::to_string(port(written)));
        at += 4;
    }
    return out;
}

} // namespace engine::testing
