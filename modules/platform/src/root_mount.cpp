module;

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/nsfs.h>
#include <linux/openat2.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
#endif
#endif

module xlings.platform.root_mount;

import std;

namespace xlings::platform::root_mount {
#if defined(__linux__)
namespace {
struct Descriptors {
    std::vector<int> values;
    ~Descriptors() { for (const int fd : values) if (fd >= 0) ::close(fd); }
    int own(int fd) { if (fd >= 0) values.push_back(fd); return fd; }
};
std::string error(std::string_view step) { return std::string(step) + ": " + std::strerror(errno); }
bool same(int a, int b) {
    struct stat first{}, second{};
    return ::fstat(a, &first) == 0 && ::fstat(b, &second) == 0
        && first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}
bool absolute_normal(const std::filesystem::path& path) {
    return path.is_absolute() && path.lexically_normal() == path && path != "/"
        && path.native().find('\0') == std::string::npos;
}
int open_beneath(int root, const std::string& path) {
    open_how how{};
    how.flags = O_PATH | O_CLOEXEC;
    how.resolve = RESOLVE_IN_ROOT | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    return static_cast<int>(::syscall(SYS_openat2, root, path.c_str(), &how, sizeof(how)));
}
struct Mount {
    int source;
    int old;
    std::string destination;
};
struct ChildError { int step; int code; int rollback; };
// Clone the opened file or directory as a detached mount, make it read-only
// before publication, then attach it. No writable mount is visible to guests.
int bind_readonly(int source, const char* destination) {
    const int tree = static_cast<int>(::syscall(SYS_open_tree, source, "",
        AT_EMPTY_PATH | OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC));
    if (tree < 0) return -1;
    mount_attr attributes{};
    attributes.attr_set = MOUNT_ATTR_RDONLY | MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV;
    const bool configured = ::syscall(SYS_mount_setattr, tree, "", AT_EMPTY_PATH,
        &attributes, sizeof(attributes)) == 0;
    const bool attached = configured && ::syscall(SYS_move_mount, tree, "", AT_FDCWD,
        destination, MOVE_MOUNT_F_EMPTY_PATH) == 0;
    const int saved = errno;
    ::close(tree);
    errno = saved;
    return attached ? 0 : -1;
}
}
#endif

std::expected<std::array<int, 3>, std::string> capture() {
#if defined(__linux__)
    std::array<int, 3> result{-1, -1, -1};
    constexpr const char* paths[]{"/proc/self/ns/user", "/proc/self/ns/mnt", "/"};
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = ::open(paths[i], O_CLOEXEC | (i == 2 ? O_PATH | O_DIRECTORY : O_RDONLY));
        if (result[i] < 0) {
            const auto reason = error("capture root mount namespace");
            for (const int fd : result) if (fd >= 0) ::close(fd);
            return std::unexpected(reason);
        }
    }
    return result;
#else
    return std::unexpected("root mount updates require Linux mount namespaces");
#endif
}

