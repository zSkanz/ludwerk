// A headset, as the RHI hands it to the renderer (ADR 0244 §7).
//
// **The RHI's interface was frozen (ADR 0037); this adds to it and changes
// nothing there.** A device may be made for a headset (`DeviceDesc::xr`), and
// then `IDevice::headset()` answers one: each frame it says whether to draw,
// for when, from where each eye looks, and which texture each eye draws into
// -- an image of the runtime's own swapchain, as a `TextureHandle` like any.
// The renderer records into them in the frame's command list, the frame is
// submitted, and `endFrame` hands the images back and shows them.
//
// No OpenXR type crosses this line (R17): a pose is a position and a
// quaternion, a field of view four tangents, an instant nanoseconds.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/types.h"
#include "engine/rhi/types.h"

namespace engine::rhi {

// **What asks for a headset when the device is made.** Nothing is asked when
// `enabled` is false, and a device made with it true where there is no
// headset is still a device -- `headset()` answers null and the game draws on
// the screen (ADR 0244 §6).
struct XrRequest
{
    bool enabled = false;
    // The application's name and version, as the runtime lists it.
    std::string application = {};
    u32 applicationVersion = 0;
    // `[xr] hands` (ADR 0245 §4): the hands themselves, joint by joint,
    // where the runtime tracks them.
    bool hands = true;
    // `[xr] depth` (ADR 0244 §5): each eye's depth handed to the runtime with
    // its colour, where the runtime takes it, for its reprojection.
    bool depth = true;
    // `[xr] refresh_rate`: the display's rate in hertz, the nearest the
    // runtime offers where it lets a game choose; nought leaves its own.
    f32 refreshRate = 0.0f;
};

// One eye's view this frame: where it is and how it is turned, in the play
// space's metres (y up, the runtime's floor at 0 where it knows one), and the
// edges of what it sees as tangents of their angles from straight ahead --
// left and down negative. An eye's frustum is not symmetric.
struct EyeView
{
    std::array<f32, 3> position{0.0f, 0.0f, 0.0f};
    // x, y, z, w.
    std::array<f32, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
    f32 tanLeft = -1.0f;
    f32 tanRight = 1.0f;
    f32 tanDown = -1.0f;
    f32 tanUp = 1.0f;
};

// Where the headset's session stands: `Running` is the only state frames are
// drawn in; `Ended` is for good, and the game goes on on the screen.
// **Which space the poses are in** (ADR 0245 §1): the floor where the player
// stood (`Standing`, OpenXR's stage) or the head where the session began
// (`Seated`, OpenXR's local). A runtime that knows no floor gives the
// head's start for both.
enum class HeadsetSpace : u8
{
    Standing,
    Seated,
};

enum class HeadsetState : u8
{
    Idle,
    Running,
    Paused,
    Ended,
};

// **One frame of the headset.** `draw` false is a frame the runtime does not
// want drawn -- the headset is off the head, the system menu is up -- and it
// is still ended, with nothing in it.
struct HeadsetFrame
{
    bool draw = false;
    // When the runtime will show it, in nanoseconds of the runtime's clock.
    i64 displayTimeNs = 0;
    std::array<EyeView, 2> eyes{};
    std::array<TextureHandle, 2> color{};
    u32 width = 0;
    u32 height = 0;
    TextureFormat format = TextureFormat::Undefined;
    // **Each eye's depth image**, the size of its colour, where the runtime
    // takes depth (ADR 0244 §5); none otherwise. A frame that has them has
    // each written before it ends, as `setDepthRange` says it is.
    std::array<TextureHandle, 2> depth{};
    TextureFormat depthFormat = TextureFormat::Undefined;
};

// **How the headset's frames have gone** since the last time it was asked
// (`IHeadset::takeTiming`): what decides whether the eyes are drawn fast
// enough. The runtime's own measurements are below zero where it gives none.
struct HeadsetTiming
{
    // The display's period, as the runtime predicts it.
    i64 displayPeriodNs = 0;
    // Frames begun; the time between them, over `intervals` of those
    // betweens -- a gap of more than a second is the session waiting (the
    // headset off the head, the link paused), not a frame, and is in neither;
    // frames the runtime said not to draw; and frames that came more than
    // half a period late.
    u32 frames = 0;
    i64 spanNs = 0;
    u32 intervals = 0;
    u32 notDrawn = 0;
    u32 late = 0;
    // The runtime's measurements of the game's frames (`XR_META_performance_
    // metrics`): its GPU and CPU time, in milliseconds, and the compositor's
    // dropped frames since the session began; below zero where it has none.
    f32 appGpuMs = -1.0f;
    f32 appCpuMs = -1.0f;
    i64 compositorDropped = -1;
};

// A place and a turn in the play space, and whether the runtime knows it.
struct HeadsetPose
{
    bool valid = false;
    std::array<f32, 3> position{0.0f, 0.0f, 0.0f};
    // x, y, z, w.
    std::array<f32, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

// **One flat part of the player's room** (ADR 0247 §2): a floor, a wall, a
// table's top. `pose` is its middle, the plane across its own x and y and
// facing along its +z, into the room; `width` along x and `height` along y.
// `kind` is `Enum.XRPlaneKind`'s value.
struct HeadsetPlane
{
    u8 kind = 7;
    HeadsetPose pose;
    f32 width = 0.0f;
    f32 height = 0.0f;
};

// **The room, as the headset knows it**: `known` false where it knows none or
// the player refused it.
struct HeadsetRoom
{
    bool known = false;
    std::vector<HeadsetPlane> planes;
};

// **What the headset answered about anchors** (ADR 0247 §3), each for the
// ask the call that made it returned: an anchor `Created`, or not; one
// `Saved`, or not; a `Found` for each anchor a load found, then its `Done`.
// `uuid` is the anchor's id, the same in every session, as 32 hex digits.
struct HeadsetAnchorEvent
{
    enum class Kind : u8
    {
        Created,
        Saved,
        Found,
        Done,
    };
    Kind kind = Kind::Done;
    u32 ask = 0;
    u64 anchor = 0;
    std::string uuid;
    bool ok = false;
};

// **The number of controller values a frame reads** (ADR 0245): one for each
// `Enum.KeyCode` whose name begins with `XR`, in the enum's order -- A, B, X,
// Y, the menu, the two triggers, the two grips, the four stick axes, the two
// sticks as one (unused here), the two sticks pressed in, the two pinches.
inline constexpr usize HeadsetKeyCount = 19;
// Where the pinches are among them.
inline constexpr usize HeadsetPinchLeft = 17;

// **A hand's joints** (ADR 0245 §4), in OpenXR's order: the palm, the wrist,
// then each finger from the thumb out, from its root to its tip.
inline constexpr usize HandJointCount = 26;
inline constexpr usize HandJointPalm = 0;
inline constexpr usize HandJointThumbTip = 5;
inline constexpr usize HandJointIndexTip = 10;

// **What the player is doing this frame**: the head, each hand where it holds
// its controller (`grip`) and where it points (`aim`), and the controllers'
// buttons, triggers and sticks. Left is 0 and right 1.
struct HeadsetInput
{
    HeadsetPose head;
    std::array<HeadsetPose, 2> grip{};
    std::array<HeadsetPose, 2> aim{};
    std::array<f32, HeadsetKeyCount> keys{};
    // Each hand's joints where it is tracked as a hand -- no controller in
    // it -- and every one not valid where it is not.
    std::array<std::array<HeadsetPose, HandJointCount>, 2> joints{};
};

class IHeadset
{
public:
    virtual ~IHeadset() = default;
    IHeadset() = default;
    IHeadset(const IHeadset&) = delete;
    IHeadset& operator=(const IHeadset&) = delete;

