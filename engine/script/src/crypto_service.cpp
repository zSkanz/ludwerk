#include "engine/script/crypto_service.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>

#include "engine/core/crypto.h"
#include "engine/core/i18n.h"
#include "engine/core/text_key.h"
#include "engine/script/binding.h"
#include "engine/script/instance_binding.h"
#include "engine/script/services.h"
#include "engine/script/signals.h"

namespace engine::script {
namespace {

// The most `RandomBytes` hands over at once. A key is 32 bytes and a salt 16;
// a megabyte is past anything a game means, and a wall before a script that
// asked for `math.huge`.
constexpr core::f64 MaxRandomBytes = 1024.0 * 1024.0;

// The longest password hashed. Long enough for any passphrase, and a wall
// before a client that sends a megabyte to be chewed at 64 MiB a time.
constexpr size_t MaxPasswordBytes = 1024;

[[nodiscard]] ServiceState& services(lua_State* L) noexcept
{
    return *context(L).services;
}

// A string's bytes or a buffer's: what is hashed is bytes, and a script has
// them as either.
[[nodiscard]] std::span<const core::u8> checkBytes(lua_State* L, int index)
{
    size_t length = 0;
    if (lua_type(L, index) == LUA_TSTRING) {
        const char* text = lua_tolstring(L, index, &length);
        return {reinterpret_cast<const core::u8*>(text), length};
    }
    if (const void* bytes = lua_tobuffer(L, index, &length); bytes != nullptr)
        return {static_cast<const core::u8*>(bytes), length};
    luaL_typeerrorL(L, index, "string or buffer");
}

[[nodiscard]] std::string checkPassword(lua_State* L, int index)
{
    if (lua_type(L, index) != LUA_TSTRING)
        luaL_typeerrorL(L, index, "string");
    size_t length = 0;
    const char* text = lua_tolstring(L, index, &length);
    if (length > MaxPasswordBytes) {
        const core::I18nArg args[] = {{"limit", static_cast<core::i64>(MaxPasswordBytes)}};
        raise(L, ENG_TR("script.err.crypto_password_length"), args);
    }
    return std::string(text, length);
}

void pushHex(lua_State* L, const core::Sha256& digest)
{
    const std::string hex = core::toHex(digest);
    lua_pushlstring(L, hex.data(), hex.size());
}

// Parks the calling thread on `ticket`, or -- the main thread, or a caller
// that cannot wait -- says it could not, and the caller answers now.
[[nodiscard]] bool park(lua_State* L, core::u64 ticket)
{
    ServiceState& state = services(L);
    lua_pushthread(L);
    state.passwordWaiters.push_back({ticket, lua_ref(L, -1)});
    lua_pop(L, 1);
    return true;
}

[[nodiscard]] bool canWait(lua_State* L)
{
    const bool main = lua_pushthread(L) != 0;
    lua_pop(L, 1);
    return !main && lua_isyieldable(L) != 0;
}

[[nodiscard]] PasswordWorker& worker(lua_State* L)
{
    ServiceState& state = services(L);
    if (!state.passwords)
        state.passwords = std::make_unique<PasswordWorker>();
    return *state.passwords;
}

} // namespace

// --- the thread ------------------------------------------------------------------

PasswordWorker::~PasswordWorker()
{
    {
        const std::lock_guard lock{m_mutex};
        m_stop = true;
        m_queue.clear();
    }
    m_wake.notify_all();
    if (m_thread.joinable())
        m_thread.join();
}

core::u64 PasswordWorker::submit(std::string password, std::string hash, bool verify)
{
    core::u64 ticket = 0;
    {
        const std::lock_guard lock{m_mutex};
        ticket = m_next++;
        m_queue.push_back({ticket, verify, std::move(password), std::move(hash)});
        if (!m_thread.joinable())
            m_thread = std::thread{[this] { run(); }};
    }
    m_wake.notify_one();
    return ticket;
}

void PasswordWorker::collect(std::vector<Done>& out)
{
    const std::lock_guard lock{m_mutex};
    out.insert(out.end(), std::make_move_iterator(m_done.begin()), std::make_move_iterator(m_done.end()));
    m_done.clear();
}

void PasswordWorker::run()
{
    for (;;) {
        Job job;
        {
            std::unique_lock lock{m_mutex};
            m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop)
                return;
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        Done done;
        done.ticket = job.ticket;
        done.verify = job.verify;
        if (job.verify)
            done.matched = core::verifyPassword(job.hash, job.password);
        else
            done.hash = core::hashPassword(job.password);
        const std::lock_guard lock{m_mutex};
        m_done.push_back(std::move(done));
    }
}

// --- CryptoService -----------------------------------------------------------------

int cryptoServiceRandomBytes(lua_State* L)
{
    (void)checkInstance(L, 1);
    const core::f64 count = luaL_checknumber(L, 2);
    if (!(count >= 0.0) || count > MaxRandomBytes || std::floor(count) != count) {
        const core::I18nArg args[] = {{"limit", static_cast<core::i64>(MaxRandomBytes)}};
        raise(L, ENG_TR("script.err.crypto_random_count"), args);
    }
    const auto size = static_cast<size_t>(count);
    void* out = lua_newbuffer(L, size);
    core::secureRandom({static_cast<core::u8*>(out), size});
    return 1;
}

int cryptoServiceRandomInteger(lua_State* L)
{
    (void)checkInstance(L, 1);
    const core::f64 low = luaL_checknumber(L, 2);
    const core::f64 high = luaL_checknumber(L, 3);
    // Whole numbers a double holds exactly, the first no more than the second.
    constexpr core::f64 Exact = 9007199254740992.0;
    if (!(low <= high) || std::floor(low) != low || std::floor(high) != high || std::abs(low) > Exact ||
        std::abs(high) > Exact || high - low >= Exact)
        raise(L, ENG_TR("script.err.crypto_random_range"));

    // **Every value as likely as the next**: a draw that falls in the uneven
    // tail of the 64-bit range is thrown away and drawn again, where taking a
    // remainder would favour the low numbers -- by a part in billions for a
    // die, and by nearly half for a range close to the generator's own.
    const auto span = static_cast<core::u64>(high - low) + 1;
    const core::u64 limit = UINT64_MAX - (UINT64_MAX % span + 1) % span;
    core::u64 draw = 0;
    do {
        core::secureRandom({reinterpret_cast<core::u8*>(&draw), sizeof(draw)});
    } while (draw > limit);
    lua_pushnumber(L, low + static_cast<core::f64>(draw % span));
    return 1;
}

int cryptoServiceUniqueId(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::string id = core::uniqueId();
    lua_pushlstring(L, id.data(), id.size());
    return 1;
}

int cryptoServiceSha256(lua_State* L)
{
    (void)checkInstance(L, 1);
    pushHex(L, core::sha256(checkBytes(L, 2)));
    return 1;
}

int cryptoServiceHmacSha256(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::span<const core::u8> key = checkBytes(L, 2);
    pushHex(L, core::hmacSha256(key, checkBytes(L, 3)));
    return 1;
}

int cryptoServiceSecureEquals(lua_State* L)
{
    (void)checkInstance(L, 1);
    const std::span<const core::u8> a = checkBytes(L, 2);
    lua_pushboolean(L, core::secureEquals(a, checkBytes(L, 3)) ? 1 : 0);
    return 1;
}

int cryptoServiceHashPasswordAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    std::string password = checkPassword(L, 2);
    if (!canWait(L)) {
        // The main thread, or a caller that cannot wait: answered now, at the
        // cost of the tick it is in.
        const std::string hash = core::hashPassword(password);
        if (hash.empty())
            raise(L, ENG_TR("script.err.crypto_password_memory"));
        lua_pushlstring(L, hash.data(), hash.size());
        return 1;
    }
    (void)park(L, worker(L).submit(std::move(password), {}, false));
    return lua_yield(L, 0);
}

