// The hand-written half of `audio`'s reflection (architecture.md §4).
//
// Twelve properties, and four of them are worth reading. `Playing` is a
// property AND a pair of methods, so the write path is the same code the methods
// call. `TimePosition` is writable, which is how a script seeks, and kept
// inside `TimeLength`, which only `audio` writes. `Group` is an
// Instance reference and therefore the one property here that can be nil.
#include <cmath>

#include "../generated/class_descriptors.gen.h"
#include "engine/audio/scene_types.h"
#include "engine/scene/world.h"

namespace engine::audio {
namespace native {
namespace {

using core::f32;
using core::f64;
using scene::Value;

[[nodiscard]] bool isFinite(f64 value) noexcept
{
    return std::isfinite(value);
}

[[nodiscard]] scene::SoundComponent* sound(scene::World& world, core::InstanceId id)
{
    return world.sounds().find(id);
}

[[nodiscard]] const scene::SoundComponent* sound(const scene::World& world, core::InstanceId id)
{
    return world.sounds().find(id);
}

// A non-negative number, which is what almost every property here is.
[[nodiscard]] bool takeAtLeastZero(const Value& value, f32& out)
{
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !isFinite(*number) || *number < 0.0)
        return false;
    out = static_cast<f32>(*number);
    return true;
}

} // namespace

// --- AudioGroup --------------------------------------------------------------

Value getAudioGroupVolume(const scene::World& world, core::InstanceId id)
{
    const scene::AudioGroupComponent* group = world.audioGroups().find(id);
    return group == nullptr ? Value{} : Value{static_cast<f64>(group->volume)};
}

bool setAudioGroupVolume(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::AudioGroupComponent* group = world.audioGroups().find(id);
    return group != nullptr && takeAtLeastZero(value, group->volume);
}

void attachAudioGroupComponents(scene::World& world, core::InstanceId id)
{
    world.audioGroups().add(id, scene::AudioGroupComponent{});
}

void detachAudioGroupComponents(scene::World& world, core::InstanceId id)
{
    world.audioGroups().remove(id);
}

// --- Sound -------------------------------------------------------------------

Value getSoundContent(const scene::World& world, core::InstanceId id)
{
    const scene::SoundComponent* self = sound(world, id);
    return self == nullptr ? Value{} : Value{self->content};
}

bool setSoundContent(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::SoundComponent* self = sound(world, id);
    if (text == nullptr || self == nullptr)
        return false;
    self->content = *text;
    // **Interned, though the component keeps the text** (protocol 44): a
    // sound's file travels as a name does, and a match's authority reads the
    // world without growing its names -- so the name is made here, where the
    // text arrives, as a picture's is by the property that takes it.
    (void)world.atoms().intern(*text);
    // A new sound is a new thing to load, so `Loaded` fires again. It costs
    // nothing today -- there is no file -- and it is the behaviour a caller will
    // expect the moment there is one.
    self->loadedFired = false;
    // And a new length, which the next tick reads from the new file.
    self->timeLength = 0.0;
    return true;
}

Value getSoundPlaying(const scene::World& world, core::InstanceId id)
{
    const scene::SoundComponent* self = sound(world, id);
    return self == nullptr ? Value{} : Value{self->playing};
}

bool setSoundPlaying(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* flag = std::get_if<bool>(&value);
    scene::SoundComponent* self = sound(world, id);
    if (flag == nullptr || self == nullptr)
        return false;
    // Deliberately NOT a rewind. `Playing = false` is `Pause` and `Playing =
    // true` is `Resume`: the property says whether the timeline is advancing,
    // and a sound that reached its end has already rewound (see `Ended`).
    self->playing = *flag;
    return true;
}

Value getSoundLooped(const scene::World& world, core::InstanceId id)
{
    const scene::SoundComponent* self = sound(world, id);
    return self == nullptr ? Value{} : Value{self->looped};
}

bool setSoundLooped(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* flag = std::get_if<bool>(&value);
    scene::SoundComponent* self = sound(world, id);
    if (flag == nullptr || self == nullptr)
        return false;
    self->looped = *flag;
    return true;
}

Value getSoundVolume(const scene::World& world, core::InstanceId id)
{
    const scene::SoundComponent* self = sound(world, id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->volume)};
}

