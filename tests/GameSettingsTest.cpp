#include "wheelbri/GameSettings.hpp"

#include "wheelbri/EmulatorConfig.hpp"
#include "wheelbri/FileText.hpp"
#include "wheelbri/GameRotation.hpp"
#include "wheelbri/MappingEdit.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace wheelbri {
namespace {

void putU32(std::string& out, const std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
}

// One entry as NFS Unbound's PROFILEOPTIONS has them: type, name length, name and 0, value length, value and 0.
void putEntry(std::string& out, const std::string& name, const std::string& value, const std::uint32_t type = 1) {
    putU32(out, type);
    putU32(out, static_cast<std::uint32_t>(name.size() + 1));
    out += name;
    out.push_back('\0');
    putU32(out, static_cast<std::uint32_t>(value.size() + 1));
    out += value;
    out.push_back('\0');
}

// A small file shaped like the game's: the header, a few entries, zero padding.
std::string unboundFile() {
    std::string out = "FBCHUNKS";
    out += std::string("\x01\x00\x08\x00\x00\x00\x00\x00\x02\x00", 10);
    putU32(out, 7);
    putEntry(out, "racevehicle.ConceptSteer.0.axis", "0", 4);
    putEntry(out, "WheelSpringMagnitude", "0.500000");
    putEntry(out, "WheelDeadzone", "0.000000");
    putEntry(out, "WheelDamperMagnitude", "0.500000");
    putEntry(out, "WheelFFBMagnitude", "0.500000");
    putEntry(out, "WheelTireForcesMagnitude", "0.500000");
    putEntry(out, "WheelMaxRotation", "270.000000");
    out += std::string(64, '\0');
    return out;
}

TEST(GameSettings, ChangesOnlyTheValuesAskedForInPlace) {
    const std::string file = unboundFile();
    const std::vector<GameSettingText> values = {{"WheelFFBMagnitude", "1.000000"},
                                                 {"WheelSpringMagnitude", "0.800000"},
                                                 {"WheelMaxRotation", "360.000000"}};
    const GameSettingsEdit edit = editFrostbiteSettings(file, values);
    ASSERT_TRUE(edit.problems.empty()) << edit.problems.front();
    ASSERT_EQ(edit.bytes.size(), file.size());
    ASSERT_EQ(edit.changes.size(), 3U);
    EXPECT_EQ(edit.changes[0].name, "WheelFFBMagnitude");
    EXPECT_EQ(edit.changes[0].before, "0.500000");
    EXPECT_EQ(edit.changes[0].after, "1.000000");
    EXPECT_EQ(frostbiteSetting(edit.bytes, "WheelFFBMagnitude"), "1.000000");
    EXPECT_EQ(frostbiteSetting(edit.bytes, "WheelSpringMagnitude"), "0.800000");
    EXPECT_EQ(frostbiteSetting(edit.bytes, "WheelMaxRotation"), "360.000000");
    EXPECT_EQ(frostbiteSetting(edit.bytes, "WheelDamperMagnitude"), "0.500000");   // not asked for: as it was
    EXPECT_EQ(frostbiteSetting(edit.bytes, "WheelTireForcesMagnitude"), "0.500000");
    std::size_t differing = 0;
    for (std::size_t i = 0; i < file.size(); ++i) differing += file[i] != edit.bytes[i] ? 1U : 0U;
    EXPECT_EQ(differing, 5U);   // 0.5->1.0 (2 characters), 0.5->0.8 (1), 270->360 (2), and nothing else
}

TEST(GameSettings, NothingToChangeWhenTheValuesAreThere) {
    const std::string file = unboundFile();
    const std::vector<GameSettingText> values = {{"WheelFFBMagnitude", "0.500000"}};
    const GameSettingsEdit edit = editFrostbiteSettings(file, values);
    EXPECT_TRUE(edit.problems.empty());
    EXPECT_TRUE(edit.changes.empty());
    EXPECT_EQ(edit.bytes, file);
}

// Any problem: nothing written at all, not even the values that were fine.
TEST(GameSettings, RefusesWhatWouldMoveTheFileOrIsNotThere) {
    const std::string file = unboundFile();
    const auto refused = [&](const std::vector<GameSettingText>& values) {
        const GameSettingsEdit edit = editFrostbiteSettings(file, values);
        return !edit.problems.empty() && edit.bytes.empty() && edit.changes.empty();
    };
    EXPECT_TRUE(refused({{"WheelFFBMagnitude", "1.000000"}, {"WheelMaxRotation", "1080.000000"}}));   // longer
    EXPECT_TRUE(refused({{"WheelFFBMagnitude", "1.0"}}));                                             // shorter
    EXPECT_TRUE(refused({{"WheelBrakeMagnitude", "1.000000"}}));                                      // not there
    EXPECT_TRUE(refused({{"Spring", "1.000000"}}));   // only a part of a name: not an entry of its own
    EXPECT_FALSE(editFrostbiteSettings("PROFILE\n", std::vector<GameSettingText>{}).problems.empty());

    std::string twice = unboundFile();
    putEntry(twice, "WheelFFBMagnitude", "0.500000");
    EXPECT_FALSE(editFrostbiteSettings(twice, std::vector<GameSettingText>{{"WheelFFBMagnitude", "1.000000"}})
                     .problems.empty());
    EXPECT_FALSE(frostbiteSetting(twice, "WheelFFBMagnitude").has_value());
}

// An entry's own lengths must match: the name somewhere else (in another value, with a wrong length) is not it.
TEST(GameSettings, OnlyWellFormedEntriesCount) {
    std::string file = "FBCHUNKS";
    putU32(file, 0);
    putEntry(file, "Note", "WheelFFBMagnitude");         // the name as a value: no length before it that fits
    putEntry(file, "WheelFFBMagnitude", "0.500000");
    EXPECT_EQ(frostbiteSetting(file, "WheelFFBMagnitude"), "0.500000");
    std::string broken = "FBCHUNKS";
    putU32(broken, 1);
    putU32(broken, 99);   // a name length that is not the name's
    broken += std::string("WheelFFBMagnitude\0", 18);
    putU32(broken, 9);
    broken += std::string("0.500000\0", 9);
    EXPECT_FALSE(frostbiteSetting(broken, "WheelFFBMagnitude").has_value());
}

TEST(GameSettings, ValuesComeFromTheUserThenTheProfile) {
    const GameProfile* const unbound = gameProfileFor("NeedForSpeedUnbound.exe");
    ASSERT_NE(unbound, nullptr);
    ASSERT_EQ(unbound->settings.size(), 3U);
    const GameSetting* const spring = findGameSetting(*unbound, "spring");
    ASSERT_NE(spring, nullptr);
    EXPECT_EQ(spring->name, "WheelSpringMagnitude");
    EXPECT_EQ(gameSettingValue(*unbound, *spring, {}), 0.8F);
    const GameSettingOverrides mine = {{"nfsunbound", {{"spring", 0.6F}}}};
    EXPECT_EQ(gameSettingValue(*unbound, *spring, mine), 0.6F);
    const GameSetting* const rotation = findGameSetting(*unbound, "rotation");
    ASSERT_NE(rotation, nullptr);
    EXPECT_FALSE(gameSettingValue(*unbound, *rotation, mine).has_value());   // follows the steering

    const std::vector<GameSettingText> texts = gameSettingTexts(*unbound, mine, 360.0F);
    // By id: force_feedback, rotation, spring.
    const std::vector<GameSettingText> expected = {{"WheelFFBMagnitude", "1.000000"},
                                                   {"WheelMaxRotation", "360.000000"},
                                                   {"WheelSpringMagnitude", "0.600000"}};
    EXPECT_EQ(texts, expected);
    EXPECT_EQ(gameSettingLabel("force_feedback"), "force feedback");

    // Its rotation is set per game (the Games tab, [game_rotation]) and written as WheelMaxRotation.
    EXPECT_TRUE(hasSteeringSetting(*unbound));
    EXPECT_FALSE(hasSteeringSetting(*gameProfileFor("NeedForSpeedPayback.exe")));
    EXPECT_FALSE(rotationWhileRunning(*unbound, {}).has_value());   // none of its own: the mapping's is written
    const GameRotations mine540 = {{"nfsunbound", GameRotationOverride{false, 540.0F}}};
    const std::optional<float> perGame = rotationWhileRunning(*unbound, mine540);
    ASSERT_TRUE(perGame.has_value());
    EXPECT_EQ(gameSettingTexts(*unbound, {}, *perGame)[1], (GameSettingText{"WheelMaxRotation", "540.000000"}));
    EXPECT_EQ(findGameSetting(*unbound, "brakes"), nullptr);
}

TEST(GameSettings, ProfileSectionIsChecked) {
    const auto parse = [](const std::string& settings) {
        return parseGameProfile("x", "[game]\nlabel = NFS X\nprogram = x.exe\ntarget = g920\n[game_settings]\n" +
                                         settings);
    };
    const ParsedGameProfile good =
        parse("file = Game/settings/PROFILEOPTIONS\nnote = Why.\nspring = WheelSpringMagnitude 0.8\n"
              "rotation = WheelMaxRotation steering\n");
    ASSERT_TRUE(good.profile.has_value()) << good.problems.front();
    ASSERT_EQ(good.profile->settings.size(), 2U);
    EXPECT_EQ(findGameSetting(*good.profile, "rotation")->value, std::nullopt);
    EXPECT_EQ(findGameSetting(*good.profile, "spring")->value, 0.8F);
    EXPECT_EQ(good.profile->settingsFile, "Game/settings/PROFILEOPTIONS");

    EXPECT_FALSE(parse("file = Game/x\nspring = WheelSpringMagnitude 0.8\n").profile.has_value());   // no note
    EXPECT_FALSE(parse("note = Why.\nspring = WheelSpringMagnitude 0.8\n").profile.has_value());     // no file
    EXPECT_FALSE(parse("file = Game/x\nnote = Why.\n").profile.has_value());                          // no setting
    for (const char* file : {"../x", "C:/x", "/x", "Game\\\\x", "Game//x", "Game/./x"}) {
        EXPECT_FALSE(parse(std::string("file = ") + file + "\nnote = Why.\nspring = WheelSpringMagnitude 0.8\n")
                         .profile.has_value())
            << file;
    }
    EXPECT_FALSE(parse("file = G/x\nnote = Why.\nspring = WheelSpringMagnitude 1.5\n").profile.has_value());
    EXPECT_FALSE(parse("file = G/x\nnote = Why.\nspring = WheelSpringMagnitude\n").profile.has_value());
    EXPECT_FALSE(parse("file = G/x\nnote = Why.\nSpring = WheelSpringMagnitude 0.8\n").profile.has_value());
    EXPECT_FALSE(parse("file = G/x\nnote = Why.\nspring = Wheel Spring 0.8\n").profile.has_value());
    EXPECT_FALSE(parse("file = G/x\nnote = Why.\na = WheelX 0.8\nb = WheelX 0.5\n").profile.has_value());   // twice
}

EmulatorConfigResult load(const std::string& text) { return loadEmulatorConfig(IniDocument::parse(text).document); }

TEST(GameSettings, ConfigReadsAndRefuses) {
    const EmulatorConfigResult good = load("[game_settings]\nnfsunbound.spring = 0.6\nnfsunbound.force_feedback = 1\n");
    ASSERT_TRUE(good.errors.empty()) << good.errors.front();
    EXPECT_EQ(good.config.gameSettings.at("nfsunbound").at("spring"), 0.6F);
    EXPECT_EQ(good.config.gameSettings.at("nfsunbound").at("force_feedback"), 1.0F);
    const EmulatorConfigResult bad = load(
        "[game_settings]\nnfsunbound.spring = 2\nnfsunbound.brakes = 0.5\nmygame.spring = 0.5\n"
        "nfsunbound.rotation = 0.5\nnfsunbound = 0.5\n");
    ASSERT_EQ(bad.errors.size(), 5U);
    std::string all;
    for (const std::string& error : bad.errors) all += error + "\n";
    EXPECT_NE(all.find("nfsunbound.spring = '2': expected a number from 0 to 1"), std::string::npos) << all;
    EXPECT_NE(all.find("nfsunbound.brakes: NFS Unbound has no such setting"), std::string::npos) << all;
    EXPECT_NE(all.find("mygame.spring: no built-in game profile"), std::string::npos) << all;
    EXPECT_NE(all.find("nfsunbound.rotation: follows the steering rotation"), std::string::npos) << all;
    EXPECT_TRUE(bad.config.gameSettings.empty());
}

TEST(GameSettings, MappingWritesAndDropsLines) {
    const std::string first = "[steering]\naxis = 0\n[game_settings]\nnfsunbound.spring = 0.6   ; mine\n";
    MappingModel model = mappingModel(load(first).config);
    EXPECT_EQ(model.gameSettings.at("nfsunbound").at("spring"), 0.6F);
    const std::string base = writeMapping(first, model).text;
    EXPECT_NE(base.find("nfsunbound.spring = 0.6   ; mine"), std::string::npos) << base;   // unchanged: kept
    EXPECT_EQ(writeMapping(base, model).text, base);
    model.gameSettings["nfsunbound"]["force_feedback"] = 0.9F;
    const std::string written = writeMapping(base, model).text;
    EXPECT_NE(written.find("nfsunbound.force_feedback = 0.9"), std::string::npos) << written;
    EXPECT_EQ(mappingModel(load(written).config), model);
    model.gameSettings.clear();   // Default: the profile's again
    EXPECT_EQ(writeMapping(written, model).text.find("nfsunbound."), std::string::npos);
    model.gameSettings["nfsunbound"]["spring"] = 1.5F;
    EXPECT_FALSE(writeMapping(written, model).error.empty());
}

// The game's force feedback gain: the profile's [ffb] gain unless the user set one ([game_ffb_gain]).
TEST(GameSettings, FfbGainComesFromTheUserThenTheProfile) {
    const GameProfile* const unbound = gameProfileFor("NeedForSpeedUnbound.exe");
    const GameProfile* const payback = gameProfileFor("NeedForSpeedPayback.exe");
    ASSERT_NE(unbound, nullptr);
    ASSERT_NE(payback, nullptr);
    EXPECT_EQ(gameFfbGain(*unbound, {}), 3.0F);
    EXPECT_EQ(gameFfbGain(*payback, {}), 1.0F);   // no gain in its profile: as the game asks
    EXPECT_EQ(gameFfbGain(*unbound, {{"nfsunbound", 2.0F}}), 2.0F);

    const EmulatorConfigResult good = load("[game_ffb_gain]\nnfsunbound = 2.5\n");
    ASSERT_TRUE(good.errors.empty()) << good.errors.front();
    EXPECT_EQ(good.config.gameFfbGains.at("nfsunbound"), 2.5F);
    const EmulatorConfigResult bad = load("[game_ffb_gain]\nnfsunbound = 9\nmygame = 2\n");
    ASSERT_EQ(bad.errors.size(), 2U);
    EXPECT_TRUE(bad.config.gameFfbGains.empty());

    const std::string first = "[steering]\naxis = 0\n[game_ffb_gain]\nnfsunbound = 2.5\n";
    MappingModel model = mappingModel(load(first).config);
    const std::string base = writeMapping(first, model).text;
    model.gameFfbGains["nfsunbound"] = 1.5F;
    const std::string written = writeMapping(base, model).text;
    EXPECT_NE(written.find("nfsunbound = 1.5"), std::string::npos) << written;
    EXPECT_EQ(mappingModel(load(written).config), model);
    model.gameFfbGains.clear();   // Default: the profile's again
    EXPECT_EQ(writeMapping(written, model).text.find("game_ffb_gain]\nnfsunbound"), std::string::npos);
    model.gameFfbGains["nfsunbound"] = 5.0F;
    EXPECT_FALSE(writeMapping(written, model).error.empty());
}

// Whatever bytes the file holds (cut short, damaged, another game's), the edit never reads past them and either
// writes nothing or keeps the file's size. Seeded: the same cases on every run.
TEST(GameSettings, DamagedFilesAreSafe) {
    std::mt19937 random(20261012U);
    const std::string good = unboundFile();
    const std::vector<GameSettingText> values = {{"WheelFFBMagnitude", "1.000000"},
                                                 {"WheelMaxRotation", "360.000000"}};
    for (int round = 0; round < 3000; ++round) {
        std::string bytes = good;
        switch (round % 3) {
            case 0: bytes.resize(random() % (good.size() + 1)); break;   // cut short anywhere
            case 1:
                for (int n = 0; n < 8; ++n) bytes[random() % bytes.size()] = static_cast<char>(random());
                break;
            default:
                bytes = "FBCHUNKS";
                for (std::size_t n = random() % 400; n > 0; --n) bytes.push_back(static_cast<char>(random()));
                break;
        }
        const GameSettingsEdit edit = editFrostbiteSettings(bytes, values);
        if (edit.problems.empty()) {
            EXPECT_EQ(edit.bytes.size(), bytes.size());
        } else {
            EXPECT_TRUE(edit.bytes.empty());
            EXPECT_TRUE(edit.changes.empty());
        }
        (void)frostbiteSetting(bytes, "WheelFFBMagnitude");
    }
}

// --- the file on disk (writeGameSettingsFile) ---------------------------------------------------------------------

class GameSettingsFile : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string test = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        root_ = std::filesystem::temp_directory_path() / ("wheelbri-gamesettings-" + test);
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_ / "Game" / "settings");
    }
    void TearDown() override { std::filesystem::remove_all(root_); }

    [[nodiscard]] std::filesystem::path file() const { return root_ / "Game" / "settings" / "PROFILEOPTIONS"; }
    [[nodiscard]] std::filesystem::path backup() const {
        return root_ / "Game" / "settings" / "PROFILEOPTIONS.before-wheelbri";
    }
    [[nodiscard]] GameSettingsFileResult write(const std::vector<GameSettingText>& values) const {
        return writeGameSettingsFile(root_, "Game/settings/PROFILEOPTIONS", values);
    }

    std::filesystem::path root_;
};

