#include "engine/script/remote.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "engine/core/error.h"
#include "engine/core/i18n.h"
#include "engine/scene/players.h"
#include "engine/scene/world.h"
#include "engine/script/binding.h"
#include "engine/script/datatypes.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

using core::u32;
using core::u8;
using core::usize;
using scene::World;

// One tag per kind of value. **Permanent numbers**: a payload is bytes on the
// wire between two builds, and a renumbered tag is a value read as another.
enum class Tag : u8
{
    Nil = 0,
    False = 1,
    True = 2,
    Number = 3,
    String = 4,
    Vector = 5,
    Instance = 6,
    Table = 7,
    // The datatypes a game sends every day (G12, protocol 35).
    Color3 = 8,
    CFrame = 9,
    Vector2 = 10,
    UDim = 11,
    UDim2 = 12,
    EnumItem = 13,
    // Bytes a game packed itself (ADR 0161, protocol 38): what a snapshot
    // sent many times a second is, without a copy into a string first.
    Buffer = 14,
};

[[nodiscard]] World& world(lua_State* L) noexcept
{
    return *context(L).world;
}

void putU32(std::vector<u8>& out, u32 value)
{
    for (int at = 0; at < 4; ++at)
        out.push_back(static_cast<u8>(value >> (8 * at)));
}

void putF32(std::vector<u8>& out, float value)
{
    putU32(out, std::bit_cast<u32>(value));
}

void putF64(std::vector<u8>& out, double value)
{
    const auto bits = std::bit_cast<core::u64>(value);
    for (int at = 0; at < 8; ++at)
        out.push_back(static_cast<u8>(bits >> (8 * at)));
}

struct Encoder
{
    lua_State* L;
    std::vector<u8>& out;
    std::vector<core::InstanceId>& refs;
    // An `UnreliableRemoteEvent`'s message: the smaller limit, and its error.
    bool unreliable = false;

    [[noreturn]] void tooLarge() const
    {
        const usize limit = unreliable ? MaxUnreliableRemotePayload : MaxRemotePayload;
        const core::I18nArg args[] = {{"limit", static_cast<core::i64>(limit)}};
        raise(L, unreliable ? ENG_TR("net.err.unreliable_too_large") : ENG_TR("net.err.remote_too_large"), args);
    }

    void checkSize() const
    {
        if (out.size() > (unreliable ? MaxUnreliableRemotePayload : MaxRemotePayload))
            tooLarge();
    }

    void value(int index, int depth, bool key)
    {
        switch (lua_type(L, index)) {
        case LUA_TNIL:
            out.push_back(static_cast<u8>(Tag::Nil));
            break;
        case LUA_TBOOLEAN:
            out.push_back(static_cast<u8>(lua_toboolean(L, index) != 0 ? Tag::True : Tag::False));
            break;
        case LUA_TNUMBER: {
            out.push_back(static_cast<u8>(Tag::Number));
            const auto bits = std::bit_cast<core::u64>(static_cast<double>(lua_tonumber(L, index)));
            for (int at = 0; at < 8; ++at)
                out.push_back(static_cast<u8>(bits >> (8 * at)));
            break;
        }
        case LUA_TSTRING: {
            usize length = 0;
            const char* text = lua_tolstring(L, index, &length);
            if (length > (unreliable ? MaxUnreliableRemotePayload : MaxRemotePayload))
                tooLarge();
            out.push_back(static_cast<u8>(Tag::String));
            putU32(out, static_cast<u32>(length));
            out.insert(out.end(), reinterpret_cast<const u8*>(text), reinterpret_cast<const u8*>(text) + length);
            break;
        }
        case LUA_TBUFFER: {
            if (key)
                refuseKey(index);
            usize length = 0;
            const void* data = lua_tobuffer(L, index, &length);
            if (length > (unreliable ? MaxUnreliableRemotePayload : MaxRemotePayload))
                tooLarge();
            out.push_back(static_cast<u8>(Tag::Buffer));
            putU32(out, static_cast<u32>(length));
            out.insert(out.end(), static_cast<const u8*>(data), static_cast<const u8*>(data) + length);
            break;
        }
        case LUA_TVECTOR: {
            if (key)
                refuseKey(index);
            const float* vector = lua_tovector(L, index);
            out.push_back(static_cast<u8>(Tag::Vector));
            for (int axis = 0; axis < 3; ++axis)
                putU32(out, std::bit_cast<u32>(vector[axis]));
            break;
        }
        case LUA_TUSERDATA: {
            if (datatype(index, key))
                break;
            const core::InstanceId* instance = toInstance(L, index);
            if (instance == nullptr)
                refuse(index);
            if (key)
                refuseKey(index);
            out.push_back(static_cast<u8>(Tag::Instance));
            putU32(out, static_cast<u32>(refs.size()));
            refs.push_back(*instance);
            break;
        }
        case LUA_TTABLE: {
            if (key)
                refuseKey(index);
            // Past the depth, and a table containing itself arrives here too:
            // walking one would never end.
            if (depth >= MaxRemoteDepth) {
                const core::I18nArg args[] = {{"depth", static_cast<core::i64>(MaxRemoteDepth)}};
                raise(L, ENG_TR("net.err.remote_nested"), args);
            }
            out.push_back(static_cast<u8>(Tag::Table));
            const usize countAt = out.size();
            putU32(out, 0);
            u32 pairs = 0;
            const int table = lua_absindex(L, index);
            luaL_checkstack(L, 3, "RemoteEvent");
            lua_pushnil(L);
            while (lua_next(L, table) != 0) {
                value(-2, depth + 1, true);
                value(-1, depth + 1, false);
                lua_pop(L, 1);
                ++pairs;
            }
            for (int at = 0; at < 4; ++at)
                out[countAt + static_cast<usize>(at)] = static_cast<u8>(pairs >> (8 * at));
            break;
        }
        default:
            refuse(index);
        }
        checkSize();
    }

