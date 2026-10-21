// The dinput8.dll shim's force feedback: FfbForwarder (slots, tickets, timing, what it sends to wheelbri-service) and
// EmulatedEffect (a game's DirectInput effect: its parameters checked, kept and turned into FfbEffects), seen through
// the FfbPackets it sends on 127.0.0.1. Windows only (MinGW: run under Wine).
//
// The shim reads wheelbri-shim.ini next to its module, here the test program: ShimTestEnvironment writes one there
// before any test runs, with ports of the tests' own (the real ones may be in use by a running WheelBri).

#include "ShimTestSupport.hpp"

#include "EmulatedEffect.hpp"
#include "FfbForwarder.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <optional>
#include <thread>
#include <vector>

namespace wheelbri::shim {
namespace {

using test::FfbCatcher;

// One device's worth of effects, freed again after each test (the forwarder is one per process).
class ShimForwarder : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(catcher_.ok());
        catcher_.drain();
    }
    void TearDown() override {
        FfbForwarder::instance().reset(&owner_);
        FfbForwarder::instance().reset(&other_);
        FfbForwarder::instance().setDeviceGain(1.0F);
    }

    FfbForwarder& forwarder() { return FfbForwarder::instance(); }

    FfbCatcher catcher_;
    int owner_ = 0;
    int other_ = 0;
};

FfbEffect constant(const float level) {
    FfbEffect effect;
    effect.constantLevel = level;
    return effect;
}

// Eight slots: the ninth effect is refused; a freed slot is handed out again with a new ticket, and the old ticket
// no longer holds it (it can neither stop nor change the new effect).
TEST_F(ShimForwarder, SlotsAndTickets) {
    std::vector<FfbForwarder::Allocation> taken;
    for (std::size_t i = 0; i < IFfbSink::kMaxSlots; ++i) {
        const auto a = forwarder().allocate(&owner_);
        ASSERT_TRUE(a);
        EXPECT_TRUE(forwarder().holds(*a));
        taken.push_back(*a);
    }
    EXPECT_FALSE(forwarder().allocate(&owner_));
    EXPECT_FALSE(forwarder().allocate(&owner_));   // and again (the warning is rate limited)

    const FfbForwarder::Allocation old = taken[3];
    forwarder().release(old);
    EXPECT_FALSE(forwarder().holds(old));
    forwarder().release(old);   // twice: nothing
    const auto fresh = forwarder().allocate(&other_);
    ASSERT_TRUE(fresh);
    EXPECT_EQ(fresh->slot, old.slot);
    EXPECT_NE(fresh->ticket, old.ticket);
    forwarder().start(*fresh, std::nullopt, std::nullopt, 0, false);
    forwarder().stop(old);                        // stale: ignored
    forwarder().setEffect(old, constant(0.9F));   // stale: ignored
    forwarder().start(old, std::nullopt, std::nullopt, 0, false);
    EXPECT_TRUE(forwarder().isPlaying(*fresh));
    EXPECT_FALSE(forwarder().isPlaying(old));
    EXPECT_FALSE(forwarder().holds(FfbForwarder::Allocation{200, 1}));   // a slot that does not exist
}

// Start sends "playing" with the effect, Stop "stopped"; a change while stopped is kept but not sent.
TEST_F(ShimForwarder, StartAndStopAreSent) {
    const auto a = forwarder().allocate(&owner_);
    ASSERT_TRUE(a);
    forwarder().setEffect(*a, constant(0.5F));
    EXPECT_FALSE(catcher_.next(FfbPacket::Kind::SlotState, 20));   // stopped: nothing to tell
    forwarder().start(*a, std::nullopt, std::nullopt, 0, false);
    auto packet = catcher_.next(FfbPacket::Kind::SlotState);
    ASSERT_TRUE(packet);
    EXPECT_EQ(packet->slot, a->slot);
    EXPECT_TRUE(packet->playing);
    EXPECT_FLOAT_EQ(packet->effect.constantLevel, 0.5F);
    forwarder().setEffect(*a, constant(0.25F));   // playing: sent at once
    packet = catcher_.next(FfbPacket::Kind::SlotState);
    ASSERT_TRUE(packet);
    EXPECT_FLOAT_EQ(packet->effect.constantLevel, 0.25F);
    forwarder().setEffect(*a, constant(0.25F));   // the same: nothing new
    EXPECT_FALSE(catcher_.next(FfbPacket::Kind::SlotState, 20));
    forwarder().stop(*a);
    packet = catcher_.next(FfbPacket::Kind::SlotState);
    ASSERT_TRUE(packet);
    EXPECT_FALSE(packet->playing);
    EXPECT_FALSE(forwarder().isPlaying(*a));
}