const std::vector<GameSettingText> kStronger = {{"WheelFFBMagnitude", "1.000000"},
                                                {"WheelMaxRotation", "360.000000"}};

TEST_F(GameSettingsFile, WritesKeepsTheOriginalOnceAndThenSaysAsSet) {
    const std::string original = unboundFile();
    ASSERT_TRUE(writeFileReplacing(file(), original));
    GameSettingsFileResult result = write(kStronger);
    ASSERT_EQ(result.outcome, GameSettingsFileResult::Outcome::Written) << result.why;
    EXPECT_EQ(result.changes.size(), 2U);
    EXPECT_EQ(readFileText(backup()).value_or(""), original);   // the user's file as it was
    const std::string written = readFileText(file()).value_or("");
    EXPECT_EQ(written.size(), original.size());
    EXPECT_EQ(frostbiteSetting(written, "WheelFFBMagnitude"), "1.000000");

    EXPECT_EQ(write(kStronger).outcome, GameSettingsFileResult::Outcome::AsSet);   // nothing more to do

    // Changed again later: the first backup stays (it is what puts the user's own file back).
    result = write({{"WheelFFBMagnitude", "0.800000"}});
    ASSERT_EQ(result.outcome, GameSettingsFileResult::Outcome::Written) << result.why;
    EXPECT_EQ(readFileText(backup()).value_or(""), original);
    EXPECT_FALSE(std::filesystem::exists(file().string() + ".tmp"));
}

