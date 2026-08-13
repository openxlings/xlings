module;

// System headers used by the sandbox backends only. `import std;` does not
// pull these in, and the named-module purview forbids including them there.
#include <cstdio>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// See src/core/subos.cppm: windows.h's min/max macros break std::min({...}).
// Not yet triggered here, and that is exactly why it is worth closing --
// the failure only appears on Windows, and only once someone writes the call.
#define NOMINMAX
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
import xlings.core.xim.compatibility;
import xlings.core.subos.gpu;
import xlings.core.xvm.shim;   // resolve_owner_home: reject another home's shim

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

nlohmann::json read_config_json_(const fs::path& path);

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
export StorageMode read_storage_mode_(const fs::path& subos_dir);


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
std::string make_etc_passwd_(const std::string& user, uid_t uid, gid_t gid);
std::string make_etc_group_(const std::string& user, gid_t gid);
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
// $XLINGS_HOME, not $HOME/.xlings.
//
// The sandbox already carries XLINGS_HOME in, correctly. These rc files did
// not read it, so a subos living in a non-default home sourced the DEFAULT
// home's profile and came up with `PATH[0]=$HOME/.xlings/subos/<name>/bin` —
// a directory that does not exist there. The shell starts fine and the first
// command reports `cannot execute: required file not found`, naming a path
// the user never asked for.
//
// Two readers of "which home is this", one taking the environment and one
// assuming the default. They agree for everybody using ~/.xlings, which is
// why it went unnoticed.
inline constexpr std::string_view kSandboxBashrc =
    "# xlings sandbox bashrc — chains to the xlings profile of THIS home so\n"
    "# PATH / prompt pill / XLINGS_BIN are set up. Edit this file to add your\n"
    "# own customizations (it's sandbox-private at <subos>/home/<user>).\n"
    "_xlings_profile=\"${XLINGS_HOME:-$HOME/.xlings}/config/shell/xlings-profile.sh\"\n"
    "if [ -r \"$_xlings_profile\" ]; then\n"
    "    . \"$_xlings_profile\"\n"
    "fi\n"
    "unset _xlings_profile\n";
inline constexpr std::string_view kSandboxProfile =
    "# xlings sandbox profile (sourced by sh / bash login shells)\n"
    "if [ -r \"$HOME/.bashrc\" ]; then\n"
    "    . \"$HOME/.bashrc\"\n"
    "fi\n";
inline constexpr std::string_view kSandboxFishConfig =
    "# xlings sandbox fish config — chains to the xlings profile of THIS home\n"
    "set -l _xlings_home $XLINGS_HOME\n"
    "test -n \"$_xlings_home\"; or set _xlings_home \"$HOME/.xlings\"\n"
    "if test -r \"$_xlings_home/config/shell/xlings-profile.fish\"\n"
    "    source \"$_xlings_home/config/shell/xlings-profile.fish\"\n"
    "end\n";


// Every shell's rc for one sandbox home, written from ONE set of templates.
//
// These files existed in three places with three copies of the same text, and
// the copies drifted: fixing the profile path in one left the other two
// pointing at `$HOME/.xlings`, so a subos in a non-default home still sourced
// the wrong profile. Duplication was not the bug, but it is what made the bug
// survive a fix.
inline void write_sandbox_rc_(const fs::path& home_dir) {
    auto try_write = [](const fs::path& path, std::string_view body) {
        if (fs::exists(path)) return;   // never clobber user edits
        platform::write_string_to_file(path.string(), std::string(body));
    };
    fs::create_directories(home_dir);
    try_write(home_dir / ".bashrc",  kSandboxBashrc);
    try_write(home_dir / ".zshrc",   kSandboxBashrc);
    try_write(home_dir / ".profile", kSandboxProfile);
    auto fish_dir = home_dir / ".config" / "fish";
    fs::create_directories(fish_dir);
    try_write(fish_dir / "config.fish", kSandboxFishConfig);
}

// Initialize the sandbox-specific dirs / templates inside an existing
// subos. Idempotent: only writes files that don't yet exist, so a
// returning sandbox session won't clobber user customizations and a
// repeated `subos use --sandbox` is cheap.
void init_sandbox_dirs_(const fs::path& subos_dir,
                        const std::string& user,
                        uid_t uid, gid_t gid);

#endif // __linux__ / __APPLE__

// ── Image storage helpers (V6) ────────────────────────────────────

// Create a sparse ext4 image file. Idempotent.
export int init_image_(const fs::path& img, const std::string& size);

// Check if a path is already a mountpoint.
export bool is_mounted_(const fs::path& path);

