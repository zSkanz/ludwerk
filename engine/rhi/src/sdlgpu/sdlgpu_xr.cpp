#include "sdlgpu_xr.h"

// OpenXR's own header first: SDL's then leaves its stand-in types out.
#include <SDL3/SDL_openxr.h>
#include <array>
#include <cmath>
#include <openxr/openxr.h>
#include <string>
#include <type_traits>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "sdlgpu_enums.h"

namespace engine::rhi::sdlgpu {
namespace {

// **An OpenXR struct, every field nought but its type**: what the
// specification asks of a struct handed to a call, written once.
template <class T>
[[nodiscard]] T made(XrStructureType type) noexcept
{
    T value{};
    value.type = type;
    return value;
}

// The calls this file makes, looked up once through the loader SDL opened.
struct XrCalls
{
    PFN_xrGetInstanceProperties getInstanceProperties = nullptr;
    PFN_xrGetSystemProperties getSystemProperties = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumerateViewConfigurationViews = nullptr;
    PFN_xrEnumerateSwapchainImages enumerateSwapchainImages = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
    PFN_xrDestroySpace destroySpace = nullptr;
    PFN_xrDestroySession destroySession = nullptr;
    PFN_xrPollEvent pollEvent = nullptr;
    PFN_xrBeginSession beginSession = nullptr;
    PFN_xrEndSession endSession = nullptr;
    PFN_xrWaitFrame waitFrame = nullptr;
    PFN_xrBeginFrame beginFrame = nullptr;
    PFN_xrEndFrame endFrame = nullptr;
    PFN_xrLocateViews locateViews = nullptr;
    PFN_xrAcquireSwapchainImage acquireSwapchainImage = nullptr;
    PFN_xrWaitSwapchainImage waitSwapchainImage = nullptr;
    PFN_xrReleaseSwapchainImage releaseSwapchainImage = nullptr;

    [[nodiscard]] bool load(XrInstance instance)
    {
        const auto getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(SDL_OpenXR_GetXrGetInstanceProcAddr());
        if (getProc == nullptr)
            return false;
        bool all = true;
        const auto get = [&](const char* name, auto& out) {
            PFN_xrVoidFunction found = nullptr;
            if (XR_FAILED(getProc(instance, name, &found)) || found == nullptr)
                all = false;
            out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(found);
        };
        get("xrGetInstanceProperties", getInstanceProperties);
        get("xrGetSystemProperties", getSystemProperties);
        get("xrEnumerateViewConfigurationViews", enumerateViewConfigurationViews);
        get("xrEnumerateSwapchainImages", enumerateSwapchainImages);
        get("xrCreateReferenceSpace", createReferenceSpace);
        get("xrDestroySpace", destroySpace);
        get("xrDestroySession", destroySession);
        get("xrPollEvent", pollEvent);
        get("xrBeginSession", beginSession);
        get("xrEndSession", endSession);
        get("xrWaitFrame", waitFrame);
        get("xrBeginFrame", beginFrame);
        get("xrEndFrame", endFrame);
        get("xrLocateViews", locateViews);
        get("xrAcquireSwapchainImage", acquireSwapchainImage);
        get("xrWaitSwapchainImage", waitSwapchainImage);
        get("xrReleaseSwapchainImage", releaseSwapchainImage);
        return all;
    }
};

void logFailure(std::string_view what, XrResult result)
{
    const std::array<core::I18nArg, 2> args{core::I18nArg{"what", what},
                                            core::I18nArg{"code", static_cast<core::i64>(result)}};
    core::log(core::LogLevel::Warn, ENG_TR("rhi.warn.xr_call_failed"), args);
}

struct EyeSwapchain
{
    XrSwapchain swapchain = XR_NULL_HANDLE;
    SDL_GPUTexture** images = nullptr;
    std::vector<TextureHandle> handles;
    bool acquired = false;
};

class OpenXrHeadset final : public IHeadset
{
public:
    OpenXrHeadset(XrInstance instance, XrSystemId system, XrDeviceHooks hooks)
        : m_instance(instance), m_system(system), m_hooks(std::move(hooks))
    {}

