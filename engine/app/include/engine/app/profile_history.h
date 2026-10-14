#pragma once

// **The frames a profiler reads** (ADR 0210): what the editor's Profiler panel
// is a picture of, and what `--profile-capture` writes to a file.
//
// A frame is where its time went -- by system, as the engine's scoped timers
// nest; by script; by pass of the GPU, where that was asked for -- with the
// few numbers of memory and of the network that were true when it ended. A
// history is the last so many of them, and the verbs somebody reading a hitch
// has: hold it still, choose a frame, go to the worst one.
//
// Nothing here draws, reads a clock or knows the engine is running: the host
// pushes a frame as it closes one, the panel reads, and so can a test. A
// history read back from a file is the same thing with nobody pushing.

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/types.h"

namespace engine::app {

// One frame, as the host hands it over. Names, not indices: the history keeps
// the tables.
struct ProfileSample
{
    // The engine's scopes that ran, depth first; `parent` is a row of this
    // list or -1 (`core::profile::lastFrame`).
    struct Scope
    {
        std::string_view name;
        core::i32 parent = -1;
        core::f32 ms = 0.0f;
        core::u32 calls = 0;
    };
    std::vector<Scope> scopes;
    // Scopes entered: what recording the frame cost is two clock reads each.
    core::u64 entries = 0;

    struct Script
    {
        std::string_view name;
        core::f32 ms = 0.0f;
        core::u32 resumes = 0;
    };
    std::vector<Script> scripts;

    // Lines sampled since the last frame; they add up over a history.
    struct Line
    {
        std::string_view source;
        int line = 0;
        core::u32 samples = 0;
    };
    std::vector<Line> lines;

    // Empty unless the GPU's passes were being timed, and then what timing
    // them cost this frame beside them.
    struct Pass
    {
        std::string_view name;
        core::f32 ms = 0.0f;
    };
    std::vector<Pass> passes;
    core::f32 gpuTimingMs = 0.0f;

    struct Memory
    {
        core::u64 scriptBytes = 0;
        core::u64 textureBytes = 0;
        core::u32 textures = 0;
        core::u64 bufferBytes = 0;
        core::u32 buffers = 0;
    };
    Memory memory;
    // The script heap, a script: what `ScriptRuntime::memoryByCategory` says.
    struct ScriptMemory
    {
        std::string_view name;
        core::u64 bytes = 0;
    };
    std::vector<ScriptMemory> scriptMemory;

    struct Network
    {
        bool connected = false;
        core::f32 pingMs = 0.0f;
        core::f32 jitterMs = 0.0f;
        core::f32 lossPercent = 0.0f;
        core::f32 snapshotsPerSecond = 0.0f;
        core::f32 resimulationMs = 0.0f;
        core::u32 peers = 0;
        // Corrections so far, the machine's and each local player's: a
        // history turns two frames' counts into what landed in a frame.
        core::u64 corrections = 0;
        core::f32 lastCorrectionMetres = 0.0f;
        std::array<core::u64, 4> playerCorrections{};
        std::array<bool, 4> playerPredicted{};
    };
    Network network;
};

// The bands a frame is drawn in on the strip, bottom to top.
enum class ProfileBand : core::u8
{
    Simulation,
    Scripts,
    Render,
    Interface,
    Waiting,
    Other,
    Count,
};
inline constexpr core::usize kProfileBandCount = static_cast<core::usize>(ProfileBand::Count);

// Which band a scope directly under the frame's root is drawn in, by its
// name: `simulation`, `scripts.*`, `render.*` and `content.*`, `ui.*`,
// `wait.*`, and anything else.
[[nodiscard]] ProfileBand profileBandOf(std::string_view scope) noexcept;

// Why a file was not read as a capture: the panel's to put into words.
enum class ProfileFileError : core::u8
{
    None,
    Unreadable,
    NotJson,
    NotACapture,
    OtherVersion,
    Broken,
};

// How a level of the systems' tree is ordered. Heaviest first is what a
// profiler opens on: the branch that holds the frame is the first screen.
enum class ProfileOrder : core::u8
{
    Heaviest,
    Entered,
};

class ProfileHistory
{
public:
    // How many frames are kept: the oldest goes as a new one comes.
    explicit ProfileHistory(core::usize capacity = 600);

