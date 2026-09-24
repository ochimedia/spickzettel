// A session driven at random against a disk that misbehaves at random.
//
// Each seed runs a few hundred steps of what a person does to a library -
// make folders, canvases and snippets, some with pictures; edit, rename and
// move them; delete, restore and delete for good; save - while a picture
// viewer holds files open, single operations fail, and the process crashes
// and starts again. Every restart is held to the rules a crash must never
// break, and the end of every run to the rule that the disk comes to agree
// with the library once nothing is in the way any more:
//
//   - nothing loads twice, and every picture a record names loads
//   - nothing deleted for good loads again
//   - nothing the library held at its last successful save is lost
//   - once let go of and saved again, nothing is owed, nothing deleted for
//     good is left anywhere on disk, and the disk loads as the library
//
// A failure prints its seed and the steps that led there. The seeds and the
// number of steps can be raised for a longer run:
//
//   SPICKZETTEL_RANDOM_SEEDS=2000 SPICKZETTEL_RANDOM_STEPS=600 sz_core_tests
//       --gtest_filter=LibraryRandomTest.*
//
// and one seed run again alone with SPICKZETTEL_RANDOM_SEED=<seed>.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/persistence/library_store.h"
#include "core/session/session.h"
#include "core/util/uid.h"
#include "support/faulty_file_system.h"
#include "support/memory_file_system.h"

namespace sz::core {
namespace {

using fakes::FaultyFileSystem;
using fakes::MemoryFileSystem;
using persistence::LibraryStore;

std::filesystem::path Root() { return std::filesystem::temp_directory_path() / "spickzettel_random_library"; }

int FromEnvironment(const char* name, int fallback) {
    const char* value = std::getenv(name);
    return value ? std::max(0, std::atoi(value)) : fallback;
}

// Everything the library holds, by id: its parent and its name.
struct Entry {
    uint64_t parent;
    std::string name;
    bool operator==(const Entry&) const = default;
};
using Layout = std::map<uint64_t, Entry>;

Layout LayoutOf(const std::vector<Folder>& folders, const std::vector<Canvas>& canvases) {
    Layout layout;
    for (const Folder& folder : folders) {
        layout[folder.id] = {0, folder.name};
    }
    for (const Canvas& canvas : canvases) {
        layout[canvas.id] = {canvas.folderId, canvas.name};
        for (const Item& item : canvas.items) {
            layout[item.id] = {canvas.id, item.name + "|" + std::to_string(item.strokes.size())};
        }
    }
    return layout;
}

std::string Describe(const Layout& layout) {
    std::string out;
    for (const auto& [id, entry] : layout) {
        out += "    " + FormatUid(id) + " in " + FormatUid(entry.parent) + ": " + entry.name + "\n";
    }
    return out;
}

class RandomRun {
public:
    RandomRun(uint32_t seed, int steps) : random_(seed), steps_(steps), faulty_(disk_) { SeedUidsForTesting(seed); }

    void Run() {
        Start();
        for (int step = 0; step < steps_ && !::testing::Test::HasFailure(); ++step) {
            Step();
        }
        if (!::testing::Test::HasFailure()) {
            Finish();
        }
    }

    std::string Log() const {
        std::string out;
        for (const std::string& line : log_) {
            out += "  " + line + "\n";
        }
        return out;
    }

private:
    size_t Pick(size_t count) { return std::uniform_int_distribution<size_t>(0, count - 1)(random_); }
    bool Chance(int percent) { return std::uniform_int_distribution<int>(0, 99)(random_) < percent; }
    void Note(const std::string& line) { log_.push_back(line); }

    CanvasManager& Manager() { return session_->Manager(); }

