// How long one frame of the overlay costs on the CPU, against a library of a
// known size. Opt-in: skipped unless SZ_PERF_LIBRARY names a library file
// (see tools/perf_library and docs/PERF.md).
//
// Why here rather than by timing the real app. The overlay presents on
// vsync, so wall-clock CPU sampling of the running process measures "how
// much of a 16.7 ms budget did it use" through two layers of noise - the
// scheduler, and a virtual GPU whose clock is not ours. Measured on this
// machine, repeated identical runs of the same scenario ranged from 15% to
// 33% of a core, which is uselessly wide for telling two builds apart.
//
// The headless harness runs the identical per-frame code path - NewFrame,
// OnFrame, Render, so the whole app plus all of ImGui's own work - with no
// vsync and no GPU in the way. What it does not measure is what the GPU then
// does with the draw lists; that is deliberate, because the CPU side is
// where the app's own decisions show up, and it is the half that a cache
// like StrokeMeshCache changes.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>

#include "core/persistence/library_store.h"
#include "fakes/headless_app.h"
#include "support/temp_dir.h"

namespace sz::test {
namespace {

class PerfBench : public test::HeadlessAppTest {
protected:
    // Median rather than mean: a frame that happened to land on a scheduler
    // hiccup should not move the number, and the question being asked is
    // what a typical frame costs.
    struct Timing {
        double medianMs = 0.0;
        double p95Ms = 0.0;
        double maxMs = 0.0;
        int vertices = 0;
        int indices = 0;
    };

    Timing TimeFrames(int frames) {
        std::vector<double> samples;
        samples.reserve(static_cast<size_t>(frames));
        Timing timing;
        for (int i = 0; i < frames; ++i) {
            const auto start = std::chrono::steady_clock::now();
            StepFrame();
            const auto end = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
            const ImDrawData* data = ImGui::GetDrawData();
            if (data != nullptr) {
                timing.vertices = data->TotalVtxCount;
                timing.indices = data->TotalIdxCount;
            }
        }
        std::sort(samples.begin(), samples.end());
        timing.medianMs = samples[samples.size() / 2];
        timing.p95Ms = samples[static_cast<size_t>(static_cast<double>(samples.size()) * 0.95)];
        timing.maxMs = samples.back();
        return timing;
    }
};

TEST_F(PerfBench, OneFrameAgainstAGeneratedLibrary) {
    const char* root = std::getenv("SZ_PERF_LIBRARY");
    if (root == nullptr || *root == '\0') {
        GTEST_SKIP() << "set SZ_PERF_LIBRARY to a library file (see tools/perf_library)";
    }
    // SZ_PERF_HW_POINTER=1 turns the software pointer off, which matters more
    // than it sounds: the app ships with it *on*, and with it on
    // ApplyPointerShape short-circuits to Default and WantedPointerShape - and
    // the item walk behind it - never runs at all. Measuring that path needs
    // this switch, or the numbers describe a branch that was never taken.
    const char* hardwarePointer = std::getenv("SZ_PERF_HW_POINTER");
    AppConfig config = DefaultConfig();
    bool restart = false;
    if (hardwarePointer != nullptr && std::string(hardwarePointer) == "1") {
        config.profileable.softwarePointer = false;
        restart = true;
    }
    if (restart) {
        StartWith(config);
    }

    persistence::LibraryStore store{std::filesystem::path(root)};
    const std::optional<CanvasManagerSnapshot> snapshot = store.Load();
    ASSERT_TRUE(snapshot.has_value()) << "no library at " << root;

    size_t items = 0;
    size_t strokes = 0;
    size_t points = 0;
    for (const Canvas& canvas : snapshot->canvases) {
        if (canvas.id != snapshot->currentCanvasId) {
            continue;
        }
        for (const Item& item : canvas.items) {
            ++items;
            strokes += item.strokes.size();
            for (const Stroke& stroke : item.strokes) {
                points += stroke.points.size();
            }
        }
    }

    // The generated libraries are laid out for a 1080p screen (see
    // tools/perf_library's --width/--height), and an item's rect is rescaled
    // to whatever display it finds - so matching it here keeps the drawing
    // the size it was meant to be rather than shrunk into the fixture's own
    // default, which would quietly change how much ink is on screen.
    ImGui::GetIO().DisplaySize = ImVec2(1920.0f, 1080.0f);

    ShowEditMode();
    controller_->GetSession().ImportLibrary(*snapshot);

    // Warm-up frames, then the measured ones. The first frames of any run
    // build the font atlas, take the first pass over the item list and fill
    // every cache there is - counting them would measure startup, which is
    // real but is not what a steady frame costs.
    StepFrames(30);
    const Timing timing = TimeFrames(240);

    std::printf("%-10s %-7s items=%-4zu strokes=%-5zu points=%-7zu | frame median=%.3f ms p95=%.3f max=%.3f "
                "| verts=%d tris=%d\n",
                std::filesystem::path(root).filename().string().c_str(),
                (hardwarePointer != nullptr && std::string(hardwarePointer) == "1") ? "hwPtr" : "swPtr", items, strokes, points, timing.medianMs,
                timing.p95Ms, timing.maxMs, timing.vertices, timing.indices / 3);
}

// What a command costs to write. Same opt-in as the frame benchmark, and the
// same generated libraries.
//
// Every change is written as it is made, on the render thread (see
// Session::Land), so these milliseconds are the frame the command lands
// in. Measured end to end through the session - the checkpoint taken before
// the command, the change, working out what changed, and the write - for a
// stroke added to a snippet (the heaviest ordinary command: the snippet's
// record and all of its strokes are written) and a canvas switch (the
// lightest: which canvas is current). Writing the whole library, which only
// a first run does, is reported beside them for scale.
TEST_F(PerfBench, WritingACommand) {
    const char* root = std::getenv("SZ_PERF_LIBRARY");
    if (root == nullptr || *root == '\0') {
        GTEST_SKIP() << "set SZ_PERF_LIBRARY to a library file (see tools/perf_library)";
    }
    persistence::LibraryStore source{std::filesystem::path(root)};
    std::optional<CanvasManagerSnapshot> loaded = source.Load();
    ASSERT_TRUE(loaded.has_value()) << "no library at " << root;

    // Written somewhere of its own, so the scenario library stays as it was.
    const std::filesystem::path out =
        sz::test::TempDir() / "sz_write_bench" / std::filesystem::path(root).filename();
    std::error_code ec;
    std::filesystem::remove(out, ec);
    persistence::LibraryStore store{out};
    const auto wholeStart = std::chrono::steady_clock::now();
    ASSERT_TRUE(store.Save(*loaded));
    const double wholeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wholeStart).count();

