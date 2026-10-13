#pragma once

// Linux: the virtual G920 for a session of the app, wheelbri-uhid (tools/uhid), a program of its own because it is
// the same one scripts/linux/wheelbri.sh starts. The app starts it with Start (target "G920 virtual device") and
// stops it with Stop; one already running (wheelbri.sh's) is used and left alone.

#include "Platform.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace wheelbri::service {

class UhidProcess {
public:
    // Starts wheelbri-uhid (next to this program, or build/tools/uhid/ in the source tree) with a stop file in
    // `stopFolder`, or finds one already running. Nullptr and `error` when it cannot be found or stopped at once.
    // `target`: which wheel it presents, the config's target_profile ("g920", "g25").
    [[nodiscard]] static std::unique_ptr<UhidProcess> start(const std::filesystem::path& stopFolder,
                                                            const std::string& target, std::string& error);
    ~UhidProcess();   // stops it (stop file, then SIGKILL after 3 s) unless it was not ours

    UhidProcess(const UhidProcess&) = delete;
    UhidProcess& operator=(const UhidProcess&) = delete;

    // For the window and the log: "wheelbri-uhid" or "wheelbri-uhid (already running)".
    [[nodiscard]] const std::string& describe() const { return describe_; }
    // False once our wheelbri-uhid ended by itself (its log says why); always true for one that was not ours.
    [[nodiscard]] bool running();

private:
    UhidProcess(std::unique_ptr<platform::ChildProcess> child, std::filesystem::path stopFile, std::string describe);

    std::unique_ptr<platform::ChildProcess> child_;   // nullptr: one that was already running
    std::filesystem::path stopFile_;
    std::string describe_;
};

// Whether a wheelbri-uhid runs now (another one would find the ports taken).
[[nodiscard]] bool uhidRunning();

}  // namespace wheelbri::service
