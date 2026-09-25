#include "platform/win32/win32_dx11_renderer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include <imgui.h>

#include "platform/platform_types.h"

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

// `subresource` of `resource`, copied out through a staging texture.
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
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, resource, subresource, nullptr);
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

TEST_F(Win32Dx11RendererTest, WithItsDeviceInPlaceTheRendererIsReadyAsItWas) {
    EXPECT_TRUE(renderer_.ReadyToRender());
    EXPECT_EQ(renderer_.DeviceGeneration(), 0u);
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

}  // namespace
}  // namespace sz::platform::win32
