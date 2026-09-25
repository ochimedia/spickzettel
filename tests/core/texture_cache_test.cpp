#include "core/gpu/texture_cache.h"

#include <cstdint>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "fakes/fake_platform_host.h"

namespace sz::core {
namespace {

// 2x2 pixels, and 3x1 - two sizes, for a texture made again at another.
const std::vector<uint8_t> kFour(16, 0x80);
const std::vector<uint8_t> kThree(12, 0x40);

TexturePixels Pixels(const std::vector<uint8_t>& pixels, int width, int height) {
    return TexturePixels{pixels.data(), width, height};
}

constexpr TextureKey kShot{TextureKey::Kind::Picture, 7};
constexpr TextureKey kOther{TextureKey::Kind::Picture, 8};

class TextureCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        window_.uploadsSucceed = true;
        cache_.AttachWindow(&window_);
    }
    void TearDown() override { EXPECT_EQ(window_.badTextureUses, 0); }

    test::FakeOverlayWindow window_;
    TextureCache cache_;
};

// Made once, and found as it was from then on.
TEST_F(TextureCacheTest, ATextureIsMadeOnceAndFoundAfter) {
    EXPECT_FALSE(cache_.Find(kShot).has_value());
    const uint64_t made = cache_.Put(kShot, Pixels(kFour, 2, 2));
    ASSERT_TRUE(window_.IsDrawable(made));
    EXPECT_EQ(cache_.Find(kShot), std::optional<uint64_t>(made));
    EXPECT_EQ(window_.uploadCount, 1);
}

// A texture lives for as long as something draws it: asked for every
// frame, it stays; not asked for through a whole frame, it goes at the
// start of the next.
TEST_F(TextureCacheTest, ATextureNobodyAsksForThroughAFrameIsReleased) {
    const uint64_t kept = cache_.Put(kShot, Pixels(kFour, 2, 2));
    const uint64_t dropped = cache_.Put(kOther, Pixels(kFour, 2, 2));
    for (int frame = 0; frame < 3; ++frame) {
        cache_.BeginFrame();
        EXPECT_TRUE(cache_.Find(kShot).has_value());
    }
    EXPECT_TRUE(window_.IsDrawable(kept));
    EXPECT_EQ(window_.liveTextures.count(dropped), 0u);
    EXPECT_FALSE(cache_.Find(kOther).has_value());
    EXPECT_EQ(cache_.Size(), 1u);
}

// Made between frames - a capture taken while the overlay is hidden - it
// is there for the frame that follows to ask for.
TEST_F(TextureCacheTest, ATextureMadeBetweenFramesIsThereForTheNext) {
    cache_.BeginFrame();
    cache_.BeginFrame();
    const uint64_t made = cache_.Put(kShot, Pixels(kFour, 2, 2));
    cache_.BeginFrame();
    EXPECT_EQ(cache_.Find(kShot), std::optional<uint64_t>(made));
}

// A replaced device takes every texture with it: each is given back, and
// none handed out again - it is made anew when next asked for.
TEST_F(TextureCacheTest, AReplacedDeviceLetsGoOfEveryTextureBeforeAnswering) {
    const uint64_t before = cache_.Put(kShot, Pixels(kFour, 2, 2));
    cache_.Put(kOther, TexturePixels{});  // a failure, forgotten too
    ++window_.textureGeneration;
    EXPECT_FALSE(cache_.Find(kShot).has_value()) << "no handle from the device that is gone";
    EXPECT_FALSE(cache_.Find(kOther).has_value());
    EXPECT_TRUE(window_.liveTextures.empty()) << "given back all the same";
    const uint64_t after = cache_.Get(kShot, 0, Pixels(kFour, 2, 2));
    EXPECT_NE(after, before);
    EXPECT_TRUE(window_.IsDrawable(after));
}

