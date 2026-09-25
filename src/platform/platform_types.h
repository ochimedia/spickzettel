#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Plain data shared between the platform backends and everything above
// them. Nothing in this directory may depend on core/ or on Dear ImGui.
namespace sz::platform {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// Screen-space rectangle, in the overlay window's own coordinates. Its own
// type rather than core::Rect because platform/ must not depend on core/.
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

enum class MouseButton {
    Left,
    Right,
    Middle,
};

enum class MouseEventKind {
    Down,
    Move,
    Up,
};

struct MouseEvent {
    Vec2 position;
    MouseButton button = MouseButton::Left;
    MouseEventKind kind = MouseEventKind::Move;
};

// A hotkey: modifiers plus one logical key. `key` is an uppercase letter or
// digit - the same value as the character, which is also its virtual-key
// code on every platform's native hotkey API - or a function key F1-F24 as
// kFunctionKeyBase + n, since those share no such convenient encoding. One
// int rather than two fields so there is no way to represent "both set".
// 0 means no key, i.e. unbound. No modifier is required: a bare function
// key is a legitimate hotkey.
struct KeyCombo {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    int key = 0;

    static constexpr int kFunctionKeyBase = 1000;  // F1 = 1001 ... F24 = 1024

    bool IsFunctionKey() const { return key > kFunctionKeyBase && key <= kFunctionKeyBase + 24; }
    // Only meaningful when IsFunctionKey().
    int FunctionKeyNumber() const { return key - kFunctionKeyBase; }

    bool IsValid() const {
        return (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || IsFunctionKey();
    }

    bool operator==(const KeyCombo&) const = default;
};

// What edit mode does with physical input while IOverlayWindow::
// SetEditModeNoActivate leaves the application underneath focused - and
// therefore receiving every mouse movement and keystroke as well.
//
// Every one of these is a measured behavior; the numbers are in
// docs/ARCHITECTURE.md under "Taking input back from the game". In short:
// a low-level hook that swallows input takes away the cursor, the legacy
// window messages, the clicks, GetAsyncKeyState and the whole raw
// *keyboard* stream. It cannot take away the raw *mouse* stream, which is
// what a first-person game reads for camera-look and which only ever goes
// to the foreground window - so the one thing that cannot be blocked is
// what counterRawMouseInput works around.
//
// All default to on: in the situation this group exists for, each helps
// more often than it hurts. They stay individually switchable because the
// right combination still depends on the game. Only meaningful together
// with the no-activate setting; with edit mode taking focus the ordinary
// way, the game has already stopped receiving input.
struct EditModeInputOptions {
    // Draw the pointer ourselves instead of relying on the OS cursor.
    //
    // Under a grab this decides only how the pointer is *shown*: the grab
    // keeps its position either way. On, it is drawn into the frame; off,
    // it is written to the real cursor with SetCursorPos, which moves at
    // report rate rather than frame rate. Off cannot work against a game
    // that pins the cursor to the screen center every frame - there is no
    // arbitrating that from another process - which is a reason to choose
    // the drawn pointer, not to force it. With nothing grabbed it is purely
    // cosmetic.
    bool useSoftwarePointer = true;
    // Take the mouse away from the game: read it from the raw input stream
    // and discard the physical events, so a drag over the overlay does not
    // also reach what is underneath. One switch because it is one
    // mechanism: the hook discards, which raw input cannot do, and raw
    // input is then the only place movement, buttons and the wheel can be
    // read from with exact sub-pixel deltas. Neither half alone works.
    bool useRawMouseInput = true;
    // Swallow physical keyboard input, so typing near the overlay does not
    // also walk the player forwards. The app's own global hotkeys keep
    // working because the grab recognizes and dispatches them itself; a
    // swallowing hook suppresses RegisterHotKey too.
    bool dontForwardKeystrokes = true;
    // Bank every physical mouse movement and inject the exact opposite, so a
    // camera integrating raw motion nets out to where it started. Needs the
    // mouse taken first - see CounterRawMouseInputCanBeUsed. Settled in
    // steps rather than per movement - see counterThreshold - so the camera
    // wanders in between, which a frozen screen hides. Anything with
    // anti-cheat discards injected input outright. Experimental, and off by
    // default: it injects input into whatever is underneath, which is asked
    // for per game rather than assumed.
    bool counterRawMouseInput = false;
    // How far, in device counts on either axis, the camera may wander before
    // the banked correction is injected; it is injected once more on leaving
    // edit mode. Settling only then puts a camera that ran into its pitch
    // limit somewhere else entirely - the game clamped the movement and the
    // correction undoes all of it - and settling per movement made the
    // camera shake and, on a 125 Hz mouse, pushed our own raw-input sink
    // past Windows' background rate cap. How many degrees a count is is up
    // to the game's sensitivity, hence a setting: at common shooter
    // sensitivities (0.02-0.07 degrees per count) the default is 2-7 -
    // small enough to pass for a twitch when the screen isn't frozen.
    int counterThreshold = 100;
    static constexpr int kCounterThresholdMin = 10;
    static constexpr int kCounterThresholdMax = 5000;

