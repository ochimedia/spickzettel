#include "core/session/history.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/persistence/library_store.h"
#include "core/session/session.h"
#include "support/failing_writes.h"
#include "support/removed_at_end.h"
#include "support/session_test_access.h"

namespace sz::core {
namespace {

using test::Model;

// ===== The rules, driven at random =====
//
// A session taken through random commands - gestures left open and ended
// by whatever comes next, undos and redos, snippets pasted, cut and sent
// between canvases, canvases and folders deleted, restored and deleted for
// good - and checked all the way: every undo change on the canvas of the
// snippet it is about, every step on the current canvas applying when it
// is reached, and a run of undos and the same run of redos giving back
// exactly the library they started from. What history::History promises,
// asked of every state a hand could leave it in.

// A canvas as the checks compare it: everything but the textures, which
// there are none of without a window.
struct CanvasState {
    CanvasId id = 0;
    std::string name;
    FolderId folderId = 0;
    int64_t deletedAt = 0;
    std::vector<Item> items;
    bool operator==(const CanvasState&) const = default;
};

struct LibraryState {
    std::vector<Folder> folders;
    std::vector<CanvasState> canvases;
    CanvasId current = 0;
    bool operator==(const LibraryState&) const = default;
};

// Where two states first differ, said so that a failure can be read.
std::string Difference(const LibraryState& a, const LibraryState& b) {
    if (a.folders != b.folders) {
        return "the folders";
    }
    if (a.current != b.current) {
        return "the current canvas";
    }
    if (a.canvases.size() != b.canvases.size()) {
        return "the number of canvases";
    }
    for (size_t c = 0; c < a.canvases.size(); ++c) {
        const CanvasState& x = a.canvases[c];
        const CanvasState& y = b.canvases[c];
        const std::string where = "canvas " + std::to_string(c) + ": ";
        if (x.id != y.id || x.name != y.name || x.folderId != y.folderId || x.deletedAt != y.deletedAt) {
            return where + "its own fields";
        }
        if (x.items.size() != y.items.size()) {
            return where + std::to_string(x.items.size()) + " snippets against " + std::to_string(y.items.size());
        }
        for (size_t i = 0; i < x.items.size(); ++i) {
            const Item& p = x.items[i];
            const Item& q = y.items[i];
            const std::string item = where + "snippet " + std::to_string(i) + " ";
            if (p.id != q.id) {
                return item + "is another one";
            }
            if (p.strokes != q.strokes) {
                return item + "strokes " + std::to_string(p.strokes.size()) + " against " +
                       std::to_string(q.strokes.size());
            }
            if (p.rect != q.rect || p.anchorRect != q.anchorRect || p.isFullscreen != q.isFullscreen) {
                return item + "placement";
            }
            if (ItemStyle::Of(p) != ItemStyle::Of(q)) {
                return item + "style";
            }
            if (p.noteText != q.noteText) {
                return item + "text '" + p.noteText + "' against '" + q.noteText + "'";
            }
            if (p.deletedAt != q.deletedAt) {
                return item + "deletion mark";
            }
            if (!(p == q)) {
                return item + "something else";
            }
        }
    }
    return "nothing";
}

LibraryState StateOf(const std::vector<Folder>& folders, const std::vector<Canvas>& canvases, CanvasId current) {
    LibraryState state;
    state.folders = folders;
    for (const Canvas& canvas : canvases) {
        state.canvases.push_back(CanvasState{canvas.id, canvas.name, canvas.folderId, canvas.deletedAt, canvas.items});
    }
    state.current = current;
    return state;
}

LibraryState StateOf(const Session& session) {
    return StateOf(session.Manager().Folders(), session.Manager().Canvases(), session.Manager().CurrentCanvasId());
}

class RandomSession {
public:
    explicit RandomSession(uint32_t seed) : random_(seed) {
        session_.SyncItemsToDisplaySize(1000.0f, 800.0f);
        session_.AddCanvas("B");
        session_.AddCanvas("C");
    }

    Session& Get() { return session_; }

    // Writes every command from here on to `store`, starting with the
    // library as it is.
    void Attach(persistence::LibraryStore& store) {
        session_.SetLibraryStore(&store);
        ASSERT_TRUE(session_.WriteWholeLibrary());
    }
    void Detach() { session_.SetLibraryStore(nullptr); }

