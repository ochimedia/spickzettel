#include "ui/tutorial/chains.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

bool PictureFaded(const SnippetFacts& now, const SnippetFacts& then) {
    return std::fabs(now.pictureOpacity - then.pictureOpacity) >= kOpacityChanged;
}
bool DrawingFaded(const SnippetFacts& now, const SnippetFacts& then) {
    return std::fabs(now.drawingOpacity - then.drawingOpacity) >= kOpacityChanged;
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
        .spot = Spot::SubjectCorner,
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

// What counts as a change that shows in what is drawn - section 15.2: a
// color a channel 64 apart, a width two notches of the wheel (a pixel
// each) less a rounding's worth, since a stroke's width is stored scaled
// and scaled back, and the ink 16 px shorter.
constexpr int kColorApart = 64;
constexpr float kWidthChanged = 2.0f - 0.25f;
constexpr float kInkGone = 16.0f;

bool ColorsApart(uint32_t a, uint32_t b) {
    for (const int shift : {24, 16, 8}) {
        const int one = static_cast<int>((a >> shift) & 0xFFu);
        const int other = static_cast<int>((b >> shift) & 0xFFu);
        if (std::abs(one - other) >= kColorApart) {
            return true;
        }
    }
    return false;
}

// Strokes on the subject that `drawn` says yes to, now and as the step
// first saw it - so a stroke from before the step never counts, and
// undoing the new one takes the step back to waiting.
using StrokeCheck = bool (*)(const Look& look, const StrokeFacts& stroke);
bool MoreDrawn(const Look& look, StrokeCheck drawn) {
    const SnippetFacts* now = nullptr;
    const SnippetFacts* then = nullptr;
    if (!SubjectBoth(look, now, then)) {
        return false;
    }
    auto count = [&](const SnippetFacts& snippet) {
        return std::count_if(snippet.strokes.begin(), snippet.strokes.end(),
                             [&](const StrokeFacts& stroke) { return drawn(look, stroke); });
    };
    return count(*now) > count(*then);
}

bool InNewColor(const Look& look, const StrokeFacts& stroke) {
    return ColorsApart(stroke.colorRGBA, look.start.penColor);
}
bool InNewWidth(const Look& look, const StrokeFacts& stroke) {
    return std::fabs(stroke.widthPx - look.start.penWidth) >= kWidthChanged;
}
bool IsFreehand(const Look&, const StrokeFacts& stroke) { return stroke.shape == core::DrawShape::Freehand; }
bool IsLine(const Look&, const StrokeFacts& stroke) { return stroke.shape == core::DrawShape::Line; }
bool IsRectangle(const Look&, const StrokeFacts& stroke) { return stroke.shape == core::DrawShape::Rectangle; }

// Whether the subject's opacity changed in a way the wheel's width step
// could be mistaken for - Ctrl or Shift held.
bool SubjectFaded(const Look& look) {
    const SnippetFacts* now = nullptr;
    const SnippetFacts* then = nullptr;
    return SubjectBoth(look, now, then) && (PictureFaded(*now, *then) || DrawingFaded(*now, *then));
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
        .goal = [](const Look& look) { return look.subject && look.world.IsDrawingOn(*look.subject); },
    });
    chain.push_back(Step{
        .id = "draw",
        .kind = StepKind::Do,
        .title = strings::kTutorialDrawTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialDrawText); },
        .spot = Spot::SelectionBarPen,
        .needs = With({DrawingOnSubject}),
        .subject = SubjectRule::Any,
        .goal =
            [](const Look& look) {
                const SnippetFacts* now = nullptr;
                const SnippetFacts* then = nullptr;
                return SubjectBoth(look, now, then) && now->strokes.size() > then->strokes.size();
            },
    });
    // The bar, a button at a time. Each counts once it shows on the
    // snippet: a color or a width once something is drawn with it.
    chain.push_back(Step{
        .id = "color",
        .kind = StepKind::Do,
        .title = strings::kTutorialColorTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialColorText); },
        .spot = Spot::SelectionBarColor,
        .needs = With({DrawingOnSubject, PenInHand}),
        .subject = SubjectRule::Any,
        .goal = [](const Look& look) { return MoreDrawn(look, &InNewColor); },
        .nearMisses =
            {
                {[](const Look& look) { return ColorsApart(look.world.PenColor(), look.start.penColor); },
                 strings::kTutorialColorMissNothingDrawn},
            },
    });
    chain.push_back(Step{
        .id = "width",
        .kind = StepKind::Do,
        .title = strings::kTutorialWidthTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialWidthText); },
        .spot = Spot::Subject,
        .needs = With({DrawingOnSubject, PenInHand}),
        .subject = SubjectRule::Any,
        .goal = [](const Look& look) { return MoreDrawn(look, &InNewWidth); },
        .nearMisses =
            {
                {&SubjectFaded, strings::kTutorialWidthMissOpacity},
                {[](const Look& look) {
                     return std::fabs(look.world.PenWidth() - look.start.penWidth) >= kWidthChanged;
                 },
                 strings::kTutorialWidthMissNothingDrawn},
            },
    });
    // The pen's shapes, picked from its button's menu: a shape is the
    // pen's until another is picked, so both come before the eraser.
    chain.push_back(Step{
        .id = "line",
        .kind = StepKind::Do,
        .title = strings::kTutorialLineTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialLineText); },
        .spot = Spot::SelectionBarPen,
        .needs = With({DrawingOnSubject, PenInHand}),
        .subject = SubjectRule::Any,
        .goal = [](const Look& look) { return MoreDrawn(look, &IsLine); },
        .nearMisses =
            {
                {[](const Look& look) { return MoreDrawn(look, &IsFreehand); }, strings::kTutorialLineMissFreehand},
            },
    });
    chain.push_back(Step{
        .id = "rectangle",
        .kind = StepKind::Do,
        .title = strings::kTutorialRectangleTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialRectangleText); },
        .spot = Spot::SelectionBarPen,
        .needs = With({DrawingOnSubject, PenInHand}),
        .subject = SubjectRule::Any,
        .goal = [](const Look& look) { return MoreDrawn(look, &IsRectangle); },
        .nearMisses =
            {
                {[](const Look& look) { return MoreDrawn(look, &IsLine); }, strings::kTutorialRectangleMissLine},
            },
    });
    // The eraser, and its two other ways. The first counts ink gone
    // however it went; the other two ask what was in hand as it went.
    chain.push_back(Step{
        .id = "erase",
        .kind = StepKind::Do,
        .title = strings::kTutorialEraseTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialEraseText); },
        .spot = Spot::SelectionBarEraser,
        .needs = With({DrawingOnSubject, SubjectDrawnOn}),
        .subject = SubjectRule::Any,
        .goal =
            [](const Look& look) {
                const SnippetFacts* now = nullptr;
                const SnippetFacts* then = nullptr;
                return SubjectBoth(look, now, then) && then->InkPx() - now->InkPx() >= kInkGone;
            },
    });
    chain.push_back(Step{
        .id = "eraseRect",
        .kind = StepKind::Do,
        .title = strings::kTutorialEraseRectTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialEraseRectText); },
        .spot = Spot::SelectionBarEraser,
        .needs = With({DrawingOnSubject, SubjectDrawnOn}),
        .subject = SubjectRule::Any,
        .goal = [](const Look& look) { return look.SubjectInkGone().rectangleEraser >= kInkGone; },
        .nearMisses =
            {
                {[](const Look& look) { return look.SubjectInkGone().eraser >= kInkGone; },
                 strings::kTutorialEraseRectMissRound},
            },
    });
    chain.push_back(Step{
        .id = "eraseRight",
        .kind = StepKind::Do,
        .title = strings::kTutorialEraseRightTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialEraseRightText); },
        .spot = Spot::SelectionBarPen,
        // No need for the pen: the eraser comes in hand from the step
        // before, and what it erases gets the near miss's line, which a
        // need's would hide.
        .needs = With({DrawingOnSubject, SubjectDrawnOn}),
        .subject = SubjectRule::Any,
        .goal = [](const Look& look) { return look.SubjectInkGone().otherTool >= kInkGone; },
        .nearMisses =
            {
                {[](const Look& look) {
                     const InkGone gone = look.SubjectInkGone();
                     return gone.eraser + gone.rectangleEraser >= kInkGone;
                 },
                 strings::kTutorialEraseRightMissEraser},
            },
    });
    // The note is on the snippet as it is typed, but the step waits for
    // the typing to end, and says so while it goes on: moved on at the
    // first letter, the next card would talk over the typing.
    chain.push_back(Step{
        .id = "note",
        .kind = StepKind::Do,
        .title = strings::kTutorialNoteTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialNoteText); },
        .spot = Spot::SelectionBarText,
        .needs = With({DrawingOnSubject}),
        .subject = SubjectRule::Any,
        .goal =
            [](const Look& look) {
                const SnippetFacts* now = nullptr;
                const SnippetFacts* then = nullptr;
                return SubjectBoth(look, now, then) && !now->note.empty() && now->note != then->note &&
                       look.world.NoteBeingTyped() != look.subject;
            },
        .nearMisses =
            {
                {[](const Look& look) {
                     return look.subject.has_value() && look.world.NoteBeingTyped() == look.subject;
                 },
                 strings::kTutorialNoteMissTyping},
            },
    });
    chain.push_back(Step{
        .id = "stopDrawing",
        .kind = StepKind::Do,
        .title = strings::kTutorialStopDrawingTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialStopDrawingText); },
        .goal = [](const Look& look) { return look.world.DrawingItems().empty(); },
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
        // Drawing mode or not: the bar has Pin in both.
        .needs = With({SubjectSelected}),
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
                       (PictureFaded(*now, *then) || (!now->strokes.empty() && DrawingFaded(*now, *then)));
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
                     return SubjectBoth(look, now, then) && now->strokes.empty() && DrawingFaded(*now, *then);
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
        .needs = With({SubjectPinned, SubjectSelected}),
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