bool setSoundVolume(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundComponent* self = sound(world, id);
    return self != nullptr && takeAtLeastZero(value, self->volume);
}

Value getSoundPlaybackSpeed(const scene::World& world, core::InstanceId id)
{
    const scene::SoundComponent* self = sound(world, id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->playbackSpeed)};
}

bool setSoundPlaybackSpeed(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* number = std::get_if<f64>(&value);
    scene::SoundComponent* self = sound(world, id);
    // Zero is refused rather than treated as a pause. `Playing` is what pauses,
    // and a speed of zero would be a sound that never ends -- which is a hang
    // dressed as a property.
    if (number == nullptr || self == nullptr || !isFinite(*number) || *number <= 0.0)
        return false;
    self->playbackSpeed = static_cast<f32>(*number);
    return true;
}

Value getSoundTimePosition(const scene::World& world, core::InstanceId id)
{
    const scene::SoundComponent* self = sound(world, id);
    return self == nullptr ? Value{} : Value{self->timePosition};
}

bool setSoundTimePosition(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* number = std::get_if<f64>(&value);
    scene::SoundComponent* self = sound(world, id);
    if (number == nullptr || self == nullptr || !isFinite(*number) || *number < 0.0)
        return false;
    // **Kept inside the clip** once its length is known: past the end is where
    // the tick would have stopped it, or wrapped it for a loop.
    f64 position = *number;
    if (self->timeLength > 0.0 && position > self->timeLength)
        position = self->looped ? std::fmod(position, self->timeLength) : self->timeLength;
    self->timePosition = position;
    // Where the next `Play` starts, rather than at 0.
    self->seeked = true;
    return true;
}

Value getSoundTimeLength(const scene::World& world, core::InstanceId id)
{
    const scene::SoundComponent* self = sound(world, id);
    return self == nullptr ? Value{} : Value{self->timeLength};
}

Value getSoundRollOffMinDistance(const scene::World& world, core::InstanceId id)
{
    const scene::SoundComponent* self = sound(world, id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->rollOffMinDistance)};
}

bool setSoundRollOffMinDistance(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundComponent* self = sound(world, id);
    return self != nullptr && takeAtLeastZero(value, self->rollOffMinDistance);
}

Value getSoundRollOffMaxDistance(const scene::World& world, core::InstanceId id)
{
    const scene::SoundComponent* self = sound(world, id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->rollOffMaxDistance)};
}

bool setSoundRollOffMaxDistance(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundComponent* self = sound(world, id);
    return self != nullptr && takeAtLeastZero(value, self->rollOffMaxDistance);
}

Value getSoundGroup(const scene::World& world, core::InstanceId id)
{
    const scene::SoundComponent* self = sound(world, id);
    if (self == nullptr || !world.alive(self->group))
        return Value{};
    return Value{self->group};
}

bool setSoundGroup(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundComponent* self = sound(world, id);
    if (self == nullptr)
        return false;

    // nil clears it, which is what an optional Instance property means.
    if (std::holds_alternative<std::monostate>(value)) {
        self->group = {};
        return true;
    }

    const auto* reference = std::get_if<core::InstanceId>(&value);
    // Refused unless it really is an `AudioGroup`. A `Part` assigned here would
    // read back as a Part and mix as nothing, which is the silent kind of wrong.
    if (reference == nullptr || world.audioGroups().find(*reference) == nullptr)
        return false;
    self->group = *reference;
    return true;
}

void attachSoundComponents(scene::World& world, core::InstanceId id)
{
    world.sounds().add(id, scene::SoundComponent{});
}

void detachSoundComponents(scene::World& world, core::InstanceId id)
{
    world.sounds().remove(id);
}

// --- AudioService ------------------------------------------------------------

Value getAudioServiceMasterVolume(const scene::World& world, core::InstanceId)
{
    return Value{static_cast<f64>(world.engineState().masterVolume)};
}

bool setAudioServiceMasterVolume(scene::World& world, core::InstanceId, const Value& value)
{
    return takeAtLeastZero(value, world.engineState().masterVolume);
}

// --- Categories, the player's volumes, captions (ADR 0200) -------------------------

