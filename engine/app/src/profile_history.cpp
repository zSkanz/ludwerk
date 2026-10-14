#include "engine/app/profile_history.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/platform/file.h"

namespace engine::app {
namespace {

using core::f32;
using core::i32;
using core::u32;
using core::u64;
using core::usize;

constexpr std::string_view kFormat = "profile-capture";
constexpr core::i64 kVersion = 1;

[[nodiscard]] f32 medianOf(std::vector<f32>& values)
{
    if (values.empty())
        return 0.0f;
    const usize middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle), values.end());
    return values[middle];
}

} // namespace

ProfileBand profileBandOf(std::string_view scope) noexcept
{
    // The names are the engine's own scopes (`ENG_PROFILE_SCOPE`); what none
    // of these says is in the band of the scope it stands in.
    if (scope == "simulation" || scope.starts_with("simulation.") || scope == "frame.simulation" ||
        scope == "physics" || scope.starts_with("physics."))
        return ProfileBand::Simulation;
    if (scope.starts_with("scripts."))
        return ProfileBand::Scripts;
    if (scope.starts_with("render.") || scope.starts_with("content.") || scope == "draw.render" ||
        scope == "draw.uploads" || scope == "draw.choose")
        return ProfileBand::Render;
    if (scope.starts_with("ui.") || scope == "draw.interface" || scope == "frame.overlays" || scope == "frame.editor")
        return ProfileBand::Interface;
    if (scope.starts_with("wait.") || scope == "frame.present")
        return ProfileBand::Waiting;
    return ProfileBand::Other;
}

ProfileHistory::ProfileHistory(usize capacity) : m_capacity(std::max<usize>(capacity, 1))
{}

u32 ProfileHistory::scopeIndex(std::string_view name, i32 parent)
{
    for (usize index = 0; index < m_scopes.size(); ++index) {
        if (m_scopes[index].parent == parent && m_scopes[index].name == name)
            return static_cast<u32>(index);
    }
    Scope scope;
    scope.name = std::string(name);
    scope.parent = parent;
    scope.depth = parent < 0 ? 0u : m_scopes[static_cast<usize>(parent)].depth + 1u;
    m_scopes.push_back(std::move(scope));
    return static_cast<u32>(m_scopes.size() - 1);
}

u32 ProfileHistory::nameIndex(std::vector<std::string>& table, std::string_view name)
{
    for (usize index = 0; index < table.size(); ++index) {
        if (table[index] == name)
            return static_cast<u32>(index);
    }
    table.emplace_back(name);
    return static_cast<u32>(table.size() - 1);
}

void ProfileHistory::push(const ProfileSample& sample)
{
    // The counts are followed even while held: the frame that comes after a
    // hold is not handed every correction of the hold.
    const bool counted = m_haveCounts;
    const u64 correctionsBefore = m_lastCorrections;
    const std::array<u64, 4> playersBefore = m_lastPlayerCorrections;
    m_haveCounts = true;
    m_lastCorrections = sample.network.corrections;
    m_lastPlayerCorrections = sample.network.playerCorrections;
    if (m_held)
        return;

    Frame frame;
    frame.number = m_pushed++;
    frame.entries = sample.entries;

    // A row's place in this frame's list, as a scope of the history's tree.
    std::vector<u32> placed(sample.scopes.size(), 0u);
    for (usize row = 0; row < sample.scopes.size(); ++row) {
        const ProfileSample::Scope& scope = sample.scopes[row];
        const i32 parent = scope.parent >= 0 && static_cast<usize>(scope.parent) < row
                               ? static_cast<i32>(placed[static_cast<usize>(scope.parent)])
                               : -1;
        placed[row] = scopeIndex(scope.name, parent);
        frame.scopes.push_back({placed[row], scope.ms, scope.calls});
        if (parent < 0)
            frame.ms += scope.ms;
    }
    bandFrame(frame);

    for (const ProfileSample::Script& script : sample.scripts)
        frame.scripts.push_back({nameIndex(m_scripts, script.name), script.ms, script.resumes});
    for (const ProfileSample::Pass& pass : sample.passes)
        frame.passes.push_back({nameIndex(m_passes, pass.name), pass.ms, 1u});
    frame.gpuTimingMs = sample.gpuTimingMs;
    frame.memory = sample.memory;
    for (const ProfileSample::ScriptMemory& held : sample.scriptMemory)
        frame.scriptMemory.push_back({nameIndex(m_memoryScripts, held.name), held.bytes});
    frame.network = sample.network;
    if (counted) {
        const auto since = [](u64 now, u64 before) { return now >= before ? static_cast<u32>(now - before) : 0u; };
        frame.corrections = since(sample.network.corrections, correctionsBefore);
        for (usize seat = 0; seat < 4; ++seat)
            frame.playerCorrections[seat] = since(sample.network.playerCorrections[seat], playersBefore[seat]);
    }

    for (const ProfileSample::Line& line : sample.lines) {
        auto kept = std::find_if(m_lines.begin(), m_lines.end(), [&line](const Line& row) {
            return row.line == line.line && row.source == line.source;
        });
        if (kept == m_lines.end()) {
            m_lines.push_back({std::string(line.source), line.line, 0u});
            kept = m_lines.end() - 1;
        }
        kept->samples += line.samples;
        m_lineSamples += line.samples;
    }

    if (m_frames.size() >= m_capacity)
        m_frames.erase(m_frames.begin());
    m_frames.push_back(std::move(frame));
}

