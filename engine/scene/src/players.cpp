#include "engine/scene/players.h"

#include <algorithm>
#include <iterator>
#include <string>

#include "engine/scene/class_registry.h"
#include "engine/scene/components.h"
#include "engine/scene/world.h"

namespace engine::scene {

namespace {

// Taking part: not destroyed, and not on its way out with `PlayerRemoving`
// still to be heard -- that one can be read, and is nobody's player.
[[nodiscard]] bool present(const World& world, core::InstanceId id) noexcept
{
    if (world.destroyed(id))
        return false;
    const std::vector<core::InstanceId>& leaving = world.engineState().leavingPlayers;
    return std::find(leaving.begin(), leaving.end(), id) == leaving.end();
}

} // namespace

core::InstanceId networkServiceOf(const World& world, core::InstanceId dataModel) noexcept
{
    const ClassId networkClass = world.classes().findId(world.atoms().lookup("NetworkService"));
    if (networkClass == InvalidClass || !world.alive(dataModel))
        return {};
    return world.findFirstChildOfClass(dataModel, networkClass);
}

core::InstanceId createPlayer(World& world, core::InstanceId networkService, core::u32 userId, bool local)
{
    const ClassId playerClass = world.classes().findId(world.atoms().intern("Player"));
    if (playerClass == InvalidClass || !world.alive(networkService))
        return {};
    const core::InstanceId id = world.create(playerClass);
    if (!id.valid())
        return {};
    if (PlayerComponent* player = world.players().find(id); player != nullptr) {
        player->userId = userId;
        player->local = local;
    }
    world.setName(id, world.atoms().intern("Player" + std::to_string(userId)));
    (void)world.setParent(id, networkService);
    world.changes().push(Change{ChangeKind::InstanceEvent, networkService, id, world.atoms().intern("PlayerAdded")});
    return id;
}

void removePlayer(World& world, core::InstanceId networkService, core::InstanceId player)
{
    if (!world.alive(player))
        return;
    world.changes().push(
        Change{ChangeKind::InstanceEvent, networkService, player, world.atoms().intern("PlayerRemoving")});
    // **Whatever they owned is the authority's again** (ADR 0099): nobody is
    // left to send where it is.
    if (const PlayerComponent* leaving = world.players().find(player); leaving != nullptr && leaving->userId != 0) {
        const core::u32 userId = leaving->userId;
        world.rigidBodies().forEach([userId](core::InstanceId, RigidBodyComponent& body) {
            if (body.networkOwner == userId)
                body.networkOwner = 0;
        });
    }
    // **Out of the list now, and gone a tick later**. It was destroyed
    // here, and `PlayerRemoving` is deferred like every signal: by the time a
    // handler ran, the player it was handed was dead, and reading the id of
    // who had left -- the one thing the event is for -- raised. So the player
    // leaves the tree, which is what takes it out of `GetPlayers`, and is
    // destroyed at the start of the next tick, after the handlers.
    (void)world.setParent(player, core::InstanceId{});
    world.engineState().leavingPlayers.push_back(player);
}

void finishLeavingPlayers(World& world)
{
    std::vector<core::InstanceId> leaving;
    leaving.swap(world.engineState().leavingPlayers);
    for (const core::InstanceId player : leaving) {
        if (world.alive(player))
            (void)world.destroy(player);
    }
}

core::InstanceId localPlayerOf(const World& world) noexcept
{
    core::InstanceId found;
    world.players().forEach([&](core::InstanceId id, const PlayerComponent& player) {
        if (!found.valid() && player.local && present(world, id))
            found = id;
    });
    return found;
}

void localPlayers(const World& world, std::vector<core::InstanceId>& out)
{
    out.clear();
    const core::InstanceId first = localPlayerOf(world);
    if (!first.valid())
        return;
    const auto& leaving = world.engineState().leavingPlayers;
    // Child order is join order: the engine parents each player as it arrives.
    for (core::InstanceId child = world.firstChild(world.parentOf(first)); child.valid();
         child = world.nextSibling(child)) {
        const PlayerComponent* player = world.players().find(child);
        if (player != nullptr && player->local && !world.destroyed(child) &&
            std::find(leaving.begin(), leaving.end(), child) == leaving.end())
            out.push_back(child);
    }
}

core::InstanceId playerByUserId(const World& world, core::u32 userId) noexcept
{
    core::InstanceId found;
    world.players().forEach([&](core::InstanceId id, const PlayerComponent& player) {
        if (!found.valid() && player.userId == userId && present(world, id))
            found = id;
    });
    return found;
}

void captureLocalIntents(World& world)
{
    const core::InstanceId localId = localPlayerOf(world);
    world.players().forEach([&](core::InstanceId id, PlayerComponent& player) {
        if (player.local && present(world, id)) {
            player.intents.clear();
            player.intentTick = world.engineState().tick;
        }
    });
    world.inputActions().forEach([&](core::InstanceId id, const InputActionComponent& action) {
        if (!action.enabled || world.destroyed(id))
            return;
        // Only what the simulation clock resolves: a Render-rate action is a
        // camera or a menu, which is this machine's business and not a fact
        // about what the player did in the world (ADR 0039).
        const InputContextComponent* context = world.inputContexts().find(world.parentOf(id));
        if (context == nullptr || context->rate != 0 || !context->enabled)
            return;
        const core::InstanceId owner = context->player.valid() ? context->player : localId;
        PlayerComponent* local = world.players().find(owner);
        if (local == nullptr || !local->local || !present(world, owner))
            return;
        const core::NameAtom name = world.name(id);
        const bool seen = std::any_of(local->intents.begin(), local->intents.end(),
                                      [name](const PlayerIntent& intent) { return intent.action == name; });
        if (seen)
            return;
        local->intents.push_back(PlayerIntent{name, action.type, action.axis, action.pressed});
    });
}

void restLocalIntents(World& world)
{
    world.players().forEach([&](core::InstanceId id, PlayerComponent& player) {
        if (!player.local || !present(world, id))
            return;
        for (PlayerIntent& intent : player.intents) {
            intent.axis = core::Vec3{};
            intent.pressed = false;
        }
    });
}

void restLocalCharacters(World& world)
{
    world.players().forEach([&](core::InstanceId id, const PlayerComponent& player) {
        if (!player.local || !present(world, id))
            return;
        if (CharacterBodyComponent* body = world.characterBodies().find(player.character); body != nullptr) {
            body->moveDirection = core::Vec3{};
            body->jumpRequested = false;
        }
    });
}

void suspect(World& world, core::InstanceId player, std::string_view kind, std::string detail, std::string who)
{
    constexpr std::size_t Kept = 1024;
    EngineState& state = world.engineState();
    if (who.empty() && player.valid()) {
        if (const PlayerComponent* found = world.players().find(player); found != nullptr) {
            if (!found->accountId.empty())
                who = "account:" + found->accountId;
            else if (!found->publicKey.empty())
                who = "key:" + found->publicKey;
        }
    }
    state.suspicions.push_back(
        EngineState::Suspicion{player, std::move(who), std::string(kind), std::move(detail), state.tick});
    if (state.suspicions.size() > Kept)
        state.suspicions.erase(state.suspicions.begin(),
                               state.suspicions.begin() + static_cast<std::ptrdiff_t>(state.suspicions.size() - Kept));
    state.suspicionsTotal += 1;
    if (const auto found = state.suspicionsByKind.find(kind); found != state.suspicionsByKind.end())
        found->second += 1;
    else
        state.suspicionsByKind.emplace(std::string(kind), 1);
}

} // namespace engine::scene
