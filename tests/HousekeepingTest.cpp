// Retention of the logs (service/src/Housekeeping.cpp): finding a compressor on PATH, deleting old files, and archiving
// them in the background with real child processes. The compressors are small shell scripts in a folder of the
// test's own, which PATH then holds alone: they write what the real tool would (an archive, or one file per input),
// remember their arguments, and can fail, write nothing, or wait until the test lets them go.

#include "Housekeeping.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>

namespace wheelbri::service {
namespace {

namespace fs = std::filesystem;

class ScopedPath {
public:
    explicit ScopedPath(const std::optional<std::string>& value) {
        if (const char* old = std::getenv("PATH")) old_ = old;
        if (value) {
            setenv("PATH", value->c_str(), 1);
        } else {
            unsetenv("PATH");
        }
    }
    ~ScopedPath() {
        if (old_) {
            setenv("PATH", old_->c_str(), 1);
        } else {
            unsetenv("PATH");
        }
    }
    ScopedPath(const ScopedPath&) = delete;
    ScopedPath& operator=(const ScopedPath&) = delete;

private:
    std::optional<std::string> old_;
};

// The script every fake compressor shares: `$0.args` gets its arguments; `$0.fail` makes it exit 3 after writing,
// `$0.noout` exit 0 without writing, `$0.hold` wait (after writing) until that file is gone, `$0.rmin` delete the
// files it was given (as gzip does without -k).
constexpr const char* kPrologue = "#!/bin/sh\n"
                                  "PATH=/usr/bin:/bin\n"   // the test's PATH holds only the fake tools
                                  "echo \"$@\" > \"$0.args\"\n";
constexpr const char* kEpilogue = "[ -f \"$0.rmin\" ] && for a in \"$@\"; do case \"$a\" in -*) ;; *) rm -f \"$a\";; esac; done\n"
                                  "while [ -f \"$0.hold\" ]; do sleep 0.02; done\n"
                                  "[ -f \"$0.fail\" ] && exit 3\n"
                                  "exit 0\n";

class HousekeepingTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = fs::temp_directory_path() /
               ("wheelbri-housekeeping-" + std::to_string(::getuid()) + "-" + std::to_string(::getpid()) + "-" +
                std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(dir_);
        fs::create_directories(bin_());
        fs::create_directories(logs_());
    }
    void TearDown() override { fs::remove_all(dir_); }

    [[nodiscard]] fs::path bin_() const { return dir_ / "bin"; }
    [[nodiscard]] fs::path logs_() const { return dir_ / "logs"; }

    static void write(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }
    [[nodiscard]] static std::string read(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    void tool(const std::string& name, const std::string& body, const bool executable = true) const {
        const fs::path path = bin_() / name;
        write(path, std::string(kPrologue) + "[ -f \"$0.noout\" ] || {\n" + body + "}\n" + kEpilogue);
        fs::permissions(path, executable ? fs::perms::owner_all : fs::perms::owner_read | fs::perms::owner_write);
    }
    // A one-archive-per-day tool: the archive is argument `archiveArg` (1-based), the rest after it are the files.
    void archiveTool(const std::string& name, const int archiveArg) const {
        tool(name, "  shift " + std::to_string(archiveArg - 1) + "\n  out=\"$1\"; shift\n  cat \"$@\" > \"$out\"\n");
    }
    // A one-file-per-input tool: every argument not starting with '-' gets `<it><suffix>`.
    void perFileTool(const std::string& name, const std::string& suffix) const {
        tool(name, "  for a in \"$@\"; do case \"$a\" in -*) ;; *) cat \"$a\" > \"$a" + suffix + "\";; esac; done\n");
    }
    void flag(const std::string& toolName, const std::string& what) const { write(bin_() / (toolName + "." + what), ""); }
    void unflag(const std::string& toolName, const std::string& what) const { fs::remove(bin_() / (toolName + "." + what)); }

    // Pumps until nothing is compressing any more (or 10 s).
    static void pumpUntilIdle(Housekeeping& keeping) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            keeping.pump();
            if (keeping.status().find("compressing") == std::string::npos) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < until);
        FAIL() << "still compressing: " << keeping.status();
    }
    [[nodiscard]] static RetentionConfig config(const OldFilesAction action, const std::string& archiver = "auto") {
        RetentionConfig c;
        c.action = action;
        c.olderThanDays = 2;
        c.archiver = archiver;
        return c;
    }

