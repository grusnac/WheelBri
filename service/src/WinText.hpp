// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Windows only: text between WheelBri's UTF-8 strings and the system's UTF-16, and the line for a failed call. One
// copy for the service's Windows files (HideFilter, PlatformWin32, UiWindowWin32, VirtualWheelDevice).

#include <windows.h>

#include <string>
#include <string_view>

namespace wheelbri::service::wintext {

// UTF-8 to UTF-16. Text that is not UTF-8 is taken byte for byte (as the ANSI code page's first 256 would be read).
[[nodiscard]] inline std::wstring widen(const std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        std::wstring out(text.size(), L'?');
        for (std::size_t i = 0; i < text.size(); ++i) out[i] = static_cast<unsigned char>(text[i]);
        return out;
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// UTF-16 to UTF-8.
[[nodiscard]] inline std::string narrow(const std::wstring_view wide) {
    if (wide.empty()) return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), size, nullptr, nullptr);
    return text;
}

// "<what> failed (error <GetLastError()>)".
[[nodiscard]] inline std::string lastError(const char* const what) {
    return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

}  // namespace wheelbri::service::wintext