    // The runtime's name ("Oculus", "SteamVR/OpenXR", "simulated") and the
    // headset's, for the log and the profiler.
    [[nodiscard]] virtual std::string_view runtimeName() const noexcept = 0;
    [[nodiscard]] virtual std::string_view systemName() const noexcept = 0;
    // The size each eye is drawn at: as the runtime recommends it, times the
    // render scale asked (`setRenderScale`).
    [[nodiscard]] virtual u32 eyeWidth() const noexcept = 0;
    [[nodiscard]] virtual u32 eyeHeight() const noexcept = 0;
    // **The eyes' images made smaller** (`[xr] render_scale`, ADR 0250):
    // `scale` of the size the runtime recommends, from the next frame begun --
    // the images remade, and the runtime's compositor brings them up to the
    // display. True where this headset does; false, and the renderer draws
    // smaller and brings the picture up itself.
    [[nodiscard]] virtual bool setRenderScale(f32 scale)
    {
        (void)scale;
        return false;
    }

    // Reads what the runtime said since the last call -- a session ready to
    // begin, asked to stop, lost -- and acts on it. Once a frame, before
    // `beginFrame`.
    virtual void poll() = 0;
    [[nodiscard]] virtual HeadsetState state() const noexcept = 0;

    // **Waits for the runtime's moment to draw**, at the headset's rate, and
    // begins the frame: the eyes' views for when it will be shown, and the
    // eyes' images to draw into. False when no frame was begun (not running,
    // or the runtime refused); `endFrame` is then not called. After
    // `IDevice::beginFrame`, since the images are recorded into its list.
    [[nodiscard]] virtual bool beginFrame(HeadsetFrame& frame) = 0;
    // **Hands the images back and shows them**, after the frame's command
    // list was submitted. Every begun frame is ended, drawn or not.
    virtual void endFrame() = 0;

