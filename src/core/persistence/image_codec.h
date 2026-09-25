#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace sz::core::persistence {

// width*height RGBA8 pixels, row-major, top-left origin, 4 bytes/pixel, no
// row padding - the same layout platform::CaptureResult::pixelsRGBA uses.
struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixelsRGBA;
};

// Downscale to fit within `maxExtent` on the longer edge, or a copy of the
// image unchanged if it already does. Each output pixel averages a spread
// of the source pixels it covers rather than picking one of them: the thing
// being shrunk here is a screenshot, which is exactly the case where
// dropping pixels shows - text and thin lines break up into noise. The
// number of samples per output pixel is capped, so shrinking a fullscreen
// capture doesn't have to read every one of its two million pixels; see the
// definition.
//
// Shared by the thumbnail stored with each picture (see
// LibraryStore::SaveImage) and the Overview's own fallback for a picture
// without one, so both produce the same picture.
DecodedImage DownscaleToFit(const uint8_t* pixelsRGBA, int width, int height, int maxExtent);
DecodedImage DownscaleToFit(const DecodedImage& source, int maxExtent);

// Encodes `pixelsRGBA` as a QOI image, or returns nothing on bad input or
// an encode error. What a snippet's picture is stored as (see
// LibraryStore::SaveImage), synchronously at capture time.
//
// QOI rather than PNG because both halves of that are measured costs.
// Encoding a 1920x1080 capture: 13ms here against 296ms for PNG, paid on
// the spot every time a screenshot is taken. Decoding the same image: 7ms
// against 43ms, paid every time its canvas becomes current. Lossless either
// way - QOI is byte-exact, not a quality tradeoff - and it comes out ~30%
// smaller than stb's PNG besides.
std::vector<uint8_t> EncodeQoi(const uint8_t* pixelsRGBA, int width, int height);

// Encodes `pixelsRGBA` as a PNG and writes it to `path`, creating parent
// directories as needed. False on any failure rather than throwing.
// Nothing in the app writes PNG today; it is the format anything else can
// open, and an export path is the obvious use for it.
bool EncodePngToFile(const std::filesystem::path& path, const uint8_t* pixelsRGBA, int width, int height);

// What a picture may be before this reads it, checked before anything is
// allocated for it: the file's size before it is read, the dimensions in
// its header before it is decoded. A header claiming 100000x100000 pixels
// asked for a 40 GB allocation before these existed. 16384 on a side and 64
// million pixels (an 8K display is 33 million) is well past any capture
// this app takes; 256 MB of file is past any picture those dimensions
// encode to.
constexpr int kMaxImageExtent = 16384;
constexpr uint64_t kMaxImagePixels = uint64_t{64} << 20;
constexpr uint64_t kMaxImageFileBytes = uint64_t{256} << 20;

// Decodes a QOI image back into raw RGBA8 pixels - a snippet's picture,
// for its GPU texture (see IOverlayWindow::CreateTextureFromPixels).
// Nullopt if it isn't QOI or is outside the budgets above - never throws.
std::optional<DecodedImage> DecodeQoi(const uint8_t* bytes, size_t size);

// Reads and decodes a PNG file, under the same budgets. The library holds
// none; like EncodePngToFile, it is here for importing pictures from
// elsewhere.
std::optional<DecodedImage> DecodePngFromFile(const std::filesystem::path& path);

}  // namespace sz::core::persistence