namespace {

// A number from `low` to `high`, both included.
[[nodiscard]] bool takeBetween(const Value& value, f32 low, f32 high, f32& out)
{
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !isFinite(*number) || *number < static_cast<f64>(low) || *number > static_cast<f64>(high))
        return false;
    out = static_cast<f32>(*number);
    return true;
}

// One of the player's own settings: a machine with no player has nobody to
// ask, as `LocalizationService.Locale` has it.
[[nodiscard]] bool takePlayers(scene::World& world, const Value& value, f32 low, f32 high, f32& out)
{
    return world.engineState().graphicsDisplay && takeBetween(value, low, high, out);
}

} // namespace

Value getSoundCategory(const scene::World& world, core::InstanceId id)
{
    const scene::SoundComponent* self = sound(world, id);
    return self == nullptr ? Value{} : Value{scene::EnumValue{generated::SoundCategoryEnumId, self->category}};
}

bool setSoundCategory(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundComponent* self = sound(world, id);
    const auto* item = std::get_if<scene::EnumValue>(&value);
    if (self == nullptr || item == nullptr || item->enumId != generated::SoundCategoryEnumId ||
        world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    self->category = item->value;
    return true;
}

// What this machine's mixer says of it, asked of the host's audio system: a
// world with none hears nothing.
Value getSoundLoudness(const scene::World& world, core::InstanceId id)
{
    scene::SoundMeter* meter = world.soundMeter();
    return Value{meter != nullptr ? static_cast<f64>(meter->loudness(world, id)) : 0.0};
}

// A caption's two keys are atoms (see `CaptionComponent`): made here, where the
// text arrives, as a sound's content is.
Value getCaptionText(const scene::World& world, core::InstanceId id)
{
    const scene::CaptionComponent* self = world.captions().find(id);
    return self == nullptr ? Value{} : Value{std::string(world.atoms().text(self->text))};
}

bool setCaptionText(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::CaptionComponent* self = world.captions().find(id);
    if (text == nullptr || self == nullptr)
        return false;
    self->text = text->empty() ? core::NameAtom{} : world.atoms().intern(*text);
    return true;
}

Value getCaptionSpeaker(const scene::World& world, core::InstanceId id)
{
    const scene::CaptionComponent* self = world.captions().find(id);
    return self == nullptr ? Value{} : Value{std::string(world.atoms().text(self->speaker))};
}

bool setCaptionSpeaker(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* text = std::get_if<std::string>(&value);
    scene::CaptionComponent* self = world.captions().find(id);
    if (text == nullptr || self == nullptr)
        return false;
    self->speaker = text->empty() ? core::NameAtom{} : world.atoms().intern(*text);
    return true;
}

Value getCaptionColor(const scene::World& world, core::InstanceId id)
{
    const scene::CaptionComponent* self = world.captions().find(id);
    return self == nullptr ? Value{} : Value{self->color};
}

bool setCaptionColor(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* color = std::get_if<core::Color3>(&value);
    scene::CaptionComponent* self = world.captions().find(id);
    if (color == nullptr || self == nullptr)
        return false;
    self->color = *color;
    return true;
}

Value getCaptionSeconds(const scene::World& world, core::InstanceId id)
{
    const scene::CaptionComponent* self = world.captions().find(id);
    return self == nullptr ? Value{} : Value{static_cast<f64>(self->seconds)};
}

bool setCaptionSeconds(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::CaptionComponent* self = world.captions().find(id);
    return self != nullptr && takeAtLeastZero(value, self->seconds);
}

void attachCaptionComponents(scene::World& world, core::InstanceId id)
{
    world.captions().add(id, scene::CaptionComponent{});
}

void detachCaptionComponents(scene::World& world, core::InstanceId id)
{
    world.captions().remove(id);
}

Value getAudioServicePlayerVolume(const scene::World& world, core::InstanceId)
{
    return Value{static_cast<f64>(world.engineState().playerSound.playerVolume)};
}

bool setAudioServicePlayerVolume(scene::World& world, core::InstanceId, const Value& value)
{
    return takePlayers(world, value, 0.0f, 1.0f, world.engineState().playerSound.playerVolume);
}

Value getAudioServiceMusicVolume(const scene::World& world, core::InstanceId)
{
    return Value{static_cast<f64>(world.engineState().playerSound.musicVolume)};
}

bool setAudioServiceMusicVolume(scene::World& world, core::InstanceId, const Value& value)
{
    return takePlayers(world, value, 0.0f, 1.0f, world.engineState().playerSound.musicVolume);
}

Value getAudioServiceEffectsVolume(const scene::World& world, core::InstanceId)
{
    return Value{static_cast<f64>(world.engineState().playerSound.effectsVolume)};
}

bool setAudioServiceEffectsVolume(scene::World& world, core::InstanceId, const Value& value)
{
    return takePlayers(world, value, 0.0f, 1.0f, world.engineState().playerSound.effectsVolume);
}

Value getAudioServiceVoiceVolume(const scene::World& world, core::InstanceId)
{
    return Value{static_cast<f64>(world.engineState().playerSound.voiceVolume)};
}

bool setAudioServiceVoiceVolume(scene::World& world, core::InstanceId, const Value& value)
{
    return takePlayers(world, value, 0.0f, 1.0f, world.engineState().playerSound.voiceVolume);
}

Value getAudioServiceMusicUnderVoice(const scene::World& world, core::InstanceId)
{
    return Value{static_cast<f64>(world.engineState().musicUnderVoice)};
}

bool setAudioServiceMusicUnderVoice(scene::World& world, core::InstanceId, const Value& value)
{
    return takeBetween(value, 0.0f, 1.0f, world.engineState().musicUnderVoice);
}

// --- DialogueService (ADR 0200) -----------------------------------------------------
//
// The three subtitle settings are the player's: each remembers that it was
// said, which is what puts it in their file.

Value getDialogueServiceSubtitles(const scene::World& world, core::InstanceId)
{
    return Value{scene::EnumValue{generated::SubtitleModeEnumId, world.engineState().playerSound.subtitles}};
}

bool setDialogueServiceSubtitles(scene::World& world, core::InstanceId, const Value& value)
{
    const auto* item = std::get_if<scene::EnumValue>(&value);
    scene::EngineState& state = world.engineState();
    if (item == nullptr || !state.graphicsDisplay || item->enumId != generated::SubtitleModeEnumId ||
        world.enums().findValue(item->enumId, item->value) == nullptr)
        return false;
    state.playerSound.subtitles = item->value;
    state.playerSound.subtitlesSaid = true;
    return true;
}

Value getDialogueServiceSubtitleScale(const scene::World& world, core::InstanceId)
{
    return Value{static_cast<f64>(world.engineState().playerSound.subtitleScale)};
}

bool setDialogueServiceSubtitleScale(scene::World& world, core::InstanceId, const Value& value)
{
    scene::PlayerSound& player = world.engineState().playerSound;
    if (!takePlayers(world, value, 0.75f, 2.0f, player.subtitleScale))
        return false;
    player.subtitleScaleSaid = true;
    return true;
}

Value getDialogueServiceSubtitleBackground(const scene::World& world, core::InstanceId)
{
    return Value{static_cast<f64>(world.engineState().playerSound.subtitleBackground)};
}

bool setDialogueServiceSubtitleBackground(scene::World& world, core::InstanceId, const Value& value)
{
    scene::PlayerSound& player = world.engineState().playerSound;
    if (!takePlayers(world, value, 0.0f, 1.0f, player.subtitleBackground))
        return false;
    player.subtitleBackgroundSaid = true;
    return true;
}

// --- Sound effects (ADR 0131) ---------------------------------------------------
//
// Nine classes over one component. Every number has a range its page says,
// and a value outside it is refused: a filter told to cut at a million hertz
// or a compressor at a ratio of nothing is a sound that goes wrong with
// nothing saying why.

namespace {

[[nodiscard]] bool takeWithin(const Value& value, f32 low, f32 high, f32& out)
{
    const auto* number = std::get_if<f64>(&value);
    if (number == nullptr || !isFinite(*number) || *number < static_cast<f64>(low) || *number > static_cast<f64>(high))
        return false;
    out = static_cast<f32>(*number);
    return true;
}

} // namespace

Value getSoundEffectEnabled(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{effect->enabled};
}

bool setSoundEffectEnabled(scene::World& world, core::InstanceId id, const Value& value)
{
    const auto* next = std::get_if<bool>(&value);
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    if (next == nullptr || effect == nullptr)
        return false;
    effect->enabled = *next;
    return true;
}

Value getSoundEffectPriority(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->priority)};
}

