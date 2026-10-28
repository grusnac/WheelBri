#include "LogiHook.hpp"

#include "Guard.hpp"
#include "ShimConfig.hpp"
#include "ShimLogging.hpp"

#include "wheelbri/Logging.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

#if defined(__GNUC__) && defined(__x86_64__)
#define WHEELBRI_LOGI_HOOK 1
#endif

namespace wheelbri::shim {

#ifdef WHEELBRI_LOGI_HOOK

namespace {

const logging::Logger& log() {
    static const logging::Logger instance = logger("shim.logi");
    return instance;
}

// One wrapped function. The stub calls through `real` (offset 0).
struct Wrapped {
    void* real = nullptr;
    char name[64] = {};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> lastLineMs{0};
};

constexpr std::size_t kMaxWrapped = 256;
constexpr std::size_t kThunkSize = 32;
Wrapped gWrapped[kMaxWrapped];   // never freed: the game may call a wrapper until it exits
std::size_t gCount = 0;
unsigned char* gCode = nullptr;  // kMaxWrapped thunks: mov r10, wrapped; mov r11, stub; jmp r11
std::mutex gMutex;

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn gRealGetProcAddress = nullptr;
int gArmed = 0;   // imports of GetProcAddress pointed at the hook (armLogiHook)

}  // namespace

extern "C" void wheelbri_logi_stub();
extern "C" void wheelbri_logi_after(Wrapped* wrapped, const std::uint64_t* args, const std::uint64_t* result) noexcept;

// Saves RCX..R9 and XMM0..3 at 96(%rsp) (args[0..3], then args[4..11] as four 16-byte XMM slots), copies 8 stack
// arguments on, calls the real function, keeps RAX and XMM0 at 208(%rsp), logs, and returns both unchanged.
// Frame: 224 bytes + the pushed RBP keeps RSP 16-byte aligned at each call.
asm(R"(
    .text
    .globl wheelbri_logi_stub
wheelbri_logi_stub:
    pushq %rbp
    movq %rsp, %rbp
    subq $224, %rsp
    movq %rcx, 96(%rsp)
    movq %rdx, 104(%rsp)
    movq %r8, 112(%rsp)
    movq %r9, 120(%rsp)
    movdqu %xmm0, 128(%rsp)
    movdqu %xmm1, 144(%rsp)
    movdqu %xmm2, 160(%rsp)
    movdqu %xmm3, 176(%rsp)
    movq %rbx, 192(%rsp)
    movq %r10, %rbx
    movq 48(%rbp), %rax
    movq %rax, 32(%rsp)
    movq 56(%rbp), %rax
    movq %rax, 40(%rsp)
    movq 64(%rbp), %rax
    movq %rax, 48(%rsp)
    movq 72(%rbp), %rax
    movq %rax, 56(%rsp)
    movq 80(%rbp), %rax
    movq %rax, 64(%rsp)
    movq 88(%rbp), %rax
    movq %rax, 72(%rsp)
    movq 96(%rbp), %rax
    movq %rax, 80(%rsp)
    movq 104(%rbp), %rax
    movq %rax, 88(%rsp)
    movq 96(%rsp), %rcx
    movq 104(%rsp), %rdx
    movq 112(%rsp), %r8
    movq 120(%rsp), %r9
    movdqu 128(%rsp), %xmm0
    movdqu 144(%rsp), %xmm1
    movdqu 160(%rsp), %xmm2
    movdqu 176(%rsp), %xmm3
    callq *(%rbx)
    movq %rax, 208(%rsp)
    movq %xmm0, 216(%rsp)
    movq %rbx, %rcx
    leaq 96(%rsp), %rdx
    leaq 208(%rsp), %r8
    callq wheelbri_logi_after
    movq 208(%rsp), %rax
    movq 216(%rsp), %xmm0
    movq 192(%rsp), %rbx
    movq %rbp, %rsp
    popq %rbp
    retq
)");

namespace {

double asDouble(const std::uint64_t bits) {
    double d = 0;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

float asFloat(const std::uint32_t bits) {
    float f = 0;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Whether 8 bytes at `address` can be read (committed, readable, no guard page).
bool readable(const std::uint64_t address, const std::uint64_t size = 8) {
    if (address < 0x10000 || address > 0x7FFFFFFFFFF8ULL) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)) return false;
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (info.State != MEM_COMMIT || (info.Protect & kReadable) == 0 || (info.Protect & PAGE_GUARD) != 0) return false;
    const auto end = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    return address + size <= end;
}

// The first `size` bytes at `address`, hex, when they can be read.
std::string bytesAt(const std::uint64_t address, const std::size_t size) {
    if (!readable(address, size)) return "(unreadable)";
    const auto* p = reinterpret_cast<const unsigned char*>(address);
    std::string text;
    for (std::size_t i = 0; i < size; ++i) {
        char b[4];
        std::snprintf(b, sizeof(b), i == 0 ? "%02x" : " %02x", static_cast<unsigned>(p[i]));
        text += b;
    }
    return text;
}

// A number as the function may have meant it: as is, and its low 32 bits as a signed int when that differs.
std::string number(const std::uint64_t v) {
    char text[64];
    const auto low = static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
    if (v <= 0xFFFF) {
        std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(v));
    } else {
        std::snprintf(text, sizeof(text), "0x%llx (i32 %d)", static_cast<unsigned long long>(v), low);
    }
    return text;
}

std::string pointee(const std::uint64_t address) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, reinterpret_cast<const void*>(address), sizeof(bits));
    char text[160];
    std::snprintf(text, sizeof(text), "[%016llx: f64 %g, f32 %g, i32 %d]", static_cast<unsigned long long>(bits),
                  asDouble(bits), static_cast<double>(asFloat(static_cast<std::uint32_t>(bits))),
                  static_cast<std::int32_t>(static_cast<std::uint32_t>(bits)));
    return text;
}

