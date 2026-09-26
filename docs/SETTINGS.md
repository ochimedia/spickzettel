# Settings

Status: **agreed** (2026-09-26); phases 1 to 3 of section 12 are built.
Every behavior below is either what the app does today (unmarked, or
said so) or a change (marked **Change**). "Today" means the app as of
`654fff5`. The questions it was reviewed with, and their answers, are in section 13.

## The principle

The same as for input and for the overlay's states: settings are one
structured, well-defined thing. That decision matters in its own right,
beyond how the code happens to be written:

- **One catalog.** Every setting is one row in one catalog. That row is
  the only place that says four things: what the setting is called in
  the file, what values it may hold, whether a profile may override it,
  and when a change to it takes effect (section 3).
- **One edit path.** Every change to the stored settings is an edit made
  through `Settings`. The edit is held to the row's rule, and one commit
  follows it (section 6).
- **One version.** The file carries a version, and a file of an older
  version is migrated before it is read (section 8).

A change that fits is a new row or a changed cell. A change to the
structure has to be well founded and argued the way this document argues
its own shape: the case that does not fit, why no row or cell can
express it, and what it does to the rest. The structure is:

- the two layers;
- the edit path and the commit's order;
- the kinds of rule;
- how the file is versioned.

If the structure changes, this document changes with it, first.

## 1. What this is for

**One setting, many places.** `counterThreshold` is named 28 times in 7
files. It exists as four fields:

- `AppConfig::editModeInput.counterThreshold`;
- `ProfileableSettings::counterThreshold`;
- `ProfileOverrides::counterThreshold`;
- `platform::EditModeInputOptions::counterThreshold`.

Four functions copy it between them: `ProfileableFrom`,
`ApplyProfileable`, `InputOptions` and `SetInputOptions`. It is read in
two places (the defaults and a profile) and written in two, and it has a
row in `kProfileableIntFields`, a clause in `ProfileOverrides::Empty`
and a row in Settings. Adding a profileable setting means finding all of
these, and nothing fails when one is missed: a setting left out of
`ProfileableFrom` just never reaches a profile.

**One setting, several names.** The code and the file do not agree on
what settings are called:

| File | `AppConfig` | Elsewhere |
|---|---|---|
| `behavior.dontStealFocus` | `editModeNoActivate` | `ProfileableSettings::dontStealFocus`; `SetEditModeNoActivate`; the HUD's `NoActivate` |
| `behavior.freezeScreen` | `freezeScreenInEditMode` | `ProfileableSettings::freezeScreen` |
| `behavior.softwarePointer` | `editModeInput.useSoftwarePointer` | `ProfileableSettings::softwarePointer` |
| `deleted.deleteForGoodAutomatically` | `purgeDeleted` | |
| `drawing.raiseSelected` | `raiseSelectedSnippet` | |
| `overview.showStrokes` | `overviewShowsStrokes` | |

**Mirrored by hand.** `TryParseConfig` and `SerializeConfig` list every
setting twice, in two shapes, and a profile's reading and writing list
the profileable ones twice more. Each choice-valued setting has a pair of
functions spelling its names both ways (`ParseStrokeRenderMode` /
`StrokeRenderModeName`, and the same for image filters and creation
triggers). The four summon hotkeys are mapped from `HotkeySlot` to their
field in six places:

- `HotkeyCombo` in `command.cpp`;
- `TrayController::Initialize`;
- `TrayController::ChangeHotkey`;
- `OverlayApp::TryChangeHotkey`;
- the rows of Settings > Hotkeys;
- the parser and serializer.

**Nothing reads the version, and there is a release now.** Every save
writes `"version": 1`, and nothing reads it. So far that has cost
nothing: `43e4a2f` renamed the `input` group to `behavior` with no
migration, on the grounds that no published build had written `input`.
`v0.1.0` is tagged since. From here on, a renamed key is a setting that
silently goes back to its default for everyone who upgrades, and no test
notices. A file written by a *newer* build is worse off. It is read
best-effort, and the first settings change writes it back in this
build's shape, dropping whatever it did not know. The library refuses
exactly this case (`RefusedANewerLibrary`); the settings file does not.

**Rules in several places, and they differ:**

| Setting | Parser | Settings panel | Elsewhere |
|---|---|---|---|
| `deleted.afterDays` | held to 1..3650 | `std::clamp` to the same | |
| `counterThreshold` | held to 10..5000 | `ProfileableInt` clamps | |
| `editModeBorder.width` | ≤0 rejected, else 1..48 | slider band 1..48 | |
| creation triggers | a clash puts both back to the defaults | choosing the other's trigger swaps them | |
| summon hotkeys | no check | | a later duplicate is unbound at startup (`Initialize`); `ChangeHotkey` moves a combination over |
| profile names | nameless becomes "Profile"; duplicates numbered | new profiles numbered; **renaming is not checked** | |
| `strokeWidth` | >0, held to 256 | | the wheel keeps 1..24 |

