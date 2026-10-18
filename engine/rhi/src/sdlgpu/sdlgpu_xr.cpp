#include "sdlgpu_xr.h"

// OpenXR's own header first: SDL's then leaves its stand-in types out.
// clang-format off
#include <openxr/openxr.h>
#include <SDL3/SDL_openxr.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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
    PFN_xrEnumerateSwapchainFormats enumerateSwapchainFormats = nullptr;
    PFN_xrEnumerateEnvironmentBlendModes enumerateBlendModes = nullptr;
    PFN_xrCreateReferenceSpace createReferenceSpace = nullptr;
    PFN_xrGetReferenceSpaceBoundsRect getBoundsRect = nullptr;
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
    PFN_xrStringToPath stringToPath = nullptr;
    PFN_xrCreateActionSet createActionSet = nullptr;
    PFN_xrDestroyActionSet destroyActionSet = nullptr;
    PFN_xrCreateAction createAction = nullptr;
    PFN_xrSuggestInteractionProfileBindings suggestBindings = nullptr;
    PFN_xrAttachSessionActionSets attachActionSets = nullptr;
    PFN_xrSyncActions syncActions = nullptr;
    PFN_xrGetActionStateFloat getFloat = nullptr;
    PFN_xrGetActionStateBoolean getBoolean = nullptr;
    PFN_xrGetActionStateVector2f getVector2 = nullptr;
    PFN_xrCreateActionSpace createActionSpace = nullptr;
    PFN_xrLocateSpace locateSpace = nullptr;
    PFN_xrApplyHapticFeedback applyHaptics = nullptr;
    // XR_EXT_hand_tracking's, where it was asked for and given.
    PFN_xrCreateHandTrackerEXT createHandTracker = nullptr;
    PFN_xrDestroyHandTrackerEXT destroyHandTracker = nullptr;
    PFN_xrLocateHandJointsEXT locateHandJoints = nullptr;
    // XR_FB_passthrough's, where it was asked for and given.
    PFN_xrCreatePassthroughFB createPassthrough = nullptr;
    PFN_xrDestroyPassthroughFB destroyPassthrough = nullptr;
    PFN_xrPassthroughStartFB startPassthrough = nullptr;
    PFN_xrCreatePassthroughLayerFB createPassthroughLayer = nullptr;
    PFN_xrDestroyPassthroughLayerFB destroyPassthroughLayer = nullptr;
    PFN_xrPassthroughLayerPauseFB pausePassthroughLayer = nullptr;
    PFN_xrPassthroughLayerResumeFB resumePassthroughLayer = nullptr;

    void loadPassthrough(XrInstance instance)
    {
        const auto getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(SDL_OpenXR_GetXrGetInstanceProcAddr());
        if (getProc == nullptr)
            return;
        const auto get = [&](const char* name, auto& out) {
            PFN_xrVoidFunction found = nullptr;
            if (XR_FAILED(getProc(instance, name, &found)))
                found = nullptr;
            out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(found);
        };
        get("xrCreatePassthroughFB", createPassthrough);
        get("xrDestroyPassthroughFB", destroyPassthrough);
        get("xrPassthroughStartFB", startPassthrough);
        get("xrCreatePassthroughLayerFB", createPassthroughLayer);
        get("xrDestroyPassthroughLayerFB", destroyPassthroughLayer);
        get("xrPassthroughLayerPauseFB", pausePassthroughLayer);
        get("xrPassthroughLayerResumeFB", resumePassthroughLayer);
    }

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
        get("xrEnumerateSwapchainFormats", enumerateSwapchainFormats);
        get("xrEnumerateEnvironmentBlendModes", enumerateBlendModes);
        get("xrCreateReferenceSpace", createReferenceSpace);
        get("xrGetReferenceSpaceBoundsRect", getBoundsRect);
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
        get("xrStringToPath", stringToPath);
        get("xrCreateActionSet", createActionSet);
        get("xrDestroyActionSet", destroyActionSet);
        get("xrCreateAction", createAction);
        get("xrSuggestInteractionProfileBindings", suggestBindings);
        get("xrAttachSessionActionSets", attachActionSets);
        get("xrSyncActions", syncActions);
        get("xrGetActionStateFloat", getFloat);
        get("xrGetActionStateBoolean", getBoolean);
        get("xrGetActionStateVector2f", getVector2);
        get("xrCreateActionSpace", createActionSpace);
        get("xrLocateSpace", locateSpace);
        get("xrApplyHapticFeedback", applyHaptics);
        return all;
    }

    void loadHands(XrInstance instance)
    {
        const auto getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(SDL_OpenXR_GetXrGetInstanceProcAddr());
        if (getProc == nullptr)
            return;
        const auto get = [&](const char* name, auto& out) {
            PFN_xrVoidFunction found = nullptr;
            if (XR_FAILED(getProc(instance, name, &found)))
                found = nullptr;
            out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(found);
        };
        get("xrCreateHandTrackerEXT", createHandTracker);
        get("xrDestroyHandTrackerEXT", destroyHandTracker);
        get("xrLocateHandJointsEXT", locateHandJoints);
    }
};