// A double that is a plain number worth showing (not 0, not a NaN or denormal from bits that were never a double).
bool showsAsDouble(const double d) { return std::isnormal(d) && std::fabs(d) >= 1e-6 && std::fabs(d) < 1e9; }

// The SDK's own view of the force ACC sets (SetTorqueKF): a getter's status and out value, raw bits too (the out
// parameter's width is not established: a float would fill only the low half).
// Logitech's KF reconstruction filter, read in place (trueforce_sdk_x64.dll 1.3.12's layout, from its code): the
// handle's index (+0x118) keys a std::map at module + 0xD8200 to the device; the device holds the filter at +0x178.
// Filter: +0x60 sections, +0x68 sample rate, +0x70/+0x78 corners now, +0x80/+0x88 their targets, +0x90 ramp,
// +0x98/+0xA0 DC factor and state, +0xA8/+0xB0 zero run limit and count, +0xB8 gain; per section, coefficients at
// the arrays +0x10..+0x30 and state at +0x38..+0x58. Nothing is written; any unreadable pointer ends the read.
template <typename T>
bool readAt(const std::uint64_t address, T& value) {
    if (!readable(address, sizeof(T))) return false;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return true;
}

std::uint64_t kfFilterOf(const std::uint64_t handle) {
    const auto sdk = reinterpret_cast<std::uint64_t>(GetModuleHandleW(L"trueforce_sdk_x64.dll"));
    std::int32_t key = 0;
    std::uint64_t head = 0;
    std::uint64_t node = 0;
    if (sdk == 0 || !readAt(handle + 0x118, key) || !readAt(sdk + 0xD8200, head) || !readAt(head + 8, node)) return 0;
    std::uint64_t found = 0;
    for (int depth = 0; depth < 64; ++depth) {
        std::uint8_t nil = 1;
        std::int32_t nodeKey = 0;
        if (!readAt(node + 0x19, nil) || nil != 0 || !readAt(node + 0x20, nodeKey)) break;
        if (nodeKey == key) {
            found = node;
            break;
        }
        if (!readAt(node + (nodeKey < key ? 0x10 : 0x0), node)) break;
    }
    std::uint64_t device = 0;
    std::uint64_t filter = 0;
    if (found == 0 || !readAt(found + 0x28, device) || !readAt(device + 0x178, filter)) return 0;
    return filter;
}

