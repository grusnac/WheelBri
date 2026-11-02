// GameWatch (service/src/GameWatch.cpp): the games with a profile followed in a session, polled as the session's
// window thread polls it, with the running programs, the processes' program files and the Documents folder given by
// the test (GameWatch::System). What each poll decides (pedal route, force feedback, the game's own mapping), what the
// window shows (status, warning, rotation, the Games tab's live game) and which events it goes through.

#include "GameWatch.hpp"
#include "ServiceLogging.hpp"

#include "wheelbri/FileText.hpp"
#include "wheelbri/GameProfiles.hpp"
#include "wheelbri/GameTuning.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace wheelbri::service {
namespace {

// Points the user's settings folder (games.ini) at a test folder while it lives.
class SettingsHome {
public:
    explicit SettingsHome(const std::filesystem::path& dir) {
        if (const char* value = std::getenv(kName)) before_ = value;
        set(dir.string());
    }
    ~SettingsHome() { set(before_.value_or("")); }
    SettingsHome(const SettingsHome&) = delete;
    SettingsHome& operator=(const SettingsHome&) = delete;

private:
#if defined(_WIN32)
    static constexpr const char* kName = "LOCALAPPDATA";
    static void set(const std::string& value) { _putenv_s(kName, value.c_str()); }
#else
    static constexpr const char* kName = "XDG_CONFIG_HOME";
    static void set(const std::string& value) {
        if (value.empty()) {
            unsetenv(kName);
        } else {
            setenv(kName, value.c_str(), 1);
        }
    }
#endif
    std::optional<std::string> before_;
};

void write(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// NFS 2015's wheel bindings as its settings file holds them (Nfs2015SettingsTest's format).
std::string nfs2015Profile(const int custom, const int accelerate, const int brake) {
    const auto binding = [&](const std::string& action, const int wheel) {
        const std::string prefix = "GstKeyBinding.racevehicle." + action + ".";
        return prefix + "2.axis 24\r\n" + prefix + "2.button " + std::to_string(wheel) + "\r\n" + prefix +
               "2.negate 0\r\n" + prefix + "2.type 3\r\n";
    };
    return "GstInput.CustomControlsWheel " + std::to_string(custom) + "\r\n" + binding("ConceptBrake", brake) +
           binding("ConceptMoveFB", accelerate);
}

GameProfile userGame(const std::string& id, const std::string& program) {
    GameProfile game;
    game.id = id;
    game.label = id;
    game.program = program;
    return game;
}

class GameWatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() /
               ("wheelbri-gamewatch-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        home_.emplace(dir_);
    }
    void TearDown() override {
        home_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    [[nodiscard]] GameWatch::System system() {
        GameWatch::System system;
        system.runningPrograms = [this] { return programs_; };
        system.processImage = [this](const std::uint32_t pid) {
            return pid == 7 ? std::filesystem::path("D:/Games/NFS16.exe") : std::filesystem::path();
        };
        system.documentsFolder = [this] { return documents_; };
        return system;
    }
    [[nodiscard]] GameSessionConfig session(const std::string& target = "g920", const bool routePedals = true) const {
        GameSessionConfig config;
        config.target = target;
        config.routePedals = routePedals;
        return config;
    }
    void run(const std::uint32_t pid, const std::string& name) { programs_ = std::vector<RunningProgram>{{pid, name}}; }
    // Two polls: a game ends after two misses in a row.
    static void twice(GameWatch& watch) {
        static_cast<void>(watch.check());
        static_cast<void>(watch.check());
    }

    std::filesystem::path dir_;
    std::optional<SettingsHome> home_;
    std::optional<std::vector<RunningProgram>> programs_ = std::vector<RunningProgram>{};
    std::filesystem::path documents_;
};

// The games the user added: none without games.ini; its games when it is there; its problems leave the bad ones out.
TEST_F(GameWatchTest, LoadUserGames) {
    EXPECT_TRUE(loadUserGames().empty());
    write(userGamesFile(), "[game.rivals]\nlabel = NFS Rivals\nprogram = NFS14.exe\n[game.BAD]\nlabel = x\n");
    const std::vector<GameProfile> games = loadUserGames();
    ASSERT_EQ(games.size(), 1U);
    EXPECT_EQ(games[0].id, "rivals");
}

// Nothing runs: no status, no warning, no rotation, no live game; the pipeline gets the defaults.
TEST_F(GameWatchTest, NothingRuns) {
    GameWatch watch(session(), 900.0F, {}, {}, system());
    const GameWatch::Decision decision = watch.check();
    EXPECT_TRUE(decision.mapperGame.empty());
    EXPECT_EQ(decision.tuning, nullptr);
    EXPECT_EQ(watch.status(), "");
    EXPECT_EQ(watch.warning(), "");
    EXPECT_EQ(watch.rotationStatus(), "");
    EXPECT_EQ(watch.live().id, "");
    EXPECT_EQ(watch.live().starts, 0U);
    EXPECT_FALSE(watch.wayAdvice().has_value());
    EXPECT_EQ(watch.wayNotices(), 0U);
}

// NFS 2015 starts (its profile fits the G920: its pedals are routed), runs again under a new pid, and ends.
TEST_F(GameWatchTest, AGameStartsRestartsAndEnds) {
    GameWatch watch(session(), 900.0F, {}, {}, system());
    run(7, "NFS16.exe");
    const GameWatch::Decision decision = watch.check();
    EXPECT_EQ(watch.status(), "NFS 2015");
    const GameWatch::Live live = watch.live();
    EXPECT_EQ(live.id, "nfs2015");
    EXPECT_EQ(live.pid, 7U);
    EXPECT_TRUE(live.profileApplies);
    EXPECT_EQ(live.starts, 1U);
    EXPECT_FALSE(decision.route == PedalRoute{});   // routed as its profile says

    run(8, "NFS16.exe");   // restarted: the same game, kept
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.live().pid, 8U);
    EXPECT_EQ(watch.live().starts, 1U);

    run(0, "bash");
    twice(watch);
    EXPECT_EQ(watch.status(), "");
    EXPECT_EQ(watch.live().id, "");
}

// In a shim session the shim applies the profile: the pedals go out as they are.
TEST_F(GameWatchTest, AShimSessionLeavesThePedalsAlone) {
    GameWatch watch(session("g920", false), 900.0F, {}, {}, system());
    run(9, "NFS16.exe");   // no program file for this pid: the name as listed
    const GameWatch::Decision decision = watch.check();
    EXPECT_EQ(watch.status(), "NFS 2015");
    EXPECT_TRUE(decision.route == PedalRoute{});
}

// A profile for another target is named in the log but not used: no status, its pedals not routed.
TEST_F(GameWatchTest, AProfileForAnotherTarget) {
    GameWatch watch(session("g923"), 900.0F, {}, {}, system());
    run(7, "NFS16.exe");
    const GameWatch::Decision decision = watch.check();
    EXPECT_EQ(watch.status(), "");
    EXPECT_EQ(watch.live().id, "nfs2015");
    EXPECT_FALSE(watch.live().profileApplies);
    EXPECT_TRUE(decision.route == PedalRoute{});
}

// NFS Payback steers with 180 while it runs (its profile), unless the mapping has it already; the user's own
// rotation for it wins; it ends: the mapping's again.
TEST_F(GameWatchTest, TheGamesRotation) {
    {
        GameWatch watch(session(), 900.0F, {}, {}, system());
        run(3, "NeedForSpeedPayback.exe");   // no pedal changes in its profile
        static_cast<void>(watch.check());
        EXPECT_EQ(watch.rotationStatus(), "180 deg");
        static_cast<void>(watch.check());   // unchanged: said once
        EXPECT_EQ(watch.rotationStatus(), "180 deg");
        run(0, "bash");
        twice(watch);
        EXPECT_EQ(watch.rotationStatus(), "");
    }
    {
        GameWatch watch(session(), 180.0F, {}, {}, system());   // the mapping's own already
        run(3, "NeedForSpeedPayback.exe");
        static_cast<void>(watch.check());
        EXPECT_EQ(watch.rotationStatus(), "");
    }
    {
        GameRotations own;
        own["nfspayback"] = GameRotationOverride{false, 360.0F};
        GameWatch watch(session(), 900.0F, own, {}, system());
        run(3, "NeedForSpeedPayback.exe");
        static_cast<void>(watch.check());
        EXPECT_EQ(watch.rotationStatus(), "360 deg");
    }
}

// The mapping's rotation changed while the session runs (the Mapping tab): rotationNow follows it while no game has
// its own, and a game's own wins while it runs; one equal to the new mapping's is nothing to say.
TEST_F(GameWatchTest, MappingRotationChangesWhileRunning) {
    GameWatch watch(session(), 900.0F, {}, {}, system());
    EXPECT_FLOAT_EQ(watch.rotationNow(), 900.0F);
    watch.setMapping(540.0F);
    EXPECT_FLOAT_EQ(watch.rotationNow(), 540.0F);
    run(3, "NeedForSpeedPayback.exe");   // 180 in its profile
    static_cast<void>(watch.check());
    EXPECT_FLOAT_EQ(watch.rotationNow(), 180.0F);
    EXPECT_EQ(watch.rotationStatus(), "180 deg");
    watch.setMapping(180.0F);   // the mapping's now: no longer the game's own
    EXPECT_FLOAT_EQ(watch.rotationNow(), 180.0F);
    EXPECT_EQ(watch.rotationStatus(), "");
    watch.setMapping(720.0F);
    EXPECT_FLOAT_EQ(watch.rotationNow(), 180.0F);
    EXPECT_EQ(watch.rotationStatus(), "180 deg");
}

// The user's own values for a game replace the mapping's, and are said to go when it ends; setValues takes new ones
// while the session runs.
TEST_F(GameWatchTest, OwnValues) {
    GameSessionConfig config = session();
    GameTuning tuning;
    tuning.pedalInvert[0] = true;   // changes the mapping: a per-game mapper
    config.tunings["nfs2015"] = tuning;
    GameWatch watch(config, 900.0F, {}, {}, system());
    run(3, "NeedForSpeedPayback.exe");
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.live().id, "nfspayback");
    run(7, "NFS16.exe");   // switched: another game while the first still ends
    twice(watch);
    const GameWatch::Decision decision = watch.check();
    EXPECT_EQ(decision.mapperGame, "nfs2015");
    ASSERT_NE(decision.tuning, nullptr);
    EXPECT_EQ(watch.live().starts, 2U);