void ProfileHistory::clear()
{
    m_frames.clear();
    m_scopes.clear();
    m_scripts.clear();
    m_passes.clear();
    m_memoryScripts.clear();
    m_lines.clear();
    m_lineSamples = 0;
    m_pushed = 0;
    m_haveCounts = false;
    m_chosen.reset();
}

// **The bands.** A scope is in the band its name says, or -- when its name
// says none -- in the band of the scope it stands in; and what a scope spent
// that the scopes under it do not account for is its own band's. So a
// script's step under the simulation's tick is the scripts', the wait under
// the present is waiting, and the bands of a frame add up to the frame.
//
// Written once, for a frame pushed and a frame read from a file: it was
// written twice, the file's copy kept the rule the first had before it was
// corrected, and a capture opened in the editor drew a wait as "the rest".
void ProfileHistory::bandFrame(Frame& frame) const
{
    frame.bands = {};
    // What each scope of the tree spent in this frame, and what stands under it.
    std::vector<f32> spent(m_scopes.size(), 0.0f);
    std::vector<f32> under(m_scopes.size(), 0.0f);
    std::vector<bool> ran(m_scopes.size(), false);
    for (const Frame::Spent& row : frame.scopes) {
        if (row.index >= m_scopes.size())
            continue;
        spent[row.index] = row.ms;
        ran[row.index] = true;
        const i32 parent = m_scopes[row.index].parent;
        if (parent >= 0)
            under[static_cast<usize>(parent)] += row.ms;
    }
    // The tree is in the order its scopes were first met, so a parent is
    // always behind what stands in it.
    std::vector<ProfileBand> bandOf(m_scopes.size(), ProfileBand::Other);
    for (usize index = 0; index < m_scopes.size(); ++index) {
        const Scope& scope = m_scopes[index];
        const ProfileBand named = profileBandOf(scope.name);
        const bool inherits =
            named == ProfileBand::Other && scope.parent >= 0 && static_cast<usize>(scope.parent) < index;
        bandOf[index] = inherits ? bandOf[static_cast<usize>(scope.parent)] : named;
        if (ran[index])
            frame.bands[static_cast<usize>(bandOf[index])] += std::max(0.0f, spent[index] - under[index]);
    }
}

void ProfileHistory::resume() noexcept
{
    m_held = false;
    m_chosen.reset();
}

void ProfileHistory::choose(usize index) noexcept
{
    if (m_frames.empty())
        return;
    m_chosen = std::min(index, m_frames.size() - 1);
    m_held = true;
}

void ProfileHistory::step(int by) noexcept
{
    if (m_frames.empty())
        return;
    const usize last = m_frames.size() - 1;
    if (!m_chosen.has_value()) {
        choose(last);
        return;
    }
    const auto moved = static_cast<core::i64>(*m_chosen) + by;
    choose(static_cast<usize>(std::clamp<core::i64>(moved, 0, static_cast<core::i64>(last))));
}

