#include "ui/view/screen_chrome.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <iterator>

#include "core/build_info/build_info.h"
#include "core/config/settings_catalog.h"
#include "generated/ui_strings.h"
#include "platform/platform_types.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <imgui.h>

namespace sz::ui {

using namespace ::sz::core;

ScreenChrome::ScreenChrome(Session& session, Settings& settings, ViewHost& host)
    : session_(session), settings_(settings), host_(host) {}

namespace {
// The rows of the input options HUD, in the order the number keys address
// them. Kept as data so the drawing and the key handling cannot disagree
// about which key means which option.
struct InputOptionRow {
    const char* label;
    // The setting the row shows and a number key flips - its row in the
    // catalog, which says where it is stored and what a profile says about
    // it.
    const ProfileSetting<BoolRule>* setting;
    // What decides whether the row can do anything - see
    // InputOptionAvailable - and whether flipping it needs edit mode
    // entered again.
    enum class Which {
        NoActivate,
        SoftwarePointer,
        DontForwardKeystrokes,
        RawMouseInput,
        CounterRawMouseInput,
        FreezeScreen,
    };
    Which which;
};

// Same names and same order as the Settings tab, so that finding the right
// combination here and then finding it again there isn't a translation
// exercise. The count travels to the platform via
// IOverlayWindow::SetInputOptionsHudDigits, so the number of digits the
// keyboard hook claims follows this table rather than a constant beside it.
//
// Ordered so that every option's prerequisites are above it, which is also
// what puts the Settings tab's tree at the top of both. "Don't steal focus"
// is what leaves the game receiving input, so the things that take it back
// follow it, contiguously, each link below the one it needs (see
// EditModeInputOptions' *CanBeUsed predicates). The last two need nothing
// here, so they sit at the bottom - the same statement the Settings tab
// makes by leaving them out of its tree.
// Which rows need edit mode re-entered to take effect at all. Freezing only
// captures the screen on entry, and no-activate decides how the window is
// shown - toggling either would otherwise read ON in the HUD while nothing
// had changed on screen. Everything else is pushed into the window live by
// OnSettingsChanged, exactly as the Settings tab does it, and needs no such
// thing - which matters because a restart is not free: see
// pendingOverlayRestart_.
constexpr bool RowNeedsOverlayRestart(InputOptionRow::Which which) {
    return which == InputOptionRow::Which::NoActivate || which == InputOptionRow::Which::FreezeScreen;
}

constexpr InputOptionRow kInputOptionRows[] = {
    {strings::kHudDontStealFocus, &setting::kDontStealFocus, InputOptionRow::Which::NoActivate},
    {strings::kHudDontForwardKeystrokes, &setting::kDontForwardKeystrokes,
     InputOptionRow::Which::DontForwardKeystrokes},
    {strings::kHudUseRawMouseInput, &setting::kRawMouseInput, InputOptionRow::Which::RawMouseInput},
    {strings::kHudCounterRawMouseInput, &setting::kCounterRawMouseInput, InputOptionRow::Which::CounterRawMouseInput},
    {strings::kHudUseSoftwarePointer, &setting::kSoftwarePointer, InputOptionRow::Which::SoftwarePointer},
    {strings::kHudFreezeScreenWhileEditing, &setting::kFreezeScreen, InputOptionRow::Which::FreezeScreen},
};
}  // namespace

bool ScreenChrome::InputOptionValue(int index) const {
    if (index < 0 || index >= static_cast<int>(std::size(kInputOptionRows))) {
        return false;
    }
    // The resolved value: the HUD reports what is running, which is the
    // whole reason it exists.
    return settings_.Live().*kInputOptionRows[index].setting->value;
}

// Whether a row's option can currently do anything - the same preconditions
// the Settings tab grays its checkboxes on, read from the one place that
// states them. An unavailable row is dimmed and its number key ignored:
// storing a change that has no effect, with nothing on screen saying so, is
// how you end up believing an option is broken.
bool ScreenChrome::InputOptionAvailable(int index) const {
    if (index < 0 || index >= static_cast<int>(std::size(kInputOptionRows))) {
        return false;
    }
    switch (kInputOptionRows[index].which) {
        case InputOptionRow::Which::NoActivate:
        case InputOptionRow::Which::FreezeScreen:
            return true;  // depend on nothing else here
        case InputOptionRow::Which::SoftwarePointer:
            return true;  // a matter of appearance; works with or without focus
        case InputOptionRow::Which::DontForwardKeystrokes:
            return platform::EditModeInputOptions::KeystrokesCanBeHeld(settings_.Live().dontStealFocus);
        case InputOptionRow::Which::RawMouseInput:
            return settings_.Live().InputOptions().RawMouseInputCanBeUsed(settings_.Live().dontStealFocus);
        case InputOptionRow::Which::CounterRawMouseInput:
            return settings_.Live().InputOptions().CounterRawMouseInputCanBeUsed(settings_.Live().dontStealFocus);
    }
    return false;
}

// A debugging aid, off by default - see AppConfig::showInputOptionsHud.
// Not always on, tempting as that is for something meant to be seen while
// standing in front of a misbehaving game, because of the cost: the number
// keys need a keyboard hook to reach an overlay that deliberately has no
// focus, so an always-on HUD meant digits never reached the game even with
// "Don't forward keystrokes" off. A diagnostic that quietly eats input is
// one to switch on deliberately.
void ScreenChrome::DrawInputOptionsHud(ImDrawList* drawList) const {
    if (!Cfg().showInputOptionsHud || drawList == nullptr) {
        return;
    }
    constexpr float kPad = 10.0f;
    constexpr float kLineHeight = 19.0f;
    constexpr float kOriginX = 14.0f;
    constexpr float kOriginY = 14.0f;
    const int rowCount = static_cast<int>(std::size(kInputOptionRows));

    float widest = 0.0f;
    for (const InputOptionRow& row : kInputOptionRows) {
        widest = std::max(widest, ImGui::CalcTextSize(row.label).x);
    }

    char fps[160];
    // Availability, not just the stored value: with raw input grayed out
    // there is no pointer of ours being driven, so its gain and step
    // histogram would be a readout of nothing.
    if (settings_.Live().InputOptions().useRawMouseInput && settings_.Live().InputOptions().RawMouseInputCanBeUsed(settings_.Live().dontStealFocus) &&
        host_.Window() != nullptr) {
        // Per-report step sizes against per-frame ones. All ones in the
        // first and twos in the second means the pointer arithmetic is fine
        // and it is the once-a-frame drawing that looks coarse - a
        // different problem with a different fix.
        const platform::InputGrabDiagnostics diag = host_.Window()->GetInputGrabDiagnostics();
        std::snprintf(fps, sizeof(fps), "%.0f fps  gain %.2f %s  report %d/%d/%d/%d  frame %d/%d/%d/%d",
                       ImGui::GetIO().Framerate, diag.pointerGain, diag.ballisticsEnabled ? "curve" : "flat",
                       diag.stepCounts[0], diag.stepCounts[1], diag.stepCounts[2], diag.stepCounts[3],
                       diag.frameSteps[0], diag.frameSteps[1], diag.frameSteps[2], diag.frameSteps[3]);
    } else {
        std::snprintf(fps, sizeof(fps), "%.0f fps   %.2f ms", ImGui::GetIO().Framerate,
                       1000.0f / std::max(1.0f, ImGui::GetIO().Framerate));
    }
    // What countering is actually managing, when it is on: how long the
    // game had each movement to itself before the negation arrived, and
    // which of the two injection timings produced that. The residual the
    // camera keeps is not in here and cannot be - see
    // InputGrabDiagnostics::correctionLagMsLast.
    // What the last number key did, and how much re-deriving has happened
    // since - see hudToggleCount_.
    char lastKey[160];
    lastKey[0] = '\0';
    if (hudLastToggledRow_ != 0) {
        std::snprintf(lastKey, sizeof(lastKey),
                       "last key %d: set %s, into %s  -  now prof=%s  (toggles %d, resolves %d)",
                       hudLastToggledRow_, hudLastToggledTo_ ? strings::kHotkeysOn : strings::kHotkeysOff,
                       hudLastWentToProfile_ ? strings::kProfilesNamePrefix : strings::kProfilesDefaults,
                       settings_.ActiveProfile() && *settings_.ActiveProfile() < settings_.Profiles().size()
                           ? settings_.Profiles()[*settings_.ActiveProfile()].name.c_str()
                           : "none",
                       hudToggleCount_, settings_.ResolveCount());
    }

    char counter[192];
    counter[0] = '\0';
    if (settings_.Live().InputOptions().counterRawMouseInput &&
        settings_.Live().InputOptions().CounterRawMouseInputCanBeUsed(settings_.Live().dontStealFocus) && host_.Window() != nullptr) {
        const platform::InputGrabDiagnostics diag = host_.Window()->GetInputGrabDiagnostics();
        std::snprintf(counter, sizeof(counter), "counter lag %.2f ms (max %.2f)  n=%d",
                       diag.correctionLagMsLast, diag.correctionLagMsMax, diag.correctionsInjected);
    }

    // What is in front and where it sits relative to us - the line that
    // answers "why is nothing in this panel moving". Above us, Windows
    // delivers that application's input to no lower-integrity process at
    // all, so every number here stays where it is however the rows are
    // set, and no shortcut of the overlay's arrives either. See
    // platform::ForegroundIntegrity.
    char foreground[224];
    foreground[0] = '\0';
    {
        const platform::ForegroundApp& app = settings_.UnderlyingApplication();
        const char* what = !app.executable.empty() ? app.executable.c_str()
                           : !app.title.empty()    ? app.title.c_str()
                                                   : "(nothing identifiable)";
        switch (app.integrity) {
            case platform::ForegroundIntegrity::Above:
                std::snprintf(foreground, sizeof(foreground),
                               "over %s  -  above us, none of its input reaches here", what);
                break;
            case platform::ForegroundIntegrity::NotAbove:
                std::snprintf(foreground, sizeof(foreground), "over %s  -  not above us", what);
                break;
            case platform::ForegroundIntegrity::Unknown:
                std::snprintf(foreground, sizeof(foreground), "over %s  -  integrity unreadable", what);
                break;
        }
    }

    // Number prefix, label, then the ON/OFF column clear of the longest label.
    const float statusX = Px(kOriginX) + Px(kPad) + Px(26.0f) + widest + Px(16.0f);
    // Wide enough for the header line too - it carries the pointer
    // diagnostics and is easily longer than the rows.
    const float panelW = std::max({statusX + Px(34.0f) + Px(kPad) - Px(kOriginX),
                                    ImGui::CalcTextSize(fps).x + Px(kPad) * 2.0f,
                                    ImGui::CalcTextSize(counter).x + Px(kPad) * 2.0f,
                                    ImGui::CalcTextSize(foreground).x + Px(kPad) * 2.0f,
                                    ImGui::CalcTextSize(lastKey).x + Px(kPad) * 2.0f});
    // One extra line for the frame rate: "the overlay feels slower with the
    // grab on" is a measurement, not an impression, and this is where it can
    // be read without leaving the situation that caused it. Then one for the
    // counter readout and one for the last key, on the frames there are any.
    const int extraLines = 1 + (counter[0] != '\0' ? 1 : 0) + (foreground[0] != '\0' ? 1 : 0) +
                          (lastKey[0] != '\0' ? 1 : 0);
    const float panelH = Px(kPad) * 2.0f + Px(kLineHeight) * static_cast<float>(rowCount + extraLines);

    const ImVec2 panelMin = Px(kOriginX, kOriginY);
    const ImVec2 panelMax(panelMin.x + panelW, panelMin.y + panelH);
    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(12, 15, 20, 205), Px(6.0f));
    drawList->AddRect(panelMin, panelMax, IM_COL32(255, 255, 255, 40), Px(6.0f));

    drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), Px(kOriginY) + Px(kPad)), IM_COL32(150, 158, 172, 255), fps);

    for (int i = 0; i < rowCount; ++i) {
        const float y = Px(kOriginY) + Px(kPad) + Px(kLineHeight) * static_cast<float>(i + 1);
        // A row whose prerequisite isn't met is dimmed and reads "--" rather
        // than ON/OFF: its stored value is still there and still what it will
        // do once the row above allows it, but saying ON about something that
        // is doing nothing is the one thing a diagnostic panel must not do.
        // Its number key is ignored to match.
        const bool available = InputOptionAvailable(i);
        char key[8];
        std::snprintf(key, sizeof(key), "%d", i + 1);
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), y),
                           available ? IM_COL32(150, 158, 172, 255) : IM_COL32(96, 102, 114, 255), key);
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad) + Px(20.0f), y),
                           available ? IM_COL32(226, 230, 238, 255) : IM_COL32(120, 126, 138, 255),
                           kInputOptionRows[i].label);

        const bool on = InputOptionValue(i);
        if (!available) {
            drawList->AddText(ImVec2(statusX, y), IM_COL32(120, 126, 138, 255), "--");
            continue;
        }
        drawList->AddText(ImVec2(statusX, y), on ? IM_COL32(90, 214, 130, 255) : IM_COL32(232, 100, 100, 255),
                           on ? strings::kHotkeysOn : strings::kHotkeysOff);
    }

    float footerLine = static_cast<float>(rowCount + 1);
    if (counter[0] != '\0') {
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), Px(kOriginY) + Px(kPad) + Px(kLineHeight) * footerLine),
                           IM_COL32(150, 158, 172, 255), counter);
        footerLine += 1.0f;
    }
    if (foreground[0] != '\0') {
        // Brighter when it is the answer: above us, nothing else in this
        // panel can be trusted to mean anything.
        const bool above = settings_.UnderlyingApplication().integrity == platform::ForegroundIntegrity::Above;
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), Px(kOriginY) + Px(kPad) + Px(kLineHeight) * footerLine),
                           above ? IM_COL32(232, 100, 100, 255) : IM_COL32(150, 158, 172, 255), foreground);
        footerLine += 1.0f;
    }
    if (lastKey[0] != '\0') {
        drawList->AddText(ImVec2(Px(kOriginX) + Px(kPad), Px(kOriginY) + Px(kPad) + Px(kLineHeight) * footerLine),
                           IM_COL32(150, 158, 172, 255), lastKey);
    }
}

