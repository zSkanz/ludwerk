#include "engine/app/backends.h"

#include "engine/core/text_key.h"
#if ENG_RHI_D3D12
#include "engine/platform/file.h"
#include "engine/platform/platform.h"
#endif

namespace engine::app {
namespace {

// Compile-time membership. A backend that is off is not merely unavailable at
// runtime -- its creator is not declared, so naming it here would not link.
// That is the property ADR 0023 is buying: what the binary contains is decided
// by the build, and it is visible in one file.
constexpr bool kHasSdlGpu =
#if ENG_RHI_SDLGPU
    true;
#else
    false;
#endif

constexpr bool kHasCapture =
#if ENG_RHI_CAPTURE
    true;
#else
    false;
#endif

constexpr bool kHasNull =
#if ENG_RHI_NULL
    true;
#else
    false;
#endif

} // namespace

std::optional<rhi::BackendId> parseBackendId(std::string_view name)
{
#if ENG_RHI_D3D12
    if (name == "d3d12")
        return rhi::BackendId::D3D12;
#endif
    if (name == "sdlgpu" && kHasSdlGpu)
        return rhi::BackendId::SdlGpu;
    if (name == "capture" && kHasCapture)
        return rhi::BackendId::Capture;
    if (name == "null" && kHasNull)
        return rhi::BackendId::Null;
    return std::nullopt;
}

std::string_view availableBackendNames()
{
#if ENG_RHI_D3D12
    static constexpr std::string_view names =
#if ENG_RHI_SDLGPU
        "sdlgpu, "
#endif
#if ENG_RHI_CAPTURE
        "capture, "
#endif
#if ENG_RHI_NULL
        "null, "
#endif
        "d3d12";
    return names;
#else
    // Spelled out per combination rather than assembled at runtime: this is a
    // compile-time fact, and building it into a string would allocate to
    // describe something that cannot change.
    if constexpr (kHasSdlGpu && kHasCapture && kHasNull)
        return "sdlgpu, capture, null";
    else if constexpr (kHasSdlGpu && kHasCapture)
        return "sdlgpu, capture";
    else if constexpr (kHasSdlGpu && kHasNull)
        return "sdlgpu, null";
    else if constexpr (kHasCapture && kHasNull)
        return "capture, null";
    else if constexpr (kHasSdlGpu)
        return "sdlgpu";
    else if constexpr (kHasCapture)
        return "capture";
    else if constexpr (kHasNull)
        return "null";
    else
        return "(none)";
#endif
}

std::string_view backendName(rhi::BackendId backend)
{
    switch (backend) {
    case rhi::BackendId::SdlGpu:
        return "sdlgpu";
    case rhi::BackendId::Capture:
        return "capture";
    case rhi::BackendId::Null:
        return "null";
    case rhi::BackendId::D3D12:
        return "d3d12";
    }
    return "unknown";
}

rhi::DeviceResult createDevice(const rhi::DeviceDesc& desc, core::EngineError* outError)
{
    switch (desc.backend) {
    case rhi::BackendId::D3D12:
#if ENG_RHI_D3D12
    {
        const auto directory = platform::paths().contentDir / "shaders" / "dxil";
        std::vector<std::byte> vertex, fragment;
        const auto readShader = [&](const char* name, std::vector<std::byte>& bytes) {
            const auto path = directory / name;
            if (platform::readFile(path, bytes))
                return true;
            if (outError) {
                const core::I18nArg args[]{{"path", path.string()}};
                *outError = core::makeError(ENG_TR("render.err.shader_blob_missing"), args);
            }
            return false;
        };
        if (!readShader("rhi_blit.vertex.dxil", vertex) || !readShader("rhi_blit.fragment.dxil", fragment)) {
            return nullptr;
        }
        return rhi::createD3D12Device(desc, vertex, fragment, outError);
    }
#else
        break;
#endif
    case rhi::BackendId::SdlGpu:
#if ENG_RHI_SDLGPU
        return rhi::createSdlGpuDevice(desc, outError);
#else
        break;
#endif
    case rhi::BackendId::Capture:
#if ENG_RHI_CAPTURE
        return rhi::createCaptureDevice(desc, outError);
#else
        break;
#endif
    case rhi::BackendId::Null:
#if ENG_RHI_NULL
        return rhi::createNullDevice(desc, outError);
#else
        break;
#endif
    }

    if (outError != nullptr)
        *outError = core::makeError(ENG_TR("rhi.err.backend_not_compiled"));
    return nullptr;
}

} // namespace engine::app
