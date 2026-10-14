// This file is part of the FidelityFX SDK.
//
// Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// **The engine's callbacks for FSR 2** (ADR 0164): how AMD's algorithm, which
// is its portable headers (third_party/fidelityfx_fsr2, MIT), reads and writes
// its images through this engine's compute passes.
//
// **Generated** by `tools/repo/fsr2_callbacks.py` from AMD's own
// `ffx_fsr2_callbacks_hlsl.h`, whose licence is above and whose every function
// is kept by name. Edit the script, not this file. What it changes:
//
//   - **The registers carry spaces**: what a compute shader samples is in
//     space 0, what it writes in space 1, its constants in space 2 -- the RHI's
//     layout, where AMD's is Direct3D's bare registers.
//   - **A sampler a texture**, at the texture's own index: the RHI binds a
//     texture with its sampler. AMD's has two samplers for every texture.
//   - **A texel is read through its texture's sampler** (`ENG_FSR2_LOAD`),
//     where AMD's indexes the texture: a texture a shader loads from and
//     never samples is another kind of binding to the RHI, in another range
//     of slots.
//   - **Every image a pass writes says its format**, and each is one of the
//     three every device stores to with no feature asked of it: sixteen-bit
//     float in four channels, thirty-two-bit float in one, eight bits in four.
//     AMD's are narrower where it has two channels or one to keep -- formats a
//     Vulkan device stores to only as an extension. An image of two channels
//     is declared with four: the compiler asks for that extension on seeing
//     two, whatever format it is then given.
//   - **The reconstructed previous depth is read through the binding it is
//     written by**, where AMD's samples it in the pass after: a uint image is
//     not sampled, and loading from it would be the RHI's other kind of
//     binding. It is written as AMD writes it -- an atomic minimum -- on
//     Direct3D and Vulkan. **On Metal it is read, compared and written**
//     (`ENG_SHADER_MSL`, which the build defines for that target): the
//     shading language has had atomics on an image only since its version
//     3.1. Two threads that reproject onto one pixel in one dispatch can
//     leave the further of their depths there, and a pixel that came out
//     from behind something is then missed for a frame.
//   - **The luminance is one image**, the size of AMD's fifth mip of half the
//     render size, made by a pass of the engine's own (`fsr2_luminance`):
//     AMD's chain is made with atomics, and only that level of it is read.
//   - **The exposure is the engine's metered luminance**, made a gain where
//     it is read (`Exposure`).
//   - The branch for AMD's internal builds is gone.

#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_fsr2_resources.h"

#if defined(FFX_GPU)
#ifdef __hlsl_dx_compiler
#pragma dxc diagnostic push
#pragma dxc diagnostic ignored "-Wambig-lit-shift"
#endif //__hlsl_dx_compiler
#include "../../third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/ffx_core.h"
#ifdef __hlsl_dx_compiler
#pragma dxc diagnostic pop
#endif //__hlsl_dx_compiler
#endif // #if defined(FFX_GPU)

#if defined(FFX_GPU)
#ifndef FFX_FSR2_PREFER_WAVE64
#define FFX_FSR2_PREFER_WAVE64
#endif // #if defined(FFX_GPU)

#if defined(FFX_GPU)
#pragma warning(disable: 3205)  // conversion from larger type to smaller
#endif // #if defined(FFX_GPU)

#define DECLARE_SRV_REGISTER(regIndex)  t##regIndex
#define DECLARE_SAMPLER_REGISTER(regIndex)  s##regIndex
#define DECLARE_UAV_REGISTER(regIndex)  u##regIndex
#define DECLARE_CB_REGISTER(regIndex)   b##regIndex
#define FFX_FSR2_DECLARE_SRV(regIndex)  register(DECLARE_SRV_REGISTER(regIndex), space0)
#define FFX_FSR2_DECLARE_SAMPLER(regIndex)  register(DECLARE_SAMPLER_REGISTER(regIndex), space0)
#define FFX_FSR2_DECLARE_UAV(regIndex)  register(DECLARE_UAV_REGISTER(regIndex), space1)
#define FFX_FSR2_DECLARE_CB(regIndex)   register(DECLARE_CB_REGISTER(regIndex), space2)

// **A texel read is a sample at the texel's middle**, through the texture's
// own sampler. At a texel's middle a sampler gives that texel, exactly.
// **Where the middle is comes from the frame's constants** -- an image is the
// render size, the output's, or the luminance's -- and not from asking the
// texture at every read, which measured a quarter of what the passes cost. An
// image of one texel, which is what stands in for a mask nobody made, answers
// that texel wherever it is read.
#define ENG_FSR2_LOAD(texture, at) texture.SampleLevel(s_##texture, engFsr2Uv_##texture(at), 0)

#if defined(FSR2_BIND_CB_FSR2)
    cbuffer cbFSR2 : FFX_FSR2_DECLARE_CB(FSR2_BIND_CB_FSR2)
    {
        FfxInt32x2    iRenderSize;
        FfxInt32x2    iMaxRenderSize;
        FfxInt32x2    iDisplaySize;
        FfxInt32x2    iInputColorResourceDimensions;
        FfxInt32x2    iLumaMipDimensions;
        FfxInt32      iLumaMipLevelToUse;
        FfxInt32      iFrameIndex;

        FfxFloat32x4  fDeviceToViewDepth;
        FfxFloat32x2  fJitter;
        FfxFloat32x2  fMotionVectorScale;
        FfxFloat32x2  fDownscaleFactor;
        FfxFloat32x2  fMotionVectorJitterCancellation;
        FfxFloat32    fPreExposure;
        FfxFloat32    fPreviousFramePreExposure;
        FfxFloat32    fTanHalfFOV;
        FfxFloat32    fJitterSequenceLength;
        FfxFloat32    fDeltaTime;
        FfxFloat32    fDynamicResChangeFactor;
        FfxFloat32    fViewSpaceToMetersFactor;
    };