    std::vector<uint64_t> FolderIds() {
        std::vector<uint64_t> ids;
        for (const Folder& folder : Manager().View().folders) {
            ids.push_back(folder.id);
        }
        return ids;
    }
    std::vector<uint64_t> CanvasIds() {
        std::vector<uint64_t> ids;
        for (const Canvas& canvas : Manager().View().canvases) {
            ids.push_back(canvas.id);
        }
        return ids;
    }
    std::vector<uint64_t> ItemIds() {
        std::vector<uint64_t> ids;
        for (const Canvas& canvas : Manager().View().canvases) {
            for (const Item& item : canvas.items) {
                ids.push_back(item.id);
            }
        }
        return ids;
    }
    std::vector<uint64_t> AllIds() {
        std::vector<uint64_t> ids = FolderIds();
        for (const uint64_t id : CanvasIds()) {
            ids.push_back(id);
        }
        for (const uint64_t id : ItemIds()) {
            ids.push_back(id);
        }
        return ids;
    }
    // `id` and everything the library holds under it.
    std::vector<uint64_t> WithEverythingUnder(uint64_t id) {
        std::vector<uint64_t> ids = {id};
        for (const Canvas& canvas : Manager().View().canvases) {
            const bool whole = canvas.id == id || canvas.folderId == id;
            if (whole && canvas.id != id) {
                ids.push_back(canvas.id);
            }
            for (const Item& item : canvas.items) {
                if (whole && item.id != id) {
                    ids.push_back(item.id);
                }
            }
        }
        return ids;
    }

    // A fresh process: a store and a session on the disk as it is, loaded
    // the way the app loads it.
    void Start() {
        store_ = std::make_unique<LibraryStore>(Root(), faulty_);
        session_ = std::make_unique<Session>();
        session_->SetLibraryStore(store_.get());
        std::optional<CanvasManagerSnapshot> loaded = store_->Load();
        const bool loadedAnything = loaded.has_value();
        if (loaded) {
            // A load erases the snippets marked deleted - for good, since
            // undo is the only way back to one and the history is gone.
            std::vector<uint64_t> marked;
            for (const Canvas& canvas : loaded->canvases) {
                for (const Item& item : canvas.items) {
                    if (item.deletedAt != 0) {
                        marked.push_back(item.id);
                    }
                }
            }
            session_->ImportLibrary(std::move(*loaded));
            erased_.insert(marked.begin(), marked.end());
        }
        // What was just loaded is on disk, as it is - all but what the load
        // made up: a first start's folder and canvas, or the "Recovered"
        // one a rescue with nowhere to go gets, which is on disk only once
        // a save has written it.
        durable_.clear();
        if (loadedAnything) {
            const std::set<uint64_t> onDisk = IdsOnDisk();
            for (const uint64_t id : AllIds()) {
                if (onDisk.count(id) > 0) {
                    durable_.insert(id);
                }
            }
        }
    }

    // Every uid a directory in the tree is named after.
    std::set<uint64_t> IdsOnDisk() {
        std::set<uint64_t> ids;
        for (const auto& [path, text] : disk_.FilesUnder(Root() / "folders")) {
            (void)text;
            for (const std::filesystem::path& part : path.lexically_relative(Root() / "folders")) {
                const std::string name = part.string();
                if (const size_t dash = name.rfind('-'); dash != std::string::npos) {
                    if (const std::optional<uint64_t> uid = ParseUid(name.substr(dash + 1))) {
                        ids.insert(*uid);
                    }
                }
            }
        }
        return ids;
    }

    void Step() {
        const int kind = std::uniform_int_distribution<int>(0, 99)(random_);
        if (kind < 12) {
            AddSnippet();
        } else if (kind < 16) {
            const FolderId folder = Manager().AddFolder("Folder " + std::to_string(++names_));
            Note("add folder " + FormatUid(folder));
        } else if (kind < 22) {
            const std::vector<uint64_t> folders = FolderIds();
            if (!folders.empty()) {
                Manager().SwitchToFolder(folders[Pick(folders.size())]);
            }
            const CanvasId canvas = Manager().AddCanvas("Canvas " + std::to_string(++names_));
            Note("add canvas " + FormatUid(canvas));
        } else if (kind < 32) {
            EditSnippet();
        } else if (kind < 42) {
            MoveSnippet();
        } else if (kind < 46) {
            MoveCanvas();
        } else if (kind < 50) {
            Rename();
        } else if (kind < 56) {
            MarkDeleted();
        } else if (kind < 59) {
            RestoreOne();
        } else if (kind < 66) {
            DeleteForGood();
        } else if (kind < 76) {
            Save();
        } else if (kind < 82) {
            HoldAFile();
        } else if (kind < 86) {
            faulty_.ReleaseAll();
            Note("release every file");
        } else if (kind < 91) {
            FailSomething();
        } else if (kind < 95) {
            Crash();
        } else {
            Restart();
        }
    }

