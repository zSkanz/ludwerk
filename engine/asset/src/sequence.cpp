#include "engine/asset/sequence.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "engine/core/json.h"

namespace engine::asset {

namespace {

using core::f32;
using core::f64;
using core::usize;

struct TypeName
{
    std::string_view name;
    SequenceTrackType type;
};
constexpr std::array<TypeName, 13> kTypes{{
    {"camera", SequenceTrackType::Camera},
    {"animation", SequenceTrackType::Animation},
    {"line", SequenceTrackType::Line},
    {"sound", SequenceTrackType::Sound},
    {"property", SequenceTrackType::Property},
    {"morph", SequenceTrackType::Morph},
    {"event", SequenceTrackType::Event},
    {"hold", SequenceTrackType::Hold},
    {"marker", SequenceTrackType::Marker},
    {"fade", SequenceTrackType::Fade},
    {"bars", SequenceTrackType::Bars},
    {"caption", SequenceTrackType::Caption},
    {"cast", SequenceTrackType::Cast},
}};

struct StyleName
{
    std::string_view name;
    core::EasingStyle style;
};
constexpr std::array<StyleName, 10> kStyles{{
    {"sine", core::EasingStyle::Sine},
    {"quad", core::EasingStyle::Quad},
    {"cubic", core::EasingStyle::Cubic},
    {"quart", core::EasingStyle::Quart},
    {"quint", core::EasingStyle::Quint},
    {"exponential", core::EasingStyle::Exponential},
    {"circular", core::EasingStyle::Circular},
    {"back", core::EasingStyle::Back},
    {"bounce", core::EasingStyle::Bounce},
    {"elastic", core::EasingStyle::Elastic},
}};

// Three finite numbers out of an array of three.
[[nodiscard]] bool readThree(core::JsonValue value, f64 (&out)[3]) noexcept
{
    if (value.type() != core::JsonType::Array || value.size() != 3)
        return false;
    for (usize index = 0; index < 3; ++index) {
        const core::JsonValue part = value.at(index);
        if (part.type() != core::JsonType::Number || !std::isfinite(part.asNumber()))
            return false;
        out[index] = part.asNumber();
    }
    return true;
}

[[nodiscard]] bool readValue(core::JsonValue value, SequenceValue& out)
{
    out = SequenceValue{};
    switch (value.type()) {
    case core::JsonType::Number:
        if (!std::isfinite(value.asNumber()))
            return false;
        out.kind = SequenceValue::Kind::Number;
        out.count = 1;
        out.numbers[0] = value.asNumber();
        return true;
    case core::JsonType::Boolean:
        out.kind = SequenceValue::Kind::Flag;
        out.flag = value.asBool();
        return true;
    case core::JsonType::String:
        out.kind = SequenceValue::Kind::Text;
        out.text = std::string(value.asString());
        return true;
    case core::JsonType::Array: {
        if (value.size() < 2 || value.size() > 6)
            return false;
        out.kind = SequenceValue::Kind::Numbers;
        out.count = static_cast<core::u8>(value.size());
        for (usize index = 0; index < value.size(); ++index) {
            const core::JsonValue part = value.at(index);
            if (part.type() != core::JsonType::Number || !std::isfinite(part.asNumber()))
                return false;
            out.numbers[index] = part.asNumber();
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace

bool parseSequenceEase(std::string_view word, core::EasingStyle& style, core::EasingDirection& direction) noexcept
{
    if (word == "linear") {
        style = core::EasingStyle::Linear;
        direction = core::EasingDirection::InOut;
        return true;
    }
    for (const StyleName& named : kStyles) {
        if (!word.starts_with(named.name))
            continue;
        const std::string_view rest = word.substr(named.name.size());
        if (rest == "In")
            direction = core::EasingDirection::In;
        else if (rest == "Out")
            direction = core::EasingDirection::Out;
        else if (rest == "InOut")
            direction = core::EasingDirection::InOut;
        else
            continue;
        style = named.style;
        return true;
    }
    return false;
}

void splitSequenceTarget(std::string_view target, std::string_view& binding, std::string_view& path) noexcept
{
    const usize slash = target.find('/');
    binding = target.substr(0, slash);
    path = slash == std::string_view::npos ? std::string_view{} : target.substr(slash + 1);
}

bool readSequence(std::string_view json, Sequence& out, std::string* what)
{
    out = Sequence{};
    const auto refuse = [&](std::string why) {
        if (what != nullptr)
            *what = std::move(why);
        out = Sequence{};
        return false;
    };
    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(json, "sequence"); !parsed)
        return refuse(std::string(parsed.diagnostic));
    const core::JsonValue root = document.root();
    if (root.type() != core::JsonType::Object || root["format"].asString() != "sequence")
        return refuse("format");
    if (root["version"].asInteger(0) != 1)
        return refuse("version");

    const core::JsonValue bindings = root["bindings"];
    if (!bindings.isNull() && bindings.type() != core::JsonType::Array)
        return refuse("bindings");
    for (usize index = 0; index < bindings.size(); ++index) {
        const std::string_view name = bindings.at(index).asString();
        if (name.empty() || name.find('/') != std::string_view::npos ||
            std::find(out.bindings.begin(), out.bindings.end(), name) != out.bindings.end())
            return refuse("bindings[" + std::to_string(index) + "]");
        out.bindings.emplace_back(name);
    }
    // **Who a cast track brings is a name too**: whatever it is bound under
    // may be played on by any track of the file, written before it or after.
    std::vector<std::string> brought;
    {
        const core::JsonValue all = root["tracks"];
        for (usize index = 0; all.type() == core::JsonType::Array && index < all.size(); ++index) {
            const core::JsonValue entry = all.at(index);
            if (entry.type() != core::JsonType::Object || entry["type"].asString() != "cast")
                continue;
            const std::string_view name = entry["as"].asString();
            if (name.empty() || name.find('/') != std::string_view::npos ||
                std::find(out.bindings.begin(), out.bindings.end(), name) != out.bindings.end() ||
                std::find(brought.begin(), brought.end(), name) != brought.end())
                return refuse("tracks[" + std::to_string(index) + "].as");
            brought.emplace_back(name);
        }
    }
    const auto bound = [&](std::string_view name) {
        return std::find(out.bindings.begin(), out.bindings.end(), name) != out.bindings.end() ||
               std::find(brought.begin(), brought.end(), name) != brought.end();
    };
    // The lines the file says, by name: what a key may be anchored after.
    std::vector<std::string> said;
    {
        const core::JsonValue all = root["tracks"];
        for (usize index = 0; all.type() == core::JsonType::Array && index < all.size(); ++index) {
            const core::JsonValue entry = all.at(index);
            if (entry.type() != core::JsonType::Object || entry["type"].asString() != "line")
                continue;
            const core::JsonValue keys = entry["keys"];
            for (usize key = 0; keys.type() == core::JsonType::Array && key < keys.size(); ++key)
                said.emplace_back(keys.at(key)["line"].asString());
        }
    }
    // A binding, or something under one: `Anna`, `Anna/Bust/Face`.
    const auto under = [&](std::string_view target) {
        std::string_view binding;
        std::string_view path;
        splitSequenceTarget(target, binding, path);
        return bound(binding);
    };

    const core::JsonValue tracks = root["tracks"];
    if (tracks.type() != core::JsonType::Array)
        return refuse("tracks");
    f32 last = 0.0f;
    for (usize trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        const core::JsonValue entry = tracks.at(trackIndex);
        const std::string where = "tracks[" + std::to_string(trackIndex) + "]";
        if (entry.type() != core::JsonType::Object)
            return refuse(where);
        SequenceTrack track;
        const std::string_view typeName = entry["type"].asString();
        const auto type =
            std::find_if(kTypes.begin(), kTypes.end(), [&](const TypeName& named) { return named.name == typeName; });
        if (type == kTypes.end())
            return refuse(where + ".type");
        track.type = type->type;
        track.target = std::string(entry["target"].asString());
        track.property = std::string(entry[track.type == SequenceTrackType::Morph ? "key" : "property"].asString());

        // What a track is set on has to be a name the file lists: a typo in a
        // binding is otherwise a track that silently moves nothing.
        const bool targeted = track.type == SequenceTrackType::Animation || track.type == SequenceTrackType::Property ||
                              track.type == SequenceTrackType::Morph;
        if (targeted) {
            std::string_view binding;
            std::string_view path;
            splitSequenceTarget(track.target, binding, path);
            if (!bound(binding))
                return refuse(where + ".target");
        }
        if ((track.type == SequenceTrackType::Property || track.type == SequenceTrackType::Morph) &&
            track.property.empty())
            return refuse(where + (track.type == SequenceTrackType::Morph ? ".key" : ".property"));

        if (track.type == SequenceTrackType::Camera && entry.has("path")) {
            const std::string_view path = entry["path"].asString();
            if (path != "curve" && path != "line")
                return refuse(where + ".path");
            track.curve = path == "curve";
        }
        if (track.type == SequenceTrackType::Cast) {
            track.target = std::string(entry["as"].asString());
            track.model = std::string(entry["model"].asString());
            if (track.model.empty())
                return refuse(where + ".model");
        }
        if (track.type == SequenceTrackType::Fade && entry.has("color")) {
            f64 colour[3]{};
            if (!readThree(entry["color"], colour))
                return refuse(where + ".color");
            for (usize channel = 0; channel < 3; ++channel)
                track.color[channel] = static_cast<f32>(colour[channel]);
        }
        if (track.type == SequenceTrackType::Bars && entry.has("aspect")) {
            const f64 aspect = entry["aspect"].asNumber(0.0);
            if (!(aspect >= 1.0 && aspect <= 10.0))
                return refuse(where + ".aspect");
            track.aspect = static_cast<f32>(aspect);
        }

        const core::JsonValue keys = entry["keys"];
        if (keys.type() != core::JsonType::Array)
            return refuse(where + ".keys");
        track.keys.reserve(keys.size());
        for (usize keyIndex = 0; keyIndex < keys.size(); ++keyIndex) {
            const core::JsonValue source = keys.at(keyIndex);
            const std::string at = where + ".keys[" + std::to_string(keyIndex) + "]";
            const f64 time = source["time"].asNumber(-1.0);
            if (source.type() != core::JsonType::Object || source["time"].type() != core::JsonType::Number ||
                !(time >= 0.0) || !std::isfinite(time))
                return refuse(at + ".time");
            SequenceKey key;
            key.time = static_cast<f32>(time);
            key.cut = source["cut"].asBool(false);
            if (source.has("ease") && !parseSequenceEase(source["ease"].asString(), key.style, key.direction))
                return refuse(at + ".ease");
            // **After a line, as heard**: a picture track's alone. What moves
            // the world, or is heard by every machine, keeps the simulation's
            // clock -- which must be every machine's.
            if (source.has("afterLine")) {
                const bool picture = track.type == SequenceTrackType::Camera || track.type == SequenceTrackType::Fade ||
                                     track.type == SequenceTrackType::Bars || track.type == SequenceTrackType::Morph;
                key.afterLine = std::string(source["afterLine"].asString());
                if (!picture || key.afterLine.empty() ||
                    std::find(said.begin(), said.end(), key.afterLine) == said.end())
                    return refuse(at + ".afterLine");
            }
            // **`"$Name"` where a value goes**: the game's to say, by the
            // player's attribute of that name. `"$$..."` is a string that
            // begins with `$`.
            const auto variable = [&](std::string_view field) {
                const std::string_view written = source[field].asString();
                if (source[field].type() != core::JsonType::String || written.size() < 2 || written[0] != '$' ||
                    written[1] == '$')
                    return false;
                key.variable = std::string(written.substr(1));
                return true;
            };

            switch (track.type) {
            case SequenceTrackType::Camera: {
                key.hasPosition = readThree(source["position"], key.position);
                if (!key.hasPosition)
                    return refuse(at + ".position");
                key.relativeTo = std::string(source["relativeTo"].asString());
                if (!key.relativeTo.empty() && !under(key.relativeTo))
                    return refuse(at + ".relativeTo");
                const core::JsonValue look = source["lookAt"];
                if (look.type() == core::JsonType::String) {
                    key.lookAtBinding = std::string(look.asString());
                    if (!under(key.lookAtBinding))
                        return refuse(at + ".lookAt");
                }
                else if (!look.isNull()) {
                    key.hasLookAt = readThree(look, key.lookAt);
                    if (!key.hasLookAt)
                        return refuse(at + ".lookAt");
                }
                if (source.has("rotation")) {
                    key.hasRotation = readThree(source["rotation"], key.rotation);
                    if (!key.hasRotation)
                        return refuse(at + ".rotation");
                }
                const f64 field = source["fieldOfView"].asNumber(0.0);
                if (source.has("fieldOfView") && !(field >= 1.0 && field <= 179.0))
                    return refuse(at + ".fieldOfView");
                key.fieldOfView = static_cast<f32>(field);
                if (source.has("shake")) {
                    const core::JsonValue shake = source["shake"];
                    if (shake.type() != core::JsonType::Array || shake.size() != 2 ||
                        !(shake.at(0).asNumber(-1.0) >= 0.0) || !(shake.at(1).asNumber(-1.0) >= 0.0))
                        return refuse(at + ".shake");
                    key.shake[0] = static_cast<f32>(shake.at(0).asNumber(0.0));
                    key.shake[1] = static_cast<f32>(shake.at(1).asNumber(0.0));
                }
                key.keepInSight = source["keepInSight"].asBool(false);
                if (key.keepInSight && key.lookAtBinding.empty())
                    return refuse(at + ".keepInSight");
                break;
            }
            case SequenceTrackType::Animation: {
                key.clip = std::string(source["clip"].asString());
                key.parameter = std::string(source["parameter"].asString());
                // One or the other: a clip begun, or a parameter set.
                if (key.clip.empty() == key.parameter.empty())
                    return refuse(at);
                if (!key.parameter.empty()) {
                    if (!readValue(source["value"], key.value) ||
                        (key.value.kind != SequenceValue::Kind::Number && key.value.kind != SequenceValue::Kind::Flag))
                        return refuse(at + ".value");
                }
                const f64 fade = source["fade"].asNumber(0.2);
                const f64 speed = source["speed"].asNumber(1.0);
                if (!(fade >= 0.0) || !std::isfinite(fade) || !(speed > 0.0) || !std::isfinite(speed))
                    return refuse(at);
                key.fade = static_cast<f32>(fade);
                key.speed = static_cast<f32>(speed);
                key.hold = source["hold"].asBool(false);
                break;
            }
            case SequenceTrackType::Line:
                key.line = std::string(source["line"].asString());
                key.at = std::string(source["at"].asString());
                if (key.line.empty())
                    return refuse(at + ".line");
                if (!key.at.empty() && !under(key.at))
                    return refuse(at + ".at");
                break;
            case SequenceTrackType::Sound: {
                key.sound = std::string(source["sound"].asString());
                key.at = std::string(source["at"].asString());
                key.category = std::string(source["category"].asString());
                const f64 volume = source["volume"].asNumber(1.0);
                if (key.sound.empty())
                    return refuse(at + ".sound");
                if (!key.at.empty() && !under(key.at))
                    return refuse(at + ".at");
                if (!(volume >= 0.0) || !std::isfinite(volume))
                    return refuse(at + ".volume");
                key.volume = static_cast<f32>(volume);
                break;
            }
            case SequenceTrackType::Property:
            case SequenceTrackType::Morph:
                if (variable("value"))
                    break;
                if (!readValue(source["value"], key.value))
                    return refuse(at + ".value");
                if (track.type == SequenceTrackType::Morph && key.value.kind != SequenceValue::Kind::Number)
                    return refuse(at + ".value");
                if (key.value.kind == SequenceValue::Kind::Text && key.value.text.starts_with("$$"))
                    key.value.text.erase(0, 1);
                break;
            case SequenceTrackType::Event:
                key.name = std::string(source["name"].asString());
                if (key.name.empty())
                    return refuse(at + ".name");
                // One plain value a script is handed: a number, a switch, a
                // word, or a few numbers.
                if (source.has("data") && !readValue(source["data"], key.value))
                    return refuse(at + ".data");
                break;
            case SequenceTrackType::Hold:
            case SequenceTrackType::Marker:
                key.name = std::string(source["name"].asString());
                if (key.name.empty())
                    return refuse(at + ".name");
                break;
            case SequenceTrackType::Fade:
            case SequenceTrackType::Bars:
                if (variable("value"))
                    break;
                // How much, nought to one.
                if (!readValue(source["value"], key.value) || key.value.kind != SequenceValue::Kind::Number ||
                    !(key.value.numbers[0] >= 0.0 && key.value.numbers[0] <= 1.0))
                    return refuse(at + ".value");
                break;
            case SequenceTrackType::Cast:
                key.show = source["show"].asBool(true);
                break;
            case SequenceTrackType::Caption: {
                key.caption = std::string(source["caption"].asString());
                key.speaker = std::string(source["speaker"].asString());
                const f64 seconds = source["seconds"].asNumber(0.0);
                if (key.caption.empty())
                    return refuse(at + ".caption");
                if (!(seconds >= 0.0) || !std::isfinite(seconds))
                    return refuse(at + ".seconds");
                key.seconds = static_cast<f32>(seconds);
                break;
            }
            }
            last = std::max(last, key.time);
            track.keys.push_back(std::move(key));
        }
        std::stable_sort(track.keys.begin(), track.keys.end(),
                         [](const SequenceKey& a, const SequenceKey& b) { return a.time < b.time; });
        // A property's keys are of one kind: a number cannot be eased into a
        // word.
        if (track.type == SequenceTrackType::Property) {
            // (A key whose value is a variable is whatever the game says it
            // is: it is held to the others' kind when it is read.)
            const SequenceKey* first = nullptr;
            for (const SequenceKey& key : track.keys) {
                if (!key.variable.empty())
                    continue;
                if (first == nullptr)
                    first = &key;
                if (key.value.kind != first->value.kind || key.value.count != first->value.count)
                    return refuse(where + ".keys");
            }
        }
        out.tracks.push_back(std::move(track));
    }

    const f64 length = root["length"].asNumber(0.0);
    if (root.has("length") && (!(length >= 0.0) || !std::isfinite(length)))
        return refuse("length");
    out.length = std::max(last, static_cast<f32>(length));
    return true;
}

namespace {

// A number as a person writes one: no exponent for the sizes a scene has, no
// trailing noughts, and what a 32-bit value was before it was widened.
void putNumber(std::string& out, f64 value, bool single = false)
{
    char text[40];
    std::snprintf(text, sizeof(text), single ? "%.7g" : "%.12g", value);
    out += text;
}

void putText(std::string& out, std::string_view text)
{
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += escaped;
            }
            else {
                out += c;
            }
        }
    }
    out += '"';
}

void putThree(std::string& out, const f64 (&numbers)[3])
{
    out += '[';
    for (usize index = 0; index < 3; ++index) {
        if (index != 0)
            out += ", ";
        putNumber(out, numbers[index]);
    }
    out += ']';
}

void putValue(std::string& out, const SequenceValue& value)
{
    switch (value.kind) {
    case SequenceValue::Kind::Number:
        putNumber(out, value.numbers[0]);
        break;
    case SequenceValue::Kind::Flag:
        out += value.flag ? "true" : "false";
        break;
    case SequenceValue::Kind::Text:
        putText(out, value.text);
        break;
    case SequenceValue::Kind::Numbers:
        out += '[';
        for (usize index = 0; index < value.count; ++index) {
            if (index != 0)
                out += ", ";
            putNumber(out, value.numbers[index]);
        }
        out += ']';
        break;
    case SequenceValue::Kind::None:
        out += "null";
        break;
    }
}

} // namespace

std::string_view sequenceTrackTypeName(SequenceTrackType type) noexcept
{
    for (const TypeName& named : kTypes) {
        if (named.type == type)
            return named.name;
    }
    return {};
}

std::string sequenceEaseName(core::EasingStyle style, core::EasingDirection direction)
{
    for (const StyleName& named : kStyles) {
        if (named.style != style)
            continue;
        return std::string(named.name) + (direction == core::EasingDirection::In    ? "In"
                                          : direction == core::EasingDirection::Out ? "Out"
                                                                                    : "InOut");
    }
    return "linear";
}

std::string writeSequence(const Sequence& sequence)
{
    std::string out = "{\n  \"format\": \"sequence\",\n  \"version\": 1,\n  \"bindings\": [";
    for (usize index = 0; index < sequence.bindings.size(); ++index) {
        if (index != 0)
            out += ", ";
        putText(out, sequence.bindings[index]);
    }
    out += "],\n";
    // Its length is said only where it is more than its last key.
    f32 last = 0.0f;
    for (const SequenceTrack& track : sequence.tracks) {
        for (const SequenceKey& key : track.keys)
            last = std::max(last, key.time);
    }
    if (sequence.length > last) {
        out += "  \"length\": ";
        putNumber(out, static_cast<f64>(sequence.length), true);
        out += ",\n";
    }
    out += "  \"tracks\": [";
    for (usize trackIndex = 0; trackIndex < sequence.tracks.size(); ++trackIndex) {
        const SequenceTrack& track = sequence.tracks[trackIndex];
        out += trackIndex == 0 ? "\n    { \"type\": " : ",\n    { \"type\": ";
        putText(out, sequenceTrackTypeName(track.type));
        if (!track.target.empty()) {
            out += track.type == SequenceTrackType::Cast ? ", \"as\": " : ", \"target\": ";
            putText(out, track.target);
        }
        if (!track.property.empty()) {
            out += track.type == SequenceTrackType::Morph ? ", \"key\": " : ", \"property\": ";
            putText(out, track.property);
        }
        if (track.type == SequenceTrackType::Fade &&
            (track.color[0] != 0.0f || track.color[1] != 0.0f || track.color[2] != 0.0f)) {
            const f64 colour[3]{static_cast<f64>(track.color[0]), static_cast<f64>(track.color[1]),
                                static_cast<f64>(track.color[2])};
            out += ", \"color\": [";
            for (usize channel = 0; channel < 3; ++channel) {
                if (channel != 0)
                    out += ", ";
                putNumber(out, colour[channel], true);
            }
            out += ']';
        }
        if (track.type == SequenceTrackType::Bars && track.aspect != 2.39f) {
            out += ", \"aspect\": ";
            putNumber(out, static_cast<f64>(track.aspect), true);
        }
        if (track.type == SequenceTrackType::Camera && track.curve)
            out += ", \"path\": \"curve\"";
        if (track.type == SequenceTrackType::Cast) {
            out += ", \"model\": ";
            putText(out, track.model);
        }
        out += ", \"keys\": [";
        for (usize keyIndex = 0; keyIndex < track.keys.size(); ++keyIndex) {
            const SequenceKey& key = track.keys[keyIndex];
            out += keyIndex == 0 ? "\n        { \"time\": " : ",\n        { \"time\": ";
            putNumber(out, static_cast<f64>(key.time), true);
            switch (track.type) {
            case SequenceTrackType::Camera:
                out += ", \"position\": ";
                putThree(out, key.position);
                if (!key.relativeTo.empty()) {
                    out += ", \"relativeTo\": ";
                    putText(out, key.relativeTo);
                }
                if (!key.lookAtBinding.empty()) {
                    out += ", \"lookAt\": ";
                    putText(out, key.lookAtBinding);
                }
                else if (key.hasLookAt) {
                    out += ", \"lookAt\": ";
                    putThree(out, key.lookAt);
                }
                if (key.hasRotation) {
                    out += ", \"rotation\": ";
                    putThree(out, key.rotation);
                }
                if (key.fieldOfView > 0.0f) {
                    out += ", \"fieldOfView\": ";
                    putNumber(out, static_cast<f64>(key.fieldOfView), true);
                }
                if (key.shake[0] != 0.0f || key.shake[1] != 0.0f) {
                    out += ", \"shake\": [";
                    putNumber(out, static_cast<f64>(key.shake[0]), true);
                    out += ", ";
                    putNumber(out, static_cast<f64>(key.shake[1]), true);
                    out += ']';
                }
                if (key.keepInSight)
                    out += ", \"keepInSight\": true";
                break;
            case SequenceTrackType::Animation:
                if (!key.clip.empty()) {
                    out += ", \"clip\": ";
                    putText(out, key.clip);
                    if (key.fade != 0.2f) {
                        out += ", \"fade\": ";
                        putNumber(out, static_cast<f64>(key.fade), true);
                    }
                    if (key.speed != 1.0f) {
                        out += ", \"speed\": ";
                        putNumber(out, static_cast<f64>(key.speed), true);
                    }
                    if (key.hold)
                        out += ", \"hold\": true";
                }
                else {
                    out += ", \"parameter\": ";
                    putText(out, key.parameter);
                    out += ", \"value\": ";
                    putValue(out, key.value);
                }
                break;
            case SequenceTrackType::Line:
                out += ", \"line\": ";
                putText(out, key.line);
                if (!key.at.empty()) {
                    out += ", \"at\": ";
                    putText(out, key.at);
                }
                break;
            case SequenceTrackType::Sound:
                out += ", \"sound\": ";
                putText(out, key.sound);
                if (!key.at.empty()) {
                    out += ", \"at\": ";
                    putText(out, key.at);
                }
                if (!key.category.empty()) {
                    out += ", \"category\": ";
                    putText(out, key.category);
                }
                if (key.volume != 1.0f) {
                    out += ", \"volume\": ";
                    putNumber(out, static_cast<f64>(key.volume), true);
                }
                break;
            case SequenceTrackType::Property:
            case SequenceTrackType::Morph:
                out += ", \"value\": ";
                if (!key.variable.empty())
                    putText(out, "$" + key.variable);
                else if (key.value.kind == SequenceValue::Kind::Text && key.value.text.starts_with('$'))
                    putText(out, "$" + key.value.text);
                else
                    putValue(out, key.value);
                break;
            case SequenceTrackType::Event:
                out += ", \"name\": ";
                putText(out, key.name);
                if (key.value.kind != SequenceValue::Kind::None) {
                    out += ", \"data\": ";
                    putValue(out, key.value);
                }
                break;
            case SequenceTrackType::Hold:
            case SequenceTrackType::Marker:
                out += ", \"name\": ";
                putText(out, key.name);
                break;
            case SequenceTrackType::Fade:
            case SequenceTrackType::Bars:
                out += ", \"value\": ";
                if (key.variable.empty())
                    putValue(out, key.value);
                else
                    putText(out, "$" + key.variable);
                break;
            case SequenceTrackType::Cast:
                if (!key.show)
                    out += ", \"show\": false";
                break;
            case SequenceTrackType::Caption:
                out += ", \"caption\": ";
                putText(out, key.caption);
                if (!key.speaker.empty()) {
                    out += ", \"speaker\": ";
                    putText(out, key.speaker);
                }
                if (key.seconds != 0.0f) {
                    out += ", \"seconds\": ";
                    putNumber(out, static_cast<f64>(key.seconds), true);
                }
                break;
            }
            if (!key.afterLine.empty()) {
                out += ", \"afterLine\": ";
                putText(out, key.afterLine);
            }
            if (key.cut)
                out += ", \"cut\": true";
            else if (key.style != core::EasingStyle::Linear) {
                out += ", \"ease\": ";
                putText(out, sequenceEaseName(key.style, key.direction));
            }
            out += " }";
        }
        out += track.keys.empty() ? "] }" : " ] }";
    }
    out += sequence.tracks.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

bool sequenceSpanAt(std::span<const SequenceKey> keys, f32 seconds, SequenceSpan& out) noexcept
{
    if (keys.empty())
        return false;
    // The last key that has been reached; of several at one moment, the last
    // of them -- a move's end and the cut that follows it share a time, and
    // from that moment on the picture is the cut's.
    usize reached = keys.size();
    for (usize index = 0; index < keys.size(); ++index) {
        if (keys[index].time <= seconds)
            reached = index;
    }
    if (reached == keys.size()) {
        out = SequenceSpan{0, 0, 0.0f};
        return true;
    }
    if (reached + 1 >= keys.size()) {
        out = SequenceSpan{reached, reached, 0.0f};
        return true;
    }
    const SequenceKey& from = keys[reached];
    const SequenceKey& to = keys[reached + 1];
    // A cut is not moved towards: the key before it holds until it is there.
    if (to.cut || !(to.time > from.time)) {
        out = SequenceSpan{reached, reached, 0.0f};
        return true;
    }
    const f32 raw = (seconds - from.time) / (to.time - from.time);
    out = SequenceSpan{reached, reached + 1, core::ease(std::clamp(raw, 0.0f, 1.0f), to.style, to.direction)};
    return true;
}

SequenceValue mixSequenceValues(const SequenceValue& from, const SequenceValue& to, f32 alpha)
{
    const bool numbers = (from.kind == SequenceValue::Kind::Number || from.kind == SequenceValue::Kind::Numbers) &&
                         from.kind == to.kind && from.count == to.count;
    if (!numbers)
        return alpha >= 1.0f ? to : from;
    SequenceValue mixed = from;
    for (usize index = 0; index < from.count; ++index)
        mixed.numbers[index] =
            from.numbers[index] + (to.numbers[index] - from.numbers[index]) * static_cast<f64>(alpha);
    return mixed;
}

} // namespace engine::asset
