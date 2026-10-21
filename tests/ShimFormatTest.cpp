// The dinput8.dll shim's renderings of DirectInput values for its log (shim/src/DiFormat.cpp) and its conversions
// between DirectInput's structures and the portable ones (DiConvert.cpp, EmulatedDevice.hpp's presentAs).
// Windows only (MinGW: run under Wine).

#include "DiConvert.hpp"
#include "DiFormat.hpp"
#include "EmulatedDevice.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <iterator>

#include <cstring>
#include <string>
#include <vector>

namespace wheelbri::shim {
namespace {

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A DIPROP_* id as the GUID reference DirectInput passes (MAKEDIPROP).
const GUID& prop(const std::uintptr_t id) { return *reinterpret_cast<const GUID*>(id); }

// Wide text with no terminator inside `maxChars` is cut there; null and empty are told apart.
TEST(ShimFormat, Utf8OfWideAndNarrowText) {
    EXPECT_EQ(toUtf8(static_cast<const WCHAR*>(nullptr)), "(null)");
    EXPECT_EQ(toUtf8(L""), "");
    EXPECT_EQ(toUtf8(L"G920 é"), "G920 \xc3\xa9");
    const WCHAR unterminated[4] = {L'a', L'b', L'c', L'd'};
    EXPECT_EQ(toUtf8(unterminated, 4), "abcd");
    EXPECT_EQ(toUtf8(unterminated, 2), "ab");
    const WCHAR lone[] = {0xD800, 0};   // an unpaired surrogate still converts (to U+FFFD)
    EXPECT_FALSE(toUtf8(lone).empty());
    EXPECT_EQ(toUtf8(static_cast<const CHAR*>(nullptr)), "(null)");
    EXPECT_EQ(toUtf8("wheel"), "wheel");
    // An ANSI name the game did not end inside its array stops at the array's end too (it used to read on).
    CHAR unendedName[MAX_PATH + 8];
    std::fill(std::begin(unendedName), std::end(unendedName), 'n');
    unendedName[MAX_PATH + 7] = '\0';
    EXPECT_EQ(toUtf8(unendedName).size(), static_cast<std::size_t>(MAX_PATH));
    EXPECT_EQ(toUtf8(unendedName, 3), "nnn");
}

// Known GUIDs by name, HID product GUIDs as vid / pid, anything else as the bare GUID.
TEST(ShimFormat, GuidsByNameOrProduct) {
    EXPECT_EQ(guidToString(GUID_XAxis).size(), 38U);
    EXPECT_TRUE(contains(describeGuid(GUID_ConstantForce), "GUID_ConstantForce {"));
    EXPECT_TRUE(contains(describeGuid(IID_IDirectInputEffect), "IID_IDirectInputEffect"));
    GUID product{};
    product.Data1 = 0xC262046D;   // PID C262, VID 046D
    const BYTE pidvid[8] = {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'};
    std::memcpy(product.Data4, pidvid, 8);
    EXPECT_TRUE(contains(describeGuid(product), "product VID_046D&PID_C262"));
    GUID notProduct = product;
    notProduct.Data2 = 1;   // the PIDVID tail alone does not make a product GUID
    EXPECT_FALSE(contains(describeGuid(notProduct), "product"));
    notProduct = product;
    notProduct.Data3 = 1;
    EXPECT_FALSE(contains(describeGuid(notProduct), "product"));
    GUID other{};
    other.Data1 = 0x12345678;
    EXPECT_EQ(describeGuid(other), guidToString(other));
}

// DIPROP_* are small integers by name (an unknown small one by number); a real GUID is a custom property.
TEST(ShimFormat, PropertiesByName) {
    EXPECT_EQ(describeProperty(DIPROP_RANGE), "DIPROP_RANGE");
    EXPECT_EQ(describeProperty(DIPROP_TYPENAME), "DIPROP_TYPENAME");
    EXPECT_EQ(describeProperty(prop(0)), "?");
    EXPECT_EQ(describeProperty(prop(27)), "DIPROP_27");
    EXPECT_TRUE(contains(describeProperty(GUID_XAxis), "property {"));
}

// Each kind of property value is read only when its header says the struct is that big.
TEST(ShimFormat, PropertyValuesReadOnlyAsFarAsTheirSize) {
    EXPECT_EQ(describePropertyValue(DIPROP_RANGE, nullptr), "header=null");

    DIPROPRANGE range{};
    range.diph.dwSize = sizeof(range);
    range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    range.lMin = -100;
    range.lMax = 100;
    for (const auto id : {4, 18, 19}) {
        EXPECT_TRUE(contains(describePropertyValue(prop(id), &range.diph), "range=[-100, 100]")) << id;
    }
    range.diph.dwSize = sizeof(DIPROPHEADER) + 2;   // too short for a range
    EXPECT_TRUE(contains(describePropertyValue(DIPROP_RANGE, &range.diph), "size="));

    DIPROPGUIDANDPATH path{};
    path.diph.dwSize = sizeof(path);
    path.guidClass = GUID_XAxis;
    std::wcscpy(path.wszPath, L"\\\\?\\hid#vid_046d");
    EXPECT_TRUE(contains(describePropertyValue(DIPROP_GUIDANDPATH, &path.diph), "path=\\\\?\\hid#vid_046d"));
    path.diph.dwSize = sizeof(DIPROPDWORD);   // too short: as a dword
    EXPECT_TRUE(contains(describePropertyValue(DIPROP_GUIDANDPATH, &path.diph), "dword="));

    DIPROPSTRING text{};
    text.diph.dwSize = sizeof(text);
    std::wcscpy(text.wsz, L"G920");
    for (const auto id : {13, 14, 16, 20, 25, 26}) {
        EXPECT_TRUE(contains(describePropertyValue(prop(id), &text.diph), "string=\"G920\"")) << id;
    }
    text.diph.dwSize = sizeof(DIPROPHEADER);
    EXPECT_TRUE(contains(describePropertyValue(DIPROP_PRODUCTNAME, &text.diph), "size="));

    DIPROPDWORD dword{};
    dword.diph.dwSize = sizeof(dword);
    dword.dwData = 0xC262046D;
    EXPECT_TRUE(contains(describePropertyValue(DIPROP_VIDPID, &dword.diph), "vid=046D pid=C262"));
    dword.dwData = 7;
    EXPECT_TRUE(contains(describePropertyValue(DIPROP_BUFFERSIZE, &dword.diph), "dword=7"));
    DIPROPHEADER bare{};
    bare.dwSize = sizeof(bare);
    EXPECT_TRUE(contains(describePropertyValue(DIPROP_VIDPID, &bare), "size="));
}

// Known codes by name, others as hex.
TEST(ShimFormat, Hresults) {
    EXPECT_EQ(describeHresult(DI_OK), "DI_OK");
    EXPECT_EQ(describeHresult(DIERR_INVALIDPARAM), "DIERR_INVALIDPARAM");
    EXPECT_EQ(describeHresult(E_NOINTERFACE), "E_NOINTERFACE");
    EXPECT_EQ(describeHresult(static_cast<HRESULT>(0x80001234)), "HRESULT 0x80001234");
}

// The device type by name with its subtype and HID flag; a type outside DirectInput 8's list is "?".
TEST(ShimFormat, DeviceTypes) {
    EXPECT_TRUE(contains(describeDevType(DI8DEVTYPE_DRIVING | (2 << 8) | DIDEVTYPE_HID), "type=DRIVING subtype=2 HID"));
    EXPECT_TRUE(contains(describeDevType(DI8DEVTYPE_DEVICE), "type=DEVICE"));
    EXPECT_TRUE(contains(describeDevType(DI8DEVTYPE_SUPPLEMENTAL), "type=SUPPLEMENTAL"));
    EXPECT_TRUE(contains(describeDevType(0x10), "type=?"));
    EXPECT_TRUE(contains(describeDevType(0x1D), "type=?"));
    EXPECT_FALSE(contains(describeDevType(DI8DEVTYPE_JOYSTICK), "HID"));
}

// Flag sets list each flag that is set; enumeration flags without any read ALLDEVICES.
TEST(ShimFormat, FlagSets) {
    EXPECT_EQ(describeEnumFlags(0), "ALLDEVICES");
    EXPECT_EQ(describeEnumFlags(DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK | DIEDFL_INCLUDEALIASES |
                                DIEDFL_INCLUDEPHANTOMS | DIEDFL_INCLUDEHIDDEN),
              "ATTACHEDONLY|FORCEFEEDBACK|INCLUDEALIASES|INCLUDEPHANTOMS|INCLUDEHIDDEN");
    EXPECT_EQ(describeCooperativeLevel(0), "");
    EXPECT_EQ(describeCooperativeLevel(DISCL_EXCLUSIVE | DISCL_NONEXCLUSIVE | DISCL_FOREGROUND | DISCL_BACKGROUND |
                                       DISCL_NOWINKEY),
              "EXCLUSIVE|NONEXCLUSIVE|FOREGROUND|BACKGROUND|NOWINKEY");
    EXPECT_EQ(describeEffectFlags(0), "none");
    EXPECT_EQ(describeEffectFlags(DIEP_ALLPARAMS | DIEP_START | DIEP_NORESTART | DIEP_NODOWNLOAD),
              "DURATION|SAMPLEPERIOD|GAIN|TRIGGERBUTTON|TRIGGERREPEATINTERVAL|AXES|DIRECTION|ENVELOPE|"
              "TYPESPECIFICPARAMS|STARTDELAY|START|NORESTART|NODOWNLOAD");
    const std::string type = describeObjectType(DIDFT_RELAXIS | DIDFT_ABSAXIS | DIDFT_PSHBUTTON | DIDFT_TGLBUTTON |
                                                DIDFT_POV | DIDFT_COLLECTION | DIDFT_NODATA | DIDFT_FFACTUATOR |
                                                DIDFT_FFEFFECTTRIGGER | DIDFT_OUTPUT | DIDFT_MAKEINSTANCE(3));
    EXPECT_TRUE(contains(type, "RELAXIS|ABSAXIS|PSHBUTTON|TGLBUTTON|POV|COLLECTION|NODATA|FFACTUATOR|FFEFFECTTRIGGER|"
                               "OUTPUT #3"));
}

// Capabilities: every flag; the force feedback fields only when the struct has them (not a DirectX 3 one).
TEST(ShimFormat, CapabilitiesOfBothSizes) {
    DIDEVCAPS caps{};
    caps.dwSize = sizeof(caps);
    caps.dwFlags = DIDC_ATTACHED | DIDC_POLLEDDEVICE | DIDC_EMULATED | DIDC_POLLEDDATAFORMAT | DIDC_FORCEFEEDBACK |
                   DIDC_FFATTACK | DIDC_FFFADE | DIDC_SATURATION | DIDC_POSNEGCOEFFICIENTS | DIDC_POSNEGSATURATION |
                   DIDC_DEADBAND | DIDC_STARTDELAY;
    caps.dwAxes = 3;
    caps.dwFFSamplePeriod = 1000;
    const std::string full = describeCaps(caps);
    EXPECT_TRUE(contains(full, "ATTACHED|POLLEDDEVICE|EMULATED|POLLEDDATAFORMAT|FORCEFEEDBACK|FFATTACK|FFFADE|"
                               "SATURATION|POSNEGCOEFFICIENTS|POSNEGSATURATION|DEADBAND|STARTDELAY"));
    EXPECT_TRUE(contains(full, "axes=3"));
    EXPECT_TRUE(contains(full, "ffSamplePeriod=1000"));
    caps.dwSize = sizeof(DIDEVCAPS_DX3);
    EXPECT_FALSE(contains(describeCaps(caps), "ffSamplePeriod"));
}

// A data format by its size, guessed against the standard ones.
TEST(ShimFormat, DataFormatsGuessed) {
    EXPECT_EQ(describeDataFormat(nullptr), "null");
    DIDATAFORMAT format{};
    format.dwSize = sizeof(format);
    const auto describeSize = [&](const DWORD size, const DWORD flags = 0) {
        format.dwDataSize = size;
        format.dwFlags = flags;
        return describeDataFormat(&format);
    };
    EXPECT_TRUE(contains(describeSize(sizeof(DIJOYSTATE2)), "c_dfDIJoystick2"));
    EXPECT_TRUE(contains(describeSize(sizeof(DIJOYSTATE)), "looks like c_dfDIJoystick)"));
    EXPECT_TRUE(contains(describeSize(256), "c_dfDIKeyboard"));
    EXPECT_TRUE(contains(describeSize(sizeof(DIMOUSESTATE)), "c_dfDIMouse"));
    EXPECT_TRUE(contains(describeSize(sizeof(DIMOUSESTATE2)), "c_dfDIMouse"));
    EXPECT_TRUE(contains(describeSize(12), "custom"));
    EXPECT_TRUE(contains(describeSize(12, DIDF_RELAXIS), "RELAXIS"));
    EXPECT_TRUE(contains(describeSize(12, DIDF_ABSAXIS), "ABSAXIS"));
}

// A joystick state is decoded only for the two joystick sizes; buttons as a mask of the pressed ones.
TEST(ShimFormat, DeviceStates) {
    EXPECT_EQ(describeDeviceState(sizeof(DIJOYSTATE), nullptr), "80 bytes");
    DIJOYSTATE2 state{};
    EXPECT_EQ(describeDeviceState(5, &state), "5 bytes");
    state.lX = -5;
    state.rgbButtons[0] = 0x80;
    state.rgbButtons[31] = 0x80;
    state.rgbButtons[1] = 0x7F;   // only the high bit means pressed
    EXPECT_TRUE(contains(describeDeviceState(sizeof(DIJOYSTATE), &state), "x=-5"));
    EXPECT_TRUE(contains(describeDeviceState(sizeof(DIJOYSTATE2), &state), "buttons=80000001"));
}

// An effect: nothing past a size DirectInput does not know; each part only when its flag says the game filled it.
TEST(ShimFormat, EffectsByTheirFlags) {
    EXPECT_EQ(describeEffect(GUID_ConstantForce, nullptr, 0), "effect=null");
    DIEFFECT effect{};
    effect.dwSize = 12;
    EXPECT_EQ(describeEffect(GUID_ConstantForce, &effect, DIEP_ALLPARAMS), "invalid parameters (dwSize=12)");

    effect.dwSize = sizeof(DIEFFECT);
    effect.dwDuration = INFINITE;
    effect.dwGain = 5000;
    effect.dwSamplePeriod = 7;
    effect.dwTriggerButton = DIEB_NOTRIGGER;
    effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    DWORD axes[9] = {0, 4, 8, 12, 16, 20, 24, 28, 32};
    LONG directions[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    effect.cAxes = 9;   // more than any device: only 8 are read
    effect.rgdwAxes = axes;
    effect.rglDirection = directions;
    DIENVELOPE envelope{sizeof(DIENVELOPE), 100, 200, 300, 400};
    effect.lpEnvelope = &envelope;
    effect.dwStartDelay = 55;
    DICONSTANTFORCE constant{-1234};
    effect.cbTypeSpecificParams = sizeof(constant);
    effect.lpvTypeSpecificParams = &constant;
    std::string all = describeEffect(GUID_ConstantForce, &effect, DIEP_ALLPARAMS);
    EXPECT_TRUE(contains(all, "duration=INFINITE"));
    EXPECT_TRUE(contains(all, "gain=5000"));
    EXPECT_TRUE(contains(all, "samplePeriod=7"));
    EXPECT_TRUE(contains(all, "trigger=none"));
    EXPECT_TRUE(contains(all, "coords=CARTESIAN/OFFSETS axes=9"));
    EXPECT_TRUE(contains(all, "0x1c dir=8]"));
    EXPECT_FALSE(contains(all, "dir=9"));
    EXPECT_TRUE(contains(all, "envelope{attack=100/200us fade=300/400us}"));
    EXPECT_TRUE(contains(all, "startDelay=55us"));
    EXPECT_TRUE(contains(all, "constant{magnitude=-1234}"));

    effect.dwDuration = 1000;
    effect.dwTriggerButton = 3;
    effect.dwFlags = DIEFF_POLAR | DIEFF_OBJECTIDS;
    all = describeEffect(GUID_ConstantForce, &effect, DIEP_DURATION | DIEP_TRIGGERBUTTON | DIEP_DIRECTION);
    EXPECT_TRUE(contains(all, "duration=1000us"));
    EXPECT_TRUE(contains(all, "trigger=0x3"));
    EXPECT_TRUE(contains(all, "POLAR/IDS"));
    EXPECT_TRUE(contains(all, "0x0 dir=1"));   // the axes were not filled in: not read
    effect.dwFlags = DIEFF_SPHERICAL;
    EXPECT_TRUE(contains(describeEffect(GUID_ConstantForce, &effect, DIEP_AXES), "SPHERICAL/IDS axes=9 [0x0 dir=0"));

    effect.lpEnvelope = nullptr;   // flag set, no envelope: nothing to show
    EXPECT_FALSE(contains(describeEffect(GUID_ConstantForce, &effect, DIEP_ENVELOPE), "envelope"));
    effect.dwSize = sizeof(DIEFFECT_DX5);   // no start delay in a DirectX 5 effect
    EXPECT_FALSE(contains(describeEffect(GUID_ConstantForce, &effect, DIEP_STARTDELAY), "startDelay"));
}

// Type-specific parameters decoded by the effect's type when the block is big enough, never read when too large.
TEST(ShimFormat, TypeSpecificParameters) {
    DIEFFECT effect{};
    effect.dwSize = sizeof(DIEFFECT);
    const auto describeParams = [&](const GUID& type, void* params, const DWORD size) {
        effect.lpvTypeSpecificParams = params;
        effect.cbTypeSpecificParams = size;
        return describeEffect(type, &effect, DIEP_TYPESPECIFICPARAMS);
    };
    DIRAMPFORCE ramp{-10, 20};
    DIPERIODIC periodic{1000, -5, 9000, 20000};
    DICONDITION conditions[10] = {};
    conditions[1].lOffset = 77;
    DICUSTOMFORCE custom{1, 2000, 3, nullptr};
    std::uint8_t big[2048] = {};

    EXPECT_TRUE(contains(describeParams(GUID_ConstantForce, nullptr, 4), "params=none"));
    EXPECT_TRUE(contains(describeParams(GUID_ConstantForce, &ramp, 0), "params=none"));
    EXPECT_TRUE(contains(describeParams(GUID_ConstantForce, big, sizeof(big)), "too large, not read"));
    EXPECT_TRUE(contains(describeParams(GUID_RampForce, &ramp, sizeof(ramp)), "ramp{start=-10 end=20}"));
    for (const GUID* type : {&GUID_Square, &GUID_Sine, &GUID_Triangle, &GUID_SawtoothUp, &GUID_SawtoothDown}) {
        EXPECT_TRUE(contains(describeParams(*type, &periodic, sizeof(periodic)), "periodic{magnitude=1000"));
    }
    for (const GUID* type : {&GUID_Spring, &GUID_Damper, &GUID_Inertia, &GUID_Friction}) {
        const std::string text = describeParams(*type, conditions, sizeof(conditions));
        EXPECT_TRUE(contains(text, "condition[1]{offset=77"));
        EXPECT_TRUE(contains(text, "condition[7]"));
        EXPECT_FALSE(contains(text, "condition[8]"));   // never more conditions than axes
    }
    EXPECT_TRUE(contains(describeParams(GUID_CustomForce, &custom, sizeof(custom)), "custom{channels=1"));
    EXPECT_TRUE(contains(describeParams(GUID_RampForce, &ramp, 4), "4 bytes (not decoded)"));   // too short
    EXPECT_TRUE(contains(describeParams(GUID_Spring, &ramp, 4), "not decoded"));
    EXPECT_TRUE(contains(describeParams(GUID_Sine, &ramp, 4), "not decoded"));
    EXPECT_TRUE(contains(describeParams(GUID_CustomForce, &ramp, 4), "not decoded"));
    EXPECT_TRUE(contains(describeParams(GUID_ConstantForce, &ramp, 2), "not decoded"));
    EXPECT_TRUE(contains(describeParams(GUID_XAxis, &ramp, sizeof(ramp)), "not decoded"));
}

// Object GUIDs both ways (null: any object; unknown ones: Other / GUID_Unknown); a game's data format as entries.
TEST(ShimConvert, GuidsAndFormats) {
    EXPECT_EQ(objectGuidOf(nullptr), ObjectGuid::Any);
    EXPECT_EQ(objectGuidOf(&GUID_RzAxis), ObjectGuid::RzAxis);
    EXPECT_EQ(objectGuidOf(&GUID_POV), ObjectGuid::Pov);
    EXPECT_EQ(objectGuidOf(&GUID_ConstantForce), ObjectGuid::Other);
    for (const ObjectGuid guid : {ObjectGuid::XAxis, ObjectGuid::YAxis, ObjectGuid::ZAxis, ObjectGuid::RxAxis,
                                  ObjectGuid::RyAxis, ObjectGuid::RzAxis, ObjectGuid::Slider, ObjectGuid::Button,
                                  ObjectGuid::Key, ObjectGuid::Pov}) {
        EXPECT_EQ(objectGuidOf(&guidOf(guid)), guid);
    }
    EXPECT_TRUE(guidOf(ObjectGuid::Other) == GUID_Unknown);
    EXPECT_TRUE(guidOf(ObjectGuid::Any) == GUID_Unknown);

    DIOBJECTDATAFORMAT objects[2] = {{&GUID_XAxis, 0, DIDFT_AXIS | DIDFT_ANYINSTANCE, DIDOI_ASPECTPOSITION},
                                     {nullptr, 4, DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0}};
    DIDATAFORMAT format{sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, 8, 2, objects};
    const std::vector<FormatEntry> entries = toEntries(format);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].guid, ObjectGuid::XAxis);
    EXPECT_EQ(entries[1].guid, ObjectGuid::Any);
    EXPECT_EQ(entries[1].offset, 4U);
    format.dwNumObjs = 0;
    EXPECT_TRUE(toEntries(format).empty());

    Guid portable{0x11223344, 0x5566, 0x7788, {1, 2, 3, 4, 5, 6, 7, 8}};
    const GUID converted = toGuid(portable);
    EXPECT_EQ(converted.Data1, 0x11223344UL);
    EXPECT_EQ(converted.Data2, 0x5566);
    EXPECT_EQ(converted.Data4[7], 8);
}

// Text into DirectInput's fixed buffers: cut to fit, always terminated, bytes above 0x7F widened as they are.
TEST(ShimConvert, CopyTextAlwaysTerminates) {
    CHAR narrow[4];
    copyText(narrow, "abcdef");
    EXPECT_STREQ(narrow, "abc");
    copyText(narrow, "");
    EXPECT_STREQ(narrow, "");
    WCHAR wide[4];
    copyText(wide, "abcdef");
    EXPECT_EQ(std::wstring(wide), L"abc");
    copyText(wide, "\xe9");
    EXPECT_EQ(wide[0], static_cast<WCHAR>(0xE9));
    EXPECT_EQ(propertyId(DIPROP_RANGE), 4U);
    EXPECT_EQ(propertyId(&DIPROP_RANGE), 4U);
}

// HID product GUIDs carry vid / pid; anything else does not. presentAs rewrites only the DirectX 3 fields.
TEST(ShimConvert, VidPidAndPresentAs) {
    GUID product{};
    product.Data1 = 0x0005346E;
    const BYTE pidvid[8] = {0, 0, 'P', 'I', 'D', 'V', 'I', 'D'};
    std::memcpy(product.Data4, pidvid, 8);
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
    ASSERT_TRUE(readVidPid(product, vid, pid));
    EXPECT_EQ(vid, 0x346E);
    EXPECT_EQ(pid, 0x0005);
    GUID other = product;
    other.Data2 = 1;
    EXPECT_FALSE(readVidPid(other, vid, pid));
    other = product;
    other.Data3 = 1;
    EXPECT_FALSE(readVidPid(other, vid, pid));
    other = product;
    other.Data4[2] = 'X';
    EXPECT_FALSE(readVidPid(other, vid, pid));

    DIDEVICEINSTANCEW instance{};
    instance.dwSize = sizeof(instance);
    instance.guidProduct = product;
    instance.dwDevType = DI8DEVTYPE_JOYSTICK | DIDEVTYPE_HID;
    const auto profile = makeProfile("g920");
    ASSERT_NE(profile, nullptr);
    presentAs(instance, profile->identity());
    EXPECT_EQ(GET_DIDEVICE_TYPE(instance.dwDevType), static_cast<DWORD>(DI8DEVTYPE_DRIVING));
    EXPECT_NE(instance.dwDevType & DIDEVTYPE_HID, 0U);
    ASSERT_TRUE(readVidPid(instance.guidProduct, vid, pid));
    EXPECT_EQ(vid, profile->identity().vendorId);
    EXPECT_EQ(toUtf8(instance.tszProductName), profile->identity().productName);
    instance.dwDevType = DI8DEVTYPE_JOYSTICK;   // not HID: stays not HID
    presentAs(instance, profile->identity());
    EXPECT_EQ(instance.dwDevType & DIDEVTYPE_HID, 0U);
}

}  // namespace
}  // namespace wheelbri::shim