    // Values that change nothing of the mapping: no per-game mapper.
    GameTunings plain;
    plain["nfs2015"].pedalAxes[0] = ObjectGuid::Slider;
    watch.setValues(plain, {});
    const GameWatch::Decision again = watch.check();
    EXPECT_TRUE(again.mapperGame.empty());
    watch.setValues({}, {});
    static_cast<void>(watch.check());

    run(0, "bash");
    twice(watch);
    EXPECT_EQ(watch.live().id, "");
}

// Two games with a profile: the first followed stays, the other is reported, and its end reported too.
TEST_F(GameWatchTest, TwoGamesRunning) {
    GameWatch watch(session(), 900.0F, {}, {}, system());
    programs_ = std::vector<RunningProgram>{{7, "NFS16.exe"}, {3, "NeedForSpeedPayback.exe"}};
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.live().id, "nfs2015");
    run(7, "NFS16.exe");
    twice(watch);
    EXPECT_EQ(watch.live().id, "nfs2015");
    // Only the other one is left: it is followed once the first one is gone.
    run(3, "NeedForSpeedPayback.exe");
    twice(watch);
    twice(watch);
    EXPECT_EQ(watch.live().id, "nfspayback");
}

// The running programs cannot be listed: the profile stays (said while one is in use), and listing again is said.
TEST_F(GameWatchTest, TheProgramsCannotBeListed) {
    GameWatch watch(session(), 900.0F, {}, {}, system());
    programs_.reset();
    static_cast<void>(watch.check());   // none in use: nothing to say
    programs_ = std::vector<RunningProgram>{};
    static_cast<void>(watch.check());
    run(7, "NFS16.exe");
    static_cast<void>(watch.check());
    programs_.reset();
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.status(), "NFS 2015");
    run(7, "NFS16.exe");
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.status(), "NFS 2015");
}

