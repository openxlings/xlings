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
import xlings.subos.policy_store;
import xlings.subos.caps;
import xlings.subos.spec;
import xlings.confine.provider;
import xlings.confine;
import xlings.confine.gates;
import xlings.subos.session;
import xlings.subos.broker;
import xlings.core.elfread;
import xlings.core.confirm;
import xlings.observe;
import xlings.core.subos.ports;
import xlings.core.subos.root;
import xlings.core.subos.store_closure;
import xlings.core.subos.root_view;
import xlings.subos.roles;
import xlings.subos.elevation;
import xlings.subos.tools;
import xlings.carrier;
import xlings.subos.userdata;

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

// /etc/* template builders + sandbox dir layout init (POSIX sandboxes;
// plain code, so it compiles everywhere and needs no #if).
// We write per-user passwd/group at sandbox init time so getpwuid
// (real_uid) inside the sandbox returns the real user's home
// (= /home/<user>) and shell — most CLI tools depend on this. Root
// is also included so anything that does getpwuid(0) (scripts that
// assume "root must exist") doesn't bail.
std::string make_etc_passwd_(const std::string& user, unsigned uid, unsigned gid) {
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

std::string make_etc_group_(const std::string& user, unsigned gid) {
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
                        unsigned uid, unsigned gid,
                        std::string_view etc_name = "etc")
{
    auto user_home = subos_dir / "home" / user;
    fs::create_directories(user_home);
    fs::create_directories(subos_dir / "tmp");
    auto etc = subos_dir / std::string(etc_name);
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

int init_image_(const fs::path& img, const std::string& size) {
    if (fs::exists(img)) return 0;
    // A sparse file of `size` (K/M/G/T suffixes, as truncate takes it), then
    // an ext4 on it with the mkfs the tool table finds.
    auto bytes = policy::parse_size(size);
    if (!bytes) {
        log::error("image size '{}' is not a size", size);
        return 1;
    }
    {
        std::ofstream create(img, std::ios::binary);
        if (!create) return 1;
    }
    std::error_code ec;
    fs::resize_file(img, *bytes, ec);
    if (ec) {
        log::error("cannot size {}: {}", img.string(), ec.message());
        return 1;
    }
    auto mkfs = subos::tools::first("mkfs.ext4", subos::home_view(), Ports{});
    if (!mkfs) {
        log::error("mkfs.ext4 is not available; {}", subos::tools::install_hint("mkfs.ext4"));
        return 1;
    }
    return platform::run_argv({mkfs->bin.string(), "-F", "-m", "0", "-q", img.string()});
}

// A mount at exactly `path`, from this process's mount table.
bool is_mounted_(const fs::path& path) {
    std::ifstream in("/proc/self/mountinfo");
    const auto table = std::string(std::istreambuf_iterator<char>(in), {});
    const auto at = subos::userdata::mount_under_in(table, path);
    return at && *at == path;
}

// Mount an image file at mountpoint. Supports multi-terminal reuse.
// After mount, chown the mountpoint root to the calling user so
// subsequent directory/file creation doesn't need sudo.
int mount_image_(const fs::path& img, const fs::path& mountpoint,
                 const std::string& user = {}) {
    fs::create_directories(mountpoint);
    if (is_mounted_(mountpoint)) return 0;  // already mounted

    // Privileged, through the one elevation path (run directly when already
    // root -- sudo is frequently absent in minimal root containers).
    const auto home = subos::home_view();
    auto rc = subos::elevation::run(home, {"mount", "-o", "loop", img.string(), mountpoint.string()},
                                    "mount a SubOS home image");
    if (rc != 0) return rc;

    // ext4 root dir is owned by root after mkfs; chown to real user
    // so sandbox init can create dirs without sudo.
    if (!user.empty())
        (void)subos::elevation::run(home, {"chown", user + ":" + user, mountpoint.string()},
                                    "hand a SubOS home image to its user");
    return 0;
}

int unmount_image_(const fs::path& mountpoint) {
    if (!is_mounted_(mountpoint)) return 0;
    return subos::elevation::run(subos::home_view(), {"umount", mountpoint.string()},
                                 "unmount a SubOS home image");
}

// ─────────────────────────────────────────────────────────────────────
// Sandbox entry: policy + host capabilities -> spec -> a session (Linux),
// or the home redirect (macOS, Windows). See docs/design/subos-isolation.md.
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
    for (const auto* name : {"bwrap", "proot"}) {  // tool-ok: package names in the index
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
        if (auto b = caps::payload_bwrap(home)) {
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
    return platform::environment();
}

// The refusal as the one ErrorEvent the entry reports. Wording kept from the
// code this replaced; the remedy is the compiler's.
void emit_refusal_(EventStream& stream, const spec::Refusal& refusal) {
    const auto& u = refusal.missing.front();
    if (refusal.missing.size() == 1 && (u.dimension == "backend" || u.dimension == "storage")) {
        stream.emit(ErrorEvent{
            .code = u.dimension == "storage" ? ErrorCode::InvalidInput : ErrorCode::NotFound,
            .message = u.reason,
            .recoverable = false,
            .hint = u.fix.empty() ? std::string{} : "run: " + u.fix,
        });
        return;
    }
    // The shared format for a requirement the host cannot meet (design §10):
    // one line per missing item, and what to do about each.
    std::string message = "cannot enter: the policy requires what this host cannot provide";
    std::string hint;
    for (const auto& m : refusal.missing) {
        message += std::format("\n  \u2717 {}: {}", m.dimension, m.reason);
        // Each remedy once: several items often share it.
        if (!m.fix.empty() && hint.find(m.fix) == std::string::npos)
            hint += (hint.empty() ? "" : "; ") + m.fix;
    }
    stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput, .message = message,
                            .recoverable = false, .hint = hint });
}

// `! <dimension> not in effect: <reason>` -- entered, with less than asked.
void report_degraded_(const spec::SandboxSpec& sb) {
    for (const auto& d : sb.degraded)
        log::warn("! {} not in effect: {}{}", d.dimension, d.reason,
                  d.fix.empty() ? std::string{} : " (" + d.fix + ")");
}


// Where session-init (this binary) lives inside every sandbox.
constexpr std::string_view kSessionInitPath = "/run/xlings/xlings";

// What a sandbox must be compared on to decide whether a call may join the
// running session: everything that isolates, nothing that varies per call.
std::string spec_digest_(const spec::SandboxSpec& sb, const spec::Request& request, const policy::Policy& pol) {
    auto stable = sb;
    if (!request.root.empty()) {
        std::erase_if(stable.mounts, [&](const spec::MountOp& mount) {
            return std::ranges::find(request.root_mounts, mount) != request.root_mounts.end();
        });
    }
    auto j = stable.describe();
    if (!request.root.empty()) {
        j["root_scope"] = request.instance;
        j["root_instance"] = request.instance_dir.generic_string();
        j["root_policy"] = policy::to_json(pol);
    }
    for (auto k : {"argv", "env", "cwd", "degraded"}) j.erase(k);
    return std::format("{:016x}", std::hash<std::string>{}(j.dump()));
}


// observe >= standard: the host paths a sandbox can write, to list what
// changed in them when the session ends.
std::vector<std::string> rw_mount_sources_(const policy::Policy& pol) {
    std::vector<std::string> out;
    if (pol.observe == policy::Observe::Off || pol.observe == policy::Observe::Basic) return out;
    for (auto& m : pol.mounts) if (m.rw) out.push_back(m.src);
    return out;
}

// This binary on the host: the broker runs what it allows with it.
// This binary, on every platform (/proc/self/exe is Linux's alone; macOS
// runs its home redirect's session-init from here too).
std::string host_exe_() {
    auto exe = platform::get_executable_path();
    return exe.empty() ? std::string("xlings") : exe.string();
}

// What a brokered command runs with: this host environment, acting on this
// instance. Not the sandbox's markers -- it runs outside.
std::map<std::string, std::string> broker_env_(std::map<std::string, std::string> env,
                                               const fs::path& home, const std::string& name) {
    for (auto k : {"XLINGS_SUBOS_MODE", "XLINGS_PROJECT_DIR", "XLINGS_SESSION_FD"}) env.erase(k);
    env["XLINGS_HOME"] = home.string();
    env["XLINGS_ACTIVE_SUBOS"] = name;
    env["XLINGS_NON_INTERACTIVE"] = "1";
    return env;
}

// This binary at kSessionInitPath, plus -- for a dynamically linked build --
// its ELF interpreter's directory and its RUNPATH, read-only at their own
// paths. A static release binary needs neither.
std::vector<spec::MountOp> self_exe_mounts_() {
    std::vector<spec::MountOp> out;
    if constexpr (!platform::is_linux) return out;
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

    // A session that outlives its shell needs a session host (design §16,
    // part 3 §6.3): Linux and macOS. On Windows the entry below is an
    // interactive shell, the opposite of detached, and waits for input.
    if (platform::is_windows && opts.detached) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "a detached session (subos start) needs a session host: Linux or macOS",
            .recoverable = false,
            .hint = std::format("run commands with `xlings subos exec {} -- <cmd>`", name),
        });
        return session::kExitSetup;
    }

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

    auto user = utils::get_env_or_default(platform::is_windows ? "USERNAME" : "USER");
    if (user.empty()) user = "user";

    fs::path image_mountpoint;
    if (storage == StorageMode::Image) image_mountpoint = subos_dir / ".mountpoint";

    if (storage == StorageMode::Shared) {
        fs::create_directories(subos_dir / "home" / user);
        fs::create_directories(subos_dir / "tmp");
    }
    if constexpr (platform::is_posix)
        if (storage == StorageMode::Shared) write_sandbox_rc_(subos_dir / "home" / user);
    if constexpr (platform::is_windows) {
        fs::create_directories(subos_dir / "home" / user / "AppData" / "Roaming");
        fs::create_directories(subos_dir / "home" / user / "AppData" / "Local");
    }

    const auto home = subos::home_view();
    const auto ports = subos::make_ports(stream);

    // A rootfs instance (design part 2 §3.1) is entered as its own root,
    // always in a sandbox; with no isolation declared it is `dev`. Its
    // projection is brought up to date first -- a no-op when nothing moved.
    const auto kind = subos_root::read_kind(p.homeDir, name);
    if (!kind) {
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput, .message = kind.error(),
                                .recoverable = false });
        return kFail;
    }
    const bool rootfs = *kind == roles::Kind::Rootfs;
    auto preset = opts.preset;
    if (rootfs) {
        if constexpr (!platform::is_linux) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = "a rootfs SubOS is entered as a Linux root; this host is not Linux",
                .recoverable = false,
                .hint = std::format("on Windows: xlings subos export {} --wsl <file> on a Linux "
                                    "machine, then wsl --import", name),
            });
            return kFail;
        }
        if (!preset && !policy_store::has_file(home, name)) preset = policy::Preset::Dev;
        if (auto r = subos_root::refresh(p.homeDir, name, "enter"); r && !*r) {
            stream.emit(ErrorEvent{ .code = ErrorCode::Internal,
                                    .message = "the root of '" + name + "' could not be prepared: "
                                               + r->error(),
                                    .recoverable = false });
            return kFail;
        }
    }

    // What this instance may do: its policy file, a stricter preset named on
    // this call, this call's (tighten-only) overrides. A policy that does not
    // parse, or names what this version cannot enforce, refuses entry.
    auto effective = policy_store::effective(home, name, preset, opts.overrides, Info::VERSION);
    if (!effective) {
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput, .message = effective.error(),
                                .recoverable = false });
        return kFail;
    }
    const auto pol = std::move(*effective);
    if (policy::fetches_into_layer(pol) && !rootfs) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "fetch = layer installs into the root's own system scope, and '" + name
                       + "' is a view of the host with none",
            .recoverable = false,
            .hint = std::format("a root: xlings subos new <name> --rootfs --from {}; or "
                                "xlings subos config {} --fetch auto|ask|deny", name, name),
        });
        return kFail;
    }

    struct RootViewLifetime {
        std::shared_ptr<subos_root::root_view::View> view;
        bool transferred { false };
        ~RootViewLifetime() {
            if (view && !transferred)
                view->close();
        }
    } rootView;

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
    if (preferred_backend == "landlock") request.preferred = spec::Backend::Landlock;
    if (gpu) request.grants.insert("gpu");
    if constexpr (platform::is_posix) request.interactive = platform::stdin_is_terminal();
    if (!cmd.empty()) request.argv = {request.shell, "-c", cmd};
    else if (!opts.argv.empty()) request.argv = opts.argv;
    request.publish = opts.publish;
    request.explicit_env = opts.env;
    request.cwd = opts.cwd;
    if (opts.detached) request.interactive = false;
    if (rootfs) {
        request.root = subos_root::tree_of(p.homeDir, name);
        if (!cmd.empty()) request.argv = {"/bin/sh", "-c", cmd};
        request.shell = "/bin/sh";
    }

    nlohmann::json payload;
    payload["name"] = name;
    payload["mode"] = "sandbox";

    std::cout.flush();
    std::cerr.flush();

    // Under a supervisor wherever there is a session host (Linux, macOS):
    // join, --timeout, exit codes and the audit are the same (part 3 §6.3).
    if constexpr (platform::is_posix) {
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

        init_sandbox_dirs_(subos_dir, user, platform::user_ids().uid, platform::user_ids().gid);
        // The read-only home's tmpfs mount points must exist on the host: bwrap
        // cannot create a directory inside a read-only bind.
        for (auto dir : {"logs", "run", "state", "subos"}) fs::create_directories(p.homeDir / dir);
        // A neutral identity's user and its passwd/group (spec: etc-neutral/).
        if (pol.identity == policy::Identity::Neutral)
            init_sandbox_dirs_(subos_dir, "user", platform::user_ids().uid, platform::user_ids().gid, "etc-neutral");
        if (storage == StorageMode::Image) {
            auto mp_home = image_mountpoint / user;
            fs::create_directories(mp_home);
            write_sandbox_rc_(mp_home);
        }

        if (rootfs) {
            auto scope = subos_root::store_closure::read_scope(p.homeDir, name, request.root);
            if (!scope) {
                stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput, .message = scope.error(),
                                       .recoverable = false});
                return kFail;
            }
            auto prepared = subos_root::root_view::prepare(*scope);
            if (!prepared) {
                stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput, .message = prepared.error(),
                                       .recoverable = false});
                return kFail;
            }
            rootView.view = std::move(*prepared);
            for (const auto& binding : rootView.view->bindings())
                request.root_mounts.push_back({spec::MountKind::RoBind, binding.source.generic_string(),
                                              binding.destination.generic_string()});
        }

        auto host_caps = caps::probe(home, ports);
        auto compiled = xlings::confine::compile(pol, home, host_caps, request);

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
            compiled = xlings::confine::compile(pol, home, host_caps, request);
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
        if (compiled->backend == spec::Backend::HomeRedirect) {
            // macOS: the home redirect, now under the same supervisor. Say
            // what it is where the person is: a dotfile redirect -- no
            // filesystem, network or process boundary (the carrier is).
            if (cmd.empty())
                log::warn("sandbox on macOS redirects the home directory only -- it does not contain the "
                          "filesystem, network or processes. A boundary: --carrier vz, or a VM.");
            if (compiled->argv.empty()) {
                compiled->argv = {platform::resolve_shell()};
                if (request.interactive) compiled->argv.push_back("-i");
            }
        }
        const auto& sb = *compiled;
        auto trace_spec = sb.describe();
        trace_spec.erase("argv");
        trace_spec["argc"] = sb.argv.size();
        observe::trace("spec", trace_spec.dump());
        if (pol.preset != policy::Preset::Legacy) report_degraded_(sb);
        // proot because bwrap is there and cannot make a sandbox (Ubuntu 24.04's
        // AppArmor restriction, most often): never silently. Asked for by name,
        // it is the user's choice and goes unremarked.
        if (sb.backend == spec::Backend::Proot && !request.preferred && host_caps.bwrap
            && !host_caps.bwrap->usable) {
            auto why = host_caps.bwrap->probe_output;
            if (auto nl = why.find('\n'); nl != std::string::npos) why.resize(nl);
            log::warn("bwrap cannot make a sandbox here ({}); entering with proot -- a view, "
                      "not a security boundary. A real sandbox: xlings self doctor --isolation --fix",
                      why.empty() ? "probe failed" : why);
        }
        if (sb.backend == spec::Backend::Proot && host_caps.proot && host_caps.proot->source == "system") {
            log::warn("using the host's proot ({}) -- no proot payload in {}. "
                      "Run `xlings install proot` to make this deterministic.",
                      sb.backend_bin.string(), p.homeDir.string());
        }

        const auto backend_name = std::string(spec::to_string(sb.backend));
        log::debug("sandbox backend: {} storage: {}", backend_name, storage_to_string_(storage));
        const auto digest = spec_digest_(sb, request, pol);

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
            std::fflush(nullptr);
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
        std::fflush(nullptr);

        // The terminal-injection filter travels to bwrap on an inherited pipe
        // (`--seccomp <fd>`); bwrap reads it and installs it for the command.
        std::vector<int> keep_fds;
        std::optional<int> seccomp_fd;
        if (sb.backend == spec::Backend::Bwrap && sb.block_tiocsti) {
            auto program = platform::seccomp::block_terminal_injection();
            if (auto fds = platform::make_pipe(); !program.empty() && fds) {
                (void)platform::write_fd((*fds)[1], std::string_view(
                    reinterpret_cast<const char*>(program.data()), program.size()));
                platform::close_fd((*fds)[1]);
                seccomp_fd = (*fds)[0];
                keep_fds.push_back((*fds)[0]);
            }
        }

        // session-init is this very binary, read-only at a fixed path inside; a
        // dynamically linked build brings its loader and library directories.
        auto launched = sb;
        for (auto& mount : self_exe_mounts_()) {
            if (rootfs && mount.dst != kSessionInitPath) {
                const auto& bindings = rootView.view->bindings();
                const auto path = fs::path(mount.src).lexically_normal();
                const bool permitted = std::ranges::any_of(bindings, [&](const auto& binding) {
                    if (binding.source != binding.destination)
                        return false;
                    const auto relative = path.lexically_relative(binding.source);
                    return path == binding.source || (!relative.empty() && !relative.is_absolute() &&
                                                       *relative.begin() != "..");
                });
                if (!permitted) {
                    stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput,
                        .message = "the session client needs a runtime directory outside this root's checked closure: " + mount.src,
                        .recoverable = false,
                        .hint = "use a static release client, or install its recorded runtime closure into this root"});
                    return kFail;
                }
                continue;
            }
            launched.mounts.push_back(std::move(mount));
        }
        // The broker's socket, made by the supervisor before the backend starts.
        const bool brokered = sb.backend == spec::Backend::Bwrap || sb.backend == spec::Backend::Landlock;
        if (sb.backend == spec::Backend::Bwrap)
            launched.mounts.push_back({spec::MountKind::Bind, home.broker_socket(name).string(),
                                       std::string(broker::kSocketInside)});
        launched.argv = {std::string(kSessionInitPath), "__session-init"};
        if (!opts.detached) {
            // A detached session has no main command: it idles until its TTL or
            // `subos stop`, and everything in it joins.
            launched.argv.push_back("--");
            launched.argv.insert(launched.argv.end(), sb.argv.begin(), sb.argv.end());
        }
        // The implementation's command (xlings.confine): bwrap's or proot's
        // argv around session-init, or -- Landlock, no view to mount it into
        // -- this binary where it is, fencing itself before it starts anything.
        const auto argv = xlings::confine::launch_argv(launched, seccomp_fd, host_exe_());

        if (observe::trace_enabled("provider")) {
            std::string line;
            line = std::format("{} ({} arguments)", backend_name, argv.size());
            observe::trace("provider", line);
        }
        std::vector<std::string> pass(policy::kBaseEnvPass.begin(), policy::kBaseEnvPass.end());
        pass.insert(pass.end(), pol.env_pass.begin(), pol.env_pass.end());
        // The supervisor hosts the session: the sandbox's owner stays outside it,
        // watching, and the audit is written where the sandbox cannot reach (F15).
        const int rc = session::host(home, session::Launch{
            .instance = name,
            .argv = std::move(argv),
            .env = xlings::confine::provider::process_env(sb, request.host_env),
            .keep_fds = keep_fds,
            .backend = backend_name,
            .digest = digest,
            .spec = sb.describe(),
            .audit_required = pol.preset == policy::Preset::Locked,
            .exec_env = sb.env,
            .env_pass = std::move(pass),
            .default_cwd = sb.cwd.string(),
            .ttl = opts.ttl,
            .detached = opts.detached,
            .timeout = opts.timeout,
            .pasta = sb.net_nat ? xlings::confine::provider::pasta_args(sb) : std::vector<std::string>{},
            .proxy_url = sb.net_proxy ? sb.proxy_url : std::string{},
            .trace_net = (sb.net_nat || sb.net_proxy) && (pol.observe == policy::Observe::Standard || pol.observe == policy::Observe::Full),
            .broker_policy = brokered ? std::optional(pol) : std::nullopt,
            .broker_exe = { host_exe_() },
            .broker_env = broker_env_(request.host_env, p.homeDir, name),
            .trace_exec = pol.observe == policy::Observe::Full && sb.backend == spec::Backend::Bwrap,
            .rw_paths = rw_mount_sources_(pol),
            .refresh_root = rootView.view
                ? std::function<std::expected<void, std::string>(std::span<const int, 3>)>{
                    [view = rootView.view](std::span<const int, 3> target) { return view->refresh(target); }}
                : std::function<std::expected<void, std::string>(std::span<const int, 3>)>{},
            .finalize_root = rootView.view ? std::function<void()>{[view = rootView.view] { view->close(); }}
                                         : std::function<void()>{},
        });
        if (opts.detached && rc == 0)
            rootView.transferred = true;
        if (storage == StorageMode::Image) unmount_image_(image_mountpoint);
        return rc;
    } else {
        // Windows: the home redirect (dotfile isolation), its command in a
        // Job Object -- the whole tree ends with it, or at --timeout (124).
        auto host_caps = caps::probe(home, ports);
        auto compiled = xlings::confine::compile(pol, home, host_caps, request);
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
            log::warn("sandbox on Windows redirects the home directory only -- "
                      "it does not contain the filesystem, network or processes. "
                      "A boundary: --carrier wsl2.");
        }
        if (pol.preset != policy::Preset::Legacy) report_degraded_(*compiled);
        payload["backend"] = "home-redirect";
        if (opts.announce) stream.emit(DataEvent{"subos_entering", payload.dump()});
        std::fflush(nullptr);
        for (const auto& [k, v] : compiled->env) platform::set_env_variable(k, v);
        // `subos exec -- argv`: the argv, not a shell (which, with no command,
        // is an interactive one -- and waits for input).
        if (!opts.argv.empty()) {
            std::error_code ec;
            fs::current_path(opts.cwd.empty() ? compiled->cwd : fs::path(opts.cwd), ec);
            return platform::run_argv_with_timeout(opts.argv, opts.timeout.value_or(std::chrono::milliseconds::max()));
        }
        return platform::run_shell(cmd, cmd.empty());
    }
}