    // One command, or one part of a gesture, chosen at random.
    void Step() {
        switch (Pick(26)) {
            case 0:
            case 1:
                Create();
                break;
            case 2:
            case 3:
                Stroke();
                break;
            case 4:
                Erase();
                break;
            case 5:
                if (const ItemId item = AnyHere()) {
                    session_.EraseRect(item, 0.0f, 0.0f, Coord(), Coord());
                }
                break;
            case 6:
                if (const ItemId item = AnyHere()) {
                    session_.ClearDrawing(item);
                }
                break;
            case 7:
                Text();
                break;
            case 8:
                Placement();
                break;
            case 9:
                Style();
                break;
            case 10:
                if (const ItemId item = AnyHere()) {
                    if (Pick(2) == 0) {
                        session_.ToggleFullscreen(item, Pick(2) == 0);
                    } else {
                        session_.ResetItemToNativeSize(item);
                    }
                }
                break;
            case 11:
                session_.DeleteItems(SomeHere());
                break;
            case 12:
            case 13:
                session_.Paste(SomeAnywhere(), /*cut=*/Pick(3) != 0);
                break;
            case 14:
                session_.Duplicate(SomeHere());
                break;
            case 15:
                if (const CanvasId to = AnyLiveCanvas()) {
                    session_.SendItemsTo(SomeHere(), to, /*copy=*/Pick(3) == 0);
                }
                break;
            case 16:
            case 17:
            case 18:
                session_.Undo();
                break;
            case 19:
            case 20:
                session_.Redo();
                break;
            case 21:
                if (const CanvasId to = AnyLiveCanvas()) {
                    session_.SwitchToCanvas(to);
                }
                break;
            case 22:
                Canvases();
                break;
            case 23:
                Shown();
                break;
            case 24:
                Shape();
                break;
            case 25:
                ForGood();
                break;
        }
        // A canvas made only when there is none left to be on. No canvas
        // current while others are live - what deleting a folder's last
        // canvas leaves - is a state of its own, which the file has to
        // keep as well; a canvas made at once hid that it did not.
        if (!session_.Manager().HasCurrentCanvas() && !AnyLive()) {
            session_.SwitchToCanvas(session_.AddCanvas("Fresh"));
        }
    }

    // Ends whatever gesture is open, the way the next command would, so a
    // check starts from a state where nothing more gets filed on its own.
    void EndGestures() {
        session_.EndTextEdit();
        session_.EndPlacement();
        session_.EndStyleEdit();
        session_.EndErase();
        if (session_.IsDrawingShape()) {
            session_.EndShape(Coord(), Coord());
        }
    }

private:
    size_t Pick(size_t n) { return std::uniform_int_distribution<size_t>(0, n - 1)(random_); }
    float Coord() { return std::uniform_real_distribution<float>(0.0f, 400.0f)(random_); }

    std::vector<ItemId> Here() const {
        std::vector<ItemId> ids;
        if (const Canvas* canvas = session_.Manager().CurrentOrNull()) {
            for (const Item& item : canvas->items) {
                if (!session_.Manager().IsDeleted(*canvas, item)) {
                    ids.push_back(item.id);
                }
            }
        }
        return ids;
    }
    ItemId AnyHere() {
        const std::vector<ItemId> ids = Here();
        return ids.empty() ? 0 : ids[Pick(ids.size())];
    }
    std::vector<ItemId> SomeOf(std::vector<ItemId> ids) {
        std::shuffle(ids.begin(), ids.end(), random_);
        ids.resize(std::min(ids.size(), 1 + Pick(3)));
        return ids;
    }
    std::vector<ItemId> SomeHere() { return SomeOf(Here()); }
    std::vector<ItemId> SomeAnywhere() {
        std::vector<ItemId> ids;
        for (const Canvas& canvas : session_.Manager().Canvases()) {
            for (const Item& item : canvas.items) {
                ids.push_back(item.id);
            }
        }
        return SomeOf(ids);
    }
    bool AnyLive() const {
        const CanvasManager& manager = session_.Manager();
        return std::any_of(manager.Canvases().begin(), manager.Canvases().end(),
                           [&manager](const Canvas& canvas) { return !manager.IsDeleted(canvas); });
    }
    CanvasId AnyLiveCanvas() {
        std::vector<CanvasId> ids;
        for (const Canvas& canvas : session_.Manager().Canvases()) {
            if (!session_.Manager().IsDeleted(canvas)) {
                ids.push_back(canvas.id);
            }
        }
        return ids.empty() ? 0 : ids[Pick(ids.size())];
    }