#define FFX_FSR2_CONSTANT_BUFFER_1_SIZE (sizeof(cbFSR2) / 4)  // Number of 32-bit values. This must be kept in sync with the cbFSR2 size.
#endif

#if defined(FFX_GPU)
#define FFX_FSR2_ROOTSIG_STRINGIFY(p) FFX_FSR2_ROOTSIG_STR(p)
#define FFX_FSR2_ROOTSIG_STR(p) #p
#define FFX_FSR2_ROOTSIG [RootSignature( "DescriptorTable(UAV(u0, numDescriptors = " FFX_FSR2_ROOTSIG_STRINGIFY(FFX_FSR2_RESOURCE_IDENTIFIER_COUNT) ")), " \
                                    "DescriptorTable(SRV(t0, numDescriptors = " FFX_FSR2_ROOTSIG_STRINGIFY(FFX_FSR2_RESOURCE_IDENTIFIER_COUNT) ")), " \
                                    "RootConstants(num32BitConstants=" FFX_FSR2_ROOTSIG_STRINGIFY(FFX_FSR2_CONSTANT_BUFFER_1_SIZE) ", b0), " \
                                    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_POINT, " \
                                                      "addressU = TEXTURE_ADDRESS_CLAMP, " \
                                                      "addressV = TEXTURE_ADDRESS_CLAMP, " \
                                                      "addressW = TEXTURE_ADDRESS_CLAMP, " \
                                                      "comparisonFunc = COMPARISON_NEVER, " \
                                                      "borderColor = STATIC_BORDER_COLOR_TRANSPARENT_BLACK), " \
                                    "StaticSampler(s1, filter = FILTER_MIN_MAG_MIP_LINEAR, " \
                                                      "addressU = TEXTURE_ADDRESS_CLAMP, " \
                                                      "addressV = TEXTURE_ADDRESS_CLAMP, " \
                                                      "addressW = TEXTURE_ADDRESS_CLAMP, " \
                                                      "comparisonFunc = COMPARISON_NEVER, " \
                                                      "borderColor = STATIC_BORDER_COLOR_TRANSPARENT_BLACK)" )]

#define FFX_FSR2_CONSTANT_BUFFER_2_SIZE 6  // Number of 32-bit values. This must be kept in sync with max( cbRCAS , cbSPD) size.

#define FFX_FSR2_CB2_ROOTSIG [RootSignature( "DescriptorTable(UAV(u0, numDescriptors = " FFX_FSR2_ROOTSIG_STRINGIFY(FFX_FSR2_RESOURCE_IDENTIFIER_COUNT) ")), " \
                                    "DescriptorTable(SRV(t0, numDescriptors = " FFX_FSR2_ROOTSIG_STRINGIFY(FFX_FSR2_RESOURCE_IDENTIFIER_COUNT) ")), " \
                                    "RootConstants(num32BitConstants=" FFX_FSR2_ROOTSIG_STRINGIFY(FFX_FSR2_CONSTANT_BUFFER_1_SIZE) ", b0), " \
                                    "RootConstants(num32BitConstants=" FFX_FSR2_ROOTSIG_STRINGIFY(FFX_FSR2_CONSTANT_BUFFER_2_SIZE) ", b1), " \
                                    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_POINT, " \
                                                      "addressU = TEXTURE_ADDRESS_CLAMP, " \
                                                      "addressV = TEXTURE_ADDRESS_CLAMP, " \
                                                      "addressW = TEXTURE_ADDRESS_CLAMP, " \
                                                      "comparisonFunc = COMPARISON_NEVER, " \
                                                      "borderColor = STATIC_BORDER_COLOR_TRANSPARENT_BLACK), " \
                                    "StaticSampler(s1, filter = FILTER_MIN_MAG_MIP_LINEAR, " \
                                                      "addressU = TEXTURE_ADDRESS_CLAMP, " \
                                                      "addressV = TEXTURE_ADDRESS_CLAMP, " \
                                                      "addressW = TEXTURE_ADDRESS_CLAMP, " \
                                                      "comparisonFunc = COMPARISON_NEVER, " \
                                                      "borderColor = STATIC_BORDER_COLOR_TRANSPARENT_BLACK)" )]
#if defined(FFX_FSR2_EMBED_ROOTSIG)
#define FFX_FSR2_EMBED_ROOTSIG_CONTENT FFX_FSR2_ROOTSIG
#define FFX_FSR2_EMBED_CB2_ROOTSIG_CONTENT FFX_FSR2_CB2_ROOTSIG
#else
#define FFX_FSR2_EMBED_ROOTSIG_CONTENT
#define FFX_FSR2_EMBED_CB2_ROOTSIG_CONTENT
#endif // #if FFX_FSR2_EMBED_ROOTSIG
#endif // #if defined(FFX_GPU)

/* Define getter functions in the order they are defined in the CB! */
FfxInt32x2 RenderSize()
{
    return iRenderSize;
}

FfxInt32x2 MaxRenderSize()
{
    return iMaxRenderSize;
}

FfxInt32x2 DisplaySize()
{
    return iDisplaySize;
}

FfxInt32x2 InputColorResourceDimensions()
{
    return iInputColorResourceDimensions;
}

FfxInt32x2 LumaMipDimensions()
{
    return iLumaMipDimensions;
}

FfxInt32  LumaMipLevelToUse()
{
    return iLumaMipLevelToUse;
}

FfxInt32 FrameIndex()
{
    return iFrameIndex;
}

