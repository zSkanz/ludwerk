// `ContentProvider` (ADR 0131 §3): assets loaded before anything shows them.
// A script names content, or instances whose content it wants; the host loads
// it through the same path a prepared scene warms by (ADR 0125) and reports,
// a tick at a time, what has arrived. This is the Luau face and the parking.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/id.h"
#include "engine/core/types.h"

struct lua_State;

namespace engine::script {

// Where one content name stands, as the host answers it.
enum class ContentState : core::u8
{
    Pending,
    Loaded,
    Failed,
};

// One item of a `PreloadAsync` call -- a content name, or an instance and
// every name found in it -- and whether its callback has run.
struct PreloadItem
{
    int itemRef = -1;
    std::vector<std::string> contents;
    bool reported = false;
};

struct PreloadRequest
{
    int threadRef = -1;
    int callbackRef = -1;
    std::vector<PreloadItem> items;
};

struct PreloadState
{
    std::vector<PreloadRequest> requests;
    // Named since the host last asked: what it should start loading.
    std::vector<std::string> wanted;
    // `RequestQueueSize`: names still on their way, as of the last pass.
    core::u32 queued = 0;
};

int contentProviderPreloadAsync(lua_State* L);

// `ContentProvider.RequestQueueSize`, answered by name. False when `key` is
// not it.
bool contentProviderMemberGet(lua_State* L, core::InstanceId id, std::string_view key);

// What scripts asked for since the last call, for the host to start loading.
[[nodiscard]] std::vector<std::string> takePreloadContent(lua_State* L);

// Once a tick: every item whose content has all arrived -- or failed -- has
// its callback run, and every call whose items all have is resumed.
void resumePreloads(lua_State* L, const std::function<ContentState(std::string_view)>& stateOf);

} // namespace engine::script