    // **A datatype a game sends every day** (G12): a colour, a place, a 2D
    // point, a layout size, an enum item. Never a key, as a vector is not.
    [[nodiscard]] bool datatype(int index, bool key)
    {
        const auto as = [&](scene::ValueType type) -> scene::Value {
            if (key)
                refuseKey(index);
            const std::optional<scene::Value> value = toValue(L, index, type);
            if (!value.has_value())
                refuse(index);
            return *value;
        };
        switch (static_cast<UserdataTag>(lua_userdatatag(L, index))) {
        case UserdataTag::Color3: {
            const auto colour = std::get<core::Color3>(as(scene::ValueType::Color3));
            out.push_back(static_cast<u8>(Tag::Color3));
            putF32(out, colour.r);
            putF32(out, colour.g);
            putF32(out, colour.b);
            return true;
        }
        case UserdataTag::CFrame: {
            const auto frame = std::get<core::CFrameD>(as(scene::ValueType::CFrame));
            out.push_back(static_cast<u8>(Tag::CFrame));
            putF64(out, frame.position.x);
            putF64(out, frame.position.y);
            putF64(out, frame.position.z);
            for (int column = 0; column < 3; ++column) {
                for (int row = 0; row < 3; ++row)
                    putF32(out, frame.rotation.m[column][row]);
            }
            return true;
        }
        case UserdataTag::Vector2: {
            const auto point = std::get<core::Vec2>(as(scene::ValueType::Vector2));
            out.push_back(static_cast<u8>(Tag::Vector2));
            putF32(out, point.x);
            putF32(out, point.y);
            return true;
        }
        case UserdataTag::UDim: {
            const auto size = std::get<core::UDim>(as(scene::ValueType::UDim));
            out.push_back(static_cast<u8>(Tag::UDim));
            putF32(out, size.scale);
            putF32(out, size.offset);
            return true;
        }
        case UserdataTag::UDim2: {
            const auto size = std::get<core::UDim2>(as(scene::ValueType::UDim2));
            out.push_back(static_cast<u8>(Tag::UDim2));
            putF32(out, size.x.scale);
            putF32(out, size.x.offset);
            putF32(out, size.y.scale);
            putF32(out, size.y.offset);
            return true;
        }
        case UserdataTag::EnumItem: {
            const auto item = std::get<scene::EnumValue>(as(scene::ValueType::EnumItem));
            out.push_back(static_cast<u8>(Tag::EnumItem));
            out.push_back(static_cast<u8>(item.enumId & 0xFFu));
            out.push_back(static_cast<u8>(item.enumId >> 8));
            putU32(out, static_cast<u32>(item.value));
            return true;
        }
        default:
            return false;
        }
    }

    [[noreturn]] void refuse(int index) const
    {
        const core::I18nArg args[] = {{"type", std::string_view{luaL_typename(L, index)}}};
        raise(L, ENG_TR("net.err.remote_value"), args);
    }

    [[noreturn]] void refuseKey(int index) const
    {
        const core::I18nArg args[] = {{"type", std::string_view{luaL_typename(L, index)}}};
        raise(L, ENG_TR("net.err.remote_key"), args);
    }
};

// Whether a value can index a table: NaN, or a vector with NaN in it, cannot.
[[nodiscard]] bool indexable(lua_State* L, int index)
{
    if (lua_type(L, index) == LUA_TNUMBER)
        return !std::isnan(lua_tonumber(L, index));
    if (lua_type(L, index) == LUA_TVECTOR) {
        const float* axes = lua_tovector(L, index);
        return axes != nullptr && !std::isnan(axes[0]) && !std::isnan(axes[1]) && !std::isnan(axes[2]);
    }
    return true;
}

struct Decoder
{
    lua_State* L;
    std::span<const u8> bytes;
    std::span<const core::InstanceId> refs;
    usize at = 0;

