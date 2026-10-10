#pragma once

#include <string>
#include <vector>

namespace wheelbri::logi {

// wheelbri-logi.ini next to the DLL, optional, read once (no logging here: it decides the log level):
//   [logi]
//   log_level = debug         ; the stand-in's own level (trace | debug | info | warn | ...), whatever the app's is
//   real_library = <path>     ; Logitech's helper DLL to pass calls on to (RealLibrary.hpp)
struct Settings {
    std::string logLevel;                // empty: the app's level
    std::wstring realLibrary;            // empty: search for it
    std::vector<std::string> problems;   // logged once logging is up
};

[[nodiscard]] const Settings& settings();

}  // namespace wheelbri::logi
