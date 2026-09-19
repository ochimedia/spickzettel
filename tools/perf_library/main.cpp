// Writes a synthetic Spickzettel library, for measuring how the overlay
// behaves against a known amount of drawing.
//
// Built through CanvasManager and LibraryStore rather than by emitting JSON
// directly, so a generated library is by construction exactly what the app
// itself would have written - if the on-disk format changes, this follows it
// without anyone remembering to update a second copy of the schema.
//
// The strokes are meant to be the shape a hand actually leaves: a long
// smooth path with a point every few pixels, which is what makes a drawing
// expensive (a hundred short straight lines with the same total ink costs a
// fraction of it). See docs/PERF.md for the scenarios this is used to build
// and what they are for.
//
//   perf_library --out <dir> [--canvases N] [--items N] [--strokes N]
//                [--points N] [--width W] [--height H] [--seed N]
//
// `--width/--height` are the display the layout is meant for, so the items
// land where they would if someone had drawn them on that screen.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>

#include "core/canvas/canvas_manager.h"
#include "core/persistence/library_store.h"

namespace {

struct Options {
    std::filesystem::path out;
    int canvases = 1;
    int items = 11;
    int strokes = 13;
    int points = 130;
    float displayW = 1920.0f;
    float displayH = 1080.0f;
    unsigned seed = 1;
};

bool ParseArgs(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        const bool hasValue = i + 1 < argc;
        const auto value = [&]() { return std::string(argv[++i]); };
        if (flag == "--out" && hasValue) {
            options.out = value();
        } else if (flag == "--canvases" && hasValue) {
            options.canvases = std::atoi(value().c_str());
        } else if (flag == "--items" && hasValue) {
            options.items = std::atoi(value().c_str());
        } else if (flag == "--strokes" && hasValue) {
            options.strokes = std::atoi(value().c_str());
        } else if (flag == "--points" && hasValue) {
            options.points = std::atoi(value().c_str());
        } else if (flag == "--width" && hasValue) {
            options.displayW = static_cast<float>(std::atof(value().c_str()));
        } else if (flag == "--height" && hasValue) {
            options.displayH = static_cast<float>(std::atof(value().c_str()));
        } else if (flag == "--seed" && hasValue) {
            options.seed = static_cast<unsigned>(std::atoi(value().c_str()));
        } else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", flag.c_str());
            return false;
        }
    }
    return !options.out.empty();
}

// One scribble in the item's own native space: a wandering path sampled
// densely, the way a pen event stream arrives. Built from a few summed sine
// terms rather than a random walk so it stays inside the item and looks like
// handwriting rather than noise - and so the same seed gives the same
// drawing on every machine, which is what makes two measurements
// comparable.
sz::core::Stroke MakeScribble(std::mt19937& random, float nativeW, float nativeH, int points) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    sz::core::Stroke stroke;
    stroke.width = 2.0f + unit(random) * 4.0f;
    const uint32_t palette[] = {0xE84A5FFFu, 0x2A9D8FFFu, 0xE9C46AFFu, 0x264653FFu, 0xF4A261FFu};
    stroke.colorRGBA = palette[random() % (sizeof(palette) / sizeof(palette[0]))];

    const float cx = nativeW * (0.25f + unit(random) * 0.5f);
    const float cy = nativeH * (0.25f + unit(random) * 0.5f);
    const float rx = nativeW * (0.12f + unit(random) * 0.22f);
    const float ry = nativeH * (0.12f + unit(random) * 0.22f);
    const float phase = unit(random) * 6.283f;
    const float turns = 1.5f + unit(random) * 3.0f;
    const float wobbleFreq = 3.0f + unit(random) * 6.0f;
    const float wobbleAmp = 0.10f + unit(random) * 0.18f;

    stroke.points.reserve(static_cast<size_t>(points));
    for (int i = 0; i < points; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(points - 1);
        const float angle = phase + t * turns * 6.283f;
        const float wobble = 1.0f + wobbleAmp * std::sin(angle * wobbleFreq);
        stroke.points.push_back(sz::core::StrokePoint{cx + std::cos(angle) * rx * wobble,
                                                       cy + std::sin(angle) * ry * wobble});
    }
    return stroke;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseArgs(argc, argv, options)) {
        std::fprintf(stderr,
                     "usage: perf_library --out <dir> [--canvases N] [--items N] [--strokes N]\n"
                     "                    [--points N] [--width W] [--height H] [--seed N]\n");
        return 2;
    }

    std::mt19937 random(options.seed);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    // A fresh manager already holds one folder with one empty canvas in it -
    // the state a first run starts from. Everything below is added into that
    // folder and the empty canvas is deleted at the end, so a generated
    // library has exactly the canvases asked for and no stray one beside
    // them. (Deleted at the end rather than up front: a folder is allowed to
    // hold no canvases, but deleting the only canvas in the library first
    // would leave nothing current to add to.)
    sz::core::CanvasManager manager;
    sz::core::CanvasId defaultCanvas = manager.CurrentCanvasId();

    // An item roughly a third of the screen across, which is the size a
    // snippet tends to end up at, and enough of them to overlap the way a
    // working canvas does.
    const float itemW = options.displayW * 0.28f;
    const float itemH = options.displayH * 0.38f;

    sz::core::CanvasId firstCanvas = 0;
    for (int c = 0; c < options.canvases; ++c) {
        const sz::core::CanvasId canvas = manager.AddCanvas("Canvas " + std::to_string(c + 1));
        if (c == 0) {
            firstCanvas = canvas;
        }
        manager.SwitchToCanvas(canvas);

        for (int n = 0; n < options.items; ++n) {
            const float x = unit(random) * std::max(1.0f, options.displayW - itemW);
            const float y = unit(random) * std::max(1.0f, options.displayH - itemH);
            const sz::core::ItemId id = manager.CreateItem(
                /*hasBackground=*/false, sz::core::Rect{x, y, itemW, itemH}, "Drawing " + std::to_string(n + 1));

            sz::core::Item* item = manager.FindItemAnywhere(id);
            if (item == nullptr) {
                std::fprintf(stderr, "could not find the item just created\n");
                return 1;
            }
            // Strokes live in the item's own native space, so this has to be
            // set before they are made - see Item::nativeW.
            item->nativeW = itemW;
            item->nativeH = itemH;
            item->anchorRect = item->rect;
            item->anchorDisplayWidth = options.displayW;
            item->anchorDisplayHeight = options.displayH;
            for (int s = 0; s < options.strokes; ++s) {
                item->strokes.push_back(MakeScribble(random, itemW, itemH, options.points));
            }
        }
    }
    if (defaultCanvas != 0) {
        manager.DeleteCanvas(defaultCanvas);
        defaultCanvas = 0;
    }
    if (firstCanvas != 0) {
        manager.SwitchToCanvas(firstCanvas);
    }

    std::error_code ec;
    std::filesystem::create_directories(options.out, ec);
    const sz::core::persistence::LibraryStore store{options.out};
    if (!store.Save(manager.ExportSnapshot())) {
        std::fprintf(stderr, "could not write the library to %s\n", options.out.string().c_str());
        return 1;
    }

    const long long totalStrokes =
        static_cast<long long>(options.canvases) * options.items * options.strokes;
    std::printf("wrote %s: %d canvas(es) x %d items x %d strokes x %d points = %lld strokes, %lld points\n",
                options.out.string().c_str(), options.canvases, options.items, options.strokes, options.points,
                totalStrokes, totalStrokes * options.points);
    return 0;
}
