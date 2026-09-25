#include "platform/win32/win32_crash_dump.h"

// dbghelp.h needs windows.h first, which the header above brings.
#include <dbghelp.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <system_error>
#include <utility>
#include <vector>

namespace sz::platform::win32 {

namespace {

// Everything the crash needs, made ready by InstallCrashDumpWriter so that
// nothing is allocated once the heap may be what broke: the directory, and
// the directory joined with the file name's prefix.
wchar_t g_directory[MAX_PATH];
wchar_t g_pathPrefix[MAX_PATH];
// Set by the first crash to reach a handler. A second one - another thread
// crashing meanwhile, or the dump writer itself - does not write over the
// first dump; see DumpAndEnd.
volatile LONG g_crashing = 0;
// The thread writing the dump, while there is one.
volatile DWORD g_writerThreadId = 0;

// A custom exception code, raised only to have a context to dump from a
// handler that is not given one (abort, the CRT's handlers). The top bits
// say "error, customer-defined"; the rest spells SZ.
constexpr DWORD kRaisedForDump = 0xE0535A01;

struct WriteRequest {
    const wchar_t* file;
    EXCEPTION_POINTERS* exception;
    DWORD threadId;
    bool written;
};

DWORD WINAPI WriteDumpThread(LPVOID param) {
    WriteRequest& request = *static_cast<WriteRequest*>(param);
    const HANDLE file = CreateFileW(request.file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = request.threadId;
    info.ExceptionPointers = request.exception;
    info.ClientPointers = FALSE;
    // Every thread's stack, the memory those stacks point into, and which
    // modules were loaded at which version: what reading a crash takes,
    // in a few hundred kilobytes rather than the whole process.
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                                 MiniDumpWithUnloadedModules);
    request.written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                                        request.exception != nullptr ? &info : nullptr, nullptr, nullptr) != FALSE;
    CloseHandle(file);
    if (!request.written) {
        DeleteFileW(request.file);
    }
    return 0;
}

bool WriteDump(const wchar_t* file, EXCEPTION_POINTERS* exception) {
    WriteRequest request{file, exception, GetCurrentThreadId(), false};
    DWORD writerId = 0;
    const HANDLE thread = CreateThread(nullptr, 0, &WriteDumpThread, &request, 0, &writerId);
    if (thread == nullptr) {
        return false;
    }
    g_writerThreadId = writerId;
    // Not for ever: see kDumpWriteTimeoutMs. A writer still going then is
    // ended with the process, and its half-written file is not a dump.
    const bool finished = WaitForSingleObject(thread, kDumpWriteTimeoutMs) == WAIT_OBJECT_0;
    g_writerThreadId = 0;
    CloseHandle(thread);
    return finished && request.written;
}

// The dump of a crash: into the folder, under the prepared prefix and the
// time. Then the process ends - there is nothing to go back to.
[[noreturn]] void DumpAndEnd(EXCEPTION_POINTERS* exception) {
    if (InterlockedExchange(&g_crashing, 1) != 0) {
        // Not the first. The writer itself crashing gives up its dump,
        // which the first crash is waiting on; any other thread waits for
        // the first to finish and end the process. Ending it here, as this
        // did, cut the first one's dump off halfway.
        if (GetCurrentThreadId() == g_writerThreadId) {
            ExitThread(1);
        }
        for (;;) {
            Sleep(INFINITE);
        }
    }
    if (g_pathPrefix[0] != L'\0') {
        CreateDirectoryChain(g_directory);
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t file[MAX_PATH];
        if (std::swprintf(file, MAX_PATH, L"%ls%04u%02u%02u-%02u%02u%02u.dmp", g_pathPrefix, now.wYear, now.wMonth,
                          now.wDay, now.wHour, now.wMinute, now.wSecond) > 0) {
            WriteDump(file, exception);
        }
    }
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {
        // TerminateProcess does not return for the current process.
    }
}

// Raises and catches an exception of our own, so that a handler without
// one still dumps with a context: the stack as it stands, here. No C++
// objects in this function - __try does not mix with their unwinding.
[[noreturn]] void DumpFromHere() {
    __try {
        RaiseException(kRaisedForDump, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    } __except (DumpAndEnd(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER) {
    }
    DumpAndEnd(nullptr);
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* exception) { DumpAndEnd(exception); }

void OnAbort(int) { DumpFromHere(); }

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) { DumpFromHere(); }

void OnPureCall() { DumpFromHere(); }

}  // namespace

std::wstring CrashDumpPrefix(std::string_view versionLine) {
    std::wstring prefix = L"Spickzettel-";
    for (const char c : versionLine) {
        const bool kept = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                          c == '-' || c == '(' || c == ')';
        prefix += kept ? static_cast<wchar_t>(c) : L'_';
    }
    prefix += L'-';
    return prefix;
}

void PruneCrashDumps(const std::filesystem::path& directory, size_t keep) {
    std::error_code ec;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> dumps;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".dmp") {
            dumps.emplace_back(it->last_write_time(ec), it->path());
        }
    }
    if (dumps.size() <= keep) {
        return;
    }
    std::sort(dumps.begin(), dumps.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = keep; i < dumps.size(); ++i) {
        std::filesystem::remove(dumps[i].second, ec);
    }
}

bool WriteMiniDump(const std::wstring& file, EXCEPTION_POINTERS* exception) { return WriteDump(file.c_str(), exception); }

bool CreateDirectoryChain(wchar_t* path) {
    // Each separator in turn made the end of the string for a moment, so
    // every folder on the way is created before the one inside it. The
    // first after a drive letter or a UNC server is skipped by
    // CreateDirectoryW failing harmlessly on what already exists.
    for (wchar_t* at = path; *at != L'\0'; ++at) {
        if ((*at == L'\\' || *at == L'/') && at != path && *(at - 1) != L':' && *(at - 1) != L'\\') {
            const wchar_t separator = *at;
            *at = L'\0';
            CreateDirectoryW(path, nullptr);
            *at = separator;
        }
    }
    CreateDirectoryW(path, nullptr);
    const DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void InstallCrashDumpWriter(const std::filesystem::path& directory, std::string_view versionLine) {
    const std::wstring dir = directory.wstring();
    const std::wstring prefix = (directory / CrashDumpPrefix(versionLine)).wstring();
    // Room for the time stamp and ".dmp" after the prefix; a profile path
    // too long for that leaves the writer uninstalled rather than cut short.
    if (dir.size() >= MAX_PATH || prefix.size() + 32 >= MAX_PATH) {
        return;
    }
    wcscpy_s(g_directory, dir.c_str());
    wcscpy_s(g_pathPrefix, prefix.c_str());
    PruneCrashDumps(directory, kCrashDumpsKept);

    // Stack for the handler after a stack overflow, on the thread most
    // likely to have one: the guard page is gone by then, and without this
    // what runs next is at the very end of the stack, and faults again
    // before a dump is begun.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(&OnUnhandledException);
    std::signal(SIGABRT, &OnAbort);
    _set_invalid_parameter_handler(&OnInvalidParameter);
    _set_purecall_handler(&OnPureCall);
    // abort() would otherwise report to Windows Error Reporting first, and
    // in a debug build ask about it in a message box.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

}  // namespace sz::platform::win32