FfxFloat32x2 Jitter()
{
    return fJitter;
}

FfxFloat32x4 DeviceToViewSpaceTransformFactors()
{
    return fDeviceToViewDepth;
}

FfxFloat32x2 MotionVectorScale()
{
    return fMotionVectorScale;
}

FfxFloat32x2 DownscaleFactor()
{
    return fDownscaleFactor;
}

FfxFloat32x2 MotionVectorJitterCancellation()
{
    return fMotionVectorJitterCancellation;
}

FfxFloat32 PreExposure()
{
    return fPreExposure;
}

FfxFloat32 PreviousFramePreExposure()
{
    return fPreviousFramePreExposure;
}

FfxFloat32 TanHalfFoV()
{
    return fTanHalfFOV;
}

FfxFloat32 JitterSequenceLength()
{
    return fJitterSequenceLength;
}

FfxFloat32 DeltaTime()
{
    return fDeltaTime;
}

FfxFloat32 DynamicResChangeFactor()
{
    return fDynamicResChangeFactor;
}

FfxFloat32 ViewSpaceToMetersFactor()
{
    return fViewSpaceToMetersFactor;
}



// SRVs
    #if defined FSR2_BIND_SRV_INPUT_COLOR
        Texture2D<FfxFloat32x4>                   r_input_color_jittered                    : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_INPUT_COLOR);
        SamplerState                               s_r_input_color_jittered : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_INPUT_COLOR);
        FfxFloat32x2 engFsr2Uv_r_input_color_jittered(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif
    #if defined FSR2_BIND_SRV_INPUT_OPAQUE_ONLY
        Texture2D<FfxFloat32x4>                   r_input_opaque_only                       : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_INPUT_OPAQUE_ONLY);
        SamplerState                               s_r_input_opaque_only : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_INPUT_OPAQUE_ONLY);
        FfxFloat32x2 engFsr2Uv_r_input_opaque_only(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif
    #if defined FSR2_BIND_SRV_INPUT_MOTION_VECTORS
        Texture2D<FfxFloat32x4>                   r_input_motion_vectors                    : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_INPUT_MOTION_VECTORS);
        SamplerState                               s_r_input_motion_vectors : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_INPUT_MOTION_VECTORS);
        FfxFloat32x2 engFsr2Uv_r_input_motion_vectors(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif
    #if defined FSR2_BIND_SRV_INPUT_DEPTH
        Texture2D<FfxFloat32>                     r_input_depth                             : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_INPUT_DEPTH);
        SamplerState                               s_r_input_depth : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_INPUT_DEPTH);
        FfxFloat32x2 engFsr2Uv_r_input_depth(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif 
    #if defined FSR2_BIND_SRV_INPUT_EXPOSURE
        Texture2D<FfxFloat32x2>                   r_input_exposure                          : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_INPUT_EXPOSURE);
        SamplerState                               s_r_input_exposure : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_INPUT_EXPOSURE);
        FfxFloat32x2 engFsr2Uv_r_input_exposure(FfxInt32x2 at)
        {
            return FfxFloat32x2(0.5f, 0.5f);
        }
    #endif
    #if defined FSR2_BIND_SRV_AUTO_EXPOSURE
        Texture2D<FfxFloat32x2>                   r_auto_exposure                           : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_AUTO_EXPOSURE);
        SamplerState                               s_r_auto_exposure : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_AUTO_EXPOSURE);
        FfxFloat32x2 engFsr2Uv_r_auto_exposure(FfxInt32x2 at)
        {
            return FfxFloat32x2(0.5f, 0.5f);
        }
    #endif
    #if defined FSR2_BIND_SRV_REACTIVE_MASK
        Texture2D<FfxFloat32>                     r_reactive_mask                           : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_REACTIVE_MASK);
        SamplerState                               s_r_reactive_mask : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_REACTIVE_MASK);
        FfxFloat32x2 engFsr2Uv_r_reactive_mask(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif 
    #if defined FSR2_BIND_SRV_TRANSPARENCY_AND_COMPOSITION_MASK
        Texture2D<FfxFloat32>                     r_transparency_and_composition_mask       : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_TRANSPARENCY_AND_COMPOSITION_MASK);
        SamplerState                               s_r_transparency_and_composition_mask : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_TRANSPARENCY_AND_COMPOSITION_MASK);
        FfxFloat32x2 engFsr2Uv_r_transparency_and_composition_mask(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif
    #if defined FSR2_BIND_SRV_RECONSTRUCTED_PREV_NEAREST_DEPTH
        Texture2D<FfxUInt32>                      r_reconstructed_previous_nearest_depth    : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_RECONSTRUCTED_PREV_NEAREST_DEPTH);
        SamplerState                               s_r_reconstructed_previous_nearest_depth : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_RECONSTRUCTED_PREV_NEAREST_DEPTH);
        FfxFloat32x2 engFsr2Uv_r_reconstructed_previous_nearest_depth(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif 
    #if defined FSR2_BIND_SRV_DILATED_MOTION_VECTORS
       Texture2D<FfxFloat32x2>                    r_dilated_motion_vectors                  : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_DILATED_MOTION_VECTORS);
       SamplerState                               s_r_dilated_motion_vectors : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_DILATED_MOTION_VECTORS);
       FfxFloat32x2 engFsr2Uv_r_dilated_motion_vectors(FfxInt32x2 at)
       {
           return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
       }
    #endif
    #if defined FSR2_BIND_SRV_PREVIOUS_DILATED_MOTION_VECTORS
           Texture2D<FfxFloat32x2>                r_previous_dilated_motion_vectors         : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_PREVIOUS_DILATED_MOTION_VECTORS);
           SamplerState                               s_r_previous_dilated_motion_vectors : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_PREVIOUS_DILATED_MOTION_VECTORS);
           FfxFloat32x2 engFsr2Uv_r_previous_dilated_motion_vectors(FfxInt32x2 at)
           {
               return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
           }
    #endif
    #if defined FSR2_BIND_SRV_DILATED_DEPTH
        Texture2D<FfxFloat32>                     r_dilatedDepth                            : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_DILATED_DEPTH);
        SamplerState                               s_r_dilatedDepth : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_DILATED_DEPTH);
        FfxFloat32x2 engFsr2Uv_r_dilatedDepth(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif
    #if defined FSR2_BIND_SRV_INTERNAL_UPSCALED
        Texture2D<FfxFloat32x4>                   r_internal_upscaled_color                 : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_INTERNAL_UPSCALED);
        SamplerState                               s_r_internal_upscaled_color : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_INTERNAL_UPSCALED);
        FfxFloat32x2 engFsr2Uv_r_internal_upscaled_color(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(DisplaySize());
        }
    #endif
    #if defined FSR2_BIND_SRV_LOCK_STATUS
        Texture2D<unorm FfxFloat32x2>             r_lock_status                             : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_LOCK_STATUS);
        SamplerState                               s_r_lock_status : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_LOCK_STATUS);
        FfxFloat32x2 engFsr2Uv_r_lock_status(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(DisplaySize());
        }
    #endif
    #if defined FSR2_BIND_SRV_LOCK_INPUT_LUMA
        Texture2D<FfxFloat32>                     r_lock_input_luma                         : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_LOCK_INPUT_LUMA);
        SamplerState                               s_r_lock_input_luma : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_LOCK_INPUT_LUMA);
        FfxFloat32x2 engFsr2Uv_r_lock_input_luma(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif
    #if defined FSR2_BIND_SRV_NEW_LOCKS
        Texture2D<unorm FfxFloat32>               r_new_locks                               : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_NEW_LOCKS);
        SamplerState                               s_r_new_locks : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_NEW_LOCKS);
        FfxFloat32x2 engFsr2Uv_r_new_locks(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(DisplaySize());
        }
    #endif
    #if defined FSR2_BIND_SRV_PREPARED_INPUT_COLOR
        Texture2D<FfxFloat32x4>                  r_prepared_input_color                    : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_PREPARED_INPUT_COLOR);
        SamplerState                               s_r_prepared_input_color : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_PREPARED_INPUT_COLOR);
        FfxFloat32x2 engFsr2Uv_r_prepared_input_color(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif
    #if defined FSR2_BIND_SRV_LUMA_HISTORY
        Texture2D<unorm FfxFloat32x4>             r_luma_history                            : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_LUMA_HISTORY);
        SamplerState                               s_r_luma_history : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_LUMA_HISTORY);
        FfxFloat32x2 engFsr2Uv_r_luma_history(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(DisplaySize());
        }
    #endif
    #if defined FSR2_BIND_SRV_RCAS_INPUT
        Texture2D<FfxFloat32x4>                   r_rcas_input                              : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_RCAS_INPUT);
        SamplerState                               s_r_rcas_input : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_RCAS_INPUT);
        FfxFloat32x2 engFsr2Uv_r_rcas_input(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(DisplaySize());
        }
    #endif
    #if defined FSR2_BIND_SRV_LANCZOS_LUT
        Texture2D<FfxFloat32>                     r_lanczos_lut                             : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_LANCZOS_LUT);
        SamplerState                               s_r_lanczos_lut : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_LANCZOS_LUT);
        FfxFloat32x2 engFsr2Uv_r_lanczos_lut(FfxInt32x2 at)
        {
            FfxUInt32 w;
            FfxUInt32 h;
            r_lanczos_lut.GetDimensions(w, h);
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(w, h);
        }
    #endif
    #if defined FSR2_BIND_SRV_SCENE_LUMINANCE_MIPS
        Texture2D<FfxFloat32>                     r_imgMips                                 : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_SCENE_LUMINANCE_MIPS);
        SamplerState                               s_r_imgMips : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_SCENE_LUMINANCE_MIPS);
        FfxFloat32x2 engFsr2Uv_r_imgMips(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(LumaMipDimensions());
        }
    #endif
    #if defined FSR2_BIND_SRV_UPSCALE_MAXIMUM_BIAS_LUT
        Texture2D<FfxFloat32>                     r_upsample_maximum_bias_lut               : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_UPSCALE_MAXIMUM_BIAS_LUT);
        SamplerState                               s_r_upsample_maximum_bias_lut : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_UPSCALE_MAXIMUM_BIAS_LUT);
        FfxFloat32x2 engFsr2Uv_r_upsample_maximum_bias_lut(FfxInt32x2 at)
        {
            FfxUInt32 w;
            FfxUInt32 h;
            r_upsample_maximum_bias_lut.GetDimensions(w, h);
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(w, h);
        }
    #endif
    #if defined FSR2_BIND_SRV_DILATED_REACTIVE_MASKS
        Texture2D<unorm FfxFloat32x2>             r_dilated_reactive_masks                  : FFX_FSR2_DECLARE_SRV(FSR2_BIND_SRV_DILATED_REACTIVE_MASKS);
        SamplerState                               s_r_dilated_reactive_masks : FFX_FSR2_DECLARE_SAMPLER(FSR2_BIND_SRV_DILATED_REACTIVE_MASKS);
        FfxFloat32x2 engFsr2Uv_r_dilated_reactive_masks(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif

    #if defined FSR2_BIND_SRV_PREV_PRE_ALPHA_COLOR
        Texture2D<float3>                         r_input_prev_color_pre_alpha              : FFX_FSR2_DECLARE_SRV(FFX_FSR2_RESOURCE_IDENTIFIER_PREV_PRE_ALPHA_COLOR);
        SamplerState                               s_r_input_prev_color_pre_alpha : FFX_FSR2_DECLARE_SAMPLER(FFX_FSR2_RESOURCE_IDENTIFIER_PREV_PRE_ALPHA_COLOR);
        FfxFloat32x2 engFsr2Uv_r_input_prev_color_pre_alpha(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif
    #if defined FSR2_BIND_SRV_PREV_POST_ALPHA_COLOR
        Texture2D<float3>                         r_input_prev_color_post_alpha             : FFX_FSR2_DECLARE_SRV(FFX_FSR2_RESOURCE_IDENTIFIER_PREV_POST_ALPHA_COLOR);
        SamplerState                               s_r_input_prev_color_post_alpha : FFX_FSR2_DECLARE_SAMPLER(FFX_FSR2_RESOURCE_IDENTIFIER_PREV_POST_ALPHA_COLOR);
        FfxFloat32x2 engFsr2Uv_r_input_prev_color_post_alpha(FfxInt32x2 at)
        {
            return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(RenderSize());
        }
    #endif
   
    // UAV declarations
    #if defined FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH
        [[vk::image_format("r32ui")]] RWTexture2D<FfxUInt32> rw_reconstructed_previous_nearest_depth : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH);
    #endif
    #if defined FSR2_BIND_UAV_DILATED_MOTION_VECTORS
        [[vk::image_format("rgba16f")]] RWTexture2D<FfxFloat32x4> rw_dilated_motion_vectors : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_DILATED_MOTION_VECTORS);
    #endif
    #if defined FSR2_BIND_UAV_DILATED_DEPTH
        [[vk::image_format("r32f")]] RWTexture2D<FfxFloat32> rw_dilatedDepth : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_DILATED_DEPTH);
    #endif
    #if defined FSR2_BIND_UAV_INTERNAL_UPSCALED
        [[vk::image_format("rgba16f")]] RWTexture2D<FfxFloat32x4> rw_internal_upscaled_color : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_INTERNAL_UPSCALED);
    #endif
    #if defined FSR2_BIND_UAV_LOCK_STATUS
        [[vk::image_format("rgba16f")]] RWTexture2D<FfxFloat32x4> rw_lock_status : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_LOCK_STATUS);
    #endif
    #if defined FSR2_BIND_UAV_LOCK_INPUT_LUMA
        [[vk::image_format("r32f")]] RWTexture2D<FfxFloat32> rw_lock_input_luma : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_LOCK_INPUT_LUMA);
    #endif
    #if defined FSR2_BIND_UAV_NEW_LOCKS
        [[vk::image_format("r32f")]] RWTexture2D<FfxFloat32> rw_new_locks : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_NEW_LOCKS);
    #endif
    #if defined FSR2_BIND_UAV_PREPARED_INPUT_COLOR
        [[vk::image_format("rgba16f")]] RWTexture2D<FfxFloat32x4> rw_prepared_input_color : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_PREPARED_INPUT_COLOR);
    #endif
    #if defined FSR2_BIND_UAV_LUMA_HISTORY
        [[vk::image_format("rgba8")]] RWTexture2D<FfxFloat32x4> rw_luma_history : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_LUMA_HISTORY);
    #endif
    #if defined FSR2_BIND_UAV_UPSCALED_OUTPUT
        [[vk::image_format("rgba16f")]] RWTexture2D<FfxFloat32x4> rw_upscaled_output : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_UPSCALED_OUTPUT);
    #endif
    #if defined FSR2_BIND_UAV_EXPOSURE_MIP_LUMA_CHANGE
        [[vk::image_format("rgba16f")]] globallycoherent RWTexture2D<FfxFloat32x4> rw_img_mip_shading_change : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_EXPOSURE_MIP_LUMA_CHANGE);
    #endif
    #if defined FSR2_BIND_UAV_EXPOSURE_MIP_5
        [[vk::image_format("r32f")]] globallycoherent RWTexture2D<FfxFloat32> rw_img_mip_5 : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_EXPOSURE_MIP_5);
    #endif
    #if defined FSR2_BIND_UAV_DILATED_REACTIVE_MASKS
        [[vk::image_format("rgba8")]] RWTexture2D<FfxFloat32x4> rw_dilated_reactive_masks : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_DILATED_REACTIVE_MASKS);
    #endif
    #if defined FSR2_BIND_UAV_EXPOSURE
        [[vk::image_format("rgba16f")]] RWTexture2D<FfxFloat32x4> rw_exposure : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_EXPOSURE);
    #endif
    #if defined FSR2_BIND_UAV_AUTO_EXPOSURE
        [[vk::image_format("rgba16f")]] RWTexture2D<FfxFloat32x4> rw_auto_exposure : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_AUTO_EXPOSURE);
    #endif
    #if defined FSR2_BIND_UAV_SPD_GLOBAL_ATOMIC
        [[vk::image_format("r32ui")]] globallycoherent RWTexture2D<FfxUInt32> rw_spd_global_atomic : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_SPD_GLOBAL_ATOMIC);
    #endif

    #if defined FSR2_BIND_UAV_AUTOREACTIVE
        [[vk::image_format("r32f")]] RWTexture2D<float> rw_output_autoreactive : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_AUTOREACTIVE);
    #endif
    #if defined FSR2_BIND_UAV_AUTOCOMPOSITION
        [[vk::image_format("r32f")]] RWTexture2D<float> rw_output_autocomposition : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_AUTOCOMPOSITION);
    #endif
    #if defined FSR2_BIND_UAV_PREV_PRE_ALPHA_COLOR
        [[vk::image_format("rgba16f")]] RWTexture2D<float3> rw_output_prev_color_pre_alpha : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_PREV_PRE_ALPHA_COLOR);
    #endif
    #if defined FSR2_BIND_UAV_PREV_POST_ALPHA_COLOR
        [[vk::image_format("rgba16f")]] RWTexture2D<float3> rw_output_prev_color_post_alpha : FFX_FSR2_DECLARE_UAV(FSR2_BIND_UAV_PREV_POST_ALPHA_COLOR);
    #endif

