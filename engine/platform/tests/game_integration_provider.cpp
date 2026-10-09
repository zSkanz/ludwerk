#include "engine/platform/game_integration_abi.h"

namespace {
struct State
{
    bool pending = false;
};
void* create(const char*)
{
    return new State;
}
void destroy(void* state)
{
    delete static_cast<State*>(state);
}
bool available(void*)
{
    return true;
}
bool signedIn(void*)
{
    return false;
}
const char* empty(void*)
{
    return "";
}
const char* begin(void* state, const char*, const char*)
{
    auto& value = *static_cast<State*>(state);
    if (value.pending)
        return "Busy";
    value.pending = true;
    return "";
}
bool poll(void* state, bool* success, const char** reason)
{
    auto& value = *static_cast<State*>(state);
    if (!value.pending)
        return false;
    value.pending = false;
    *success = false;
    *reason = "NotSignedIn";
    return true;
}
const engine::platform::GameIntegrationApi Api{
    engine::platform::GameIntegrationAbi, create, destroy, available, signedIn, empty, empty, begin, poll};
} // namespace
#ifdef _WIN32
#define ENG_TEST_PROVIDER_EXPORT __declspec(dllexport)
#else
#define ENG_TEST_PROVIDER_EXPORT __attribute__((visibility("default")))
#endif
extern "C" ENG_TEST_PROVIDER_EXPORT const engine::platform::GameIntegrationApi* engineGameIntegration()
{
    return &Api;
}
