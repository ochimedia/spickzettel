#include "platform/win32/win32_dx11_renderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <vector>

#include <imgui.h>

#include "core/canvas/item.h"
#include "core/drawing/stroke.h"
#include "core/drawing/stroke_mesh_cache.h"
#include "core/persistence/library_store.h"
#include "platform/platform_types.h"
#include "ui/item_painting.h"

namespace sz::platform::win32 {
namespace {

using Microsoft::WRL::ComPtr;

// One RGBA8 pixel per 4 bytes, like the textures.
std::vector<uint8_t> Stripes(int width, int height) {
    std::vector<uint8_t> pixels;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t v = x % 2 == 0 ? 255 : 0;
            pixels.insert(pixels.end(), {v, v, v, 255});
        }
    }
    return pixels;
}

// The top-left `width` x `height` of `subresource` of `resource`, copied
// out through a staging texture of that size. The region is named: a copy
// with none takes the whole subresource, which must then fit the staging
// texture. A GPU's driver let a 64x64 frame into a 64x1 one; WARP, which
// a CI runner without a GPU draws with, does not, and the row came back
// empty.
std::vector<uint8_t> ReadBack(ID3D11Device* device, ID3D11Resource* resource, UINT subresource, UINT width,
                              UINT height) {
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
        return {};
    }
    const D3D11_BOX region{0, 0, 0, width, height, 1};
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, resource, subresource, &region);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return {};
    }
    std::vector<uint8_t> pixels;
    for (UINT y = 0; y < height; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        pixels.insert(pixels.end(), row, row + static_cast<size_t>(width) * 4);
    }
    context->Unmap(staging.Get(), 0);
    return pixels;
}

class Win32Dx11RendererTest : public ::testing::Test {
protected:
    void SetUp() override {
        hwnd_ = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(hwnd_, nullptr);
        if (!renderer_.Initialize(hwnd_)) {
            GTEST_SKIP() << "no D3D11 device here";
        }
    }
    void TearDown() override {
        renderer_.Shutdown();
        if (hwnd_) {
            DestroyWindow(hwnd_);
        }
    }

    // A texture with one extra reference held by the test, so its count
    // can be read back without touching freed memory.
    ID3D11ShaderResourceView* WatchedTexture() {
        const std::array<uint8_t, 4> pixel{255, 0, 0, 255};
        ID3D11ShaderResourceView* srv = renderer_.CreateTextureFromRGBA(pixel.data(), 1, 1);
        if (srv) {
            srv->AddRef();
        }
        return srv;
    }
    // References left besides the test's own.
    static ULONG OthersHolding(ID3D11ShaderResourceView* srv) {
        srv->AddRef();
        return srv->Release() - 1;
    }