// Any snippet made here since the step began that `is`.
template <typename Is>
bool MadeHereThat(const Look& look, Is is) {
    const std::vector<const SnippetFacts*> made = look.MadeHere();
    return std::any_of(made.begin(), made.end(), [&](const SnippetFacts* snippet) { return is(*snippet); });
}

std::vector<Step> MakeCapturing() {
    using enum Need;
    std::vector<Step> chain;
    // The canvas steps first: a screenshot of the whole screen covers the
    // canvas, and each hotkey goes to a canvas of its own (docs/TUTORIAL.md,
    // section 16.1).
    chain.push_back(Step{
        .id = "newDrawing",
        .kind = StepKind::Do,
        .title = strings::kTutorialNewDrawingTitle,
        .text =
            [](const World& world) {
                switch (world.DrawingTrigger()) {
                    case core::CreationTrigger::Plain:
                        break;
                    case core::CreationTrigger::Ctrl:
                    case core::CreationTrigger::Alt:
                        return Fixed(strings::kTutorialNewDrawingTextTrigger);
                    case core::CreationTrigger::Off:
                        return Fixed(world.KeyLabel(CommandId::NewDrawingTool) ? strings::kTutorialNewDrawingTextTool
                                                                               : strings::kTutorialNewDrawingTextMenu);
                }
                return Fixed(strings::kTutorialNewDrawingText);
            },
        // In drawing mode a press on empty canvas only leaves it.
        .needs = {InTutorialFolder, CanvasUncovered, NoDrawingMode},
        .goal =
            [](const Look& look) {
                return MadeHereThat(look, [](const SnippetFacts& made) { return !made.picture && !made.fullscreen; });
            },
        .nearMisses =
            {
                // Either kind: it covers the canvas, and has to go first.
                {[](const Look& look) {
                     return MadeHereThat(look, [](const SnippetFacts& made) { return made.fullscreen; });
                 },
                 strings::kTutorialNewDrawingMissFullscreen},
                {[](const Look& look) {
                     return MadeHereThat(look, [](const SnippetFacts& made) { return made.picture; });
                 },
                 strings::kTutorialNewDrawingMissScreenshot},
            },
    });
    chain.push_back(Step{
        .id = "fullscreen",
        .kind = StepKind::Do,
        .title = strings::kTutorialFullscreenTitle,
        .text =
            [](const World& world) {
                switch (world.ScreenshotTrigger()) {
                    case core::CreationTrigger::Plain:
                        break;
                    case core::CreationTrigger::Ctrl:
                    case core::CreationTrigger::Alt:
                        return Fixed(strings::kTutorialFullscreenTextTrigger);
                    case core::CreationTrigger::Off:
                        return Fixed(world.KeyLabel(CommandId::NewScreenshotTool) ? strings::kTutorialFullscreenTextTool
                                                                                  : strings::kTutorialFullscreenTextMenu);
                }
                return Fixed(strings::kTutorialFullscreenText);
            },
        // No drawing mode needed: a double-click, or a hold, leaves it and
        // makes the snippet.
        .needs = {InTutorialFolder, CanvasUncovered, NoOtherTool},
        .goal =
            [](const Look& look) {
                return MadeHereThat(look, [](const SnippetFacts& made) { return made.picture && made.fullscreen; });
            },
        // A drawing of the whole screen covers the canvas, and Delete does
        // nothing in drawing mode, which it comes in: out of that first,
        // then deleted.
        .nearMisses =
            {
                {[](const Look& look) {
                     return MadeHereThat(look, [&](const SnippetFacts& made) {
                         return !made.picture && made.fullscreen && look.world.IsDrawingOn(made.id);
                     });
                 },
                 strings::kTutorialFullscreenMissDrawingMode},
                {[](const Look& look) {
                     return MadeHereThat(look, [](const SnippetFacts& made) { return !made.picture && made.fullscreen; });
                 },
                 strings::kTutorialFullscreenMissDelete},
                {[](const Look& look) {
                     return MadeHereThat(look, [](const SnippetFacts& made) { return made.picture; });
                 },
                 strings::kTutorialFullscreenMissBox},
                {[](const Look& look) {
                     return MadeHereThat(look, [](const SnippetFacts& made) { return !made.picture; });
                 },
                 strings::kTutorialFullscreenMissDrawing},
            },
    });
    // The capture hotkeys, in one step: the quick capture, and the silent
    // one as a word beside it while it has a key. Either one's capture
    // does it, wherever it was pressed (section 16.2) - the step is about
    // capturing from a program, and the line offers both.
    chain.push_back(Step{
        .id = "quickCapture",
        .kind = StepKind::Do,
        .title = strings::kTutorialQuickCaptureTitle,
        .text =
            [](const World& world) {
                if (!world.KeyLabel(CommandId::QuickCapture)) {
                    return Fixed(strings::kTutorialQuickCaptureTextNoKey);
                }
                const bool silent = world.KeyLabel(CommandId::SilentCapture).has_value();
                if (world.KeyLabel(CommandId::ToggleEditMode)) {
                    return Fixed(silent ? strings::kTutorialQuickCaptureText
                                        : strings::kTutorialQuickCaptureTextNoSilent);
                }
                return Fixed(silent ? strings::kTutorialQuickCaptureTextTray
                                    : strings::kTutorialQuickCaptureTextTrayNoSilent);
            },
        // The capture lands in the current folder.
        .needs = {InTutorialFolder},
        .goal =
            [](const Look& look) {
                return look.world.Captures(core::HotkeySlot::QuickCapture) > look.start.quickCaptures ||
                       look.world.Captures(core::HotkeySlot::SilentCapture) > look.start.silentCaptures;
            },
    });
    chain.push_back(Step{
        .id = "end",
        .title = strings::kTutorialCapturingEndTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialCapturingEndText); },
    });
    return chain;
}

