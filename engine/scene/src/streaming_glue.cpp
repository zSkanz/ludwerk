#include "engine/scene/streaming_glue.h"

#include <algorithm>
#include <chrono>
#include <string>

#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"

namespace engine::scene {
namespace {

[[nodiscard]] core::f64 nowMs() noexcept
{
    // A frame budget rather than simulation state, exactly as
    // `asset::StreamingManager` explains: which instances a chunk creates is
    // decided by the chunk, and only how many chunks fit in a frame depends on
    // the clock.
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<core::f64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()) / 1.0e6;
}

} // namespace

StreamingGlue::StreamingGlue(World& world, core::InstanceId root) : m_world(world), m_root(root)
{}

core::f64 StreamingGlue::materialize(asset::ChunkId id, const asset::Chunk& chunk)
{
    const core::f64 started = nowMs();

    if (m_chunks.find(id) != m_chunks.end()) {
        // Already resident. Not an error: a manager that asked twice would be a
        // bug, but silently building a second copy of a chunk would be a world
        // with two of everything and no way to tell.
        return 0.0;
    }

    const ClassId folderClass = m_world.classes().findId(m_world.atoms().intern("Folder"));
    const ClassId partClass = m_world.classes().findId(m_world.atoms().intern("Part"));
    // Resolved but not required: a chunk of nothing but `Part`s must not fail
    // because a registry has no `MeshPart` in it. A class is a dependency of
    // the instances that need it, not of the chunk that might have had one.
    const ClassId meshPartClass = m_world.classes().findId(m_world.atoms().intern("MeshPart"));
    const ClassId modelClass = m_world.classes().findId(m_world.atoms().intern("Model"));
    if (folderClass == InvalidClass || partClass == InvalidClass) {
        return 0.0;
    }

    Resident resident;
    // **A folder for the cell, only for what has nowhere authored to go**: a
    // generated world's records, which no scene ever held. Named for its
    // coordinates; what eviction removes when nothing inside it is held.
    const auto cellFolder = [&]() {
        if (resident.folder.valid())
            return resident.folder;
        resident.folder = m_world.create(folderClass);
        if (!resident.folder.valid())
            return resident.folder;
        const std::string folderName =
            "Chunk_" + std::to_string(id.x) + "_" + std::to_string(id.z) + "_" + std::to_string(id.layer);
        m_world.setName(resident.folder, m_world.atoms().intern(folderName));
        (void)m_world.setParent(resident.folder, m_root);
        // Made by streaming, not authored by anybody, so a scene does not
        // record it -- nor anything under it.
        m_world.setGenerated(resident.folder, true);
        return resident.folder;
    };

    // **Where each group's records are born** (D422), by the group's index:
    // an authored instance that stayed in the scene, a `Model` made here, or
    // the cell's folder. And whether they are born at all: a group whose
    // authored parent a script destroyed is `gone`, because what its parts
    // were part of is -- a felled tree does not grow back when its cell comes
    // round again.
    //
    // An atomic model's `Model` is made first, so its parts have somewhere to
    // be born (ADR 0053): the whole of it arrives in this call or none does.
    std::vector<core::InstanceId> parents;
    std::vector<bool> gone;
    parents.reserve(chunk.groups.size());
    gone.reserve(chunk.groups.size());
    for (const asset::ChunkGroup& group : chunk.groups) {
        const bool anchored = group.anchor != asset::ChunkGroup::NoAnchor;
        const core::InstanceId anchor = anchored ? m_world.streamAnchor(group.anchor) : core::InstanceId{};
        if (anchored && !anchor.valid()) {
            parents.push_back(core::InstanceId{});
            gone.push_back(true);
            continue;
        }
        if (group.existing) {
            parents.push_back(anchor);
            gone.push_back(false);
            continue;
        }
        const core::InstanceId model = modelClass != InvalidClass ? m_world.create(modelClass) : core::InstanceId{};
        if (model.valid()) {
            const std::string_view name = chunk.stringAt(group.name);
            if (!name.empty()) {
                m_world.setName(model, m_world.atoms().intern(name));
            }
            (void)m_world.setParent(model, anchored ? anchor : cellFolder());
            // Under an authored parent it is its own mark that keeps it out
            // of a saved scene; under the cell's folder the folder's does.
            m_world.setGenerated(model, true);
            resident.models.push_back(model);
        }
        // Pushed even when it is invalid, so a record's `group` stays an index
        // into this vector. A model that could not be created leaves its parts
        // in the chunk folder, which is a flatter world rather than a missing
        // one.
        parents.push_back(model);
        gone.push_back(false);
    }

    resident.instances.reserve(chunk.instances.size());
    for (const asset::ChunkInstance& source : chunk.instances) {
        if (source.group < gone.size() && gone[source.group])
            continue;
        const bool isMesh = source.kind == asset::ChunkInstance::Kind::MeshPart && meshPartClass != InvalidClass;
        const core::InstanceId id2 = m_world.create(isMesh ? meshPartClass : partClass);
        if (!id2.valid()) {
            continue;
        }

        if (PartComponent* part = m_world.parts().find(id2); part != nullptr) {
            part->cframe = source.cframe;
            part->size = source.size;
            // The record's colour and transparency are the default material's
            // two parameters; the defaults are not overrides.
            asset::MaterialProperties values;
            values.color = source.color;
            values.transparency = source.transparency;
            if (!(source.color == core::Color3{1.0f, 1.0f, 1.0f}))
                (void)asset::setOverride(part->materialParameters, asset::MaterialField::Color, values);
            if (source.transparency != 0.0f)
                (void)asset::setOverride(part->materialParameters, asset::MaterialField::Transparency, values);
            part->shape = static_cast<core::i32>(source.shape);
            part->castShadow = source.castShadow;
            part->receivesDecals = source.receivesDecals;
        }
        if (isMesh) {
            if (MeshPartComponent* mesh = m_world.meshParts().find(id2); mesh != nullptr) {
                mesh->meshContent = m_world.atoms().intern(chunk.stringAt(source.meshContent));
                mesh->collisionFidelity = static_cast<core::i32>(source.collisionFidelity);
            }
        }
        if (RigidBodyComponent* body = m_world.rigidBodies().find(id2); body != nullptr) {
            body->anchored = source.anchored;
            body->canCollide = source.canCollide;
            body->canQuery = source.canQuery;
            body->friction = source.friction;
            body->restitution = source.restitution;
            body->density = source.density;
            // An absent group is the world's `Default`, which is what the
            // component was already constructed with -- naming it again would
            // intern a string on every record of every chunk for no change.
            if (const std::string_view group = chunk.stringAt(source.collisionGroup); !group.empty()) {
                body->collisionGroup = m_world.atoms().intern(group);
            }
        }

        const std::string_view name = chunk.stringAt(source.name);
        if (!name.empty()) {
            m_world.setName(id2, m_world.atoms().intern(name));
        }

        // **Tags, which are how a script finds any of this** (ADR 0053). A path
        // into `Workspace` is sometimes nil in a world that is not all present;
        // `TagService`'s added and removed signals are the answer, and they fire
        // from here because `World::addTag` enqueues a change like any other.
        for (core::u32 slot = 0; slot < source.tagCount; ++slot) {
            const usize at2 = static_cast<usize>(source.firstTag) + slot;
            if (at2 >= chunk.tagRefs.size()) {
                break;
            }
            const std::string_view tag = chunk.stringAt(chunk.tagRefs[at2]);
            if (!tag.empty()) {
                (void)m_world.addTag(id2, m_world.atoms().intern(tag));
            }
        }

        const core::InstanceId parent =
            source.group < parents.size() && parents[source.group].valid() ? parents[source.group] : cellFolder();
        // Made by streaming wherever it is born: a scene saved with it
        // resident does not write it a second time.
        m_world.setGenerated(id2, true);
        (void)m_world.setParent(id2, parent);
        resident.instances.push_back(id2);
    }

    m_residentInstances += static_cast<u32>(resident.instances.size());
    m_chunks.emplace(id, std::move(resident));
    return nowMs() - started;
}

