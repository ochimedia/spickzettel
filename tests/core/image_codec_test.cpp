#include "core/persistence/image_codec.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace sz::core::persistence {
namespace {

// A small, distinctive 3x2 RGBA image - large enough that a channel-order
// or stride bug would show up as a wrong pixel somewhere, small enough to
// write out by hand.
std::vector<uint8_t> SamplePixels() {
    return {
        255, 0,   0,   255,  // red
        0,   255, 0,   200,  // green, translucent
        0,   0,   255, 255,  // blue
        255, 255, 0,   255,  // yellow
        0,   255, 255, 128,  // cyan, half-transparent
        255, 0,   255, 255,  // magenta
    };
}

// Lossless is the whole reason QOI was picked over anything cheaper: the
// pixels that come back have to be the ones that went in, alpha included
// (SamplePixels carries two translucent pixels for exactly this).
TEST(ImageCodecTest, QoiEncodeThenDecodeRoundTripsPixelsExactly) {
    const std::vector<uint8_t> pixels = SamplePixels();
    const std::vector<uint8_t> encoded = EncodeQoi(pixels.data(), /*width=*/3, /*height=*/2);
    ASSERT_FALSE(encoded.empty());

    const std::optional<DecodedImage> decoded = DecodeQoi(encoded.data(), encoded.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 3);
    EXPECT_EQ(decoded->height, 2);
    EXPECT_EQ(decoded->pixelsRGBA, pixels);
}

// A run of identical pixels takes QOI's run op, a repeat takes its index
// op, and a small step takes its diff op - so an image made only of the
// 3x2 sample would never exercise them. This one is wide enough to.
TEST(ImageCodecTest, QoiRoundTripsRunsAndRepeatsExactly) {
    std::vector<uint8_t> pixels;
    for (int i = 0; i < 64; ++i) {
        const uint8_t v = static_cast<uint8_t>((i / 8) * 3);  // eight-long runs, one small step apart
        pixels.insert(pixels.end(), {v, static_cast<uint8_t>(255 - v), 128, 255});
    }
    const std::vector<uint8_t> encoded = EncodeQoi(pixels.data(), /*width=*/8, /*height=*/8);
    const std::optional<DecodedImage> decoded = DecodeQoi(encoded.data(), encoded.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->pixelsRGBA, pixels);
}

TEST(ImageCodecTest, QoiDecodeReturnsNulloptForUnknownContent) {
    const std::string garbage = "not a qoi, just some bytes";
    EXPECT_FALSE(DecodeQoi(reinterpret_cast<const uint8_t*>(garbage.data()), garbage.size()).has_value());
    EXPECT_FALSE(DecodeQoi(nullptr, 0).has_value());
}

TEST(ImageCodecTest, QoiEncodeRejectsInvalidDimensions) {
    const std::vector<uint8_t> pixels = SamplePixels();
    EXPECT_TRUE(EncodeQoi(pixels.data(), 0, 2).empty());
    EXPECT_TRUE(EncodeQoi(pixels.data(), 3, -1).empty());
    EXPECT_TRUE(EncodeQoi(nullptr, 3, 2).empty());
}

// A header is a claim, and the decoder allocates on the strength of it.
// One claiming more than any capture could be is refused before that.
TEST(ImageCodecTest, DecodeRefusesAHeaderClaimingMoreThanTheBudget) {
    std::vector<uint8_t> huge = {'q', 'o', 'i', 'f', 0x00, 0x01, 0x86, 0xA0,  // 100000 wide
                                 0x00, 0x01, 0x86, 0xA0,                       // 100000 high
                                 4,    0};
    huge.resize(huge.size() + 64, 0);
    EXPECT_FALSE(DecodeQoi(huge.data(), huge.size()).has_value());
}

TEST(ImageCodecTest, TheBudgetEndsAtTheLongestSideAndThePixelCount) {
    EXPECT_TRUE(WithinImageBudget(kMaxImageExtent, 1));
    EXPECT_TRUE(WithinImageBudget(kMaxImageExtent, kMaxImagePixels / kMaxImageExtent));
    EXPECT_FALSE(WithinImageBudget(kMaxImageExtent, kMaxImagePixels / kMaxImageExtent + 1));
    EXPECT_FALSE(WithinImageBudget(kMaxImageExtent + 1, 1));
    EXPECT_FALSE(WithinImageBudget(1, kMaxImageExtent + 1));
    EXPECT_FALSE(WithinImageBudget(0, 1));
    EXPECT_FALSE(WithinImageBudget(1, -1));
}

// What the encoder writes, the decoder reads: an image at the edge of the
// budget goes both ways, and one past it is not written at all - rather
// than written and then never read again.
TEST(ImageCodecTest, AnImageIsWrittenOnlyIfItIsReadBack) {
    std::vector<uint8_t> row(static_cast<size_t>(kMaxImageExtent + 1) * 4, 200);
    const std::vector<uint8_t> atTheEdge = EncodeQoi(row.data(), kMaxImageExtent, 1);
    ASSERT_FALSE(atTheEdge.empty());
    const std::optional<DecodedImage> decoded = DecodeQoi(atTheEdge.data(), atTheEdge.size());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, kMaxImageExtent);