#if defined(FSR2_BIND_SRV_SCENE_LUMINANCE_MIPS)
FfxFloat32 LoadMipLuma(FfxUInt32x2 iPxPos, FfxUInt32 mipLevel)
{
    return ENG_FSR2_LOAD(r_imgMips, iPxPos);
}
#endif

#if defined(FSR2_BIND_SRV_SCENE_LUMINANCE_MIPS)
FfxFloat32 SampleMipLuma(FfxFloat32x2 fUV, FfxUInt32 mipLevel)
{
    return r_imgMips.SampleLevel(s_r_imgMips, fUV, 0);
}
#endif

#if defined(FSR2_BIND_SRV_INPUT_DEPTH)
FfxFloat32 LoadInputDepth(FfxUInt32x2 iPxPos)
{
    return ENG_FSR2_LOAD(r_input_depth, iPxPos);
}
#endif

#if defined(FSR2_BIND_SRV_INPUT_DEPTH)
FfxFloat32 SampleInputDepth(FfxFloat32x2 fUV)
{
    return r_input_depth.SampleLevel(s_r_input_depth, fUV, 0).x;
}
#endif

#if defined(FSR2_BIND_SRV_REACTIVE_MASK)
FfxFloat32 LoadReactiveMask(FfxUInt32x2 iPxPos)
{
    return ENG_FSR2_LOAD(r_reactive_mask, iPxPos);
}
#endif