TEST_F(GameSettingsFile, NoFileYetIsNotAnError) {
    const GameSettingsFileResult result = write(kStronger);
    EXPECT_EQ(result.outcome, GameSettingsFileResult::Outcome::NoFile);
    EXPECT_FALSE(std::filesystem::exists(file()));
    EXPECT_FALSE(std::filesystem::exists(backup()));
}

TEST_F(GameSettingsFile, ANotExpectedFileIsLeftAsItIs) {
    ASSERT_TRUE(writeFileReplacing(file(), "PROFILE: not a Frostbite file"));
    EXPECT_EQ(write(kStronger).outcome, GameSettingsFileResult::Outcome::Refused);
    ASSERT_TRUE(writeFileReplacing(file(), unboundFile()));
    const GameSettingsFileResult longer = write({{"WheelMaxRotation", "1080.000000"}});
    EXPECT_EQ(longer.outcome, GameSettingsFileResult::Outcome::Refused);
    EXPECT_NE(longer.why.find("not as long"), std::string::npos) << longer.why;
    EXPECT_EQ(readFileText(file()).value_or(""), unboundFile());   // untouched
    EXPECT_FALSE(std::filesystem::exists(backup()));

    std::filesystem::remove(file());
    std::filesystem::create_directory(file());   // a folder where the file should be
    EXPECT_EQ(write(kStronger).outcome, GameSettingsFileResult::Outcome::Refused);
    std::filesystem::remove(file());
    ASSERT_TRUE(writeFileReplacing(file(), std::string(kMaxGameSettingsFileBytes + 1, 'x')));
    EXPECT_EQ(write(kStronger).outcome, GameSettingsFileResult::Outcome::Refused);   // too large to be it
}

