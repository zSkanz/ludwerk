#include "engine/script/save_store.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <span>
#include <system_error>
#include <variant>

#include "engine/core/json.h"
#include "engine/core/json_writer.h"
#include "engine/core/sequence.h"
#include "engine/platform/file.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace engine::script {
namespace {

using core::JsonType;
using core::JsonValue;
using core::JsonWriter;

// "ESAV": the file is a save. The format's own version follows it, apart from
// the game's, which is `SaveService.Version`.
constexpr std::string_view Magic = "ESAV";
constexpr int FormatVersion = 1;
constexpr std::string_view SaveSuffix = ".save";
constexpr std::string_view BackupSuffix = ".bak";

void tagged(JsonWriter& out, std::string_view type, std::span<const core::f64> numbers)
{
    out.beginObject();
    out.field("$", type);
    out.key("v");
    out.beginInlineArray();
    for (const core::f64 number : numbers)
        out.value(number);
    out.endArray();
    out.endObject();
}

void writeSaveValue(JsonWriter& out, const SaveValue& value)
{
    if (value.table != nullptr) {
        out.beginObject();
        out.field("$", "table");
        out.key("a");
        out.beginArray();
        for (const SaveValue& item : value.table->array)
            writeSaveValue(out, item);
        out.endArray();
        out.key("m");
        out.beginObject();
        for (const auto& [key, item] : value.table->fields) {
            out.key(key);
            writeSaveValue(out, item);
        }
        out.endObject();
        out.endObject();
        return;
    }
    // Widened by hand: `-Wdouble-promotion` is right that an f32 becoming an
    // f64 is a change, and here it is the one wanted.
    const auto d = [](core::f32 number) { return static_cast<core::f64>(number); };
    std::visit(
        [&](const auto& held) {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, bool>) {
                out.value(held);
            }
            else if constexpr (std::is_same_v<T, core::f64>) {
                out.value(held);
            }
            else if constexpr (std::is_same_v<T, std::string>) {
                out.value(std::string_view(held));
            }
            else if constexpr (std::is_same_v<T, core::Vec3>) {
                const core::f64 v[] = {d(held.x), d(held.y), d(held.z)};
                tagged(out, "Vector3", v);
            }
            else if constexpr (std::is_same_v<T, core::CFrameD>) {
                const core::Mat3& r = held.rotation;
                const core::f64 v[] = {held.position.x, held.position.y, held.position.z, d(r.m[0][0]),
                                       d(r.m[0][1]),    d(r.m[0][2]),    d(r.m[1][0]),    d(r.m[1][1]),
                                       d(r.m[1][2]),    d(r.m[2][0]),    d(r.m[2][1]),    d(r.m[2][2])};
                tagged(out, "CFrame", v);
            }
            else if constexpr (std::is_same_v<T, core::Color3>) {
                const core::f64 v[] = {d(held.r), d(held.g), d(held.b)};
                tagged(out, "Color3", v);
            }
            else if constexpr (std::is_same_v<T, core::Vec2>) {
                const core::f64 v[] = {d(held.x), d(held.y)};
                tagged(out, "Vector2", v);
            }
            else if constexpr (std::is_same_v<T, core::UDim>) {
                const core::f64 v[] = {d(held.scale), d(held.offset)};
                tagged(out, "UDim", v);
            }
            else if constexpr (std::is_same_v<T, core::UDim2>) {
                const core::f64 v[] = {d(held.x.scale), d(held.x.offset), d(held.y.scale), d(held.y.offset)};
                tagged(out, "UDim2", v);
            }
            else if constexpr (std::is_same_v<T, core::Rect>) {
                const core::f64 v[] = {d(held.min.x), d(held.min.y), d(held.max.x), d(held.max.y)};
                tagged(out, "Rect", v);
            }
            else if constexpr (std::is_same_v<T, core::ColorSequence>) {
                std::vector<core::f64> v;
                for (const core::ColorKeypoint& stop : held.keypoints)
                    v.insert(v.end(), {d(stop.time), d(stop.value.r), d(stop.value.g), d(stop.value.b)});
                tagged(out, "ColorSequence", v);
            }
            else if constexpr (std::is_same_v<T, core::NumberSequence>) {
                std::vector<core::f64> v;
                for (const core::NumberKeypoint& stop : held.keypoints)
                    v.insert(v.end(), {d(stop.time), d(stop.value), d(stop.envelope)});
                tagged(out, "NumberSequence", v);
            }
            else {
                // Nil, and what `Set` never lets in: an Instance, an enum item,
                // a material.
                out.nullValue();
            }
        },
        value.scalar);
}