    void Create() {
        const Rect rect{Coord(), Coord(), 60.0f + Coord() * 0.5f, 60.0f + Coord() * 0.5f};
        session_.CreateItem(false, rect, "Snippet");
    }
    void Stroke() {
        const ItemId item = AnyHere();
        if (item == 0) {
            return;
        }
        const Rect rect = session_.Manager().FindItemAnywhere(item)->rect;
        session_.LiveLayer().BeginStroke(StrokePoint{rect.x + 5.0f, rect.y + 5.0f}, 0xFF0000FFu, 4.0f);
        session_.LiveLayer().ExtendStroke(StrokePoint{rect.x + rect.w * 0.5f, rect.y + rect.h * 0.7f});
        session_.LiveLayer().ExtendStroke(StrokePoint{rect.x + rect.w - 5.0f, rect.y + 5.0f});
        session_.LiveLayer().EndStroke();
        session_.CommitLiveStroke(item);
    }
    void Erase() {
        const ItemId item = AnyHere();
        if (item == 0) {
            return;
        }
        const Rect rect = session_.Manager().FindItemAnywhere(item)->rect;
        const auto at = [&](float fx, float fy) { return std::make_pair(rect.x + rect.w * fx, rect.y + rect.h * fy); };
        const auto [x0, y0] = at(0.5f, 0.1f);
        session_.BeginErase(item, x0, y0, 30.0f);
        for (float t = 0.2f; t < 1.0f; t += 0.2f) {
            const auto [x, y] = at(0.5f, t);
            session_.ExtendErase(x, y, 30.0f);
        }
        if (Pick(4) != 0) {
            session_.EndErase();
        }
    }
    void Text() {
        const ItemId item = AnyHere();
        if (item == 0) {
            return;
        }
        static const char* const kTexts[] = {"", "a", "hello", "two words"};
        session_.BeginTextEdit(item);
        session_.PreviewText(kTexts[Pick(4)]);
        if (Pick(3) != 0) {
            session_.EndTextEdit(Pick(2) == 0 ? std::optional<std::string>(kTexts[Pick(4)]) : std::nullopt);
        }
    }
    void Placement() {
        const std::vector<ItemId> ids = SomeHere();
        if (ids.empty()) {
            return;
        }
        if (Pick(2) == 0) {
            std::vector<std::pair<ItemId, Rect>> rects;
            for (const ItemId id : ids) {
                rects.emplace_back(id, Rect{Coord(), Coord(), 50.0f + Coord() * 0.3f, 50.0f + Coord() * 0.3f});
            }
            session_.SetRects(rects);
            return;
        }
        session_.BeginPlacement(ids);
        session_.PreviewLeaveFullscreen(ids.front());
        for (const ItemId id : ids) {
            session_.PreviewRect(id, Rect{Coord(), Coord(), 80.0f, 80.0f});
        }
        if (Pick(3) != 0) {
            session_.EndPlacement();
        }
    }
    void Style() {
        const std::vector<ItemId> ids = SomeHere();
        if (ids.empty()) {
            return;
        }
        ItemStyle style = ItemStyle::Of(*session_.Manager().FindItemAnywhere(ids.front()));
        style.foregroundOpacity = 0.1f + static_cast<float>(Pick(10)) * 0.1f;
        style.pictureTintRGBA = Pick(2) == 0 ? 0xFFFFFFFFu : 0x336699FFu;
        if (Pick(2) == 0) {
            std::vector<std::pair<ItemId, ItemStyle>> styles;
            for (const ItemId id : ids) {
                styles.emplace_back(id, style);
            }
            session_.SetStyles(styles);
            return;
        }
        session_.PreviewStyle(ids.front(), style);
        if (Pick(3) != 0) {
            session_.EndStyleEdit();
        }
    }
    void Shape() {
        const ItemId item = AnyHere();
        if (item == 0) {
            return;
        }
        session_.BeginShape(item, Pick(2) == 0 ? Session::Shape::Line : Session::Shape::Rectangle, Coord(), Coord(),
                            0x00FF00FFu, 3.0f);
        session_.UpdateShape(Coord(), Coord());
        if (Pick(3) != 0) {
            session_.EndShape(Coord(), Coord());
        }
    }
    void Canvases() {
        const CanvasManager& manager = session_.Manager();
        switch (Pick(6)) {
            case 0:
                if (manager.Canvases().size() < 6) {
                    session_.AddCanvas("More");
                }
                break;
            case 1:
                // The canvas on screen as often as any other: what a person
                // deletes most, and what leaves a folder with none on it.
                if (const CanvasId canvas = Pick(2) == 0 ? manager.CurrentCanvasId() : AnyLiveCanvas()) {
                    session_.Delete(canvas);
                }
                break;
            case 2:
                for (const Canvas& canvas : manager.Canvases()) {
                    if (canvas.deletedAt != 0) {
                        session_.Restore(canvas.id);
                        break;
                    }
                }
                break;
            case 3:
                if (manager.Folders().size() < 3) {
                    session_.AddFolder("Folder");
                }
                break;
            case 4:
                if (const CanvasId canvas = AnyLiveCanvas()) {
                    const std::vector<Folder>& folders = manager.Folders();
                    session_.MoveCanvasToFolder(canvas, folders[Pick(folders.size())].id);
                }
                break;
            case 5: {
                const std::vector<Folder>& folders = manager.Folders();
                const FolderId folder = folders[Pick(folders.size())].id;
                if (manager.FindFolder(folder)->deletedAt == 0) {
                    session_.Delete(folder);
                } else {
                    session_.Restore(folder);
                }
                break;
            }
        }
    }
    void Shown() {
        const std::vector<ItemId> ids = SomeHere();
        switch (Pick(4)) {
            case 0:
                session_.SetPinned(ids, Pick(2) == 0);
                break;
            case 1:
                session_.SetMinimized(ids, Pick(2) == 0);
                break;
            case 2:
                session_.BringItemsToFront(ids);
                break;
            case 3:
                if (!ids.empty()) {
                    session_.MoveItemLayer(ids.front(), Pick(2) == 0 ? 1 : -1);
                }
                break;
        }
    }
    void ForGood() {
        const CanvasManager& manager = session_.Manager();
        switch (Pick(3)) {
            case 0: {
                const std::vector<ItemId> ids = SomeAnywhere();
                if (!ids.empty()) {
                    session_.DeletePermanently(ids.front());
                }
                break;
            }
            case 1:
                for (const Canvas& canvas : manager.Canvases()) {
                    if (canvas.deletedAt != 0) {
                        session_.DeletePermanently(canvas.id);
                        break;
                    }
                }
                break;
            case 2:
                if (const ItemId item = AnyHere()) {
                    session_.DiscardIfUntouched(item);
                }
                break;
        }
    }