bool ProfileHistory::chooseWorst() noexcept
{
    if (m_frames.empty())
        return false;
    usize worst = 0;
    for (usize index = 1; index < m_frames.size(); ++index) {
        if (m_frames[index].ms > m_frames[worst].ms)
            worst = index;
    }
    choose(worst);
    return true;
}

bool ProfileHistory::chooseNextWorst() noexcept
{
    if (m_frames.empty())
        return false;
    if (!m_chosen.has_value())
        return chooseWorst();
    // The longest of those shorter than the one chosen -- or as long and
    // later on the strip, so equal frames are each visited once.
    const usize from = *m_chosen;
    const f32 limit = m_frames[from].ms;
    std::optional<usize> next;
    for (usize index = 0; index < m_frames.size(); ++index) {
        const f32 ms = m_frames[index].ms;
        const bool after = ms < limit || (ms == limit && index > from);
        if (!after)
            continue;
        if (!next.has_value() || ms > m_frames[*next].ms)
            next = index;
    }
    if (!next.has_value())
        return chooseWorst();
    choose(*next);
    return true;
}

usize ProfileHistory::overBudget(f32 budgetMs) const noexcept
{
    return static_cast<usize>(std::count_if(m_frames.begin(), m_frames.end(),
                                            [budgetMs](const Frame& frame) { return frame.ms > budgetMs; }));
}

f32 ProfileHistory::shownMs() const
{
    if (m_frames.empty())
        return 0.0f;
    if (m_chosen.has_value())
        return m_frames[*m_chosen].ms;
    std::vector<f32> lengths;
    lengths.reserve(m_frames.size());
    for (const Frame& frame : m_frames)
        lengths.push_back(frame.ms);
    return medianOf(lengths);
}

std::vector<ProfileHistory::Row> ProfileHistory::systems(ProfileOrder order) const
{
    // What each scope spent: the chosen frame's, or its median over the
    // frames kept -- a frame it did not run in counting as nought.
    std::vector<f32> ms(m_scopes.size(), 0.0f);
    std::vector<f32> count(m_scopes.size(), 0.0f);
    std::vector<bool> ran(m_scopes.size(), false);
    if (m_chosen.has_value()) {
        for (const Frame::Spent& spent : m_frames[*m_chosen].scopes) {
            ms[spent.index] = spent.ms;
            count[spent.index] = static_cast<f32>(spent.count);
            ran[spent.index] = true;
        }
    }
    else if (!m_frames.empty()) {
        std::vector<std::vector<f32>> times(m_scopes.size());
        std::vector<std::vector<f32>> calls(m_scopes.size());
        for (const Frame& frame : m_frames) {
            for (const Frame::Spent& spent : frame.scopes) {
                times[spent.index].push_back(spent.ms);
                calls[spent.index].push_back(static_cast<f32>(spent.count));
            }
        }
        for (usize index = 0; index < m_scopes.size(); ++index) {
            if (times[index].empty())
                continue;
            ran[index] = true;
            times[index].resize(m_frames.size(), 0.0f);
            calls[index].resize(m_frames.size(), 0.0f);
            ms[index] = medianOf(times[index]);
            count[index] = medianOf(calls[index]);
        }
    }

    std::vector<f32> under(m_scopes.size(), 0.0f);
    std::vector<std::vector<u32>> children(m_scopes.size());
    std::vector<u32> roots;
    for (usize index = 0; index < m_scopes.size(); ++index) {
        if (!ran[index])
            continue;
        if (m_scopes[index].parent < 0) {
            roots.push_back(static_cast<u32>(index));
            continue;
        }
        const auto parent = static_cast<usize>(m_scopes[index].parent);
        children[parent].push_back(static_cast<u32>(index));
        under[parent] += ms[index];
    }

    // Each level heaviest first -- scopes as long as each other staying in
    // the order they were entered -- unless that order is what was asked.
    const auto heavier = [&ms](u32 a, u32 b) { return ms[a] > ms[b]; };
    if (order == ProfileOrder::Heaviest) {
        std::stable_sort(roots.begin(), roots.end(), heavier);
        for (std::vector<u32>& level : children)
            std::stable_sort(level.begin(), level.end(), heavier);
    }
    // The heaviest path: the longest root, and the longest under each.
    std::vector<bool> onPath(m_scopes.size(), false);
    {
        const std::vector<u32>* level = &roots;
        while (!level->empty()) {
            const u32 longest = *std::min_element(level->begin(), level->end(), heavier);
            onPath[longest] = true;
            level = &children[longest];
        }
    }

    std::vector<Row> rows;
    std::vector<u32> stack(roots.rbegin(), roots.rend());
    while (!stack.empty()) {
        const u32 index = stack.back();
        stack.pop_back();
        const Scope& scope = m_scopes[index];
        Row row{scope.name, scope.depth, ms[index], std::max(0.0f, ms[index] - under[index]), count[index]};
        row.scope = index;
        row.leaf = children[index].empty();
        row.heaviest = onPath[index];
        rows.push_back(row);
        for (auto child = children[index].rbegin(); child != children[index].rend(); ++child)
            stack.push_back(*child);
    }
    return rows;
}