    [[nodiscard]] bool read(usize width, core::u64& value)
    {
        if (bytes.size() - at < width)
            return false;
        value = 0;
        for (usize byte = 0; byte < width; ++byte)
            value |= static_cast<core::u64>(bytes[at + byte]) << (8 * byte);
        at += width;
        return true;
    }

    [[nodiscard]] bool readF32(float& value)
    {
        core::u64 bits = 0;
        if (!read(4, bits))
            return false;
        value = std::bit_cast<float>(static_cast<u32>(bits));
        return true;
    }

    [[nodiscard]] bool readF64(double& value)
    {
        core::u64 bits = 0;
        if (!read(8, bits))
            return false;
        value = std::bit_cast<double>(bits);
        return true;
    }

    // Pushes one value; false, with the stack as it was, on anything malformed.
    [[nodiscard]] bool value(int depth)
    {
        core::u64 tag = 0;
        if (depth > MaxRemoteDepth || !read(1, tag) || !lua_checkstack(L, 3))
            return false;
        switch (static_cast<Tag>(tag)) {
        case Tag::Nil:
            lua_pushnil(L);
            return true;
        case Tag::False:
        case Tag::True:
            lua_pushboolean(L, static_cast<Tag>(tag) == Tag::True ? 1 : 0);
            return true;
        case Tag::Number: {
            core::u64 bits = 0;
            if (!read(8, bits))
                return false;
            lua_pushnumber(L, std::bit_cast<double>(bits));
            return true;
        }
        case Tag::String: {
            core::u64 length = 0;
            if (!read(4, length) || bytes.size() - at < length)
                return false;
            lua_pushlstring(L, reinterpret_cast<const char*>(bytes.data() + at), static_cast<usize>(length));
            at += static_cast<usize>(length);
            return true;
        }
        case Tag::Buffer: {
            core::u64 length = 0;
            if (!read(4, length) || bytes.size() - at < length)
                return false;
            void* data = lua_newbuffer(L, static_cast<usize>(length));
            if (length != 0)
                std::memcpy(data, bytes.data() + at, static_cast<usize>(length));
            at += static_cast<usize>(length);
            return true;
        }
        case Tag::Vector: {
            std::array<float, 3> axes{};
            for (float& axis : axes) {
                core::u64 bits = 0;
                if (!read(4, bits))
                    return false;
                axis = std::bit_cast<float>(static_cast<u32>(bits));
            }
            lua_pushvector(L, axes[0], axes[1], axes[2]);
            return true;
        }
        case Tag::Instance: {
            core::u64 index = 0;
            if (!read(4, index) || index >= refs.size())
                return false;
            // An instance the receiver does not have arrives as nil: it was
            // out of this machine's interest, or never replicated.
            pushInstance(L, refs[static_cast<usize>(index)]);
            return true;
        }
        case Tag::Color3: {
            core::Color3 colour;
            if (!readF32(colour.r) || !readF32(colour.g) || !readF32(colour.b))
                return false;
            pushValue(L, scene::Value{colour});
            return true;
        }
        case Tag::CFrame: {
            core::CFrameD frame;
            if (!readF64(frame.position.x) || !readF64(frame.position.y) || !readF64(frame.position.z))
                return false;
            for (int column = 0; column < 3; ++column) {
                for (int row = 0; row < 3; ++row) {
                    if (!readF32(frame.rotation.m[column][row]))
                        return false;
                }
            }
            pushValue(L, scene::Value{frame});
            return true;
        }
        case Tag::Vector2: {
            core::Vec2 point;
            if (!readF32(point.x) || !readF32(point.y))
                return false;
            pushValue(L, scene::Value{point});
            return true;
        }
        case Tag::UDim: {
            core::UDim size;
            if (!readF32(size.scale) || !readF32(size.offset))
                return false;
            pushValue(L, scene::Value{size});
            return true;
        }
        case Tag::UDim2: {
            core::UDim2 size;
            if (!readF32(size.x.scale) || !readF32(size.x.offset) || !readF32(size.y.scale) || !readF32(size.y.offset))
                return false;
            pushValue(L, scene::Value{size});
            return true;
        }
        case Tag::EnumItem: {
            core::u64 id = 0;
            core::u64 raw = 0;
            if (!read(2, id) || !read(4, raw))
                return false;
            // An item of an enum this build does not have, or a value it does
            // not name, arrives as nil: a peer's word, never trusted.
            const scene::EnumValue item{static_cast<core::u16>(id), static_cast<core::i32>(static_cast<u32>(raw))};
            if (world(L).enums().findValue(item.enumId, item.value) == nullptr) {
                lua_pushnil(L);
                return true;
            }
            pushEnumItem(L, item);
            return true;
        }
        case Tag::Table: {
            core::u64 pairs = 0;
            // Each pair is at least two bytes, which bounds a count a mangled
            // payload claims before anything is allocated for it.
            if (!read(4, pairs) || pairs > (bytes.size() - at) / 2)
                return false;
            const int top = lua_gettop(L);
            lua_createtable(L, 0, 0);
            for (core::u64 pair = 0; pair < pairs; ++pair) {
                if (!value(depth + 1) || !value(depth + 1)) {
                    lua_settop(L, top);
                    return false;
                }
                // A key that arrived as nil -- an instance the receiver does
                // not have -- has nowhere to go.
                if (lua_isnil(L, -2)) {
                    lua_pop(L, 2);
                    continue;
                }
                // **A NaN cannot be a key** (audit N1): Luau raises on one, and
                // a raise here is a peer's message ending the process. A
                // malformed payload, like any other.
                if (!indexable(L, -2)) {
                    lua_settop(L, top);
                    return false;
                }
                lua_rawset(L, -3);
            }
            return true;
        }
        }
        return false;
    }
};

