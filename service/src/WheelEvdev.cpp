// SPDX-License-Identifier: GPL-3.0-or-later
// The physical wheel on Linux, through evdev (/dev/input/event*): listing the game controllers, reading
// the wheel and its force feedback (the kernel's EV_FF, e.g. hid-universal-pidff for direct drive wheels).
//
// Numbering and values follow SDL3 (EvdevLayout.hpp), the force feedback makes the same calls SDL3 (3.4)
// made: gain 100 %, auto-centre off, one uploaded effect per slot on the steering axis (direction 0x4000,
// played until stopped), updated in place. Needs read access to the wheel's event node and write access for
// force feedback (scripts/linux/install-udev-rules.sh).

#include "Wheel.hpp"

#include "wheelbri/EvdevLayout.hpp"
#include "wheelbri/HidWheelDevice.hpp"
#include "wheelbri/Logging.hpp"
#include "wheelbri/WheelOutput.hpp"

#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

namespace wheelbri::service {

namespace {

static_assert(evdev::kFfConstant == FF_CONSTANT && evdev::kFfPeriodic == FF_PERIODIC &&
              evdev::kFfSpring == FF_SPRING && evdev::kFfDamper == FF_DAMPER && evdev::kFfFriction == FF_FRICTION &&
              evdev::kFfInertia == FF_INERTIA && evdev::kFfSine == FF_SINE && evdev::kFfSquare == FF_SQUARE &&
              evdev::kFfTriangle == FF_TRIANGLE && evdev::kFfSawUp == FF_SAW_UP && evdev::kFfSawDown == FF_SAW_DOWN &&
              evdev::kFfGain == FF_GAIN && evdev::kFfAutocenter == FF_AUTOCENTER);
static_assert(evdev::kKeyCount == KEY_CNT && evdev::kAbsCount == ABS_CNT && evdev::kBtnJoystick == BTN_JOYSTICK &&
              evdev::kAbsHat0X == ABS_HAT0X && evdev::kSynDropped == SYN_DROPPED);

const logging::Logger& inputLog() {
    static const logging::Logger logger = logging::get("service.input");
    return logger;
}

const logging::Logger& ffbLog() {
    static const logging::Logger logger = logging::get("service.ffb");
    return logger;
}

constexpr std::size_t kLongBits = sizeof(unsigned long) * 8;

// An EVIOCGBIT / EVIOCGKEY bit array as a bitset; `request` makes the ioctl request for a buffer length.
template <std::size_t N, typename Request>
std::bitset<N> readBits(const int fd, Request request) {
    std::array<unsigned long, (N + kLongBits - 1) / kLongBits> words{};
    std::bitset<N> bits;
    if (ioctl(fd, request(sizeof(words)), words.data()) < 0) {
        return bits;
    }
    for (std::size_t i = 0; i < N; ++i) {
        if ((words[i / kLongBits] >> (i % kLongBits)) & 1UL) {
            bits.set(i);
        }
    }
    return bits;
}

// Range and value of each axis in `axes`; an axis the kernel cannot describe is taken out of `axes` (SDL
// leaves it out too).
std::array<evdev::AbsInfo, evdev::kAbsCount> readAbs(const int fd, std::bitset<evdev::kAbsCount>& axes) {
    std::array<evdev::AbsInfo, evdev::kAbsCount> out{};
    for (std::size_t code = 0; code < axes.size(); ++code) {
        if (!axes.test(code)) continue;
        input_absinfo info{};
        if (ioctl(fd, EVIOCGABS(code), &info) == 0) {
            out[code] = {info.value, info.minimum, info.maximum, info.fuzz, info.flat, info.resolution};
        } else {
            axes.reset(code);
        }
    }
    return out;
}

evdev::Capabilities readCapabilities(const int fd) {
    evdev::Capabilities caps;
    const auto types = readBits<EV_CNT>(fd, [](std::size_t length) { return EVIOCGBIT(0, length); });
    if (types.test(EV_KEY)) caps.keys = readBits<KEY_CNT>(fd, [](std::size_t length) { return EVIOCGBIT(EV_KEY, length); });
    if (types.test(EV_ABS)) caps.axes = readBits<ABS_CNT>(fd, [](std::size_t length) { return EVIOCGBIT(EV_ABS, length); });
    caps.hasRelative = types.test(EV_REL);
    caps.absInfo = readAbs(fd, caps.axes);
    return caps;
}

// /dev/input/event0, event1, ... in number order.
std::vector<std::string> eventNodes() {
    std::vector<std::pair<int, std::string>> nodes;
    if (DIR* dir = opendir("/dev/input")) {
        while (const dirent* entry = readdir(dir)) {
            if (std::strncmp(entry->d_name, "event", 5) == 0) {
                nodes.emplace_back(std::atoi(entry->d_name + 5), std::string("/dev/input/") + entry->d_name);
            }
        }
        closedir(dir);
    }
    std::sort(nodes.begin(), nodes.end());
    std::vector<std::string> paths;
    for (auto& node : nodes) paths.push_back(std::move(node.second));
    return paths;
}

// --- force feedback -----------------------------------------------------------------------------------

class EvdevFfbSink final : public IFfbSink {
public:
    static std::unique_ptr<IFfbSink> open(const WheelInfo& wheel, const bool invert) {
        const int fd = ::open(wheel.path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            const int error = errno;
            WE_LOG_ERROR(ffbLog(), "cannot open force feedback on '{}': {}: {}{}", wheel.name, wheel.path,
                         std::strerror(error),
                         error == EACCES ? " (run scripts/linux/install-udev-rules.sh once, then log out and in)" : "");
            return nullptr;
        }
        const auto features = readBits<FF_CNT>(fd, [](std::size_t length) { return EVIOCGBIT(EV_FF, length); });
        if (features.none()) {
            WE_LOG_ERROR(ffbLog(), "'{}' has no force feedback (no EV_FF); on Linux 6.15+ many direct drive wheels need the "
                                   "hid-universal-pidff driver (modinfo hid-universal-pidff)",
                         wheel.name);
            ::close(fd);
            return nullptr;
        }
        return std::unique_ptr<IFfbSink>(new EvdevFfbSink(fd, features, wheel, invert));
    }