    fs::path dir_;
};

// Each kind's name, and which of them make one archive per day.
TEST_F(HousekeepingTest, ArchiversHaveNames) {
    using Kind = Archiver::Kind;
    EXPECT_STREQ((Archiver{Kind::SevenZip, {}}.name()), "7z");
    EXPECT_STREQ((Archiver{Kind::Xz, {}}.name()), "xz");
    EXPECT_STREQ((Archiver{Kind::Zstd, {}}.name()), "zstd");
    EXPECT_STREQ((Archiver{Kind::WindowsZip, {}}.name()), "zip");
    EXPECT_STREQ((Archiver{Kind::InfoZip, {}}.name()), "zip");
    EXPECT_STREQ((Archiver{Kind::Gzip, {}}.name()), "gzip");
    EXPECT_STREQ((Archiver{static_cast<Kind>(99), {}}.name()), "?");
    EXPECT_TRUE((Archiver{Kind::SevenZip, {}}.onePerDay()));
    EXPECT_TRUE((Archiver{Kind::WindowsZip, {}}.onePerDay()));
    EXPECT_TRUE((Archiver{Kind::InfoZip, {}}.onePerDay()));
    EXPECT_FALSE((Archiver{Kind::Xz, {}}.onePerDay()));
    EXPECT_FALSE((Archiver{Kind::Zstd, {}}.onePerDay()));
    EXPECT_FALSE((Archiver{Kind::Gzip, {}}.onePerDay()));
}

// "auto" takes the best one there (7z > xz > zstd > zip > gzip); a named one only itself; nothing on PATH, no PATH,
// or empty PATH entries: none. 7-Zip is also found as 7zz or 7za; "zip" here is Info-ZIP (no Windows tar).
TEST_F(HousekeepingTest, TheBestCompressorOnPathIsFound) {
    {
        const ScopedPath path(std::nullopt);
        EXPECT_FALSE(findArchiver("auto"));
    }
    const ScopedPath path("::" + bin_().string() + ":");
    for (const char* name : {"auto", "7z", "xz", "zstd", "zip", "gzip"}) EXPECT_FALSE(findArchiver(name)) << name;

    perFileTool("gzip", ".gz");
    EXPECT_EQ(findArchiver("auto")->kind, Archiver::Kind::Gzip);
    archiveTool("zip", 4);
    EXPECT_EQ(findArchiver("auto")->kind, Archiver::Kind::InfoZip);
    EXPECT_EQ(findArchiver("zip")->kind, Archiver::Kind::InfoZip);
    perFileTool("zstd", ".zst");
    EXPECT_EQ(findArchiver("auto")->kind, Archiver::Kind::Zstd);
    perFileTool("xz", ".xz");
    EXPECT_EQ(findArchiver("auto")->kind, Archiver::Kind::Xz);
    archiveTool("7za", 7);
    EXPECT_EQ(findArchiver("auto")->kind, Archiver::Kind::SevenZip);
    EXPECT_EQ(findArchiver("auto")->executable, bin_() / "7za");
    EXPECT_EQ(findArchiver("gzip")->kind, Archiver::Kind::Gzip);
    EXPECT_EQ(findArchiver("something else")->kind, Archiver::Kind::SevenZip);   // as "auto"
    archiveTool("7z", 7);
    EXPECT_EQ(findArchiver("7z")->executable, bin_() / "7z");
}

// Keep: nothing is looked at or touched.
TEST_F(HousekeepingTest, KeepTouchesNothing) {
    write(logs_() / "telemetry-2020-01-01-01.log", "old");
    Housekeeping keeping(config(OldFilesAction::Keep), {logs_()});
    keeping.run(WallClock::now());
    keeping.pump();
    EXPECT_TRUE(fs::exists(logs_() / "telemetry-2020-01-01-01.log"));
    EXPECT_EQ(keeping.status(), "kept forever ([retention] old_files = keep)");
    EXPECT_FALSE(keeping.failed());
}

