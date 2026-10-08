#pragma once

// The Overview's Settings tab - docs/VIEW_LAYER.md, section 7. Every row
// in it is bound to its setting's row in the catalog and makes its own
// edit as it is changed (docs/SETTINGS.md, section 9); what it keeps of its
// own is which section is showing, whose values it shows, a profile name
// refused, and the displays last listed.

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/config/profile.h"
#include "core/config/settings_catalog.h"
#include "core/config/shortcut_action.h"
#include "core/session/settings.h"
#include "ui/editor.h"
#include "ui/icon_draw.h"
#include "ui/view/view_host.h"

namespace sz::ui {

// Every tool, with the icon, name and tooltip it is offered under - the
// marking tools and Select first, then the two creation tools, which is
// the order the Settings page's Shortcuts section lists them in. One table, so a
// tool is drawn and named the same way wherever it is shown.
struct GalleryTool {
    Tool tool;
    const Icon* icon;
    const char* name;
    const char* tooltip;
};
// Every create action - things that happen at once rather than being a tool.
struct CreateActionInfo {
    CreateAction action;
    const Icon* icon;
    const char* tooltip;
    const char* name;
};
// Defined once, in settings_page.cpp: the icons they point at are
// per-translation-unit constants, so a table defined in the header would
// hand each file a different set of addresses.
// Copy, Cut, Paste and Duplicate, for the Shortcuts tab to list them by.
struct ClipboardActionInfo {
    ClipboardAction action;
    const Icon* icon;
    const char* name;
};
extern const GalleryTool kGalleryTools[6];
extern const CreateActionInfo kCreateActions[2];
extern const ClipboardActionInfo kClipboardActions[6];

// The key a Settings row binds, for a tool, a create action and a
// clipboard action - the rows are listed by those, and bind the
// ShortcutAction config stores the key under. What the key then runs is
// the command table's (see CommandForShortcut).
ShortcutAction ShortcutForTool(Tool tool);
ShortcutAction ShortcutForCreateAction(CreateAction action);
ShortcutAction ShortcutForClipboardAction(ClipboardAction action);

class SettingsPage {
public:
    SettingsPage(core::Settings& settings, Editor& editor, ViewHost& host);

    // Which body the tab shows - see settingsSection_.
    //
    // Appearance/Drawing/Diagnostics are about you and are global;
    // Input/Shortcuts are about whatever is underneath, and are what a
    // per-application profile may override - see Draw.
    enum class SettingsSection { Appearance, Interaction, Profiles, Behavior, Hotkeys, Defaults, Debug };
    SettingsSection Section() const { return settingsSection_; }
    // `section` shown, as its row pressed shows it.
    void ShowSection(SettingsSection section);
    // Whose values the Behavior and Hotkeys sections show: nothing for the
    // defaults, else an index into Settings::Profiles. See editProfile_.
    std::optional<size_t> Showing() const { return editProfile_; }
    // Deletes these profiles, as their rows' trash buttons would, with
    // Showing following the list as it does then - the tutorial's Done
    // (docs/TUTORIAL.md, section 18.3).
    void RemoveProfiles(const std::vector<core::ProfileId>& ids);

    // The tab's body, inside the Overview's: the section list, and the
    // section picked.
    void Draw();

    // The overlay has been put back on screen: whose settings show is the
    // profile in effect, which the host resolved for what the overlay is
    // coming up over. Coming back to Settings over a game almost always
    // means coming back to that game's settings, and the picker says which
    // either way.
    void OnOverlayShown();
    // The Overview has opened: the displays are listed again - a trip
    // through the display configuration, so asked then and when the
    // monitor list opens, and not every frame.
    void OnPanelOpened();

    // Whether a hotkey row is armed, waiting for the combo the user wants -
    // a KeyCapture on the machine's Text level (see KeyCapture, which also
    // says why a hotkey that fires meanwhile is the press).
    bool IsCapturingHotkey() const { return CapturingHotkey().has_value(); }
    bool IsCapturingShortcut() const { return CapturingShortcut().has_value(); }
    // Ends the capture with `combo` as the answer. Nothing while none is
    // armed.
    void CompleteHotkeyCapture(platform::KeyCombo combo);
    // What clicking a hotkey row's button, or a shortcut row's, does. Arming
    // either disarms the other: both rows are on one page, each waits for
    // the next key, and one press bound it to both.
    void ArmHotkeyCapture(core::HotkeySlot slot);
    void ArmShortcutCapture(core::ShortcutAction action);

    // For as long as this runs, the settings file is left as it is - one a
    // newer version wrote, or one that could not be read (see
    // TrayController::StartOnStandInSettings) - so nothing changed here is
    // saved. Said above every section, where the changes are made: the
    // message box at the start said so too, but the app runs for days
    // after it. `path` in UTF-8.
    void SetFileKept(core::ConfigSource why, const std::string& path);
    // That line as it is drawn, or empty while settings are saved as usual.
    const std::string& FileKeptNotice() const { return fileKeptNotice_; }

private:
    const core::AppConfig& Cfg() const { return settings_.Stored(); }

