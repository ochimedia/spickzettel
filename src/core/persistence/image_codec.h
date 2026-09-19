#pragma once

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
// Shared by the sidecar thumbnails written beside each image (see
// LibraryStore::SaveThumbnail) and the Overview's own fallback for a
// library that has none yet, so both produce the same picture.
DecodedImage DownscaleToFit(const uint8_t* pixelsRGBA, int width, int height, int maxExtent);
DecodedImage DownscaleToFit(const DecodedImage& source, int maxExtent);

// Encodes `pixelsRGBA` as a QOI image and writes it to `path`, creating
// parent directories as needed. Returns false on any failure (bad input,
// encode error, can't write the file) - never throws. This is what a Shot
// item's captured screenshot is written with, synchronously at capture
// time (see Session::CaptureShotItem) rather than as part of the
// debounced library autosave - losing a few seconds of drawing to a crash
// is a much smaller deal than losing a screenshot that can never be
// recaptured.
//
// QOI rather than PNG because both halves of that sentence are measured
// costs. Encoding a 1920x1080 capture: 13ms here against 296ms for
// EncodePngToFile, paid on the spot every time a screenshot is taken.
// Decoding the same image: 7ms against 43ms, paid every time its canvas
// becomes current. Lossless either way - QOI is byte-exact, not a quality
// tradeoff - and the files come out ~30% smaller than stb's PNG besides.
// The cost is that a .qoi file opens in far fewer image viewers than a
// .png; see FetchQoi.cmake.
bool EncodeQoiToFile(const std::filesystem::path& path, const uint8_t* pixelsRGBA, int width, int height);

// Encodes `pixelsRGBA` as a PNG, same contract as EncodeQoiToFile above.
// Nothing in the app writes PNG today; it is the format anything else can
// open, and an export path is the obvious use for it.
bool EncodePngToFile(const std::filesystem::path& path, const uint8_t* pixelsRGBA, int width, int height);

// What a picture may be before this reads it, checked before anything is
// allocated for it: the file's size before it is read, the dimensions in
// its header before it is decoded. A library is a directory tree anyone
// can drop files into, and a header claiming 100000x100000 pixels asked
// for a 40 GB allocation before these existed. 16384 on a side and 64
// million pixels (an 8K display is 33 million) is well past any capture
// this app takes; 256 MB of file is past any picture those dimensions
// encode to.
constexpr int kMaxImageExtent = 16384;
constexpr uint64_t kMaxImagePixels = uint64_t{64} << 20;
constexpr uint64_t kMaxImageFileBytes = uint64_t{256} << 20;

// Reads and decodes a previously written image back into raw RGBA8 pixels
// - used to reload a persisted Shot item's image into a GPU texture (see
// IOverlayWindow::CreateTextureFromPixels). Returns nullopt if the file
// doesn't exist, isn't decodable, or is outside the budgets above - never
// throws.
//
// QOI and PNG both, told apart by the bytes at the front of the file
// rather than by the extension: a filename is a weaker claim about content
// than the content itself, and a snippet's directory may hold either.
std::optional<DecodedImage> DecodeImageFromFile(const std::filesystem::path& path);

// The single-format decoders behind DecodeImageFromFile. Prefer that one
// for anything read back from the library; these are for a caller that
// genuinely knows which format it has (and for testing each in isolation).
std::optional<DecodedImage> DecodeQoiFromFile(const std::filesystem::path& path);
std::optional<DecodedImage> DecodePngFromFile(const std::filesystem::path& path);

}  // namespace sz::core::persistence
