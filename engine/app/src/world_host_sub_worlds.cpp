// `SubWorld` (ADR 0107 §3): a scene running beside this one, in a `WorldHost`
// of its own. What is here is the simulation half -- booting, ticking,
// carrying messages and input across, and throwing a world away. Drawing one is
// the frame's business (`engine.cpp`), through `ViewHost`.

#include <algorithm>
#include <array>
#include <string>

#include "engine/app/world_host.h"
#include "engine/asset/content.h"
#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/platform/file.h"

namespace engine::app {

WorldHost* WorldHost::subWorld(core::InstanceId owner) noexcept
{
    for (SubWorldRun& run : m_subWorlds) {
        if (run.owner == owner)
            return run.host.get();
    }
    return nullptr;
}

bool WorldHost::bootSubWorld(core::InstanceId owner)
{
    scene::World& w = *m_world;
    const scene::SubWorldComponent* self = w.subWorlds().find(owner);
    if (self == nullptr)
        return false;
    const std::string scenePath{w.atoms().text(self->scene)};

    // **Found as `LoadScene` finds a scene** (ADR 0106): the game's reader
    // first -- a file, then the pack -- and anything mounted after that, which
    // is how a conformance suite's own content answers.
    std::string text;
    if (!scenePath.empty()) {
        if (m_readContent) {
            if (std::optional<std::string> read = m_readContent(scenePath); read.has_value())
                text = std::move(*read);
        }
        if (text.empty() && m_mounts != nullptr) {
            const asset::ResolvedContent found = m_mounts->resolve("asset://" + scenePath);
            if (found.source == asset::ResolvedContent::Source::Pack)
                text.assign(reinterpret_cast<const char*>(found.bytes.data()), found.bytes.size());
            else if (found.source == asset::ResolvedContent::Source::Loose)
                (void)platform::readTextFile(found.path, text);
        }
        if (text.empty() && !m_root.empty())
            (void)platform::readTextFile(m_root / "content" / std::filesystem::path(scenePath), text);
    }
    if (text.empty()) {
        const std::array<core::I18nArg, 1> args{core::I18nArg{"path", scenePath}};
        core::log(core::LogLevel::Error, ENG_TR("scene.err.sub_world_scene_missing"), args);
        return false;
    }

    auto host = std::make_unique<WorldHost>();
    host->setContentMounts(m_mounts);
    // Lent, as the editor lends its own: one URN, one material, in every world
    // this process runs.
    host->setMaterialLibrary(m_materials);
    const core::u64 serial = m_nextSubWorld++;
    // Field by field rather than designated: most of the options are the
    // defaults, and a designated initialiser that skips a field with no
    // default member initializer is an error under Clang's `-Werror`.
    WorldHostOptions options;
    options.projectPath = m_projectIsDirectory ? m_root : std::filesystem::path{};
    // Its own seed, drawn from nothing but this world's and the order its
    // sub-worlds were loaded in -- a replay of this world loads them in the
    // same order.
    options.seed = m_seed * 0x9E3779B97F4A7C15ull + serial;
    options.fixedTimestep = w.engineState().fixedTimestep;
    options.bootStamps = m_stamps;
    options.bootScene = m_root / "content" / std::filesystem::path(scenePath);
    options.bootSceneText = std::move(text);
    options.bootScenePath = scenePath;
    options.readContent = m_readContent;
    options.networkTopology = scene::NetworkTopology::Solo;
    options.subWorld = true;
    if (std::optional<core::EngineError> error = host->boot(options); error.has_value()) {
        core::logText(core::LogLevel::Error, error->message);
        return false;
    }
    m_subWorlds.push_back(SubWorldRun{.owner = owner, .serial = serial, .host = std::move(host)});
    w.engineState().subWorldsLoaded.push_back(owner);
    // Deferred like every signal (ADR 0015): it drains with this world's next
    // tick, after the sub-world's scripts have had their first resumption.
    w.changes().push(scene::Change{scene::ChangeKind::InstanceEventNoArgs, owner, {}, w.atoms().intern("Loaded")});
    return true;
}

void WorldHost::stepSubWorlds()
{
    scene::World& w = *m_world;
    scene::EngineState& state = w.engineState();
    if (state.subWorldsWanted.empty() && m_subWorlds.empty())
        return;

    // A `SubWorld` destroyed is a `SubWorld` unloaded.
    std::erase_if(state.subWorldsWanted, [&w](core::InstanceId id) {
        return !w.alive(id) || w.destroyed(id) || w.subWorlds().find(id) == nullptr;
    });

    // What is no longer wanted goes first, so its budget is free for a load
    // made in the same tick.
    for (std::size_t index = m_subWorlds.size(); index-- > 0;) {
        SubWorldRun& run = m_subWorlds[index];
        if (std::find(state.subWorldsWanted.begin(), state.subWorldsWanted.end(), run.owner) !=
            state.subWorldsWanted.end())
            continue;
        run.host->closeSubWorlds();
        run.host->close(0.0);
        std::erase(state.subWorldsLoaded, run.owner);
        m_subWorlds.erase(m_subWorlds.begin() + static_cast<std::ptrdiff_t>(index));
    }

    // Then what was asked for, in the order it was asked.
    for (std::size_t index = 0; index < state.subWorldsWanted.size();) {
        const core::InstanceId owner = state.subWorldsWanted[index];
        if (subWorld(owner) != nullptr || bootSubWorld(owner)) {
            ++index;
            continue;
        }
        // A scene that would not start is not retried every tick: the next
        // `Load()` asks again.
        state.subWorldsWanted.erase(state.subWorldsWanted.begin() + static_cast<std::ptrdiff_t>(index));
    }

    // What this world gave each one: its input, then its messages.
    for (const scene::EngineState::SubWorldInput& input : state.subWorldInputs) {
        if (WorldHost* inner = subWorld(input.subWorld); inner != nullptr)
            inner->input().setActionState(input.action, input.value, input.pressed);
    }
    state.subWorldInputs.clear();
    for (scene::EngineState::SubWorldMessage& message : state.subWorldOutbox) {
        if (WorldHost* inner = subWorld(message.subWorld); inner != nullptr)
            inner->world().engineState().hostInbox.push_back(std::move(message.payload));
    }
    state.subWorldOutbox.clear();

    // **One tick for one tick** (ADR 0107 §3), at this world's step, in the
    // order they were loaded -- which is what makes a replay of this world a
    // replay of them too.
    for (SubWorldRun& run : m_subWorlds) {
        const scene::SubWorldComponent* self = w.subWorlds().find(run.owner);
        // Captured whether it steps or not: one held still has a previous
        // place that is its current one, and is drawn standing, not swinging
        // between its last two ticks.
        run.history->capture(run.host->world());
        if (self != nullptr && self->running)
            run.host->tick();

        // What it sent, for this world's next tick.
        std::vector<std::vector<core::u8>>& sent = run.host->world().engineState().hostOutbox;
        for (std::vector<core::u8>& payload : sent)
            state.subWorldInbox.push_back(scene::EngineState::SubWorldMessage{run.owner, std::move(payload)});
        sent.clear();
    }
}

void WorldHost::closeSubWorlds()
{
    for (SubWorldRun& run : m_subWorlds) {
        run.host->closeSubWorlds();
        run.host->close(0.0);
    }
    m_subWorlds.clear();
    if (m_world.has_value()) {
        scene::EngineState& state = m_world->engineState();
        state.subWorldsWanted.clear();
        state.subWorldsLoaded.clear();
        state.subWorldInputs.clear();
        state.subWorldOutbox.clear();
        state.subWorldInbox.clear();
    }
}

} // namespace engine::app
