#pragma once

#include <cstddef>
#include <functional>

#include "support/faulty_file_system.h"
#include "support/memory_file_system.h"

namespace sz::core::fakes {

// Runs `scenario` against a crash at every point it could have one.
//
// First once through, to count the changes it makes to the disk (N). Then
// N + 1 more times, each on a fresh disk that `setUp` has prepared, with
// the disk stopping after 0, 1, ... N of the scenario's changes - the last
// run being the one no crash interrupts. `check` gets each disk as the
// crash left it, which point it was and of how many, to restart from and
// judge.
//
// A scenario has to make the same changes in the same order every time for
// the points to line up, which a MemoryFileSystem (listings in name order)
// and a store that iterates ordered containers give it. Holds and failures
// that `setUp` arranges stay in force through the scenario; `check` gets
// the disk underneath, with none of them, as a restart after the other
// program has let go would.
//
// `setUp` gets the disk underneath too, to look at without counting.
//
// Returns N, so that a test can make sure its scenario did something.
inline size_t ForEachCrashPoint(const std::function<void(FaultyFileSystem&, MemoryFileSystem&)>& setUp,
                                const std::function<void(FaultyFileSystem&)>& scenario,
                                const std::function<void(MemoryFileSystem&, size_t crashedAfter, size_t changes)>& check) {
    size_t changes = 0;
    {
        MemoryFileSystem disk;
        FaultyFileSystem faulty(disk);
        setUp(faulty, disk);
        const size_t before = faulty.ChangesAttempted();
        scenario(faulty);
        changes = faulty.ChangesAttempted() - before;
    }
    for (size_t crashAfter = 0; crashAfter <= changes; ++crashAfter) {
        MemoryFileSystem disk;
        FaultyFileSystem faulty(disk);
        setUp(faulty, disk);
        faulty.CrashAfter(crashAfter);
        scenario(faulty);
        check(disk, crashAfter, changes);
    }
    return changes;
}

}  // namespace sz::core::fakes
