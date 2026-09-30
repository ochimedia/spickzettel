#include "core/persistence/library_store.h"

// Every library a test opens is shared with other connections, rather than
// held for its store alone as the app's is: tests read a library back, and
// make its writes fail (see FailingWrites, HeldLibrary), through a
// connection of their own while a store has it open. The tests of the hold
// itself turn it back on (see ExclusiveLocking in library_store_test.cpp).
// Before main, so before the first store is made.
namespace {
[[maybe_unused]] const bool kShared = [] {
    sz::core::persistence::LibraryStore::LockSharedForTesting(true);
    return true;
}();
}  // namespace
