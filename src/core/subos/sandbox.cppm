module;

// System headers used by the sandbox backends only. `import std;` does not
// pull these in, and the named-module purview forbids including them there.
#include <cstdio>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#endif

export module xlings.core.subos.sandbox;

import std;

import xlings.core.config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.runtime;
import xlings.core.utils;
import xlings.core.xim.commands;  // auto_install_backend_ needs cmd_install
import xlings.core.subos.gpu;

// Runtime isolation for a subos: proot/bwrap backends, storage images, GPU
// passthrough, and entering an isolated session.
//
// Split out of subos.cppm, which was 2285 lines holding three unrelated
// things: which versions are active (selection), the FHS tree they are
// materialized into (view), and this. The three have different lifetimes,
// different failure modes and different correctness criteria, and naming them
// alike is why "should this state be per-subos" had no single answer.
//
// The coupling turned out to be one directory path and one environment
// variable name -- across ~800 lines this code referenced the version model
// only as `subos_dir` (somewhere to bind-mount) and XLINGS_ACTIVE_SUBOS
// (something to set in the child). There is no shared invariant, which is why
// this move is mechanical and why getting it wrong fails to compile rather
// than failing quietly at runtime.
//
// Nothing here changes behaviour, and nothing here is user-visible: the
// command surface is unchanged. `subos use <n> --sandbox` still dispatches
// here, because a sandbox is always some subos's sandbox -- promoting it to a
// top-level command would only make users repeat the subos name.
//
// Refs: .agents/docs/2026-07-31-xvm-subos-architecture-review.md D5
namespace xlings::subos::sandbox {

namespace fs = std::filesystem;

nlohmann::json read_config_json_(const fs::path& path) {
    if (!fs::exists(path)) return nlohmann::json::object();
    try {
        auto content = platform::read_file_to_string(path.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        return json.is_discarded() ? nlohmann::json::object() : json;
    } catch (...) { return nlohmann::json::object(); }
}

// ── Storage mode (V6) ──────────────────────────────────────────────
// Storage isolation mode for sandbox data. Set at subos creation time
// via `--storage <mode>`, persisted in subos/.xlings.json["storage"].
// `image` and `tmpfs` are consumed only on the sandbox path; shell-
// level entry stays env/PATH-only regardless of storage mode (the two
// axes are orthogonal per V4 design — see use_spawn_shell).
export enum class StorageMode { Shared, Image, Tmpfs };

export inline std::string storage_to_string_(StorageMode m) {
    switch (m) {
    case StorageMode::Image: return "image";
    case StorageMode::Tmpfs: return "tmpfs";
    default: return "shared";
    }
}

export inline StorageMode storage_from_string_(const std::string& s) {
    if (s == "image") return StorageMode::Image;
    if (s == "tmpfs") return StorageMode::Tmpfs;
    return StorageMode::Shared;
}

// Read storage mode from subos config file.
export StorageMode read_storage_mode_(const fs::path& subos_dir) {
    auto cfg = subos_dir / ".xlings.json";
    if (!fs::exists(cfg)) return StorageMode::Shared;
    auto json = read_config_json_(cfg);
    if (json.contains("storage") && json["storage"].is_string())
        return storage_from_string_(json["storage"].get<std::string>());
    return StorageMode::Shared;
}


// /etc/* template builders + sandbox dir layout init. uid_t / gid_t
// are POSIX types — Windows MSVC doesn't have them. Sandbox is
// Linux-only by design (proot uses ptrace + Linux syscall semantics);
// the only caller (use_sandbox_mode_) is also Linux-guarded, so we
// guard these helpers too rather than fight the type system with
// platform-portable substitutes.
#if defined(__linux__) || defined(__APPLE__)

// We write per-user passwd/group at sandbox init time so getpwuid
// (real_uid) inside the sandbox returns the real user's home
// (= /home/<user>) and shell — most CLI tools depend on this. Root
// is also included so anything that does getpwuid(0) (scripts that
// assume "root must exist") doesn't bail.
std::string make_etc_passwd_(const std::string& user, uid_t uid, gid_t gid) {
    auto home = "/home/" + user;
    // When running as root (uid 0), emit a SINGLE uid-0 entry whose home is
    // the sandbox home (/home/<user>), so getpwuid(0)->pw_dir agrees with
    // $HOME and the bind-mounted home. The historical extra
    // "root:x:0:0:root:/root" line would otherwise create a second,
    // conflicting uid-0 record that getpwuid(0) resolves to /root — an
    // empty, unbound path — breaking `cd ~`, ~/.config, and profile sourcing
    // for the root user inside the sandbox.
    if (uid == 0) {
        return std::format("{}:x:0:0:{}:{}:/bin/sh\n", user, user, home);
    }
    return std::format(
        "root:x:0:0:root:/root:/bin/sh\n"
        "{}:x:{}:{}:{}:{}:/bin/sh\n",
        user, uid, gid, user, home);
}
std::string make_etc_group_(const std::string& user, gid_t gid) {
    // Mirror passwd: a single gid-0 entry when running as root avoids a
    // duplicate group-0 record (root + <user> both gid 0).
    if (gid == 0) {
        return std::format("{}:x:0:\n", user);
    }
    return std::format(
        "root:x:0:\n"
        "{}:x:{}:\n",
        user, gid);
}
inline constexpr std::string_view kEtcHosts =
    "127.0.0.1 localhost\n::1 localhost\n";
inline constexpr std::string_view kEtcNsswitch =
    "hosts: files dns\npasswd: files\ngroup: files\n";

// Minimal user shell rc files written into <subos>/home/<user>/ at
// init. Without these, bash starts with no .bashrc / .profile (sandbox
// $HOME is fresh empty dir, dotfile isolation by design) and never
// sources the xlings profile — so the prompt pill is missing AND
// PATH doesn't pick up the per-subos bin dir. We seed minimal rc
// files that just chain into the host xlings profile; user can edit
// these to add their own customizations later (they're sandbox-private
// so they won't pollute host).
inline constexpr std::string_view kSandboxBashrc =
    "# xlings sandbox bashrc — chains to host xlings profile so PATH /\n"
    "# prompt pill / XLINGS_BIN are set up. Edit this file to add your\n"
    "# own customizations (it's sandbox-private at <subos>/home/<user>).\n"
    "if [ -r \"$HOME/.xlings/config/shell/xlings-profile.sh\" ]; then\n"
    "    . \"$HOME/.xlings/config/shell/xlings-profile.sh\"\n"
    "fi\n";
inline constexpr std::string_view kSandboxProfile =
    "# xlings sandbox profile (sourced by sh / bash login shells)\n"
    "if [ -r \"$HOME/.bashrc\" ]; then\n"
    "    . \"$HOME/.bashrc\"\n"
    "fi\n";
inline constexpr std::string_view kSandboxFishConfig =
    "# xlings sandbox fish config — chains to host xlings profile\n"
    "if test -r \"$HOME/.xlings/config/shell/xlings-profile.fish\"\n"
    "    source \"$HOME/.xlings/config/shell/xlings-profile.fish\"\n"
    "end\n";

// Initialize the sandbox-specific dirs / templates inside an existing
// subos. Idempotent: only writes files that don't yet exist, so a
// returning sandbox session won't clobber user customizations and a
// repeated `subos use --sandbox` is cheap.
void init_sandbox_dirs_(const fs::path& subos_dir,
                        const std::string& user,
                        uid_t uid, gid_t gid)
{
    auto user_home = subos_dir / "home" / user;
    fs::create_directories(user_home);
    fs::create_directories(subos_dir / "tmp");
    auto etc = subos_dir / "etc";
    fs::create_directories(etc);

    // Empty `subos/` marker dir at sandbox root. xlings's project
    // discovery walks cwd → / looking for `.xlings.json`; it stops at
    // any dir that ALSO contains a `subos/` sibling (xlings-home
    // boundary check, see config.cppm load_project_config_). Without
    // this marker, the sandbox's own `<subos>/.xlings.json` (the
    // workspace file, which appears at `/.xlings.json` inside the
    // chroot) gets misinterpreted as an anonymous-project root, which
    // OVERRIDES the per-shell XLINGS_ACTIVE_SUBOS=<name> and routes
    // all install / shim / workspace paths into a phantom
    // `<subos>/.xlings/subos/_/` tree disjoint from where PATH points.
    // Empty `<subos>/subos/` ≈ "this is an xlings-managed dir, not a
    // user project root" — boundary check terminates the walk.
    fs::create_directories(subos_dir / "subos");

    // Pseudo-root for proot chroot (`-r`). Using an empty directory
    // that is NOT the subos_dir itself avoids a proot detranslate_path
    // bug: when the subos rootfs is under `~/.xlings/` and proot
    // also has `--bind=~/.xlings:~/.xlings`, the host path
    // `~/.xlings/subos/<name>/usr` is ambiguously interpretable as
    // both "rootfs + /usr" (→ matches --bind=/usr:/usr) and "~/.xlings
    // bind + subos/<name>/usr" (→ the real subos dir). proot chooses
    // the former, causing `cd subos/<name>/usr && ls` to show host
    // /usr instead of the subos's own usr/.
    //
    // Fix: use `<subos>/sandbox-root/` (always empty, mode 0555) as
    // the proot chroot root. Since `sandbox-root/usr` never exists,
    // the detranslate prefix-strip cannot produce `/usr`, and the
    // ambiguity disappears. All real content is bind-mounted in.
    {
        auto sandbox_root = subos_dir / "sandbox-root";
        fs::create_directories(sandbox_root);
        std::error_code perm_ec;
        fs::permissions(sandbox_root,
                        fs::perms::owner_read | fs::perms::owner_exec |
                        fs::perms::group_read | fs::perms::group_exec |
                        fs::perms::others_read | fs::perms::others_exec,
                        fs::perm_options::replace, perm_ec);
    }

    auto try_write = [&](const fs::path& path, std::string_view body) {
        if (fs::exists(path)) return;
        platform::write_string_to_file(path.string(), std::string(body));
    };
    try_write(etc / "passwd", make_etc_passwd_(user, uid, gid));
    try_write(etc / "group", make_etc_group_(user, gid));
    try_write(etc / "hosts", kEtcHosts);
    try_write(etc / "nsswitch.conf", kEtcNsswitch);

    // Seed shell rc files so the xlings profile gets sourced (PATH +
    // prompt pill). Sandbox-private; user can edit freely.
    try_write(user_home / ".bashrc", kSandboxBashrc);
    try_write(user_home / ".profile", kSandboxProfile);
    auto fish_config_dir = user_home / ".config" / "fish";
    fs::create_directories(fish_config_dir);
    try_write(fish_config_dir / "config.fish", kSandboxFishConfig);
}

#endif // __linux__ / __APPLE__

// ── Image storage helpers (V6) ────────────────────────────────────

// Create a sparse ext4 image file. Idempotent.
export int init_image_(const fs::path& img, const std::string& size) {
    if (fs::exists(img)) return 0;
    auto truncate_cmd = "truncate -s " + size + " " + img.string();
    if (std::system(truncate_cmd.c_str()) != 0) return 1;
    auto mkfs_cmd = "mkfs.ext4 -F -m 0 -q " + img.string() + " 2>/dev/null";
    return std::system(mkfs_cmd.c_str());
}

// Check if a path is already a mountpoint.
export bool is_mounted_(const fs::path& path) {
    auto cmd = "mountpoint -q " + path.string() + " 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}

// Mount an image file at mountpoint. Supports multi-terminal reuse.
// After mount, chown the mountpoint root to the calling user so
// subsequent directory/file creation doesn't need sudo.
int mount_image_(const fs::path& img, const fs::path& mountpoint,
                 const std::string& user = {}) {
    fs::create_directories(mountpoint);
    if (is_mounted_(mountpoint)) return 0;  // already mounted

    // mount (privileged). priv_prefix() is "" when already root — sudo is
    // redundant and frequently absent in minimal root containers, where the
    // old hardcoded "sudo mount" died with "sudo: command not found".
    auto cmd = platform::priv_prefix() + "mount -o loop " + img.string() + " "
               + mountpoint.string();
    auto rc = std::system(cmd.c_str());
    if (rc != 0) return rc;

    // ext4 root dir is owned by root after mkfs; chown to real user
    // so sandbox init can create dirs without sudo.
    if (!user.empty()) {
        auto chown_cmd = platform::priv_prefix() + "chown " + user + ":" + user
                         + " " + mountpoint.string();
        std::system(chown_cmd.c_str());
    }
    return 0;
}

// Unmount an image file.
export int unmount_image_(const fs::path& mountpoint) {
    if (!is_mounted_(mountpoint)) return 0;
    auto cmd = platform::priv_prefix() + "umount " + mountpoint.string()
               + " 2>/dev/null";
    return std::system(cmd.c_str());
}


// ─────────────────────────────────────────────────────────────────────
// Sandbox subos entry — proot-based fs-isolated session.
//
// Triggered when `subos use <name>` reads a subos config that has the
// `sandbox-shell` field. Only available on Linux (proot uses ptrace +
// Linux syscall conventions). On non-Linux platforms the sandbox config
// is rejected at create time, so this code path is unreachable there;
// guard with #if defined(__linux__) and stub elsewhere.
//
// See docs/plans/2026-05-09-subos-sandbox-design.md for full rationale.
// ─────────────────────────────────────────────────────────────────────


// Probe order for the proot binary:
//   1. ~/.xlings/data/xpkgs/xim-x-proot/<active>/bin/proot   (xpkg-managed,
//      future once xim:proot ships)
//   2. ~/.xlings/runtimedir/proot                            (auto-fetch
//      cache, populated on first sandbox use)
//   3. PATH-resolved `proot`                                 (system pkg)
//
// Returns the path to a usable proot, or unexpected with a hint string.
std::expected<fs::path, std::string>
locate_proot_(const fs::path& home_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // (1) xpkg-managed (look at versions DB)
    //
    // Iterating with explicit directory_iterator + sentinel comparison
    // (not range-for over a wrapper) sidesteps a libstdc++ copy-ctor
    // visibility issue we hit on gcc 15.1.0 musl-cross when this header
    // is exposed via BMI: the range-for path required `directory_iterator
    // (directory_iterator const&)` to be visible at the inline expansion
    // site inside another module's TU, and the cross-toolchain link
    // surfaced it as "undefined reference to __cxx11::directory_iterator
    // ctor" in capabilities.cppm. Using sentinel `{}` comparison avoids
    // the copy entirely.
    auto xpkgs_dir = home_dir / "data" / "xpkgs";
    auto xpkg_proot_root = xpkgs_dir / "xim-x-proot";
    if (fs::is_directory(xpkg_proot_root, ec)) {
        // Sentinel form (`it != default_sentinel`) instead of comparing
        // two directory_iterators directly. Clang/libc++ only provides
        // the sentinel comparison in C++20+; comparing two
        // directory_iterators yields "invalid operands to binary
        // expression" on macOS clang. Sentinel form works on both
        // libstdc++ (Linux gcc) and libc++ (macOS clang).
        std::error_code it_ec;
        for (auto it = fs::directory_iterator(xpkg_proot_root, it_ec);
             !it_ec && it != std::default_sentinel;
             it.increment(it_ec))
        {
            auto candidate = it->path() / "bin" / "proot";
            if (fs::is_regular_file(candidate, ec)) return candidate;
        }
    }

    // (2) auto-fetch cache
    auto runtime_proot = home_dir / "runtimedir" / "proot";
    if (fs::is_regular_file(runtime_proot, ec)) return runtime_proot;

    // (3) PATH-resolved
    if (auto* path_env = std::getenv("PATH"); path_env && *path_env) {
        std::string_view pv = path_env;
        std::size_t start = 0;
        while (start <= pv.size()) {
            auto end = pv.find(':', start);
            auto seg = pv.substr(start, end == std::string_view::npos
                                        ? pv.size() - start : end - start);
            if (!seg.empty()) {
                auto candidate = fs::path(seg) / "proot";
                if (fs::is_regular_file(candidate, ec)) return candidate;
            }
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
    }

    return std::unexpected(
        "proot not found. Install via your package manager "
        "(e.g. `sudo apt install proot` / `sudo dnf install proot`) "
        "or place a proot binary at ~/.xlings/runtimedir/proot");
}

// ── Unified bind list (shared by proot + bwrap) ──────────────────────
//
// Both backends use the SAME set of host-RO paths and sandbox-private
// overrides. This ensures identical security profile (same info exposed,
// same paths isolated) regardless of backend. Only the CLI syntax
// differs: proot uses `--bind=src:dst`, bwrap uses `--ro-bind src dst`
// or `--bind src dst`.
//
// Design principle: MINIMAL host exposure. Only bind paths that are
// functionally required. Everything else stays invisible (proot: maps
// to empty <subos>/<path>; bwrap: not bound at all).
//
// See .agents/docs/sandbox-v5-dual-backend-design.md for rationale.

struct SandboxBind {
    std::string src;
    std::string dst;
    bool readonly;   // true = host RO; false = sandbox RW override
};

std::vector<SandboxBind>
sandbox_binds_(const fs::path& subos_dir,
               const fs::path& host_xlings_home,
               const std::string& user,
               StorageMode storage = StorageMode::Shared,
               const fs::path& mountpoint = {})
{
    auto etc = subos_dir / "etc";
    auto user_home = "/home/" + user;

    std::error_code ec;

    auto try_ro = [&](std::vector<SandboxBind>& v,
                      const char* src, const char* dst) {
        if (fs::exists(src, ec))
            v.push_back({src, dst, true});
    };

    std::vector<SandboxBind> binds;

    // ── Host RO: POSIX userland (tools + libs + loader) ──
    try_ro(binds, "/usr", "/usr");
    try_ro(binds, "/bin", "/bin");
    try_ro(binds, "/usr/lib", "/lib");
    try_ro(binds, "/usr/lib64", "/lib64");

    // ── Host RO: /etc (by-need) ──
    try_ro(binds, "/etc/resolv.conf", "/etc/resolv.conf");
    try_ro(binds, "/etc/ld.so.cache", "/etc/ld.so.cache");
    try_ro(binds, "/etc/ssl", "/etc/ssl");
    try_ro(binds, "/etc/pki", "/etc/pki");
    try_ro(binds, "/etc/alternatives", "/etc/alternatives");
    try_ro(binds, "/etc/localtime", "/etc/localtime");

    // ── Sandbox RW: private overrides (storage-dependent) ──
    if (storage == StorageMode::Shared) {
        binds.push_back({(subos_dir / "home").string(), "/home", false});
        binds.push_back({(subos_dir / "tmp").string(), "/tmp", false});
    } else if (storage == StorageMode::Image) {
        // home from mounted image; tmp uses bwrap --tmpfs (added in argv)
        binds.push_back({mountpoint.string(), "/home", false});
    }
    // tmpfs: home and tmp handled by bwrap --tmpfs (no bind needed)

    // xlings shared — always bound regardless of storage mode
    binds.push_back({host_xlings_home.string(), user_home + "/.xlings", false});

    // ── Sandbox: NSS templates ──
    binds.push_back({(etc / "passwd").string(), "/etc/passwd", false});
    binds.push_back({(etc / "group").string(), "/etc/group", false});
    binds.push_back({(etc / "hosts").string(), "/etc/hosts", false});
    binds.push_back({(etc / "nsswitch.conf").string(), "/etc/nsswitch.conf", false});

    return binds;
}

// Build proot argv from unified bind list. When `cmd` is non-empty, ends
// in `<shell> -c <cmd>` for non-interactive single-command exec (M3);
// otherwise just `<shell>` for the standard interactive entry.
std::vector<std::string>
build_proot_argv_(const fs::path& proot_bin,
                  const fs::path& subos_dir,
                  const fs::path& host_xlings_home,
                  const std::string& user,
                  const std::string& shell_path,
                  const std::string& cmd = "")
{
    auto user_home = "/home/" + user;
    // Use sandbox-root/ as the chroot root instead of subos_dir itself.
    // This avoids the proot detranslate_path bug where host paths under
    // subos_dir (e.g. subos/<name>/usr) get mis-translated to /usr when
    // the rootfs prefix is stripped. See init_sandbox_dirs_ for details.
    auto proot_root = subos_dir / "sandbox-root";
    std::vector<std::string> argv = {
        proot_bin.string(),
        "-r", proot_root.string(),
        "--bind=/proc:/proc",
        "--bind=/sys:/sys",
        "--bind=/dev:/dev",
    };

    for (auto& b : sandbox_binds_(subos_dir, host_xlings_home, user)) {
        argv.push_back(std::format("--bind={}:{}", b.src, b.dst));
    }

    argv.push_back(std::format("--cwd={}", user_home));
    argv.push_back(shell_path);
    if (!cmd.empty()) {
        argv.push_back("-c");
        argv.push_back(cmd);
    }
    return argv;
}

// ── bwrap backend (V5) ────────────────────────────────────────────────

// Locate bwrap binary — only searches xim:bwrap pool.
// System-installed bwrap (/usr/bin/bwrap) is intentionally skipped:
// distro packages lack setuid, and AppArmor/kernel restrictions on
// unprivileged user namespaces make system binaries unreliable on
// Ubuntu 24+, Fedora, etc. xim:bwrap is built with
// `-Dsupport_setuid=true` (xlings-res/bwrap mirror) and its install
// hook sets the binary setuid root (chmod 4755) — that combination is
// what makes the xim-managed binary work across distros where unpriv
// userns is restricted.
std::expected<fs::path, std::string>
locate_bwrap_(const fs::path& home_dir) {
    std::error_code ec;

    auto xpkg_root = home_dir / "data" / "xpkgs" / "xim-x-bwrap";
    if (fs::is_directory(xpkg_root, ec)) {
        std::error_code it_ec;
        for (auto it = fs::directory_iterator(xpkg_root, it_ec);
             !it_ec && it != std::default_sentinel;
             it.increment(it_ec))
        {
            auto candidate = it->path() / "bin" / "bwrap";
            if (fs::is_regular_file(candidate, ec)) return candidate;
        }
    }

    return std::unexpected(
        "bwrap not installed, run: xlings install bwrap");
}

// Probe whether a bwrap binary can actually create a sandbox.
// Returns {ok, output}. `output` captures stdout+stderr (the platform
// helper appends `2>&1` internally) so callers can classify the
// failure cause — see classify_bwrap_probe_error_ for the known modes.
std::pair<bool, std::string>
probe_bwrap_(const fs::path& bwrap_bin) {
    auto cmd = platform::shell_quote(bwrap_bin.string())
             + " --ro-bind / / -- /bin/true";
    auto [status, output] = platform::run_command_capture(cmd);
    return { status == 0, std::move(output) };
}

// Translate a failed bwrap probe's captured output into an actionable
// hint. Three known modes; anything else falls through to "show the
// raw stderr" so users always see the truth instead of a stale
// "xlings install bwrap" suggestion that won't help.
std::string classify_bwrap_probe_error_(const std::string& output,
                                         const fs::path& bin) {
    if (output.find("setuid use of bubblewrap is not supported")
        != std::string::npos)
    {
        return "xim:bwrap binary was built without setuid support; "
               "rebuild xlings-res/bwrap mirror with "
               "`-Dsupport_setuid=true` and bump the version\n"
               "  binary: " + bin.string();
    }
    if (output.find("setting up uid map: Permission denied")
        != std::string::npos
        || output.find("write to uid_map failed")
        != std::string::npos)
    {
        return "kernel restricts unprivileged user namespaces "
               "(LSM/AppArmor)\n"
               "  check: cat /proc/sys/kernel/"
               "apparmor_restrict_unprivileged_userns\n"
               "  workaround: sudo sysctl -w "
               "kernel.apparmor_restrict_unprivileged_userns=0";
    }
    if (output.find("clone() failed: Operation not permitted")
        != std::string::npos
        || output.find("user namespaces are not enabled")
        != std::string::npos)
    {
        return "kernel disables unprivileged_userns_clone\n"
               "  check: cat /proc/sys/kernel/"
               "unprivileged_userns_clone";
    }
    auto trimmed = output;
    while (!trimmed.empty() &&
           (trimmed.back() == '\n' || trimmed.back() == '\r'))
        trimmed.pop_back();
    return "bwrap probe failed; raw output:\n  " + trimmed;
}

// Build bwrap argv from unified bind list. Uses targeted --ro-bind
// instead of `--ro-bind / /` to match proot's security profile
// (same host paths exposed, same sandbox-private paths).
// When `cmd` is non-empty, ends with `<shell> -c <cmd>` for non-
// interactive single-command exec (M3); otherwise interactive shell.
std::vector<std::string>
build_bwrap_argv_(const fs::path& bwrap_bin,
                  const fs::path& subos_dir,
                  const fs::path& host_xlings_home,
                  const std::string& user,
                  const std::string& shell_path,
                  bool interactive_shell,
                  StorageMode storage = StorageMode::Shared,
                  const fs::path& mountpoint = {},
                  bool gpu = false,
                  const std::string& cmd = "")
{
    auto user_home = "/home/" + user;
    std::vector<std::string> argv = {
        bwrap_bin.string(),
        "--dev", "/dev",
        "--proc", "/proc",
    };

    for (auto& b : sandbox_binds_(subos_dir, host_xlings_home, user,
                                   storage, mountpoint)) {
        if (b.readonly) {
            argv.insert(argv.end(), {"--ro-bind", b.src, b.dst});
        } else {
            argv.insert(argv.end(), {"--bind", b.src, b.dst});
        }
    }

    // GPU passthrough: must come after `--dev /dev` (which creates the
    // empty tmpfs we'll mount nodes onto). Each --dev-bind is host-path
    // → sandbox-path; missing nodes are silently skipped by gpu.cppm.
    if (gpu) {
        auto extra = xlings::subos::gpu::passthrough_args();
        argv.insert(argv.end(), extra.begin(), extra.end());
    }

    // tmpfs mode: home and tmp are pure memory (exit = gone)
    if (storage == StorageMode::Tmpfs) {
        argv.insert(argv.end(), {"--tmpfs", user_home});
        argv.insert(argv.end(), {"--tmpfs", "/tmp"});
    }
    // image mode: tmp uses tmpfs (home is bind-mounted from .mountpoint)
    if (storage == StorageMode::Image) {
        argv.insert(argv.end(), {"--tmpfs", "/tmp"});
    }

    argv.insert(argv.end(), {"--chdir", user_home, "--", shell_path});
    if (!cmd.empty()) {
        // Non-interactive single-command exec: shell -c <cmd>. The -i
        // flag would print prompts/job-control warnings to captured
        // stdout, so we omit it even if interactive_shell=true.
        argv.push_back("-c");
        argv.push_back(cmd);
    } else if (interactive_shell) {
        argv.push_back("-i");
    }
    return argv;
}

// ── Backend detection + auto-install ─────────────────────────────────

enum class SandboxBackend { Bwrap, Proot };

struct BackendInfo {
    SandboxBackend type;
    fs::path binary;
};

// Show bwrap hint — only on first sandbox use of a subos (detected by
// whether <subos>/home/ exists; init_sandbox_dirs_ creates it, so if
// it's missing, this is the first --sandbox entry for this subos).
void maybe_show_bwrap_hint_(const fs::path& subos_dir) {
    std::error_code ec;
    if (fs::is_directory(subos_dir / "home", ec)) return;

    log::info("bwrap not installed or namespace probe failed");
    log::info("  to enable bwrap (recommended): xlings install bwrap");
    log::info("  using proot fallback for now");
}

std::optional<BackendInfo>
detect_backend_(const fs::path& home_dir,
                const fs::path& subos_dir = {}) {
    // 1. bwrap (xim pool) — preferred for native compat
    if (auto bin = locate_bwrap_(home_dir)) {
        if (probe_bwrap_(*bin).first)
            return BackendInfo{ SandboxBackend::Bwrap, *bin };
    }

    // bwrap found but probe failed → show hint (first use only)
    if (locate_bwrap_(home_dir) && !subos_dir.empty()) {
        maybe_show_bwrap_hint_(subos_dir);
    }

    // 2. proot — zero-privilege fallback
    if (auto bin = locate_proot_(home_dir))
        return BackendInfo{ SandboxBackend::Proot, *bin };

    return std::nullopt;
}

int auto_install_backend_(const fs::path& home_dir, EventStream& stream) {
    // Try bwrap first (build from source, config hook sets setuid)
    log::info("installing sandbox backend...");
    {
        std::vector<std::string> t = {"xim:bwrap"};
        xim::cmd_install(t, /*yes=*/true, /*noDeps=*/false, stream);
        auto bin = locate_bwrap_(home_dir);
        if (bin && probe_bwrap_(*bin).first) return 0;
    }

    // bwrap installed but namespace restricted → proot fallback
    // (hint already shown by detect_backend_ on first use)

    std::vector<std::string> t = {"xim:proot"};
    return xim::cmd_install(t, /*yes=*/true, /*noDeps=*/false, stream);
}


// V5 sandbox entry. Reached via `xlings subos use <name> --sandbox [backend]`.
// Linux-only. Dual backend: bwrap (preferred, native compat) or proot
// (fallback, zero-privilege). Auto-detects which is available; user can
// force one via `--sandbox bwrap` or `--sandbox proot`.
//
// See .agents/docs/sandbox-v5-dual-backend-design.md for full design.
export int enter(const std::string& name, EventStream& stream,
                      const std::string& preferred_backend = "",
                      bool gpu = false,
                      const std::string& cmd = "") {

    // Refuse nested sandbox entry.
    if (utils::get_env_or_default("XLINGS_SUBOS_MODE") == "sandbox") {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "cannot enter sandbox from inside another sandbox",
            .recoverable = true,
            .hint = "type 'exit' first to leave the current one",
        });
        return 1;
    }

    auto& p = Config::paths();
    auto subos_dir = p.homeDir / "subos" / name;

    // ── Storage mode (V6) ──
    auto storage = read_storage_mode_(subos_dir);

    // ── Resolve user ──
#if defined(_WIN32)
    auto user = utils::get_env_or_default("USERNAME");
    if (user.empty()) user = "user";
#else
    auto user = utils::get_env_or_default("USER");
    if (user.empty()) user = "user";
#endif

    auto sandbox_home = (subos_dir / "home" / user).string();
    auto sandbox_tmp = (subos_dir / "tmp").string();

    // Image mountpoint (populated in the Linux block below if image mode)
    fs::path image_mountpoint;
    if (storage == StorageMode::Image)
        image_mountpoint = subos_dir / ".mountpoint";

    // ── Lazy init sandbox dirs (shared mode only) ──
    if (storage == StorageMode::Shared) {
        fs::create_directories(subos_dir / "home" / user);
        fs::create_directories(subos_dir / "tmp");
    }

#if defined(__linux__) || defined(__APPLE__)
    // Seed shell rc files so profile is sourced (prompt pill + PATH).
    // For image mode, rc files go into the mounted image (deferred until
    // after mount in the Linux block below). For shared/tmpfs, seed now.
    if (storage == StorageMode::Shared) {
        auto user_home_dir = subos_dir / "home" / user;
        fs::create_directories(user_home_dir);
        auto try_write = [](const fs::path& path, std::string_view body) {
            if (fs::exists(path)) return;
            platform::write_string_to_file(path.string(), std::string(body));
        };
        try_write(user_home_dir / ".bashrc",
            "# xlings sandbox bashrc\n"
            "if [ -r \"$HOME/.xlings/config/shell/xlings-profile.sh\" ]; then\n"
            "    . \"$HOME/.xlings/config/shell/xlings-profile.sh\"\n"
            "fi\n");
        try_write(user_home_dir / ".profile",
            "# xlings sandbox profile\n"
            "if [ -r \"$HOME/.bashrc\" ]; then . \"$HOME/.bashrc\"; fi\n");
        auto fish_dir = user_home_dir / ".config" / "fish";
        fs::create_directories(fish_dir);
        try_write(fish_dir / "config.fish",
            "# xlings sandbox fish config\n"
            "if test -r \"$HOME/.xlings/config/shell/xlings-profile.fish\"\n"
            "    source \"$HOME/.xlings/config/shell/xlings-profile.fish\"\n"
            "end\n");
        try_write(user_home_dir / ".zshrc",
            "# xlings sandbox zshrc\n"
            "if [ -r \"$HOME/.xlings/config/shell/xlings-profile.sh\" ]; then\n"
            "    . \"$HOME/.xlings/config/shell/xlings-profile.sh\"\n"
            "fi\n");
    }
#endif

    // ── Common env (all platforms) ──
    platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
    platform::set_env_variable("XLINGS_SUBOS_MODE", "sandbox");

    nlohmann::json payload;
    payload["name"] = name;
    payload["mode"] = "sandbox";

    std::cout.flush();
    std::cerr.flush();

#if defined(__linux__)
    // ═══════════════════════════════════════════════════════════════
    // Linux L3: FS view isolation via bwrap/proot
    // ═══════════════════════════════════════════════════════════════

    // image/tmpfs storage requires bwrap (mount namespace needed)
    if (storage != StorageMode::Shared && !preferred_backend.empty()
        && preferred_backend == "proot") {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "image/tmpfs storage requires bwrap sandbox backend",
            .recoverable = false,
            .hint = "run: xlings install bwrap",
        });
        return 1;
    }

