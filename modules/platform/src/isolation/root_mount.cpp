module;

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/nsfs.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#endif

module xlings.platform.root_mount;

import std;

namespace xlings::platform::root_mount {
#if defined(__linux__)
namespace {
// Stable kernel ABIs; the static toolchain does not ship recent UAPI headers.
struct OpenHow {
    std::uint64_t flags, mode, resolve;
};
struct MountAttributes {
    std::uint64_t attr_set, attr_clr, propagation, userns_fd;
};
constexpr long kOpenat2 = 437, kOpenTree = 428, kMoveMount = 429, kMountSetattr = 442;
constexpr std::uint64_t kInRoot = 0x10, kNoSymlinks = 0x04, kNoMagicLinks = 0x02;
constexpr unsigned kOpenTreeClone = 1, kMoveMountEmpty = 0x04, kRecursive = 0x8000;
constexpr std::uint64_t kReadOnly = 0x01, kNoSuid = 0x02, kNoDev = 0x04;
struct Descriptors {
    std::vector<int> values;
    ~Descriptors() {
        for (const int fd : values)
            if (fd >= 0)
                ::close(fd);
    }
    int own(int fd) {
        if (fd >= 0)
            values.push_back(fd);
        return fd;
    }
};
std::string error(std::string_view step) {
    return std::string(step) + ": " + std::strerror(errno);
}
bool same(int a, int b) {
    struct stat first{}, second{};
    return ::fstat(a, &first) == 0 && ::fstat(b, &second) == 0 && first.st_dev == second.st_dev &&
           first.st_ino == second.st_ino;
}
bool absolute_normal(const std::filesystem::path& path) {
    return path.is_absolute() && path.lexically_normal() == path && path != "/" &&
           path.native().find('\0') == std::string::npos;
}
int open_beneath(int root, const std::string& path) {
    OpenHow how{};
    how.flags = O_PATH | O_CLOEXEC;
    how.resolve = kInRoot | kNoSymlinks | kNoMagicLinks;
    return static_cast<int>(::syscall(kOpenat2, root, path.c_str(), &how, sizeof(how)));
}
struct Mount {
    int source;
    int old;
    std::string sourcePath;
    std::string destination;
    bool replace{false};
};
enum class Stage {
    None,
    UserNamespace,
    SourceNamespace,
    PrivatePropagation,
    SourceRoot,
    OpenSource,
    SourceIdentity,
    CloneSource,
    ReadOnlySource,
    TargetNamespace,
    TargetRoot,
    CloneRollback,
    ReadOnlyRollback,
    Publish,
    Remove,
    Restore,
    UndoPublication,
};
std::string_view stage_name(Stage stage) {
    switch (stage) {
    case Stage::UserNamespace:
        return "setns-user";
    case Stage::SourceNamespace:
        return "unshare-source-mountns";
    case Stage::PrivatePropagation:
        return "private-source-propagation";
    case Stage::SourceRoot:
        return "open-source-root";
    case Stage::OpenSource:
        return "open-source";
    case Stage::SourceIdentity:
        return "source-identity";
    case Stage::CloneSource:
        return "open_tree-source";
    case Stage::ReadOnlySource:
        return "mount_setattr-source";
    case Stage::TargetNamespace:
        return "setns-target-mountns";
    case Stage::TargetRoot:
        return "chroot-target";
    case Stage::CloneRollback:
        return "open_tree-rollback";
    case Stage::ReadOnlyRollback:
        return "mount_setattr-rollback";
    case Stage::Publish:
        return "move_mount";
    case Stage::Remove:
        return "unmount";
    case Stage::Restore:
        return "move_mount-rollback";
    case Stage::UndoPublication:
        return "unmount-publication";
    case Stage::None:
        return "none";
    }
    return "unknown";
}
struct ChildError {
    Stage step{};
    int code{};
    std::size_t index{};
    Stage rollbackStep{};
    int rollbackCode{};
    std::size_t rollbackIndex{};
};
// A source must belong to the current mount namespace. A detached, recursively
// read-only copy can then be moved to the captured namespace without exposing RW.
int clone_readonly(int source, Stage cloneStage, Stage attributeStage, std::size_t index,
                   ChildError& failure) {
    const int tree = static_cast<int>(
        ::syscall(kOpenTree, source, "", AT_EMPTY_PATH | kRecursive | kOpenTreeClone | O_CLOEXEC));
    if (tree < 0) {
        failure = {cloneStage, errno, index};
        return -1;
    }
    MountAttributes attributes{};
    attributes.attr_set = kReadOnly | kNoSuid | kNoDev;
    if (::syscall(kMountSetattr, tree, "", AT_EMPTY_PATH | kRecursive, &attributes,
                  sizeof(attributes)) < 0) {
        failure = {attributeStage, errno, index};
        ::close(tree);
        return -1;
    }
    return tree;
}
int publish(int tree, const std::string& destination) {
    return static_cast<int>(
        ::syscall(kMoveMount, tree, "", AT_FDCWD, destination.c_str(), kMoveMountEmpty));
}
} // namespace
#endif

std::expected<std::array<int, 3>, std::string> capture() {
#if defined(__linux__)
    std::array<int, 3> result{-1, -1, -1};
    constexpr const char* paths[]{"/proc/self/ns/user", "/proc/self/ns/mnt", "/"};
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i] = ::open(paths[i], O_CLOEXEC | (i == 2 ? O_PATH | O_DIRECTORY : O_RDONLY));
        if (result[i] < 0) {
            const auto reason = error("capture root mount namespace");
            for (const int fd : result)
                if (fd >= 0)
                    ::close(fd);
            return std::unexpected(reason);
        }
    }
    return result;
