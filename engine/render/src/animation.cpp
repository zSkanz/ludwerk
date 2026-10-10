#include "engine/render/animation.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iterator>

#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/profile.h"
#include "engine/core/text_key.h"
#include "engine/scene/world.h"

namespace engine::render {
namespace {

using core::CFrameD;
using core::DVec3;
using core::Mat3;
using core::Mat4;
using core::Vec3;

// The key at or before `time`, by binary search. The last key for a time past
// the end, which is what a clip that is not looping should hold.
[[nodiscard]] usize keyBefore(const std::vector<f32>& times, f32 time) noexcept
{
    if (times.size() < 2)
        return 0;
    const auto upper = std::upper_bound(times.begin(), times.end(), time);
    if (upper == times.begin())
        return 0;
    return static_cast<usize>(std::distance(times.begin(), upper)) - 1;
}

[[nodiscard]] f32 fractionBetween(const std::vector<f32>& times, usize key, f32 time) noexcept
{
    if (key + 1 >= times.size())
        return 0.0f;
    const f32 span = times[key + 1] - times[key];
    if (span <= 0.0f)
        return 0.0f;
    return std::clamp((time - times[key]) / span, 0.0f, 1.0f);
}

// A channel between two keys, into `out`: held, along a line, or along the
// cubic spline its tangents describe (D515).
[[nodiscard]] bool sampleChannel(const asset::AnimationChannel& channel, f32 time, f32* out) noexcept
{
    const usize stride = channel.stride;
    const usize perKey = channel.valuesPerKey();
    // `decodeMesh` refuses anything else (audit F1); a channel built in memory
    // is not decoded, and a sample past these four floats is somebody's stack.
    if ((stride != 3 && stride != 4) || channel.times.empty() ||
        channel.values.size() < channel.times.size() * stride * perKey)
        return false;
    const usize key = keyBefore(channel.times, time);
    const bool last = key + 1 >= channel.times.size();
    // A key's value: the middle of its three on a spline.
    const auto valueOf = [&](usize index) {
        return &channel.values[(index * perKey + (perKey == 3 ? 1 : 0)) * stride];
    };
    const f32* from = valueOf(key);
    const f32* to = last ? from : valueOf(key + 1);

    if (channel.interpolation == asset::AnimationChannel::Interpolation::Step || last) {
        for (usize lane = 0; lane < stride; ++lane)
            out[lane] = from[lane];
        return true;
    }

    if (channel.interpolation == asset::AnimationChannel::Interpolation::CubicSpline) {
        // Hermite between the two keys, its tangents scaled by the time
        // between them -- glTF's own definition -- and a rotation normalized
        // after, as the spec says a sampled quaternion is.
        const f32 span = channel.times[key + 1] - channel.times[key];
        const f32 s = fractionBetween(channel.times, key, time);
        const f32 s2 = s * s;
        const f32 s3 = s2 * s;
        const f32* leaving = &channel.values[(key * 3 + 2) * stride];
        const f32* arriving = &channel.values[((key + 1) * 3) * stride];
        f32 length = 0.0f;
        for (usize lane = 0; lane < stride; ++lane) {
            out[lane] = (2.0f * s3 - 3.0f * s2 + 1.0f) * from[lane] + (s3 - 2.0f * s2 + s) * span * leaving[lane] +
                        (-2.0f * s3 + 3.0f * s2) * to[lane] + (s3 - s2) * span * arriving[lane];
            length += out[lane] * out[lane];
        }
        if (stride == 4) {
            length = std::sqrt(length);
            if (length <= 0.0f) {
                out[0] = out[1] = out[2] = 0.0f;
                out[3] = 1.0f;
                return true;
            }
            for (usize lane = 0; lane < 4; ++lane)
                out[lane] /= length;
        }
        return true;
    }

    const f32 alpha = fractionBetween(channel.times, key, time);

    if (stride == 4) {
        // Two quaternions describe one rotation with opposite signs, and
        // interpolating q against -q takes the long way round -- which reads as
        // a joint spinning the wrong way for exactly one key.
        f32 dot = 0.0f;
        for (usize lane = 0; lane < 4; ++lane)
            dot += from[lane] * to[lane];
        const f32 sign = dot < 0.0f ? -1.0f : 1.0f;

        f32 length = 0.0f;
        for (usize lane = 0; lane < 4; ++lane) {
            out[lane] = from[lane] + (to[lane] * sign - from[lane]) * alpha;
            length += out[lane] * out[lane];
        }
        // Normalized linear rather than true slerp: at the key densities an
        // exported clip carries the angular error is below a tenth of a degree,
        // and a nlerp is branchless where a slerp has a small-angle case that
        // has to be got right. If a clip ever needs the difference, this is the
        // one function to change.
        length = std::sqrt(length);
        if (length <= 0.0f) {
            out[0] = out[1] = out[2] = 0.0f;
            out[3] = 1.0f;
            return true;
        }
        for (usize lane = 0; lane < 4; ++lane)
            out[lane] /= length;
        return true;
    }

    for (usize lane = 0; lane < stride; ++lane)
        out[lane] = from[lane] + (to[lane] - from[lane]) * alpha;
    return true;
}

// Translation, rotation and scale into one column-major matrix. Written out
// rather than composed from three matrix multiplies, because it is the inner
// loop of the pose and the multiplies would be forty-eight of the sixty-four
// products doing nothing.
// A rigid transform as a matrix. `toRenderMatrix` with no origin to subtract,
// which is what a JOINT wants: a bind pose is measured from the mesh's own
// origin and has nothing to do with where in the world the mesh stands.
[[nodiscard]] Mat4 toMatrix(const core::CFrameD& frame) noexcept
{
    return core::toRenderMatrix(frame, core::DVec3{});
}

[[nodiscard]] Mat4 composeTrs(const DVec3& translation, const f32* quaternion, Vec3 scale) noexcept
{
    const Mat3 rotation = core::fromQuaternion(quaternion[0], quaternion[1], quaternion[2], quaternion[3]);
    Mat4 result;
    for (int column = 0; column < 3; ++column) {
        const f32 axisScale = column == 0 ? scale.x : (column == 1 ? scale.y : scale.z);
        for (int row = 0; row < 3; ++row)
            result.m[column][row] = rotation.m[column][row] * axisScale;
        result.m[column][3] = 0.0f;
    }
    result.m[3][0] = static_cast<f32>(translation.x);
    result.m[3][1] = static_cast<f32>(translation.y);
    result.m[3][2] = static_cast<f32>(translation.z);
    result.m[3][3] = 1.0f;
    return result;
}

// a * b, the rotation that does `b` and then `a` -- the order two rotation
// matrices multiply in.
void multiplyQuaternions(const f32* a, const f32* b, f32* out) noexcept
{
    const f32 x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const f32 y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const f32 z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const f32 w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

void normalizeQuaternion(f32* q) noexcept
{
    const f32 length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (length <= 0.0f) {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    for (usize lane = 0; lane < 4; ++lane)
        q[lane] /= length;
}

// Whether a mask's entry names this joint: by the joint's own name.
[[nodiscard]] bool maskNames(const SkeletonLibrary::Entry& skeleton, u32 joint, std::string_view name) noexcept
{
    return skeleton.joints[joint].name == name;
}

[[nodiscard]] core::u64 keyOf(core::InstanceId id) noexcept
{
    return static_cast<core::u64>(id.index) | (static_cast<core::u64>(id.generation) << 32);
}

// The pose walk is index first, generation second. Its driver runs use that
// same order so one forward cursor replaces a binary search for each rig.
[[nodiscard]] core::u64 driverKeyOf(core::InstanceId id) noexcept
{
    return (static_cast<core::u64>(id.index) << 32) | id.generation;
}

// A tick that has at least this much of the fade's remaining time in it
// finishes the fade. The slack is what stops one tick's worth of remaining time
// taking two ticks to spend: fifteen subtractions of 1/60 from 0.25 leave
// 4.9e-17 behind, and a fade that never quite finishes is a track that never
// quite stops.
constexpr f64 FadeSlack = 1.0e-6;

} // namespace

void SkeletonLibrary::set(core::NameAtom content, Entry entry)
{
    const auto slot = std::lower_bound(entries_.begin(), entries_.end(), content,
                                       [](const Slot& lhs, core::NameAtom rhs) { return lhs.content.id < rhs.id; });
    if (slot != entries_.end() && slot->content == content) {
        slot->entry = std::move(entry);
        ++revision_;
        ++replaced_;
        return;
    }
    entries_.insert(slot, Slot{content, std::move(entry)});
    ++revision_;
}

void SkeletonLibrary::forget(core::NameAtom content)
{
    const auto slot = std::lower_bound(entries_.begin(), entries_.end(), content,
                                       [](const Slot& lhs, core::NameAtom rhs) { return lhs.content.id < rhs.id; });
    if (slot == entries_.end() || !(slot->content == content))
        return;
    entries_.erase(slot);
    ++revision_;
    ++replaced_;
}

void SkeletonLibrary::clear() noexcept
{
    ++revision_;
    ++replaced_;
    entries_.clear();
}

const SkeletonLibrary::Entry* SkeletonLibrary::find(core::NameAtom content) const noexcept
{
    const auto slot = std::lower_bound(entries_.begin(), entries_.end(), content,
                                       [](const Slot& lhs, core::NameAtom rhs) { return lhs.content.id < rhs.id; });
    if (slot == entries_.end() || !(slot->content == content))
        return nullptr;
    return &slot->entry;
}

void GraphLibrary::set(core::NameAtom content, asset::AnimationGraph graph, core::AtomTable& atoms)
{
    auto entry = std::make_unique<Entry>();
    // Which file and which clip each use means: a name with a `#` says its
    // own file, and a plain one is in the graph's library -- or, with none,
    // in the mesh the player drives.
    const core::NameAtom library = graph.library.empty() ? core::NameAtom{} : atoms.intern(graph.library);
    for (const asset::GraphClip& clip : graph.clips) {
        const std::string_view name = clip.clip;
        if (const usize hash = name.rfind('#'); hash != std::string_view::npos) {
            entry->clipFiles.push_back(hash == 0 ? library : atoms.intern(name.substr(0, hash)));
            entry->clipNames.emplace_back(name.substr(hash + 1));
        }
        else {
            entry->clipFiles.push_back(library);
            entry->clipNames.emplace_back(name);
        }
    }
    for (const asset::GraphParameter& parameter : graph.parameters) {
        const std::string_view from = parameter.from;
        Source source = Source::None;
        core::NameAtom attribute;
        if (from.starts_with("Attribute.")) {
            source = Source::Attribute;
            attribute = atoms.intern(from.substr(std::string_view("Attribute.").size()));
        }
        else if (from == "CharacterBody.Speed")
            source = Source::Speed;
        else if (from == "CharacterBody.VerticalSpeed")
            source = Source::VerticalSpeed;
        else if (from == "CharacterBody.MoveX")
            source = Source::MoveX;
        else if (from == "CharacterBody.MoveZ")
            source = Source::MoveZ;
        else if (from == "CharacterBody.Grounded")
            source = Source::Grounded;
        else if (from == "CharacterBody.State")
            source = Source::State;
        entry->sources.push_back(source);
        entry->attributes.push_back(attribute);
    }
    entry->graph = std::move(graph);

    const auto slot = std::lower_bound(entries_.begin(), entries_.end(), content,
                                       [](const Slot& lhs, core::NameAtom rhs) { return lhs.content.id < rhs.id; });
    ++revision_;
    if (slot != entries_.end() && slot->content == content) {
        slot->entry = std::move(entry);
        return;
    }
    entries_.insert(slot, Slot{content, std::move(entry)});
}

void GraphLibrary::forget(core::NameAtom content)
{
    const auto slot = std::lower_bound(entries_.begin(), entries_.end(), content,
                                       [](const Slot& lhs, core::NameAtom rhs) { return lhs.content.id < rhs.id; });
    if (slot == entries_.end() || !(slot->content == content))
        return;
    entries_.erase(slot);
    ++revision_;
}

void GraphLibrary::clear() noexcept
{
    ++revision_;
    entries_.clear();
}

const GraphLibrary::Entry* GraphLibrary::find(core::NameAtom content) const noexcept
{
    const auto slot = std::lower_bound(entries_.begin(), entries_.end(), content,
                                       [](const Slot& lhs, core::NameAtom rhs) { return lhs.content.id < rhs.id; });
    if (slot == entries_.end() || !(slot->content == content))
        return nullptr;
    return slot->entry.get();
}

AnimationSystem::AnimationSystem(const scene::World& world, const SkeletonLibrary& skeletons)
    : world_(&world), skeletons_(&skeletons)
{}

scene::TrackId AnimationSystem::createTrack(core::InstanceId player, core::NameAtom content, std::string_view clip)
{
    Track track;
    track.player = player;

    // The skeleton comes from the player's PARENT when that parent is a
    // `MeshPart` -- the older shape, and the one a character made of one mesh
    // uses.
    //
    // **When it is not, the parent is a drive root and every skinned mesh under
    // it is driven by this one track.** A character is a body, a shirt and a
    // pair of trousers: several skinned meshes wearing the same skeleton, which
    // one clip has to move together. Parent the player to the `Model`.
    //
    // The clip is sourced from the FIRST skinned mesh under the root that has
    // one, in tree order -- so it does not matter which of the pieces the artist
    // exported the animation with, and it is the same choice on every machine.
    track.meshPart = world_->parentOf(player);
    if (world_->meshParts().find(track.meshPart) == nullptr && track.meshPart.valid()) {
        track.driveRoot = track.meshPart;
        track.meshPart = clipSourceUnder(track.driveRoot);
    }
    track.clipFrom = content;
    track.clipName = std::string(clip);
    (void)bindTrack(track);

    tracks_.push_back(track);
    return static_cast<scene::TrackId>(tracks_.size() - 1);
}

bool AnimationSystem::bindTrack(Track& track) const
{
    if (track.clip != NoClip)
        return true;
    // Under a drive root the clip's mesh is the first skinned one that carries
    // clips -- none, before any of them has loaded.
    if (track.driveRoot.valid() && world_->meshParts().find(track.meshPart) == nullptr)
        track.meshPart = clipSourceUnder(track.driveRoot);
    const scene::MeshPartComponent* mesh = world_->meshParts().find(track.meshPart);
    if (mesh == nullptr)
        return false;
    // **The clip's own file when one was named, and the mesh's otherwise**
    // (S6.8). `track.content` is what the sampler reads the clip out of, and
    // `jointMapFor` already maps that rig's joints onto whichever mesh it is
    // driving -- so a clip from elsewhere is retargeted by the same code that
    // drives a shirt from a body's skeleton.
    track.content = track.clipFrom.valid() ? track.clipFrom : mesh->meshContent;
    const SkeletonLibrary::Entry* entry = skeletons_->find(track.content);
    if (entry == nullptr)
        return false;
    for (u32 index = 0; index < entry->clips.size(); ++index) {
        if (track.clipName.empty() || entry->clips[index].name == track.clipName) {
            track.clip = index;
            track.length = entry->clips[index].duration;
            return true;
        }
    }
    return false;
}

void AnimationSystem::play(scene::TrackId id, f32 fadeTime, f32 weight, f32 speed)
{
    if (id == 0 || id >= tracks_.size())
        return;
    Track& track = tracks_[id];
    // From the beginning, every time. The opposite of `Tween:Play`, and
    // deliberately: a jump animation triggered twice should play twice.
    // **From where the fade is, not from nothing** (D438): a `Play` that
    // lands while a `Stop` is still fading out carries on from that weight,
    // and the fade-out is over. A track that was not playing fades in from
    // zero.
    const f32 from = track.playing ? track.weight : 0.0f;
    track.time = 0.0;
    track.speed = speed;
    track.ownWeight = weight;
    track.targetWeight = weight;
    track.playing = true;
    track.holding = false;
    track.stopping = false;
    if (fadeTime > 0.0f) {
        track.weight = from;
        track.fadeRemaining = static_cast<f64>(fadeTime);
    }
    else {
        track.weight = weight;
        track.fadeRemaining = 0.0;
    }
}

void AnimationSystem::stop(scene::TrackId id, f32 fadeTime)
{
    if (id == 0 || id >= tracks_.size())
        return;
    Track& track = tracks_[id];
    // The fade goes to nothing; what the script set stays what it set.
    track.targetWeight = 0.0f;
    if (fadeTime > 0.0f && track.playing) {
        track.fadeRemaining = static_cast<f64>(fadeTime);
        track.stopping = true;
        return;
    }
    track.weight = 0.0f;
    track.fadeRemaining = 0.0;
    track.playing = false;
    track.holding = false;
    track.stopping = false;
}

void AnimationSystem::adjustWeight(scene::TrackId id, f32 weight, f32 fadeTime)
{
    if (id == 0 || id >= tracks_.size())
        return;
    Track& track = tracks_[id];
    track.ownWeight = weight;
    // A track that is not playing, or is on its way out, keeps the number for
    // its next `Play` and is not brought back by it.
    if (track.stopping || (!track.playing && !track.holding))
        return;
    track.targetWeight = weight;
    if (fadeTime > 0.0f) {
        track.fadeRemaining = static_cast<f64>(fadeTime);
        return;
    }
    track.weight = weight;
    track.fadeRemaining = 0.0;
}

void AnimationSystem::adjustSpeed(scene::TrackId id, f32 speed)
{
    if (id == 0 || id >= tracks_.size())
        return;
    tracks_[id].speed = speed;
}

void AnimationSystem::setLooped(scene::TrackId id, bool looped)
{
    if (id == 0 || id >= tracks_.size())
        return;
    tracks_[id].looped = looped;
}

scene::TrackState AnimationSystem::state(scene::TrackId id) const
{
    scene::TrackState out;
    if (id == 0 || id >= tracks_.size())
        return out;
    const Track& track = tracks_[id];
    out.timePosition = track.time;
    out.length = track.length;
    out.speed = track.speed;
    out.weight = track.ownWeight;
    out.blend = track.playing || track.holding ? track.weight : 0.0f;
    out.looped = track.looped;
    out.playing = track.playing;
    return out;
}

void AnimationSystem::sample(f64 fixedDt)
{
    core::profile::Sections sections;
    ENG_PROFILE_NEXT(sections, "animation.tracks");
    // Every skinned mesh with at least one live track under it, collected first
    // so the pose walk is one pass per mesh rather than one per track.
    meshes_.clear();

    // Collected, then sorted and made unique below: a search per note was a
    // pass over every mesh for every track, quadratic in a crowd.
    const auto note = [this](core::InstanceId mesh) {
        if (!mesh.valid())
            return;
        if (mesh.index >= meshMarks_.size())
            meshMarks_.resize(static_cast<usize>(mesh.index) + 1);
        MeshMark& mark = meshMarks_[mesh.index];
        const core::u64 stamp = sampled_ + 1;
        if (mark.stamp == stamp && mark.generation == mesh.generation)
            return;
        mark = MeshMark{stamp, mesh.generation};
        meshes_.push_back(mesh);
    };
    // A track whose player says `AlwaysAnimate` (H3): its meshes are posed
    // every tick, seen or not.
    always_.clear();
    const auto alwaysFor = [this](const Track& track) {
        const scene::AnimationPlayerComponent* player = world_->animationPlayers().find(track.player);
        return player != nullptr && player->cullingMode == 1;
    };
    // What skipped a pose it was due: noted again, so it catches up.
    for (const core::InstanceId mesh : skipped_)
        note(mesh);
    skipped_.clear();
    // What a ragdoll drove last tick (`commitOverrides`) is posed every tick.
    for (const core::InstanceId mesh : overridden_)
        always_.push_back(mesh);

    // **A rig that was read again is another rig** (D611): a model exported
    // anew may have its clips in another order, another length, its joints
    // another count. What every track found in its rig, by index, is found
    // again by name; what it had playing goes on from the time it was at;
    // and every pose is built again, from the joints there are now.
    if (skeletons_->replaced() != replacedSeen_) {
        replacedSeen_ = skeletons_->replaced();
        for (usize index = 1; index < tracks_.size(); ++index) {
            Track& track = tracks_[index];
            if (!track.alive)
                continue;
            track.clip = NoClip;
            track.posed = false;
            if (bindTrack(track) && track.time > static_cast<f64>(track.length))
                track.time = static_cast<f64>(track.length);
        }
        poses_.clear();
        presented_.clear();
        shared_.clear();
        sharedJoints_ = 0;
        keyPeriods_.clear();
    }

    // The graphs first (ADR 0197): each steps its states and writes its
    // tracks' times and weights, which the walk below then takes like any.
    stepGraphs(fixedDt);

    wantedFiles_.clear();
    for (usize index = 1; index < tracks_.size(); ++index) {
        Track& track = tracks_[index];
        // **A track made before its file arrived binds when it does** (D509):
        // its length becomes the clip's, and what it was told -- Play, a speed,
        // a weight -- takes effect from the clip's beginning. Until then it
        // drives nothing and is not a reason to build a pose; a script reading
        // it sees a track rather than a hole.
        if (!track.alive)
            continue;
        if (!bindTrack(track)) {
            // Its clip is in a file nothing has loaded: asked for, so a file
            // that only holds clips is read though no mesh wears it.
            if (track.clipFrom.valid() && skeletons_->find(track.clipFrom) == nullptr)
                wantedFiles_.push_back(track.clipFrom);
            continue;
        }
        // Its player, looked up once: whether its meshes are posed every
        // tick, and how its clips are carried onto them.
        if (const scene::AnimationPlayerComponent* const player = world_->animationPlayers().find(track.player);
            player != nullptr) {
            if (player->cullingMode == 1)
                always_.push_back(track.meshPart);
            track.retargeting = static_cast<core::u8>(player->retargeting);
        }
        if (!track.playing) {
            if (!quiet(track))
                note(track.meshPart);
            continue;
        }
        // A graph's track was stepped by its graph: its time and weight are
        // this tick's already, and it never ends of its own accord.
        if (track.graph != NoGraph) {
            note(track.meshPart);
            continue;
        }

        if (track.fadeRemaining > 0.0) {
            if (track.fadeRemaining <= fixedDt * (1.0 + FadeSlack)) {
                track.weight = track.targetWeight;
                track.fadeRemaining = 0.0;
                // A fade to zero is a stop that took a moment, which is what
                // `Stop(fadeTime)` means.
                if (track.targetWeight <= 0.0f) {
                    track.playing = false;
                    track.holding = false;
                    track.stopping = false;
                    note(track.meshPart);
                    continue;
                }
            }
            else {
                // Towards the target by the fraction of the remaining time this
                // tick is, which lands exactly on the target whatever
                // `AdjustWeight` did to it partway.
                track.weight += (track.targetWeight - track.weight) * static_cast<f32>(fixedDt / track.fadeRemaining);
                track.fadeRemaining -= fixedDt;
            }
        }

        track.time += fixedDt * static_cast<f64>(track.speed);
        if (track.length > 0.0f && track.time >= static_cast<f64>(track.length)) {
            if (track.looped) {
                // Wrapped rather than reset, so a loop does not lose the
                // fraction of a tick it overshot by -- over a minute that is a
                // loop drifting against everything else in the scene.
                track.time = std::fmod(track.time, static_cast<f64>(track.length));
            }
            else {
                track.time = static_cast<f64>(track.length);
                track.playing = false;
                track.holding = true;
                ended_.push_back(static_cast<scene::TrackId>(index));
            }
        }

        note(track.meshPart);
    }

    std::sort(wantedFiles_.begin(), wantedFiles_.end(), [](core::NameAtom a, core::NameAtom b) { return a.id < b.id; });
    wantedFiles_.erase(std::unique(wantedFiles_.begin(), wantedFiles_.end()), wantedFiles_.end());

    // **Every mesh a drive root covers, not just the one the clip came from.**
    // `note(track.meshPart)` above collects the mesh the track was MADE against,
    // which for a player parented to a `Model` is whichever piece carried the
    // animation. The shirt is driven by the same track and would otherwise never
    // have its pose rebuilt -- a body that walks and a shirt that stands still.
    // And which tracks drive each mesh (H10): a track's own mesh, and every
    // skinned mesh under its drive root -- found here, once a tick, rather than
    // asked of every track by every pose.
    drivers_.clear();
    ENG_PROFILE_NEXT(sections, "animation.drivers");
    rootDrivers_.clear();
    for (usize index = 1; index < tracks_.size(); ++index) {
        const Track& track = tracks_[index];
        if (!track.alive || track.clip == NoClip)
            continue;
        const bool contributes = (track.playing || track.holding) && track.weight > 0.0f;
        if (contributes && track.meshPart.valid())
            drivers_.emplace_back(driverKeyOf(track.meshPart), static_cast<u32>(index));
        const bool noting = !quiet(track);
        if (!track.driveRoot.valid() || (!noting && !contributes))
            continue;
        rootDrivers_.emplace_back(keyOf(track.driveRoot), static_cast<u32>(index));
    }
    std::sort(rootDrivers_.begin(), rootDrivers_.end());
    core::InstanceId lastRoot;
    for (const auto& driver : rootDrivers_) {
        const u32 index = driver.second;
        const Track& track = tracks_[index];
        if (track.driveRoot != lastRoot) {
            lastRoot = track.driveRoot;
            descendants_.clear();
            driveMeshes_.clear();
            world_->collectDescendants(lastRoot, descendants_);
            for (const core::InstanceId id : descendants_) {
                const scene::MeshPartComponent* mesh = world_->meshParts().find(id);
                if (mesh == nullptr)
                    continue;
                const SkeletonLibrary::Entry* entry = skeletons_->find(mesh->meshContent);
                if (entry != nullptr && !entry->joints.empty())
                    driveMeshes_.push_back(id);
            }
        }
        const bool contributes = (track.playing || track.holding) && track.weight > 0.0f;
        const bool noting = !quiet(track);
        const bool always = alwaysFor(track);
        for (const core::InstanceId id : driveMeshes_) {
            if (noting)
                note(id);
            if (always)
                always_.push_back(id);
            // The track's own mesh was already indexed above. Avoid sorting
            // a second copy of that pair for every track in a crowd.
            if (contributes && id != track.meshPart)
                drivers_.emplace_back(driverKeyOf(id), index);
        }
    }
    std::sort(drivers_.begin(), drivers_.end());
    drivers_.erase(std::unique(drivers_.begin(), drivers_.end()), drivers_.end());

    // **And every mesh a `Bone` turns** (G9): `Transform` is an offset on the
    // pose, and a mesh nothing plays has a pose to turn as much as one that
    // walks.
    // The ones turned last tick too, once more: a bone set back to the
    // identity leaves a pose to take away.
    for (const core::InstanceId mesh : turned_) {
        if (world_->alive(mesh))
            note(mesh);
    }
    turned_.clear();
    boned_.clear();
    ENG_PROFILE_NEXT(sections, "animation.bones");
    world_->attachments().forEach([&](core::InstanceId id, const scene::AttachmentComponent& bone) {
        if (bone.jointIndex < 0)
            return;
        const core::InstanceId rig = rigOf(id);
        // **A rig a `Bone` hangs from is posed every tick** (H3): what hangs
        // there -- a sword, a hitbox, a camera -- follows the joint, and is
        // gameplay as much as it is a picture.
        if (rig.valid()) {
            always_.push_back(rig);
            boned_.push_back(rig);
        }
        if (!(bone.transform == core::CFrameD{})) {
            note(rig);
            if (rig.valid() && std::find(turned_.begin(), turned_.end(), rig) == turned_.end())
                turned_.push_back(rig);
        }
    });

    // A mesh with overrides and no track is deliberately NOT collected here.
    // It was, at first, on the reasoning that a limp ragdoll has no track and
    // so would never be visited -- but there is nothing to rebuild for it:
    // `commitOverrides` builds a pose from the rest chain when it finds none,
    // and a mesh no clip drives has no clip to rebuild from. Adding the visit
    // changed no observable behaviour, so it is not here.
    // In id order: what a pose is does not depend on which is built first,
    // and a sorted list is the same list on every run (R10).
    ENG_PROFILE_NEXT(sections, "animation.prepare");
    std::sort(meshes_.begin(), meshes_.end(), [](core::InstanceId a, core::InstanceId b) {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    });
    meshes_.erase(std::unique(meshes_.begin(), meshes_.end()), meshes_.end());
    const auto byId = [](core::InstanceId a, core::InstanceId b) {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    };
    std::sort(always_.begin(), always_.end(), byId);
    std::sort(boned_.begin(), boned_.end(), byId);
    boned_.erase(std::unique(boned_.begin(), boned_.end()), boned_.end());
    ++sampled_;
    // The shared poses: all of them when a rig or a clip changed, and every
    // so often the ones nothing has copied for half a second.
    if (skeletons_->revision() != sharedRevision_) {
        shared_.clear();
        sharedJoints_ = 0;
        sharedRevision_ = skeletons_->revision();
    }
    // **Let go a little a tick, never all at once.** The index was swept
    // whole every sixty-fourth tick and emptied whole when it was full: with
    // thousands of poses in it, each some arrays to free, that was a tick of
    // five milliseconds every so often. A sixty-fourth of its buckets a tick
    // instead, so every entry is still looked at once in sixty-four.
    if (const usize buckets = shared_.bucket_count(); buckets > 0 && !shared_.empty()) {
        const usize span = buckets / 64 + 1;
        expired_.clear();
        for (usize step = 0; step < span; ++step) {
            const usize bucket = (sharedSweep_ + step) % buckets;
            for (auto entry = shared_.begin(bucket); entry != shared_.end(bucket); ++entry) {
                if (sampled_ - entry->second.used > SharedPoseTicks)
                    expired_.push_back(&entry->first);
            }
        }
        sharedSweep_ = (sharedSweep_ + span) % buckets;
        for (const std::vector<core::u64>* signature : expired_) {
            if (const auto gone = shared_.find(*signature); gone != shared_.end()) {
                sharedJoints_ -= std::min(sharedJoints_, gone->second.pose->palette.size());
                shared_.erase(gone);
            }
        }
    }
    ENG_PROFILE_NEXT(sections, "animation.poses");
    auto driver = drivers_.begin();
    for (const core::InstanceId meshPart : meshes_) {
        const scene::MeshPartComponent* mesh = world_->meshParts().find(meshPart);
        if (mesh == nullptr)
            continue;
        const SkeletonLibrary::Entry* entry = skeletons_->find(mesh->meshContent);
        // No rig, no pose: a mesh with morph targets alone has an entry for
        // its targets and its clips (ADR 0196).
        if (entry == nullptr || entry->joints.empty())
            continue;
        // **Posed as often as it is seen** (H3): not at all where neither the
        // camera nor a shadow reached it last frame, and every second, fourth
        // or eighth tick as it gets small -- staggered by id, so a crowd's
        // poses are spread over the ticks. Its clips keep time regardless, and
        // what skipped is carried to the next tick, so the pose it gets is the
        // one it would have had.
        bool reduced = false;
        if (seeing_ && !std::binary_search(always_.begin(), always_.end(), meshPart, byId)) {
            const auto seen = seen_.find(keyOf(meshPart));
            const core::u32 interval = seen == seen_.end() ? 0u : updateInterval(seen->second * detail_);
            if (interval == 0 || (sampled_ + meshPart.index) % interval != 0) {
                skipped_.push_back(meshPart);
                stale_[keyOf(meshPart)] = true;
                continue;
            }
            reduced = interval > 1;
        }
        // This mesh's drivers, in track order: the sorted index's run of it.
        const core::u64 key = keyOf(meshPart);
        const core::u64 driverKey = driverKeyOf(meshPart);
        while (driver != drivers_.end() && driver->first < driverKey)
            ++driver;
        driving_.clear();
        while (driver != drivers_.end() && driver->first == driverKey) {
            driving_.push_back(driver->second);
            ++driver;
        }
        rebuildPose(meshPart, *entry, driving_, true, reduced);
        stale_.erase(key);
    }

    ENG_PROFILE_NEXT(sections, "animation.history");
    // Every track is now as the poses took it in.
    for (usize index = 1; index < tracks_.size(); ++index) {
        Track& track = tracks_[index];
        track.posed = true;
        track.posedPlaying = track.playing;
        track.posedHolding = track.holding;
        track.posedWeight = track.weight;
        track.posedTime = track.time;
        track.posedLayerWeight = track.layerWeight;
    }
}

f32 AnimationSystem::sampleTime(const Track& track, bool quantise)
{
    const auto exact = static_cast<f32>(track.time);
    if (!quantise)
        return exact;
    // The clip's key period: the shortest gap between two keys of its first
    // channel -- the rate it was baked at.
    const core::u64 key = (static_cast<core::u64>(track.content.id) << 32) | track.clip;
    auto found = keyPeriods_.find(key);
    if (found == keyPeriods_.end()) {
        f32 period = 0.0f;
        if (const SkeletonLibrary::Entry* source = skeletons_->find(track.content);
            source != nullptr && track.clip < source->clips.size() && !source->clips[track.clip].channels.empty()) {
            const std::vector<f32>& times = source->clips[track.clip].channels.front().times;
            for (usize index = 1; index < times.size(); ++index) {
                const f32 gap = times[index] - times[index - 1];
                if (gap > 0.0f && (period == 0.0f || gap < period))
                    period = gap;
            }
        }
        found = keyPeriods_.emplace(key, period).first;
    }
    const f32 period = found->second;
    return period > 0.0f ? std::floor(exact / period) * period : exact;
}

void AnimationSystem::rebuildPose(core::InstanceId meshPart, const SkeletonLibrary::Entry& skeleton,
                                  std::span<const u32> drivers, bool indexed, bool quantise)
{
    ENG_PROFILE_SCOPE("animation.pose");
    const usize jointCount = skeleton.joints.size();
    if (jointCount == 0)
        return;

    // Which rig this is, so a clip arriving from ANOTHER one can be remapped
    // onto it by joint name.
    const scene::MeshPartComponent* meshComponent = world_->meshParts().find(meshPart);
    const core::NameAtom content = meshComponent != nullptr ? meshComponent->meshContent : core::NameAtom{};

    // What the mesh's bones turn (G9), by joint: `Bone.Transform` in the
    // joint's own space, after whatever the clips did. **Asked only of a rig
    // that has a bone** (`boned_`, this tick's): every body of a crowd had its
    // children walked for bones it does not have, and two lists made to say
    // so. Outside a tick the list is the last tick's, and the walk is made.
    std::vector<std::pair<u32, Mat4>> offsets;
    if (!indexed ||
        std::binary_search(boned_.begin(), boned_.end(), meshPart, [](core::InstanceId a, core::InstanceId b) {
            return a.index != b.index ? a.index < b.index : a.generation < b.generation;
        }))
        offsets = boneOffsets(meshPart, jointCount);

    // **The same inputs, the same pose** (H10): the rig, and each driving
    // track's clip, time and weight. A crowd of one rig walking one clip from
    // one moment is one pose built and the rest copied -- exactly what each
    // would have computed. Not with a bone turning it or a ragdoll holding
    // it: those are this mesh's own.
    signature_.clear();
    const bool shareable = indexed && offsets.empty() && overridesFor(meshPart) == nullptr;
    if (shareable) {
        signature_.push_back(content.id);
        for (const u32 index : drivers) {
            const Track& track = tracks_[index];
            if (!track.alive || !(track.playing || track.holding) || track.clip == NoClip || track.weight <= 0.0f)
                continue;
            signature_.push_back((static_cast<core::u64>(track.content.id) << 32) | track.clip);
            signature_.push_back((static_cast<core::u64>(std::bit_cast<u32>(sampleTime(track, quantise))) << 32) |
                                 std::bit_cast<u32>(track.weight));
            // And its layer, whether it adds, its mask and its layer's
            // weight (ADR 0197): two bodies in one state of one graph at one
            // moment have these alike, and still share.
            signature_.push_back(layerWordOf(track, content));
        }
        if (signature_.size() > 1) {
            if (const auto same = shared_.find(signature_); same != shared_.end()) {
                same->second.used = sampled_;
                // The pose itself, held by one more: not a copy of it.
                poses_[keyOf(meshPart)] = same->second.pose;
                ++posesShared_;
                return;
            }
        }
    }
    ++posesBuilt_;

    // Accumulators, one currency per component. A weighted average per joint
    // rather than per track, because a joint no clip drives has to keep its rest
    // transform -- a zero-weight average would collapse it to the origin, which
    // is what makes a clip that animates one arm eat the other.
    base_.clear(jointCount, false);

    bool contributed = false;

    // **Track index order, which is load order.** R10 forbids the order coming
    // out of a container that does not promise one, and two tracks at weight
    // 0.5 have to blend the same way on every run. The tick's index of them
    // is in that order; asked outside a tick, every track is.
    const usize considered = indexed ? drivers.size() : tracks_.size() - 1;
    const auto driving = [&](usize position) -> const Track* {
        const usize index = indexed ? drivers[position] : position + 1;
        const Track& track = tracks_[index];
        if (!track.alive || !(track.playing || track.holding) || track.clip == NoClip)
            return nullptr;
        // Every track made against THIS mesh, and every track whose drive root
        // this mesh is under. Two players under one mesh are two sources
        // blending into one pose, which is what they look like on screen; one
        // player over a body and a shirt is one source moving both.
        if (!indexed && !drives(track, meshPart))
            return nullptr;
        return track.weight > 0.0f ? &track : nullptr;
    };

    // **The first layer: one weighted average** -- every track a script plays
    // and a graph's first layer, mixed as two tracks always were. What is
    // above it and what adds are counted on the way and walked after.
    core::u16 topLayer = 0;
    bool adds = false;
    // How much of a graph's first layer is clips: the rest of it is a state
    // with none, which is the rest pose and has to weigh what it weighs --
    // or a fade in from nothing would arrive whole on its first tick.
    bool graphBase = false;
    f32 graphShare = 0.0f;
    for (usize position = 0; position < considered; ++position) {
        const Track* const track = driving(position);
        if (track == nullptr)
            continue;
        if (track->additive) {
            adds = true;
            continue;
        }
        if (track->layer > 0) {
            topLayer = std::max(topLayer, track->layer);
            continue;
        }
        if (!accumulate(*track, content, jointCount, quantise, base_))
            continue;
        contributed = true;
        if (track->graph != NoGraph) {
            graphBase = true;
            graphShare += track->weight * track->layerWeight;
        }
    }
    if (graphBase && graphShare < 1.0f - 1.0e-4f) {
        const f32 rest = 1.0f - graphShare;
        f32 quaternion[4]{};
        for (usize joint = 0; joint < jointCount; ++joint) {
            const asset::Joint& bone = skeleton.joints[joint];
            base_.translation[joint].x += bone.localBind.position.x * static_cast<f64>(rest);
            base_.translation[joint].y += bone.localBind.position.y * static_cast<f64>(rest);
            base_.translation[joint].z += bone.localBind.position.z * static_cast<f64>(rest);
            base_.weightT[joint] += rest;
            core::toQuaternion(bone.localBind.rotation, quaternion[0], quaternion[1], quaternion[2], quaternion[3]);
            f32* accumulator = &base_.rotation[joint * 4];
            f32 dot = 0.0f;
            for (usize lane = 0; lane < 4; ++lane)
                dot += accumulator[lane] * quaternion[lane];
            const f32 sign = (base_.weightR[joint] > 0.0f && dot < 0.0f) ? -1.0f : 1.0f;
            for (usize lane = 0; lane < 4; ++lane)
                accumulator[lane] += quaternion[lane] * sign * rest;
            base_.weightR[joint] += rest;
            base_.scale[joint] = base_.scale[joint] + Vec3{rest, rest, rest};
            base_.weightS[joint] += rest;
        }
    }

    // **Each layer above replaces** (ADR 0197): where it has a joint it takes
    // its cover of it, and what is under keeps the rest -- a cross-fade
    // written as weights, a layer at a time, bottom first.
    for (core::u16 layer = 1; layer <= topLayer; ++layer) {
        upper_.clear(jointCount, true);
        bool any = false;
        for (usize position = 0; position < considered; ++position) {
            const Track* const track = driving(position);
            if (track != nullptr && !track->additive && track->layer == layer)
                any = accumulate(*track, content, jointCount, quantise, upper_) || any;
        }
        if (any) {
            mergeLayer(skeleton, base_, upper_);
            contributed = true;
        }
    }

    // **And what adds, after the average**: each clip as how far it is from
    // its own first frame.
    if (adds) {
        addT_.assign(jointCount, DVec3{});
        addR_.assign(jointCount * 4, 0.0f);
        for (usize joint = 0; joint < jointCount; ++joint)
            addR_[joint * 4 + 3] = 1.0f;
        addS_.assign(jointCount, Vec3{1.0f, 1.0f, 1.0f});
        bool any = false;
        for (usize position = 0; position < considered; ++position) {
            const Track* const track = driving(position);
            if (track != nullptr && track->additive)
                any = accumulateAdditive(*track, content, jointCount, quantise) || any;
        }
        adds = any;
        contributed = contributed || any;
    }

    if (!contributed && offsets.empty()) {
        // Nothing drives this player any more. Its pose is taken away rather
        // than left holding the last thing that did -- a null pose means "bind
        // pose", and a stale palette would freeze the character mid-stride.
        //
        // **Unless something is overriding it**, and then the pose is left
        // exactly as it is. Not merely un-erased: falling through would rebuild
        // it from accumulators that are all zero weight, which IS the rest pose
        // -- the first version of this did that and lost the very thing it was
        // written to keep. What it keeps is the LOCALS. A ragdoll simulates a
        // dozen bones; the fingers it does not simulate ride on their own local
        // from this pose, so rebuilding from rest snaps every unsimulated joint
        // out of the animation it was in on the exact frame the character goes
        // limp -- a hand that springs open as the body drops.
        if (overridesFor(meshPart) == nullptr)
            poses_.erase(keyOf(meshPart));
        return;
    }

    Pose& pose = ownPose(meshPart, false);
    pose.palette.assign(jointCount, Mat4{});
    // Kept rather than thrown away. `model` is what a socket asks for and
    // `local` is what an override needs to re-run the forward pass -- both were
    // already being computed into a scratch that ended at the closing brace.
    pose.model.assign(jointCount, Mat4{});
    pose.local.assign(jointCount, Mat4{});

    f32 restRotation[4]{};
    for (usize joint = 0; joint < jointCount; ++joint) {
        const asset::Joint& bone = skeleton.joints[joint];

        DVec3 translation = bone.localBind.position;
        if (base_.weightT[joint] > 0.0f) {
            const f64 inverse = 1.0 / static_cast<f64>(base_.weightT[joint]);
            translation = DVec3{base_.translation[joint].x * inverse, base_.translation[joint].y * inverse,
                                base_.translation[joint].z * inverse};
        }

        Vec3 boneScale{1.0f, 1.0f, 1.0f};
        if (base_.weightS[joint] > 0.0f) {
            const f32 inverse = 1.0f / base_.weightS[joint];
            boneScale =
                Vec3{base_.scale[joint].x * inverse, base_.scale[joint].y * inverse, base_.scale[joint].z * inverse};
        }

        const f32* quaternion = restRotation;
        if (base_.weightR[joint] > 0.0f) {
            f32* accumulator = &base_.rotation[joint * 4];
            f32 length = 0.0f;
            for (usize lane = 0; lane < 4; ++lane)
                length += accumulator[lane] * accumulator[lane];
            length = std::sqrt(length);
            if (length > 0.0f) {
                for (usize lane = 0; lane < 4; ++lane)
                    accumulator[lane] /= length;
                quaternion = accumulator;
            }
        }
        if (quaternion == restRotation) {
            // The rest rotation, back as a quaternion so the accumulator has one
            // currency. `CFrameD` stores a basis and reading a quaternion out of
            // it is cheaper than carrying a second representation on `Joint`.
            core::toQuaternion(bone.localBind.rotation, restRotation[0], restRotation[1], restRotation[2],
                               restRotation[3]);
        }

        f32 added[4]{};
        if (adds) {
            // What the additive tracks made of this joint, on top: a turn in
            // the joint's own space, a move from its parent, a stretch.
            translation = translation + addT_[joint];
            multiplyQuaternions(quaternion, &addR_[joint * 4], added);
            normalizeQuaternion(added);
            quaternion = added;
            boneScale = core::mul(boneScale, addS_[joint]);
        }

        Mat4 local = composeTrs(translation, quaternion, boneScale);
        for (const auto& [turned, offset] : offsets) {
            if (turned == joint)
                local = local * offset;
        }
        pose.local[joint] = local;
        // One forward pass, parents first -- which the loader guarantees by
        // sorting the joints (asset/model.h). A graph walk per frame would be
        // the alternative, and this is the whole reason it is not needed.
        pose.model[joint] = bone.parent == asset::Joint::NoParent ? local : pose.model[bone.parent] * local;
        pose.palette[joint] = pose.model[joint] * bone.inverseBind;
    }
    if (shareable && signature_.size() > 1) {
        // Bounded, and full is full: a pose there is no room for is this
        // mesh's alone, and built again by whoever comes to its moment. It
        // was emptied instead -- every pose of a crowd built again at once,
        // and thousands of them freed in the same tick.
        // Held by the crowd's index too from here: whoever writes to this
        // mesh's pose next takes one of its own.
        if (shared_.size() < MostSharedPoses && sharedJoints_ + jointCount <= MostSharedJoints) {
            // Its buckets made once, for all it may hold: grown as it filled,
            // every doubling was every entry filed again inside one tick.
            if (shared_.empty())
                shared_.reserve(MostSharedPoses);
            if (shared_.insert_or_assign(signature_, SharedPose{poses_[keyOf(meshPart)], sampled_}).second)
                sharedJoints_ += jointCount;
        }
    }
}

void AnimationSystem::Lanes::clear(usize joints, bool cover)
{
    translation.assign(joints, DVec3{});
    rotation.assign(joints * 4, 0.0f);
    scale.assign(joints, Vec3{});
    weightT.assign(joints, 0.0f);
    weightR.assign(joints, 0.0f);
    weightS.assign(joints, 0.0f);
    if (cover) {
        coverT.assign(joints, 0.0f);
        coverR.assign(joints, 0.0f);
        coverS.assign(joints, 0.0f);
    }
    else {
        coverT.clear();
        coverR.clear();
        coverS.clear();
    }
}

bool AnimationSystem::accumulate(const Track& track, core::NameAtom rig, usize jointCount, bool quantise, Lanes& lanes)
{
    // **The clip comes from the track's OWN rig, not from this mesh's.** A
    // shirt exported without the animation has no clips of its own, and the
    // whole point of one player over several meshes is that only one of them
    // needs to carry it.
    const SkeletonLibrary::Entry* source = skeletons_->find(track.content);
    if (source == nullptr || track.clip >= source->clips.size())
        return false;

    // Null when the clip is being applied to the rig it came from, which is
    // every character made of one mesh -- and then the channel's own index
    // is used, exactly as before.
    const JointMap* const map = jointMapFor(track.content, rig, track.retargeting);
    const std::vector<f32>* const mask = maskFor(track, rig);
    // In the first layer a layer's weight is the track's; above it, it is
    // how much of the joint the layer covers.
    const bool covering = !lanes.coverT.empty();
    const f32 own = covering ? track.weight : track.weight * track.layerWeight;

    const asset::AnimationClip& clip = source->clips[track.clip];
    const f32 time = sampleTime(track, quantise);
    f32 sample[4]{};
    // **Carried by role** (ADR 0199): a turn goes from the clip's rest to
    // this rig's, the hips' travel is scaled, and nothing else of where a
    // joint is from its parent is the clip's to say.
    const bool carried = map != nullptr && map->carried.roles;
    if (carried) {
        keyed_.assign(map->carried.slots.size(), 0);
        if (track.retargeting == 0)
            warnUnmapped(*map, clip);
    }

    for (const asset::AnimationChannel& channel : clip.channels) {
        // The joint on THIS rig: the channel's own, or remapped by name.
        // Remapped as a number -- the channel was copied, keys and all, for
        // every pose that remapped it (H10).
        u32 joint = channel.joint;
        if (map != nullptr) {
            if (joint >= map->slots.size() || map->slots[joint] < 0) {
                // A joint this rig does not have. Skipped rather than
                // guessed: a shirt with no fingers should keep its own
                // sleeve, not inherit a finger's rotation.
                continue;
            }
            joint = static_cast<u32>(map->slots[joint]);
        }
        if (joint >= jointCount || channel.times.empty())
            continue;
        f32 weight = own;
        if (mask != nullptr) {
            if ((*mask)[joint] <= 0.0f)
                continue;
            weight = own * (*mask)[joint];
        }
        if (!sampleChannel(channel, time, sample))
            continue;
        if (carried && channel.joint < map->carried.byRole.size() && map->carried.byRole[channel.joint] != 0) {
            if (channel.target == asset::AnimationChannel::Target::Rotation) {
                retarget::rotation(map->carried, channel.joint, sample, sample);
                keyed_[channel.joint] = 1;
            }
            else if (channel.target == asset::AnimationChannel::Target::Translation &&
                     static_cast<core::i32>(channel.joint) == map->carried.hips) {
                const DVec3 moved = retarget::hipsTranslation(
                    map->carried,
                    DVec3{static_cast<f64>(sample[0]), static_cast<f64>(sample[1]), static_cast<f64>(sample[2])});
                sample[0] = static_cast<f32>(moved.x);
                sample[1] = static_cast<f32>(moved.y);
                sample[2] = static_cast<f32>(moved.z);
            }
            else {
                // A bone's length and its stretch are this body's own.
                continue;
            }
        }

        switch (channel.target) {
        case asset::AnimationChannel::Target::Translation:
            lanes.translation[joint].x += static_cast<f64>(sample[0] * weight);
            lanes.translation[joint].y += static_cast<f64>(sample[1] * weight);
            lanes.translation[joint].z += static_cast<f64>(sample[2] * weight);
            lanes.weightT[joint] += weight;
            if (covering)
                lanes.coverT[joint] += weight * track.layerWeight;
            break;
        case asset::AnimationChannel::Target::Rotation: {
            f32* accumulator = &lanes.rotation[joint * 4];
            // Sign-aligned against whatever is already there, for the same
            // reason `sampleChannel` aligns two keys: blending q against -q
            // is the long way round, and here it would show as a joint
            // snapping when a second track faded in.
            f32 dot = 0.0f;
            for (usize lane = 0; lane < 4; ++lane)
                dot += accumulator[lane] * sample[lane];
            const f32 sign = (lanes.weightR[joint] > 0.0f && dot < 0.0f) ? -1.0f : 1.0f;
            for (usize lane = 0; lane < 4; ++lane)
                accumulator[lane] += sample[lane] * sign * weight;
            lanes.weightR[joint] += weight;
            if (covering)
                lanes.coverR[joint] += weight * track.layerWeight;
            break;
        }
        case asset::AnimationChannel::Target::Scale:
            lanes.scale[joint].x += sample[0] * weight;
            lanes.scale[joint].y += sample[1] * weight;
            lanes.scale[joint].z += sample[2] * weight;
            lanes.weightS[joint] += weight;
            if (covering)
                lanes.coverS[joint] += weight * track.layerWeight;
            break;
        case asset::AnimationChannel::Target::Weight:
            // A morph target's weight is in a clip's `weights`, never among
            // the channels this walks (ADR 0196); one that is here came from
            // a file that says otherwise, and poses nothing.
            break;
        }
    }
    if (carried) {
        // **A role the clip does not turn stands as the clip's rig rests**,
        // not as this one does: its parent was carried into the clip's
        // stance, and a joint left in its own would stand at the angle
        // between the two.
        for (usize from = 0; from < keyed_.size(); ++from) {
            if (keyed_[from] != 0 || map->carried.byRole[from] == 0 || map->carried.slots[from] < 0)
                continue;
            const auto joint = static_cast<usize>(map->carried.slots[from]);
            if (joint >= jointCount)
                continue;
            f32 weight = own;
            if (mask != nullptr) {
                if ((*mask)[joint] <= 0.0f)
                    continue;
                weight = own * (*mask)[joint];
            }
            const f32* const rest = &map->carried.rest[from * 4];
            f32* accumulator = &lanes.rotation[joint * 4];
            f32 dot = 0.0f;
            for (usize lane = 0; lane < 4; ++lane)
                dot += accumulator[lane] * rest[lane];
            const f32 sign = (lanes.weightR[joint] > 0.0f && dot < 0.0f) ? -1.0f : 1.0f;
            for (usize lane = 0; lane < 4; ++lane)
                accumulator[lane] += rest[lane] * sign * weight;
            lanes.weightR[joint] += weight;
            if (covering)
                lanes.coverR[joint] += weight * track.layerWeight;
        }
    }
    return true;
}

bool AnimationSystem::accumulateAdditive(const Track& track, core::NameAtom rig, usize jointCount, bool quantise)
{
    const SkeletonLibrary::Entry* source = skeletons_->find(track.content);
    if (source == nullptr || track.clip >= source->clips.size())
        return false;
    const JointMap* const map = jointMapFor(track.content, rig, track.retargeting);
    const std::vector<f32>* const mask = maskFor(track, rig);
    const asset::AnimationClip& clip = source->clips[track.clip];
    const f32 time = sampleTime(track, quantise);
    f32 now[4]{};
    f32 first[4]{};
    const bool carried = map != nullptr && map->carried.roles;

    for (const asset::AnimationChannel& channel : clip.channels) {
        u32 joint = channel.joint;
        if (map != nullptr) {
            if (joint >= map->slots.size() || map->slots[joint] < 0)
                continue;
            joint = static_cast<u32>(map->slots[joint]);
        }
        if (joint >= jointCount || channel.times.empty())
            continue;
        f32 weight = track.weight * track.layerWeight;
        if (mask != nullptr)
            weight *= (*mask)[joint];
        if (weight <= 0.0f)
            continue;
        // **From the clip's own first frame**: what it adds is how far it has
        // moved from there, so at its start it adds nothing.
        if (!sampleChannel(channel, time, now) || !sampleChannel(channel, channel.times.front(), first))
            continue;
        if (carried && channel.joint < map->carried.byRole.size() && map->carried.byRole[channel.joint] != 0) {
            // A turn is carried as a turn; what a carried joint adds to its
            // place is the hips' alone, scaled as their travel is.
            if (channel.target == asset::AnimationChannel::Target::Rotation) {
                retarget::rotation(map->carried, channel.joint, now, now);
                retarget::rotation(map->carried, channel.joint, first, first);
            }
            else if (channel.target == asset::AnimationChannel::Target::Translation &&
                     static_cast<core::i32>(channel.joint) == map->carried.hips) {
                const auto carry = [&](f32* sample) {
                    const DVec3 moved = retarget::hipsTranslation(
                        map->carried,
                        DVec3{static_cast<f64>(sample[0]), static_cast<f64>(sample[1]), static_cast<f64>(sample[2])});
                    sample[0] = static_cast<f32>(moved.x);
                    sample[1] = static_cast<f32>(moved.y);
                    sample[2] = static_cast<f32>(moved.z);
                };
                carry(now);
                carry(first);
            }
            else {
                continue;
            }
        }

        switch (channel.target) {
        case asset::AnimationChannel::Target::Translation:
            addT_[joint].x += static_cast<f64>((now[0] - first[0]) * weight);
            addT_[joint].y += static_cast<f64>((now[1] - first[1]) * weight);
            addT_[joint].z += static_cast<f64>((now[2] - first[2]) * weight);
            break;
        case asset::AnimationChannel::Target::Rotation: {
            // The turn from the first frame to now, in the joint's own space,
            // and `weight` of it: towards the identity the short way.
            const f32 back[4]{-first[0], -first[1], -first[2], first[3]};
            f32 turn[4]{};
            multiplyQuaternions(back, now, turn);
            const f32 sign = turn[3] < 0.0f ? -1.0f : 1.0f;
            f32 part[4]{turn[0] * sign * weight, turn[1] * sign * weight, turn[2] * sign * weight,
                        1.0f + (turn[3] * sign - 1.0f) * weight};
            normalizeQuaternion(part);
            f32 sum[4]{};
            multiplyQuaternions(&addR_[joint * 4], part, sum);
            for (usize lane = 0; lane < 4; ++lane)
                addR_[joint * 4 + lane] = sum[lane];
            break;
        }
        case asset::AnimationChannel::Target::Scale: {
            const auto ratio = [weight](f32 current, f32 start) {
                return start != 0.0f ? 1.0f + (current / start - 1.0f) * weight : 1.0f;
            };
            addS_[joint] = core::mul(addS_[joint],
                                     Vec3{ratio(now[0], first[0]), ratio(now[1], first[1]), ratio(now[2], first[2])});
            break;
        }
        case asset::AnimationChannel::Target::Weight:
            break;
        }
    }
    return true;
}

void AnimationSystem::mergeLayer(const SkeletonLibrary::Entry& skeleton, Lanes& base, const Lanes& upper)
{
    const usize jointCount = skeleton.joints.size();
    f32 under[4]{};
    f32 over[4]{};
    for (usize joint = 0; joint < jointCount; ++joint) {
        const asset::Joint& bone = skeleton.joints[joint];
        if (upper.weightT[joint] > 0.0f) {
            const f64 inverse = 1.0 / static_cast<f64>(upper.weightT[joint]);
            const f64 cover = static_cast<f64>(std::min(1.0f, upper.coverT[joint]));
            DVec3 from = bone.localBind.position;
            if (base.weightT[joint] > 0.0f) {
                const f64 baseInverse = 1.0 / static_cast<f64>(base.weightT[joint]);
                from = DVec3{base.translation[joint].x * baseInverse, base.translation[joint].y * baseInverse,
                             base.translation[joint].z * baseInverse};
            }
            base.translation[joint] = DVec3{from.x + (upper.translation[joint].x * inverse - from.x) * cover,
                                            from.y + (upper.translation[joint].y * inverse - from.y) * cover,
                                            from.z + (upper.translation[joint].z * inverse - from.z) * cover};
            base.weightT[joint] = 1.0f;
        }
        if (upper.weightS[joint] > 0.0f) {
            const f32 inverse = 1.0f / upper.weightS[joint];
            const f32 cover = std::min(1.0f, upper.coverS[joint]);
            Vec3 from{1.0f, 1.0f, 1.0f};
            if (base.weightS[joint] > 0.0f)
                from = base.scale[joint] * (1.0f / base.weightS[joint]);
            base.scale[joint] = from + (upper.scale[joint] * inverse - from) * cover;
            base.weightS[joint] = 1.0f;
        }
        if (upper.weightR[joint] > 0.0f) {
            for (usize lane = 0; lane < 4; ++lane)
                over[lane] = upper.rotation[joint * 4 + lane];
            normalizeQuaternion(over);
            if (base.weightR[joint] > 0.0f) {
                for (usize lane = 0; lane < 4; ++lane)
                    under[lane] = base.rotation[joint * 4 + lane];
                normalizeQuaternion(under);
            }
            else {
                core::toQuaternion(bone.localBind.rotation, under[0], under[1], under[2], under[3]);
            }
            const f32 cover = std::min(1.0f, upper.coverR[joint]);
            f32 dot = 0.0f;
            for (usize lane = 0; lane < 4; ++lane)
                dot += under[lane] * over[lane];
            const f32 sign = dot < 0.0f ? -1.0f : 1.0f;
            f32 mixed[4]{};
            for (usize lane = 0; lane < 4; ++lane)
                mixed[lane] = under[lane] + (over[lane] * sign - under[lane]) * cover;
            normalizeQuaternion(mixed);
            for (usize lane = 0; lane < 4; ++lane)
                base.rotation[joint * 4 + lane] = mixed[lane];
            base.weightR[joint] = 1.0f;
        }
    }
}

const std::vector<f32>* AnimationSystem::maskFor(const Track& track, core::NameAtom rig)
{
    if (track.graph == NoGraph || graphs_ == nullptr)
        return nullptr;
    // A rig or a graph loaded again: every mask is made again.
    const core::u64 revision = skeletons_->revision() * 0x9E3779B97F4A7C15ull + graphs_->revision();
    if (revision != masksRevision_) {
        masks_.clear();
        masksRevision_ = revision;
    }
    for (const Mask& mask : masks_) {
        if (mask.graph == track.graphContent && mask.rig == rig && mask.layer == track.layer)
            return mask.whole ? nullptr : &mask.weights;
    }
    Mask made;
    made.graph = track.graphContent;
    made.rig = rig;
    made.layer = track.layer;
    const GraphLibrary::Entry* const entry = graphs_->find(track.graphContent);
    const asset::AnimationGraph* graph = entry != nullptr ? &entry->graph : nullptr;
    const SkeletonLibrary::Entry* skeleton = skeletons_->find(rig);
    if (graph != nullptr && skeleton != nullptr && track.layer < graph->layers.size() &&
        !graph->layers[track.layer].mask.empty()) {
        made.whole = false;
        made.weights.assign(skeleton->joints.size(), 0.0f);
        // A joint the mask names, and everything below it: parents come
        // first, so one pass carries a joint's place in the mask down.
        for (usize joint = 0; joint < skeleton->joints.size(); ++joint) {
            const asset::Joint& bone = skeleton->joints[joint];
            bool in = bone.parent != asset::Joint::NoParent && made.weights[bone.parent] > 0.0f;
            for (const std::string& name : graph->layers[track.layer].mask) {
                in = in || maskNames(*skeleton, static_cast<u32>(joint), name);
                // **Or by what the joint is** (ADR 0199): one graph on bodies
                // whose files call their spines different things.
                if (const retarget::Role role = retarget::roleFromName(name); !in && role != retarget::Role::None) {
                    const retarget::RigRoles* const roles = rolesFor(rig);
                    in = roles != nullptr && roles->joint(role) == static_cast<core::i32>(joint);
                }
            }
            made.weights[joint] = in ? 1.0f : 0.0f;
        }
    }
    masks_.push_back(std::move(made));
    return masks_.back().whole ? nullptr : &masks_.back().weights;
}

core::u64 AnimationSystem::layerWordOf(const Track& track, core::NameAtom rig)
{
    // How its clip is carried onto the rig is part of the pose too: two
    // bodies whose players retarget differently do not share one.
    const core::u64 mode = static_cast<core::u64>(track.retargeting & 0x3u) << 30;
    if (track.graph == NoGraph)
        return mode;
    core::u64 mask = 0;
    if (maskFor(track, rig) != nullptr) {
        for (usize index = 0; index < masks_.size(); ++index) {
            if (masks_[index].graph == track.graphContent && masks_[index].rig == rig &&
                masks_[index].layer == track.layer)
                mask = index + 1;
        }
    }
    return (static_cast<core::u64>(std::bit_cast<u32>(track.layerWeight)) << 32) |
           (static_cast<core::u64>(track.layer & 0xFFFu) << 17) |
           (static_cast<core::u64>(track.additive ? 1u : 0u) << 16) | mode | (mask & 0xFFFFu);
}

Pose& AnimationSystem::ownPose(core::InstanceId meshPart, bool keep)
{
    std::shared_ptr<const Pose>& held = poses_[keyOf(meshPart)];
    // Nobody else holds it: written where it is, its arrays kept.
    if (held != nullptr && held.use_count() == 1)
        return const_cast<Pose&>(*held);
    std::shared_ptr<Pose> own = keep && held != nullptr ? std::make_shared<Pose>(*held) : std::make_shared<Pose>();
    Pose& pose = *own;
    held = std::move(own);
    return pose;
}

// The rig a bone is on: the nearest `MeshPart` above it, through the bones it
// may hang from.
core::InstanceId AnimationSystem::rigOf(core::InstanceId bone) const
{
    for (core::InstanceId at = world_->parentOf(bone); at.valid(); at = world_->parentOf(at)) {
        if (world_->meshParts().find(at) != nullptr)
            return at;
        if (world_->attachments().find(at) == nullptr)
            return {};
    }
    return {};
}

// Every bone of this rig that turns its joint, as the joint and the offset.
std::vector<std::pair<core::u32, core::Mat4>> AnimationSystem::boneOffsets(core::InstanceId meshPart,
                                                                           core::usize jointCount) const
{
    std::vector<std::pair<core::u32, Mat4>> out;
    std::vector<core::InstanceId> stack;
    for (core::InstanceId child = world_->firstChild(meshPart); child.valid(); child = world_->nextSibling(child))
        stack.push_back(child);
    while (!stack.empty()) {
        const core::InstanceId id = stack.back();
        stack.pop_back();
        const scene::AttachmentComponent* bone = world_->attachments().find(id);
        if (bone == nullptr)
            continue;
        if (bone->jointIndex >= 0 && static_cast<core::usize>(bone->jointIndex) < jointCount &&
            !(bone->transform == core::CFrameD{}))
            out.emplace_back(static_cast<core::u32>(bone->jointIndex), toMatrix(bone->transform));
        // Bones hang from bones; another mesh below is another rig.
        for (core::InstanceId child = world_->firstChild(id); child.valid(); child = world_->nextSibling(child))
            stack.push_back(child);
    }
    // In joint order, so two bones on one joint compose the same way each run.
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

// The first skinned mesh under `root` that carries clips, in tree order.
//
// Tree order rather than pool order, because it is the order a person sees in
// the Explorer -- and because it is what a scene file's own order produces, so
// the answer does not change when an unrelated instance is created.
core::InstanceId AnimationSystem::clipSourceUnder(core::InstanceId root) const
{
    if (world_ == nullptr || skeletons_ == nullptr)
        return {};

    std::vector<core::InstanceId> descendants;
    world_->collectDescendants(root, descendants);
    core::InstanceId firstSkinned;
    for (const core::InstanceId id : descendants) {
        const scene::MeshPartComponent* mesh = world_->meshParts().find(id);
        if (mesh == nullptr)
            continue;
        const SkeletonLibrary::Entry* entry = skeletons_->find(mesh->meshContent);
        if (entry == nullptr)
            continue;
        if (!firstSkinned.valid() && !entry->joints.empty())
            firstSkinned = id;
        // A mesh with clips: a rig's, or the weight clips of a face that has
        // morph targets and no skeleton (ADR 0196).
        if (!entry->clips.empty())
            return id;
    }
    // Nothing under it has clips yet -- the meshes may still be loading. The
    // first skinned one is the honest answer: the track is made against that
    // rig, and `createTrack` reports no clip, which is what its own doc says
    // happens for a name the file does not have.
    return firstSkinned;
}

bool AnimationSystem::drives(const Track& track, core::InstanceId meshPart) const
{
    if (track.meshPart == meshPart)
        return true;
    if (!track.driveRoot.valid() || world_ == nullptr)
        return false;
    // Under the root the player was parented to. Walked upwards, because a
    // character is a handful of meshes and this is once per mesh per tick.
    for (core::InstanceId cursor = meshPart; cursor.valid(); cursor = world_->parentOf(cursor)) {
        if (cursor == track.driveRoot)
            return true;
    }
    return false;
}

const retarget::RigRoles* AnimationSystem::rolesFor(core::NameAtom content) const
{
    for (const RolesOf& held : rigRoles_) {
        if (held.content == content)
            return &held.roles;
    }
    const SkeletonLibrary::Entry* entry = skeletons_->find(content);
    if (entry == nullptr)
        return nullptr;
    rigRoles_.push_back(RolesOf{content, retarget::assignRoles(entry->joints, entry->roles)});
    return &rigRoles_.back().roles;
}

namespace {

// Whether two rigs are one skeleton under two files' names: every joint of
// the source that has a role is, on the target, the joint of the same name
// with the same rest from its parent. A body and the shirt cut for it; and a
// clip between them is carried as it always was, bit for bit.
[[nodiscard]] bool oneSkeleton(const SkeletonLibrary::Entry& source, const retarget::RigRoles& sourceRoles,
                               const SkeletonLibrary::Entry& target, const retarget::RigRoles& targetRoles) noexcept
{
    constexpr f64 Near = 1.0e-4;
    for (usize joint = 0; joint < source.joints.size() && joint < sourceRoles.ofJoint.size(); ++joint) {
        const retarget::Role role = sourceRoles.ofJoint[joint];
        if (role == retarget::Role::None)
            continue;
        const core::i32 other = targetRoles.joint(role);
        if (other < 0)
            continue;
        const asset::Joint& a = source.joints[joint];
        const asset::Joint& b = target.joints[static_cast<usize>(other)];
        if (a.name != b.name)
            return false;
        if (std::fabs(a.localBind.position.x - b.localBind.position.x) > Near ||
            std::fabs(a.localBind.position.y - b.localBind.position.y) > Near ||
            std::fabs(a.localBind.position.z - b.localBind.position.z) > Near)
            return false;
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 3; ++row) {
                if (std::fabs(a.localBind.rotation.m[column][row] - b.localBind.rotation.m[column][row]) > 1.0e-4f)
                    return false;
            }
        }
    }
    return true;
}

} // namespace

const AnimationSystem::JointMap* AnimationSystem::jointMapFor(core::NameAtom from, core::NameAtom to,
                                                              core::u8 mode) const
{
    // A clip applied to its own rig needs no map, which is every character made
    // of one mesh.
    if (from == to || skeletons_ == nullptr)
        return nullptr;
    // A rig read again is another rig: its maps and its roles are made again.
    if (skeletons_->revision() != mapsRevision_) {
        jointMaps_.clear();
        rigRoles_.clear();
        mapsRevision_ = skeletons_->revision();
    }

    const bool automatic = mode == 0;
    for (const JointMap& map : jointMaps_) {
        if (map.from == from && map.to == to && map.automatic == automatic)
            return &map;
    }

    const SkeletonLibrary::Entry* source = skeletons_->find(from);
    const SkeletonLibrary::Entry* target = skeletons_->find(to);
    if (source == nullptr || target == nullptr)
        return nullptr;

    JointMap made;
    made.from = from;
    made.to = to;
    made.automatic = automatic;
    // **By roles where both are bodies of different build** (ADR 0199).
    if (automatic) {
        // Both made before either is held: the second being made may move
        // the first.
        (void)rolesFor(from);
        (void)rolesFor(to);
        const retarget::RigRoles* const sourceRoles = rolesFor(from);
        const retarget::RigRoles* const targetRoles = rolesFor(to);
        if (sourceRoles != nullptr && targetRoles != nullptr && sourceRoles->body() && targetRoles->body() &&
            !oneSkeleton(*source, *sourceRoles, *target, *targetRoles)) {
            made.carried = retarget::buildMap(source->joints, *sourceRoles, target->joints, *targetRoles);
            if (made.carried.roles)
                made.slots = made.carried.slots;
        }
    }
    if (!made.carried.roles) {
        made.slots.assign(source->joints.size(), -1);
        for (usize index = 0; index < source->joints.size(); ++index) {
            for (usize other = 0; other < target->joints.size(); ++other) {
                if (source->joints[index].name == target->joints[other].name) {
                    made.slots[index] = static_cast<core::i32>(other);
                    break;
                }
            }
        }
    }
    jointMaps_.push_back(std::move(made));
    return &jointMaps_.back();
}

void AnimationSystem::warnUnmapped(const JointMap& map, const asset::AnimationClip& clip) const
{
    if (map.warned || !map.carried.roles)
        return;
    // Said once a pair, whatever the clip: the first that moves a role the
    // target lacks names them all.
    const retarget::RigRoles* const sourceRoles = rolesFor(map.from);
    if (sourceRoles == nullptr)
        return;
    std::string missing;
    for (const retarget::Role role : map.carried.unmapped) {
        const core::i32 joint = sourceRoles->joint(role);
        bool moved = false;
        for (const asset::AnimationChannel& channel : clip.channels)
            moved = moved || (joint >= 0 && channel.joint == static_cast<u32>(joint));
        if (!moved)
            continue;
        if (!missing.empty())
            missing += ", ";
        missing += retarget::roleName(role);
    }
    if (missing.empty())
        return;
    map.warned = true;
    const std::array<core::I18nArg, 3> said{core::I18nArg{"clips", world_->atoms().text(map.from)},
                                            core::I18nArg{"rig", world_->atoms().text(map.to)},
                                            core::I18nArg{"roles", std::string_view(missing)}};
    core::log(core::LogLevel::Warn, ENG_TR("render.warn.retarget_unmapped"), said);
}

// --- scene::SkeletonHost -----------------------------------------------------

const SkeletonLibrary::Entry* AnimationSystem::skeletonOf(core::InstanceId meshPart) const
{
    if (world_ == nullptr || skeletons_ == nullptr)
        return nullptr;
    const scene::MeshPartComponent* mesh = world_->meshParts().find(meshPart);
    if (mesh == nullptr)
        return nullptr;
    const SkeletonLibrary::Entry* entry = skeletons_->find(mesh->meshContent);
    return entry != nullptr && !entry->joints.empty() ? entry : nullptr;
}

Mat4 AnimationSystem::restModelOf(const SkeletonLibrary::Entry& skeleton, core::u32 joint)
{
    // Walked from the joint UP rather than from the roots down: this answers
    // one question about one joint, and building the whole chain to reach a
    // wrist would be the rest of the skeleton's worth of work for nothing.
    Mat4 out = toMatrix(skeleton.joints[joint].localBind);
    for (core::u32 walk = skeleton.joints[joint].parent; walk != asset::Joint::NoParent;
         walk = skeleton.joints[walk].parent) {
        out = toMatrix(skeleton.joints[walk].localBind) * out;
    }
    return out;
}

core::u32 AnimationSystem::jointCount(core::InstanceId meshPart) const
{
    const SkeletonLibrary::Entry* entry = skeletonOf(meshPart);
    return entry == nullptr ? 0u : static_cast<core::u32>(entry->joints.size());
}

core::i32 AnimationSystem::findJoint(core::InstanceId meshPart, std::string_view name) const
{
    const SkeletonLibrary::Entry* entry = skeletonOf(meshPart);
    if (entry == nullptr)
        return -1;
    for (usize index = 0; index < entry->joints.size(); ++index) {
        if (entry->joints[index].name == name)
            return static_cast<core::i32>(index);
    }
    // A linear scan, because a rig has tens of joints and a `Bone` resolves its
    // name once and then holds the index.
    return -1;
}

core::i32 AnimationSystem::jointParent(core::InstanceId meshPart, core::u32 joint) const
{
    const SkeletonLibrary::Entry* entry = skeletonOf(meshPart);
    if (entry == nullptr || joint >= entry->joints.size())
        return -1;
    const core::u32 parent = entry->joints[joint].parent;
    return parent == asset::Joint::NoParent ? -1 : static_cast<core::i32>(parent);
}

std::string_view AnimationSystem::jointName(core::InstanceId meshPart, core::u32 joint) const
{
    const SkeletonLibrary::Entry* entry = skeletonOf(meshPart);
    if (entry == nullptr || joint >= entry->joints.size())
        return {};
    return entry->joints[joint].name;
}

std::string_view AnimationSystem::jointRole(core::InstanceId meshPart, core::u32 joint) const
{
    const retarget::RigRoles* const roles = rolesOf(meshPart);
    if (roles == nullptr || joint >= roles->ofJoint.size() || roles->ofJoint[joint] == retarget::Role::None)
        return {};
    return retarget::roleName(roles->ofJoint[joint]);
}

bool AnimationSystem::jointModel(core::InstanceId meshPart, core::u32 joint, core::CFrameD& out) const
{
    const SkeletonLibrary::Entry* entry = skeletonOf(meshPart);
    if (entry == nullptr || joint >= entry->joints.size())
        return false;
    // **A pose that skipped its tick is built when a joint is asked for** (H3):
    // the answer is the one every tick would have given. Logically const -- the
    // pose is a cache of the tracks' state -- which is what the cast says.
    if (!stale_.empty() && stale_.contains(keyOf(meshPart)))
        const_cast<AnimationSystem*>(this)->catchUp(meshPart);

    // The posed transform when there is a pose, and the REST chain when there is
    // not -- a character standing still has no pose at all, and a socket on its
    // hand still has to be somewhere.
    const auto found = poses_.find(keyOf(meshPart));
    const Mat4 model = found != poses_.end() && joint < found->second->model.size() ? found->second->model[joint]
                                                                                    : restModelOf(*entry, joint);
    // Orthonormalised on the way out: an exporter is free to bake scale into a
    // bind pose and often does, and a socket welded to a joint has to be rigid
    // or every part hanging off it inherits that scale (`core::cframeFromMatrix`).
    out = core::cframeFromMatrix(model);
    return true;
}

AnimationSystem::OverrideSet* AnimationSystem::overridesFor(core::InstanceId meshPart) noexcept
{
    return const_cast<OverrideSet*>(static_cast<const AnimationSystem*>(this)->overridesFor(meshPart));
}

const AnimationSystem::OverrideSet* AnimationSystem::overridesFor(core::InstanceId meshPart) const noexcept
{
    for (const OverrideSet& set : overrides_) {
        if (set.meshPart == meshPart)
            return &set;
    }
    return nullptr;
}

void AnimationSystem::setJointOverride(core::InstanceId meshPart, core::u32 joint, const core::CFrameD& model)
{
    if (jointCount(meshPart) <= joint)
        return;

    OverrideSet* set = overridesFor(meshPart);
    if (set == nullptr) {
        // Appended in the order meshes were first driven, which is an order the
        // caller controls and can therefore make deterministic. A map keyed on
        // the instance would put the iteration order in the hash (R10).
        overrides_.push_back(OverrideSet{meshPart, {}});
        set = &overrides_.back();
    }

    const Mat4 matrix = toMatrix(model);
    const auto at = std::lower_bound(set->joints.begin(), set->joints.end(), joint,
                                     [](const Override& entry, core::u32 key) { return entry.joint < key; });
    if (at != set->joints.end() && at->joint == joint) {
        // The last writer wins, and it wins the same way every time -- which is
        // what makes two things reaching for one joint a bug the author can see
        // rather than a bug that depends on iteration order.
        at->model = matrix;
        return;
    }
    set->joints.insert(at, Override{joint, matrix});
}

void AnimationSystem::clearJointOverrides(core::InstanceId meshPart)
{
    for (usize index = 0; index < overrides_.size(); ++index) {
        if (overrides_[index].meshPart != meshPart)
            continue;
        overrides_.erase(overrides_.begin() + static_cast<std::ptrdiff_t>(index));
        return;
    }
}

void AnimationSystem::catchUp(core::InstanceId meshPart)
{
    const scene::MeshPartComponent* mesh = world_->meshParts().find(meshPart);
    const SkeletonLibrary::Entry* entry = mesh != nullptr ? skeletons_->find(mesh->meshContent) : nullptr;
    if (entry != nullptr && !entry->joints.empty())
        rebuildPose(meshPart, *entry);
    stale_.erase(keyOf(meshPart));
}

void AnimationSystem::reportSeen(std::span<const SeenSkin> seen, bool fresh)
{
    if (fresh)
        seen_.clear();
    seeing_ = true;
    for (const SeenSkin& skin : seen) {
        f32& size = seen_[keyOf(skin.meshPart)];
        size = std::max(size, skin.screenHeight);
    }
}

bool AnimationSystem::animates(core::InstanceId meshPart) const
{
    const SkeletonLibrary::Entry* entry = skeletonOf(meshPart);
    return entry != nullptr && !entry->joints.empty();
}

void AnimationSystem::commitOverrides()
{
    // What a ragdoll drives this tick is posed every tick from the next (H3).
    overridden_.clear();
    for (const OverrideSet& set : overrides_)
        overridden_.push_back(set.meshPart);
    for (const OverrideSet& set : overrides_) {
        const SkeletonLibrary::Entry* entry = skeletonOf(set.meshPart);
        if (entry == nullptr || set.joints.empty())
            continue;

        const usize jointCount = entry->joints.size();
        // Its own, with what it held: the joints the override does not name
        // keep their locals, and a pose a crowd shares is not written to.
        Pose& pose = ownPose(set.meshPart, true);
        if (pose.model.size() != jointCount) {
            // No pose this tick: the mesh has a rig and nothing playing, which
            // is exactly a limp ragdoll. Built from rest so the joints the
            // override does NOT name are still somewhere sensible.
            pose.palette.assign(jointCount, Mat4{});
            pose.model.assign(jointCount, Mat4{});
            pose.local.assign(jointCount, Mat4{});
            for (usize joint = 0; joint < jointCount; ++joint)
                pose.local[joint] = toMatrix(entry->joints[joint].localBind);
        }

        // **One forward pass with substitutions**, and this is the whole reason
        // an override is not just a write into the palette. A ragdoll simulates
        // a dozen bones; a hand has twenty. The fingers are not overridden, so
        // they take their parent's new model transform and their own unchanged
        // local -- which is what makes them ride along on the wrist instead of
        // staying where the clip left them.
        usize next = 0;
        for (usize joint = 0; joint < jointCount; ++joint) {
            if (next < set.joints.size() && set.joints[next].joint == joint) {
                pose.model[joint] = set.joints[next].model;
                ++next;
            }
            else {
                const core::u32 parent = entry->joints[joint].parent;
                pose.model[joint] =
                    parent == asset::Joint::NoParent ? pose.local[joint] : pose.model[parent] * pose.local[joint];
            }
            pose.palette[joint] = pose.model[joint] * entry->joints[joint].inverseBind;
        }
    }

    // Cleared, always. An override that outlived the tick that set it is a
    // ragdoll that keeps driving a character nobody is simulating any more.
    overrides_.clear();
}

std::span<const scene::TrackId> AnimationSystem::drainEnded()
{
    endedDrained_.swap(ended_);
    ended_.clear();
    return endedDrained_;
}

void AnimationSystem::retire(const scene::World& world)
{
    for (usize index = 1; index < tracks_.size(); ++index) {
        Track& track = tracks_[index];
        if (track.alive && !world.alive(track.player)) {
            // A track is a reference to a player and never a reason to keep one
            // alive. It stops rather than being erased, so that a handle a
            // script still holds keeps answering reads instead of resolving to
            // whatever took its slot.
            track.alive = false;
            track.playing = false;
            track.holding = false;
            poses_.erase(keyOf(track.meshPart));
        }
    }
    // A graph whose player is gone gives its tracks back.
    for (GraphInstance& instance : graphInstances_) {
        if (instance.alive && !world.alive(instance.player)) {
            unbindGraph(instance);
            instance.alive = false;
            graphIndex_.erase(keyOf(instance.player));
        }
    }
    // And what scripts set on meshes that are gone.
    for (auto entry = morphOverrides_.begin(); entry != morphOverrides_.end();) {
        const core::InstanceId id{static_cast<core::u32>(entry->first & 0xFFFFFFFFu),
                                  static_cast<core::u32>(entry->first >> 32)};
        entry = world.alive(id) ? std::next(entry) : morphOverrides_.erase(entry);
    }
}

// --- Animation graphs (ADR 0197) -----------------------------------------------

namespace {

// How quickly a body's speed, read from where it was a tick ago, settles:
// three ticks. A replica moves another player's character in steps as
// snapshots arrive, and a speed taken from one tick alone would make a walk
// flicker between a run and a stand.
constexpr f64 SpeedSettleSeconds = 0.05;

// A value as one number, for telling whether it changed.
[[nodiscard]] core::u64 digestOf(const scene::Value& value) noexcept
{
    if (const auto* number = std::get_if<f64>(&value))
        return std::bit_cast<core::u64>(*number) ^ 0x1ull;
    if (const auto* flag = std::get_if<bool>(&value))
        return *flag ? 0x3ull : 0x2ull;
    if (const auto* text = std::get_if<std::string>(&value)) {
        core::u64 hash = 0xCBF29CE484222325ull;
        for (const char letter : *text)
            hash = (hash ^ static_cast<core::u8>(letter)) * 0x100000001B3ull;
        return hash;
    }
    return 0x9E3779B97F4A7C15ull * (static_cast<core::u64>(value.index()) + 1);
}

// And as a parameter's number: a number is itself, true is one.
[[nodiscard]] f32 numberOf(const scene::Value& value) noexcept
{
    if (const auto* number = std::get_if<f64>(&value))
        return static_cast<f32>(*number);
    if (const auto* flag = std::get_if<bool>(&value))
        return *flag ? 1.0f : 0.0f;
    return 0.0f;
}

} // namespace

AnimationSystem::GraphInstance* AnimationSystem::graphOf(core::InstanceId player) noexcept
{
    const auto found = graphIndex_.find(keyOf(player));
    return found == graphIndex_.end() ? nullptr : &graphInstances_[found->second];
}

const AnimationSystem::GraphInstance* AnimationSystem::graphOf(core::InstanceId player) const noexcept
{
    const auto found = graphIndex_.find(keyOf(player));
    return found == graphIndex_.end() ? nullptr : &graphInstances_[found->second];
}

AnimationSystem::GraphInstance& AnimationSystem::graphFor(core::InstanceId player)
{
    if (GraphInstance* const held = graphOf(player); held != nullptr)
        return *held;
    // A place a graph that went left behind, before a new one.
    u32 slot = static_cast<u32>(graphInstances_.size());
    for (u32 index = 0; index < graphInstances_.size(); ++index) {
        if (!graphInstances_[index].alive) {
            slot = index;
            break;
        }
    }
    if (slot == graphInstances_.size())
        graphInstances_.emplace_back();
    graphInstances_[slot] = GraphInstance{};
    graphInstances_[slot].player = player;
    graphIndex_[keyOf(player)] = slot;
    return graphInstances_[slot];
}

void AnimationSystem::unbindGraph(GraphInstance& instance)
{
    for (const u32 index : instance.tracks) {
        Track& track = tracks_[index];
        if (track.meshPart.valid())
            poses_.erase(keyOf(track.meshPart));
        track = Track{};
        track.alive = false;
        freeTracks_.push_back(index);
    }
    instance.tracks.clear();
    instance.lengths.clear();
    instance.bound = false;
}

void AnimationSystem::bindGraph(GraphInstance& instance, const GraphLibrary::Entry& entry)
{
    unbindGraph(instance);
    const asset::AnimationGraph& graph = entry.graph;
    instance.evaluator.reset(graph);
    instance.seen.assign(graph.parameters.size(), 0);
    instance.sighted.assign(graph.parameters.size(), 0);
    instance.tracks.reserve(graph.clips.size());
    for (usize clip = 0; clip < graph.clips.size(); ++clip) {
        // A track as a script would load it, in a slot no script holds.
        scene::TrackId made = 0;
        if (!freeTracks_.empty()) {
            // The lowest first: the same slots on every run.
            const auto lowest = std::min_element(freeTracks_.begin(), freeTracks_.end());
            const u32 slot = *lowest;
            freeTracks_.erase(lowest);
            const scene::TrackId fresh = createTrack(instance.player, entry.clipFiles[clip], entry.clipNames[clip]);
            tracks_[slot] = tracks_[fresh];
            tracks_.pop_back();
            made = slot;
        }
        else {
            made = createTrack(instance.player, entry.clipFiles[clip], entry.clipNames[clip]);
        }
        Track& track = tracks_[made];
        track.graph = static_cast<u32>(&instance - graphInstances_.data());
        track.graphContent = instance.content;
        track.layer = static_cast<core::u16>(graph.clips[clip].layer);
        track.additive = graph.layers[graph.clips[clip].layer].additive;
        track.weight = 0.0f;
        track.ownWeight = 0.0f;
        track.targetWeight = 0.0f;
        instance.tracks.push_back(made);
    }
    instance.lengths.assign(graph.clips.size(), 0.0f);
    instance.bound = true;
    instance.revision = graphs_->revision();

    // What a script set while the file was on its way, in the order it did.
    for (const GraphInstance::Pending& pending : instance.pending) {
        const core::i32 parameter = graph.parameterNamed(pending.name);
        if (parameter < 0)
            continue;
        if (pending.clear)
            instance.evaluator.clearOverride(static_cast<u32>(parameter));
        else if (graph.parameters[static_cast<usize>(parameter)].kind == asset::GraphParameterKind::Trigger)
            instance.evaluator.fire(static_cast<u32>(parameter));
        else
            instance.evaluator.setOverride(static_cast<u32>(parameter), pending.value);
    }
    instance.pending.clear();
}

core::InstanceId AnimationSystem::bodyOf(core::InstanceId player) const
{
    // The body the character is: the player's parent or one above it, or --
    // for a player parented to a `Model` -- the first one in the model.
    const core::InstanceId parent = world_->parentOf(player);
    for (core::InstanceId at = parent; at.valid(); at = world_->parentOf(at)) {
        if (world_->characterBodies().find(at) != nullptr)
            return at;
    }
    std::vector<core::InstanceId> below;
    if (parent.valid())
        world_->collectDescendants(parent, below);
    for (const core::InstanceId id : below) {
        if (world_->characterBodies().find(id) != nullptr)
            return id;
    }
    return {};
}

void AnimationSystem::readSources(GraphInstance& instance, const GraphLibrary::Entry& entry, f64 fixedDt)
{
    const asset::AnimationGraph& graph = entry.graph;
    bool wantsBody = false;
    for (const GraphLibrary::Source source : entry.sources)
        wantsBody = wantsBody || (source != GraphLibrary::Source::None && source != GraphLibrary::Source::Attribute);

    const scene::CharacterBodyComponent* body = nullptr;
    const scene::PartComponent* part = nullptr;
    if (wantsBody) {
        if (!instance.body.valid() || !world_->alive(instance.body) ||
            world_->characterBodies().find(instance.body) == nullptr) {
            instance.body = bodyOf(instance.player);
            instance.placed = false;
        }
        body = world_->characterBodies().find(instance.body);
        part = world_->parts().find(instance.body);
    }
    // **How fast it is going, from where it was**: the one thing every
    // machine has of a character -- the authority that walks it, the replica
    // that predicts it and the replica that is only told where it is.
    Vec3 local{0.0f, 0.0f, 0.0f};
    // **The tick a body is first seen says nothing of it.** A body says
    // whether it is on the ground after the physics has stepped it, and the
    // graph is stepped before the physics: a character just made says "not
    // on the ground" of a floor it is standing on, and the plainest rule a
    // graph has -- in the air is a jump -- played a tick of the jump for
    // every character that appeared. Its parameters keep their rest that
    // one tick.
    const bool met = instance.placed;
    if (body != nullptr && part != nullptr) {
        const DVec3 position = part->cframe.position;
        if (instance.placed && fixedDt > 0.0) {
            const Vec3 moved{static_cast<f32>((position.x - instance.lastPosition.x) / fixedDt),
                             static_cast<f32>((position.y - instance.lastPosition.y) / fixedDt),
                             static_cast<f32>((position.z - instance.lastPosition.z) / fixedDt)};
            const auto settle = static_cast<f32>(std::min(1.0, fixedDt / SpeedSettleSeconds));
            instance.velocity = instance.velocity + (moved - instance.velocity) * settle;
        }
        instance.lastPosition = position;
        instance.placed = true;
        // In the body's own frame: right and forward.
        local = core::transpose(part->cframe.rotation) * instance.velocity;
    }

    const core::InstanceId holder = world_->parentOf(instance.player);
    for (usize index = 0; index < graph.parameters.size(); ++index) {
        const GraphLibrary::Source source = entry.sources[index];
        if (source == GraphLibrary::Source::None)
            continue;
        const bool trigger = graph.parameters[index].kind == asset::GraphParameterKind::Trigger;
        f32 value = 0.0f;
        core::u64 digest = 0;
        if (source == GraphLibrary::Source::Attribute) {
            const scene::Value held =
                holder.valid() ? world_->getAttribute(holder, entry.attributes[index]) : scene::Value{};
            // An attribute nobody has set says nothing: the parameter is at
            // its rest.
            value = std::holds_alternative<std::monostate>(held) ? graph.parameters[index].rest : numberOf(held);
            digest = digestOf(held);
        }
        else {
            if (body == nullptr || !met)
                continue;
            switch (source) {
            case GraphLibrary::Source::Speed:
                value =
                    std::sqrt(instance.velocity.x * instance.velocity.x + instance.velocity.z * instance.velocity.z);
                break;
            case GraphLibrary::Source::VerticalSpeed:
                value = instance.velocity.y;
                break;
            case GraphLibrary::Source::MoveX:
                value = local.x;
                break;
            case GraphLibrary::Source::MoveZ:
                // Forward is -Z.
                value = -local.z;
                break;
            case GraphLibrary::Source::Grounded:
                value = body->grounded ? 1.0f : 0.0f;
                break;
            case GraphLibrary::Source::State:
                value = static_cast<f32>(body->state);
                break;
            case GraphLibrary::Source::None:
            case GraphLibrary::Source::Attribute:
                break;
            }
            digest = static_cast<core::u64>(std::bit_cast<u32>(value)) + 1;
        }
        if (trigger) {
            // **A trigger fires on a change, and a first sight is not one**:
            // somebody who joins after an attack does not see it again.
            if (instance.sighted[index] != 0 && instance.seen[index] != digest)
                instance.evaluator.fire(static_cast<u32>(index));
        }
        else {
            instance.evaluator.setSource(static_cast<u32>(index), value);
        }
        instance.seen[index] = digest;
        instance.sighted[index] = 1;
    }
}

void AnimationSystem::stepGraphs(f64 fixedDt)
{
    graphSignals_.clear();
    if (graphs_ == nullptr)
        return;
    // Every player that names a graph, in id order: the same order on every
    // run, whatever order the pool holds them in.
    graphPlayers_.clear();
    world_->animationPlayers().forEach([&](core::InstanceId id, const scene::AnimationPlayerComponent& player) {
        if (player.graph.valid())
            graphPlayers_.push_back(id);
    });
    if (graphPlayers_.empty() && graphInstances_.empty())
        return;
    std::sort(graphPlayers_.begin(), graphPlayers_.end(), [](core::InstanceId a, core::InstanceId b) {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    });
    const core::u64 stamp = sampled_ + 1;
    for (const core::InstanceId id : graphPlayers_) {
        GraphInstance& instance = graphFor(id);
        instance.stamp = stamp;
        const core::NameAtom content = world_->animationPlayers().find(id)->graph;
        if (!(instance.content == content)) {
            unbindGraph(instance);
            instance.content = content;
        }
    }

    for (usize index = 0; index < graphInstances_.size(); ++index) {
        GraphInstance& instance = graphInstances_[index];
        if (!instance.alive)
            continue;
        // Its player took the graph off, or is gone.
        if (instance.stamp != stamp) {
            unbindGraph(instance);
            instance.alive = false;
            graphIndex_.erase(keyOf(instance.player));
            continue;
        }
        const GraphLibrary::Entry* const entry = graphs_->find(instance.content);
        if (entry == nullptr) {
            // Not loaded, or taken away to be read again.
            if (instance.bound)
                unbindGraph(instance);
            continue;
        }
        if (!instance.bound || instance.revision != graphs_->revision())
            bindGraph(instance, *entry);
        const asset::AnimationGraph& graph = entry->graph;

        readSources(instance, *entry, fixedDt);
        for (usize clip = 0; clip < instance.tracks.size(); ++clip) {
            Track& track = tracks_[instance.tracks[clip]];
            instance.lengths[clip] = bindTrack(track) ? track.length : 0.0f;
        }
        graphScratch_.clear();
        instance.evaluator.step(graph, fixedDt, instance.lengths, graphScratch_);

        const std::span<const GraphPlayer::ClipState> clips = instance.evaluator.clips();
        for (usize clip = 0; clip < instance.tracks.size() && clip < clips.size(); ++clip) {
            Track& track = tracks_[instance.tracks[clip]];
            track.time = clips[clip].time;
            track.weight = clips[clip].weight;
            track.playing = clips[clip].active && clips[clip].weight > 0.0f;
            track.holding = false;
            track.layerWeight = instance.evaluator.layerWeight(graph, graph.clips[clip].layer);
        }
        for (const GraphPlayer::Signal& signal : graphScratch_) {
            scene::GraphSignal said;
            said.player = instance.player;
            if (signal.layer >= graph.layers.size())
                continue;
            const asset::GraphLayer& layer = graph.layers[signal.layer];
            said.layer = layer.name;
            if (signal.kind == GraphPlayer::Signal::Kind::Event) {
                if (signal.clip >= graph.clips.size() || signal.event >= graph.clips[signal.clip].events.size())
                    continue;
                said.event = true;
                said.to = graph.clips[signal.clip].events[signal.event].name;
            }
            else {
                if (signal.from >= layer.states.size() || signal.to >= layer.states.size())
                    continue;
                said.from = layer.states[signal.from].name;
                said.to = layer.states[signal.to].name;
            }
            graphSignals_.push_back(said);
        }
    }
}

scene::GraphWrite AnimationSystem::setGraphParameter(core::InstanceId player, std::string_view name, f32 value)
{
    GraphInstance& instance = graphFor(player);
    const GraphLibrary::Entry* const entry =
        instance.bound && graphs_ != nullptr ? graphs_->find(instance.content) : nullptr;
    if (entry == nullptr) {
        instance.pending.push_back(GraphInstance::Pending{std::string(name), value, false});
        return scene::GraphWrite::Done;
    }
    const core::i32 parameter = entry->graph.parameterNamed(name);
    if (parameter < 0)
        return scene::GraphWrite::Unknown;
    if (entry->graph.parameters[static_cast<usize>(parameter)].kind == asset::GraphParameterKind::Trigger)
        instance.evaluator.fire(static_cast<u32>(parameter));
    else
        instance.evaluator.setOverride(static_cast<u32>(parameter), value);
    return scene::GraphWrite::Done;
}

scene::GraphWrite AnimationSystem::clearGraphParameter(core::InstanceId player, std::string_view name)
{
    GraphInstance& instance = graphFor(player);
    const GraphLibrary::Entry* const entry =
        instance.bound && graphs_ != nullptr ? graphs_->find(instance.content) : nullptr;
    if (entry == nullptr) {
        instance.pending.push_back(GraphInstance::Pending{std::string(name), 0.0f, true});
        return scene::GraphWrite::Done;
    }
    const core::i32 parameter = entry->graph.parameterNamed(name);
    if (parameter < 0)
        return scene::GraphWrite::Unknown;
    instance.evaluator.clearOverride(static_cast<u32>(parameter));
    return scene::GraphWrite::Done;
}

scene::GraphParameterValue AnimationSystem::graphParameter(core::InstanceId player, std::string_view name) const
{
    const GraphInstance* const instance = graphOf(player);
    if (instance == nullptr || !instance->bound || graphs_ == nullptr)
        return {};
    const GraphLibrary::Entry* const entry = graphs_->find(instance->content);
    if (entry == nullptr)
        return {};
    const core::i32 parameter = entry->graph.parameterNamed(name);
    if (parameter < 0)
        return {};
    scene::GraphParameterValue answer;
    switch (entry->graph.parameters[static_cast<usize>(parameter)].kind) {
    case asset::GraphParameterKind::Number:
        answer.kind = scene::GraphParameterValue::Kind::Number;
        break;
    case asset::GraphParameterKind::Boolean:
        answer.kind = scene::GraphParameterValue::Kind::Boolean;
        break;
    case asset::GraphParameterKind::Trigger:
        answer.kind = scene::GraphParameterValue::Kind::Trigger;
        break;
    }
    answer.value = instance->evaluator.value(static_cast<u32>(parameter));
    return answer;
}

std::string_view AnimationSystem::graphState(core::InstanceId player, std::string_view layer) const
{
    const GraphInstance* const instance = graphOf(player);
    if (instance == nullptr || !instance->bound || graphs_ == nullptr)
        return {};
    const GraphLibrary::Entry* const entry = graphs_->find(instance->content);
    if (entry == nullptr || entry->graph.layers.empty())
        return {};
    const core::i32 at = layer.empty() ? 0 : entry->graph.layerNamed(layer);
    if (at < 0)
        return {};
    const asset::GraphLayer& in = entry->graph.layers[static_cast<usize>(at)];
    const u32 state = instance->evaluator.state(static_cast<u32>(at));
    return state < in.states.size() ? std::string_view(in.states[state].name) : std::string_view{};
}

std::span<const scene::GraphSignal> AnimationSystem::drainGraphSignals()
{
    graphSignalsDrained_.swap(graphSignals_);
    graphSignals_.clear();
    return graphSignalsDrained_;
}

void AnimationSystem::graphDigests(std::vector<std::pair<core::InstanceId, core::u64>>& into) const
{
    std::vector<core::u64> words;
    for (const GraphInstance& instance : graphInstances_) {
        if (!instance.alive || !instance.bound)
            continue;
        words.clear();
        instance.evaluator.hashInto(words);
        into.emplace_back(instance.player, SignatureHash{}(words) | 1ull);
    }
}

void AnimationSystem::describeGraph(core::InstanceId player, std::vector<scene::GraphLayerView>& layers,
                                    std::vector<scene::GraphParameterView>& parameters) const
{
    layers.clear();
    parameters.clear();
    const GraphInstance* const instance = graphOf(player);
    if (instance == nullptr || !instance->bound || graphs_ == nullptr)
        return;
    const GraphLibrary::Entry* const entry = graphs_->find(instance->content);
    if (entry == nullptr)
        return;
    const asset::AnimationGraph& graph = entry->graph;
    for (u32 index = 0; index < graph.layers.size(); ++index) {
        const asset::GraphLayer& layer = graph.layers[index];
        scene::GraphLayerView view;
        view.name = layer.name;
        const u32 state = instance->evaluator.state(index);
        if (state < layer.states.size())
            view.state = layer.states[state].name;
        view.progress = instance->evaluator.stateProgress(index);
        view.weight = instance->evaluator.layerWeight(graph, index);
        for (const GraphPlayer::Fading& fading : instance->evaluator.fading(index)) {
            if (fading.state < layer.states.size())
                view.fading.emplace_back(layer.states[fading.state].name, fading.weight);
        }
        layers.push_back(std::move(view));
    }
    for (u32 index = 0; index < graph.parameters.size(); ++index) {
        scene::GraphParameterView view;
        view.name = graph.parameters[index].name;
        view.from = graph.parameters[index].from;
        view.value = graphParameter(player, view.name);
        view.overridden = instance->evaluator.overridden(index);
        parameters.push_back(view);
    }
}

// --- Morph targets (ADR 0196) --------------------------------------------------

namespace {

// A weight channel at `time`: one number a key, the middle of three on a
// spline. `sampleChannel` is for a joint's three and four.
[[nodiscard]] bool sampleWeight(const asset::AnimationChannel& channel, f32 time, f32& out) noexcept
{
    const usize perKey = channel.valuesPerKey();
    if (channel.stride != 1 || channel.times.empty() || channel.values.size() < channel.times.size() * perKey)
        return false;
    const usize key = keyBefore(channel.times, time);
    const bool last = key + 1 >= channel.times.size();
    const auto valueOf = [&](usize index) { return channel.values[index * perKey + (perKey == 3 ? 1 : 0)]; };
    const f32 from = valueOf(key);
    if (channel.interpolation == asset::AnimationChannel::Interpolation::Step || last) {
        out = from;
        return true;
    }
    const f32 to = valueOf(key + 1);
    const f32 s = fractionBetween(channel.times, key, time);
    if (channel.interpolation == asset::AnimationChannel::Interpolation::CubicSpline) {
        // Hermite, as `sampleChannel` has it.
        const f32 span = channel.times[key + 1] - channel.times[key];
        const f32 s2 = s * s;
        const f32 s3 = s2 * s;
        const f32 leaving = channel.values[key * 3 + 2];
        const f32 arriving = channel.values[(key + 1) * 3];
        out = (2.0f * s3 - 3.0f * s2 + 1.0f) * from + (s3 - 2.0f * s2 + s) * span * leaving +
              (-2.0f * s3 + 3.0f * s2) * to + (s3 - s2) * span * arriving;
        return true;
    }
    out = from + (to - from) * s;
    return true;
}

// Where `name` is in `names`, or -1.
[[nodiscard]] core::i32 targetNamed(const std::vector<std::string>& names, std::string_view name) noexcept
{
    for (usize index = 0; index < names.size(); ++index) {
        if (names[index] == name)
            return static_cast<core::i32>(index);
    }
    return -1;
}

} // namespace

const SkeletonLibrary::Entry* AnimationSystem::morphsOf(core::InstanceId meshPart) const
{
    if (world_ == nullptr || skeletons_ == nullptr)
        return nullptr;
    const scene::MeshPartComponent* mesh = world_->meshParts().find(meshPart);
    if (mesh == nullptr)
        return nullptr;
    const SkeletonLibrary::Entry* entry = skeletons_->find(mesh->meshContent);
    return entry != nullptr && !entry->morphNames.empty() ? entry : nullptr;
}

core::u32 AnimationSystem::morphTargetCount(core::InstanceId meshPart) const
{
    const SkeletonLibrary::Entry* entry = morphsOf(meshPart);
    return entry != nullptr ? static_cast<core::u32>(entry->morphNames.size()) : 0u;
}

std::string_view AnimationSystem::morphTargetName(core::InstanceId meshPart, core::u32 target) const
{
    const SkeletonLibrary::Entry* entry = morphsOf(meshPart);
    return entry != nullptr && target < entry->morphNames.size() ? std::string_view{entry->morphNames[target]}
                                                                 : std::string_view{};
}

std::span<const u32> AnimationSystem::weightTracks() const
{
    if (weightTracksAt_ == sampled_ && weightTracksOf_ == tracks_.size())
        return weightTracks_;
    weightTracksAt_ = sampled_;
    weightTracksOf_ = tracks_.size();
    weightTracks_.clear();
    for (usize index = 1; index < tracks_.size(); ++index) {
        const Track& track = tracks_[index];
        if (!track.alive || track.clip == NoClip)
            continue;
        const SkeletonLibrary::Entry* source = skeletons_->find(track.content);
        if (source != nullptr && track.clip < source->clips.size() && !source->clips[track.clip].weights.empty())
            weightTracks_.push_back(static_cast<u32>(index));
    }
    return weightTracks_;
}

bool AnimationSystem::clipMorphWeights(core::InstanceId meshPart, const SkeletonLibrary::Entry& entry) const
{
    const std::span<const u32> candidates = weightTracks();
    if (candidates.empty())
        return false;
    const scene::MeshPartComponent* mesh = world_->meshParts().find(meshPart);
    if (mesh == nullptr)
        return false;
    const usize count = entry.morphNames.size();
    morphSum_.assign(count, 0.0f);
    morphTotal_.assign(count, 0.0f);
    bool any = false;
    for (const u32 index : candidates) {
        const Track& track = tracks_[index];
        if (!track.alive || track.clip == NoClip || track.weight <= 0.0f || !(track.playing || track.holding))
            continue;
        if (!drives(track, meshPart))
            continue;
        const SkeletonLibrary::Entry* source = skeletons_->find(track.content);
        if (source == nullptr || track.clip >= source->clips.size())
            continue;
        // **By name when the clip is another file's**, as a joint is: two
        // files number their targets as their exporter pleased.
        const bool own = track.content == mesh->meshContent;
        const auto time = static_cast<f32>(track.time);
        for (const asset::AnimationChannel& channel : source->clips[track.clip].weights) {
            core::i32 target = static_cast<core::i32>(channel.joint);
            if (!own) {
                target = channel.joint < source->morphNames.size()
                             ? targetNamed(entry.morphNames, source->morphNames[channel.joint])
                             : -1;
            }
            if (target < 0 || static_cast<usize>(target) >= count)
                continue;
            f32 value = 0.0f;
            if (!sampleWeight(channel, time, value))
                continue;
            morphSum_[static_cast<usize>(target)] += value * track.weight;
            morphTotal_[static_cast<usize>(target)] += track.weight;
            any = true;
        }
    }
    if (!any)
        return false;
    // **A target the tracks do not wholly speak for keeps the rest of its
    // file's weight**: one clip fading in eases a smile in, where a joint --
    // which always has a pose under it -- is averaged. Past a whole, the
    // tracks are averaged as joints are.
    morphScratch_.resize(count);
    for (usize target = 0; target < count; ++target) {
        const f32 rest = target < entry.morphDefaults.size() ? entry.morphDefaults[target] : 0.0f;
        const f32 total = morphTotal_[target];
        morphScratch_[target] = total <= 0.0f   ? rest
                                : total >= 1.0f ? morphSum_[target] / total
                                                : morphSum_[target] + (1.0f - total) * rest;
    }
    return true;
}

std::span<const f32> AnimationSystem::drawnMorphWeights(core::InstanceId meshPart) const
{
    const auto overrides = morphOverrides_.find(keyOf(meshPart));
    const bool scripted = overrides != morphOverrides_.end() && !overrides->second.empty();
    // The common answer, before anything is looked up: nobody set a weight
    // on this mesh and no track anywhere plays one.
    if (!scripted && weightTracks().empty())
        return {};
    const SkeletonLibrary::Entry* entry = morphsOf(meshPart);
    if (entry == nullptr)
        return {};
    const bool clips = clipMorphWeights(meshPart, *entry);
    if (!clips && !scripted)
        return {};
    if (!clips) {
        morphScratch_.assign(entry->morphNames.size(), 0.0f);
        for (usize target = 0; target < morphScratch_.size() && target < entry->morphDefaults.size(); ++target)
            morphScratch_[target] = entry->morphDefaults[target];
    }
    // **The script's value wins while it is set.**
    if (scripted) {
        for (const MorphOverride& set : overrides->second) {
            if (const core::i32 target = targetNamed(entry->morphNames, set.name); target >= 0)
                morphScratch_[static_cast<usize>(target)] = set.weight;
        }
    }
    return morphScratch_;
}

f32 AnimationSystem::morphWeight(core::InstanceId meshPart, std::string_view name) const
{
    // What a script set answers even before the mesh has loaded.
    if (const auto overrides = morphOverrides_.find(keyOf(meshPart)); overrides != morphOverrides_.end()) {
        for (const MorphOverride& set : overrides->second) {
            if (set.name == name)
                return set.weight;
        }
    }
    const SkeletonLibrary::Entry* entry = morphsOf(meshPart);
    if (entry == nullptr)
        return 0.0f;
    const core::i32 target = targetNamed(entry->morphNames, name);
    if (target < 0)
        return 0.0f;
    if (clipMorphWeights(meshPart, *entry))
        return morphScratch_[static_cast<usize>(target)];
    return static_cast<usize>(target) < entry->morphDefaults.size() ? entry->morphDefaults[static_cast<usize>(target)]
                                                                    : 0.0f;
}

void AnimationSystem::setMorphWeight(core::InstanceId meshPart, std::string_view name, f32 weight)
{
    if (!meshPart.valid() || name.empty())
        return;
    std::vector<MorphOverride>& overrides = morphOverrides_[keyOf(meshPart)];
    for (MorphOverride& set : overrides) {
        if (set.name == name) {
            set.weight = weight;
            return;
        }
    }
    overrides.push_back(MorphOverride{std::string(name), weight});
}

void AnimationSystem::clearMorphWeight(core::InstanceId meshPart, std::string_view name)
{
    const auto overrides = morphOverrides_.find(keyOf(meshPart));
    if (overrides == morphOverrides_.end())
        return;
    std::erase_if(overrides->second, [&](const MorphOverride& set) { return set.name == name; });
    if (overrides->second.empty())
        morphOverrides_.erase(overrides);
}

void AnimationSystem::present(core::InstanceId meshPart, std::span<const PresentedJoint> joints, bool over)
{
    const SkeletonLibrary::Entry* entry = skeletonOf(meshPart);
    if (entry == nullptr || joints.empty())
        return;
    const usize jointCount = entry->joints.size();
    if (!stale_.empty() && stale_.contains(keyOf(meshPart)))
        catchUp(meshPart);

    const auto held = presented_.find(keyOf(meshPart));
    const bool again = over && held != presented_.end() && held->second.model.size() == jointCount;
    if (held == presented_.end())
        presentedMeshes_.push_back(meshPart);
    Pose& drawn = again ? held->second : presented_[keyOf(meshPart)];
    const Pose* own = pose(meshPart);
    if (again) {
        // **On top of what the frame already has**: each joint from its
        // parent as that picture has it, so what is not named now stays
        // where the first presenting put it.
        for (usize joint = 0; joint < jointCount; ++joint) {
            const core::u32 parent = entry->joints[joint].parent;
            drawn.local[joint] = parent == asset::Joint::NoParent
                                     ? drawn.model[joint]
                                     : core::inverse(drawn.model[parent]) * drawn.model[joint];
        }
    }
    else if (own != nullptr && own->model.size() == jointCount && own->local.size() == jointCount) {
        drawn.palette.assign(own->palette.begin(), own->palette.end());
        drawn.model.assign(own->model.begin(), own->model.end());
        drawn.local.assign(own->local.begin(), own->local.end());
    }
    else {
        // Nothing playing: the rest pose is what it is drawn in.
        drawn.palette.assign(jointCount, Mat4{});
        drawn.model.assign(jointCount, Mat4{});
        drawn.local.assign(jointCount, Mat4{});
        for (usize joint = 0; joint < jointCount; ++joint)
            drawn.local[joint] = toMatrix(entry->joints[joint].localBind);
    }

    // The forward pass of `commitOverrides`, on the copy: a joint that is
    // named takes its place as given, and one that is not rides on its parent.
    usize next = 0;
    for (usize joint = 0; joint < jointCount; ++joint) {
        if (next < joints.size() && joints[next].joint == joint) {
            drawn.model[joint] = joints[next].model;
            ++next;
        }
        else {
            const core::u32 parent = entry->joints[joint].parent;
            drawn.model[joint] =
                parent == asset::Joint::NoParent ? drawn.local[joint] : drawn.model[parent] * drawn.local[joint];
        }
        drawn.palette[joint] = drawn.model[joint] * entry->joints[joint].inverseBind;
    }
}

bool AnimationSystem::drawnJointModel(core::InstanceId meshPart, core::u32 joint, core::CFrameD& out) const
{
    if (!presented_.empty()) {
        if (const auto found = presented_.find(keyOf(meshPart));
            found != presented_.end() && joint < found->second.model.size()) {
            out = core::cframeFromMatrix(found->second.model[joint]);
            return true;
        }
    }
    return jointModel(meshPart, joint, out);
}

std::span<const asset::Joint> AnimationSystem::jointsOf(core::InstanceId meshPart) const
{
    const SkeletonLibrary::Entry* entry = skeletonOf(meshPart);
    return entry == nullptr ? std::span<const asset::Joint>{} : std::span<const asset::Joint>{entry->joints};
}

bool AnimationSystem::modelOf(core::InstanceId meshPart, std::vector<core::Mat4>& out) const
{
    const SkeletonLibrary::Entry* entry = skeletonOf(meshPart);
    if (entry == nullptr || entry->joints.empty())
        return false;
    if (!stale_.empty() && stale_.contains(keyOf(meshPart)))
        const_cast<AnimationSystem*>(this)->catchUp(meshPart);
    const usize jointCount = entry->joints.size();
    if (const auto found = poses_.find(keyOf(meshPart));
        found != poses_.end() && found->second->model.size() == jointCount) {
        out.assign(found->second->model.begin(), found->second->model.end());
        return true;
    }
    // Nothing drives it: the rest chain, parents first.
    out.resize(jointCount);
    for (usize joint = 0; joint < jointCount; ++joint) {
        const Mat4 local = toMatrix(entry->joints[joint].localBind);
        const core::u32 parent = entry->joints[joint].parent;
        out[joint] = parent == asset::Joint::NoParent ? local : out[parent] * local;
    }
    return true;
}

core::Mat4 AnimationSystem::restModel(core::InstanceId meshPart, core::u32 joint) const
{
    const SkeletonLibrary::Entry* entry = skeletonOf(meshPart);
    return entry == nullptr || joint >= entry->joints.size() ? Mat4{} : restModelOf(*entry, joint);
}

const retarget::RigRoles* AnimationSystem::rolesOf(core::InstanceId meshPart) const
{
    const scene::MeshPartComponent* mesh = world_->meshParts().find(meshPart);
    if (mesh == nullptr || skeletons_ == nullptr || skeletons_->find(mesh->meshContent) == nullptr)
        return nullptr;
    if (skeletons_->revision() != mapsRevision_) {
        jointMaps_.clear();
        rigRoles_.clear();
        mapsRevision_ = skeletons_->revision();
    }
    return rolesFor(mesh->meshContent);
}

const Pose* AnimationSystem::drawnPose(core::InstanceId meshPart) const noexcept
{
    if (!presented_.empty()) {
        if (const auto found = presented_.find(keyOf(meshPart)); found != presented_.end())
            return &found->second;
    }
    return pose(meshPart);
}

bool AnimationSystem::jointLocal(core::InstanceId meshPart, core::u32 joint, core::CFrameD& out) const
{
    const SkeletonLibrary::Entry* entry = skeletonOf(meshPart);
    if (entry == nullptr || joint >= entry->joints.size())
        return false;
    if (!stale_.empty() && stale_.contains(keyOf(meshPart)))
        const_cast<AnimationSystem*>(this)->catchUp(meshPart);
    const Pose* own = pose(meshPart);
    if (own != nullptr && joint < own->local.size())
        out = core::cframeFromMatrix(own->local[joint]);
    else
        out = entry->joints[joint].localBind;
    return true;
}

bool AnimationSystem::seenLately(core::InstanceId meshPart) const noexcept
{
    return !seeing_ || seen_.contains(keyOf(meshPart));
}

const Pose* AnimationSystem::pose(core::InstanceId meshPart) const noexcept
{
    const auto found = poses_.find(keyOf(meshPart));
    return found == poses_.end() ? nullptr : found->second.get();
}

} // namespace engine::render
