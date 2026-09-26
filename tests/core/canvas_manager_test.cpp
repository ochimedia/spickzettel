#include "core/canvas/canvas_manager.h"

#include <algorithm>

#include <gtest/gtest.h>

#include "core/canvas/item_geometry.h"  // kItemMinWidth/kItemMinHeight, for the display-sync floor tests

namespace sz::core {
namespace {

TEST(CanvasManagerTest, StartsWithOneCanvasCurrent) {
    CanvasManager manager;
    ASSERT_EQ(manager.Canvases().size(), 1u);
    EXPECT_EQ(manager.CurrentOrNull()->id, manager.Canvases().front().id);
    EXPECT_TRUE(manager.CurrentOrNull()->items.empty());
}

TEST(CanvasManagerTest, AddCanvasAppendsButDoesNotSwitch) {
    CanvasManager manager("First");
    const CanvasId originalId = manager.CurrentOrNull()->id;

    const CanvasId secondId = manager.AddCanvas("Second");

    ASSERT_EQ(manager.Canvases().size(), 2u);
    EXPECT_EQ(manager.Canvases()[0].id, originalId);
    EXPECT_EQ(manager.Canvases()[1].id, secondId);
    EXPECT_EQ(manager.Canvases()[1].name, "Second");
    EXPECT_EQ(manager.CurrentOrNull()->id, originalId);  // still on the original canvas
}

// The end of its *own folder*, which is not the end of the list: another
// folder's canvases may well sit after it, and they stay where they are.
TEST(CanvasManagerTest, AddCanvasLandsAtTheEndOfItsOwnFolder) {
    CanvasManager manager("A1");
    const FolderId folderA = manager.CurrentFolderId();
    const CanvasId a1 = manager.CurrentOrNull()->id;
    manager.AddFolder("Folder B");
    const CanvasId b1 = manager.AddCanvas("B1");  // folder B is current now
    manager.SwitchToFolder(folderA);

    const CanvasId a2 = manager.AddCanvas("A2");

    ASSERT_EQ(manager.Canvases().size(), 3u);
    EXPECT_EQ(manager.Canvases()[0].id, a1);
    EXPECT_EQ(manager.Canvases()[1].id, a2);
    EXPECT_EQ(manager.Canvases()[2].id, b1);
}

// "2026-09-07 22:36:14" - the shape of a name nobody chose, checkable
// without freezing a clock.
bool IsATimestampName(const std::string& name) {
    return name.size() == 19 && name[4] == '-' && name[7] == '-' && name[10] == ' ' && name[13] == ':' &&
           name[16] == ':';
}

// Every folder and canvas the manager mints on its own is named for the
// moment, the same as one the user makes and doesn't name: the one a
// fresh library starts with, and the folder minted for a canvas when
// every folder has been deleted. "Folder 1" said nothing.
TEST(CanvasManagerTest, WhatTheManagerMintsItselfIsNamedForTheMoment) {
    CanvasManager fresh;
    ASSERT_EQ(fresh.Folders().size(), 1u);
    EXPECT_TRUE(IsATimestampName(fresh.Folders().front().name)) << fresh.Folders().front().name;
    ASSERT_TRUE(fresh.CurrentOrNull() != nullptr);
    EXPECT_TRUE(IsATimestampName(fresh.CurrentOrNull()->name)) << fresh.CurrentOrNull()->name;

    // A test may still hand the first canvas a name of its own.
    CanvasManager named("Named");
    EXPECT_EQ(named.CurrentOrNull()->name, "Named");

    // Every folder gone, then a canvas added: the folder it needs is minted
    // and named the same way.
    CanvasManager emptied;
    emptied.ImportSnapshot(CanvasManagerSnapshot{});
    ASSERT_TRUE(emptied.Folders().empty());
    emptied.AddCanvas("Comeback");
    ASSERT_EQ(emptied.Folders().size(), 1u);
    EXPECT_TRUE(IsATimestampName(emptied.Folders().front().name)) << emptied.Folders().front().name;
}

TEST(CanvasManagerTest, DuplicateNamesGetDistinctIds) {
    CanvasManager manager("Same Name");
    const CanvasId second = manager.AddCanvas("Same Name");

    const auto it = std::find_if(manager.Canvases().begin(), manager.Canvases().end(),
                                  [second](const Canvas& c) { return c.id == second; });
    ASSERT_NE(it, manager.Canvases().end());
    // The id is what tells two same-named canvases apart.
    EXPECT_NE(manager.CurrentOrNull()->id, it->id);
    EXPECT_EQ(manager.CurrentOrNull()->name, it->name);
}

TEST(CanvasManagerTest, SwitchToCanvasChangesCurrent) {
    CanvasManager manager;
    const CanvasId second = manager.AddCanvas("Second");

    manager.SwitchToCanvas(second);

    EXPECT_EQ(manager.CurrentOrNull()->id, second);
}

TEST(CanvasManagerTest, SwitchToUnknownCanvasIsNoOp) {
    CanvasManager manager;
    const CanvasId original = manager.CurrentOrNull()->id;

    manager.SwitchToCanvas(9999);

    EXPECT_EQ(manager.CurrentOrNull()->id, original);
}

TEST(CanvasManagerTest, PinnedItemsCountOnTheCurrentCanvasOnlyWhileTheyAreShown) {
    CanvasManager manager;
    const CanvasId first = manager.CurrentOrNull()->id;
    EXPECT_FALSE(manager.CurrentCanvasHasPinnedItems());
    const ItemId id = manager.CreateItem(false, Rect{0, 0, 100, 100}, "Note");
    EXPECT_FALSE(manager.CurrentCanvasHasPinnedItems()) << "not pinned yet";

    manager.FindItemAnywhere(id)->pinned = true;
    EXPECT_TRUE(manager.CurrentCanvasHasPinnedItems());

    // Minimized is drawn nowhere but the dock, which the pinned view doesn't have.
    manager.FindItemAnywhere(id)->minimized = true;
    EXPECT_FALSE(manager.CurrentCanvasHasPinnedItems());
    manager.FindItemAnywhere(id)->minimized = false;

    // Another canvas's pins are not this one's.
    manager.SwitchToCanvas(manager.AddCanvas("Second"));
    EXPECT_FALSE(manager.CurrentCanvasHasPinnedItems());
    manager.SwitchToCanvas(first);
    EXPECT_TRUE(manager.CurrentCanvasHasPinnedItems());

    manager.MarkDeleted(id, 1);
    EXPECT_FALSE(manager.CurrentCanvasHasPinnedItems());
}

TEST(CanvasManagerTest, DeleteCanvasErasesItForGood) {
    CanvasManager manager("First");
    const CanvasId second = manager.AddCanvas("Second");

    manager.DeleteCanvas(second);

    ASSERT_EQ(manager.Canvases().size(), 1u);
    EXPECT_TRUE(std::none_of(manager.Canvases().begin(), manager.Canvases().end(),
                              [second](const Canvas& c) { return c.id == second; }));
}

// An empty folder is a legal state the moment one is created, so refusing
// to let the library reach the same state would be an inconsistency rather
// than a safeguard. See CanvasManager's class comment.
TEST(CanvasManagerTest, DeletingTheLastRemainingCanvasLeavesNone) {
    CanvasManager manager;
    const CanvasId onlyId = manager.CurrentOrNull()->id;

    manager.DeleteCanvas(onlyId);

    EXPECT_TRUE(manager.Canvases().empty());
    EXPECT_EQ(manager.CurrentOrNull(), nullptr);
    EXPECT_FALSE(manager.HasCurrentCanvas());
    EXPECT_EQ(manager.CurrentCanvasId(), 0u);
    // The folder it lived in is untouched - deleting its last canvas
    // empties a folder, it doesn't remove it.
    EXPECT_EQ(manager.Folders().size(), 1u);
}

TEST(CanvasManagerTest, AddingACanvasBackAfterDeletingThemAllMakesItCurrent) {
    CanvasManager manager;
    manager.DeleteCanvas(manager.CurrentOrNull()->id);
    ASSERT_FALSE(manager.HasCurrentCanvas());

    const CanvasId revived = manager.AddCanvas("Back again");

    ASSERT_TRUE(manager.HasCurrentCanvas());
    EXPECT_EQ(manager.CurrentOrNull()->id, revived);
    EXPECT_EQ(manager.CurrentOrNull()->name, "Back again");
}

// Every item operation has to survive being called with no canvas at all -
// none of these can reach a Canvas to work on, and all of them say so by
// doing nothing rather than by dereferencing one that isn't there (the
// old reference-returning Current() made exactly this undefined).
TEST(CanvasManagerTest, ItemOperationsAreNoOpsWithNoCanvasAtAll) {
    CanvasManager manager;
    manager.DeleteCanvas(manager.CurrentOrNull()->id);
    ASSERT_FALSE(manager.HasCurrentCanvas());

    EXPECT_EQ(manager.CreateItem(false, Rect{0, 0, 100, 100}, "D1"), 0u);
    manager.DeleteItem(1);
    manager.MoveItemLayer(1, 1);
    manager.BringItemsToFront({1});
    EXPECT_EQ(manager.DuplicateItem(1), 0u);
    manager.EraseAt(1, 10.0f, 10.0f, 8.0f);
    manager.EraseRectAt(1, 0.0f, 0.0f, 50.0f, 50.0f);
    EXPECT_EQ(manager.FindItemAnywhere(1), nullptr);
}

TEST(CanvasManagerTest, DeletingTheCurrentCanvasLeavesAnotherOneCurrent) {
    CanvasManager manager("First");
    const CanvasId current = manager.CurrentOrNull()->id;
    const CanvasId second = manager.AddCanvas("Second");

    manager.DeleteCanvas(current);

    EXPECT_EQ(manager.CurrentOrNull()->id, second);
    EXPECT_TRUE(std::none_of(manager.Canvases().begin(), manager.Canvases().end(),
                              [current](const Canvas& c) { return c.id == current; }));
}

TEST(CanvasManagerTest, ReorderCanvasMovesToNewIndex) {
    CanvasManager manager("First");
    const CanvasId first = manager.CurrentOrNull()->id;
    const CanvasId second = manager.AddCanvas("Second");  // order: [First, Second]
    const CanvasId third = manager.AddCanvas("Third");    // order: [First, Second, Third]

    manager.ReorderCanvas(third, 0);  // move "Third" to the front

    ASSERT_EQ(manager.Canvases().size(), 3u);
    EXPECT_EQ(manager.Canvases()[0].id, third);
    EXPECT_EQ(manager.Canvases()[1].id, first);
    EXPECT_EQ(manager.Canvases()[2].id, second);
}

TEST(CanvasManagerTest, ReorderCanvasClampsOutOfRangeIndex) {
    CanvasManager manager("First");
    const CanvasId first = manager.CurrentOrNull()->id;
    const CanvasId second = manager.AddCanvas("Second");  // order: [First, Second]

    manager.ReorderCanvas(first, 9999);  // past the end, so clamped to the last slot

    ASSERT_EQ(manager.Canvases().size(), 2u);
    EXPECT_EQ(manager.Canvases()[0].id, second);
    EXPECT_EQ(manager.Canvases()[1].id, first);
}

TEST(CanvasManagerTest, ReorderUnknownCanvasIsNoOp) {
    CanvasManager manager("First");
    const CanvasId first = manager.CurrentOrNull()->id;

    manager.ReorderCanvas(9999, 0);

    ASSERT_EQ(manager.Canvases().size(), 1u);  // First
    EXPECT_EQ(manager.Canvases()[0].id, first);
}

TEST(CanvasManagerTest, StartsWithOneFolderCurrent) {
    CanvasManager manager;
    ASSERT_EQ(manager.Folders().size(), 1u);
    EXPECT_EQ(manager.CurrentFolderId(), manager.Folders().front().id);
    EXPECT_EQ(manager.CurrentOrNull()->folderId, manager.CurrentFolderId());
}

TEST(CanvasManagerTest, AddCanvasTargetsCurrentFolder) {
    CanvasManager manager;
    const FolderId folder = manager.CurrentFolderId();

    const CanvasId id = manager.AddCanvas("Second");

    const auto it = std::find_if(manager.Canvases().begin(), manager.Canvases().end(),
                                  [id](const Canvas& c) { return c.id == id; });
    ASSERT_NE(it, manager.Canvases().end());
    EXPECT_EQ(it->folderId, folder);
}

TEST(CanvasManagerTest, AddFolderCreatesEmptyFolderAndSwitchesBrowseFolder) {
    CanvasManager manager("First");
    const CanvasId originalCanvas = manager.CurrentOrNull()->id;

    const FolderId newFolder = manager.AddFolder("Game B");

    ASSERT_EQ(manager.Folders().size(), 2u);
    EXPECT_EQ(manager.Folders().back().id, newFolder);  // newest last - see AddFolder
    EXPECT_EQ(manager.Folders().back().name, "Game B");
    EXPECT_EQ(manager.CurrentFolderId(), newFolder);  // browse state follows the new folder
    EXPECT_EQ(manager.CurrentOrNull()->id, originalCanvas);  // edit state is untouched

    // New folders start empty - they don't need a starter canvas.
    const size_t canvasesInNewFolder =
        std::count_if(manager.Canvases().begin(), manager.Canvases().end(),
                       [newFolder](const Canvas& c) { return c.folderId == newFolder; });
    EXPECT_EQ(canvasesInNewFolder, 0u);
}

TEST(CanvasManagerTest, SwitchToFolderOnlyChangesBrowseState) {
    CanvasManager manager;
    const CanvasId originalCanvas = manager.CurrentOrNull()->id;
    const FolderId folderB = manager.AddFolder("Folder B");
    manager.SwitchToFolder(manager.Folders().front().id);  // back to the original folder

    EXPECT_EQ(manager.CurrentFolderId(), manager.Folders().front().id);
    EXPECT_NE(manager.CurrentFolderId(), folderB);
    EXPECT_EQ(manager.CurrentOrNull()->id, originalCanvas);  // never moved by browsing
}

TEST(CanvasManagerTest, SwitchToUnknownFolderIsNoOp) {
    CanvasManager manager;
    const FolderId original = manager.CurrentFolderId();

    manager.SwitchToFolder(9999);

    EXPECT_EQ(manager.CurrentFolderId(), original);
}

TEST(CanvasManagerTest, SwitchToCanvasResyncsBrowseFolder) {
    CanvasManager manager;
    const FolderId folderA = manager.CurrentFolderId();
    const CanvasId canvasA = manager.CurrentOrNull()->id;
    manager.AddFolder("Folder B");
    const CanvasId canvasB = manager.AddCanvas("B1");  // lands in folder B (now current)
    manager.SwitchToCanvas(canvasB);  // now actually editing folder B's canvas

    manager.SwitchToCanvas(canvasA);

    EXPECT_EQ(manager.CurrentOrNull()->id, canvasA);
    EXPECT_EQ(manager.CurrentFolderId(), folderA);
}

TEST(CanvasManagerTest, DeletingAFoldersLastCanvasLeavesItEmpty) {
    CanvasManager manager("First");
    const CanvasId onlyInFolderA = manager.CurrentOrNull()->id;
    const FolderId folderA = manager.CurrentFolderId();
    manager.AddFolder("Folder B");
    manager.AddCanvas("B1");  // folder B has a canvas, so deleting folder A's isn't the global-last-canvas case

    manager.DeleteCanvas(onlyInFolderA);

    const size_t canvasesInFolderA = std::count_if(manager.Canvases().begin(), manager.Canvases().end(),
                                                     [folderA](const Canvas& c) { return c.folderId == folderA; });
    EXPECT_EQ(canvasesInFolderA, 0u);  // folders may now be empty
}

// Stays put with nothing current rather than dropping into another folder
// entirely; the app layer makes a canvas in that same folder - see
// DeleteCanvas's own doc comment.
TEST(CanvasManagerTest, DeletingAFoldersOnlyCurrentCanvasLeavesNoneCurrentAndStaysInThatFolder) {
    CanvasManager manager("First");
    const FolderId folderA = manager.CurrentFolderId();
    const CanvasId onlyInFolderA = manager.CurrentOrNull()->id;
    manager.AddFolder("Folder B");
    manager.AddCanvas("B1");
    manager.SwitchToFolder(folderA);  // browsing folder A again; B1 (folder B) still isn't current

    manager.DeleteCanvas(onlyInFolderA);  // folder A's only (and current) canvas disappears

    EXPECT_FALSE(manager.HasCurrentCanvas());
    EXPECT_EQ(manager.CurrentFolderId(), folderA);  // still the folder that was being worked in
    const size_t canvasesInFolderA = std::count_if(manager.Canvases().begin(), manager.Canvases().end(),
                                                     [folderA](const Canvas& c) { return c.folderId == folderA; });
    EXPECT_EQ(canvasesInFolderA, 0u);
}

TEST(CanvasManagerTest, DeletingTheCurrentCanvasSwitchesToTheOneBeforeItInItsFolder) {
    CanvasManager manager("First");
    const CanvasId first = manager.CurrentOrNull()->id;
    const CanvasId second = manager.AddCanvas("Second");
    const CanvasId third = manager.AddCanvas("Third");
    manager.AddFolder("Folder B");  // unrelated other folder
    manager.SwitchToCanvas(third);

    manager.DeleteCanvas(third);

    EXPECT_EQ(manager.CurrentOrNull()->id, second);
    EXPECT_EQ(manager.CurrentOrNull()->name, "Second");
    EXPECT_NE(manager.CurrentOrNull()->id, first);
}

// Nothing before the first one, so the folder's new first takes over -
// which is the canvas that was after it.
TEST(CanvasManagerTest, DeletingTheFirstCanvasInAFolderSwitchesToItsNewFirst) {
    CanvasManager manager("First");
    const CanvasId first = manager.CurrentOrNull()->id;
    const CanvasId second = manager.AddCanvas("Second");
    manager.AddCanvas("Third");

    manager.DeleteCanvas(first);

    EXPECT_EQ(manager.CurrentOrNull()->id, second);
}

TEST(CanvasManagerTest, ReorderCanvasIsScopedWithinFolder) {
    CanvasManager manager("First");
    const CanvasId first = manager.CurrentOrNull()->id;
    const CanvasId second = manager.AddCanvas("Second");  // folder A order: [First, Second]
    const FolderId folderB = manager.AddFolder("Folder B");
    const CanvasId inFolderB = manager.AddCanvas("B1");  // folder B's own canvas, at the end of the list

    manager.ReorderCanvas(first, 1);  // move "First" to the end of folder A's own subsequence

    ASSERT_EQ(manager.Canvases().size(), 3u);
    EXPECT_EQ(manager.Canvases()[0].id, second);
    EXPECT_EQ(manager.Canvases()[1].id, first);
    EXPECT_EQ(manager.Canvases()[2].id, inFolderB);  // folder B's canvas untouched at the end
    EXPECT_EQ(manager.Canvases()[2].folderId, folderB);
}

TEST(CanvasManagerTest, DeleteFolderErasesItsCanvasesForGood) {
    CanvasManager manager("First");
    manager.AddCanvas("Second");  // still folder A (2 canvases in A)
    const FolderId folderB = manager.AddFolder("Folder B");
    const CanvasId b1 = manager.AddCanvas("B1");  // folder B now has 1 canvas

    manager.DeleteFolder(folderB);

    ASSERT_EQ(manager.Folders().size(), 1u);
    // B1 is gone for good - deleting a folder deletes its canvases too
    // (see the class comment).
    ASSERT_EQ(manager.Canvases().size(), 2u);  // First, Second
    EXPECT_TRUE(std::none_of(manager.Canvases().begin(), manager.Canvases().end(),
                              [b1](const Canvas& c) { return c.id == b1; }));
}

// The same "nothing at all is a legal state" rule as
// DeletingTheLastRemainingCanvasLeavesNone above, for folders.
TEST(CanvasManagerTest, DeletingTheFolderHoldingEveryCanvasLeavesNoCanvases) {
    CanvasManager manager("First");
    const FolderId folderA = manager.CurrentFolderId();
    const FolderId folderB = manager.AddFolder("Folder B");  // empty - every canvas is still in folder A

    manager.DeleteFolder(folderA);

    EXPECT_TRUE(manager.Canvases().empty());
    EXPECT_FALSE(manager.HasCurrentCanvas());
    ASSERT_EQ(manager.Folders().size(), 1u);
    EXPECT_EQ(manager.Folders().front().id, folderB);
    // folderA was the browsed folder as well as the current canvas's, and
    // there was no canvas left to re-home the browse pointer onto - so it
    // has to have moved to the one folder that still exists.
    EXPECT_EQ(manager.CurrentFolderId(), folderB);
}

TEST(CanvasManagerTest, DeletingTheLastRemainingFolderLeavesNothingAtAll) {
    CanvasManager manager;
    const FolderId onlyFolder = manager.CurrentFolderId();

    manager.DeleteFolder(onlyFolder);

    EXPECT_TRUE(manager.Folders().empty());
    EXPECT_TRUE(manager.Canvases().empty());
    EXPECT_EQ(manager.CurrentFolderId(), 0u);
    EXPECT_EQ(manager.CurrentCanvasId(), 0u);
    EXPECT_FALSE(manager.HasCurrentCanvas());
}

// The one thing AddCanvas can't do is hand back a canvas whose folderId
// names a folder that isn't there - nothing downstream (the Overview's
// folder-grouped list least of all) is prepared for that - so it mints one.
TEST(CanvasManagerTest, AddingACanvasWithEveryFolderDeletedCreatesAFolderForIt) {
    CanvasManager manager;
    manager.DeleteFolder(manager.CurrentFolderId());
    ASSERT_TRUE(manager.Folders().empty());

    const CanvasId revived = manager.AddCanvas("Fresh start");

    ASSERT_EQ(manager.Folders().size(), 1u);
    const FolderId newFolder = manager.Folders().front().id;
    EXPECT_EQ(manager.CurrentFolderId(), newFolder);
    ASSERT_TRUE(manager.HasCurrentCanvas());
    EXPECT_EQ(manager.CurrentOrNull()->id, revived);
    EXPECT_EQ(manager.CurrentOrNull()->folderId, newFolder);
}

TEST(CanvasManagerTest, DeletingCurrentFolderSwitchesCanvas) {
    CanvasManager manager("First");
    const CanvasId canvasA = manager.CurrentOrNull()->id;
    manager.CreateItem(false, Rect{0, 0, 100, 100}, "D1");
    const FolderId folderB = manager.AddFolder("Folder B");  // empty, now browsed

    manager.DeleteFolder(folderB);  // deleting the merely-browsed (and empty) folder

    EXPECT_EQ(manager.CurrentOrNull()->id, canvasA);

    // Now delete the folder actually holding the current canvas - give
    // another folder a canvas of its own first, so the app doesn't end up
    // with zero canvases (which would make the delete a no-op).
    const FolderId folderA = manager.Folders().back().id;
    const FolderId folderC = manager.AddFolder("Folder C");
    const CanvasId inFolderC = manager.AddCanvas("C1");

    manager.DeleteFolder(folderA);

    EXPECT_EQ(manager.CurrentOrNull()->id, inFolderC);
    EXPECT_EQ(manager.CurrentFolderId(), folderC);
}

TEST(CanvasManagerTest, DeletingBrowsedFolderSwitchesBrowseStateToNewFront) {
    CanvasManager manager;
    const FolderId folderB = manager.AddFolder("Folder B");

    manager.DeleteFolder(folderB);

    EXPECT_EQ(manager.CurrentFolderId(), manager.Folders().front().id);
    EXPECT_NE(manager.CurrentFolderId(), folderB);
}

TEST(CanvasManagerTest, DeleteUnknownFolderIsNoOp) {
    CanvasManager manager;
    manager.DeleteFolder(9999);
    EXPECT_EQ(manager.Folders().size(), 1u);
}

// Newest last, like a canvas within its folder - so the sidebar reads in
// the order the folders were made, and a new one appears next to the
// button that made it.
TEST(CanvasManagerTest, AddFolderAppendsToTheEndOfTheList) {
    CanvasManager manager("First");
    const FolderId first = manager.CurrentFolderId();

    const FolderId second = manager.AddFolder("B");
    const FolderId third = manager.AddFolder("C");

    ASSERT_EQ(manager.Folders().size(), 3u);
    EXPECT_EQ(manager.Folders()[0].id, first);
    EXPECT_EQ(manager.Folders()[1].id, second);
    EXPECT_EQ(manager.Folders()[2].id, third);
}

TEST(CanvasManagerTest, ReorderFolderMovesToNewIndex) {
    CanvasManager manager("First");
    const FolderId folderA = manager.CurrentFolderId();
    const FolderId folderB = manager.AddFolder("B");  // order: [A, B]
    const FolderId folderC = manager.AddFolder("C");  // order: [A, B, C]

    manager.ReorderFolder(folderC, 0);

    ASSERT_EQ(manager.Folders().size(), 3u);
    EXPECT_EQ(manager.Folders()[0].id, folderC);
    EXPECT_EQ(manager.Folders()[1].id, folderA);
    EXPECT_EQ(manager.Folders()[2].id, folderB);
}

TEST(CanvasManagerTest, ReorderFolderClampsOutOfRangeIndex) {
    CanvasManager manager("First");
    const FolderId folderA = manager.CurrentFolderId();
    const FolderId folderB = manager.AddFolder("B");

    manager.ReorderFolder(folderA, 9999);

    ASSERT_EQ(manager.Folders().size(), 2u);
    EXPECT_EQ(manager.Folders()[0].id, folderB);
    EXPECT_EQ(manager.Folders()[1].id, folderA);
}

TEST(CanvasManagerTest, MoveCanvasToFolderReassignsFolderIdEvenIfItEmptiesTheSourceFolder) {
    CanvasManager manager("First");
    const CanvasId toMove = manager.CurrentOrNull()->id;
    const FolderId folderA = manager.CurrentFolderId();
    const FolderId folderB = manager.AddFolder("Folder B");

    manager.MoveCanvasToFolder(toMove, folderB);  // "First" is folder A's only canvas

    const auto it = std::find_if(manager.Canvases().begin(), manager.Canvases().end(),
                                  [toMove](const Canvas& c) { return c.id == toMove; });
    ASSERT_NE(it, manager.Canvases().end());
    EXPECT_EQ(it->folderId, folderB);
    const size_t canvasesLeftInFolderA = std::count_if(manager.Canvases().begin(), manager.Canvases().end(),
                                                         [folderA](const Canvas& c) { return c.folderId == folderA; });
    EXPECT_EQ(canvasesLeftInFolderA, 0u);  // folders may now be empty
}

TEST(CanvasManagerTest, MoveCanvasToFolderIsNoOpForUnknownCanvasOrFolder) {
    CanvasManager manager("First");
    manager.AddCanvas("Second");
    const FolderId folderB = manager.AddFolder("Folder B");
    const CanvasId canvasInA = manager.Canvases().back().id;

    manager.MoveCanvasToFolder(9999, folderB);
    manager.MoveCanvasToFolder(canvasInA, 9999);

    const auto it = std::find_if(manager.Canvases().begin(), manager.Canvases().end(),
                                  [canvasInA](const Canvas& c) { return c.id == canvasInA; });
    ASSERT_NE(it, manager.Canvases().end());
    EXPECT_NE(it->folderId, folderB);
}

TEST(CanvasManagerTest, RenameFolderChangesName) {
    CanvasManager manager;
    const FolderId folder = manager.CurrentFolderId();

    manager.RenameFolder(folder, "Renamed");

    EXPECT_EQ(manager.Folders().front().name, "Renamed");
}

TEST(CanvasManagerTest, RenameFolderIsNoOpForEmptyNameOrUnknownId) {
    CanvasManager manager;
    const FolderId folder = manager.CurrentFolderId();
    manager.RenameFolder(folder, "Kept");

    manager.RenameFolder(folder, "");
    manager.RenameFolder(9999, "Nope");

    EXPECT_EQ(manager.Folders().front().name, "Kept");
}

TEST(CanvasManagerTest, RenameCanvasChangesName) {
    CanvasManager manager;
    const CanvasId canvas = manager.CurrentOrNull()->id;

    manager.RenameCanvas(canvas, "Renamed");

    EXPECT_EQ(manager.CurrentOrNull()->name, "Renamed");
}

TEST(CanvasManagerTest, RenameCanvasIsNoOpForEmptyNameOrUnknownId) {
    CanvasManager manager;
    const CanvasId canvas = manager.CurrentOrNull()->id;
    manager.RenameCanvas(canvas, "Kept");

    manager.RenameCanvas(canvas, "");
    manager.RenameCanvas(9999, "Nope");

    EXPECT_EQ(manager.CurrentOrNull()->name, "Kept");
}

TEST(CanvasManagerTest, CreateItemAppendsToCurrentCanvas) {
    CanvasManager manager;
    const ItemId id = manager.CreateItem(false, Rect{10, 20, 300, 200}, "My Drawing");

    ASSERT_EQ(manager.CurrentOrNull()->items.size(), 1u);
    const Item& item = manager.CurrentOrNull()->items.front();
    EXPECT_EQ(item.id, id);
    EXPECT_FALSE(item.hasBackground);
    EXPECT_EQ(item.name, "My Drawing");
    EXPECT_EQ(item.rect, (Rect{10, 20, 300, 200}));
    // nativeW/nativeH are fixed at creation from the initial rect.
    EXPECT_FLOAT_EQ(item.nativeW, 300.0f);
    EXPECT_FLOAT_EQ(item.nativeH, 200.0f);
    EXPECT_FLOAT_EQ(item.foregroundOpacity, 1.0f);
    // A plain Drawing starts with no visible background at all.
    EXPECT_FLOAT_EQ(item.picture.opacity, 0.0f);
    EXPECT_EQ(item.picture.tintColorRGBA, 0xffffffffu);
    EXPECT_FALSE(item.isFullscreen);
}

TEST(CanvasManagerTest, CreateItemWithBackgroundDefaultsToFullyOpaque) {
    CanvasManager manager;
    const ItemId id = manager.CreateItem(true, Rect{0, 0, 1920, 1080}, "My Screenshot");

    const Item& item = manager.CurrentOrNull()->items.front();
    EXPECT_EQ(item.id, id);
    EXPECT_TRUE(item.hasBackground);
    // A Screenshot starts fully opaque, so a fresh capture is immediately
    // visible without the user having to raise a slider first.
    EXPECT_FLOAT_EQ(item.picture.opacity, 1.0f);
    EXPECT_EQ(item.picture.tintColorRGBA, 0xffffffffu);
}

// A Screenshot's picture stands in with the placeholder gradient until its
// capture loads; a Drawing's has nothing to stand in for.
TEST(CanvasManagerTest, OnlyAScreenshotsPictureStandsInWithThePlaceholder) {
    CanvasManager manager;
    manager.CreateItem(true, Rect{0, 0, 100, 100}, "Shot");
    EXPECT_TRUE(manager.CurrentOrNull()->items.front().picture.showsPlaceholder);

    manager.CreateItem(false, Rect{0, 0, 100, 100}, "Drawing");
    EXPECT_FALSE(manager.CurrentOrNull()->items.back().picture.showsPlaceholder);
}

TEST(CanvasManagerTest, DeleteItemRemovesIt) {
    CanvasManager manager;
    const ItemId a = manager.CreateItem(false, Rect{}, "A");
    const ItemId b = manager.CreateItem(false, Rect{}, "B");

    manager.DeleteItem(a);

    ASSERT_EQ(manager.CurrentOrNull()->items.size(), 1u);
    EXPECT_EQ(manager.CurrentOrNull()->items.front().id, b);
}

// A step forward lands in front of the snippet that was covering it, and
// a step back behind the one it was covering - the whole point of the two
// buttons being to change what can be seen.
TEST(CanvasManagerTest, MoveItemLayerPassesTheSnippetItOverlaps) {
    CanvasManager manager;
    const ItemId bottom = manager.CreateItem(false, Rect{0, 0, 200, 200}, "Bottom");
    const ItemId top = manager.CreateItem(false, Rect{100, 100, 200, 200}, "Top");
    ASSERT_EQ(manager.CurrentOrNull()->items[0].id, bottom);

    EXPECT_TRUE(manager.CanMoveItemLayer(bottom, 1));
    manager.MoveItemLayer(bottom, 1);

    EXPECT_EQ(manager.CurrentOrNull()->items[0].id, top);
    EXPECT_EQ(manager.CurrentOrNull()->items[1].id, bottom);

    manager.MoveItemLayer(bottom, -1);

    EXPECT_EQ(manager.CurrentOrNull()->items[0].id, bottom);
    EXPECT_EQ(manager.CurrentOrNull()->items[1].id, top);
}

// The one this is really for: a snippet in between that this one is
// nowhere near is stepped straight over, in one press, because passing it
// on its own would have looked like the button doing nothing.
TEST(CanvasManagerTest, MoveItemLayerSkipsWhateverItDoesNotTouch) {
    CanvasManager manager;
    const ItemId bottom = manager.CreateItem(false, Rect{0, 0, 200, 200}, "Bottom");
    const ItemId elsewhere = manager.CreateItem(false, Rect{800, 600, 200, 200}, "Elsewhere");
    const ItemId covering = manager.CreateItem(false, Rect{100, 100, 200, 200}, "Covering");

    manager.MoveItemLayer(bottom, 1);

    const std::vector<Item>& items = manager.CurrentOrNull()->items;
    EXPECT_EQ(items[0].id, elsewhere);
    EXPECT_EQ(items[1].id, covering);
    EXPECT_EQ(items[2].id, bottom) << "past the one covering it, not merely past the one above it";
}

// Nothing overlapping in that direction means nothing to do, and the
// buttons say so before they are pressed rather than after.
TEST(CanvasManagerTest, MoveItemLayerDoesNothingWithNothingToPass) {
    CanvasManager manager;
    const ItemId alone = manager.CreateItem(false, Rect{0, 0, 200, 200}, "Alone");
    const ItemId elsewhere = manager.CreateItem(false, Rect{800, 600, 200, 200}, "Elsewhere");

    EXPECT_FALSE(manager.CanMoveItemLayer(alone, 1));
    EXPECT_FALSE(manager.CanMoveItemLayer(elsewhere, -1));
    manager.MoveItemLayer(alone, 1);
    manager.MoveItemLayer(elsewhere, -1);

    EXPECT_EQ(manager.CurrentOrNull()->items[0].id, alone);
    EXPECT_EQ(manager.CurrentOrNull()->items[1].id, elsewhere);
}

// A snippet that isn't on screen covers nothing, so it is neither passed
// nor counted - a step over it would be the invisible kind again.
TEST(CanvasManagerTest, MoveItemLayerIgnoresWhatIsNotOnScreen) {
    CanvasManager manager;
    const ItemId bottom = manager.CreateItem(false, Rect{0, 0, 200, 200}, "Bottom");
    const ItemId minimized = manager.CreateItem(false, Rect{100, 100, 200, 200}, "Minimized");
    const ItemId deleted = manager.CreateItem(false, Rect{100, 100, 200, 200}, "Deleted");
    manager.FindItemAnywhere(minimized)->minimized = true;
    // Marked deleted where it stands, which is what a delete is - see
    // CanvasManager's class comment; DeleteItem erases outright instead.
    manager.FindItemAnywhere(deleted)->deletedAt = 1;
    ASSERT_FALSE(manager.CanMoveItemLayer(bottom, 1));

    const ItemId covering = manager.CreateItem(false, Rect{120, 120, 200, 200}, "Covering");
    manager.MoveItemLayer(bottom, 1);

    const std::vector<Item>& items = manager.CurrentOrNull()->items;
    EXPECT_EQ(items[2].id, covering);
    EXPECT_EQ(items[3].id, bottom);
}

// The general form both the picker's move and Duplicate are built on: it
// finds the snippet wherever it lives, not only on the canvas being looked
// at, which is what a paste needs - the snippet may have been copied on a
// canvas nobody is looking at any more.
TEST(CanvasManagerTest, PlaceItemOnCanvasFindsTheSnippetOnAnyCanvas) {
    CanvasManager manager("First");
    const CanvasId first = manager.CurrentOrNull()->id;
    const ItemId item = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const CanvasId second = manager.AddCanvas("Second");
    manager.SwitchToCanvas(second);
    ASSERT_EQ(manager.CurrentCanvasId(), second);
    EXPECT_EQ(manager.CanvasHoldingItem(item), first);

    // A copy of a snippet on a canvas that is not current, onto the one
    // that is: a new snippet, the original left where it was.
    const ItemId copy = manager.PlaceItemOnCanvas(item, second, /*copy=*/true);
    ASSERT_NE(copy, 0u);
    EXPECT_NE(copy, item);
    EXPECT_EQ(manager.FindCanvas(first)->items.size(), 1u);
    EXPECT_EQ(manager.FindCanvas(second)->items.size(), 1u);

    // ...and a move of the original, which keeps its id and leaves the
    // canvas it was on empty.
    EXPECT_EQ(manager.PlaceItemOnCanvas(item, second, /*copy=*/false), item);
    EXPECT_TRUE(manager.FindCanvas(first)->items.empty());
    EXPECT_EQ(manager.CanvasHoldingItem(item), second);
    EXPECT_FALSE(manager.CanvasHoldingItem(9999).has_value());
    // Onto the canvas it is already on, a move moves nothing.
    EXPECT_EQ(manager.PlaceItemOnCanvas(item, second, /*copy=*/false), item);
    EXPECT_EQ(manager.FindCanvas(second)->items.size(), 2u);
}

TEST(CanvasManagerTest, BringItemsToFrontMovesOneToTopPreservingOtherOrder) {
    CanvasManager manager;
    const ItemId a = manager.CreateItem(false, Rect{}, "A");
    const ItemId b = manager.CreateItem(false, Rect{}, "B");
    const ItemId c = manager.CreateItem(false, Rect{}, "C");
    ASSERT_EQ(manager.CurrentOrNull()->items[0].id, a);
    ASSERT_EQ(manager.CurrentOrNull()->items[1].id, b);
    ASSERT_EQ(manager.CurrentOrNull()->items[2].id, c);

    manager.BringItemsToFront({a});

    EXPECT_EQ(manager.CurrentOrNull()->items[0].id, b);
    EXPECT_EQ(manager.CurrentOrNull()->items[1].id, c);
    EXPECT_EQ(manager.CurrentOrNull()->items[2].id, a);
}

TEST(CanvasManagerTest, BringItemsToFrontRaisesThemAsABlockInTheirOwnOrder) {
    CanvasManager manager;
    const ItemId a = manager.CreateItem(false, Rect{}, "A");
    const ItemId b = manager.CreateItem(false, Rect{}, "B");
    const ItemId c = manager.CreateItem(false, Rect{}, "C");
    const ItemId d = manager.CreateItem(false, Rect{}, "D");
    const auto order = [&manager] {
        std::vector<ItemId> ids;
        for (const Item& item : manager.CurrentOrNull()->items) {
            ids.push_back(item.id);
        }
        return ids;
    };

    // In whatever order they are named: their own order is the stack's.
    manager.BringItemsToFront({c, a});
    EXPECT_EQ(order(), (std::vector<ItemId>{b, d, a, c}));

    const uint64_t before = manager.Generation();
    manager.BringItemsToFront({a, c});
    manager.BringItemsToFront({});
    manager.BringItemsToFront({424242});
    EXPECT_EQ(order(), (std::vector<ItemId>{b, d, a, c}));
    EXPECT_EQ(manager.Generation(), before) << "on top already, or nothing of theirs here: no change";
}

TEST(CanvasManagerTest, BringItemsToFrontOnAlreadyTopItemIsNoOp) {
    CanvasManager manager;
    const ItemId a = manager.CreateItem(false, Rect{}, "A");
    const ItemId b = manager.CreateItem(false, Rect{}, "B");
    const uint64_t before = manager.Generation();

    manager.BringItemsToFront({b});  // b is already at the top

    EXPECT_EQ(manager.CurrentOrNull()->items[0].id, a);
    EXPECT_EQ(manager.CurrentOrNull()->items[1].id, b);
    EXPECT_EQ(manager.Generation(), before);
}

TEST(CanvasManagerTest, BringItemsToFrontIgnoresUnknownId) {
    CanvasManager manager;
    const ItemId a = manager.CreateItem(false, Rect{}, "A");
    const uint64_t before = manager.Generation();

    manager.BringItemsToFront({/*bogus id=*/999999});

    EXPECT_EQ(manager.CurrentOrNull()->items[0].id, a);
    EXPECT_EQ(manager.Generation(), before);
}

TEST(CanvasManagerTest, ToggleFullscreenSavesAndRestoresRect) {
    CanvasManager manager;
    const Rect original{50, 60, 320, 200};
    const ItemId id = manager.CreateItem(false, original, "A");

    // 320x200 (ratio 1.6) is relatively *taller* than the 1920x1080 (ratio
    // 1.778) viewport, so it's pillarboxed: full height, width scaled to
    // match the item's own ratio, centered horizontally.
    manager.ToggleFullscreen(id, 1920.0f, 1080.0f);
    ASSERT_TRUE(manager.CurrentOrNull()->items.front().isFullscreen);
    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, (Rect{96, 0, 1728, 1080}));

