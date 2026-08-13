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
import xlings.core.subos.gpu;
import xlings.core.xvm.shim;

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

int mount_image_(const fs::path& img, const fs::path& mountpoint, const std::string& user) {
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

    // (3) The host's proot, at the two paths a distribution puts it.
    //
    // There is no PATH step. An earlier version walked PATH and rejected the
    // candidates that turned out to be xlings shims; that fixed the symptom
    // and left the cause. By R6 the whole step is wrong for internal use: PATH
    // is a *view*, it is what the user selected, and letting it decide which
    // proot runs makes the sandbox's own identity depend on the environment
    // the sandbox was launched from. A shim on PATH is the worst case -- it
    // re-enters xlings, anchors to the home that owns it, and re-exports
    // XLINGS_HOME to match, so an isolated home with no backend installed
    // would silently run against the developer's real home, and every
    // measurement taken inside would be of the wrong home while looking
    // exactly like a measurement of the right one.
    //
    // These two paths are different in kind: they are the host's, they are
    // named here rather than discovered, and using one is reported. Per §2.8
    // of the architecture proposal, depending on the host is allowed when it
    // is DECLARED -- what is not allowed is depending on it by accident.
    for (const auto* p : {"/usr/bin/proot", "/usr/local/bin/proot"}) {
        auto candidate = fs::path(p);
        if (!fs::is_regular_file(candidate, ec)) continue;
        // Defensive: a home that installed itself under /usr/local would put
        // a shim on one of these paths, and it is still a shim.
        if (auto owner = xvm::resolve_owner_home(candidate)) {
            log::debug("skipping {}: an xlings shim owned by {}, not the "
                       "host's proot", candidate.string(), owner->string());
            continue;
        }
        log::warn("using the host's proot ({}) -- no proot payload in {}. "
                  "Run `xlings install proot` to make this deterministic.",
                  candidate.string(), home_dir.string());
        return candidate;
    }

    // Naming the home rather than "~/.xlings": with an isolated XLINGS_HOME
    // the tilde form points somewhere the caller is not using, and a
    // reader who follows it lands on the very home this search excluded.
    return std::unexpected(std::format(
        "proot not found in {}. Run `xlings install proot`, or place a proot "
        "binary at {}/runtimedir/proot. The host's proot at /usr/bin/proot is "
        "used when present (`sudo apt install proot`), but PATH is not "
        "searched: a `proot` on PATH may be an xlings shim, and running it "
        "would move the whole session to whichever home owns it.",
        home_dir.string(), home_dir.string()));
}

std::vector<SandboxBind>
sandbox_binds_(const fs::path& subos_dir, const fs::path& host_xlings_home, const std::string& user, StorageMode storage, const fs::path& mountpoint)
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

    // xlings shared — always bound regardless of storage mode, and always
    // at its OWN absolute path.
    //
    // xvm alias targets, RPATH and INTERP are absolute host paths baked at
    // install time, and nothing rewrites them on the way in; the home has
    // to answer to the same spelling in here that it does outside. Mapping
    // it to <user_home>/.xlings instead strands all of them, and mapping it
    // to both makes one directory reachable by two real paths — which a
    // bind mount does not collapse, so canonicalising does not save you and
    // a shim ends up reporting a conflict with itself. For the default home
    // this bind IS <user_home>/.xlings; that coincidence is the only reason
    // the remapped form ever appeared to work.
    binds.push_back({host_xlings_home.string(), host_xlings_home.string(), false});

    // ── Sandbox: NSS templates ──
    binds.push_back({(etc / "passwd").string(), "/etc/passwd", false});
    binds.push_back({(etc / "group").string(), "/etc/group", false});
    binds.push_back({(etc / "hosts").string(), "/etc/hosts", false});
    binds.push_back({(etc / "nsswitch.conf").string(), "/etc/nsswitch.conf", false});

    return binds;
}

std::vector<std::string>
build_proot_argv_(const fs::path& proot_bin, const fs::path& subos_dir, const fs::path& host_xlings_home, const std::string& user, const std::string& shell_path, const std::string& cmd)
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

std::pair<bool, std::string>
probe_bwrap_(const fs::path& bwrap_bin) {
    auto cmd = platform::shell_quote(bwrap_bin.string())
             + " --ro-bind / / -- /bin/true";
    auto [status, output] = platform::run_command_capture(cmd);
    return { status == 0, std::move(output) };
}

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

std::vector<std::string>
build_bwrap_argv_(const fs::path& bwrap_bin, const fs::path& subos_dir, const fs::path& host_xlings_home, const std::string& user, const std::string& shell_path, bool interactive_shell, StorageMode storage, const fs::path& mountpoint, bool gpu, const std::string& cmd)
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