// Who this machine is, as far as a message is concerned.
[[nodiscard]] bool onReplica(const World& w) noexcept
{
    return w.engineState().networkTopology == scene::NetworkTopology::Replica;
}

[[nodiscard]] bool networked(const World& w) noexcept
{
    return w.engineState().networkTopology != scene::NetworkTopology::Solo;
}

[[noreturn]] void refuseOnReplica(lua_State* L, std::string_view method)
{
    const core::I18nArg args[] = {{"method", method}};
    raise(L, ENG_TR("net.err.remote_authority_only"), args);
}

int fireServer(lua_State* L, bool unreliable, bool explicitPlayer = false)
{
    const core::InstanceId remote = checkInstance(L, 1);
    World& w = world(L);
    scene::RemoteMessage message;
    message.remote = remote;
    message.toServer = true;
    message.unreliable = unreliable;
    core::InstanceId sender;
    if (explicitPlayer) {
        sender = checkInstance(L, 2);
        const auto* player = w.players().find(sender);
        if (onReplica(w) || player == nullptr || !player->local || w.destroyed(sender) ||
            std::find(w.engineState().leavingPlayers.begin(), w.engineState().leavingPlayers.end(), sender) !=
                w.engineState().leavingPlayers.end())
            raise(L, ENG_TR("net.err.remote_not_player"));
    }
    const int first = explicitPlayer ? 3 : 2;
    encodeRemoteArguments(L, first, lua_gettop(L) - first + 1, message.payload, message.refs, unreliable);
    if (onReplica(w)) {
        w.engineState().remoteOutbox.push_back(std::move(message));
        return 0;
    }
    // **The authority is its own server**: solo or hosting, its player's
    // message is delivered here, the way a replica's would be.
    const core::InstanceId local = explicitPlayer ? sender : scene::localPlayerOf(w);
    if (!local.valid())
        raise(L, ENG_TR("net.err.remote_no_player"));
    message.player = local;
    w.engineState().remoteInbox.push_back(std::move(message));
    return 0;
}

int remoteFireServer(lua_State* L)
{
    return fireServer(L, false);
}

int unreliableFireServer(lua_State* L)
{
    return fireServer(L, true);
}

int remoteFireServerFor(lua_State* L)
{
    return fireServer(L, false, true);
}

int unreliableFireServerFor(lua_State* L)
{
    return fireServer(L, true, true);
}

int fireClient(lua_State* L, bool unreliable)
{
    const core::InstanceId remote = checkInstance(L, 1);
    World& w = world(L);
    if (onReplica(w))
        refuseOnReplica(L, "FireClient");
    const core::InstanceId player = checkInstance(L, 2);
    const scene::PlayerComponent* who = w.players().find(player);
    if (who == nullptr)
        raise(L, ENG_TR("net.err.remote_not_player"));
    scene::RemoteMessage message;
    message.remote = remote;
    message.unreliable = unreliable;
    encodeRemoteArguments(L, 3, lua_gettop(L) - 2, message.payload, message.refs, unreliable);
    if (who->local) {
        message.player = player;
        w.engineState().remoteInbox.push_back(std::move(message));
        return 0;
    }
    if (networked(w)) {
        message.userId = who->userId;
        w.engineState().remoteOutbox.push_back(std::move(message));
    }
    return 0;
}

int remoteFireClient(lua_State* L)
{
    return fireClient(L, false);
}

int unreliableFireClient(lua_State* L)
{
    return fireClient(L, true);
}

int fireAllClients(lua_State* L, bool unreliable)
{
    const core::InstanceId remote = checkInstance(L, 1);
    World& w = world(L);
    if (onReplica(w))
        refuseOnReplica(L, "FireAllClients");
    scene::RemoteMessage message;
    message.remote = remote;
    message.unreliable = unreliable;
    encodeRemoteArguments(L, 2, lua_gettop(L) - 1, message.payload, message.refs, unreliable);
    // A host's own player is a client too; a dedicated server has none.
    if (scene::localPlayerOf(w).valid())
        w.engineState().remoteInbox.push_back(message);
    if (networked(w))
        w.engineState().remoteOutbox.push_back(std::move(message));
    return 0;
}

