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

}  // namespace

extern "C" void wheelbri_logi_after(Wrapped* wrapped, const std::uint64_t* args,
                                    const std::uint64_t* result) noexcept {
    try {
        const std::uint64_t call = ++wrapped->calls;
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
        // The texture's samples: its header says 0x1040 bytes in all (as r8), 48,000 Hz and 1,024 samples, so they
        // likely follow the 64-byte header; +0x28 holds a pointer that may lead to them instead. Both are shown, each
        // with how many of its 4,096 bytes are not 0 (silence or not).
        if (std::strcmp(wrapped->name, "logiTrueForceSetStreamTF") == 0) {
            constexpr std::uint64_t kHeader = 64;
            constexpr std::uint64_t kSamples = 4096;
            const auto texture = [](const std::uint64_t at) {
                if (!readable(at, kSamples)) return std::string("(unreadable)");
                const auto* p = reinterpret_cast<const unsigned char*>(at);
                std::size_t set = 0;
                for (std::uint64_t i = 0; i < kSamples; ++i) set += p[i] != 0 ? 1U : 0U;
                return std::to_string(set) + " of " + std::to_string(kSamples) + " bytes not 0: " + bytesAt(at, 64);
            };
            line += " | after the header: " + texture(args[1] + kHeader);
            std::uint64_t pointer = 0;
            if (readable(args[1] + 0x28, sizeof(pointer))) {
                std::memcpy(&pointer, reinterpret_cast<const void*>(args[1] + 0x28), sizeof(pointer));
                line += " | at +0x28's pointer: " + texture(pointer);
            }
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