#else
    return std::unexpected("root mount updates require Linux mount namespaces");
#endif
}

std::expected<void, std::string> normalize(std::array<int, 3>& target) {
#if defined(__linux__)
    if (::ioctl(target[0], NS_GET_NSTYPE) != CLONE_NEWUSER ||
        ::ioctl(target[1], NS_GET_NSTYPE) != CLONE_NEWNS)
        return std::unexpected("invalid captured root namespace descriptors");
    struct stat root{};
    if (::fstat(target[2], &root) < 0 || !S_ISDIR(root.st_mode))
        return std::unexpected("invalid captured root directory");
    Descriptors descriptors;
    const int owner = descriptors.own(::ioctl(target[1], NS_GET_USERNS));
    if (owner < 0)
        return std::unexpected(error("derive captured mount namespace owner"));
    int current = target[0];
    bool proved = false;
    for (unsigned depth = 0; depth <= 32; ++depth) {
        if (same(current, owner)) {
            proved = true;
            break;
        }
        current = descriptors.own(::ioctl(current, NS_GET_PARENT));
        if (current < 0)
            return std::unexpected(error("prove captured user namespace ancestry"));
    }
    if (!proved)
        return std::unexpected("captured user namespace is outside the mount owner's ancestry");
    ::close(target[0]);
    target[0] = owner;
    for (auto& descriptor : descriptors.values)
        if (descriptor == owner)
            descriptor = -1;
    return {};
#else
    return std::unexpected("root mount namespace authority requires Linux");
#endif
}