int remoteFireAllClients(lua_State* L)
{
    return fireAllClients(L, false);
}

int unreliableFireAllClients(lua_State* L)
{
    return fireAllClients(L, true);
}

// --- RemoteFunction (ADR 0079) -------------------------------------------------

constexpr std::string_view InvokeCallback = "OnServerInvoke";
// An error's text travels whole up to here; past it, the start says enough.
constexpr usize MaxInvokeErrorText = 4096;

[[nodiscard]] ServiceState& invokeState(lua_State* L) noexcept
{
    return *context(L).services;
}

[[nodiscard]] bool isRemoteFunction(const World& w, core::InstanceId id) noexcept
{
    if (!w.alive(id))
        return false;
    const scene::ClassDescriptor* descriptor = w.classes().find(w.classOf(id));
    return descriptor != nullptr && w.atoms().text(descriptor->name) == "RemoteFunction";
}

int remoteInvokeServer(lua_State* L)
{
    const core::InstanceId remote = checkInstance(L, 1);
    // Before the question is sent: one that cannot wait for its answer is
    // not asked.
    requireYieldable(L, "InvokeServer");
    World& w = world(L);
    ServiceState& state = invokeState(L);
    scene::RemoteMessage message;
    message.remote = remote;
    message.toServer = true;
    encodeRemoteArguments(L, 2, lua_gettop(L) - 1, message.payload, message.refs);
    // Zero means "not a question" on the wire, so the counter skips it when
    // it wraps.
    const u32 call = state.nextInvoke++;
    if (state.nextInvoke == 0)
        state.nextInvoke = 1;
    message.call = call;
    if (onReplica(w)) {
        w.engineState().remoteOutbox.push_back(std::move(message));
    }
    else {
        // **The authority asks itself**, as its own player, at the start of the
        // next tick -- the moment a replica's question would be answered too.
        const core::InstanceId local = scene::localPlayerOf(w);
        if (!local.valid())
            raise(L, ENG_TR("net.err.remote_no_player"));
        message.player = local;
        w.engineState().remoteInbox.push_back(std::move(message));
    }
    lua_pushthread(L);
    const int threadRef = lua_ref(L, -1);
    lua_pop(L, 1);
    state.invokeWaiters.push_back(ServiceState::InvokeWaiter{call, threadRef});
    return lua_yield(L, 0);
}

// The answer's values, encoded under protection: a handler that returns a
// function is the handler's mistake, and it is answered as a failure rather
// than raised into the tick.
int protectedEncode(lua_State* L)
{
    auto* answer = static_cast<scene::RemoteMessage*>(lua_tolightuserdata(L, 1));
    encodeRemoteArguments(L, 2, lua_gettop(L) - 1, answer->payload, answer->refs);
    return 0;
}

void failAnswer(lua_State* L, scene::RemoteMessage& answer, std::string text)
{
    if (text.size() > MaxInvokeErrorText)
        text.resize(MaxInvokeErrorText);
    answer.failed = true;
    lua_pushlstring(L, text.data(), text.size());
    encodeRemoteArguments(L, lua_gettop(L), 1, answer.payload, answer.refs);
    lua_pop(L, 1);
}

// Resumes the caller parked on `answer.call`, with the answer or its error.
void deliverAnswer(lua_State* L, const scene::RemoteMessage& answer)
{
    std::vector<ServiceState::InvokeWaiter>& waiters = invokeState(L).invokeWaiters;
    const auto found = std::find_if(waiters.begin(), waiters.end(),
                                    [&](const ServiceState::InvokeWaiter& w) { return w.call == answer.call; });
    if (found == waiters.end())
        return;
    const int threadRef = found->threadRef;
    waiters.erase(found);

    lua_getref(L, threadRef);
    lua_State* co = lua_tothread(L, -1);
    if (co == nullptr) {
        lua_pop(L, 1);
        (void)lua_unref(L, threadRef);
        return;
    }
    const int decoded = decodeRemoteArguments(co, answer.payload, answer.refs);
    if (answer.failed) {
        if (decoded != 1) {
            // A failure is one string; anything else is a payload somebody
            // mangled, and the caller still has to learn it failed.
            if (decoded > 0)
                lua_pop(co, decoded);
            lua_pushstring(co, "RemoteFunction: the answer could not be read");
        }
        (void)resumeScheduledWithError(L, co);
    }
    else {
        (void)resumeScheduled(L, co, std::max(decoded, 0));
    }
    lua_pop(L, 1);
    // Whatever the thread did (audit S3): one that parks again took its
    // own reference where it waits.
    (void)lua_unref(L, threadRef);
}