#if defined(FSR2_BIND_SRV_TRANSPARENCY_AND_COMPOSITION_MASK)
FfxFloat32 LoadTransparencyAndCompositionMask(FfxUInt32x2 iPxPos)
{
    return ENG_FSR2_LOAD(r_transparency_and_composition_mask, iPxPos);
}
#endif

#if defined(FSR2_BIND_SRV_INPUT_COLOR)
FfxFloat32x3 LoadInputColor(FfxUInt32x2 iPxPos)
{
    return ENG_FSR2_LOAD(r_input_color_jittered, iPxPos).rgb;
}
#endif

#if defined(FSR2_BIND_SRV_INPUT_COLOR)
FfxFloat32x3 SampleInputColor(FfxFloat32x2 fUV)
{
    return r_input_color_jittered.SampleLevel(s_r_input_color_jittered, fUV, 0).rgb;
}
#endif

#if defined(FSR2_BIND_SRV_PREPARED_INPUT_COLOR)
FfxFloat32x3 LoadPreparedInputColor(FfxUInt32x2 iPxPos)
{
    return ENG_FSR2_LOAD(r_prepared_input_color, iPxPos).xyz;
}
#endif

#if defined(FSR2_BIND_SRV_INPUT_MOTION_VECTORS)
FfxFloat32x2 LoadInputMotionVector(FfxUInt32x2 iPxDilatedMotionVectorPos)
{
    FfxFloat32x2 fSrcMotionVector = ENG_FSR2_LOAD(r_input_motion_vectors, iPxDilatedMotionVectorPos).xy;

    FfxFloat32x2 fUvMotionVector = fSrcMotionVector * MotionVectorScale();

#if FFX_FSR2_OPTION_JITTERED_MOTION_VECTORS
    fUvMotionVector -= MotionVectorJitterCancellation();
#endif

    return fUvMotionVector;
}
#endif

