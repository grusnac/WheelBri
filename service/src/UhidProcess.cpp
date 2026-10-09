// UhidProcess.hpp on Linux.

#include "UhidProcess.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/Logging.hpp"

#include <unistd.h>

#include <fstream>
#include <optional>
#include <vector>

namespace wheelbri::service {

namespace {

const logging::Logger& log() {
    static const logging::Logger logger = logging::get("service.uhid");
    return logger;
}

std::filesystem::path findUhid() {
    const std::filesystem::path folder = platform::executablePath().parent_path();
    if (folder.empty()) return {};
    for (const std::filesystem::path& candidate :
         {folder / "wheelbri-uhid", folder.parent_path() / "tools" / "uhid" / "wheelbri-uhid"}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate.lexically_normal();
    }
    return {};
}

}  // namespace

bool uhidRunning() {
    bool found = false;
    forEachEntry("/proc", [&found](const std::filesystem::directory_entry& entry) {
        if (found) return;
        std::ifstream comm(entry.path() / "comm");
        std::string name;
        found = comm && std::getline(comm, name) && name == "wheelbri-uhid";
    });
    return found;
}

std::unique_ptr<UhidProcess> UhidProcess::start(const std::filesystem::path& stopFolder, std::string& error) {
    if (uhidRunning()) {
        WE_LOG_INFO(log(), "wheelbri-uhid is already running (wheelbri.sh?): using its virtual G920");
        return std::unique_ptr<UhidProcess>(new UhidProcess(nullptr, {}, "wheelbri-uhid (already running)"));
    }
    const std::filesystem::path program = findUhid();
    if (program.empty()) {
        error = "wheelbri-uhid not found next to " + pathToUtf8(platform::executablePath()) +
                " (build it, or install WheelBri again)";
        return nullptr;
    }
    const std::filesystem::path stopFile = stopFolder / "uhid-app.stop";
    std::error_code ec;
    std::filesystem::remove(stopFile, ec);
    auto child = platform::ChildProcess::start(
        {pathToUtf8(program), "--stop-file", pathToUtf8(stopFile), "--parent", std::to_string(::getpid())}, error);
    if (child == nullptr) return nullptr;
    // A missing /dev/uhid permission or a taken port ends it at once: wait that long before calling it started.
    for (int i = 0; i < 10; ++i) {
        if (const std::optional<int> code = child->poll()) {
            error = "wheelbri-uhid stopped at once (exit " + std::to_string(*code) +
                    "): its log says why (scripts/linux/install-udev-rules.sh run once?)";
            return nullptr;
        }
        platform::sleepMs(50);
    }
    WE_LOG_INFO(log(), "virtual G920 created: {} (pid of this program {})", pathToUtf8(program), ::getpid());
    return std::unique_ptr<UhidProcess>(new UhidProcess(std::move(child), stopFile, "wheelbri-uhid"));
}

UhidProcess::UhidProcess(std::unique_ptr<platform::ChildProcess> child, std::filesystem::path stopFile,
                         std::string describe)
    : child_(std::move(child)), stopFile_(std::move(stopFile)), describe_(std::move(describe)) {}

UhidProcess::~UhidProcess() {
    if (child_ == nullptr || child_->poll()) return;
    { std::ofstream(stopFile_).put('\n'); }   // the same clean stop as wheelbri.sh: forces off, device removed
    for (int i = 0; i < 60 && !child_->poll(); ++i) {
        platform::sleepMs(50);
    }
    if (!child_->poll()) {
        WE_LOG_WARN(log(), "wheelbri-uhid did not stop within 3 s: killing it");
        child_->kill();
    } else {
        WE_LOG_INFO(log(), "virtual G920 removed (wheelbri-uhid stopped)");
    }
    std::error_code ec;
    std::filesystem::remove(stopFile_, ec);
}

bool UhidProcess::running() { return child_ == nullptr || !child_->poll(); }

}  // namespace wheelbri::service
