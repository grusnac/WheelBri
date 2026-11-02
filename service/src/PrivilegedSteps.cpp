#include "PrivilegedSteps.hpp"

#include "AppLogLevel.hpp"
#include "HideFilter.hpp"
#include "Platform.hpp"
#include "VirtualWheelDevice.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/Logging.hpp"

#include <algorithm>

#ifdef _WIN32
#include "HelperClient.hpp"

#include <windows.h>
#endif

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service");
    return logger;
}

// Driver scripts next to the program and up to this many folders above (build/service/ in the source tree).
constexpr int kDriverScriptLevels = 3;

// `list` with each of `ids` once.
void remember(std::vector<std::string>& list, const std::vector<std::string>& ids) {
    for (const std::string& id : ids) {
        if (std::find(list.begin(), list.end(), id) == list.end()) list.push_back(id);
    }
}

// `list` without `ids`.
void forget(std::vector<std::string>& list, const std::vector<std::string>& ids) {
    std::erase_if(list, [&ids](const std::string& id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); });
}

// As Administrator: the steps themselves, in this process (as WheelBri always did).
class DirectSteps final : public PrivilegedSteps {
public:
    ~DirectSteps() override {
        showRig();
        removeVirtualWheel();
        if (!filterOn_.empty()) {
            std::string message;
            (void)takeHideFilterOff(filterOn_, {}, message);
        }
    }

    [[nodiscard]] const char* how() const override { return "as Administrator"; }

    bool installWheelDriver(const Cancelled& cancelled, std::string& message) override {
        return privileged::installWheelDriver(kDriverScriptLevels, {}, cancelled, message);   // the user's own logs
    }

    privileged::HideSetup putHideFilter(const std::vector<std::string>& install,
                                        const std::vector<std::string>& interfaces, const Cancelled& cancelled,
                                        std::string& message) override {
        const privileged::HideSetup result =
            privileged::putHideFilter(install, interfaces, kDriverScriptLevels, {}, cancelled, message);
        if (result == privileged::HideSetup::Running || result == privileged::HideSetup::Waiting) {
            remember(filterOn_, interfaces);
        }
        return result;
    }

    privileged::HideSetup takeHideFilterOff(const std::vector<std::string>& interfaces, const Cancelled& cancelled,
                                            std::string& message) override {
        const privileged::HideSetup result = privileged::takeHideFilterOff(interfaces, cancelled, message);
        if (result != privileged::HideSetup::Failed) forget(filterOn_, interfaces);
        return result;
    }

    bool createVirtualWheel(const VirtualWheel& wheel, const bool trueForceProbe, const Cancelled& cancelled,
                            std::string& instanceId, std::string& error) override {
        if (device_ && deviceWheel_ != &wheel) removeVirtualWheel();   // another wheel than the one there: it goes
        if (!device_) device_ = VirtualWheelDevice::create(wheel, error, cancelled, true, trueForceProbe);
        if (!device_) return false;
        deviceWheel_ = &wheel;
        instanceId = device_->instanceId();
        return true;
    }

    void removeVirtualWheel() override {
        if (!device_) return;
        WE_LOG_INFO(log(), "removing the virtual wheel {}", device_->instanceId());
        device_.reset();
    }

    bool hideRig(std::string& message) override {
        if (hider_) return true;   // hidden already: a new HideFilter would show the rig for a moment first
        hider_ = std::make_unique<HideFilter>();
        if (!hider_->hide()) {
            hider_.reset();
            message = "cannot write HKLM\\SOFTWARE\\wheelbri\\hide";
            return false;
        }
        return true;
    }

    void showRig() override {
        if (hider_) hider_->restore();
        hider_.reset();
    }

    void shareLogFolder() override {}   // done at start (service::shareLogFolder), as Administrator

    void setDriverLogLevel(const LogLevel level) override {
        std::string message;
        if (!privileged::writeLogLevel(level, message)) {
            WE_LOG_WARN(log(), "logs: the drivers keep their log level: {}", message);
        }
    }

private:
    std::unique_ptr<VirtualWheelDevice> device_;
    const VirtualWheel* deviceWheel_ = nullptr;   // which wheel device_ is
    std::unique_ptr<HideFilter> hider_;
    std::vector<std::string> filterOn_;   // interfaces putHideFilter put the filter in, until takeHideFilterOff
};

#ifdef _WIN32

// As a standard user: everything asked of the WheelBri Helper over one connection, which stays open for the session
// (closing it, or this process dying, makes the helper undo what it did). The user's own DirectInput entries (HKCU)
// are written here, where HKCU is this user's.
class HelperSteps final : public PrivilegedSteps {
public:
    explicit HelperSteps(std::unique_ptr<helper::HelperClient> client) : client_(std::move(client)) {}

