// wheelbri-logi.dll: a logging stand-in for Logitech's Steering Wheel SDK helper DLL. A game built with the SDK (NFS
// Payback) finds the helper through the registry (ServerBinary of CLSID {63BD165D-...}) and looks up the 48 Logi*
// functions in it by name. Pointed at this DLL instead, the game calls these exports; each passes the call on to
// Logitech's real DLL (RealLibrary.hpp) unchanged and logs what the game asked and got: the wheel's state the game
// reads each frame, the steering curve it asks for, the range, every force.
//
// Every argument travels in a 64-bit register (x64: RCX, RDX, R8, R9; none of the 48 has more than four), so one raw
// signature forwards any of them exactly: the real function reads what it declared from the same registers, and a
// bool or int result comes back in RAX as it is. Only floats travel elsewhere (XMM registers): the two LED functions
// are typed. The SDK's documented signatures (LogiSdk.hpp) are used only to print the values.
#include "LogiLogging.hpp"
#include "RealLibrary.hpp"

#include "wheelbri/LogiSdk.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string>

namespace {

using namespace wheelbri;
using namespace wheelbri::logi;

using Raw = std::int64_t;
using RawFn = Raw (*)(Raw, Raw, Raw, Raw);
using LedsFn = Raw (*)(Raw, float, float, float);

constexpr std::size_t kCount = 48;

std::array<std::atomic<std::uint64_t>, kCount> gCalls{};
ChangeLog gChanges;

// Steering the game read since the last summary.
std::atomic<std::int32_t> gMinX{std::numeric_limits<std::int32_t>::max()};
std::atomic<std::int32_t> gMaxX{std::numeric_limits<std::int32_t>::min()};

const logging::Logger& loggerFor(const Kind kind) {
    static const logging::Logger lifecycle = logger("logi");
    static const logging::Logger query = logger("logi.query");
    static const logging::Logger input = logger("logi.input");
    static const logging::Logger force = logger("logi.ffb");
    static const logging::Logger lights = logger("logi.leds");
    switch (kind) {
        case Kind::Query: return query;
        case Kind::Input: return input;
        case Kind::Force: return force;
        case Kind::Lights: return lights;
        case Kind::Lifecycle:
        case Kind::Setup: break;
    }
    return lifecycle;
}

std::int32_t low32(const Raw raw) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(raw)); }

// A key argument as the SDK sees it: handles and pointers whole, numbers as their 32 bits.
Raw keyOf(const Function& f, const std::size_t i, const std::array<Raw, 4>& args) {
    if (i >= f.keyArgs) return 0;
    return f.kinds[i] == Arg::Handle || f.kinds[i] == Arg::Pointer ? args[i] : low32(args[i]);
}

std::optional<ControllerProperties> readProperties(const Raw address) {
    if (address == 0) return std::nullopt;
    std::array<std::uint8_t, kPropertiesSize> bytes{};
    std::memcpy(bytes.data(), reinterpret_cast<const void*>(address), bytes.size());
    return decodeProperties(bytes);
}

std::optional<StateSummary> readState(const Raw address) {
    if (address == 0) return std::nullopt;
    std::array<std::uint8_t, kStatePrefixSize> bytes{};
    std::memcpy(bytes.data(), reinterpret_cast<const void*>(address), bytes.size());
    return decodeState(bytes);
}