    struct Frame
    {
        // Counted from the first frame this history was handed.
        core::u64 number = 0;
        // The frame's length: its root scopes together.
        core::f32 ms = 0.0f;
        std::array<core::f32, kProfileBandCount> bands{};
        core::u64 entries = 0;
        // Indices into the history's tables, with what the frame spent there.
        struct Spent
        {
            core::u32 index = 0;
            core::f32 ms = 0.0f;
            core::u32 count = 0;
        };
        std::vector<Spent> scopes;
        std::vector<Spent> scripts;
        std::vector<Spent> passes;
        core::f32 gpuTimingMs = 0.0f;
        ProfileSample::Memory memory;
        struct Bytes
        {
            core::u32 index = 0;
            core::u64 bytes = 0;
        };
        std::vector<Bytes> scriptMemory;
        ProfileSample::Network network;
        // Corrections that landed in this frame: the machine's, and each
        // local player's.
        core::u32 corrections = 0;
        std::array<core::u32, 4> playerCorrections{};
    };

    // A scope of the tree: its name and the scope it is under, or -1.
    struct Scope
    {
        std::string name;
        core::i32 parent = -1;
        core::u32 depth = 0;
    };

    // Takes a frame -- unless the history is held, when it is dropped: the
    // frames being read stay the frames being read.
    void push(const ProfileSample& sample);
    void clear();

    [[nodiscard]] core::usize size() const noexcept { return m_frames.size(); }
    [[nodiscard]] core::usize capacity() const noexcept { return m_capacity; }
    // Oldest first.
    [[nodiscard]] const Frame& frame(core::usize index) const noexcept { return m_frames[index]; }
    [[nodiscard]] const std::vector<Scope>& scopes() const noexcept { return m_scopes; }
    [[nodiscard]] const std::vector<std::string>& scripts() const noexcept { return m_scripts; }
    [[nodiscard]] const std::vector<std::string>& passes() const noexcept { return m_passes; }
    [[nodiscard]] const std::vector<std::string>& memoryScripts() const noexcept { return m_memoryScripts; }

    // **Held, and a frame chosen.** Holding stops frames coming in. Choosing
    // one holds; `resume` lets go of both. With nothing chosen a reader is
    // shown the run as a whole.
    void hold() noexcept { m_held = true; }
    void resume() noexcept;
    [[nodiscard]] bool held() const noexcept { return m_held; }
    void choose(core::usize index) noexcept;
    [[nodiscard]] std::optional<core::usize> chosen() const noexcept { return m_chosen; }
    // One frame older or newer than the one chosen; from none, the newest.
    void step(int by) noexcept;

    // **The frame worth reading**: the longest on the strip, chosen and held.
    // Again, the next longest after the one chosen; and round to the longest
    // after the shortest. False with no frames.
    bool chooseWorst() noexcept;
    bool chooseNextWorst() noexcept;
    // How many frames ran over `budgetMs`.
    [[nodiscard]] core::usize overBudget(core::f32 budgetMs) const noexcept;

    // What a tab shows: the chosen frame's, or -- with none chosen -- the
    // median over the frames kept of every row that ran in any.
    struct Row
    {
        std::string_view name;
        core::u32 depth = 0;
        core::f32 ms = 0.0f;
        // What of it the rows under it do not account for (systems only).
        core::f32 selfMs = 0.0f;
        // Calls of a scope, resumes of a script; at the median with no frame
        // chosen.
        core::f32 count = 0.0f;
        // Of the systems' tree only: which scope of the history's tree this
        // is, whether anything stands under it, and whether it is on **the
        // heaviest path** -- from the longest root down through the longest
        // scope under each, to a leaf. Where a frame's time is, is there.
        core::u32 scope = 0;
        bool leaf = true;
        bool heaviest = false;
    };
    // Depth first, as the engine's scopes nest; each level heaviest first,
    // or in the order its scopes were entered.
    [[nodiscard]] std::vector<Row> systems(ProfileOrder order = ProfileOrder::Heaviest) const;
    // Longest first.
    [[nodiscard]] std::vector<Row> scriptRows() const;
    // In the order the frame draws them.
    [[nodiscard]] std::vector<Row> passRows() const;
    // The frame's length the tabs are showing: the chosen frame's, or the median.
    [[nodiscard]] core::f32 shownMs() const;

