#include "ui/tutorial/chains.h"

#include <cmath>

#include "generated/ui_strings.h"

namespace sz::ui::tutorial {

namespace {

namespace strings = ::sz::strings;

// How far a snippet has to go to count as moved, and how much its size
// has to change to count as resized: past what a click that wobbles, or
// a resize by accident on the way to a move, would do.
constexpr float kMovedPx = 16.0f;
constexpr float kResizedShare = 0.10f;
// How much an opacity has to change to count: two notches of the wheel
// (Editor's kWheelOpacityStep), less a rounding's worth, since the wheel
// and the sliders keep it on a grid of hundredths.
constexpr float kOpacityChanged = 0.10f - 0.005f;

bool Resized(const SnippetFacts& now, const SnippetFacts& then) {
    auto changed = [](float a, float b) { return b > 0.0f && std::fabs(a - b) >= kResizedShare * b; };
    return changed(now.rect.w, then.rect.w) || changed(now.rect.h, then.rect.h);
}

bool Moved(const SnippetFacts& now, const SnippetFacts& then) {
    return std::fabs(now.rect.x - then.rect.x) >= kMovedPx || std::fabs(now.rect.y - then.rect.y) >= kMovedPx;
}

// The subject, now and as the step first saw it - both, or neither.
bool SubjectBoth(const Look& look, const SnippetFacts*& now, const SnippetFacts*& then) {
    now = look.Subject();
    then = look.SubjectAtStart();
    return now != nullptr && then != nullptr;
}

// The subject as it can be moved and resized: both there, and not
// fullscreen now - which changes its rectangle, and is neither.
bool SubjectOutOfFullscreen(const Look& look, const SnippetFacts*& now, const SnippetFacts*& then) {
    return SubjectBoth(look, now, then) && !now->fullscreen;
}

const char* Fixed(const char* text) { return text; }

// What every step about a snippet on the canvas needs first.
std::vector<Need> OnTheCanvas() {
    using enum Need;
    return {InTutorialFolder, CanvasUncovered, ASubject, SubjectHere, SubjectOnScreen};
}
std::vector<Need> With(std::initializer_list<Need> more) {
    std::vector<Need> needs = OnTheCanvas();
    needs.insert(needs.end(), more);
    return needs;
}

std::vector<Step> MakeBasics() {
    using enum Need;
    std::vector<Step> chain;
    chain.push_back(Step{
        .id = "welcome",
        .title = strings::kTutorialWelcomeTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialWelcomeText); },
    });
    chain.push_back(Step{
        .id = "screenshot",
        .kind = StepKind::Do,
        .gated = true,
        .title = strings::kTutorialScreenshotTitle,
        .text =
            [](const World& world) {
                switch (world.ScreenshotTrigger()) {
                    case core::CreationTrigger::Plain:
                        break;
                    case core::CreationTrigger::Ctrl:
                    case core::CreationTrigger::Alt:
                        return Fixed(strings::kTutorialScreenshotTextTrigger);
                    case core::CreationTrigger::Off:
                        return Fixed(world.KeyLabel(CommandId::NewScreenshotTool)
                                         ? strings::kTutorialScreenshotTextTool
                                         : strings::kTutorialScreenshotTextMenu);
                }
                return Fixed(strings::kTutorialScreenshotText);
            },
        .needs = {InTutorialFolder, CanvasUncovered, NoDrawingMode, NoOtherTool},
        .goal =
            [](const Look& look) {
                for (const SnippetFacts* made : look.MadeHere()) {
                    if (made->picture && !made->fullscreen) {
                        return true;
                    }
                }
                return false;
            },
        .nearMisses =
            {
                {[](const Look& look) {
                     for (const SnippetFacts* made : look.MadeHere()) {
                         if (made->picture && made->fullscreen) {
                             return true;
                         }
                     }
                     return false;
                 },
                 strings::kTutorialScreenshotMissFullscreen},
                {[](const Look& look) {
                     for (const SnippetFacts* made : look.MadeHere()) {
                         if (!made->picture) {
                             return true;
                         }
                     }
                     return false;
                 },
                 strings::kTutorialScreenshotMissDrawing},
            },
    });
    chain.push_back(Step{
        .id = "move",
        .kind = StepKind::Do,
        .title = strings::kTutorialMoveTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialMoveText); },
        .spot = Spot::Subject,
        // In drawing mode a drag on the snippet draws.
        .needs = With({SubjectCanMove, NoDrawingMode}),
        .subject = SubjectRule::CanMove,
        .goal =
            [](const Look& look) {
                const SnippetFacts* now = nullptr;
                const SnippetFacts* then = nullptr;
                return SubjectOutOfFullscreen(look, now, then) && Moved(*now, *then) && !Resized(*now, *then);
            },
        .nearMisses =
            {
                {[](const Look& look) {
                     const SnippetFacts* now = nullptr;
                     const SnippetFacts* then = nullptr;
                     return SubjectOutOfFullscreen(look, now, then) && Resized(*now, *then);
                 },
                 strings::kTutorialMoveMissResized},
            },
    });
    chain.push_back(Step{
        .id = "resize",
        .kind = StepKind::Do,
        .title = strings::kTutorialResizeTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialResizeText); },
        .spot = Spot::SubjectHandle,
        .needs = With({SubjectCanMove, NoDrawingMode, SubjectSelected}),
        .subject = SubjectRule::CanMove,
        .goal =
            [](const Look& look) {
                const SnippetFacts* now = nullptr;
                const SnippetFacts* then = nullptr;
                return SubjectOutOfFullscreen(look, now, then) && Resized(*now, *then);
            },
    });
    chain.push_back(Step{
        .id = "delete",
        .kind = StepKind::Do,
        .gated = true,
        .title = strings::kTutorialDeleteTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialDeleteText); },
        .spot = Spot::SelectionBarClose,
        // Delete does nothing in drawing mode.
        .needs = With({NoDrawingMode}),
        .subject = SubjectRule::Any,
        // Any of the tutorial's snippets, live when the step saw it first:
        // the subject follows the hand, and which one went is the next
        // step's to know.
        .goal =
            [](const Look& look) {
                for (const core::ItemId id : look.deletedThisStep) {
                    const SnippetFacts* then = look.AtStart(id);
                    if (then != nullptr && !then->deleted) {
                        return true;
                    }
                }
                return false;
            },
    });
    chain.push_back(Step{
        .id = "undo",
        .kind = StepKind::Do,
        .title = strings::kTutorialUndoTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialUndoText); },
        // Undo takes back the canvas's own steps, and none while a panel
        // has the keys.
        .needs = {InTutorialFolder, CanvasUncovered, SubjectHere, DeletedSubject},
        .subject = SubjectRule::LastDeleted,
        // Back after being deleted in this step - deleted when it began
        // counts, being what the step before left.
        .goal =
            [](const Look& look) {
                const SnippetFacts* now = look.Subject();
                return now != nullptr && !now->deleted && look.deletedThisStep.contains(now->id);
            },
    });
    chain.push_back(Step{
        .id = "programs",
        .warning = true,
        .title = strings::kTutorialProgramsTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialProgramsText); },
    });
    chain.push_back(Step{
        .id = "antiCheat",
        .warning = true,
        .title = strings::kTutorialAntiCheatTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialAntiCheatText); },
    });
    chain.push_back(Step{
        .id = "away",
        .kind = StepKind::Do,
        .title = strings::kTutorialAwayTitle,
        .text =
            [](const World& world) {
                return Fixed(world.KeyLabel(CommandId::ToggleEditMode) ? strings::kTutorialAwayText
                                                                        : strings::kTutorialAwayTextTray);
            },
        .goal = [](const Look& look) { return look.world.Showings() > look.start.showings; },
    });
    chain.push_back(Step{
        .id = "end",
        .title = strings::kTutorialEndTitle,
        .text =
            [](const World& world) {
                return Fixed(world.KeyLabel(CommandId::CheatSheet) ? strings::kTutorialEndText
                                                                    : strings::kTutorialEndTextNoCheatSheetKey);
            },
    });
    return chain;
}