// What a call returned besides its result: the text, range, properties or state behind its pointers.
std::string extraOf(const Function& f, const std::array<Raw, 4>& args, const Raw result,
                    const std::optional<ControllerProperties>& propertiesIn) {
    const bool ok = (result & 0xFF) != 0;
    switch (f.extra) {
        case Extra::None: return {};
        case Extra::TextBuffer: {
            const auto* buffer = reinterpret_cast<const wchar_t*>(args[1]);
            const std::int32_t size = low32(args[2]);
            if (!ok || buffer == nullptr || size <= 0) return {};
            std::string text;
            for (std::int32_t i = 0; i < size && buffer[i] != L'\0'; ++i) {
                text += buffer[i] < 0x80 ? static_cast<char>(buffer[i]) : '?';
            }
            return "'" + text + "'";
        }
        case Extra::RangeOut: {
            const auto* range = reinterpret_cast<const std::int32_t*>(args[1]);
            return ok && range != nullptr ? "range " + std::to_string(*range) + " deg" : std::string();
        }
        case Extra::PropertiesIn: return propertiesIn ? describe(*propertiesIn) : std::string("no properties");
        case Extra::PropertiesOut: {
            const auto properties = ok ? readProperties(args[1]) : std::nullopt;
            return properties ? describe(*properties) : std::string();
        }
        case Extra::State: {
            const auto state = readState(result);
            if (!state) return "no state";
            std::int32_t seen = gMinX.load();
            while (state->x < seen && !gMinX.compare_exchange_weak(seen, state->x)) {
            }
            seen = gMaxX.load();
            while (state->x > seen && !gMaxX.compare_exchange_weak(seen, state->x)) {
            }
            return describe(*state);
        }
    }
    return {};
}

// Every 60 s: how often the game called each function, and the steering it read.
void maybeSummarise() {
    using Clock = std::chrono::steady_clock;
    static std::atomic<Clock::rep> next{0};
    static std::mutex mutex;
    static std::array<std::uint64_t, kCount> last{};
    const Clock::rep now = Clock::now().time_since_epoch().count();
    Clock::rep due = next.load();
    if (due == 0) {
        next.compare_exchange_strong(due, now + std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(60))
                                                   .count());
        return;
    }
    if (now < due || !next.compare_exchange_strong(due, now + std::chrono::duration_cast<Clock::duration>(
                                                                    std::chrono::seconds(60))
                                                                    .count())) {
        return;
    }
    const std::lock_guard lock(mutex);
    std::string calls;
    for (std::size_t i = 0; i < kCount; ++i) {
        const std::uint64_t total = gCalls[i].load();
        if (total != last[i]) {
            calls += (calls.empty() ? "" : ", ") + std::string(functions()[i].name) + " " +
                     std::to_string(total - last[i]);
        }
        last[i] = total;
    }
    const std::int32_t minX = gMinX.exchange(std::numeric_limits<std::int32_t>::max());
    const std::int32_t maxX = gMaxX.exchange(std::numeric_limits<std::int32_t>::min());
    const std::string steering =
        minX <= maxX ? "; steering the game read: x " + std::to_string(minX) + " .. " + std::to_string(maxX) : "";
    WE_LOG_INFO(loggerFor(Kind::Lifecycle), "last 60 s: {}{}", calls.empty() ? "no calls" : calls, steering);
}

void note(const std::size_t id, const std::array<Raw, 4>& args, const Raw result,
          const std::optional<ControllerProperties>& propertiesIn, const bool realMissing) {
    const Function& f = functions()[id];
    const std::uint64_t call = gCalls[id].fetch_add(1) + 1;
    const std::string extra = extraOf(f, args, result, propertiesIn);

    // What tells this call from the last one with the same key: the state, or the other arguments and the result.
    std::string value;
    if (f.extra == Extra::State) {
        value = extra;
    } else {
        for (std::size_t i = f.keyArgs; i < f.args; ++i) value += std::to_string(args[i]) + ",";
        value += std::to_string(result & (f.ret == Ret::Bool ? 0xFF : -1)) + ":" + extra;
    }
    const ChangeLog::Seen seen = gChanges.observe(id, keyOf(f, 0, args), keyOf(f, 1, args), value);

    const logging::Logger& log = loggerFor(f.kind);
    const auto line = [&] {
        std::string text = formatCall(f, args) + " -> " + formatResult(f, result);
        if (!extra.empty()) text += ": " + extra;
        if (realMissing) text += " (not in the real library)";
        return text;
    };
    const bool sampled = call <= 3 || call % 1000 == 0;
    const bool first = seen == ChangeLog::Seen::First;
    const bool changed = seen == ChangeLog::Seen::Changed;
    switch (f.kind) {
        case Kind::Lifecycle:
        case Kind::Setup: WE_LOG_INFO(log, "#{} {}", call, line()); break;
        case Kind::Query:
            if (first || changed) {
                WE_LOG_INFO(log, "#{} {}", call, line());
            } else if (sampled) {
                WE_LOG_TRACE(log, "#{} {}", call, line());
            }
            break;
        case Kind::Input:
        case Kind::Force:
            if (first) {
                WE_LOG_INFO(log, "#{} first {}", call, line());
            } else if (changed) {
                WE_LOG_DEBUG(log, "#{} {}", call, line());
            } else if (sampled) {
                WE_LOG_TRACE(log, "#{} {}", call, line());
            }
            break;
        case Kind::Lights:
            if (first) {
                WE_LOG_INFO(log, "#{} first {}", call, line());
            } else if (sampled) {
                WE_LOG_TRACE(log, "#{} {}", call, line());
            }
            break;
    }
    maybeSummarise();
}

