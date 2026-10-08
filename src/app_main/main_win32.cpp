#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <backends/imgui_impl_win32.h>

#include "app/command_line.h"
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

// The arguments after the program's name, as UTF-8 - see
// sz::app::ParseCommandLine.
std::vector<std::string> CommandLineArguments() {
    std::vector<std::string> args;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (argv == nullptr) {
        return args;
    }
    for (int i = 1; i < count; ++i) {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(bytes > 0 ? static_cast<size_t>(bytes - 1) : 0, '\0');
        if (bytes > 1) {
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg.data(), bytes, nullptr, nullptr);
        }
        args.push_back(std::move(arg));
    }
    LocalFree(argv);
    return args;
}

}  // namespace

int WINAPI WinMain(HINSTANCE /*instance*/, HINSTANCE /*prevInstance*/, LPSTR /*cmdLine*/, int /*showCmd*/) {
    // Must happen before any window is created: without this, a fullscreen
    // window's size/coordinates get DPI-virtualized by Windows on any scaled
    // display (125%/150% is common), which can misalign the overlay against
    // real screen/mouse coordinates.
    ImGui_ImplWin32_EnableDpiAwareness();

    // Arguments nobody understood are said, and nothing starts: a start that
    // quietly ignored --data-dir misspelled would be a start on the user's
    // own library.
    const sz::app::ParsedCommandLine commandLine = sz::app::ParseCommandLine(CommandLineArguments());
    if (!commandLine.error.empty()) {
        std::string body(2048, '\0');
        const int length = std::snprintf(body.data(), body.size(), sz::strings::kStartupCommandLine,
                                         commandLine.error.c_str(), sz::app::CommandLineUsage().c_str());
        body.resize(length > 0 ? std::min(static_cast<size_t>(length), body.size() - 1) : 0);
        MessageBoxA(nullptr, body.c_str(), "Spickzettel", MB_OK | MB_ICONWARNING);
        return 1;
    }

    auto host = sz::platform::CreatePlatformHost(commandLine.options.dataDir.value_or(std::filesystem::path{}));
    // First, so that a crash anywhere after it - starting up included -
    // leaves a dump to be sent in. Beside the library, in
    // %LOCALAPPDATA%\Spickzettel\crashes (or the data folder).
    sz::platform::win32::InstallCrashDumpWriter(host->GetLibraryPath().parent_path() / "crashes",
                                                sz::core::build::VersionLine());
    if (!host->Initialize("Spickzettel")) {
        return 1;
    }
    // Before the config is read: a second copy that could not read it
    // would set it aside and write defaults in its place - under the copy
    // already running, which then saves over both - and show the set-aside
    // message before giving up. TrayController
    // asks again, and is answered from the mutex already held.
    // Started again while a copy runs, what is wanted is that copy: it is
    // asked to come up, and this one goes. The message only when no copy
    // answers - one running as another account, say.
    //
    // Not with a data folder of its own: the copy running is the user's,
    // on the user's library, and a script that meant its own would go on
    // to drive that one. The single-instance mutex is per user, not per
    // folder - the hotkeys are per user too.
    if (!host->AcquireSingleInstance()) {
        if (commandLine.options.dataDir.has_value()) {
            MessageBoxA(nullptr, sz::strings::kStartupRunningWithDataDir, "Spickzettel", MB_OK | MB_ICONWARNING);
            return 1;
        }
        if (host->PassOpeningToRunningCopy()) {
            return 0;
        }
        MessageBoxA(nullptr, sz::strings::kStartupFailed, "Spickzettel", MB_OK | MB_ICONWARNING);
        return 1;
    }

    const sz::core::LoadedConfig config = LoadConfig(host->GetConfigFilePath());

    sz::app::TrayController trayController(*host, config.config);
    // Kept, and the retention period skipped, for a file that could not be
    // read and for one a newer build wrote: whether retention is on, and
    // for how long, is what this build cannot be sure it read.
    if (config.source == sz::core::ConfigSource::SetAside || config.source == sz::core::ConfigSource::Unreadable ||
        config.source == sz::core::ConfigSource::Newer) {
        trayController.StartOnStandInSettings(config.source, /*keepFile=*/config.setAsideAs.empty());
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
    // TrayController::Start - and nothing brings it up while a box says so.
    // Native boxes rather than drawn by the overlay: on a first run it
    // comes up fullscreen, topmost and in edit mode, and would cover them.
    trayController.HoldUntilStart();
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
            const char* label = slot == sz::core::HotkeySlot::EditMode        ? sz::strings::kHotkeysEditMode
                                : slot == sz::core::HotkeySlot::ViewMode      ? sz::strings::kHotkeysViewMode
                                : slot == sz::core::HotkeySlot::QuickCapture  ? sz::strings::kHotkeysQuickCapture
                                : slot == sz::core::HotkeySlot::SilentCapture ? sz::strings::kHotkeysSilentCapture
                                                                              : sz::strings::kHotkeysBehaviorPanel;
            list += std::string("\n    ") + label + ": " + sz::core::HotkeyText(combo);
        }
        char body[1024];
        std::snprintf(body, sizeof(body), sz::strings::kStartupHotkeysTaken, list.c_str());
        MessageBoxA(nullptr, body, "Spickzettel", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    }

    trayController.Start();
    // Asked for on the command line: up in edit mode, as a second start
    // brings it - for the scripts that measure the app, which would
    // otherwise press the edit hotkey, a global one that reaches whichever
    // copy holds it.
    if (commandLine.options.editMode) {
        trayController.OnOpenedAgain();
    }
    return host->RunEventLoop();
}