    manager.ToggleFullscreen(id, 1920.0f, 1080.0f);
    EXPECT_FALSE(manager.CurrentOrNull()->items.front().isFullscreen);
    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, original);
}

TEST(CanvasManagerTest, ToggleFullscreenLetterboxesAnItemWiderThanTheViewport) {
    CanvasManager manager;
    // 400x100 (ratio 4.0) is relatively *wider* than the 1920x1080 (ratio
    // 1.778) viewport, so it's letterboxed: full width, height scaled to
    // match the item's own ratio, centered vertically.
    const ItemId id = manager.CreateItem(false, Rect{0, 0, 400, 100}, "A");

    manager.ToggleFullscreen(id, 1920.0f, 1080.0f);

    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, (Rect{0, 300, 1920, 480}));
}

TEST(CanvasManagerTest, ToggleFullscreenRestoresADisplayFillingItemWhereItWasAnchored) {
    // What clicking (rather than dragging) while placing a snippet makes:
    // created fullscreen, so the anchor underneath is the whole display,
    // and that is where it restores to.
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1920.0f, 1080.0f);
    const ItemId id = manager.CreateItem(true, Rect{0, 0, 1920, 1080}, "A");
    manager.CurrentOrNull()->items.front().isFullscreen = true;

    manager.ToggleFullscreen(id, 1920.0f, 1080.0f);

    EXPECT_FALSE(manager.CurrentOrNull()->items.front().isFullscreen);
    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, (Rect{0, 0, 1920, 1080}));
    EXPECT_EQ(manager.CurrentOrNull()->items.front().anchorRect, (Rect{0, 0, 1920, 1080}));
}

