#pragma once

#include "Privileged.hpp"

#include "wheelbri/LogLevel.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wheelbri::service {

// The steps of a session that need Administrator, as the session sees them: done directly when this process is
// elevated (as WheelBri always did; development builds), or asked of the WheelBri Helper (service/helper) when it
// runs as a standard user. One object per session; what it set up (the virtual G920, the rig hidden) is undone when
// it goes, and, through the helper, also when this process dies (the helper sees the connection close).
class PrivilegedSteps {
public:
    using Cancelled = std::function<bool()>;

    enum class Problem {
        None,
        NoHelper,       // not elevated and no helper running (not installed, or its service stopped)
        HelperFailed,   // a helper is there but would not take this process (the message says why)
    };

    // Direct when elevated, else through the helper. Null with `problem` and `message` when neither is possible.
    [[nodiscard]] static std::unique_ptr<PrivilegedSteps> open(Problem& problem, std::string& message);

    // Whether a session could get its privileged steps now: elevated, or a WheelBri Helper is listening (checked
    // without connecting to it; cheap enough for once a second). For the app's status next to Start.
    [[nodiscard]] static bool available();

    virtual ~PrivilegedSteps() = default;

    // "as Administrator" or "through the WheelBri Helper" (the log).
    [[nodiscard]] virtual const char* how() const = 0;

    virtual bool installG920Driver(const Cancelled& cancelled, std::string& message) = 0;
    [[nodiscard]] virtual privileged::HideSetup putHideFilter(const std::vector<std::string>& install,
                                                              const std::vector<std::string>& interfaces,
                                                              const Cancelled& cancelled, std::string& message) = 0;
    // The virtual G920, there until removeVirtualG920 or the end of this object. `instanceId`: Windows' id for it.
    virtual bool createVirtualG920(const Cancelled& cancelled, std::string& instanceId, std::string& error) = 0;
    virtual void removeVirtualG920() = 0;
    // Only this process may open the rig's filtered devices, until showRig or the end of this object.
    virtual bool hideRig(std::string& message) = 0;
    virtual void showRig() = 0;
    // The drivers may write into this user's log folder (nothing when that is not possible; the log says why).
    virtual void shareLogFolder() = 0;
    // The drivers log at `level` (the app's Level button): HKLM\SOFTWARE\wheelbri LogLevel. A failure is only logged.
    virtual void setDriverLogLevel(LogLevel level) = 0;
};

}  // namespace wheelbri::service