// Delete: WheelBri's files (and archives) from before the last 2 days go, in every folder; today's, files that are
// not WheelBri's, and folders stay. A missing folder is no problem.
TEST_F(HousekeepingTest, DeleteRemovesOnlyOldManagedFiles) {
    write(logs_() / "telemetry-2020-01-01-01.log", std::string(1'000'001, 'x'));   // the size said in MB
    write(logs_() / "wheelbri-service-2020-01-02-01.log", "old");
    write(logs_() / "wheelbri-logs-2020-01-03.7z", "old archive");
    write(logs_() / "notes-2020-01-01.txt", "not ours");
    write(logs_() / "telemetry-today.log", "written today");
    fs::create_directories(logs_() / "ffb-2020-01-01-01.log");   // a folder, whatever its name
    write(dir_ / "other" / "ffb-2020-01-04-01.log", "old");
    Housekeeping keeping(config(OldFilesAction::Delete), {logs_(), dir_ / "other", dir_ / "missing"});
    keeping.run(WallClock::now());
    EXPECT_FALSE(fs::exists(logs_() / "telemetry-2020-01-01-01.log"));
    EXPECT_FALSE(fs::exists(logs_() / "wheelbri-service-2020-01-02-01.log"));
    EXPECT_FALSE(fs::exists(logs_() / "wheelbri-logs-2020-01-03.7z"));
    EXPECT_FALSE(fs::exists(dir_ / "other" / "ffb-2020-01-04-01.log"));
    EXPECT_TRUE(fs::exists(logs_() / "notes-2020-01-01.txt"));
    EXPECT_TRUE(fs::exists(logs_() / "telemetry-today.log"));
    EXPECT_TRUE(fs::is_directory(logs_() / "ffb-2020-01-01-01.log"));
    EXPECT_EQ(keeping.status(), "deleted after 2 days, 4 so far (or [retention] old_files = archive)");
    keeping.run(WallClock::now());   // nothing left to delete
    EXPECT_EQ(keeping.status(), "deleted after 2 days, 4 so far (or [retention] old_files = archive)");
}

// Archive with no compressor: old files are kept, and the status says why ("auto" asks for 7-Zip, a named one is
// named).
TEST_F(HousekeepingTest, NoCompressorKeepsTheFilesAndSaysSo) {
    const ScopedPath path(bin_().string());
    write(logs_() / "telemetry-2020-01-01-01.log", "old");
    {
        Housekeeping keeping(config(OldFilesAction::Archive), {logs_()});
        keeping.run(WallClock::now());
        EXPECT_TRUE(keeping.failed());
        EXPECT_EQ(keeping.status(), "archive after 2 days: no compressor found (install 7-Zip), old files kept");
    }
    {
        Housekeeping keeping(config(OldFilesAction::Archive, "xz"), {logs_()});
        keeping.run(WallClock::now());
        EXPECT_EQ(keeping.status(), "archive after 2 days: 'xz' not found, old files kept");
    }
    EXPECT_TRUE(fs::exists(logs_() / "telemetry-2020-01-01-01.log"));
}

