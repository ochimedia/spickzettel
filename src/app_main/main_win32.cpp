#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <backends/imgui_impl_win32.h>

#include "app/tray_app.h"
#include "core/config/app_config.h"
#include "generated/ui_strings.h"
#include "platform/i_platform_host.h"

namespace {

// Reads the config file if present; otherwise writes out the defaults so
// the user has something to edit, and returns them.
sz::core::AppConfig LoadOrCreateConfig(const std::filesystem::path& path) {
    if (std::ifstream in(path); in) {
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return sz::core::ParseConfig(buffer.str());
    }

    const sz::core::AppConfig config = sz::core::DefaultConfig();
    sz::core::WriteConfigFile(path, config);
    return config;
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

    sz::app::TrayController trayController(*host, config);
    if (!trayController.Initialize()) {
        // The one message box in the app: there is no tray icon yet to
        // hang a notice on, and a tray app that starts and silently isn't
        // there is indistinguishable from one that never started.
        MessageBoxA(nullptr, sz::strings::kStartupFailed, "Spickzettel", MB_OK | MB_ICONWARNING);
        return 1;
    }

    return host->RunEventLoop();
}