    Session session_;
    std::mt19937 random_;
};

// Every undo change is on the stack of the canvas holding its snippet -
// the rule the others follow from: an undo never reaches a canvas that is
// not on screen.
void ExpectEveryUndoChangeIsOnItsSnippetsCanvas(const Session& session) {
    for (const CanvasId canvas : session.History().Canvases()) {
        const std::deque<history::Step>* undo = session.History().UndoStack(canvas);
        for (const history::Step& step : *undo) {
            for (const history::Change& change : step.changes) {
                ASSERT_EQ(session.Manager().CanvasHoldingItem(change.item), std::optional<CanvasId>(canvas))
                    << "a change about a snippet on another canvas";
            }
        }
    }
}

// Every step the current canvas can undo is undone - none refused - and
// the same number redone gives back exactly the library there was; then
// the same the other way round, with what it can redo.
void ExpectTheHistoryRoundTrips(Session& session) {
    const LibraryState before = StateOf(session);
    size_t undone = 0;
    while (session.CanUndo()) {
        ASSERT_TRUE(session.Undo().has_value()) << "refused after " << undone;
        ++undone;
        ASSERT_LE(undone, 2 * history::History::kStackCap);
    }
    for (size_t i = 0; i < undone; ++i) {
        ASSERT_TRUE(session.CanRedo());
        ASSERT_TRUE(session.Redo().has_value()) << "redo refused after " << i;
    }
    const LibraryState afterRoundTrip = StateOf(session);
    ASSERT_TRUE(afterRoundTrip == before) << "undone and redone " << undone << ": " << Difference(afterRoundTrip, before);

    size_t redone = 0;
    while (session.CanRedo()) {
        ASSERT_TRUE(session.Redo().has_value()) << "refused after " << redone;
        ++redone;
        ASSERT_LE(redone, 2 * history::History::kStackCap);
    }
    for (size_t i = 0; i < redone; ++i) {
        ASSERT_TRUE(session.Undo().has_value()) << "undo refused after " << i;
    }
    const LibraryState afterRedoAndUndo = StateOf(session);
    ASSERT_TRUE(afterRedoAndUndo == before) << "redone and undone " << redone << ": " << Difference(afterRedoAndUndo, before);
}

TEST(HistoryTest, EveryStepAppliesWhateverHappensInBetween) {
    for (uint32_t seed = 1; seed <= 120; ++seed) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        RandomSession random(seed);
        for (int op = 0; op < 250; ++op) {
            random.Step();
            ASSERT_NO_FATAL_FAILURE(ExpectEveryUndoChangeIsOnItsSnippetsCanvas(random.Get())) << "op " << op;
            if (op % 9 == 8) {
                random.EndGestures();
                ASSERT_NO_FATAL_FAILURE(ExpectTheHistoryRoundTrips(random.Get())) << "op " << op;
            }
        }
    }
}

// ===== The file holds what the model holds =====
//
// The same random session, written to a library as it goes, with some of
// its writes made to fail (see FailingWrites): every command is written as it is made, and one
// whose write fails is not made - so at every moment but the middle of a
// gesture, the file holds exactly what the model holds, and the history's
// rules still hold of what was made.

void ExpectTheFileHoldsTheModel(const Session& session, const std::filesystem::path& file) {
    std::optional<CanvasManagerSnapshot> disk = persistence::LibraryStore(file).Load();
    ASSERT_TRUE(disk.has_value());
    const LibraryState onDisk = StateOf(disk->folders, disk->canvases, disk->currentCanvasId);
    const LibraryState inMemory = StateOf(session);
    ASSERT_TRUE(onDisk == inMemory) << "the file differs in " << Difference(onDisk, inMemory);
}

TEST(HistoryTest, TheFileHoldsWhatTheModelHoldsWhateverFailsToBeWritten) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "spickzettel_history_test_written";
    for (uint32_t seed = 1; seed <= 6; ++seed) {
        SCOPED_TRACE("seed " + std::to_string(seed));
        std::filesystem::remove_all(dir);
        const test::RemovedAtEnd cleanup(dir);
        const std::filesystem::path file = dir / "library.db";
        std::optional<persistence::LibraryStore> store(file);
        ASSERT_EQ(store->Open(), persistence::LibraryStore::OpenResult::Opened);
        RandomSession random(seed);
        ASSERT_NO_FATAL_FAILURE(random.Attach(*store));
        test::FailingWrites failures(file);
        std::mt19937 fail(seed * 7919u);
        size_t failed = 0;
        for (int op = 0; op < 120; ++op) {
            const bool failing = std::uniform_int_distribution<int>(0, 5)(fail) == 0;
            failures.Set(failing);
            random.Step();
            failures.Set(false);
            failed += failing && random.Get().LastWriteFailed() ? 1 : 0;
            ASSERT_NO_FATAL_FAILURE(ExpectEveryUndoChangeIsOnItsSnippetsCanvas(random.Get())) << "op " << op;
            if (op % 5 == 4) {
                random.EndGestures();
                ASSERT_NO_FATAL_FAILURE(ExpectTheFileHoldsTheModel(random.Get(), file)) << "op " << op;
            }
            if (op % 25 == 24) {
                ASSERT_NO_FATAL_FAILURE(ExpectTheHistoryRoundTrips(random.Get())) << "op " << op;
                ASSERT_NO_FATAL_FAILURE(ExpectTheFileHoldsTheModel(random.Get(), file)) << "op " << op;
            }
        }
        EXPECT_GT(failed, 0u) << "no write failed: the test tested nothing of that";
        random.Detach();
        store.reset();
    }
}