void ScreenChrome::Update(bool editMode) {
    // Both halves of the same decision: the digits belong to the HUD only
    // while it is visible, and the platform needs to know so it can stop
    // holding a keyboard hook open on the HUD's behalf. Pushed only on a
    // change - installing or removing a hook is not a per-frame ask.
    const bool hudActive = Cfg().showInputOptionsHud && editMode;
    const int hudDigits = hudActive ? static_cast<int>(std::size(kInputOptionRows)) : 0;
    if (host_.Window() != nullptr && hudDigits != appliedInputOptionsHudDigits_) {
        host_.Window()->SetInputOptionsHudDigits(hudDigits);
        appliedInputOptionsHudDigits_ = hudDigits;
    }
    if (!hudActive) {
        // A restart asked for by a row is the HUD's business; with the panel
        // gone there is nobody left to have asked, and firing it later would
        // hide and show the overlay for no reason anyone could see.
        pendingOverlayRestart_ = false;
        return;
    }

    // A restart tears the whole input path down and builds it again - the
    // window is hidden and shown, which takes the keyboard hook with it. Doing
    // that while the key that triggered it is still held loses the key-up, and
    // a key ImGui still believes is held makes the *next* press no press at
    // all. So the restart waits for every HUD digit to be up; without the
    // wait about one press in three went missing.
    if (pendingOverlayRestart_) {
        bool anyDown = false;
        for (int i = 0; i < static_cast<int>(std::size(kInputOptionRows)); ++i) {
            anyDown = anyDown || ImGui::IsKeyDown(static_cast<ImGuiKey>(ImGuiKey_1 + i));
        }
        if (!anyDown) {
            pendingOverlayRestart_ = false;
            host_.RestartOverlay();
            return;  // rebuilt after this frame - see IPlatformHost::Post
        }
    }
}

