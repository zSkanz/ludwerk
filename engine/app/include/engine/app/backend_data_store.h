// **A game's own backend as the game's database** (ADR 0238 §5): every
// operation the engine's own store does, sent as JSON to
// `POST <backend>/v1/data` and answered in the same JSON -- the shapes
// `runDataOp` reads and writes, plus a record's `load` and `write`.
//
// It runs on the data worker's thread (`DataWorker`), so the time a backend
// takes is never a frame of the manager's. Each request carries `request`, an
// id of its own: one that failed in transport is sent again with the same id,
// up to three times in ten seconds, and a backend applies a `transact` once
// per id -- the answer lost, never the trade doubled. Past that, the
// operation is `Unavailable`.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "engine/app/data_store.h"
#include "engine/core/types.h"

namespace engine::app {

struct BackendOptions
{
    // `https://...` anywhere; `http://` only to this machine or the local
    // network (see `backendUrlAllowed`).
    std::string url;
    // The whole address asked, where it is not `<url>/v1/data`: Nakama's
    // RPC (`nakamaDataEndpoint`).
    std::string endpoint;
    // Sent as `Authorization: Bearer <token>`; empty for none.
    std::string token;
    // Each try's time, and all tries' together.
    core::u32 tryMs = 4000;
    core::u32 totalMs = 10000;
    core::u32 tries = 3;
    // What sends a request and answers its body, or none where it got no
    // answer: the HTTP client, or a test's.
    std::function<std::optional<std::string>(const std::string& url, const std::string& body, const std::string& token,
                                             core::u32 timeoutMs)>
        send;
};

// **Whether `url` may be a backend**: `https://` to anywhere, `http://` only
// to `localhost` or an address written out that is loopback (127.0.0.0/8,
// ::1), private (RFC 1918: 10/8, 172.16/12, 192.168/16) or link-local
// (169.254/16, fe80::/10) -- a name could resolve anywhere, and what crosses
// the open internet in the clear is a player's inventory. False, and why.
[[nodiscard]] bool backendUrlAllowed(std::string_view url, std::string& why);

[[nodiscard]] std::unique_ptr<IDataStore> makeBackendDataStore(BackendOptions options);

// **The game's Nakama** (ADR 0242): the module's `engine_data` RPC, asked
// with the server's HTTP key and its body as the payload (`unwrap`); and who
// a player's session is, by `engine_whoami` with the session as its bearer --
// none for one Nakama refuses or that did not answer.
[[nodiscard]] std::string nakamaDataEndpoint(std::string_view url, std::string_view httpKey);
[[nodiscard]] std::function<std::optional<std::string>(const std::string& session)> nakamaWhoami(std::string url);
// **A match Nakama's matchmaker made**, by the module's `engine_match_users`
// with the server's HTTP key: its players and queue, or none.
struct NakamaMatchRecord
{
    std::vector<std::string> users;
    std::string queue;
};
[[nodiscard]] std::function<std::optional<NakamaMatchRecord>(const std::string& match)>
nakamaMatchRecords(std::string url, std::string httpKey);

} // namespace engine::app