void maybe_show_bwrap_hint_(const fs::path& subos_dir) {
    std::error_code ec;
    if (fs::is_directory(subos_dir / "home", ec)) return;

    log::info("bwrap not installed or namespace probe failed");
    log::info("  to enable bwrap (recommended): xlings install bwrap");
    log::info("  using proot fallback for now");
}

std::optional<BackendInfo>
detect_backend_(const fs::path& home_dir, const fs::path& subos_dir) {
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

int enter(const std::string& name, EventStream& stream, const std::string& preferred_backend, bool gpu, const std::string& cmd) {

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
        write_sandbox_rc_(user_home_dir);
    }
#endif

    // ── Common env (all platforms) ──
    platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
    platform::set_env_variable("XLINGS_SUBOS_MODE", "sandbox");
    // E2a, same declaration as the non-sandbox path. Declared here too and
    // not only there: a contract that holds in one of the two ways users
    // enter a subos is a contract consumers cannot rely on, and the sandbox
    // is the mode the graphics acceptance tooling runs in.
    platform::set_env_variable("XLINGS_SUBOS_LIB",
                               (subos_dir / "lib").string());

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
            write_sandbox_rc_(mp_home);
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
                .hint = "run: xlings install proot",
            });
            return 1;
        }
        backend = BackendInfo{ SandboxBackend::Proot, *bin };
    } else {
        backend = detect_backend_(p.homeDir, subos_dir);
        if (!backend) {
            // Ask the index whether it ships a backend for this target before
            // touching the network, so an unsupported host gets one causal
            // error and zero download requests.
            //
            // This used to be `#if defined(__aarch64__)`, which is a claim
            // about the package index frozen into the binary at compile time:
            // the day xim-pkgindex publishes an aarch64 bwrap, every client
            // already in the field would still refuse. Reading the on-disk
            // index costs nothing and is right both before and after that day.
            if (auto unavailable = backend_target_unavailable_(); unavailable) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::InvalidInput,
                    .message = *unavailable,
                    .recoverable = false,
                    .hint = "use shell isolation, install your distribution's "
                            "bubblewrap package, or `xlings install bwrap` to "
                            "try anyway",
                });
                return 1;
            }
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
    // Pin the home explicitly rather than relying on inheritance: it is
    // reachable at its own absolute path in here (see sandbox_binds_), and
    // every path baked at install time — xvm targets, RPATH, INTERP —
    // assumes exactly that spelling.
    platform::set_env_variable("XLINGS_HOME", p.homeDir.string());
    // Same spelling as XLINGS_HOME above. A shim decides which home owns it
    // by comparing the path it was invoked through against XLINGS_HOME as
    // strings; spelling the home two ways here makes it warn about a
    // conflict with itself.
    platform::set_env_variable("PATH", std::format(
        "{0}/subos/{1}/bin:{0}/bin:/usr/local/bin:/usr/bin:/bin",
        p.homeDir.string(), name));
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

    // Ask the same resolver the launch is about to use. Deriving the reported
    // shell here independently is how `subos_entering` came to announce
    // `/bin/zsh` while the process that started was `/bin/sh`: two fallbacks
    // for the same question, in two places, that only agree when SHELL is set.
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
                  "macOS");
    }

    payload["backend"] = "home-redirect";
    payload["shell"] = platform::resolve_shell();
    stream.emit(DataEvent{"subos_entering", payload.dump()});

    platform::set_env_variable("HOME", sandbox_home);
    platform::set_env_variable("TMPDIR", sandbox_tmp);
    platform::set_env_variable("XDG_CONFIG_HOME", sandbox_home + "/.config");
    platform::set_env_variable("XDG_DATA_HOME", sandbox_home + "/.local/share");
    platform::set_env_variable("XDG_CACHE_HOME", sandbox_home + "/.cache");
    platform::set_env_variable("XDG_STATE_HOME", sandbox_home + "/.local/state");

    return platform::run_shell(cmd, cmd.empty());

#elif defined(_WIN32)
    // ═══════════════════════════════════════════════════════════════
    // Windows L2: USERPROFILE redirect (dotfile isolation)
    // ═══════════════════════════════════════════════════════════════

    // Pre-create AppData structure
    fs::create_directories(fs::path(sandbox_home) / "AppData" / "Roaming");
    fs::create_directories(fs::path(sandbox_home) / "AppData" / "Local");

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
                  "Windows");
    }

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

    return platform::run_shell(cmd, cmd.empty());
#endif
}

}
