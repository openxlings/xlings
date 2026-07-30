module;

// System headers used by use_spawn_shell only. `import std;` doesn't
// pull these in, and we want execl/errno (POSIX) or CreateProcess
// (Win32) without #include in the named-module purview (which the
// standard forbids for headers that aren't importable units).
#include <cstdio>  // stderr (used by std::println(stderr, ...))
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#endif

export module xlings.core.subos;

import std;

import xlings.core.config;
import xlings.core.home_config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.runtime;
import xlings.core.utils;
import xlings.core.xself;
import xlings.core.xim.commands;  // auto_install_backend_ needs cmd_install
import xlings.core.subos.keeper;
import xlings.core.subos.gpu;

namespace xlings::subos {

namespace fs = std::filesystem;

export struct SubosInfo {
    std::string   name;
    fs::path      dir;
    bool          isActive;
    int           toolCount;
};

nlohmann::json read_config_json_(const fs::path& path) {
    if (!fs::exists(path)) return nlohmann::json::object();
    try {
        auto content = platform::read_file_to_string(path.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        return json.is_discarded() ? nlohmann::json::object() : json;
    } catch (...) { return nlohmann::json::object(); }
}

void write_config_json_(const fs::path& path, const nlohmann::json& json) {
    platform::write_string_to_file(path.string(), json.dump(2));
}

export std::vector<SubosInfo> list_all() {
    auto& p = Config::paths();
    auto configPath = p.homeDir / ".xlings.json";
    auto json = read_config_json_(configPath);

    std::vector<SubosInfo> result;

    if (json.contains("subos") && json["subos"].is_object()) {
        for (auto it = json["subos"].begin(); it != json["subos"].end(); ++it) {
            auto name = it.key();
            auto dir  = Config::subos_dir(name);
            int toolCount = 0;
            auto binDir   = dir / "bin";
            if (fs::exists(binDir)) {
                for (auto& e : platform::dir_entries(binDir)) {
                    auto stem = e.path().stem().string();
                    if (!xself::is_builtin_shim(stem) && stem != "xvm-alias")
                        ++toolCount;
                }
            }
            result.push_back({name, dir, p.activeSubos == name, toolCount});
        }
    } else {
        result.push_back({"default", Config::subos_dir("default"),
                          p.activeSubos == "default", 0});
    }

    std::ranges::sort(result, {}, &SubosInfo::name);
    return result;
}

void update_current_symlink_(EventStream& stream,
                              const fs::path& homeDir,
                              const fs::path& targetDir) {
    auto linkPath = homeDir / "subos" / "current";
    std::error_code ec;
    fs::remove(linkPath, ec);
    // The same helper `self init` uses to create this link, rather than
    // `fs::create_directory_symlink` directly. On Windows a symlink needs
    // developer mode or elevation and a junction does not, so the two spellings
    // disagree exactly there: init would lay down a junction and every later
    // switch would fail to replace it, leaving `subos/current` pointing at
    // whatever was active the day the home was created -- while `subos list`
    // reported the switch as done.
    if (!platform::create_directory_link(linkPath, targetDir)) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Permission,
            .message = std::format("failed to update current symlink: {}",
                                   Config::display_path(linkPath)),
            .recoverable = true,
        });
    }
}

// ─────────────────────────────────────────────────────────────────────
// Sandbox subos helpers (0.4.23 V4 — see .agents/docs/sandbox-v4-design.md)
//
// V4 model: sandbox is NOT a creation property of subos. It's a `use`
// modifier — `xlings subos use <name> --sandbox` enters the same subos
// via proot fs-isolation, with sandbox-private $HOME / /tmp / /etc and
// host-shared ~/.xlings / /usr / /lib*. Real user identity (no fake
// root). Shell is whatever $SHELL is. Prompt switches from
// `[xsubos:<name>]` to `<xsubos:<name>>` to signal sandbox mode.
//
// Helpers here:
//   - sandbox_detail_::init_sandbox_dirs_ — lazy-init the per-subos
//     sandbox dirs (<subos>/{home/<user>, tmp, etc/...}) at first
//     `subos use --sandbox`. Idempotent.
//   - sandbox_detail_::locate_proot_ — search for the proot binary
//     (defined later, alongside build_proot_argv_).
//   - sandbox_detail_::build_proot_argv_ — assemble the proot CLI for
//     entering the sandbox (defined later).
//
// Note: V1.1-V1.3 (`--sandbox-shell <xpkg>`, `sandbox-shell` /
// `sandbox-shell-xpkg` config fields, eager shell install at
// create-time) was removed in V4. Old sandboxes still in user homes
// retain those fields; V4 silently ignores them, and `subos use
// --sandbox` works on them because init_sandbox_dirs_ is idempotent.
// ─────────────────────────────────────────────────────────────────────

// ── Storage mode (V6) ──────────────────────────────────────────────
// Storage isolation mode for sandbox data. Set at subos creation time
// via `--storage <mode>`, persisted in subos/.xlings.json["storage"].
// `image` and `tmpfs` are consumed only on the sandbox path; shell-
// level entry stays env/PATH-only regardless of storage mode (the two
// axes are orthogonal per V4 design — see use_spawn_shell).
enum class StorageMode { Shared, Image, Tmpfs };

inline std::string storage_to_string_(StorageMode m) {
    switch (m) {
    case StorageMode::Image: return "image";
    case StorageMode::Tmpfs: return "tmpfs";
    default: return "shared";
    }
}

inline StorageMode storage_from_string_(const std::string& s) {
    if (s == "image") return StorageMode::Image;
    if (s == "tmpfs") return StorageMode::Tmpfs;
    return StorageMode::Shared;
}

// Read storage mode from subos config file.
StorageMode read_storage_mode_(const fs::path& subos_dir) {
    auto cfg = subos_dir / ".xlings.json";
    if (!fs::exists(cfg)) return StorageMode::Shared;
    auto json = read_config_json_(cfg);
    if (json.contains("storage") && json["storage"].is_string())
        return storage_from_string_(json["storage"].get<std::string>());
    return StorageMode::Shared;
}