    ~HelperSteps() override {
        showRig();
        removeVirtualWheel();
        client_->close();
    }

    [[nodiscard]] const char* how() const override { return "through the WheelBri Helper"; }

    bool installWheelDriver(const Cancelled& cancelled, std::string& message) override {
        return ask(helper::RequestType::InstallWheelDriver, {}, kLongMs, cancelled, message) == helper::Status::Ok;
    }

    privileged::HideSetup putHideFilter(const std::vector<std::string>& install,
                                        const std::vector<std::string>& interfaces, const Cancelled& cancelled,
                                        std::string& message) override {
        if (interfaces.empty()) return privileged::HideSetup::NothingToDo;
        // The helper puts it on those without it (the script skips the others) and waits for all of them.
        (void)install;
        WE_LOG_INFO(log(), "hide: asking the WheelBri Helper to hide the rig's devices {}", joined(interfaces));
        switch (ask(helper::RequestType::PutHideFilter, interfaces, kLongMs, cancelled, message)) {
            case helper::Status::Ok:
                WE_LOG_INFO(log(), "hide: wheelbri-hide runs on every connected device of the rig");
                return privileged::HideSetup::Running;
            case helper::Status::Pending:
                WE_LOG_ERROR(log(), "hide: {}", message);
                return privileged::HideSetup::Waiting;
            default:
                WE_LOG_ERROR(log(), "hide: the WheelBri Helper could not hide the rig: {}", message);
                return privileged::HideSetup::Failed;
        }
    }

    privileged::HideSetup takeHideFilterOff(const std::vector<std::string>& interfaces, const Cancelled& cancelled,
                                            std::string& message) override {
        if (interfaces.empty()) return privileged::HideSetup::NothingToDo;
        switch (ask(helper::RequestType::TakeHideFilterOff, interfaces, kCreateMs, cancelled, message)) {
            case helper::Status::Ok: return privileged::HideSetup::Running;
            case helper::Status::Pending:
                WE_LOG_WARN(log(), "hide: {}", message);
                return privileged::HideSetup::Waiting;
            case helper::Status::Unsupported:
                message = "this WheelBri Helper is older and leaves the hide filter on (it costs CPU while the wheel "
                          "is plugged in): install WheelBri again to update it";
                WE_LOG_WARN(log(), "hide: {}", message);
                return privileged::HideSetup::Failed;
            default:
                WE_LOG_WARN(log(), "hide: the WheelBri Helper could not take the hide filter off: {}", message);
                return privileged::HideSetup::Failed;
        }
    }

    bool createVirtualWheel(const VirtualWheel& wheel, const bool trueForceProbe, const Cancelled& cancelled,
                            std::string& instanceId, std::string& error) override {
        // The G920 as an older helper knows it (no wheel named); the G25 by name (an older helper refuses that).
        std::vector<std::string> named;
        if (wheel.profile != "g920") named.emplace_back(wheel.profile);
        if (trueForceProbe && wheel.trueForce) named.emplace_back(helper::kTrueForceProbe);
        if (ask(helper::RequestType::CreateVirtualWheel, named, kCreateMs, cancelled, error) != helper::Status::Ok) {
            return false;
        }
        instanceId = error;   // the reply's text is its instance id
        error.clear();
        device_ = instanceId;
        userEntries_ = std::make_unique<DirectInputUserEntries>(wheel, trueForceProbe && wheel.trueForce);
        WE_LOG_INFO(log(), "virtual {} {} ready (created by the WheelBri Helper)", wheel.instancePrefix, instanceId);
        return true;
    }

    void removeVirtualWheel() override {
        if (device_.empty()) return;
        WE_LOG_INFO(log(), "removing the virtual wheel {}", device_);
        std::string message;
        if (connected() && ask(helper::RequestType::RemoveVirtualWheel, {}, kShortMs, {}, message) != helper::Status::Ok) {
            WE_LOG_WARN(log(), "the WheelBri Helper did not remove the virtual wheel: {} (it does when this app ends)",
                        message);
        }
        userEntries_.reset();
        device_.clear();
    }

    bool hideRig(std::string& message) override {
        if (hidden_) return true;
        if (ask(helper::RequestType::HideRig, {}, kShortMs, {}, message) != helper::Status::Ok) return false;
        hidden_ = true;
        WE_LOG_INFO(log(), "hide: the rig is hidden from games (only this process, {}, may open its devices)",
                    GetCurrentProcessId());
        return true;
    }

    void showRig() override {
        if (!hidden_) return;
        std::string message;
        if (connected() && ask(helper::RequestType::ShowRig, {}, kShortMs, {}, message) == helper::Status::Ok) {
            WE_LOG_INFO(log(), "hide: the rig is visible to games again");
        } else {
            WE_LOG_WARN(log(), "hide: the WheelBri Helper did not show the rig again: {} (it does when this app ends)",
                        message);
        }
        hidden_ = false;
    }

