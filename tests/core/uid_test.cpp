#include "core/util/uid.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "core/canvas/canvas_manager.h"

namespace sz::core {
namespace {

// The check is the point. A space this size makes a collision unlikely, and
// "unlikely" is exactly the assumption that breaks the one time it matters
// - so the retry path has to work, not merely exist.
TEST(UidTest, RetriesUntilItFindsOneNothingHolds) {
    const std::set<uint64_t> taken = {11, 22, 33};
    // Hands back three ids that are already in use before offering a free
    // one, so every branch of the retry runs.
    std::vector<uint64_t> sequence = {11, 22, 33, 44};
    size_t next = 0;
    const uint64_t id = MakeUid([&taken](uint64_t candidate) { return taken.count(candidate) > 0; },
                                 [&sequence, &next]() { return sequence[next++]; });
    EXPECT_EQ(id, 44u);
    EXPECT_EQ(next, 4u) << "should have drawn exactly four times";
}

TEST(UidTest, NeverHandsOutZeroBecauseZeroMeansNoId) {
    std::vector<uint64_t> sequence = {0, 0, 7};
    size_t next = 0;
    const uint64_t id = MakeUid([](uint64_t) { return false; },
                                 [&sequence, &next]() { return sequence[next++]; });
    EXPECT_EQ(id, 7u);
}

// A caller whose "is this taken" answers yes to everything is a bug, and it
// should surface as an id nothing accepts rather than as a hang inside a
// save.
TEST(UidTest, GivesUpRatherThanSpinningWhenNothingIsFree) {
    EXPECT_EQ(MakeUid([](uint64_t) { return true; }), 0u);
}

TEST(UidTest, DefaultSourceStaysInsideTheSpace) {
    for (int i = 0; i < 1000; ++i) {
        const uint64_t id = MakeUid([](uint64_t) { return false; });
        ASSERT_NE(id, 0u);
        ASSERT_LT(id, kUidSpace);
    }
}

// What the allocator is actually for: everything a library holds - folders,
// canvases and items alike - draws from one space and none of them may
// repeat, however much is created.
TEST(UidTest, EverythingACanvasManagerMintsIsDistinct) {
    CanvasManager manager;
    for (int i = 0; i < 40; ++i) {
        const CanvasId canvas = manager.AddCanvas("c");
        manager.CreateItem(/*hasBackground=*/false, Rect{0, 0, 10, 10}, "i");
        manager.CreateItem(/*hasBackground=*/true, Rect{0, 0, 10, 10}, "j");
        if (i % 5 == 0) {
            manager.AddFolder("f");
        }
        ASSERT_NE(canvas, 0u);
    }

    std::set<uint64_t> seen;
    for (const Folder& folder : manager.Folders()) {
        EXPECT_TRUE(seen.insert(folder.id).second) << "folder id repeated: " << folder.id;
    }
    for (const Canvas& canvas : manager.Canvases()) {
        EXPECT_TRUE(seen.insert(canvas.id).second) << "canvas id repeated: " << canvas.id;
        for (const Item& item : canvas.items) {
            EXPECT_TRUE(seen.insert(item.id).second) << "item id repeated: " << item.id;
        }
    }
    EXPECT_GT(seen.size(), 80u) << "the walk above should have covered everything created";
}

}  // namespace
}  // namespace sz::core
