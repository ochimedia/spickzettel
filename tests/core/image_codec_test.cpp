#include "core/persistence/image_codec.h"

#include <filesystem>
#include <fstream>

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

class ImageCodecTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / (std::string("spickzettel_image_codec_test_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }

    std::filesystem::path dir_;
};

TEST_F(ImageCodecTest, EncodeThenDecodeRoundTripsPixelsExactly) {
    const std::vector<uint8_t> pixels = SamplePixels();
    const std::filesystem::path path = dir_ / "sample.png";

    ASSERT_TRUE(EncodePngToFile(path, pixels.data(), /*width=*/3, /*height=*/2));
    ASSERT_TRUE(std::filesystem::exists(path));

    const std::optional<DecodedImage> decoded = DecodePngFromFile(path);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 3);
    EXPECT_EQ(decoded->height, 2);
    EXPECT_EQ(decoded->pixelsRGBA, pixels);
}

// Lossless is the whole reason QOI was picked over anything cheaper: the
// pixels that come back have to be the ones that went in, alpha included
// (SamplePixels carries two translucent pixels for exactly this).
TEST_F(ImageCodecTest, QoiEncodeThenDecodeRoundTripsPixelsExactly) {
    const std::vector<uint8_t> pixels = SamplePixels();
    const std::filesystem::path path = dir_ / "sample.qoi";

    ASSERT_TRUE(EncodeQoiToFile(path, pixels.data(), /*width=*/3, /*height=*/2));
    ASSERT_TRUE(std::filesystem::exists(path));

    const std::optional<DecodedImage> decoded = DecodeQoiFromFile(path);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 3);
    EXPECT_EQ(decoded->height, 2);
    EXPECT_EQ(decoded->pixelsRGBA, pixels);
}

// A run of identical pixels takes QOI's run op, a repeat takes its index
// op, and a small step takes its diff op - so an image made only of the
// 3x2 sample would never exercise them. This one is wide enough to.
TEST_F(ImageCodecTest, QoiRoundTripsRunsAndRepeatsExactly) {
    std::vector<uint8_t> pixels;
    for (int i = 0; i < 64; ++i) {
        const uint8_t v = static_cast<uint8_t>((i / 8) * 3);  // eight-long runs, one small step apart
        pixels.insert(pixels.end(), {v, static_cast<uint8_t>(255 - v), 128, 255});
    }
    const std::filesystem::path path = dir_ / "runs.qoi";

    ASSERT_TRUE(EncodeQoiToFile(path, pixels.data(), /*width=*/8, /*height=*/8));
    const std::optional<DecodedImage> decoded = DecodeQoiFromFile(path);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->pixelsRGBA, pixels);
}

// The point of sniffing the magic bytes rather than the extension: both
// formats have to work from one call, whatever the file is called.
TEST_F(ImageCodecTest, DecodeImageReadsBothFormats) {
    const std::vector<uint8_t> pixels = SamplePixels();
    const std::filesystem::path pngPath = dir_ / "sample.png";
    const std::filesystem::path qoiPath = dir_ / "sample.qoi";
    ASSERT_TRUE(EncodePngToFile(pngPath, pixels.data(), 3, 2));
    ASSERT_TRUE(EncodeQoiToFile(qoiPath, pixels.data(), 3, 2));

    const std::optional<DecodedImage> fromPng = DecodeImageFromFile(pngPath);
    const std::optional<DecodedImage> fromQoi = DecodeImageFromFile(qoiPath);
    ASSERT_TRUE(fromPng.has_value());
    ASSERT_TRUE(fromQoi.has_value());
    EXPECT_EQ(fromPng->pixelsRGBA, pixels);
    EXPECT_EQ(fromQoi->pixelsRGBA, pixels);
}

// ...and the content is what decides, not the name.
TEST_F(ImageCodecTest, DecodeImageIgnoresAMisleadingExtension) {
    const std::vector<uint8_t> pixels = SamplePixels();
    const std::filesystem::path path = dir_ / "actually_qoi.png";
    ASSERT_TRUE(EncodeQoiToFile(path, pixels.data(), 3, 2));

    const std::optional<DecodedImage> decoded = DecodeImageFromFile(path);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->pixelsRGBA, pixels);
}

TEST_F(ImageCodecTest, DecodeImageReturnsNulloptForUnknownContent) {
    std::filesystem::create_directories(dir_);
    const std::filesystem::path path = dir_ / "garbage.qoi";
    std::ofstream(path, std::ios::binary) << "neither a png nor a qoi, just some bytes";

    EXPECT_FALSE(DecodeImageFromFile(path).has_value());
    EXPECT_FALSE(DecodeImageFromFile(dir_ / "does_not_exist.qoi").has_value());
}