void logFailure(std::string_view what, XrResult result)
{
    const std::array<core::I18nArg, 2> args{core::I18nArg{"what", what},
                                            core::I18nArg{"code", static_cast<core::i64>(result)}};
    core::log(core::LogLevel::Warn, ENG_TR("rhi.warn.xr_call_failed"), args);
}

// **What the controllers have**, as one action each with a left and a right
// sub-path (ADR 0245 §2). `Key` is the place of the value in
// `HeadsetInput::keys` for each hand, -1 where that hand has none.
enum class ActionKind : u8
{
    Boolean,
    Float,
    Stick,
    Pose,
    Haptic,
};
struct ActionSpec
{
    const char* name;
    ActionKind kind;
    std::array<int, 2> key;
};
// Grip and aim, the triggers and grips, the sticks (their X; Y is the next
// place), the sticks pressed in, A or X, B or Y, the menu, and the haptic.
constexpr std::array<ActionSpec, 10> Actions{{
    {"grip_pose", ActionKind::Pose, {-1, -1}},
    {"aim_pose", ActionKind::Pose, {-1, -1}},
    {"trigger", ActionKind::Float, {5, 6}},
    {"squeeze", ActionKind::Float, {7, 8}},
    {"thumbstick", ActionKind::Stick, {9, 11}},
    {"thumbstick_click", ActionKind::Boolean, {15, 16}},
    {"primary", ActionKind::Boolean, {2, 0}},
    {"secondary", ActionKind::Boolean, {3, 1}},
    {"menu", ActionKind::Boolean, {4, -1}},
    {"haptic", ActionKind::Haptic, {-1, -1}},
}};

// **Where each action is on a controller the engine knows**, by its OpenXR
// path under a hand, and by action. The runtime may rebind them for the
// player; these are only suggestions. Empty where a controller has none.
struct ProfileSpec
{
    const char* profile;
    std::array<const char*, 10> left;
    std::array<const char*, 10> right;
};
constexpr std::array<ProfileSpec, 4> Profiles{{
    {"/interaction_profiles/oculus/touch_controller",
     {"input/grip/pose", "input/aim/pose", "input/trigger/value", "input/squeeze/value", "input/thumbstick",
      "input/thumbstick/click", "input/x/click", "input/y/click", "input/menu/click", "output/haptic"},
     {"input/grip/pose", "input/aim/pose", "input/trigger/value", "input/squeeze/value", "input/thumbstick",
      "input/thumbstick/click", "input/a/click", "input/b/click", "", "output/haptic"}},
    {"/interaction_profiles/valve/index_controller",
     {"input/grip/pose", "input/aim/pose", "input/trigger/value", "input/squeeze/value", "input/thumbstick",
      "input/thumbstick/click", "input/a/click", "input/b/click", "", "output/haptic"},
     {"input/grip/pose", "input/aim/pose", "input/trigger/value", "input/squeeze/value", "input/thumbstick",
      "input/thumbstick/click", "input/a/click", "input/b/click", "", "output/haptic"}},
    {"/interaction_profiles/htc/vive_controller",
     {"input/grip/pose", "input/aim/pose", "input/trigger/value", "input/squeeze/click", "input/trackpad",
      "input/trackpad/click", "", "", "input/menu/click", "output/haptic"},
     {"input/grip/pose", "input/aim/pose", "input/trigger/value", "input/squeeze/click", "input/trackpad",
      "input/trackpad/click", "", "", "input/menu/click", "output/haptic"}},
    {"/interaction_profiles/khr/simple_controller",
     {"input/grip/pose", "input/aim/pose", "input/select/click", "", "", "", "", "", "input/menu/click",
      "output/haptic"},
     {"input/grip/pose", "input/aim/pose", "input/select/click", "", "", "", "", "", "input/menu/click",
      "output/haptic"}},
}};