// 7-Zip: one archive per old day and folder, one compressor at a time, originals deleted only after it succeeded;
// an archive already there is never overwritten (-2, -3, ...). Archives are not archived again.
TEST_F(HousekeepingTest, SevenZipMakesOneArchivePerDay) {
    const ScopedPath path(bin_().string());
    archiveTool("7z", 7);
    write(logs_() / "telemetry-2020-01-01-01.log", "a1");
    write(logs_() / "wheelbri-service-2020-01-01-01.log", "s1");
    write(logs_() / "telemetry-2020-01-02-01.log", "a2");
    write(logs_() / "wheelbri-logs-2020-01-01.7z", "an older archive");
    write(logs_() / "wheelbri-logs-2020-01-01-2.7z", "and another");
    flag("7z", "hold");   // the first day keeps compressing until the checks below are done
    Housekeeping keeping(config(OldFilesAction::Archive, "7z"), {logs_()});
    keeping.run(WallClock::now());
    EXPECT_EQ(keeping.status(), "archived with 7z after 2 days: compressing 2020-01-01, 1 more");
    keeping.run(WallClock::now());   // the same days again while they wait: not queued twice
    EXPECT_EQ(keeping.status(), "archived with 7z after 2 days: compressing 2020-01-01, 1 more");
    unflag("7z", "hold");
    pumpUntilIdle(keeping);
    pumpUntilIdle(keeping);
    EXPECT_FALSE(keeping.failed());
    EXPECT_EQ(keeping.status(), "archived with 7z after 2 days (2 day(s) archived)");
    EXPECT_EQ(read(logs_() / "wheelbri-logs-2020-01-01-3.7z"), "a1s1");
    EXPECT_EQ(read(logs_() / "wheelbri-logs-2020-01-01.7z"), "an older archive");
    EXPECT_EQ(read(logs_() / "wheelbri-logs-2020-01-02.7z"), "a2");
    EXPECT_FALSE(fs::exists(logs_() / "telemetry-2020-01-01-01.log"));
    EXPECT_FALSE(fs::exists(logs_() / "telemetry-2020-01-02-01.log"));
    EXPECT_EQ(read(bin_() / "7z.args"), "a -t7z -mx=9 -mmt=1 -bd -y " + (logs_() / "wheelbri-logs-2020-01-02.7z").string() +
                                            " " + (logs_() / "telemetry-2020-01-02-01.log").string() + "\n");
}

// Archive names run out after 999 for one day: the last one is used (7-Zip adds to it) rather than looping on.
TEST_F(HousekeepingTest, ArchiveNamesStopAt999) {
    const ScopedPath path(bin_().string());
    archiveTool("7z", 7);
    write(logs_() / "wheelbri-logs-2020-01-01.7z", "");
    for (int n = 2; n < 1000; ++n) write(logs_() / ("wheelbri-logs-2020-01-01-" + std::to_string(n) + ".7z"), "");
    write(logs_() / "telemetry-2020-01-01-01.log", "a1");
    Housekeeping keeping(config(OldFilesAction::Archive, "7z"), {logs_()});
    keeping.run(WallClock::now());
    pumpUntilIdle(keeping);
    EXPECT_EQ(read(logs_() / "wheelbri-logs-2020-01-01-999.7z"), "a1");
    EXPECT_FALSE(fs::exists(logs_() / "wheelbri-logs-2020-01-01-1000.7z"));
}

// A compressor that removes its inputs itself: fine, the day counts as archived (the size before is then unknown).
TEST_F(HousekeepingTest, ACompressorThatRemovesItsInputsIsFine) {
    const ScopedPath path(bin_().string());
    perFileTool("gzip", ".gz");
    flag("gzip", "rmin");
    write(logs_() / "telemetry-2020-01-01-01.log", "a1");
    Housekeeping keeping(config(OldFilesAction::Archive, "gzip"), {logs_()});
    keeping.run(WallClock::now());
    pumpUntilIdle(keeping);
    EXPECT_FALSE(keeping.failed());
    EXPECT_EQ(read(logs_() / "telemetry-2020-01-01-01.log.gz"), "a1");
    EXPECT_EQ(keeping.status(), "archived with gzip after 2 days (1 day(s) archived)");
}

// Before the first run nothing is known about the compressor yet.
TEST_F(HousekeepingTest, TheStatusBeforeTheFirstRun) {
    Housekeeping keeping(config(OldFilesAction::Archive), {logs_()});
    EXPECT_EQ(keeping.status(), "archived with ? after 2 days");
}

// Info-ZIP: one .zip per day, the files without their folders.
TEST_F(HousekeepingTest, InfoZipMakesOneZipPerDay) {
    const ScopedPath path(bin_().string());
    archiveTool("zip", 4);
    write(logs_() / "ffb-2020-01-01-01.log", "f");
    Housekeeping keeping(config(OldFilesAction::Archive, "zip"), {logs_()});
    keeping.run(WallClock::now());
    pumpUntilIdle(keeping);
    EXPECT_EQ(read(logs_() / "wheelbri-logs-2020-01-01.zip"), "f");
    EXPECT_FALSE(fs::exists(logs_() / "ffb-2020-01-01-01.log"));
    EXPECT_EQ(read(bin_() / "zip.args"), "-9 -j -q " + (logs_() / "wheelbri-logs-2020-01-01.zip").string() + " " +
                                             (logs_() / "ffb-2020-01-01-01.log").string() + "\n");
}

