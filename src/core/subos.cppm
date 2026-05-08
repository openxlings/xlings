module;

// System headers used by use_spawn_shell only. `import std;` doesn't
// pull these in, and we want execl/errno (POSIX) or CreateProcess
// (Win32) without #include in the named-module purview (which the
// standard forbids for headers that aren't importable units).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <unistd.h>
#endif

export module xlings.core.subos;

import std;

import xlings.core.config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.runtime;
import xlings.core.utils;
import xlings.core.xself;
import xlings.core.xvm.db;
import xlings.core.xim.commands;

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
    fs::create_directory_symlink(targetDir, linkPath, ec);
    if (ec) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Permission,
            .message = "failed to update current symlink: " + ec.message(),
            .recoverable = true,
        });
    }
}

// ─────────────────────────────────────────────────────────────────────
// Sandbox subos helpers (0.4.21+)
//
// A "sandbox" subos is a regular subos plus a `sandbox-shell` field in
// its .xlings.json. At `subos use` time, presence of that field triggers
// the proot-based fs-isolated entry path (see use_sandbox_) instead of
// the env-spawn shell-level path. Sandbox subos design lives in
// docs/plans/2026-05-09-subos-sandbox-design.md.
// ─────────────────────────────────────────────────────────────────────

namespace sandbox_detail_ {

// Minimum /etc/* templates a sandbox needs so:
//   - whoami / id work (passwd has root entry)
//   - /etc/hosts resolves localhost without hitting DNS
//   - getent users / nsswitch behaves
// Total ~80 bytes. xlings writes these on `subos new --sandbox-shell`.
inline constexpr std::string_view kEtcPasswd =
    "root:x:0:0:root:/root:/bin/sh\n";
inline constexpr std::string_view kEtcGroup =
    "root:x:0:\n";
inline constexpr std::string_view kEtcHosts =
    "127.0.0.1 localhost\n::1 localhost\n";
inline constexpr std::string_view kEtcNsswitch =
    "hosts: files dns\npasswd: files\ngroup: files\n";

// Lay down /etc/{passwd,group,hosts,nsswitch.conf} templates inside
// the sandbox subos directory. Files are only written if absent — a
// returning user's customizations survive subos use re-init.
void write_etc_templates_(const fs::path& subos_dir) {
    auto etc = subos_dir / "etc";
    fs::create_directories(etc);

    auto try_write = [&](const fs::path& path, std::string_view body) {
        if (fs::exists(path)) return;
        platform::write_string_to_file(path.string(), std::string(body));
    };

    try_write(etc / "passwd", kEtcPasswd);
    try_write(etc / "group", kEtcGroup);
    try_write(etc / "hosts", kEtcHosts);
    try_write(etc / "nsswitch.conf", kEtcNsswitch);
}

// Initialize sandbox-specific directory layout in addition to the
// regular subos dirs. /root is $HOME inside; /tmp is per-sandbox tmp.
// /etc gets the templates above.
// Forward declaration — definition lives in the second sandbox_detail_
// block alongside locate_proot_ / build_proot_argv_, which it depends on.
// create_impl_ (between the two blocks) calls this for the eager-install
// path on `subos new --sandbox-shell`.
int ensure_shell_installed_and_linked_(const fs::path& sandbox_dir,
                                       const std::string& xpkg_id,
                                       const std::string& shell_inside_path,
                                       EventStream& stream);

void init_sandbox_layout_(const fs::path& subos_dir) {
    fs::create_directories(subos_dir / "root");
    fs::create_directories(subos_dir / "tmp");
    write_etc_templates_(subos_dir);
}

} // namespace sandbox_detail_