// Sends an answer to whoever asked: straight to the waiting caller when the
// player is this machine's own, over the network otherwise.
void sendAnswer(lua_State* L, scene::RemoteMessage& answer, core::InstanceId player)
{
    World& w = world(L);
    const scene::PlayerComponent* who = w.players().find(player);
    if (who == nullptr)
        return; // they left while the handler ran
    if (who->local) {
        deliverAnswer(L, answer);
        return;
    }
    if (networked(w)) {
        answer.userId = who->userId;
        w.engineState().remoteOutbox.push_back(std::move(answer));
    }
}

[[nodiscard]] std::string remoteName(const World& w, core::InstanceId remote)
{
    return w.alive(remote) ? std::string(w.atoms().text(w.name(remote))) : std::string("RemoteFunction");
}

void failInvoke(lua_State* L, scene::RemoteMessage& answer, core::InstanceId remote, std::string_view text)
{
    answer.payload.clear();
    answer.refs.clear();
    const std::string name = remoteName(world(L), remote);
    const core::I18nArg args[] = {{"name", std::string_view{name}}, {"message", text}};
    failAnswer(L, answer, core::formatKeyPrefixed(ENG_TR("net.err.remote_invoke_failed"), args));
}

// A handler's thread has stopped for good: `status` is `LUA_OK` with its
// results on `co`'s stack, or the error it raised on top.
void finishInvoke(lua_State* L, lua_State* co, int status, core::InstanceId remote, core::InstanceId player, u32 call)
{
    scene::RemoteMessage answer;
    answer.remote = remote;
    answer.call = call;
    answer.reply = true;
    if (status == LUA_OK) {
        const int count = lua_gettop(co);
        lua_pushcfunction(L, protectedEncode, "OnServerInvoke");
        lua_pushlightuserdata(L, &answer);
        lua_xmove(co, L, count);
        if (lua_pcall(L, count + 1, 0, 0) != LUA_OK) {
            const char* message = lua_tostring(L, -1);
            const std::string text = message != nullptr ? message : "";
            lua_pop(L, 1);
            failInvoke(L, answer, remote, text);
        }
    }
    else {
        const char* message = lua_tostring(co, -1);
        const std::string text = message != nullptr ? message : "";
        failInvoke(L, answer, remote, text);
    }
    sendAnswer(L, answer, player);
}

// A question arrived at the authority: its handler runs in a thread of its
// own, and answers now or -- having yielded -- when it finishes.
void startInvoke(lua_State* L, const scene::RemoteMessage& message)
{
    World& w = world(L);
    ServiceState& state = invokeState(L);
    const auto handler = std::find_if(state.invokeHandlers.begin(), state.invokeHandlers.end(),
                                      [&](const ServiceState::InvokeHandler& h) { return h.remote == message.remote; });
    if (handler == state.invokeHandlers.end() || !isRemoteFunction(w, message.remote)) {
        scene::RemoteMessage answer;
        answer.remote = message.remote;
        answer.call = message.call;
        answer.reply = true;
        const std::string name = remoteName(w, message.remote);
        const core::I18nArg args[] = {{"name", std::string_view{name}}};
        failAnswer(L, answer, core::formatKeyPrefixed(ENG_TR("net.err.remote_no_handler"), args));
        sendAnswer(L, answer, message.player);
        return;
    }

    lua_State* co = lua_newthread(L);
    const int threadRef = lua_ref(L, -1);
    lua_pop(L, 1);
    lua_getref(L, handler->functionRef);
    lua_xmove(L, co, 1);
    pushInstance(co, message.player);
    const int decoded = decodeRemoteArguments(co, message.payload, message.refs);
    if (decoded < 0) {
        // Mangled on the way: dropped, as an event's would be.
        (void)lua_unref(L, threadRef);
        return;
    }
    const int status = startScheduled(L, co, 1 + decoded);
    if (status == LUA_YIELD || status == LUA_BREAK) {
        state.invokeRunning.push_back(
            ServiceState::InvokeRunning{threadRef, message.remote, message.player, message.call});
        return;
    }
    finishInvoke(L, co, status, message.remote, message.player, message.call);
    (void)lua_unref(L, threadRef);
}

// The handlers that yielded and have since finished, in the order they
// started: their answers go now.
void finishRunningInvokes(lua_State* L)
{
    ServiceState& state = invokeState(L);
    std::vector<ServiceState::InvokeRunning> done;
    for (usize index = 0; index < state.invokeRunning.size();) {
        const ServiceState::InvokeRunning& entry = state.invokeRunning[index];
        lua_getref(L, entry.threadRef);
        lua_State* co = lua_tothread(L, -1);
        lua_pop(L, 1);
        const int status = co != nullptr ? lua_status(co) : LUA_ERRRUN;
        if (status == LUA_YIELD || status == LUA_BREAK) {
            ++index;
            continue;
        }
        done.push_back(entry);
        state.invokeRunning.erase(state.invokeRunning.begin() + static_cast<std::ptrdiff_t>(index));
    }
    for (const ServiceState::InvokeRunning& entry : done) {
        lua_getref(L, entry.threadRef);
        lua_State* co = lua_tothread(L, -1);
        if (co != nullptr)
            finishInvoke(L, co, lua_status(co), entry.remote, entry.player, entry.call);
        lua_pop(L, 1);
        (void)lua_unref(L, entry.threadRef);
    }
}