    ~OpenXrHeadset() override
    {
        for (EyeSwapchain& eye : m_eyes) {
            for (const TextureHandle handle : eye.handles)
                m_hooks.unwrap(handle);
            if (eye.swapchain != XR_NULL_HANDLE)
                (void)SDL_DestroyGPUXRSwapchain(m_hooks.device, eye.swapchain, eye.images);
        }
        if (m_space != XR_NULL_HANDLE && m_calls.destroySpace != nullptr)
            (void)m_calls.destroySpace(m_space);
        if (m_session != XR_NULL_HANDLE && m_calls.destroySession != nullptr)
            (void)m_calls.destroySession(m_session);
        // The instance is the device's: SDL ends it with the device.
    }

    [[nodiscard]] bool open()
    {
        if (!m_calls.load(m_instance)) {
            logFailure("xrGetInstanceProcAddr", XR_ERROR_FUNCTION_UNSUPPORTED);
            return false;
        }
        auto instance = made<XrInstanceProperties>(XR_TYPE_INSTANCE_PROPERTIES);
        if (XR_SUCCEEDED(m_calls.getInstanceProperties(m_instance, &instance)))
            m_runtime = instance.runtimeName;
        auto system = made<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
        if (XR_SUCCEEDED(m_calls.getSystemProperties(m_instance, m_system, &system)))
            m_systemName = system.systemName;

        // **Two eyes**, at the size the runtime recommends for each.
        u32 count = 0;
        XrResult result = m_calls.enumerateViewConfigurationViews(
            m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
        if (XR_FAILED(result) || count != 2) {
            logFailure("xrEnumerateViewConfigurationViews", result);
            return false;
        }
        std::array<XrViewConfigurationView, 2> views{};
        for (XrViewConfigurationView& view : views)
            view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        result = m_calls.enumerateViewConfigurationViews(
            m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views.data());
        if (XR_FAILED(result)) {
            logFailure("xrEnumerateViewConfigurationViews", result);
            return false;
        }
        m_width = views[0].recommendedImageRectWidth;
        m_height = views[0].recommendedImageRectHeight;

        auto sessionInfo = made<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
        result = SDL_CreateGPUXRSession(m_hooks.device, &sessionInfo, &m_session);
        if (XR_FAILED(result)) {
            logFailure("SDL_CreateGPUXRSession", result);
            return false;
        }
        // **The play space**: the floor's when the runtime knows it (a room
        // set up), the head's start otherwise.
        auto spaceInfo = made<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
        spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        result = m_calls.createReferenceSpace(m_session, &spaceInfo, &m_space);
        if (XR_FAILED(result)) {
            spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            result = m_calls.createReferenceSpace(m_session, &spaceInfo, &m_space);
        }
        if (XR_FAILED(result)) {
            logFailure("xrCreateReferenceSpace", result);
            return false;
        }

        // **An sRGB colour format the runtime takes**, its own order first:
        // the engine's last pass writes display colour, as it does a window.
        int formatCount = 0;
        SDL_GPUTextureFormat* formats = SDL_GetGPUXRSwapchainFormats(m_hooks.device, m_session, &formatCount);
        if (formats == nullptr || formatCount == 0) {
            logFailure("SDL_GetGPUXRSwapchainFormats", XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
            SDL_free(formats);
            return false;
        }
        SDL_GPUTextureFormat chosen = formats[0];
        for (int at = 0; at < formatCount; ++at) {
            if (formats[at] == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB ||
                formats[at] == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB) {
                chosen = formats[at];
                break;
            }
        }
        SDL_free(formats);
        m_format = fromSdl(chosen);

        for (EyeSwapchain& eye : m_eyes) {
            auto info = made<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
            info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            info.sampleCount = 1;
            info.width = m_width;
            info.height = m_height;
            info.faceCount = 1;
            info.arraySize = 1;
            info.mipCount = 1;
            result = SDL_CreateGPUXRSwapchain(m_hooks.device, m_session, &info, chosen, &eye.swapchain, &eye.images);
            if (XR_FAILED(result)) {
                logFailure("SDL_CreateGPUXRSwapchain", result);
                return false;
            }
            u32 images = 0;
            (void)m_calls.enumerateSwapchainImages(eye.swapchain, 0, &images, nullptr);
            for (u32 at = 0; at < images; ++at)
                eye.handles.push_back(m_hooks.wrap(eye.images[at], m_format, m_width, m_height));
        }
        const std::array<core::I18nArg, 4> args{core::I18nArg{"runtime", std::string_view{m_runtime}},
                                                core::I18nArg{"system", std::string_view{m_systemName}},
                                                core::I18nArg{"width", static_cast<core::i64>(m_width)},
                                                core::I18nArg{"height", static_cast<core::i64>(m_height)}};
        core::log(core::LogLevel::Info, ENG_TR("rhi.info.xr_ready"), args);
        return true;
    }

    [[nodiscard]] std::string_view runtimeName() const noexcept override { return m_runtime; }
    [[nodiscard]] std::string_view systemName() const noexcept override { return m_systemName; }
    [[nodiscard]] u32 eyeWidth() const noexcept override { return m_width; }
    [[nodiscard]] u32 eyeHeight() const noexcept override { return m_height; }
    [[nodiscard]] HeadsetState state() const noexcept override { return m_state; }

    void poll() override
    {
        auto event = made<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
        while (m_calls.pollEvent(m_instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
                sessionBecame(changed.state);
            }
            else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                m_state = HeadsetState::Ended;
            }
            event = made<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
        }
    }

    [[nodiscard]] bool beginFrame(HeadsetFrame& frame) override
    {
        frame = HeadsetFrame{};
        if (!m_running)
            return false;
        auto timing = made<XrFrameState>(XR_TYPE_FRAME_STATE);
        auto waitInfo = made<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO);
        XrResult result = m_calls.waitFrame(m_session, &waitInfo, &timing);
        if (XR_FAILED(result)) {
            logFailure("xrWaitFrame", result);
            return false;
        }
        auto beginInfo = made<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
        result = m_calls.beginFrame(m_session, &beginInfo);
        if (XR_FAILED(result)) {
            logFailure("xrBeginFrame", result);
            return false;
        }
        m_displayTime = timing.predictedDisplayTime;
        m_drawing = false;
        frame.displayTimeNs = timing.predictedDisplayTime;
        frame.width = m_width;
        frame.height = m_height;
        frame.format = m_format;
        if (timing.shouldRender == XR_FALSE)
            return true;

        auto locateInfo = made<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO);
        locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = timing.predictedDisplayTime;
        locateInfo.space = m_space;
        auto viewState = made<XrViewState>(XR_TYPE_VIEW_STATE);
        u32 located = 0;
        for (XrView& view : m_views)
            view = made<XrView>(XR_TYPE_VIEW);
        result = m_calls.locateViews(m_session, &locateInfo, &viewState, 2, &located, m_views.data());
        // **No view the runtime is sure of, no frame drawn**: drawn from a
        // guessed head is the picture that makes a player ill.
        if (XR_FAILED(result) || located != 2 || (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0)
            return true;
        for (usize at = 0; at < 2; ++at) {
            EyeSwapchain& eye = m_eyes[at];
            u32 index = 0;
            auto acquireInfo = made<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
            if (XR_FAILED(m_calls.acquireSwapchainImage(eye.swapchain, &acquireInfo, &index)) ||
                index >= eye.handles.size()) {
                releaseImages();
                return true;
            }
            eye.acquired = true;
            auto waitImage = made<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
            waitImage.timeout = XR_INFINITE_DURATION;
            if (XR_FAILED(m_calls.waitSwapchainImage(eye.swapchain, &waitImage))) {
                releaseImages();
                return true;
            }
            const XrView& view = m_views[at];
            EyeView& out = frame.eyes[at];
            out.position = {view.pose.position.x, view.pose.position.y, view.pose.position.z};
            out.orientation = {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
                               view.pose.orientation.w};
            out.tanLeft = std::tan(view.fov.angleLeft);
            out.tanRight = std::tan(view.fov.angleRight);
            out.tanDown = std::tan(view.fov.angleDown);
            out.tanUp = std::tan(view.fov.angleUp);
            frame.color[at] = eye.handles[index];
        }
        m_drawing = true;
        frame.draw = true;
        return true;
    }

