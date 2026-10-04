#include <SDL3/SDL_hints.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "engine/core/i18n.h"
#include "engine/core/text_key.h"
#include "engine/platform/clipboard.h"
#include "engine/platform/event.h"
#include "engine/platform/file.h"
#include "engine/platform/platform.h"
#include "engine/platform/sdl_interop.h"
#include "engine/platform/window.h"
#ifdef _WIN32
#include <windows.h>
#endif

using engine::core::engineCatalog;
using engine::core::EngineError;

namespace {

void seedRealCatalog()
{
    const auto result = engineCatalog().loadFromFile(ENG_TEST_CATALOG);
    REQUIRE_MESSAGE(result.ok, result.diagnostic);
}

// Every test here runs headless. A CI runner has no display, so asking for a
// real video driver would make these tests pass on the dev machine and fail on
// Linux -- which is the failure mode the offscreen driver exists to remove.
struct HeadlessPlatform
{
    HeadlessPlatform()
    {
        const auto error = engine::platform::init({.headless = true});
        // Parenthesised: `<<` binds tighter than `?:`, so an unwrapped ternary
        // here is a compile error rather than a message.
        REQUIRE_MESSAGE(!error.has_value(), (error ? error->detail : std::string{}));
    }

    ~HeadlessPlatform() { engine::platform::shutdown(); }

    HeadlessPlatform(const HeadlessPlatform&) = delete;
    HeadlessPlatform& operator=(const HeadlessPlatform&) = delete;
};

} // namespace

TEST_CASE("paths resolve without bringing up a video subsystem")
{
    // Deliberately no init(): a tool that only wants to find content should not
    // have to open a display connection to do it.
    const auto& paths = engine::platform::paths();

    CHECK(paths.executableDir.is_absolute());
    CHECK(paths.contentDir == paths.executableDir / "content");
}

TEST_CASE("the clock runs before init")
{
    // Regression guard: SDL's tick clock counts from SDL_Init and reads 0
    // before it, which made this test pass standalone and fail under CTest
    // depending on which test case happened to run first.
    REQUIRE_FALSE(engine::platform::isInitialized());

    const auto first = engine::platform::nowNs();
    const auto second = engine::platform::nowNs();

    CHECK(first > 0);
    CHECK(second >= first);
}

TEST_CASE("init is idempotent and shutdown is safe without it")
{
    CHECK_FALSE(engine::platform::isInitialized());

    // Safe before any init -- the host's error paths unwind through here.
    engine::platform::shutdown();

    {
        HeadlessPlatform platform;
        CHECK(engine::platform::isInitialized());

        const auto again = engine::platform::init({.headless = true});
        CHECK_FALSE(again.has_value());
        CHECK(engine::platform::isInitialized());
    }

    CHECK_FALSE(engine::platform::isInitialized());
}

TEST_CASE("a window cannot be created before init")
{
    seedRealCatalog();
    REQUIRE_FALSE(engine::platform::isInitialized());

    EngineError error;
    const auto window = engine::platform::createWindow({.titleKey = ENG_TR("platform.window.title")}, &error);

    CHECK(window == nullptr);
    CHECK_FALSE(error.message.empty());
}

TEST_CASE("a window reports an id and a drawable size")
{
    seedRealCatalog();
    HeadlessPlatform platform;

    EngineError error;
    const auto window = engine::platform::createWindow(
        {.titleKey = ENG_TR("platform.window.title"), .width = 640, .height = 360, .visible = false}, &error);

    REQUIRE_MESSAGE(window != nullptr, error.detail);
    CHECK(engine::platform::windowId(*window) != 0);

    const auto size = engine::platform::windowPixelSize(*window);
    CHECK(size.width == 640);
    CHECK(size.height == 360);
}

TEST_CASE("the pump translates what the engine models and drops the rest")
{
    using engine::platform::EventType;
    using engine::platform::Key;

    HeadlessPlatform platform;
    // Drain whatever bringing up a driver queued.
    static_cast<void>(engine::platform::pumpEvents());

    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    REQUIRE(SDL_PushEvent(&quit));

    SDL_Event overlayKey{};
    overlayKey.type = SDL_EVENT_KEY_DOWN;
    overlayKey.key.scancode = SDL_SCANCODE_F3;
    overlayKey.key.down = true;
    REQUIRE(SDL_PushEvent(&overlayKey));

    // A key the engine does not model. It must survive in the raw stream --
    // that is the contract sdl_interop.h promises the ImGui backend -- while
    // staying out of the translated one.
    //
    // It was SDL_SCANCODE_A until M6, and by then it had stopped being unmapped:
    // M5 grew `Key` to a whole keyboard, so this case had spent a milestone
    // pushing a key the engine DOES model and asserting nothing. Insert is
    // unmodelled on purpose -- nothing in the surface names it, and if something
    // ever does, this line is where the test says so.
    SDL_Event unmappedKey{};
    unmappedKey.type = SDL_EVENT_KEY_DOWN;
    unmappedKey.key.scancode = SDL_SCANCODE_INSERT;
    unmappedKey.key.down = true;
    REQUIRE(SDL_PushEvent(&unmappedKey));

    const auto events = engine::platform::pumpEvents();
    const auto raw = engine::platform::rawEvents();

    CHECK(std::ranges::any_of(events, [](const auto& e) { return e.type == EventType::Quit; }));
    CHECK(std::ranges::any_of(events, [](const auto& e) { return e.type == EventType::KeyDown && e.key == Key::F3; }));
    CHECK_FALSE(std::ranges::any_of(
        events, [](const auto& e) { return e.type == EventType::KeyDown && e.key == Key::Unknown; }));

    CHECK(std::ranges::any_of(
        raw, [](const SDL_Event& e) { return e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_INSERT; }));
}