int cryptoServiceVerifyPasswordAsync(lua_State* L)
{
    (void)checkInstance(L, 1);
    std::string password = checkPassword(L, 2);
    if (lua_type(L, 3) != LUA_TSTRING)
        luaL_typeerrorL(L, 3, "string");
    size_t length = 0;
    const char* text = lua_tolstring(L, 3, &length);
    std::string hash(text, length);
    if (!canWait(L)) {
        lua_pushboolean(L, core::verifyPassword(hash, password) ? 1 : 0);
        return 1;
    }
    (void)park(L, worker(L).submit(std::move(password), std::move(hash), true));
    return lua_yield(L, 0);
}

void resumeCryptoWaiters(lua_State* L)
{
    ServiceState& state = services(L);
    if (!state.passwords || state.passwordWaiters.empty())
        return;
    std::vector<PasswordWorker::Done> done;
    state.passwords->collect(done);
    for (const PasswordWorker::Done& result : done) {
        // Taken out before it is resumed: a resumed thread may hash another,
        // and the vector it would push onto is the one being searched.
        const auto found = std::find_if(state.passwordWaiters.begin(), state.passwordWaiters.end(),
                                        [&](const auto& waiter) { return waiter.ticket == result.ticket; });
        if (found == state.passwordWaiters.end())
            continue;
        const int threadRef = found->threadRef;
        state.passwordWaiters.erase(found);

        lua_getref(L, threadRef);
        lua_State* co = lua_tothread(L, -1);
        if (co != nullptr) {
            if (result.verify) {
                lua_pushboolean(co, result.matched ? 1 : 0);
                (void)resumeScheduled(L, co, 1);
            }
            else if (!result.hash.empty()) {
                lua_pushlstring(co, result.hash.data(), result.hash.size());
                (void)resumeScheduled(L, co, 1);
            }
            else {
                const std::string message = core::formatKeyPrefixed(ENG_TR("script.err.crypto_password_memory"), {});
                lua_pushlstring(co, message.data(), message.size());
                (void)resumeScheduledWithError(L, co);
            }
        }
        lua_pop(L, 1);
        (void)lua_unref(L, threadRef);
    }
}

} // namespace engine::script