// Internal worker — exported variants below dispatch into this with
// or without a sandbox-shell xpkg. Keeping the public `create(name,
// dir, stream)` signature byte-stable for ABI compatibility with
// existing capability layer / external imports of the BMI.
int create_impl_(const std::string& name, const fs::path& customDir,
                 EventStream& stream,
                 std::optional<std::string> sandboxShellXpkg) {
    auto& p = Config::paths();

    // Sandbox mode is Linux-only. Reject early with a clear hint.
    if (sandboxShellXpkg.has_value()) {
#if !defined(__linux__)
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "--sandbox-shell is only supported on Linux (current: "
                       + std::string(platform::OS_NAME) + ")",
            .recoverable = false,
            .hint = "create a regular subos by omitting --sandbox-shell, "
                    "or use the global / shell-level subos modes",
        });
        return 1;
#endif
    }

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

    auto configPath = p.homeDir / ".xlings.json";
    auto json = read_config_json_(configPath);
    if (json.contains("subos") && json["subos"].contains(name)) {
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

    // Sandbox subos: lay down /etc templates + /root + /tmp on top of
    // the regular subos directory layout. The regular dirs above are
    // still created (other code paths assume `bin/`, `lib/` exist).
    if (sandboxShellXpkg.has_value()) {
        sandbox_detail_::init_sandbox_layout_(dir);
    }

    // Create empty .xlings.json with workspace; sandbox-shell field
    // gets written below if applicable.
    auto subosConfig = dir / ".xlings.json";
    if (!fs::exists(subosConfig)) {
        nlohmann::json j;
        j["workspace"] = nlohmann::json::object();
        if (sandboxShellXpkg.has_value()) {
            // sandbox-shell stored as the inside-view path for now.
            // Convention: xlings install <xpkg> drops the binary into
            // bin/<basename>, so /bin/<basename> is its inside path.
            // Bare-name xpkg → /bin/<bare>. ns:name → strip ns prefix.
            auto bare = *sandboxShellXpkg;
            if (auto colon = bare.rfind(':'); colon != std::string::npos)
                bare = bare.substr(colon + 1);
            j["sandbox-shell"] = "/bin/" + bare;
            j["sandbox-shell-xpkg"] = *sandboxShellXpkg;  // remember the xpkg id for lazy install
        }
        write_config_json_(subosConfig, j);
    }

    // Create shim hardlinks from xlings binary
    auto xlingsBin = p.homeDir / "xlings";
    if (!fs::exists(xlingsBin))
        xlingsBin = p.homeDir / "bin" / "xlings";
    if (fs::exists(xlingsBin)) {
        xself::ensure_subos_shims(dir / "bin", xlingsBin, p.homeDir);
    }

    if (!json.contains("subos")) json["subos"] = nlohmann::json::object();
    json["subos"][name] = {{"dir", customDir.empty() ? "" : customDir.string()}};
    write_config_json_(configPath, json);

    // Sandbox: install the configured shell xpkg and symlink its binary
    // into <sandbox>/bin/<basename> so `subos use <name>` can exec it.
    // Done at create time so the user's mental model holds:
    // "I asked for a sandbox with this shell — it's ready."
    if (sandboxShellXpkg.has_value()) {
#if defined(__linux__)
        auto bare = *sandboxShellXpkg;
        if (auto colon = bare.rfind(':'); colon != std::string::npos)
            bare = bare.substr(colon + 1);
        auto rc = sandbox_detail_::ensure_shell_installed_and_linked_(
            dir, *sandboxShellXpkg, "/bin/" + bare, stream);
        if (rc != 0) return rc;
#endif
    }

    nlohmann::json payload;
    payload["name"] = name;
    payload["dir"]  = dir.string();
    stream.emit(DataEvent{"subos_created", payload.dump()});
    return 0;
}

// Public exports.
//
// `create` keeps its original 3-arg signature byte-stable so existing
// callers (capabilities.cppm, downstream BMI consumers) link against
// the same symbol. `create_sandbox` is a separate symbol for the
// sandbox flow — splitting avoids cross-TU BMI / ABI churn that
// inflicting an extra default param on `create` would cause under
// gcc 15.1.0 musl-cross (observed as spurious link errors in CI).
export int create(const std::string& name, const fs::path& customDir,
                  EventStream& stream) {
    return create_impl_(name, customDir, stream, std::nullopt);
}