**More than one way to change a stored setting:**

- **Plain fields:** written in place through `Mutable()`, then
  `Commit()`.
- **Profileable fields:** written through setters that name a target.
  But `Mutable()` reaches them as well. `interaction_cases_test.cpp`
  binds a shortcut that way, going around the profile path.
- **Hotkeys:** never commit. `ChangeHotkey` writes the field and calls
  `PersistConfig` itself. `TryChangeHotkey` then writes the same field
  again.
- **Startup:** `Initialize` unbinds duplicate hotkeys through
  `Mutable()` and persists, with no commit.

**Effects decided in odd places:**

- **Freeze release in the draw:** the Behavior section releases the
  frozen screen from inside its draw, on every frame it is shown, when
  the live `freezeScreen` is off. Everywhere else, freezing is the tray
  controller's business (`Apply` step 9, `MoveOverlayTo`).
- **A stale latch:** a display chosen in Settings is committed through a
  one-frame latch, `displayChoiceCommitPending_`. The latch exists so
  that the move and the freeze it retakes come before the frame draws.
  Since `a7a18f0`, the window's half of every commit runs after the
  frame anyway, so the reason is gone.
- **Two answers for one change:** "Don't steal focus" and "Freeze
  screen while editing" take effect differently depending on where they
  are changed (section 7).

## 2. Vocabulary

- **Setting**: one value the user can set, such as `counterThreshold`,
  or `showCanvasBar`. A tool shortcut is one setting per action.
- **Row**: a setting's entry in the catalog (section 3).
- **Scope**: Global, or Profile. A Profile setting may be overridden per
  application; a Global one may not.
- **Layers**: the *defaults* (what `config.json` holds at the top level)
  and a profile's *overrides* (sparse). The *live* values are the
  defaults with the matching profile's overrides applied. There are two
  levels, and that stays (section 4).
- **Target**: what an edit writes into, either the defaults or one
  profile. Always the defaults for a Global setting.
- **Rule**: what values a setting may hold, and what happens to one it
  may not: held to the nearest end, or rejected (section 5).
- **Invariant**: a condition across settings, such as "no two summon
  hotkeys share a combination". Each invariant has a *load repair*, for
  a file that breaks it, and an *edit repair*, for an edit that would.
- **Edit**: a change to the stored settings (section 6). A **preview**
  is an edit shown before it is finished (a color being dragged), and
  the **commit** finishes it.
- **Effect**: when a committed change is seen (section 7).

## 3. The catalog

Every setting, as the file names it. Every row is today's behavior
except where marked. "Edited from" is where the app changes it. The
effect classes are defined in section 7.

**`hotkeys`**: Global. Rule: a key combination, or `null` for unbound;
a mouse button reads as nothing said. Invariant: distinct (section 5).
Edited from Settings > Hotkeys, upper box. Effect: Registration.

| Key | Field | Default |
|---|---|---|
| `editMode` | `hotkeyEditMode` | Ctrl+Alt+S |
| `viewMode` | `hotkeyViewMode` | Ctrl+Alt+V |
| `quickCapture` | `hotkeyQuickCapture` | Ctrl+Alt+C |
| `silentCapture` | `hotkeySilentCapture` | Ctrl+Alt+X |

**`drawing`**: Global.

| Key | Field | Default | Rule | Edited from | Effect |
|---|---|---|---|---|---|
| `strokeColor` | `strokeColorRGBA` | `#FF0000` | color | the color chooser, as it closes | Start (the pen holds its own) |
| `strokeWidth` | `strokeWidth` | 3 | >0, held to 256 | the wheel, once its preview fades | Start |
| `renderMode` | `strokeRenderMode` | tessellated | tessellated, polyline, rasterized | Settings > Interaction | Frame |
| `raiseSelected` | `raiseSelectedSnippet` | true | bool | Settings > Interaction | Use |
| `screenshotTrigger` | `screenshotTrigger` | plain | plain, ctrl, alt, off; distinct | Settings > Interaction | Use |
| `drawingTrigger` | `drawingTrigger` | ctrl | the same | the same | Use |

**`appearance`**: Global. Edited from Settings > Appearance, except the
one row noted.