nlohmann::json preview(const std::string& name, const policy::Policy& pol, EventStream& stream) {
    const auto home = subos::home_view();
    const auto ports = subos::make_ports(stream);
    auto user = utils::get_env_or_default(platform::OS_NAME == "windows" ? "USERNAME" : "USER");
    if (user.empty()) user = "user";
    const auto dir = Config::paths().homeDir / "subos" / name;
    spec::Request request{ .instance = name, .instance_dir = dir, .user = user,
                           .storage = to_spec_storage_(read_storage_mode_(dir)) };
    const auto host_caps = caps::probe(home, ports);
    nlohmann::json out;
    auto compiled = xlings::confine::compile(pol, home, host_caps, request);
    if (compiled) {
        out["enters"] = true;
        out["spec"] = compiled->describe();
    } else {
        out["enters"] = false;
        out["missing"] = nlohmann::json::array();
        for (auto& m : compiled.error().missing)
            out["missing"].push_back({{"dimension", m.dimension}, {"reason", m.reason}, {"fix", m.fix}});
    }
    out["gates"] = nlohmann::json::array();
    for (auto& g : xlings::confine::gates::probe(host_caps))
        out["gates"].push_back({{"gate", g.gate}, {"supported", g.supported},
                                {"enforced", std::string(xlings::confine::gates::to_string(g.enforced))},
                                {"reason", g.reason}, {"route", g.route}});
    return out;
}