TEST(CanvasManagerTest, ToggleFullscreenRestoreLeavesTheAnchorAloneWhenNothingHadToMove) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1920.0f, 1080.0f);
    const Rect original{50, 60, 320, 200};
    const ItemId id = manager.CreateItem(false, original, "A");

    manager.ToggleFullscreen(id, 1920.0f, 1080.0f);
    manager.ToggleFullscreen(id, 1920.0f, 1080.0f);

    EXPECT_EQ(manager.CurrentOrNull()->items.front().anchorRect, original);
}

TEST(CanvasManagerTest, ToggleFullscreenRestoreLeavesAnItemWhereItWas) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1920.0f, 1080.0f);
    const Rect original{50, 60, 320, 200};
    const ItemId id = manager.CreateItem(false, original, "A");

    manager.ToggleFullscreen(id, 1920.0f, 1080.0f);
    manager.ToggleFullscreen(id, 1920.0f, 1080.0f);

    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, original);
}

TEST(CanvasManagerTest, ToggleFullscreenStretchIgnoresAspectRatio) {
    CanvasManager manager;
    const ItemId id = manager.CreateItem(false, Rect{50, 60, 320, 200}, "A");

    manager.ToggleFullscreen(id, 1920.0f, 1080.0f, /*stretch=*/true);

    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, (Rect{0, 0, 1920, 1080}));
}