[[nodiscard]] HeadsetPose poseOf(const XrSpaceLocation& location) noexcept
{
    HeadsetPose pose;
    const XrSpaceLocationFlags known = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    pose.valid = (location.locationFlags & known) == known;
    pose.position = {location.pose.position.x, location.pose.position.y, location.pose.position.z};
    pose.orientation = {location.pose.orientation.x, location.pose.orientation.y, location.pose.orientation.z,
                        location.pose.orientation.w};
    return pose;
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
    OpenXrHeadset(XrInstance instance, XrSystemId system, XrDeviceHooks hooks, bool handTracking, bool depthLayer,
                  bool fbPassthrough)
        : m_instance(instance), m_system(system), m_hooks(std::move(hooks)), m_fbPassthrough(fbPassthrough),
          m_depthLayer(depthLayer), m_handTracking(handTracking)
    {}

    ~OpenXrHeadset() override
    {
        if (m_passthroughLayer != XR_NULL_HANDLE && m_calls.destroyPassthroughLayer != nullptr)
            (void)m_calls.destroyPassthroughLayer(m_passthroughLayer);
        if (m_passthrough != XR_NULL_HANDLE && m_calls.destroyPassthrough != nullptr)
            (void)m_calls.destroyPassthrough(m_passthrough);
        for (const XrHandTrackerEXT tracker : m_handTrackers) {
            if (tracker != XR_NULL_HANDLE && m_calls.destroyHandTracker != nullptr)
                (void)m_calls.destroyHandTracker(tracker);
        }
        for (const XrSpace space : m_handSpaces) {
            if (space != XR_NULL_HANDLE && m_calls.destroySpace != nullptr)
                (void)m_calls.destroySpace(space);
        }
        if (m_headSpace != XR_NULL_HANDLE && m_calls.destroySpace != nullptr)
            (void)m_calls.destroySpace(m_headSpace);
        if (m_actionSet != XR_NULL_HANDLE && m_calls.destroyActionSet != nullptr)
            (void)m_calls.destroyActionSet(m_actionSet);
        for (std::array<EyeSwapchain, 2>* set : {&m_eyes, &m_depthEyes}) {
            for (EyeSwapchain& eye : *set) {
                for (const TextureHandle handle : eye.handles)
                    m_hooks.unwrap(handle);
                if (eye.swapchain != XR_NULL_HANDLE)
                    (void)SDL_DestroyGPUXRSwapchain(m_hooks.device, eye.swapchain, eye.images);
            }
        }
        for (const XrSpace space : {m_stage, m_local}) {
            if (space != XR_NULL_HANDLE && m_calls.destroySpace != nullptr)
                (void)m_calls.destroySpace(space);
        }
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
        auto tracking = made<XrSystemHandTrackingPropertiesEXT>(XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT);
        if (m_handTracking)
            system.next = &tracking;
        if (XR_SUCCEEDED(m_calls.getSystemProperties(m_instance, m_system, &system)))
            m_systemName = system.systemName;
        // The runtime knows the extension; this headset may still have no
        // cameras for hands.
        m_handTracking = m_handTracking && tracking.supportsHandTracking == XR_TRUE;

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
        // **The play spaces**: the floor's when the runtime knows it (a room
        // set up), and the head's start, which every runtime has. Standing
        // uses the floor's, or the head's where there is no floor; seated,
        // the head's.
        auto spaceInfo = made<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
        spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        result = m_calls.createReferenceSpace(m_session, &spaceInfo, &m_local);
        if (XR_FAILED(result)) {
            logFailure("xrCreateReferenceSpace", result);
            return false;
        }
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        if (XR_FAILED(m_calls.createReferenceSpace(m_session, &spaceInfo, &m_stage)))
            m_stage = XR_NULL_HANDLE;
        m_space = m_stage != XR_NULL_HANDLE ? m_stage : m_local;
        auto headInfo = made<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
        headInfo.poseInReferenceSpace.orientation.w = 1.0f;
        headInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        if (XR_FAILED(m_calls.createReferenceSpace(m_session, &headInfo, &m_headSpace)))
            m_headSpace = XR_NULL_HANDLE;
        // **The controllers** (ADR 0245): a headset with none -- or a runtime
        // that refuses the set -- is still drawn to.
        if (!openControllers())
            core::log(core::LogLevel::Info, ENG_TR("rhi.info.xr_no_controllers"));
        // **The hands** (ADR 0245 §4), where the headset tracks them.
        if (m_handTracking) {
            m_calls.loadHands(m_instance);
            for (usize hand = 0; hand < 2 && m_calls.createHandTracker != nullptr; ++hand) {
                auto info = made<XrHandTrackerCreateInfoEXT>(XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT);
                info.hand = hand == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
                info.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
                if (XR_FAILED(m_calls.createHandTracker(m_session, &info, &m_handTrackers[hand])))
                    m_handTrackers[hand] = XR_NULL_HANDLE;
            }
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
        // **The room seen through** (ADR 0247 §1): the alpha blend where the
        // runtime offers it, the Quest's own passthrough where it does not.
        if (m_calls.enumerateBlendModes != nullptr) {
            u32 modes = 0;
            if (XR_SUCCEEDED(m_calls.enumerateBlendModes(
                    m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &modes, nullptr)) &&
                modes > 0) {
                std::vector<XrEnvironmentBlendMode> listed(modes);
                if (XR_SUCCEEDED(m_calls.enumerateBlendModes(
                        m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, modes, &modes, listed.data())))
                    m_alphaBlend =
                        std::find(listed.begin(), listed.end(), XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) != listed.end();
            }
        }
        if (m_fbPassthrough)
            m_calls.loadPassthrough(m_instance);
        m_fbPassthrough = m_fbPassthrough && m_calls.createPassthrough != nullptr &&
                          m_calls.createPassthroughLayer != nullptr && m_calls.startPassthrough != nullptr;
        // **Each eye's depth** (ADR 0244 §5), where the runtime takes it: a
        // headset whose runtime takes none is drawn to as before.
        if (m_depthLayer && !openDepth()) {
            m_depthLayer = false;
            core::log(core::LogLevel::Info, ENG_TR("rhi.info.xr_no_depth"));
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
        if (m_depthLayer) {
            for (usize at = 0; at < 2; ++at) {
                EyeSwapchain& eye = m_depthEyes[at];
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
                frame.depth[at] = eye.handles[index];
            }
            frame.depthFormat = m_depthFormat;
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
        std::array<XrCompositionLayerDepthInfoKHR, 2> depths{};
        auto passthroughLayer = made<XrCompositionLayerPassthroughFB>(XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB);
        std::array<const XrCompositionLayerBaseHeader*, 2> twoLayers{};
        if (m_drawing) {
            for (usize at = 0; at < 2; ++at) {
                views[at].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                views[at].pose = m_views[at].pose;
                views[at].fov = m_views[at].fov;
                views[at].subImage.swapchain = m_eyes[at].swapchain;
                views[at].subImage.imageRect.extent = {static_cast<int32_t>(m_width), static_cast<int32_t>(m_height)};
                if (m_depthLayer) {
                    XrCompositionLayerDepthInfoKHR& depth = depths[at];
                    depth.type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
                    depth.subImage.swapchain = m_depthEyes[at].swapchain;
                    depth.subImage.imageRect.extent = views[at].subImage.imageRect.extent;
                    depth.minDepth = 0.0f;
                    depth.maxDepth = 1.0f;
                    depth.nearZ = m_near;
                    depth.farZ = m_far;
                    views[at].next = &depth;
                }
            }
            layer.space = m_space;
            layer.viewCount = 2;
            layer.views = views.data();
            endInfo.layerCount = 1;
            endInfo.layers = layers;
            // **The room behind the picture** (ADR 0247 §1): where a pixel's
            // alpha is below one, through the alpha blend or under the
            // Quest's passthrough layer.
            if (m_passthroughOn) {
                layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                   XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                if (m_alphaBlend) {
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
                }
                else if (m_passthroughLayer != XR_NULL_HANDLE) {
                    passthroughLayer.layerHandle = m_passthroughLayer;
                    passthroughLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    twoLayers[0] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&passthroughLayer);
                    twoLayers[1] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);
                    endInfo.layerCount = 2;
                    endInfo.layers = twoLayers.data();
                }
            }
        }
        const XrResult result = m_calls.endFrame(m_session, &endInfo);
        if (XR_FAILED(result))
            logFailure("xrEndFrame", result);
        m_drawing = false;
    }

    void readInput(HeadsetInput& input) override
    {
        input = HeadsetInput{};
        if (!m_running || m_displayTime == 0)
            return;
        const XrTime when = m_displayTime;
        if (m_headSpace != XR_NULL_HANDLE) {
            auto location = made<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
            if (XR_SUCCEEDED(m_calls.locateSpace(m_headSpace, m_space, when, &location)))
                input.head = poseOf(location);
        }
        // Each hand's joints, where it is tracked as a hand now.
        for (usize hand = 0; hand < 2; ++hand) {
            if (m_handTrackers[hand] == XR_NULL_HANDLE || m_calls.locateHandJoints == nullptr)
                continue;
            std::array<XrHandJointLocationEXT, HandJointCount> located{};
            auto locations = made<XrHandJointLocationsEXT>(XR_TYPE_HAND_JOINT_LOCATIONS_EXT);
            locations.jointCount = static_cast<uint32_t>(located.size());
            locations.jointLocations = located.data();
            auto info = made<XrHandJointsLocateInfoEXT>(XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT);
            info.baseSpace = m_space;
            info.time = when;
            if (XR_FAILED(m_calls.locateHandJoints(m_handTrackers[hand], &info, &locations)) ||
                locations.isActive != XR_TRUE)
                continue;
            const XrSpaceLocationFlags known =
                XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            for (usize joint = 0; joint < HandJointCount; ++joint) {
                const XrHandJointLocationEXT& one = located[joint];
                HeadsetPose& pose = input.joints[hand][joint];
                pose.valid = (one.locationFlags & known) == known;
                pose.position = {one.pose.position.x, one.pose.position.y, one.pose.position.z};
                pose.orientation = {one.pose.orientation.x, one.pose.orientation.y, one.pose.orientation.z,
                                    one.pose.orientation.w};
            }
        }
        if (m_actionSet == XR_NULL_HANDLE)
            return;
        const XrActiveActionSet active{m_actionSet, XR_NULL_PATH};
        auto sync = made<XrActionsSyncInfo>(XR_TYPE_ACTIONS_SYNC_INFO);
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        // Not focused -- the system menu is up -- is not an error: every
        // action reads as at rest, which is what the player is doing here.
        if (XR_FAILED(m_calls.syncActions(m_session, &sync)))
            return;
        for (usize hand = 0; hand < 2; ++hand) {
            for (usize which = 0; which < 2; ++which) {
                const XrSpace space = m_handSpaces[which * 2 + hand];
                if (space == XR_NULL_HANDLE)
                    continue;
                auto location = made<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
                if (XR_SUCCEEDED(m_calls.locateSpace(space, m_space, when, &location)))
                    (which == 0 ? input.grip : input.aim)[hand] = poseOf(location);
            }
            for (usize at = 0; at < Actions.size(); ++at) {
                const ActionSpec& spec = Actions[at];
                const int key = spec.key[hand];
                if (key < 0 || m_actions[at] == XR_NULL_HANDLE)
                    continue;
                auto get = made<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
                get.action = m_actions[at];
                get.subactionPath = m_hands[hand];
                if (spec.kind == ActionKind::Boolean) {
                    auto state = made<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
                    if (XR_SUCCEEDED(m_calls.getBoolean(m_session, &get, &state)) && state.isActive == XR_TRUE)
                        input.keys[static_cast<usize>(key)] = state.currentState == XR_TRUE ? 1.0f : 0.0f;
                }
                else if (spec.kind == ActionKind::Float) {
                    auto state = made<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT);
                    if (XR_SUCCEEDED(m_calls.getFloat(m_session, &get, &state)) && state.isActive == XR_TRUE)
                        input.keys[static_cast<usize>(key)] = state.currentState;
                }
                else if (spec.kind == ActionKind::Stick) {
                    auto state = made<XrActionStateVector2f>(XR_TYPE_ACTION_STATE_VECTOR2F);
                    if (XR_SUCCEEDED(m_calls.getVector2(m_session, &get, &state)) && state.isActive == XR_TRUE) {
                        input.keys[static_cast<usize>(key)] = state.currentState.x;
                        input.keys[static_cast<usize>(key) + 1] = state.currentState.y;
                    }
                }
            }
        }
    }

    [[nodiscard]] bool supportsPassthrough() const noexcept override { return m_alphaBlend || m_fbPassthrough; }

    void setPassthrough(bool on) override
    {
        on = on && supportsPassthrough();
        if (on == m_passthroughOn)
            return;
        m_passthroughOn = on;
        if (m_alphaBlend)
            return;
        // The Quest's: made the first time it is asked for, then paused and
        // resumed -- a layer under the picture, the cameras' view.
        if (on && m_passthrough == XR_NULL_HANDLE) {
            auto info = made<XrPassthroughCreateInfoFB>(XR_TYPE_PASSTHROUGH_CREATE_INFO_FB);
            info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
            if (XR_FAILED(m_calls.createPassthrough(m_session, &info, &m_passthrough))) {
                m_passthrough = XR_NULL_HANDLE;
                m_passthroughOn = false;
                return;
            }
            auto layerInfo = made<XrPassthroughLayerCreateInfoFB>(XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB);
            layerInfo.passthrough = m_passthrough;
            layerInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
            layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
            if (XR_FAILED(m_calls.createPassthroughLayer(m_session, &layerInfo, &m_passthroughLayer))) {
                m_passthroughLayer = XR_NULL_HANDLE;
                m_passthroughOn = false;
            }
            return;
        }
        if (m_passthroughLayer == XR_NULL_HANDLE)
            return;
        if (on && m_calls.resumePassthroughLayer != nullptr)
            (void)m_calls.resumePassthroughLayer(m_passthroughLayer);
        else if (!on && m_calls.pausePassthroughLayer != nullptr)
            (void)m_calls.pausePassthroughLayer(m_passthroughLayer);
    }

    void setDepthRange(f32 nearZ, f32 farZ) override
    {
        m_near = nearZ;
        m_far = farZ;
    }

    void setSpace(HeadsetSpace space) override
    {
        const XrSpace wanted = space == HeadsetSpace::Seated || m_stage == XR_NULL_HANDLE ? m_local : m_stage;
        if (wanted != XR_NULL_HANDLE)
            m_space = wanted;
    }

    [[nodiscard]] std::optional<std::array<f32, 2>> playArea() const override
    {
        if (m_stage == XR_NULL_HANDLE || m_calls.getBoundsRect == nullptr)
            return std::nullopt;
        XrExtent2Df bounds{};
        if (m_calls.getBoundsRect(m_session, XR_REFERENCE_SPACE_TYPE_STAGE, &bounds) != XR_SUCCESS ||
            bounds.width <= 0.0f || bounds.height <= 0.0f)
            return std::nullopt;
        return std::array<f32, 2>{bounds.width, bounds.height};
    }

    void vibrate(u32 hand, f32 amplitude, f32 seconds) override
    {
        const usize haptic = Actions.size() - 1;
        if (hand > 1 || m_actions[haptic] == XR_NULL_HANDLE || !m_running)
            return;
        auto info = made<XrHapticActionInfo>(XR_TYPE_HAPTIC_ACTION_INFO);
        info.action = m_actions[haptic];
        info.subactionPath = m_hands[hand];
        auto vibration = made<XrHapticVibration>(XR_TYPE_HAPTIC_VIBRATION);
        vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
        vibration.duration = static_cast<XrDuration>(std::max(seconds, 0.0f) * 1.0e9f);
        vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
        (void)m_calls.applyHaptics(m_session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
    }

private:
    // The action set, its actions, the hands' spaces and the bindings each
    // known controller is suggested; attached to the session once, before
    // it begins.
    [[nodiscard]] bool openControllers()
    {
        if (XR_FAILED(m_calls.stringToPath(m_instance, "/user/hand/left", &m_hands[0])) ||
            XR_FAILED(m_calls.stringToPath(m_instance, "/user/hand/right", &m_hands[1])))
            return false;
        auto setInfo = made<XrActionSetCreateInfo>(XR_TYPE_ACTION_SET_CREATE_INFO);
        std::snprintf(setInfo.actionSetName, sizeof(setInfo.actionSetName), "%s", "gameplay");
        std::snprintf(setInfo.localizedActionSetName, sizeof(setInfo.localizedActionSetName), "%s", "Gameplay");
        if (XR_FAILED(m_calls.createActionSet(m_instance, &setInfo, &m_actionSet)))
            return false;
        for (usize at = 0; at < Actions.size(); ++at) {
            const ActionSpec& spec = Actions[at];
            auto info = made<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO);
            std::snprintf(info.actionName, sizeof(info.actionName), "%s", spec.name);
            std::snprintf(info.localizedActionName, sizeof(info.localizedActionName), "%s", spec.name);
            info.actionType = spec.kind == ActionKind::Boolean ? XR_ACTION_TYPE_BOOLEAN_INPUT
                              : spec.kind == ActionKind::Float ? XR_ACTION_TYPE_FLOAT_INPUT
                              : spec.kind == ActionKind::Stick ? XR_ACTION_TYPE_VECTOR2F_INPUT
                              : spec.kind == ActionKind::Pose  ? XR_ACTION_TYPE_POSE_INPUT
                                                               : XR_ACTION_TYPE_VIBRATION_OUTPUT;
            info.countSubactionPaths = 2;
            info.subactionPaths = m_hands.data();
            if (XR_FAILED(m_calls.createAction(m_actionSet, &info, &m_actions[at])))
                m_actions[at] = XR_NULL_HANDLE;
        }
        // A profile the runtime does not know is refused alone; the others
        // still stand.
        for (const ProfileSpec& profile : Profiles) {
            std::vector<XrActionSuggestedBinding> bindings;
            for (usize hand = 0; hand < 2; ++hand) {
                const auto& paths = hand == 0 ? profile.left : profile.right;
                for (usize at = 0; at < Actions.size(); ++at) {
                    if (paths[at][0] == '\0' || m_actions[at] == XR_NULL_HANDLE)
                        continue;
                    const std::string full =
                        std::string(hand == 0 ? "/user/hand/left/" : "/user/hand/right/") + paths[at];
                    XrPath path = XR_NULL_PATH;
                    if (XR_SUCCEEDED(m_calls.stringToPath(m_instance, full.c_str(), &path)))
                        bindings.push_back(XrActionSuggestedBinding{m_actions[at], path});
                }
            }
            XrPath profilePath = XR_NULL_PATH;
            if (bindings.empty() || XR_FAILED(m_calls.stringToPath(m_instance, profile.profile, &profilePath)))
                continue;
            auto suggested = made<XrInteractionProfileSuggestedBinding>(XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
            suggested.interactionProfile = profilePath;
            suggested.countSuggestedBindings = static_cast<u32>(bindings.size());
            suggested.suggestedBindings = bindings.data();
            (void)m_calls.suggestBindings(m_instance, &suggested);
        }
        for (usize which = 0; which < 2; ++which) {
            if (m_actions[which] == XR_NULL_HANDLE)
                continue;
            for (usize hand = 0; hand < 2; ++hand) {
                auto spaceInfo = made<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
                spaceInfo.action = m_actions[which];
                spaceInfo.subactionPath = m_hands[hand];
                spaceInfo.poseInActionSpace.orientation.w = 1.0f;
                if (XR_FAILED(m_calls.createActionSpace(m_session, &spaceInfo, &m_handSpaces[which * 2 + hand])))
                    m_handSpaces[which * 2 + hand] = XR_NULL_HANDLE;
            }
        }
        auto attach = made<XrSessionActionSetsAttachInfo>(XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
        attach.countActionSets = 1;
        attach.actionSets = &m_actionSet;
        const XrResult attached = m_calls.attachActionSets(m_session, &attach);
        if (XR_FAILED(attached)) {
            logFailure("xrAttachSessionActionSets", attached);
            return false;
        }
        return true;
    }

    void releaseImages()
    {
        for (EyeSwapchain& eye : m_eyes) {
            if (!eye.acquired)
                continue;
            auto releaseInfo = made<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
            (void)m_calls.releaseSwapchainImage(eye.swapchain, &releaseInfo);
            eye.acquired = false;
        }
        for (EyeSwapchain& eye : m_depthEyes) {
            if (!eye.acquired)
                continue;
            auto releaseInfo = made<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
            (void)m_calls.releaseSwapchainImage(eye.swapchain, &releaseInfo);
            eye.acquired = false;
        }
    }

    // **A depth swapchain for each eye**: in a depth format the runtime lists,
    // as the backend SDL drew the session on names it -- SDL lists colour
    // formats only.
    [[nodiscard]] bool openDepth()
    {
        if (m_calls.enumerateSwapchainFormats == nullptr)
            return false;
        u32 count = 0;
        if (XR_FAILED(m_calls.enumerateSwapchainFormats(m_session, 0, &count, nullptr)) || count == 0)
            return false;
        std::vector<int64_t> listed(count);
        if (XR_FAILED(m_calls.enumerateSwapchainFormats(m_session, count, &count, listed.data())))
            return false;
        const std::string_view driver = SDL_GetGPUDeviceDriver(m_hooks.device);
        const bool vulkan = driver == "vulkan";
        // Vulkan's VkFormat and Direct3D's DXGI_FORMAT, the best first.
        struct Candidate
        {
            int64_t native;
            SDL_GPUTextureFormat sdl;
            TextureFormat engine;
        };
        const std::array<Candidate, 3> candidates =
            vulkan ? std::array<Candidate, 3>{Candidate{126, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, TextureFormat::D32Float},
                                              Candidate{129, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
                                                        TextureFormat::D24UnormS8Uint},
                                              Candidate{124, SDL_GPU_TEXTUREFORMAT_D16_UNORM, TextureFormat::D16Unorm}}
                   : std::array<Candidate, 3>{
                         Candidate{40, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, TextureFormat::D32Float},
                         Candidate{45, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, TextureFormat::D24UnormS8Uint},
                         Candidate{55, SDL_GPU_TEXTUREFORMAT_D16_UNORM, TextureFormat::D16Unorm}};
        const Candidate* chosen = nullptr;
        for (const Candidate& candidate : candidates) {
            if (std::find(listed.begin(), listed.end(), candidate.native) != listed.end()) {
                chosen = &candidate;
                break;
            }
        }
        if (chosen == nullptr)
            return false;
        for (EyeSwapchain& eye : m_depthEyes) {
            auto info = made<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
            info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            info.sampleCount = 1;
            info.width = m_width;
            info.height = m_height;
            info.faceCount = 1;
            info.arraySize = 1;
            info.mipCount = 1;
            const XrResult result =
                SDL_CreateGPUXRSwapchain(m_hooks.device, m_session, &info, chosen->sdl, &eye.swapchain, &eye.images);
            if (XR_FAILED(result)) {
                logFailure("SDL_CreateGPUXRSwapchain", result);
                return false;
            }
            u32 images = 0;
            (void)m_calls.enumerateSwapchainImages(eye.swapchain, 0, &images, nullptr);
            for (u32 at = 0; at < images; ++at)
                eye.handles.push_back(m_hooks.wrap(eye.images[at], chosen->engine, m_width, m_height));
        }
        m_depthFormat = chosen->engine;
        return true;
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
    // The space every pose and view is located in: one of the two below.
    XrSpace m_space = XR_NULL_HANDLE;
    XrSpace m_stage = XR_NULL_HANDLE;
    XrSpace m_local = XR_NULL_HANDLE;
    XrSpace m_headSpace = XR_NULL_HANDLE;
    XrActionSet m_actionSet = XR_NULL_HANDLE;
    std::array<XrAction, Actions.size()> m_actions{};
    std::array<XrPath, 2> m_hands{};
    // Grip then aim, each left then right.
    std::array<XrSpace, 4> m_handSpaces{};
    // **The room seen through**: whether the runtime blends by alpha, has the
    // Quest's passthrough, and whether it is on; and the Quest's objects.
    bool m_alphaBlend = false;
    bool m_fbPassthrough = false;
    bool m_passthroughOn = false;
    XrPassthroughFB m_passthrough = XR_NULL_HANDLE;
    XrPassthroughLayerFB m_passthroughLayer = XR_NULL_HANDLE;
    // **Each eye's depth swapchain**, where the runtime takes depth, its
    // format, and what its values mean.
    bool m_depthLayer = false;
    std::array<EyeSwapchain, 2> m_depthEyes{};
    TextureFormat m_depthFormat = TextureFormat::Undefined;
    f32 m_near = 0.1f;
    f32 m_far = 5000.0f;
    // Whether the runtime was asked for hand tracking and the headset has
    // it, and each hand's tracker.
    bool m_handTracking = false;
    std::array<XrHandTrackerEXT, 2> m_handTrackers{};
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
    // **The extensions the runtime lists, of those asked**: the list is read
    // through the loader before any instance exists.
    extensions.clear();
    handTracking = false;
    depthLayer = false;
    fbPassthrough = false;
    if (SDL_OpenXR_LoadLibrary()) {
        const auto getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(SDL_OpenXR_GetXrGetInstanceProcAddr());
        PFN_xrVoidFunction found = nullptr;
        if (getProc != nullptr &&
            XR_SUCCEEDED(getProc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &found)) &&
            found != nullptr) {
            const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(found);
            uint32_t count = 0;
            if (XR_SUCCEEDED(enumerate(nullptr, 0, &count, nullptr)) && count > 0) {
                std::vector<XrExtensionProperties> listed(count,
                                                          made<XrExtensionProperties>(XR_TYPE_EXTENSION_PROPERTIES));
                if (XR_SUCCEEDED(enumerate(nullptr, count, &count, listed.data()))) {
                    for (const XrExtensionProperties& one : listed) {
                        const std::string_view name{one.extensionName};
                        if (request.hands && name == XR_EXT_HAND_TRACKING_EXTENSION_NAME)
                            handTracking = true;
                        if (request.depth && name == XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME)
                            depthLayer = true;
                        if (name == XR_FB_PASSTHROUGH_EXTENSION_NAME)
                            fbPassthrough = true;
                    }
                }
            }
        }
        SDL_OpenXR_UnloadLibrary();
    }
    if (handTracking)
        extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    if (depthLayer)
        extensions.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    if (fbPassthrough)
        extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (!extensions.empty()) {
        SDL_SetNumberProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_EXTENSION_COUNT_NUMBER,
                              static_cast<Sint64>(extensions.size()));
        SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_XR_EXTENSION_NAMES_POINTER, extensions.data());
    }
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
    auto headset =
        std::make_unique<OpenXrHeadset>(reinterpret_cast<XrInstance>(ask.instance), static_cast<XrSystemId>(ask.system),
                                        std::move(hooks), ask.handTracking, ask.depthLayer, ask.fbPassthrough);
    if (!headset->open())
        return nullptr;
    return headset;
}

} // namespace engine::rhi::sdlgpu
