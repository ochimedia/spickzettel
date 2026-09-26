#include "ui/overlay_app.h"
#include "ui/overlay_app_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "core/canvas/item_geometry.h"
#include "ui/interaction/recognizer.h"

#include <imgui.h>

namespace sz::ui {

using namespace overlay_detail;

// ================= Tool / creation-arming state =================

namespace {

// The shortcut name of a mouse button a shortcut may be, or 0 for the two
// gestures are made with.
int ComboKeyForMouseButton(platform::MouseButton button) {
    switch (button) {
        case platform::MouseButton::Middle:
            return platform::KeyCombo::kMiddleButton;
        case platform::MouseButton::X1:
            return platform::KeyCombo::kX1Button;
        case platform::MouseButton::X2:
            return platform::KeyCombo::kX2Button;
        case platform::MouseButton::Left:
        case platform::MouseButton::Right:
            return 0;
    }
    return 0;
}

}  // namespace

void OverlayApp::SetToolShortcut(ShortcutAction action, platform::KeyCombo combo) {
    if (combo.key != 0) {
        // Whatever else held this key loses it. The alternative - refusing
        // the change - leaves the user to go and find the other holder
        // themselves, and two rows claiming one key is a state where only
        // the first of them could ever fire (see HandleCommandKey).
        //
        // Judged against what the *edited* target resolves to, not against
        // what is running: a collision inside a profile is a collision when
        // that profile is active, and a key the defaults use elsewhere is
        // not this profile's problem to solve.
        const ProfileableSettings edited = EditedSettings();
        for (const ShortcutAction other : kAllShortcutActions) {
            if (other != action && edited.shortcuts[ShortcutActionIndex(other)] == combo) {
                SetEditedShortcut(other, platform::KeyCombo{});
            }
        }
    }
    SetEditedShortcut(action, combo);
}

void OverlayApp::PlaceWelcomeNotes(float displayW, float displayH) {
    // Each sized to its text rather than to the screen - fitted to the
    // hand-wrapped lines at their size, a hair wider than the longest -
    // so none sits there mostly empty. The welcome is 10 lines at 18px;
    // the two warnings 8 lines at 22px, larger because they are the two
    // things a new user must not skip.
    //
    // All of it at the interface scale, although notes are content and
    // content is not scaled: these are the app talking, made while it
    // starts, and someone who reads at 150% should not have to find the
    // text-size slider to read the note that tells them where it is.
    const ImVec2 welcomeSize = Px(400.0f, 214.0f);
    const ImVec2 warningSize = Px(300.0f, 214.0f);
    const float gap = Px(24.0f);
    // A light red: a warning, readable on the note's dark backing, and not
    // the danger red of a delete button.
    constexpr uint32_t kWarningTextRGBA = 0xFF8C80FFu;

    // In a row, the welcome first, centered - or, on a screen too narrow
    // for that, in a column. On one too small for either they overlap
    // rather than going off screen, which ClampRectToViewport sees to.
    const float rowW = welcomeSize.x + 2.0f * (warningSize.x + gap);
    const bool row = rowW <= displayW * 0.95f;
    const ImVec2 group = row ? ImVec2(rowW, welcomeSize.y)
                             : ImVec2(welcomeSize.x, welcomeSize.y + 2.0f * (warningSize.y + gap));
    ImVec2 at((displayW - group.x) * 0.5f, (displayH - group.y) * 0.5f);

    editor_.EnsureCanvasForNewItem();
    // Made as they are, text and all, and not on the history: nobody made
    // them, so there is nothing for an undo to take back.
    const auto place = [&](ImVec2 size, const char* name, std::string text, float textSizePx,
                           uint32_t textColorRGBA) -> ItemId {
        Item note;
        note.name = name;
        note.rect = ClampRectToViewport(Rect{at.x, at.y, size.x, size.y}, displayW, displayH);
        (row ? at.x : at.y) += (row ? size.x : size.y) + gap;
        // Boxes of text, whose shape is the point of resizing them: the text
        // wraps to the new width rather than scaling with it.
        note.keepAspect = false;
        // A Text Note's backing (see kNoteBackgroundColorRGBA), but darker:
        // these land on whatever the desktop happens to show, and half
        // transparent over a white window left the red text washed out.
        note.picture.tintColorRGBA = kNoteBackgroundColorRGBA;
        note.picture.opacity = 0.8f;
        note.noteText = std::move(text);
        note.noteTextSizePx = std::min(textSizePx, kNoteTextSizeMax);
        note.noteTextColorRGBA = textColorRGBA;
        return session_.CreateItem(std::move(note), /*undoable=*/false);
    };
    // Deliberately short. This is the first thing anyone sees, and its job
    // is only to get them to the point where the app can explain itself:
    // one gesture, the right-click menus, the key that brings the overlay
    // back, and the cheat sheet for everything else - with the keys as
    // they are bound, which a config carried over from elsewhere may have
    // changed. Wrapped by hand at a width the note's own rect fits, since
    // Item::noteText is drawn as-is (DrawItemContent wraps too, but on its
    // own boundaries - keeping the key lines intact reads better than
    // letting them break wherever the item's width happens to fall).
    const std::string showKey = FormatKeyComboLabel(Cfg().hotkeyEditMode);
    const platform::KeyCombo& sheetKey =
        settings_.Live().shortcuts[ShortcutActionIndex(ShortcutAction::CheatSheet)];
    char text[512];
    if (sheetKey.key != 0) {
        std::snprintf(text, sizeof(text), strings::kWelcomeBody, showKey.c_str(),
                      FormatKeyComboLabel(sheetKey).c_str());
    } else {
        std::snprintf(text, sizeof(text), strings::kWelcomeBodyNoCheatSheetKey, showKey.c_str());
    }
    if (place(welcomeSize, strings::kWelcomeName, text, Px(18.0f), Item{}.noteTextColorRGBA) == 0) {
        return;
    }

    // Behavior and profiles, because the right input settings differ by
    // game and the defaults will be wrong for some; anti-cheat, because
    // hooking input and drawing over a game is what such a system looks
    // for, and a ban is not something to find out about afterwards.
    for (const auto& [name, body] : {std::pair{strings::kWelcomeBehaviorName, strings::kWelcomeBehaviorBody},
                                     std::pair{strings::kWelcomeAntiCheatName, strings::kWelcomeAntiCheatBody}}) {
        place(warningSize, name, body, Px(22.0f), kWarningTextRGBA);
    }
}

// The Canvas level: a press is what the recognizer says it is (see
// RecognizePress), and what the Gesture level above does not claim of the
// rest of a press is nobody's - a hover, or a button the gesture ignores.
// Keys and the wheel go to the handlers that took them before the
// machine, until the levels above take them over - see
// docs/INTERACTIONS.md, "Phase 3, in steps".
class OverlayApp::CanvasRoot : public Interaction {
public:
    explicit CanvasRoot(OverlayApp& app) : app_(app) {}
    Level level() const override { return Level::Canvas; }
    const char* Name() const override { return "Canvas"; }
    Answer Offer(const Event& event, Editor& editor) override {
        switch (event.kind) {
            case EventKind::PointerDown:
                if (event.button == platform::MouseButton::Left || event.button == platform::MouseButton::Right) {
                    return RecognizePress(event, editor);
                }
                // A shortcut the button may be - never while a gesture is in
                // flight, which the Gesture level above sees to.
                app_.HandleCommandKey(ComboKeyForMouseButton(event.button), /*repeat=*/false);
                return Answer::Claim();
            case EventKind::PointerMove:
            case EventKind::PointerUp:
                return Answer::Claim();  // dropped
            case EventKind::Wheel:
                app_.HandleMouseWheel(event.wheel);
                return Answer::Claim();
            case EventKind::KeyDown:
                app_.HandleCommandKey(event.key, event.repeat);
                return Answer::Claim();
            case EventKind::KeyUp:
            case EventKind::Modifiers:
            case EventKind::Tick:
                return Answer::Claim();  // nothing waits on these here
            case EventKind::Hotkey:
                return Answer::Start(Command{event.command});
            case EventKind::Lifecycle:
                return Answer::Claim();  // the tray still tells the app directly
        }
        return Answer::Claim();
    }
    // The root is never ended.
    void Interrupt(Editor& /*editor*/) override {}
    void Cancel(Editor& /*editor*/) override {}

private:
    OverlayApp& app_;
};

void OverlayApp::InstallCanvasRoot() { editor_.Input().SetRoot(std::make_unique<CanvasRoot>(*this)); }

void OverlayApp::OnHotkey(CommandId command, const platform::KeyCombo& combo) {
    Event event;
    event.kind = EventKind::Hotkey;
    event.command = command;
    event.combo = combo;
    event.modifiers = editor_.Held();
    editor_.Input().Offer(event);
}

void OverlayApp::OnInput(const platform::InputEvent& event) {
    // Real OS-level click-through (see IOverlayWindow::SetInputPassthrough)
    // means view-only mode receives no input on Windows; guarded here too so
    // it is read-only on every backend, not just the real one.
    if (viewOnly_) {
        return;
    }
    editor_.SetHeld(event.modifiers);
    editor_.SetNow(event.seconds);
    // The display as the last frame saw it, which is what the event's
    // position is in.
    if (ImGui::GetCurrentContext() != nullptr) {
        editor_.SetDisplaySize(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
    }
    editor_.Input().Offer(Event::FromInput(event));
}

}  // namespace sz::ui