| Key | Field | Default | Rule | Effect |
|---|---|---|---|---|
| `showItemBorders` | `showItemBorders` | true | bool | Frame |
| `showToastsWhileHidden` | `showToastsWhileHidden` | true | bool; edited from Settings > Hotkeys | Use |
| `imageFilter` | `imageFilter` | bilinear | bilinear, nearest, bicubic, lanczos | Frame |
| `accentColor` | `accentColorRGBA` | `#2C6C7C` | color; previewed while dragged | Frame |
| `uiScale` | `uiScalePercent` | `"auto"` (0) | `"auto"`, or 75..300 held | Frame |
| `snippetColors.borderFront` | `itemBorderColorFrontRGBA` | `#F5F7F96E` | color; previewed | Frame |
| `snippetColors.borderOther` | `itemBorderColorOtherRGBA` | `#F5F7F93C` | the same | Frame |
| `snippetColors.borderPinned` | `itemBorderColorPinnedRGBA` | `#FF6A3D99` | the same | Frame |
| `canvasBar.show` | `showCanvasBar` | true | bool | Frame |
| `editModeBorder.show` | `showEditModeBorder` | true | bool | Frame |
| `editModeBorder.color` | `editModeBorderColorRGBA` | `#FFFFFF` | color; previewed | Frame |
| `editModeBorder.opacity` | `editModeBorderOpacity` | 0.22 | 0..1 held; previewed | Frame |
| `editModeBorder.width` | `editModeBorderWidthPx` | 10 | ≤0 rejected, else 1..48 held; previewed | Frame |
| `editModeBorder.onlyWhenEmpty` | `editModeBorderOnlyWhenEmpty` | false | bool | Frame |

**`bars`**: Global. Rule: a list of that bar's buttons with a shown
flag; normalized to hold each button exactly once. Edited from Settings
> Interaction. Effect: Frame.

| Key | Field |
|---|---|
| `snippet` | `snippetBar` |
| `drawing` | `drawingBar` |

**`overview`**: Global. Rule: bool. Edited from the Overview's header
checkboxes. Effect: Frame.

| Key | Field | Default |
|---|---|---|
| `showStrokes` | `overviewShowsStrokes` | true |
| `showBitmaps` | `overviewShowsBitmaps` | true |

**`defaults`**: Global. Edited from Settings > Defaults. Effect: Use
(the next snippet made).

| Key | Field | Default | Rule |
|---|---|---|---|
| `screenshot.keepAspect` | `screenshotDefaults.keepAspect` | true | bool |
| `screenshot.foregroundOpacity` | `screenshotDefaults.foregroundOpacity` | 1 | 0.1..1 held; previewed |
| `screenshot.backgroundOpacity` | `screenshotDefaults.backgroundOpacity` | 1 | 0..1 held; previewed |
| `drawing.keepAspect` | `drawingDefaults.keepAspect` | true | bool |
| `drawing.foregroundOpacity` | `drawingDefaults.foregroundOpacity` | 1 | 0.1..1 held; previewed |
| `drawing.backgroundOpacity` | `drawingDefaults.backgroundOpacity` | 0 | 0..1 held; previewed |
| `drawing.backgroundColor` | `drawingBackgroundColorRGBA` | `#FFFFFF` | color; previewed |
| `text.color` | `noteTextColorRGBA` | `#FFFFFF` | color; previewed |
| `text.size` | `noteTextSizePx` | undecided (0) | ≤0 or absent is undecided, else 8..96 held; not written while undecided; decided on the first frame at Windows' scale; previewed |

**`deleted`**: Global. Edited from Settings > Behavior, upper box.

| Key | Field | Default | Rule | Effect |
|---|---|---|---|---|
| `confirmDelete` | `confirmDelete` | true | bool | Use |
| `confirmDeleteForGood` | `confirmDeleteForGood` | true | bool | Use |
| `deleteForGoodAutomatically` | `purgeDeleted` | true | bool | Start |
| `afterDays` | `purgeDeletedAfterDays` | 14 | 1..3650 held | Start |

**`display`**: Global. Rule: text. Edited from Settings > Appearance.
Effect: Display.

| Key | Field | Default |
|---|---|---|
| `id` | `overlayDisplayId` | empty (the primary) |
| `name` | `overlayDisplayName` | empty |

**`behavior`**: Profile. Edited from Settings > Behavior, lower box, and
from the input options HUD, where the six booleans are a key each.

| Key | Field | Default | Rule | Effect |
|---|---|---|---|---|
| `dontStealFocus` | `editModeNoActivate` | true | bool | Window; two answers (section 7) |
| `takeFocusOverElevated` | `takeFocusOverElevated` | true | bool | Window |
| `softwarePointer` | `editModeInput.useSoftwarePointer` | true | bool | Window, and Frame |
| `rawMouseInput` | `editModeInput.useRawMouseInput` | true | bool | Window |
| `dontForwardKeystrokes` | `editModeInput.dontForwardKeystrokes` | true | bool | Window |
| `counterRawMouseInput` | `editModeInput.counterRawMouseInput` | false | bool | Window |
| `counterThreshold` | `editModeInput.counterThreshold` | 100 | 10..5000 held | Window |
| `freezeScreen` | `freezeScreenInEditMode` | false | bool | Freeze; two answers (section 7) |