// Reaches here as the Canvas level's (see CanvasLevel), so never while text
// is being typed or a panel or a popup is up: each of those takes every key.
bool ScreenChrome::HandleKey(const Event& event, bool editMode) {
    if (!Cfg().showInputOptionsHud || !editMode || event.repeat) {
        return false;
    }
    for (int i = 0; i < static_cast<int>(std::size(kInputOptionRows)); ++i) {
        if (event.key != '1' + i) {
            continue;
        }
        if (!InputOptionAvailable(i)) {
            // Dimmed in the panel, and inert here to match. Its prerequisite
            // is one of the rows above, so it is one keypress away.
            return true;
        }
        // Into the profile that matched, if one did - the HUD is about the
        // configuration that is running, and the running configuration is
        // that profile's. Deliberately not the Settings panel's own edit
        // target, which may be some other profile entirely.
        const bool wanted = !InputOptionValue(i);
        settings_.Set(*kInputOptionRows[i].setting, wanted, settings_.ActiveProfile());
        // Recorded for the HUD, which is the only place this can be seen
        // happening - see hudToggleCount_.
        ++hudToggleCount_;
        hudLastToggledRow_ = i + 1;
        hudLastToggledTo_ = wanted;
        hudLastWentToProfile_ = settings_.ActiveProfile().has_value();
        // Only two rows need edit mode re-entered, and the restart is now
        // deferred until the key that asked for it is back up - see
        // pendingOverlayRestart_.
        if (RowNeedsOverlayRestart(kInputOptionRows[i].which)) {
            pendingOverlayRestart_ = true;
        }
        return true;
    }
    return false;
}