TEST_F(GameSettingsFile, OnlyPathsUnderTheFolder) {
    // "/abs" and "C:x" leave the folder on Windows even though neither is absolute there; "\\server\x" is a share.
    for (const char* relative :
         {"", "../outside", "Game/../../outside", "/abs/PROFILEOPTIONS", "C:x/PROFILEOPTIONS", "\\\\server\\x"}) {
        EXPECT_EQ(writeGameSettingsFile(root_, relative, kStronger).outcome, GameSettingsFileResult::Outcome::Refused)
            << relative;
    }
    EXPECT_EQ(writeGameSettingsFile({}, "Game/settings/PROFILEOPTIONS", kStronger).outcome,
              GameSettingsFileResult::Outcome::Refused);
}

// The service may run as Administrator: a link put in place of the file, its backup or a folder is never followed.
TEST_F(GameSettingsFile, LinksAreNotFollowed) {
    std::error_code ec;
    const std::filesystem::path elsewhere = root_ / "elsewhere";
    ASSERT_TRUE(writeFileReplacing(elsewhere, unboundFile()));
    std::filesystem::create_symlink(elsewhere, file(), ec);
    if (ec) GTEST_SKIP() << "links cannot be made here: " << ec.message();
    EXPECT_EQ(write(kStronger).outcome, GameSettingsFileResult::Outcome::Refused);
    EXPECT_EQ(readFileText(elsewhere).value_or(""), unboundFile());
    // Nor read through by the Games tab.
    EXPECT_EQ(readGameSettingsState(root_, "Game/settings/PROFILEOPTIONS", kStronger).status,
              GameSettingsFileState::Status::Unreadable);

    // The backup's name is a link (dangling: the place it points to does not exist yet): nothing is written there.
    std::filesystem::remove(file());
    ASSERT_TRUE(writeFileReplacing(file(), unboundFile()));
    const std::filesystem::path target = root_ / "target";
    std::filesystem::create_symlink(target, backup(), ec);
    ASSERT_FALSE(ec);
    EXPECT_EQ(write(kStronger).outcome, GameSettingsFileResult::Outcome::Refused);
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_EQ(readFileText(file()).value_or(""), unboundFile());
    std::filesystem::remove(backup());

    // A folder on the way is a link.
    std::filesystem::create_directories(root_ / "real" / "settings");
    ASSERT_TRUE(writeFileReplacing(root_ / "real" / "settings" / "PROFILEOPTIONS", unboundFile()));
    std::filesystem::create_directory_symlink(root_ / "real", root_ / "Linked", ec);
    ASSERT_FALSE(ec);
    EXPECT_EQ(writeGameSettingsFile(root_, "Linked/settings/PROFILEOPTIONS", kStronger).outcome,
              GameSettingsFileResult::Outcome::Refused);
    EXPECT_EQ(readGameSettingsState(root_, "Linked/settings/PROFILEOPTIONS", kStronger).status,
              GameSettingsFileState::Status::Unreadable);
}