**`shortcuts`**: Profile, one setting per `ShortcutAction` (13). Rule: a
key, or Mouse3-5, with modifiers, or `null` for unbound. Invariant: one
combination, one action within a target. Default: `DefaultShortcuts()`.
Edited from Settings > Hotkeys, lower box. Effect: Use.

**`diagnostics`**: Global. Rule: bool. Edited from Settings > Debug.
Effect: Frame.

| Key | Field | Default |
|---|---|---|
| `showDebugOverlay` | `showDebugOverlay` | false |
| `showInputOptionsHud` | `showInputOptionsHud` | false |

**`profiles`**: not a setting but the list of profiles. Each profile has
a name, match rules, and sparse `behavior` and `shortcuts` objects (the
two groups above, holding only what the profile overrides). Invariant:
names unique and not empty. Edited from Settings > Profiles. Effect:
Session.

The row order is the file's order, and the file does not change: the
catalog-driven writer produces today's text byte for byte (section 12,
phase 1).

## 4. Layers

This part stays as it is. There are two levels:

- **The defaults, then the matching profile's overrides.** The first
  profile in the list that matches wins.
- **Absent means inherit, `null` means explicitly unset.**
- **An override is what was touched, not what happens to differ.**
- **Resolved once per session.** The profile is resolved against what
  the session came up over (`docs/OVERLAY_STATES.md`, section 6), and
  resolved again at every commit against the same application.

**Change (structure only, C1)**: the profileable settings live in one
struct, not two. Today `AppConfig` keeps its own copies under its own
names (`editModeNoActivate`, `editModeInput`, `freezeScreenInEditMode`,
`toolShortcuts`). `ProfileableFrom` and `ApplyProfileable` copy them to
and from `ProfileableSettings` on every read and every edit. Instead,
`AppConfig` holds one `ProfileableSettings` (the defaults layer). The
two functions go, `Settings::Base()` becomes a reference instead of a
copy, and the names are the file's.

The reason `app_config.h` gives for two structs was that the file nests
these settings differently from how a profile addresses them. The
catalog rows now say where each is in the file, so that reason no
longer needs a second struct.

The plain fields keep their names. Renaming them is churn that buys
nothing; a row puts the file key and the field side by side, which is
where the difference matters.

## 5. Rules and invariants

**A rule belongs to its row, and everything that holds a value to it
asks the row**:

- the parser, reading the file;
- `Settings`, taking an edit;
- the Settings panel, which takes a slider's band or a field's limits
  from the rule rather than restating it.

The kinds are those the parser has today, named once:

| Rule | Held or rejected | Used by |
|---|---|---|
| bool | wrong type rejected | most rows |
| whole number in a band | a fraction rounded, held to the band | `afterDays`, `counterThreshold` |
| number in a band | held; not finite rejected | opacities |
| positive number, capped | ≤0 rejected, held to the cap | `strokeWidth` |
| positive number in a band | ≤0 rejected, held to the band | `editModeBorder.width`, `text.size` |
| color | `#RRGGBB` or `#RRGGBBAA`; written with eight digits only when not opaque | every color |
| choice | one of a list of names, any case | render mode, image filter, triggers |
| auto or percent | `"auto"`, or a whole number held to the band | `uiScale` |
| hotkey | a key combination, or `null`; a mouse button rejected | summon hotkeys |
| shortcut | a key or Mouse3-5, or `null` | tool shortcuts |
| text | any string | display |
| bar | names or `{button, shown}` objects; unknown names dropped; made to hold each of that bar's buttons once | bars |

**Change (C2)**: a number too large for a float is rejected by every
float rule. Today the banded positive rule reads `1e100` as the top of
its band (`editModeBorder.width` 48, `text.size` 96). The plain band
rule rejects it, for the reason its own comment gives: "a value the
user did not choose either".

**Invariants.** Each has one repair for a file and one for an edit.
These are today's except where marked:

| Invariant | Load repair | Edit repair |
|---|---|---|
| Creation triggers differ, unless off | both back to the defaults | the other takes the edited one's old trigger |
| Summon hotkeys differ, unless unbound | a later one is unbound (moved from `Initialize`) | the one that had the combination is unbound; the OS registration is tried first (section 6) |
| One shortcut, one action, per target | none | the others in the target are unbound |
| A profile's binding wins over an inherited same key | applied when resolving, not stored | the same |
| Profile names are not empty | "Profile" | a cleared field keeps the old name |
| Profile names are unique | numbered ("Game 2") | new profiles numbered; **Change (C3)**: a rename to a name another profile has is refused. The profile keeps its old name for as long as the typed one is taken, as with a cleared field, and a line under the field says the name is taken |

