#include "UiFakes.hpp"

#include "DeviceWatch.hpp"
#include "Platform.hpp"

#include "wheelbri/LogConfig.hpp"
#include "wheelbri/Logging.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <thread>
#include <chrono>
#include <sstream>

namespace wheelbri::service {

namespace uitest {

// --- ScriptedWindow -----------------------------------------------------------------------------------------------

ui::WindowInput ScriptedWindow::pump() {
    ++pumps;
    while (!script.empty()) {
        Step& step = script.front();
        if (step.until && step.until(*this)) {
            script.pop_front();   // reached: the next step answers this pump
            continue;
        }
        ui::WindowInput input = step.input ? step.input(*this) : ui::WindowInput{};
        if (!step.until) {
            script.pop_front();   // a one-pump step
        } else if (--step.maxPumps <= 0) {
            timedOut = true;
            timedOutAt = step.what;
            script.clear();
        }
        return input;
    }
    ui::WindowInput input;
    input.close = closeAtEnd;
    return input;
}

const ScriptedWindow::Text* ScriptedWindow::find(const std::string& value, const bool part) const {
    for (const Text& text : last.texts) {
        if (part ? text.value.find(value) != std::string::npos : text.value == value) return &text;
    }
    return nullptr;
}

std::string ScriptedWindow::dump() const {
    std::ostringstream out;
    for (const Text& text : last.texts) out << text.x << "," << text.y << " " << text.value << "\n";
    return out.str();
}

std::optional<ui::Click> ScriptedWindow::clickOn(const std::string& value, const bool part) const {
    const Text* const text = find(value, part);
    if (text == nullptr) return std::nullopt;
    return ui::Click{text->x + 4, text->y + 4};
}

ui::WindowInput click(const float x, const float y) {
    ui::WindowInput input;
    input.clicks.push_back({x, y});
    input.pointer = {x, y};
    input.pointerInside = true;
    return input;
}

ui::WindowInput keys(std::string keys) {
    ui::WindowInput input;
    input.keys = std::move(keys);
    return input;
}

ui::WindowInput typed(std::string text) {
    ui::WindowInput input;
    input.text = std::move(text);
    return input;
}

ui::WindowInput nothing() { return {}; }

// --- devices ---------------------------------------------------------------------------------------------------------

FakeDevices& FakeDevices::shared() {
    static FakeDevices devices;
    return devices;
}

std::shared_ptr<FakeDevice> FakeDevices::add(const std::string& name, const std::uint16_t vendorId,
                                             const std::uint16_t productId, const int axes, const int buttons) {
    auto device = std::make_shared<FakeDevice>();
    device->info.name = name;
    device->info.vendorId = vendorId;
    device->info.productId = productId;
    device->axes = axes;
    device->buttons = buttons;
    const std::lock_guard lock(mutex_);
    device->info.path = "/dev/input/fake" + std::to_string(next_++);
    devices_.push_back(device);
    announce();
    return device;
}

// As the system does when a device comes or goes (CM_Register_Notification, inotify on /dev/input): where those
// notifications work, the app lists devices again only when told (or on its 30 s safety net).
void FakeDevices::announce() { platform::DeviceChanges::shared().counter().announce(); }

void FakeDevices::clear() {
    {
        const std::lock_guard lock(mutex_);
        devices_.clear();
        backendFails = false;
        listerUnavailable = false;
        listerHolds = false;
    }
    announce();   // every device unplugged at once
}

void FakeDevices::change(const std::shared_ptr<FakeDevice>& device, const std::function<void(FakeDevice&)>& edit) {
    bool plugged = false;
    {
        const std::lock_guard lock(mutex_);
        const bool before = device->plugged;
        edit(*device);
        plugged = device->plugged != before;
    }
    if (plugged) announce();   // plugged in or out: the system tells
}

std::vector<WheelInfo> FakeDevices::list() const {
    const std::lock_guard lock(mutex_);
    std::vector<WheelInfo> found;
    for (const auto& device : devices_) {
        if (device->plugged) found.push_back(device->info);
    }
    return found;
}

std::shared_ptr<FakeDevice> FakeDevices::find(const std::string& path) const {
    const std::lock_guard lock(mutex_);
    for (const auto& device : devices_) {
        if (device->info.path == path) return device;
    }
    return nullptr;
}

bool FakeDevices::poll(const std::shared_ptr<FakeDevice>& device, RawInputState& raw) const {
    const std::lock_guard lock(mutex_);
    if (!device->plugged || device->pollFails) return false;
    raw = device->raw;
    return true;
}

namespace {

class FakeWheel final : public Wheel {
public:
    explicit FakeWheel(std::shared_ptr<FakeDevice> device) : device_(std::move(device)), info_(device_->info) {}
    [[nodiscard]] const WheelInfo& info() const override { return info_; }
    [[nodiscard]] int axes() const override { return device_->axes; }
    [[nodiscard]] int buttons() const override { return device_->buttons; }
    [[nodiscard]] int hats() const override { return 1; }
    bool poll(RawInputState& state) override { return FakeDevices::shared().poll(device_, state); }
    [[nodiscard]] std::unique_ptr<IFfbSink> openForceFeedback(bool) override { return nullptr; }

private:
    std::shared_ptr<FakeDevice> device_;
    WheelInfo info_;
};

class FakeBackend final : public WheelBackend {
public:
    [[nodiscard]] std::vector<WheelInfo> list() override { return FakeDevices::shared().list(); }
    [[nodiscard]] std::unique_ptr<Wheel> open(const WheelInfo& wheel) override {
        const auto device = FakeDevices::shared().find(wheel.path);
        if (device == nullptr || device->openFails || !device->plugged) return nullptr;
        return std::make_unique<FakeWheel>(device);
    }
    [[nodiscard]] std::string describe() const override { return "fake"; }
};

ProxyWindow* gLastProxy = nullptr;

}  // namespace

// --- windows -------------------------------------------------------------------------------------------------------

WindowFactory& WindowFactory::shared() {
    static WindowFactory factory;
    return factory;
}

ui::WindowInput ProxyWindow::pump() {
    sync();
    ui::WindowInput input = target_.pump();
    input.close = input.close || closed_;
    return input;
}

ProxyWindow* lastProxy() { return gLastProxy; }

std::function<std::unique_ptr<ui::UiWindow>()> proxyOf(ScriptedWindow& target) {
    return [&target]() -> std::unique_ptr<ui::UiWindow> {
        auto proxy = std::make_unique<ProxyWindow>(target);
        gLastProxy = proxy.get();
        return proxy;
    };
}

// --- user folders -------------------------------------------------------------------------------------------------

namespace {

std::optional<std::string> getEnv(const std::string& name) {
    const char* const value = std::getenv(name.c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>(value);
}

void setEnv(const std::string& name, const std::optional<std::string>& value) {
#if defined(_WIN32)
    _putenv_s(name.c_str(), value ? value->c_str() : "");
#else
    if (value) {
        setenv(name.c_str(), value->c_str(), 1);
    } else {
        unsetenv(name.c_str());
    }
#endif
}

}  // namespace

UserFolders::UserFolders(const std::string& name)
    : root_(std::filesystem::temp_directory_path() / ("wheelbri-ui-" + name)) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "config");
    std::filesystem::create_directories(root_ / "state");
    for (const auto& [variable, folder] :
         {std::pair<std::string, std::string>{"XDG_CONFIG_HOME", "config"}, {"XDG_STATE_HOME", "state"},
          {"LOCALAPPDATA", "local"}}) {
        saved_.emplace_back(variable, getEnv(variable));
        setEnv(variable, (root_ / folder).string());
    }
}

UserFolders::~UserFolders() {
    for (const auto& [variable, value] : saved_) setEnv(variable, value);
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
}

// The app's log, as runApp has it: a file (the Logs tab's first row reads it), level debug so every line is formatted.
class LogEnvironment final : public ::testing::Environment {
public:
    void SetUp() override {
        folder_ = std::filesystem::temp_directory_path() / ("wheelbri-ui-log-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(folder_);
        logging::LogConfig config;
        config.level = "debug";
        config.console = false;
        config.periodicFlush = false;
        logging::init(config, (folder_ / "wheelbri-service.log").string());
        // Windows: the components' loggers (function statics, made from here on) hold the file open until the
        // program ends, so TearDown cannot delete the folder. At exit, after they are gone (handlers registered
        // before them run after them), it can.
        folderAtExit() = folder_;
        std::atexit([] {
            std::error_code ec;
            std::filesystem::remove_all(folderAtExit(), ec);
        });
    }
    void TearDown() override {
        logging::shutdown();
        std::error_code ec;
        std::filesystem::remove_all(folder_, ec);
    }

private:
    static std::filesystem::path& folderAtExit() {
        static std::filesystem::path* path = new std::filesystem::path();   // never destroyed: used at exit
        return *path;
    }

    std::filesystem::path folder_;
};

[[maybe_unused]] ::testing::Environment* const gLogEnvironment =
    ::testing::AddGlobalTestEnvironment(new LogEnvironment);

}  // namespace uitest

// --- the stand-ins the app links against ---------------------------------------------------------------------------

namespace ui {

std::unique_ptr<UiWindow> openWindow(const std::string& title, int, int, int, int, std::string& error) {
    auto& factory = uitest::WindowFactory::shared();
    factory.titles.push_back(title);
    if (!factory.make) {
        error = factory.openError;
        return nullptr;
    }
    return factory.make();
}

}  // namespace ui

std::unique_ptr<WheelBackend> startWheelBackend() {
    auto& devices = uitest::FakeDevices::shared();
    ++devices.backendStarts;
    if (devices.backendFails) return nullptr;
    return std::make_unique<uitest::FakeBackend>();
}

DeviceWatch::Lister DeviceWatch::systemLister() {
    return []() -> std::optional<std::vector<WheelInfo>> {
        auto& devices = uitest::FakeDevices::shared();
        for (int i = 0; i < 500 && devices.listerHolds; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (devices.listerUnavailable) return std::nullopt;
        return devices.list();
    };
}

}  // namespace wheelbri::service