    void endFrame() override
    {
        releaseImages();
        std::array<XrCompositionLayerProjectionView, 2> views{};
        auto layer = made<XrCompositionLayerProjection>(XR_TYPE_COMPOSITION_LAYER_PROJECTION);
        const XrCompositionLayerBaseHeader* layers[1] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
        auto endInfo = made<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
        endInfo.displayTime = m_displayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        if (m_drawing) {
            for (usize at = 0; at < 2; ++at) {
                views[at].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                views[at].pose = m_views[at].pose;
                views[at].fov = m_views[at].fov;
                views[at].subImage.swapchain = m_eyes[at].swapchain;
                views[at].subImage.imageRect.extent = {static_cast<int32_t>(m_width), static_cast<int32_t>(m_height)};
            }
            layer.space = m_space;
            layer.viewCount = 2;
            layer.views = views.data();
            endInfo.layerCount = 1;
            endInfo.layers = layers;
        }
        const XrResult result = m_calls.endFrame(m_session, &endInfo);
        if (XR_FAILED(result))
            logFailure("xrEndFrame", result);
        m_drawing = false;
    }

private:
    void releaseImages()
    {
        for (EyeSwapchain& eye : m_eyes) {
            if (!eye.acquired)
                continue;
            auto releaseInfo = made<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
            (void)m_calls.releaseSwapchainImage(eye.swapchain, &releaseInfo);
            eye.acquired = false;
        }
    }