**Change (C4)**: load repairs are written back at start. Today only the
hotkey repair writes the file (`Initialize` calls `PersistConfig`); the
others leave it until the next settings change. With the repairs in one
place, a load that repaired anything or migrated the file (section 8)
reports it, and the tray writes the file once as it starts. Holding a
single value to its rule does not count: a file missing keys, or with a
value out of range, is written back at the next settings change, as
today.

## 6. Edits and the commit

Every change to the stored settings is one of these edits, through
`Settings`:

| Edit | What it does | Used by |
|---|---|---|
| `Set(row, value, target)` | holds the value to the row's rule, applies the edit repairs, and commits | a click, a picked item, a finished field |
| `Preview(row, value)` | holds and stores the value, commits nothing | a color or slider while dragged; Global rows only, as today |
| `Commit()` | finishes the previews | the dragged widget, as it lets go |
| `ClearOverride(row, profile)` | the profile inherits the row again; commits | the revert arrow |
| `SetProfiles(list)` | replaces the list, applies the name repairs, and commits | Settings > Profiles |

**Change (C5)**: `Mutable()` goes. Nothing writes the stored settings
except these edits. The app's own writes become `Set` too:

- the pen's color and width;
- the text size decided on the first frame;
- the tray's startup repair, which moves into the load repairs.

The tests use `Set` as well.

**Change (C6)**: a hotkey edit is an edit like the others. The tray
still tries the OS registration first, since that can fail and a refused
combination must not be stored. Only once it is registered does the
tray make the edit through `Set`, which unbinds the other hotkey (the
edit repair), commits, and writes the file the one way every commit
does. `TryChangeHotkey` no longer writes the field a second time. One
table maps `HotkeySlot` to its row, and the six mappings of section 1
use it.

**Change (C7)**: a preview is committed when the overlay settles. A drag
is committed when ImGui reports the widget deactivated, and that report
comes only in a frame that draws the widget. A drag the overlay is put
away in the middle of is not drawn again, so it stays in the stored
settings, where it is shown, and never reaches the file. This was found
by reading the code; its phase starts with a test that shows it.

**The commit, in order.** Today it runs from step 3; steps 1 and 2 are
the edit's own:

1. The value is held to the row's rule.
2. The edit repairs are applied.
3. The live values are resolved again, against what the session came up
   over.
4. The tray is told, and it posts the window's reconcile to after the
   frame (section 7). A commit usually happens in a frame
   (`docs/OVERLAY_STATES.md`, section 8).
5. The tray writes the file. It does this now, not after the frame, so a
   failure is said in the same frame, and a failed write is owed and
   retried as today.

**Considered and not proposed: saying which setting changed.** The
survey suggested notifying per setting. The window's side already works
the better way. After a commit, it compares what the settings want with
what it last gave the window, and applies the difference. That cannot
miss a change the way a missed notification can. It is what the overlay
work did for presentations, and the file is written whole either way.
What was wrong is only where some effects were decided (section 7), not
how the news travels.

## 7. When a change takes effect

Every row names one of these effect classes:

| Effect | When | Who |
|---|---|---|
| Frame | the next frame drawn | the view reads it as it draws |
| Use | the next time it is acted on: a press, a snippet made, a delete, a hotkey while hidden | the code that acts |
| Window | after the frame of the commit, compared with what the window was last given | `TrayController::ApplySettingsToWindow` |
| Display | after the frame, if the overlay is up; else at the next showing | the same, through `MoveOverlayTo` |
| Freeze | see below | |
| Registration | at the edit, before it is accepted | `TrayController::ChangeHotkey` |
| Session | at the commit: the matching profile is resolved again, and its values reach the window as Window does | `Settings`, then the tray |
| Start | the next start | `TrayController::Initialize`; the editor's pen |

**Change (C8)**: the display latch goes. A display picked in Settings is
set in the frame like any other setting. The move and the retaken frozen
screen come after the frame through the Window reconcile, which is what
the latch was for.

**Change (C9)**: releasing the frozen screen moves out of the Behavior
section's draw into the Window reconcile. The tray already owns the
condition for freezing, both on entry and on a display move. The release
comes after the frame instead of during it: one frame later, and no
longer on every frame the section is drawn.

**Two answers today.** Two Behavior rows take effect differently
depending on where they are changed:

| While in edit mode | Settings > Behavior | Input options HUD |
|---|---|---|
| "Don't steal focus" on | restyled at once; the overlay keeps the focus it took until the next entry | restart (hidden and shown), which hands focus back |
| "Don't steal focus" off | restyled, and focus taken at once | restart |
| "Freeze screen" on | at the next entry into edit mode | restart, which freezes on the way up |
| "Freeze screen" off | released at once (today in the draw; C9) | restart |

The Settings panel's reason for waiting to freeze is out of date
(section 10, finding 1). A display move already retakes the frozen
screen in place in edit mode. Removing the HUD's restart in favor of
applying these in place was left for later during the overlay work
(`docs/OVERLAY_STATES.md`, question 4). It needs the camera settle
measured with the input grab on in a game. This document records both
answers and does not settle them (question 4 here).

## 8. The file: version and migration

**Change (C10)**: the version is read.

- **Missing:** read as 1.
- **Older:** migrated before anything is read, through every step from
  its version to this build's (see "Migrations" below). The migrated
  file is written back at start (C4).
- **This build's:** read as today.
- **Newer, Change (C11):** read best-effort, as today, but not written
  over for as long as this build runs, and a message box says so. This
  is the same arrangement as a file that cannot be read
  (`StartOnStandInSettings(keepFile)`), and it skips the retention
  period for that start as that does. A newer build may have moved the
  retention keys, and read as defaults they would turn a 14-day purge
  back on. Settings changes still work for the session and are not
  saved.

**Migrations.** One step per version, from n to n+1 only. A file of
version n reaches version n+k by running all k steps in order. The
alternative, a conversion from each old version straight to the
current one, means rewriting every existing conversion whenever a new
version comes. Each rewrite would be made against a shape nobody has
looked at in a while. A chain writes each conversion once, against the
two shapes it sits between, while both are fresh. The price is running
k small steps over a file of a few kilobytes, once per upgrade, since
the result is written back.

The rules that keep a chain sound:

- **A step works on the JSON document, with the key names written out
  in it.** It never calls the catalog, the parser or anything else of
  the current build. Those change, and a step that used them would
  quietly change meaning with them. `MigrateV1ToV2(json&)` renames and
  moves keys in the text as version 1 and version 2 had them.
- **A step is never edited once a build that writes its target version
  has been released.** A fix is a new step.
- **Adding a key or dropping one is not a version.** An absent key reads
  as its default and an unknown key is ignored, so neither needs a
  step. A version is for what an old file would otherwise be read
  wrong in: a key renamed or moved, or a value whose meaning changed.
- **No steps downward.** A newer file is C11's case.

Tests: each step on its own, from a small document of its source
version. And the files each release wrote, read through the whole chain
to the config they were written from (below).

**No migration exists yet.** Version 1 is current, and C10 adds only the
means. Settings files are kept as test fixtures in
`tests/core/config_files/`, of two kinds:

- **What a release wrote**: `v0.1.0-defaults.json` and
  `v0.1.0-everything.json`, the second with every setting that release
  had at a value other than its default. They were produced by that
  release's own serializer and are never edited. Each must read as the
  config it was written from, so a later rename that forgets its
  migration fails a test instead of resetting a setting in the field.
- **What this build writes**: `current-defaults.json` and
  `current-everything.json`, which the writer must produce byte for
  byte. A change to the file, such as a new setting, then shows as a
  diff to review, and the test says where the new text was written.

**Stays as today:**

- malformed input is never an error: a value of the wrong type or out
  of range falls back as its rule says;
- a file that is not settings is set aside;
- one that cannot be read is left alone;
- the retention period is skipped for that start;
- the file is written through temp-then-rename;
- every setting is written, defaults included;
- keys are ordered, and floats are rounded to six decimals.

## 9. The Settings panel and the HUD

**The layout stays hand-written.** The survey suggested generating the
Settings rows from the catalog. `profile.h` records why the panel does
not walk a list: "its rows are as many different explanations, not a
list". Headings, the dependency tree of the input options, the Global
and Per-profile boxes and the order all stay as they are.

**The binding comes from the row.** Today each row has its own:

- plain rows bind ImGui to a field through `Cfg()` and set a shared
  `anyChanged`;
- profileable rows go through `ProfileableCheckbox` and
  `ProfileableInt`;
- sliders and swatches commit on deactivation, each written out again.

Instead, a few bound widgets each take a row and the label and help
text:

- a checkbox;
- a whole number field;
- a percent slider;
- a color swatch;
- a choice.

Each widget reads the value for the panel's target, writes with `Set`
or `Preview` and `Commit`, and takes its band from the rule. On a
profileable row it shows the override mark and the revert arrow. The
`anyChanged` flag goes. Grayed rows keep asking the preconditions in
`EditModeInputOptions`, as the HUD and the grab do.