    ~EvdevFfbSink() override {
        stopAll();
        for (Slot& slot : slots_) {
            release(slot);
        }
        ::close(fd_);
    }

    EvdevFfbSink(const EvdevFfbSink&) = delete;
    EvdevFfbSink& operator=(const EvdevFfbSink&) = delete;

    bool play(const std::size_t slotIndex, const FfbEffect& effect) override {
        if (slotIndex >= slots_.size()) {
            return false;
        }
        static logging::RateLimiter unsupportedLimiter(std::chrono::seconds(30));
        static logging::RateLimiter failureLimiter(std::chrono::seconds(5));

        const evdev::Output out = evdev::toEvdev(toWheelEffect(effect, invert_));
        if (!features_.test(out.type) || (out.type == FF_PERIODIC && !features_.test(out.waveform))) {
            WE_LOG_WARN_LIMITED(ffbLog(), unsupportedLimiter, "slot {}: the wheel cannot play {} effects, dropped",
                                slotIndex, ffbTypeName(effect.type));
            return false;
        }

        Slot& slot = slots_[slotIndex];
        ff_effect data = toKernel(out);
        if (slot.id >= 0 && slot.type == effect.type) {
            data.id = static_cast<std::int16_t>(slot.id);   // update in place
            if (ioctl(fd_, EVIOCSFF, &data) < 0) {
                WE_LOG_ERROR_LIMITED(ffbLog(), failureLimiter, "slot {}: updating {} effect failed: {}", slotIndex,
                                     ffbTypeName(effect.type), std::strerror(errno));
                return false;
            }
        } else {
            release(slot);
            data.id = -1;   // the kernel gives it an id
            if (ioctl(fd_, EVIOCSFF, &data) < 0) {
                WE_LOG_ERROR_LIMITED(ffbLog(), failureLimiter, "slot {}: creating {} effect failed: {}", slotIndex,
                                     ffbTypeName(effect.type), std::strerror(errno));
                return false;
            }
            slot.id = data.id;
            slot.type = effect.type;
            WE_LOG_DEBUG(ffbLog(), "slot {}: created {} effect (id {})", slotIndex, ffbTypeName(effect.type), slot.id);
        }

        if (!slot.running) {
            if (!send(EV_FF, static_cast<std::uint16_t>(slot.id), 1)) {
                WE_LOG_ERROR_LIMITED(ffbLog(), failureLimiter, "slot {}: starting {} effect failed: {}", slotIndex,
                                     ffbTypeName(effect.type), std::strerror(errno));
                return false;
            }
            slot.running = true;
            WE_LOG_DEBUG(ffbLog(), "slot {}: {} effect started", slotIndex, ffbTypeName(effect.type));
        }
        return true;
    }