// ===== The stacks themselves =====

history::Step StrokeStep(ItemId item) {
    return history::Step{0, history::What::Stroke, {history::Change{item, history::StrokeAdded{}}}};
}

TEST(HistoryTest, AStackKeepsItsNewestFiftySteps) {
    history::History history;
    for (ItemId item = 1; item <= 60; ++item) {
        history.Record(7, StrokeStep(item));
    }
    ASSERT_EQ(history.UndoStack(7)->size(), history::History::kStackCap);
    EXPECT_EQ(history.UndoStack(7)->front().changes.front().item, 11u);
    EXPECT_EQ(history.UndoStack(7)->back().changes.front().item, 60u);
}

// A step split between two canvases rejoins itself when its parts meet
// again, in its own place among the steps there.
TEST(HistoryTest, APartOfAStepMigratedBackRejoinsIt) {
    history::History history;
    history.Record(1, StrokeStep(100));
    history.Record(1, history::Step{0, history::What::Placement,
                                    {history::Change{10, history::PlacementChanged{}},
                                     history::Change{11, history::PlacementChanged{}}}});
    history.Record(1, StrokeStep(101));

    history.Migrate(10, 2);
    ASSERT_EQ(history.UndoStack(1)->size(), 3u);
    EXPECT_EQ((*history.UndoStack(1))[1].changes.size(), 1u) << "11's part stays";
    ASSERT_EQ(history.UndoStack(2)->size(), 1u);

    history.Migrate(10, 1);
    EXPECT_EQ(history.UndoStack(2), nullptr);
    ASSERT_EQ(history.UndoStack(1)->size(), 3u) << "rejoined, not beside it";
    EXPECT_EQ((*history.UndoStack(1))[1].changes.size(), 2u);
}