// Timing: a start delay holds it back; a duration times iterations ends it; without iterations it plays on.
TEST_F(ShimForwarder, DelayDurationAndIterations) {
    const auto a = forwarder().allocate(&owner_);
    ASSERT_TRUE(a);
    forwarder().start(*a, std::nullopt, std::nullopt, 60'000'000, false);   // a minute's delay
    EXPECT_FALSE(forwarder().isPlaying(*a));
    forwarder().start(*a, 1, 1, 0, false);   // 1 us, once: over by the time it is asked
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_FALSE(forwarder().isPlaying(*a));
    forwarder().start(*a, 1, std::nullopt, 0, false);   // 1 us, no count: until stopped
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_TRUE(forwarder().isPlaying(*a));
    forwarder().start(*a, std::nullopt, 3, 0, false);   // no duration: until stopped too
    EXPECT_TRUE(forwarder().isPlaying(*a));
}

// Solo stops the owner's other effects, not another device's.
TEST_F(ShimForwarder, SoloStopsOnlyTheOwnersOthers) {
    const auto mine = forwarder().allocate(&owner_);
    const auto mine2 = forwarder().allocate(&owner_);
    const auto theirs = forwarder().allocate(&other_);
    ASSERT_TRUE(mine && mine2 && theirs);
    forwarder().start(*mine, std::nullopt, std::nullopt, 0, false);
    forwarder().start(*theirs, std::nullopt, std::nullopt, 0, false);
    forwarder().start(*mine2, std::nullopt, std::nullopt, 0, true);
    EXPECT_FALSE(forwarder().isPlaying(*mine));
    EXPECT_TRUE(forwarder().isPlaying(*mine2));
    EXPECT_TRUE(forwarder().isPlaying(*theirs));
}

// StopAll for one device stops its effects; the service's StopAll goes out only when no other device has one started.
TEST_F(ShimForwarder, StopAllRespectsOtherDevices) {
    const auto mine = forwarder().allocate(&owner_);
    const auto theirs = forwarder().allocate(&other_);
    ASSERT_TRUE(mine && theirs);
    forwarder().start(*mine, std::nullopt, std::nullopt, 0, false);
    forwarder().start(*theirs, std::nullopt, std::nullopt, 0, false);
    catcher_.drain();
    forwarder().stopAll(&owner_);
    EXPECT_FALSE(forwarder().isPlaying(*mine));
    EXPECT_TRUE(forwarder().isPlaying(*theirs));
    EXPECT_FALSE(catcher_.next(FfbPacket::Kind::StopAll, 20));
    forwarder().stop(*theirs);
    forwarder().stopAll(&owner_);
    EXPECT_TRUE(catcher_.next(FfbPacket::Kind::StopAll));
    EXPECT_TRUE(forwarder().holds(*mine));   // stopped, still loaded
}

// Reset frees the owner's slots (effects unloaded) and ends a pause and actuators off.
TEST_F(ShimForwarder, ResetFreesTheOwnersSlots) {
    const auto mine = forwarder().allocate(&owner_);
    const auto theirs = forwarder().allocate(&other_);
    ASSERT_TRUE(mine && theirs);
    forwarder().setPaused(true);
    forwarder().setActuators(false);
    forwarder().reset(&owner_);
    EXPECT_FALSE(forwarder().holds(*mine));
    EXPECT_TRUE(forwarder().holds(*theirs));
    const std::uint32_t flags = forwarder().stateFlags();
    EXPECT_EQ(flags & DIGFFS_PAUSED, 0U);
    EXPECT_NE(flags & DIGFFS_ACTUATORSON, 0U);
}

// The device state flags: empty, stopped, paused, actuators off; a pause or actuators off silences a started effect.
TEST_F(ShimForwarder, StateFlags) {
    forwarder().reset(&owner_);
    EXPECT_NE(forwarder().stateFlags() & DIGFFS_STOPPED, 0U);
    const auto a = forwarder().allocate(&owner_);
    ASSERT_TRUE(a);
    EXPECT_EQ(forwarder().stateFlags() & DIGFFS_EMPTY, 0U);
    forwarder().start(*a, std::nullopt, std::nullopt, 0, false);
    EXPECT_EQ(forwarder().stateFlags() & DIGFFS_STOPPED, 0U);
    forwarder().setPaused(true);
    EXPECT_FALSE(forwarder().isPlaying(*a));
    EXPECT_NE(forwarder().stateFlags() & DIGFFS_PAUSED, 0U);
    forwarder().setPaused(false);
    forwarder().setActuators(false);
    EXPECT_FALSE(forwarder().isPlaying(*a));
    EXPECT_NE(forwarder().stateFlags() & DIGFFS_ACTUATORSOFF, 0U);
    forwarder().setActuators(true);
    EXPECT_TRUE(forwarder().isPlaying(*a));
}

