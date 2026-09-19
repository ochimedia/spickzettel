#include "platform/win32/win32_dx11_renderer.h"

#include <dxgi.h>

#include <algorithm>
#include <array>
#include <iterator>

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include "platform/spickzettel_fonts.h"

namespace sz::platform::win32 {

using Microsoft::WRL::ComPtr;

bool Win32Dx11Renderer::Initialize(HWND hwnd) {
    hwnd_ = hwnd;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd_;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags, featureLevels,
        static_cast<UINT>(std::size(featureLevels)), D3D11_SDK_VERSION, &desc, &swapChain_, &device_,
        &featureLevel, &context_);
    if (FAILED(hr)) {
        return false;
    }

    if (!CreateRenderTarget()) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // ImGuiConfigFlags_NoMouseCursorChange is deliberately *not* set:
    // leaving it clear lets ImGui_ImplWin32_WndProcHandler's WM_SETCURSOR
    // handling swap the OS cursor per ImGui::GetMouseCursor() every frame
    // (the resize handles' arrows, the dock's hand). Set, it silently
    // swallows every SetMouseCursor call in the app.
    // Must run before ImGui_ImplWin32_Init/ImGui_ImplDX11_Init below - see
    // LoadSpickzettelFonts's own doc comment for why the ordering matters.
    LoadSpickzettelFonts(io);

    if (!ImGui_ImplWin32_Init(hwnd_)) {
        return false;
    }
    if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) {
        ImGui_ImplWin32_Shutdown();
        return false;
    }
    imguiInitialized_ = true;
    return true;
}

void Win32Dx11Renderer::Shutdown() {
    if (imguiInitialized_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized_ = false;
    }
    CleanupRenderTarget();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
}

bool Win32Dx11Renderer::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        return false;
    }
    return SUCCEEDED(device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTargetView_));
}

void Win32Dx11Renderer::CleanupRenderTarget() { renderTargetView_.Reset(); }

void Win32Dx11Renderer::HandleResize() {
    if (!swapChain_) {
        return;
    }
    CleanupRenderTarget();
    swapChain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
}

void Win32Dx11Renderer::SetMousePositionOverride(bool active, float clientX, float clientY) {
    mouseOverrideActive_ = active;
    mouseOverrideX_ = clientX;
    mouseOverrideY_ = clientY;
}

void Win32Dx11Renderer::SetModifierOverride(bool ctrl, bool shift, bool alt) {
    modCtrl_ = ctrl;
    modShift_ = shift;
    modAlt_ = alt;
}

void Win32Dx11Renderer::SetSoftwarePointerActive(bool active) { softwarePointerActive_ = active; }

void Win32Dx11Renderer::NewFrame() {
    // Before the backend's own frame setup, because that is where it reads
    // this flag. io.MouseDrawCursor is ImGui's own "the app draws the
    // pointer" switch: with it set, the Win32 backend resolves the wanted
    // cursor to ImGuiMouseCursor_None and calls SetCursor(nullptr) on both
    // of its paths.
    //
    // Both paths is the whole point. Answering WM_SETCURSOR was not enough:
    // the backend also installs a cursor once per frame from NewFrame,
    // whenever ImGui's wanted shape changes, and knows nothing about the
    // grab - so moving onto a resize handle or a text field re-installed a
    // real cursor behind the drawn one. And WM_SETCURSOR is sent when the
    // cursor moves over the window, which under a mouse grab it has stopped
    // doing, so the message that would have hidden it again barely arrives.
    // That was the native pointer appearing intermittently.
    ImGui::GetIO().MouseDrawCursor = softwarePointerActive_;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // Deliberately between the backend's frame setup and ImGui's own: the
    // Win32 backend reads the real cursor with GetCursorPos and submits it
    // as a position event, and under an input grab the real cursor is the
    // game's to do what it likes with (see Win32InputGrab). Submitting ours
    // afterwards means the later event wins, and it has to land before
    // ImGui::NewFrame consumes the queue to decide what is hovered.
    if (mouseOverrideActive_) {
        ImGui::GetIO().AddMousePosEvent(mouseOverrideX_, mouseOverrideY_);
    }
    // Same placement, same reason: the backend has just finished its own
    // modifier bookkeeping (including its Shift/Win release workarounds),
    // and these must land after it so they are what ImGui::NewFrame reads.
    // AddKeyEvent ignores a value that hasn't changed, so this is free on
    // the frames where nothing moved.
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, modCtrl_);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, modShift_);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Alt, modAlt_);
    // The gap since the last frame, not the gap since the last *shown*
    // frame. The Win32 backend keeps its own clock and never hears that
    // the overlay was hidden, so the first frame after an hour in the tray
    // arrives with a DeltaTime of an hour - and ImGui's own clock, which
    // is that sum, jumps by it. Everything timed against ImGui::GetTime()
    // is then already in the past: a message meant to last two seconds is
    // over before it is drawn once, which is exactly what a capture taken
    // while hidden looked like - the shot landed and nothing was ever
    // said. Win32OverlayWindow::Show already resets the app's *own* delta
    // for the same reason; this is the same fix for the clock ImGui keeps.
    //
    // A ceiling rather than a reset, because it costs nothing to apply
    // every frame and covers every other way a frame can be late (a
    // display change, a laptop coming out of sleep) without anything
    // having to remember to tell us. Well above a slow frame at 30Hz, so
    // an ordinary hitch is left alone.
    constexpr float kMaxFrameDelta = 0.1f;
    ImGui::GetIO().DeltaTime = std::min(ImGui::GetIO().DeltaTime, kMaxFrameDelta);
    ImGui::NewFrame();
}