void noteGuarded(const std::size_t id, const std::array<Raw, 4>& args, const Raw result,
                 const std::optional<ControllerProperties>& propertiesIn, const bool realMissing) noexcept {
    try {
        note(id, args, result, propertiesIn, realMissing);
    } catch (const std::exception& e) {
        noteEscapedException(functions()[id].name.data(), e.what());
    } catch (...) {
        noteEscapedException(functions()[id].name.data(), "unknown exception");
    }
}

void* realGuarded(const std::size_t id) noexcept {
    try {
        return realFunction(id);
    } catch (...) {
        return nullptr;   // finding the real library failed in its logging: answered as if it were missing
    }
}

Raw forward(const std::size_t id, const Raw a, const Raw b, const Raw c, const Raw d) noexcept {
    if (id >= kCount) return 0;
    const std::array<Raw, 4> args{a, b, c, d};
    std::optional<ControllerProperties> propertiesIn;
    if (functions()[id].extra == Extra::PropertiesIn) {
        try {
            propertiesIn = readProperties(a);   // before the call: the real function may change its copy
        } catch (...) {
        }
    }
    const auto real = reinterpret_cast<RawFn>(realGuarded(id));
    const Raw result = real != nullptr ? real(a, b, c, d) : 0;
    noteGuarded(id, args, result, propertiesIn, real == nullptr);
    return result;
}

Raw forwardLeds(const std::size_t id, const Raw first, const float rpm, const float firstLed,
                const float redline) noexcept {
    if (id >= kCount) return 0;
    const auto real = reinterpret_cast<LedsFn>(realGuarded(id));
    const Raw result = real != nullptr ? real(first, rpm, firstLed, redline) : 0;
    const std::array<Raw, 4> args{first, std::bit_cast<std::uint32_t>(rpm), std::bit_cast<std::uint32_t>(firstLed),
                                  std::bit_cast<std::uint32_t>(redline)};
    noteGuarded(id, args, result, std::nullopt, real == nullptr);
    return result;
}

std::size_t idOf(const char* name) noexcept { return functionIndex(name); }

}  // namespace