[[nodiscard]] std::optional<SaveValue> readSaveValue(const JsonValue& json, int depth)
{
    if (depth > 64)
        return std::nullopt;
    switch (json.type()) {
    case JsonType::Null:
        return SaveValue{};
    case JsonType::Boolean:
        return SaveValue{scene::Value{json.asBool()}, nullptr};
    case JsonType::Number:
        return SaveValue{scene::Value{json.asNumber()}, nullptr};
    case JsonType::String:
        return SaveValue{scene::Value{std::string(json.asString())}, nullptr};
    case JsonType::Object:
        break;
    default:
        return std::nullopt;
    }

    const std::string_view type = json["$"].asString();
    if (type == "table") {
        auto table = std::make_shared<SaveTable>();
        const JsonValue array = json["a"];
        for (core::usize index = 0; index < array.size(); ++index) {
            std::optional<SaveValue> item = readSaveValue(array.at(index), depth + 1);
            if (!item.has_value())
                return std::nullopt;
            table->array.push_back(std::move(*item));
        }
        const JsonValue fields = json["m"];
        for (core::usize index = 0; index < fields.size(); ++index) {
            const std::string_view key = fields.keyAt(index);
            std::optional<SaveValue> item = readSaveValue(fields[key], depth + 1);
            if (!item.has_value())
                return std::nullopt;
            table->fields.emplace(std::string(key), std::move(*item));
        }
        return SaveValue{scene::Value{}, std::move(table)};
    }

    const JsonValue numbers = json["v"];
    std::vector<core::f64> v;
    for (core::usize index = 0; index < numbers.size(); ++index)
        v.push_back(numbers.at(index).asNumber());
    const auto f = [&](core::usize index) { return static_cast<core::f32>(v[index]); };
    const auto sized = [&](core::usize count) { return v.size() == count; };

    if (type == "Vector3" && sized(3))
        return SaveValue{scene::Value{core::Vec3{f(0), f(1), f(2)}}, nullptr};
    if (type == "Color3" && sized(3))
        return SaveValue{scene::Value{core::Color3{f(0), f(1), f(2)}}, nullptr};
    if (type == "Vector2" && sized(2))
        return SaveValue{scene::Value{core::Vec2{f(0), f(1)}}, nullptr};
    if (type == "UDim" && sized(2))
        return SaveValue{scene::Value{core::UDim{f(0), f(1)}}, nullptr};
    if (type == "UDim2" && sized(4))
        return SaveValue{scene::Value{core::UDim2{core::UDim{f(0), f(1)}, core::UDim{f(2), f(3)}}}, nullptr};
    if (type == "Rect" && sized(4))
        return SaveValue{scene::Value{core::Rect{core::Vec2{f(0), f(1)}, core::Vec2{f(2), f(3)}}}, nullptr};
    if (type == "CFrame" && sized(12)) {
        core::CFrameD frame;
        frame.position = core::DVec3{v[0], v[1], v[2]};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column)
                frame.rotation.m[row][column] = f(static_cast<core::usize>(3 + row * 3 + column));
        }
        return SaveValue{scene::Value{frame}, nullptr};
    }
    if (type == "ColorSequence" && !v.empty() && v.size() % 4 == 0) {
        core::ColorSequence sequence;
        sequence.keypoints.clear();
        for (core::usize at = 0; at < v.size(); at += 4)
            sequence.keypoints.push_back(core::ColorKeypoint{f(at), core::Color3{f(at + 1), f(at + 2), f(at + 3)}});
        return SaveValue{scene::Value{std::move(sequence)}, nullptr};
    }
    if (type == "NumberSequence" && !v.empty() && v.size() % 3 == 0) {
        core::NumberSequence sequence;
        sequence.keypoints.clear();
        for (core::usize at = 0; at < v.size(); at += 3)
            sequence.keypoints.push_back(core::NumberKeypoint{f(at), f(at + 1), f(at + 2)});
        return SaveValue{scene::Value{std::move(sequence)}, nullptr};
    }
    return std::nullopt;
}

[[nodiscard]] std::string payloadOf(const SaveSlotData& slot)
{
    JsonWriter out;
    out.beginObject();
    for (const auto& [key, value] : slot.values) {
        out.key(key);
        writeSaveValue(out, value);
    }
    out.endObject();
    return out.text();
}