// Windows' own tar.exe (bsdtar): one .zip per day, made inside the folder (-C) so the archive holds bare file names.
TEST_F(HousekeepingTest, WindowsTarMakesOneZipPerDay) {
    tool("tar.exe", "  out=\"$4\"; dir=\"$6\"; shift 6\n  cd \"$dir\" && cat \"$@\" > \"$out\"\n");
    write(logs_() / "ffb-2020-01-01-01.log", "f");
    write(logs_() / "telemetry-2020-01-01-01.log", "t");
    Housekeeping keeping(config(OldFilesAction::Archive, "zip"), {logs_()},
                         Archiver{Archiver::Kind::WindowsZip, bin_() / "tar.exe"});
    keeping.run(WallClock::now());
    pumpUntilIdle(keeping);
    EXPECT_FALSE(keeping.failed());
    EXPECT_EQ(keeping.status(), "archived with zip after 2 days (1 day(s) archived)");
    EXPECT_EQ(read(logs_() / "wheelbri-logs-2020-01-01.zip"), "ft");
    EXPECT_FALSE(fs::exists(logs_() / "ffb-2020-01-01-01.log"));
    EXPECT_EQ(read(bin_() / "tar.exe.args"), "-a -c -f " + (logs_() / "wheelbri-logs-2020-01-01.zip").string() +
                                                 " -C " + logs_().string() +
                                                 " ffb-2020-01-01-01.log telemetry-2020-01-01-01.log\n");
}

// xz, zstd and gzip compress each file next to itself with their own options; the originals then go. Empty files
// (an archive of 0 bytes) are fine.
TEST_F(HousekeepingTest, PerFileCompressorsCompressEachFile) {
    const ScopedPath path(bin_().string());
    struct Case {
        const char* name;
        const char* suffix;
        const char* options;
    };
    for (const Case& c : {Case{"xz", ".xz", "-9e -T1 -k -f"}, Case{"zstd", ".zst", "-19 --long=27 -T1 -q -f"},
                          Case{"gzip", ".gz", "-9 -k -f"}}) {
        perFileTool(c.name, c.suffix);
        const fs::path a = logs_() / (std::string("telemetry-2020-01-01-") + c.name + ".log");
        const fs::path b = logs_() / (std::string("ffb-2020-01-01-") + c.name + ".log");
        write(a, "");
        write(b, "");
        Housekeeping keeping(config(OldFilesAction::Archive, c.name), {logs_()});
        keeping.run(WallClock::now());
        pumpUntilIdle(keeping);
        EXPECT_FALSE(keeping.failed()) << c.name;
        EXPECT_TRUE(fs::exists(a.string() + c.suffix)) << c.name;
        EXPECT_TRUE(fs::exists(b.string() + c.suffix)) << c.name;
        EXPECT_FALSE(fs::exists(a)) << c.name;
        EXPECT_EQ(read(bin_() / (std::string(c.name) + ".args")),
                  std::string(c.options) + " " + b.string() + " " + a.string() + "\n")
            << c.name;
        fs::remove(bin_() / c.name);
    }
}