    void AddSnippet() {
        const std::vector<uint64_t> canvases = CanvasIds();
        if (!canvases.empty()) {
            Manager().SwitchToCanvas(canvases[Pick(canvases.size())]);
        }
        const bool withPicture = Chance(50);
        const ItemId id =
            session_->CreateItem(withPicture, Rect{0.0f, 0.0f, 2.0f, 2.0f}, "Snippet " + std::to_string(++names_));
        if (id == 0) {
            return;
        }
        if (withPicture) {
            const std::vector<uint8_t> pixels(2 * 2 * 4, static_cast<uint8_t>(names_));
            if (const std::optional<std::string> file = store_->SaveImage(id, pixels.data(), 2, 2)) {
                Manager().FindItemAnywhere(id)->ImageLayer()->imageFile = *file;
                Manager().MarkChanged();
            }
        }
        Note("add snippet " + FormatUid(id) + (withPicture ? " with a picture" : ""));
    }

    void EditSnippet() {
        const std::vector<uint64_t> items = ItemIds();
        if (items.empty()) {
            return;
        }
        Item* item = Manager().FindItemAnywhere(items[Pick(items.size())]);
        Stroke stroke;
        stroke.points = {StrokePoint{0, 0}, StrokePoint{1, 1}};
        item->strokes.push_back(stroke);
        Manager().MarkChanged();
        Note("draw on " + FormatUid(item->id));
    }

    void MoveSnippet() {
        const std::vector<uint64_t> items = ItemIds();
        const std::vector<uint64_t> canvases = CanvasIds();
        if (items.empty() || canvases.empty()) {
            return;
        }
        const uint64_t item = items[Pick(items.size())];
        const uint64_t canvas = canvases[Pick(canvases.size())];
        Manager().PlaceItemOnCanvas(item, canvas, /*copy=*/false);
        Note("move snippet " + FormatUid(item) + " to " + FormatUid(canvas));
    }

    void MoveCanvas() {
        const std::vector<uint64_t> canvases = CanvasIds();
        const std::vector<uint64_t> folders = FolderIds();
        if (canvases.empty() || folders.empty()) {
            return;
        }
        const uint64_t canvas = canvases[Pick(canvases.size())];
        const uint64_t folder = folders[Pick(folders.size())];
        Manager().MoveCanvasToFolder(canvas, folder);
        Note("move canvas " + FormatUid(canvas) + " to " + FormatUid(folder));
    }

    void Rename() {
        const std::string name = "Renamed " + std::to_string(++names_);
        if (Chance(50)) {
            const std::vector<uint64_t> canvases = CanvasIds();
            if (!canvases.empty()) {
                const uint64_t canvas = canvases[Pick(canvases.size())];
                Manager().RenameCanvas(canvas, name);
                Note("rename canvas " + FormatUid(canvas));
            }
        } else {
            const std::vector<uint64_t> folders = FolderIds();
            if (!folders.empty()) {
                const uint64_t folder = folders[Pick(folders.size())];
                Manager().RenameFolder(folder, name);
                Note("rename folder " + FormatUid(folder));
            }
        }
    }

    void MarkDeleted() {
        const std::vector<uint64_t> ids = AllIds();
        if (ids.empty()) {
            return;
        }
        const uint64_t id = ids[Pick(ids.size())];
        if (session_->Delete(id)) {
            Note("delete " + FormatUid(id));
        }
    }

    void RestoreOne() {
        const std::vector<uint64_t> ids = AllIds();
        if (ids.empty()) {
            return;
        }
        const uint64_t id = ids[Pick(ids.size())];
        if (session_->Restore(id)) {
            Note("restore " + FormatUid(id));
        }
    }

