#include "engine/replication/script_templates.h"

#include <algorithm>
#include <optional>
#include <string_view>

namespace engine::replication {

namespace {

[[nodiscard]] bool isScript(const scene::World& world, core::InstanceId id)
{
    const scene::ClassId classId = world.classOf(id);
    return classId == world.classes().findId(world.atoms().lookup("Script")) ||
           classId == world.classes().findId(world.atoms().lookup("ModuleScript"));
}

[[nodiscard]] bool isSceneAsset(const scene::World& world, core::NameAtom asset)
{
    return asset.valid() && world.atoms().text(asset).starts_with("scene:");
}

// The topmost scripts under `root`, in preorder: a script inside a script
// travels with its holder.
void collectScripts(const scene::World& world, core::InstanceId root, std::vector<core::InstanceId>& out)
{
    for (core::InstanceId child = world.firstChild(root); child.valid(); child = world.nextSibling(child)) {
        if (isScript(world, child))
            out.push_back(child);
        else
            collectScripts(world, child, out);
    }
}

} // namespace

void ScriptTemplates::keep(scene::World& world, core::InstanceId script, scene::World::Origin parent)
{
    // Detached, so it is never live (ADR 0137 §1): a template does not run.
    (void)world.setParent(script, core::InstanceId{});
    m_templates[Key{parent.asset.id, parent.index}].push_back(script);
}

void ScriptTemplates::takeFrom(scene::World& world, core::InstanceId root)
{
    std::vector<core::InstanceId> scripts;
    if (isScript(world, root))
        scripts.push_back(root);
    else
        collectScripts(world, root, scripts);
    for (const core::InstanceId script : scripts) {
        const scene::World::Origin parent = world.originOf(world.parentOf(script));
        if (parent.asset.valid())
            keep(world, script, parent);
    }
}

void ScriptTemplates::dropSceneTemplates(scene::World& world)
{
    for (auto it = m_templates.begin(); it != m_templates.end();) {
        if (isSceneAsset(world, core::NameAtom{it->first.first})) {
            for (const core::InstanceId script : it->second) {
                if (world.alive(script))
                    (void)world.destroy(script);
            }
            it = m_templates.erase(it);
        }
        else {
            ++it;
        }
    }
}

void ScriptTemplates::clear(scene::World& world)
{
    for (const auto& [key, scripts] : m_templates) {
        for (const core::InstanceId script : scripts) {
            if (world.alive(script))
                (void)world.destroy(script);
        }
    }
    m_templates.clear();
    m_stampsRead.clear();
}

void ScriptTemplates::readStamp(scene::World& world, core::NameAtom asset)
{
    if (std::find(m_stampsRead.begin(), m_stampsRead.end(), asset.id) != m_stampsRead.end())
        return;
    m_stampsRead.push_back(asset.id);
    if (!m_stamps)
        return;
    constexpr std::string_view Prefix = "stamp:";
    const std::string_view text = world.atoms().text(asset);
    if (!text.starts_with(Prefix))
        return;
    const std::string name(text.substr(Prefix.size()));
    const std::optional<std::string> source = m_stamps(name);
    if (!source.has_value())
        return;
    // **Built as `Instance.stamp` builds it, and numbered the same way**, so a
    // place in this copy is the same place in the authority's.
    scene::SceneIoReport report;
    const core::InstanceId placed = scene::readStamp(world, *source, core::InstanceId{}, name, &report);
    if (!placed.valid())
        return;
    (void)world.numberOrigins(placed, asset, 0);
    takeFrom(world, placed);
    (void)world.destroy(placed);
}

core::usize ScriptTemplates::attach(scene::World& world, core::InstanceId instance, scene::World::Origin origin)
{
    if (!origin.asset.valid() || !world.alive(instance))
        return 0;
    if (!isSceneAsset(world, origin.asset))
        readStamp(world, origin.asset);
    const auto found = m_templates.find(Key{origin.asset.id, origin.index});
    if (found == m_templates.end())
        return 0;
    core::usize put = 0;
    for (const core::InstanceId script : found->second) {
        if (!world.alive(script))
            continue;
        const core::InstanceId copy = world.clone(script);
        if (copy.valid() && !world.setParent(copy, instance).has_value())
            ++put;
    }
    return put;
}

core::NameAtom ScriptTemplates::stampAsset(scene::World& world, std::string_view origin)
{
    if (const core::NameAtom known = world.atoms().lookup(origin); known.valid())
        return known;
    constexpr std::string_view Prefix = "stamp:";
    constexpr core::usize MaxMissing = 64;
    if (!origin.starts_with(Prefix) || !m_stamps)
        return {};
    if (m_missing.size() >= MaxMissing || std::find(m_missing.begin(), m_missing.end(), origin) != m_missing.end())
        return {};
    if (!m_stamps(std::string(origin.substr(Prefix.size()))).has_value()) {
        m_missing.emplace_back(origin);
        return {};
    }
    return world.atoms().intern(origin);
}

core::usize ScriptTemplates::size() const noexcept
{
    core::usize count = 0;
    for (const auto& [key, scripts] : m_templates)
        count += scripts.size();
    return count;
}

} // namespace engine::replication
