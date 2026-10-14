// After the foliage cull (ADR 0116): each indirect draw's instance count is
// its mesh's count of visible instances, held to the list's capacity -- the
// cull counts every instance that passed, including any it had no room for.

struct FoliageCommand
{
    // The counter it reads: a bucket's level.
    uint Counter;
    uint Capacity;
};

StructuredBuffer<FoliageCommand> Commands : register(t0, space0);
StructuredBuffer<uint> Counters : register(t1, space0);
RWStructuredBuffer<uint> Arguments : register(u0, space1);

cbuffer GpuFoliageFinalize : register(b0, space2)
{
    uint CommandCount;
    uint3 Unused;
};

[numthreads(64, 1, 1)]
void ComputeMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= CommandCount)
        return;
    const FoliageCommand command = Commands[id.x];
    // The second of the five words of `DrawIndexedIndirectCommand`.
    Arguments[id.x * 5u + 1u] = min(Counters[command.Counter], command.Capacity);
}