The HUD's rows name their row, not one of the two switches
(`InputOptionField` and `InputOptionValue`) it keeps in parallel today.
Keys 1-6 and what each writes into (the active profile, or the
defaults) stay as they are.

## 10. What changes

| | Change | What it fixes |
|---|---|---|
| C1 | `AppConfig` holds `ProfileableSettings`; `ProfileableFrom` and `ApplyProfileable` go | two copies of the profileable settings under different names, copied on every read and edit |
| C2 | Every float rule rejects a number too large for a float | `1e100` read as the top of some bands and as the default in others |
| C3 | A rename to a taken profile name is refused; the old name stays | two profiles of one name, numbered behind the user's back at the next start |
| C4 | Load repairs and migrations are written back at start | only the hotkey repair was; the file said one thing and the app did another |
| C5 | `Mutable()` goes; every write is an edit through `Settings` | unvalidated writes; profileable fields reachable around the profile path |
| C6 | A hotkey edit is registered, then made with `Set` | hotkeys bypassing the commit; the field written twice; six slot-to-field mappings |
| C7 | A preview is committed when the overlay settles | a drag cut short by putting the overlay away is shown and never saved (found by reading) |
| C8 | The display latch goes | a one-frame latch whose reason went in `a7a18f0` |
| C9 | The frozen screen is released by the Window reconcile, not in a draw | a side effect of drawing, run every frame the section is shown |
| C10 | The version is read, and older files are migrated | a renamed key silently resets for everyone who upgrades |
| C11 | A file from a newer build is not written over, and the retention period is skipped for that start | the first settings change dropped what the newer build had stored |

Everything else in section 3 is as it is today.

## 11. Found while writing this

1. **An outdated reason in the Behavior section.** Its comment on
   "Freeze screen while editing" says switching it on cannot take
   effect at once, because "capturing means hiding this window and
   waiting for a composition pass". The capture leaves the window out
   with `WDA_EXCLUDEFROMCAPTURE`, and hides it only on Windows 10
   before 2004 (`win32_screen_capture.h`). `MoveOverlayTo` already
   retakes the frozen screen in place in edit mode. What the Settings
   panel does is a choice, recorded in section 7; the comment is
   corrected when C9 touches that code.
2. **`displayChoiceCommitPending_` outlived its reason**: C8.
3. **Profile renames are not checked for uniqueness**: C3.
4. **Two bands for the stroke width.** The file allows up to 256 px,
   while the wheel keeps 1..24, so a width of 100 set in the file
   becomes 24 on the first notch. Not changed: the row's rule is the
   file's, and the wheel's band belongs to the tool.
5. **`showToastsWhileHidden` sits in the file's `appearance` group and
   is edited in Settings > Hotkeys.** Not changed: moving a key is now a
   migration, and this one buys nothing.
6. **`docs/ARCHITECTURE.md` has drifted:**
   - its list of the file's groups leaves out `defaults` and `deleted`;
   - its Settings paragraph names the global sections "Appearance,
     Drawing and Debug", where they are now Appearance, Interaction,
     Defaults and Debug;
   - it speaks of "three global summon keys", where there are four.

   Corrected in the last phase.

## 12. Where the code goes, and getting there

Roughly, for review. The names may change in the building:

- **`core/config/setting.h`**: the row type and the rule kinds of
  section 5. A row holds:
  - its path in the file (group, optional subgroup, key);
  - its rule, scope and effect;
  - where its value lives: an accessor into `AppConfig` for a Global
    row, or member pointers into `ProfileableSettings` and
    `ProfileOverrides` for a Profile row. The compiler then checks that
    the two have the same type, as `kProfileableFields` does today.
- **`core/config/settings_catalog.{h,cpp}`**: the rows, in file order,
  and one way to visit them all. Reading, writing and a profile's
  sparse objects become loops over the rows. The hand-written parts are
  the profiles list and the invariants.
- **`core/config/config_migrations.cpp`**: the steps of section 8, none
  yet.
- **`core/session/settings.{h,cpp}`**: the edits of section 6.
- **`ui/settings_widgets.{h,cpp}`**: the bound widgets of section 9.

**Considered and not proposed:**

- **Overrides as an array of variants indexed by row, instead of
  `ProfileOverrides`.** One struct fewer, but untyped, and tests read
  worse without named fields. The row's member pointers already keep
  the two structs in step.
- **Declaring each setting once through macros.** It would be
  unreadable, and every field here carries a comment that is its
  documentation.

**Phases.** Each phase is its own set of commits. The tests stay green
throughout, and the file's text does not change until a phase says so.

