#include "platform/win32/win32_dx11_renderer.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace sz::platform::win32 {
namespace {

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

    HWND hwnd_ = nullptr;
    Win32Dx11Renderer renderer_;
};

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

}  // namespace
}  // namespace sz::platform::win32