// Whether a canvas of the tutorial's that was there when the step began,
// live then, is `now` - in the trash, or in another folder.
bool ACanvasThatWas(const Look& look, bool (*now)(const Look& look, const CanvasFacts& then)) {
    return std::any_of(look.start.canvases.begin(), look.start.canvases.end(),
                       [&](const CanvasFacts& then) { return !then.deleted && now(look, then); });
}

// The folders and canvases of the tutorial's that were there when the
// step began, by id, as they were.
const FolderFacts* FolderAtStart(const Look& look, core::FolderId id) {
    for (const FolderFacts& facts : look.start.folders) {
        if (facts.id == id) {
            return &facts;
        }
    }
    return nullptr;
}
const CanvasFacts* CanvasAtStart(const Look& look, core::CanvasId id) {
    for (const CanvasFacts& facts : look.start.canvases) {
        if (facts.id == id) {
            return &facts;
        }
    }
    return nullptr;
}

// Whether `copy` could be a copy of `first`: the same kind, at the same
// size, with the same on it. Where either is says nothing, since a paste
// puts a copy at the pointer.
bool Twins(const SnippetFacts& first, const SnippetFacts& copy) {
    if (first.picture != copy.picture || first.rect.w != copy.rect.w || first.rect.h != copy.rect.h ||
        first.note != copy.note || first.strokes.size() != copy.strokes.size()) {
        return false;
    }
    for (size_t i = 0; i < first.strokes.size(); ++i) {
        const StrokeFacts& a = first.strokes[i];
        const StrokeFacts& b = copy.strokes[i];
        if (a.colorRGBA != b.colorRGBA || a.widthPx != b.widthPx || a.lengthPx != b.lengthPx || a.shape != b.shape) {
            return false;
        }
    }
    return true;
}