    core::Session session;
    session.SetLibraryStore(&store);
    session.ImportLibrary(std::move(*loaded));
    session.SyncItemsToDisplaySize(1920.0f, 1080.0f);
    // The canvas with the most snippets, current, and another to switch to.
    std::vector<const Canvas*> live;
    for (const Canvas& canvas : session.Manager().Canvases()) {
        if (!session.Manager().IsDeleted(canvas)) {
            live.push_back(&canvas);
        }
    }
    ASSERT_FALSE(live.empty());
    std::sort(live.begin(), live.end(), [](const Canvas* a, const Canvas* b) { return a->items.size() > b->items.size(); });
    ASSERT_FALSE(live[0]->items.empty());
    const CanvasId busiest = live[0]->id;
    // A library of one canvas is given another, empty, to switch to.
    const CanvasId other = live.size() > 1 ? live[1]->id : session.AddCanvas("Other");
    session.SwitchToCanvas(busiest);
    const ItemId target = session.Manager().CurrentOrNull()->items.front().id;

    constexpr int kRuns = 15;
    std::vector<double> strokeMs;
    std::vector<double> switchMs;
    for (int i = 0; i < kRuns; ++i) {
        const Rect rect = session.Manager().FindItemAnywhere(target)->rect;
        session.LiveLayer().BeginStroke(StrokePoint{rect.x + 2.0f, rect.y + 2.0f}, 0xFF0000FFu, 3.0f);
        session.LiveLayer().ExtendStroke(StrokePoint{rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f});
        session.LiveLayer().EndStroke();
        auto start = std::chrono::steady_clock::now();
        session.CommitLiveStroke(target);
        auto end = std::chrono::steady_clock::now();
        ASSERT_FALSE(session.LastWriteFailed());
        strokeMs.push_back(std::chrono::duration<double, std::milli>(end - start).count());

        start = std::chrono::steady_clock::now();
        session.SwitchToCanvas(other);
        end = std::chrono::steady_clock::now();
        switchMs.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        session.SwitchToCanvas(busiest);
    }
    std::sort(strokeMs.begin(), strokeMs.end());
    std::sort(switchMs.begin(), switchMs.end());
    const uintmax_t bytes = std::filesystem::file_size(out, ec);

    std::printf("%-10s %.0f KB | whole library %.2f ms | a stroke %.2f ms | a canvas switch %.2f ms\n",
                std::filesystem::path(root).filename().string().c_str(), static_cast<double>(bytes) / 1024.0, wholeMs,
                strokeMs[strokeMs.size() / 2], switchMs[switchMs.size() / 2]);
    session.SetLibraryStore(nullptr);
    std::filesystem::remove(out, ec);
}

}  // namespace
}  // namespace sz::test