    void DeleteForGood() {
        const std::vector<uint64_t> ids = AllIds();
        if (ids.empty()) {
            return;
        }
        const uint64_t id = ids[Pick(ids.size())];
        const std::vector<uint64_t> going = WithEverythingUnder(id);
        const bool crashedBefore = faulty_.Crashed();
        const Session::Removal removal = session_->DeletePermanently(id);
        if (removal == Session::Removal::NotFound) {
            return;
        }
        // Certain only if the process was still writing throughout: a crash
        // before the removal was recorded leaves it as if never asked for.
        const bool certain = !crashedBefore && !faulty_.Crashed();
        for (const uint64_t gone : going) {
            durable_.erase(gone);
            if (certain) {
                erased_.insert(gone);
            }
        }
        Note("delete for good " + FormatUid(id) + " (" + std::to_string(going.size()) + ")" +
             (removal == Session::Removal::FilesRemain ? ", files remain" : "") + (certain ? "" : ", crashed"));
    }

    void Save() {
        // A flush with nothing unsaved writes nothing - a first start's
        // library is not on disk for being flushed.
        const bool hadChanges = session_->HasUnsavedChanges();
        const bool saved = session_->Flush();
        if (hadChanges && saved && !faulty_.Crashed()) {
            durable_.clear();
            for (const uint64_t id : AllIds()) {
                durable_.insert(id);
            }
        }
        Note(saved ? "save" : "save, failed");
    }

    void HoldAFile() {
        std::vector<std::filesystem::path> files;
        for (const auto& [path, text] : disk_.FilesUnder(Root() / "folders")) {
            (void)text;
            // Pictures mostly - a viewer looking at a capture - and now and
            // then a record.
            if (path.extension() == ".qoi" || Chance(10)) {
                files.push_back(path);
            }
        }
        if (files.empty()) {
            return;
        }
        const std::filesystem::path held = files[Pick(files.size())];
        faulty_.Hold(held);
        Note("hold " + held.lexically_relative(Root()).generic_string());
    }

    void FailSomething() {
        using Op = FaultyFileSystem::Op;
        const Op ops[] = {Op::MakeDirectory, Op::WriteNewFile, Op::Rename, Op::Remove, Op::List, Op::Read};
        const Op op = ops[Pick(std::size(ops))];
        const std::vector<uint64_t> ids = AllIds();
        const std::string uid = ids.empty() ? std::string() : FormatUid(ids[Pick(ids.size())]);
        const int times = 1 + static_cast<int>(Pick(3));
        // Never pending.json: a removal that cannot be recorded is owed in
        // memory only, and deliberately not what these runs judge.
        faulty_.FailWhen(op, [uid](const std::filesystem::path& path) {
            const std::string text = path.generic_string();
            return text.find("pending.json") == std::string::npos && text.find(uid) != std::string::npos;
        }, times);
        Note("fail " + std::to_string(static_cast<int>(op)) + " on " + uid + " x" + std::to_string(times));
    }

    void Crash() {
        const size_t after = Pick(12);
        faulty_.CrashAfter(after);
        Note("crash after " + std::to_string(after) + " more changes");
    }

    void Restart() {
        Note(faulty_.Crashed() ? "restart after the crash" : "restart");
        session_->SetLibraryStore(nullptr);
        session_.reset();
        store_.reset();
        faulty_.ClearCrash();
        faulty_.ClearFailures();  // passing trouble passes; what is held stays held
        CheckRestart();
        Start();
    }

    // What any restart may find, whatever happened before it.
    void CheckRestart() {
        LibraryStore store(Root(), faulty_);
        const std::optional<CanvasManagerSnapshot> loaded = store.Load();
        if (!loaded) {
            EXPECT_TRUE(durable_.empty()) << "nothing loaded, and something was saved";
            return;
        }
        std::multiset<uint64_t> ids;
        for (const Folder& folder : loaded->folders) {
            ids.insert(folder.id);
        }
        for (const Canvas& canvas : loaded->canvases) {
            ids.insert(canvas.id);
            for (const Item& item : canvas.items) {
                ids.insert(item.id);
                for (const Layer& layer : item.layers) {
                    if (!layer.imageFile.empty()) {
                        EXPECT_TRUE(store.LoadImage(item.id, layer.imageFile).has_value())
                            << FormatUid(item.id) << " names " << layer.imageFile << ", which does not load";
                    }
                }
            }
        }
        const Layout layout = LayoutOf(loaded->folders, loaded->canvases);
        for (const uint64_t id : ids) {
            EXPECT_EQ(ids.count(id), 1u) << FormatUid(id) << " loaded more than once";
        }
        for (const uint64_t id : erased_) {
            EXPECT_EQ(ids.count(id), 0u) << FormatUid(id) << " was deleted for good and loaded again:\n"
                                         << Describe(layout);
        }
        for (const uint64_t id : durable_) {
            EXPECT_EQ(ids.count(id), 1u) << FormatUid(id) << " was saved and is lost:\n"
                                         << Describe(layout) << "  on disk:\n" << DiskListing();
        }
    }