    // Whether the overlay draws its own pointer - one question, asked in one
    // place, so the side that hides the OS cursor and the side that draws
    // the replacement cannot disagree.
    bool SoftwarePointerDrawn() const { return useSoftwarePointer; }

    // Which options can do anything, given whether the game still holds
    // focus. Preconditions, not settings: an option whose precondition
    // fails keeps its stored value and has no effect, and every reader -
    // the Settings tab, the HUD, the grab - asks these rather than the
    // stored value. With focus taken the ordinary way there is nothing to
    // take: a game registers raw input without RIDEV_INPUTSINK and so only
    // receives it while foreground.
    static bool KeystrokesCanBeHeld(bool gameKeepsFocus) { return gameKeepsFocus; }
    bool RawMouseInputCanBeUsed(bool gameKeepsFocus) const { return gameKeepsFocus; }
    // A correction is injected as relative motion and goes through the
    // pointer ballistics on its way to the game, so with Windows still
    // driving the cursor it eats the user's own slow movement (measured:
    // 22px of travel became 1px). Taking the mouse makes the position ours
    // and the question moot.
    bool CounterRawMouseInputCanBeUsed(bool gameKeepsFocus) const {
        return gameKeepsFocus && useRawMouseInput;
    }
    bool operator==(const EditModeInputOptions&) const = default;
};

// Whether the foreground application runs at a higher integrity level
// than we do - something started as administrator, of which Task Manager
// is the everyday example on an account with admin rights. On an account
// without them it comes up at the ordinary level, since it asks for the
// highest one it can have rather than for administrator outright, and is
// then no higher than us. Which is why this is a comparison against our
// own level and never a test for elevation in the abstract.
//
// It matters because Windows cuts a lower-integrity process out of that
// application's input entirely: the low-level hooks stop being called and
// the raw-input sink stops receiving reports, so the grab has nothing to
// drive the pointer with and nothing to carry shortcuts. See
// docs/ARCHITECTURE.md.
//
// `Unknown` is a third answer and not a synonym for either: a process
// whose token cannot be read at all is one we know nothing about, and
// the two reasons it can happen point opposite ways. An elevated tool
// would want focus taken; a game behind an anti-cheat driver blocks the
// same query and is the one thing that must never have focus taken from
// it. Only `Above` is acted on.
enum class ForegroundIntegrity { Unknown, NotAbove, Above };

// What the overlay is up over: the application holding the foreground
// while edit mode deliberately does not, or the one that held it just
// before the overlay took focus. Every field is best-effort: the title
// may be empty, and the image path and the integrity level both need the
// process opened, which fails for one owned by another account (every
// service) or shielded by an anti-cheat driver. A higher integrity level
// on its own does not stop either - measured, and an elevated Task
// Manager reports its executable name like anything else.
struct ForegroundApp {
    std::string executable;  // "eldenring.exe", lowercased; empty if unreadable
    std::string title;       // the window's title at the time; may be empty
    ForegroundIntegrity integrity = ForegroundIntegrity::Unknown;

    bool Known() const { return !executable.empty() || !title.empty(); }
    bool operator==(const ForegroundApp&) const = default;
};

enum class TrayCommand {
    ToggleOverlay,
    Exit,
};

// One attached display. Position and size are desktop pixels with the
// primary's top-left at (0,0); a display left of or above it has negative
// coordinates. `id` has to mean the same monitor after a restart - on
// Windows the device path, which stays put for as long as the monitor is
// on the same port, unlike the \\.\DISPLAYn name Windows renumbers. `name`
// is the monitor's own model name, for showing and for finding the same
// monitor again on another port.
struct DisplayInfo {
    std::string id;
    std::string name;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool primary = false;
    int refreshHz = 0;  // 0 when unknown
    int scalePercent = 100;