std::vector<ProfileHistory::Row> ProfileHistory::spentRows(const std::vector<std::string>& table,
                                                           std::vector<Frame::Spent> Frame::*member, bool sorted) const
{
    std::vector<Row> rows;
    if (m_frames.empty())
        return rows;
    if (m_chosen.has_value()) {
        for (const Frame::Spent& spent : m_frames[*m_chosen].*member)
            rows.push_back({table[spent.index], 0u, spent.ms, spent.ms, static_cast<f32>(spent.count)});
    }
    else {
        std::vector<std::vector<f32>> times(table.size());
        std::vector<std::vector<f32>> counts(table.size());
        for (const Frame& frame : m_frames) {
            for (const Frame::Spent& spent : frame.*member) {
                times[spent.index].push_back(spent.ms);
                counts[spent.index].push_back(static_cast<f32>(spent.count));
            }
        }
        for (usize index = 0; index < table.size(); ++index) {
            if (times[index].empty())
                continue;
            times[index].resize(m_frames.size(), 0.0f);
            counts[index].resize(m_frames.size(), 0.0f);
            const f32 ms = medianOf(times[index]);
            rows.push_back({table[index], 0u, ms, ms, medianOf(counts[index])});
        }
    }
    if (sorted) {
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ms > b.ms; });
    }
    return rows;
}

std::vector<ProfileHistory::Row> ProfileHistory::scriptRows() const
{
    return spentRows(m_scripts, &Frame::scripts, true);
}

std::vector<ProfileHistory::Row> ProfileHistory::passRows() const
{
    return spentRows(m_passes, &Frame::passes, false);
}

std::vector<ProfileHistory::Line> ProfileHistory::lines() const
{
    std::vector<Line> rows = m_lines;
    std::stable_sort(rows.begin(), rows.end(), [](const Line& a, const Line& b) { return a.samples > b.samples; });
    return rows;
}

// --- a file ----------------------------------------------------------------------

