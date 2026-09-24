#include "platform/pen_glyph.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using sz::platform::pen_glyph::Ink;
using sz::platform::pen_glyph::InkAt;

// The glyph as the cursor bitmap sees it: one character per pixel, sampled
// at the pixel's own center, with the nib on the pixel a cursor's hotspot
// would name. The same grid Win32OverlayWindow::PenCursor rasterizes (it
// supersamples for the edges; this samples once, which is enough to say
// what is where).
constexpr int kSize = 24;
constexpr int kHotspotX = 2;
constexpr int kHotspotY = 21;

std::vector<std::string> RenderGlyph() {
    std::vector<std::string> rows;
    for (int y = 0; y < kSize; ++y) {
        std::string row;
        for (int x = 0; x < kSize; ++x) {
            switch (InkAt(static_cast<float>(x - kHotspotX), static_cast<float>(y - kHotspotY))) {
                case Ink::Edge:
                    row += '#';
                    break;
                case Ink::Fill:
                    row += '.';
                    break;
                case Ink::None:
                    row += ' ';
                    break;
            }
        }
        rows.push_back(row);
    }
    return rows;
}

std::string AsArt(const std::vector<std::string>& rows) {
    std::string art = "\n";
    for (const std::string& row : rows) {
        art += '|' + row + "|\n";
    }
    return art;
}

TEST(PenGlyphTest, NibSitsOnTheHotspotPixel) {
    const std::vector<std::string> rows = RenderGlyph();
    // The nib tip is the point the pointer marks with, so the hotspot pixel
    // has to carry ink rather than sit just outside the shape.
    EXPECT_NE(rows[kHotspotY][kHotspotX], ' ') << AsArt(rows);
}

TEST(PenGlyphTest, FitsTheCursorBitmapWithRoomForItsOutline) {
    const std::vector<std::string> rows = RenderGlyph();
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            if (rows[y][x] == ' ') {
                continue;
            }
            // Nothing may touch the bitmap's own border: a cursor is
            // clipped to its bitmap, and an outline running off the edge is
            // what a shape that has outgrown its box looks like.
            EXPECT_GT(x, 0) << AsArt(rows);
            EXPECT_LT(x, kSize - 1) << AsArt(rows);
            EXPECT_GT(y, 0) << AsArt(rows);
            EXPECT_LT(y, kSize - 1) << AsArt(rows);
        }
    }
}

TEST(PenGlyphTest, WhiteBodySurvivesTheOutline) {
    const std::vector<std::string> rows = RenderGlyph();
    // A pen whose outline eats its core reads as a black stick - the
    // failure this width was chosen against. Measured along the body, not
    // at the nib, which is a point and legitimately all outline.
    int filledRows = 0;
    for (const std::string& row : rows) {
        filledRows += row.find('.') != std::string::npos ? 1 : 0;
    }
    EXPECT_GE(filledRows, 12) << AsArt(rows);
}

TEST(PenGlyphTest, PointsUpAndToTheRightFromTheNib) {
    const std::vector<std::string> rows = RenderGlyph();
    // Everything above the nib row and to the right of the nib column: a
    // pen held the way a right-handed pointer holds one, which is also what
    // keeps the shape clear of the pixel it is marking.
    for (int y = kHotspotY + 1; y < kSize; ++y) {
        EXPECT_EQ(rows[y].find_first_not_of(' '), std::string::npos) << AsArt(rows);
    }
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kHotspotX; ++x) {
            EXPECT_EQ(rows[y][x], ' ') << AsArt(rows);
        }
    }
}

}  // namespace