export int create_sandbox(const std::string& name, const fs::path& customDir,
                          const std::string& sandboxShellXpkg,
                          EventStream& stream) {
    return create_impl_(name, customDir, stream, sandboxShellXpkg);
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
    auto configPath = p.homeDir / ".xlings.json";
    auto json = read_config_json_(configPath);
    json["activeSubos"] = name;
    write_config_json_(configPath, json);

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
int use_emit_shell(const std::string& name,
                          std::string_view shell_kind,
                          EventStream& stream) {
    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;

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

// Build the proot argv for entering a sandbox subos. Output layout
// matches the design doc §3.3.
std::vector<std::string>
build_proot_argv_(const fs::path& proot_bin,
                  const fs::path& subos_dir,
                  const fs::path& home_dir,
                  const std::string& subos_name,
                  const std::string& shell_path)
{
    // proot's CLI uses positional args for the inside-sandbox command;
    // no `--` separator (passing `--` triggers `unknown option '--'`).
    //
    // Note on /etc bindings: proot 5.x has built-in "kompat" auto-binds
    // for /etc/{passwd,group,hosts,host.conf,localtime,mtab,networks,
    // nsswitch.conf,resolv.conf} that surface host versions inside the
    // guest rootfs. For sandbox semantics we want our own templates
    // (which xlings wrote at subos creation time) to take precedence —
    // explicit per-file --bind=<sandbox-etc-file>:<inside-path> overrides
    // the auto-bind. /etc/resolv.conf stays bound to host so DNS works.
    auto etc = subos_dir / "etc";
    std::vector<std::string> argv = {
        proot_bin.string(),
        "-0",                                            // fake root (uid 0 from getuid)
        "-R", subos_dir.string(),                        // rootfs
        "--bind=/proc:/proc",
        "--bind=/sys:/sys",
        "--bind=/dev:/dev",
        "--bind=/etc/resolv.conf:/etc/resolv.conf",      // DNS from host
        // sandbox-owned /etc files (override proot kompat auto-binds)
        std::format("--bind={}:/etc/passwd",       (etc / "passwd").string()),
        std::format("--bind={}:/etc/group",        (etc / "group").string()),
        std::format("--bind={}:/etc/hosts",        (etc / "hosts").string()),
        std::format("--bind={}:/etc/nsswitch.conf",(etc / "nsswitch.conf").string()),
        std::format("--bind={}:/xlings", home_dir.string()),
        // Self-bind xlings home at its host absolute path. xim-installed
        // binaries are elfpatched with the absolute host path of their
        // loader / rpath libs (e.g. ld-linux from xim:d2x). Without this
        // bind, those paths route to <rootfs>/<host-path> which is empty,
        // and the shell fails to start with "no such file or directory".
        // The /xlings bind above is for user-facing convenience; this
        // self-bind is what makes elfpatched binaries actually run.
        std::format("--bind={}:{}", home_dir.string(), home_dir.string()),
        "--cwd=/root",
        shell_path,
    };
    return argv;
}

// Ensure the configured sandbox shell binary is installed and present at
// `<sandbox>/bin/<basename>` as a symlink to the xim-managed payload.
// Idempotent — safe to re-run from `subos new --sandbox-shell` (eager
// hydration) and from `subos use <sandbox>` (lazy self-heal when the
// payload was removed or the symlink dangles).
//
// Strategy:
//   1. If the destination exists and resolves to a real file, no-op.
//   2. Otherwise, ensure the xpkg is installed (call xim::cmd_install
//      with yes=true). The install registers a bindir in the xvm DB.
//   3. Resolve <bindir>/<basename> from the xvm DB and symlink it into
//      the sandbox's bin/.
//
// Returns 0 on success. On failure emits an ErrorEvent and returns 1.
int ensure_shell_installed_and_linked_(const fs::path& sandbox_dir,
                                       const std::string& xpkg_id,
                                       const std::string& shell_inside_path,
                                       EventStream& stream)
{
    namespace fs = std::filesystem;

    // bare name = xpkg id with namespace prefix stripped (xim:fish → fish)
    std::string bare = xpkg_id;
    if (auto colon = bare.rfind(':'); colon != std::string::npos)
        bare = bare.substr(colon + 1);

    // shell_inside_path looks like "/bin/fish"; strip leading '/' to get
    // the host-side path under <sandbox_dir>.
    auto rel = shell_inside_path;
    if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
    auto shell_dst = sandbox_dir / rel;
    auto basename  = fs::path(shell_inside_path).filename().string();

    std::error_code ec;

    // Already there and resolves to a real file → done.
    if (fs::exists(shell_dst, ec) && !ec) return 0;
    ec.clear();

    // Dangling symlink (target gone) → remove so create_symlink can run.
    if (fs::is_symlink(shell_dst, ec)) {
        fs::remove(shell_dst, ec);
        ec.clear();
    }

    // Look up the xpkg in the xvm DB. If no version is registered yet,
    // run a full install pass — xim::cmd_install handles download +
    // extract + install hook + xvm registration.
    //
    // Important: Config::versions() returns the merged DB BY VALUE, so
    // the returned object is a temporary. We MUST bind it to a local
    // variable before pulling pointers out of it (`get_vdata` returns
    // a pointer into the DB). Calling Config::versions() inline as a
    // function argument creates a temporary that dies at the end of
    // the expression, leaving `vd` dangling — observed as a corrupted
    // path string and a spurious "shell binary not found" error.
    auto db = Config::versions();
    auto resolved = xvm::match_version(db, bare, "");
    if (resolved.empty()) {
        std::vector<std::string> targets = { xpkg_id };
        auto rc = xim::cmd_install(targets, /*yes=*/true,
                                   /*noDeps=*/false, stream);
        if (rc != 0) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = std::format(
                    "failed to install sandbox shell xpkg '{}'", xpkg_id),
                .recoverable = true,
                .hint = std::format(
                    "try manually: xlings install {} (then re-run subos use)",
                    xpkg_id),
            });
            return 1;
        }
        db = Config::versions();
        resolved = xvm::match_version(db, bare, "");
        if (resolved.empty()) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = std::format(
                    "xpkg '{}' installed but no version in xvm DB "
                    "(unexpected — package may not register a binary)",
                    xpkg_id),
                .recoverable = false,
            });
            return 1;
        }
    }

    auto* vd = xvm::get_vdata(db, bare, resolved);
    if (!vd || vd->path.empty()) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::NotFound,
            .message = std::format(
                "xpkg '{}' has no bindir registered in xvm DB", xpkg_id),
            .recoverable = false,
        });
        return 1;
    }

    auto bindir = fs::path(xvm::expand_path(
        vd->path, Config::paths().homeDir.string()));
    auto shell_src = bindir / basename;
    if (!fs::exists(shell_src, ec)) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::NotFound,
            .message = std::format(
                "shell binary '{}' not found in xpkg payload (looked at {})",
                basename, shell_src.string()),
            .recoverable = false,
            .hint = std::format(
                "the xpkg '{}' may not provide a binary called '{}'",
                xpkg_id, basename),
        });
        return 1;
    }

    fs::create_directories(shell_dst.parent_path(), ec);
    ec.clear();
    fs::create_symlink(shell_src, shell_dst, ec);
    if (ec) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = std::format(
                "failed to symlink {} -> {}: {}",
                shell_dst.string(), shell_src.string(), ec.message()),
            .recoverable = false,
        });
        return 1;
    }
    log::debug("[sandbox] hydrated shell {} -> {}",
               shell_dst.string(), shell_src.string());
    return 0;
}

} // namespace sandbox_detail_