void StreamingGlue::evict(asset::ChunkId id)
{
    const auto at = m_chunks.find(id);
    if (at == m_chunks.end()) {
        return;
    }

    for (const core::InstanceId instance : at->second.instances) {
        // §4's contract, and the reason this is not just a destroy: an instance
        // a script still holds would leave the Luau side with a handle to
        // nothing, so it is reparented to nil and becomes a husk. That is
        // exactly what `StreamingService.InstanceStreamedOut` reports.
        if (m_probe && m_probe(instance)) {
            (void)m_world.setParent(instance, core::InstanceId{});
            m_streamedOut.push_back(instance);
            m_husksCreated += 1;
            continue;
        }
        (void)m_world.destroy(instance);
    }

    m_residentInstances -= std::min<u32>(m_residentInstances, static_cast<u32>(at->second.instances.size()));

    // A group's `Model` goes the way its folder does, and for the same reason:
    // empty means nothing is left in it that was not this chunk's. An authored
    // instance records were born under is the scene's, and stays.
    for (const core::InstanceId model : at->second.models) {
        if (m_world.alive(model) && m_world.childCount(model) == 0) {
            (void)m_world.destroy(model);
        }
    }

    // The folder goes only when it is EMPTY, which is a narrower condition than
    // "nothing was kept": a husk reparented to nil has already left it, but
    // something a script parented in there has not -- and destroying the folder
    // out from under that is how a streamed world eats things that were never
    // its to remove.
    if (at->second.folder.valid() && m_world.alive(at->second.folder) && m_world.childCount(at->second.folder) == 0) {
        (void)m_world.destroy(at->second.folder);
    }

    m_chunks.erase(at);
}

std::vector<core::InstanceId> StreamingGlue::drainStreamedOut()
{
    std::vector<core::InstanceId> drained;
    drained.swap(m_streamedOut);
    return drained;
}

void StreamingGlue::clear()
{
    for (const auto& entry : m_chunks) {
        for (const core::InstanceId instance : entry.second.instances) {
            (void)m_world.destroy(instance);
        }
        for (const core::InstanceId model : entry.second.models) {
            (void)m_world.destroy(model);
        }
        if (entry.second.folder.valid())
            (void)m_world.destroy(entry.second.folder);
    }
    m_chunks.clear();
    m_streamedOut.clear();
    m_residentInstances = 0;
}

} // namespace engine::scene
