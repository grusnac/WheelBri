#pragma once

#include <memory>
#include <string>

namespace wheelemu::service {

// Hides the real wheel from games while the service presents the virtual G920 (--virtual-g920), through
// HidHide's command-line tool (https://github.com/nefarius/HidHide), and puts everything back when it stops: the
// steps tools/vbridge did (HidHide.cs). The service itself is put on HidHide's application list, so it keeps
// reading the wheel. Only the wheel's game-controller interfaces are hidden; Moza Pit House keeps working.
// Output parsing: core/HidHideCli.hpp. Windows only; elsewhere find() returns null.
class HidHide {
public:
    // Null when HidHideCLI.exe is not installed.
    [[nodiscard]] static std::unique_ptr<HidHide> find();
    ~HidHide();

    HidHide(const HidHide&) = delete;
    HidHide& operator=(const HidHide&) = delete;

    // Allows this program, hides every game-controller interface of `vendorId` ("346E"), turns hiding on.
    // False (and logged) if nothing could be hidden.
    bool hideWheel(const std::string& vendorId);

    // Undoes hideWheel: what we hid becomes visible again; hiding goes back off if it was off before.
    void restore();

    // After a crash: everything of `vendorId` HidHide hides becomes visible again.
    void unhideAll(const std::string& vendorId);

    // Cloak state and the hidden list, for the log.
    void logState();

private:
    struct Impl;
    explicit HidHide(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace wheelemu::service
