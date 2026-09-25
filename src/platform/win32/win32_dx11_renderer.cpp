#include "platform/win32/win32_dx11_renderer.h"

#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>

#include "platform/platform_types.h"
#include "platform/spickzettel_fonts.h"

namespace sz::platform::win32 {

using Microsoft::WRL::ComPtr;

namespace {

// See ApplyImageFilter.
Win32Dx11Renderer* g_rendering = nullptr;

// One triangle over the whole target, from the vertex index alone - no
// buffers, no input layout. What each mip level is drawn with.
constexpr const char* kFullscreenVS = R"(
float4 main(uint id : SV_VertexID) : SV_POSITION {
    float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

// One mip level from the level above it: each texel the average of the
// four it covers. Averaged premultiplied, which is the whole reason this is
// not ID3D11DeviceContext::GenerateMips: the pictures are straight alpha,
// and a painted layer is mostly (0,0,0,0) around its ink, so a plain
// average darkens every edge toward black as the picture shrinks.
constexpr const char* kMipPS = R"(
Texture2D above : register(t0);
float4 main(float4 pos : SV_POSITION) : SV_Target {
    uint w, h;
    above.GetDimensions(w, h);
    int2 last = int2(w, h) - 1;
    int2 first = int2(pos.xy) * 2;
    float4 sum = 0.0;
    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            float4 c = above.Load(int3(min(first + int2(x, y), last), 0));
            sum += float4(c.rgb * c.a, c.a);
        }
    }
    return sum.a > 0.0 ? float4(sum.rgb / sum.a, sum.a * 0.25) : 0.0;
}
)";

// Bicubic (Catmull-Rom) or Lanczos-3, by LANCZOS. Takes the place of
// ImGui's pixel shader for one picture, so it reads ImGui's vertex output
// and returns what that shader would: straight alpha, times the vertex
// color (the layer's tint and opacity).
//
// Enlarging, the kernel is the textbook one, 4x4 or 6x6 texels. Shrinking,
// a kernel that size would alias just as bilinear does: it has to widen by
// the reduction so that every texel under the pixel counts. Widened all the
// way, a screenshot shown at a tenth of its size would be 60x60 taps a
// pixel for Lanczos. So the mip level just above the target size carries
// the reduction down to under 2x, and the kernel widens by what is left -
// at most 12x12 taps (8x8 bicubic), and far fewer for the usual picture
// shown near its own size. The mips are box-filtered, which is where the
// quality goes; this keeps most of it at a fraction of the cost.
//
// Premultiplied in the sum for the same reason as kMipPS, and clamped
// after, because both kernels have negative lobes that ring past 0 and 1
// at a hard edge.
constexpr const char* kResamplePS = R"(
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv : TEXCOORD0;
};
Texture2D picture : register(t0);

#if LANCZOS
static const float kSupport = 3.0;
float Kernel(float x) {
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= kSupport) return 0.0;
    float px = 3.14159265 * x;
    return kSupport * sin(px) * sin(px / kSupport) / (px * px);
}
#else
static const float kSupport = 2.0;
float Kernel(float x) {
    x = abs(x);
    if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}
#endif
// Taps along one axis with the kernel at its widest, 2x.
static const int kMaxTaps = int(4.0 * kSupport) + 1;

float4 main(PS_INPUT input) : SV_Target {
    uint fullW, fullH, levels;
    picture.GetDimensions(0, fullW, fullH, levels);
    // Top-level texels per screen pixel, along each axis of the picture.
    float2 footprint = float2(length(float2(ddx(input.uv.x), ddy(input.uv.x))) * fullW,
                              length(float2(ddx(input.uv.y), ddy(input.uv.y))) * fullH);
    // By the lesser reduction, so a picture squeezed along one axis is not
    // blurred along the other; the kernel's cap then lets the squeezed
    // axis alias a little rather than cost more.
    uint level = uint(clamp(floor(log2(max(min(footprint.x, footprint.y), 1.0))), 0.0, float(levels - 1)));
    uint w, h;
    picture.GetDimensions(level, w, h, levels);
    float2 size = float2(w, h);
    float2 stretch = clamp(footprint * size / float2(fullW, fullH), 1.0, 2.0);
    float2 radius = kSupport * stretch;

    float2 center = input.uv * size - 0.5;
    int2 first = int2(ceil(center - radius));
    int2 count = min(int2(floor(center + radius)) - first + 1, kMaxTaps);
    float weightX[kMaxTaps];
    float weightY[kMaxTaps];
    float sumX = 0.0;
    float sumY = 0.0;
    [unroll] for (int i = 0; i < kMaxTaps; ++i) {
        weightX[i] = i < count.x ? Kernel((first.x + i - center.x) / stretch.x) : 0.0;
        weightY[i] = i < count.y ? Kernel((first.y + i - center.y) / stretch.y) : 0.0;
        sumX += weightX[i];
        sumY += weightY[i];
    }

    int2 last = int2(w, h) - 1;
    float4 sum = 0.0;
    [loop] for (int y = 0; y < count.y; ++y) {
        int row = clamp(first.y + y, 0, last.y);
        float4 rowSum = 0.0;
        [loop] for (int x = 0; x < count.x; ++x) {
            float4 c = picture.Load(int3(clamp(first.x + x, 0, last.x), row, level));
            rowSum += weightX[x] * float4(c.rgb * c.a, c.a);
        }
        sum += weightY[y] * rowSum;
    }
    sum /= sumX * sumY;

    float alpha = saturate(sum.a);
    float3 rgb = sum.a > 1.0 / 1024.0 ? saturate(sum.rgb / sum.a) : 0.0;
    return float4(rgb, alpha) * input.col;
}
)";