std::vector<Step> MakeFolders() {
    using enum Need;
    std::vector<Step> chain;
    // Waits: the next step moves a snippet to another canvas.
    chain.push_back(Step{
        .id = "newCanvas",
        .kind = StepKind::Do,
        .gated = true,
        .title = strings::kTutorialNewCanvasTitle,
        .text =
            [](const World& world) {
                if (world.CanvasBarOn()) {
                    return Fixed(strings::kTutorialNewCanvasText);
                }
                return Fixed(world.KeyLabel(CommandId::NewCanvas) ? strings::kTutorialNewCanvasTextKey
                                                                  : strings::kTutorialNewCanvasTextOverview);
            },
        .spot = Spot::CanvasBarNew,
        // Not the canvas uncovered: the Overview's New canvas does it too.
        .needs = {InTutorialFolder},
        .goal =
            [](const Look& look) {
                const core::CanvasId here = look.world.CurrentCanvas();
                return look.Tutorials(look.world.FolderOf(here)) && CanvasAtStart(look, here) == nullptr;
            },
    });
    // A move keeps the snippet's id (Session::Paste, SendItemsTo): one the
    // step has seen, now on another canvas. Moved while the step's
    // subject is cut and waiting, it is not on the canvas being looked at,
    // so there is no need for it there.
    chain.push_back(Step{
        .id = "moveSnippet",
        .kind = StepKind::Do,
        .title = strings::kTutorialMoveSnippetTitle,
        .text =
            [](const World& world) {
                const bool keys = world.KeyLabel(CommandId::Cut) && world.KeyLabel(CommandId::Paste);
                if (keys) {
                    return Fixed(world.CanvasBarOn() ? strings::kTutorialMoveSnippetText
                                                     : strings::kTutorialMoveSnippetTextNoBar);
                }
                return Fixed(world.CanvasBarOn() ? strings::kTutorialMoveSnippetTextMenu
                                                 : strings::kTutorialMoveSnippetTextMenuNoBar);
            },
        .spot = Spot::Subject,
        .needs = {InTutorialFolder, CanvasUncovered, ASubject},
        .subject = SubjectRule::Any,
        .goal =
            [](const Look& look) {
                return std::any_of(look.snippets.begin(), look.snippets.end(), [&](const SnippetFacts& now) {
                    const SnippetFacts* then = look.AtStart(now.id);
                    return !now.deleted && then != nullptr && then->canvas != now.canvas;
                });
            },
        // A copy pasted onto another canvas: made during the step, a twin
        // of one of the tutorial's elsewhere - what is on it, at its size.
        // Not where it is: Paste puts it at the pointer.
        .nearMisses =
            {
                {[](const Look& look) {
                     return std::any_of(look.snippets.begin(), look.snippets.end(), [&](const SnippetFacts& copy) {
                         return !copy.deleted && !look.start.present.contains(copy.id) &&
                                std::any_of(look.snippets.begin(), look.snippets.end(), [&](const SnippetFacts& first) {
                                    return !first.deleted && first.id != copy.id && first.canvas != copy.canvas &&
                                           Twins(first, copy);
                                });
                     });
                 },
                 strings::kTutorialMoveSnippetMissCopy},
            },
    });
    chain.push_back(Step{
        .id = "overview",
        .kind = StepKind::Do,
        .title = strings::kTutorialOverviewTitle,
        .text =
            [](const World& world) {
                return Fixed(world.CanvasBarOn() ? strings::kTutorialOverviewText : strings::kTutorialOverviewTextMenu);
            },
        .spot = Spot::CanvasBarOverview,
        .needs = {InTutorialFolder},
        .goal = [](const Look& look) { return look.world.CanvasCover() == Cover::Overview; },
    });
    // Waits: the next two are about the folder it makes, which is the
    // tutorial's from then on (section 17.3).
    chain.push_back(Step{
        .id = "newFolder",
        .kind = StepKind::Do,
        .gated = true,
        .keepsFolders = true,
        .title = strings::kTutorialNewFolderTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialNewFolderText); },
        .spot = Spot::NewFolder,
        .needs = {InTutorialFolder, OverviewUp, CanvasesTab},
        .goal =
            [](const Look& look) {
                return std::any_of(look.allFolders.begin(), look.allFolders.end(), [&](const FolderFacts& folder) {
                    return !folder.deleted && FolderAtStart(look, folder.id) == nullptr;
                });
            },
        .nearMisses =
            {
                {[](const Look& look) {
                     return std::any_of(look.canvases.begin(), look.canvases.end(), [&](const CanvasFacts& canvas) {
                         return !canvas.deleted && CanvasAtStart(look, canvas.id) == nullptr;
                     });
                 },
                 strings::kTutorialNewFolderMissCanvas},
            },
    });
    // Its folders are where it goes on, so no need for the tutorial's.
    chain.push_back(Step{
        .id = "rename",
        .kind = StepKind::Do,
        .title = strings::kTutorialRenameTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialRenameText); },
        .spot = Spot::MadeFolder,
        .needs = {OverviewUp, CanvasesTab},
        .goal =
            [](const Look& look) {
                const bool folder = std::any_of(look.folders.begin(), look.folders.end(), [&](core::FolderId id) {
                    const FolderFacts* now = look.FolderNow(id);
                    const FolderFacts* then = FolderAtStart(look, id);
                    return now != nullptr && then != nullptr && !now->deleted && now->name != then->name;
                });
                return folder ||
                       std::any_of(look.canvases.begin(), look.canvases.end(), [&](const CanvasFacts& now) {
                           const CanvasFacts* then = CanvasAtStart(look, now.id);
                           return then != nullptr && !now.deleted && now.name != then->name;
                       });
            },
    });
    // The one step that asks for the tutorial's own folder, not any of
    // its folders: the way back from the new one. Its canvases shown is
    // enough - a tile picked would close the Overview the next steps use,
    // so that is the topic's last step instead (openCanvas).
    chain.push_back(Step{
        .id = "switchFolder",
        .kind = StepKind::Do,
        .title = strings::kTutorialSwitchFolderTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialSwitchFolderText); },
        .spot = Spot::TutorialFolder,
        .needs = {OverviewUp, CanvasesTab},
        .goal = [](const Look& look) { return look.world.OverviewFolder() == look.folder; },
    });
    chain.push_back(Step{
        .id = "moveCanvas",
        .kind = StepKind::Do,
        .title = strings::kTutorialMoveCanvasTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialMoveCanvasText); },
        .spot = Spot::MadeFolder,
        .needs = {InTutorialFolder, OverviewUp, CanvasesTab},
        .goal =
            [](const Look& look) {
                return ACanvasThatWas(look, [](const Look& seen, const CanvasFacts& then) {
                    // Into one of the tutorial's folders, or out of them.
                    const CanvasFacts* now = seen.CanvasNow(then.id);
                    const core::FolderId folder =
                        now != nullptr ? (now->deleted ? 0 : now->folder) : seen.world.FolderOf(then.id);
                    return folder != 0 && folder != then.folder;
                });
            },
    });
    // The tutorial's own folder deleted takes the current canvas out of
    // its folders, so the trash's steps do not need them: restore brings
    // it back.
    chain.push_back(Step{
        .id = "deleteCanvas",
        .kind = StepKind::Do,
        .title = strings::kTutorialDeleteCanvasTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialDeleteCanvasText); },
        .spot = Spot::DeleteCanvas,
        .needs = {OverviewUp, CanvasesTab},
        .goal =
            [](const Look& look) {
                const bool canvas = ACanvasThatWas(look, [](const Look& seen, const CanvasFacts& then) {
                    const CanvasFacts* now = seen.CanvasNow(then.id);
                    return now != nullptr && now->deleted;
                });
                return canvas || std::any_of(look.folders.begin(), look.folders.end(), [&](core::FolderId id) {
                           const FolderFacts* now = look.FolderNow(id);
                           const FolderFacts* then = FolderAtStart(look, id);
                           return now != nullptr && then != nullptr && now->deleted && !then->deleted;
                       });
            },
    });
    chain.push_back(Step{
        .id = "showDeleted",
        .kind = StepKind::Do,
        .title = strings::kTutorialShowDeletedTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialShowDeletedText); },
        .spot = Spot::ShowDeleted,
        .needs = {OverviewUp, CanvasesTab},
        .goal = [](const Look& look) { return look.world.OverviewShowsDeleted(); },
    });
    chain.push_back(Step{
        .id = "restore",
        .kind = StepKind::Do,
        .title = strings::kTutorialRestoreTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialRestoreText); },
        .spot = Spot::Restore,
        .needs = {OverviewUp, CanvasesTab, DeletedShown, SomethingInTrash},
        .goal =
            [](const Look& look) {
                return std::any_of(look.trashedThisStep.begin(), look.trashedThisStep.end(), [&](uint64_t id) {
                    const CanvasFacts* canvas = look.CanvasNow(id);
                    const FolderFacts* folder = look.FolderNow(id);
                    return (canvas != nullptr && !canvas->deleted) || (folder != nullptr && !folder->deleted);
                });
            },
    });
    // Last, since a tile closes the Overview: on a canvas of the
    // tutorial's, with nothing over it. A close by Escape or the backdrop
    // on one counts as well - the tile's job is done either way - and the
    // goal comes before the needs, so the Overview gone needs nothing.
    chain.push_back(Step{
        .id = "openCanvas",
        .kind = StepKind::Do,
        .title = strings::kTutorialOpenCanvasTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialOpenCanvasText); },
        .needs = {OverviewUp, CanvasesTab},
        .goal =
            [](const Look& look) {
                return look.world.CanvasCover() == Cover::None &&
                       look.Tutorials(look.world.FolderOf(look.world.CurrentCanvas()));
            },
    });
    chain.push_back(Step{
        .id = "end",
        .title = strings::kTutorialFoldersEndTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialFoldersEndText); },
    });
    return chain;
}

