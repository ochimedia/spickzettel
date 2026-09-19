#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace sz::platform::win32 {

// UTF-16 to UTF-8. What Windows hands over wide - window titles, executable
// names, monitor names - ends up in a UTF-8 JSON file (a profile's match
// rules, a remembered display), so the conversion has to be lossless rather
// than the code page's best effort.
inline std::string Narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr,
                                            0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed, nullptr,
                         nullptr);
    return out;
}

}  // namespace sz::platform::win32