// Internal — not exported. `xlings subos use <name>` reaches here when
// the subos's .xlings.json carries a `sandbox-shell` field.
int use_sandbox_(const std::string& name, EventStream& stream) {
    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;

    // Refuse nested sandbox entry. xlings already running inside a sandbox
    // (XLINGS_ACTIVE_SUBOS set + we're seeing a sandbox config marker) —
    // ptrace-based reroot inside a ptrace-based reroot is asking for
    // trouble (proot quirks, exec-veto loops). Tell the user to exit first.
    if (!utils::get_env_or_default("XLINGS_ACTIVE_SUBOS").empty()
        && fs::exists("/.xlings.json"))
    {
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

    // Read sandbox-shell path from this subos's config.
    auto subos_config_path = subos_dir / ".xlings.json";
    auto subos_config = read_config_json_(subos_config_path);
    std::string shell_path;
    if (subos_config.contains("sandbox-shell")
        && subos_config["sandbox-shell"].is_string())
    {
        shell_path = subos_config["sandbox-shell"].get<std::string>();
    }
    if (shell_path.empty()) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "subos '" + name + "' has no sandbox-shell field",
            .recoverable = false,
        });
        return 1;
    }

    // Probe proot.
    auto proot = sandbox_detail_::locate_proot_(p.homeDir);
    if (!proot) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::NotFound,
            .message = std::move(proot).error(),
            .recoverable = false,
            .hint = "see docs/plans/2026-05-09-subos-sandbox-design.md",
        });
        return 1;
    }

    // Self-heal: shell binary may be missing because the user removed
    // the underlying xpkg (`xlings remove ...`) after creating the
    // sandbox, leaving a dangling symlink. Re-run the hydrate path,
    // which is idempotent and reinstalls the xpkg if needed. Eager
    // install at `subos new` is the primary path; this is the
    // recovery path so users never get stuck in chicken-and-egg
    // (can't `subos use` to install, can't install without entering).
    auto host_shell_path = subos_dir / shell_path.substr(
        shell_path.front() == '/' ? 1 : 0);  // strip leading "/"
    if (!fs::exists(host_shell_path)) {
        std::string xpkg_id;
        if (subos_config.contains("sandbox-shell-xpkg")
            && subos_config["sandbox-shell-xpkg"].is_string())
        {
            xpkg_id = subos_config["sandbox-shell-xpkg"].get<std::string>();
        }
        if (xpkg_id.empty()) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = std::format(
                    "sandbox shell '{}' not found in subos '{}' (expected at {})",
                    shell_path, name, host_shell_path.string()),
                .recoverable = false,
                .hint = std::format(
                    "no sandbox-shell-xpkg recorded — recreate the subos: "
                    "xlings subos remove {} && xlings subos new {} --sandbox-shell <xpkg>",
                    name, name),
            });
            return 1;
        }
        log::info("[sandbox] shell missing — rehydrating from xpkg '{}'",
                  xpkg_id);
        if (auto rc = sandbox_detail_::ensure_shell_installed_and_linked_(
                subos_dir, xpkg_id, shell_path, stream); rc != 0)
        {
            return rc;
        }
    }

    nlohmann::json payload;
    payload["name"] = name;
    payload["mode"] = "sandbox";
    payload["shell"] = shell_path;
    stream.emit(DataEvent{"subos_entering", payload.dump()});

    // Flush before exec/spawn (same reasoning as use_spawn_shell).
    std::cout.flush();
    std::cerr.flush();