    // `srv` drawn into the top-left `width` x 1 pixels of a frame through
    // `filter`, the way the UI draws a picture; the red channel of those
    // pixels back.
    std::vector<int> DrawnRow(ID3D11ShaderResourceView* srv, int width, ImageFilter filter) {
        ComPtr<ID3D11Device> device;
        srv->GetDevice(&device);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 64;
        desc.Height = 64;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        ComPtr<ID3D11RenderTargetView> targetView;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &target)) ||
            FAILED(device->CreateRenderTargetView(target.Get(), nullptr, &targetView))) {
            return {};
        }

        renderer_.NewFrame();
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        drawList->AddCallback(&Win32Dx11Renderer::ApplyImageFilter,
                              reinterpret_cast<void*>(static_cast<intptr_t>(filter)));
        drawList->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(srv)), ImVec2(0.0f, 0.0f),
                           ImVec2(static_cast<float>(width), 1.0f));
        drawList->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
        renderer_.RenderTo(targetView.Get());

        const std::vector<uint8_t> pixels = ReadBack(device.Get(), target.Get(), 0, 64, 1);
        std::vector<int> red;
        for (int x = 0; x < width && static_cast<size_t>(x) * 4 < pixels.size(); ++x) {
            red.push_back(pixels[static_cast<size_t>(x) * 4]);
        }
        return red;
    }

    // What `draw` puts in a 64x64 frame cleared to nothing, drawn by the
    // renderer; the frame's RGBA pixels back.
    std::vector<uint8_t> Drawn(const std::function<void(ImDrawList*)>& draw) {
        ID3D11ShaderResourceView* probe = WatchedTexture();
        if (!probe) {
            return {};
        }
        ComPtr<ID3D11Device> device;
        probe->GetDevice(&device);
        renderer_.ReleaseTexture(probe);
        probe->Release();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 64;
        desc.Height = 64;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        ComPtr<ID3D11RenderTargetView> targetView;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &target)) ||
            FAILED(device->CreateRenderTargetView(target.Get(), nullptr, &targetView))) {
            return {};
        }

        renderer_.NewFrame();
        draw(ImGui::GetForegroundDrawList());
        renderer_.RenderTo(targetView.Get());
        return ReadBack(device.Get(), target.Get(), 0, 64, 64);
    }
    // `item` drawn by DrawItemContent, as the canvas draws it, into the whole
    // of the frame, with `hooks`.
    std::vector<uint8_t> DrawnItem(const core::Item& item, const ui::PaintHooks& hooks) {
        return Drawn([&](ImDrawList* drawList) {
            drawList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(64.0f, 64.0f), true);
            ui::DrawItemContent(drawList, item, ImVec2(0.0f, 0.0f), ImVec2(64.0f, 64.0f), 0, false, {}, hooks);
            drawList->PopClipRect();
        });
    }
    static int ChannelAt(const std::vector<uint8_t>& pixels, int x, int y, int channel) {
        return pixels[(static_cast<size_t>(y) * 64 + x) * 4 + channel];
    }
    static int AlphaAt(const std::vector<uint8_t>& pixels, int x, int y) {
        return ChannelAt(pixels, x, y, 3);
    }

    static int Spread(const std::vector<int>& values) {
        if (values.empty()) {
            return 0;
        }
        const auto [low, high] = std::minmax_element(values.begin(), values.end());
        return *high - *low;
    }

    HWND hwnd_ = nullptr;
    Win32Dx11Renderer renderer_;
};

// The reason the chain is built by hand: a straight-alpha picture averaged
// as it is darkens its ink toward the (0,0,0,0) around it.
TEST_F(Win32Dx11RendererTest, AMipAveragesTheInkNotTheTransparencyAroundIt) {
    const std::array<uint8_t, 16> pixels{0, 0, 0, 0, 255, 255, 255, 255,  //
                                         0, 0, 0, 0, 255, 255, 255, 255};
    ID3D11ShaderResourceView* srv = renderer_.CreateTextureFromRGBA(pixels.data(), 2, 2);
    ASSERT_NE(srv, nullptr);
    renderer_.RefreshMips();

    ComPtr<ID3D11Device> device;
    srv->GetDevice(&device);
    ComPtr<ID3D11Resource> texture;
    srv->GetResource(&texture);
    const std::vector<uint8_t> mip = ReadBack(device.Get(), texture.Get(), 1, 1, 1);
    ASSERT_EQ(mip.size(), 4u);
    EXPECT_EQ(mip[0], 255);
    EXPECT_EQ(mip[1], 255);
    EXPECT_EQ(mip[2], 255);
    EXPECT_NEAR(mip[3], 128, 1);
    renderer_.ReleaseTexture(srv);
}

// One-pixel stripes at a third of their size: bilinear lands on one stripe
// per pixel and shows them at full contrast - the aliasing that breaks up
// text - where the two filters that widen with the reduction come out an
// even gray.
TEST_F(Win32Dx11RendererTest, ShrunkStripesAliasBilinearButGoGrayBicubicAndLanczos) {
    const std::vector<uint8_t> stripes = Stripes(48, 1);
    ID3D11ShaderResourceView* srv = renderer_.CreateTextureFromRGBA(stripes.data(), 48, 1);
    ASSERT_NE(srv, nullptr);

    EXPECT_GT(Spread(DrawnRow(srv, 16, ImageFilter::Bilinear)), 200);
    for (const ImageFilter filter : {ImageFilter::Bicubic, ImageFilter::Lanczos}) {
        const std::vector<int> row = DrawnRow(srv, 16, filter);
        ASSERT_EQ(row.size(), 16u);
        EXPECT_LT(Spread(row), 24) << static_cast<int>(filter);
        EXPECT_NEAR(row[8], 128, 24) << static_cast<int>(filter);
    }
    renderer_.ReleaseTexture(srv);
}