namespace sandbox_detail_ {

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
int init_image_(const fs::path& img, const std::string& size) {
    if (fs::exists(img)) return 0;
    auto truncate_cmd = "truncate -s " + size + " " + img.string();
    if (std::system(truncate_cmd.c_str()) != 0) return 1;
    auto mkfs_cmd = "mkfs.ext4 -F -m 0 -q " + img.string() + " 2>/dev/null";
    return std::system(mkfs_cmd.c_str());
}

// Check if a path is already a mountpoint.
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

// Unmount an image file.
int unmount_image_(const fs::path& mountpoint) {
    if (!is_mounted_(mountpoint)) return 0;
    auto cmd = platform::priv_prefix() + "umount " + mountpoint.string()
               + " 2>/dev/null";
    return std::system(cmd.c_str());
}

} // namespace sandbox_detail_

// Create a subos. V6: storage mode is a creation-time property
// (`--storage image|tmpfs|shared`). Non-shared modes force sandbox
// entry at use-time. The sandbox-private dirs are laid down lazily.
export int create(const std::string& name, const fs::path& customDir,
                  StorageMode storage, const std::string& imageSize,
                  EventStream& stream) {
    auto& p = Config::paths();

    if (name == "current") {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "'current' is a reserved subos name",
            .recoverable = false,
        });
        return 1;
    }

    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = "invalid subos name: '" + name
                           + "' (allowed: alphanumeric, underscore, dash)",
                .recoverable = false,
            });
            return 1;
        }
    }

    // Cheap pre-check so the common "already exists" mistake fails before we
    // lay down directories or run mkfs. It is advisory only -- the binding
    // check that actually decides is the one inside the locked commit below.
    if (read_home_config(p.homeDir).value("subos", nlohmann::json::object())
            .contains(name)) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "subos '" + name + "' already exists",
            .recoverable = false,
        });
        return 1;
    }

    auto dir = customDir.empty() ? (p.homeDir / "subos" / name) : customDir;

    fs::create_directories(dir / "bin");
    fs::create_directories(dir / "lib");
    fs::create_directories(dir / "usr");
    fs::create_directories(dir / "generations");

    auto subosConfig = dir / ".xlings.json";
    if (!fs::exists(subosConfig)) {
        nlohmann::json j;
        j["workspace"] = nlohmann::json::object();
        if (storage != StorageMode::Shared)
            j["storage"] = storage_to_string_(storage);
        if (storage == StorageMode::Image)
            j["imageSize"] = imageSize;
        write_config_json_(subosConfig, j);
    }

    // Image mode: create sparse ext4 image
    if (storage == StorageMode::Image) {
        auto rc = sandbox_detail_::init_image_(dir / "home.img", imageSize);
        if (rc != 0) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Internal,
                .message = "failed to create home.img",
                .recoverable = false,
                .hint = "ensure mkfs.ext4 is available (e2fsprogs)",
            });
            return 1;
        }
        fs::create_directories(dir / ".mountpoint");
    }

    // Create shim hardlinks from xlings binary
    auto xlingsBin = p.homeDir / "xlings";
    if (!fs::exists(xlingsBin))
        xlingsBin = p.homeDir / "bin" / "xlings";
    if (fs::exists(xlingsBin)) {
        xself::ensure_subos_shims(dir / "bin", xlingsBin, p.homeDir);
    }

    // Everything above this point -- mkfs.ext4 for image storage in
    // particular -- can take seconds. Reading the config before it and
    // writing it after would put back a document that predates any install
    // that finished meanwhile. Re-read under the lock and edit only our key.
    bool raced = false;
    auto committed = update_home_config(p.homeDir, [&](nlohmann::json& json) {
        if (!json.contains("subos") || !json["subos"].is_object()) {
            json["subos"] = nlohmann::json::object();
        }
        if (json["subos"].contains(name)) {
            raced = true;
            return false;
        }
        json["subos"][name] =
            {{"dir", customDir.empty() ? "" : customDir.string()}};
        return true;
    });
    if (!committed) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = "subos '" + name + "' was created on disk but could "
                       "not be recorded: " + committed.error(),
            .recoverable = true,
            .hint = "retry once the other xlings finishes; the directory at "
                    + dir.string() + " is reused as-is",
        });
        return 1;
    }
    if (raced) {
        // Another xlings registered this name while we were building. Its
        // entry is the one on disk; ours would overwrite a directory the
        // other command is using.
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "subos '" + name + "' already exists",
            .recoverable = false,
        });
        return 1;
    }

    nlohmann::json payload;
    payload["name"] = name;
    payload["dir"]  = dir.string();
    payload["storage"] = storage_to_string_(storage);
    stream.emit(DataEvent{"subos_created", payload.dump()});
    return 0;
}

// Back-compat overload (no storage argument → shared).
export int create(const std::string& name, const fs::path& customDir,
                  EventStream& stream) {
    return create(name, customDir, StorageMode::Shared, "50G", stream);
}

// ─────────────────────────────────────────────────────────────────────
// M2: subos new --from <spec>
//
// Two flavours dispatched by spec shape:
//   1. pkg-spec (`<ns>:<name>[@<ver>]` or `<name>@<ver>`): the source is
//      a `type="subos"` xpkg. If not yet installed, this auto-invokes
//      `xlings install <spec>` (E5 — agent always 1 command). After
//      install, the base lives at xpkgs/<ns>-x-<name>/<ver>/.
//   2. local-name: source is an existing subos in subos/<name>/. Plain
//      local fork.
//
// Both flavours land in subos/<new-name>/ with content copied from the
// base. xpkg deps remain global (shared via xpkgs/) so the fork is
// near-instant for shared storage; image storage clones home.img too.
//
// Cross-platform copy: Linux reflink-where-possible via `cp -a
// --reflink=auto`, macOS APFS clonefile via `cp -ac`, Windows std::fs
// recursive copy (full byte copy).
//
// Refs: .agents/docs/subos-as-xpkg-design-2026-05-16.md (M2, E1-E5).
namespace new_from_detail_ {

bool is_pkg_spec_(const std::string& spec) {
    return spec.find(':') != std::string::npos || spec.find('@') != std::string::npos;
}

// Parse `[<ns>:]<name>[@<ver>]` → {ns, name, ver}. Empty ns if absent;
// empty ver if absent ("latest" semantics handled downstream).
struct PkgRef {
    std::string ns;
    std::string name;
    std::string ver;
};

PkgRef parse_pkg_spec_(const std::string& spec) {
    PkgRef r;
    std::string rest = spec;
    if (auto colon = rest.find(':'); colon != std::string::npos) {
        r.ns = rest.substr(0, colon);
        rest = rest.substr(colon + 1);
    }
    if (auto at = rest.find('@'); at != std::string::npos) {
        r.name = rest.substr(0, at);
        r.ver  = rest.substr(at + 1);
    } else {
        r.name = rest;
    }
    return r;
}

// Recursive directory copy with reflink/clonefile preferred where the
// filesystem supports COW. Falls back to full byte-copy. Excludes the
// caller's xlings binary shims (those are minted fresh in the target).
int copy_tree_(const fs::path& src, const fs::path& dst,
               EventStream& stream) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = "failed to create fork target dir: " + ec.message(),
            .recoverable = false,
        });
        return 1;
    }

    // Use the system cp with reflink/clonefile flags; falls back to
    // full copy when the FS doesn't support it. Skip the bin/ subtree
    // here — shims are regenerated below.
    std::string copy_cmd;
