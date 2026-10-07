// Platform.hpp on Linux and macOS.

#include "Platform.hpp"

#include "wheelbri/Pacing.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <thread>

extern char** environ;

namespace wheelbri::service::platform {

void init() {}

std::uint64_t nowNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * kNsPerSecond + static_cast<std::uint64_t>(now.tv_nsec);
}

void sleepNs(const std::uint64_t ns) {
    if (ns == 0) {
        std::this_thread::yield();
        return;
    }
    timespec remaining{static_cast<time_t>(ns / kNsPerSecond), static_cast<long>(ns % kNsPerSecond)};
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
}

namespace {
std::atomic<std::uint64_t> gLastSpinMarginNs{0};   // what sleepPrecise used last, for the log
}  // namespace

void sleepPrecise(const std::uint64_t ns) {
    thread_local wheelbri::SpinMargin margin;
    const std::uint64_t start = nowNs();
    const std::uint64_t target = start + ns;
    if (ns > margin.ns()) {
        const std::uint64_t wakeAt = target - margin.ns();
        sleepNs(wakeAt - start);
        const std::uint64_t woke = nowNs();
        margin.observe(woke > wakeAt ? woke - wakeAt : 0);
        gLastSpinMarginNs.store(margin.ns(), std::memory_order_relaxed);
    }
    while (nowNs() < target) {
        std::this_thread::yield();
    }
}

std::uint64_t preciseSleepMarginNs() {
    return gLastSpinMarginNs.load(std::memory_order_relaxed);
}

void nameThisThread(const char* const name) {
#if defined(__linux__)
    if (name == nullptr) return;
    char shortName[16] = {};   // the kernel keeps 15 characters and the end
    std::strncpy(shortName, name, sizeof(shortName) - 1);
    pthread_setname_np(pthread_self(), shortName);
#else
    (void)name;
#endif
}

bool raiseThreadPriority(std::string& error) {
#if defined(__linux__)
    // A lower nice value for this thread only (Linux schedules threads separately). Needs CAP_SYS_NICE or
    // an RLIMIT_NICE that allows it; without, the loop runs at normal priority.
    const auto thread = static_cast<id_t>(syscall(SYS_gettid));
    if (setpriority(PRIO_PROCESS, thread, -10) == 0) {
        return true;
    }
    error = std::string("setpriority: ") + std::strerror(errno);
    return false;
#else
    error = "not supported on this system";
    return false;
#endif
}

const char* name() {
#if defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unix";
#endif
}

std::filesystem::path systemFolder() { return "/usr/bin"; }

std::optional<std::uint32_t> udpPortOwner(std::uint16_t /*port*/) { return std::nullopt; }

std::filesystem::path processImage(std::uint32_t /*pid*/) { return {}; }

std::string lastProcessImageError() { return {}; }

std::filesystem::path executablePath() {
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);   // Linux
    return ec ? std::filesystem::path() : self;
}

struct SingleInstance::Impl {
    int fd = -1;
    ~Impl() {
        if (fd >= 0) close(fd);   // also releases the lock
    }
};

std::unique_ptr<SingleInstance> SingleInstance::acquire(const std::string& name, std::string& error) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string folder = runtime != nullptr && runtime[0] != '\0' ? runtime : "/tmp";
    const std::string path = folder + "/" + name + "-" + std::to_string(getuid()) + ".lock";
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = "cannot create the single-instance lock " + path + ": " + std::strerror(errno);
        return nullptr;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int why = errno;
        close(fd);
        error = why == EWOULDBLOCK ? "another wheelbri-service is already running (" + path + " is locked): stop it first"
                                   : "cannot lock " + path + ": " + std::strerror(why);
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->fd = fd;
    return std::unique_ptr<SingleInstance>(new SingleInstance(std::move(impl)));
}

SingleInstance::SingleInstance(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SingleInstance::~SingleInstance() = default;

bool isElevated() { return true; }

void installConsoleStopHandler(void (*)(), bool (*)()) {}

struct ChildProcess::Impl {
    pid_t pid = -1;
    bool running = true;
    bool detached = false;   // keepRunningAfterExit(): never killed by this object
    int exitCode = -1;
};

std::unique_ptr<ChildProcess> ChildProcess::start(const std::vector<std::string>& argv, std::string& error) {
    if (argv.empty()) {
        error = "no program";
        return nullptr;
    }
    std::vector<char*> args;
    std::vector<std::string> copies(argv);
    for (std::string& arg : copies) {
        args.push_back(arg.data());
    }
    args.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    auto impl = std::make_unique<Impl>();
    const int result = posix_spawn(&impl->pid, args.front(), &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        error = std::string("cannot start ") + argv.front() + ": " + std::strerror(result);
        return nullptr;
    }
    return std::unique_ptr<ChildProcess>(new ChildProcess(std::move(impl)));
}

ChildProcess::ChildProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ChildProcess::~ChildProcess() {
    if (impl_->running && !impl_->detached) {
        kill();
    }
}

std::optional<int> ChildProcess::poll() {
    if (impl_->running) {
        int status = 0;
        const pid_t done = waitpid(impl_->pid, &status, WNOHANG);
        if (done == 0 || (done < 0 && errno == EINTR)) {
            return std::nullopt;
        }
        impl_->running = false;
        if (done < 0) {
            impl_->exitCode = -1;
        } else if (WIFEXITED(status)) {
            impl_->exitCode = WEXITSTATUS(status);
        } else {
            impl_->exitCode = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
        }
    }
    return impl_->exitCode;
}

void ChildProcess::release() { impl_->running = false; }

void ChildProcess::keepRunningAfterExit() { impl_->detached = true; }

void ChildProcess::kill() {
    if (!impl_->running) {
        return;
    }
    ::kill(impl_->pid, SIGKILL);
    int status = 0;
    while (waitpid(impl_->pid, &status, 0) < 0 && errno == EINTR) {
    }
    impl_->running = false;
    impl_->exitCode = 128 + SIGKILL;
}

}  // namespace wheelbri::service::platform