// The Games tab reads what the file holds now, with the writer's checks, and never changes it.
TEST_F(GameSettingsFile, StateSaysWhatTheFileHolds) {
    using Status = GameSettingsFileState::Status;
    const std::string path = "Game/settings/PROFILEOPTIONS";
    GameSettingsFileState state = readGameSettingsState(root_, path, kStronger);
    EXPECT_EQ(state.status, Status::NoFile);
    ASSERT_EQ(state.entries.size(), 2U);
    EXPECT_EQ(state.entries[0].wanted, "1.000000");
    EXPECT_FALSE(state.entries[0].inFile);
    EXPECT_FALSE(state.asSet());

    ASSERT_TRUE(writeFileReplacing(file(), unboundFile()));
    state = readGameSettingsState(root_, path, kStronger);
    ASSERT_EQ(state.status, Status::Read) << state.why;
    EXPECT_EQ(state.entries[0].inFile, "0.500000");
    EXPECT_EQ(state.entries[1].inFile, "270.000000");
    EXPECT_FALSE(state.asSet());
    EXPECT_EQ(readFileText(file()).value_or(""), unboundFile());   // only read
    EXPECT_FALSE(std::filesystem::exists(backup()));

    ASSERT_EQ(write(kStronger).outcome, GameSettingsFileResult::Outcome::Written);
    state = readGameSettingsState(root_, path, kStronger);
    EXPECT_TRUE(state.asSet());

    // A name the file does not have: read, not as set.
    const std::vector<GameSettingText> unknown = {{"WheelNothing", "1.000000"}};
    state = readGameSettingsState(root_, path, unknown);
    EXPECT_EQ(state.status, Status::Read);
    EXPECT_FALSE(state.entries[0].inFile);
    EXPECT_FALSE(state.asSet());
    // Nothing asked: as set (nothing to write).
    EXPECT_TRUE(readGameSettingsState(root_, path, {}).asSet());
}

