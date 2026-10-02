#include "engine/scene/voxel_fluid.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace engine::scene {
namespace {

using asset::BlockId;
using core::i32;
using core::u32;
using core::u64;
using core::usize;
using Position = std::array<i32, 3>;

constexpr std::array<Position, 6> Neighbours{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
constexpr std::array<std::array<i32, 2>, 4> Sides{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};

// **Every block whose decision reads a given block**, as offsets from it: the
// block itself, its six neighbours, and the four diagonally above -- a block
// reads what is UNDER each of its sides, to tell a fluid that stands on
// something and spreads from one that is pouring past. The last four were
// never woken: a floor put under a pouring source left the air beside the
// source dry until something else disturbed it.
constexpr std::array<Position, 11> Readers{{{0, 0, 0},
                                            {1, 0, 0},
                                            {-1, 0, 0},
                                            {0, 1, 0},
                                            {0, -1, 0},
                                            {0, 0, 1},
                                            {0, 0, -1},
                                            {1, 1, 0},
                                            {-1, 1, 0},
                                            {0, 1, 1},
                                            {0, 1, -1}}};

// The bits of the fluid types in `VoxelChunk::typesSeen`; zero when the
// registry holds no fluid.
[[nodiscard]] u64 fluidBits(const VoxelComponent& voxels) noexcept
{
    u64 bits = 0;
    for (usize at = 0; at < voxels.types.size(); ++at) {
        if (voxels.types[at].fluidReach > 0)
            bits |= asset::blockTypeBit(static_cast<BlockId>(at + 1));
    }
    return bits;
}

// Whether any chunk the box touches may hold a fluid. No is certain.
[[nodiscard]] bool fluidNear(const VoxelComponent& voxels, u64 bits, i32 minX, i32 minY, i32 minZ, i32 maxX, i32 maxY,
                             i32 maxZ) noexcept
{
    const asset::VoxelChunkKey low = asset::voxelChunkOf(minX, minY, minZ);
    const asset::VoxelChunkKey high = asset::voxelChunkOf(maxX, maxY, maxZ);
    // A box wider than the world has chunks is asked of the chunks instead.
    const u64 spanned = static_cast<u64>(high.x - low.x + 1) * static_cast<u64>(high.y - low.y + 1) *
                        static_cast<u64>(high.z - low.z + 1);
    if (spanned > voxels.grid.chunkCount()) {
        for (const asset::VoxelGrid::Entry& entry : voxels.grid.chunks()) {
            const asset::VoxelChunkKey& key = entry.first;
            if (key.x >= low.x && key.x <= high.x && key.y >= low.y && key.y <= high.y && key.z >= low.z &&
                key.z <= high.z && (entry.second->typesSeen & bits) != 0)
                return true;
        }
        return false;
    }
    for (i32 y = low.y; y <= high.y; ++y) {
        for (i32 z = low.z; z <= high.z; ++z) {
            for (i32 x = low.x; x <= high.x; ++x) {
                const asset::VoxelChunk* chunk = voxels.grid.findChunk(asset::VoxelChunkKey{x, y, z});
                if (chunk != nullptr && (chunk->typesSeen & bits) != 0)
                    return true;
            }
        }
    }
    return false;
}

[[nodiscard]] BlockId decide(const VoxelComponent& voxels, const Position& at);

// **Asks for `at` to be looked at by `due` -- if a look would change it**
// (D448). The step decides a block from the grid as it stands; so does this,
// and a block whose decision is what it already is, is not queued.
//
// Every write used to queue itself and its neighbours whenever the registry
// held a fluid TYPE, whether or not there was fluid within a mile: a mountain
// of stone built beside a registered sea paid a map and a set insert for
// every block of its surface -- seventy thousand a column, twenty times the
// cost of the build -- and the step then read each one to find nothing to do.
// What changes the answer later is a write, and every write asks again, of
// every block that reads it (`Readers`).
void consider(VoxelComponent& voxels, const Position& at, u64 due)
{
    const BlockId here = voxels.grid.get(at[0], at[1], at[2]);
    if (here != asset::AirBlock) {
        // Solid is no fluid's business, and a source stays a source unless
        // something it touches turns it into something else.
        if (!isFluidType(voxels, asset::blockTypeOf(here)))
            return;
        if (asset::blockStateOf(here) == 0 && voxels.fluidReactions.empty())
            return;
    }
    if (decide(voxels, at) != here)
        voxels.fluidWakes.schedule(at, due);
}

void wakeAround(VoxelComponent& voxels, const Position& at, u64 due)
{
    for (const Position& step : Readers)
        consider(voxels, Position{at[0] + step[0], at[1] + step[1], at[2] + step[2]}, due);
}

// What the block at `at` should be by the flow rules alone, from the grid as it
// stands.
[[nodiscard]] BlockId decideFlow(const VoxelComponent& voxels, const Position& at)
{
    const asset::VoxelGrid& grid = voxels.grid;
    const BlockId here = grid.get(at[0], at[1], at[2]);
    const BlockId hereType = asset::blockTypeOf(here);
    const bool hereFluid = here != asset::AirBlock && isFluidType(voxels, hereType);
    // Solid blocks are no fluid's business, and a source stays a source.
    if (here != asset::AirBlock && !hereFluid)
        return here;
    if (hereFluid && asset::blockStateOf(here) == 0)
        return here;

    // Whether the fluid at `side` stands on something and can therefore spread:
    // a fluid over air pours rather than spreading, and one over its own
    // flowing water is still being fed from above.
    const auto spreads = [&](i32 x, i32 y, i32 z) {
        const BlockId below = grid.get(x, y - 1, z);
        if (below == asset::AirBlock)
            return false;
        const BlockId self = grid.get(x, y, z);
        return !(asset::blockTypeOf(below) == asset::blockTypeOf(self) && asset::blockStateOf(below) != 0);
    };

    // Which fluid this block is about: its own; else the one above it; else
    // the lowest-numbered type beside it that could spread here, so two fluids
    // meeting at a block settle it the same way everywhere.
    const BlockId above = grid.get(at[0], at[1] + 1, at[2]);
    const BlockId aboveType = asset::blockTypeOf(above);
    BlockId fluid = hereFluid ? hereType : asset::AirBlock;
    if (fluid == asset::AirBlock && above != asset::AirBlock && isFluidType(voxels, aboveType))
        fluid = aboveType;
    if (fluid == asset::AirBlock) {
        for (const std::array<i32, 2>& side : Sides) {
            const BlockId next = grid.get(at[0] + side[0], at[1], at[2] + side[1]);
            const BlockId nextType = asset::blockTypeOf(next);
            if (next == asset::AirBlock || !isFluidType(voxels, nextType))
                continue;
            if (!spreads(at[0] + side[0], at[1], at[2] + side[1]))
                continue;
            if (fluid == asset::AirBlock || nextType < fluid)
                fluid = nextType;
        }
    }
    if (fluid == asset::AirBlock)
        return here;

    // Down first: fed from above, a block falls, and a falling block is full.
    if (above != asset::AirBlock && aboveType == fluid)
        return asset::blockWithState(fluid, asset::FluidFalling);

    // Sideways: one level weaker than the strongest neighbour that spreads, a
    // falling block landing counting as a source.
    u32 nearest = asset::MaxFluidReach + 1;
    for (const std::array<i32, 2>& side : Sides) {
        const BlockId next = grid.get(at[0] + side[0], at[1], at[2] + side[1]);
        if (next == asset::AirBlock || asset::blockTypeOf(next) != fluid)
            continue;
        if (!spreads(at[0] + side[0], at[1], at[2] + side[1]))
            continue;
        const u32 state = asset::blockStateOf(next);
        const u32 level = (state & asset::FluidFalling) != 0 ? 0u : (state & asset::FluidLevelMask);
        nearest = std::min(nearest, level);
    }
    const u32 reach = voxels.types[fluid - 1u].fluidReach;
    if (nearest + 1 <= reach)
        return asset::blockWithState(fluid, nearest + 1);
    // Nothing feeds it: it drains.
    return asset::AirBlock;
}

// What `fluid` at `at` turns into because of what it touches, or air when it
// touches nothing it reacts with. The six neighbours in a fixed order, and the
// first reaction found wins -- the same one on every machine.
[[nodiscard]] BlockId reactionAt(const VoxelComponent& voxels, const Position& at, BlockId fluid)
{
    if (voxels.fluidReactions.empty())
        return asset::AirBlock;
    for (const Position& step : Neighbours) {
        const BlockId next = voxels.grid.get(at[0] + step[0], at[1] + step[1], at[2] + step[2]);
        const BlockId touching = asset::blockTypeOf(next);
        if (next == asset::AirBlock || touching == fluid)
            continue;
        const auto found =
            std::lower_bound(voxels.fluidReactions.begin(), voxels.fluidReactions.end(), std::pair{fluid, touching},
                             [](const VoxelComponent::FluidReaction& entry, const std::pair<BlockId, BlockId>& key) {
                                 return std::pair{entry.from, entry.touching} < key;
                             });
        if (found != voxels.fluidReactions.end() && found->from == fluid && found->touching == touching)
            return found->result;
    }
    return asset::AirBlock;
}

// What the block at `at` should be, from the grid as it stands. Pure: the step
// decides every block it visits this way before writing any of them.
[[nodiscard]] BlockId decide(const VoxelComponent& voxels, const Position& at)
{
    const BlockId flowed = decideFlow(voxels, at);
    // **A fluid that touches one it reacts with becomes the reaction's block**,
    // whether it was already here or has just flowed in: lava reaching water
    // sets as stone where the two meet, and the water stays water.
    const BlockId type = asset::blockTypeOf(flowed);
    if (flowed != asset::AirBlock && isFluidType(voxels, type)) {
        if (const BlockId result = reactionAt(voxels, at, type); result != asset::AirBlock)
            return result;
    }
    return flowed;
}

} // namespace

bool isFluidType(const VoxelComponent& voxels, asset::BlockId type) noexcept
{
    return type != asset::AirBlock && type <= voxels.types.size() && voxels.types[type - 1u].fluidReach > 0;
}

void setFluidReaction(VoxelComponent& voxels, asset::BlockId from, asset::BlockId touching, asset::BlockId result)
{
    std::vector<VoxelComponent::FluidReaction>& list = voxels.fluidReactions;
    const auto at =
        std::lower_bound(list.begin(), list.end(), std::pair{from, touching},
                         [](const VoxelComponent::FluidReaction& entry, const std::pair<BlockId, BlockId>& key) {
                             return std::pair{entry.from, entry.touching} < key;
                         });
    const bool exists = at != list.end() && at->from == from && at->touching == touching;
    if (result == asset::AirBlock) {
        if (exists)
            list.erase(at);
        return;
    }
    if (exists)
        at->result = result;
    else
        list.insert(at, VoxelComponent::FluidReaction{from, touching, result});
}

void wakeFluids(VoxelComponent& voxels, i32 x, i32 y, i32 z)
{
    // Nothing to do where no fluid can be: not in the registry, or not in any
    // chunk this block's readers are in.
    const u64 bits = fluidBits(voxels);
    if (bits == 0 || !fluidNear(voxels, bits, x - 1, y - 1, z - 1, x + 1, y + 1, z + 1))
        return;
    // Due at once: the next step, whatever tick it runs on.
    wakeAround(voxels, Position{x, y, z}, 0);
}

void wakeFluidsInBox(VoxelComponent& voxels, i32 minX, i32 minY, i32 minZ, i32 maxX, i32 maxY, i32 maxZ)
{
    const u64 bits = fluidBits(voxels);
    if (bits == 0)
        return;
    if (minX > maxX)
        std::swap(minX, maxX);
    if (minY > maxY)
        std::swap(minY, maxY);
    if (minZ > maxZ)
        std::swap(minZ, maxZ);
    // **Building dry land costs no fluid work at all**: a box none of whose
    // chunks, nor the ones around it, may hold a fluid is left without a
    // block of it being read.
    if (!fluidNear(voxels, bits, minX - 1, minY - 1, minZ - 1, maxX + 1, maxY + 1, maxZ + 1))
        return;
    // The box's own shell and the layer outside it, face by face, so a box a
    // thousand blocks across costs its surface and not its volume. Each is
    // queued only if a look would change it: a sea filled against stone and
    // against more sea queues its open faces and nothing else.
    for (i32 y = minY - 1; y <= maxY + 1; ++y) {
        for (i32 z = minZ - 1; z <= maxZ + 1; ++z) {
            const bool edgeRow = y <= minY || y >= maxY || z <= minZ || z >= maxZ;
            if (edgeRow) {
                for (i32 x = minX - 1; x <= maxX + 1; ++x)
                    consider(voxels, Position{x, y, z}, 0);
                continue;
            }
            for (const i32 x : {minX - 1, minX, maxX, maxX + 1})
                consider(voxels, Position{x, y, z}, 0);
        }
    }
}

void wakeAllFluids(VoxelComponent& voxels)
{
    wakeFluidsIn(voxels, voxels.grid);
}

void wakeFluidsIn(VoxelComponent& voxels, const asset::VoxelGrid& arrived)
{
    const u64 bits = fluidBits(voxels);
    if (bits == 0)
        return;
    const auto edge = static_cast<i32>(asset::VoxelChunkEdge);
    for (const asset::VoxelChunkKey key : arrived.chunkKeys()) {
        const asset::VoxelChunk* chunk = arrived.findChunk(key);
        // A chunk never given a fluid is not read.
        if (chunk == nullptr || (chunk->typesSeen & bits) == 0)
            continue;
        for (i32 y = 0; y < edge; ++y) {
            for (i32 z = 0; z < edge; ++z) {
                for (i32 x = 0; x < edge; ++x) {
                    const BlockId id =
                        chunk->blocks[asset::voxelIndex(static_cast<u32>(x), static_cast<u32>(y), static_cast<u32>(z))];
                    if (id != asset::AirBlock && isFluidType(voxels, asset::blockTypeOf(id)))
                        wakeAround(voxels, Position{key.x * edge + x, key.y * edge + y, key.z * edge + z}, 0);
                }
            }
        }
    }
}

u32 stepFluids(VoxelComponent& voxels, u64 tick)
{
    if (voxels.fluidWakes.empty())
        return 0;

    // What is due, in position order, up to the budget.
    const std::vector<Position> due = voxels.fluidWakes.takeDue(tick, MaxFluidUpdatesPerTick);
    if (due.empty())
        return 0;

    // Decide everything from the grid as it stood, then write everything.
    std::vector<std::pair<Position, BlockId>> writes;
    for (const Position& at : due) {
        const BlockId next = decide(voxels, at);
        if (next != voxels.grid.get(at[0], at[1], at[2]))
            writes.emplace_back(at, next);
    }
    // Everything written, and only then who is due because of it: asked of
    // the grid as the whole step left it, a block two of the writes touch is
    // judged once, by what it now reads.
    std::vector<u64> paces;
    paces.reserve(writes.size());
    for (const auto& [at, next] : writes) {
        const BlockId before = voxels.grid.get(at[0], at[1], at[2]);
        (void)voxels.grid.set(at[0], at[1], at[2], next);
        // At the pace of the fluid that moved -- the one arriving, or the one
        // that drained away.
        const BlockId type = asset::blockTypeOf(next != asset::AirBlock ? next : before);
        paces.push_back(isFluidType(voxels, type) ? std::max<u64>(voxels.types[type - 1u].fluidTicks, 1) : 1);
    }
    for (usize written = 0; written < writes.size(); ++written) {
        const Position& at = writes[written].first;
        for (const Position& step : Readers) {
            // The block just written was decided this tick; its readers are
            // what this write changed.
            if (step == Position{0, 0, 0})
                continue;
            consider(voxels, Position{at[0] + step[0], at[1] + step[1], at[2] + step[2]}, tick + paces[written]);
        }
    }
    if (!writes.empty())
        voxels.revision += 1;
    return static_cast<u32>(writes.size());
}

} // namespace engine::scene
