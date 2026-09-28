#include "ui/tutorial/welcome_chain.h"

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

std::vector<Step> Make() {
    using enum Need;
    // What every step about a snippet on the canvas needs first.
    const std::vector<Need> onTheCanvas = {InTutorialFolder, CanvasUncovered, ASubject, SubjectHere,
                                           SubjectOnScreen};
    auto with = [&](std::initializer_list<Need> more) {
        std::vector<Need> needs = onTheCanvas;
        needs.insert(needs.end(), more);
        return needs;
    };

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
        .needs = with({SubjectCanMove, NoDrawingMode}),
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
        .needs = with({SubjectCanMove, NoDrawingMode, SubjectSelected}),
        .subject = SubjectRule::CanMove,
        .goal =
            [](const Look& look) {
                const SnippetFacts* now = nullptr;
                const SnippetFacts* then = nullptr;
                return SubjectOutOfFullscreen(look, now, then) && Resized(*now, *then);
            },
    });
    chain.push_back(Step{
        .id = "drawingMode",
        .kind = StepKind::Do,
        .gated = true,
        .title = strings::kTutorialDrawingModeTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialDrawingModeText); },
        .spot = Spot::Subject,
        .needs = onTheCanvas,
        .subject = SubjectRule::Any,
        .goal = [](const Look& look) { return look.subject && look.world.DrawingItem() == look.subject; },
    });
    chain.push_back(Step{
        .id = "draw",
        .kind = StepKind::Do,
        .title = strings::kTutorialDrawTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialDrawText); },
        .spot = Spot::DrawingBarPen,
        .needs = with({DrawingOnSubject}),
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
        .id = "delete",
        .kind = StepKind::Do,
        .gated = true,
        .title = strings::kTutorialDeleteTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialDeleteText); },
        .spot = Spot::SelectionBarClose,
        // Delete does nothing in drawing mode.
        .needs = with({NoDrawingMode}),
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

}  // namespace

const std::vector<Step>& WelcomeChain() {
    static const std::vector<Step> chain = Make();
    return chain;
}

}  // namespace sz::ui::tutorial