// Games the user added are followed like the profiles; one with a built-in profile's program or id is that profile.
TEST_F(GameWatchTest, GamesTheUserAdded) {
    std::vector<GameProfile> added = {userGame("rivals", "nfs14.exe"), userGame("copy", "nfs16.exe"),
                                      userGame("nfs2015", "other.exe")};
    GameWatch watch(session(), 900.0F, {}, added, system());
    run(5, "NFS14.exe");
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.live().id, "rivals");
    EXPECT_EQ(watch.status(), "rivals");
    run(6, "other.exe");   // not followed: its id is a built-in profile's
    twice(watch);
    EXPECT_EQ(watch.live().id, "");
}

// A game that works one way only, run the other way: the advice once per start, in red in the window.
TEST_F(GameWatchTest, AGameRunTheWrongWay) {
    GameProfile virtualOnly = userGame("vonly", "vonly.exe");
    virtualOnly.shim = false;
    virtualOnly.waysNote = "it reads the wheel through a driver";
    GameWatch watch(session("g920", false), 900.0F, {}, {virtualOnly}, system());
    run(4, "vonly.exe");
    static_cast<void>(watch.check());
    ASSERT_TRUE(watch.wayAdvice().has_value());
    EXPECT_EQ(watch.wayNotices(), 1U);
    EXPECT_NE(watch.warning(), "");
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.wayNotices(), 1U);   // the same start: once
    run(0, "bash");
    twice(watch);
    EXPECT_EQ(watch.warning(), "");
}