bool setSoundEffectPriority(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, -1.0e6f, 1.0e6f, effect->priority);
}

void attachReverbSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    scene::SoundEffectComponent effect;
    effect.kind = 1;
    effect.roomSize = 0.5f;
    effect.damping = 0.5f;
    effect.wetLevel = 0.3f;
    effect.dryLevel = 1.0f;
    effect.width = 1.0f;
    world.soundEffects().add(id, effect);
}

void detachReverbSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    world.soundEffects().remove(id);
}

Value getReverbSoundEffectRoomSize(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->roomSize)};
}

bool setReverbSoundEffectRoomSize(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.0f, 1.0f, effect->roomSize);
}

Value getReverbSoundEffectDamping(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->damping)};
}

bool setReverbSoundEffectDamping(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.0f, 1.0f, effect->damping);
}

Value getReverbSoundEffectWetLevel(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->wetLevel)};
}

bool setReverbSoundEffectWetLevel(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.0f, 1.0f, effect->wetLevel);
}

Value getReverbSoundEffectDryLevel(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->dryLevel)};
}

bool setReverbSoundEffectDryLevel(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.0f, 1.0f, effect->dryLevel);
}

Value getReverbSoundEffectWidth(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->width)};
}

bool setReverbSoundEffectWidth(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.0f, 1.0f, effect->width);
}

void attachEchoSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    scene::SoundEffectComponent effect;
    effect.kind = 2;
    effect.delay = 0.3f;
    effect.feedback = 0.4f;
    effect.wetLevel = 0.5f;
    effect.dryLevel = 1.0f;
    world.soundEffects().add(id, effect);
}

void detachEchoSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    world.soundEffects().remove(id);
}

Value getEchoSoundEffectDelay(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->delay)};
}

bool setEchoSoundEffectDelay(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.01f, 2.0f, effect->delay);
}

Value getEchoSoundEffectFeedback(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->feedback)};
}

bool setEchoSoundEffectFeedback(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.0f, 0.95f, effect->feedback);
}

Value getEchoSoundEffectWetLevel(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->wetLevel)};
}

bool setEchoSoundEffectWetLevel(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.0f, 1.0f, effect->wetLevel);
}

Value getEchoSoundEffectDryLevel(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->dryLevel)};
}

bool setEchoSoundEffectDryLevel(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.0f, 1.0f, effect->dryLevel);
}

void attachEqualizerSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    scene::SoundEffectComponent effect;
    effect.kind = 3;
    effect.lowGain = 0.0f;
    effect.midGain = 0.0f;
    effect.highGain = 0.0f;
    effect.midLow = 400.0f;
    effect.midHigh = 4000.0f;
    world.soundEffects().add(id, effect);
}

void detachEqualizerSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    world.soundEffects().remove(id);
}

Value getEqualizerSoundEffectLowGain(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->lowGain)};
}

bool setEqualizerSoundEffectLowGain(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, -80.0f, 12.0f, effect->lowGain);
}

Value getEqualizerSoundEffectMidGain(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->midGain)};
}

bool setEqualizerSoundEffectMidGain(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, -80.0f, 12.0f, effect->midGain);
}

Value getEqualizerSoundEffectHighGain(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->highGain)};
}

bool setEqualizerSoundEffectHighGain(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, -80.0f, 12.0f, effect->highGain);
}

Value getEqualizerSoundEffectMidLow(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->midLow)};
}

bool setEqualizerSoundEffectMidLow(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 20.0f, 20000.0f, effect->midLow);
}

Value getEqualizerSoundEffectMidHigh(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->midHigh)};
}

bool setEqualizerSoundEffectMidHigh(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 20.0f, 20000.0f, effect->midHigh);
}

void attachLowPassSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    scene::SoundEffectComponent effect;
    effect.kind = 4;
    effect.cutoff = 2000.0f;
    effect.resonance = 0.707f;
    world.soundEffects().add(id, effect);
}

void detachLowPassSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    world.soundEffects().remove(id);
}

Value getLowPassSoundEffectCutoff(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->cutoff)};
}

