#include "engine/app/mouths.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include "engine/asset/content.h"
#include "engine/audio/audio.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/platform/file.h"
#include "engine/render/animation.h"
#include "engine/scene/world.h"

namespace engine::app {

namespace {

using core::f32;

[[nodiscard]] core::u64 keyOf(core::InstanceId id) noexcept
{
    return (static_cast<core::u64>(id.index) << 32) | static_cast<core::u64>(id.generation);
}

// A file's text through the mounts, packed or loose, or nothing.
[[nodiscard]] std::optional<std::string> textOf(const asset::ContentMounts& mounts, std::string_view name)
{
    const asset::ResolvedContent resolved = mounts.resolve(name);
    if (!resolved.found())
        return std::nullopt;
    if (resolved.source == asset::ResolvedContent::Source::Loose) {
        std::vector<std::byte> bytes;
        if (!platform::readFile(resolved.path, bytes))
            return std::nullopt;
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return std::string(reinterpret_cast<const char*>(resolved.bytes.data()), resolved.bytes.size());
}

// Whether `id` is `under`, or is under it.
[[nodiscard]] bool within(const scene::World& world, core::InstanceId id, core::InstanceId under)
{
    for (int guard = 0; id.valid() && guard < 64; ++guard) {
        if (id == under)
            return true;
        id = world.parentOf(id);
    }
    return false;
}

// The key a face with no map opens by loudness: the names such a key has.
[[nodiscard]] std::string_view jawOf(const render::AnimationSystem& animation, core::InstanceId mesh)
{
    constexpr std::array<std::string_view, 4> Names{"JawOpen", "MouthOpen", "jawOpen", "mouthOpen"};
    const core::u32 count = animation.morphTargetCount(mesh);
    for (const std::string_view wanted : Names) {
        for (core::u32 target = 0; target < count; ++target) {
            if (animation.morphTargetName(mesh, target) == wanted)
                return wanted;
        }
    }
    return {};
}

template <class List>
[[nodiscard]] auto placeOf(List& list, std::string_view name)
{
    return std::lower_bound(list.begin(), list.end(), name,
                            [](const auto& entry, std::string_view key) { return entry.name < key; });
}

} // namespace

const asset::VisemeTrack* Mouths::trackFor(const asset::ContentMounts* mounts, std::string_view sound)
{
    if (mounts == nullptr || sound.empty())
        return nullptr;
    const std::string name = asset::visemeTrackName(sound);
    auto at = placeOf(tracks_, name);
    if (at == tracks_.end() || at->name != name) {
        Read<asset::VisemeTrack> read;
        read.name = name;
        if (const std::optional<std::string> text = textOf(*mounts, name); text.has_value()) {
            std::string why;
            read.found = asset::readVisemeTrack(*text, read.file, &why);
            if (!read.found) {
                const std::array<core::I18nArg, 2> args{core::I18nArg{"file", std::string_view{name}},
                                                        core::I18nArg{"why", std::string_view{why}}};
                core::log(core::LogLevel::Warn, ENG_TR("app.warn.viseme_track_unreadable"), args);
            }
        }
        at = tracks_.insert(at, std::move(read));
    }
    return at->found ? &at->file : nullptr;
}

const asset::FaceMap* Mouths::mapFor(const asset::ContentMounts* mounts, std::string_view name)
{
    if (mounts == nullptr || name.empty())
        return nullptr;
    auto at = placeOf(maps_, name);
    if (at == maps_.end() || at->name != name) {
        Read<asset::FaceMap> read;
        read.name = std::string(name);
        if (const std::optional<std::string> text = textOf(*mounts, name); text.has_value()) {
            std::string why;
            read.found = asset::readFaceMap(*text, read.file, &why);
            if (!read.found) {
                const std::array<core::I18nArg, 2> args{core::I18nArg{"file", name},
                                                        core::I18nArg{"why", std::string_view{why}}};
                core::log(core::LogLevel::Warn, ENG_TR("app.warn.face_map_unreadable"), args);
            }
        }
        at = maps_.insert(at, std::move(read));
    }
    return at->found ? &at->file : nullptr;
}

void Mouths::forget(std::span<const std::string> names)
{
    for (const std::string& name : names) {
        if (const auto at = placeOf(tracks_, name); at != tracks_.end() && at->name == name)
            tracks_.erase(at);
        if (const auto at = placeOf(maps_, name); at != maps_.end() && at->name == name)
            maps_.erase(at);
    }
}

void Mouths::clear()
{
    tracks_.clear();
    maps_.clear();
    mouths_.clear();
}

void Mouths::step(const scene::World& world, audio::AudioSystem& audio, render::AnimationSystem& animation,
                  const asset::ContentMounts* mounts, f32 seconds)
{
    stats_ = Stats{};
    animation.clearPresentedMorphWeights();
    // No mouth anywhere is every game that has none: one size asked.
    if (world.lipSyncs().size() == 0) {
        mouths_.clear();
        return;
    }
    for (Mouth& mouth : mouths_)
        mouth.seen = false;

    const std::span<const core::InstanceId> heard = audio.heardNow();
    world.lipSyncs().forEach([&](core::InstanceId id, const scene::LipSyncComponent& lips) {
        if (!lips.enabled || world.destroyed(id))
            return;
        const core::InstanceId mesh = world.parentOf(id);
        const scene::MeshPartComponent* face = mesh.valid() ? world.meshParts().find(mesh) : nullptr;
        if (face == nullptr)
            return;

        // Whose voice: the first that can be heard here and is said under
        // the instance it names -- or under the mesh's own parent.
        const core::InstanceId source =
            lips.source.valid() && world.alive(lips.source) ? lips.source : world.parentOf(mesh);
        core::InstanceId voice;
        for (const core::InstanceId sound : heard) {
            const scene::SoundComponent* said = world.sounds().find(sound);
            if (said != nullptr && said->category == 2 && within(world, sound, source)) {
                voice = sound;
                break;
            }
        }

        // Where its keys are going.
        wanted_.clear();
        const auto want = [&](std::string_view key, f32 weight) {
            if (key.empty())
                return;
            const f32 scaled = std::clamp(weight * lips.weight, 0.0f, 2.0f);
            for (auto& [name, held] : wanted_) {
                if (name == key) {
                    held = std::max(held, scaled);
                    return;
                }
            }
            wanted_.emplace_back(std::string(key), scaled);
        };
        if (voice.valid()) {
            const std::string_view mapName = world.atoms().text(lips.map);
            const asset::FaceMap* map =
                mapFor(mounts, mapName.empty() ? asset::faceMapName(world.atoms().text(face->meshContent))
                                               : std::string(mapName));
            const scene::SoundComponent* said = world.sounds().find(voice);
            // 0 `Auto`, 1 `Bands`, 2 `Loudness`.
            const asset::VisemeTrack* track = lips.mode == 0 && map != nullptr && !map->visemes.empty()
                                                  ? trackFor(mounts, audio.heardAs(world, voice))
                                                  : nullptr;
            if (track != nullptr) {
                // The track of the language this machine hears, at the
                // moment the line has got to. Between two cues the mouth
                // goes back to rest, eased like any change.
                if (const asset::VisemeCue* cue = track->at(static_cast<f32>(said->timePosition)); cue != nullptr) {
                    if (const std::vector<asset::FaceKey>* keys = map->viseme(cue->value); keys != nullptr) {
                        for (const asset::FaceKey& key : *keys)
                            want(key.key, key.weight);
                    }
                }
                stats_.byTrack += 1;
            }
            else if (lips.mode != 2 && map != nullptr && map->hasBands()) {
                // What three bands say of a mouth: energy low and in the
                // middle is one that is open, energy high up is one that is
                // wide (an "ee", an "s"), and low with little above it is
                // one that is round (an "oo"). Rough, and enough to tell the
                // three apart with no file at all.
                const std::array<f32, 3> bands = audio.bands(world, voice);
                want(map->open, std::min(1.0f, 1.6f * std::max(bands[0], bands[1])));
                want(map->wide, std::min(1.0f, 2.2f * bands[2]));
                want(map->round, std::min(1.0f, 2.2f * std::max(0.0f, bands[0] - bands[1])));
                stats_.byBands += 1;
            }
            else {
                const std::string_view jaw =
                    map != nullptr && !map->loudness.empty() ? std::string_view{map->loudness} : jawOf(animation, mesh);
                want(jaw, std::min(1.0f, 1.5f * audio.loudness(world, voice)));
                stats_.byLoudness += 1;
            }
        }

        // Eased from where each key is: a mouth does not snap, and one whose
        // line has ended closes rather than vanishing.
        const core::u64 key = keyOf(id);
        auto mouth = std::lower_bound(mouths_.begin(), mouths_.end(), key,
                                      [](const Mouth& entry, core::u64 wanted) { return entry.key < wanted; });
        if (mouth == mouths_.end() || mouth->key != key) {
            if (wanted_.empty())
                return;
            mouth = mouths_.insert(mouth, Mouth{key, true, {}});
        }
        mouth->seen = true;
        const f32 ease = lips.smoothing > 0.0f && seconds > 0.0f ? 1.0f - std::exp(-seconds / lips.smoothing) : 1.0f;
        for (auto& [name, held] : mouth->weights) {
            f32 target = 0.0f;
            for (const auto& [wantedName, weight] : wanted_) {
                if (wantedName == name)
                    target = weight;
            }
            held += (target - held) * ease;
        }
        for (const auto& [name, weight] : wanted_) {
            const bool known = std::any_of(mouth->weights.begin(), mouth->weights.end(),
                                           [&](const auto& one) { return one.first == name; });
            if (!known)
                mouth->weights.emplace_back(name, weight * ease);
        }
        std::erase_if(mouth->weights, [](const auto& one) { return one.second < 0.001f; });
        if (mouth->weights.empty())
            return;
        animation.presentMorphWeights(mesh, mouth->weights);
        stats_.moved += 1;
    });

    std::erase_if(mouths_, [](const Mouth& mouth) { return !mouth.seen || mouth.weights.empty(); });
}

} // namespace engine::app