// Enlarged, nearest keeps every pixel one of the picture's own; the others
// blend across the edge.
TEST_F(Win32Dx11RendererTest, EnlargedNearestKeepsThePicturesOwnPixels) {
    const std::vector<uint8_t> stripes = Stripes(2, 1);
    ID3D11ShaderResourceView* srv = renderer_.CreateTextureFromRGBA(stripes.data(), 2, 1);
    ASSERT_NE(srv, nullptr);

    for (const int value : DrawnRow(srv, 16, ImageFilter::Nearest)) {
        EXPECT_TRUE(value == 0 || value == 255) << value;
    }
    for (const ImageFilter filter : {ImageFilter::Bilinear, ImageFilter::Bicubic, ImageFilter::Lanczos}) {
        const std::vector<int> row = DrawnRow(srv, 16, filter);
        EXPECT_TRUE(std::any_of(row.begin(), row.end(), [](int v) { return v > 20 && v < 235; }))
            << static_cast<int>(filter);
    }
    renderer_.ReleaseTexture(srv);
}

TEST_F(Win32Dx11RendererTest, OutsideAFrameATextureIsReleasedAtOnce) {
    ID3D11ShaderResourceView* srv = WatchedTexture();
    ASSERT_NE(srv, nullptr);
    renderer_.ReleaseTexture(srv);
    EXPECT_EQ(OthersHolding(srv), 0u);
    srv->Release();
}

// Draw commands built during a frame hold the raw pointer and no
// reference, so a texture released mid-frame has to outlive the frame.
TEST_F(Win32Dx11RendererTest, DuringAFrameTheReleaseWaitsForTheFrameToBeDrawn) {
    ID3D11ShaderResourceView* srv = WatchedTexture();
    ASSERT_NE(srv, nullptr);
    renderer_.NewFrame();
    renderer_.ReleaseTexture(srv);
    EXPECT_EQ(OthersHolding(srv), 1u);
    renderer_.RenderAndPresent();
    EXPECT_EQ(OthersHolding(srv), 0u);
    srv->Release();
}

// Once a present has found the window occluded, the next frame asks again
// before drawing - and this window, never hidden, is seen again at once.
TEST_F(Win32Dx11RendererTest, AnOccludedWindowIsAskedAboutAgainBeforeDrawing) {
    renderer_.OccludeForTesting();
    EXPECT_TRUE(renderer_.ReadyToRender());
    EXPECT_EQ(renderer_.DeviceGeneration(), 0u) << "the device was never in question";
}

// A resize whose ResizeBuffers failed is tried again by the next frame,
// rather than drawing at the old size, stretched, until the next WM_SIZE.
TEST_F(Win32Dx11RendererTest, AFailedResizeIsTriedAgainByTheNextFrame) {
    renderer_.FailResizeForTesting();
    EXPECT_TRUE(renderer_.ReadyToRender());
    EXPECT_FALSE(renderer_.ResizePendingForTesting());
}

TEST_F(Win32Dx11RendererTest, WithItsDeviceInPlaceTheRendererIsReadyAsItWas) {
    EXPECT_TRUE(renderer_.ReadyToRender());
    EXPECT_EQ(renderer_.DeviceGeneration(), 0u);
}

// A rectangle past the texture's edge, or past the edge of the rows it is
// read from, is refused rather than handed to D3D.
TEST_F(Win32Dx11RendererTest, ARegionOutsideTheTextureIsRefused) {
    const std::vector<uint8_t> stripes = Stripes(48, 2);
    ID3D11ShaderResourceView* texture = renderer_.CreateTextureFromRGBA(stripes.data(), 48, 2);
    ASSERT_NE(texture, nullptr);
    EXPECT_TRUE(renderer_.UpdateTextureRegionRGBA(texture, stripes.data(), 48, 40, 1, 8, 1));
    EXPECT_FALSE(renderer_.UpdateTextureRegionRGBA(texture, stripes.data(), 48, 41, 0, 8, 1)) << "past the right";
    EXPECT_FALSE(renderer_.UpdateTextureRegionRGBA(texture, stripes.data(), 48, 0, 1, 8, 2)) << "past the bottom";
    EXPECT_FALSE(renderer_.UpdateTextureRegionRGBA(texture, stripes.data(), 16, 10, 0, 8, 1)) << "past the source";
    renderer_.ReleaseTexture(texture);
}