TEST_F(GameSettingsFile, StateOfWhatCannotBeRead) {
    using Status = GameSettingsFileState::Status;
    const std::string path = "Game/settings/PROFILEOPTIONS";
    const GameSettingsFileState notHere = readGameSettingsState({}, path, kStronger);
    EXPECT_EQ(notHere.status, Status::NotHere);
    EXPECT_FALSE(notHere.why.empty());
    EXPECT_EQ(notHere.entries.size(), 2U);
    EXPECT_FALSE(notHere.asSet());
    for (const char* relative : {"", "../outside", "/abs/PROFILEOPTIONS", "C:x/PROFILEOPTIONS", "\\\\server\\x"}) {
        EXPECT_EQ(readGameSettingsState(root_, relative, kStronger).status, Status::Unreadable) << relative;
    }
    ASSERT_TRUE(writeFileReplacing(file(), "PROFILE: not a Frostbite file"));
    GameSettingsFileState state = readGameSettingsState(root_, path, kStronger);
    EXPECT_EQ(state.status, Status::Unreadable);
    EXPECT_NE(state.why.find("FBCHUNKS"), std::string::npos) << state.why;
    ASSERT_TRUE(writeFileReplacing(file(), std::string(kMaxGameSettingsFileBytes + 1, 'x')));
    EXPECT_EQ(readGameSettingsState(root_, path, kStronger).status, Status::Unreadable);
    std::filesystem::remove(file());
    std::filesystem::create_directory(file());
    EXPECT_EQ(readGameSettingsState(root_, path, kStronger).status, Status::Unreadable);
}

