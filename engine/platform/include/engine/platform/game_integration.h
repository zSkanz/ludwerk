// Optional native game integrations. No SDK type crosses this boundary (ADR 0188).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "engine/core/types.h"
namespace engine::platform {
struct PlatformServiceConfiguration
{
    // Service key -> provider ID, or Auto (the default).
    std::map<std::string, std::string, std::less<>> providers;
    // Service key/provider ID -> project ID -> native ID.
    std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> ids;
};
struct IntegrationUser
{
    std::string id;
    std::string name;
};
class GameIntegration
{
public:
    virtual ~GameIntegration() = default;
    virtual bool available() const = 0;
    virtual bool signedIn() const = 0;
    virtual IntegrationUser user() const = 0;
    // Empty means accepted; otherwise a stable failure code, not display text.
    virtual std::string begin(std::string_view operation, std::string_view argument) = 0;
    virtual bool poll(bool& success, std::string& failure) = 0;
    // Optional extension: older providers keep working through XboxService but
    // do not advertise capabilities they never declared.
    virtual bool supports(std::string_view) const { return false; }
    // UTF-8 JSON for data-returning operations; empty for boolean actions.
    virtual std::string response() const { return {}; }
};
// Absolute path supplied by the host, never a name searched on PATH or cwd.
[[nodiscard]] std::unique_ptr<GameIntegration> loadGameIntegration(const std::string& library,
                                                                   const std::string& configuration);
} // namespace engine::platform
