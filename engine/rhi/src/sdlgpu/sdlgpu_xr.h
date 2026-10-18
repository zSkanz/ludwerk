// The headset of an SDL GPU device, through SDL's OpenXR integration (ADR
// 0244). Internal to the backend: nothing outside `engine/rhi` sees OpenXR.
#pragma once

#include <SDL3/SDL_gpu.h>
#include <functional>
#include <memory>
#include <vector>

#include "engine/rhi/xr.h"

namespace engine::rhi::sdlgpu {

// What the headset needs of the device it was made with: the device, and a
// way to name SDL's images as the device's textures and to let the names go.
struct XrDeviceHooks
{
    SDL_GPUDevice* device = nullptr;
    std::function<TextureHandle(SDL_GPUTexture*, TextureFormat, u32, u32)> wrap;
    std::function<void(TextureHandle)> unwrap;
};

// The properties that make `SDL_CreateGPUDeviceWithProperties` make the
// device for a headset, and where SDL writes back the instance and system it
// found. `ready` after the device was made says whether it was.
struct XrDeviceAsk
{
    u64 instance = 0;
    u64 system = 0;
    // The extensions asked of the runtime beyond SDL's own, each one it
    // lists: an instance asked for one it does not have is not made.
    std::vector<const char*> extensions;
    bool handTracking = false;
    // XR_KHR_composition_layer_depth, asked for and listed.
    bool depthLayer = false;
    void apply(SDL_PropertiesID props, const XrRequest& request);
    [[nodiscard]] bool ready() const noexcept { return instance != 0 && system != 0; }
};

// The headset, with its session begun on `hooks.device`; null, with the
// reason logged, when the runtime would not make one.
[[nodiscard]] std::unique_ptr<IHeadset> createOpenXrHeadset(const XrDeviceAsk& ask, XrDeviceHooks hooks);

} // namespace engine::rhi::sdlgpu