// A device the driver took away is replaced, and drawing goes on on the new
// one - ImGui's own objects and the filter shaders with it. A texture from
// before is refused an update, since the new context cannot write it, and
// can still be released.
TEST_F(Win32Dx11RendererTest, ALostDeviceIsReplacedAndDrawnWithAgain) {
    const std::vector<uint8_t> stripes = Stripes(48, 1);
    ID3D11ShaderResourceView* before = renderer_.CreateTextureFromRGBA(stripes.data(), 48, 1);
    ASSERT_NE(before, nullptr);

    renderer_.LoseDeviceForTesting();
    ASSERT_TRUE(renderer_.ReadyToRender());
    EXPECT_EQ(renderer_.DeviceGeneration(), 1u);
    EXPECT_FALSE(renderer_.UpdateTextureRegionRGBA(before, stripes.data(), 48, 0, 0, 48, 1));
    renderer_.ReleaseTexture(before);

    ID3D11ShaderResourceView* after = renderer_.CreateTextureFromRGBA(stripes.data(), 48, 1);
    ASSERT_NE(after, nullptr);
    EXPECT_TRUE(renderer_.UpdateTextureRegionRGBA(after, stripes.data(), 48, 0, 0, 48, 1));
    EXPECT_GT(Spread(DrawnRow(after, 16, ImageFilter::Bilinear)), 200);
    const std::vector<int> row = DrawnRow(after, 16, ImageFilter::Lanczos);
    ASSERT_EQ(row.size(), 16u);
    EXPECT_LT(Spread(row), 24) << "the filter shaders are made again";
    renderer_.ReleaseTexture(after);
}

// While a lost device cannot be replaced yet - the driver still on its way
// back - no texture can be made, and the generation moves once the new
// device is there. Moved as the old one went, a texture asked for in
// between - a capture's - was kept by the app as one that could not be
// made, under the new device's generation, and never asked for again
// while it was on screen.
TEST_F(Win32Dx11RendererTest, TheGenerationMovesWhenTheNewDeviceIsMade) {
    const std::vector<uint8_t> stripes = Stripes(48, 1);
    renderer_.LoseDeviceForTesting();
    renderer_.FailDeviceCreationForTesting(2);
    ASSERT_FALSE(renderer_.ReadyToRender());
    ASSERT_FALSE(renderer_.ReadyToRender());
    const uint64_t between = renderer_.DeviceGeneration();
    EXPECT_EQ(renderer_.CreateTextureFromRGBA(stripes.data(), 48, 1), nullptr) << "nothing to make it on";

    ASSERT_TRUE(renderer_.ReadyToRender()) << "the driver back";
    EXPECT_NE(renderer_.DeviceGeneration(), between) << "what could not be made in between is made again";
    EXPECT_EQ(renderer_.DeviceGeneration(), 1u) << "one replacement";
}

// What this renderer lends a snippet's painting: all of it, as the window
// hands it over.
ui::PaintHooks RendererHooks() {
    return ui::PaintHooks{ImageFilter::Bilinear, &Win32Dx11Renderer::ApplyImageFilter,
                          &Win32Dx11Renderer::ApplyStrokeDepth, &Win32Dx11Renderer::ApplyStrokeLayer};
}

// A translucent stroke along y = 32 that turns and comes back down x = 32,
// crossing itself at (32, 32). Eight wide, so both bodies cover the pixels
// around the crossing fully.
core::Stroke CrossingStroke() {
    core::Stroke stroke;
    stroke.points = {{8.0f, 32.0f}, {56.0f, 32.0f}, {56.0f, 8.0f}, {32.0f, 8.0f}, {32.0f, 56.0f}};
    stroke.colorRGBA = 0xFF000080;  // red, half strength
    stroke.width = 8.0f;
    return stroke;
}

// Why strokes are drawn under the depth test: without it, the crossing
// takes the color twice and comes out darker than the stroke anywhere else.
TEST_F(Win32Dx11RendererTest, UntestedAStrokeDarkensWhereItCrossesItself) {
    const std::vector<uint8_t> pixels =
        Drawn([](ImDrawList* drawList) { ui::DrawStroke(drawList, CrossingStroke(), 0.0f, 0.0f, 1.0f, 1.0f); });
    ASSERT_EQ(pixels.size(), 64u * 64u * 4u);
    EXPECT_NEAR(AlphaAt(pixels, 16, 32), 128, 2) << "once";
    EXPECT_GT(AlphaAt(pixels, 32, 32), 180) << "twice";
}