    bool operator==(const DisplayInfo&) const = default;
};

// Debug scaffolding for the input options HUD: what the pointer is actually
// doing. `stepCounts` buckets how far the pointer moved per mouse report
// ([0] no whole pixel, [1] one, [2] two, [3] three or more), which tells a
// coarse gain from a smooth pointer sampled coarsely. Zeroed when a grab
// starts.
struct InputGrabDiagnostics {
    float pointerGain = 0.0f;        // pixels per device count, most recent report
    bool ballisticsEnabled = false;  // "enhance pointer precision" is on and its curve loaded
    int stepCounts[4] = {};
    int frameSteps[4] = {};  // the same, per rendered frame rather than per report

    // How long a physical movement went uncountered, from the first report
    // banked after a flush to the moment the correction is injected. The
    // one number our side of "hold the camera still" can be judged by; the
    // residual the game keeps depends on when it sampled, which nothing
    // outside it can see.
    float correctionLagMsLast = 0.0f;
    float correctionLagMsMax = 0.0f;
    int correctionsInjected = 0;
};

// What IOverlayWindow::CaptureRegionAsTexture returns: the uploaded GPU
// texture (0 if capture is unsupported or failed) plus the same pixels as
// a CPU buffer, so a caller can persist them without a second capture.
// `pixelsRGBA` is empty when the capture failed. A capture whose upload
// failed has pixels and a `textureHandle` of 0.
struct CaptureResult {
    uint64_t textureHandle = 0;
    std::vector<uint8_t> pixelsRGBA;  // width*height*4, row-major, top-left origin, RGBA8
    int width = 0;
    int height = 0;
};

// Cursor shapes the overlay asks the OS for over and above what Dear
// ImGui manages. Tiny on purpose: it exists because ImGuiMouseCursor_ has
// no crosshair and no pen, and every stand-in among its values claims
// something untrue. Add a shape only when the platform has it and ImGui
// does not.
enum class CursorShape {
    // Leave the cursor to ImGui, which is installing a shape of its own
    // this frame (a resize arrow, a hand, an I-beam). The platform keeps
    // its hands off entirely so the two do not fight.
    Default,
    // The plain arrow, asked for rather than merely not overridden. ImGui
    // installs a cursor only when *its* wanted shape changes, so on the
    // frame the pointer leaves a snippet nobody would put the arrow back
    // and the pen would stay until the mouse next moved.
    Arrow,
    // "Click or drag to place this" - a creation tool, or an eraser over a
    // snippet, where the exact point matters.
    Crosshair,
    // "Draw here" - a drawing tool over the snippet it would mark. Neither
    // ImGui nor Windows has a stock pen, so both pointers draw the one in
    // pen_glyph.h.
    Pen,
};

// How often the overlay needs drawing - see IOverlayWindow::SetFramePacing.
enum class FramePacing {
    // At the display's refresh rate: something moves by itself, or the
    // overlay is taking input.
    EveryFrame,
    // Only now and then, and whenever an event arrives that could change
    // what is shown. For a picture that does not change on its own.
    Idle,
};

// How a snippet's picture - a screenshot, rasterized strokes - is
// resampled when it is drawn at a size other than its own.
// See IOverlayWindow::ImageFilterCallback.
enum class ImageFilter {
    // The GPU's own bilinear filter, which is what every picture had before
    // there was a choice: soft enlarged, and aliasing below about half size.
    Bilinear,
    // One texel per pixel, unblended: blocky enlarged, which is the point
    // for pixel art and small captures blown up.
    Nearest,
    // Catmull-Rom, widened to the reduction when shrinking.
    Bicubic,
    // Lanczos-3, the same way: the sharpest of the four, at the most taps.
    Lanczos,
};

using FrameCallback = std::function<void(float deltaSeconds)>;
using MouseCallback = std::function<void(const MouseEvent&)>;
using TrayCommandCallback = std::function<void(TrayCommand)>;
using HotkeyCallback = std::function<void()>;

}  // namespace sz::platform
