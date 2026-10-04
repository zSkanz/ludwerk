// **Optical flow: where each block of eight pixels came from** (ADR 0165):
// the offset, within eight pixels of the guess the coarser level made, at
// which the last frame's luminance is least unlike this one's. AMD's
// `ComputeOpticalFlowAdvanced`, a group of sixty-four threads a pair of
// blocks.
//
// **Its reductions are over the group, not over a wave.** AMD's takes the
// least and the sum of a value across the threads with the wave operations,
// and is written for a wave of sixty-four or of thirty-two -- what its own
// GPUs and one other vendor's have. On a wave of sixteen, of eight or of four
// (other vendors', and every software device, which is what CI draws with)
// it reduces over a part of the group and each part writes its own answer.
// So the four operations it uses are the engine's here: every thread puts
// its value in the group's shared memory, and reads all sixty-four. The same
// answer on every device, for sixty-four reads a thread a reduction.

#include "engine_fsr3_options.hlsli"

#define FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_INPUT 0
#define FFX_OPTICALFLOW_BIND_SRV_OPTICAL_FLOW_PREVIOUS_INPUT 1
#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW 0
#define FFX_OPTICALFLOW_BIND_UAV_OPTICAL_FLOW_SCD_OUTPUT 1
#define FFX_OPTICALFLOW_BIND_CB_COMMON 0

#include "engine_fsr3_of_callbacks.hlsli"

#define ENG_OF_THREADS 64
// Which of the group's threads this is, for the four operations below.
static uint engLane = 0;
groupshared uint engShared[ENG_OF_THREADS];

uint ffxWaveLaneCount()
{
    return ENG_OF_THREADS;
}

bool ffxWaveIsFirstLane()
{
    return engLane == 0;
}

uint ffxWaveSum(uint value)
{
    engShared[engLane] = value;
    GroupMemoryBarrierWithGroupSync();
    uint sum = 0;
    for (uint lane = 0; lane < ENG_OF_THREADS; ++lane)
        sum += engShared[lane];
    // Nobody writes the next value before everybody has read this one.
    GroupMemoryBarrierWithGroupSync();
    return sum;
}

uint ffxWaveMin(uint value)
{
    engShared[engLane] = value;
    GroupMemoryBarrierWithGroupSync();
    uint least = 0xffffffffu;
    for (uint lane = 0; lane < ENG_OF_THREADS; ++lane)
        least = min(least, engShared[lane]);
    GroupMemoryBarrierWithGroupSync();
    return least;
}

#include "../../third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/opticalflow/ffx_opticalflow_compute_optical_flow_v5.h"

[numthreads(ENG_OF_THREADS, 1, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupThreadId : SV_GroupThreadID,
                 uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex)
{
    engLane = groupIndex;
    ComputeOpticalFlowAdvanced(int2(dispatchThreadId.xy), int2(groupThreadId.xy), int2(groupId.xy), int(groupIndex));
}
