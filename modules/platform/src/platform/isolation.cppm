// The kernel's isolation mechanisms, as calls (Linux; the SubOS design §16-
// §20 decides when each is used). Every one is declared everywhere and
// reports "unavailable" where the kernel does not have it, so a caller
// branches with `if constexpr (platform::is_linux)` or on the result -- never
// with an #if or a header of its own.
//
// The kernel ABIs (Landlock, seccomp, openat2) are written out in the
// implementation rather than taken from <linux/*.h>: the static musl
// toolchains' kernel headers can predate them, and the numbers are the
// kernel's stable interface.
export module xlings.platform:isolation;

import std;

export namespace xlings::platform {

// ── namespaces ───────────────────────────────────────────────────────

// In this process: a user namespace mapping this user to itself (not to
// root) and a network namespace it owns -- one an unprivileged pasta can
// join, which it cannot do for one bwrap made.
bool enter_private_network(unsigned uid, unsigned gid);

// ── Landlock: a write fence without namespaces ───────────────────────

namespace landlock {
// 0 when the kernel has no Landlock, or it is not in the LSM list.
int abi();
// From here on this process and its children may write only beneath `rw`
// (paths that do not exist grant nothing). Sets no_new_privs.
std::expected<void, std::string> restrict_writes(std::span<const std::filesystem::path> rw);
}  // namespace landlock

// ── seccomp ──────────────────────────────────────────────────────────

namespace seccomp {
// `ioctl(TIOCSTI | TIOCLINUX)` refused with EPERM, as the bytes bwrap reads
// from `--seccomp <fd>`. Empty on an architecture this does not know.
std::vector<std::uint8_t> block_terminal_injection();
std::size_t instruction_count(std::span<const std::uint8_t> program);

// execve / execveat turned into user notifications, installed in this
// process (inherited by everything it starts); the listener, or -1.
int exec_listener();
struct ExecNotice {
    int pid { 0 };
    std::string path;           // empty when it could not be read safely
};
// The next exec the listener holds: its path read from the caller, the call
// let through. nullopt when there was nothing to read.
std::optional<ExecNotice> next_exec(int listener);
}  // namespace seccomp

// ── the filesystem, beneath a root ───────────────────────────────────
//
// Paths under `root` are resolved without following a link at any level
// (openat2 RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS, a per-component O_NOFOLLOW
// walk before Linux 5.6). A link at the destination is replaced, a linked
// directory on the way refused, and a link copied out is copied as a link.
// Elsewhere: std::filesystem's copy.

// host `src` -> `root` / `rel` (a file, a directory's contents; `rel` ending
// in `/` or naming a directory of its own receives `src` inside it).
std::expected<void, std::string> copy_into_beneath(const std::filesystem::path& src,
                                                   const std::filesystem::path& root,
                                                   const std::filesystem::path& rel);
// `root` / `rel` -> host `dst`.
std::expected<void, std::string> copy_out_of_beneath(const std::filesystem::path& root,
                                                     const std::filesystem::path& rel,
                                                     const std::filesystem::path& dst);

// ── facts ────────────────────────────────────────────────────────────

struct FileOwnership {
    unsigned uid { 0 };
    unsigned mode { 0 };        // st_mode, type and permission bits
};
std::optional<FileOwnership> file_ownership(const std::filesystem::path& path);

// `uname -r`; empty where there is none.
std::string kernel_release();

}  // namespace xlings::platform