// A compressor that fails, or exits 0 without its output: its partial output goes, the originals stay, the rest of
// the queue is dropped and archiving stops until the next start.
TEST_F(HousekeepingTest, AFailedCompressorKeepsTheOriginals) {
    const ScopedPath path(bin_().string());
    archiveTool("7z", 7);
    write(logs_() / "telemetry-2020-01-01-01.log", "a1");
    write(logs_() / "telemetry-2020-01-02-01.log", "a2");
    flag("7z", "fail");
    Housekeeping keeping(config(OldFilesAction::Archive, "7z"), {logs_()});
    keeping.run(WallClock::now());
    pumpUntilIdle(keeping);
    keeping.pump();
    EXPECT_TRUE(keeping.failed());
    EXPECT_EQ(keeping.status(), "archive after 2 days: 7z failed on 2020-01-01 (exit code 3)");
    EXPECT_FALSE(fs::exists(logs_() / "wheelbri-logs-2020-01-01.7z"));
    EXPECT_FALSE(fs::exists(logs_() / "wheelbri-logs-2020-01-02.7z"));   // the second day never started
    EXPECT_TRUE(fs::exists(logs_() / "telemetry-2020-01-01-01.log"));
    EXPECT_TRUE(fs::exists(logs_() / "telemetry-2020-01-02-01.log"));
    keeping.run(WallClock::now());   // run again (after midnight): the days are queued but nothing starts any more
    keeping.pump();
    EXPECT_EQ(keeping.status(), "archive after 2 days: 7z failed on 2020-01-01 (exit code 3)");
    EXPECT_FALSE(fs::exists(logs_() / "wheelbri-logs-2020-01-01.7z"));

    unflag("7z", "fail");
    flag("7z", "noout");
    Housekeeping again(config(OldFilesAction::Archive, "7z"), {logs_()});
    again.run(WallClock::now());
    pumpUntilIdle(again);
    EXPECT_EQ(again.status(), "archive after 2 days: 7z failed on 2020-01-01 (exit code 0)");
    EXPECT_TRUE(fs::exists(logs_() / "telemetry-2020-01-01-01.log"));
}

// A compressor that cannot be started (not executable): archiving stops with why, the files stay.
TEST_F(HousekeepingTest, ACompressorThatCannotStartKeepsTheOriginals) {
    const ScopedPath path(bin_().string());
    tool("7z", "  :\n", false);
    write(logs_() / "telemetry-2020-01-01-01.log", "a1");
    Housekeeping keeping(config(OldFilesAction::Archive, "7z"), {logs_()});
    keeping.run(WallClock::now());
    EXPECT_TRUE(keeping.failed());
    EXPECT_EQ(keeping.status().rfind("archive after 2 days: cannot start 7z: ", 0), 0U) << keeping.status();
    EXPECT_TRUE(fs::exists(logs_() / "telemetry-2020-01-01-01.log"));
}

// Quitting while a day compresses: the compressor is stopped and its partial archive removed; the originals stay for
// the next start. Nothing running at the end: nothing to stop.
TEST_F(HousekeepingTest, QuittingMidArchiveRemovesThePartialArchive) {
    const ScopedPath path(bin_().string());
    archiveTool("7z", 7);
    write(logs_() / "telemetry-2020-01-01-01.log", "a1");
    flag("7z", "hold");
    {
        Housekeeping keeping(config(OldFilesAction::Archive, "7z"), {logs_()});
        keeping.run(WallClock::now());
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!fs::exists(logs_() / "wheelbri-logs-2020-01-01.7z") && std::chrono::steady_clock::now() < until) {
            keeping.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(fs::exists(logs_() / "wheelbri-logs-2020-01-01.7z"));
        keeping.pump();   // still running
        EXPECT_EQ(keeping.status(), "archived with 7z after 2 days: compressing 2020-01-01");
    }
    EXPECT_FALSE(fs::exists(logs_() / "wheelbri-logs-2020-01-01.7z"));
    EXPECT_TRUE(fs::exists(logs_() / "telemetry-2020-01-01-01.log"));
    unflag("7z", "hold");
}

// Archiving with nothing old: no job, nothing said beyond the compressor.
TEST_F(HousekeepingTest, NothingOldToArchive) {
    const ScopedPath path(bin_().string());
    archiveTool("7z", 7);
    write(logs_() / "telemetry-today.log", "new");
    Housekeeping keeping(config(OldFilesAction::Archive), {logs_()});
    keeping.run(WallClock::now());
    keeping.pump();
    EXPECT_FALSE(keeping.failed());
    EXPECT_EQ(keeping.status(), "archived with 7z after 2 days");
}

}  // namespace
}  // namespace wheelbri::service