    // **The head, the hands and the controllers** (ADR 0245), for the frame
    // begun last -- read at the time it will be shown. Nothing known reads
    // as poses not valid and every key at rest.
    virtual void readInput(HeadsetInput& input) { input = HeadsetInput{}; }
    // **A controller's vibration** (ADR 0245 §3): `hand` 0 left, 1 right; an
    // amplitude 0 to 1 for `seconds`, replacing what it was doing.
    virtual void vibrate(u32 hand, f32 amplitude, f32 seconds)
    {
        (void)hand;
        (void)amplitude;
        (void)seconds;
    }
    // **The room** (ADR 0247 §2): asked for, and answered at a later frame --
    // the runtime finds it, and the player may be asked to let the game see
    // it. `takeRoom` is true once, the frame the answer comes.
    virtual void requestRoom() {}
    [[nodiscard]] virtual bool takeRoom(HeadsetRoom& out)
    {
        (void)out;
        return false;
    }
    // **Anchors** (ADR 0247 §3): places pinned in the room, each by the
    // runtime's handle, its pose in the space the headset gives. Each ask
    // returns its number -- 0 where the headset has no anchors -- and is
    // answered at a later frame through `takeAnchorEvents`.
    [[nodiscard]] virtual u32 createAnchor(const HeadsetPose& pose)
    {
        (void)pose;
        return 0;
    }
    [[nodiscard]] virtual u32 saveAnchor(u64 anchor)
    {
        (void)anchor;
        return 0;
    }
    [[nodiscard]] virtual u32 loadAnchors() { return 0; }
    virtual void destroyAnchor(u64 anchor) { (void)anchor; }
    [[nodiscard]] virtual HeadsetPose locateAnchor(u64 anchor)
    {
        (void)anchor;
        return {};
    }
    virtual void takeAnchorEvents(std::vector<HeadsetAnchorEvent>& out) { (void)out; }
    // **How the frames have gone since this was last called**, and starts
    // counting again.
    [[nodiscard]] virtual HeadsetTiming takeTiming() { return {}; }
    // **The room seen through** (ADR 0247 §1): whether the headset can show
    // the player's room behind the picture, and, from the next frame,
    // whether it does -- where a pixel's alpha is below one, the room shows.
    [[nodiscard]] virtual bool supportsPassthrough() const noexcept { return false; }
    virtual void setPassthrough(bool on) { (void)on; }
    // **The display's rate**, asked again mid-session (`XRService.RefreshRate`,
    // ADR 0250): the nearest the headset offers, nought its own. A headset
    // that lets a game choose none goes on at its own.
    virtual void setRefreshRate(f32 hertz) { (void)hertz; }
    // **What the depth images hold**: depth 0 at `nearZ` metres and 1 at
    // `farZ`, as the eyes were drawn. Before `endFrame`.
    virtual void setDepthRange(f32 nearZ, f32 farZ)
    {
        (void)nearZ;
        (void)farZ;
    }
    // **The space the poses and the eyes are in from the next frame**
    // (ADR 0245 §1). Standing until asked.
    virtual void setSpace(HeadsetSpace space) { (void)space; }
    // **The floor's play area**: the width and depth, in metres, of the
    // rectangle the player set up around the standing space's origin, or
    // nothing where the runtime knows none.
    [[nodiscard]] virtual std::optional<std::array<f32, 2>> playArea() const { return std::nullopt; }
};

} // namespace engine::rhi