// Under it, every pixel down the second pass through the crossing - the
// first pass's anti-aliased edges included, at y 27 and 36 - is the
// stroke's color once. With the edges drawn in the mesh's old order, the
// first pass's edge took those pixels ahead of the second pass's body and
// left a fainter line on each side of the crossing.
TEST_F(Win32Dx11RendererTest, AStrokeTakesItsColorOnceWhereItCrossesItself) {
    const std::vector<uint8_t> pixels = Drawn([](ImDrawList* drawList) {
        ui::DrawStroke(drawList, CrossingStroke(), 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, {},
                       &Win32Dx11Renderer::ApplyStrokeDepth);
    });
    ASSERT_EQ(pixels.size(), 64u * 64u * 4u);
    for (int y = 20; y <= 44; ++y) {
        EXPECT_NEAR(AlphaAt(pixels, 32, y), 128, 2) << "y " << y;
    }
    EXPECT_NEAR(AlphaAt(pixels, 16, 32), 128, 2);
    // Nowhere twice - the corners, where only the two edges meet, included.
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            EXPECT_LE(AlphaAt(pixels, x, y), 130) << x << ", " << y;
        }
    }
}

// Only a stroke's own fragments are held back: a second stroke over the
// first is laid over it as ever.
TEST_F(Win32Dx11RendererTest, AStrokeStillLaysOverTheOneBefore) {
    core::Stroke across;
    across.points = {{8.0f, 32.0f}, {56.0f, 32.0f}};
    across.colorRGBA = 0xFF000080;
    across.width = 8.0f;
    core::Stroke down = across;
    down.points = {{32.0f, 8.0f}, {32.0f, 56.0f}};
    down.colorRGBA = 0x0000FF80;

    const std::vector<uint8_t> pixels = Drawn([&](ImDrawList* drawList) {
        for (const core::Stroke& stroke : {across, down}) {
            ui::DrawStroke(drawList, stroke, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, {}, &Win32Dx11Renderer::ApplyStrokeDepth);
        }
    });
    ASSERT_EQ(pixels.size(), 64u * 64u * 4u);
    EXPECT_NEAR(AlphaAt(pixels, 16, 32), 128, 2);
    EXPECT_NEAR(AlphaAt(pixels, 32, 32), 191, 2) << "half over half";
}

// Two strokes crossing on a 64x64 snippet: red along y = 32, then blue
// down x = 32. `alpha` is both inks' own.
core::Item CrossedSnippet(uint32_t alpha, float opacity) {
    core::Item item;
    item.id = 1;
    item.nativeW = 64.0f;
    item.nativeH = 64.0f;
    item.foregroundOpacity = opacity;
    core::Stroke across;
    across.points = {{8.0f, 32.0f}, {56.0f, 32.0f}};
    across.colorRGBA = 0xFF000000u | alpha;
    across.width = 8.0f;
    core::Stroke down = across;
    down.points = {{32.0f, 8.0f}, {32.0f, 56.0f}};
    down.colorRGBA = 0x0000FF00u | alpha;
    item.strokes = {across, down};
    return item;
}

// The snippet's opacity fades the finished drawing: where the opaque blue
// crosses the opaque red there is only blue, at half strength, as anywhere
// else on the snippet. Faded stroke by stroke - with no layer to be had -
// the red shows through the blue and the crossing is stronger than the rest.
TEST_F(Win32Dx11RendererTest, ASnippetsOpacityFadesTheFinishedDrawingNotEachStroke) {
    const std::vector<uint8_t> layered = DrawnItem(CrossedSnippet(0xFF, 0.5f), RendererHooks());
    ASSERT_EQ(layered.size(), 64u * 64u * 4u);
    EXPECT_NEAR(AlphaAt(layered, 16, 32), 128, 2) << "red alone";
    EXPECT_NEAR(ChannelAt(layered, 16, 32, 0), 128, 2) << "premultiplied red";
    EXPECT_NEAR(AlphaAt(layered, 32, 32), 128, 2) << "the crossing, as strong as the rest";
    EXPECT_NEAR(ChannelAt(layered, 32, 32, 0), 0, 2) << "no red under the blue";
    EXPECT_NEAR(ChannelAt(layered, 32, 32, 2), 128, 2);
    EXPECT_EQ(AlphaAt(layered, 2, 2), 0) << "nothing where no stroke is";

    ui::PaintHooks noLayer = RendererHooks();
    noLayer.strokeLayer = nullptr;
    const std::vector<uint8_t> perStroke = DrawnItem(CrossedSnippet(0xFF, 0.5f), noLayer);
    ASSERT_EQ(perStroke.size(), 64u * 64u * 4u);
    EXPECT_NEAR(AlphaAt(perStroke, 32, 32), 191, 2);
    EXPECT_NEAR(ChannelAt(perStroke, 32, 32, 0), 64, 2) << "red through the blue";
}