#if defined(FSR2_BIND_SRV_INTERNAL_UPSCALED)
FfxFloat32x4 LoadHistory(FfxUInt32x2 iPxHistory)
{
    return ENG_FSR2_LOAD(r_internal_upscaled_color, iPxHistory);
}
#endif

#if defined(FSR2_BIND_UAV_LUMA_HISTORY)
void StoreLumaHistory(FfxUInt32x2 iPxPos, FfxFloat32x4 fLumaHistory)
{
    rw_luma_history[iPxPos] = fLumaHistory;
}
#endif

#if defined(FSR2_BIND_SRV_LUMA_HISTORY)
FfxFloat32x4 SampleLumaHistory(FfxFloat32x2 fUV)
{
    return r_luma_history.SampleLevel(s_r_luma_history, fUV, 0);
}
#endif


#if defined(FSR2_BIND_UAV_INTERNAL_UPSCALED)
void StoreReprojectedHistory(FfxUInt32x2 iPxHistory, FfxFloat32x4 fHistory)
{
    rw_internal_upscaled_color[iPxHistory] = fHistory;
}
#endif

#if defined(FSR2_BIND_UAV_INTERNAL_UPSCALED)
void StoreInternalColorAndWeight(FfxUInt32x2 iPxPos, FfxFloat32x4 fColorAndWeight)
{
    rw_internal_upscaled_color[iPxPos] = fColorAndWeight;
}
#endif

#if defined(FSR2_BIND_UAV_UPSCALED_OUTPUT)
void StoreUpscaledOutput(FfxUInt32x2 iPxPos, FfxFloat32x3 fColor)
{
    rw_upscaled_output[iPxPos] = FfxFloat32x4(fColor, 1.f);
}
#endif