#if defined(__linux__)
    // Set sandbox-friendly env before proot exec.
    platform::set_env_variable("XLINGS_HOME", "/xlings");
    platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
    platform::set_env_variable("HOME", "/root");
    platform::set_env_variable("USER", "root");
    platform::set_env_variable(
        "PATH", "/bin:/xlings/bin:/usr/bin:/usr/local/bin");

    // Build argv and exec proot.
    auto argv = sandbox_detail_::build_proot_argv_(
        *proot, subos_dir, p.homeDir, name, shell_path);
    std::vector<char*> c_argv;
    c_argv.reserve(argv.size() + 1);
    for (auto& s : argv) c_argv.push_back(const_cast<char*>(s.c_str()));
    c_argv.push_back(nullptr);

    ::execvp(c_argv[0], c_argv.data());
    log::error("failed to exec proot '{}': {}", proot->string(),
               std::strerror(errno));
    return 127;
#else
    stream.emit(ErrorEvent{
        .code = ErrorCode::InvalidInput,
        .message = "sandbox subos is only supported on Linux",
        .recoverable = false,
    });
    return 1;
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
int use_spawn_shell(const std::string& name, EventStream& stream) {
    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;

    // Sandbox dispatch: if this subos has a `sandbox-shell` field in its
    // .xlings.json, route to the proot-based entry instead of the
    // env-spawn shell. Detection is cheap (one JSON file read), so we do
    // it on every `subos use` rather than a separate command surface —
    // the user types `xlings subos use foo` regardless of subos type.
    {
        auto& p = Config::paths();
        auto subos_cfg = read_config_json_(
            p.homeDir / "subos" / name / ".xlings.json");
        if (subos_cfg.contains("sandbox-shell")
            && subos_cfg["sandbox-shell"].is_string()
            && !subos_cfg["sandbox-shell"].get<std::string>().empty())
        {
            return use_sandbox_(name, stream);
        }
    }

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
        std::string cmdline = exe;
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
    auto shell = utils::get_env_or_default("SHELL");
    if (shell.empty()) shell = "/bin/sh";
    ::execl(shell.c_str(), shell.c_str(), "-i", static_cast<char*>(nullptr));

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

    auto configPath = p.homeDir / ".xlings.json";
    auto json = read_config_json_(configPath);

    if (!json.contains("subos") || !json["subos"].contains(name)) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::NotFound,
            .message = "subos '" + name + "' not found",
            .recoverable = true,
        });
        return 1;
    }

    auto dir = Config::subos_dir(name);
    if (fs::exists(dir)) {
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

    json["subos"].erase(name);
    write_config_json_(configPath, json);

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
            .hint = "usage: xlings subos <new|use|list|ls|remove|rm|info|i> [name]",
        });
    };

    if (sub == "new") {
        if (argc < 4) { usageError("missing <name> for: xlings subos new"); return 1; }
        // Parse: xlings subos new <name> [--sandbox-shell <xpkg>]
        std::string name;
        std::optional<std::string> sandbox_shell;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--sandbox-shell") {
                if (i + 1 >= argc) {
                    usageError("--sandbox-shell needs an xpkg argument");
                    return 1;
                }
                sandbox_shell = argv[++i];
            }
            else if (a.rfind("--sandbox-shell=", 0) == 0) {
                sandbox_shell = a.substr(16);
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
        if (sandbox_shell) return create_sandbox(name, {}, *sandbox_shell, stream);
        return create(name, {}, stream);
    }
    if (sub == "use") {
        // Flags supported:
        //   --global         persist the choice into ~/.xlings.json + symlink
        //                    (legacy behavior; affects every shell)
        //   --shell <kind>   emit shell code on stdout for the user to
        //                    eval/Invoke-Expression. <kind> ∈ {sh,bash,zsh,
        //                    fish,pwsh}. Defaults to "sh" if not provided.
        //   (no flag)        spawn a fresh interactive shell with
        //                    XLINGS_ACTIVE_SUBOS=<name> in env. Per-shell.
        std::string name;
        std::string mode = "spawn";        // default
        std::string shell_kind = "sh";
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
            else if (!a.empty() && a[0] != '-' && name.empty()) {
                name = std::move(a);
            }
            else {
                usageError("unknown option for `xlings subos use`: " + a);
                return 1;
            }
        }
        if (name.empty()) { usageError("missing <name> for: xlings subos use"); return 1; }

        if (mode == "global") return use_global(name, stream);
        if (mode == "shell")  return use_emit_shell(name, shell_kind, stream);
        return use_spawn_shell(name, stream);
    }
    if (sub == "list")   return run_list_(stream);
    if (sub == "remove") {
        if (argc < 4) { usageError("missing <name> for: xlings subos remove|rm"); return 1; }
        return remove(argv[3], stream);
    }
    if (sub == "info")   return run_info_(argc > 3 ? argv[3] : "", stream);

    usageError("unknown subcommand: " + sub);
    return 1;
}

} // namespace xlings::subos