// The device gain scales every effect's gain, kept within 0..1.
TEST_F(ShimForwarder, DeviceGain) {
    const auto a = forwarder().allocate(&owner_);
    ASSERT_TRUE(a);
    forwarder().start(*a, std::nullopt, std::nullopt, 0, false);
    catcher_.drain();
    forwarder().setDeviceGain(0.5F);
    EXPECT_FLOAT_EQ(forwarder().deviceGain(), 0.5F);
    const auto packet = catcher_.next(FfbPacket::Kind::SlotState);
    ASSERT_TRUE(packet);
    EXPECT_FLOAT_EQ(packet->effect.gain, 0.5F);
    forwarder().setDeviceGain(-1.0F);
    EXPECT_FLOAT_EQ(forwarder().deviceGain(), 0.0F);
    forwarder().setDeviceGain(7.0F);
    EXPECT_FLOAT_EQ(forwarder().deviceGain(), 1.0F);
}

// tick(): every 250 ms every slot is sent again, free ones too (a lost stop heals), and keep-alives keep coming.
// Timed from the refresh, so the second tick falls between the keep-alive (100 ms) and the next refresh (250 ms)
// however long the receiving took (Windows sleeps in steps of up to 15.6 ms).
TEST_F(ShimForwarder, TickRefreshesEverySlotAndKeepsAlive) {
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    catcher_.drain();
    forwarder().tick();
    const auto refreshed = std::chrono::steady_clock::now();
    std::size_t slots = 0;
    while (catcher_.next(FfbPacket::Kind::SlotState, 50)) ++slots;
    EXPECT_EQ(slots, IFfbSink::kMaxSlots);
    std::this_thread::sleep_until(refreshed + std::chrono::milliseconds(130));
    catcher_.drain();
    ASSERT_LT(std::chrono::steady_clock::now() - refreshed, std::chrono::milliseconds(240)) << "too slow to tell";
    forwarder().tick();   // no refresh yet, but quiet for 100 ms: a keep-alive
    EXPECT_TRUE(catcher_.next(FfbPacket::Kind::KeepAlive));
}