    // The tab's own bodies, one per section in its list - see Draw for
    // what decides which settings live where.
    void RenderSettingsAppearance();
    void RenderSettingsInteraction();
    // One bar's buttons as a row to arrange: each is a tile that switches
    // it on or off when clicked and can be dragged onto another to move it
    // there.
    void RenderBarButtonRow(const char* id, const char* label, const core::GlobalSetting<core::BarRule>& row);
    void RenderSettingsBehavior();
    // What a new snippet starts with - see AppConfig::screenshotDefaults.
    void RenderSettingsDefaults();
    void RenderSettingsDebug();
    // The list of profiles, and what the overlay is up over. Edits are
    // collected into a copy and handed to Settings::SetProfiles once, at
    // the end - the codebase's usual
    // don't-mutate-while-rendering-from-it rule, and here it also keeps the
    // callback from re-entering the loop it was called from.
    void RenderSettingsProfiles();
    // Its two parts: the buttons that make a profile - for what the
    // overlay is up over, or a blank one - and one profile's row, closed
    // or open. Both edit what they are handed in place and say whether
    // they did; a row asks to be removed through `remove` rather than
    // erasing from the list it is being drawn from.
    bool RenderProfileMakers(std::vector<core::Profile>& edited);
    bool RenderProfileRow(size_t index, core::Profile& profile, bool& remove);
    // The picker at the top of the two overridable sections: whose values
    // are on screen - the defaults, the profile that matched, or any other
    // one - plus what it inherits and how much of it is set here.
    void RenderEditTargetPicker(core::ProfileGroup group);
    // The Settings calls, aimed at whichever of the defaults or a profile
    // the panel is showing (editProfile_) - which is not necessarily the
    // profile in effect. What the section being edited currently resolves
    // to; what it would resolve to with this target's own overrides taken
    // away is Settings::Base and needs no call: a profile inherits the
    // defaults and nothing else.
    core::ProfileableSettings EditedSettings() const { return settings_.ResolvedFor(editProfile_); }
    // A shortcut binding's override in the target being edited: handed back
    // to the defaults, and whether there is one.
    void ClearShortcutOverride(core::ShortcutAction action) { settings_.ClearShortcutOverride(action, editProfile_); }
    bool IsShortcutOverriddenHere(core::ShortcutAction action) const {
        return settings_.IsShortcutOverridden(action, editProfile_);
    }
    // One hotkey's own press-to-capture editor row, for the
    // edit/view/quick-capture hotkeys. A button showing the current combo;
    // clicking it arms a KeyCapture for this slot (clicking the armed
    // button again cancels), and the very next real key press becomes the
    // new combo, whatever modifiers happen to be held at that moment -
    // including none at all, and including a function key (see
    // platform::KeyCombo's own doc comment). "Press the combo you want"
    // rather than checkboxes plus a letter picker, which could not
    // represent a function key at all. See TryChangeHotkey for how a
    // captured combo takes effect.
    // `buttonX` is where the key's button starts, the same for every row
    // of the section - see KeyButtonColumn.
    void RenderHotkeyEditor(const char* id, const char* label, core::HotkeySlot slot, float buttonX);
    // Offers `combo` to the host (see ViewHost::ChangeHotkey), which stores
    // it once the OS has registered it. Returns false (and the hotkey is
    // left untouched) if the OS rejects it, so RenderHotkeyEditor's widgets
    // can show the edit didn't take rather than silently keeping a value
    // nothing downstream actually agreed to. A collision with one of this
    // app's own other hotkeys is not a rejection: that one is unbound
    // instead (see TrayController::ChangeHotkey).
    bool TryChangeHotkey(core::HotkeySlot slot, platform::KeyCombo combo);
    // The row waiting, if one is.
    std::optional<core::HotkeySlot> CapturingHotkey() const;
    std::optional<core::ShortcutAction> CapturingShortcut() const;
    // Stops a row waiting, binding nothing.
    void DisarmCapture();
    // The keyboard a waiting row borrows, and what gives it back - see
    // the definition of BorrowKeyboard.
    void BorrowKeyboard();
    std::function<void()> KeyboardRelease();
    // The Shortcuts section: every tool and create action, each with the
    // key it answers to. A section of its own rather than rows appended to
    // another one - eleven key editors would be most of whatever panel
    // they were put in, and "what is bound to what" is a question people
    // come to answer on its own. A summon hotkey goes to the OS before it
    // is stored - see TryChangeHotkey.
    void RenderSettingsHotkeys();
    // One row of it. A row waiting takes the next key pressed, or Escape,
    // Backspace or Delete for none - see KeyCapture.
    void RenderShortcutEditor(core::ShortcutAction action, const Icon& icon, const char* label, float buttonX);
    float KeyButtonColumn() const;

    core::Settings& settings_;
    Editor& editor_;
    ViewHost& host_;

    // Which body the tab shows. Not reset with the panel: coming back to
    // Settings usually means coming back to the same section, and the list
    // down the side makes where you are obvious anyway.
    SettingsSection settingsSection_ = SettingsSection::Appearance;
    // Whose values the Input and Shortcuts sections are showing. Nullopt is
    // the defaults; otherwise an index into Settings::Profiles. See
    // OnOverlayShown, and RenderProfileMakers, which point it at a profile
    // just made.
    std::optional<size_t> editProfile_;
    // A profile's name field while it says another profile's name, which
    // the rename refused: the row, and what was typed - said under the field
    // until it lets go. See RenderProfileRow.
    std::optional<std::pair<size_t, std::string>> takenProfileName_;
    // The host's displays, as last listed - see OnPanelOpened.
    std::vector<platform::DisplayInfo> displays_;
    // See SetFileKept.
    std::string fileKeptNotice_;
};

}  // namespace sz::ui