#if defined(__linux__)
    // cp -a preserves mode/ownership/timestamps; --reflink=auto uses
    // COW where available (btrfs/xfs) and full copy otherwise.
    copy_cmd = std::format(
        "cp -a --reflink=auto '{}/.' '{}/'", src.string(), dst.string());
#elif defined(__APPLE__)
    // APFS clonefile via /bin/cp -c
    copy_cmd = std::format("cp -ac '{}/.' '{}/'", src.string(), dst.string());
#endif

    if (!copy_cmd.empty()) {
        auto rc = std::system(copy_cmd.c_str());
        if (rc != 0) {
            log::warn("cp -a/--reflink failed (rc={}), falling back to "
                      "std::filesystem::copy", rc);
            fs::copy(src, dst,
                     fs::copy_options::recursive |
                     fs::copy_options::overwrite_existing |
                     fs::copy_options::copy_symlinks, ec);
            if (ec) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::Internal,
                    .message = "fork copy failed: " + ec.message(),
                    .recoverable = false,
                });
                return 1;
            }
        }
    } else {
        // Windows / generic
        fs::copy(src, dst,
                 fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing |
                 fs::copy_options::copy_symlinks, ec);
        if (ec) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Internal,
                .message = "fork copy failed: " + ec.message(),
                .recoverable = false,
            });
            return 1;
        }
    }
    return 0;
}

// Locate xpkgs/<ns>-x-<name>/<ver>/ for a parsed pkg ref. Returns empty
// path if no matching install exists.
fs::path locate_base_pkg_(const PkgRef& ref) {
    auto& p = Config::paths();
    auto storeName = ref.ns.empty() ? ref.name : (ref.ns + "-x-" + ref.name);
    auto base = p.dataDir / "xpkgs" / storeName;
    if (!fs::is_directory(base)) return {};

    // Specific version requested
    if (!ref.ver.empty()) {
        auto candidate = base / ref.ver;
        return fs::is_directory(candidate) ? candidate : fs::path{};
    }

    // No version → take the highest-sorted installed version directory.
    fs::path latest;
    std::error_code ec;
    for (auto it = fs::directory_iterator(base, ec);
         !ec && it != std::default_sentinel; it.increment(ec)) {
        if (it->is_directory(ec)) latest = it->path();
    }
    return latest;
}

} // namespace new_from_detail_

export int new_from(const std::string& name, const fs::path& customDir,
                    StorageMode storage, const std::string& imageSize,
                    const std::string& fromSpec, EventStream& stream) {
    auto& p = Config::paths();

    fs::path baseDir;

    if (new_from_detail_::is_pkg_spec_(fromSpec)) {
        // ── pkg-spec path: locate or install the base xpkg ────────────
        auto ref = new_from_detail_::parse_pkg_spec_(fromSpec);
        baseDir = new_from_detail_::locate_base_pkg_(ref);

        if (baseDir.empty()) {
            // Auto-install (E5a): invoke `xlings install <spec>` so the
            // base lands at xpkgs/<ns>-x-<name>/<ver>/. We use the host
            // xlings binary (same process binary) so the install runs
            // with the same context (XLINGS_HOME, mirror config, etc.).
            log::info("base subos pkg '{}' not installed; auto-installing...",
                      fromSpec);
            auto xlings_bin = p.homeDir / "xlings";
            if (!fs::exists(xlings_bin))
                xlings_bin = p.homeDir / "bin" / "xlings";

            auto cmd = std::format("{} install -y {}",
                                   xlings_bin.string(), fromSpec);
            auto rc = std::system(cmd.c_str());
            if (rc != 0) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::Internal,
                    .message = "auto-install of base '" + fromSpec
                               + "' failed",
                    .recoverable = true,
                    .hint = "run manually: xlings install " + fromSpec,
                });
                return 1;
            }
            baseDir = new_from_detail_::locate_base_pkg_(ref);
        }

        if (baseDir.empty()) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "couldn't locate base pkg payload for '" + fromSpec
                           + "' after install",
                .recoverable = false,
            });
            return 1;
        }
    } else {
        // ── local fork path: source is an existing subos by name ──────
        baseDir = p.homeDir / "subos" / fromSpec;
        if (!fs::is_directory(baseDir)) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "source subos '" + fromSpec + "' not found",
                .recoverable = true,
                .hint = "list available: xlings subos list",
            });
            return 1;
        }
    }

    // Validate base shape: must contain .xlings.json for fork to make sense
    if (!fs::is_regular_file(baseDir / ".xlings.json")) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "source '" + fromSpec
                       + "' has no .xlings.json (not a valid fork source)",
            .recoverable = false,
        });
        return 1;
    }

    // Create target subos via standard `create`. This sets up
    // bin/lib/usr/generations, writes initial .xlings.json, optionally
    // creates home.img, and registers the subos.
    if (auto rc = create(name, customDir, storage, imageSize, stream); rc != 0) {
        return rc;
    }

    auto dstDir = customDir.empty() ? (p.homeDir / "subos" / name) : customDir;

    // Overlay base content on top — workspace (.xlings.json), any
    // templates/static files. We re-issue create()'s file writes
    // afterwards for storage/imageSize keys so the new subos's own
    // storage choice wins over the base's. The base's .xlings.json
    // workspace map is the data we want to inherit.
    if (auto rc = new_from_detail_::copy_tree_(baseDir, dstDir, stream); rc != 0) {
        return rc;
    }

    // Restore storage/imageSize fields in target's .xlings.json since
    // copy_tree_ overwrote it with base's version (base usually has no
    // explicit storage key — it inherits at fork time).
    auto subosCfgPath = dstDir / ".xlings.json";
    auto subosCfg = read_config_json_(subosCfgPath);
    if (storage != StorageMode::Shared)
        subosCfg["storage"] = storage_to_string_(storage);
    else
        subosCfg.erase("storage");
    if (storage == StorageMode::Image)
        subosCfg["imageSize"] = imageSize;
    else
        subosCfg.erase("imageSize");
    write_config_json_(subosCfgPath, subosCfg);

    // Re-mint subos shims (they may have been clobbered by copy_tree_
    // if base happened to ship its own bin/ — defensive).
    auto xlingsBin = p.homeDir / "xlings";
    if (!fs::exists(xlingsBin))
        xlingsBin = p.homeDir / "bin" / "xlings";
    if (fs::exists(xlingsBin)) {
        xself::ensure_subos_shims(dstDir / "bin", xlingsBin, p.homeDir);
    }

    nlohmann::json payload;
    payload["name"]    = name;
    payload["from"]    = fromSpec;
    payload["base"]    = baseDir.string();
    payload["storage"] = storage_to_string_(storage);
    stream.emit(DataEvent{"subos_forked", payload.dump()});
    return 0;
}

