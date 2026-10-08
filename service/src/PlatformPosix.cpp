// Platform.hpp on Linux and macOS.

#include "Platform.hpp"


#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/inotify.h>
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
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

// --- WaitSet: poll on the devices' and the sockets' file descriptors ---

struct WaitSet::Impl {
    std::vector<pollfd> fds;
    std::string failure;
};

WaitSet::WaitSet() : impl_(std::make_unique<Impl>()) {}
WaitSet::~WaitSet() = default;

int WaitSet::addEvent(const std::intptr_t source) {
    if (source < 0) return -1;
    impl_->fds.push_back({static_cast<int>(source), POLLIN, 0});
    return static_cast<int>(impl_->fds.size()) - 1;
}

int WaitSet::addSocket(const std::intptr_t socket) { return addEvent(socket); }

int WaitSet::waitUntil(const std::uint64_t deadlineNs) {
    const std::uint64_t now = nowNs();
    const std::uint64_t remainingNs = deadlineNs > now ? deadlineNs - now : 0;
    // Whole milliseconds, rounded up (poll is everywhere; the deadlines are 4 ms and more).
    const auto timeoutMs = static_cast<int>(std::min<std::uint64_t>((remainingNs + kNsPerMs - 1) / kNsPerMs, 60'000));
    const int ready = ::poll(impl_->fds.data(), static_cast<nfds_t>(impl_->fds.size()), timeoutMs);
    if (ready > 0) {
        for (std::size_t i = 0; i < impl_->fds.size(); ++i) {
            // A descriptor that is no longer open (closed behind the set's back): poll would report it at once every
            // time, so the loop cannot wait on it; say so, and the loop polls instead (EventLoopControl).
            if ((impl_->fds[i].revents & POLLNVAL) != 0) {
                impl_->failure = "poll: source " + std::to_string(i) + " is not an open descriptor";
                return kFailed;
            }
        }
        for (std::size_t i = 0; i < impl_->fds.size(); ++i) {
            // POLLIN, or POLLERR / POLLHUP (an unplugged evdev device): the read says which.
            if (impl_->fds[i].revents != 0) return static_cast<int>(i);
        }
    } else if (ready < 0 && errno != EINTR) {
        impl_->failure = std::string("poll: ") + std::strerror(errno);
        return kFailed;
    }
    return kTimedOut;
}

int WaitSet::size() const { return static_cast<int>(impl_->fds.size()); }

const std::string& WaitSet::failureText() const { return impl_->failure; }

void nameThisThread(const char* const name) {
#if defined(__linux__)
    if (name == nullptr) return;
    char shortName[16] = {};   // the kernel keeps 15 characters and the end: longer names are cut on purpose
    std::memcpy(shortName, name, std::min(std::strlen(name), sizeof(shortName) - 1));
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

// --- FolderWatch (inotify, Linux) and DeviceChanges on /dev/input ---

struct FolderWatch::Impl {
    int inotify = -1;
    std::array<int, 2> stopPipe{-1, -1};   // written to by the destructor: poll wakes, the thread ends
    std::function<void()> onChange;
    std::thread thread;

    ~Impl() {
        for (const int fd : {inotify, stopPipe[0], stopPipe[1]}) {
            if (fd >= 0) ::close(fd);
        }
    }

    void run() {
        nameThisThread("wheelbri folder watch");
        alignas(8) char buffer[4096];
        for (;;) {
            std::array<pollfd, 2> fds{{{inotify, POLLIN, 0}, {stopPipe[0], POLLIN, 0}}};
            const int ready = ::poll(fds.data(), fds.size(), -1);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0 || fds[1].revents != 0) return;           // stopped (or poll itself failed)
            if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return;
            if ((fds[0].revents & POLLIN) == 0) continue;
            const ssize_t got = ::read(inotify, buffer, sizeof(buffer));   // every event queued now: one call
            if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (got <= 0) return;   // the descriptor is broken: callers still have their safety timer
            onChange();
        }
    }
};

std::unique_ptr<FolderWatch> FolderWatch::create(const std::filesystem::path& folder, std::function<void()> onChange) {
#if defined(__linux__)
    if (!onChange) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->inotify = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (impl->inotify < 0 || ::pipe2(impl->stopPipe.data(), O_CLOEXEC) != 0) return nullptr;
    if (inotify_add_watch(impl->inotify, folder.c_str(),
                          IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_TO | IN_MOVED_FROM | IN_ONLYDIR) < 0) {
        return nullptr;
    }
    impl->onChange = std::move(onChange);
    Impl* const raw = impl.get();
    impl->thread = std::thread([raw] { raw->run(); });
    return std::unique_ptr<FolderWatch>(new FolderWatch(std::move(impl)));
#else
    (void)folder;
    (void)onChange;
    return nullptr;
#endif
}

FolderWatch::FolderWatch(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FolderWatch::~FolderWatch() {
    const char stop = 's';
    (void)!::write(impl_->stopPipe[1], &stop, 1);
    if (impl_->thread.joinable()) impl_->thread.join();
}

namespace detail {

bool watchDeviceChanges(std::function<void()> onChange) {
    // Event devices appearing or going, or becoming readable once udev has set their permissions. Kept for the life
    // of the process, as DeviceChanges.
    static std::unique_ptr<FolderWatch>* const watch =
        new std::unique_ptr<FolderWatch>(FolderWatch::create("/dev/input", std::move(onChange)));
    return *watch != nullptr;
}

}  // namespace detail

}  // namespace wheelbri::service::platform
