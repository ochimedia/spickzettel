#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include <backends/imgui_impl_win32.h>

#include "app/tray_app.h"
#include "core/build_info/build_info.h"
#include "core/config/app_config.h"
#include "generated/ui_strings.h"
#include "platform/i_platform_host.h"

namespace {

// Reads the config file if present; otherwise writes out the defaults so
// the user has something to edit, and returns them.
sz::core::AppConfig LoadOrCreateConfig(const std::filesystem::path& path) {
    if (const std::optional<sz::core::AppConfig> config = sz::core::ReadConfigFile(path)) {
        return *config;
    }

    const sz::core::AppConfig config = sz::core::DefaultConfig();
    sz::core::WriteConfigFile(path, config);
    return config;
}

// A prerelease build's notice, at every start and before anything else
// comes up. Native rather than drawn by the overlay: on most starts the
// overlay is not shown at all, and on a first run it comes up fullscreen,
// topmost and in edit mode - the box is shown first so it is neither
// hidden behind that nor competing with it for input. Blocks until
// dismissed, which is the point.
void ShowPrereleaseNotice() {
    const std::string version = sz::core::build::VersionLine();
    char body[512];
    std::snprintf(body, sizeof(body), sz::strings::kPrereleaseBody, version.c_str());
    MessageBoxA(nullptr, body, sz::strings::kPrereleaseTitle,
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
}

}  // namespace

int WINAPI WinMain(HINSTANCE /*instance*/, HINSTANCE /*prevInstance*/, LPSTR /*cmdLine*/, int /*showCmd*/) {
    // Must happen before any window is created: without this, a fullscreen
    // window's size/coordinates get DPI-virtualized by Windows on any scaled
    // display (125%/150% is common), which can misalign the overlay against
    // real screen/mouse coordinates.
    ImGui_ImplWin32_EnableDpiAwareness();

    auto host = sz::platform::CreatePlatformHost();
    if (!host->Initialize("Spickzettel")) {
        return 1;
    }

    const sz::core::AppConfig config = LoadOrCreateConfig(host->GetConfigFilePath());

    if constexpr (sz::core::build::kPrereleaseNotice) {
        ShowPrereleaseNotice();
    }

    sz::app::TrayController trayController(*host, config);
    if (!trayController.Initialize()) {
        // A message box because there is no tray icon yet to hang a
        // notice on, and a tray app that starts and silently isn't there
        // is indistinguishable from one that never started.
        MessageBoxA(nullptr, sz::strings::kStartupFailed, "Spickzettel", MB_OK | MB_ICONWARNING);
        return 1;
    }

    return host->RunEventLoop();
}