// `xlings subos use` modes:
//
//   spawn  (default) — exec a fresh interactive $SHELL with
//                      XLINGS_ACTIVE_SUBOS=<name> set. The user lives in a
//                      sub-context until they `exit`, then PATH / env are
//                      restored to whatever the parent shell had. Other
//                      shells are unaffected.
//   shell  (--shell) — emit shell code on stdout for the user to eval into
//                      the current shell. No fork. Useful in scripts and for
//                      tools that don't want a sub-shell layer.
//   global (--global)— legacy behavior: write activeSubos into
//                      ~/.xlings.json and re-point subos/current symlink.
//                      Affects every shell that picks up the next profile
//                      load (i.e., new shells; existing shells via the
//                      indirect symlink).
//
// All three share validation: subos must exist in ~/.xlings.json's subos map.

namespace use_detail_ {

inline int validate_subos_(const std::string& name, EventStream& stream) {
    auto& p = Config::paths();
    auto json = read_config_json_(p.homeDir / ".xlings.json");
    if (!json.contains("subos") || !json["subos"].contains(name)) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::NotFound,
            .message = "subos '" + name + "' not found",
            .recoverable = true,
            .hint = "create it first: xlings subos new " + name,
        });
        return 1;
    }
    return 0;
}

// Build the new PATH for shell-level switching. The fresh subos bin goes to
// the front; any *other* xlings-managed subos bin segments are removed so
// repeated --shell / spawn calls don't pile up stale entries. The PATH
// separator differs by platform — `;` on Windows, `:` everywhere else.
inline std::string rebuild_path_for_subos_(const std::string& orig_path,
                                          const fs::path& home_dir,
                                          const fs::path& new_bin) {
#if defined(_WIN32)
    constexpr char SEP = ';';
#else
    constexpr char SEP = ':';
#endif
    auto subos_root = (home_dir / "subos").string();
    std::string out;
    out.reserve(orig_path.size() + new_bin.string().size() + 1);

    out += new_bin.string();

    std::size_t pos = 0;
    while (pos < orig_path.size()) {
        auto sep = orig_path.find(SEP, pos);
        auto end = (sep == std::string::npos) ? orig_path.size() : sep;
        std::string_view seg{orig_path.data() + pos, end - pos};
        // Skip empty segments and any xlings-owned subos bin — those will
        // be replaced by new_bin.
        bool is_subos_bin =
            seg.size() > subos_root.size() &&
            seg.starts_with(subos_root) &&
            (seg[subos_root.size()] == '/' || seg[subos_root.size()] == '\\');
        if (!seg.empty() && !is_subos_bin) {
            out += SEP;
            out.append(seg);
        }
        if (sep == std::string::npos) break;
        pos = sep + 1;
    }
    return out;
}

} // namespace use_detail_

// Internal — not exported. `xlings subos use --global <name>` and
// the back-compat single-arg `use()` both route here.
int use_global(const std::string& name, EventStream& stream) {
    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;

    auto& p = Config::paths();
    // The window here is short, but a full-document rewrite is a full-document
    // rewrite: an install committing between the read and the write loses its
    // `versions` entry all the same.
    auto committed = update_home_config(p.homeDir, [&](nlohmann::json& json) {
        json["activeSubos"] = name;
        return true;
    });
    if (!committed) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = "failed to switch subos: " + committed.error(),
            .recoverable = true,
        });
        return 1;
    }

    auto dir = Config::subos_dir(name);
    update_current_symlink_(stream, p.homeDir, dir);

    nlohmann::json payload;
    payload["name"] = name;
    payload["dir"]  = dir.string();
    stream.emit(DataEvent{"subos_switched", payload.dump()});
    return 0;
}

// Internal — not exported. Powers the hidden `--shell <kind>` flag,
// kept available for tests and power users that want eval-able output
// without a sub-shell layer. The default user-facing path is
// use_spawn_shell; --shell is intentionally not in the help text.
// Shell-level entry never activates storage isolation (V4 orthogonality
// — see use_spawn_shell). Emit a single hint to stderr if the subos
// was created with image/tmpfs storage so the user understands the
// attribute is dormant in this entry. Writes to stderr (not stdout)
// so the --shell <kind> path stays eval-safe.
void warn_storage_dormant_on_shell_(const std::string& name) {
    auto& p = Config::paths();
    auto storage = read_storage_mode_(p.homeDir / "subos" / name);
    if (storage == StorageMode::Shared) return;
    // Hint disabled: wording was ambiguous ("use --sandbox to activate"
    // reads as either the verb or the `subos use` command) and it fired
    // on every shell-level entry for image/tmpfs subos, becoming noise
    // once the user already knows the layout. Revisit with a clearer
    // one-time / opt-in form before re-enabling.
    // std::println(stderr,
    //              "[xlings] storage={} is sandbox-only; entering "
    //              "shell-level (use --sandbox to activate)",
    //              storage_to_string_(storage));
    (void)storage;
}

