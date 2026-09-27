#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include <backends/imgui_impl_win32.h>

#include "app/tray_app.h"
#include "core/build_info/build_info.h"
#include "core/config/app_config.h"
#include "core/util/timestamp_name.h"
#include "generated/ui_strings.h"
#include "platform/i_platform_host.h"
#include "platform/win32/win32_crash_dump.h"

namespace {

// The settings to start on - see sz::core::LoadOrCreateConfig - and, when
// they are defaults standing in for a file that could not be read, a word
// about it before anything else comes up: settings that silently went back
// to the defaults are the kind of thing nobody connects to a stray comma.
// The same for a file a newer build wrote, whose changes this run will not
// save.
sz::core::LoadedConfig LoadConfig(const std::filesystem::path& path) {
    std::string stamp = sz::core::TimestampName();
    for (char& c : stamp) {
        if (c == ' ' || c == ':') {
            c = '-';
        }
    }
    sz::core::LoadedConfig loaded = sz::core::LoadOrCreateConfig(path, stamp);
    if (loaded.source == sz::core::ConfigSource::SetAside || loaded.source == sz::core::ConfigSource::Unreadable ||
        loaded.source == sz::core::ConfigSource::Newer) {
        char body[1024];
        if (loaded.source == sz::core::ConfigSource::Newer) {
            std::snprintf(body, sizeof(body), sz::strings::kStartupConfigNewer, path.string().c_str());
        } else if (!loaded.setAsideAs.empty()) {
            std::snprintf(body, sizeof(body), sz::strings::kStartupConfigSetAside, path.string().c_str(),
                          loaded.setAsideAs.filename().string().c_str());
        } else {
            std::snprintf(body, sizeof(body), sz::strings::kStartupConfigUnreadable, path.string().c_str());
        }
        MessageBoxA(nullptr, body, "Spickzettel", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }
    return loaded;
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
    // First, so that a crash anywhere after it - starting up included -
    // leaves a dump to be sent in. Beside the library and config.json,
    // in %APPDATA%\Spickzettel\crashes.
    sz::platform::win32::InstallCrashDumpWriter(host->GetLibraryPath().parent_path() / "crashes",
                                                sz::core::build::VersionLine());
    if (!host->Initialize("Spickzettel")) {
        return 1;
    }
    // Before the config is read: a second copy that could not read it
    // would set it aside and write defaults in its place - under the copy
    // already running, which then saves over both - and show the prerelease
    // notice and the set-aside message before giving up. TrayController
    // asks again, and is answered from the mutex already held.
    if (!host->AcquireSingleInstance()) {
        MessageBoxA(nullptr, sz::strings::kStartupFailed, "Spickzettel", MB_OK | MB_ICONWARNING);
        return 1;
    }

    const sz::core::LoadedConfig config = LoadConfig(host->GetConfigFilePath());

    if constexpr (sz::core::build::kPrereleaseNotice) {
        ShowPrereleaseNotice();
    }

    sz::app::TrayController trayController(*host, config.config);
    // Kept, and the retention period skipped, for a file that could not be
    // read and for one a newer build wrote: whether retention is on, and
    // for how long, is what this build cannot be sure it read.
    if (config.source == sz::core::ConfigSource::SetAside || config.source == sz::core::ConfigSource::Unreadable ||
        config.source == sz::core::ConfigSource::Newer) {
        trayController.StartOnStandInSettings(/*keepFile=*/config.setAsideAs.empty());
    }
    if (config.writeBack) {
        trayController.WriteConfigAtStart();
    }
    if (!trayController.Initialize()) {
        // A message box because there is no tray icon yet to hang a
        // notice on, and a tray app that starts and silently isn't there
        // is indistinguishable from one that never started.
        char body[1024];
        if (trayController.RefusedAnUnreadableLibrary()) {
            std::snprintf(body, sizeof(body), sz::strings::kStartupLibraryUnreadable,
                          trayController.LibraryPath().string().c_str());
        } else {
            std::snprintf(body, sizeof(body), "%s",
                          trayController.RefusedANewerLibrary() ? sz::strings::kStartupNewerLibrary
                                                                : sz::strings::kStartupFailed);
        }
        MessageBoxA(nullptr, body, "Spickzettel", MB_OK | MB_ICONWARNING);
        return 1;
    }
    // What the start found is said before the overlay comes up - see
    // TrayController::Start, and ShowPrereleaseNotice for the same reason.
    //
    // Started on an empty library because the file there could not be read:
    // said once, with where it was kept.
    if (!trayController.LibrarySetAsideAs().empty()) {
        char body[1024];
        std::snprintf(body, sizeof(body), sz::strings::kStartupLibrarySetAside,
                      trayController.LibraryPath().string().c_str(),
                      trayController.LibrarySetAsideAs().filename().string().c_str());
        MessageBoxA(nullptr, body, "Spickzettel", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }

    // Started without some of its hotkeys: said once, naming each and the
    // way round it, rather than a tray app whose hotkey silently does
    // nothing.
    if (!trayController.UnregisteredHotkeys().empty()) {
        std::string list;
        for (const auto& [slot, combo] : trayController.UnregisteredHotkeys()) {
            const char* label = slot == sz::core::HotkeySlot::EditMode       ? sz::strings::kHotkeysEditMode
                                : slot == sz::core::HotkeySlot::ViewMode     ? sz::strings::kHotkeysViewMode
                                : slot == sz::core::HotkeySlot::QuickCapture ? sz::strings::kHotkeysQuickCapture
                                                                             : sz::strings::kHotkeysSilentCapture;
            list += std::string("\n    ") + label + ": " + sz::core::HotkeyText(combo);
        }
        char body[1024];
        std::snprintf(body, sizeof(body), sz::strings::kStartupHotkeysTaken, list.c_str());
        MessageBoxA(nullptr, body, "Spickzettel", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }

    trayController.Start();
    return host->RunEventLoop();
}