TEST(CanvasManagerTest, MoveItemToCanvasTransfersOwnership) {
    CanvasManager manager("First");
    const CanvasId target = manager.AddCanvas("Second");
    const ItemId id = manager.CreateItem(false, Rect{}, "A");

    manager.MoveOrCopyItemToCanvas(id, target, /*copy=*/false);

    EXPECT_TRUE(manager.CurrentOrNull()->items.empty());
    const auto& targetCanvas =
        *std::find_if(manager.Canvases().begin(), manager.Canvases().end(),
                      [target](const Canvas& c) { return c.id == target; });
    ASSERT_EQ(targetCanvas.items.size(), 1u);
    EXPECT_EQ(targetCanvas.items.front().id, id);
}

TEST(CanvasManagerTest, MoveItemToCanvasReturnsZeroSinceThereIsNoNewId) {
    CanvasManager manager("First");
    const CanvasId target = manager.AddCanvas("Second");
    const ItemId id = manager.CreateItem(false, Rect{}, "A");

    EXPECT_EQ(manager.MoveOrCopyItemToCanvas(id, target, /*copy=*/false), 0u);
}

TEST(CanvasManagerTest, CopyItemToCanvasReturnsTheNewItemsId) {
    CanvasManager manager("First");
    const CanvasId target = manager.AddCanvas("Second");
    const ItemId id = manager.CreateItem(false, Rect{}, "A");

    const ItemId newId = manager.MoveOrCopyItemToCanvas(id, target, /*copy=*/true);

    EXPECT_NE(newId, 0u);
    EXPECT_NE(newId, id);
    const auto& targetCanvas =
        *std::find_if(manager.Canvases().begin(), manager.Canvases().end(),
                      [target](const Canvas& c) { return c.id == target; });
    ASSERT_EQ(targetCanvas.items.size(), 1u);
    EXPECT_EQ(targetCanvas.items.front().id, newId);
}