//LOCK_LIFETIME_REMAINING == 0
//Should make LockInitialLifetime() return a const 1.0f later
#if defined(FSR2_BIND_SRV_LOCK_STATUS)
FfxFloat32x2 LoadLockStatus(FfxUInt32x2 iPxPos)
{
    return ENG_FSR2_LOAD(r_lock_status, iPxPos);
}
#endif

#if defined(FSR2_BIND_UAV_LOCK_STATUS)
void StoreLockStatus(FfxUInt32x2 iPxPos, FfxFloat32x2 fLockStatus)
{
    rw_lock_status[iPxPos] = FfxFloat32x4(fLockStatus, 0.0f, 0.0f);
}
#endif

#if defined(FSR2_BIND_SRV_LOCK_INPUT_LUMA)
FfxFloat32 LoadLockInputLuma(FfxUInt32x2 iPxPos)
{
    return ENG_FSR2_LOAD(r_lock_input_luma, iPxPos);
}
#endif

#if defined(FSR2_BIND_UAV_LOCK_INPUT_LUMA)
void StoreLockInputLuma(FfxUInt32x2 iPxPos, FfxFloat32 fLuma)
{
    rw_lock_input_luma[iPxPos] = fLuma;
}
#endif

#if defined(FSR2_BIND_SRV_NEW_LOCKS)
FfxFloat32 LoadNewLocks(FfxUInt32x2 iPxPos)
{
    return ENG_FSR2_LOAD(r_new_locks, iPxPos);
}
#endif

#if defined(FSR2_BIND_UAV_NEW_LOCKS)
FfxFloat32 LoadRwNewLocks(FfxUInt32x2 iPxPos)
{
    return rw_new_locks[iPxPos];
}
#endif

#if defined(FSR2_BIND_UAV_NEW_LOCKS)
void StoreNewLocks(FfxUInt32x2 iPxPos, FfxFloat32 newLock)
{
    rw_new_locks[iPxPos] = newLock;
}
#endif

#if defined(FSR2_BIND_UAV_PREPARED_INPUT_COLOR)
void StorePreparedInputColor(FFX_PARAMETER_IN FfxUInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32x4 fTonemapped)
{
    rw_prepared_input_color[iPxPos] = fTonemapped;
}
#endif

#if defined(FSR2_BIND_SRV_PREPARED_INPUT_COLOR)
FfxFloat32 SampleDepthClip(FfxFloat32x2 fUV)
{
    return r_prepared_input_color.SampleLevel(s_r_prepared_input_color, fUV, 0).w;
}
#endif

#if defined(FSR2_BIND_SRV_LOCK_STATUS)
FfxFloat32x2 SampleLockStatus(FfxFloat32x2 fUV)
{
    FfxFloat32x2 fLockStatus = r_lock_status.SampleLevel(s_r_lock_status, fUV, 0);
    return fLockStatus;
}
#endif

#if defined(FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH)
// **Read through the binding it is written by**: a uint image is not sampled,
// and one a shader loads from is another kind of binding to the RHI.
FfxFloat32 LoadReconstructedPrevDepth(FfxUInt32x2 iPxPos)
{
    return asfloat(rw_reconstructed_previous_nearest_depth[iPxPos]);
}

void StoreReconstructedDepth(FfxUInt32x2 iPxSample, FfxFloat32 fDepth)
{
    FfxUInt32 uDepth = asuint(fDepth);

#if defined(ENG_SHADER_MSL)
    // **Not atomic on Metal** (the head of this file): read, compared, written.
    const FfxUInt32 uHeld = rw_reconstructed_previous_nearest_depth[iPxSample];
    #if FFX_FSR2_OPTION_INVERTED_DEPTH
        rw_reconstructed_previous_nearest_depth[iPxSample] = max(uHeld, uDepth);
    #else
        rw_reconstructed_previous_nearest_depth[iPxSample] = min(uHeld, uDepth);
    #endif
#else
    #if FFX_FSR2_OPTION_INVERTED_DEPTH
        InterlockedMax(rw_reconstructed_previous_nearest_depth[iPxSample], uDepth);
    #else
        InterlockedMin(rw_reconstructed_previous_nearest_depth[iPxSample], uDepth); // min for standard, max for inverted depth
    #endif
#endif
}

void SetReconstructedDepth(FfxUInt32x2 iPxSample, const FfxUInt32 uValue)
{
    rw_reconstructed_previous_nearest_depth[iPxSample] = uValue;
}
#endif

#if defined(FSR2_BIND_UAV_DILATED_DEPTH)
void StoreDilatedDepth(FFX_PARAMETER_IN FfxUInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32 fDepth)
{
    rw_dilatedDepth[iPxPos] = fDepth;
}
#endif

#if defined(FSR2_BIND_UAV_DILATED_MOTION_VECTORS)
void StoreDilatedMotionVector(FFX_PARAMETER_IN FfxUInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32x2 fMotionVector)
{
    rw_dilated_motion_vectors[iPxPos] = FfxFloat32x4(fMotionVector, 0.0f, 0.0f);
}
#endif

#if defined(FSR2_BIND_SRV_DILATED_MOTION_VECTORS)
FfxFloat32x2 LoadDilatedMotionVector(FfxUInt32x2 iPxInput)
{
    return ENG_FSR2_LOAD(r_dilated_motion_vectors, iPxInput).xy;
}
#endif

#if defined(FSR2_BIND_SRV_PREVIOUS_DILATED_MOTION_VECTORS)
FfxFloat32x2 LoadPreviousDilatedMotionVector(FfxUInt32x2 iPxInput)
{
    return ENG_FSR2_LOAD(r_previous_dilated_motion_vectors, iPxInput).xy;
}