// An envelope or a ramp changes the level as the effect plays: tick() sends each moment's level.
TEST_F(ShimForwarder, ShapesPlayOverTime) {
    const auto a = forwarder().allocate(&owner_);
    ASSERT_TRUE(a);
    FfbShape ramp;
    ramp.rampEndLevel = 1.0F;
    forwarder().setEffect(*a, constant(0.0F), ramp);
    forwarder().start(*a, 200'000, 1, 0, false);   // 0.2 s from 0 to 1
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    catcher_.drain();
    forwarder().tick();
    std::optional<FfbPacket> packet;
    while (const auto next = catcher_.next(FfbPacket::Kind::SlotState, 20)) {
        if (next->slot == a->slot) packet = next;
    }
    ASSERT_TRUE(packet);
    EXPECT_GT(packet->effect.constantLevel, 0.2F);
    EXPECT_LT(packet->effect.constantLevel, 0.95F);

    FfbShape envelope;
    envelope.envelope = FfbEnvelope{};
    forwarder().setEffect(*a, constant(0.5F), envelope);
    forwarder().start(*a, std::nullopt, std::nullopt, 10'000'000, false);   // delayed: the shape waits too
    forwarder().tick();
    EXPECT_FALSE(forwarder().isPlaying(*a));
}

// --- EmulatedEffect --------------------------------------------------------------------------------------------------

// An effect as CreateEffect makes it (a slot of its own), released after each test.
class ShimEffect : public ShimForwarder {
protected:
    EmulatedEffect* make(const GUID& type, const DWORD wheelOffset = 0, const DWORD wheelId = DIDFT_ABSAXIS) {
        const auto effectType = EmulatedEffect::typeFor(type);
        EXPECT_TRUE(effectType);
        const auto slot = forwarder().allocate(&owner_);
        EXPECT_TRUE(slot);
        return new EmulatedEffect(type, *effectType, *slot, &owner_, "[test]", wheelOffset, wheelId);
    }

    static DIEFFECT effectWith(void* params, const DWORD size) {
        DIEFFECT effect{};
        effect.dwSize = sizeof(DIEFFECT);
        effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
        effect.dwDuration = INFINITE;
        effect.dwGain = DI_FFNOMINALMAX;
        effect.cbTypeSpecificParams = size;
        effect.lpvTypeSpecificParams = params;
        return effect;
    }

    // The level the forwarder sends for `e` when it is started now.
    std::optional<FfbPacket> startAndCatch(EmulatedEffect* e) {
        catcher_.drain();
        e->Stop();
        e->Start(1, 0);
        return catcher_.next(FfbPacket::Kind::SlotState);
    }
};

// The types DirectInput effects map to; anything else is not supported.
TEST(ShimEffectTypes, EveryDirectInputType) {
    EXPECT_EQ(EmulatedEffect::typeFor(GUID_ConstantForce), FfbEffectType::Constant);
    EXPECT_EQ(EmulatedEffect::typeFor(GUID_RampForce), FfbEffectType::Constant);
    EXPECT_EQ(EmulatedEffect::typeFor(GUID_Spring), FfbEffectType::Spring);
    EXPECT_EQ(EmulatedEffect::typeFor(GUID_Damper), FfbEffectType::Damper);
    EXPECT_EQ(EmulatedEffect::typeFor(GUID_Friction), FfbEffectType::Friction);
    EXPECT_EQ(EmulatedEffect::typeFor(GUID_Inertia), FfbEffectType::Inertia);
    EXPECT_EQ(EmulatedEffect::typeFor(GUID_Sine), FfbEffectType::Sine);
    EXPECT_EQ(EmulatedEffect::typeFor(GUID_Square), FfbEffectType::Square);
    EXPECT_EQ(EmulatedEffect::typeFor(GUID_Triangle), FfbEffectType::Triangle);
    EXPECT_EQ(EmulatedEffect::typeFor(GUID_SawtoothUp), FfbEffectType::SawtoothUp);
    EXPECT_EQ(EmulatedEffect::typeFor(GUID_SawtoothDown), FfbEffectType::SawtoothDown);
    EXPECT_FALSE(EmulatedEffect::typeFor(GUID_CustomForce));
}

// COM basics: its own interfaces, a reference count, and its slot given back at the last Release.
TEST_F(ShimEffect, ComBasics) {
    EmulatedEffect* e = make(GUID_ConstantForce);
    EXPECT_EQ(e->QueryInterface(IID_IUnknown, nullptr), static_cast<HRESULT>(E_POINTER));
    void* out = nullptr;
    EXPECT_EQ(e->QueryInterface(IID_IDirectInputEffect, &out), static_cast<HRESULT>(S_OK));
    EXPECT_EQ(out, e);
    EXPECT_EQ(e->QueryInterface(IID_IUnknown, &out), static_cast<HRESULT>(S_OK));
    EXPECT_EQ(e->QueryInterface(IID_IDirectInputDevice8W, &out), static_cast<HRESULT>(E_NOINTERFACE));
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(e->Release(), 2U);
    EXPECT_EQ(e->Release(), 1U);
    EXPECT_EQ(e->Initialize(nullptr, 0x0800, GUID_ConstantForce), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(e->GetEffectGuid(nullptr), static_cast<HRESULT>(E_POINTER));
    GUID guid{};
    EXPECT_EQ(e->GetEffectGuid(&guid), static_cast<HRESULT>(DI_OK));
    EXPECT_TRUE(guid == GUID_ConstantForce);
    EXPECT_EQ(e->Escape(nullptr), static_cast<HRESULT>(DIERR_UNSUPPORTED));
    EXPECT_EQ(e->Release(), 0U);
    // Its slot is free again: all eight can be taken.
    for (std::size_t i = 0; i < IFfbSink::kMaxSlots; ++i) EXPECT_TRUE(forwarder().allocate(&owner_));
}

// What a call may not pass: no effect, an unknown size, too many axes, missing arrays, an oversized block; a
// refused call changes nothing.
TEST_F(ShimEffect, RefusesWhatItCannotRead) {
    EmulatedEffect* e = make(GUID_ConstantForce);
    DICONSTANTFORCE force{5000};
    DIEFFECT effect = effectWith(&force, sizeof(force));
    EXPECT_EQ(e->SetParameters(nullptr, DIEP_ALLPARAMS), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    effect.dwSize = 8;
    EXPECT_EQ(e->SetParameters(&effect, DIEP_ALLPARAMS), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    effect.dwSize = sizeof(DIEFFECT);
    effect.cAxes = 9;
    DWORD axes[9] = {};
    LONG directions[9] = {};
    effect.rgdwAxes = axes;
    effect.rglDirection = directions;
    EXPECT_EQ(e->SetParameters(&effect, DIEP_AXES), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    EXPECT_EQ(e->SetParameters(&effect, DIEP_DIRECTION), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    effect.cAxes = 1;
    effect.rgdwAxes = nullptr;
    EXPECT_EQ(e->SetParameters(&effect, DIEP_AXES), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    effect.rglDirection = nullptr;
    EXPECT_EQ(e->SetParameters(&effect, DIEP_DIRECTION), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    effect.cbTypeSpecificParams = 2000;
    EXPECT_EQ(e->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    effect.cbTypeSpecificParams = 4;
    effect.lpvTypeSpecificParams = nullptr;
    EXPECT_EQ(e->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    // Nothing of those calls was kept.
    DIEFFECT got{};
    got.dwSize = sizeof(got);
    got.cbTypeSpecificParams = 0;
    EXPECT_EQ(e->GetParameters(&got, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(got.cbTypeSpecificParams, 0U);
    EXPECT_EQ(e->GetParameters(nullptr, DIEP_ALLPARAMS), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    got.dwSize = 3;
    EXPECT_EQ(e->GetParameters(&got, DIEP_ALLPARAMS), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    e->Release();
}

// Everything the game sets comes back from GetParameters as it set it; the gain is capped at the nominal maximum and a
// DirectX 5 effect has no start delay.
TEST_F(ShimEffect, ParametersComeBackAsSet) {
    EmulatedEffect* e = make(GUID_ConstantForce);
    DICONSTANTFORCE force{-2500};
    DIEFFECT effect = effectWith(&force, sizeof(force));
    DWORD axes[2] = {0, 4};
    LONG directions[2] = {1, 0};
    DIENVELOPE envelope{sizeof(DIENVELOPE), 1000, 20000, 0, 30000};
    effect.cAxes = 2;
    effect.rgdwAxes = axes;
    effect.rglDirection = directions;
    effect.lpEnvelope = &envelope;
    effect.dwDuration = 5000;
    effect.dwSamplePeriod = 3;
    effect.dwGain = 20000;
    effect.dwTriggerButton = 7;
    effect.dwTriggerRepeatInterval = 9;
    effect.dwStartDelay = 11;
    ASSERT_EQ(e->SetParameters(&effect, DIEP_ALLPARAMS), static_cast<HRESULT>(DI_OK));

    DIEFFECT got{};
    got.dwSize = sizeof(got);
    DWORD gotAxes[2] = {};
    LONG gotDirections[2] = {};
    DIENVELOPE gotEnvelope{};
    DICONSTANTFORCE gotForce{};
    got.cAxes = 1;   // too few: says how many
    got.rgdwAxes = gotAxes;
    got.rglDirection = gotDirections;
    got.lpEnvelope = &gotEnvelope;
    got.lpvTypeSpecificParams = &gotForce;
    got.cbTypeSpecificParams = 2;   // too small: says how big
    EXPECT_EQ(e->GetParameters(&got, DIEP_AXES), static_cast<HRESULT>(DIERR_MOREDATA));
    EXPECT_EQ(got.cAxes, 2U);
    EXPECT_EQ(e->GetParameters(&got, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DIERR_MOREDATA));
    EXPECT_EQ(got.cbTypeSpecificParams, sizeof(DICONSTANTFORCE));
    ASSERT_EQ(e->GetParameters(&got, DIEP_ALLPARAMS), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(got.dwDuration, 5000U);
    EXPECT_EQ(got.dwSamplePeriod, 3U);
    EXPECT_EQ(got.dwGain, static_cast<DWORD>(DI_FFNOMINALMAX));
    EXPECT_EQ(got.dwTriggerButton, 7U);
    EXPECT_EQ(got.dwTriggerRepeatInterval, 9U);
    EXPECT_EQ(got.dwStartDelay, 11U);
    EXPECT_EQ(gotAxes[1], 4U);
    EXPECT_EQ(gotDirections[0], 1);
    EXPECT_NE(got.dwFlags & DIEFF_CARTESIAN, 0U);
    EXPECT_EQ(gotEnvelope.dwAttackTime, 20000U);
    EXPECT_EQ(gotForce.lMagnitude, -2500);

    // Arrays not given: their parts are skipped; no envelope set: the pointer comes back null.
    got.rgdwAxes = nullptr;
    got.rglDirection = nullptr;
    got.lpvTypeSpecificParams = nullptr;
    got.cbTypeSpecificParams = 100;
    EXPECT_EQ(e->GetParameters(&got, DIEP_AXES | DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DI_OK));
    effect.lpEnvelope = nullptr;
    ASSERT_EQ(e->SetParameters(&effect, DIEP_ENVELOPE), static_cast<HRESULT>(DI_OK));
    got.lpEnvelope = &gotEnvelope;
    EXPECT_EQ(e->GetParameters(&got, DIEP_ENVELOPE), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(got.lpEnvelope, nullptr);

    // A DirectX 5 effect: the start delay is neither taken nor written.
    effect.dwSize = sizeof(DIEFFECT_DX5);
    effect.dwStartDelay = 99;
    ASSERT_EQ(e->SetParameters(&effect, DIEP_STARTDELAY), static_cast<HRESULT>(DI_OK));
    got.dwSize = sizeof(DIEFFECT_DX5);
    got.dwStartDelay = 1234;
    EXPECT_EQ(e->GetParameters(&got, DIEP_STARTDELAY), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(got.dwStartDelay, 1234U);
    got.dwSize = sizeof(DIEFFECT);
    EXPECT_EQ(e->GetParameters(&got, DIEP_STARTDELAY), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(got.dwStartDelay, 11U);
    e->Release();
}

// Directions given without new axes count the axes set before; new axes set in the same call count instead, so a
// call that sets no axes needs no directions (it was refused before, checked against the old axes).
TEST_F(ShimEffect, DirectionsCountTheAxesOfTheSameCall) {
    EmulatedEffect* e = make(GUID_ConstantForce);
    DWORD axes[2] = {0, 4};
    LONG directions[2] = {1, 1};
    DIEFFECT effect = effectWith(nullptr, 0);
    effect.cAxes = 2;
    effect.rgdwAxes = axes;
    effect.rglDirection = directions;
    ASSERT_EQ(e->SetParameters(&effect, DIEP_AXES | DIEP_DIRECTION), static_cast<HRESULT>(DI_OK));
    // cAxes 0: the two axes set before.
    effect.cAxes = 0;
    effect.rglDirection = nullptr;
    EXPECT_EQ(e->SetParameters(&effect, DIEP_DIRECTION), static_cast<HRESULT>(DIERR_INVALIDPARAM));
    effect.rglDirection = directions;
    EXPECT_EQ(e->SetParameters(&effect, DIEP_DIRECTION), static_cast<HRESULT>(DI_OK));
    // No axes any more, set in this call: no directions needed.
    effect.rglDirection = nullptr;
    EXPECT_EQ(e->SetParameters(&effect, DIEP_AXES | DIEP_DIRECTION), static_cast<HRESULT>(DI_OK));
    DIEFFECT got{};
    got.dwSize = sizeof(got);
    EXPECT_EQ(e->GetParameters(&got, DIEP_AXES | DIEP_DIRECTION), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(got.cAxes, 0U);
    e->Release();
}

// The level along the wheel: a cartesian direction's share on the wheel axis (found by offset or by object id,
// whose flag bits do not count), a polar one's sine, and full when no direction was given.
TEST_F(ShimEffect, DirectionAlongTheWheel) {
    EmulatedEffect* e = make(GUID_ConstantForce, 8, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0));
    DICONSTANTFORCE force{10000};
    DIEFFECT effect = effectWith(&force, sizeof(force));
    ASSERT_EQ(e->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DI_OK));
    auto packet = startAndCatch(e);
    ASSERT_TRUE(packet);
    EXPECT_NEAR(std::fabs(packet->effect.constantLevel), 1.0F, 1e-3F);   // no direction: all of it

    DWORD axes[2] = {4, 8};   // the wheel is the second, by offset
    LONG directions[2] = {3, 4};
    effect.cAxes = 2;
    effect.rgdwAxes = axes;
    effect.rglDirection = directions;
    ASSERT_EQ(e->SetParameters(&effect, DIEP_AXES | DIEP_DIRECTION), static_cast<HRESULT>(DI_OK));
    packet = startAndCatch(e);
    ASSERT_TRUE(packet);
    EXPECT_NEAR(std::fabs(packet->effect.constantLevel), 0.8F, 1e-3F);

    DWORD ids[2] = {DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0) | DIDFT_FFACTUATOR, 99};   // the wheel first, by id
    effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTIDS;
    effect.rgdwAxes = ids;
    ASSERT_EQ(e->SetParameters(&effect, DIEP_AXES | DIEP_DIRECTION), static_cast<HRESULT>(DI_OK));
    packet = startAndCatch(e);
    ASSERT_TRUE(packet);
    EXPECT_NEAR(std::fabs(packet->effect.constantLevel), 0.6F, 1e-3F);

    LONG none[2] = {0, 0};   // a zero vector: full
    effect.rglDirection = none;
    ASSERT_EQ(e->SetParameters(&effect, DIEP_DIRECTION), static_cast<HRESULT>(DI_OK));
    packet = startAndCatch(e);
    ASSERT_TRUE(packet);
    EXPECT_NEAR(std::fabs(packet->effect.constantLevel), 1.0F, 1e-3F);

    DWORD missing[2] = {40, 44};   // the wheel not named: the first axis
    LONG first[2] = {0, 1};
    effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
    effect.rgdwAxes = missing;
    effect.rglDirection = first;
    ASSERT_EQ(e->SetParameters(&effect, DIEP_AXES | DIEP_DIRECTION), static_cast<HRESULT>(DI_OK));
    packet = startAndCatch(e);
    ASSERT_TRUE(packet);
    EXPECT_NEAR(packet->effect.constantLevel, 0.0F, 1e-3F);

    LONG polar[1] = {9000};   // polar: 90 degrees is all the way along x
    effect.dwFlags = DIEFF_POLAR | DIEFF_OBJECTOFFSETS;
    effect.cAxes = 1;
    effect.rglDirection = polar;
    ASSERT_EQ(e->SetParameters(&effect, DIEP_AXES | DIEP_DIRECTION), static_cast<HRESULT>(DI_OK));
    packet = startAndCatch(e);
    ASSERT_TRUE(packet);
    EXPECT_NEAR(std::fabs(packet->effect.constantLevel), 1.0F, 1e-3F);
    e->Release();
}

// Each kind of effect becomes its FfbEffect: conditions per axis (the wheel's, else the one for all), periodic
// effects, ramps from start to end, an envelope (not for conditions).
TEST_F(ShimEffect, EveryKindConverted) {
    EmulatedEffect* spring = make(GUID_Spring, 4);
    DICONDITION conditions[2] = {};
    conditions[0].lPositiveCoefficient = 1000;
    conditions[1].lPositiveCoefficient = 8000;   // the wheel's (second axis)
    DIEFFECT effect = effectWith(conditions, sizeof(conditions));
    DWORD axes[2] = {0, 4};
    LONG directions[2] = {0, 0};
    effect.cAxes = 2;
    effect.rgdwAxes = axes;
    effect.rglDirection = directions;
    DIENVELOPE envelope{sizeof(DIENVELOPE), 0, 1000, 0, 1000};
    effect.lpEnvelope = &envelope;
    ASSERT_EQ(spring->SetParameters(&effect, DIEP_ALLPARAMS), static_cast<HRESULT>(DI_OK));
    auto packet = startAndCatch(spring);
    ASSERT_TRUE(packet);
    EXPECT_EQ(packet->effect.type, FfbEffectType::Spring);
    EXPECT_NEAR(packet->effect.condition.positiveCoefficient, 0.8F, 1e-3F);
    effect.cbTypeSpecificParams = sizeof(DICONDITION);   // one for all axes
    ASSERT_EQ(spring->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DI_OK));
    packet = startAndCatch(spring);
    ASSERT_TRUE(packet);
    EXPECT_NEAR(packet->effect.condition.positiveCoefficient, 0.1F, 1e-3F);
    spring->Release();

    EmulatedEffect* sine = make(GUID_Sine);
    DIPERIODIC periodic{5000, 0, 0, 100000};
    effect = effectWith(&periodic, sizeof(periodic));
    effect.lpEnvelope = &envelope;
    ASSERT_EQ(sine->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS | DIEP_ENVELOPE), static_cast<HRESULT>(DI_OK));
    packet = startAndCatch(sine);
    ASSERT_TRUE(packet);
    EXPECT_EQ(packet->effect.type, FfbEffectType::Sine);
    EXPECT_NEAR(packet->effect.periodic.periodMs, 100.0F, 1e-3F);
    sine->Release();

    EmulatedEffect* ramp = make(GUID_RampForce);
    DIRAMPFORCE levels{-10000, 10000};
    effect = effectWith(&levels, sizeof(levels));
    ASSERT_EQ(ramp->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DI_OK));
    packet = startAndCatch(ramp);
    ASSERT_TRUE(packet);
    EXPECT_NEAR(packet->effect.constantLevel, 0.0F, 0.05F);   // no duration: its mean level
    ramp->Release();

    EmulatedEffect* short_ = make(GUID_ConstantForce);   // a block too short for its type: zeros
    std::uint8_t two[2] = {1, 2};
    effect = effectWith(two, sizeof(two));
    ASSERT_EQ(short_->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DI_OK));
    packet = startAndCatch(short_);
    ASSERT_TRUE(packet);
    EXPECT_FLOAT_EQ(packet->effect.constantLevel, 0.0F);
    short_->Release();
}

// Slots: Unload frees one (the object stays), Download / Start / SetParameters take one again; with every slot taken
// they say the device is full; a device reset unloads it too. NODOWNLOAD sets without taking a slot.
TEST_F(ShimEffect, SlotsComeAndGo) {
    EmulatedEffect* e = make(GUID_ConstantForce);
    EXPECT_EQ(e->Unload(), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(e->Unload(), static_cast<HRESULT>(DI_OK));   // twice: nothing
    DWORD status = 99;
    EXPECT_EQ(e->GetEffectStatus(nullptr), static_cast<HRESULT>(E_POINTER));
    EXPECT_EQ(e->GetEffectStatus(&status), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(status, 0U);
    EXPECT_EQ(e->Stop(), static_cast<HRESULT>(DI_OK));   // no slot: nothing to stop

    std::vector<FfbForwarder::Allocation> others;
    while (const auto a = forwarder().allocate(&other_)) others.push_back(*a);
    DICONSTANTFORCE force{100};
    DIEFFECT effect = effectWith(&force, sizeof(force));
    EXPECT_EQ(e->Download(), static_cast<HRESULT>(DIERR_DEVICEFULL));
    EXPECT_EQ(e->Start(1, 0), static_cast<HRESULT>(DIERR_DEVICEFULL));
    EXPECT_EQ(e->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DIERR_DEVICEFULL));
    EXPECT_EQ(e->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS | DIEP_NODOWNLOAD), static_cast<HRESULT>(DI_OK));
    forwarder().release(others.back());

    EXPECT_EQ(e->Download(), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(e->Download(), static_cast<HRESULT>(DI_OK));   // already
    EXPECT_EQ(e->Start(INFINITE, DIES_SOLO), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(e->GetEffectStatus(&status), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(status, static_cast<DWORD>(DIEGES_PLAYING));
    forwarder().reset(&owner_);   // the device's reset: the effect lost its slot
    EXPECT_EQ(e->GetEffectStatus(&status), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(status, 0U);
    EXPECT_EQ(e->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS | DIEP_START), static_cast<HRESULT>(DI_OK));   // downloaded, started
    EXPECT_EQ(e->GetEffectStatus(&status), static_cast<HRESULT>(DI_OK));
    EXPECT_EQ(status, static_cast<DWORD>(DIEGES_PLAYING));
    EXPECT_EQ(e->Start(0, 0), static_cast<HRESULT>(DI_OK));   // 0 iterations: once
    EXPECT_EQ(e->Stop(), static_cast<HRESULT>(DI_OK));
    e->Release();
}

// A constant force's magnitude as the game passed it is logged when it changes (300 times, then no more); a value
// far outside -10000..10000 is warned about once and plays no force.
TEST_F(ShimEffect, RawConstantsLoggedAndGarbageRefused) {
    EmulatedEffect* e = make(GUID_ConstantForce);
    DICONSTANTFORCE force{0};
    DIEFFECT effect = effectWith(&force, sizeof(force));
    for (int i = 0; i < 305; ++i) {
        force.lMagnitude = i;
        ASSERT_EQ(e->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DI_OK));
        ASSERT_EQ(e->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DI_OK));   // the same: not a change
    }
    force.lMagnitude = 1'000'000'000;
    ASSERT_EQ(e->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DI_OK));
    force.lMagnitude = -1'000'000'000;
    ASSERT_EQ(e->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS), static_cast<HRESULT>(DI_OK));
    const auto packet = startAndCatch(e);
    ASSERT_TRUE(packet);
    EXPECT_FLOAT_EQ(packet->effect.constantLevel, 0.0F);
    // Polar and spherical effects say so in the log line too.
    effect.dwFlags = DIEFF_POLAR;
    LONG direction[1] = {0};
    effect.cAxes = 1;
    DWORD axis[1] = {0};
    effect.rgdwAxes = axis;
    effect.rglDirection = direction;
    EXPECT_EQ(e->SetParameters(&effect, DIEP_AXES | DIEP_DIRECTION), static_cast<HRESULT>(DI_OK));
    effect.dwFlags = DIEFF_SPHERICAL;
    EXPECT_EQ(e->SetParameters(&effect, DIEP_AXES | DIEP_DIRECTION), static_cast<HRESULT>(DI_OK));
    e->Release();
}

}  // namespace
}  // namespace wheelbri::shim