// The exports (names fixed by wheelbri-logi.def). Each looks up its index once.
#define WB_LOGI_EXPORT(Name)                                                    \
    extern "C" Raw Name(const Raw a, const Raw b, const Raw c, const Raw d) {   \
        static const std::size_t id = idOf(#Name);                              \
        return forward(id, a, b, c, d);                                         \
    }

extern "C" {
WB_LOGI_EXPORT(LogiSteeringInitializeWithWindow)
WB_LOGI_EXPORT(LogiSteeringInitialize)
WB_LOGI_EXPORT(LogiSteeringShutdown)
WB_LOGI_EXPORT(LogiUpdate)
WB_LOGI_EXPORT(LogiGetStateENGINES)
WB_LOGI_EXPORT(LogiGetState)
WB_LOGI_EXPORT(LogiGetDevicePath)
WB_LOGI_EXPORT(LogiGetFriendlyProductName)
WB_LOGI_EXPORT(LogiIsConnected)
WB_LOGI_EXPORT(LogiIsDeviceConnected)
WB_LOGI_EXPORT(LogiIsManufacturerConnected)
WB_LOGI_EXPORT(LogiIsModelConnected)
WB_LOGI_EXPORT(LogiButtonTriggered)
WB_LOGI_EXPORT(LogiButtonReleased)
WB_LOGI_EXPORT(LogiButtonIsPressed)
WB_LOGI_EXPORT(LogiGenerateNonLinearValues)
WB_LOGI_EXPORT(LogiGetNonLinearValue)
WB_LOGI_EXPORT(LogiHasForceFeedback)
WB_LOGI_EXPORT(LogiIsPlaying)
WB_LOGI_EXPORT(LogiPlaySpringForce)
WB_LOGI_EXPORT(LogiStopSpringForce)
WB_LOGI_EXPORT(LogiPlayConstantForce)
WB_LOGI_EXPORT(LogiStopConstantForce)
WB_LOGI_EXPORT(LogiPlayDamperForce)
WB_LOGI_EXPORT(LogiStopDamperForce)
WB_LOGI_EXPORT(LogiPlaySideCollisionForce)
WB_LOGI_EXPORT(LogiPlayFrontalCollisionForce)
WB_LOGI_EXPORT(LogiPlayDirtRoadEffect)
WB_LOGI_EXPORT(LogiStopDirtRoadEffect)
WB_LOGI_EXPORT(LogiPlayBumpyRoadEffect)
WB_LOGI_EXPORT(LogiStopBumpyRoadEffect)
WB_LOGI_EXPORT(LogiPlaySlipperyRoadEffect)
WB_LOGI_EXPORT(LogiStopSlipperyRoadEffect)
WB_LOGI_EXPORT(LogiPlaySurfaceEffect)
WB_LOGI_EXPORT(LogiStopSurfaceEffect)
WB_LOGI_EXPORT(LogiPlayCarAirborne)
WB_LOGI_EXPORT(LogiStopCarAirborne)
WB_LOGI_EXPORT(LogiPlaySoftstopForce)
WB_LOGI_EXPORT(LogiStopSoftstopForce)
WB_LOGI_EXPORT(LogiSetPreferredControllerProperties)
WB_LOGI_EXPORT(LogiGetCurrentControllerProperties)
WB_LOGI_EXPORT(LogiGetShifterMode)
WB_LOGI_EXPORT(LogiSetOperatingRange)
WB_LOGI_EXPORT(LogiGetOperatingRange)
WB_LOGI_EXPORT(LogiSetOperatingRangeDInput)
WB_LOGI_EXPORT(LogiGetOperatingRangeDInput)

// The rev lights: (index or device handle, rpm, rpm the first light comes on, redline), the floats in XMM1..XMM3.
Raw LogiPlayLeds(const Raw index, const float rpm, const float firstLed, const float redline) {
    static const std::size_t id = idOf("LogiPlayLeds");
    return forwardLeds(id, index, rpm, firstLed, redline);
}

Raw LogiPlayLedsDInput(const Raw device, const float rpm, const float firstLed, const float redline) {
    static const std::size_t id = idOf("LogiPlayLedsDInput");
    return forwardLeds(id, device, rpm, firstLed, redline);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    // Keep DllMain trivial: no logging, no LoadLibrary, no threads (loader lock). Work starts on the first export call.
    switch (reason) {
        case DLL_PROCESS_ATTACH: DisableThreadLibraryCalls(instance); break;
        case DLL_PROCESS_DETACH: wheelbri::logi::onDllDetach(reserved != nullptr); break;
        default: break;
    }
    return TRUE;
}
}  // extern "C"

static_assert(sizeof(Raw) == sizeof(void*), "wheelbri-logi.dll forwards register-sized values: x64 only");