FfxFloat32x2 SamplePreviousDilatedMotionVector(FfxFloat32x2 uv)
{
    return r_previous_dilated_motion_vectors.SampleLevel(s_r_previous_dilated_motion_vectors, uv, 0).xy;
}
#endif

#if defined(FSR2_BIND_SRV_DILATED_DEPTH)
FfxFloat32 LoadDilatedDepth(FfxUInt32x2 iPxInput)
{
    return ENG_FSR2_LOAD(r_dilatedDepth, iPxInput);
}
#endif

#if defined(FSR2_BIND_SRV_INPUT_EXPOSURE)
FfxFloat32 Exposure()
{
    // **The engine meters a luminance, not a gain** (`luminance_adapt.hlsl`):
    // the frame's average, which the tonemap brings to its key. The same
    // division here, so what the algorithm weighs is what the eye is shown.
    const FfxFloat32 measured = ENG_FSR2_LOAD(r_input_exposure, FfxUInt32x2(0, 0)).x;

    if (measured <= 0.0f) {
        return 1.0f;
    }

    return 0.45f / max(measured, 1e-4f);
}
#endif

#if defined(FSR2_BIND_SRV_AUTO_EXPOSURE)
FfxFloat32 AutoExposure()
{
    FfxFloat32 exposure = ENG_FSR2_LOAD(r_auto_exposure, FfxUInt32x2(0, 0)).x;

    if (exposure == 0.0f) {
        exposure = 1.0f;
    }

    return exposure;
}
#endif

FfxFloat32 SampleLanczos2Weight(FfxFloat32 x)
{
#if defined(FSR2_BIND_SRV_LANCZOS_LUT)
    return r_lanczos_lut.SampleLevel(s_r_lanczos_lut, FfxFloat32x2(x / 2, 0.5f), 0);
#else
    return 0.f;
#endif
}

#if defined(FSR2_BIND_SRV_UPSCALE_MAXIMUM_BIAS_LUT)
FfxFloat32 SampleUpsampleMaximumBias(FfxFloat32x2 uv)
{
    // Stored as a SNORM, so make sure to multiply by 2 to retrieve the actual expected range.
    return FfxFloat32(2.0) * r_upsample_maximum_bias_lut.SampleLevel(s_r_upsample_maximum_bias_lut, abs(uv) * 2.0, 0);
}
#endif

#if defined(FSR2_BIND_SRV_DILATED_REACTIVE_MASKS)
FfxFloat32x2 SampleDilatedReactiveMasks(FfxFloat32x2 fUV)
{
	return r_dilated_reactive_masks.SampleLevel(s_r_dilated_reactive_masks, fUV, 0);
}
#endif

#if defined(FSR2_BIND_SRV_DILATED_REACTIVE_MASKS)
FfxFloat32x2 LoadDilatedReactiveMasks(FFX_PARAMETER_IN FfxUInt32x2 iPxPos)
{
    return ENG_FSR2_LOAD(r_dilated_reactive_masks, iPxPos);
}
#endif

#if defined(FSR2_BIND_UAV_DILATED_REACTIVE_MASKS)
void StoreDilatedReactiveMasks(FFX_PARAMETER_IN FfxUInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32x2 fDilatedReactiveMasks)
{
    rw_dilated_reactive_masks[iPxPos] = FfxFloat32x4(fDilatedReactiveMasks, 0.0f, 0.0f);
}
#endif

#if defined(FSR2_BIND_SRV_INPUT_OPAQUE_ONLY)
FfxFloat32x3 LoadOpaqueOnly(FFX_PARAMETER_IN FFX_MIN16_I2 iPxPos)
{
    return ENG_FSR2_LOAD(r_input_opaque_only, iPxPos).xyz;
}
#endif

#if defined(FSR2_BIND_SRV_PREV_PRE_ALPHA_COLOR)
FfxFloat32x3 LoadPrevPreAlpha(FFX_PARAMETER_IN FFX_MIN16_I2 iPxPos)
{
    return ENG_FSR2_LOAD(r_input_prev_color_pre_alpha, iPxPos);
}
#endif

#if defined(FSR2_BIND_SRV_PREV_POST_ALPHA_COLOR)
FfxFloat32x3 LoadPrevPostAlpha(FFX_PARAMETER_IN FFX_MIN16_I2 iPxPos)
{
    return ENG_FSR2_LOAD(r_input_prev_color_post_alpha, iPxPos);
}
#endif

#if defined(FSR2_BIND_UAV_AUTOREACTIVE)
#if defined(FSR2_BIND_UAV_AUTOCOMPOSITION)
void StoreAutoReactive(FFX_PARAMETER_IN FFX_MIN16_I2 iPxPos, FFX_PARAMETER_IN FFX_MIN16_F2 fReactive)
{
    rw_output_autoreactive[iPxPos] = fReactive.x;

    rw_output_autocomposition[iPxPos] = fReactive.y;
}
#endif
#endif

#if defined(FSR2_BIND_UAV_PREV_PRE_ALPHA_COLOR)
void StorePrevPreAlpha(FFX_PARAMETER_IN FFX_MIN16_I2 iPxPos, FFX_PARAMETER_IN FFX_MIN16_F3 color)
{
    rw_output_prev_color_pre_alpha[iPxPos] = color;

}
#endif

#if defined(FSR2_BIND_UAV_PREV_POST_ALPHA_COLOR)
void StorePrevPostAlpha(FFX_PARAMETER_IN FFX_MIN16_I2 iPxPos, FFX_PARAMETER_IN FFX_MIN16_F3 color)
{
    rw_output_prev_color_post_alpha[iPxPos] = color;
}
#endif

#endif // #if defined(FFX_GPU)