    EXPECT_TRUE(EncodeQoi(row.data(), kMaxImageExtent + 1, 1).empty());
}

// The file-size limit is the encoder's own worst case - every pixel a full
// five-byte RGBA chunk - taken to the most pixels the budget allows, so a
// picture within the budget is never refused for the size it came out at.
TEST(ImageCodecTest, TheFileLimitIsTheEncodersWorstCaseForTheBudget) {
    constexpr int kPixels = 64;
    std::vector<uint8_t> pixels;
    for (int i = 0; i < kPixels; ++i) {
        // No two alike and the alpha always changing: no run, no index
        // hit, no difference chunk - each one written in full. Not from
        // zero: the index starts out all zeros, which a first pixel of
        // zeros would be found in.
        const auto v = static_cast<uint8_t>(i + 1);
        pixels.insert(pixels.end(), {v, static_cast<uint8_t>(v * 7), static_cast<uint8_t>(v * 13), v});
    }
    const std::vector<uint8_t> encoded = EncodeQoi(pixels.data(), kPixels, 1);
    const size_t framing = 14 + 8;  // header and end marker
    ASSERT_EQ(encoded.size(), kPixels * 5 + framing);
    EXPECT_EQ(kMaxImageFileBytes, kMaxImagePixels * 5 + framing);
}

// ===== DownscaleToFit =====

DecodedImage SolidImage(int width, int height, uint8_t value) {
    DecodedImage image;
    image.width = width;
    image.height = height;
    image.pixelsRGBA.assign(static_cast<size_t>(width) * height * 4, value);
    return image;
}

TEST(DownscaleToFitTest, LeavesAnImageThatAlreadyFitsAlone) {
    const DecodedImage source = SolidImage(40, 20, 77);
    const DecodedImage out = DownscaleToFit(source, 256);
    EXPECT_EQ(out.width, 40);
    EXPECT_EQ(out.height, 20);
    EXPECT_EQ(out.pixelsRGBA, source.pixelsRGBA);
}

TEST(DownscaleToFitTest, FitsTheLongEdgeAndKeepsTheAspect) {
    const DecodedImage out = DownscaleToFit(SolidImage(1920, 1080, 200), 256);
    EXPECT_EQ(out.width, 256);
    EXPECT_EQ(out.height, 144);
    EXPECT_EQ(out.pixelsRGBA.size(), static_cast<size_t>(256) * 144 * 4);
}

TEST(DownscaleToFitTest, AveragesRatherThanPickingOnePixel) {
    // Half black, half white in 2px vertical stripes: any single sample is
    // 0 or 255, and only an average lands in the middle. This is the
    // difference between a legible thumbnail of a screenshot and one where
    // text has broken up into noise.
    DecodedImage source = SolidImage(512, 8, 0);
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const uint8_t value = (x % 4) < 2 ? 0 : 255;
            const size_t i = (static_cast<size_t>(y) * source.width + x) * 4;
            source.pixelsRGBA[i] = value;
            source.pixelsRGBA[i + 1] = value;
            source.pixelsRGBA[i + 2] = value;
            source.pixelsRGBA[i + 3] = 255;
        }
    }
    const DecodedImage out = DownscaleToFit(source, 64);
    ASSERT_EQ(out.width, 64);
    for (size_t i = 0; i < out.pixelsRGBA.size(); i += 4) {
        EXPECT_NEAR(out.pixelsRGBA[i], 128, 64);
        EXPECT_EQ(out.pixelsRGBA[i + 3], 255);
    }
}

// An output index times the source width passes 2^31 at x = 32768 for a
// source this narrow (256 KB of pixels). Taken in 32 bits, the last output
// pixel would read from before the buffer; it should average the last two
// source pixels like any other.
TEST(DownscaleToFitTest, AWideImageIsSampledWithoutTheProductOverflowing) {
    DecodedImage source = SolidImage(65536, 1, 0);
    source.pixelsRGBA[source.pixelsRGBA.size() - 8] = 100;
    source.pixelsRGBA[source.pixelsRGBA.size() - 4] = 200;

    const DecodedImage out = DownscaleToFit(source, 32769);

    ASSERT_EQ(out.width, 32769);
    ASSERT_EQ(out.height, 1);
    EXPECT_EQ(out.pixelsRGBA[out.pixelsRGBA.size() - 4], 150);
}

TEST(DownscaleToFitTest, EmptyInputComesBackEmptyRatherThanCrashing) {
    const DecodedImage out = DownscaleToFit(nullptr, 0, 0, 256);
    EXPECT_EQ(out.width, 0);
    EXPECT_EQ(out.height, 0);
    EXPECT_TRUE(out.pixelsRGBA.empty());
}

}  // namespace
}  // namespace sz::core::persistence
