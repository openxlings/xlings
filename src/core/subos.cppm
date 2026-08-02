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
import xlings.core.subos.sandbox;

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
//   - sandbox::init_sandbox_dirs_ — lazy-init the per-subos
//     sandbox dirs (<subos>/{home/<user>, tmp, etc/...}) at first
//     `subos use --sandbox`. Idempotent.
//   - sandbox::locate_proot_ — search for the proot binary
//     (defined later, alongside build_proot_argv_).
//   - sandbox::build_proot_argv_ — assemble the proot CLI for
//     entering the sandbox (defined later).
//
// Note: V1.1-V1.3 (`--sandbox-shell <xpkg>`, `sandbox-shell` /
// `sandbox-shell-xpkg` config fields, eager shell install at
// create-time) was removed in V4. Old sandboxes still in user homes
// retain those fields; V4 silently ignores them, and `subos use
// --sandbox` works on them because init_sandbox_dirs_ is idempotent.
// ─────────────────────────────────────────────────────────────────────


// Create a subos. V6: storage mode is a creation-time property
// (`--storage image|tmpfs|shared`). Non-shared modes force sandbox
// entry at use-time. The sandbox-private dirs are laid down lazily.
export int create(const std::string& name, const fs::path& customDir,
                  sandbox::StorageMode storage, const std::string& imageSize,
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
        if (storage != sandbox::StorageMode::Shared)
            j["storage"] = sandbox::storage_to_string_(storage);
        if (storage == sandbox::StorageMode::Image)
            j["imageSize"] = imageSize;
        write_config_json_(subosConfig, j);
    }

    // Image mode: create sparse ext4 image
    if (storage == sandbox::StorageMode::Image) {
        auto rc = sandbox::init_image_(dir / "home.img", imageSize);
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
    payload["storage"] = sandbox::storage_to_string_(storage);
    stream.emit(DataEvent{"subos_created", payload.dump()});
    return 0;
}

// Back-compat overload (no storage argument → shared).
export int create(const std::string& name, const fs::path& customDir,
                  EventStream& stream) {
    return create(name, customDir, sandbox::StorageMode::Shared, "50G", stream);
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
                    sandbox::StorageMode storage, const std::string& imageSize,
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
    if (storage != sandbox::StorageMode::Shared)
        subosCfg["storage"] = sandbox::storage_to_string_(storage);
    else
        subosCfg.erase("storage");
    if (storage == sandbox::StorageMode::Image)
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
    payload["storage"] = sandbox::storage_to_string_(storage);
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
    auto storage = sandbox::read_storage_mode_(p.homeDir / "subos" / name);
    if (storage == sandbox::StorageMode::Shared) return;
    // Hint disabled: wording was ambiguous ("use --sandbox to activate"
    // reads as either the verb or the `subos use` command) and it fired
    // on every shell-level entry for image/tmpfs subos, becoming noise
    // once the user already knows the layout. Revisit with a clearer
    // one-time / opt-in form before re-enabling.
    // std::println(stderr,
    //              "[xlings] storage={} is sandbox-only; entering "
    //              "shell-level (use --sandbox to activate)",
    //              sandbox::storage_to_string_(storage));
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
    if (sandbox) return sandbox::enter(name, stream, sandbox_backend, gpu, cmd);
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

    return platform::run_shell(cmd, cmd.empty());
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
            && sandbox::is_mounted_(mountpoint))
        {
            if (sandbox::unmount_image_(mountpoint) != 0) {
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
        sandbox::StorageMode storage = sandbox::StorageMode::Shared;
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
                if (s == "image") storage = sandbox::StorageMode::Image;
                else if (s == "tmpfs") storage = sandbox::StorageMode::Tmpfs;
                else if (s == "shared") storage = sandbox::StorageMode::Shared;
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