TEST_CASE("a phone's back button is Escape, and is the game's to answer (ADR 0170)")
{
    // Back closed the app: the system took it, the activity finished, and a
    // game with a sheet open had no way to close the sheet with it.
    using engine::platform::EventType;
    using engine::platform::Key;

    HeadlessPlatform platform;
    static_cast<void>(engine::platform::pumpEvents());
    // Asked of the system once, when the platform starts: on Android the
    // button is delivered and not acted on; anywhere else there is none.
    const char* trapped = SDL_GetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON);
    REQUIRE(trapped != nullptr);
    CHECK(std::string_view(trapped) == "1");

    SDL_Event back{};
    back.type = SDL_EVENT_KEY_DOWN;
    back.key.scancode = SDL_SCANCODE_AC_BACK;
    back.key.down = true;
    REQUIRE(SDL_PushEvent(&back));
    SDL_Event released = back;
    released.type = SDL_EVENT_KEY_UP;
    released.key.down = false;
    REQUIRE(SDL_PushEvent(&released));

    const auto events = engine::platform::pumpEvents();
    CHECK(std::ranges::any_of(events,
                              [](const auto& e) { return e.type == EventType::KeyDown && e.key == Key::Escape; }));
    CHECK(
        std::ranges::any_of(events, [](const auto& e) { return e.type == EventType::KeyUp && e.key == Key::Escape; }));
    CHECK_FALSE(std::ranges::any_of(events, [](const auto& e) { return e.type == EventType::Quit; }));
}

// --- The device layer (M6) ---------------------------------------------------
//
// The Input Action System binds mouse buttons, wheel notches and gamepad
// buttons and axes as first-class inputs (ADR 0029), so the translation of each
// is a contract rather than a convenience. These cases push synthetic SDL
// events, which is the only way to test a device layer with no device.

TEST_CASE("the pump translates pointer, wheel and text events")
{
    using engine::platform::EventType;
    using engine::platform::MouseButton;

    HeadlessPlatform platform;
    static_cast<void>(engine::platform::pumpEvents());

    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = 120.0f;
    motion.motion.y = 48.0f;
    motion.motion.xrel = -3.0f;
    motion.motion.yrel = 7.0f;
    REQUIRE(SDL_PushEvent(&motion));

    SDL_Event press{};
    press.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    press.button.button = SDL_BUTTON_RIGHT;
    press.button.down = true;
    press.button.x = 120.0f;
    press.button.y = 48.0f;
    REQUIRE(SDL_PushEvent(&press));

    // FLIPPED is what a natural-scrolling trackpad reports, and SDL documents
    // the fix as multiplying by -1. The engine undoes it so that one convention
    // reaches every consumer -- otherwise a binding saved on one machine scrolls
    // the wrong way on another.
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.x = 0.0f;
    wheel.wheel.y = 2.0f;
    wheel.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    REQUIRE(SDL_PushEvent(&wheel));

    const auto events = engine::platform::pumpEvents();

    const auto moved = std::ranges::find_if(events, [](const auto& e) { return e.type == EventType::MouseMoved; });
    REQUIRE(moved != events.end());
    CHECK(moved->pointerX == doctest::Approx(120.0));
    CHECK(moved->pointerY == doctest::Approx(48.0));
    CHECK(moved->pointerDeltaX == doctest::Approx(-3.0));
    CHECK(moved->pointerDeltaY == doctest::Approx(7.0));

    const auto down = std::ranges::find_if(events, [](const auto& e) { return e.type == EventType::MouseButtonDown; });
    REQUIRE(down != events.end());
    CHECK(down->button == MouseButton::Right);

    const auto scrolled = std::ranges::find_if(events, [](const auto& e) { return e.type == EventType::MouseWheel; });
    REQUIRE(scrolled != events.end());
    CHECK(scrolled->wheelY == doctest::Approx(-2.0));
}

TEST_CASE("a text field's events: the modifiers, the press count, composition and long text (ADR 0139)")
{
    using engine::platform::EventType;
    using engine::platform::KeyModifier;

    HeadlessPlatform platform;
    static_cast<void>(engine::platform::pumpEvents());

    // Ctrl+Shift+Left: what selects a word to the left.
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.scancode = SDL_SCANCODE_LEFT;
    key.key.mod = SDL_KMOD_LCTRL | SDL_KMOD_RSHIFT;
    key.key.down = true;
    REQUIRE(SDL_PushEvent(&key));

    // A double press.
    SDL_Event press{};
    press.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    press.button.button = SDL_BUTTON_LEFT;
    press.button.down = true;
    press.button.clicks = 2;
    REQUIRE(SDL_PushEvent(&press));

    // An input method's composition, with its cursor.
    SDL_Event editing{};
    editing.type = SDL_EVENT_TEXT_EDITING;
    editing.edit.text = SDL_strdup("\xE3\x81\x8B\xE3\x81\xAA"); // two kana
    editing.edit.start = 1;
    editing.edit.length = 1;
    REQUIRE(SDL_PushEvent(&editing));

    // A whole phrase committed at once -- longer than a keystroke's text.
    const std::string phrase(120, 'x');
    SDL_Event committed{};
    committed.type = SDL_EVENT_TEXT_INPUT;
    committed.text.text = SDL_strdup(phrase.c_str());
    REQUIRE(SDL_PushEvent(&committed));

    const auto events = engine::platform::pumpEvents();

    const auto down = std::ranges::find_if(events, [](const auto& e) { return e.type == EventType::KeyDown; });
    REQUIRE(down != events.end());
    CHECK((down->modifiers & KeyModifier::Ctrl) != 0);
    CHECK((down->modifiers & KeyModifier::Shift) != 0);
    CHECK((down->modifiers & KeyModifier::Alt) == 0);

    const auto clicked =
        std::ranges::find_if(events, [](const auto& e) { return e.type == EventType::MouseButtonDown; });
    REQUIRE(clicked != events.end());
    CHECK(clicked->clicks == 2);

    const auto composing = std::ranges::find_if(events, [](const auto& e) { return e.type == EventType::TextEditing; });
    REQUIRE(composing != events.end());
    CHECK(std::string_view(composing->text) == "\xE3\x81\x8B\xE3\x81\xAA");
    CHECK(composing->editStart == 1);
    CHECK(composing->editLength == 1);

    const auto typed = std::ranges::find_if(events, [](const auto& e) { return e.type == EventType::TextInput; });
    REQUIRE(typed != events.end());
    CHECK(std::string_view(typed->text) == phrase);
}

TEST_CASE("the clipboard carries text in and out (ADR 0139)")
{
    HeadlessPlatform platform;
    REQUIRE(engine::platform::setClipboardText("copied \xC3\xA9"));
    const std::optional<std::string> read = engine::platform::clipboardText();
    REQUIRE(read.has_value());
    CHECK(*read == "copied \xC3\xA9");
}