std::string ProfileHistory::toJson() const
{
    core::JsonWriter out;
    out.beginObject();
    out.field("format", kFormat);
    out.field("version", kVersion);
    out.field("capacity", static_cast<u64>(m_capacity));

    out.key("scopes");
    out.beginArray();
    for (const Scope& scope : m_scopes) {
        out.beginObject();
        out.field("name", std::string_view(scope.name));
        out.field("parent", static_cast<core::i64>(scope.parent));
        out.endObject();
    }
    out.endArray();
    const auto names = [&out](std::string_view key, const std::vector<std::string>& table) {
        out.key(key);
        out.beginArray();
        for (const std::string& name : table)
            out.value(std::string_view(name));
        out.endArray();
    };
    names("scripts", m_scripts);
    names("passes", m_passes);
    names("memoryScripts", m_memoryScripts);

    out.key("lines");
    out.beginArray();
    for (const Line& line : m_lines) {
        out.beginObject();
        out.field("source", std::string_view(line.source));
        out.field("line", static_cast<core::i64>(line.line));
        out.field("samples", static_cast<u64>(line.samples));
        out.endObject();
    }
    out.endArray();

    // A frame's lists are flat runs of numbers: index, milliseconds, count.
    const auto spent = [&out](std::string_view key, const std::vector<Frame::Spent>& rows) {
        out.key(key);
        out.beginInlineArray();
        for (const Frame::Spent& row : rows) {
            out.value(static_cast<u64>(row.index));
            out.valueFloat(row.ms);
            out.value(static_cast<u64>(row.count));
        }
        out.endArray();
    };
    out.key("frames");
    out.beginArray();
    for (const Frame& frame : m_frames) {
        out.beginObject();
        out.field("number", frame.number);
        out.fieldFloat("ms", frame.ms);
        out.field("entries", frame.entries);
        spent("scopes", frame.scopes);
        spent("scripts", frame.scripts);
        spent("passes", frame.passes);
        out.fieldFloat("gpuTimingMs", frame.gpuTimingMs);
        out.key("memory");
        out.beginInlineArray();
        out.value(frame.memory.scriptBytes);
        out.value(frame.memory.textureBytes);
        out.value(static_cast<u64>(frame.memory.textures));
        out.value(frame.memory.bufferBytes);
        out.value(static_cast<u64>(frame.memory.buffers));
        out.endArray();
        out.key("scriptMemory");
        out.beginInlineArray();
        for (const Frame::Bytes& held : frame.scriptMemory) {
            out.value(static_cast<u64>(held.index));
            out.value(held.bytes);
        }
        out.endArray();
        out.key("network");
        out.beginInlineArray();
        out.value(frame.network.connected);
        out.valueFloat(frame.network.pingMs);
        out.valueFloat(frame.network.jitterMs);
        out.valueFloat(frame.network.lossPercent);
        out.valueFloat(frame.network.snapshotsPerSecond);
        out.valueFloat(frame.network.resimulationMs);
        out.value(static_cast<u64>(frame.network.peers));
        out.valueFloat(frame.network.lastCorrectionMetres);
        out.value(static_cast<u64>(frame.corrections));
        for (usize seat = 0; seat < 4; ++seat) {
            out.value(frame.network.playerPredicted[seat]);
            out.value(static_cast<u64>(frame.playerCorrections[seat]));
        }
        out.endArray();
        out.endObject();
    }
    out.endArray();
    out.endObject();
    return out.text();
}

