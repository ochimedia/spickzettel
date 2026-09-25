#include "platform/win32/win32_screen_capture.h"

#include <dwmapi.h>

namespace sz::platform::win32 {

namespace {

bool ReadScreen(POINT origin, int width, int height, std::vector<uint8_t>& pixelsBGRA) {
    pixelsBGRA.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    bool captured = false;
    if (HDC screenDC = GetDC(nullptr)) {
        if (HDC memDC = CreateCompatibleDC(screenDC)) {
            if (HBITMAP bitmap = CreateCompatibleBitmap(screenDC, width, height)) {
                HGDIOBJ oldObj = SelectObject(memDC, bitmap);
                // CAPTUREBLT includes layered windows (other apps' own
                // translucent UI) in the capture, matching what's visually
                // on screen rather than just the opaque desktop.
                const BOOL blitted =
                    BitBlt(memDC, 0, 0, width, height, screenDC, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
                // Deselected *before* it is read: GetDIBits documents that
                // the bitmap must not be selected into a DC when it is
                // called. It happened to work while selected, on the
                // drivers tried, which is not the same as being allowed.
                SelectObject(memDC, oldObj);
                if (blitted) {
                    BITMAPINFOHEADER bi{};
                    bi.biSize = sizeof(bi);
                    bi.biWidth = width;
                    bi.biHeight = -height;  // negative = top-down DIB, matching our RGBA row order
                    bi.biPlanes = 1;
                    bi.biBitCount = 32;
                    bi.biCompression = BI_RGB;
                    BITMAPINFO bmi{};
                    bmi.bmiHeader = bi;
                    // Every row, or nothing: a short read is a picture
                    // with garbage along its bottom, not a capture.
                    captured = GetDIBits(memDC, bitmap, 0, static_cast<UINT>(height), pixelsBGRA.data(), &bmi,
                                         DIB_RGB_COLORS) == height;
                }
                DeleteObject(bitmap);
            }
            DeleteDC(memDC);
        }
        ReleaseDC(nullptr, screenDC);
    }
    return captured;
}

// Whether this system leaves a window out of a capture when asked to.
// Windows 10 before 2004 (build 19041) accepts WDA_EXCLUDEFROMCAPTURE all
// the same and treats it as WDA_MONITOR, which captures the window as
// black - so the call succeeding says nothing there, and the build has to
// be asked. RtlGetVersion, since GetVersionEx answers what the manifest
// claims to support rather than what is running.
bool ExcludesFromCapture() {
    static const bool excludes = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto getVersion =
            ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
                  : nullptr;
        RTL_OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (!getVersion || getVersion(&version) != 0) {
            return false;
        }
        return version.dwMajorVersion > 10 || (version.dwMajorVersion == 10 && version.dwBuildNumber >= 19041);
    }();
    return excludes;
}

}  // namespace

bool CaptureScreen(HWND exclude, POINT origin, int width, int height, std::vector<uint8_t>& pixelsBGRA) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (exclude == nullptr) {
        return ReadScreen(origin, width, height, pixelsBGRA);
    }
    // DwmFlush blocks until the next composition pass has happened, which
    // is what makes the change actually take effect in what BitBlt sees
    // before the capture runs - without it, this is a race.
    if (ExcludesFromCapture() && SetWindowDisplayAffinity(exclude, WDA_EXCLUDEFROMCAPTURE)) {
        DwmFlush();
        const bool captured = ReadScreen(origin, width, height, pixelsBGRA);
        SetWindowDisplayAffinity(exclude, WDA_NONE);
        return captured;
    }
    ShowWindow(exclude, SW_HIDE);
    DwmFlush();
    const bool captured = ReadScreen(origin, width, height, pixelsBGRA);
    ShowWindow(exclude, SW_SHOW);
    return captured;
}

}  // namespace sz::platform::win32
