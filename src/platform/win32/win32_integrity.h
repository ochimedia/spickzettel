#pragma once

#include <windows.h>

#include <vector>

#include "platform/platform_types.h"

namespace sz::platform::win32 {

// Where another process sits relative to us on Windows' mandatory
// integrity scale, which is what decides whether we can see its input at
// all - see ForegroundIntegrity and docs/ARCHITECTURE.md.

// The mandatory label's RID: untrusted, low, medium, high and system, each
// a documented constant and ordered by value. Zero when the token cannot
// be opened or read, which is not a level of its own and is why the caller
// turns it into Unknown rather than into the lowest one.
//
// Measured across every process on a machine, from a medium-integrity
// process: of the 36 that could be opened at all, 35 answered this and one
// refused the token. The other 91 were owned by another account and
// refused the handle itself, before this is ever reached.
inline DWORD IntegrityRidOf(HANDLE process) {
    HANDLE token = nullptr;
    if (process == nullptr || !OpenProcessToken(process, TOKEN_QUERY, &token)) {
        return 0;
    }
    DWORD size = 0;
    // The documented two-call shape: the first asks how much room the
    // label needs and fails with ERROR_INSUFFICIENT_BUFFER by design.
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    if (size == 0) {
        CloseHandle(token);
        return 0;
    }
    std::vector<BYTE> buffer(size);
    auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
    DWORD rid = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, label, size, &size)) {
        // The level is the last sub-authority of the label's SID.
        const UCHAR* count = GetSidSubAuthorityCount(label->Label.Sid);
        if (count != nullptr && *count > 0) {
            rid = *GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(*count - 1));
        }
    }
    CloseHandle(token);
    return rid;
}

// Our own level is read on every call rather than remembered: it is one
// token query, and an overlay started elevated should not depend on when
// it first happened to ask. Either side being unreadable is Unknown, since
// a comparison needs both.
inline ForegroundIntegrity IntegrityComparedToOurs(HANDLE process) {
    const DWORD theirs = IntegrityRidOf(process);
    if (theirs == 0) {
        return ForegroundIntegrity::Unknown;
    }
    const DWORD ours = IntegrityRidOf(GetCurrentProcess());
    if (ours == 0) {
        return ForegroundIntegrity::Unknown;
    }
    return theirs > ours ? ForegroundIntegrity::Above : ForegroundIntegrity::NotAbove;
}

}  // namespace sz::platform::win32
