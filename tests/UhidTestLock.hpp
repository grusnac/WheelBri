// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Linux: the tests that run a wheelbri-uhid (UhidMainTest, UhidProcessTest) take this lock for each test, so that
// test runners running tests side by side (ctest -j) never have two of them at once: wheelbri-uhid's ports, the
// fake next to the test program and "is one running?" are shared by the whole computer.

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <filesystem>
#include <string>

namespace wheelbri {

class UhidTestLock {
public:
    UhidTestLock() {
        const std::string path =
            (std::filesystem::temp_directory_path() / ("wheelbri-uhid-tests-" + std::to_string(::getuid()) + ".lock"))
                .string();
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ >= 0) ::flock(fd_, LOCK_EX);
    }
    ~UhidTestLock() {
        if (fd_ >= 0) ::close(fd_);   // closing releases the lock
    }
    UhidTestLock(const UhidTestLock&) = delete;
    UhidTestLock& operator=(const UhidTestLock&) = delete;

private:
    int fd_ = -1;
};

}  // namespace wheelbri