TEST_CASE("text on the clipboard that another program holds open for a moment is still read (D361)")
{
    // A clipboard history opens the clipboard about 200 ms after every change,
    // and a read in that moment answered "" -- a paste right after a copy
    // pasted nothing. Two reads come back empty while text is there; the third
    // gets it.
    int reads = 0;
    int waits = 0;
    engine::platform::ClipboardSource source;
    source.hasText = [] { return true; };
    source.read = [&reads]() -> std::optional<std::string> {
        ++reads;
        return reads < 3 ? std::string() : std::string("pasted");
    };
    source.wait = [&waits] { ++waits; };
    const std::optional<std::string> read = engine::platform::clipboardTextFrom(source);
    REQUIRE(read.has_value());
    CHECK(*read == "pasted");
    CHECK(reads == 3);
    CHECK(waits == 2);

    // Nothing on it is nothing, asked once.
    reads = 0;
    source.hasText = [] { return false; };
    CHECK_FALSE(engine::platform::clipboardTextFrom(source).has_value());
    CHECK(reads == 0);

    // And text that really is empty is given up on, not waited for for ever.
    source.hasText = [] { return true; };
    source.read = [&reads]() -> std::optional<std::string> {
        ++reads;
        return std::string();
    };
    const std::optional<std::string> empty = engine::platform::clipboardTextFrom(source);
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
    CHECK(reads == engine::platform::ClipboardAttempts);
}

TEST_CASE("a gamepad axis is normalized by what the axis IS")
{
    using engine::platform::EventType;
    using engine::platform::GamepadAxis;
    using engine::platform::GamepadButton;

    HeadlessPlatform platform;
    static_cast<void>(engine::platform::pumpEvents());

    // A stick at its most negative. SDL's range is -32768..32767, so dividing
    // by the positive half overshoots -1 -- which is why the translation
    // clamps, and why this case pushes the extreme rather than a round number.
    SDL_Event stick{};
    stick.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    stick.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
    stick.gaxis.value = -32768;
    REQUIRE(SDL_PushEvent(&stick));

    // A trigger at rest. Over the same divisor a stick uses this would read
    // -1: a resting input indistinguishable from a fully held one.
    SDL_Event trigger{};
    trigger.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    trigger.gaxis.axis = SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    trigger.gaxis.value = 0;
    REQUIRE(SDL_PushEvent(&trigger));

    SDL_Event button{};
    button.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    button.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
    button.gbutton.down = true;
    REQUIRE(SDL_PushEvent(&button));

    // A paddle: a real button on a minority of pads, and deliberately outside
    // the engine's enum. It must be dropped rather than translated to Unknown.
    SDL_Event paddle{};
    paddle.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    paddle.gbutton.button = SDL_GAMEPAD_BUTTON_LEFT_PADDLE1;
    paddle.gbutton.down = true;
    REQUIRE(SDL_PushEvent(&paddle));

    const auto events = engine::platform::pumpEvents();

    const auto axes =
        std::ranges::count_if(events, [](const auto& e) { return e.type == EventType::GamepadAxisMoved; });
    REQUIRE(axes == 2);

    for (const auto& event : events) {
        if (event.type != EventType::GamepadAxisMoved)
            continue;
        if (event.gamepadAxis == GamepadAxis::LeftX)
            CHECK(event.axisValue == doctest::Approx(-1.0));
        if (event.gamepadAxis == GamepadAxis::LeftTrigger)
            CHECK(event.axisValue == doctest::Approx(0.0));
    }

    CHECK(std::ranges::count_if(events, [](const auto& e) { return e.type == EventType::GamepadButtonDown; }) == 1);
    CHECK(std::ranges::any_of(events, [](const auto& e) {
        return e.type == EventType::GamepadButtonDown && e.gamepadButton == GamepadButton::South;
    }));
}

TEST_CASE("every device enumerator round-trips through its name")
{
    using engine::platform::GamepadAxis;
    using engine::platform::GamepadButton;
    using engine::platform::Key;
    using engine::platform::MouseButton;

    // The recorded input stream the determinism gate replays is written in
    // these names. A name table that fell behind its enum would produce a trace
    // that reads fine and replays a different key -- so the tables are walked
    // rather than spot-checked, and `Unknown` is required to name nothing.
    for (int raw = 1; raw < static_cast<int>(Key::Count); ++raw) {
        const auto key = static_cast<Key>(raw);
        CAPTURE(raw);
        const std::string_view name = engine::platform::keyName(key);
        CHECK_FALSE(name.empty());
        CHECK(engine::platform::keyFromName(name) == key);
    }

    for (int raw = 1; raw < static_cast<int>(MouseButton::Count); ++raw) {
        const auto button = static_cast<MouseButton>(raw);
        CAPTURE(raw);
        const std::string_view name = engine::platform::mouseButtonName(button);
        CHECK_FALSE(name.empty());
        CHECK(engine::platform::mouseButtonFromName(name) == button);
    }

    for (int raw = 1; raw < static_cast<int>(GamepadButton::Count); ++raw) {
        const auto button = static_cast<GamepadButton>(raw);
        CAPTURE(raw);
        const std::string_view name = engine::platform::gamepadButtonName(button);
        CHECK_FALSE(name.empty());
        CHECK(engine::platform::gamepadButtonFromName(name) == button);
    }

    for (int raw = 1; raw < static_cast<int>(GamepadAxis::Count); ++raw) {
        const auto axis = static_cast<GamepadAxis>(raw);
        CAPTURE(raw);
        const std::string_view name = engine::platform::gamepadAxisName(axis);
        CHECK_FALSE(name.empty());
        CHECK(engine::platform::gamepadAxisFromName(name) == axis);
    }

    CHECK(engine::platform::keyName(Key::Unknown).empty());
    CHECK(engine::platform::mouseButtonName(MouseButton::Unknown).empty());
    CHECK(engine::platform::gamepadButtonName(GamepadButton::Unknown).empty());
    CHECK(engine::platform::gamepadAxisName(GamepadAxis::Unknown).empty());

    // The four namespaces share one spelling space in a recorded trace and in
    // `Enum.KeyCode`, so a collision between them would make a name ambiguous.
    std::vector<std::string_view> names;
    for (int raw = 1; raw < static_cast<int>(Key::Count); ++raw)
        names.push_back(engine::platform::keyName(static_cast<Key>(raw)));
    for (int raw = 1; raw < static_cast<int>(MouseButton::Count); ++raw)
        names.push_back(engine::platform::mouseButtonName(static_cast<MouseButton>(raw)));
    for (int raw = 1; raw < static_cast<int>(GamepadButton::Count); ++raw)
        names.push_back(engine::platform::gamepadButtonName(static_cast<GamepadButton>(raw)));
    for (int raw = 1; raw < static_cast<int>(GamepadAxis::Count); ++raw)
        names.push_back(engine::platform::gamepadAxisName(static_cast<GamepadAxis>(raw)));

    std::ranges::sort(names);
    CHECK(std::ranges::adjacent_find(names) == names.end());
}

