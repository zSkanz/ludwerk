#include "engine/core/profile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <numeric>

namespace engine::core::profile {

namespace {

[[nodiscard]] u64 steadyNs()
{
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// How many frames a scope remembers: the last four and a half minutes at
// sixty a second, which is longer than any `--frame-stats` run, and bounded
// so a long one costs a fixed few megabytes rather than growing.
constexpr usize HistoryFrames = 16384;

struct Node
{
    i32 parent = -1;
    u32 site = 0;
    std::vector<i32> children;
    u64 frameNs = 0;
    u32 frameCalls = 0;
    // A frame's sample at `frame % HistoryFrames`; a scope first entered late
    // has zeros for the frames before it.
    std::vector<f32> ms;
    std::vector<u32> calls;
};

struct State
{
    std::mutex sitesMutex;
    std::vector<const char*> sites;
    std::vector<Node> nodes;
    std::vector<i32> roots;
    i32 current = -1;
    u64 frames = 0;
    // Each kept frame's mark, as `ms` keeps a scope's, and this frame's.
    std::vector<u8> settling;
    bool settlingNow = false;
    // Bumped when the tree is thrown away, so a scope open across it does not
    // close into a node that is somebody else's.
    u32 generation = 0;
    Clock clock = steadyNs;
};

State g_state;
// Only the thread that turned it on records.
thread_local bool t_recording = false;
thread_local u32 t_generation = 0;
enum class CapturePhase
{
    Idle,
    Warmup,
    Recording,
    Complete
};
thread_local CapturePhase t_capturePhase = CapturePhase::Idle;
thread_local u64 t_captureRequested = 0;
thread_local u64 t_captureBegan = 0;
thread_local u64 t_captureDuration = 0;
thread_local u64 t_captureWarmup = 0;
thread_local CaptureReport t_captureReport;

[[nodiscard]] i32 childOf(State& s, u32 site)
{
    std::vector<i32>& siblings = s.current < 0 ? s.roots : s.nodes[static_cast<usize>(s.current)].children;
    for (const i32 index : siblings) {
        if (s.nodes[static_cast<usize>(index)].site == site)
            return index;
    }
    const i32 index = static_cast<i32>(s.nodes.size());
    Node node;
    node.parent = s.current;
    node.site = site;
    s.nodes.push_back(std::move(node));
    // `siblings` may have moved with the push when it is a node's own list.
    (s.current < 0 ? s.roots : s.nodes[static_cast<usize>(s.current)].children).push_back(index);
    return index;
}

[[nodiscard]] f64 at(const std::vector<f64>& ordered, f64 fraction)
{
    if (ordered.empty())
        return 0.0;
    const auto index = static_cast<usize>(fraction * static_cast<f64>(ordered.size() - 1) + 0.5);
    return ordered[std::min(index, ordered.size() - 1)];
}

} // namespace

u32 registerSite(const char* name)
{
    const std::lock_guard lock(g_state.sitesMutex);
    g_state.sites.push_back(name);
    return static_cast<u32>(g_state.sites.size() - 1);
}

void setEnabled(bool on)
{
    t_capturePhase = CapturePhase::Idle;
    g_state.nodes.clear();
    g_state.roots.clear();
    g_state.current = -1;
    g_state.frames = 0;
    g_state.settling.clear();
    g_state.settlingNow = false;
    g_state.generation += 1;
    t_generation = g_state.generation;
    t_recording = on;
}

bool enabled() noexcept
{
    return t_recording;
}

void setClockForTests(Clock clock) noexcept
{
    g_state.clock = clock != nullptr ? clock : steadyNs;
}

namespace {

void open(u32 site, i32& node, u64& start) noexcept
{
    if (!t_recording)
        return;
    node = childOf(g_state, site);
    g_state.current = node;
    start = g_state.clock();
}

void close(i32& index, u64 start) noexcept
{
    const i32 opened = index;
    index = -1;
    if (opened < 0 || !t_recording || t_generation != g_state.generation ||
        static_cast<usize>(opened) >= g_state.nodes.size())
        return;
    Node& node = g_state.nodes[static_cast<usize>(opened)];
    node.frameNs += g_state.clock() - start;
    node.frameCalls += 1;
    g_state.current = node.parent;
}

} // namespace

Scope::Scope(u32 site) noexcept
{
    open(site, m_node, m_start);
}

Scope::~Scope()
{
    close(m_node, m_start);
}

void Sections::next(u32 site) noexcept
{
    close();
    open(site, m_node, m_start);
}

void Sections::close() noexcept
{
    profile::close(m_node, m_start);
}

void endFrame()
{
    const bool warming = t_capturePhase == CapturePhase::Warmup;
    if (warming && g_state.clock() - t_captureRequested >= t_captureWarmup) {
        const u64 duration = t_captureDuration;
        setEnabled(true);
        t_captureDuration = duration;
        t_captureBegan = g_state.clock();
        t_capturePhase = CapturePhase::Recording;
        return; // The frame just closed belongs to warm-up, not the sample.
    }
    if (!t_recording)
        return;
    const usize slot = static_cast<usize>(g_state.frames % HistoryFrames);
    const usize length = static_cast<usize>(std::min<u64>(g_state.frames + 1, HistoryFrames));
    for (Node& node : g_state.nodes) {
        if (node.ms.size() < length) {
            node.ms.resize(length, 0.0f);
            node.calls.resize(length, 0u);
        }
        node.ms[slot] = static_cast<f32>(static_cast<f64>(node.frameNs) / 1.0e6);
        node.calls[slot] = node.frameCalls;
        node.frameNs = 0;
        node.frameCalls = 0;
    }
    if (g_state.settling.size() < length)
        g_state.settling.resize(length, 0);
    g_state.settling[slot] = g_state.settlingNow ? 1 : 0;
    g_state.settlingNow = false;
    g_state.frames += 1;
    if (t_capturePhase == CapturePhase::Recording && g_state.clock() - t_captureBegan >= t_captureDuration) {
        t_captureReport.frames = g_state.frames;
        t_captureReport.seconds = static_cast<f64>(g_state.clock() - t_captureBegan) / 1.0e9;
        t_captureReport.scopes = report(0);
        t_recording = false;
        t_capturePhase = CapturePhase::Complete;
    }
}

bool requestCapture(f64 seconds, f64 warmupSeconds)
{
    if (!std::isfinite(seconds) || !std::isfinite(warmupSeconds) || seconds <= 0.0 || seconds > 120.0 ||
        warmupSeconds < 0.0 || warmupSeconds > 120.0 || t_capturePhase == CapturePhase::Warmup ||
        t_capturePhase == CapturePhase::Recording)
        return false;
    t_captureReport = {};
    t_captureDuration = static_cast<u64>(seconds * 1.0e9);
    t_captureWarmup = static_cast<u64>(warmupSeconds * 1.0e9);
    t_captureRequested = g_state.clock();
    t_capturePhase = CapturePhase::Warmup;
    return true;
}

const CaptureReport* captured() noexcept
{
    return t_capturePhase == CapturePhase::Complete ? &t_captureReport : nullptr;
}

void markSettling() noexcept
{
    if (t_recording)
        g_state.settlingNow = true;
}

std::vector<SpikeReport> spikes(usize skipFrames, usize worst, f64 factor, usize most, f64 smallestMs)
{
    std::vector<SpikeReport> out;
    // The frames kept, as `report` keeps them, with the frame number of each.
    std::vector<std::pair<usize, u64>> kept;
    if (g_state.frames <= HistoryFrames) {
        for (usize frame = std::min<usize>(skipFrames, static_cast<usize>(g_state.frames));
             frame < static_cast<usize>(g_state.frames); ++frame)
            kept.emplace_back(frame, frame);
    }
    else {
        for (u64 frame = g_state.frames - HistoryFrames; frame < g_state.frames; ++frame)
            kept.emplace_back(static_cast<usize>(frame % HistoryFrames), frame);
    }
    // Not the world settling: the slots are play's.
    std::erase_if(kept, [](const std::pair<usize, u64>& frame) {
        return frame.first < g_state.settling.size() && g_state.settling[frame.first] != 0;
    });
    if (kept.empty())
        return out;

    const auto sampleOf = [](const Node& node, usize slot) {
        return slot < node.ms.size() ? static_cast<f64>(node.ms[slot]) : 0.0;
    };
    // A frame is its roots together.
    std::vector<std::pair<f64, usize>> totals;
    totals.reserve(kept.size());
    for (usize index = 0; index < kept.size(); ++index) {
        f64 total = 0.0;
        for (const i32 root : g_state.roots)
            total += sampleOf(g_state.nodes[static_cast<usize>(root)], kept[index].first);
        totals.emplace_back(total, index);
    }
    std::vector<f64> ordered;
    ordered.reserve(totals.size());
    for (const auto& [total, index] : totals)
        ordered.push_back(total);
    std::sort(ordered.begin(), ordered.end());
    const f64 median = at(ordered, 0.5);

    std::sort(totals.begin(), totals.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (usize rank = 0; rank < totals.size() && out.size() < most; ++rank) {
        const auto [total, index] = totals[rank];
        if (rank >= worst && total <= factor * median)
            break;
        const usize slot = kept[index].first;
        SpikeReport spike;
        spike.frame = kept[index].second;
        spike.ms = total;
        std::vector<std::pair<i32, u32>> stack;
        for (auto root = g_state.roots.rbegin(); root != g_state.roots.rend(); ++root)
            stack.emplace_back(*root, 0u);
        while (!stack.empty()) {
            const auto [node, depth] = stack.back();
            stack.pop_back();
            const Node& at = g_state.nodes[static_cast<usize>(node)];
            const f64 ms = sampleOf(at, slot);
            // What is smaller than this is smaller below it too.
            if (ms < smallestMs)
                continue;
            SpikeRow row;
            {
                const std::lock_guard lock(g_state.sitesMutex);
                row.name = g_state.sites[at.site];
            }
            row.depth = depth;
            row.ms = ms;
            row.calls = slot < at.calls.size() ? at.calls[slot] : 0u;
            spike.rows.push_back(std::move(row));
            for (auto child = at.children.rbegin(); child != at.children.rend(); ++child)
                stack.emplace_back(*child, depth + 1);
        }
        out.push_back(std::move(spike));
    }
    return out;
}

std::vector<ScopeReport> report(usize skipFrames)
{
    std::vector<ScopeReport> out;
    // The frames kept: all of them less the warm-up while they fit, and the
    // last `HistoryFrames` once they do not -- the warm-up is long gone then.
    std::vector<usize> kept;
    if (g_state.frames <= HistoryFrames) {
        for (usize frame = std::min<usize>(skipFrames, static_cast<usize>(g_state.frames));
             frame < static_cast<usize>(g_state.frames); ++frame)
            kept.push_back(frame);
    }
    else {
        for (usize slot = 0; slot < HistoryFrames; ++slot)
            kept.push_back(slot);
    }

    const auto sampleOf = [](const Node& node, usize slot) {
        return slot < node.ms.size() ? static_cast<f64>(node.ms[slot]) : 0.0;
    };

    // Depth first, in the order scopes were first entered.
    std::vector<std::pair<i32, u32>> stack;
    for (auto root = g_state.roots.rbegin(); root != g_state.roots.rend(); ++root)
        stack.emplace_back(*root, 0u);
    while (!stack.empty()) {
        const auto [index, depth] = stack.back();
        stack.pop_back();
        const Node& node = g_state.nodes[static_cast<usize>(index)];

        std::vector<f64> total;
        std::vector<f64> self;
        std::vector<f64> calls;
        total.reserve(kept.size());
        for (const usize slot : kept) {
            const f64 spent = sampleOf(node, slot);
            f64 children = 0.0;
            for (const i32 child : node.children)
                children += sampleOf(g_state.nodes[static_cast<usize>(child)], slot);
            total.push_back(spent);
            self.push_back(std::max(0.0, spent - children));
            calls.push_back(slot < node.calls.size() ? static_cast<f64>(node.calls[slot]) : 0.0);
        }
        std::sort(total.begin(), total.end());
        std::sort(self.begin(), self.end());
        std::sort(calls.begin(), calls.end());
        // A scope that did not run in any frame kept -- one entered while a
        // scene loaded, before the frames measured -- is not a row of zeros;
        // nor is anything under it.
        if (calls.empty() || calls.back() == 0.0)
            continue;

        ScopeReport row;
        {
            const std::lock_guard lock(g_state.sitesMutex);
            row.name = g_state.sites[node.site];
        }
        row.depth = depth;
        row.medianMs = at(total, 0.5);
        row.p95Ms = at(total, 0.95);
        row.p99Ms = at(total, 0.99);
        row.bestMs = total.empty() ? 0.0 : total.front();
        row.meanMs =
            total.empty() ? 0.0 : std::accumulate(total.begin(), total.end(), 0.0) / static_cast<f64>(total.size());
        row.worstMs = total.empty() ? 0.0 : total.back();
        row.selfMedianMs = at(self, 0.5);
        row.calls = at(calls, 0.5);
        out.push_back(std::move(row));

        for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
            stack.emplace_back(*child, depth + 1);
    }
    return out;
}

} // namespace engine::core::profile