TEST_F(ImageCodecTest, QoiEncodeRejectsInvalidDimensions) {
    const std::vector<uint8_t> pixels = SamplePixels();
    EXPECT_FALSE(EncodeQoiToFile(dir_ / "bad.qoi", pixels.data(), 0, 2));
    EXPECT_FALSE(EncodeQoiToFile(dir_ / "bad.qoi", pixels.data(), 3, -1));
    EXPECT_FALSE(EncodeQoiToFile(dir_ / "bad.qoi", nullptr, 3, 2));
}

// A painted layer is re-encoded over its own previous file on every save,
// so the destination has to be replaced whole: a crash or a full disk
// halfway through a truncate-and-write left the only copy of a drawing as
// the first half of it.
TEST_F(ImageCodecTest, QoiEncodeReplacesTheDestinationWholeOrNotAtAll) {
    const std::vector<uint8_t> pixels = SamplePixels();
    const std::filesystem::path path = dir_ / "layer.qoi";
    ASSERT_TRUE(EncodeQoiToFile(path, pixels.data(), 3, 2));
    const uintmax_t before = std::filesystem::file_size(path);

    // A write that cannot even start - a directory where the temp file
    // wants to be - reports failure and leaves the previous file intact.
    std::filesystem::create_directories(dir_ / "layer.qoi.tmp");
    std::vector<uint8_t> other = pixels;
    other[0] ^= 0xFF;
    EXPECT_FALSE(EncodeQoiToFile(path, other.data(), 3, 2));
    EXPECT_EQ(std::filesystem::file_size(path), before);
    const std::optional<DecodedImage> kept = DecodeImageFromFile(path);
    ASSERT_TRUE(kept.has_value());
    EXPECT_EQ(kept->pixelsRGBA, pixels) << "the previous picture must survive a failed write";

    // ...and a write that works leaves nothing beside the result.
    std::filesystem::remove_all(dir_ / "layer.qoi.tmp");
    EXPECT_TRUE(EncodeQoiToFile(path, other.data(), 3, 2));
    EXPECT_FALSE(std::filesystem::exists(dir_ / "layer.qoi.tmp"));
    EXPECT_EQ(DecodeImageFromFile(path)->pixelsRGBA, other);
}

TEST_F(ImageCodecTest, QoiEncodeCreatesParentDirectories) {
    const std::vector<uint8_t> pixels = SamplePixels();
    const std::filesystem::path path = dir_ / "nested" / "deeper" / "sample.qoi";

    EXPECT_TRUE(EncodeQoiToFile(path, pixels.data(), 3, 2));
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(ImageCodecTest, EncodeCreatesParentDirectories) {
    const std::vector<uint8_t> pixels = SamplePixels();
    const std::filesystem::path path = dir_ / "nested" / "deeper" / "sample.png";

    EXPECT_TRUE(EncodePngToFile(path, pixels.data(), 3, 2));
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(ImageCodecTest, EncodeRejectsInvalidDimensions) {
    const std::vector<uint8_t> pixels = SamplePixels();
    EXPECT_FALSE(EncodePngToFile(dir_ / "bad.png", pixels.data(), 0, 2));
    EXPECT_FALSE(EncodePngToFile(dir_ / "bad.png", pixels.data(), 3, -1));
    EXPECT_FALSE(EncodePngToFile(dir_ / "bad.png", nullptr, 3, 2));
}

TEST_F(ImageCodecTest, DecodeReturnsNulloptForMissingFile) {
    EXPECT_FALSE(DecodePngFromFile(dir_ / "does_not_exist.png").has_value());
}

TEST_F(ImageCodecTest, DecodeReturnsNulloptForGarbageContent) {
    std::filesystem::create_directories(dir_);
    const std::filesystem::path path = dir_ / "garbage.png";
    std::ofstream(path, std::ios::binary) << "not a png file, just some bytes";

    EXPECT_FALSE(DecodePngFromFile(path).has_value());
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

TEST(DownscaleToFitTest, EmptyInputComesBackEmptyRatherThanCrashing) {
    const DecodedImage out = DownscaleToFit(nullptr, 0, 0, 256);
    EXPECT_EQ(out.width, 0);
    EXPECT_EQ(out.height, 0);
    EXPECT_TRUE(out.pixelsRGBA.empty());
}

}  // namespace
}  // namespace sz::core::persistence