TEST(CanvasManagerTest, MoveOrCopyItemToCanvasReturnsZeroOnANoOp) {
    CanvasManager manager("First");
    const ItemId id = manager.CreateItem(false, Rect{}, "A");

    // Same-canvas target, and a target that doesn't exist - both no-ops
    // per the method's own doc comment.
    EXPECT_EQ(manager.MoveOrCopyItemToCanvas(id, manager.CurrentOrNull()->id, /*copy=*/true), 0u);
    EXPECT_EQ(manager.MoveOrCopyItemToCanvas(id, 999999, /*copy=*/true), 0u);
}

// A copy is a new thing: it starts unmarked whatever its source's mark,
// rather than arriving invisible with a stamp from before it existed.
TEST(CanvasManagerTest, ACopyOfADeletedSnippetIsNotDeleted) {
    CanvasManager manager("First");
    const ItemId id = manager.CreateItem(false, Rect{}, "A");
    ASSERT_TRUE(manager.MarkDeleted(id, 1234));

    const ItemId copy = manager.PlaceItemOnCanvas(id, manager.CurrentCanvasId(), /*copy=*/true);

    ASSERT_NE(copy, 0u);
    EXPECT_FALSE(manager.IsItemDeleted(copy));
    EXPECT_TRUE(manager.IsItemDeleted(id)) << "the source is as it was";
}

// Nothing lands somewhere it cannot be seen: a deleted canvas takes no
// snippet, a deleted folder takes no canvas.
TEST(CanvasManagerTest, NothingIsPlacedOnADeletedCanvasOrMovedIntoADeletedFolder) {
    CanvasManager manager("First");
    const CanvasId first = manager.CurrentCanvasId();
    const FolderId firstFolder = manager.CurrentFolderId();
    const ItemId id = manager.CreateItem(false, Rect{}, "A");
    const CanvasId gone = manager.AddCanvas("Gone");
    ASSERT_TRUE(manager.MarkDeleted(gone, 1234));

    EXPECT_EQ(manager.PlaceItemOnCanvas(id, gone, /*copy=*/true), 0u);
    EXPECT_EQ(manager.PlaceItemOnCanvas(id, gone, /*copy=*/false), 0u);
    EXPECT_EQ(manager.CanvasHoldingItem(id), std::optional<CanvasId>(first));

    const FolderId deletedFolder = manager.AddFolder("Deleted");
    ASSERT_TRUE(manager.MarkDeleted(deletedFolder, 1234));
    manager.MoveCanvasToFolder(first, deletedFolder);
    EXPECT_EQ(manager.FindCanvas(first)->folderId, firstFolder);
}

TEST(CanvasManagerTest, CopyItemToCanvasLeavesOriginalAndDeepCopiesStrokes) {
    CanvasManager manager("First");
    const CanvasId originalCanvasId = manager.CurrentOrNull()->id;
    const CanvasId target = manager.AddCanvas("Second");
    const ItemId id = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    manager.CurrentOrNull()->items.front().strokes.push_back(Stroke{{StrokePoint{1, 1}, StrokePoint{2, 2}}, 0xFF0000FF, 3.0f});

    manager.MoveOrCopyItemToCanvas(id, target, /*copy=*/true);

    ASSERT_EQ(manager.CurrentOrNull()->items.size(), 1u);  // original still present
    EXPECT_EQ(manager.CurrentOrNull()->items.front().id, id);

    const auto& targetCanvas =
        *std::find_if(manager.Canvases().begin(), manager.Canvases().end(),
                      [target](const Canvas& c) { return c.id == target; });
    ASSERT_EQ(targetCanvas.items.size(), 1u);
    const Item& copiedItem = targetCanvas.items.front();
    EXPECT_NE(copiedItem.id, id);  // distinct id
    ASSERT_EQ(copiedItem.strokes.size(), 1u);
    EXPECT_EQ(copiedItem.strokes.front().points.size(), 2u);

    // Mutating the copy's strokes must not affect the original's - proves
    // the copy is a deep copy, not a shared reference.
    manager.SwitchToCanvas(target);
    manager.CurrentOrNull()->items.front().strokes.front().points.push_back(StrokePoint{3, 3});
    manager.SwitchToCanvas(originalCanvasId);
    EXPECT_EQ(manager.CurrentOrNull()->items.front().strokes.front().points.size(), 2u);
}

// A snippet moved keeps its picture: it is the same snippet, stored under
// the same id.
TEST(CanvasManagerTest, MoveItemToCanvasKeepsItsStoredPicture) {
    CanvasManager manager("First");
    const CanvasId target = manager.AddCanvas("Second");
    const ItemId id = manager.CreateItem(true, Rect{0, 0, 100, 100}, "A");
    manager.CurrentOrNull()->items.front().picture.stored = true;

    manager.MoveOrCopyItemToCanvas(id, target, /*copy=*/false);

    const auto& targetCanvas =
        *std::find_if(manager.Canvases().begin(), manager.Canvases().end(),
                      [target](const Canvas& c) { return c.id == target; });
    ASSERT_EQ(targetCanvas.items.size(), 1u);
    EXPECT_TRUE(targetCanvas.items.front().picture.stored);
}

// A copy starts without the source's stored picture; the session gives it
// one of its own (see Session::ClonePicturesForCopy).
TEST(CanvasManagerTest, CopyItemToCanvasLeavesTheCopyWithoutAStoredPicture) {
    CanvasManager manager("First");
    const CanvasId target = manager.AddCanvas("Second");
    const ItemId id = manager.CreateItem(true, Rect{0, 0, 100, 100}, "A");
    manager.CurrentOrNull()->items.front().picture.stored = true;

    manager.MoveOrCopyItemToCanvas(id, target, /*copy=*/true);

    EXPECT_TRUE(manager.CurrentOrNull()->items.front().picture.stored);
    const auto& targetCanvas =
        *std::find_if(manager.Canvases().begin(), manager.Canvases().end(),
                      [target](const Canvas& c) { return c.id == target; });
    ASSERT_EQ(targetCanvas.items.size(), 1u);
    EXPECT_FALSE(targetCanvas.items.front().picture.stored);
}

TEST(CanvasManagerTest, MoveOrCopyToOwnCanvasIsNoOp) {
    CanvasManager manager;
    const CanvasId current = manager.CurrentOrNull()->id;
    const ItemId id = manager.CreateItem(false, Rect{}, "A");

    manager.MoveOrCopyItemToCanvas(id, current, /*copy=*/false);

    ASSERT_EQ(manager.CurrentOrNull()->items.size(), 1u);
    EXPECT_EQ(manager.CurrentOrNull()->items.front().id, id);
}

TEST(CanvasManagerTest, DuplicateItemAddsASecondItemOnTheSameCanvasWithDeepCopiedStrokes) {
    CanvasManager manager;
    const Rect rect{10, 20, 300, 200};
    const ItemId id = manager.CreateItem(false, rect, "A");
    manager.CurrentOrNull()->items.front().strokes.push_back(Stroke{{StrokePoint{1, 1}, StrokePoint{2, 2}}, 0xFF0000FF, 3.0f});

    const ItemId newId = manager.DuplicateItem(id);

    ASSERT_NE(newId, 0u);
    EXPECT_NE(newId, id);
    ASSERT_EQ(manager.CurrentOrNull()->items.size(), 2u);
    const auto& original = manager.CurrentOrNull()->items.front();
    const auto& copy = manager.CurrentOrNull()->items.back();
    EXPECT_EQ(original.id, id);
    EXPECT_EQ(copy.id, newId);
    EXPECT_EQ(copy.rect, rect);  // placed at the same rect, not offset
    ASSERT_EQ(copy.strokes.size(), 1u);

    // Mutating the copy's strokes must not affect the original's - proves
    // the copy is a deep copy, not a shared reference.
    manager.CurrentOrNull()->items.back().strokes.front().points.push_back(StrokePoint{3, 3});
    EXPECT_EQ(manager.CurrentOrNull()->items.front().strokes.front().points.size(), 2u);
}

TEST(CanvasManagerTest, DuplicateItemLeavesTheCopyWithoutAStoredPicture) {
    CanvasManager manager;
    const ItemId id = manager.CreateItem(true, Rect{0, 0, 100, 100}, "A");
    manager.CurrentOrNull()->items.front().picture.stored = true;

    const ItemId newId = manager.DuplicateItem(id);

    // The picture stored is the source's; the caller gives the copy its own.
    EXPECT_TRUE(manager.CurrentOrNull()->items.front().picture.stored);
    const auto& copy = manager.CurrentOrNull()->items.back();
    EXPECT_EQ(copy.id, newId);
    EXPECT_FALSE(copy.picture.stored);
}

TEST(CanvasManagerTest, DuplicateUnknownItemIsNoOpAndReturnsZero) {
    CanvasManager manager;
    manager.CreateItem(false, Rect{}, "A");

    const ItemId result = manager.DuplicateItem(999);

    EXPECT_EQ(result, 0u);
    EXPECT_EQ(manager.CurrentOrNull()->items.size(), 1u);
}