// Mount an image file at mountpoint. Supports multi-terminal reuse.
// After mount, chown the mountpoint root to the calling user so
// subsequent directory/file creation doesn't need sudo.
int mount_image_(const fs::path& img, const fs::path& mountpoint,
                 const std::string& user = {});

// Unmount an image file.
export int unmount_image_(const fs::path& mountpoint);


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
//   1. <home>/data/xpkgs/xim-x-proot/<ver>/bin/proot   the PAYLOAD
//   2. <home>/runtimedir/proot                         auto-fetch cache
//   3. /usr/bin/proot, /usr/local/bin/proot            the HOST (reported)
//
// PATH is deliberately absent -- see (3) below. The same rule, in the same
// shape, as libxpkg's elfpatch tool lookup: payload, then a named fallback
// that says so. Both implement R6 (internal consumers bind the payload, not
// the view), .agents/docs/2026-08-06-subos-architecture-proposal.md §1.5.
//
// Returns the path to a usable proot, or unexpected with a hint string.
std::expected<fs::path, std::string>
locate_proot_(const fs::path& home_dir);

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
               const fs::path& mountpoint = {});

// Build proot argv from unified bind list. When `cmd` is non-empty, ends
// in `<shell> -c <cmd>` for non-interactive single-command exec (M3);
// otherwise just `<shell>` for the standard interactive entry.
std::vector<std::string>
build_proot_argv_(const fs::path& proot_bin,
                  const fs::path& subos_dir,
                  const fs::path& host_xlings_home,
                  const std::string& user,
                  const std::string& shell_path,
                  const std::string& cmd = "");

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
locate_bwrap_(const fs::path& home_dir);

// Probe whether a bwrap binary can actually create a sandbox.
// Returns {ok, output}. `output` captures stdout+stderr (the platform
// helper appends `2>&1` internally) so callers can classify the
// failure cause — see classify_bwrap_probe_error_ for the known modes.
std::pair<bool, std::string>
probe_bwrap_(const fs::path& bwrap_bin);

// Translate a failed bwrap probe's captured output into an actionable
// hint. Three known modes; anything else falls through to "show the
// raw stderr" so users always see the truth instead of a stale
// "xlings install bwrap" suggestion that won't help.
std::string classify_bwrap_probe_error_(const std::string& output,
                                         const fs::path& bin);

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
                  const std::string& cmd = "");

// ── Backend detection + auto-install ─────────────────────────────────

enum class SandboxBackend { Bwrap, Proot };

struct BackendInfo {
    SandboxBackend type;
    fs::path binary;
};

// Show bwrap hint — only on first sandbox use of a subos (detected by
// whether <subos>/home/ exists; init_sandbox_dirs_ creates it, so if
// it's missing, this is the first --sandbox entry for this subos).
void maybe_show_bwrap_hint_(const fs::path& subos_dir);

std::optional<BackendInfo>
detect_backend_(const fs::path& home_dir,
                const fs::path& subos_dir = {});

// Should we even try to fetch a sandbox backend for this host? Answered from
// the on-disk index, with no network access.
//
// The evidence rule here is deliberately stricter than the one `xlings
// install` uses, and the difference is who asked. A package-level `archs`
// union is weak evidence -- it went unenforced for all of spec V1 and is
// routinely under-declared -- so it must never veto an install the USER named:
// being wrong there means refusing a package that works, and the user has no
// way around it.
//
// This install is one nobody asked for. `--sandbox` triggers it implicitly,
// and being wrong costs only a skipped auto-install that
// `xlings install bwrap` still overrides. Being wrong the other way costs
// every `--sandbox` invocation on the arch a download that 404s, ~45s of
// waiting, and a diagnostic that blames the installer rather than naming the
// target. So a weak signal is enough to decline here.
//
// What it is NOT is a compile-time `#if defined(__aarch64__)`, which is what
// this replaced: that freezes a claim about the package index into the binary,
// so the day an aarch64 backend is published every client in the field still
// refuses. This is re-read from the index on every call.
std::optional<std::string> backend_target_unavailable_();

int auto_install_backend_(const fs::path& home_dir, EventStream& stream);


// V5 sandbox entry. Reached via `xlings subos use <name> --sandbox [backend]`.
// Linux-only. Dual backend: bwrap (preferred, native compat) or proot
// (fallback, zero-privilege). Auto-detects which is available; user can
// force one via `--sandbox bwrap` or `--sandbox proot`.
//
// See .agents/docs/sandbox-v5-dual-backend-design.md for full design.
export int enter(const std::string& name, EventStream& stream,
                      const std::string& preferred_backend = "",
                      bool gpu = false,
                      const std::string& cmd = "");

}  // namespace xlings::subos::sandbox