ComPtr<ID3DBlob> CompileShader(const char* source, const char* target, const D3D_SHADER_MACRO* defines = nullptr) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3DCompile(source, std::strlen(source), nullptr, defines, nullptr, "main", target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        if (errors) {
            OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        }
        return nullptr;
    }
    return code;
}

}  // namespace

bool Win32Dx11Renderer::Initialize(HWND hwnd) {
    hwnd_ = hwnd;
    if (!CreateDeviceAndSwapChain() || !CreateRenderTarget()) {
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

    // Failing past here, the context made above goes too: Shutdown only
    // destroys one that got as far as imguiInitialized_.
    if (!ImGui_ImplWin32_Init(hwnd_)) {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX11_Init(device_.Get(), context_.Get())) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    imguiInitialized_ = true;
    imguiBackendInitialized_ = true;
    CreateFilterShaders();
    return true;
}

bool Win32Dx11Renderer::CreateDeviceAndSwapChain() {
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
    return SUCCEEDED(hr);
}

bool Win32Dx11Renderer::CreateFilterShaders() {
    const D3D_SHADER_MACRO bicubic[] = {{"LANCZOS", "0"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO lanczos[] = {{"LANCZOS", "1"}, {nullptr, nullptr}};
    const ComPtr<ID3DBlob> vs = CompileShader(kFullscreenVS, "vs_4_0");
    const ComPtr<ID3DBlob> mip = CompileShader(kMipPS, "ps_4_0");
    const ComPtr<ID3DBlob> cubic = CompileShader(kResamplePS, "ps_4_0", bicubic);
    const ComPtr<ID3DBlob> lanczos3 = CompileShader(kResamplePS, "ps_4_0", lanczos);
    return vs && mip && cubic && lanczos3 &&
           SUCCEEDED(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                                 &fullscreenVS_)) &&
           SUCCEEDED(device_->CreatePixelShader(mip->GetBufferPointer(), mip->GetBufferSize(), nullptr, &mipPS_)) &&
           SUCCEEDED(device_->CreatePixelShader(cubic->GetBufferPointer(), cubic->GetBufferSize(), nullptr,
                                                &bicubicPS_)) &&
           SUCCEEDED(device_->CreatePixelShader(lanczos3->GetBufferPointer(), lanczos3->GetBufferSize(), nullptr,
                                                &lanczosPS_));
}

void Win32Dx11Renderer::Shutdown() {
    ReleaseDevice();
    if (imguiInitialized_) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized_ = false;
    }
}

void Win32Dx11Renderer::ReleaseDevice() {
    ReleaseDeferredTextures();
    if (imguiBackendInitialized_) {
        ImGui_ImplDX11_Shutdown();
        imguiBackendInitialized_ = false;
    }
    CleanupRenderTarget();
    staleMips_.clear();
    fullscreenVS_.Reset();
    mipPS_.Reset();
    bicubicPS_.Reset();
    lanczosPS_.Reset();
    swapChain_.Reset();
    context_.Reset();
    device_.Reset();
}

bool Win32Dx11Renderer::ReadyToRender() {
    if (!imguiInitialized_) {
        return false;
    }
    if (device_ && !deviceLost_ && device_->GetDeviceRemovedReason() == S_OK) {
        // Asked without drawing anything, and only once a present has said
        // so, so an ordinary frame pays nothing for it.
        if (occluded_) {
            if (swapChain_->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                return false;
            }
            occluded_ = false;
        }
        // A resize that could not make its target leaves none, and a frame
        // drawn into none is an access violation inside d3d11.dll.
        return renderTargetView_ || CreateRenderTarget();
    }
    // Replaced whole, in place: the ImGui context and the window stay, and
    // with them everything the app has laid out. The textures the app holds
    // were made on the old device and cannot be moved across; the new
    // generation is what tells it to make them again.
    if (device_) {
        ReleaseDevice();
        deviceLost_ = false;
        ++deviceGeneration_;
    }
    if (!CreateDeviceAndSwapChain() || !CreateRenderTarget() ||
        !ImGui_ImplDX11_Init(device_.Get(), context_.Get())) {
        // The driver may still be on its way back. Tried again next frame.
        ReleaseDevice();
        return false;
    }
    imguiBackendInitialized_ = true;
    CreateFilterShaders();
    return true;
}

bool Win32Dx11Renderer::CreateRenderTarget() {
    if (!swapChain_) {
        return false;
    }
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
    // Either failing leaves no target, which ReadyToRender tries again, or
    // a lost device, which it replaces.
    if (SUCCEEDED(swapChain_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0))) {
        CreateRenderTarget();
    }
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
    inFrame_ = true;
}