// Inks of their own strength still blend with each other inside the layer,
// and the layer is laid down once over that: half over half is 0.75, at
// half opacity 0.375.
TEST_F(Win32Dx11RendererTest, TranslucentInksBlendBeforeTheSnippetsOpacity) {
    const std::vector<uint8_t> pixels = DrawnItem(CrossedSnippet(0x80, 0.5f), RendererHooks());
    ASSERT_EQ(pixels.size(), 64u * 64u * 4u);
    EXPECT_NEAR(AlphaAt(pixels, 16, 32), 64, 2);
    EXPECT_NEAR(AlphaAt(pixels, 32, 32), 96, 2);
    EXPECT_NEAR(ChannelAt(pixels, 32, 32, 0), 32, 2) << "the red under the blue, faded with it";
}

// Not a test: a picture for a person to look at, written only when
// SZ_STROKE_COMPARE_OUT names a .bmp file. The same snippet drawn by
// DrawItemContent stroke by stroke, as strokes were drawn before they had
// the depth test and the layer, and as they are now (columns); at full and
// half opacity, enlarged twice over, and with opaque inks at half opacity
// (rows) - over white with a black bar, so darkening, sharpness and how
// opacity fades stacked strokes all show.
TEST_F(Win32Dx11RendererTest, StrokesSideBySide) {
    const char* out = std::getenv("SZ_STROKE_COMPARE_OUT");
    if (out == nullptr || *out == '\0') {
        GTEST_SKIP() << "set SZ_STROKE_COMPARE_OUT to a .bmp path";
    }
    constexpr int kCell = 240;
    constexpr int kColumns = 2;
    constexpr int kRows = 4;
    constexpr int kWidth = kCell * kColumns;
    constexpr int kHeight = kCell * kRows;

    core::Item item;
    item.id = 1;
    item.nativeW = static_cast<float>(kCell);
    item.nativeH = static_cast<float>(kCell);
    core::Stroke loop;  // a figure eight, crossing itself in the middle
    loop.colorRGBA = 0xE0201080;
    loop.width = 22.0f;
    for (int i = 0; i <= 200; ++i) {
        const float t = 6.2831853f * static_cast<float>(i) / 200.0f;
        loop.points.push_back({120.0f + 84.0f * std::sin(t), 120.0f + 60.0f * std::sin(t) * std::cos(t)});
    }
    core::Stroke diagonal;
    diagonal.colorRGBA = 0x2050E080;
    diagonal.width = 16.0f;
    diagonal.points = {{24.0f, 216.0f}, {216.0f, 24.0f}};
    core::Stroke zigzag;  // turns tighter than it is wide
    zigzag.colorRGBA = 0x20A040A0;
    zigzag.width = 14.0f;
    for (int i = 0; i <= 16; ++i) {
        zigzag.points.push_back({30.0f + 11.0f * static_cast<float>(i), i % 2 == 0 ? 200.0f : 186.0f});
    }
    item.strokes = {loop, diagonal, zigzag};
    core::Item opaque = item;
    for (core::Stroke& stroke : opaque.strokes) {
        stroke.colorRGBA |= 0xFFu;
    }

    ID3D11ShaderResourceView* probe = WatchedTexture();
    ASSERT_NE(probe, nullptr);
    ComPtr<ID3D11Device> device;
    probe->GetDevice(&device);
    renderer_.ReleaseTexture(probe);
    probe->Release();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> targetView;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &target)) &&
                SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &targetView)));

    // ImGui takes the display size from the window as the frame starts.
    SetWindowPos(hwnd_, nullptr, 0, 0, kWidth, kHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    renderer_.NewFrame();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ui::PaintHooks hooks[kColumns] = {ui::PaintHooks{}, RendererHooks()};
    const char* names[kColumns] = {"Stroke by stroke", "Layered"};
    const char* rows[kRows] = {"opacity 100%", "opacity 50%", "100%, enlarged 2x", "opaque inks, 50%"};
    for (int row = 0; row < kRows; ++row) {
        for (int column = 0; column < kColumns; ++column) {
            const ImVec2 cellMin(static_cast<float>(column * kCell), static_cast<float>(row * kCell));
            const ImVec2 cellMax(cellMin.x + kCell, cellMin.y + kCell);
            drawList->PushClipRect(cellMin, cellMax, true);
            drawList->AddRectFilled(cellMin, cellMax, IM_COL32(255, 255, 255, 255));
            drawList->AddRectFilled(ImVec2(cellMin.x + 100.0f, cellMin.y), ImVec2(cellMin.x + 140.0f, cellMax.y),
                                    IM_COL32(0, 0, 0, 255));
            core::Item& drawn = row == 3 ? opaque : item;
            drawn.foregroundOpacity = row == 1 || row == 3 ? 0.5f : 1.0f;
            // Enlarged about the cell's middle: the snippet twice the cell.
            const float scale = row == 2 ? 2.0f : 1.0f;
            const ImVec2 pMin(cellMin.x + kCell * 0.5f * (1.0f - scale), cellMin.y + kCell * 0.5f * (1.0f - scale));
            const ImVec2 pMax(pMin.x + kCell * scale, pMin.y + kCell * scale);
            ui::DrawItemContent(drawList, drawn, pMin, pMax, 0, false, {}, hooks[column]);
            drawList->AddText(ImVec2(cellMin.x + 4.0f, cellMin.y + 2.0f), IM_COL32(0, 0, 0, 255), names[column]);
            drawList->AddText(ImVec2(cellMin.x + 4.0f, cellMax.y - 18.0f), IM_COL32(0, 0, 0, 255), rows[row]);
            drawList->AddRect(cellMin, cellMax, IM_COL32(160, 160, 160, 255));
            drawList->PopClipRect();
        }
    }
    renderer_.RenderTo(targetView.Get());
    const std::vector<uint8_t> pixels = ReadBack(device.Get(), target.Get(), 0, kWidth, kHeight);
    ASSERT_EQ(pixels.size(), static_cast<size_t>(kWidth) * kHeight * 4);

    // A 24-bit BMP, bottom row first.
    const uint32_t rowBytes = kWidth * 3;
    const uint32_t imageBytes = rowBytes * kHeight;
    std::vector<uint8_t> file(54, 0);
    const auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            file[at + i] = static_cast<uint8_t>(v >> (8 * i));
        }
    };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, 54 + imageBytes);
    put32(10, 54);
    put32(14, 40);
    put32(18, kWidth);
    put32(22, kHeight);
    file[26] = 1;
    file[28] = 24;
    put32(34, imageBytes);
    for (int y = kHeight - 1; y >= 0; --y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t at = (static_cast<size_t>(y) * kWidth + x) * 4;
            file.insert(file.end(), {pixels[at + 2], pixels[at + 1], pixels[at]});
        }
    }
    std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(file.data()),
                                               static_cast<std::streamsize>(file.size()));
}

