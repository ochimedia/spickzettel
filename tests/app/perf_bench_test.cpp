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
    // SZ_PERF_MODE picks the stroke renderer, so the three can be compared
    // against identical content. Default is what the app ships with.
    //
    // SZ_PERF_HW_POINTER=1 turns the software pointer off, which matters more
    // than it sounds: the app ships with it *on*, and with it on
    // ApplyPointerShape short-circuits to Default and WantedPointerShape - and
    // the item walk behind it - never runs at all. Measuring that path needs
    // this switch, or the numbers describe a branch that was never taken.
    const char* modeName = std::getenv("SZ_PERF_MODE");
    const char* hardwarePointer = std::getenv("SZ_PERF_HW_POINTER");
    AppConfig config = DefaultConfig();
    bool restart = false;
    if (hardwarePointer != nullptr && std::string(hardwarePointer) == "1") {
        config.editModeInput.useSoftwarePointer = false;
        restart = true;
    }
    if (modeName != nullptr) {
        const std::string mode = modeName;
        if (mode == "polyline") {
            config.strokeRenderMode = StrokeRenderMode::Polyline;
        } else if (mode == "rasterized") {
            config.strokeRenderMode = StrokeRenderMode::Rasterized;
        } else if (mode == "tessellated") {
            config.strokeRenderMode = StrokeRenderMode::Tessellated;
        } else {
            FAIL() << "SZ_PERF_MODE must be tessellated, polyline or rasterized";
        }
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

    std::printf("%-10s %-11s %-7s items=%-4zu strokes=%-5zu points=%-7zu | frame median=%.3f ms p95=%.3f max=%.3f "
                "| verts=%d tris=%d\n",
                std::filesystem::path(root).filename().string().c_str(),
                modeName != nullptr ? modeName : "tessellated",
                (hardwarePointer != nullptr && std::string(hardwarePointer) == "1") ? "hwPtr" : "swPtr", items, strokes, points, timing.medianMs,
                timing.p95Ms, timing.maxMs, timing.vertices, timing.indices / 3);
}

// What one autosave costs. Same opt-in as the frame benchmark, and the same
// generated libraries.
//
// Reported as two numbers, because they answer different questions and can
// move independently: what a save costs when nothing changed (the autosave
// that fires because something *somewhere* moved), and what it costs when one
// snippet did (a save two seconds after drawing a stroke). ExportSnapshot is
// timed apart from the write for the same reason.
//
// The save runs on the render thread - see Session::SaveLibraryNow - so
// these milliseconds are frames, not background work.
TEST_F(PerfBench, SavingTheWholeLibrary) {
    const char* root = std::getenv("SZ_PERF_LIBRARY");
    if (root == nullptr || *root == '\0') {
        GTEST_SKIP() << "set SZ_PERF_LIBRARY to a library file (see tools/perf_library)";
    }
    persistence::LibraryStore source{std::filesystem::path(root)};
    const std::optional<CanvasManagerSnapshot> loaded = source.Load();
    ASSERT_TRUE(loaded.has_value()) << "no library at " << root;

    CanvasManager manager;
    manager.ImportSnapshot(*loaded);

    // Written somewhere of its own, so the scenario library stays as it was
    // and the first save has the same work to do as the tenth.
    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "sz_save_bench" / std::filesystem::path(root).filename();
    std::error_code ec;
    std::filesystem::remove(out, ec);
    persistence::LibraryStore store{out};
    ASSERT_TRUE(store.Save(manager.ExportSnapshot()));

    // The first save of a session: a store that has only loaded, saving
    // what it loaded. It costs what the load repaired, which for a library
    // that needed no repair is nothing.
    persistence::LibraryStore reopened{out};
    ASSERT_TRUE(reopened.Load().has_value());
    const auto firstStart = std::chrono::steady_clock::now();
    ASSERT_TRUE(reopened.Save(manager.ExportSnapshot()));
    const double firstSaveMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - firstStart).count();

    constexpr int kRuns = 15;
    std::vector<double> exportMs;
    std::vector<double> idleMs;
    std::vector<double> oneItemMs;
    for (int i = 0; i < kRuns; ++i) {
        // Nothing changed - the autosave that fires because something
        // somewhere moved, on a library where most of it did not.
        auto start = std::chrono::steady_clock::now();
        const CanvasManagerSnapshot snapshot = manager.ExportSnapshot();
        auto mid = std::chrono::steady_clock::now();
        ASSERT_TRUE(store.Save(snapshot));
        auto end = std::chrono::steady_clock::now();
        exportMs.push_back(std::chrono::duration<double, std::milli>(mid - start).count());
        idleMs.push_back(std::chrono::duration<double, std::milli>(end - mid).count());

        // ...and the ordinary case: one stroke added to one snippet, which is
        // what a save two seconds after drawing actually has to record.
        Canvas* canvas = manager.CurrentOrNull();
        ASSERT_NE(canvas, nullptr);
        ASSERT_FALSE(canvas->items.empty());
        Stroke stroke;
        stroke.points = {StrokePoint{1.0f, 2.0f}, StrokePoint{3.0f, 4.0f}};
        canvas->items.front().strokes.push_back(stroke);
        start = std::chrono::steady_clock::now();
        ASSERT_TRUE(store.Save(manager.ExportSnapshot()));
        end = std::chrono::steady_clock::now();
        oneItemMs.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(exportMs.begin(), exportMs.end());
    std::sort(idleMs.begin(), idleMs.end());
    std::sort(oneItemMs.begin(), oneItemMs.end());
    const std::vector<double>& saveMs = idleMs;

    const uintmax_t bytes = std::filesystem::file_size(out, ec);

    std::printf("%-10s %.0f KB | ExportSnapshot %.2f ms | Save: first after load %.2f ms, "
                "nothing changed %.2f ms, one snippet changed %.2f ms\n",
                std::filesystem::path(root).filename().string().c_str(), static_cast<double>(bytes) / 1024.0,
                exportMs[exportMs.size() / 2], firstSaveMs, idleMs[idleMs.size() / 2],
                oneItemMs[oneItemMs.size() / 2]);
    (void)saveMs;
    std::filesystem::remove(out, ec);
}

}  // namespace
}  // namespace sz::test