// A new change to a snippet ends its redo future on every canvas, and the
// new step's own canvas's redo stack goes whole, as ever.
TEST(HistoryTest, ANewChangeEndsItsSnippetsRedoEverywhere) {
    history::History history;
    history.Record(1, StrokeStep(10));
    history.Record(1, StrokeStep(11));
    history.Undone(1, history.TakeUndo(1));  // 11
    history.Undone(1, history.TakeUndo(1));  // 10
    ASSERT_EQ(history.RedoStack(1)->size(), 2u);

    history.Record(2, StrokeStep(10));
    ASSERT_EQ(history.RedoStack(1)->size(), 1u) << "10's redo is gone, 11's is not";
    EXPECT_EQ(history.RedoStack(1)->back().changes.front().item, 11u);
}

// Undone on one canvas, a change to a snippet leaves its redo on any
// other canvas behind: that future was for a state it has just left.
TEST(HistoryTest, AnUndoEndsItsSnippetsRedoOnOtherCanvases) {
    history::History history;
    history.Record(1, StrokeStep(10));
    history.Undone(1, history.TakeUndo(1));
    history.Record(2, StrokeStep(20));
    history.Record(2, StrokeStep(10));  // ends 10's redo on 1 anyway
    ASSERT_FALSE(history.CanRedo(1));

    history.Record(1, StrokeStep(30));
    history.Undone(1, history.TakeUndo(1));
    ASSERT_TRUE(history.CanRedo(1));
    // 30 moves to canvas 2 by some undo there, which undoes a change of it.
    history.Record(2, StrokeStep(31));
    history.Undone(2, history::Step{0, history::What::Stroke, {history::Change{30, history::StrokeAdded{}}}});
    EXPECT_FALSE(history.CanRedo(1));
}

// A canvas deleted for good takes its stacks, and every move from or to it
// anywhere else, with it.
TEST(HistoryTest, ACanvasDeletedForGoodTakesEveryMoveFromOrToIt) {
    history::History history;
    history.Record(2, history::Step{0, history::What::Paste,
                                    {history::Change{10, history::Moved{1, 0, 2}},
                                     history::Change{11, history::Moved{3, 0, 2}}}});
    history.Record(1, StrokeStep(20));
    history.ForgetCanvas(1);
    EXPECT_EQ(history.UndoStack(1), nullptr);
    ASSERT_EQ(history.UndoStack(2)->size(), 1u);
    ASSERT_EQ(history.UndoStack(2)->back().changes.size(), 1u);
    EXPECT_EQ(history.UndoStack(2)->back().changes.front().item, 11u);
}

}  // namespace
}  // namespace sz::core
