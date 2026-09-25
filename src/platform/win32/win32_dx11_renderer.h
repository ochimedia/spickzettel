#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

struct ImDrawList;
struct ImDrawCmd;

namespace sz::platform::win32 {

// Thin wrapper around the standard Dear ImGui Win32 + DirectX 11 example
// backend (device/swapchain creation, ImGui_ImplWin32/ImGui_ImplDX11
// init/shutdown, and per-frame New/Render/Present calls). Owns exactly one
// swapchain tied to one HWND for the lifetime of the overlay window.
class Win32Dx11Renderer {
public:
    bool Initialize(HWND hwnd);
    void Shutdown();

    // Recreates just the swapchain's backbuffer/render-target (e.g. on
    // WM_DISPLAYCHANGE), without tearing down the device or ImGui context.
    void HandleResize();

    // Whether a frame can be drawn, first making it so where it can be. A
    // device the driver took away - a driver update, or one restarted after
    // it stopped responding - is replaced, with the swapchain, the shaders
    // and ImGui's own objects on it; a render target a resize could not
    // make is tried again. False while neither works: there is nothing to
    // draw with, and the frame is skipped rather than drawn into nothing.
    // False as well while the window cannot be seen at all - the screen
    // locked, the secure desktop up - which Present said last time and a
    // test present says has not changed: Present returns at once then,
    // without waiting for vsync, and drawing on would spin a core.
    bool ReadyToRender();
    // How many times ReadyToRender has replaced the device. Every texture
    // from before a change is lost - see IOverlayWindow::TextureGeneration.
    uint64_t DeviceGeneration() const { return deviceGeneration_; }
    // Treats the device as lost, as a removed one is, for the next
    // ReadyToRender. For tests: nothing short of a driver can remove one.
    void LoseDeviceForTesting() { deviceLost_ = true; }
    // As if the last Present had found the window occluded.
    void OccludeForTesting() { occluded_ = true; }
    // As a WM_SIZE whose ResizeBuffers failed leaves it.
    void FailResizeForTesting() {
        resizePending_ = true;
        CleanupRenderTarget();
    }
    bool ResizePendingForTesting() const { return resizePending_; }

    // Overrides the mouse position ImGui sees for subsequent frames, in
    // client coordinates - for when the overlay is navigating by its own
    // pointer rather than the OS one (see Win32InputGrab::
    // VirtualCursorActive). Pass active=false to go back to the real
    // cursor.
    void SetMousePositionOverride(bool active, float clientX, float clientY);

    // Overrides the Ctrl/Shift/Alt state ImGui sees, applied once per frame
    // after the Win32 backend's own bookkeeping. The backend learns
    // modifiers from key messages, and key messages need keyboard focus -
    // which the overlay deliberately withholds from itself under
    // dontStealFocus, and which never carries a key the input grab
    // has swallowed. Alt-drag lives or dies by this.
    void SetModifierOverride(bool ctrl, bool shift, bool alt);

    // Whether the overlay is drawing its own pointer, which means the OS
    // cursor has to stay hidden over this window. Applied through ImGui's
    // own io.MouseDrawCursor - see NewFrame for why answering WM_SETCURSOR
    // was not enough on its own.
    void SetSoftwarePointerActive(bool active);

    void NewFrame();
    void RenderAndPresent();
    // RenderAndPresent's drawing, into `target` and without presenting -
    // for tests, which read the pixels back from a target of their own.
    void RenderTo(ID3D11RenderTargetView* target);

