#include "sdlgpu_xr.h"

// OpenXR's own header first: SDL's then leaves its stand-in types out.
// clang-format off
#include <openxr/openxr.h>
#include <SDL3/SDL_openxr.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_timer.h>
// clang-format on

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/math.h"
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
    // XR_META_performance_metrics'.
    PFN_xrSetPerformanceMetricsStateMETA setMetricsState = nullptr;
    PFN_xrQueryPerformanceMetricsCounterMETA queryMetric = nullptr;
    // XR_FB_spatial_entity, its query and XR_FB_scene's.
    PFN_xrQuerySpacesFB querySpaces = nullptr;
    PFN_xrRetrieveSpaceQueryResultsFB retrieveSpaces = nullptr;
    PFN_xrGetSpaceComponentStatusFB componentStatus = nullptr;
    PFN_xrGetSpaceBoundingBox2DFB boundingBox2D = nullptr;
    PFN_xrGetSpaceSemanticLabelsFB semanticLabels = nullptr;
    // XR_FB_spatial_entity's anchors, and XR_FB_spatial_entity_storage's.
    PFN_xrCreateSpatialAnchorFB createAnchor = nullptr;
    PFN_xrSetSpaceComponentStatusFB setComponentStatus = nullptr;
    PFN_xrSaveSpaceFB saveSpace = nullptr;

    void loadScene(XrInstance instance)
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
        get("xrQuerySpacesFB", querySpaces);
        get("xrRetrieveSpaceQueryResultsFB", retrieveSpaces);
        get("xrGetSpaceComponentStatusFB", componentStatus);
        get("xrGetSpaceBoundingBox2DFB", boundingBox2D);
        get("xrGetSpaceSemanticLabelsFB", semanticLabels);
        get("xrCreateSpatialAnchorFB", createAnchor);
        get("xrSetSpaceComponentStatusFB", setComponentStatus);
        get("xrSaveSpaceFB", saveSpace);
    }

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

    void loadMetrics(XrInstance instance)
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
        get("xrSetPerformanceMetricsStateMETA", setMetricsState);
        get("xrQueryPerformanceMetricsCounterMETA", queryMetric);
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
                  bool fbPassthrough, bool metrics, bool scene, bool anchors)
        : m_instance(instance), m_system(system), m_hooks(std::move(hooks)), m_scene(scene), m_anchors(anchors),
          m_metrics(metrics), m_fbPassthrough(fbPassthrough), m_depthLayer(depthLayer), m_handTracking(handTracking)
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
        if (m_scene || m_anchors)
            m_calls.loadScene(m_instance);
        m_scene = m_scene && m_calls.querySpaces != nullptr && m_calls.retrieveSpaces != nullptr &&
                  m_calls.componentStatus != nullptr && m_calls.boundingBox2D != nullptr;
        m_anchors = m_anchors && m_calls.createAnchor != nullptr && m_calls.setComponentStatus != nullptr &&
                    m_calls.saveSpace != nullptr && m_calls.querySpaces != nullptr && m_calls.retrieveSpaces != nullptr;
        // **The runtime's measurements** of the game's frames, where it
        // offers them: turned on once, read when the timing is taken.
        if (m_metrics) {
            m_calls.loadMetrics(m_instance);
            auto state = made<XrPerformanceMetricsStateMETA>(XR_TYPE_PERFORMANCE_METRICS_STATE_META);
            state.enabled = XR_TRUE;
            m_metrics = m_calls.setMetricsState != nullptr && m_calls.queryMetric != nullptr &&
                        XR_SUCCEEDED(m_calls.setMetricsState(m_session, &state));
        }
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
            else if (event.type == XR_TYPE_EVENT_DATA_SPACE_QUERY_RESULTS_AVAILABLE_FB) {
                const auto& available = reinterpret_cast<const XrEventDataSpaceQueryResultsAvailableFB&>(event);
                if (available.requestId == m_roomQuery)
                    collectRoom();
                else
                    collectAnchors(available.requestId);
            }
            else if (event.type == XR_TYPE_EVENT_DATA_SPACE_QUERY_COMPLETE_FB) {
                const auto& complete = reinterpret_cast<const XrEventDataSpaceQueryCompleteFB&>(event);
                if (complete.requestId == m_roomQuery && m_roomAsked) {
                    m_roomAsked = false;
                    m_roomReady = true;
                }
                else if (const std::optional<AnchorWait> wait = anchorWait(complete.requestId); wait.has_value()) {
                    m_anchorEvents.push_back({HeadsetAnchorEvent::Kind::Done, wait->ask, 0, {}, true});
                }
            }
            else if (event.type == XR_TYPE_EVENT_DATA_SPATIAL_ANCHOR_CREATE_COMPLETE_FB) {
                const auto& done = reinterpret_cast<const XrEventDataSpatialAnchorCreateCompleteFB&>(event);
                anchorCreated(done.requestId, done.result, done.space, done.uuid);
            }
            else if (event.type == XR_TYPE_EVENT_DATA_SPACE_SET_STATUS_COMPLETE_FB) {
                const auto& done = reinterpret_cast<const XrEventDataSpaceSetStatusCompleteFB&>(event);
                if (const std::optional<AnchorWait> wait = anchorWait(done.requestId); wait.has_value())
                    m_anchorEvents.push_back(
                        {HeadsetAnchorEvent::Kind::Created, wait->ask, handleOf(wait->space), wait->uuid, true});
            }
            else if (event.type == XR_TYPE_EVENT_DATA_SPACE_SAVE_COMPLETE_FB) {
                const auto& done = reinterpret_cast<const XrEventDataSpaceSaveCompleteFB&>(event);
                if (const std::optional<AnchorWait> wait = anchorWait(done.requestId); wait.has_value())
                    m_anchorEvents.push_back(
                        {HeadsetAnchorEvent::Kind::Saved, wait->ask, 0, {}, XR_SUCCEEDED(done.result)});
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
        // **How the frames go** (`takeTiming`): when each came back from its
        // wait, against the display's period.
        {
            const i64 now = static_cast<i64>(SDL_GetTicksNS());
            m_timing.displayPeriodNs = timing.predictedDisplayPeriod;
            if (m_lastWaitNs != 0) {
                const i64 between = now - m_lastWaitNs;
                m_timing.spanNs += between;
                if (timing.predictedDisplayPeriod > 0 && between > timing.predictedDisplayPeriod * 3 / 2)
                    m_timing.late += 1;
            }
            m_lastWaitNs = now;
            m_timing.frames += 1;
            if (timing.shouldRender == XR_FALSE)
                m_timing.notDrawn += 1;
        }
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

    [[nodiscard]] HeadsetTiming takeTiming() override
    {
        HeadsetTiming taken = m_timing;
        m_timing = HeadsetTiming{};
        m_timing.displayPeriodNs = taken.displayPeriodNs;
        if (m_metrics) {
            const auto read = [this](const char* name, f32& asFloat, i64& asInt) {
                XrPath path = XR_NULL_PATH;
                if (XR_FAILED(m_calls.stringToPath(m_instance, name, &path)))
                    return false;
                auto counter = made<XrPerformanceMetricsCounterMETA>(XR_TYPE_PERFORMANCE_METRICS_COUNTER_META);
                if (XR_FAILED(m_calls.queryMetric(m_session, path, &counter)))
                    return false;
                if ((counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META) != 0)
                    asFloat = counter.floatValue;
                if ((counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META) != 0)
                    asInt = static_cast<i64>(counter.uintValue);
                return true;
            };
            i64 unused = 0;
            f32 unusedFloat = 0.0f;
            f32 dropped = -1.0f;
            (void)read("/perfmetrics_meta/app/gpu_frametime", taken.appGpuMs, unused);
            (void)read("/perfmetrics_meta/app/cpu_frametime", taken.appCpuMs, unused);
            if (!read("/perfmetrics_meta/compositor/dropped_frame_count", dropped, taken.compositorDropped))
                taken.compositorDropped = -1;
            else if (taken.compositorDropped < 0 && dropped >= 0.0f)
                taken.compositorDropped = static_cast<i64>(dropped);
            (void)unusedFloat;
        }
        return taken;
    }

    void requestRoom() override
    {
        if (m_roomAsked)
            return;
        m_room = HeadsetRoom{};
        if (!m_scene) {
            m_roomReady = true;
            return;
        }
#if defined(SDL_PLATFORM_ANDROID)
        // **The player is asked first**, on the Quest: the room is theirs.
        // The answer comes on another thread; the query waits for it.
        m_roomAsked = true;
        m_permission = 0;
        if (!SDL_RequestAndroidPermission(
                "com.oculus.permission.USE_SCENE",
                [](void* self, const char*, bool granted) {
                    static_cast<OpenXrHeadset*>(self)->m_permission.store(granted ? 1 : 2);
                },
                this)) {
            m_roomAsked = false;
            m_roomReady = true;
        }
#else
        m_roomAsked = true;
        m_permission = 1;
#endif
    }

    [[nodiscard]] bool takeRoom(HeadsetRoom& out) override
    {
        // The player's answer, then the runtime's query.
        if (m_roomAsked && m_roomQuery == 0 && m_permission.load() != 0) {
            if (m_permission.load() == 2) {
                m_roomAsked = false;
                m_roomReady = true;
            }
            else {
                auto filter = made<XrSpaceComponentFilterInfoFB>(XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB);
                filter.componentType = XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB;
                auto info = made<XrSpaceQueryInfoFB>(XR_TYPE_SPACE_QUERY_INFO_FB);
                info.queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB;
                info.maxResultCount = 256;
                info.timeout = XR_INFINITE_DURATION;
                info.filter = reinterpret_cast<const XrSpaceFilterInfoBaseHeaderFB*>(&filter);
                if (XR_FAILED(m_calls.querySpaces(
                        m_session, reinterpret_cast<const XrSpaceQueryInfoBaseHeaderFB*>(&info), &m_roomQuery))) {
                    m_roomQuery = 0;
                    m_roomAsked = false;
                    m_roomReady = true;
                }
            }
        }
        if (!m_roomReady)
            return false;
        m_roomReady = false;
        m_roomQuery = 0;
        out = std::move(m_room);
        m_room = HeadsetRoom{};
        return true;
    }

    [[nodiscard]] u32 createAnchor(const HeadsetPose& pose) override
    {
        if (!m_anchors || m_session == XR_NULL_HANDLE || m_displayTime == 0)
            return 0;
        auto info = made<XrSpatialAnchorCreateInfoFB>(XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_FB);
        info.space = m_space;
        info.poseInSpace.position = {pose.position[0], pose.position[1], pose.position[2]};
        info.poseInSpace.orientation = {pose.orientation[0], pose.orientation[1], pose.orientation[2],
                                        pose.orientation[3]};
        info.time = m_displayTime;
        XrAsyncRequestIdFB request = 0;
        if (XR_FAILED(m_calls.createAnchor(m_session, &info, &request)))
            return 0;
        return awaitAnchor(request);
    }

    [[nodiscard]] u32 saveAnchor(u64 anchor) override
    {
        if (!m_anchors || m_session == XR_NULL_HANDLE || anchor == 0)
            return 0;
        auto info = made<XrSpaceSaveInfoFB>(XR_TYPE_SPACE_SAVE_INFO_FB);
        info.space = spaceOf(anchor);
        info.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
        info.persistenceMode = XR_SPACE_PERSISTENCE_MODE_INDEFINITE_FB;
        XrAsyncRequestIdFB request = 0;
        if (XR_FAILED(m_calls.saveSpace(m_session, &info, &request)))
            return 0;
        return awaitAnchor(request);
    }

    // **Every anchor this app saved on the headset**: those that can be
    // located, kept in its own storage.
    [[nodiscard]] u32 loadAnchors() override
    {
        if (!m_anchors || m_session == XR_NULL_HANDLE)
            return 0;
        auto storage = made<XrSpaceStorageLocationFilterInfoFB>(XR_TYPE_SPACE_STORAGE_LOCATION_FILTER_INFO_FB);
        storage.location = XR_SPACE_STORAGE_LOCATION_LOCAL_FB;
        auto filter = made<XrSpaceComponentFilterInfoFB>(XR_TYPE_SPACE_COMPONENT_FILTER_INFO_FB);
        filter.next = &storage;
        filter.componentType = XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB;
        auto info = made<XrSpaceQueryInfoFB>(XR_TYPE_SPACE_QUERY_INFO_FB);
        info.queryAction = XR_SPACE_QUERY_ACTION_LOAD_FB;
        info.maxResultCount = 64;
        info.timeout = XR_INFINITE_DURATION;
        info.filter = reinterpret_cast<const XrSpaceFilterInfoBaseHeaderFB*>(&filter);
        XrAsyncRequestIdFB request = 0;
        if (XR_FAILED(
                m_calls.querySpaces(m_session, reinterpret_cast<const XrSpaceQueryInfoBaseHeaderFB*>(&info), &request)))
            return 0;
        return awaitAnchor(request);
    }

    void destroyAnchor(u64 anchor) override
    {
        if (anchor != 0 && m_calls.destroySpace != nullptr)
            (void)m_calls.destroySpace(spaceOf(anchor));
    }

    [[nodiscard]] HeadsetPose locateAnchor(u64 anchor) override
    {
        auto location = made<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
        if (anchor == 0 || m_displayTime == 0 ||
            XR_FAILED(m_calls.locateSpace(spaceOf(anchor), m_space, m_displayTime, &location)))
            return {};
        return poseOf(location);
    }

    void takeAnchorEvents(std::vector<HeadsetAnchorEvent>& out) override
    {
        for (HeadsetAnchorEvent& one : m_anchorEvents)
            out.push_back(std::move(one));
        m_anchorEvents.clear();
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

    // **What a room query found**: each space with a rectangle, where it is in
    // the play space, how big, and what the scene calls it.
    void collectRoom()
    {
        auto results = made<XrSpaceQueryResultsFB>(XR_TYPE_SPACE_QUERY_RESULTS_FB);
        if (XR_FAILED(m_calls.retrieveSpaces(m_session, m_roomQuery, &results)) || results.resultCountOutput == 0)
            return;
        std::vector<XrSpaceQueryResultFB> found(results.resultCountOutput);
        results.resultCapacityInput = static_cast<u32>(found.size());
        results.results = found.data();
        if (XR_FAILED(m_calls.retrieveSpaces(m_session, m_roomQuery, &results)))
            return;
        m_room.known = true;
        for (const XrSpaceQueryResultFB& one : found) {
            auto status = made<XrSpaceComponentStatusFB>(XR_TYPE_SPACE_COMPONENT_STATUS_FB);
            if (XR_FAILED(m_calls.componentStatus(one.space, XR_SPACE_COMPONENT_TYPE_BOUNDED_2D_FB, &status)) ||
                status.enabled != XR_TRUE)
                continue;
            XrRect2Df rect{};
            if (XR_FAILED(m_calls.boundingBox2D(m_session, one.space, &rect)))
                continue;
            auto location = made<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
            if (XR_FAILED(m_calls.locateSpace(one.space, m_space, m_displayTime, &location)))
                continue;
            const HeadsetPose origin = poseOf(location);
            if (!origin.valid)
                continue;
            HeadsetPlane plane;
            plane.width = rect.extent.width;
            plane.height = rect.extent.height;
            // The rectangle's middle, from the space's origin along its own x and y.
            const core::Mat3 turn = core::fromQuaternion(origin.orientation[0], origin.orientation[1],
                                                         origin.orientation[2], origin.orientation[3]);
            const core::Vec3 middle = turn * core::Vec3{rect.offset.x + rect.extent.width * 0.5f,
                                                        rect.offset.y + rect.extent.height * 0.5f, 0.0f};
            plane.pose = origin;
            plane.pose.position = {origin.position[0] + middle.x, origin.position[1] + middle.y,
                                   origin.position[2] + middle.z};
            plane.kind = 7;
            if (m_calls.semanticLabels != nullptr) {
                auto labels = made<XrSemanticLabelsFB>(XR_TYPE_SEMANTIC_LABELS_FB);
                if (XR_SUCCEEDED(m_calls.semanticLabels(m_session, one.space, &labels)) &&
                    labels.bufferCountOutput > 0) {
                    std::string text(labels.bufferCountOutput, '\0');
                    labels.bufferCapacityInput = labels.bufferCountOutput;
                    labels.buffer = text.data();
                    if (XR_SUCCEEDED(m_calls.semanticLabels(m_session, one.space, &labels)))
                        plane.kind = kindOf(text);
                }
            }
            m_room.planes.push_back(plane);
        }
    }

    // A runtime request an anchor's ask waits on -- a new anchor's own,
    // while it is made storable.
    struct AnchorWait
    {
        XrAsyncRequestIdFB request = 0;
        u32 ask = 0;
        XrSpace space = XR_NULL_HANDLE;
        std::string uuid;
    };

    [[nodiscard]] static u64 handleOf(XrSpace space) noexcept { return reinterpret_cast<u64>(space); }
    [[nodiscard]] static XrSpace spaceOf(u64 anchor) noexcept { return reinterpret_cast<XrSpace>(anchor); }

    [[nodiscard]] static std::string uuidText(const XrUuidEXT& uuid)
    {
        static constexpr char Digits[] = "0123456789abcdef";
        std::string text;
        text.reserve(32);
        for (const u8 byte : uuid.data) {
            text.push_back(Digits[byte >> 4]);
            text.push_back(Digits[byte & 15]);
        }
        return text;
    }

    [[nodiscard]] u32 awaitAnchor(XrAsyncRequestIdFB request)
    {
        const u32 ask = m_nextAnchorAsk++;
        m_anchorWaits.push_back({request, ask, XR_NULL_HANDLE, {}});
        return ask;
    }

    // Takes the wait a runtime request answers, where it is an anchor's.
    [[nodiscard]] std::optional<AnchorWait> anchorWait(XrAsyncRequestIdFB request)
    {
        const auto found = std::find_if(m_anchorWaits.begin(), m_anchorWaits.end(),
                                        [&](const AnchorWait& wait) { return wait.request == request; });
        if (found == m_anchorWaits.end())
            return std::nullopt;
        AnchorWait wait = std::move(*found);
        m_anchorWaits.erase(found);
        return wait;
    }

    // **A new anchor**, made storable before it is handed over, so that
    // saving it is one step.
    void anchorCreated(XrAsyncRequestIdFB request, XrResult result, XrSpace space, const XrUuidEXT& uuid)
    {
        const std::optional<AnchorWait> wait = anchorWait(request);
        if (!wait.has_value())
            return;
        if (XR_FAILED(result) || space == XR_NULL_HANDLE) {
            m_anchorEvents.push_back({HeadsetAnchorEvent::Kind::Created, wait->ask, 0, {}, false});
            return;
        }
        auto info = made<XrSpaceComponentStatusSetInfoFB>(XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB);
        info.componentType = XR_SPACE_COMPONENT_TYPE_STORABLE_FB;
        info.enabled = XR_TRUE;
        XrAsyncRequestIdFB next = 0;
        if (XR_SUCCEEDED(m_calls.setComponentStatus(space, &info, &next))) {
            m_anchorWaits.push_back({next, wait->ask, space, uuidText(uuid)});
            return;
        }
        m_anchorEvents.push_back({HeadsetAnchorEvent::Kind::Created, wait->ask, handleOf(space), uuidText(uuid), true});
    }

    // **What a load found**: each saved anchor, made locatable -- a loaded
    // space is not until it is asked to be.
    void collectAnchors(XrAsyncRequestIdFB request)
    {
        const auto wait = std::find_if(m_anchorWaits.begin(), m_anchorWaits.end(),
                                       [&](const AnchorWait& one) { return one.request == request; });
        if (wait == m_anchorWaits.end())
            return;
        auto results = made<XrSpaceQueryResultsFB>(XR_TYPE_SPACE_QUERY_RESULTS_FB);
        if (XR_FAILED(m_calls.retrieveSpaces(m_session, request, &results)) || results.resultCountOutput == 0)
            return;
        std::vector<XrSpaceQueryResultFB> found(results.resultCountOutput);
        results.resultCapacityInput = static_cast<u32>(found.size());
        results.results = found.data();
        if (XR_FAILED(m_calls.retrieveSpaces(m_session, request, &results)))
            return;
        for (const XrSpaceQueryResultFB& one : found) {
            auto info = made<XrSpaceComponentStatusSetInfoFB>(XR_TYPE_SPACE_COMPONENT_STATUS_SET_INFO_FB);
            info.componentType = XR_SPACE_COMPONENT_TYPE_LOCATABLE_FB;
            info.enabled = XR_TRUE;
            XrAsyncRequestIdFB ignored = 0;
            (void)m_calls.setComponentStatus(one.space, &info, &ignored);
            m_anchorEvents.push_back(
                {HeadsetAnchorEvent::Kind::Found, wait->ask, handleOf(one.space), uuidText(one.uuid), true});
        }
    }

    // `Enum.XRPlaneKind` from the scene's first label.
    [[nodiscard]] static u8 kindOf(std::string_view labels) noexcept
    {
        const std::string_view first = labels.substr(0, labels.find(','));
        if (first.starts_with("FLOOR"))
            return 0;
        if (first.starts_with("CEILING"))
            return 1;
        if (first.starts_with("WALL") || first.starts_with("INVISIBLE_WALL"))
            return 2;
        if (first.starts_with("TABLE"))
            return 3;
        if (first.starts_with("COUCH"))
            return 4;
        if (first.starts_with("DOOR"))
            return 5;
        if (first.starts_with("WINDOW"))
            return 6;
        return 7;
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
    // **The room**: whether the runtime has the scene, whether it was asked
    // for, the player's answer (0 none yet, 1 yes, 2 no), the query, and the
    // planes found so far.
    bool m_scene = false;
    // **Anchors**: whether the runtime has them, the runtime's requests in
    // flight, the next ask's number, and the answers for the host.
    bool m_anchors = false;
    std::vector<AnchorWait> m_anchorWaits;
    u32 m_nextAnchorAsk = 1;
    std::vector<HeadsetAnchorEvent> m_anchorEvents;
    bool m_roomAsked = false;
    bool m_roomReady = false;
    std::atomic<int> m_permission{0};
    XrAsyncRequestIdFB m_roomQuery = 0;
    HeadsetRoom m_room;
    // **How the frames have gone**, since the timing was last taken; when the
    // last wait came back; and whether the runtime measures the game too.
    HeadsetTiming m_timing{};
    i64 m_lastWaitNs = 0;
    bool m_metrics = false;
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
    metrics = false;
    scene = false;
    anchors = false;
    bool storage = false;
    bool entity = false;
    bool query = false;
    bool sceneListed = false;
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
                        if (name == XR_META_PERFORMANCE_METRICS_EXTENSION_NAME)
                            metrics = true;
                        if (name == XR_FB_SPATIAL_ENTITY_EXTENSION_NAME)
                            entity = true;
                        if (name == XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME)
                            query = true;
                        if (name == XR_FB_SCENE_EXTENSION_NAME)
                            sceneListed = true;
                        if (name == XR_FB_SPATIAL_ENTITY_STORAGE_EXTENSION_NAME)
                            storage = true;
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
    if (metrics)
        extensions.push_back(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
    scene = entity && query && sceneListed;
    anchors = entity && query && storage;
    if (scene || anchors) {
        extensions.push_back(XR_FB_SPATIAL_ENTITY_EXTENSION_NAME);
        extensions.push_back(XR_FB_SPATIAL_ENTITY_QUERY_EXTENSION_NAME);
    }
    if (scene)
        extensions.push_back(XR_FB_SCENE_EXTENSION_NAME);
    if (anchors)
        extensions.push_back(XR_FB_SPATIAL_ENTITY_STORAGE_EXTENSION_NAME);
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
    auto headset = std::make_unique<OpenXrHeadset>(
        reinterpret_cast<XrInstance>(ask.instance), static_cast<XrSystemId>(ask.system), std::move(hooks),
        ask.handTracking, ask.depthLayer, ask.fbPassthrough, ask.metrics, ask.scene, ask.anchors);
    if (!headset->open())
        return nullptr;
    return headset;
}

} // namespace engine::rhi::sdlgpu