int use_emit_shell(const std::string& name,
                          std::string_view shell_kind,
                          EventStream& stream) {
    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;
    warn_storage_dormant_on_shell_(name);

    auto& p = Config::paths();
    auto bin_dir = p.homeDir / "subos" / name / "bin";

    bool is_fish = (shell_kind == "fish");
    bool is_pwsh = (shell_kind == "pwsh" || shell_kind == "powershell" ||
                    shell_kind == "ps1" || shell_kind == "ps");

    if (is_fish) {
        std::println(R"(set -gx XLINGS_ACTIVE_SUBOS "{}";)", name);
        std::println(R"(set -gx XLINGS_BIN "{}";)", bin_dir.string());
        // Strip any old subos bin segments from PATH, then prepend the new
        // bin. fish's $PATH is a list, so we use string match -v.
        std::println(R"(set -gx PATH "{}" (string match -v -r "^{}/subos/[^/]+/bin$" -- $PATH);)",
                     bin_dir.string(), p.homeDir.string());
        return 0;
    }
    if (is_pwsh) {
        std::println(R"($env:XLINGS_ACTIVE_SUBOS = '{}')", name);
        std::println(R"($env:XLINGS_BIN = '{}')", bin_dir.string());
        std::println(R"($env:Path = '{}' + ';' + (($env:Path -split ';') -notmatch '^{}\\subos\\[^\\]+\\bin$' -join ';'))",
                     bin_dir.string(), p.homeDir.string());
        return 0;
    }
    // POSIX (sh/bash/zsh) default
    auto orig_path = utils::get_env_or_default("PATH");
    auto new_path  = use_detail_::rebuild_path_for_subos_(
        orig_path, p.homeDir, bin_dir);
    std::println(R"(export XLINGS_ACTIVE_SUBOS="{}";)", name);
    std::println(R"(export XLINGS_BIN="{}";)", bin_dir.string());
    std::println(R"(export PATH="{}";)", new_path);
    return 0;
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

namespace sandbox_detail_ {

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

} // namespace sandbox_detail_

// V5 sandbox entry. Reached via `xlings subos use <name> --sandbox [backend]`.
// Linux-only. Dual backend: bwrap (preferred, native compat) or proot
// (fallback, zero-privilege). Auto-detects which is available; user can
// force one via `--sandbox bwrap` or `--sandbox proot`.
//
// See .agents/docs/sandbox-v5-dual-backend-design.md for full design.
int use_sandbox_mode_(const std::string& name, EventStream& stream,
                      const std::string& preferred_backend = "",
                      bool gpu = false,
                      const std::string& cmd = "") {
    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;

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
        auto rc = sandbox_detail_::mount_image_(img, image_mountpoint, user);
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
        sandbox_detail_::init_sandbox_dirs_(subos_dir, user, uid, gid);
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

    // Backend selection
    using sandbox_detail_::SandboxBackend;
    using sandbox_detail_::BackendInfo;
    std::optional<BackendInfo> backend;

    if (preferred_backend == "bwrap") {
        auto bin = sandbox_detail_::locate_bwrap_(p.homeDir);
        if (!bin) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "bwrap not installed",
                .recoverable = false,
                .hint = "run: xlings install bwrap",
            });
            return 1;
        }
        if (auto probe = sandbox_detail_::probe_bwrap_(*bin); !probe.first) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "bwrap probe failed",
                .recoverable = false,
                .hint = sandbox_detail_::classify_bwrap_probe_error_(
                            probe.second, *bin),
            });
            return 1;
        }
        backend = BackendInfo{ SandboxBackend::Bwrap, *bin };
    } else if (preferred_backend == "proot") {
        auto bin = sandbox_detail_::locate_proot_(p.homeDir);
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
        backend = sandbox_detail_::detect_backend_(p.homeDir, subos_dir);
        if (!backend) {
            auto rc = sandbox_detail_::auto_install_backend_(p.homeDir, stream);
            if (rc != 0) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::NotFound,
                    .message = "failed to install sandbox backend",
                    .recoverable = false,
                    .hint = "manually: xlings install bwrap (or: xlings install proot)",
                });
                return 1;
            }
            backend = sandbox_detail_::detect_backend_(p.homeDir, subos_dir);
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
        if (auto bin = sandbox_detail_::locate_bwrap_(p.homeDir)) {
            if (auto probe = sandbox_detail_::probe_bwrap_(*bin);
                !probe.first)
            {
                hint = sandbox_detail_::classify_bwrap_probe_error_(
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
            sandbox_detail_::unmount_image_(image_mountpoint);
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
        argv = sandbox_detail_::build_bwrap_argv_(
            backend->binary, subos_dir, p.homeDir, user, shell,
            interactive_shell, storage, image_mountpoint, gpu, cmd);
    } else {
        // proot already exposes the full host /dev and /sys via
        // `--bind=/dev:/dev --bind=/sys:/sys`, so GPU devices are
        // visible for free. --gpu is a no-op here.
        argv = sandbox_detail_::build_proot_argv_(
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
            sandbox_detail_::unmount_image_(image_mountpoint);
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
        sandbox_detail_::unmount_image_(image_mountpoint);
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

// Spawn a fresh interactive shell with XLINGS_ACTIVE_SUBOS set. Uses execvp
// to replace the current process — xlings exits, the new shell takes its
// place, and `exit` returns to the parent shell with original env intact.
//
// On Windows we'd need CreateProcess + WaitForSingleObject (no exec
// equivalent); for now POSIX-only with a stub that falls back to global
// mode on Windows.
// Internal — not exported. Default `xlings subos use <name>` routes here.
// Replaces the xlings process with a fresh interactive shell that has
// XLINGS_ACTIVE_SUBOS set; `exit` returns the user to the parent shell.
//
// Nesting policy:
//   * already in the same subos → print a friendly note and exit 0
//     (no point spawning a redundant duplicate layer);
//   * already in a different subos → spawn anyway (intentional nesting),
//     but print one line so the user can see the layering they just
//     created and remembers `exit` returns them to the previous one.
//
// "Exit current subos then enter new" is not implementable from a child
// process without shell-function infrastructure (we'd have to manipulate
// the parent shell's env, which exec(2) cannot do). Users who really
// want flat semantics type `exit` first.
int use_spawn_shell(const std::string& name, EventStream& stream,
                    bool sandbox = false,
                    const std::string& sandbox_backend = "",
                    bool gpu = false,
                    const std::string& cmd = "")
{
    // V5: --sandbox [backend] is a `use`-time modifier. Dispatch to the
    // sandbox path when set; auto-detect backend (bwrap preferred, proot
    // fallback) or use the explicitly requested one.
    //
    // Storage mode (image/tmpfs) and sandbox are orthogonal axes per
    // V4 design: shell-level entry only swaps env/PATH and never
    // mounts. Image / tmpfs only take effect when `--sandbox` is
    // explicitly passed — earlier V6 auto-upgrade fused the two axes
    // and made `subos use <image-subos>` silently require root, bwrap,
    // and a working mount namespace just to switch shells.
    //
    // M3: `cmd` non-empty switches to non-interactive single-command
    // execution — `shell -c <cmd>` instead of an interactive shell.
    // Useful for scripts and agent workflows.
    if (sandbox) return use_sandbox_mode_(name, stream, sandbox_backend, gpu, cmd);
    warn_storage_dormant_on_shell_(name);

    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;

    auto already = utils::get_env_or_default("XLINGS_ACTIVE_SUBOS");
    if (already == name) {
        nlohmann::json p; p["name"] = name;
        stream.emit(DataEvent{"subos_already_in", p.dump()});
        return 0;
    }
    if (!already.empty()) {
        nlohmann::json p; p["from"] = already; p["to"] = name;
        stream.emit(DataEvent{"subos_nesting", p.dump()});
    }

    auto& p = Config::paths();
    auto bin_dir = p.homeDir / "subos" / name / "bin";

    auto orig_path = utils::get_env_or_default("PATH");
    auto new_path  = use_detail_::rebuild_path_for_subos_(
        orig_path, p.homeDir, bin_dir);

    // Set env BEFORE spawning so the child shell inherits it. The profile
    // sourced by the child reads XLINGS_ACTIVE_SUBOS and re-computes
    // XLINGS_BIN; XLINGS_BIN we set here is mostly defensive (covers the
    // case where the child shell is started with --norc and never sources
    // the profile).
    platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
    platform::set_env_variable("XLINGS_BIN", bin_dir.string());
    platform::set_env_variable("PATH", new_path);

    nlohmann::json payload;
    payload["name"] = name;
    payload["mode"] = "spawn";
    stream.emit(DataEvent{"subos_entering", payload.dump()});

    // Flush std streams before exec/CreateProcess — buffered output isn't
    // preserved across execve(2), and on Windows the child shell may start
    // writing before the parent's pending output drains; in either case
    // CI capture (where stdout is a pipe rather than a TTY) loses any
    // block-buffered bytes that didn't get flushed.
    std::cout.flush();
    std::cerr.flush();

#if defined(_WIN32)
    // Windows: CreateProcess + WaitForSingleObject. We try shells in
    // preference order (pwsh > powershell > cmd) and inherit the parent's
    // stdio handles so the user can interact normally. Unlike POSIX exec,
    // CreateProcess can't replace the current process — xlings stays
    // alive parked on WaitForSingleObject. The child's exit code becomes
    // ours so `xlings subos use` exits with whatever the shell exited.
    constexpr const char* shells[] = { "pwsh.exe", "powershell.exe", "cmd.exe" };
    for (auto* exe : shells) {
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        // CreateProcessA needs a writable command-line buffer; std::string
        // ::data() returns a non-const char* since C++17. We pass null for
        // lpApplicationName so Windows resolves the bare exe via PATH.
        //
        // M3: when `cmd` is set, append the appropriate non-interactive
        // single-command flag. pwsh/powershell use `-Command "<cmd>"`;
        // cmd.exe uses `/c "<cmd>"`.
        std::string cmdline = exe;
        if (!cmd.empty()) {
            std::string_view exe_sv = exe;
            if (exe_sv.find("cmd.exe") != std::string_view::npos) {
                cmdline += " /c \"" + cmd + "\"";
            } else {
                cmdline += " -Command \"" + cmd + "\"";
            }
        }
        if (::CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr,
                             /*bInheritHandles=*/TRUE,
                             /*dwCreationFlags=*/0,
                             /*lpEnvironment=*/nullptr,
                             /*lpCurrentDirectory=*/nullptr,
                             &si, &pi)) {
            ::WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD exitCode = 0;
            ::GetExitCodeProcess(pi.hProcess, &exitCode);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            return static_cast<int>(exitCode);
        }
    }
    log::error("could not launch any shell on Windows "
               "(tried pwsh.exe, powershell.exe, cmd.exe)");
    return 127;
#else
    // POSIX: exec(2) replaces the current process so xlings exits and the
    // child shell takes over. `exit` from that shell returns directly to
    // the parent shell with the original env intact.
    //
    // M3: `--cmd` switches to non-interactive single-command mode —
    // `shell -c <cmd>`. The shell exits after the command, propagating
    // its exit code as xlings's exit code.
    auto shell = utils::get_env_or_default("SHELL");
    if (shell.empty()) shell = "/bin/sh";
    if (!cmd.empty()) {
        ::execl(shell.c_str(), shell.c_str(), "-c", cmd.c_str(),
                static_cast<char*>(nullptr));
    } else {
        ::execl(shell.c_str(), shell.c_str(), "-i", static_cast<char*>(nullptr));
    }

    // Only reached if exec failed.
    log::error("failed to exec shell '{}': {}", shell, std::strerror(errno));
    return 127;
#endif
}

// Back-compat single-arg entry point: keeps existing callers (anyone who
// imports xlings::subos::use directly) on the legacy global behavior.
// CLI dispatch goes through `run()` which uses the flag-aware path below.
export int use(const std::string& name, EventStream& stream) {
    return use_global(name, stream);
}

export int remove(const std::string& name, EventStream& stream) {
    if (name == "default") {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "cannot remove the 'default' subos",
            .recoverable = false,
        });
        return 1;
    }

    auto& p = Config::paths();
    if (p.activeSubos == name) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "cannot remove the active subos '" + name + "'",
            .recoverable = true,
            .hint = "switch first: xlings subos use default",
        });
        return 1;
    }

    if (!read_home_config(p.homeDir).value("subos", nlohmann::json::object())
             .contains(name)) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::NotFound,
            .message = "subos '" + name + "' not found",
            .recoverable = true,
        });
        return 1;
    }

    auto dir = Config::subos_dir(name);
    if (fs::exists(dir)) {
        // V6: image-storage subos has an ext4 mount at <subos>/.mountpoint.
        // remove_all would either (a) hit EBUSY at the mountpoint, leaving
        // a half-cleaned tree behind, or (b) silently recurse into the
        // live mount and erase the image's contents before EBUSY surfaces.
        // Detect and umount first.
#if defined(__linux__)
        auto mountpoint = dir / ".mountpoint";
        if (fs::exists(mountpoint)
            && sandbox_detail_::is_mounted_(mountpoint))
        {
            if (sandbox_detail_::unmount_image_(mountpoint) != 0) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::Permission,
                    .message = "failed to unmount " + mountpoint.string()
                               + " (image storage subos); refusing to "
                                 "remove to avoid corrupting the live "
                                 "filesystem",
                    .recoverable = true,
                    .hint = "ensure no shell is inside this subos, then "
                            "retry — or manually: sudo umount "
                            + mountpoint.string(),
                });
                return 1;
            }
        }