TEST(CanvasManagerTest, BakeStrokeToNativeIsIdentityWhenRectMatchesNative) {
    Item item;
    item.rect = Rect{100, 200, 300, 150};
    item.nativeW = 300;
    item.nativeH = 150;

    Stroke screenStroke;
    screenStroke.colorRGBA = 0xAABBCCDD;
    screenStroke.width = 4.0f;
    screenStroke.points = {StrokePoint{100, 200}, StrokePoint{400, 350}};

    const Stroke native = CanvasManager::BakeStrokeToNative(item, screenStroke);

    EXPECT_EQ(native.colorRGBA, 0xAABBCCDDu);
    EXPECT_FLOAT_EQ(native.width, 4.0f);  // scale == 1 when rect == native size
    ASSERT_EQ(native.points.size(), 2u);
    EXPECT_FLOAT_EQ(native.points[0].x, 0.0f);
    EXPECT_FLOAT_EQ(native.points[0].y, 0.0f);
    EXPECT_FLOAT_EQ(native.points[1].x, 300.0f);
    EXPECT_FLOAT_EQ(native.points[1].y, 150.0f);
}

TEST(CanvasManagerTest, EraseAtRemovesFromTheItemInNativeSpace) {
    CanvasManager manager;
    // nativeW/H are fixed at creation from this rect; then simulate the
    // item having been resized to half that size on screen, so erasing has
    // an actual native<->screen scale factor to exercise.
    const ItemId id = manager.CreateItem(false, Rect{0, 0, 300, 150}, "A");
    Item* item = &manager.CurrentOrNull()->items.front();
    // A short native-space segment straddling (60,30) - both endpoints end
    // up within the erase circle below, so the whole stroke is consumed.
    item->strokes.push_back(Stroke{{StrokePoint{58, 30}, StrokePoint{62, 30}}, 0xFF0000FF, 2.0f});
    item->rect = Rect{0, 0, 150, 75};

    // Screen-space (30,15) maps to native (60,30) at this item's 2x scale,
    // and the 2.0px screen-space radius likewise scales to a 4.0 native
    // radius - erasing near the screen point should reach the native-space
    // stroke.
    manager.EraseAt(id, 30.0f, 15.0f, 2.0f);

    EXPECT_TRUE(manager.CurrentOrNull()->items.front().strokes.empty());
}

TEST(CanvasManagerTest, EraseAtShortensTheItemsStrokeInNativeSpace) {
    CanvasManager manager;
    const ItemId id = manager.CreateItem(false, Rect{0, 0, 300, 150}, "A");
    Item* item = &manager.CurrentOrNull()->items.front();
    // Only the (60,30) end falls within the native-space erase circle
    // below - the far end should survive, shortened rather than deleted.
    item->strokes.push_back(Stroke{{StrokePoint{60, 30}, StrokePoint{160, 30}}, 0xFF0000FF, 2.0f});
    item->rect = Rect{0, 0, 150, 75};

    manager.EraseAt(id, 30.0f, 15.0f, 2.0f);  // native center (60,30), native radius 4.0

    ASSERT_EQ(manager.CurrentOrNull()->items.front().strokes.size(), 1u);
    const Stroke& remaining = manager.CurrentOrNull()->items.front().strokes.front();
    ASSERT_EQ(remaining.points.size(), 2u);
    EXPECT_NEAR(remaining.points[0].x, 64.0f, 1e-3f);   // cut at the native circle boundary
    EXPECT_NEAR(remaining.points[1].x, 160.0f, 1e-3f);  // untouched far end survives
}

TEST(CanvasManagerTest, BakeStrokeToNativeScalesWhenItemIsShrunkOnScreen) {
    // Item drawn at half its native size on screen: a screen-space point
    // should map to double the offset in native space, and stroke width
    // should scale up by the same factor.
    Item item;
    item.rect = Rect{0, 0, 150, 75};
    item.nativeW = 300;
    item.nativeH = 150;

    Stroke screenStroke;
    screenStroke.width = 2.0f;
    screenStroke.points = {StrokePoint{30, 15}};

    const Stroke native = CanvasManager::BakeStrokeToNative(item, screenStroke);

    ASSERT_EQ(native.points.size(), 1u);
    EXPECT_FLOAT_EQ(native.points[0].x, 60.0f);
    EXPECT_FLOAT_EQ(native.points[0].y, 30.0f);
    EXPECT_FLOAT_EQ(native.width, 4.0f);
}

// ================= Generation() / MarkChanged() (drives the draw caches) =================

TEST(CanvasManagerTest, GenerationStartsAtZeroForAFreshlyConstructedManager) {
    CanvasManager manager;
    // Construction creates a default folder+canvas but isn't itself a
    // user change - see the constructor's own comment.
    EXPECT_EQ(manager.Generation(), 0u);
}

TEST(CanvasManagerTest, GenerationBumpsOnAMutatingCallButNotOnANoOp) {
    CanvasManager manager;
    const uint64_t before = manager.Generation();

    manager.RenameCanvas(manager.CurrentOrNull()->id, "Renamed");
    EXPECT_GT(manager.Generation(), before);

    const uint64_t afterRename = manager.Generation();
    manager.RenameCanvas(/*bogus id=*/999999, "Ignored");  // no-op: id doesn't exist
    EXPECT_EQ(manager.Generation(), afterRename);

    manager.RenameCanvas(manager.CurrentOrNull()->id, "");  // no-op: empty name
    EXPECT_EQ(manager.Generation(), afterRename);
}

TEST(CanvasManagerTest, MarkChangedBumpsGeneration) {
    CanvasManager manager;
    const uint64_t before = manager.Generation();
    manager.MarkChanged();
    EXPECT_EQ(manager.Generation(), before + 1);
}

TEST(CanvasManagerTest, CreateItemAndEraseAtBumpGeneration) {
    CanvasManager manager;
    const uint64_t afterConstruction = manager.Generation();

    const ItemId id = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    EXPECT_GT(manager.Generation(), afterConstruction);

    const uint64_t afterCreate = manager.Generation();
    // Baking a stroke into item.strokes happens outside CanvasManager's
    // own methods in the real app (see OverlayApp::
    // Session::CommitLiveStroke) - simulate that here directly.
    Item* item = &manager.CurrentOrNull()->items.back();
    item->strokes.push_back(Stroke{});
    manager.MarkChanged();
    EXPECT_GT(manager.Generation(), afterCreate);

    const uint64_t afterBake = manager.Generation();
    manager.EraseAt(id, 0, 0, 1000.0f);  // marks changed whether or not anything came away
    EXPECT_GT(manager.Generation(), afterBake);
}

// ================= ExportSnapshot() / ImportSnapshot() =================

TEST(CanvasManagerTest, ExportThenImportSnapshotRoundTrips) {
    CanvasManager manager;
    const FolderId secondFolder = manager.AddFolder("Folder 2");
    const CanvasId secondCanvas = manager.AddCanvas("Canvas 2");
    manager.MoveCanvasToFolder(secondCanvas, secondFolder);
    const ItemId itemId = manager.CreateItem(true, Rect{1, 2, 3, 4}, "Shot 1");
    Item* item = &manager.CurrentOrNull()->items.back();
    item->picture.stored = true;
    item->strokes.push_back(Stroke{{StrokePoint{1, 1}, StrokePoint{2, 2}}, 0xAABBCCDDu, 5.0f});
    manager.SwitchToCanvas(secondCanvas);

    const CanvasManagerSnapshot snapshot = manager.ExportSnapshot();

    CanvasManager loaded;
    loaded.ImportSnapshot(snapshot);

    EXPECT_EQ(loaded.Folders().size(), 2u);
    EXPECT_EQ(loaded.Canvases().size(), 2u);  // Canvas 1, Canvas 2
    EXPECT_EQ(loaded.CurrentFolderId(), manager.CurrentFolderId());
    EXPECT_EQ(loaded.CurrentOrNull()->id, secondCanvas);

    const auto it = std::find_if(loaded.Canvases().begin(), loaded.Canvases().end(),
                                  [](const Canvas& c) { return !c.items.empty(); });
    ASSERT_NE(it, loaded.Canvases().end());
    ASSERT_EQ(it->items.size(), 1u);
    const Item& roundTripped = it->items.front();
    EXPECT_EQ(roundTripped.id, itemId);
    EXPECT_TRUE(roundTripped.hasBackground);
    EXPECT_TRUE(roundTripped.picture.stored);
    ASSERT_EQ(roundTripped.strokes.size(), 1u);
    EXPECT_EQ(roundTripped.strokes.front().colorRGBA, 0xAABBCCDDu);
}

TEST(CanvasManagerTest, ImportSnapshotDoesNotBumpGeneration) {
    CanvasManager source;
    const CanvasManagerSnapshot snapshot = source.ExportSnapshot();

    CanvasManager loaded;
    loaded.ImportSnapshot(snapshot);

    // A fresh load isn't itself a user change - matches the constructor's
    // own "generation starts at 0" behavior.
    EXPECT_EQ(loaded.Generation(), 0u);
}

// ================= SyncItemsToDisplaySize (resolution-relative item sizing) =================

TEST(CanvasManagerTest, SyncFirstCallJustAdoptsTheCurrentRectAsTheAnchorWithoutTouchingItems) {
    CanvasManager manager;
    const Rect original{50, 60, 300, 200};
    manager.CreateItem(false, original, "A");

    manager.SyncItemsToDisplaySize(1920.0f, 1080.0f);

    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, original);
}

TEST(CanvasManagerTest, SyncToTheSameSizeIsANoOpAndDoesNotBumpGeneration) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1920.0f, 1080.0f);
    const Rect original{50, 60, 300, 200};
    manager.CreateItem(false, original, "A");
    const uint64_t before = manager.Generation();

    manager.SyncItemsToDisplaySize(1920.0f, 1080.0f);

    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, original);
    EXPECT_EQ(manager.Generation(), before);
}

TEST(CanvasManagerTest, SyncBumpsGenerationWhenItActuallyChangesSomething) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    manager.CreateItem(false, Rect{200, 150, 300, 200}, "A");
    const uint64_t before = manager.Generation();

    manager.SyncItemsToDisplaySize(2000.0f, 1000.0f);

    EXPECT_GT(manager.Generation(), before);
}

TEST(CanvasManagerTest, SyncPositionScalesPerAxisWhileSizeStaysAtTheSmallerAxisRatio) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    manager.CreateItem(false, Rect{200, 150, 300, 200}, "A");

    // scaleX = 2000/1000 = 2.0, scaleY = 1000/1000 = 1.0 - the smaller of
    // the two (1.0) is the uniform size factor, so size is untouched here
    // while x alone doubles.
    manager.SyncItemsToDisplaySize(2000.0f, 1000.0f);

    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, (Rect{400, 150, 300, 200}));
}