// Entries that only look like settings: a name inside the header, a value with control bytes in it. Neither is read
// or written.
TEST(GameSettings, NamesInTheHeaderAndUnprintableValuesDoNotCount) {
    std::string header = std::string("FBCHUNKS", 8) + std::string(1, '\0');   // "KS" and a 0 inside the header
    putEntry(header, "KS", "0.5");
    EXPECT_EQ(frostbiteSetting(header, "KS"), "0.5");   // the entry, not the header's bytes
    std::string bytes = unboundFile();
    putEntry(bytes, "WheelOdd", std::string("0.5\x01", 4));
    EXPECT_FALSE(frostbiteSetting(bytes, "WheelOdd").has_value());
    putEntry(bytes, "WheelHigh", std::string("0.5\x80", 4));   // a byte past ASCII
    EXPECT_FALSE(frostbiteSetting(bytes, "WheelHigh").has_value());
    const GameSettingsEdit edit = editFrostbiteSettings(bytes, std::vector<GameSettingText>{{"WheelOdd", "1.00"}});
    ASSERT_EQ(edit.problems.size(), 1U);
    EXPECT_EQ(edit.problems[0], "WheelOdd: not in the file");
    EXPECT_TRUE(edit.bytes.empty());
}

// Several problems at once are all said, joined, and nothing is written.
TEST_F(GameSettingsFile, EveryProblemIsSaid) {
    ASSERT_TRUE(writeFileReplacing(file(), unboundFile()));
    const GameSettingsFileResult result =
        write({{"WheelNothing", "1.000000"}, {"WheelFFBMagnitude", "1.0"}, {"WheelOther", "1"}});
    EXPECT_EQ(result.outcome, GameSettingsFileResult::Outcome::Refused);
    EXPECT_EQ(result.why.find("WheelNothing: not in the file; WheelFFBMagnitude: 1.0 is not as long"), 0U)
        << result.why;
    EXPECT_NE(result.why.find("; WheelOther: not in the file"), std::string::npos) << result.why;
    EXPECT_EQ(readFileText(file()).value_or(""), unboundFile());
}

