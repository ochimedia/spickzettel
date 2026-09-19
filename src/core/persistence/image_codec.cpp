#include "core/persistence/image_codec.h"

#include "core/util/atomic_file.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

// Only PNG is needed of stb: pictures are written as QOI and read back
// through DecodeImageFromFile, never sourced from arbitrary user files.
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// qoi.h's own file helpers (qoi_read/qoi_write) are fopen(const char*)
// based, which would mangle a data directory containing non-ASCII
// characters exactly the way stb_image_write's would (see WriteCallback
// below). Only the memory-to-memory half is used here, with std::ifstream/
// std::ofstream either side of it.
#define QOI_NO_STDIO
#define QOI_IMPLEMENTATION
#include <qoi.h>

namespace sz::core::persistence {

namespace {

// Routes stb_image_write's output through std::ofstream (which respects
// std::filesystem::path's native/wide representation on Windows) instead
// of the library's own fopen(narrow char*)-based file writer, which would
// mangle a data directory containing non-ASCII characters (e.g. a Windows
// username with accented/CJK characters in it - not unusual, and
// %APPDATA% sits under the user's profile directory).
void WriteCallback(void* context, void* data, int size) {
    auto* out = static_cast<std::ofstream*>(context);
    out->write(static_cast<const char*>(data), size);
}

// The whole file as bytes, or empty on any failure - shared by both
// decoders, which each want the file in memory before handing it to a
// memory-to-memory codec. The size is asked first and refused past the
// budget, so a stray multi-gigabyte file in a snippet's directory is not
// read into memory to find out what it is.
std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kMaxImageFileBytes) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (static_cast<uintmax_t>(in.gcount()) != size) {
        return {};
    }
    return bytes;
}

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

    for (int y = 0; y < out.height; ++y) {
        const int srcY0 = y * height / out.height;
        const int srcY1 = std::max(srcY0 + 1, (y + 1) * height / out.height);
        const int stepY = std::max(1, (srcY1 - srcY0 + kMaxSamplesPerAxis - 1) / kMaxSamplesPerAxis);
        for (int x = 0; x < out.width; ++x) {
            const int srcX0 = x * width / out.width;
            const int srcX1 = std::max(srcX0 + 1, (x + 1) * width / out.width);
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

bool EncodeQoiToFile(const std::filesystem::path& path, const uint8_t* pixelsRGBA, int width, int height) {
    if (!pixelsRGBA || width <= 0 || height <= 0) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    const qoi_desc desc{static_cast<unsigned int>(width), static_cast<unsigned int>(height),
                        /*channels=*/4, QOI_SRGB};
    int encodedSize = 0;
    void* encoded = qoi_encode(pixelsRGBA, &desc, &encodedSize);
    if (!encoded || encodedSize <= 0) {
        free(encoded);  // qoi_encode uses QOI_MALLOC, which is malloc by default
        return false;
    }

    // Written beside the destination and renamed onto it, the same
    // discipline every record gets (see core/util/atomic_file.h): a painted
    // layer is re-encoded over its own previous file every time it is
    // saved, and truncating that file in place left a window in which a
    // crash - or a full disk - replaced the only copy on disk of a drawing
    // with the first half of it.
    const bool ok = WriteFileAtomically(path, encoded, static_cast<size_t>(encodedSize));
    free(encoded);
    return ok;
}

std::optional<DecodedImage> DecodeQoiFromFile(const std::filesystem::path& path) {
    const std::vector<uint8_t> fileBytes = ReadFileBytes(path);
    if (fileBytes.empty()) {
        return std::nullopt;
    }

    // The header first: "qoif", then width and height as big-endian 32-bit
    // integers. qoi_decode allocates width*height*4 on the strength of
    // those two numbers before it has looked at a single pixel.
    if (fileBytes.size() < QOI_HEADER_SIZE || std::memcmp(fileBytes.data(), "qoif", 4) != 0 ||
        !WithinPixelBudget(ReadBigEndian32(fileBytes.data() + 4), ReadBigEndian32(fileBytes.data() + 8))) {
        return std::nullopt;
    }
    qoi_desc desc{};
    void* decoded = qoi_decode(fileBytes.data(), static_cast<int>(fileBytes.size()), &desc, /*channels=*/4);
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

std::optional<DecodedImage> DecodeImageFromFile(const std::filesystem::path& path) {
    // The format's own signature, not the extension - see the header. Both
    // are fixed-length and at offset 0: "qoif" for QOI, and PNG's own
    // 8-byte magic, of which the \x89PNG at the front is already unique
    // enough to dispatch on.
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    char magic[4] = {};
    in.read(magic, sizeof(magic));
    if (in.gcount() < static_cast<std::streamsize>(sizeof(magic))) {
        return std::nullopt;  // too short to be either
    }
    in.close();

    if (std::memcmp(magic, "qoif", 4) == 0) {
        return DecodeQoiFromFile(path);
    }
    if (std::memcmp(magic, "\x89PNG", 4) == 0) {
        return DecodePngFromFile(path);
    }
    return std::nullopt;
}

bool EncodePngToFile(const std::filesystem::path& path, const uint8_t* pixelsRGBA, int width, int height) {
    if (!pixelsRGBA || width <= 0 || height <= 0) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    const int ok = stbi_write_png_to_func(&WriteCallback, &out, width, height, /*channels=*/4, pixelsRGBA,
                                           /*strideBytes=*/width * 4);
    out.flush();
    return ok != 0 && out.good();
}

std::optional<DecodedImage> DecodePngFromFile(const std::filesystem::path& path) {
    const std::vector<uint8_t> fileBytes = ReadFileBytes(path);
    if (fileBytes.empty()) {
        return std::nullopt;
    }

    int width = 0;
    int height = 0;
    int sourceChannels = 0;
    // The header first, for the same reason as QOI's: the dimensions are
    // what the decoder allocates from.
    if (!stbi_info_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()), &width, &height,
                               &sourceChannels) ||
        !WithinPixelBudget(static_cast<uint64_t>(std::max(width, 0)), static_cast<uint64_t>(std::max(height, 0)))) {
        return std::nullopt;
    }
    uint8_t* decoded = stbi_load_from_memory(fileBytes.data(), static_cast<int>(fileBytes.size()), &width, &height,
                                              &sourceChannels, /*desiredChannels=*/4);
    if (!decoded) {
        return std::nullopt;
    }

    DecodedImage result;
    result.width = width;
    result.height = height;
    result.pixelsRGBA.assign(decoded, decoded + (static_cast<size_t>(width) * static_cast<size_t>(height) * 4));
    stbi_image_free(decoded);
    return result;
}

}  // namespace sz::core::persistence
