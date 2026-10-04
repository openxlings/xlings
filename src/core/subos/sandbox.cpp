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
#if defined(__APPLE__)
#include <crt_externs.h>
#define XLINGS_ENVIRON (*_NSGetEnviron())
#else
// glibc and musl declare environ in <unistd.h> under _GNU_SOURCE
#define XLINGS_ENVIRON environ
#endif
#endif

module xlings.core.subos.sandbox;

import std;
import xlings.core.config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.runtime;
import xlings.core.utils;
import xlings.core.xim.commands;
import xlings.core.xim.compatibility;
import xlings.subos.gpu;
import xlings.core.xvm.shim;
import xlings.subos.home_view;
import xlings.subos.ports;
import xlings.subos.policy;
import xlings.subos.caps;
import xlings.subos.spec;
import xlings.subos.provider;
import xlings.subos.seccomp;
import xlings.subos.session;
import xlings.core.elfread;
import xlings.core.subos.ports;

namespace xlings::subos::sandbox {

nlohmann::json read_config_json_(const fs::path& path) {
    if (!fs::exists(path)) return nlohmann::json::object();
    try {
        auto content = platform::read_file_to_string(path.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        return json.is_discarded() ? nlohmann::json::object() : json;
    } catch (...) { return nlohmann::json::object(); }
}

StorageMode read_storage_mode_(const fs::path& subos_dir) {
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
    write_sandbox_rc_(user_home);
    auto fish_config_dir = user_home / ".config" / "fish";
    fs::create_directories(fish_config_dir);
    try_write(fish_config_dir / "config.fish", kSandboxFishConfig);
}

#endif // __linux__ / __APPLE__

int init_image_(const fs::path& img, const std::string& size) {
    if (fs::exists(img)) return 0;
    auto truncate_cmd = "truncate -s " + size + " " + img.string();
    if (std::system(truncate_cmd.c_str()) != 0) return 1;
    auto mkfs_cmd = "mkfs.ext4 -F -m 0 -q " + img.string() + " 2>/dev/null";
    return std::system(mkfs_cmd.c_str());
}

bool is_mounted_(const fs::path& path) {
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

int unmount_image_(const fs::path& mountpoint) {
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


// Translate a failed bwrap probe into what happened and what to do about it.
//
// The raw output always comes first: "bwrap failed" without bwrap's own words
// is how #640 was misdiagnosed. The remedies are in least-privilege order and
// never include turning off the kernel's user-namespace restriction for the
// whole machine -- that was the old advice, and it trades every other
// program's protection for this one's convenience.
std::string classify_bwrap_probe_error_(const std::string& output,
                                         const fs::path& bin) {
    auto first_line = output.substr(0, output.find('\n'));
    while (!first_line.empty() && (first_line.back() == '\r' || first_line.back() == ' '))
        first_line.pop_back();
    std::string out = "bwrap cannot create a sandbox here: " + first_line
                      + "\n  binary: " + bin.string();

    if (output.find("setuid use of bubblewrap is not supported") != std::string::npos) {
        out += "\n  this bwrap is setuid but was built without setuid support;"
               " xlings no longer uses setuid bwrap";
    } else if (output.find("uid map") != std::string::npos
               || output.find("Permission denied") != std::string::npos
               || output.find("Operation not permitted") != std::string::npos
               || output.find("user namespaces are not enabled") != std::string::npos) {
        // Name the restriction that is in force, read from the kernel, so the
        // reader does not have to guess which of the two it is.
        auto read_sysctl = [](const char* path) -> std::string {
            std::ifstream in(path);
            std::string v;
            std::getline(in, v);
            return v;
        };
        auto apparmor = read_sysctl("/proc/sys/kernel/apparmor_restrict_unprivileged_userns");
        auto clone = read_sysctl("/proc/sys/kernel/unprivileged_userns_clone");
        if (apparmor == "1")
            out += "\n  cause: AppArmor restricts unprivileged user namespaces for "
                   "programs without a profile (kernel.apparmor_restrict_unprivileged_userns=1)";
        else if (clone == "0")
            out += "\n  cause: the kernel disables unprivileged user namespaces "
                   "(kernel.unprivileged_userns_clone=0)";
        else
            out += "\n  cause: user namespaces are not available to this user";
    }
    out += "\n  fix, least privilege first:"
           "\n    1. xlings self doctor --isolation --fix"
           "   (one sudo: a root-owned bwrap with a narrow AppArmor profile)"
           "\n    2. --sandbox proot   (works anywhere; a view, not a security boundary)";
    return out;
}

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
std::optional<std::string> backend_target_unavailable_() {
    auto& catalog = xim::get_catalog(xim::CatalogAccess::LocalOnly);
    if (!catalog.is_loaded()) return std::nullopt;

    const auto hostArch = xim::host_architecture();
    std::string target;
    for (const auto* name : {"bwrap", "proot"}) {
        auto match = catalog.resolve_target(name, "linux");
        if (!match) return std::nullopt;          // cannot say -> let it try
        auto pkg = catalog.load_package(*match);
        if (!pkg) return std::nullopt;
        const auto* entry = xim::find_entry(*pkg, "linux", match->version);
        const auto compatibility = xim::check_target_compatibility(
            *pkg, entry, "linux", hostArch);
        target = compatibility.target;
        // Supported AND not merely "we could not tell": an advisory means the
        // recipe does not claim this arch, which is as much as an
        // auto-install needs to hear.
        if (compatibility.supported && compatibility.advisory.empty()) {
            return std::nullopt;
        }
    }
    return std::format(
        "E_UNSUPPORTED_TARGET: no sandbox backend has a {} artifact", target);
}


int auto_install_backend_(const fs::path& home_dir, EventStream& stream) {
    // bwrap first; when it installs but its probe fails (user namespaces
    // restricted), proot is the fallback.
    log::info("installing sandbox backend...");
    auto home = subos::home_view();
    auto ports = subos::make_ports(stream);
    (void)home_dir;
    if (ports.install_backend("bwrap")) {
        if (auto b = caps::locate_bwrap(home)) {
            caps::probe_bwrap(*b);
            if (b->usable) return 0;
        }
    }
    return ports.install_backend("proot") ? 0 : 1;
}

namespace {

spec::Storage to_spec_storage_(StorageMode m) {
    switch (m) {
    case StorageMode::Image: return spec::Storage::Image;
    case StorageMode::Tmpfs: return spec::Storage::Tmpfs;
    default:                 return spec::Storage::Shared;
    }
}

std::map<std::string, std::string> current_env_() {
    std::map<std::string, std::string> env;
#if !defined(_WIN32)
    for (char** e = XLINGS_ENVIRON; e && *e; ++e) {
        std::string_view kv(*e);
        auto eq = kv.find('=');
        if (eq != std::string_view::npos)
            env[std::string(kv.substr(0, eq))] = std::string(kv.substr(eq + 1));
    }
#endif
    return env;
}

// The refusal as the one ErrorEvent the entry reports. Wording kept from the
// code this replaced; the remedy is the compiler's.
void emit_refusal_(EventStream& stream, const spec::Refusal& refusal) {
    const auto& u = refusal.missing.front();
    stream.emit(ErrorEvent{
        .code = u.dimension == "storage" ? ErrorCode::InvalidInput : ErrorCode::NotFound,
        .message = u.reason,
        .recoverable = false,
        .hint = u.fix.empty() ? std::string{} : "run: " + u.fix,
    });
}


// Where session-init (this binary) lives inside every sandbox.
constexpr std::string_view kSessionInitPath = "/run/xlings/xlings";

// What a sandbox must be compared on to decide whether a call may join the
// running session: everything that isolates, nothing that varies per call.
std::string spec_digest_(const spec::SandboxSpec& sb) {
    auto j = sb.describe();
    for (auto k : {"argv", "env", "cwd", "degraded"}) j.erase(k);
    return std::format("{:016x}", std::hash<std::string>{}(j.dump()));
}

#if defined(__linux__)
// This binary at kSessionInitPath, plus -- for a dynamically linked build --
// its ELF interpreter's directory and its RUNPATH, read-only at their own
// paths. A static release binary needs neither.
std::vector<spec::MountOp> self_exe_mounts_() {
    std::vector<spec::MountOp> out;
    std::error_code ec;
    auto exe = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return out;
    out.push_back({spec::MountKind::RoBind, exe.string(), std::string(kSessionInitPath)});
    auto info = elfread::read(exe);
    if (!info) return out;
    std::set<std::string> dirs;
    if (!info->interpreter.empty()) dirs.insert(fs::path(info->interpreter).parent_path().string());
    for (auto p : info->searchPaths) {
        if (auto pos = p.find("$ORIGIN"); pos != std::string::npos)
            p.replace(pos, 7, exe.parent_path().string());
        dirs.insert(p);
    }
    auto covered = [](const std::string& d) {
        for (auto root : {"/usr", "/bin", "/lib", "/lib64"})
            if (d == root || d.starts_with(std::string(root) + "/")) return true;
        return false;
    };
    for (const auto& d : dirs) {
        if (d.empty() || covered(d) || !fs::is_directory(d, ec)) continue;
        out.push_back({spec::MountKind::RoBind, d, d});
    }
    return out;
}
#endif

}  // namespace

int enter(const std::string& name, EventStream& stream, const std::string& preferred_backend, bool gpu, const std::string& cmd) {
    return enter(name, stream, EnterOptions{ .backend = preferred_backend, .gpu = gpu, .cmd = cmd });
}

int enter(const std::string& name, EventStream& stream, const EnterOptions& opts) {
    const auto& preferred_backend = opts.backend;
    const bool gpu = opts.gpu;
    const auto& cmd = opts.cmd;
    // `subos exec` reports a failure before the command started as 125.
    const int kFail = opts.exec_codes ? session::kExitSetup : 1;

    // Refuse nested sandbox entry.
    if (utils::get_env_or_default("XLINGS_SUBOS_MODE") == "sandbox") {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "cannot enter sandbox from inside another sandbox",
            .recoverable = true,
            .hint = "type 'exit' first to leave the current one",
        });
        return kFail;
    }

    auto& p = Config::paths();
    auto subos_dir = p.homeDir / "subos" / name;
    auto storage = read_storage_mode_(subos_dir);

#if defined(_WIN32)
    auto user = utils::get_env_or_default("USERNAME");
#else
    auto user = utils::get_env_or_default("USER");
#endif
    if (user.empty()) user = "user";

    fs::path image_mountpoint;
    if (storage == StorageMode::Image) image_mountpoint = subos_dir / ".mountpoint";

    if (storage == StorageMode::Shared) {
        fs::create_directories(subos_dir / "home" / user);
        fs::create_directories(subos_dir / "tmp");
    }
#if defined(__linux__) || defined(__APPLE__)
    if (storage == StorageMode::Shared) write_sandbox_rc_(subos_dir / "home" / user);
#endif
#if defined(_WIN32)
    fs::create_directories(subos_dir / "home" / user / "AppData" / "Roaming");
    fs::create_directories(subos_dir / "home" / user / "AppData" / "Local");
#endif

    const auto home = subos::home_view();
    const auto ports = subos::make_ports(stream);

    spec::Request request{
        .instance = name,
        .instance_dir = subos_dir,
        .user = user,
        .storage = to_spec_storage_(storage),
        .image_mountpoint = image_mountpoint,
        .host_env = current_env_(),
    };
    {
        auto shell = utils::get_env_or_default("SHELL");
        request.shell = shell.empty() ? "/bin/sh" : shell;
    }
    if (preferred_backend == "bwrap") request.preferred = spec::Backend::Bwrap;
    if (preferred_backend == "proot") request.preferred = spec::Backend::Proot;
    if (gpu) request.grants.insert("gpu");
#if !defined(_WIN32)
    request.interactive = ::isatty(STDIN_FILENO) == 1;
#endif
    if (!cmd.empty()) request.argv = {request.shell, "-c", cmd};
    else if (!opts.argv.empty()) request.argv = opts.argv;
    request.explicit_env = opts.env;
    request.cwd = opts.cwd;
    if (opts.detached) request.interactive = false;

    nlohmann::json payload;
    payload["name"] = name;
    payload["mode"] = "sandbox";

    std::cout.flush();
    std::cerr.flush();

#if defined(__linux__)
    // image/tmpfs storage requires bwrap (mount namespace needed)
    if (storage != StorageMode::Shared && preferred_backend == "proot") {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "image/tmpfs storage requires bwrap sandbox backend",
            .recoverable = false,
            .hint = "run: xlings install bwrap",
        });
        return kFail;
    }

