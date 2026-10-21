// A stand-in for Logitech's real wheel SDK helper DLL, for LogiDllTest: wheelbri-logi.dll is pointed at it
// (wheelbri-logi.ini real_library, or where G HUB / the old software keep theirs) and passes the game's calls on to it.
// It answers with fixed values and keeps the arguments of the last call (FakeLogiLast), so a test sees what came
// through. LogiGetShifterMode is left out on purpose: a function the real library does not have.
#include <windows.h>

#include <cstdint>
#include <cstring>

using Raw = std::int64_t;

struct FakeLast {
    char name[64];
    Raw args[4];
    float floats[3];
    std::uint8_t properties[36];   // what LogiSetPreferredControllerProperties was handed
    int calls;
};

namespace {

FakeLast gLast{};
std::uint8_t gState[272]{};   // DIJOYSTATE2's size; the stand-in reads its first 80 bytes

Raw note(const char* name, const Raw a, const Raw b, const Raw c, const Raw d) {
    std::strncpy(gLast.name, name, sizeof(gLast.name) - 1);
    gLast.args[0] = a;
    gLast.args[1] = b;
    gLast.args[2] = c;
    gLast.args[3] = d;
    ++gLast.calls;
    return 0;
}

void fillText(const Raw buffer, const Raw size, const wchar_t* text) {
    auto* out = reinterpret_cast<wchar_t*>(buffer);
    const auto room = static_cast<std::int32_t>(size);
    std::int32_t i = 0;
    for (; i + 1 < room && text[i] != L'\0'; ++i) out[i] = text[i];
    if (room > 0) out[i] = L'\0';
}

}  // namespace

#define FAKE_EXPORT extern "C" __declspec(dllexport)

FAKE_EXPORT FakeLast* FakeLogiLast() { return &gLast; }

FAKE_EXPORT Raw LogiSteeringInitialize(Raw a, Raw b, Raw c, Raw d) { note(__func__, a, b, c, d); return 1; }
FAKE_EXPORT Raw LogiSteeringInitializeWithWindow(Raw a, Raw b, Raw c, Raw d) { note(__func__, a, b, c, d); return 1; }
FAKE_EXPORT Raw LogiSteeringShutdown(Raw a, Raw b, Raw c, Raw d) { return note(__func__, a, b, c, d); }
FAKE_EXPORT Raw LogiUpdate(Raw a, Raw b, Raw c, Raw d) { note(__func__, a, b, c, d); return 1; }
FAKE_EXPORT Raw LogiIsConnected(Raw a, Raw b, Raw c, Raw d) { note(__func__, a, b, c, d); return a == 0 ? 1 : 0; }
FAKE_EXPORT Raw LogiButtonIsPressed(Raw a, Raw b, Raw c, Raw d) { note(__func__, a, b, c, d); return b == 3 ? 1 : 0; }
FAKE_EXPORT Raw LogiPlaySpringForce(Raw a, Raw b, Raw c, Raw d) { note(__func__, a, b, c, d); return 1; }
FAKE_EXPORT Raw LogiPlayConstantForce(Raw a, Raw b, Raw c, Raw d) { note(__func__, a, b, c, d); return 1; }
FAKE_EXPORT Raw LogiGetNonLinearValue(Raw a, Raw b, Raw c, Raw d) { note(__func__, a, b, c, d); return b / 2; }

FAKE_EXPORT Raw LogiGetState(Raw a, Raw b, Raw c, Raw d) {
    note(__func__, a, b, c, d);
    if (a != 0) return 0;   // no wheel at that index: no state
    const std::int32_t x = 1234;
    std::memcpy(gState, &x, sizeof(x));
    return reinterpret_cast<Raw>(gState);
}

FAKE_EXPORT Raw LogiGetFriendlyProductName(Raw a, Raw b, Raw c, Raw d) {
    note(__func__, a, b, c, d);
    if (a != 0) return 0;
    fillText(b, c, L"G923 é");   // one letter outside ASCII: the log prints it as '?'
    return 1;
}

FAKE_EXPORT Raw LogiGetOperatingRange(Raw a, Raw b, Raw c, Raw d) {
    note(__func__, a, b, c, d);
    if (a != 0 || b == 0) return 0;
    *reinterpret_cast<int*>(b) = 900;
    return 1;
}

FAKE_EXPORT Raw LogiGetCurrentControllerProperties(Raw a, Raw b, Raw c, Raw d) {
    note(__func__, a, b, c, d);
    if (a != 0 || b == 0) return 0;
    std::uint8_t bytes[36]{};
    bytes[0] = 1;                                  // forceEnable
    const int gain = 80;
    std::memcpy(bytes + 4, &gain, sizeof(gain));   // overallGain
    std::memcpy(reinterpret_cast<void*>(b), bytes, sizeof(bytes));
    return 1;
}

FAKE_EXPORT Raw LogiSetPreferredControllerProperties(Raw a, Raw b, Raw c, Raw d) {
    note(__func__, a, b, c, d);
    // Changes its copy, as the real one may: the stand-in reads the game's values before the call.
    if (a != 0) {
        std::memcpy(gLast.properties, reinterpret_cast<const void*>(a), sizeof(gLast.properties));
        std::memset(reinterpret_cast<void*>(a), 0, sizeof(gLast.properties));
    }
    return 1;
}

FAKE_EXPORT Raw LogiPlayLeds(Raw a, float rpm, float firstLed, float redline) {
    note(__func__, a, 0, 0, 0);
    gLast.floats[0] = rpm;
    gLast.floats[1] = firstLed;
    gLast.floats[2] = redline;
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