// One line: the filter's settings, and every section's coefficients and state when `sections` (else the first).
std::string kfFilter(const std::uint64_t handle, const bool sections) {
    const std::uint64_t f = kfFilterOf(handle);
    if (f == 0) return " | kf filter: not found";
    std::uint32_t n = 0;
    double v[12] = {};
    static constexpr std::uint64_t kAt[12] = {0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8, 0x0};
    if (!readAt(f + 0x60, n)) return " | kf filter: unreadable";
    for (int i = 0; i < 11; ++i) (void)readAt(f + kAt[i], v[i]);
    std::uint64_t zerosLimit = 0;
    std::uint64_t zeros = 0;
    (void)readAt(f + 0xA8, zerosLimit);
    (void)readAt(f + 0xB0, zeros);
    char text[256];
    std::snprintf(text, sizeof(text),
                  " | kf filter: %u sections, fs %g, corners %g/%g (to %g/%g, ramp %g), dc %g/%g, zeros %llu/%llu, "
                  "gain %g",
                  n, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], static_cast<unsigned long long>(zeros),
                  static_cast<unsigned long long>(zerosLimit), v[10]);
    std::string line = text;
    for (std::uint32_t s = 0; s < std::min<std::uint32_t>(n, sections ? 8U : 1U); ++s) {
        double c[10] = {};
        for (int a = 0; a < 10; ++a) {
            std::uint64_t array = 0;
            if (readAt(f + 0x10 + 8 * static_cast<std::uint64_t>(a), array)) (void)readAt(array + 8ULL * s, c[a]);
        }
        std::snprintf(text, sizeof(text), " [%u] c %g %g %g %g %g s %g %g %g %g %g", s, c[0], c[1], c[2], c[3], c[4],
                      c[5], c[6], c[7], c[8], c[9]);
        line += text;
    }
    return line;
}

// On every SetTorqueKF: the whole filter once a state runs away (NaN, infinite or past 100), at most once a second.
void watchKfFilter(const std::uint64_t handle) {
    static std::atomic<std::uint64_t> lastMs{0};
    static std::uint64_t lastFilter = 0;
    const std::uint64_t now = GetTickCount64();
    // Looked up on every call: a cached address can outlive the filter (the SDK replaces it), and then reads freed
    // memory. Its address is logged whenever it changes.
    const std::uint64_t f = kfFilterOf(handle);
    if (f != lastFilter) {
        WE_LOG_INFO(log(), "logi: KF filter object at {:#x} (was {:#x})", f, lastFilter);
        lastFilter = f;
    }
    std::uint32_t n = 0;
    if (f == 0 || !readAt(f + 0x60, n)) return;
    // The first section's gain and feedback coefficients (+0x10..+0x30): logged whenever they change. A runaway ran
    // with one of them at 0 (its pole 1.32 matched the growth, 2026-10-28), and they should not change while the
    // corners stay put.
    static double lastCoefficients[5] = {};
    static int changesShown = 0;
    double coefficients[5] = {};
    for (int a = 0; a < 5; ++a) {
        std::uint64_t array = 0;
        if (readAt(f + 0x10 + 8 * static_cast<std::uint64_t>(a), array)) (void)readAt(array, coefficients[a]);
    }
    if (std::memcmp(coefficients, lastCoefficients, sizeof(coefficients)) != 0 && changesShown < 200) {
        ++changesShown;
        double corners[4] = {};
        (void)readAt(f + 0x70, corners[0]);
        (void)readAt(f + 0x78, corners[1]);
        (void)readAt(f + 0x80, corners[2]);
        (void)readAt(f + 0x88, corners[3]);
        WE_LOG_INFO(log(), "logi: KF filter coefficients {} {} {} {} {} (were {} {} {} {} {}), corners {}/{} to {}/{}",
                    coefficients[0], coefficients[1], coefficients[2], coefficients[3], coefficients[4],
                    lastCoefficients[0], lastCoefficients[1], lastCoefficients[2], lastCoefficients[3],
                    lastCoefficients[4], corners[0], corners[1], corners[2], corners[3]);
        std::memcpy(lastCoefficients, coefficients, sizeof(coefficients));
    }
    bool wild = false;
    for (std::uint32_t s = 0; s < std::min<std::uint32_t>(n, 8U) && !wild; ++s) {
        for (int a = 5; a < 10 && !wild; ++a) {
            std::uint64_t array = 0;
            double x = 0;
            if (readAt(f + 0x10 + 8 * static_cast<std::uint64_t>(a), array) && readAt(array + 8ULL * s, x)) {
                wild = !std::isfinite(x) || std::fabs(x) > 100.0;
            }
        }
    }
    if (!wild || now - lastMs.load() < 1000) return;
    lastMs = now;
    WE_LOG_WARN(log(), "logi: the KF filter ran away{}", kfFilter(handle, true));
}