    // ── Image mode: mount image before sandbox entry ──
    if (storage == StorageMode::Image) {
        auto img = subos_dir / "home.img";
        if (!fs::exists(img)) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "home.img not found — was this subos created with --storage image?",
                .recoverable = false,
            });
            return 1;
        }
        auto rc = mount_image_(img, image_mountpoint, user);
        if (rc != 0) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Internal,
                .message = "failed to mount home.img",
                .recoverable = false,
                .hint = "ensure you have sudo permission for mount",
            });
            return 1;
        }
    }

    // /etc templates for Linux sandbox (NSS)
    {
        auto uid = ::getuid();
        auto gid = ::getgid();
        init_sandbox_dirs_(subos_dir, user, uid, gid);
        // For image mode, init home and seed shell rc files inside the mount
        if (storage == StorageMode::Image) {
            auto mp_home = image_mountpoint / user;
            fs::create_directories(mp_home);
            auto try_write = [](const fs::path& path, std::string_view body) {
                if (fs::exists(path)) return;
                platform::write_string_to_file(path.string(), std::string(body));
            };
            try_write(mp_home / ".bashrc",
                "# xlings sandbox bashrc\n"
                "if [ -r \"$HOME/.xlings/config/shell/xlings-profile.sh\" ]; then\n"
                "    . \"$HOME/.xlings/config/shell/xlings-profile.sh\"\n"
                "fi\n");
            try_write(mp_home / ".profile",
                "# xlings sandbox profile\n"
                "if [ -r \"$HOME/.bashrc\" ]; then . \"$HOME/.bashrc\"; fi\n");
            auto fish_dir = mp_home / ".config" / "fish";
            fs::create_directories(fish_dir);
            try_write(fish_dir / "config.fish",
                "# xlings sandbox fish config\n"
                "if test -r \"$HOME/.xlings/config/shell/xlings-profile.fish\"\n"
                "    source \"$HOME/.xlings/config/shell/xlings-profile.fish\"\n"
                "end\n");
            try_write(mp_home / ".zshrc",
                "# xlings sandbox zshrc\n"
                "if [ -r \"$HOME/.xlings/config/shell/xlings-profile.sh\" ]; then\n"
                "    . \"$HOME/.xlings/config/shell/xlings-profile.sh\"\n"
                "fi\n");
        }
    }

    // Backend selection (the using-declarations that stood here named
    // sandbox_detail_:: and are redundant now that this IS that namespace)
    std::optional<BackendInfo> backend;

    if (preferred_backend == "bwrap") {
        auto bin = locate_bwrap_(p.homeDir);
        if (!bin) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "bwrap not installed",
                .recoverable = false,
                .hint = "run: xlings install bwrap",
            });
            return 1;
        }
        if (auto probe = probe_bwrap_(*bin); !probe.first) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "bwrap probe failed",
                .recoverable = false,
                .hint = classify_bwrap_probe_error_(
                            probe.second, *bin),
            });
            return 1;
        }
        backend = BackendInfo{ SandboxBackend::Bwrap, *bin };
    } else if (preferred_backend == "proot") {
        auto bin = locate_proot_(p.homeDir);
        if (!bin) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = std::move(bin).error(),
                .recoverable = false,
            });
            return 1;
        }
        backend = BackendInfo{ SandboxBackend::Proot, *bin };
    } else {
        backend = detect_backend_(p.homeDir, subos_dir);
        if (!backend) {
            auto rc = auto_install_backend_(p.homeDir, stream);
            if (rc != 0) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::NotFound,
                    .message = "failed to install sandbox backend",
                    .recoverable = false,
                    .hint = "manually: xlings install bwrap (or: xlings install proot)",
                });
                return 1;
            }
            backend = detect_backend_(p.homeDir, subos_dir);
            if (!backend) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::NotFound,
                    .message = "no sandbox backend available after install attempt",
                    .recoverable = false,
                });
                return 1;
            }
        }
    }

    // image/tmpfs requires bwrap — reject proot after auto-detect
    if (storage != StorageMode::Shared
        && backend->type != SandboxBackend::Bwrap) {
        // If bwrap is actually installed but probe failed (the common
        // case that triggers the proot fallback), surface the real
        // probe error rather than the misleading "xlings install bwrap"
        // suggestion that won't help.
        std::string hint = "run: xlings install bwrap";
        if (auto bin = locate_bwrap_(p.homeDir)) {
            if (auto probe = probe_bwrap_(*bin);
                !probe.first)
            {
                hint = classify_bwrap_probe_error_(
                            probe.second, *bin);
            }
        }
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = storage_to_string_(storage)
                       + " storage requires bwrap (proot does not support mount namespace)",
            .recoverable = false,
            .hint = std::move(hint),
        });
        if (storage == StorageMode::Image)
            unmount_image_(image_mountpoint);
        return 1;
    }

    auto backend_name = (backend->type == SandboxBackend::Bwrap) ? "bwrap" : "proot";
    log::debug("sandbox backend: {} storage: {}", backend_name,
               storage_to_string_(storage));

    auto user_home = "/home/" + user;
    auto shell = utils::get_env_or_default("SHELL");
    if (shell.empty()) shell = "/bin/sh";

    payload["backend"] = backend_name;
    payload["shell"] = shell;
    payload["storage"] = storage_to_string_(storage);
    stream.emit(DataEvent{"subos_entering", payload.dump()});

    platform::set_env_variable("HOME", user_home);
    platform::set_env_variable("PATH", std::format(
        "{}/.xlings/subos/{}/bin:{}/.xlings/bin:/usr/local/bin:/usr/bin:/bin",
        user_home, name, user_home));
    // bwrap `sh -i` prints prompts/job-control warnings into piped CI
    // commands; keep `-i` only for real terminal sessions.
    const bool interactive_shell = ::isatty(STDIN_FILENO) == 1;

    std::vector<std::string> argv;
    if (backend->type == SandboxBackend::Bwrap) {
        argv = build_bwrap_argv_(
            backend->binary, subos_dir, p.homeDir, user, shell,
            interactive_shell, storage, image_mountpoint, gpu, cmd);
    } else {
        // proot already exposes the full host /dev and /sys via
        // `--bind=/dev:/dev --bind=/sys:/sys`, so GPU devices are
        // visible for free. --gpu is a no-op here.
        argv = build_proot_argv_(
            backend->binary, subos_dir, p.homeDir, user, shell, cmd);
        platform::set_env_variable("PROOT_NO_SECCOMP", "1");
    }

    std::vector<char*> c_argv;
    c_argv.reserve(argv.size() + 1);
    for (auto& s : argv) c_argv.push_back(const_cast<char*>(s.c_str()));
    c_argv.push_back(nullptr);

    // Image mode: fork+exec+wait so we can unmount after sandbox exits.
    // Shared/tmpfs: execvp replaces the process (no cleanup needed).
    if (storage == StorageMode::Image) {
        auto pid = ::fork();
        if (pid < 0) {
            log::error("fork failed: {}", std::strerror(errno));
            unmount_image_(image_mountpoint);
            return 1;
        }
        if (pid == 0) {
            // Child: enter sandbox
            ::execvp(c_argv[0], c_argv.data());
            log::error("failed to exec {}: {}", backend_name, std::strerror(errno));
            ::_exit(127);
        }
        // Parent: wait then unmount
        int status = 0;
        ::waitpid(pid, &status, 0);
        unmount_image_(image_mountpoint);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }

    ::execvp(c_argv[0], c_argv.data());
    log::error("failed to exec {} '{}': {}", backend_name,
               backend->binary.string(), std::strerror(errno));
    return 127;

