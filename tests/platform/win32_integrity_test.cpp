// Where another process sits relative to us on Windows' mandatory
// integrity scale. The overlay asks this about whatever is in front of it,
// because Windows cuts a lower-integrity process out of a higher one's
// input entirely - see ForegroundIntegrity.
#include "platform/win32/win32_integrity.h"

#include <gtest/gtest.h>

#include <windows.h>
#include <tlhelp32.h>

namespace sz::platform::win32 {
namespace {

TEST(Win32IntegrityTest, OurOwnProcessIsNotAboveUs) {
    EXPECT_EQ(IntegrityComparedToOurs(GetCurrentProcess()), ForegroundIntegrity::NotAbove);
    EXPECT_GT(IntegrityRidOf(GetCurrentProcess()), 0u) << "our own level is always readable";
}

// The answer for a process we cannot ask about is Unknown, and Unknown is
// a third answer rather than a quiet "no": acting on it as though it meant
// NotAbove would leave the elevated case broken, and acting on it as
// though it meant Above would take focus from the anti-cheat-shielded
// game that refuses the same query - the one thing that must never
// happen. Only Above is ever acted on; see TrayController.
TEST(Win32IntegrityTest, AProcessWeCannotOpenIsUnknownRatherThanEitherAnswer) {
    EXPECT_EQ(IntegrityComparedToOurs(nullptr), ForegroundIntegrity::Unknown);
    EXPECT_EQ(IntegrityRidOf(nullptr), 0u);
}

// The level of anything that does open is a real one, and the comparison
// agrees with the raw levels it is derived from. Run over whatever the
// machine happens to have: on a medium-integrity test run most of the
// system's own processes refuse the handle, which is exactly the case
// above and is skipped here.
TEST(Win32IntegrityTest, EveryProcessThatOpensReportsALevelConsistentWithOurs) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    ASSERT_NE(snapshot, INVALID_HANDLE_VALUE);
    const DWORD ours = IntegrityRidOf(GetCurrentProcess());
    ASSERT_GT(ours, 0u);

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    int compared = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) {
                continue;
            }
            const DWORD theirs = IntegrityRidOf(process);
            const ForegroundIntegrity verdict = IntegrityComparedToOurs(process);
            if (theirs == 0) {
                EXPECT_EQ(verdict, ForegroundIntegrity::Unknown) << "no level read, so no comparison";
            } else {
                ++compared;
                EXPECT_EQ(verdict, theirs > ours ? ForegroundIntegrity::Above : ForegroundIntegrity::NotAbove);
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    EXPECT_GT(compared, 0) << "not even this process was compared";
}

}  // namespace
}  // namespace sz::platform::win32