    std::string DiskListing() {
        std::string out;
        for (const auto& [path, text] : disk_.FilesUnder(Root())) {
            out += "    " + path.lexically_relative(Root()).generic_string();
            if (path.filename() == "pending.json") {
                out += " " + text;
            }
            out += "\n";
        }
        return out;
    }

    // Everything let go of, a restart, two saves - and the disk agrees with
    // the library.
    void Finish() {
        faulty_.ReleaseAll();
        Restart();
        if (::testing::Test::HasFailure()) {
            return;
        }
        EXPECT_TRUE(session_->Flush());
        EXPECT_TRUE(session_->Flush());
        EXPECT_FALSE(store_->HasPendingRemovals()) << "a removal still owed with nothing in the way";
        EXPECT_EQ(disk_.Status(Root() / "pending.json"), FileSystem::Kind::None)
            << "pending.json still says:\n"
            << disk_.Read(Root() / "pending.json", 1 << 20).value_or("(unreadable)");
        for (const auto& [path, text] : disk_.FilesUnder(Root())) {
            (void)text;
            for (const std::filesystem::path& part : path) {
                const std::string name = part.string();
                const size_t dash = name.rfind('-');
                if (dash == std::string::npos) {
                    continue;
                }
                const std::optional<uint64_t> uid = ParseUid(name.substr(dash + 1));
                EXPECT_FALSE(uid && erased_.count(*uid) > 0)
                    << path << " is left of " << name << ", deleted for good";
            }
        }
        const Layout want = LayoutOf(Manager().View().folders, Manager().View().canvases);
        LibraryStore reopened(Root(), disk_);
        const std::optional<CanvasManagerSnapshot> loaded = reopened.Load();
        ASSERT_TRUE(loaded.has_value());
        const Layout got = LayoutOf(loaded->folders, loaded->canvases);
        EXPECT_EQ(got, want) << "the library:\n" << Describe(want) << "the disk:\n" << Describe(got);
    }

    std::mt19937 random_;
    int steps_;
    MemoryFileSystem disk_;
    FaultyFileSystem faulty_;
    std::unique_ptr<LibraryStore> store_;
    std::unique_ptr<Session> session_;

    // Deleted for good for certain: never to load again.
    std::set<uint64_t> erased_;
    // On disk at the last save that landed, or the last load: never to be
    // lost, unless deleted for good since.
    std::set<uint64_t> durable_;
    int names_ = 0;
    std::vector<std::string> log_;
};

TEST(LibraryRandomTest, NothingIsLostOrBroughtBackWhateverTheDiskDoes) {
    const int seeds = FromEnvironment("SPICKZETTEL_RANDOM_SEEDS", 25);
    const int steps = FromEnvironment("SPICKZETTEL_RANDOM_STEPS", 150);
    const int only = FromEnvironment("SPICKZETTEL_RANDOM_SEED", 0);  // one seed, to run a failure again
    for (int seed = only > 0 ? only : 1; seed <= (only > 0 ? only : seeds) && !::testing::Test::HasFailure();
         ++seed) {
        RandomRun run(static_cast<uint32_t>(seed), steps);
        run.Run();
        if (::testing::Test::HasFailure()) {
            ADD_FAILURE() << "seed " << seed << ", steps:\n" << run.Log();
        }
    }
}

}  // namespace
}  // namespace sz::core