// Not a test: what the GPU spends on one frame of a generated library's
// current canvas, run only when SZ_PERF_LIBRARY names a library file (see
// tools/perf_library), stroke by stroke and layered. Offscreen at
// 1920x1080, timed with timestamp queries, so the number is the GPU's own
// and no window, vsync or input is involved; the CPU side is PerfBench's.
// The two take turns frame by frame, so the GPU's clocks moving under a
// run move under both alike; the minimum is the GPU at full speed.
TEST_F(Win32Dx11RendererTest, StrokesGpuTime) {
    const char* root = std::getenv("SZ_PERF_LIBRARY");
    if (root == nullptr || *root == '\0') {
        GTEST_SKIP() << "set SZ_PERF_LIBRARY to a library file (see tools/perf_library)";
    }
    core::persistence::LibraryStore store{std::filesystem::path(root)};
    const std::optional<core::CanvasManagerSnapshot> snapshot = store.Load();
    ASSERT_TRUE(snapshot.has_value());
    std::vector<core::Item> items;
    for (const core::Canvas& canvas : snapshot->canvases) {
        if (canvas.id == snapshot->currentCanvasId) {
            items = canvas.items;
        }
    }
    ASSERT_FALSE(items.empty());

    constexpr int kWidth = 1920;
    constexpr int kHeight = 1080;
    ID3D11ShaderResourceView* probe = WatchedTexture();
    ASSERT_NE(probe, nullptr);
    ComPtr<ID3D11Device> device;
    probe->GetDevice(&device);
    renderer_.ReleaseTexture(probe);
    probe->Release();
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> targetView;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &target)) &&
                SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, &targetView)));
    SetWindowPos(hwnd_, nullptr, 0, 0, kWidth, kHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    D3D11_QUERY_DESC disjointDesc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC stampDesc{D3D11_QUERY_TIMESTAMP, 0};
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> begin;
    ComPtr<ID3D11Query> end;
    ASSERT_TRUE(SUCCEEDED(device->CreateQuery(&disjointDesc, &disjoint)) &&
                SUCCEEDED(device->CreateQuery(&stampDesc, &begin)) &&
                SUCCEEDED(device->CreateQuery(&stampDesc, &end)));

    core::StrokeMeshCache meshes;
    // From RenderTo's start to the GPU done with it: ImGui's submission on
    // the CPU and the GPU's work, which the timestamps alone can mix up -
    // a GPU waiting on commands still to come counts as busy.
    double lastWallMs = 0.0;
    const auto frameMs = [&](const ui::PaintHooks& hooks) {
        renderer_.NewFrame();
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        meshes.BeginFrame();
        for (const core::Item& item : items) {
            const ImVec2 pMin(item.rect.x, item.rect.y);
            const ImVec2 pMax(item.rect.x + item.rect.w, item.rect.y + item.rect.h);
            drawList->PushClipRect(pMin, pMax, true);
            ui::DrawItemContent(drawList, item, pMin, pMax, 0, false, core::StrokeMeshSlot{&meshes, 1}, hooks);
            drawList->PopClipRect();
        }
        meshes.EndFrame();
        const auto submitted = std::chrono::steady_clock::now();
        context->Begin(disjoint.Get());
        context->End(begin.Get());
        renderer_.RenderTo(targetView.Get());
        context->End(end.Get());
        context->End(disjoint.Get());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
        while (context->GetData(disjoint.Get(), &clock, sizeof(clock), 0) == S_FALSE) {
        }
        lastWallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submitted).count();
        UINT64 from = 0;
        UINT64 to = 0;
        while (context->GetData(begin.Get(), &from, sizeof(from), 0) == S_FALSE) {
        }
        while (context->GetData(end.Get(), &to, sizeof(to), 0) == S_FALSE) {
        }
        return clock.Disjoint ? -1.0 : static_cast<double>(to - from) * 1000.0 / static_cast<double>(clock.Frequency);
    };

    const std::pair<ui::PaintHooks, const char*> ways[] = {
        {ui::PaintHooks{}, "stroke by stroke"},
        {RendererHooks(), "layered"},
    };
    std::vector<double> samples[std::size(ways)];
    std::vector<double> walls[std::size(ways)];
    for (int i = 0; i < 330; ++i) {
        for (size_t w = 0; w < std::size(ways); ++w) {
            const double ms = frameMs(ways[w].first);
            if (i >= 30 && ms >= 0.0) {
                samples[w].push_back(ms);
                walls[w].push_back(lastWallMs);
            }
        }
    }
    for (size_t w = 0; w < std::size(ways); ++w) {
        ASSERT_FALSE(samples[w].empty());
        std::sort(samples[w].begin(), samples[w].end());
        std::sort(walls[w].begin(), walls[w].end());
        std::printf("gpu %-16s min=%.3f median=%.3f p95=%.3f ms | submit+gpu min=%.3f median=%.3f ms\n",
                    ways[w].second, samples[w].front(), samples[w][samples[w].size() / 2],
                    samples[w][samples[w].size() * 95 / 100], walls[w].front(), walls[w][walls[w].size() / 2]);
    }
}

}  // namespace
}  // namespace sz::platform::win32