ProfileFileError ProfileHistory::fromJson(std::string_view text)
{
    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(text); !parsed.ok)
        return ProfileFileError::NotJson;
    const core::JsonValue root = document.root();
    if (root["format"].asString() != kFormat)
        return ProfileFileError::NotACapture;
    if (root["version"].asInteger() != kVersion)
        return ProfileFileError::OtherVersion;

    ProfileHistory read(static_cast<usize>(std::max<core::i64>(root["capacity"].asInteger(600), 1)));
    const core::JsonValue scopes = root["scopes"];
    for (usize index = 0; index < scopes.size(); ++index) {
        const core::JsonValue scope = scopes.at(index);
        const auto parent = static_cast<i32>(scope["parent"].asInteger(-1));
        // A scope is under one read before it, or under none.
        if (parent >= static_cast<i32>(index))
            return ProfileFileError::Broken;
        Scope made;
        made.name = std::string(scope["name"].asString());
        made.parent = parent < 0 ? -1 : parent;
        made.depth = made.parent < 0 ? 0u : read.m_scopes[static_cast<usize>(made.parent)].depth + 1u;
        read.m_scopes.push_back(std::move(made));
    }
    const auto names = [&root](std::string_view key, std::vector<std::string>& table) {
        const core::JsonValue list = root[key];
        for (usize index = 0; index < list.size(); ++index)
            table.emplace_back(list.at(index).asString());
    };
    names("scripts", read.m_scripts);
    names("passes", read.m_passes);
    names("memoryScripts", read.m_memoryScripts);
    const core::JsonValue lines = root["lines"];
    for (usize index = 0; index < lines.size(); ++index) {
        const core::JsonValue line = lines.at(index);
        const auto samples = static_cast<u32>(std::max<core::i64>(line["samples"].asInteger(), 0));
        read.m_lines.push_back(
            {std::string(line["source"].asString()), static_cast<int>(line["line"].asInteger()), samples});
        read.m_lineSamples += samples;
    }

    bool sound = true;
    const auto spent = [&sound](const core::JsonValue& list, usize tableSize, std::vector<Frame::Spent>& rows) {
        for (usize index = 0; index + 2 < list.size(); index += 3) {
            const core::i64 at = list.at(index).asInteger(-1);
            if (at < 0 || static_cast<usize>(at) >= tableSize) {
                sound = false;
                return;
            }
            rows.push_back({static_cast<u32>(at), static_cast<f32>(list.at(index + 1).asNumber()),
                            static_cast<u32>(std::max<core::i64>(list.at(index + 2).asInteger(), 0))});
        }
    };
    const core::JsonValue frames = root["frames"];
    for (usize index = 0; index < frames.size() && sound; ++index) {
        const core::JsonValue from = frames.at(index);
        Frame frame;
        frame.number = static_cast<u64>(std::max<core::i64>(from["number"].asInteger(), 0));
        frame.ms = static_cast<f32>(from["ms"].asNumber());
        frame.entries = static_cast<u64>(std::max<core::i64>(from["entries"].asInteger(), 0));
        spent(from["scopes"], read.m_scopes.size(), frame.scopes);
        spent(from["scripts"], read.m_scripts.size(), frame.scripts);
        spent(from["passes"], read.m_passes.size(), frame.passes);
        frame.gpuTimingMs = static_cast<f32>(from["gpuTimingMs"].asNumber());
        const auto whole = [](const core::JsonValue& value) {
            return static_cast<u64>(std::max<core::i64>(value.asInteger(), 0));
        };
        const core::JsonValue memory = from["memory"];
        frame.memory.scriptBytes = whole(memory.at(0));
        frame.memory.textureBytes = whole(memory.at(1));
        frame.memory.textures = static_cast<u32>(whole(memory.at(2)));
        frame.memory.bufferBytes = whole(memory.at(3));
        frame.memory.buffers = static_cast<u32>(whole(memory.at(4)));
        const core::JsonValue held = from["scriptMemory"];
        for (usize at = 0; at + 1 < held.size(); at += 2) {
            const core::i64 script = held.at(at).asInteger(-1);
            if (script < 0 || static_cast<usize>(script) >= read.m_memoryScripts.size()) {
                sound = false;
                break;
            }
            frame.scriptMemory.push_back({static_cast<u32>(script), whole(held.at(at + 1))});
        }
        const core::JsonValue network = from["network"];
        frame.network.connected = network.at(0).asBool();
        frame.network.pingMs = static_cast<f32>(network.at(1).asNumber());
        frame.network.jitterMs = static_cast<f32>(network.at(2).asNumber());
        frame.network.lossPercent = static_cast<f32>(network.at(3).asNumber());
        frame.network.snapshotsPerSecond = static_cast<f32>(network.at(4).asNumber());
        frame.network.resimulationMs = static_cast<f32>(network.at(5).asNumber());
        frame.network.peers = static_cast<u32>(whole(network.at(6)));
        frame.network.lastCorrectionMetres = static_cast<f32>(network.at(7).asNumber());
        frame.corrections = static_cast<u32>(whole(network.at(8)));
        for (usize seat = 0; seat < 4; ++seat) {
            frame.network.playerPredicted[seat] = network.at(9 + seat * 2).asBool();
            frame.playerCorrections[seat] = static_cast<u32>(whole(network.at(10 + seat * 2)));
        }
        // The bands are the scopes', by the rule a frame pushed has them by.
        read.bandFrame(frame);
        read.m_frames.push_back(std::move(frame));
    }
    // A frame that names a scope, a script or a pass the file does not list.
    if (!sound)
        return ProfileFileError::Broken;

    read.m_pushed = read.m_frames.empty() ? 0 : read.m_frames.back().number + 1;
    // Nobody is pushing: it is held as it was written.
    read.m_held = true;
    *this = std::move(read);
    return ProfileFileError::None;
}

bool ProfileHistory::save(const std::filesystem::path& file) const
{
    return platform::writeTextFile(file, toJson());
}

ProfileFileError ProfileHistory::load(const std::filesystem::path& file)
{
    std::string text;
    if (!platform::readTextFile(file, text))
        return ProfileFileError::Unreadable;
    return fromJson(text);
}

} // namespace engine::app
