#include "platform/win32/win32_displays.h"

#include <windows.h>

#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

#include "platform/win32/win32_text.h"

namespace sz::platform::win32 {

namespace {

struct MonitorDetails {
    std::string id;
    std::string name;
    int refreshHz = 0;
};

// What QueryDisplayConfig knows about each active display, by the GDI name
// of the source it shows. A cloned display is one source driving several
// monitors, and one HMONITOR; the first monitor found stands for it.
std::unordered_map<std::wstring, MonitorDetails> DetailsBySourceName() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG status = ERROR_SUCCESS;
    // The configuration can change between asking for the sizes and asking
    // for the data, which is what ERROR_INSUFFICIENT_BUFFER means here.
    do {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
            return {};
        }
        paths.resize(pathCount);
        modes.resize(modeCount);
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
                                    nullptr);
        paths.resize(pathCount);
    } while (status == ERROR_INSUFFICIENT_BUFFER);
    if (status != ERROR_SUCCESS) {
        return {};
    }

    std::unordered_map<std::wstring, MonitorDetails> details;
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
            continue;
        }
        MonitorDetails monitor;
        monitor.id = Narrow(target.monitorDevicePath);
        monitor.name = Narrow(target.monitorFriendlyDeviceName);
        const DISPLAYCONFIG_RATIONAL& rate = path.targetInfo.refreshRate;
        if (rate.Denominator != 0) {
            monitor.refreshHz =
                static_cast<int>(std::lround(static_cast<double>(rate.Numerator) / rate.Denominator));
        }
        details.try_emplace(source.viewGdiDeviceName, std::move(monitor));
    }
    return details;
}

// "\\.\DISPLAY2" as "Display 2" - for a monitor whose EDID gives no name,
// which internal laptop panels often don't.
std::string NameFromGdiName(std::wstring_view gdiName) {
    constexpr std::wstring_view kPrefix = L"\\\\.\\DISPLAY";
    if (gdiName.substr(0, kPrefix.size()) == kPrefix) {
        return "Display " + Narrow(gdiName.substr(kPrefix.size()));
    }
    return Narrow(gdiName);
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC /*dc*/, LPRECT /*rect*/, LPARAM param) {
    auto& monitors = *reinterpret_cast<std::vector<HMONITOR>*>(param);
    monitors.push_back(monitor);
    return TRUE;
}

}  // namespace

std::vector<DisplayInfo> EnumerateDisplays() {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, &CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    const std::unordered_map<std::wstring, MonitorDetails> details = DetailsBySourceName();

    std::vector<DisplayInfo> displays;
    for (HMONITOR monitor : monitors) {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) {
            continue;
        }
        DisplayInfo display;
        // Physical pixels, because the process is per-monitor DPI aware (see
        // main_win32.cpp) - the same pixels the overlay's window is sized in.
        display.x = info.rcMonitor.left;
        display.y = info.rcMonitor.top;
        display.width = info.rcMonitor.right - info.rcMonitor.left;
        display.height = info.rcMonitor.bottom - info.rcMonitor.top;
        display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        UINT dpiX = 0;
        UINT dpiY = 0;
        if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX > 0) {
            display.scalePercent = MulDiv(static_cast<int>(dpiX), 100, USER_DEFAULT_SCREEN_DPI);
        }
        if (const auto found = details.find(info.szDevice); found != details.end()) {
            display.id = found->second.id;
            display.name = found->second.name;
            display.refreshHz = found->second.refreshHz;
        }
        if (display.id.empty()) {
            display.id = Narrow(info.szDevice);
        }
        if (display.name.empty()) {
            display.name = NameFromGdiName(info.szDevice);
        }
        displays.push_back(std::move(display));
    }
    std::sort(displays.begin(), displays.end(), [](const DisplayInfo& a, const DisplayInfo& b) {
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    return displays;
}

}  // namespace sz::platform::win32