TEST(CanvasManagerTest, SyncNeverDistortsAnItemsOwnAspectRatio) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    // 200x150 - a 4:3 aspect ratio.
    manager.CreateItem(false, Rect{100, 100, 200, 150}, "A");

    // scaleX = 2.0, scaleY = 3.0 - very different per-axis ratios, the
    // exact case a naive independent-axis size scale would stretch.
    manager.SyncItemsToDisplaySize(2000.0f, 3000.0f);

    const Rect& rect = manager.CurrentOrNull()->items.front().rect;
    EXPECT_EQ(rect, (Rect{200, 300, 400, 300}));
    // Still exactly 4:3 - the uniform 2.0x size factor (min(2.0, 3.0))
    // scaled both dimensions together rather than independently.
    EXPECT_FLOAT_EQ(rect.w / rect.h, 200.0f / 150.0f);
}

TEST(CanvasManagerTest, SyncSetsAStretchModeFullscreenItemToExactlyTheCurrentViewportEveryCall) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    manager.CreateItem(false, Rect{0, 0, 1000, 1000}, "A");
    manager.CurrentOrNull()->items.front().isFullscreen = true;
    manager.CurrentOrNull()->items.front().isFullscreenStretch = true;

    manager.SyncItemsToDisplaySize(1600.0f, 900.0f);

    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, (Rect{0, 0, 1600, 900}));
}

// The sync runs every frame, so a sync that forced every fullscreen item
// to fill the viewport exactly would stretch a fit-mode item one frame
// after it was entered. It has to keep fitting instead.
TEST(CanvasManagerTest, SyncKeepsAFitModeFullscreenItemsAspectRatioIntactAcrossADisplayChange) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    // 300x200 - a 3:2 aspect ratio, deliberately not the same as either
    // viewport below.
    const ItemId id = manager.CreateItem(false, Rect{100, 100, 300, 200}, "A");
    manager.ToggleFullscreen(id, 1000.0f, 1000.0f);  // stretch defaults false - fit mode
    ASSERT_TRUE(manager.CurrentOrNull()->items.front().isFullscreen);
    ASSERT_FALSE(manager.CurrentOrNull()->items.front().isFullscreenStretch);

    // A later display-size change must re-fit, not stretch - the item's
    // own 3:2 aspect ratio (from its anchor, the pre-fullscreen rect)
    // must survive, not get silently replaced with a 16:9 fill.
    manager.SyncItemsToDisplaySize(1600.0f, 900.0f);
    const Rect& rect = manager.CurrentOrNull()->items.front().rect;
    EXPECT_EQ(rect, (Rect{125, 0, 1350, 900}));
    EXPECT_FLOAT_EQ(rect.w / rect.h, 300.0f / 200.0f);
}

TEST(CanvasManagerTest, SyncLeavesAFullscreenItemsAnchorUntouchedUnderneath) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    const ItemId id = manager.CreateItem(false, Rect{100, 100, 300, 200}, "A");
    manager.ToggleFullscreen(id, 1000.0f, 1000.0f);
    ASSERT_TRUE(manager.CurrentOrNull()->items.front().isFullscreen);

    // The item's rect re-fits live to match the new display, same as any
    // fit-mode fullscreen item - but its anchor (where it'll go on
    // exiting) is untouched, so it isn't derived from the fullscreen rect
    // the display change just produced.
    manager.SyncItemsToDisplaySize(1600.0f, 900.0f);
    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, (Rect{125, 0, 1350, 900}));

    // Exiting against the *original* 1000x1000 the item was anchored
    // against (not the 1600x900 it was just synced to) restores exactly
    // the original rect - RescaleRectForDisplaySize's from==to identity -
    // proving the restore came from the untouched anchor, not from
    // {125, 0, 1350, 900} or any other stale value.
    manager.ToggleFullscreen(id, 1000.0f, 1000.0f);
    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, (Rect{100, 100, 300, 200}));
}

TEST(CanvasManagerTest, SyncFloorsSizeAtTheUsualResizeMinimumsOnADrasticDownscale) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(2000.0f, 2000.0f);
    manager.CreateItem(false, Rect{500, 500, 1000, 800}, "A");

    // Uniform size factor 0.05 would otherwise shrink this to 50x40 - well
    // under the usual 90x70 resize floor (see item_geometry.h).
    manager.SyncItemsToDisplaySize(100.0f, 100.0f);

    const Rect& rect = manager.CurrentOrNull()->items.front().rect;
    // 5:4, floored without being reshaped - not a flat 90x70, which would
    // be the item silently coming back at 9:7. See MinimumSizeForAspectRatio.
    EXPECT_GE(rect.w, kItemMinWidth);
    EXPECT_GE(rect.h, kItemMinHeight);
    EXPECT_FLOAT_EQ(rect.w / rect.h, 1000.0f / 800.0f);
    EXPECT_FLOAT_EQ(rect.w, 90.0f);  // 5:4 is narrower than 9:7, so width is the binding floor
    EXPECT_FLOAT_EQ(rect.h, 72.0f);
}

// The wide case, where independent per-axis floors would distort the most:
// a 16:9 capture from a 4K library, reopened small enough that the uniform
// downscale takes it under both floors, has to come back 16:9.
TEST(CanvasManagerTest, SyncKeepsAWideItemsAspectRatioWhenTheFloorKicksIn) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(3840.0f, 2160.0f);
    manager.CreateItem(false, Rect{0, 0, 1920.0f, 1080.0f}, "A");

    manager.SyncItemsToDisplaySize(120.0f, 120.0f);

    const Rect& rect = manager.CurrentOrNull()->items.front().rect;
    EXPECT_NEAR(rect.w / rect.h, 16.0f / 9.0f, 0.001f);
    EXPECT_GE(rect.w, kItemMinWidth);
    EXPECT_FLOAT_EQ(rect.h, kItemMinHeight);  // wider than 9:7, so height binds
}

TEST(CanvasManagerTest, SyncWithANonPositiveNewSizeIsANoOp) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    const Rect original{200, 150, 300, 200};
    manager.CreateItem(false, original, "A");
    const uint64_t before = manager.Generation();

    manager.SyncItemsToDisplaySize(0.0f, 1000.0f);
    manager.SyncItemsToDisplaySize(1000.0f, -5.0f);

    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, original);
    EXPECT_EQ(manager.Generation(), before);
}

TEST(CanvasManagerTest, SyncBackAndForthBetweenTheSameTwoSizesNeverCompoundsAndAlwaysReturnsExactly) {
    // The bug this whole design fixes: an earlier version rescaled
    // whatever the *previous* call had already produced, so repeatedly
    // squeezing the display narrower and widening it back ratcheted items
    // smaller every cycle, never back up. Every item now recomputes fresh
    // from its own fixed anchor every call, so this must be exactly
    // reversible, indefinitely.
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1400.0f, 900.0f);
    const Rect original{200, 150, 300, 200};
    manager.CreateItem(false, original, "A");

    for (int i = 0; i < 5; ++i) {
        manager.SyncItemsToDisplaySize(700.0f, 900.0f);   // squeeze narrower
        manager.SyncItemsToDisplaySize(1400.0f, 900.0f);  // widen back
    }

    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, original);
}

// ================= CommitItemLayout / ResetItemToNativeSize =================

TEST(CanvasManagerTest, CommitItemLayoutReAnchorsSoALaterDisplayChangeStartsFromTheNewRectNotTheOldOne) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    const ItemId id = manager.CreateItem(false, Rect{100, 100, 300, 200}, "A");

    // Simulates an interactive drag: mutate rect directly, then commit -
    // exactly what OverlayApp does on every move of a move/resize gesture.
    manager.CurrentOrNull()->items.front().rect = Rect{50, 50, 400, 400};
    manager.CommitItemLayout(id);

    // Without the commit, this display change would rescale from the
    // *original* anchor (100,100,300,200), not the drag's result.
    manager.SyncItemsToDisplaySize(2000.0f, 1000.0f);

    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, (Rect{100, 50, 400, 400}));
}

TEST(CanvasManagerTest, ResetItemToNativeSizeRestoresTheCreationSizeCenteredOnItsCurrentPosition) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    const ItemId id = manager.CreateItem(false, Rect{100, 100, 300, 200}, "A");
    manager.CurrentOrNull()->items.front().rect = Rect{500, 500, 30, 20};  // shrunk by an interactive resize

    manager.ResetItemToNativeSize(id);

    // nativeW/H (300x200, fixed at creation) restored, centered on the
    // shrunk rect's own center (515, 510).
    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, (Rect{365, 410, 300, 200}));
}

TEST(CanvasManagerTest, ResetItemToNativeSizeExitsFullscreenFirst) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    const ItemId id = manager.CreateItem(false, Rect{100, 100, 300, 200}, "A");
    manager.ToggleFullscreen(id, 1000.0f, 1000.0f, /*stretch=*/true);
    ASSERT_TRUE(manager.CurrentOrNull()->items.front().isFullscreen);

    manager.ResetItemToNativeSize(id);

    EXPECT_FALSE(manager.CurrentOrNull()->items.front().isFullscreen);
    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect.w, 300.0f);
    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect.h, 200.0f);
}

TEST(CanvasManagerTest, ResetItemToNativeSizeCentersADisplaySizedItemOnTheDisplay) {
    // A fullscreen capture's native size is the display's, so centering it
    // back on its own center lands at 0,0, filling the display.
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    const ItemId id = manager.CreateItem(true, Rect{0, 0, 1000, 1000}, "A");

    manager.ResetItemToNativeSize(id);

    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, (Rect{0, 0, 1000, 1000}));
}

TEST(CanvasManagerTest, ResetItemToNativeSizeReAnchorsSoALaterDisplayChangeUsesTheRestoredSize) {
    CanvasManager manager;
    manager.SyncItemsToDisplaySize(1000.0f, 1000.0f);
    const ItemId id = manager.CreateItem(false, Rect{100, 100, 300, 200}, "A");
    manager.CurrentOrNull()->items.front().rect = Rect{500, 500, 30, 20};
    manager.ResetItemToNativeSize(id);

    manager.SyncItemsToDisplaySize(2000.0f, 1000.0f);

    // Anchor is now the restored 300x200 rect at (365, 410) against
    // 1000x1000 - scaleX = 2.0, scaleY = 1.0, uniform size factor 1.0.
    EXPECT_EQ(manager.CurrentOrNull()->items.front().rect, (Rect{730, 410, 300, 200}));
}

// ===== Deleted things =====
//
// A delete is a mark made in place (see CanvasManager's class comment).
// What these check is what a mark hides, what it moves off, what it keeps
// from being changed, and what a restore brings back.

TEST(CanvasManagerTest, ADeletedSnippetStaysWhereItIsAndIsHidden) {
    CanvasManager manager("Canvas");
    const ItemId a = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    manager.CreateItem(false, Rect{0, 0, 100, 100}, "B");

    ASSERT_TRUE(manager.MarkDeleted(a, 1234));
    const Canvas& canvas = *manager.CurrentOrNull();
    ASSERT_EQ(canvas.items.size(), 2u) << "marked, not taken out";
    EXPECT_EQ(canvas.items[0].deletedAt, 1234);
    EXPECT_TRUE(manager.IsItemDeleted(a));
    EXPECT_FALSE(manager.IsDeleted(canvas, canvas.items[1]));
    EXPECT_FALSE(manager.MarkDeleted(a, 5678)) << "marked already";
}

