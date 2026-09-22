#pragma once

#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace sz::platform::win32 {

// A minidump of the process when it crashes, for a crash on someone else's
// machine to be something more than "it just disappeared". Read with the
// PDB of the very build that crashed - see docs/ARCHITECTURE.md.

// How many dumps are kept: each start drops all but the newest of these,
// so a build that crashes every time it starts cannot fill the disk.
inline constexpr size_t kCrashDumpsKept = 10;

// From here on, a crash writes a dump into `directory` - created then, not
// now, so that a machine that never crashes never has the folder - named
// after `versionLine` and the local time, and the process ends without
// Windows' own error dialog. Covers what the process does not survive: an
// unhandled SEH exception (an access violation, a stack overflow, an
// uncaught C++ exception), abort(), which std::terminate ends in, and the
// CRT's invalid-parameter and pure-call handlers. Once, early in WinMain:
// the name is prepared here, so that the crash itself allocates nothing.
void InstallCrashDumpWriter(const std::filesystem::path& directory, std::string_view versionLine);

// Writes a minidump of this process to `file`, from a thread of its own -
// a thread cannot reliably walk its own stack for the dump, and a crashed
// one may have little stack left. `exception` names the crash, or is null
// for a dump of the process as it stands. True if the file was written.
bool WriteMiniDump(const std::wstring& file, EXCEPTION_POINTERS* exception);

// "Spickzettel-<version line>-": the start of every dump's file name, with
// anything but letters, digits and .-() in the version line made '_' so
// that it is always a file name.
std::wstring CrashDumpPrefix(std::string_view versionLine);

// Deletes all but the `keep` newest .dmp files in `directory`.
void PruneCrashDumps(const std::filesystem::path& directory, size_t keep);

}  // namespace sz::platform::win32