std::vector<Step> MakeDrawing() {
    using enum Need;
    std::vector<Step> chain;
    // First: in a new folder there is no snippet yet, and the need for one
    // offers the practice snippet (Put one here).
    chain.push_back(Step{
        .id = "drawingMode",
        .kind = StepKind::Do,
        .gated = true,
        .title = strings::kTutorialDrawingModeTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialDrawingModeText); },
        .spot = Spot::Subject,
        .needs = OnTheCanvas(),
        .subject = SubjectRule::Any,
        .goal = [](const Look& look) { return look.subject && look.world.DrawingItem() == look.subject; },
    });
    chain.push_back(Step{
        .id = "draw",
        .kind = StepKind::Do,
        .title = strings::kTutorialDrawTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialDrawText); },
        .spot = Spot::DrawingBarPen,
        .needs = With({DrawingOnSubject}),
        .subject = SubjectRule::Any,
        .goal =
            [](const Look& look) {
                const SnippetFacts* now = nullptr;
                const SnippetFacts* then = nullptr;
                return SubjectBoth(look, now, then) && now->strokes > then->strokes;
            },
    });
    chain.push_back(Step{
        .id = "stopDrawing",
        .kind = StepKind::Do,
        .title = strings::kTutorialStopDrawingTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialStopDrawingText); },
        .goal = [](const Look& look) { return !look.world.DrawingItem().has_value(); },
    });
    chain.push_back(Step{
        .id = "end",
        .title = strings::kTutorialDrawingEndTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialDrawingEndText); },
    });
    return chain;
}