#endif
        std::error_code ec;
        fs::remove_all(dir, ec);
        if (ec) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Permission,
                .message = "failed to remove " + dir.string() + ": " + ec.message(),
                .recoverable = false,
            });
            return 1;
        }
    }

    // remove_all above walks the whole subos tree and can run for a long
    // time on a big one. Deleting our key out of a document read before that
    // walk would resurrect every other key's pre-walk value.
    auto committed = update_home_config(p.homeDir, [&](nlohmann::json& json) {
        if (!json.contains("subos") || !json["subos"].is_object()) return false;
        return json["subos"].erase(name) > 0;
    });
    if (!committed) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = "subos '" + name + "' was deleted from disk but its "
                       "entry could not be removed: " + committed.error(),
            .recoverable = true,
            .hint = "retry once the other xlings finishes",
        });
        return 1;
    }

    nlohmann::json payload;
    payload["name"] = name;
    stream.emit(DataEvent{"subos_removed", payload.dump()});
    return 0;
}

export std::optional<SubosInfo> info(const std::string& name) {
    auto& p = Config::paths();
    auto dir = Config::subos_dir(name);
    if (!fs::exists(dir)) return std::nullopt;

    int toolCount = 0;
    auto binDir = dir / "bin";
    if (fs::exists(binDir)) {
        for (auto& e : platform::dir_entries(binDir)) {
            auto stem = e.path().stem().string();
            if (!xself::is_builtin_shim(stem) && stem != "xvm-alias")
                ++toolCount;
        }
    }
    return SubosInfo{name, dir, p.activeSubos == name, toolCount};
}