1. **Fixtures first** (done). The files of section 8: what `v0.1.0`
   wrote and what today's build writes, for the defaults and with every
   setting changed, and the tests that read them.
2. **The catalog (C1, C2)** (done). Add the rows, and make reading and
   writing loops over them, the profiles' sparse objects included.
   `AppConfig` holds `ProfileableSettings`. Add one test that walks every
   row:
   - the default is written at the row's path;
   - a changed value survives the file, and changes that row's field and
     nobody else's;
   - a value outside the rule is held or rejected as the rule's `Hold`
     says, which is what `Set` will hold it to;
   - a wrong type keeps the default;
   - for a Profile row, an override survives the file inside a profile,
     and an absent one stays absent.

   The `current-*` files come out byte for byte.

   *Found while building it:* a bar's normalization concerns that one
   setting, so it is the bar rule's `Hold` rather than an invariant
   (section 5). And a `"uiScale": 0` written into the file by hand reads
   as 75, the bottom of the band, not as "auto": the file spells that
   `"auto"`, and 0 is only how the setting holds it. Kept as it was.
3. **The version (C10, C11), the repairs in one place, and writing them
   back (C4)** (done). The hotkey repair moves out of `Initialize`.

   *Found while building it:* a `version` that cannot be one (a string,
   a fraction, 0 or below) reads as 1, as a missing one does. A newer
   file still has the load repairs applied, since this build has to run
   on it, and is not written back all the same. The loader does not
   write a repaired file itself: the tray does, as it starts, so that a
   write that fails is said and retried like any other. With no step yet,
   a migrated file cannot be tested through the reader; the chain is
   tested with steps of its own.
4. **One edit path (C5, C6, C3, C7).** `Settings::Set`, `Preview` and
   `Commit`; the bound widgets; the HUD's rows; hotkeys through `Set`.
   C7 starts with the test that shows the lost drag.
5. **Effects (C8, C9).** The display latch and the release in the draw
   go. Headless tests check the Window and Display rows of section 7.
6. **The docs.** Set this document's status to built, update
   `docs/ARCHITECTURE.md` ("Configuration", "Session and settings", the
   Settings paragraph) including finding 6, and correct the comments
   that name `Mutable()`.

A guard against a new field that has no row: C++ cannot list a struct's
fields, so the per-row test cannot see one that is missing. Built in
phase 2 as two checks (question 6):

- **At compile time,** the number of initializers each struct takes. For
  `ProfileableSettings` and `ProfileOverrides` it is compared with the
  number of Profile rows, so it is exact. For `AppConfig` it is compared
  with a constant, a tripwire whose comment says to add the row.
- **A test** that takes the structs apart with structured bindings and
  checks that setting every row to another value changes every field
  (`profiles` aside). A row pointing at the wrong field of the same type,
  which no count can see, leaves a field unchanged here. The per-row test
  also names both rows.

## 13. Questions for review, and the answers

1. **A file from a newer build (C11).** Recommended: read it, and do
   not write over it for the run, with a message. The alternatives were
   to refuse to start, as for a newer library, or to write it but keep
   the keys this build does not know. Keeping them is subtle: a newer
   build's migration may have moved a key this build writes back.
   *Answer:* as recommended, if it is cheap and does not spread through
   the code. It is: a `ConfigSource` of its own, one message, and the
   existing `StartOnStandInSettings(/*keepFile=*/true)`, retention
   skip included.
2. **Writing load repairs back at start (C4).** Recommended: yes, so the
   file says what runs. Without it, the hotkey repair stays the one
   exception. *Answer:* yes.
3. **Renaming a profile to a taken name (C3).** Recommended: the old
   name stays until the typed one is free, as for a cleared field. The
   alternative was to number it on the spot ("Game 2"), which changes
   the text under the cursor while typing. *Answer:* refuse a taken
   name, which is the recommendation. A line under the field says why.
4. **The two answers of section 7.** Recommended: leave them. They are
   the overlay's question 4, which needs a measurement in a game first.
   The alternative was to settle them here by applying both in place,
   and dropping the HUD's restart. *Answer:* as recommended.
5. **Profileable fields named as the file names them (C1).** For
   example, `config.editModeNoActivate` becomes
   `config.profileable.dontStealFocus`. Recommended: yes, since they
   move anyway. The plain fields keep their names. *Answer:* as
   recommended.
6. **The field-count guard (section 12).** Recommended: yes. It is a
   tripwire, not a proof, and the only check available without
   reflection. *Answer:* as recommended.
7. **Migrations** (asked in review): should each migration go from n to
   n+1, with a file of version n running all k steps to reach n+k?
   *Answer:* yes. Section 8, "Migrations", gives the reasons and the
   rules that keep such a chain sound.