    void sessionBecame(XrSessionState state)
    {
        switch (state) {
        case XR_SESSION_STATE_READY: {
            auto beginInfo = made<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            const XrResult result = m_calls.beginSession(m_session, &beginInfo);
            if (XR_FAILED(result)) {
                logFailure("xrBeginSession", result);
                return;
            }
            m_running = true;
            m_state = HeadsetState::Running;
            return;
        }
        case XR_SESSION_STATE_SYNCHRONIZED:
        case XR_SESSION_STATE_VISIBLE:
            m_state = HeadsetState::Paused;
            return;
        case XR_SESSION_STATE_FOCUSED:
            m_state = HeadsetState::Running;
            return;
        case XR_SESSION_STATE_STOPPING:
            (void)m_calls.endSession(m_session);
            m_running = false;
            m_state = HeadsetState::Idle;
            return;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            m_running = false;
            m_state = HeadsetState::Ended;
            core::log(core::LogLevel::Info, ENG_TR("rhi.info.xr_ended"));
            return;
        default:
            return;
        }
    }

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_system = XR_NULL_SYSTEM_ID;
    XrDeviceHooks m_hooks;
    XrCalls m_calls;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_space = XR_NULL_HANDLE;
    std::array<EyeSwapchain, 2> m_eyes{};
    std::array<XrView, 2> m_views{};
    std::string m_runtime;
    std::string m_systemName;
    u32 m_width = 0;
    u32 m_height = 0;
    TextureFormat m_format = TextureFormat::Undefined;
    XrTime m_displayTime = 0;
    bool m_running = false;
    bool m_drawing = false;
    HeadsetState m_state = HeadsetState::Idle;
};

} // namespace

void XrDeviceAsk::apply(SDL_PropertiesID props, const XrRequest& request)
{
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_ENABLE_BOOLEAN, true);
    SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_INSTANCE_POINTER, &instance);
    SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_SYSTEM_ID_POINTER, &system);
    if (!request.application.empty())
        SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_APPLICATION_NAME_STRING,
                              request.application.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_APPLICATION_VERSION_NUMBER, request.applicationVersion);
}

// SDL writes an instance handle into the u64 the ask holds.
static_assert(sizeof(XrInstance) == sizeof(u64));

std::unique_ptr<IHeadset> createOpenXrHeadset(const XrDeviceAsk& ask, XrDeviceHooks hooks)
{
    if (!ask.ready())
        return nullptr;
    auto headset = std::make_unique<OpenXrHeadset>(reinterpret_cast<XrInstance>(ask.instance),
                                                   static_cast<XrSystemId>(ask.system), std::move(hooks));
    if (!headset->open())
        return nullptr;
    return headset;
}

} // namespace engine::rhi::sdlgpu
