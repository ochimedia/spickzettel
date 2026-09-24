#include "core/util/uid.h"

#include <chrono>
#include <random>

namespace sz::core {

namespace {
constexpr char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
constexpr uint64_t kBase = 36;

// Lowercase only, and that is load-bearing rather than a style choice:
// these render into directory names, and Windows and macOS filesystems are
// case-insensitive, so "a7K2q9" and "a7k2q9" would be the same directory
// while being different ids in memory.
int ValueOfDigit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 10;
    }
    return -1;
}

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

void SeedUidsForTesting(uint64_t seed) { Generator().seed(seed); }

std::string FormatUid(uint64_t id) {
    std::string out(kUidLength, '0');
    uint64_t value = id % kUidSpace;
    for (size_t i = kUidLength; i-- > 0;) {
        out[i] = kAlphabet[value % kBase];
        value /= kBase;
    }
    return out;
}

std::optional<uint64_t> ParseUid(std::string_view text) {
    if (text.size() != kUidLength) {
        return std::nullopt;
    }
    uint64_t value = 0;
    for (const char ch : text) {
        const int digit = ValueOfDigit(ch);
        if (digit < 0) {
            return std::nullopt;
        }
        value = value * kBase + static_cast<uint64_t>(digit);
    }
    return value;
}

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
