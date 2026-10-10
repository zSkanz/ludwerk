#include "engine/script/content_provider.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <string>
#include <vector>

#include "class_descriptors.gen.h"
#include "engine/core/i18n.h"
#include "engine/core/text_key.h"
#include "engine/scene/scene_file.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/datatypes.h"
#include "engine/script/instance_binding.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

[[nodiscard]] PreloadState& preloads(lua_State* L) noexcept
{
    return context(L).services->preloads;
}

[[nodiscard]] scene::World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

void addUnique(std::vector<std::string>& names, std::string_view name)
{
    if (std::find(names.begin(), names.end(), name) == names.end())
        names.emplace_back(name);
}

// Every content name an instance and its descendants hold: any text property
// that names an asset. A walk over the properties rather than a list of which
// ones are content, so a class added later is covered without a line here.
void collectContent(scene::World& w, core::InstanceId root, std::vector<std::string>& out)
{
    std::vector<core::InstanceId> instances{root};
    w.collectDescendants(root, instances);
    for (const core::InstanceId id : instances) {
        for (const scene::ClassDescriptor* current = w.classes().find(w.classOf(id)); current != nullptr;
             current = w.classes().find(current->super)) {
            for (const scene::PropertyDesc& property : current->properties) {
                if (property.get == nullptr)
                    continue;
                const scene::Value value = property.get(w, id);
                if (const auto* text = std::get_if<std::string>(&value);
                    text != nullptr && text->starts_with("asset://"))
                    addUnique(out, *text);
            }
        }
    }
}

[[nodiscard]] ContentState itemState(const PreloadItem& item,
                                     const std::function<ContentState(std::string_view)>& stateOf)
{
    bool failed = item.failed;
    for (const std::string& content : item.contents) {
        switch (stateOf(content)) {
        case ContentState::Pending:
            return ContentState::Pending;
        case ContentState::Failed:
            failed = true;
            break;
        case ContentState::Loaded:
            break;
        }
    }
    return failed ? ContentState::Failed : ContentState::Loaded;
}

// The item on top of the stack -- a content name, a stamp's name or an
// instance -- as the content it names. Raises for anything else.
void readItem(lua_State* L, scene::World& w, PreloadItem& item)
{
    if (lua_type(L, -1) == LUA_TSTRING) {
        size_t length = 0;
        const char* text = lua_tolstring(L, -1, &length);
        if (length == 0)
            raise(L, ENG_TR("script.err.preload_item"));
        const std::string_view named(text, length);
        if (named.find("://") != std::string_view::npos) {
            item.contents.emplace_back(named);
            return;
        }
        // **A stamp, named as `Instance.stamp` names it** (ADR 0155 §10):
        // read now and kept, and every asset it names asked for with it, so
        // its first copy neither reads a file nor waits.
        VmContext& ctx = context(L);
        const std::string path = scene::normalizeStampPath(named);
        const std::optional<std::string> source = ctx.stamps ? ctx.stamps(path) : std::optional<std::string>{};
        if (source.has_value()) {
            for (core::usize at = source->find("asset://"); at != std::string::npos;
                 at = source->find("asset://", at + 1)) {
                const core::usize end = source->find('"', at);
                if (end != std::string::npos)
                    addUnique(item.contents, std::string_view(*source).substr(at, end - at));
            }
            ctx.preloadedStamps[path] = *source;
        }
        else {
            item.failed = true;
        }
        return;
    }
    if (const core::InstanceId* instance = toInstance(L, -1); instance != nullptr && w.alive(*instance)) {
        collectContent(w, *instance, item.contents);
        return;
    }
    raise(L, ENG_TR("script.err.preload_item"));
}

// `Keep` and `Release`: every name the list at index 2 holds, noted for the
// host. A kept name is asked for as a preload is, so holding it loads it.
int holdItems(lua_State* L, bool keep)
{
    (void)checkInstance(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    scene::World& w = world(L);
    PreloadState& state = preloads(L);
    const int count = lua_objlen(L, 2);
    for (int index = 1; index <= count; ++index) {
        lua_rawgeti(L, 2, index);
        PreloadItem item;
        readItem(L, w, item);
        lua_pop(L, 1);
        for (const std::string& content : item.contents) {
            state.held.push_back(PreloadState::Held{content, keep});
            if (keep)
                addUnique(state.wanted, content);
        }
    }
    return 0;
}

} // namespace

int contentProviderKeep(lua_State* L)
{
    return holdItems(L, true);
}

int contentProviderRelease(lua_State* L)
{
    return holdItems(L, false);
}

int contentProviderPreloadAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const bool hasCallback = lua_gettop(L) >= 3 && !lua_isnil(L, 3);
    if (hasCallback)
        luaL_checktype(L, 3, LUA_TFUNCTION);

    scene::World& w = world(L);
    PreloadRequest request;
    const int count = lua_objlen(L, 2);
    for (int index = 1; index <= count; ++index) {
        lua_rawgeti(L, 2, index);
        PreloadItem item;
        readItem(L, w, item);
        item.itemRef = lua_ref(L, -1);
        lua_pop(L, 1);
        request.items.push_back(std::move(item));
    }

    PreloadState& state = preloads(L);
    for (const PreloadItem& item : request.items) {
        for (const std::string& content : item.contents)
            addUnique(state.wanted, content);
        state.queued += static_cast<core::u32>(item.contents.size());
    }
    if (hasCallback) {
        lua_pushvalue(L, 3);
        request.callbackRef = lua_ref(L, -1);
        lua_pop(L, 1);
    }

    // The main thread cannot park, nor can a caller inside a metamethod
    // (audit S7): what it asked for still loads, and it is answered at once.
    if (lua_pushthread(L) || !lua_isyieldable(L)) {
        lua_pop(L, 1);
        for (PreloadItem& item : request.items)
            (void)lua_unref(L, item.itemRef);
        if (request.callbackRef != -1)
            (void)lua_unref(L, request.callbackRef);
        return 0;
    }
    request.threadRef = lua_ref(L, -1);
    lua_pop(L, 1);
    state.requests.push_back(std::move(request));
    return lua_yield(L, 0);
}

bool contentProviderMemberGet(lua_State* L, core::InstanceId id, std::string_view key)
{
    scene::World& w = world(L);
    if (key != "RequestQueueSize" || w.atoms().text(w.classes().find(w.classOf(id))->name) != "ContentProvider")
        return false;
    lua_pushnumber(L, static_cast<double>(preloads(L).queued));
    return true;
}

std::vector<std::string> takePreloadContent(lua_State* L)
{
    std::vector<std::string> taken;
    taken.swap(preloads(L).wanted);
    return taken;
}

std::vector<PreloadState::Held> takeHeldContent(lua_State* L)
{
    std::vector<PreloadState::Held> taken;
    taken.swap(preloads(L).held);
    return taken;
}

void resumePreloads(lua_State* L, const std::function<ContentState(std::string_view)>& stateOf)
{
    PreloadState& state = preloads(L);
    if (state.requests.empty()) {
        state.queued = 0;
        return;
    }
    // Counted before anything is resumed, so a call resumed below reads the
    // queue as it stands after this pass.
    core::u32 queued = 0;
    for (const PreloadRequest& request : state.requests) {
        for (const PreloadItem& item : request.items) {
            for (const std::string& content : item.contents) {
                if (!item.reported && stateOf(content) == ContentState::Pending)
                    ++queued;
            }
        }
    }
    state.queued = queued;

    // Taken out first: a callback may call `PreloadAsync` again, and the list
    // it would push onto is the one being walked.
    std::vector<PreloadRequest> requests;
    requests.swap(state.requests);
    std::vector<PreloadRequest> waiting;
    for (PreloadRequest& request : requests) {
        bool done = true;
        for (PreloadItem& item : request.items) {
            if (item.reported)
                continue;
            const ContentState itemNow = itemState(item, stateOf);
            if (itemNow == ContentState::Pending) {
                done = false;
                continue;
            }
            item.reported = true;
            if (request.callbackRef != -1) {
                lua_State* co = lua_newthread(L);
                lua_getref(L, request.callbackRef);
                lua_xmove(L, co, 1);
                lua_getref(co, item.itemRef);
                pushEnumItem(co, scene::EnumValue{scene::generated::AssetFetchStatusEnumId,
                                                  itemNow == ContentState::Loaded ? 0 : 1});
                (void)resumeScheduled(L, co, 2);
                lua_pop(L, 1);
            }
        }
        if (!done) {
            waiting.push_back(std::move(request));
            continue;
        }
        for (PreloadItem& item : request.items)
            (void)lua_unref(L, item.itemRef);
        if (request.callbackRef != -1)
            (void)lua_unref(L, request.callbackRef);
        lua_getref(L, request.threadRef);
        lua_State* co = lua_tothread(L, -1);
        if (co != nullptr)
            (void)resumeScheduled(L, co, 0);
        lua_pop(L, 1);
        // Whatever the thread did (audit S3): one that parks again took its
        // own reference where it waits.
        (void)lua_unref(L, request.threadRef);
    }
    for (PreloadRequest& request : state.requests)
        waiting.push_back(std::move(request));
    state.requests = std::move(waiting);
}

} // namespace engine::script
