#include "core/util/timestamp_name.h"

#include <ctime>

namespace sz::core {

std::string TimestampName() {
    const std::time_t now = std::time(nullptr);
    // std::localtime rather than a platform _s/_r variant: it is the one
    // spelling MSVC and GCC both have, and its shared buffer is read on the
    // next line, on the one thread that calls this. MSVC deprecates it for
    // the shared-state reason that does not apply here.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const std::tm* local = std::localtime(&now);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    char buf[32];
    if (local == nullptr || std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", local) == 0) {
        // A clock this broken is not worth a special case beyond a name
        // that still says what the thing is.
        return "Untitled";
    }
    return std::string(buf);
}

}  // namespace sz::core
