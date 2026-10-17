// **A world's health, for whatever watches it** (ADR 0220): `GET /health`
// and `GET /metrics` over plain HTTP, as JSON, on the game's port number over
// TCP unless told another.
//
// Off unless asked for, and on the machine's own address unless
// `health_allow` names others, by the rule administration uses (ADR 0219 §1).
// It says numbers and nothing else: no command reaches the world this way,
// and a request is read no further than its first line. One request at a time,
// each closed after its answer -- what a load balancer's probe or a scraper
// asks every few seconds, not a web server.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "engine/core/error.h"
#include "engine/core/types.h"

namespace engine::app {

struct HealthListener;

// What a request's first line asks, answered: the status code and the body.
struct HealthAnswer
{
    core::u16 status = 404;
    std::string body;
};
[[nodiscard]] HealthAnswer answerHealth(std::string_view requestLine, bool ready, const std::string& metrics);

class HealthServer
{
public:
    struct Options
    {
        core::u16 port = 0;
        // Listens beyond 127.0.0.1 only where `allow` names somebody.
        std::vector<std::string> allow;
    };

    HealthServer();
    ~HealthServer();
    HealthServer(const HealthServer&) = delete;
    HealthServer& operator=(const HealthServer&) = delete;

    [[nodiscard]] std::optional<core::EngineError> start(Options options);
    void stop();
    [[nodiscard]] bool running() const noexcept { return m_listener != nullptr; }

    // **From the frame loop**: whether the world takes players now, and its
    // numbers as a JSON object. What a request is answered with until the next.
    void publish(bool ready, std::string metrics);

private:
    void acceptLoop();

    Options m_options;
    std::unique_ptr<HealthListener> m_listener;
    std::thread m_acceptor;
    std::atomic<bool> m_stopping{false};
    std::mutex m_mutex;
    bool m_ready = false;
    std::string m_metrics = "{}";
};

} // namespace engine::app