// --- platform::readFile ------------------------------------------------------
//
// The seam ShaderLibrary reads content through. On this tier it is an ordinary
// file read; the reason it exists at all is Android, where the same call lands
// on AAssetManager instead -- and that half cannot be tested without a device.
// What IS testable here is the contract every caller relies on: exact bytes,
// including embedded NULs and CR, and a clean false for anything unreadable.

TEST_CASE("readFile returns the file's bytes exactly")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "engine-platform-readfile.bin";

    // A NUL in the middle and a bare CR: a reader that went through a text-mode
    // FILE* or treated the buffer as a C string would lose one or the other.
    const std::string payload("ab\0cd\r\n", 7);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    std::vector<std::byte> bytes;
    REQUIRE(engine::platform::readFile(path, bytes));
    REQUIRE(bytes.size() == payload.size());
    CHECK(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);

    std::string text;
    REQUIRE(engine::platform::readTextFile(path, text));
    CHECK(text == payload);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("readFile fails without touching the caller's buffer")
{
    const std::filesystem::path missing = std::filesystem::temp_directory_path() / "engine-platform-does-not-exist.bin";

    std::vector<std::byte> bytes{std::byte{0x7f}};
    CHECK_FALSE(engine::platform::readFile(missing, bytes));
    CHECK(bytes.size() == 1);

    // A directory is not a file. It is worth pinning because the platform
    // layers disagree: opening one succeeds on POSIX and fails on Windows, and
    // a caller that got a zero-byte "success" would report a corrupt shader
    // instead of a missing one.
    std::string text{"kept"};
    CHECK_FALSE(engine::platform::readTextFile(std::filesystem::temp_directory_path(), text));
    CHECK(text == "kept");
}

// --- platform::writeFile -----------------------------------------------------
//
// The write seam the host saves through. It is deliberately unreachable from
// the game VM (see file.h), so the only caller these cases stand in for is a
// tool. What they pin is the contract a save depends on: exact bytes back,
// an overwrite that replaces rather than appends, and a clean false -- no
// crash, no truncated target, no litter -- for every path that cannot take a
// write.

namespace {

// Every write test works inside its own directory under the system temporary
// one, so a failure leaves evidence in a named place and two cases can never
// collide over a filename.
struct ScratchDir
{
    explicit ScratchDir(std::string_view name)
        : path(std::filesystem::temp_directory_path() /
               std::filesystem::path(std::string("engine-write-").append(name)))
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }

    ~ScratchDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    std::filesystem::path path;
};