// Folders first, each once; a canvas with its folder going is left to go
// with it; one that went only with its folder is never listed on its own;
// and a stamp at the cutoff is not before it.
TEST(CanvasManagerTest, MarkedBeforeListsWhatWasDeletedLongEnoughAgo) {
    CanvasManager manager("First");
    const FolderId home = manager.CurrentFolderId();
    const CanvasId current = manager.CurrentCanvasId();
    const CanvasId early = manager.AddCanvas("Early");
    const CanvasId atCutoff = manager.AddCanvas("At cutoff");
    const FolderId folder = manager.AddFolder("Folder");
    manager.SwitchToFolder(folder);
    const CanvasId markedFirst = manager.AddCanvas("Marked first");
    const CanvasId wentWithIt = manager.AddCanvas("Went with it");
    manager.SwitchToFolder(home);
    manager.SwitchToCanvas(current);
    ASSERT_TRUE(manager.MarkDeleted(early, 10));
    ASSERT_TRUE(manager.MarkDeleted(atCutoff, 50));
    ASSERT_TRUE(manager.MarkDeleted(markedFirst, 20));
    ASSERT_TRUE(manager.MarkDeleted(folder, 60));

    EXPECT_TRUE(manager.MarkedBefore(10).empty());
    EXPECT_EQ(manager.MarkedBefore(50), (std::vector<uint64_t>{early, markedFirst}))
        << "the folder is not due yet; a canvas marked in it before it went is";
    EXPECT_EQ(manager.MarkedBefore(100), (std::vector<uint64_t>{folder, early, atCutoff}))
        << "what is in the folder goes with it";
    (void)wentWithIt;
}

TEST(CanvasManagerTest, WhatIsInsideADeletedThingCountsAsDeleted) {
    CanvasManager manager("Canvas");
    const FolderId folder = manager.CurrentFolderId();
    const ItemId item = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");

    ASSERT_TRUE(manager.MarkDeleted(folder, 7));
    const Canvas& canvas = manager.Canvases()[0];
    EXPECT_EQ(canvas.deletedAt, 0) << "only the folder is marked";
    EXPECT_TRUE(manager.IsDeleted(canvas));
    EXPECT_TRUE(manager.IsItemDeleted(item));
    EXPECT_FALSE(manager.HasCurrentCanvas());
    EXPECT_EQ(manager.CurrentFolderId(), 0u) << "no folder left to browse";
}

TEST(CanvasManagerTest, DeletingTheCurrentCanvasMovesToTheShownOneBeforeIt) {
    CanvasManager manager("First");
    const CanvasId first = manager.CurrentCanvasId();
    const CanvasId second = manager.AddCanvas("Second");
    const CanvasId third = manager.AddCanvas("Third");
    manager.SwitchToCanvas(third);

    ASSERT_TRUE(manager.MarkDeleted(second, 1));
    EXPECT_EQ(manager.CurrentCanvasId(), third) << "not what was being looked at";
    ASSERT_TRUE(manager.MarkDeleted(third, 2));
    EXPECT_EQ(manager.CurrentCanvasId(), first) << "past the one before it, which is hidden too";
    manager.SwitchToCanvas(second);
    EXPECT_EQ(manager.CurrentCanvasId(), first) << "a hidden canvas isn't switched to";
}

TEST(CanvasManagerTest, WhatIsMarkedIsCountedAndFoundByFolder) {
    CanvasManager manager("First");
    const FolderId folder = manager.CurrentFolderId();
    const CanvasId first = manager.CurrentCanvasId();
    const ItemId early = manager.CreateItem(false, Rect{0, 0, 100, 100}, "Early");
    manager.CreateItem(false, Rect{0, 0, 100, 100}, "WentWithTheCanvas");
    const CanvasId second = manager.AddCanvas("Second");
    const FolderId other = manager.AddFolder("Other");
    manager.AddCanvas("Elsewhere");
    EXPECT_EQ(manager.DeletedFolderAndCanvasCount(), 0u);
    EXPECT_FALSE(manager.HoldsDeleted(*manager.FindFolder(folder)));

    ASSERT_TRUE(manager.MarkDeleted(early, 100));
    ASSERT_TRUE(manager.MarkDeleted(first, 200));
    EXPECT_TRUE(manager.HoldsDeleted(*manager.FindFolder(folder))) << "a canvas in it is deleted";
    EXPECT_FALSE(manager.HoldsDeleted(*manager.FindFolder(other)));
    EXPECT_EQ(manager.MarkedCanvasesIn(folder), std::vector<CanvasId>{first});

    ASSERT_TRUE(manager.MarkDeleted(folder, 300));
    EXPECT_EQ(manager.DeletedFolderAndCanvasCount(), 2u) << "the folder and the canvas marked before it";
    EXPECT_EQ(manager.MarkedCanvasesIn(folder), std::vector<CanvasId>{first})
        << "not the canvas that is deleted only because its folder is";
    EXPECT_TRUE(manager.IsDeleted(*manager.FindCanvas(second)));
    EXPECT_EQ(manager.MarkedSnippets(), std::vector<ItemId>{early})
        << "the snippet that went with its canvas has no mark of its own";
}

TEST(CanvasManagerTest, RestoringAFolderBringsBackEveryCanvasInIt) {
    CanvasManager manager("First");
    const FolderId folder = manager.CurrentFolderId();
    const CanvasId first = manager.CurrentCanvasId();
    const CanvasId second = manager.AddCanvas("Second");
    manager.SwitchToCanvas(second);
    const ItemId item = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    manager.AddFolder("Other");
    manager.AddCanvas("Elsewhere");
    ASSERT_TRUE(manager.MarkDeleted(item, 1));
    ASSERT_TRUE(manager.MarkDeleted(first, 2));
    ASSERT_TRUE(manager.MarkDeleted(folder, 3));

    ASSERT_TRUE(manager.Restore(folder));
    EXPECT_EQ(manager.FindFolder(folder)->deletedAt, 0);
    EXPECT_EQ(manager.FindCanvas(first)->deletedAt, 0) << "deleted on its own before the folder, and back too";
    EXPECT_FALSE(manager.IsDeleted(*manager.FindCanvas(second)));
    EXPECT_TRUE(manager.IsItemDeleted(item)) << "a snippet is not the Overview's to bring back";
    EXPECT_FALSE(manager.Restore(folder)) << "nothing left deleted in it";
}

TEST(CanvasManagerTest, RestoringAFolderThatIsNotDeletedBringsBackItsDeletedCanvases) {
    CanvasManager manager("First");
    const FolderId folder = manager.CurrentFolderId();
    const CanvasId first = manager.CurrentCanvasId();
    const CanvasId second = manager.AddCanvas("Second");
    manager.AddCanvas("Third");
    ASSERT_TRUE(manager.MarkDeleted(first, 1));
    ASSERT_TRUE(manager.MarkDeleted(second, 2));

    ASSERT_TRUE(manager.Restore(folder));
    EXPECT_EQ(manager.FindCanvas(first)->deletedAt, 0);
    EXPECT_EQ(manager.FindCanvas(second)->deletedAt, 0);
    EXPECT_FALSE(manager.HoldsDeleted(*manager.FindFolder(folder)));
}

TEST(CanvasManagerTest, RestoringACanvasOutOfADeletedFolderLeavesTheRestDeleted) {
    CanvasManager manager("First");
    const FolderId folder = manager.CurrentFolderId();
    const CanvasId first = manager.CurrentCanvasId();
    const ItemId item = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const CanvasId second = manager.AddCanvas("Second");
    const CanvasId third = manager.AddCanvas("Third");
    manager.AddFolder("Other");
    manager.AddCanvas("Elsewhere");
    ASSERT_TRUE(manager.MarkDeleted(item, 1));
    ASSERT_TRUE(manager.MarkDeleted(third, 2));
    ASSERT_TRUE(manager.MarkDeleted(folder, 3));

    ASSERT_TRUE(manager.Restore(first));
    EXPECT_EQ(manager.FindFolder(folder)->deletedAt, 0) << "the folder comes back to hold it";
    EXPECT_EQ(manager.FindCanvas(first)->deletedAt, 0);
    EXPECT_EQ(manager.FindCanvas(second)->deletedAt, 3) << "went with the folder, and stays gone as of then";
    EXPECT_EQ(manager.FindCanvas(third)->deletedAt, 2) << "deleted on its own before, and keeps its own stamp";
    EXPECT_TRUE(manager.IsItemDeleted(item)) << "deleted on its own, before its canvas";
    EXPECT_FALSE(manager.Restore(first)) << "nothing left deleted about it";
    ASSERT_TRUE(manager.Restore(item));
    EXPECT_FALSE(manager.IsItemDeleted(item));
}

TEST(CanvasManagerTest, ANewCanvasWithEveryFolderDeletedGetsAFolderOfItsOwn) {
    CanvasManager manager("Canvas");
    const FolderId folder = manager.CurrentFolderId();
    ASSERT_TRUE(manager.MarkDeleted(folder, 1));
    ASSERT_EQ(manager.CurrentFolderId(), 0u) << "nothing left to browse";

    const CanvasId made = manager.AddCanvas("New");
    ASSERT_NE(manager.FindCanvas(made), nullptr);
    EXPECT_NE(manager.FindCanvas(made)->folderId, folder);
    EXPECT_FALSE(manager.IsDeleted(*manager.FindCanvas(made)));
    EXPECT_EQ(manager.CurrentCanvasId(), made);
}

TEST(CanvasManagerTest, EraseTakesAnyKindOfThingOutForGood) {
    CanvasManager manager("First");
    const FolderId folder = manager.CurrentFolderId();
    const ItemId item = manager.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    const CanvasId second = manager.AddCanvas("Second");

    ASSERT_TRUE(manager.Erase(item));
    EXPECT_EQ(manager.FindItemAnywhere(item), nullptr);
    ASSERT_TRUE(manager.Erase(second));
    EXPECT_EQ(manager.FindCanvas(second), nullptr);
    ASSERT_TRUE(manager.Erase(folder));
    EXPECT_TRUE(manager.Folders().empty());
    EXPECT_TRUE(manager.Canvases().empty());
    EXPECT_FALSE(manager.Erase(folder)) << "nothing by that id any more";
}

TEST(CanvasManagerTest, ALibraryOpenedOnADeletedCanvasSettlesOnALiveOne) {
    CanvasManager source("First");
    const CanvasId first = source.CurrentCanvasId();
    const CanvasId second = source.AddCanvas("Second");
    source.SwitchToCanvas(second);
    CanvasManagerSnapshot snapshot = source.ExportSnapshot();
    for (Canvas& canvas : snapshot.canvases) {
        if (canvas.id == second) {
            canvas.deletedAt = 99;
        }
    }

    CanvasManager loaded;
    const uint64_t before = loaded.Generation();
    loaded.ImportSnapshot(std::move(snapshot));
    EXPECT_EQ(loaded.CurrentCanvasId(), first);
    EXPECT_EQ(loaded.Generation(), before) << "opening a library is not a change to it";
}

}  // namespace
}  // namespace sz::core
