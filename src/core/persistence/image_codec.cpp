#include "core/persistence/image_codec.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Only QOI's memory-to-memory half is used: pictures are stored in the
// library, not in files of their own.
#define QOI_NO_STDIO
#define QOI_IMPLEMENTATION
#include <qoi.h>

namespace sz::core::persistence {

namespace {

// Whether a picture of this size is one this app will decode - see
// kMaxImageExtent. Checked against the header, before the decoder is
// given a chance to allocate for it.
bool WithinPixelBudget(uint64_t width, uint64_t height) {
    return width >= 1 && height >= 1 && width <= static_cast<uint64_t>(kMaxImageExtent) &&
           height <= static_cast<uint64_t>(kMaxImageExtent) && width * height <= kMaxImagePixels;
}

uint32_t ReadBigEndian32(const uint8_t* bytes) {
    return (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) | (uint32_t{bytes[2]} << 8) | uint32_t{bytes[3]};
}

}  // namespace

DecodedImage DownscaleToFit(const uint8_t* pixelsRGBA, int width, int height, int maxExtent) {
    DecodedImage out;
    if (pixelsRGBA == nullptr || width <= 0 || height <= 0) {
        return out;
    }
    const int longEdge = std::max(width, height);
    if (longEdge <= maxExtent || maxExtent <= 0) {
        out.width = width;
        out.height = height;
        out.pixelsRGBA.assign(pixelsRGBA, pixelsRGBA + static_cast<size_t>(width) * height * 4);
        return out;
    }
    const float scale = static_cast<float>(maxExtent) / static_cast<float>(longEdge);
    out.width = std::max(1, static_cast<int>(std::lround(width * scale)));
    out.height = std::max(1, static_cast<int>(std::lround(height * scale)));
    out.pixelsRGBA.assign(static_cast<size_t>(out.width) * out.height * 4, 0);

    // At most this many samples per axis out of each source cell. A true
    // box filter reads every source pixel, which at 1920x1080 -> 256 is 2M
    // reads for a 40 KB result, paid on the render thread right after a
    // capture. Sampling a 4x4 spread of each cell instead is a quarter of
    // the work and, at these reductions, indistinguishable in the tile it
    // ends up in - while still being an *average*, which is the part that
    // matters: point-sampling a screenshot breaks text and thin lines up
    // into noise.
    constexpr int kMaxSamplesPerAxis = 4;

    // The products are taken in 64 bits: an output index times a source
    // extent passes 2^31 well before either is unusual on its own.
    const auto sourceStart = [](int index, int sourceExtent, int outExtent) {
        return static_cast<int>(static_cast<int64_t>(index) * sourceExtent / outExtent);
    };
    for (int y = 0; y < out.height; ++y) {
        const int srcY0 = sourceStart(y, height, out.height);
        const int srcY1 = std::max(srcY0 + 1, sourceStart(y + 1, height, out.height));
        const int stepY = std::max(1, (srcY1 - srcY0 + kMaxSamplesPerAxis - 1) / kMaxSamplesPerAxis);
        for (int x = 0; x < out.width; ++x) {
            const int srcX0 = sourceStart(x, width, out.width);
            const int srcX1 = std::max(srcX0 + 1, sourceStart(x + 1, width, out.width));
            const int stepX = std::max(1, (srcX1 - srcX0 + kMaxSamplesPerAxis - 1) / kMaxSamplesPerAxis);
            uint32_t sums[4] = {0, 0, 0, 0};
            uint32_t count = 0;
            for (int sy = srcY0; sy < srcY1; sy += stepY) {
                for (int sx = srcX0; sx < srcX1; sx += stepX) {
                    const size_t i = (static_cast<size_t>(sy) * width + sx) * 4;
                    for (int c = 0; c < 4; ++c) {
                        sums[c] += pixelsRGBA[i + c];
                    }
                    ++count;
                }
            }
            const size_t dst = (static_cast<size_t>(y) * out.width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                out.pixelsRGBA[dst + c] = static_cast<uint8_t>(sums[c] / std::max(count, 1u));
            }
        }
    }
    return out;
}

DecodedImage DownscaleToFit(const DecodedImage& source, int maxExtent) {
    return DownscaleToFit(source.pixelsRGBA.data(), source.width, source.height, maxExtent);
}

std::vector<uint8_t> EncodeQoi(const uint8_t* pixelsRGBA, int width, int height) {
    if (!pixelsRGBA || width <= 0 || height <= 0) {
        return {};
    }
    const qoi_desc desc{static_cast<unsigned int>(width), static_cast<unsigned int>(height),
                        /*channels=*/4, QOI_SRGB};
    int encodedSize = 0;
    void* encoded = qoi_encode(pixelsRGBA, &desc, &encodedSize);
    if (!encoded || encodedSize <= 0) {
        free(encoded);  // qoi_encode uses QOI_MALLOC, which is malloc by default
        return {};
    }
    const auto* bytes = static_cast<const uint8_t*>(encoded);
    std::vector<uint8_t> result(bytes, bytes + encodedSize);
    free(encoded);
    return result;
}

std::optional<DecodedImage> DecodeQoi(const uint8_t* bytes, size_t size) {
    // The header first: "qoif", then width and height as big-endian 32-bit
    // integers. qoi_decode allocates width*height*4 on the strength of
    // those two numbers before it has looked at a single pixel.
    if (bytes == nullptr || size < QOI_HEADER_SIZE || size > kMaxImageFileBytes ||
        std::memcmp(bytes, "qoif", 4) != 0 ||
        !WithinPixelBudget(ReadBigEndian32(bytes + 4), ReadBigEndian32(bytes + 8))) {
        return std::nullopt;
    }
    qoi_desc desc{};
    void* decoded = qoi_decode(bytes, static_cast<int>(size), &desc, /*channels=*/4);
    if (!decoded) {
        return std::nullopt;
    }
    DecodedImage result;
    result.width = static_cast<int>(desc.width);
    result.height = static_cast<int>(desc.height);
    const auto* pixels = static_cast<const uint8_t*>(decoded);
    result.pixelsRGBA.assign(pixels, pixels + (static_cast<size_t>(desc.width) * desc.height * 4));
    free(decoded);
    return result;
}

}  // namespace sz::core::persistence