namespace {
// The one repair that needs root (self doctor --isolation --fix, and the
// first root a host makes): a root-owned copy of a bwrap at
// /usr/lib/xlings/bwrap with an AppArmor profile that grants it user
// namespaces and nothing else. Asked once; the commands are printed; run
// through the one door for administrator rights (sudo asks for the password
// at the terminal).
int repair_isolation_(const HomeView& home, const subos::Ports& ports,
                      const std::vector<caps::Backend>& candidates, bool yes, EventStream& stream,
                      std::string_view question) {
    auto read_sysctl = [](const char* path) -> std::string {
        std::ifstream in(path);
        std::string v;
        std::getline(in, v);
        return v.empty() ? std::string("(absent)") : v;
    };
    // The repair: only for the case it fixes -- AppArmor restricting
    // unprivileged user namespaces for unconfined programs.
    if (read_sysctl("/proc/sys/kernel/apparmor_restrict_unprivileged_userns") != "1") {
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput,
            .message = "user namespaces are not restricted by AppArmor here; nothing this repair "
                       "changes would help",
            .recoverable = false,
            .hint = "the kernel disables them outright (see the sysctl values above); "
                    "--sandbox proot works without them" });
        return 1;
    }
    std::optional<fs::path> source;
    for (auto& b : candidates) if (b.source != "root-owned") { source = b.bin; break; }
    if (!source) {
        stream.emit(ErrorEvent{ .code = ErrorCode::NotFound,
            .message = "no bwrap to install", .recoverable = true,
            .hint = "xlings install bwrap, then run this again" });
        return 1;
    }
    const std::string profile =
        "# xlings: grant user namespaces to the root-owned bwrap xlings uses for SubOS\n"
        "# sandboxes (xlings self doctor --isolation --fix). Nothing else.\n"
        "abi <abi/4.0>,\n"
        "include <tunables/global>\n\n"
        "profile xlings-bwrap /usr/lib/xlings/bwrap flags=(unconfined) {\n"
        "  userns,\n\n"
        "  include if exists <local/xlings-bwrap>\n"
        "}\n";
    const auto tmp = fs::temp_directory_path() / std::format("xlings-bwrap-profile-{}", platform::get_pid());
    platform::write_string_to_file(tmp.string(), profile);
    // Each step runs through elevation::run below (tool-ok on each line).
    const std::vector<std::vector<std::string>> steps{
        {"install", "-D", "-o", "root", "-g", "root", "-m", "0755", source->string(),  // tool-ok: elevated below
         std::string(caps::kRootOwnedBwrap)},
        {"install", "-D", "-o", "root", "-g", "root", "-m", "0644", tmp.string(),  // tool-ok: elevated below
         "/etc/apparmor.d/xlings-bwrap"},
        {"apparmor_parser", "-r", "/etc/apparmor.d/xlings-bwrap"},  // tool-ok: elevated below
    };
    auto spelled = [](const std::vector<std::string>& argv) {
        std::string line;
        for (const auto& a : argv) line += (line.empty() ? "" : " ") + platform::shell_quote(a);
        return line;
    };
    std::string plan = "this runs, as root:";
    for (auto& c : steps) plan += "\n    " + spelled(c);
    log::info("{}", plan);
    auto asked = confirm::ask(stream, "self.doctor.isolation.fix",
                              std::string(question), yes, "-y");
    if (asked.outcome != confirm::Outcome::Confirmed) {
        std::error_code ec;
        fs::remove(tmp, ec);
        if (asked.outcome == confirm::Outcome::NobodyToAsk) {
            log::error("nothing changed: this needs confirmation -- re-run with -y");
            return 2;
        }
        log::info("nothing changed");
        return 1;
    }
    for (auto& c : steps) {
        log::info("$ {}", spelled(c));
        if (subos::elevation::run(home, c, "self doctor --isolation --fix") != 0) {
            std::error_code ec;
            fs::remove(tmp, ec);
            log::error("failed: {}", spelled(c));
            return 1;
        }
    }
    std::error_code ec;
    fs::remove(tmp, ec);
    auto after = caps::locate_bwrap(home, ports);
    if (after && after->usable && after->source == "root-owned") {
        log::info("sandboxes now use {} (root-owned, AppArmor profile xlings-bwrap)",
                  after->bin.string());
        return 0;
    }
    log::error("installed, but the probe still fails: {}",
               after ? after->probe_output.substr(0, after->probe_output.find('\n')) : std::string("no bwrap"));
    return 1;
}
}  // namespace