std::expected<void, std::string> update(std::span<const int, 3> target,
    std::span<const Binding> add, std::span<const std::filesystem::path> remove) {
#if defined(__linux__)
    if (::ioctl(target[0], NS_GET_NSTYPE) != CLONE_NEWUSER
        || ::ioctl(target[1], NS_GET_NSTYPE) != CLONE_NEWNS)
        return std::unexpected("invalid root namespace descriptors");
    Descriptors descriptors;
    const int owner = descriptors.own(::ioctl(target[1], NS_GET_USERNS));
    if (owner < 0 || !same(owner, target[0]))
        return std::unexpected("mount namespace does not belong to the captured user namespace");
    struct stat root{};
    if (::fstat(target[2], &root) < 0 || !S_ISDIR(root.st_mode))
        return std::unexpected("invalid captured root directory");
    std::vector<Mount> additions, removals;
    std::set<std::string> destinations;
    auto prepare = [&](const std::filesystem::path& source, const std::filesystem::path& destination,
                       bool removing) -> std::expected<Mount, std::string> {
        if (!absolute_normal(destination) || (!removing && !absolute_normal(source)))
            return std::unexpected("root mount paths must be normalized absolute paths");
        const auto dst = destination.native();
        if (!destinations.insert(dst).second) return std::unexpected("duplicate root mount destination: " + dst);
        const int old = descriptors.own(open_beneath(target[2], dst));
        if (old < 0) return std::unexpected(error("open private root mountpoint " + dst));
        struct stat prior{};
        if (::fstat(old, &prior) < 0 || (!S_ISDIR(prior.st_mode) && !S_ISREG(prior.st_mode)))
            return std::unexpected("root mountpoint is not a regular file or directory: " + dst);
        const int src = removing ? -1 : descriptors.own(::open(source.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
        if (!removing) {
            if (src < 0) return std::unexpected(error("open checked root mount source " + source.string()));
            struct stat next{};
            if (::fstat(src, &next) < 0 || (next.st_mode & S_IFMT) != (prior.st_mode & S_IFMT))
                return std::unexpected("root mount source and destination types differ: " + dst);
        }
        return Mount{src, old, dst};
    };
    for (const auto& binding : add) {
        const auto mount = prepare(binding.source, binding.destination, false);
        if (!mount) return std::unexpected(mount.error());
        additions.push_back(*mount);
    }
    for (const auto& destination : remove) {
        const auto mount = prepare({}, destination, true);
        if (!mount) return std::unexpected(mount.error());
        removals.push_back(*mount);
    }
    if (additions.empty() && removals.empty()) return {};
    int channel[2];
    if (::pipe2(channel, O_CLOEXEC) < 0) return std::unexpected(error("root mount update channel"));
    descriptors.own(channel[0]); descriptors.own(channel[1]);
    const int pid = ::fork();
    if (pid < 0) return std::unexpected(error("start root mount update helper"));
    if (pid == 0) {
        ::close(channel[0]);
        ChildError failure{};
        const int own_user = ::open("/proc/self/ns/user", O_RDONLY | O_CLOEXEC);
        if (own_user < 0 || (!same(own_user, target[0]) && ::setns(target[0], CLONE_NEWUSER) < 0))
            failure = {1, errno, 0};
        if (own_user >= 0) ::close(own_user);
        if (!failure.step && ::setns(target[1], CLONE_NEWNS) < 0) failure = {2, errno, 0};
        if (!failure.step && (::fchdir(target[2]) < 0 || ::chroot(".") < 0 || ::chdir("/") < 0))
            failure = {3, errno, 0};
        std::size_t mounted = 0, removed = 0;
        if (!failure.step) {
            for (const auto& mount : additions) {
                if (bind_readonly(mount.source, mount.destination.c_str()) < 0) { failure = {4, errno, 0}; break; }
                ++mounted;
            }
            if (!failure.step)
                for (const auto& mount : removals) {
                    if (::umount2(mount.destination.c_str(), MNT_DETACH) < 0) { failure = {5, errno, 0}; break; }
                    ++removed;
                }
        }
        if (failure.step) {
            while (removed > 0) {
                const auto& mount = removals[--removed];
                if (bind_readonly(mount.old, mount.destination.c_str()) < 0) failure.rollback = errno;
            }
            while (mounted > 0)
                if (::umount2(additions[--mounted].destination.c_str(), MNT_DETACH) < 0) failure.rollback = errno;
        }
        (void)::write(channel[1], &failure, sizeof(failure));
        ::_exit(failure.step ? 125 : 0);
    }
    ::close(channel[1]);
    // The descriptor is already closed; prevent a later destructor from
    // closing an unrelated descriptor that reused its number.
    descriptors.values.back() = -1;
    pollfd reply{channel[0], POLLIN, 0};
    int ready;
    do { ready = ::poll(&reply, 1, 5000); } while (ready < 0 && errno == EINTR);
    ChildError failure{};
    const auto received = ready > 0 ? ::read(channel[0], &failure, sizeof(failure)) : -1;
    if (received != sizeof(failure)) (void)::kill(pid, SIGKILL);
    int status{};
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (received != sizeof(failure)) return std::unexpected("root mount update helper failed or timed out; end the session");
    if (failure.step) return std::unexpected(std::format("root mount update step {}: {}{}", failure.step,
        std::strerror(failure.code), failure.rollback ? std::format("; rollback failed: {} (end the session)",
            std::strerror(failure.rollback)) : std::string{}));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return std::unexpected("root mount update helper did not complete; end the session");
    return {};
#else
    return std::unexpected("root mount updates require Linux mount namespaces");
#endif
}
}