constexpr InstanceMethodBinding RemoteMethods[] = {
    {"RemoteEvent", "FireServer", remoteFireServer},
    {"RemoteEvent", "FireServerFor", remoteFireServerFor},
    {"RemoteEvent", "FireClient", remoteFireClient},
    {"RemoteEvent", "FireAllClients", remoteFireAllClients},
    {"UnreliableRemoteEvent", "FireServer", unreliableFireServer},
    {"UnreliableRemoteEvent", "FireServerFor", unreliableFireServerFor},
    {"UnreliableRemoteEvent", "FireClient", unreliableFireClient},
    {"UnreliableRemoteEvent", "FireAllClients", unreliableFireAllClients},
    {"RemoteFunction", "InvokeServerAsync", remoteInvokeServer},
};

} // namespace

void encodeRemoteArguments(lua_State* L, int first, int count, std::vector<u8>& payload,
                           std::vector<core::InstanceId>& refs, bool unreliable)
{
    if (count > MaxRemoteArguments) {
        const core::I18nArg args[] = {{"count", static_cast<core::i64>(count)},
                                      {"limit", static_cast<core::i64>(MaxRemoteArguments)}};
        raise(L, ENG_TR("net.err.remote_too_many"), args);
    }
    payload.clear();
    refs.clear();
    payload.push_back(static_cast<u8>(std::max(count, 0)));
    Encoder encoder{L, payload, refs, unreliable};
    for (int at = 0; at < count; ++at)
        encoder.value(first + at, 0, false);
}

namespace {

struct DecodeJob
{
    std::span<const u8> payload;
    std::span<const core::InstanceId> refs;
    int count = -1;
};

int decodeProtected(lua_State* L)
{
    auto* job = static_cast<DecodeJob*>(lua_tolightuserdata(L, 1));
    lua_settop(L, 0);
    const int count = job->payload[0];
    Decoder decoder{L, job->payload, job->refs, 1};
    for (int at = 0; at < count; ++at) {
        if (!decoder.value(0))
            return 0;
    }
    // Trailing bytes are a payload somebody else wrote; nothing here did.
    if (decoder.at != job->payload.size())
        return 0;
    job->count = count;
    return count;
}

} // namespace

int decodeRemoteArguments(lua_State* L, std::span<const u8> payload, std::span<const core::InstanceId> refs)
{
    if (payload.empty())
        return -1;
    // **On a thread of its own, under a protected call** (audit N1): the bytes
    // are a peer's, and whatever they make Luau raise -- a key it refuses, an
    // allocation it cannot make -- is a message dropped, never an exception
    // through the host. Its own thread because `L` may be a coroutine parked
    // on a yield, where a call cannot run; the values are moved to it after.
    lua_State* scratch = lua_newthread(L);
    const int anchor = lua_gettop(L);
    DecodeJob job{payload, refs};
    lua_pushcfunction(scratch, decodeProtected, "decode");
    lua_pushlightuserdata(scratch, &job);
    const int status = lua_pcall(scratch, 1, LUA_MULTRET, 0);
    int count = -1;
    if (status == LUA_OK && job.count >= 0 && lua_gettop(scratch) == job.count && lua_checkstack(L, job.count)) {
        count = job.count;
        lua_xmove(scratch, L, count);
    }
    lua_remove(L, anchor);
    return count;
}

// How many calls one `RemoteEvent` keeps, each way, for a listener that has
// not connected yet. A scene's first exchange is a handful; a remote fired
// every tick at nobody reaches this in four seconds and is told.
constexpr usize MaxHeldRemoteCalls = 256;