[[nodiscard]] std::filesystem::path fileOf(const std::filesystem::path& directory, std::string_view name,
                                           std::string_view suffix)
{
    return directory / std::filesystem::path(std::string(name) + std::string(suffix));
}

} // namespace

SaveStore::SaveStore(Options options) : m_options(std::move(options))
{
    m_worker = std::thread([this] { run(); });
}

SaveStore::~SaveStore()
{
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

bool SaveStore::validName(std::string_view name) noexcept
{
    if (name.empty() || name.size() > 64)
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

std::string SaveStore::encode(const SaveSlotData& slot, core::f64 version)
{
    const std::string payload = payloadOf(slot);
    char header[160];
    (void)std::snprintf(header, sizeof(header), "%.*s %d %.17g %llu %016llx\n", static_cast<int>(Magic.size()),
                        Magic.data(), FormatVersion, version, static_cast<unsigned long long>(payload.size()),
                        static_cast<unsigned long long>(XXH3_64bits(payload.data(), payload.size())));
    return std::string(header) + payload;
}

bool SaveStore::decode(std::string_view text, SaveSlotData& slot)
{
    const std::size_t end = text.find('\n');
    if (end == std::string_view::npos)
        return false;
    const std::string header(text.substr(0, end));
    const std::string_view payload = text.substr(end + 1);
    // Five fields, space-separated, read by hand: nothing here needs a locale.
    std::vector<std::string> fields;
    for (std::size_t at = 0; at < header.size();) {
        const std::size_t space = header.find(' ', at);
        fields.push_back(header.substr(at, space == std::string::npos ? std::string::npos : space - at));
        if (space == std::string::npos)
            break;
        at = space + 1;
    }
    if (fields.size() != 5 || fields[0] != Magic)
        return false;
    char* stop = nullptr;
    const long format = std::strtol(fields[1].c_str(), &stop, 10);
    const double version = std::strtod(fields[2].c_str(), &stop);
    const unsigned long long length = std::strtoull(fields[3].c_str(), &stop, 10);
    const unsigned long long hash = std::strtoull(fields[4].c_str(), &stop, 16);
    if (format != FormatVersion || length != payload.size() || hash != XXH3_64bits(payload.data(), payload.size()))
        return false;

    core::JsonDocument document;
    if (!document.parse(payload).ok || document.root().type() != JsonType::Object)
        return false;
    std::map<std::string, SaveValue> values;
    const JsonValue root = document.root();
    for (core::usize index = 0; index < root.size(); ++index) {
        const std::string_view key = root.keyAt(index);
        std::optional<SaveValue> value = readSaveValue(root[key], 0);
        if (!value.has_value())
            return false;
        values.emplace(std::string(key), std::move(*value));
    }
    slot.values = std::move(values);
    slot.version = version >= 1.0 ? version : 1.0;
    return true;
}

core::u64 SaveStore::encodedSize(const SaveSlotData& slot)
{
    return payloadOf(slot).size();
}

SaveSlotData* SaveStore::open(std::string_view name, SaveDamage* damage)
{
    if (!validName(name))
        return nullptr;
    if (const auto found = m_slots.find(name); found != m_slots.end())
        return found->second.get();
    if (m_slots.size() >= m_options.maxSlots)
        return nullptr;

    auto slot = std::make_unique<SaveSlotData>();
    slot->name = std::string(name);
    if (!m_options.directory.empty()) {
        std::string text;
        const std::filesystem::path main = fileOf(m_options.directory, name, SaveSuffix);
        const std::filesystem::path backup = fileOf(m_options.directory, name, BackupSuffix);
        const bool hadMain = platform::readTextFile(main, text);
        if (!hadMain || !decode(text, *slot)) {
            std::string saved;
            const bool hadBackup = platform::readTextFile(backup, saved);
            if (hadBackup && decode(saved, *slot)) {
                if (hadMain && damage != nullptr)
                    *damage = SaveDamage::FromBackup;
                // Written again, so the damaged file goes at the next write.
                ++slot->generation;
            }
            else if (hadMain || hadBackup) {
                slot->values.clear();
                slot->recovered = false;
                if (damage != nullptr)
                    *damage = SaveDamage::Lost;
            }
        }
    }
    SaveSlotData* out = slot.get();
    m_slots.emplace(std::string(name), std::move(slot));
    return out;
}

std::vector<std::string> SaveStore::list() const
{
    std::set<std::string> names;
    for (const auto& [name, slot] : m_slots) {
        if (!slot->values.empty())
            names.insert(name);
    }
    std::error_code error;
    if (!m_options.directory.empty()) {
        for (const auto& entry : std::filesystem::directory_iterator(m_options.directory, error)) {
            const std::string file = entry.path().filename().string();
            for (const std::string_view suffix : {SaveSuffix, BackupSuffix}) {
                if (file.size() > suffix.size() && file.ends_with(suffix)) {
                    const std::string name = file.substr(0, file.size() - suffix.size());
                    if (!validName(name))
                        continue;
                    // A slot emptied in memory is not listed for a file it
                    // has not yet removed.
                    const auto held = m_slots.find(name);
                    if (held == m_slots.end() || !held->second->values.empty())
                        names.insert(name);
                }
            }
        }
    }
    return {names.begin(), names.end()};
}

bool SaveStore::remove(std::string_view name)
{
    if (!validName(name))
        return false;
    {
        // A write of it still queued would bring the file back.
        std::unique_lock<std::mutex> lock(m_mutex);
        for (auto job = m_jobs.begin(); job != m_jobs.end();) {
            if (job->name == name) {
                if (job->tracked)
                    m_done[job->ticket] = true;
                job = m_jobs.erase(job);
            }
            else {
                ++job;
            }
        }
        m_idle.wait(lock, [this] { return !m_busy; });
    }
    if (const auto found = m_slots.find(name); found != m_slots.end()) {
        found->second->values.clear();
        found->second->version = 1.0;
        found->second->recovered = true;
        ++found->second->generation;
        found->second->written = found->second->generation;
    }
    if (!m_options.directory.empty()) {
        platform::removeFile(fileOf(m_options.directory, name, SaveSuffix));
        platform::removeFile(fileOf(m_options.directory, name, BackupSuffix));
    }
    return true;
}

core::u64 SaveStore::write(SaveSlotData& slot, core::f64 version, bool tracked)
{
    Job job;
    job.tracked = tracked;
    job.name = slot.name;
    job.text = encode(slot, version);
    slot.written = slot.generation;
    slot.version = version;
    const std::lock_guard<std::mutex> lock(m_mutex);
    job.ticket = m_nextTicket++;
    const core::u64 ticket = job.ticket;
    if (m_options.directory.empty()) {
        // Nowhere to write: kept in memory for this run, which is what a build
        // with no folder can do.
        if (tracked)
            m_done[ticket] = true;
        return ticket;
    }
    m_jobs.push_back(std::move(job));
    m_wake.notify_one();
    return ticket;
}

bool SaveStore::finished(core::u64 ticket, bool& succeeded)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_done.find(ticket);
    if (found == m_done.end())
        return false;
    succeeded = found->second;
    m_done.erase(found);
    return true;
}

void SaveStore::pump(core::f64 dt, core::f64 version)
{
    m_sinceWrite += dt;
    if (m_sinceWrite < 1.0)
        return;
    m_sinceWrite = 0.0;
    for (auto& [name, slot] : m_slots) {
        if (slot->dirty())
            (void)write(*slot, version, false);
    }
}

void SaveStore::flush(core::f64 version)
{
    for (auto& [name, slot] : m_slots) {
        if (slot->dirty())
            (void)write(*slot, version, false);
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idle.wait(lock, [this] { return m_jobs.empty() && !m_busy; });
}

bool SaveStore::writeNow(const std::string& name, const std::string& text)
{
    if (!platform::createDirectories(m_options.directory))
        return false;
    const std::filesystem::path main = fileOf(m_options.directory, name, SaveSuffix);
    // The previous save becomes the backup FIRST: a write that dies after this
    // leaves the backup, which `open` reads.
    std::error_code error;
    if (std::filesystem::exists(main, error))
        (void)platform::renameFile(main, fileOf(m_options.directory, name, BackupSuffix));
    return platform::writeFileDurable(
        main, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

void SaveStore::run()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_wake.wait(lock, [this] { return m_stopping || !m_jobs.empty(); });
        if (m_jobs.empty()) {
            if (m_stopping)
                return;
            continue;
        }
        Job job = std::move(m_jobs.front());
        m_jobs.pop_front();
        m_busy = true;
        lock.unlock();
        const bool ok = writeNow(job.name, job.text);
        lock.lock();
        if (job.tracked)
            m_done[job.ticket] = ok;
        m_busy = false;
        m_idle.notify_all();
    }
}

} // namespace engine::script