int run_list_(EventStream& stream) {
    auto all = list_all();
    std::vector<std::tuple<std::string, std::string, int, bool>> entries;
    for (auto& s : all) {
        entries.emplace_back(s.name, s.dir.string(), s.toolCount, s.isActive);
    }
    nlohmann::json entriesJson = nlohmann::json::array();
    for (auto& [n, d, tc, active] : entries) {
        entriesJson.push_back({{"name", n}, {"dir", d}, {"pkgCount", tc}, {"active", active}});
    }
    nlohmann::json payload;
    payload["entries"] = std::move(entriesJson);
    stream.emit(DataEvent{"subos_list", payload.dump()});
    return 0;
}

int run_info_(const std::string& name, EventStream& stream) {
    auto& p = Config::paths();
    auto target = name.empty() ? p.activeSubos : name;
    auto si = info(target);
    if (!si) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::NotFound,
            .message = "subos '" + target + "' not found",
            .recoverable = true,
        });
        return 1;
    }
    nlohmann::json fieldsJson = nlohmann::json::array();
    fieldsJson.push_back({{"label", "active"}, {"value", si->isActive ? "yes" : "no"}, {"highlight", si->isActive}});
    fieldsJson.push_back({{"label", "dir"}, {"value", si->dir.string()}, {"highlight", false}});
    fieldsJson.push_back({{"label", "tools"}, {"value", std::to_string(si->toolCount)}, {"highlight", false}});
    nlohmann::json payload;
    payload["title"] = si->name;
    payload["fields"] = std::move(fieldsJson);
    stream.emit(DataEvent{"info_panel", payload.dump()});
    return 0;
}