// The user's pedal axes for a game that the virtual device cannot route: said once while it runs, again at its next
// start; the pedals stay as they are.
TEST_F(GameWatchTest, PedalsThatCannotBeRouted) {
    GameSessionConfig config = session();
    GameTuning tuning;
    tuning.pedalAxes[0] = ObjectGuid::RxAxis;
    config.tunings["heat"] = tuning;
    GameWatch watch(config, 900.0F, {}, {userGame("heat", "heat.exe")}, system());
    run(5, "heat.exe");
    EXPECT_TRUE(watch.check().route == PedalRoute{});
    EXPECT_TRUE(watch.check().route == PedalRoute{});   // said once
    run(0, "bash");
    twice(watch);
    run(5, "heat.exe");
    static_cast<void>(watch.check());
}

// NFS 2015's own wheel bindings, read when its settings file changes: an old workaround that swaps the pedals again
// is a red warning; the defaults, the expected ones or a file that says nothing are not; no Documents folder, no file,
// or a file too big are not read.
TEST_F(GameWatchTest, Nfs2015sOwnWheelBindings) {
    GameWatch watch(session(), 900.0F, {}, {}, system());
    run(7, "NFS16.exe");
    static_cast<void>(watch.check());   // no Documents folder (not Windows): not checked
    EXPECT_EQ(watch.warning(), "");

    documents_ = dir_ / "Documents";
    static_cast<void>(watch.check());   // no settings file yet
    EXPECT_EQ(watch.warning(), "");

    const std::filesystem::path file = documents_ / "Need For Speed" / "settings" / "PROFILEOPTIONS_profile";
    write(file, nfs2015Profile(1, 25, 26));   // the old workaround
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.warning(), "NFS 2015: RESET ITS WHEEL CONTROLS");
    static_cast<void>(watch.check());   // unchanged: not read again
    EXPECT_EQ(watch.warning(), "NFS 2015: RESET ITS WHEEL CONTROLS");

    write(file, nfs2015Profile(1, 24, 25));
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(5));
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.warning(), "");

    write(file, nfs2015Profile(0, 25, 26));   // its own defaults: the bindings are not used
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(10));
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.warning(), "");

    write(file, "nothing it knows\r\n");   // unreadable: nothing known, nothing said
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(15));
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.warning(), "");

    write(file, nfs2015Profile(1, 25, 26) + std::string(2 * 1024 * 1024, ' '));   // too big: not NFS 2015's
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(20));
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.warning(), "");

    // Another game: the bindings are not looked at, and the warning goes.
    write(file, nfs2015Profile(1, 25, 26));
    std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(25));
    static_cast<void>(watch.check());
    EXPECT_NE(watch.warning(), "");
    run(3, "NeedForSpeedPayback.exe");
    twice(watch);
    twice(watch);
    EXPECT_EQ(watch.warning(), "");
}


// Three games with a profile at once: both others are named; the session's target in upper case is the same target.
TEST_F(GameWatchTest, ThreeGamesAndAnUpperCaseTarget) {
    GameWatch watch(session("G920"), 900.0F, {}, {}, system());
    programs_ = std::vector<RunningProgram>{{7, "NFS16.exe"}, {3, "NeedForSpeedPayback.exe"},
                                            {4, "NeedForSpeedUnbound.exe"}};
    static_cast<void>(watch.check());
    EXPECT_EQ(watch.status(), "NFS 2015");
}

// The system's own running programs, program files and Documents folder when none are given.
TEST_F(GameWatchTest, TheSystemsOwn) {
    GameWatch plain(session(), 900.0F, {}, {});
    static_cast<void>(plain.check());   // whatever runs here: no game with a profile
    EXPECT_EQ(plain.status(), "");
    GameWatch::System only;
    only.runningPrograms = [this] { return programs_; };
    GameWatch watch(session(), 900.0F, {}, {}, only);
    run(7, "NFS16.exe");
    static_cast<void>(watch.check());   // its program file and NFS 2015's settings from the system
    EXPECT_EQ(watch.status(), "NFS 2015");
}

}  // namespace
}  // namespace wheelbri::service