std::string probe(const char* name, const std::uint64_t handle) {
    const HMODULE sdk = GetModuleHandleW(L"trueforce_sdk_x64.dll");
    const FARPROC fn = sdk != nullptr && gRealGetProcAddress != nullptr ? gRealGetProcAddress(sdk, name) : nullptr;
    if (fn == nullptr) return std::string(" ") + name + "=missing";
    std::uint64_t out = 0x7FF8DEADDEADDEADULL;
    using GetterFn = int (*)(std::uint64_t, std::uint64_t*);
    const int status = reinterpret_cast<GetterFn>(reinterpret_cast<void*>(fn))(handle, &out);
    char text[160];
    std::snprintf(text, sizeof(text), " %s=%g (status %d, bits %016llx)", name, asDouble(out), status,
                  static_cast<unsigned long long>(out));
    return text;
}

}  // namespace

extern "C" void wheelbri_logi_after(Wrapped* wrapped, const std::uint64_t* args,
                                    const std::uint64_t* result) noexcept {
    try {
        const std::uint64_t call = ++wrapped->calls;
        const bool torqueKf = std::strcmp(wrapped->name, "logiTrueForceSetTorqueKF") == 0;
        if (torqueKf) watchKfFilter(args[0]);
        const std::uint64_t now = GetTickCount64();
        if (call > static_cast<std::uint64_t>(kFullCalls)) {
            std::uint64_t last = wrapped->lastLineMs.load();
            if (now - last < 1000 || !wrapped->lastLineMs.compare_exchange_strong(last, now)) return;
        } else {
            wrapped->lastLineMs = now;
        }
        static const char* const kRegs[] = {"rcx", "rdx", "r8", "r9"};
        std::string line = std::string(wrapped->name) + " #" + std::to_string(call) + ":";
        for (int i = 0; i < 4; ++i) {
            line += std::string(" ") + kRegs[i] + "=" + number(args[i]);
            if (readable(args[i])) line += " -> " + pointee(args[i]);
        }
        for (int i = 0; i < 4; ++i) {
            const std::uint64_t bits = args[4 + i * 2];
            const double f = asFloat(static_cast<std::uint32_t>(bits));
            if (const double x = asDouble(bits); showsAsDouble(x)) {
                line += " xmm" + std::to_string(i) + "=" + std::to_string(x);
            } else if ((bits >> 32U) == 0 && showsAsDouble(f)) {   // a float
                line += " xmm" + std::to_string(i) + "=f" + std::to_string(f);
            }
        }
        line += " | rax=" + number(result[0]);
        if (const double x0 = asDouble(result[1]); showsAsDouble(x0)) line += " xmm0=" + std::to_string(x0);
        if (call == static_cast<std::uint64_t>(kFullCalls)) line += " (from now one call a second)";
        // What the texture and Sync carry: their buffers (the second argument), raw.
        if (std::strcmp(wrapped->name, "logiTrueForceSetStreamTF") == 0 ||
            std::strcmp(wrapped->name, "logiTrueForceSync") == 0) {
            line += " | rdx bytes: " + bytesAt(args[1], 64);
        }
        if (torqueKf) {
            line += " |" + probe("logiTrueForceGetGainKF", args[0]) + probe("logiTrueForceGetTorqueKF", args[0]) +
                    probe("logiTrueForceGetAngularVelocityDegrees", args[0]) +
                    probe("logiTrueForceGetDamping", args[0]) + kfFilter(args[0], false);
        }
        WE_LOG_INFO(log(), "{}", line);
    } catch (...) {
        // Never into Logitech's caller.
    }
}