bool AnyPinned(const Look& look) {
    for (const SnippetFacts& snippet : look.snippets) {
        if (snippet.pinned && !snippet.deleted) {
            return true;
        }
    }
    return false;
}

bool PictureFaded(const SnippetFacts& now, const SnippetFacts& then) {
    return std::fabs(now.pictureOpacity - then.pictureOpacity) >= kOpacityChanged;
}
bool DrawingFaded(const SnippetFacts& now, const SnippetFacts& then) {
    return std::fabs(now.drawingOpacity - then.drawingOpacity) >= kOpacityChanged;
}

std::vector<Step> MakePinning() {
    using enum Need;
    std::vector<Step> chain;
    // First, as in Drawing: a new folder has nothing to pin, and the need
    // for a subject offers the practice snippet.
    chain.push_back(Step{
        .id = "pin",
        .kind = StepKind::Do,
        .gated = true,
        .title = strings::kTutorialPinTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialPinText); },
        .spot = Spot::SelectionBarPin,
        // In drawing mode the bar over it is the drawing bar, with no Pin.
        .needs = With({NoDrawingMode, SubjectSelected}),
        .subject = SubjectRule::Any,
        // Any of the tutorial's: the bar pins the whole selection.
        .goal = &AnyPinned,
    });
    chain.push_back(Step{
        .id = "pinnedAway",
        .kind = StepKind::Do,
        .title = strings::kTutorialPinnedAwayTitle,
        .text =
            [](const World& world) {
                return Fixed(world.KeyLabel(CommandId::ToggleEditMode) ? strings::kTutorialPinnedAwayText
                                                                        : strings::kTutorialPinnedAwayTextTray);
            },
        // The pinned view shows the current canvas's pinned snippets that
        // are on screen.
        .needs = {InTutorialFolder, ASubject, SubjectHere, SubjectOnScreen, SubjectPinned},
        .subject = SubjectRule::Pinned,
        .goal = [](const Look& look) { return look.world.PinnedViews() > look.start.pinnedViews; },
        .nearMisses =
            {
                {[](const Look& look) { return look.world.ViewModes() > look.start.viewModes; },
                 strings::kTutorialPinnedAwayMissViewMode},
            },
    });
    chain.push_back(Step{
        .id = "opacity",
        .kind = StepKind::Do,
        .title = strings::kTutorialOpacityTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialOpacityText); },
        .spot = Spot::Subject,
        // The wheel changes the selection's.
        .needs = With({SubjectSelected}),
        .subject = SubjectRule::Any,
        // Only a change that shows: the strokes' opacity shows while there
        // are strokes - a note's text fades with its color instead.
        .goal =
            [](const Look& look) {
                const SnippetFacts* now = nullptr;
                const SnippetFacts* then = nullptr;
                return SubjectBoth(look, now, then) &&
                       (PictureFaded(*now, *then) || (now->strokes > 0 && DrawingFaded(*now, *then)));
            },
        .nearMisses =
            {
                {[](const Look& look) {
                     const SnippetFacts* now = nullptr;
                     const SnippetFacts* then = nullptr;
                     return SubjectOutOfFullscreen(look, now, then) && Resized(*now, *then);
                 },
                 strings::kTutorialOpacityMissResized},
                {[](const Look& look) {
                     const SnippetFacts* now = nullptr;
                     const SnippetFacts* then = nullptr;
                     return SubjectBoth(look, now, then) && now->strokes == 0 && DrawingFaded(*now, *then);
                 },
                 strings::kTutorialOpacityMissNothingDrawn},
            },
    });
    chain.push_back(Step{
        .id = "viewMode",
        .kind = StepKind::Do,
        .title = strings::kTutorialViewModeTitle,
        .text =
            [](const World& world) {
                if (!world.KeyLabel(CommandId::ToggleViewMode)) {
                    return Fixed(strings::kTutorialViewModeTextNoKey);
                }
                return Fixed(world.KeyLabel(CommandId::ToggleEditMode) ? strings::kTutorialViewModeText
                                                                        : strings::kTutorialViewModeTextTray);
            },
        .goal = [](const Look& look) { return look.world.ViewModes() > look.start.viewModes; },
        .nearMisses =
            {
                // Put away and back instead - with a key to name.
                {[](const Look& look) {
                     return look.world.Showings() > look.start.showings &&
                            look.world.KeyLabel(CommandId::ToggleViewMode).has_value();
                 },
                 strings::kTutorialViewModeMissAway},
            },
    });
    chain.push_back(Step{
        .id = "unpin",
        .kind = StepKind::Do,
        .title = strings::kTutorialUnpinTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialUnpinText); },
        .spot = Spot::SelectionBarPin,
        .needs = With({SubjectPinned, NoDrawingMode, SubjectSelected}),
        .subject = SubjectRule::Pinned,
        // One seen pinned in this step, pinned no more: a step begun with
        // nothing pinned asks for a pin first, and the unpin after it
        // counts.
        .goal =
            [](const Look& look) {
                for (const core::ItemId id : look.pinnedThisStep) {
                    const SnippetFacts* now = look.Now(id);
                    if (now != nullptr && !now->deleted && !now->pinned) {
                        return true;
                    }
                }
                return false;
            },
    });
    chain.push_back(Step{
        .id = "end",
        .title = strings::kTutorialPinningEndTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialPinningEndText); },
    });
    return chain;
}

}  // namespace

const std::vector<Step>& BasicsChain() {
    static const std::vector<Step> chain = MakeBasics();
    return chain;
}

const std::vector<Step>& DrawingChain() {
    static const std::vector<Step> chain = MakeDrawing();
    return chain;
}

const std::vector<Step>& PinningChain() {
    static const std::vector<Step> chain = MakePinning();
    return chain;
}

}  // namespace sz::ui::tutorial
