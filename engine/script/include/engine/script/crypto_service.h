// `CryptoService`, bound into the VM (ADR 0151): chance that is not the
// simulation's, two hashes, and a hash meant for passwords. The primitives are
// `engine/core/crypto.h`'s; this is the Luau face of them, and the thread a
// password is hashed on.
#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/core/types.h"

struct lua_State;

namespace engine::script {

// **One thread, and a queue.** A password's hash costs tens of milliseconds
// and 64 MiB on purpose, which is several ticks of a server every player on it
// would feel -- so it is not done on the tick. And it is ONE thread rather
// than one a request: a hundred logins arriving together are hashed one after
// another in 64 MiB, where a thread each would be 6 GiB and a way to take a
// server down by knocking.
//
// Started by the first request, so a game that never hashes a password never
// has the thread.
class PasswordWorker
{
public:
    PasswordWorker() = default;
    PasswordWorker(const PasswordWorker&) = delete;
    PasswordWorker& operator=(const PasswordWorker&) = delete;
    // Finishes the hash it is in the middle of, drops the ones still queued.
    ~PasswordWorker();

    struct Done
    {
        core::u64 ticket = 0;
        bool verify = false;
        // `verify`: whether the password was the hash's.
        bool matched = false;
        // Not `verify`: the hash, or empty when the memory could not be had.
        std::string hash;
    };

    // Queues a hash of `password`, or -- `verify` -- a check of it against
    // `hash`. The ticket is what `collect` names it by.
    core::u64 submit(std::string password, std::string hash, bool verify);

    // What has finished since the last call, in the order it was asked for.
    void collect(std::vector<Done>& out);

private:
    struct Job
    {
        core::u64 ticket = 0;
        bool verify = false;
        std::string password;
        std::string hash;
    };

    void run();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_queue;
    std::vector<Done> m_done;
    std::thread m_thread;
    core::u64 m_next = 1;
    bool m_stop = false;
};

int cryptoServiceRandomBytes(lua_State* L);
int cryptoServiceRandomInteger(lua_State* L);
int cryptoServiceUniqueId(lua_State* L);
int cryptoServiceSha256(lua_State* L);
int cryptoServiceHmacSha256(lua_State* L);
int cryptoServiceSecureEquals(lua_State* L);
int cryptoServiceHashPasswordAsync(lua_State* L);
int cryptoServiceVerifyPasswordAsync(lua_State* L);

// Every tick: each thread whose password has been hashed or checked, resumed.
void resumeCryptoWaiters(lua_State* L);

} // namespace engine::script