bool setLowPassSoundEffectCutoff(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 20.0f, 20000.0f, effect->cutoff);
}

Value getLowPassSoundEffectResonance(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->resonance)};
}

bool setLowPassSoundEffectResonance(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.1f, 10.0f, effect->resonance);
}

void attachHighPassSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    scene::SoundEffectComponent effect;
    effect.kind = 5;
    effect.cutoff = 500.0f;
    effect.resonance = 0.707f;
    world.soundEffects().add(id, effect);
}

void detachHighPassSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    world.soundEffects().remove(id);
}

Value getHighPassSoundEffectCutoff(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->cutoff)};
}

bool setHighPassSoundEffectCutoff(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 20.0f, 20000.0f, effect->cutoff);
}

Value getHighPassSoundEffectResonance(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->resonance)};
}

bool setHighPassSoundEffectResonance(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.1f, 10.0f, effect->resonance);
}

void attachDistortionSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    scene::SoundEffectComponent effect;
    effect.kind = 6;
    effect.level = 0.5f;
    world.soundEffects().add(id, effect);
}

void detachDistortionSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    world.soundEffects().remove(id);
}

Value getDistortionSoundEffectLevel(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->level)};
}

bool setDistortionSoundEffectLevel(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.0f, 1.0f, effect->level);
}

void attachCompressorSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    scene::SoundEffectComponent effect;
    effect.kind = 7;
    effect.threshold = -20.0f;
    effect.ratio = 4.0f;
    effect.attack = 0.01f;
    effect.release = 0.1f;
    effect.makeupGain = 0.0f;
    world.soundEffects().add(id, effect);
}

void detachCompressorSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    world.soundEffects().remove(id);
}

Value getCompressorSoundEffectThreshold(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->threshold)};
}

bool setCompressorSoundEffectThreshold(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, -60.0f, 0.0f, effect->threshold);
}

Value getCompressorSoundEffectRatio(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->ratio)};
}

bool setCompressorSoundEffectRatio(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 1.0f, 20.0f, effect->ratio);
}

Value getCompressorSoundEffectAttack(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->attack)};
}

bool setCompressorSoundEffectAttack(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.001f, 1.0f, effect->attack);
}

Value getCompressorSoundEffectRelease(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->release)};
}

bool setCompressorSoundEffectRelease(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.01f, 2.0f, effect->release);
}

Value getCompressorSoundEffectMakeupGain(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->makeupGain)};
}

bool setCompressorSoundEffectMakeupGain(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.0f, 24.0f, effect->makeupGain);
}

void attachChorusSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    scene::SoundEffectComponent effect;
    effect.kind = 8;
    effect.rate = 0.5f;
    effect.depth = 0.5f;
    effect.mix = 0.5f;
    world.soundEffects().add(id, effect);
}

void detachChorusSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    world.soundEffects().remove(id);
}

Value getChorusSoundEffectRate(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->rate)};
}

bool setChorusSoundEffectRate(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.01f, 10.0f, effect->rate);
}

Value getChorusSoundEffectDepth(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->depth)};
}

bool setChorusSoundEffectDepth(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.0f, 1.0f, effect->depth);
}

Value getChorusSoundEffectMix(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->mix)};
}

bool setChorusSoundEffectMix(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.0f, 1.0f, effect->mix);
}

void attachPitchShiftSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    scene::SoundEffectComponent effect;
    effect.kind = 9;
    effect.octave = 1.25f;
    world.soundEffects().add(id, effect);
}

void detachPitchShiftSoundEffectComponents(scene::World& world, core::InstanceId id)
{
    world.soundEffects().remove(id);
}

Value getPitchShiftSoundEffectOctave(const scene::World& world, core::InstanceId id)
{
    const scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect == nullptr ? Value{} : Value{static_cast<f64>(effect->octave)};
}

bool setPitchShiftSoundEffectOctave(scene::World& world, core::InstanceId id, const Value& value)
{
    scene::SoundEffectComponent* effect = world.soundEffects().find(id);
    return effect != nullptr && takeWithin(value, 0.5f, 2.0f, effect->octave);
}

} // namespace native

void registerSceneTypes(scene::ClassRegistry& classes, core::AtomTable& atoms)
{
    generated::registerClasses(classes, atoms);
}

} // namespace engine::audio