// A profile there now that was not when the step began.
bool AProfileMade(const Look& look, bool (*wanted)(const ProfileFacts& facts)) {
    return std::any_of(look.profiles.begin(), look.profiles.end(), [&](const ProfileFacts& now) {
        const bool wasThere = std::any_of(look.start.profiles.begin(), look.start.profiles.end(),
                                          [&](const ProfileFacts& then) { return then.id == now.id; });
        return !wasThere && wanted(now);
    });
}

// The Behavior settings the tutorial's profile states, now and as the
// step began - none when it was not there.
size_t StatedNow(const Look& look) { return look.Profile() != nullptr ? look.Profile()->stated : 0; }
size_t StatedAtStart(const Look& look) { return look.ProfileAtStart() != nullptr ? look.ProfileAtStart()->stated : 0; }

bool OnSection(const World& world, SettingsSection section) {
    return world.OverviewShowsSettings() && world.SettingsSectionShown() == section;
}

std::vector<Step> MakeProfiles() {
    using enum Need;
    std::vector<Step> chain;
    // A profile made in the second before the card moves on is the
    // tutorial's too (section 18.8).
    chain.push_back(Step{
        .id = "openProfiles",
        .kind = StepKind::Do,
        .keepsProfiles = true,
        .title = strings::kTutorialOpenProfilesTitle,
        .text =
            [](const World& world) {
                return Fixed(world.CanvasCover() == Cover::Overview && !world.OverviewShowsSettings()
                                 ? strings::kTutorialOpenProfilesTextOverview
                                 : strings::kTutorialOpenProfilesText);
            },
        .spot = Spot::SectionProfiles,
        .goal = [](const Look& look) { return OnSection(look.world, SettingsSection::Profiles); },
    });
    // Waits: every later step is about the profile it makes, which is the
    // tutorial's from then on (section 18.3).
    chain.push_back(Step{
        .id = "makeProfile",
        .kind = StepKind::Do,
        .gated = true,
        .keepsProfiles = true,
        .title = strings::kTutorialMakeProfileTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialMakeProfileText); },
        .spot = Spot::MakeProfile,
        .needs = {SettingsUp, SettingsTab, ProfilesSection, AProgramUnderneath},
        .goal = [](const Look& look) { return look.Profile() != nullptr && look.Profile()->matchesUnderneath; },
        .nearMisses =
            {
                {[](const Look& look) {
                     return AProfileMade(look, [](const ProfileFacts& facts) { return facts.program.empty(); });
                 },
                 strings::kTutorialMakeProfileMissBlank},
                // Not a mistake: a profile of the user's comes first, and a
                // second one made for practice will not run (question 44).
                {[](const Look& look) {
                     return std::any_of(look.profiles.begin(), look.profiles.end(),
                                        [](const ProfileFacts& facts) { return facts.matchesUnderneath; });
                 },
                 strings::kTutorialMakeProfileMissTaken},
            },
    });
    chain.push_back(Step{
        .id = "behavior",
        .kind = StepKind::Do,
        .title = strings::kTutorialBehaviorTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialBehaviorText); },
        .spot = Spot::SectionBehavior,
        .needs = {SettingsUp, SettingsTab, TutorialsProfile},
        .goal = [](const Look& look) { return OnSection(look.world, SettingsSection::Behavior); },
    });
    // Waits: revert hands back what it set. Any Behavior row counts,
    // stated in the profile - its mark, not its value.
    chain.push_back(Step{
        .id = "change",
        .kind = StepKind::Do,
        .gated = true,
        .title = strings::kTutorialChangeTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialChangeText); },
        .spot = Spot::DontStealFocus,
        .needs = {SettingsUp, SettingsTab, BehaviorSection, TutorialsProfile, ShowingIt},
        .goal = [](const Look& look) { return StatedNow(look) > StatedAtStart(look); },
    });
    chain.push_back(Step{
        .id = "revert",
        .kind = StepKind::Do,
        .title = strings::kTutorialRevertTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialRevertText); },
        .spot = Spot::Revert,
        .needs = {SettingsUp, SettingsTab, BehaviorSection, TutorialsProfile, ShowingIt, SomethingSetInIt},
        .goal = [](const Look& look) { return StatedNow(look) < StatedAtStart(look); },
        .nearMisses =
            {
                {[](const Look& look) { return look.Profile() != nullptr && look.Profile()->statedAsDefaults > 0; },
                 strings::kTutorialRevertMissTickedBack},
            },
    });
    chain.push_back(Step{
        .id = "end",
        .title = strings::kTutorialProfilesEndTitle,
        .text = [](const World&) { return Fixed(strings::kTutorialProfilesEndText); },
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

const std::vector<Step>& CapturingChain() {
    static const std::vector<Step> chain = MakeCapturing();
    return chain;
}

const std::vector<Step>& FoldersChain() {
    static const std::vector<Step> chain = MakeFolders();
    return chain;
}

const std::vector<Step>& ProfilesChain() {
    static const std::vector<Step> chain = MakeProfiles();
    return chain;
}

}  // namespace sz::ui::tutorial