void fireRemoteMessages(lua_State* L)
{
    World& w = world(L);
    ServiceState& state = invokeState(L);
    if (!state.invokeRunning.empty())
        finishRunningInvokes(L);
    // **What waited for a listener first, then what has just arrived** (D451):
    // one list, in the order the calls were made.
    std::vector<scene::RemoteMessage> inbox;
    inbox.swap(state.heldRemoteCalls);
    for (scene::RemoteMessage& arrived : w.engineState().remoteInbox)
        inbox.push_back(std::move(arrived));
    w.engineState().remoteInbox.clear();
    std::erase_if(state.heldRemoteWarned, [&w](core::InstanceId remote) { return !w.alive(remote); });
    if (inbox.empty())
        return;
    const core::NameAtom serverEvent = w.atoms().intern("ServerReceived");
    const core::NameAtom clientEvent = w.atoms().intern("ClientReceived");
    const core::NameAtom localClientEvent = w.atoms().intern("LocalClientReceived");
    for (scene::RemoteMessage& message : inbox) {
        // An answer is for a caller, and finds it by number whatever became
        // of the instance it named.
        if (message.reply) {
            deliverAnswer(L, message);
            continue;
        }
        if (!w.alive(message.remote))
            continue;
        if (message.call != 0) {
            startInvoke(L, message);
            continue;
        }
        const bool guest = !message.toServer && message.player.valid() && message.player != scene::localPlayerOf(w);
        if (guest && (!w.alive(message.player) || w.players().find(message.player) == nullptr ||
                      !w.players().find(message.player)->local))
            continue;
        const scene::EventDesc* event = w.classes().findEvent(
            w.classOf(message.remote), message.toServer ? serverEvent : (guest ? localClientEvent : clientEvent));
        if (event == nullptr)
            continue;
        // **A call that arrives before anybody listens is kept for the first
        // who does** (D451). A scene's client and server scripts start in the
        // same tick, so a client that fires at once fired into a remote the
        // server had not connected yet -- it was still yielding on its saves
        // -- and the call was dropped without a word: the first message of
        // every scene was a race, won by luck the first time and lost the
        // second. Kept in order, both ways, up to a bound; past it the call
        // is dropped, and the remote says so once.
        if (!instanceEventHeard(L, message.remote, event->slot)) {
            // An unreliable one waits for nobody (ADR 0161): the next is on
            // its way, and one kept would be delivered stale.
            if (message.unreliable)
                continue;
            const auto waiting = static_cast<usize>(std::count_if(
                state.heldRemoteCalls.begin(), state.heldRemoteCalls.end(), [&](const scene::RemoteMessage& held) {
                    return held.remote == message.remote && held.toServer == message.toServer;
                }));
            if (waiting < MaxHeldRemoteCalls) {
                state.heldRemoteCalls.push_back(std::move(message));
            }
            else if (std::find(state.heldRemoteWarned.begin(), state.heldRemoteWarned.end(), message.remote) ==
                     state.heldRemoteWarned.end()) {
                state.heldRemoteWarned.push_back(message.remote);
                const core::I18nArg args[] = {
                    {"remote", w.atoms().text(w.name(message.remote))},
                    {"event",
                     message.toServer ? std::string_view{"ServerReceived"} : std::string_view{"ClientReceived"}},
                    {"limit", static_cast<core::i64>(MaxHeldRemoteCalls)},
                };
                core::log(core::LogLevel::Warn, ENG_TR("net.warn.remote_unheard"), args);
            }
            continue;
        }
        // Whoever sent it has gone: there is nobody to say it is from.
        if (message.toServer && !w.alive(message.player))
            continue;
        const int top = lua_gettop(L);
        int count = 0;
        if (message.toServer || guest) {
            pushInstance(L, message.player);
            count = 1;
        }
        const int decoded = decodeRemoteArguments(L, message.payload, message.refs);
        if (decoded < 0) {
            lua_settop(L, top);
            continue;
        }
        fireInstanceEvent(L, message.remote, event->slot, top + 1, count + decoded);
        lua_settop(L, top);
    }
}

bool remoteCallbackGet(lua_State* L, core::InstanceId id, std::string_view key)
{
    if (key != InvokeCallback || !isRemoteFunction(world(L), id))
        return false;
    const std::vector<ServiceState::InvokeHandler>& handlers = invokeState(L).invokeHandlers;
    const auto found = std::find_if(handlers.begin(), handlers.end(),
                                    [&](const ServiceState::InvokeHandler& h) { return h.remote == id; });
    if (found != handlers.end())
        lua_getref(L, found->functionRef);
    else
        lua_pushnil(L);
    return true;
}

bool remoteCallbackSet(lua_State* L, core::InstanceId id, std::string_view key, int valueIndex)
{
    World& w = world(L);
    if (key != InvokeCallback || !isRemoteFunction(w, id))
        return false;
    if (!lua_isnil(L, valueIndex) && !lua_isfunction(L, valueIndex))
        raise(L, ENG_TR("net.err.remote_handler"));
    std::vector<ServiceState::InvokeHandler>& handlers = invokeState(L).invokeHandlers;
    // The one it replaces goes, and so does any whose instance has: the list
    // is walked per question, and a destroyed function keeps nothing alive.
    for (usize index = 0; index < handlers.size();) {
        if (handlers[index].remote == id || !w.alive(handlers[index].remote)) {
            (void)lua_unref(L, handlers[index].functionRef);
            handlers.erase(handlers.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ++index;
    }
    if (lua_isfunction(L, valueIndex)) {
        lua_pushvalue(L, valueIndex);
        handlers.push_back(ServiceState::InvokeHandler{id, lua_ref(L, -1)});
        lua_pop(L, 1);
    }
    return true;
}

std::span<const InstanceMethodBinding> remoteMethodBindings() noexcept
{
    return RemoteMethods;
}

} // namespace engine::script
