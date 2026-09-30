// A replica's own scripts, put back under what the authority sends (ADR 0138
// §6).
//
// **Scripts never cross the wire**, and that stays so: a server that could send
// code could run code on every player. So a replica runs the client and shared
// scripts of its OWN package, and this is where it keeps them between the
// moment a join clears its copy of the scene and the moment the authority's
// instances arrive. Each is keyed by where its parent was authored -- the tree
// of the scene a read made it in, or the stamp `Instance.stamp` placed it from,
// and the parent's preorder place there (`World::Origin`) -- which the
// authority sends with every spawn.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine/core/id.h"
#include "engine/scene/scene_file.h"
#include "engine/scene/world.h"

namespace engine::replication {

class ScriptTemplates
{
public:
    // Where a stamp's text comes from: the same source `Instance.stamp` reads,
    // this machine's own package.
    void setStampSource(scene::StampSource source) { m_stamps = std::move(source); }

    // **Takes every script out of the subtree at `root`** before a clear
    // destroys it: each topmost `Script` or `ModuleScript` whose parent was
    // authored is detached and kept, keyed by that parent's origin. A script
    // whose parent was made at run time goes with it, because nothing the
    // authority sends could name that parent.
    void takeFrom(scene::World& world, core::InstanceId root);

    // **A clear that destroyed something replaces the scene's templates**: the
    // scene the replica holds now is the one the authority is in, and the
    // previous one's keys would land on this one's instances. Called by
    // `clearForReplica` before its first destroy, and never when it found
    // nothing to clear -- a replica's boot clears, and the socket opening clears
    // again and finds nothing.
    void dropSceneTemplates(scene::World& world);

    // Everything held, destroyed: back in solo, nothing here is needed.
    void clear(scene::World& world);

    // **Puts under `instance` a copy of each script its origin holds**, reading a
    // stamp from this machine's package the first time one of its instances
    // arrives. Returns how many were put there. The scripts start as S1 starts
    // any script that comes into the world, once `instance` is in it.
    core::usize attach(scene::World& world, core::InstanceId instance, scene::World::Origin origin);

    // **The atom a spawn's `stamp:` origin names, or none** -- interned only
    // when this machine's package holds that stamp. A name interned is kept
    // for ever, so one a hostile server made up must not be: every spawn would
    // grow the table (the script-sides audit, S3).
    // A name the package does not hold is remembered, up to a bound, and past
    // it none is read: a server that names a thousand is answered from
    // memory, not from the disk a thousand times.
    [[nodiscard]] core::NameAtom stampAsset(scene::World& world, std::string_view origin);

    [[nodiscard]] core::usize size() const noexcept;

private:
    using Key = std::pair<core::u32, core::u32>;

    void keep(scene::World& world, core::InstanceId script, scene::World::Origin parent);
    void readStamp(scene::World& world, core::NameAtom asset);

    scene::StampSource m_stamps;
    std::map<Key, std::vector<core::InstanceId>> m_templates;
    // Stamps read already, whether or not they carried a script.
    std::vector<core::u32> m_stampsRead;
    // Names asked for that this package does not hold, up to `MaxMissing`.
    std::vector<std::string> m_missing;
};

} // namespace engine::replication