ID3D11ShaderResourceView* Win32Dx11Renderer::CreateTextureFromRGBA(const uint8_t* pixelsRGBA, int width,
                                                                     int height) {
    if (!device_ || width <= 0 || height <= 0) {
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    // The whole chain, down to 1x1. Bilinear and Nearest never look past
    // the top level (ImGui's samplers clamp to it), so what the chain buys
    // is Bicubic and Lanczos shrinking without aliasing - for a third more
    // memory per picture. See kResamplePS.
    desc.MipLevels = 0;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    // DEFAULT rather than IMMUTABLE: a painted layer's texture is written
    // to over and over as a brush moves across it (see
    // UpdateTextureRegionRGBA), and IMMUTABLE forbids that outright. A
    // screenshot never takes that path and pays nothing for the difference
    // - the GPU-side placement is the same, only the promise is weaker.
    desc.Usage = D3D11_USAGE_DEFAULT;
    // A render target as well, because that is how BuildMips writes the
    // levels below the top.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    // No initial data: with a mip chain it would have to be given for
    // every level, and only the top one exists yet.
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture))) {
        return nullptr;
    }
    context_->UpdateSubresource(texture.Get(), 0, nullptr, pixelsRGBA, static_cast<UINT>(width) * 4, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = static_cast<UINT>(-1);

    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(device_->CreateShaderResourceView(texture.Get(), &srvDesc, &srv))) {
        return nullptr;
    }
    // The texture itself stays alive via the SRV's own reference to it
    // (AddRef'd internally by CreateShaderResourceView) - `texture` going
    // out of scope here just drops our extra ComPtr reference, not the
    // last one.
    staleMips_.push_back(srv);
    return srv;
}