export int run(int argc, char* argv[], EventStream& stream) {
    if (argc < 3) return run_list_(stream);

    std::string sub = argv[2];
    if (sub == "ls") sub = "list";
    if (sub == "rm") sub = "remove";
    if (sub == "i")  sub = "info";

    auto usageError = [&](std::string_view detail) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = std::string(detail),
            .recoverable = false,
            .hint = "usage: xlings subos <new|use|list|ls|remove|rm|info|i|stop> [name]",
        });
    };

    if (sub == "new") {
        if (argc < 4) { usageError("missing <name> for: xlings subos new"); return 1; }
        // Parse: xlings subos new <name> [--storage <mode>] [--image-size <size>] [--from <spec>]
        std::string name;
        StorageMode storage = StorageMode::Shared;
        std::string imageSize = "50G";
        // M2: --from <spec> creates the new subos by forking an existing
        // source. Spec containing `:` or `@` is treated as a pkg-spec
        // (auto-installs the base xpkg if missing); bare name is treated
        // as a local subos to fork from.
        std::string fromSpec;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--storage" && i + 1 < argc) {
                auto s = std::string(argv[++i]);
                if (s == "image") storage = StorageMode::Image;
                else if (s == "tmpfs") storage = StorageMode::Tmpfs;
                else if (s == "shared") storage = StorageMode::Shared;
                else {
                    usageError("unknown storage mode: " + s
                               + " (valid: shared, image, tmpfs)");
                    return 1;
                }
            }
            else if (a == "--image-size" && i + 1 < argc) {
                imageSize = argv[++i];
            }
            else if (a == "--from" && i + 1 < argc) {
                fromSpec = argv[++i];
            }
            else if (a.rfind("--from=", 0) == 0) {
                fromSpec = a.substr(7);
            }
            else if (!a.empty() && a[0] != '-' && name.empty()) {
                name = std::move(a);
            }
            else {
                usageError("unknown option for `xlings subos new`: " + a);
                return 1;
            }
        }
        if (name.empty()) {
            usageError("missing <name> for: xlings subos new");
            return 1;
        }
        if (!fromSpec.empty()) {
            return new_from(name, {}, storage, imageSize, fromSpec, stream);
        }
        return create(name, {}, storage, imageSize, stream);
    }
    if (sub == "use") {
        // Flags supported:
        //   --global         persist the choice into ~/.xlings.json + symlink
        //                    (legacy behavior; affects every shell)
        //   --shell <kind>   emit shell code on stdout for the user to
        //                    eval/Invoke-Expression. <kind> ∈ {sh,bash,zsh,
        //                    fish,pwsh}. Defaults to "sh" if not provided.
        //   --sandbox        Linux-only: enter via proot fs-isolation. $HOME,
        //                    /tmp, /etc/passwd are sandbox-private; ~/.xlings,
        //                    /usr, /lib*, /etc/{resolv.conf,ld.so.cache} are
        //                    bound from host. Same shell (`$SHELL`) as outside.
        //                    Prompt switches from `[xsubos:<name>]` to
        //                    `<xsubos:<name>>` so the user can tell at a glance.
        //   (no flag)        spawn a fresh interactive shell with
        //                    XLINGS_ACTIVE_SUBOS=<name> in env. Per-shell.
        std::string name;
        std::string mode = "spawn";        // default
        std::string shell_kind = "sh";
        bool sandbox = false;
        std::string sandbox_backend;       // "" = auto, "bwrap", "proot"
        // M3: --cmd <string> runs a single command non-interactively
        // and exits with the command's exit code. Works in both shell-
        // level and sandbox modes. Internally routed to `sh -c <cmd>`
        // (POSIX) or `pwsh -Command <cmd>` / `cmd /c <cmd>` (Windows).
        std::string cmd;
        // M5: explicit keeper policy overrides (D9). The runtime auto-
        // default (storage=image|tmpfs + sandbox + Linux → keeper on,
        // TTL=5min) is encoded in keeper::should_auto_keeper. These
        // flags let the user override per call:
        //   --no-keep      force disable (one-shot, even if auto would)
        //   --keep         never-expiring (use until `subos stop`)
        //   --ttl <sec>    custom idle TTL
        bool no_keep = false;
        bool keep_forever = false;
        int  ttl_sec = 0;                  // 0 = use default
        // --gpu: opt-in NVIDIA + DRM device passthrough for bwrap
        // sandbox. Missing devices are silently skipped. Requires
        // --sandbox; ignored when the backend resolves to proot (proot
        // already passes /dev and /sys through wholesale).
        // Ref: .agents/docs/2026-05-22-subos-sandbox-gpu-passthrough.md
        bool gpu = false;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--global") { mode = "global"; }
            else if (a == "--shell") {
                mode = "shell";
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    shell_kind = argv[++i];
                }
            }
            else if (a.rfind("--shell=", 0) == 0) {
                mode = "shell";
                shell_kind = a.substr(8);
            }
            else if (a == "--sandbox") {
                sandbox = true;
                // Optional backend argument: --sandbox bwrap | --sandbox proot
                if (i + 1 < argc) {
                    std::string next = argv[i + 1];
                    if (next == "bwrap" || next == "proot") {
                        sandbox_backend = next;
                        ++i;
                    }
                }
            }
            else if (a == "--cmd" && i + 1 < argc) {
                cmd = argv[++i];
            }
            else if (a.rfind("--cmd=", 0) == 0) {
                cmd = a.substr(6);
            }
            // M5 keeper policy flags. The runtime spawning of the keeper
            // is wired separately (see keeper.cppm); these flags are
            // accepted on the CLI and threaded through for forward
            // compatibility. Auto-default (D9) is governed by
            // should_auto_keeper() at runtime.
            else if (a == "--no-keep") {
                no_keep = true;
            }
            else if (a == "--keep") {
                keep_forever = true;
            }
            else if (a == "--ttl" && i + 1 < argc) {
                try { ttl_sec = std::stoi(argv[++i]); }
                catch (...) {
                    usageError("--ttl expects an integer (seconds)");
                    return 1;
                }
            }
            else if (a.rfind("--ttl=", 0) == 0) {
                try { ttl_sec = std::stoi(std::string(a.substr(6))); }
                catch (...) {
                    usageError("--ttl=<sec> expects an integer");
                    return 1;
                }
            }
            else if (a == "--gpu") {
                gpu = true;
            }
            else if (!a.empty() && a[0] != '-' && name.empty()) {
                name = std::move(a);
            }
            else {
                usageError("unknown option for `xlings subos use`: " + a);
                return 1;
            }
        }
        if (name.empty()) { usageError("missing <name> for: xlings subos use"); return 1; }

        if (no_keep && keep_forever) {
            usageError("--no-keep and --keep are mutually exclusive");
            return 1;
        }

        if (gpu && !sandbox) {
            usageError("--gpu requires --sandbox "
                       "(GPU passthrough only applies to bwrap-sandboxed sessions)");
            return 1;
        }

        if (mode == "global") {
            if (!cmd.empty()) {
                usageError("--cmd is incompatible with --global "
                           "(--global persists the active subos but doesn't spawn a shell)");
                return 1;
            }
            return use_global(name, stream);
        }
        if (mode == "shell") {
            if (!cmd.empty()) {
                usageError("--cmd is incompatible with --shell <kind> "
                           "(--shell emits env code; use plain `subos use --cmd` for non-interactive exec)");
                return 1;
            }
            return use_emit_shell(name, shell_kind, stream);
        }
        return use_spawn_shell(name, stream, sandbox, sandbox_backend, gpu, cmd);
    }
    if (sub == "list")   return run_list_(stream);
    if (sub == "remove") {
        if (argc < 4) { usageError("missing <name> for: xlings subos remove|rm"); return 1; }
        return remove(argv[3], stream);
    }
    if (sub == "info")   return run_info_(argc > 3 ? argv[3] : "", stream);
    if (sub == "stop") {
        // M4/D9: stop the auto-keeper for a sandboxed subos.
        // Safe to invoke even when no keeper is running — it's a no-op.
        if (argc < 4) { usageError("missing <name> for: xlings subos stop"); return 1; }
        return keeper::stop_keeper(argv[3]);
    }

    usageError("unknown subcommand: " + sub);
    return 1;
}

} // namespace xlings::subos