    // Uploads `width`*`height` RGBA8 pixels (row-major, top-left origin, 4
    // bytes/pixel, no row padding) as a new GPU texture with a full mip
    // chain (built on the next RefreshMips)
    // and returns its shader-resource-view, ready to hand straight to
    // ImGui as an ImTextureID (see IOverlayWindow::CaptureRegionAsTexture).
    // Caller owns the returned pointer's single reference and must
    // eventually pass it to ReleaseTexture - mirrors the exact
    // CreateTexture2D/CreateShaderResourceView pattern
    // imgui_impl_dx11.cpp's own ImGui_ImplDX11_UpdateTexture uses for the
    // font atlas, so this stays consistent with what the backend already
    // does elsewhere. Returns nullptr on failure.
    ID3D11ShaderResourceView* CreateTextureFromRGBA(const uint8_t* pixelsRGBA, int width, int height);
    // Replaces a rectangle of an existing texture's pixels in place.
    // `pixelsRGBA` is the whole source image, `sourceWidth` its width, and
    // x/y/w/h the part of it that changed - see
    // IOverlayWindow::UpdateTextureRegion.
    bool UpdateTextureRegionRGBA(ID3D11ShaderResourceView* srv, const uint8_t* pixelsRGBA, int sourceWidth,
                                  int x, int y, int w, int h);
    // Releases a texture returned by CreateTextureFromRGBA. No-op for nullptr.
    // Between NewFrame and RenderAndPresent the release waits until the
    // frame has been drawn: ImGui's draw commands carry the raw pointer and
    // no reference, and they are only submitted in RenderAndPresent - so a
    // texture drawn earlier in the frame and released later in it (a tile
    // clicked in the overview, an item moved away) would otherwise be drawn
    // from freed memory.
    void ReleaseTexture(ID3D11ShaderResourceView* srv);

    // IOverlayWindow::ImageFilterCallback. Static because ImGui calls it
    // with nothing but the draw command; the renderer it acts for is the
    // one inside RenderTo, which is the only place it runs.
    static void ApplyImageFilter(const ImDrawList* parentList, const ImDrawCmd* cmd);

    // Brings every mip chain up to date with its top level - what
    // RenderTo does first. Public for tests, which read mips back.
    void RefreshMips();

private:
    bool CreateDeviceAndSwapChain();
    // Everything made on the device, and the device, let go of - what a
    // lost one leaves to replace.
    void ReleaseDevice();
    bool CreateRenderTarget();
    void CleanupRenderTarget();
    // The swapchain's buffers resized to the window, and a target made on
    // them. True once they are; see resizePending_.
    bool ResizeSwapChain();
    // The shaders ImGui does not have: the mip builder and the two
    // resampling filters. False if any failed to compile, which leaves
    // Bicubic and Lanczos drawing as Bilinear rather than the overlay not
    // starting.
    bool CreateFilterShaders();
    // Rebuilds levels 1.. of `srv`'s texture from level 0.
    void BuildMips(ID3D11ShaderResourceView* srv);
    void ForgetTexture(ID3D11ShaderResourceView* srv);
    // Once the frame's draw commands are with D3D, which keeps what they
    // use alive itself from there on.
    void ReleaseDeferredTextures();

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView_;
    bool imguiInitialized_ = false;
    // ImGui's DX11 backend, which is shut down with the device it was
    // given and started again on the next one.
    bool imguiBackendInitialized_ = false;
    // See ReadyToRender.
    bool deviceLost_ = false;
    uint64_t deviceGeneration_ = 0;
    bool occluded_ = false;
    // A resize whose ResizeBuffers failed, tried again by every frame until
    // it works; drawn at the old size, stretched, meanwhile. Not retried,
    // the frames went on stretched until the next WM_SIZE.
    bool resizePending_ = false;
    // See ReleaseTexture.
    bool inFrame_ = false;
    std::vector<ID3D11ShaderResourceView*> releaseAfterFrame_;
    // Textures whose top level changed since their mips were built: every
    // new one, and a stroke raster after each update. Rebuilt once a
    // frame, however many updates there were.
    std::vector<ID3D11ShaderResourceView*> staleMips_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> fullscreenVS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> mipPS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> bicubicPS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> lanczosPS_;
    // See SetMousePositionOverride.
    bool mouseOverrideActive_ = false;
    float mouseOverrideX_ = 0.0f;
    float mouseOverrideY_ = 0.0f;
    // See SetSoftwarePointerActive.
    bool softwarePointerActive_ = false;
    // See SetModifierOverride.
    bool modCtrl_ = false;
    bool modShift_ = false;
    bool modAlt_ = false;
};

}  // namespace sz::platform::win32