    void stop(const std::size_t slotIndex) override {
        if (slotIndex >= slots_.size()) {
            return;
        }
        Slot& slot = slots_[slotIndex];
        if (slot.id >= 0 && slot.running) {
            if (!send(EV_FF, static_cast<std::uint16_t>(slot.id), 0)) {
                WE_LOG_WARN(ffbLog(), "slot {}: stopping effect failed: {}", slotIndex, std::strerror(errno));
            }
            slot.running = false;
            WE_LOG_DEBUG(ffbLog(), "slot {}: stopped", slotIndex);
        }
    }

    void stopAll() override {
        for (Slot& slot : slots_) {   // evdev has no "stop all": every uploaded effect, as SDL does
            if (slot.id >= 0) {
                send(EV_FF, static_cast<std::uint16_t>(slot.id), 0);
            }
            slot.running = false;
        }
        WE_LOG_DEBUG(ffbLog(), "all effects stopped");
    }

private:
    struct Slot {
        int id = -1;
        FfbEffectType type = FfbEffectType::Constant;
        bool running = false;
    };

    EvdevFfbSink(const int fd, const std::bitset<FF_CNT>& features, const WheelInfo& wheel, const bool invert)
        : fd_(fd), features_(features), invert_(invert) {
        int capacity = 0;
        ioctl(fd_, EVIOCGEFFECTS, &capacity);
        std::string names;
        const std::pair<int, const char*> kNames[] = {
            {FF_CONSTANT, "constant"}, {FF_SINE, "sine"},     {FF_SQUARE, "square"},   {FF_TRIANGLE, "triangle"},
            {FF_SAW_UP, "saw up"},     {FF_SAW_DOWN, "saw down"}, {FF_SPRING, "spring"}, {FF_DAMPER, "damper"},
            {FF_INERTIA, "inertia"},   {FF_FRICTION, "friction"}, {FF_GAIN, "gain"},     {FF_AUTOCENTER, "autocenter"},
        };
        for (const auto& [bit, name] : kNames) {
            if (features_.test(static_cast<std::size_t>(bit))) {
                names += (names.empty() ? "" : ", ") + std::string(name);
            }
        }
        // The user's gain lives in the limiter; the game's spring does the centring.
        if (features_.test(FF_GAIN) && !send(EV_FF, FF_GAIN, 0xFFFF)) {
            WE_LOG_WARN(ffbLog(), "setting the gain failed: {}", std::strerror(errno));
        }
        if (features_.test(FF_AUTOCENTER) && !send(EV_FF, FF_AUTOCENTER, 0)) {
            WE_LOG_WARN(ffbLog(), "switching auto-centre off failed: {}", std::strerror(errno));
        }
        WE_LOG_INFO(ffbLog(), "force feedback on '{}' (evdev {}): {} (up to {} effects), invert={}", wheel.name,
                    wheel.path, names, capacity, invert_);
        if (!features_.test(FF_CONSTANT) && !features_.test(FF_SPRING) && !features_.test(FF_DAMPER)) {
            WE_LOG_WARN(ffbLog(), "device reports none of constant/spring/damper; racing-game force feedback will be "
                                  "missing");
        }
    }