// A "." on the way is refused like "..": the path must name the file one way only.
TEST_F(GameSettingsFile, ADotInThePathIsRefused) {
    ASSERT_TRUE(writeFileReplacing(file(), unboundFile()));
    const GameSettingsFileResult result = writeGameSettingsFile(root_, "Game/./settings/PROFILEOPTIONS", kStronger);
    EXPECT_EQ(result.outcome, GameSettingsFileResult::Outcome::Refused);
    EXPECT_EQ(result.why, "the path leaves the settings folder");
    EXPECT_EQ(readFileText(file()).value_or(""), unboundFile());
}

// The backup cannot be made (its temporary name is taken by a folder that is not empty): nothing is changed.
TEST_F(GameSettingsFile, NoBackupNoChange) {
    ASSERT_TRUE(writeFileReplacing(file(), unboundFile()));
    std::filesystem::create_directories(backup().string() + ".tmp/busy");
    const GameSettingsFileResult result = write(kStronger);
    EXPECT_EQ(result.outcome, GameSettingsFileResult::Outcome::Failed);
    EXPECT_EQ(result.why, "cannot keep a copy of the file as it was");
    EXPECT_EQ(readFileText(file()).value_or(""), unboundFile());
    EXPECT_FALSE(std::filesystem::exists(backup()));
}

// The file cannot be replaced (as when the game holds it): Failed, the file as it was, the backup kept for next time.
TEST_F(GameSettingsFile, AFileThatCannotBeReplacedIsLeftAsItWas) {
    ASSERT_TRUE(writeFileReplacing(file(), unboundFile()));
    std::filesystem::create_directories(file().string() + ".tmp/busy");
    const GameSettingsFileResult result = write(kStronger);
    EXPECT_EQ(result.outcome, GameSettingsFileResult::Outcome::Failed);
    EXPECT_EQ(result.why, "cannot write it (the game may have it open)");
    EXPECT_EQ(readFileText(file()).value_or(""), unboundFile());
    EXPECT_EQ(readFileText(backup()).value_or(""), unboundFile());
    EXPECT_TRUE(result.changes.empty());
}

}  // namespace
}  // namespace wheelbri