std::expected<void, std::string> update(std::span<const int, 3> target,
                                        std::span<const Binding> add,
                                        std::span<const std::filesystem::path> remove) {
#if defined(__linux__)
    if (::ioctl(target[0], NS_GET_NSTYPE) != CLONE_NEWUSER ||
        ::ioctl(target[1], NS_GET_NSTYPE) != CLONE_NEWNS)
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
    auto prepare = [&](const std::filesystem::path& source,
                       const std::filesystem::path& destination, bool removing,
                       bool replacing = false) -> std::expected<Mount, std::string> {
        if (!absolute_normal(destination) || (!removing && !absolute_normal(source)))
            return std::unexpected("root mount paths must be normalized absolute paths");
        const auto dst = destination.native();
        if (!destinations.insert(dst).second)
            return std::unexpected("duplicate root mount destination: " + dst);
        const int old = descriptors.own(open_beneath(target[2], dst));
        if (old < 0)
            return std::unexpected(error("open private root mountpoint " + dst));
        struct stat prior{};
        if (::fstat(old, &prior) < 0 || (!S_ISDIR(prior.st_mode) && !S_ISREG(prior.st_mode)))
            return std::unexpected("root mountpoint is not a regular file or directory: " + dst);
        const int src =
            removing ? -1
                     : descriptors.own(::open(source.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
        if (!removing) {
            if (src < 0)
                return std::unexpected(error("open checked root mount source " + source.string()));
            struct stat next{};
            if (::fstat(src, &next) < 0 || (next.st_mode & S_IFMT) != (prior.st_mode & S_IFMT))
                return std::unexpected("root mount source and destination types differ: " + dst);
        }
        return Mount{src, old, source.native(), dst, replacing};
    };
    for (const auto& binding : add) {
        const auto mount = prepare(binding.source, binding.destination, false, binding.replace);
        if (!mount)
            return std::unexpected(mount.error());
        additions.push_back(*mount);
    }
    for (const auto& destination : remove) {
        const auto mount = prepare({}, destination, true);
        if (!mount)
            return std::unexpected(mount.error());
        removals.push_back(*mount);
    }
    if (additions.empty() && removals.empty())
        return {};
    std::vector<Mount> retirements = removals;
    for (const auto& mount : additions)
        if (mount.replace)
            retirements.push_back(mount);
    std::ranges::sort(retirements, [](const Mount& a, const Mount& b) {
        return a.destination.size() > b.destination.size();
    });
    std::vector<int> sourceTrees(additions.size(), -1), rollbackTrees(retirements.size(), -1);
    std::vector<bool> restoredTrees(retirements.size(), false);
    int channel[2];
    if (::pipe2(channel, O_CLOEXEC) < 0)
        return std::unexpected(error("root mount update channel"));
    descriptors.own(channel[0]);
    descriptors.own(channel[1]);
    const int pid = ::fork();
    if (pid < 0)
        return std::unexpected(error("start root mount update helper"));
    if (pid == 0) {
        ::close(channel[0]);
        ChildError failure{};
        const int ownUser = ::open("/proc/self/ns/user", O_RDONLY | O_CLOEXEC);
        if (ownUser < 0 || (!same(ownUser, target[0]) && ::setns(target[0], CLONE_NEWUSER) < 0))
            failure = {Stage::UserNamespace, errno};
        if (ownUser >= 0)
            ::close(ownUser);
        // Copy the supervisor's source view into a namespace owned by the proven
        // USER. Original O_PATH descriptors still point into the supervisor's
        // namespace, so reopen and compare their exact file identities here.
        if (failure.step == Stage::None && !additions.empty()) {
            if (::unshare(CLONE_NEWNS) < 0)
                failure = {Stage::SourceNamespace, errno};
            if (failure.step == Stage::None &&
                ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) < 0)
                failure = {Stage::PrivatePropagation, errno};
            const int sourceRoot =
                failure.step == Stage::None ? ::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC) : -1;
            if (failure.step == Stage::None && sourceRoot < 0)
                failure = {Stage::SourceRoot, errno};
            for (std::size_t i = 0; failure.step == Stage::None && i < additions.size(); ++i) {
                const auto& mount = additions[i];
                OpenHow how{};
                how.flags = O_PATH | O_CLOEXEC | O_NOFOLLOW;
                how.resolve = kInRoot | kNoMagicLinks;
                const int source = static_cast<int>(
                    ::syscall(kOpenat2, sourceRoot, mount.sourcePath.c_str(), &how, sizeof(how)));
                if (source < 0) {
                    failure = {Stage::OpenSource, errno, i};
                    break;
                }
                if (!same(source, mount.source))
                    failure = {Stage::SourceIdentity, ESTALE, i};
                else
                    sourceTrees[i] = clone_readonly(source, Stage::CloneSource,
                                                    Stage::ReadOnlySource, i, failure);
                ::close(source);
                ::close(mount.source);
                if (!mount.replace)
                    ::close(mount.old);
            }
            if (sourceRoot >= 0)
                ::close(sourceRoot);
        }
        if (failure.step == Stage::None && ::setns(target[1], CLONE_NEWNS) < 0)
            failure = {Stage::TargetNamespace, errno};
        if (failure.step == Stage::None &&
            (::fchdir(target[2]) < 0 || ::chroot(".") < 0 || ::chdir("/") < 0))
            failure = {Stage::TargetRoot, errno};
        // Preserve exact mounted trees before any removal. Reopening the old
        // pathname after a reinstall could instead bind the replacement inode.
        for (std::size_t i = 0; failure.step == Stage::None && i < retirements.size(); ++i) {
            rollbackTrees[i] = clone_readonly(retirements[i].old, Stage::CloneRollback,
                                              Stage::ReadOnlyRollback, i, failure);
            ::close(retirements[i].old);
        }
        std::size_t mounted = 0, removed = 0;
        // Retire children before parents. Replacing a parent removes its entire
        // old subtree, so publishing the new leaves cannot accumulate old layers.
        for (std::size_t i = 0; failure.step == Stage::None && i < retirements.size(); ++i) {
            if (::umount2(retirements[i].destination.c_str(), MNT_DETACH) < 0)
                failure = {Stage::Remove, errno, i};
            else
                ++removed;
        }
        for (std::size_t i = 0; failure.step == Stage::None && i < additions.size(); ++i) {
            if (publish(sourceTrees[i], additions[i].destination) < 0)
                failure = {Stage::Publish, errno, i};
            else
                ++mounted;
        }
        if (failure.step != Stage::None) {
            while (mounted > 0) {
                const auto i = --mounted;
                if (::umount2(additions[i].destination.c_str(), MNT_DETACH) < 0) {
                    failure.rollbackStep = Stage::UndoPublication;
                    failure.rollbackCode = errno;
                    failure.rollbackIndex = i;
                }
            }
            while (removed > 0) {
                const auto i = --removed;
                const auto& destination = retirements[i].destination;
                bool covered = false;
                for (std::size_t j = i + 1; j < retirements.size(); ++j) {
                    const auto& parent = retirements[j].destination;
                    if (restoredTrees[j] && destination.starts_with(parent + "/")) {
                        covered = true;
                        break;
                    }
                }
                // A recursively restored parent already contains its original
                // child mounts. Publishing their snapshots again would stack them.
                if (covered)
                    continue;
                if (publish(rollbackTrees[i], destination) < 0) {
                    failure.rollbackStep = Stage::Restore;
                    failure.rollbackCode = errno;
                    failure.rollbackIndex = i;
                } else {
                    restoredTrees[i] = true;
                }
            }
        }
        for (const int tree : sourceTrees)
            if (tree >= 0)
                ::close(tree);
        for (const int tree : rollbackTrees)
            if (tree >= 0)
                ::close(tree);
        (void)::write(channel[1], &failure, sizeof(failure));
        ::_exit(failure.step != Stage::None ? 125 : 0);
    }
    ::close(channel[1]);
    // The descriptor is already closed; prevent a later destructor from
    // closing an unrelated descriptor that reused its number.
    descriptors.values.back() = -1;
    pollfd reply{channel[0], POLLIN, 0};
    int ready;
    do {
        ready = ::poll(&reply, 1, 5000);
    } while (ready < 0 && errno == EINTR);
    ChildError failure{};
    const auto received = ready > 0 ? ::read(channel[0], &failure, sizeof(failure)) : -1;
    if (received != sizeof(failure))
        (void)::kill(pid, SIGKILL);
    int status{};
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (received != sizeof(failure))
        return std::unexpected("root mount update helper failed or timed out; end the session");
    if (failure.step != Stage::None) {
        const bool removal = failure.step == Stage::Remove ||
                             failure.step == Stage::CloneRollback ||
                             failure.step == Stage::ReadOnlyRollback;
        const auto& mounts = removal ? retirements : additions;
        const auto destination = failure.index < mounts.size()
                                     ? " at " + mounts[failure.index].destination
                                     : std::string{};
        const auto rollback =
            failure.rollbackStep == Stage::None
                ? std::string{}
                : std::format("; rollback {}[{}]: {} (end the session)",
                              stage_name(failure.rollbackStep), failure.rollbackIndex,
                              std::strerror(failure.rollbackCode));
        return std::unexpected(std::format("root mount update {}[{}]{}: {}{}",
                                           stage_name(failure.step), failure.index, destination,
                                           std::strerror(failure.code), rollback));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return std::unexpected("root mount update helper did not complete; end the session");
    return {};
#else
    return std::unexpected("root mount updates require Linux mount namespaces");
#endif
}
} // namespace xlings::platform::root_mount