bool Win32Dx11Renderer::UpdateTextureRegionRGBA(ID3D11ShaderResourceView* srv, const uint8_t* pixelsRGBA,
                                                  int sourceWidth, int x, int y, int w, int h) {
    if (!context_ || !srv || !pixelsRGBA || sourceWidth <= 0 || w <= 0 || h <= 0 || x < 0 || y < 0) {
        return false;
    }
    // One made on a device since replaced cannot be written with this
    // one's context. The app lets go of those (see ReadyToRender), but a
    // stray one is refused here rather than handed to D3D.
    ComPtr<ID3D11Device> madeOn;
    srv->GetDevice(&madeOn);
    if (madeOn.Get() != device_.Get()) {
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    srv->GetResource(&resource);
    if (!resource) {
        return false;
    }
    // Inside the texture, and inside the rows it is read from: a box past
    // the texture's edge is undefined behavior in UpdateSubresource, and a
    // window past the source's edge reads beyond the caller's buffer.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const int64_t right = static_cast<int64_t>(x) + w;
    const int64_t bottom = static_cast<int64_t>(y) + h;
    if (right > desc.Width || bottom > desc.Height || right > sourceWidth) {
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
    if (std::find(staleMips_.begin(), staleMips_.end(), srv) == staleMips_.end()) {
        staleMips_.push_back(srv);
    }
    return true;
}

void Win32Dx11Renderer::ReleaseTexture(ID3D11ShaderResourceView* srv) {
    if (!srv) {
        return;
    }
    if (inFrame_) {
        releaseAfterFrame_.push_back(srv);
        return;
    }
    ForgetTexture(srv);
    srv->Release();
}

void Win32Dx11Renderer::ForgetTexture(ID3D11ShaderResourceView* srv) {
    staleMips_.erase(std::remove(staleMips_.begin(), staleMips_.end(), srv), staleMips_.end());
}

void Win32Dx11Renderer::ReleaseDeferredTextures() {
    for (ID3D11ShaderResourceView* srv : releaseAfterFrame_) {
        ForgetTexture(srv);
        srv->Release();
    }
    releaseAfterFrame_.clear();
    inFrame_ = false;
}

void Win32Dx11Renderer::RefreshMips() {
    for (ID3D11ShaderResourceView* srv : staleMips_) {
        BuildMips(srv);
    }
    staleMips_.clear();
}

void Win32Dx11Renderer::BuildMips(ID3D11ShaderResourceView* srv) {
    if (!fullscreenVS_ || !mipPS_) {
        return;
    }
    ComPtr<ID3D11Resource> resource;
    srv->GetResource(&resource);
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource.As(&texture))) {
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(fullscreenVS_.Get(), nullptr, 0);
    context_->PSSetShader(mipPS_.Get(), nullptr, 0);
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    context_->OMSetDepthStencilState(nullptr, 0);
    context_->RSSetState(nullptr);
    ID3D11ShaderResourceView* const noTexture = nullptr;
    for (UINT level = 1; level < desc.MipLevels; ++level) {
        D3D11_SHADER_RESOURCE_VIEW_DESC aboveDesc{};
        aboveDesc.Format = desc.Format;
        aboveDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        aboveDesc.Texture2D.MostDetailedMip = level - 1;
        aboveDesc.Texture2D.MipLevels = 1;
        D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
        targetDesc.Format = desc.Format;
        targetDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        targetDesc.Texture2D.MipSlice = level;
        ComPtr<ID3D11ShaderResourceView> above;
        ComPtr<ID3D11RenderTargetView> target;
        if (FAILED(device_->CreateShaderResourceView(texture.Get(), &aboveDesc, &above)) ||
            FAILED(device_->CreateRenderTargetView(texture.Get(), &targetDesc, &target))) {
            break;
        }
        // Unbound before the next level is bound as the target: D3D will
        // not have one resource as input and output at once.
        context_->PSSetShaderResources(0, 1, &noTexture);
        context_->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(std::max(1u, desc.Width >> level));
        viewport.Height = static_cast<float>(std::max(1u, desc.Height >> level));
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        context_->PSSetShaderResources(0, 1, above.GetAddressOf());
        context_->Draw(3, 0);
    }
    context_->PSSetShaderResources(0, 1, &noTexture);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
}

void Win32Dx11Renderer::ApplyImageFilter(const ImDrawList* parentList, const ImDrawCmd* cmd) {
    Win32Dx11Renderer* const self = g_rendering;
    if (!self) {
        return;
    }
    // Everything ImGui set up for its own shader stays as it is - the
    // vertex shader, the blend, the sampler - so this is one swap, and
    // DrawCallback_ResetRenderState undoes it.
    switch (static_cast<ImageFilter>(reinterpret_cast<intptr_t>(cmd->UserCallbackData))) {
        case ImageFilter::Nearest:
            if (const ImDrawCallback nearest = ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest) {
                nearest(parentList, cmd);
            }
            break;
        case ImageFilter::Bicubic:
            if (self->bicubicPS_) {
                self->context_->PSSetShader(self->bicubicPS_.Get(), nullptr, 0);
            }
            break;
        case ImageFilter::Lanczos:
            if (self->lanczosPS_) {
                self->context_->PSSetShader(self->lanczosPS_.Get(), nullptr, 0);
            }
            break;
        case ImageFilter::Bilinear:
            break;
    }
}

void Win32Dx11Renderer::RenderAndPresent() {
    RenderTo(renderTargetView_.Get());
    if (!renderTargetView_) {
        return;
    }
    const HRESULT presented = swapChain_->Present(1, 0);  // vsync-paced; avoids busy-spinning while visible
    if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET) {
        deviceLost_ = true;
    } else if (presented == DXGI_STATUS_OCCLUDED) {
        occluded_ = true;
    }
}

void Win32Dx11Renderer::RenderTo(ID3D11RenderTargetView* target) {
    ImGui::Render();
    if (!target || !context_) {
        // Nothing to draw into: the frame ends all the same, so ImGui is
        // ready for the next one, and what waited for it is let go.
        ReleaseDeferredTextures();
        return;
    }

    // Before the frame's own target is bound, since building mips binds
    // targets of its own.
    RefreshMips();

    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // fully transparent backdrop
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->ClearRenderTargetView(target, clearColor);
    g_rendering = this;
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_rendering = nullptr;

    ReleaseDeferredTextures();
}

}  // namespace sz::platform::win32
