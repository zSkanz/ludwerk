#include "engine/asset/lip_sync.h"

#include <algorithm>
#include <cmath>

#include "engine/core/json.h"

namespace engine::asset {

namespace {

// `name` with `tail` where its extension was: what follows the last dot of
// its last segment.
[[nodiscard]] std::string besideAs(std::string_view name, std::string_view tail)
{
    const core::usize slash = name.find_last_of('/');
    const core::usize dot = name.find_last_of('.');
    const bool extended = dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash);
    std::string out(extended ? name.substr(0, dot) : name);
    out.append(tail);
    return out;
}

} // namespace

const VisemeCue* VisemeTrack::at(core::f32 seconds) const noexcept
{
    // The last cue that has started. A track is a few hundred cues at most,
    // asked once a frame a speaker.
    const auto after = std::upper_bound(cues.begin(), cues.end(), seconds,
                                        [](core::f32 moment, const VisemeCue& cue) { return moment < cue.start; });
    if (after == cues.begin())
        return nullptr;
    const VisemeCue& cue = *(after - 1);
    return seconds < cue.end ? &cue : nullptr;
}

bool readVisemeTrack(std::string_view json, VisemeTrack& out, std::string* what)
{
    out = VisemeTrack{};
    const auto refuse = [&](std::string_view why) {
        if (what != nullptr)
            *what = std::string(why);
        out = VisemeTrack{};
        return false;
    };
    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(json, "visemes"); !parsed)
        return refuse(parsed.diagnostic);
    const core::JsonValue root = document.root();
    if (root.type() != core::JsonType::Object)
        return refuse("not an object");
    // Ours, or the list as the common free tool names it.
    const core::JsonValue cues = root.has("cues") ? root["cues"] : root["mouthCues"];
    if (cues.type() != core::JsonType::Array)
        return refuse("cues");
    out.cues.reserve(cues.size());
    for (core::usize index = 0; index < cues.size(); ++index) {
        const core::JsonValue entry = cues.at(index);
        const core::f64 start = entry["start"].asNumber(-1.0);
        const core::f64 end = entry["end"].asNumber(-1.0);
        const std::string_view value = entry["value"].asString();
        if (entry.type() != core::JsonType::Object || !(start >= 0.0) || !(end >= start) || !std::isfinite(end) ||
            value.empty())
            return refuse("cues");
        out.cues.push_back(VisemeCue{static_cast<core::f32>(start), static_cast<core::f32>(end), std::string(value)});
    }
    // In order of start, whatever order the file had them in; two that start
    // together keep the file's order.
    std::stable_sort(out.cues.begin(), out.cues.end(),
                     [](const VisemeCue& a, const VisemeCue& b) { return a.start < b.start; });
    return true;
}

std::string visemeTrackName(std::string_view sound)
{
    return besideAs(sound, ".visemes.json");
}

const std::vector<FaceKey>* FaceMap::viseme(std::string_view name) const noexcept
{
    const auto at = std::lower_bound(visemes.begin(), visemes.end(), name,
                                     [](const auto& entry, std::string_view key) { return entry.first < key; });
    return at != visemes.end() && at->first == name ? &at->second : nullptr;
}

bool readFaceMap(std::string_view json, FaceMap& out, std::string* what)
{
    out = FaceMap{};
    const auto refuse = [&](std::string_view why) {
        if (what != nullptr)
            *what = std::string(why);
        out = FaceMap{};
        return false;
    };
    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(json, "face"); !parsed)
        return refuse(parsed.diagnostic);
    const core::JsonValue root = document.root();
    if (root.type() != core::JsonType::Object)
        return refuse("not an object");

    const core::JsonValue visemes = root["visemes"];
    if (root.has("visemes") && visemes.type() != core::JsonType::Object)
        return refuse("visemes");
    for (core::usize index = 0; index < visemes.size(); ++index) {
        const std::string_view name = visemes.keyAt(index);
        const core::JsonValue keys = visemes.at(index);
        if (name.empty() || keys.type() != core::JsonType::Object)
            return refuse("visemes");
        std::vector<FaceKey> set;
        for (core::usize at = 0; at < keys.size(); ++at) {
            const core::f64 weight = keys.at(at).asNumber(-1.0);
            if (keys.keyAt(at).empty() || keys.at(at).type() != core::JsonType::Number || !(weight >= 0.0) ||
                weight > 4.0)
                return refuse("visemes");
            set.push_back(FaceKey{std::string(keys.keyAt(at)), static_cast<core::f32>(weight)});
        }
        out.visemes.emplace_back(std::string(name), std::move(set));
    }
    std::sort(out.visemes.begin(), out.visemes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    const core::JsonValue bands = root["bands"];
    if (root.has("bands") && bands.type() != core::JsonType::Object)
        return refuse("bands");
    out.open = std::string(bands["open"].asString());
    out.wide = std::string(bands["wide"].asString());
    out.round = std::string(bands["round"].asString());
    out.loudness = std::string(root["loudness"].asString());
    return true;
}

std::string faceMapName(std::string_view model)
{
    // `hero.glb#Head` is a piece of `hero.glb`: one face map a file.
    const core::usize piece = model.find('#');
    return besideAs(piece == std::string_view::npos ? model : model.substr(0, piece), ".face.json");
}

} // namespace engine::asset
