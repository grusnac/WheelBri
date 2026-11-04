// The probe's Windows side: the G923's HID collections (SetupAPI + hid.dll, loaded at run time as the service does)
// and overlapped reads and writes on them.

#include "g923cap/Probe.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace wheelbri::g923cap {

#ifdef _WIN32
namespace {

struct HidAttributes {
    ULONG size;
    USHORT vendor;
    USHORT product;
    USHORT version;
};
struct HidCaps {
    USHORT usage;
    USHORT usagePage;
    USHORT inputLength;
    USHORT outputLength;
    USHORT featureLength;
    USHORT reserved[17];
    USHORT rest[10];
};
struct InterfaceData {
    DWORD cbSize;
    GUID guid;
    DWORD flags;
    ULONG_PTR reserved;
};

using GetHidGuidFn = void(WINAPI*)(GUID*);
using GetPreparsedFn = BOOLEAN(WINAPI*)(HANDLE, void**);
using FreePreparsedFn = BOOLEAN(WINAPI*)(void*);
using GetCapsFn = LONG(WINAPI*)(void*, HidCaps*);
using GetClassDevsFn = HANDLE(WINAPI*)(const GUID*, PCWSTR, HWND, DWORD);
using EnumInterfacesFn = BOOL(WINAPI*)(HANDLE, void*, const GUID*, DWORD, InterfaceData*);
using GetDetailFn = BOOL(WINAPI*)(HANDLE, InterfaceData*, void*, DWORD, DWORD*, void*);
using DestroyListFn = BOOL(WINAPI*)(HANDLE);

struct Collection {
    std::wstring path;
    USHORT usagePage = 0;
    USHORT usage = 0;
    USHORT inputLength = 0;
    USHORT outputLength = 0;
    USHORT featureLength = 0;
};

template <class F>
F proc(HMODULE m, const char* name) {
    return m == nullptr ? nullptr : reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

std::vector<Collection> g923Collections() {
    std::vector<Collection> found;
    const HMODULE hid = LoadLibraryExW(L"hid.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const HMODULE setup = LoadLibraryExW(L"setupapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto getGuid = proc<GetHidGuidFn>(hid, "HidD_GetHidGuid");
    const auto getPreparsed = proc<GetPreparsedFn>(hid, "HidD_GetPreparsedData");
    const auto freePreparsed = proc<FreePreparsedFn>(hid, "HidD_FreePreparsedData");
    const auto getCaps = proc<GetCapsFn>(hid, "HidP_GetCaps");
    const auto classDevs = proc<GetClassDevsFn>(setup, "SetupDiGetClassDevsW");
    const auto enumInterfaces = proc<EnumInterfacesFn>(setup, "SetupDiEnumDeviceInterfaces");
    const auto detail = proc<GetDetailFn>(setup, "SetupDiGetDeviceInterfaceDetailW");
    const auto destroy = proc<DestroyListFn>(setup, "SetupDiDestroyDeviceInfoList");
    if (!getGuid || !getPreparsed || !freePreparsed || !getCaps || !classDevs || !enumInterfaces || !detail || !destroy) {
        return found;
    }
    GUID guid{};
    getGuid(&guid);
    constexpr DWORD kPresent = 0x2, kInterface = 0x10;
    HANDLE infos = classDevs(&guid, nullptr, nullptr, kPresent | kInterface);
    if (infos == INVALID_HANDLE_VALUE) return found;
    for (DWORD i = 0;; ++i) {
        InterfaceData data{};
        data.cbSize = sizeof(data);
        if (!enumInterfaces(infos, nullptr, &guid, i, &data)) break;
        DWORD needed = 0;
        detail(infos, &data, nullptr, 0, &needed, nullptr);
        if (needed < 8) continue;
        std::vector<std::uint8_t> buf(needed);
        *reinterpret_cast<DWORD*>(buf.data()) = sizeof(void*) == 8 ? 8 : 6;   // SP_DEVICE_INTERFACE_DETAIL_DATA_W
        if (!detail(infos, &data, buf.data(), needed, nullptr, nullptr)) continue;
        const std::wstring path(reinterpret_cast<const wchar_t*>(buf.data() + 4));
        std::wstring low = path;
        for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
        if (low.find(L"vid_046d&pid_c26e") == std::wstring::npos || low.find(L"wheelbri") != std::wstring::npos) continue;
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        void* pre = nullptr;
        HidCaps caps{};
        if (getPreparsed(h, &pre)) {
            getCaps(pre, &caps);
            freePreparsed(pre);
            found.push_back({path, caps.usagePage, caps.usage, caps.inputLength, caps.outputLength, caps.featureLength});
        }
        CloseHandle(h);
    }
    destroy(infos);
    return found;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (const wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

class WindowsHid : public Transport {
public:
    explicit WindowsHid(const Collection& c) : input_(c.inputLength), output_(c.outputLength) {
        handle_ = CreateFileW(c.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("cannot open " + narrow(c.path) + " (error " + std::to_string(GetLastError()) + ")");
        }
        readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        buffer_.resize(input_);
    }
    WindowsHid(const WindowsHid&) = delete;
    WindowsHid& operator=(const WindowsHid&) = delete;
    ~WindowsHid() override {
        if (pending_) CancelIo(handle_);
        CloseHandle(readEvent_);
        CloseHandle(handle_);
    }

    void write(const std::span<const std::uint8_t> report) override {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const auto done = [&ov] { CloseHandle(ov.hEvent); };
        if (!WriteFile(handle_, report.data(), static_cast<DWORD>(report.size()), nullptr, &ov) &&
            GetLastError() != ERROR_IO_PENDING) {
            const DWORD e = GetLastError();
            done();
            throw std::runtime_error("write failed at once (error " + std::to_string(e) + ")");
        }
        if (WaitForSingleObject(ov.hEvent, 1000) != WAIT_OBJECT_0) {
            CancelIo(handle_);
            done();
            throw std::runtime_error("write timed out");
        }
        DWORD n = 0;
        const BOOL ok = GetOverlappedResult(handle_, &ov, &n, FALSE);
        done();
        if (!ok) throw std::runtime_error("write failed (error " + std::to_string(GetLastError()) + ")");
    }

    // One read in flight at a time, kept across timeouts.
    std::optional<Bytes> read(const double timeoutS) override {
        if (!pending_) {
            ResetEvent(readEvent_);
            readOv_ = OVERLAPPED{};
            readOv_.hEvent = readEvent_;
            if (!ReadFile(handle_, buffer_.data(), static_cast<DWORD>(buffer_.size()), nullptr, &readOv_) &&
                GetLastError() != ERROR_IO_PENDING) {
                throw std::runtime_error("read failed at once (error " + std::to_string(GetLastError()) + ")");
            }
            pending_ = true;
        }
        const DWORD ms = static_cast<DWORD>(std::max(1.0, timeoutS * 1000.0));
        if (WaitForSingleObject(readEvent_, ms) != WAIT_OBJECT_0) return std::nullopt;
        DWORD n = 0;
        const BOOL ok = GetOverlappedResult(handle_, &readOv_, &n, FALSE);
        pending_ = false;
        if (!ok) return std::nullopt;
        return Bytes(buffer_.begin(), buffer_.begin() + n);
    }

    [[nodiscard]] std::size_t outputLength() const override { return output_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE readEvent_ = nullptr;
    OVERLAPPED readOv_{};
    bool pending_ = false;
    std::size_t input_;
    std::size_t output_;
    std::vector<std::uint8_t> buffer_;
};

// HID++: written on the long (0x11) collection, answered on either it or the very long (0x12) one.
class HidppPair : public Transport {
public:
    HidppPair(std::unique_ptr<WindowsHid> longOne, std::unique_ptr<WindowsHid> veryLong)
        : long_(std::move(longOne)), veryLong_(std::move(veryLong)) {}
    void write(const std::span<const std::uint8_t> report) override { long_->write(report); }
    std::optional<Bytes> read(const double timeoutS) override {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutS);
        while (std::chrono::steady_clock::now() < end) {
            if (auto d = long_->read(0.005)) return d;
            if (veryLong_) {
                if (auto d = veryLong_->read(0.005)) return d;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] std::size_t outputLength() const override { return long_->outputLength(); }

private:
    std::unique_ptr<WindowsHid> long_;
    std::unique_ptr<WindowsHid> veryLong_;
};

}  // namespace

int runProbe(const std::string& out, const std::string& ghub) {
    ProbeRun run;
    const std::time_t now = std::time(nullptr);
    char when[32];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    run.when = when;
    run.ghub = ghub;
    const auto cols = g923Collections();
    if (cols.empty()) {
        std::cout << "no G923 (046D:C26E) HID collections found: is it plugged in, in PC mode?\n";
        return 1;
    }
    const Collection* tf = nullptr;
    const Collection* longOne = nullptr;
    const Collection* veryLong = nullptr;
    for (const Collection& c : cols) {
        char b[96];
        std::snprintf(b, sizeof(b), ": usage page 0x%04X usage 0x%04X, in/out/feature %u/%u/%u", c.usagePage, c.usage,
                      c.inputLength, c.outputLength, c.featureLength);
        run.collections.push_back(narrow(c.path) + b);
        std::cout << "found" << b << "\n";
        if (c.usagePage == 0xFFFD) tf = &c;
        if (c.usagePage == 0xFF43 && c.outputLength == 20) longOne = &c;
        if (c.usagePage == 0xFF43 && c.outputLength == 64) veryLong = &c;
    }
    if (tf == nullptr) {
        run.problems.emplace_back("no TrueForce collection (usage page 0xFFFD)");
    } else {
        try {
            WindowsHid t(*tf);
            std::cout << "TrueForce: asking parameters 0x00..0x3F...\n";
            run.trueForce = probeTrueForce(t);
        } catch (const std::exception& e) {   // one part failing must not lose the other
            run.problems.push_back(std::string("TrueForce probe: ") + e.what());
            std::cout << "TrueForce probe failed: " << e.what() << "\n";
        }
    }
    if (longOne == nullptr) {
        run.problems.emplace_back("no HID++ long collection (usage page 0xFF43, 20 bytes out)");
    } else {
        try {
            HidppPair t(std::make_unique<WindowsHid>(*longOne),
                        veryLong != nullptr ? std::make_unique<WindowsHid>(*veryLong) : nullptr);
            std::cout << "HID++: protocol, features, firmware, name...\n";
            run.hidpp = probeHidpp(t);
        } catch (const std::exception& e) {
            run.problems.push_back(std::string("HID++ probe: ") + e.what());
            std::cout << "HID++ probe failed: " << e.what() << "\n";
        }
    }
    std::filesystem::create_directories(out);
    const std::filesystem::path file = std::filesystem::path(out) / ("probe-ghub-" + ghub + ".md");
    std::ofstream(file, std::ios::binary | std::ios::trunc) << probeMarkdown(run);
    std::cout << "written: " << file.string() << "\n";
    return 0;
}
#else
int runProbe(const std::string&, const std::string&) {
    std::cout << "the probe talks to the wheel through Windows' HID: run it on Windows\n";
    return 2;
}
#endif

}  // namespace wheelbri::g923cap
