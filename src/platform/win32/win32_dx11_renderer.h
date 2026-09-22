#pragma once

#include <windows.h>

#include <cstdint>

#include <d3d11.h>
#include <wrl/client.h>

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

    // Uploads `width`*`height` RGBA8 pixels (row-major, top-left origin, 4
    // bytes/pixel, no row padding) as a new immutable-content GPU texture
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
    // IOverlayWindow::UpdateTextureRegion for why painting needs this
    // rather than re-creating the texture.
    bool UpdateTextureRegionRGBA(ID3D11ShaderResourceView* srv, const uint8_t* pixelsRGBA, int sourceWidth,
                                  int x, int y, int w, int h);
    // Releases a texture returned by CreateTextureFromRGBA. No-op for nullptr.
    void ReleaseTexture(ID3D11ShaderResourceView* srv);

private:
    bool CreateRenderTarget();
    void CleanupRenderTarget();

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView_;
    bool imguiInitialized_ = false;
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