[[nodiscard]] std::span<const std::byte> asBytes(std::string_view text)
{
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// How many entries the directory holds. The atomic write leaves a temporary
// beside its target while it runs, and a caller must never find one afterwards
// -- neither on the path that succeeded nor on the one that failed.
[[nodiscard]] std::size_t entryCount(const std::filesystem::path& directory)
{
    std::error_code ec;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        (void)entry;
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE("writeFile round-trips its bytes through readFile")
{
    const ScratchDir scratch("roundtrip");
    const std::filesystem::path path = scratch.path / "scene.bin";

    // The same NUL-and-bare-CR payload the read cases use: a writer that went
    // through a text-mode FILE* or a C string would lose one or the other.
    const std::string payload("ab\0cd\r\n", 7);
    REQUIRE(engine::platform::writeFile(path, asBytes(payload)));

    std::vector<std::byte> bytes;
    REQUIRE(engine::platform::readFile(path, bytes));
    REQUIRE(bytes.size() == payload.size());
    CHECK(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);

    // And the text spelling of the same call, read back the same way.
    const std::filesystem::path textPath = scratch.path / "scene.json";
    REQUIRE(engine::platform::writeTextFile(textPath, payload));

    std::string text;
    REQUIRE(engine::platform::readTextFile(textPath, text));
    CHECK(text == payload);

    // Nothing but the two files: the temporaries the atomic write used are gone.
    CHECK(entryCount(scratch.path) == 2);
}

TEST_CASE("writeFile replaces an existing file rather than appending to it")
{
    const ScratchDir scratch("overwrite");
    const std::filesystem::path path = scratch.path / "scene.json";

    REQUIRE(engine::platform::writeTextFile(path, "a longer first version"));

    // Shorter on purpose. A rename replaces the whole file; a write that seeked
    // to zero without truncating would leave the tail of the first version
    // behind, and the length is what catches that.
    REQUIRE(engine::platform::writeTextFile(path, "second"));

    std::string text;
    REQUIRE(engine::platform::readTextFile(path, text));
    CHECK(text == "second");
    CHECK(entryCount(scratch.path) == 1);
}

TEST_CASE("an empty payload writes an empty file")
{
    const ScratchDir scratch("empty");
    const std::filesystem::path path = scratch.path / "empty.bin";

    const std::span<const std::byte> nothing;
    REQUIRE(engine::platform::writeFile(path, nothing));
    CHECK(engine::platform::fileExists(path));

    std::vector<std::byte> bytes{std::byte{0x7f}};
    REQUIRE(engine::platform::readFile(path, bytes));
    CHECK(bytes.empty());
}

TEST_CASE("a write into a directory that does not exist fails until it is made")
{
    const ScratchDir scratch("mkdir");
    const std::filesystem::path nested = scratch.path / "saves" / "level-01";
    const std::filesystem::path path = nested / "scene.json";

    // `writeFile` does not create parents on its own -- a typo'd path has to
    // fail rather than scatter directories.
    CHECK_FALSE(engine::platform::writeTextFile(path, "scene"));
    CHECK_FALSE(engine::platform::fileExists(path));
    // And it left no temporary behind in the directory that DOES exist.
    CHECK(entryCount(scratch.path) == 0);

    // Which is what `createDirectories` is for, missing parents included.
    REQUIRE(engine::platform::createDirectories(nested));
    REQUIRE(engine::platform::writeTextFile(path, "scene"));

    std::string text;
    REQUIRE(engine::platform::readTextFile(path, text));
    CHECK(text == "scene");

    // Already there is success, not a failure: a save over an existing project
    // hits this every time.
    CHECK(engine::platform::createDirectories(nested));
}

TEST_CASE("a path that cannot be written fails instead of crashing")
{
    const ScratchDir scratch("unwritable");

    // A directory is not a file. The temporary write may well succeed -- it is
    // a sibling name -- and the rename onto a directory is what must not.
    CHECK_FALSE(engine::platform::writeTextFile(scratch.path, "nope"));
    CHECK(entryCount(scratch.path) == 0);

    // A regular file used as a directory. Portable on purpose: an illegal
    // character is a Windows notion and `*` is a perfectly good POSIX
    // filename, but no platform lets a path descend through a file.
    const std::filesystem::path file = scratch.path / "not-a-directory";
    REQUIRE(engine::platform::writeTextFile(file, "leaf"));

    CHECK_FALSE(engine::platform::writeTextFile(file / "child.json", "nope"));
    CHECK_FALSE(engine::platform::createDirectories(file));

    // The file it tried to descend through is untouched.
    std::string text;
    REQUIRE(engine::platform::readTextFile(file, text));
    CHECK(text == "leaf");
    CHECK(entryCount(scratch.path) == 1);

    // An empty path is a caller's bug, and must not become a file named after
    // the temporary suffix in the working directory.
    const std::filesystem::path nowhere;
    CHECK_FALSE(engine::platform::writeFile(nowhere, asBytes("nope")));
    CHECK_FALSE(engine::platform::createDirectories(nowhere));
}

// --- Application identity (roadmap M8) ---------------------------------------
//
// This test binary links `branding/app.rc`, the same resource `engine-host`
// carries, precisely so this claim can be made at all: an icon is a property of
// an EXECUTABLE, and no library can be asked whether it has one.

TEST_CASE("an executable can read its own icon back out of itself")
{
    const std::vector<std::byte> icon = engine::platform::applicationIconBytes();

#if defined(_WIN32)
    // The whole failure this closes is an icon that is wrong and breaks
    // nothing: the program runs, the window opens, and it wears the wrong face.
    // Nothing fails, so nothing reports it -- unless something reads it back.
    REQUIRE_FALSE(icon.empty());

    // Every entry of `branding/icon/icon.ico` is PNG-compressed, which is not
    // decoration: the window icon is decoded by stb_image, and a BMP-encoded
    // icon entry would need a DIB reader nothing in this engine has. If the
    // artwork is ever regenerated with BMP entries, this is where it says so
    // rather than in a window that quietly has no icon.
    REQUIRE(icon.size() > 8);
    CHECK(static_cast<unsigned char>(icon[0]) == 0x89);
    CHECK(static_cast<char>(icon[1]) == 'P');
    CHECK(static_cast<char>(icon[2]) == 'N');
    CHECK(static_cast<char>(icon[3]) == 'G');
#else
    // Empty rather than an error: a Linux window's icon comes from a `.desktop`
    // entry and a macOS one from the bundle, so there is nothing in the
    // executable to find and that is not a failure.
    CHECK(icon.empty());
#endif
}

namespace {

// One entry of an `.ico` directory, read from the file's own layout: 16 bytes,
// little-endian, and a zero in the width or height byte means 256.
struct IconEntry
{
    engine::core::u32 width = 0;
    engine::core::u32 height = 0;
    engine::core::u32 offset = 0;
    engine::core::u32 size = 0;
};

[[nodiscard]] engine::core::u32 readU16(const std::vector<std::byte>& bytes, std::size_t at)
{
    return static_cast<engine::core::u32>(static_cast<unsigned char>(bytes[at])) |
           (static_cast<engine::core::u32>(static_cast<unsigned char>(bytes[at + 1])) << 8);
}

[[nodiscard]] engine::core::u32 readU32(const std::vector<std::byte>& bytes, std::size_t at)
{
    return readU16(bytes, at) | (readU16(bytes, at + 2) << 16);
}

[[nodiscard]] bool isPng(const std::vector<std::byte>& bytes, std::size_t at, std::size_t size)
{
    static constexpr unsigned char kSignature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < sizeof(kSignature) || at + sizeof(kSignature) > bytes.size())
        return false;
    for (std::size_t index = 0; index < sizeof(kSignature); ++index) {
        if (static_cast<unsigned char>(bytes[at + index]) != kSignature[index])
            return false;
    }
    return true;
}

} // namespace

// --- The icon FILE, not the icon the executable happens to carry (S7.14) -----
//
// The case above reads an icon back out of this binary, which proves an icon is
// there and that the largest entry decodes. It cannot say the icon is the RIGHT
// one, it cannot see the six smaller sizes at all -- `applicationIconBytes`
// returns only the largest, because that is what a window wants -- and on
// anything but Windows it asserts emptiness and checks nothing.
//
// Those smaller sizes are the ones a person actually sees: 16 in the title bar,
// 32 in the taskbar, 48 and 256 in Explorer. An icon rebuilt with a size missing
// or a BMP entry looks perfect in the one place this suite was looking.

TEST_CASE("every size in the icon file is present and PNG-compressed")
{
    std::vector<std::byte> bytes;
    REQUIRE(engine::platform::readFile(ENG_TEST_ICON, bytes));
    REQUIRE(bytes.size() > 6);

    // `ICONDIR`: a reserved zero, a type of 1 for an icon, then the count.
    CHECK(readU16(bytes, 0) == 0);
    CHECK(readU16(bytes, 2) == 1);
    const engine::core::u32 count = readU16(bytes, 4);
    REQUIRE(count > 0);
    REQUIRE(bytes.size() >= 6 + static_cast<std::size_t>(count) * 16);

    std::vector<IconEntry> entries;
    for (engine::core::u32 index = 0; index < count; ++index) {
        const std::size_t at = 6 + static_cast<std::size_t>(index) * 16;
        IconEntry entry;
        // Zero means 256 here, which is the size that matters most and the one
        // a naive read discards -- the same trap `applicationIconBytes` names.
        entry.width = static_cast<unsigned char>(bytes[at]) == 0 ? 256u : static_cast<unsigned char>(bytes[at]);
        entry.height =
            static_cast<unsigned char>(bytes[at + 1]) == 0 ? 256u : static_cast<unsigned char>(bytes[at + 1]);
        entry.size = readU32(bytes, at + 8);
        entry.offset = readU32(bytes, at + 12);
        entries.push_back(entry);
    }

    for (const IconEntry& entry : entries) {
        INFO("entry ", entry.width, "x", entry.height);
        CHECK(entry.width == entry.height);
        REQUIRE(entry.size > 0);
        REQUIRE(entry.offset + entry.size <= bytes.size());
        // **Every** entry, not just the largest. A BMP-encoded entry needs a DIB
        // reader nothing in this engine has, and it is the small sizes an icon
        // tool is most likely to emit that way.
        CHECK(isPng(bytes, entry.offset, entry.size));
    }

    // One entry per `engine-<size>.png` beside it, which is where they came from.
    // A size added to the folder and forgotten in the `.ico` is the ordinary way
    // this decays, and it decays silently.
    for (const std::filesystem::directory_entry& png : std::filesystem::directory_iterator{ENG_TEST_ICON_DIR}) {
        const std::string name = png.path().filename().string();
        if (png.path().extension() != ".png" || name.rfind("engine-", 0) != 0)
            continue;
        const std::string digits = name.substr(6, name.size() - 6 - 4);
        const auto wanted = static_cast<engine::core::u32>(std::stoul(digits));
        INFO("no ", wanted, "x", wanted, " entry for ", name);
        CHECK(std::any_of(entries.begin(), entries.end(),
                          [wanted](const IconEntry& entry) { return entry.width == wanted; }));
    }
}

TEST_CASE("the icon in the executable is the icon in the tree")
{
    const std::vector<std::byte> embedded = engine::platform::applicationIconBytes();

#if defined(_WIN32)
    std::vector<std::byte> bytes;
    REQUIRE(engine::platform::readFile(ENG_TEST_ICON, bytes));

    // The largest entry, chosen the same way `applicationIconBytes` chooses it.
    const engine::core::u32 count = readU16(bytes, 4);
    REQUIRE(count > 0);
    engine::core::u32 bestPixels = 0;
    std::size_t bestOffset = 0;
    std::size_t bestSize = 0;
    for (engine::core::u32 index = 0; index < count; ++index) {
        const std::size_t at = 6 + static_cast<std::size_t>(index) * 16;
        const engine::core::u32 width =
            static_cast<unsigned char>(bytes[at]) == 0 ? 256u : static_cast<unsigned char>(bytes[at]);
        if (width * width > bestPixels) {
            bestPixels = width * width;
            bestSize = readU32(bytes, at + 8);
            bestOffset = readU32(bytes, at + 12);
        }
    }
    REQUIRE(bestSize > 0);

    // **Byte for byte.** This is the assertion that ties the two halves
    // together: the case above checks a FILE and the case before it checks a
    // BINARY, and without this nothing says they are the same artwork. A
    // rebuilt `.ico` that never reached the resource compiler passes both of
    // them on its own.
    REQUIRE(embedded.size() == bestSize);
    CHECK(std::memcmp(embedded.data(), bytes.data() + bestOffset, bestSize) == 0);
#else
    // Nothing to compare against: a Linux window's icon comes from a `.desktop`
    // entry and a macOS one from the bundle. The file itself is still checked by
    // the case above, on every platform.
    CHECK(embedded.empty());
#endif
}

TEST_CASE("setting the application id is safe to call with nothing to set")
{
    // Idempotent and total, because it runs before anything else on a path where
    // a project may not name itself. The assertion is that none of these faults.
    engine::platform::setApplicationId("");
    engine::platform::setApplicationId("dev.local.engine-test");
    engine::platform::setApplicationId("dev.local.engine-test");
}

TEST_CASE("raising the process priority is honest about whether it happened")
{
    // D193: Windows is the one platform with a foreground boost for this to
    // stand in for, and the only one that answers yes. The priority is put back
    // afterwards so the rest of this run is not measured under it.
    const bool raised = engine::platform::raiseProcessPriority();
#if defined(_WIN32)
    CHECK(raised);
    CHECK(::GetPriorityClass(::GetCurrentProcess()) == ABOVE_NORMAL_PRIORITY_CLASS);
    ::SetPriorityClass(::GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
#else
    CHECK_FALSE(raised);
#endif
}

TEST_CASE("a thread's CPU clock runs while it works and stops while it waits")
{
    // The first call starts the Windows calibration; give it its millisecond.
    (void)engine::platform::threadCpuNs();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (engine::platform::threadCpuNs() < 0) {
        MESSAGE("no per-thread CPU clock on this platform");
        return;
    }

    // Working: most of the wall time is CPU time. **Up to three tries** (D522):
    // on a shared runner the thread can be taken off its core for most of a
    // 40 ms burst, and one try failed a build that changed nothing here. A
    // clock that does not run fails all three.
    bool ran = false;
    for (int attempt = 0; attempt < 3 && !ran; ++attempt) {
        const engine::core::u64 wallStart = engine::platform::nowNs();
        const engine::core::i64 cpuStart = engine::platform::threadCpuNs();
        volatile engine::core::u64 sink = 0;
        while (engine::platform::nowNs() - wallStart < 40'000'000)
            sink = sink + 1;
        const double wall = static_cast<double>(engine::platform::nowNs() - wallStart);
        const double cpu = static_cast<double>(engine::platform::threadCpuNs() - cpuStart);
        // Never more than the wall clock allows, on any try.
        CHECK(cpu < 1.25 * wall + 2.0e6);
        ran = cpu > 0.25 * wall;
    }
    CHECK(ran);

    // Waiting: next to none.
    const engine::core::i64 idleStart = engine::platform::threadCpuNs();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(static_cast<double>(engine::platform::threadCpuNs() - idleStart) < 10.0e6);
}

TEST_CASE("text input switches on and off for a window, which is what raises a phone's keyboard")
{
    // D204: the switch existed and nothing called it.
    seedRealCatalog();
    HeadlessPlatform platform;
    EngineError error;
    const auto window =
        engine::platform::createWindow({.titleKey = ENG_TR("platform.window.title"), .visible = false}, &error);
    REQUIRE_MESSAGE(window != nullptr, error.detail);
    const engine::core::u32 id = engine::platform::windowId(*window);
    engine::platform::setTextInputEnabled(id, true);
    CHECK(engine::platform::textInputEnabled(id));
    engine::platform::setTextInputEnabled(id, false);
    CHECK_FALSE(engine::platform::textInputEnabled(id));
}

// --- Vibration (ADR 0131 section 2) ---------------------------------------------
//
// What is asked of a motor reaches the device, and stops reaching it when the
// window is not in front. The device here is a fake standing where SDL does:
// it records what arrived.

namespace {

struct FakeVibration final : engine::platform::VibrationSink
{
    bool hasGamepad = true;
    bool hasTriggers = false;
    bool hasDevice = false;
    int rumbles = 0;
    int triggerRumbles = 0;
    int stops = 0;
    float heavy = 0.0f;
    float light = 0.0f;
    float left = 0.0f;
    float right = 0.0f;
    float strength = 0.0f;
    float seconds = 0.0f;

    [[nodiscard]] bool gamepad() const override { return hasGamepad; }
    [[nodiscard]] bool triggers() const override { return hasTriggers; }
    [[nodiscard]] bool device() const override { return hasDevice; }
    void rumble(float l, float s) override
    {
        ++rumbles;
        heavy = l;
        light = s;
    }
    void rumbleTriggers(float l, float r) override
    {
        ++triggerRumbles;
        left = l;
        right = r;
    }
    void vibrate(float s, float t) override
    {
        strength = s;
        seconds = t;
    }
    void stop() override { ++stops; }
};

// Puts the fake in for a case and the hardware back after it, with every
// level at zero and the window in front -- whatever the case before left.
struct VibrationRig
{
    FakeVibration fake;

    VibrationRig()
    {
        engine::platform::setVibrationSink(&fake);
        engine::platform::setVibrationFocus(true);
        engine::platform::stopVibration();
        fake.stops = 0;
    }

    ~VibrationRig()
    {
        engine::platform::stopVibration();
        engine::platform::setVibrationFocus(true);
        engine::platform::setVibrationSink(nullptr);
    }
};

} // namespace

TEST_CASE("a motor's level reaches the device every frame until it is set to nothing")
{
    using namespace engine::platform;
    VibrationRig rig;

    // Nothing asked: nothing sent.
    pumpVibration();
    CHECK(rig.fake.rumbles == 0);
    CHECK(rig.fake.stops == 0);

    setVibrationMotor(VibrationMotor::Large, 0.75f);
    setVibrationMotor(VibrationMotor::Small, 0.25f);
    pumpVibration();
    pumpVibration();
    CHECK(rig.fake.rumbles == 2);
    CHECK(static_cast<double>(rig.fake.heavy) == doctest::Approx(0.75));
    CHECK(static_cast<double>(rig.fake.light) == doctest::Approx(0.25));
    // No trigger was asked for, so none was driven.
    CHECK(rig.fake.triggerRumbles == 0);

    setVibrationMotor(VibrationMotor::RightTrigger, 1.0f);
    pumpVibration();
    CHECK(rig.fake.triggerRumbles == 1);
    CHECK(static_cast<double>(rig.fake.right) == doctest::Approx(1.0));

    // Back to nothing: told to stop once, not once a frame.
    setVibrationMotor(VibrationMotor::Large, 0.0f);
    setVibrationMotor(VibrationMotor::Small, 0.0f);
    setVibrationMotor(VibrationMotor::RightTrigger, 0.0f);
    pumpVibration();
    pumpVibration();
    CHECK(rig.fake.rumbles == 3);
    CHECK(rig.fake.stops == 1);
}

TEST_CASE("a level outside nought to one is held inside it, and a hand motor is nobody's")
{
    using namespace engine::platform;
    VibrationRig rig;
    setVibrationMotor(VibrationMotor::Large, 7.0f);
    setVibrationMotor(VibrationMotor::Small, -3.0f);
    setVibrationMotor(VibrationMotor::LeftHand, 1.0f);
    pumpVibration();
    CHECK(static_cast<double>(rig.fake.heavy) == doctest::Approx(1.0));
    CHECK(static_cast<double>(rig.fake.light) == doctest::Approx(0.0));

    CHECK(vibrationMotorSupported(true, VibrationMotor::Large));
    CHECK_FALSE(vibrationMotorSupported(true, VibrationMotor::LeftTrigger));
    CHECK_FALSE(vibrationMotorSupported(true, VibrationMotor::LeftHand));
    rig.fake.hasTriggers = true;
    CHECK(vibrationMotorSupported(true, VibrationMotor::LeftTrigger));
    // A phone has no motors by name.
    CHECK_FALSE(vibrationMotorSupported(false, VibrationMotor::Large));
    CHECK(vibrationSupported(true));
    CHECK_FALSE(vibrationSupported(false));
    rig.fake.hasDevice = true;
    CHECK(vibrationSupported(false));
}

TEST_CASE("everything is still while the window is not in front, and returns with it")
{
    using namespace engine::platform;
    VibrationRig rig;
    setVibrationMotor(VibrationMotor::Large, 0.5f);
    pumpVibration();
    REQUIRE(rig.fake.rumbles == 1);

    // Focus lost: stopped at once, and nothing sent while it is away.
    setVibrationFocus(false);
    CHECK(rig.fake.stops == 1);
    pumpVibration();
    pumpVibration();
    CHECK(rig.fake.rumbles == 1);
    CHECK(rig.fake.stops == 1);
    // Nor does the device shake for a game nobody is looking at.
    vibrateDevice(1.0f, 1.0f);
    CHECK(rig.fake.seconds == 0.0f);

    // And back: the level was kept.
    setVibrationFocus(true);
    pumpVibration();
    CHECK(rig.fake.rumbles == 2);
    CHECK(static_cast<double>(rig.fake.heavy) == doctest::Approx(0.5));
}

TEST_CASE("the device itself shakes for as long as it is told, up to five seconds, and closing a game stills all")
{
    using namespace engine::platform;
    VibrationRig rig;
    vibrateDevice(0.5f, 0.25f);
    CHECK(static_cast<double>(rig.fake.strength) == doctest::Approx(0.5));
    CHECK(static_cast<double>(rig.fake.seconds) == doctest::Approx(0.25));
    vibrateDevice(2.0f, 60.0f);
    CHECK(static_cast<double>(rig.fake.strength) == doctest::Approx(1.0));
    CHECK(static_cast<double>(rig.fake.seconds) == doctest::Approx(5.0));

    setVibrationMotor(VibrationMotor::Small, 1.0f);
    pumpVibration();
    stopVibration();
    const int stopped = rig.fake.stops;
    CHECK(stopped >= 1);
    // The levels went with it: the next frame sends nothing.
    const int before = rig.fake.rumbles;
    pumpVibration();
    CHECK(rig.fake.rumbles == before);
}

// --- Gamepads start after the first frame (D476) -----------------------------------
//
// Starting the platform library's joystick subsystem is the slowest thing a
// start does that a player cannot see. So a run that wants gamepads does not
// start it in `init`, nor in the pump before the first frame: the second pump
// does, which is the one after the first frame was presented.

namespace {

struct TimedPlatform
{
    double toFirstFrame = 0.0;
    bool startedBeforeFirstFrame = false;
    bool startedAfterFirstFrame = false;
};

// One start of the platform with or without gamepads: how long until the
// pump before the first frame has returned, and when the subsystem began.
[[nodiscard]] TimedPlatform timedStart(bool gamepads)
{
    using Clock = std::chrono::steady_clock;
    TimedPlatform out;
    const auto begin = Clock::now();
    const auto error = engine::platform::init({.headless = true, .gamepads = gamepads});
    REQUIRE_MESSAGE(!error.has_value(), (error ? error->detail : std::string{}));
    (void)engine::platform::pumpEvents();
    out.toFirstFrame = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    out.startedBeforeFirstFrame = engine::platform::gamepadsStarted();
    // The frame is drawn and presented here; then the next pump.
    (void)engine::platform::pumpEvents();
    out.startedAfterFirstFrame = engine::platform::gamepadsStarted();
    engine::platform::shutdown();
    return out;
}

} // namespace

TEST_CASE("D476: gamepads are started by the pump after the first frame, and not before it")
{
    const TimedPlatform with = timedStart(true);
    CHECK_FALSE(with.startedBeforeFirstFrame);
    CHECK(with.startedAfterFirstFrame);

    // A run with no hands never starts them.
    const TimedPlatform without = timedStart(false);
    CHECK_FALSE(without.startedBeforeFirstFrame);
    CHECK_FALSE(without.startedAfterFirstFrame);
}

TEST_CASE("D476: wanting gamepads does not make the first frame later")
{
    // The best of five each, so one slow start of the video driver is not
    // what is compared. The subsystem itself takes about 95 ms on Windows:
    // within ten means it is not in here.
    double with = 1.0e9;
    double without = 1.0e9;
    for (int run = 0; run < 5; ++run) {
        without = (std::min)(without, timedStart(false).toFirstFrame);
        with = (std::min)(with, timedStart(true).toFirstFrame);
    }
    MESSAGE("to the first frame: ", without, " ms without gamepads, ", with, " ms with");
    CHECK(with - without < 10.0);
}

#ifdef _WIN32

TEST_CASE("D535: a window the system holds still hands the engine its frames")
{
    // **A real window**: the loop that held the engine is the system's, inside
    // a window procedure, and the offscreen driver has neither.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "windows");
    const auto error = engine::platform::init({.headless = false, .gamepads = false});
    if (error.has_value()) {
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
        MESSAGE("no window system here: ", error->detail);
        return;
    }
    seedRealCatalog();
    {
        EngineError failed;
        const auto window = engine::platform::createWindow(
            {.titleKey = ENG_TR("platform.window.title"), .width = 320, .height = 200, .visible = false}, &failed);
        REQUIRE_MESSAGE(window != nullptr, failed.detail);
        static_cast<void>(engine::platform::pumpEvents());
        CHECK_FALSE(engine::platform::pumpHeld());

        {
            using Clock = std::chrono::steady_clock;
            const Clock::time_point began = Clock::now();
            for (int at = 0; at < 2000; ++at)
                static_cast<void>(engine::platform::pumpEvents());
            MESSAGE("an idle pump: ", std::chrono::duration<double, std::micro>(Clock::now() - began).count() / 2000.0,
                    " us");
        }

        // Held for a second. The loop below is the engine's: a pump, and then
        // whatever a frame does.
        REQUIRE(engine::platform::simulateWindowHold(engine::platform::windowId(*window), 1000));
        using Clock = std::chrono::steady_clock;
        const Clock::time_point start = Clock::now();
        Clock::time_point last = start;
        double longestMs = 0.0;
        int held = 0;
        while (Clock::now() - start < std::chrono::milliseconds(1500)) {
            static_cast<void>(engine::platform::pumpEvents());
            const Clock::time_point now = Clock::now();
            longestMs = (std::max)(longestMs, std::chrono::duration<double, std::milli>(now - last).count());
            last = now;
            held += engine::platform::pumpHeld() ? 1 : 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        MESSAGE("held for a second: ", held, " frames lent, the longest wait for the pump ", longestMs, " ms");
        // It was one wait of the whole second, and no frame.
        CHECK(longestMs < 250.0);
        // Half the simulation's sixty a second at the least, on a loaded machine.
        CHECK(held >= 30);
        // And it is over: the pump is the system's queue again.
        static_cast<void>(engine::platform::pumpEvents());
        CHECK_FALSE(engine::platform::pumpHeld());
    }
    engine::platform::shutdown();
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
}

#endif