    if (storage == StorageMode::Image) {
        auto img = subos_dir / "home.img";
        if (!fs::exists(img)) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "home.img not found — was this subos created with --storage image?",
                .recoverable = false,
            });
            return kFail;
        }
        if (mount_image_(img, image_mountpoint, user) != 0) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Internal,
                .message = "failed to mount home.img",
                .recoverable = false,
                .hint = "ensure you have sudo permission for mount",
            });
            return kFail;
        }
    }

    init_sandbox_dirs_(subos_dir, user, ::getuid(), ::getgid());
    if (storage == StorageMode::Image) {
        auto mp_home = image_mountpoint / user;
        fs::create_directories(mp_home);
        write_sandbox_rc_(mp_home);
    }

    auto host_caps = caps::probe(home, ports);
    // First sandbox use with a bwrap that does not work: say so once.
    if (host_caps.bwrap && !host_caps.bwrap->usable && !request.preferred
        && !fs::is_directory(subos_dir / "home" / user / ".config")) {
        log::info("bwrap not installed or namespace probe failed");
        log::info("  to enable bwrap (recommended): xlings install bwrap");
        log::info("  using proot fallback for now");
    }
    auto compiled = spec::compile(policy::legacy(), home, host_caps, request);

    // Nothing usable and nothing asked for: fetch a backend, once.
    if (!compiled && !request.preferred && compiled.error().missing.front().dimension == "backend") {
        // Ask the index whether it ships a backend for this target before
        // touching the network, so an unsupported host gets one causal error
        // and zero download requests.
        if (auto unavailable = backend_target_unavailable_(); unavailable) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = *unavailable,
                .recoverable = false,
                .hint = "use shell isolation, install your distribution's "
                        "bubblewrap package, or `xlings install bwrap` to "
                        "try anyway",
            });
            if (storage == StorageMode::Image) unmount_image_(image_mountpoint);
            return kFail;
        }
        if (auto_install_backend_(p.homeDir, stream) != 0) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "failed to install sandbox backend",
                .recoverable = false,
                .hint = "manually: xlings install bwrap (or: xlings install proot)",
            });
            if (storage == StorageMode::Image) unmount_image_(image_mountpoint);
            return kFail;
        }
        host_caps = caps::probe(home, ports);
        compiled = spec::compile(policy::legacy(), home, host_caps, request);
        if (!compiled && compiled.error().missing.front().dimension == "backend") {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "no sandbox backend available after install attempt",
                .recoverable = false,
            });
            if (storage == StorageMode::Image) unmount_image_(image_mountpoint);
            return kFail;
        }
    }
    if (!compiled) {
        auto refusal = compiled.error();
        // A bwrap that is installed and fails its probe: the probe's own
        // words, classified, instead of a generic "install bwrap".
        if (host_caps.bwrap && !host_caps.bwrap->usable
            && (refusal.missing.front().dimension == "storage"
                || request.preferred == spec::Backend::Bwrap)) {
            auto& u = refusal.missing.front();
            if (request.preferred == spec::Backend::Bwrap) u.reason = "bwrap probe failed";
            u.fix.clear();
            stream.emit(ErrorEvent{
                .code = u.dimension == "storage" ? ErrorCode::InvalidInput : ErrorCode::NotFound,
                .message = u.reason,
                .recoverable = false,
                .hint = classify_bwrap_probe_error_(host_caps.bwrap->probe_output,
                                                    host_caps.bwrap->bin),
            });
        } else {
            emit_refusal_(stream, refusal);
        }
        if (storage == StorageMode::Image) unmount_image_(image_mountpoint);
        return kFail;
    }
    const auto& sb = *compiled;
    if (sb.backend == spec::Backend::Proot && host_caps.proot && host_caps.proot->source == "host") {
        log::warn("using the host's proot ({}) -- no proot payload in {}. "
                  "Run `xlings install proot` to make this deterministic.",
                  sb.backend_bin.string(), p.homeDir.string());
    }

    const auto backend_name = std::string(spec::to_string(sb.backend));
    log::debug("sandbox backend: {} storage: {}", backend_name, storage_to_string_(storage));
    const auto digest = spec_digest_(sb);

    // One session per instance (design §12.1): a running one is JOINED --
    // same /tmp, processes and network as the terminal that started it.
    if (auto live = session::find(home, name)) {
        if (storage == StorageMode::Image) unmount_image_(image_mountpoint);
        if (live->digest != digest) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = std::format("'{}' is running with a different isolation; this "
                                       "call would not get what it asked for", name),
                .recoverable = true,
                .hint = std::format("xlings subos stop {}", name),
            });
            return session::kExitSetup;
        }
        payload["backend"] = backend_name;
        payload["joined"] = live->id;
        if (opts.announce) stream.emit(DataEvent{"subos_entering", payload.dump()});
        if (request.interactive && cmd.empty())
            log::info("joined the running session of '{}' (job control stays with the "
                      "terminal that started it)", name);
        auto r = session::join(home, name, session::ExecRequest{
            .argv = sb.argv, .env = request.host_env, .tty = request.interactive });
        if (!r.phase.empty() && r.phase == "setup")
            log::error("{}", r.error);
        return r.exit_code;
    }

    payload["backend"] = backend_name;
    payload["shell"] = request.shell;
    payload["storage"] = storage_to_string_(storage);
    if (opts.announce) stream.emit(DataEvent{"subos_entering", payload.dump()});

    // The terminal-injection filter travels to bwrap on an inherited pipe
    // (`--seccomp <fd>`); bwrap reads it and installs it for the command.
    std::vector<int> keep_fds;
    std::optional<int> seccomp_fd;
    if (sb.backend == spec::Backend::Bwrap && sb.block_tiocsti) {
        auto program = seccomp::block_terminal_injection();
        int fds[2];
        if (!program.empty() && ::pipe(fds) == 0) {
            (void)::write(fds[1], program.data(), program.size());
            ::close(fds[1]);
            seccomp_fd = fds[0];
            keep_fds.push_back(fds[0]);
        }
    }

    // session-init is this very binary, read-only at a fixed path inside; a
    // dynamically linked build brings its loader and library directories.
    auto launched = sb;
    for (auto& m : self_exe_mounts_()) launched.mounts.push_back(std::move(m));
    launched.argv = {std::string(kSessionInitPath), "__session-init"};
    if (!opts.detached) {
        // A detached session has no main command: it idles until its TTL or
        // `subos stop`, and everything in it joins.
        launched.argv.push_back("--");
        launched.argv.insert(launched.argv.end(), sb.argv.begin(), sb.argv.end());
    }
    auto argv = sb.backend == spec::Backend::Bwrap ? provider::bwrap_argv(launched, seccomp_fd)
                                                   : provider::proot_argv(launched);

    std::vector<std::string> pass(policy::kBaseEnvPass.begin(), policy::kBaseEnvPass.end());
    {
        auto legacy = policy::legacy();
        pass.insert(pass.end(), legacy.env_pass.begin(), legacy.env_pass.end());
    }
    // The supervisor hosts the session: the sandbox's owner stays outside it,
    // watching, and the audit is written where the sandbox cannot reach (F15).
    const int rc = session::host(home, session::Launch{
        .instance = name,
        .argv = std::move(argv),
        .env = provider::process_env(sb, request.host_env),
        .keep_fds = keep_fds,
        .backend = backend_name,
        .digest = digest,
        .spec = sb.describe(),
        .exec_env = sb.env,
        .env_pass = std::move(pass),
        .default_cwd = sb.cwd.string(),
        .ttl = opts.ttl,
        .detached = opts.detached,
        .timeout = opts.timeout,
    });
    if (storage == StorageMode::Image) unmount_image_(image_mountpoint);
    return rc;