// The demo build's permanent mark (see build::kDemoMode). Drawn in *both*
// edit and view-only mode - it goes wherever the overlay is visible at all,
// and a mark you could drop by pressing the other hotkey wouldn't be one.
// It needs no suppression for screen capture: the platform layer leaves the
// whole overlay window out of what it grabs (see CaptureScreen), so nothing
// this draws can reach a captured image.
//
// It moves, every kMoveSeconds. A mark that lives in one
// corner is a mark you stop seeing after a minute and can work around
// permanently - putting one snippet over it and never moving that snippet
// again. Wandering, it has to be dealt with rather than arranged around,
// which is the whole point of a nag.
//
// The screen is divided into a 3x3 grid and each move picks a *different*
// cell, plus a jitter within it: pure randomness lands in nearly the same
// spot often enough to read as the mark being stuck, and picking a new
// cell by stepping 1..8 cells on cannot repeat by construction.
void ScreenChrome::DrawDemoMark(ImDrawList* drawList, float displayW, float displayH) {
    if constexpr (!build::kDemoMode) {
        // Compiled and type-checked in every build; folded away entirely in
        // the ones where it's false. See build_config.h.in.
        return;
    } else {
        constexpr float kTextSize = 30.0f;
        constexpr float kMargin = 26.0f;
        constexpr float kLineGap = 2.0f;
        constexpr double kMoveSeconds = 10.0;
        constexpr int kGrid = 3;  // cells per axis

        // ImGui's clock only advances while frames are being drawn, so a
        // hidden overlay doesn't burn through positions it never showed -
        // the mark moves ten seconds of *being visible* after the last one.
        const auto move = static_cast<int64_t>(ImGui::GetTime() / kMoveSeconds);
        if (move != demoWatermarkMove_) {
            // One scramble, three uses: which cell to step to, and where in
            // it to sit. Cheap enough to not be worth a real generator, and
            // being a pure function of the move number keeps this
            // reproducible when something looks wrong.
            auto scramble = static_cast<uint32_t>(move) * 2654435761u;
            scramble ^= scramble >> 15;
            scramble *= 2246822519u;
            scramble ^= scramble >> 13;
            // 1..(cells-1), so the new cell is never the current one.
            constexpr int kCells = kGrid * kGrid;
            demoWatermarkCell_ = (demoWatermarkCell_ + 1 + static_cast<int>(scramble % (kCells - 1))) % kCells;
            demoWatermarkJitter_ = ImVec2(static_cast<float>((scramble >> 8) & 0xFF) / 255.0f,
                                           static_cast<float>((scramble >> 16) & 0xFF) / 255.0f);
            demoWatermarkMove_ = move;
        }

        // Faint enough to read as a mark on the glass rather than as
        // content, but not so faint it can be missed on a bright
        // background - which is the whole job.
        const ImU32 color = ToImColor(0xFFFFFFFFu, 0.20f);
        ImFont* font = ImGui::GetFont();
        const char* lines[] = {strings::kDemoTitle, strings::kDemoSubtitle};
        // Measured at the size actually being drawn, not the UI font's -
        // GetFont()->CalcTextSizeA takes the size, ImGui::CalcTextSize
        // doesn't - so the block's own width is the wider of the two lines.
        float blockW = 0.0f;
        for (const char* line : lines) {
            blockW = std::max(blockW, font->CalcTextSizeA(Px(kTextSize), FLT_MAX, 0.0f, line).x);
        }
        const float blockH = 2.0f * Px(kTextSize) + Px(kLineGap);

        // The cell grid covers the positions the block's *top-left* may
        // take, so the whole mark stays inside the margin whichever cell it
        // lands in.
        const float spanX = std::max(0.0f, displayW - 2.0f * Px(kMargin) - blockW);
        const float spanY = std::max(0.0f, displayH - 2.0f * Px(kMargin) - blockH);
        const float cellW = spanX / kGrid;
        const float cellH = spanY / kGrid;
        const float x = Px(kMargin) + static_cast<float>(demoWatermarkCell_ % kGrid) * cellW +
                        demoWatermarkJitter_.x * cellW;
        float y = Px(kMargin) + static_cast<float>(demoWatermarkCell_ / kGrid) * cellH +
                  demoWatermarkJitter_.y * cellH;
        for (const char* line : lines) {
            drawList->AddText(font, Px(kTextSize), ImVec2(x, y), color, line);
            y += Px(kTextSize) + Px(kLineGap);
        }
    }
}