    static ff_effect toKernel(const evdev::Output& out) {
        ff_effect data{};
        data.type = out.type;
        data.direction = out.direction;
        data.replay.length = 0;   // until stopped
        data.replay.delay = 0;
        data.trigger.button = 0;
        data.trigger.interval = 0;
        switch (out.type) {
            case FF_CONSTANT:
                data.u.constant.level = out.level;
                break;
            case FF_PERIODIC:
                data.u.periodic.waveform = out.waveform;
                data.u.periodic.period = out.periodMs;
                data.u.periodic.magnitude = out.magnitude;
                data.u.periodic.offset = out.offset;
                data.u.periodic.phase = out.phase;
                break;
            default:   // conditions: the first axis only, the second stays zero
                data.u.condition[0].right_saturation = out.rightSaturation;
                data.u.condition[0].left_saturation = out.leftSaturation;
                data.u.condition[0].right_coeff = out.rightCoefficient;
                data.u.condition[0].left_coeff = out.leftCoefficient;
                data.u.condition[0].deadband = out.deadband;
                data.u.condition[0].center = out.center;
                break;
        }
        return data;
    }

    bool send(const std::uint16_t type, const std::uint16_t code, const std::int32_t value) {
        input_event event{};
        event.type = type;
        event.code = code;
        event.value = value;
        return ::write(fd_, &event, sizeof(event)) == static_cast<ssize_t>(sizeof(event));
    }

    void release(Slot& slot) {
        if (slot.id >= 0) {
            ioctl(fd_, EVIOCRMFF, slot.id);
        }
        slot = Slot{};
    }

    int fd_;
    std::bitset<FF_CNT> features_;
    bool invert_;
    std::array<Slot, kWheelSlots> slots_{};   // the game's and the centring spring's
};

// --- the wheel ----------------------------------------------------------------------------------------

class EvdevWheel final : public Wheel {
public:
    EvdevWheel(WheelInfo info, const int fd, const evdev::Capabilities& caps)
        : info_(std::move(info)), fd_(fd), layout_(caps) {
        resync();
    }

    ~EvdevWheel() override { ::close(fd_); }

    EvdevWheel(const EvdevWheel&) = delete;
    EvdevWheel& operator=(const EvdevWheel&) = delete;

    [[nodiscard]] const WheelInfo& info() const override { return info_; }
    [[nodiscard]] int axes() const override { return layout_.axes(); }
    [[nodiscard]] int buttons() const override { return layout_.buttons(); }
    [[nodiscard]] int hats() const override { return layout_.hats(); }
    [[nodiscard]] std::intptr_t waitSource() const override { return fd_; }

    bool poll(RawInputState& state) override {
        std::array<input_event, 64> events{};
        bool dropped = false;
        while (true) {
            const ssize_t got = ::read(fd_, events.data(), sizeof(events));
            if (got < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    break;
                }
                WE_LOG_ERROR(inputLog(), "'{}' was disconnected: {}", info_.name, std::strerror(errno));
                return false;
            }
            if (got == 0) {
                break;
            }
            const auto count = static_cast<std::size_t>(got) / sizeof(input_event);
            for (std::size_t i = 0; i < count; ++i) {
                dropped = layout_.apply(events[i].type, events[i].code, events[i].value) || dropped;
            }
            if (count < events.size()) {
                break;
            }
        }
        if (dropped) {
            static logging::RateLimiter limiter(std::chrono::seconds(10));
            WE_LOG_WARN_LIMITED(inputLog(), limiter, "'{}': the kernel dropped input events; reading the state again",
                                info_.name);
            resync();
        }
        layout_.fill(state);
        return true;
    }