#else
    // macOS / Windows: home redirect (dotfile isolation).
    auto host_caps = caps::probe(home, ports);
    auto compiled = spec::compile(policy::legacy(), home, host_caps, request);
    if (!compiled) {
        emit_refusal_(stream, compiled.error());
        return kFail;
    }
    // Say it where the person is, not only in the README. A user who reaches
    // for `--sandbox` to run something they do not trust is exactly the user
    // who did not read the isolation matrix, and on these two platforms this
    // is a dotfile redirect -- no filesystem, network or process boundary.
    //
    // Only on interactive entry: a `--cmd` run is a script, and a warning it
    // emits on every invocation is noise nobody reads. `--quiet` silences it.
    if (cmd.empty()) {
        log::warn("sandbox on {} redirects the home directory only -- "
                  "it does not contain the filesystem, network or processes. "
                  "Use an OS sandbox or a VM for untrusted code.",
#if defined(__APPLE__)
                  "macOS");
#else
                  "Windows");
#endif
    }
    payload["backend"] = "home-redirect";
#if defined(__APPLE__)
    payload["shell"] = platform::resolve_shell();
#endif
    if (opts.announce) stream.emit(DataEvent{"subos_entering", payload.dump()});
    for (const auto& [k, v] : compiled->env) platform::set_env_variable(k, v);
    return platform::run_shell(cmd, cmd.empty());
#endif
}

}
