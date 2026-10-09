// Backend constructors (ADR 0023).
//
// There is no plugin ABI and no self-registering static: consoles and iOS want
// a fully static, dead-strippable binary, and a registry defeats both. Each
// backend the build compiled in declares its creator here, and `app` owns the
// single hand-written switch that picks one. A backend that is switched off is
// not declared, so choosing it is a compile error rather than a null at
// startup.
#pragma once

#include <memory>

#include "engine/core/error.h"
#include "engine/rhi/device.h"

namespace engine::rhi {

// Null on failure, with `outError` filled when it is not null.
using DeviceResult = std::unique_ptr<IDevice>;

#if ENG_RHI_D3D12
[[nodiscard]] DeviceResult createD3D12Device(const DeviceDesc& desc, std::span<const std::byte> blitVertex,
                                             std::span<const std::byte> blitFragment,
                                             core::EngineError* outError = nullptr);
#endif

#if ENG_RHI_NULL
[[nodiscard]] DeviceResult createNullDevice(const DeviceDesc& desc, core::EngineError* outError = nullptr);
#endif

#if ENG_RHI_SDLGPU
[[nodiscard]] DeviceResult createSdlGpuDevice(const DeviceDesc& desc, core::EngineError* outError = nullptr);
#endif

#if ENG_RHI_CAPTURE
[[nodiscard]] DeviceResult createCaptureDevice(const DeviceDesc& desc, core::EngineError* outError = nullptr);
#endif

} // namespace engine::rhi