    void shareLogFolder() override {
        std::string message;
        if (ask(helper::RequestType::ShareLogFolder, {}, kShortMs, {}, message) == helper::Status::Ok) {
            WE_LOG_INFO(log(), "logs: every log goes to {} (the drivers too)", message);
        } else {
            WE_LOG_WARN(log(), "logs: the drivers keep writing to C:\\ProgramData\\wheelbri\\logs: {}", message);
        }
    }

    void setDriverLogLevel(const LogLevel level) override {
        std::string message;
        switch (ask(helper::RequestType::SetLogLevel, {logLevelName(level)}, kShortMs, {}, message)) {
            case helper::Status::Ok: break;
            case helper::Status::Unsupported:
                WE_LOG_DEBUG(log(), "logs: this WheelBri Helper is older: the drivers keep their log level");
                break;
            default: WE_LOG_WARN(log(), "logs: the drivers keep their log level: {}", message); break;
        }
    }

private:
    static constexpr int kShortMs = 10'000;
    static constexpr int kCreateMs = 60'000;
    static constexpr int kLongMs = 180'000;   // a driver installer (2 min at most) and the wait for the device

    static std::string joined(const std::vector<std::string>& items) {
        std::string text;
        for (const std::string& item : items) text += (text.empty() ? "" : ",") + item;
        return text;
    }

    [[nodiscard]] bool connected() const { return connected_; }

    // One request; `text` gets the reply's text, or why there was none.
    helper::Status ask(const helper::RequestType type, const std::vector<std::string>& devices, const int timeoutMs,
                       const Cancelled& cancelled, std::string& text) {
        if (!connected_) {
            text = "the connection to the WheelBri Helper was lost earlier";
            return helper::Status::Failed;
        }
        std::string error;
        const std::optional<helper::Reply> reply = client_->call(type, devices, timeoutMs, error, cancelled);
        if (!reply) {
            connected_ = false;   // the helper undoes everything of this connection now
            device_.clear();
            userEntries_.reset();
            hidden_ = false;
            text = error;
            return helper::Status::Failed;
        }
        text = reply->text;
        return reply->status;
    }

    std::unique_ptr<helper::HelperClient> client_;
    bool connected_ = true;
    std::string device_;   // its instance id while there
    std::unique_ptr<DirectInputUserEntries> userEntries_;
    bool hidden_ = false;
};

#endif

}  // namespace

bool PrivilegedSteps::available() {
#ifdef _WIN32
    static const bool elevated = platform::isElevated();
    if (elevated) return true;
    // Waiting 1 ms for a free instance: there is one, or all are busy (ERROR_SEM_TIMEOUT); no pipe at all is
    // ERROR_FILE_NOT_FOUND. Nothing connects.
    const std::wstring name(helper::kPipeName.begin(), helper::kPipeName.end());
    return WaitNamedPipeW(name.c_str(), 1) || GetLastError() != ERROR_FILE_NOT_FOUND;
#else
    return true;   // away from Windows the process always counts as elevated (platform::isElevated)
#endif
}

std::unique_ptr<PrivilegedSteps> PrivilegedSteps::open(Problem& problem, std::string& message) {
    problem = Problem::None;
#ifdef _WIN32
    if (platform::isElevated()) return std::make_unique<DirectSteps>();
    auto client = std::make_unique<helper::HelperClient>();
    std::string error;
    const std::string folder = pathToUtf8(platform::executablePath().parent_path());
    switch (client->connect(error, folder)) {
        case helper::HelperClient::Connect::Ok: break;
        case helper::HelperClient::Connect::NoHelper:
            problem = Problem::NoHelper;
            message = "the WheelBri Helper is not running (install WheelBri again, or start the WheelBriHelper service)";
            return nullptr;
        case helper::HelperClient::Connect::Failed:
            problem = Problem::HelperFailed;
            message = error;
            return nullptr;
    }
    const auto hello =
        client->call(helper::RequestType::Hello, {logLevelName(AppLogLevel::shared().current())}, 5000, error);
    if (!hello || hello->status != helper::Status::Ok) {
        problem = Problem::HelperFailed;
        message = hello ? hello->text : error;
        return nullptr;
    }
    WE_LOG_INFO(log(), "privileged steps through {} (pid {}, {})", hello->text, client->serverPid(),
                client->serverImage());
    return std::make_unique<HelperSteps>(std::move(client));
#else
    static_cast<void>(message);
    return std::make_unique<DirectSteps>();   // away from Windows the process always counts as elevated
#endif
}

}  // namespace wheelbri::service