// The "your clicks land here, not in the game" frame - see
// AppConfig::showEditModeBorder. Drawn only by Draw, which is
// edit-mode-only; view-only mode passes input straight through and so
// has nothing to warn about, and deliberately draws no border of its own.
//
// Inset by half its own width rather than drawn on the screen edge:
// ImDrawList::AddRect centers thickness on the path it's given, so a rect
// at the actual edge would have half of every side clipped away off-screen
// and the border would render at half the width the user asked for.
void ScreenChrome::DrawEditModeBorder(ImDrawList* drawList, float displayW, float displayH) const {
    if (!Cfg().showEditModeBorder || (Cfg().editModeBorderColorRGBA & 0xFFu) == 0 || Cfg().editModeBorderWidthPx <= 0.0f) {
        return;
    }
    if (Cfg().editModeBorderOnlyWhenEmpty) {
        // "Empty" means nothing at all on this canvas - no items (even a
        // blank one still proves the overlay is up) and no un-armed ink on
        // the live layer either. No canvas at all counts as empty too, and
        // is exactly when the cue is worth the most: the screen is
        // otherwise completely blank.
        const Canvas* canvas = Manager().CurrentOrNull();
        const bool anythingShown =
            canvas != nullptr && (!session_.LiveLayer().Strokes().empty() ||
                                  std::any_of(canvas->items.begin(), canvas->items.end(),
                                              [&](const Item& item) { return !Manager().IsDeleted(*canvas, item); }));
        if (anythingShown) {
            return;
        }
    }
    const float half = Cfg().editModeBorderWidthPx * 0.5f;
    drawList->AddRect(ImVec2(half, half), ImVec2(displayW - half, displayH - half),
                       ToImColor(Cfg().editModeBorderColorRGBA), 0.0f, ImDrawFlags_None,
                       Cfg().editModeBorderWidthPx);
}

// The three things that belong over the canvas rather than in it, each in
// its own layer so their heights can be stated rather than inherited from
// where in the frame they happen to be drawn. Called after everything the
// canvas holds and before the Overview, so the whole group sits between
// them - the Overview is the one panel that covers everything, because it
// is the one you go to when something on screen is in the way.
//
// Bottom to top:
//  - The input options HUD. On the foreground draw list it would sit on
//    top of the Overview - including the Settings tab holding the switch
//    that turns it off.
//  - The edit-mode border, which is the "your clicks land here" cue: a
//    frame drawn under the snippets is a frame a fullscreen snippet hides
//    completely, which is exactly when the cue matters.
//  - The demo mark, above the border and everything below it, so nothing
//    but the Overview can cover it.
//
// Nothing here takes input (see BeginScreenLayer), so none of it changes
// what can be clicked, dragged or drawn on.
void ScreenChrome::Draw(float displayW, float displayH) {
    DrawInputOptionsHud(BeginScreenLayer("##sz_input_hud_layer", displayW, displayH));
    EndScreenLayer();

    ImDrawList* chrome = BeginScreenLayer("##sz_chrome_layer", displayW, displayH);
    DrawEditModeBorder(chrome, displayW, displayH);
    DrawDemoMark(chrome, displayW, displayH);
    EndScreenLayer();
}

}  // namespace sz::ui