ID3D11ShaderResourceView* Win32Dx11Renderer::CreateTextureFromRGBA(const uint8_t* pixelsRGBA, int width,
                                                                     int height) {
    if (!device_ || width <= 0 || height <= 0) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    // DEFAULT rather than IMMUTABLE: a painted layer's texture is written
    // to over and over as a brush moves across it (see
    // UpdateTextureRegionRGBA), and IMMUTABLE forbids that outright. A
    // screenshot never takes that path and pays nothing for the difference
    // - the GPU-side placement is the same, only the promise is weaker.
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subResource{};
    subResource.pSysMem = pixelsRGBA;
    subResource.SysMemPitch = static_cast<UINT>(width) * 4;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device_->CreateTexture2D(&desc, &subResource, &texture))) {
        return nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = desc.MipLevels;

    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(device_->CreateShaderResourceView(texture.Get(), &srvDesc, &srv))) {
        return nullptr;
    }
    // The texture itself stays alive via the SRV's own reference to it
    // (AddRef'd internally by CreateShaderResourceView) - `texture` going
    // out of scope here just drops our extra ComPtr reference, not the
    // last one.
    return srv;
}

bool Win32Dx11Renderer::UpdateTextureRegionRGBA(ID3D11ShaderResourceView* srv, const uint8_t* pixelsRGBA,
                                                  int sourceWidth, int x, int y, int w, int h) {
    if (!context_ || !srv || !pixelsRGBA || sourceWidth <= 0 || w <= 0 || h <= 0 || x < 0 || y < 0) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    srv->GetResource(&resource);
    if (!resource) {
        return false;
    }
    // The destination box, and a source pointer at the box's own first
    // pixel with the *full* row stride - UpdateSubresource walks the source
    // by that stride, so the caller hands over the whole image and this
    // reads the window it needs out of it rather than the caller packing a
    // copy first.
    D3D11_BOX box{};
    box.left = static_cast<UINT>(x);
    box.top = static_cast<UINT>(y);
    box.front = 0;
    box.right = static_cast<UINT>(x + w);
    box.bottom = static_cast<UINT>(y + h);
    box.back = 1;
    const uint8_t* first = pixelsRGBA + (static_cast<size_t>(y) * sourceWidth + x) * 4;
    context_->UpdateSubresource(resource.Get(), 0, &box, first, static_cast<UINT>(sourceWidth) * 4, 0);
    return true;
}

void Win32Dx11Renderer::ReleaseTexture(ID3D11ShaderResourceView* srv) {
    if (srv) {
        srv->Release();
    }
}

void Win32Dx11Renderer::RenderAndPresent() {
    ImGui::Render();

    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // fully transparent backdrop
    context_->OMSetRenderTargets(1, renderTargetView_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(renderTargetView_.Get(), clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    swapChain_->Present(1, 0);  // vsync-paced; avoids busy-spinning while visible
}

}  // namespace sz::platform::win32
