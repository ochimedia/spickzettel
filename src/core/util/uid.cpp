#include "core/util/uid.h"

#include <chrono>
#include <random>

namespace sz::core {

namespace {
std::mt19937_64& Generator() {
    // Seeded once per thread. random_device is used only for the seed: on
    // some standard libraries it is deterministic, which would be a
    // catastrophe for a per-draw source and is merely uninteresting for a
    // seed that also mixes in the clock.
    static thread_local std::mt19937_64 rng([] {
        std::random_device device;
        const uint64_t entropy = (static_cast<uint64_t>(device()) << 32) ^ device();
        const auto now = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return entropy ^ now;
    }());
    return rng;
}

uint64_t DefaultRandomBits() {
    std::uniform_int_distribution<uint64_t> spread(1, kUidSpace - 1);
    return spread(Generator());
}
}  // namespace

uint64_t MakeUid(const std::function<bool(uint64_t)>& isTaken,
                  const std::function<uint64_t()>& randomBits) {
    const auto draw = randomBits ? randomBits : std::function<uint64_t()>(&DefaultRandomBits);
    // Bounded rather than a bare `while (true)`: a caller whose isTaken
    // says yes to everything (a bug, or a library that has somehow used up
    // the space) should get a bad id it can notice, not a hang inside a
    // save. Two thousand draws against a space this size means the answer
    // is not "unlucky".
    constexpr int kMaxAttempts = 2000;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const uint64_t candidate = draw() % kUidSpace;
        if (candidate == 0) {
            continue;  // 0 means "no id" everywhere else
        }
        if (!isTaken(candidate)) {
            return candidate;
        }
    }
    return 0;
}

}  // namespace sz::core