    std::unique_ptr<IFfbSink> openForceFeedback(const bool invert) override {
        return EvdevFfbSink::open(info_, invert);
    }

private:
    // The device's current values, after opening and after the kernel dropped events.
    void resync() {
        const auto keys = readBits<KEY_CNT>(fd_, [](std::size_t length) { return EVIOCGKEY(length); });
        auto axes = readBits<ABS_CNT>(fd_, [](std::size_t length) { return EVIOCGBIT(EV_ABS, length); });
        layout_.reset(readAbs(fd_, axes), keys);
    }

    WheelInfo info_;
    int fd_;
    evdev::Layout layout_;
};

class EvdevBackend final : public WheelBackend {
public:
    std::vector<WheelInfo> list() override {
        std::vector<WheelInfo> wheels;
        int unreadable = 0;
        int own = 0;
        int steamPads = 0;
        for (const std::string& path : eventNodes()) {
            const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                unreadable += errno == EACCES ? 1 : 0;
                continue;
            }
            if (isOwnVirtualDevice(fd)) {
                ++own;
            } else if (isSteamVirtualPad(fd)) {
                ++steamPads;
            } else if (evdev::isGameController(readCapabilities(fd))) {
                wheels.push_back(describe(fd, path));
            }
            ::close(fd);
        }
        if (own > 0) {
            WE_LOG_DEBUG(inputLog(), "{} WheelBri virtual wheel(s) left out (wheelbri-uhid's own)", own);
        }
        if (steamPads > 0) {
            WE_LOG_DEBUG(inputLog(), "{} Steam virtual pad(s) left out (28de:11ff: Steam Input's copy of another "
                                     "controller)",
                         steamPads);
        }
        if (unreadable > 0) {
            WE_LOG_DEBUG(inputLog(), "{} input device(s) not readable by this user (keyboards and mice usually are "
                                     "not; a wheel is after install-udev-rules.sh)",
                         unreadable);
        }
        WE_LOG_DEBUG(inputLog(), "{} game controller(s) connected", wheels.size());
        return wheels;
    }

    std::unique_ptr<Wheel> open(const WheelInfo& info) override {
        const int fd = ::open(info.path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            WE_LOG_ERROR(inputLog(), "cannot open '{}' ({}): {}", info.name, info.path, std::strerror(errno));
            return nullptr;
        }
        return std::make_unique<EvdevWheel>(info, fd, readCapabilities(fd));
    }

    [[nodiscard]] std::string describe() const override { return "evdev"; }

private:
    // wheelbri-uhid's virtual wheel (kVirtualWheelUniq): a rig built from it, or a session reading it as the wheel,
    // would feed the service its own output.
    static bool isOwnVirtualDevice(const int fd) {
        char uniq[64] = {};
        return ioctl(fd, EVIOCGUNIQ(sizeof(uniq) - 1), uniq) >= 0 && std::strcmp(uniq, kVirtualWheelUniq) == 0;
    }

    // Steam Input's virtual Xbox pad (28de:11ff, made through uinput): a copy of another controller (the Steam Deck's
    // own, or one Steam Input took over), never a rig device; every one has the same id, so none could be told apart.
    static bool isSteamVirtualPad(const int fd) {
        input_id id{};
        return ioctl(fd, EVIOCGID, &id) == 0 && id.vendor == 0x28DE && id.product == 0x11FF;
    }

    static WheelInfo describe(const int fd, const std::string& path) {
        WheelInfo info;
        info.path = path;
        char name[256] = {};
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && name[0] != '\0') {
            info.name = name;
        } else {
            info.name = "(unnamed)";
        }
        input_id id{};
        if (ioctl(fd, EVIOCGID, &id) == 0) {
            info.vendorId = id.vendor;
            info.productId = id.product;
        }
        return info;
    }
};

}  // namespace

std::unique_ptr<WheelBackend> startWheelBackend() { return std::make_unique<EvdevBackend>(); }

}  // namespace wheelbri::service