namespace {

bool isLogitechSdk(const HMODULE module) {
    static std::vector<HMODULE> yes;
    static std::vector<HMODULE> no;
    if (std::find(yes.begin(), yes.end(), module) != yes.end()) return true;
    if (std::find(no.begin(), no.end(), module) != no.end()) return false;
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring lower(path, length);
    for (wchar_t& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    const bool sdk = lower.find(L"trueforce_sdk") != std::wstring::npos ||
                     lower.find(L"logi_steering_wheel") != std::wstring::npos;
    (sdk ? yes : no).push_back(module);
    if (sdk) {
        std::string narrow;
        for (const wchar_t c : lower) narrow += c < 128 ? static_cast<char>(c) : '?';
        WE_LOG_INFO(log(), "logi: the game looks up functions in {}", narrow);
    }
    return sdk;
}

FARPROC wrap(const char* name, const FARPROC real) {
    for (std::size_t i = 0; i < gCount; ++i) {
        if (gWrapped[i].real == reinterpret_cast<void*>(real)) return reinterpret_cast<FARPROC>(gCode + i * kThunkSize);
    }
    if (gCode == nullptr) {
        gCode = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, kMaxWrapped * kThunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (gCode == nullptr) {
            WE_LOG_ERROR(log(), "logi_log: no memory for the wrappers ({})", GetLastError());
            return real;
        }
    }
    if (gCount == kMaxWrapped) return real;
    Wrapped& w = gWrapped[gCount];
    w.real = reinterpret_cast<void*>(real);
    std::snprintf(w.name, sizeof(w.name), "%s", name);
    unsigned char* code = gCode + gCount * kThunkSize;
    const auto wrappedAddress = reinterpret_cast<std::uint64_t>(&w);
    const auto stubAddress = reinterpret_cast<std::uint64_t>(&wheelbri_logi_stub);
    code[0] = 0x49;   // mov r10, imm64
    code[1] = 0xBA;
    std::memcpy(code + 2, &wrappedAddress, 8);
    code[10] = 0x49;  // mov r11, imm64
    code[11] = 0xBB;
    std::memcpy(code + 12, &stubAddress, 8);
    code[20] = 0x41;  // jmp r11
    code[21] = 0xFF;
    code[22] = 0xE3;
    FlushInstructionCache(GetCurrentProcess(), code, kThunkSize);
    ++gCount;
    WE_LOG_INFO(log(), "logi: {} wrapped", name);
    return reinterpret_cast<FARPROC>(code);
}

FARPROC WINAPI hookedGetProcAddress(const HMODULE module, const LPCSTR name) {
    const FARPROC real = gRealGetProcAddress(module, name);
    if (real == nullptr || IS_INTRESOURCE(name)) return real;
    return guardedOr(real, "hookedGetProcAddress", [&] {
        // Armed in every game the shim is in (armLogiHook); only logi_log wraps anything.
        if (!shimConfig().logiLog) return real;
        std::lock_guard lock(gMutex);
        return isLogitechSdk(module) ? wrap(name, real) : real;
    });
}

// Points the module's imports of GetProcAddress (from any DLL: kernel32, api-ms-win-core-libraryloader-*) at the hook.
int patchImports(const HMODULE module) {
    auto* const base = reinterpret_cast<unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return 0;
    int patched = 0;
    for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name != 0;
         ++desc) {
        if (desc->OriginalFirstThunk == 0) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), "GetProcAddress") != 0) continue;
            if (slots->u1.Function == names->u1.AddressOfData) continue;   // not bound yet: installLogiHook retries
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) continue;
            if (gRealGetProcAddress == nullptr) {
                gRealGetProcAddress = reinterpret_cast<GetProcAddressFn>(slots->u1.Function);
            }
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(&hookedGetProcAddress);
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
            ++patched;
        }
    }
    return patched;
}

}  // namespace

void armLogiHook() noexcept {
    // Loader lock: only the import table is touched (VirtualProtect), nothing logged, nothing allocated.
    gArmed = patchImports(GetModuleHandleW(nullptr));
}

void installLogiHook() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (gArmed == 0) gArmed = patchImports(GetModuleHandleW(nullptr));
        if (gArmed == 0 || gRealGetProcAddress == nullptr) {
            WE_LOG_WARN(log(), "logi_log: the game's .exe imports no GetProcAddress: nothing is wrapped");
            return;
        }
        WE_LOG_INFO(log(), "logi_log: on since the game loaded the shim ({} import(s) of GetProcAddress); Logitech "
                           "SDK calls are logged here (component shim.logi)", gArmed);
    });
}

#else

void armLogiHook() noexcept {}

void installLogiHook() {
    static std::once_flag once;
    std::call_once(once, [] {
        WE_LOG_WARN(logger("shim.logi"), "logi_log: only in the 64-bit MinGW build of the shim");
    });
}

#endif

}  // namespace wheelbri::shim
