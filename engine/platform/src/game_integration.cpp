#include "engine/platform/game_integration.h"

#ifndef _WIN32
#include <SDL3/SDL_loadso.h>
#endif
#include <filesystem>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include "engine/platform/game_integration_abi.h"
namespace engine::platform {
namespace {
#ifdef _WIN32
using SharedLibrary = HMODULE;
SharedLibrary openLibrary(const std::filesystem::path& path)
{
    // Dependencies are resolved beside the trusted provider, never in the project cwd.
#if ENG_PLATFORM_UWP
    return LoadPackagedLibrary(path.c_str(), 0);
#else
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#endif
}
void closeLibrary(SharedLibrary library)
{
    FreeLibrary(library);
}
GameIntegrationEntry entryPoint(SharedLibrary library)
{
    return reinterpret_cast<GameIntegrationEntry>(GetProcAddress(library, "engineGameIntegration"));
}
GameIntegrationCapabilitiesEntry capabilitiesEntry(SharedLibrary library)
{
    return reinterpret_cast<GameIntegrationCapabilitiesEntry>(
        GetProcAddress(library, "engineGameIntegrationCapabilities"));
}
#else
using SharedLibrary = SDL_SharedObject*;
SharedLibrary openLibrary(const std::filesystem::path& path)
{
    return SDL_LoadObject(path.c_str());
}
void closeLibrary(SharedLibrary library)
{
    SDL_UnloadObject(library);
}
GameIntegrationEntry entryPoint(SharedLibrary library)
{
    return reinterpret_cast<GameIntegrationEntry>(SDL_LoadFunction(library, "engineGameIntegration"));
}
GameIntegrationCapabilitiesEntry capabilitiesEntry(SharedLibrary library)
{
    return reinterpret_cast<GameIntegrationCapabilitiesEntry>(
        SDL_LoadFunction(library, "engineGameIntegrationCapabilities"));
}
#endif
class LoadedIntegration final : public GameIntegration
{
public:
    LoadedIntegration(SharedLibrary library, const GameIntegrationApi* api, void* provider,
                      const GameIntegrationCapabilitiesApi* capabilities)
        : m_library(library), m_api(api), m_provider(provider), m_capabilities(capabilities)
    {}
    ~LoadedIntegration() override
    {
        m_api->destroy(m_provider);
        closeLibrary(m_library);
    }
    bool available() const override { return m_api->available(m_provider); }
    bool signedIn() const override { return m_api->signedIn(m_provider); }
    bool supports(std::string_view operation) const override
    {
        return m_capabilities && m_capabilities->supports(m_provider, std::string(operation).c_str());
    }
    std::string response() const override
    {
        if (!m_capabilities)
            return {};
        const char* value = m_capabilities->response(m_provider);
        return value ? value : "";
    }
    IntegrationUser user() const override { return {m_api->userId(m_provider), m_api->userName(m_provider)}; }
    std::string begin(std::string_view operation, std::string_view argument) override
    {
        const std::string op(operation), arg(argument);
        return m_api->begin(m_provider, op.c_str(), arg.c_str());
    }
    bool poll(bool& success, std::string& failure) override
    {
        const char* reason = "";
        const bool complete = m_api->poll(m_provider, &success, &reason);
        if (complete)
            failure = reason;
        return complete;
    }

private:
    SharedLibrary m_library;
    const GameIntegrationApi* m_api;
    void* m_provider;
    const GameIntegrationCapabilitiesApi* m_capabilities;
};
} // namespace
std::unique_ptr<GameIntegration> loadGameIntegration(const std::string& library, const std::string& configuration)
{
    if (!std::filesystem::path(library).is_absolute())
        return {};
    SharedLibrary loaded = openLibrary(std::filesystem::path(library));
    if (loaded == nullptr)
        return {};
    const auto entry = entryPoint(loaded);
    const GameIntegrationApi* api = entry != nullptr ? entry() : nullptr;
    if (api == nullptr || api->version != GameIntegrationAbi || !api->create || !api->destroy || !api->available ||
        !api->signedIn || !api->userId || !api->userName || !api->begin || !api->poll) {
        closeLibrary(loaded);
        return {};
    }
    void* provider = api->create(configuration.c_str());
    if (!provider) {
        closeLibrary(loaded);
        return {};
    }
    const auto extension = capabilitiesEntry(loaded);
    const auto* capabilities = extension ? extension() : nullptr;
    if (capabilities && (capabilities->version != 1 || !capabilities->supports || !capabilities->response))
        capabilities = nullptr;
    return std::make_unique<LoadedIntegration>(loaded, api, provider, capabilities);
}
} // namespace engine::platform