// A texture that cannot be made is remembered as such, so that a picture
// that cannot be read is not read on every frame - until it has gone
// unasked for a frame, or the device is replaced.
TEST_F(TextureCacheTest, AFailureIsRememberedUntilItIsLetGoOf) {
    EXPECT_EQ(cache_.Put(kShot, TexturePixels{}), 0u);
    EXPECT_EQ(cache_.Find(kShot), std::optional<uint64_t>(0));
    window_.uploadsSucceed = false;
    EXPECT_EQ(cache_.Put(kOther, Pixels(kFour, 2, 2)), 0u);
    EXPECT_EQ(cache_.Find(kOther), std::optional<uint64_t>(0)) << "an upload that failed is a failure too";
    EXPECT_EQ(cache_.Get(kOther, 0, Pixels(kFour, 2, 2)), 0u) << "not tried again for the same pixels";

    cache_.BeginFrame();
    cache_.BeginFrame();
    EXPECT_FALSE(cache_.Find(kShot).has_value());
    EXPECT_FALSE(cache_.Find(kOther).has_value());
}

// Pixels at hand that change: the same revision is the same texture, a
// new one the same size goes into it, and a new size is a new texture.
TEST_F(TextureCacheTest, GetFollowsTheRevisionOfThePixels) {
    const uint64_t first = cache_.Get(kShot, 1, Pixels(kFour, 2, 2));
    EXPECT_EQ(cache_.Get(kShot, 1, Pixels(kFour, 2, 2)), first);
    EXPECT_EQ(window_.uploadCount, 1);

    EXPECT_EQ(cache_.Get(kShot, 2, Pixels(kFour, 2, 2)), first) << "updated in place";
    EXPECT_EQ(window_.uploadCount, 1);

    const uint64_t resized = cache_.Get(kShot, 3, Pixels(kThree, 3, 1));
    EXPECT_NE(resized, first);
    EXPECT_EQ(window_.liveTextures.count(first), 0u);
    EXPECT_TRUE(window_.IsDrawable(resized));
}

TEST_F(TextureCacheTest, DropReleasesAtOnce) {
    const uint64_t made = cache_.Put(kShot, Pixels(kFour, 2, 2));
    cache_.Drop(kShot);
    EXPECT_EQ(window_.liveTextures.count(made), 0u);
    EXPECT_FALSE(cache_.Find(kShot).has_value());
    cache_.Drop(kShot);  // nothing there: nothing released twice
}

// A texture replaced by another under the same name gives the first back.
TEST_F(TextureCacheTest, PutInPlaceOfAnotherReleasesIt) {
    const uint64_t first = cache_.Put(kShot, Pixels(kFour, 2, 2));
    const uint64_t second = cache_.Put(kShot, Pixels(kFour, 2, 2));
    EXPECT_EQ(window_.liveTextures.count(first), 0u);
    EXPECT_TRUE(window_.IsDrawable(second));
    EXPECT_EQ(window_.liveTextures.size(), 1u);
}

TEST_F(TextureCacheTest, AnotherWindowStartsFromNothing) {
    cache_.Put(kShot, Pixels(kFour, 2, 2));
    test::FakeOverlayWindow other;
    other.uploadsSucceed = true;
    cache_.AttachWindow(&other);
    EXPECT_TRUE(window_.liveTextures.empty());
    EXPECT_FALSE(cache_.Find(kShot).has_value());
    cache_.AttachWindow(nullptr);
}

TEST(TextureCacheWithoutAWindowTest, MakesNothing) {
    TextureCache cache;
    EXPECT_EQ(cache.Put(kShot, Pixels(kFour, 2, 2)), 0u);
    EXPECT_EQ(cache.Get(kShot, 0, Pixels(kFour, 2, 2)), 0u);
    EXPECT_FALSE(cache.Find(kShot).has_value());
    EXPECT_EQ(cache.Size(), 0u);
    cache.BeginFrame();
}

}  // namespace
}  // namespace sz::core