#elif defined(__APPLE__)
    // ═══════════════════════════════════════════════════════════════
    // macOS L2: HOME redirect (dotfile isolation)
    // ═══════════════════════════════════════════════════════════════

    auto shell = utils::get_env_or_default("SHELL");
    if (shell.empty()) shell = "/bin/zsh";  // macOS default

    payload["backend"] = "home-redirect";
    payload["shell"] = shell;
    stream.emit(DataEvent{"subos_entering", payload.dump()});

    platform::set_env_variable("HOME", sandbox_home);
    platform::set_env_variable("TMPDIR", sandbox_tmp);
    platform::set_env_variable("XDG_CONFIG_HOME", sandbox_home + "/.config");
    platform::set_env_variable("XDG_DATA_HOME", sandbox_home + "/.local/share");
    platform::set_env_variable("XDG_CACHE_HOME", sandbox_home + "/.cache");
    platform::set_env_variable("XDG_STATE_HOME", sandbox_home + "/.local/state");

    ::execl(shell.c_str(), shell.c_str(), "-i", static_cast<char*>(nullptr));
    log::error("failed to exec shell '{}': {}", shell, std::strerror(errno));
    return 127;

#elif defined(_WIN32)
    // ═══════════════════════════════════════════════════════════════
    // Windows L2: USERPROFILE redirect (dotfile isolation)
    // ═══════════════════════════════════════════════════════════════

    // Pre-create AppData structure
    fs::create_directories(fs::path(sandbox_home) / "AppData" / "Roaming");
    fs::create_directories(fs::path(sandbox_home) / "AppData" / "Local");

    payload["backend"] = "home-redirect";
    stream.emit(DataEvent{"subos_entering", payload.dump()});

    platform::set_env_variable("USERPROFILE", sandbox_home);
    platform::set_env_variable("APPDATA", sandbox_home + "\\AppData\\Roaming");
    platform::set_env_variable("LOCALAPPDATA", sandbox_home + "\\AppData\\Local");
    platform::set_env_variable("TEMP", sandbox_tmp);
    platform::set_env_variable("TMP", sandbox_tmp);
    platform::set_env_variable("XDG_CONFIG_HOME", sandbox_home + "\\.config");
    platform::set_env_variable("XDG_DATA_HOME", sandbox_home + "\\.local\\share");
    platform::set_env_variable("XDG_CACHE_HOME", sandbox_home + "\\.cache");

    // Windows: CreateProcess + WaitForSingleObject (same as use_spawn_shell)
    constexpr const char* shells[] = { "pwsh.exe", "powershell.exe", "cmd.exe" };
    for (auto* exe : shells) {
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::string cmdline = exe;
        if (::CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                             TRUE, 0, nullptr, nullptr, &si, &pi)) {
            ::WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD exitCode = 0;
            ::GetExitCodeProcess(pi.hProcess, &exitCode);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            return static_cast<int>(exitCode);
        }
    }
    log::error("could not launch any shell on Windows");
    return 127;
#endif
}

}  // namespace xlings::subos::sandbox