// `xlings self doctor --isolation [--fix]` (design §18, §20; #640 F10, F12).
//
// What this host can isolate with, measured: each bwrap found and what its
// probe said, the kernel's user-namespace settings, pasta, the platform
// interfaces. --fix does the one repair that needs root, and only that: a
// root-owned copy of a bwrap at /usr/lib/xlings/bwrap with an AppArmor
// profile that grants it user namespaces and nothing else -- never turning
// the restriction off for the machine. Every command it runs is printed.
int doctor_isolation(bool fix, bool yes, bool json, EventStream& stream) {
    const auto home = subos::home_view();
    const auto ports = subos::make_ports(stream);
    auto read_sysctl = [](const char* path) -> std::string {
        std::ifstream in(path);
        std::string v;
        std::getline(in, v);
        return v.empty() ? std::string("(absent)") : v;
    };
    nlohmann::json report;
    report["platform"] = std::string(caps::platform_name());
    std::vector<caps::Backend> candidates;
    if constexpr (platform::is_linux) {
        report["sysctl"] = {
            {"kernel.apparmor_restrict_unprivileged_userns",
             read_sysctl("/proc/sys/kernel/apparmor_restrict_unprivileged_userns")},
            {"kernel.unprivileged_userns_clone", read_sysctl("/proc/sys/kernel/unprivileged_userns_clone")},
            {"user.max_user_namespaces", read_sysctl("/proc/sys/user/max_user_namespaces")},
        };
        // Whose answer this is. A process confined by an AppArmor profile
        // that allows user namespaces (Ubuntu's `busybox` one, inherited from
        // whatever started it) succeeds where a shell started normally is
        // refused -- measured on 2026-10-09: the same machine said yes in one
        // terminal and no in another.
        for (const char* attr : {"/proc/self/attr/apparmor/current", "/proc/self/attr/current"}) {
            std::ifstream in(attr);
            std::string label;
            if (in && std::getline(in, label) && !label.empty()) {
                while (!label.empty() && (label.back() == '\n' || label.back() == '\0')) label.pop_back();
                report["apparmor_label"] = label;
                break;
            }
        }
        candidates = caps::bwrap_candidates(home, ports, /*fresh=*/true);
        report["bwrap"] = nlohmann::json::array();
        for (auto& b : candidates) {
            auto first = b.probe_output.substr(0, b.probe_output.find('\n'));
            report["bwrap"].push_back({{"path", b.bin.string()}, {"source", b.source}, {"usable", b.usable},
                                       {"probe", b.usable ? std::string("ok") : first}});
        }
    }
    // Where every external tool would come from (xlings.subos.tools): the
    // one table, its answer on this machine, and what brings a missing one.
    report["tools"] = nlohmann::json::array();
    for (const auto& tool : subos::tools::known()) {
        auto found = subos::tools::first(tool.name, home, ports);
        report["tools"].push_back(found
            ? nlohmann::json{{"name", tool.name}, {"path", found->bin.string()},
                             {"source", subos::tools::to_string(found->source)}}
            : nlohmann::json{{"name", tool.name}, {"path", nullptr},
                             {"install", subos::tools::install_hint(tool.name)}});  // tool-ok: a JSON key
    }
    // Where a SubOS can run on this machine (design part 3 §5): each carrier
    // this platform has, measured.
    report["carriers"] = nlohmann::json::array();
    for (const auto name : carrier::carriers_of(caps::platform_name())) {
        const auto* c = carrier::find(name);
        const auto p = c ? c->probe(home) : carrier::Probe{.reason = "not in this build"};
        report["carriers"].push_back({{"name", name}, {"supported", p.supported}, {"reason", p.reason},
                                      {"route", p.route}, {"evidence", p.evidence}});
    }
    auto host_caps = caps::probe(home, ports);
    report["backend"] = host_caps.bwrap && host_caps.bwrap->usable
        ? nlohmann::json{{"name", "bwrap"}, {"path", host_caps.bwrap->bin.string()}, {"source", host_caps.bwrap->source}}
        : host_caps.proot ? nlohmann::json{{"name", "proot"}, {"path", host_caps.proot->bin.string()}}
                          : nlohmann::json(nullptr);
    report["pasta"] = host_caps.pasta ? nlohmann::json(host_caps.pasta->string())
                                      : nlohmann::json(host_caps.pasta_missing);
    report["gates"] = nlohmann::json::array();
    for (auto& g : xlings::confine::gates::probe(host_caps))
        report["gates"].push_back({{"gate", g.gate}, {"supported", g.supported},
                                   {"enforced", std::string(xlings::confine::gates::to_string(g.enforced))}, {"reason", g.reason}});
    const bool ok = host_caps.platform != "linux" || (host_caps.bwrap && host_caps.bwrap->usable);
    report["ok"] = ok;

    auto print = [&] {
        if (json) { std::println(std::cout, "{}", report.dump()); return; }
        std::println(std::cout, "isolation on this host ({})", report["platform"].get<std::string>());
        if (report.contains("sysctl"))
            for (auto it = report["sysctl"].begin(); it != report["sysctl"].end(); ++it)
                std::println(std::cout, "  {:<46} {}", it.key(), it.value().get<std::string>());
        if (report.contains("apparmor_label")) {
            const auto label = report["apparmor_label"].get<std::string>();
            std::println(std::cout, "  {:<46} {}", "apparmor label of this process", label);
            if (label != "unconfined")
                std::println(std::cout, "    (inherited from what started it: a shell started normally is "
                                        "`unconfined`, and the probes below may differ there)");
        }
        if (report.contains("bwrap")) {
            if (report["bwrap"].empty()) std::println(std::cout, "  bwrap: none found");
            for (auto& b : report["bwrap"])
                std::println(std::cout, "  bwrap {:<10} {} -- {}", b["source"].get<std::string>(),
                             b["path"].get<std::string>(), b["probe"].get<std::string>());
        }
        std::println(std::cout, "  backend: {}", report["backend"].is_null() ? std::string("none")
            : report["backend"]["name"].get<std::string>() + " " + report["backend"]["path"].get<std::string>());
        std::println(std::cout, "  pasta (net=nat): {}", report["pasta"].get<std::string>());
        for (auto& g : report["gates"])
            std::println(std::cout, "    {:<14} {}", g["gate"].get<std::string>(),
                         g["supported"].get<bool>() ? g["enforced"].get<std::string>() : "no -- " + g["reason"].get<std::string>());
    };
    print();
    if (ok || !fix) {
        if (!ok && !json)
            std::println(std::cout, "\n  fix: xlings self doctor --isolation --fix   "
                                 "(one sudo: a root-owned bwrap with a narrow AppArmor profile)");
        return ok ? 0 : 1;
    }

    if constexpr (platform::is_linux) {
        return repair_isolation_(home, ports, candidates, yes, stream,
                                 "install the root-owned bwrap and its AppArmor profile?");
    } else {
        (void)yes;
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput,
            .message = "this platform's isolation is not implemented yet; there is nothing to repair",
            .recoverable = false });
        return 1;
    }
}

