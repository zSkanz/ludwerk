// Versioned C ABI shared with optional providers; no STL ownership crosses a DLL.
#pragma once
#include <cstdint>
namespace engine::platform {
inline constexpr std::uint32_t GameIntegrationAbi = 1;
struct GameIntegrationApi
{
    std::uint32_t version;
    void* (*create)(const char* configuration);
    void (*destroy)(void*);
    bool (*available)(void*);
    bool (*signedIn)(void*);
    const char* (*userId)(void*);
    const char* (*userName)(void*);
    const char* (*begin)(void*, const char* operation, const char* argument);
    bool (*poll)(void*, bool* success, const char** failure);
};
using GameIntegrationEntry = const GameIntegrationApi* (*)();
// Separately discovered symbol; the v1 function table and old DLLs stay valid.
// begin's argument for new operations is a JSON request; response is borrowed
// UTF-8 JSON, valid until the next begin/poll. Identity/Unlock retain v1 arguments.
struct GameIntegrationCapabilitiesApi
{
    std::uint32_t version;
    bool (*supports)(void*, const char* operation);
    const char* (*response)(void*);
};
using GameIntegrationCapabilitiesEntry = const GameIntegrationCapabilitiesApi* (*)();
} // namespace engine::platform