    // Lines sampled, over everything this history was handed since it was
    // cleared: most samples first. A share of `lineSamples()`.
    struct Line
    {
        std::string source;
        int line = 0;
        core::u32 samples = 0;
    };
    [[nodiscard]] std::vector<Line> lines() const;
    [[nodiscard]] core::u64 lineSamples() const noexcept { return m_lineSamples; }

    // **Taken home**: the history as a file, and a file as a history -- held,
    // since nobody is pushing. A capture from a machine the editor is not on
    // is read exactly as a live run is. `load` leaves the history as it was
    // and says why when the file is not one of these.
    [[nodiscard]] bool save(const std::filesystem::path& file) const;
    [[nodiscard]] ProfileFileError load(const std::filesystem::path& file);
    [[nodiscard]] std::string toJson() const;
    [[nodiscard]] ProfileFileError fromJson(std::string_view text);

private:
    // The bands of a frame whose scopes are set, from the tree: the one rule,
    // for a frame pushed and a frame read from a file.
    void bandFrame(Frame& frame) const;
    [[nodiscard]] core::u32 scopeIndex(std::string_view name, core::i32 parent);
    [[nodiscard]] static core::u32 nameIndex(std::vector<std::string>& table, std::string_view name);
    [[nodiscard]] std::vector<Row> spentRows(const std::vector<std::string>& table,
                                             std::vector<Frame::Spent> Frame::*member, bool sorted) const;

    core::usize m_capacity = 600;
    std::vector<Frame> m_frames;
    std::vector<Scope> m_scopes;
    std::vector<std::string> m_scripts;
    std::vector<std::string> m_passes;
    std::vector<std::string> m_memoryScripts;
    std::vector<Line> m_lines;
    core::u64 m_lineSamples = 0;
    core::u64 m_pushed = 0;
    // The counts the last frame handed in ended on, to turn the next into a
    // difference; unset before the first.
    bool m_haveCounts = false;
    core::u64 m_lastCorrections = 0;
    std::array<core::u64, 4> m_lastPlayerCorrections{};
    bool m_held = false;
    std::optional<core::usize> m_chosen;
};

// The five things a frame is read by.
enum class ProfilerTab : core::u8
{
    Systems,
    Scripts,
    Gpu,
    Memory,
    Network,
};

// **The profiler as the editor has it**: the frames, and what the person
// reading them has switched. The host owns one and feeds it; the panel draws
// it and writes the switches.
struct Profiler
{
    explicit Profiler(core::usize frames = 600) : history(frames) {}

    ProfileHistory history;
    // The panel is on screen and reading the run: the host measures. A
    // profiler nobody is looking at measures nothing.
    bool watching = false;
    // The two things that cost a frame more than the scopes do, each asked
    // for by itself: the GPU's passes timed (a stop after every pass), and
    // the script's lines sampled.
    bool gpuPasses = false;
    bool lines = false;
    // The tab being read; the panel writes it.
    ProfilerTab tab = ProfilerTab::Systems;
    // How the Systems tree is ordered: the column whose header was pressed.
    ProfileOrder order = ProfileOrder::Heaviest;
    // What entering and leaving one scope costs on this machine, measured by
    // the host as it starts: two readings of the clock.
    core::f32 scopeCostMs = 0.0f;
    // The display's frame: the line across the strip.
    core::f32 budgetMs = 1000.0f / 60.0f;
    // **What counts as over it**: the display's frame and a tenth. A frame
    // held to the display lands a hair either side of its period, and a
    // strip where every other frame is marked says nothing about the one
    // that hitched.
    [[nodiscard]] core::f32 overMs() const noexcept { return budgetMs * 1.1f; }
    // A file being read rather than the run, and why the last one asked for
    // was not read.
    std::string file;
    ProfileFileError fileError = ProfileFileError::None;

    // **Whether the host measures this frame.** Only while somebody reads:
    // a capture was asked for, or the panel is on a screen that is shown and
    // is reading the run and not a file. Everything the profiler turns on --
    // the scopes, a script's time, the lines, the passes -- hangs off this
    // one answer, so a panel that is shut costs what a branch costs.
    [[nodiscard]] bool measures(bool capturing, bool interfaceShown) const noexcept
    {
        return capturing || (interfaceShown && watching && file.empty());
    }
};

} // namespace engine::app