int prepare_root_host(bool yes, EventStream& stream) {
    if constexpr (!platform::is_linux) return 0;   // a root elsewhere is refused where it is made
    const auto home = subos::home_view();
    const auto ports = subos::make_ports(stream);
    auto found = caps::locate_bwrap(home, ports);
    if (found && found->usable) return 0;
    std::ifstream sysctl("/proc/sys/kernel/apparmor_restrict_unprivileged_userns");
    std::string restricted;
    std::getline(sysctl, restricted);
    if (restricted != "1") {
        // Not the restriction the repair lifts: say what is in the way.
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput,
            .message = found ? classify_bwrap_probe_error_(found->probe_output, found->bin)
                             : std::string("a root needs bwrap, and this host has none"),
            .recoverable = true,
            .hint = found ? "nothing was downloaded; see `xlings self doctor --isolation`"
                          : "xlings install bwrap" });
        return 1;
    }
    // The restriction most desktops ship (Ubuntu 23.10+). It is lifted once,
    // for this machine, by one narrow profile -- before anything is fetched.
    auto candidates = caps::bwrap_candidates(home, ports, /*fresh=*/true);
    if (std::ranges::none_of(candidates, [](const caps::Backend& b) { return b.source != "root-owned"; })) {
        const std::vector<std::string> bwrap{"bwrap"};
        if (xim::cmd_install_step(bwrap, "", stream, /*plumbing=*/true) != 0) return 1;
        candidates = caps::bwrap_candidates(home, ports, /*fresh=*/true);
    }
    log::println("this machine needs a one-time setup before it can run a root:");
    log::println("  AppArmor keeps programs without a profile from making the sandbox a root runs in.");
    log::println("  The setup installs a root-owned bwrap and one AppArmor profile that allows it");
    log::println("  exactly that -- once, for every user of this machine.");
    return repair_isolation_(home, ports, candidates, yes, stream, "do it now? (sudo)");
}

}
