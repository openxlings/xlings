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
    return std::format(
        "root:x:0:0:root:/root:/bin/sh\n"
        "{}:x:{}:{}:{}:/home/{}:/bin/sh\n",
        user, uid, gid, user, user);
}
std::string make_etc_group_(const std::string& user, gid_t gid) {
    return std::format(
        "root:x:0:\n"
        "{}:x:{}:\n",
        user, gid);
}
inline constexpr std::string_view kEtcHosts =
    "127.0.0.1 localhost\n::1 localhost\n";
inline constexpr std::string_view kEtcNsswitch =
    "hosts: files dns\npasswd: files\ngroup: files\n";

// Initialize the sandbox-specific dirs / templates inside an existing
// subos. Idempotent: only writes files that don't yet exist, so a
// returning sandbox session won't clobber user customizations and a
// repeated `subos use --sandbox` is cheap.
void init_sandbox_dirs_(const fs::path& subos_dir,
                        const std::string& user,
                        uid_t uid, gid_t gid)
{
    fs::create_directories(subos_dir / "home" / user);
    fs::create_directories(subos_dir / "tmp");
    auto etc = subos_dir / "etc";
    fs::create_directories(etc);

    auto try_write = [&](const fs::path& path, std::string_view body) {
        if (fs::exists(path)) return;
        platform::write_string_to_file(path.string(), std::string(body));
    };
    try_write(etc / "passwd", make_etc_passwd_(user, uid, gid));
    try_write(etc / "group", make_etc_group_(user, gid));
    try_write(etc / "hosts", kEtcHosts);
    try_write(etc / "nsswitch.conf", kEtcNsswitch);
}

#endif // __linux__ / __APPLE__

} // namespace sandbox_detail_

// Create a regular subos. V4: there is only one create path; sandbox
// is a `use`-time modifier (`--sandbox`), not a create-time property.
// The sandbox-private dirs (<subos>/{home/<user>, tmp, etc/...}) are
// laid down lazily on first `subos use --sandbox`, not here.
export int create(const std::string& name, const fs::path& customDir,
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

    auto subosConfig = dir / ".xlings.json";
    if (!fs::exists(subosConfig)) {
        nlohmann::json j;
        j["workspace"] = nlohmann::json::object();
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

    nlohmann::json payload;
    payload["name"] = name;
    payload["dir"]  = dir.string();
    stream.emit(DataEvent{"subos_created", payload.dump()});
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

// Build the proot argv for `subos use --sandbox` entry. V4 layout:
// real user identity (no `-0`), bare chroot (`-r`, no auto-binds), with
// explicit binds chosen for "isolate $HOME / /tmp / /etc; share xlings,
// kernel, POSIX userland from host". See .agents/docs/sandbox-v4-design.md.
std::vector<std::string>
build_proot_argv_(const fs::path& proot_bin,
                  const fs::path& subos_dir,
                  const fs::path& host_xlings_home,
                  const std::string& user,
                  const std::string& shell_path)
{
    // proot CLI uses positional args for the inside-sandbox command;
    // no `--` separator (proot rejects bare `--` with "unknown option").
    auto etc = subos_dir / "etc";
    auto subos_home = subos_dir / "home";
    auto user_home = "/home/" + user;
    std::vector<std::string> argv = {
        proot_bin.string(),
        // -r (bare chroot) instead of -R. -R auto-binds $HOME from host
        // (proot manpage: "host's $HOME → guest's $HOME"); with $HOME =
        // /home/<user> set below, that would shadow our sandbox-private
        // <subos>/home/<user> with the host's real home — full read-write
        // access to host dotfiles. -r leaves us in full control of binds.
        // No `-0` either: V4 preserves real user identity (no fake root).
        "-r", subos_dir.string(),
        // Kernel pseudo-fs (must come from host)
        "--bind=/proc:/proc",
        "--bind=/sys:/sys",
        "--bind=/dev:/dev",
        // DNS + dynamic-linker cache (loader needs /etc/ld.so.cache to
        // resolve libs without scanning every dir)
        "--bind=/etc/resolv.conf:/etc/resolv.conf",
        "--bind=/etc/ld.so.cache:/etc/ld.so.cache",
        // sandbox-owned /etc templates (passwd has the real user entry +
        // root; getpwuid(real_uid) inside the sandbox returns the right
        // home / shell). proot -r doesn't auto-bind these — explicit binds
        // are what surface them.
        std::format("--bind={}:/etc/passwd",       (etc / "passwd").string()),
        std::format("--bind={}:/etc/group",        (etc / "group").string()),
        std::format("--bind={}:/etc/hosts",        (etc / "hosts").string()),
        std::format("--bind={}:/etc/nsswitch.conf",(etc / "nsswitch.conf").string()),
        // Host /usr provides the POSIX userland (mkdir, uname, ls, cat,
        // sh, ...). Without this even basic startup of fish / bash
        // (which spawn `mkdir`, `uname` from their config.* / .bashrc)
        // fails immediately. /lib and /lib64 overlaid onto host's
        // /usr/lib* match the modern usrmerge layout (Ubuntu 22+,
        // Fedora, Arch, recent Debian) so ELF interpreters baked as
        // /lib64/ld-linux* resolve correctly.
        "--bind=/usr:/usr",
        "--bind=/usr/lib:/lib",
        "--bind=/usr/lib64:/lib64",
        // /home: parent bind to sandbox-private dir. This gives dotfile
        // isolation — anything the user writes under their home goes
        // into <subos>/home/<user>/ and doesn't pollute host ~/.
        std::format("--bind={}:/home", subos_home.string()),
        // ~/.xlings: nested bind ON TOP of the parent /home bind. proot
        // resolves the more-specific path first, so /home/<user>/.xlings
        // hits this host-bound path while everything else under
        // /home/<user>/ stays in the sandbox-private dir. Effect:
        //   - sandbox-private:  ~/.config, ~/.cache, ~/.bashrc, ...
        //   - host-shared:      ~/.xlings/data/xpkgs/, .xlings.json, etc.
        // The host-shared path makes `xlings install/list/use` inside
        // the sandbox behave EXACTLY like outside (same xpkg pool, same
        // xvm DB, workspace per-subos as always — sandbox is just a
        // different way to enter the same subos).
        std::format("--bind={}:{}/.xlings",
                    host_xlings_home.string(), user_home),
        std::format("--cwd={}", user_home),
        shell_path,
    };
    return argv;
}

} // namespace sandbox_detail_

// V4 sandbox entry. Reached only via `xlings subos use <name> --sandbox`.
// Linux-only (proot uses ptrace + Linux syscall semantics). On non-Linux
// the dispatcher in use_spawn_shell rejects --sandbox before reaching here.
//
// What it does:
//   1. proot probe (same as V1 — locate_proot_)
//   2. lazy init of <subos>/{home/<user>, tmp, etc/...} via init_sandbox_dirs_
//   3. set env: XLINGS_ACTIVE_SUBOS, XLINGS_SUBOS_MODE=sandbox, HOME=/home/<user>
//   4. build proot argv with V4 binds, execvp
//
// XLINGS_SUBOS_MODE=sandbox is the marker the shell profile checks to
// switch the prompt pill from `[xsubos:<name>]` to `<xsubos:<name>>`.
int use_sandbox_mode_(const std::string& name, EventStream& stream) {
#if !defined(__linux__)
    stream.emit(ErrorEvent{
        .code = ErrorCode::InvalidInput,
        .message = "sandbox mode (`subos use --sandbox`) is only supported "
                   "on Linux (current: " + std::string(platform::OS_NAME) + ")",
        .recoverable = false,
        .hint = "drop --sandbox to use the regular env-spawn shell mode",
    });
    return 1;
#else
    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;

    // Refuse nested sandbox entry. ptrace-based reroot inside a
    // ptrace-based reroot is unstable (proot quirks, exec-veto loops).
    // Detection: XLINGS_SUBOS_MODE=sandbox in env means we're already in
    // one; tell the user to exit first.
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

    auto proot = sandbox_detail_::locate_proot_(p.homeDir);
    if (!proot) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::NotFound,
            .message = std::move(proot).error(),
            .recoverable = false,
            .hint = "see .agents/docs/sandbox-v4-design.md",
        });
        return 1;
    }

    // Resolve the real user identity. We rely on $USER for the username
    // (the standard convention; setpwent / getpwuid is libc-heavy and
    // proot would re-route it anyway). uid / gid come from getuid /
    // getgid — the kernel value, since we're not using `-0`.
    auto user = utils::get_env_or_default("USER");
    if (user.empty()) user = "user";  // defensive; unset $USER is rare
    auto uid = ::getuid();
    auto gid = ::getgid();

    // Lazy init: create sandbox-private dirs and /etc templates if not
    // already there. Idempotent — repeat `subos use --sandbox` is cheap.
    sandbox_detail_::init_sandbox_dirs_(subos_dir, user, uid, gid);

    auto user_home = "/home/" + user;
    auto shell = utils::get_env_or_default("SHELL");
    if (shell.empty()) shell = "/bin/sh";

    // /bin is sandbox-private and only contains the subos's shims, NOT
    // the host's coreutils / shells. The user's $SHELL on a usrmerge
    // distro is typically /bin/bash (a symlink to /usr/bin/bash on
    // host), but proot doesn't follow that — inside the sandbox view
    // /bin/bash resolves to <subos>/bin/bash which doesn't exist.
    //
    // Translate /bin/<x> → /usr/bin/<x> when the latter exists on
    // host (which means it'll exist inside the sandbox too via the
    // /usr RO bind). For all other $SHELL values (full path under
    // /usr/bin, or path under ~/.xlings/data/xpkgs via the nested
    // .xlings bind), use as-is — they already resolve correctly.
    if (shell.starts_with("/bin/")) {
        auto candidate = "/usr/bin/" + shell.substr(5);
        std::error_code ec;
        if (fs::exists(candidate, ec)) shell = std::move(candidate);
    }

    nlohmann::json payload;
    payload["name"] = name;
    payload["mode"] = "sandbox";
    payload["shell"] = shell;
    stream.emit(DataEvent{"subos_entering", payload.dump()});

    // Flush before exec (buffered output is lost across execve(2)).
    std::cout.flush();
    std::cerr.flush();

    // Set sandbox-friendly env before proot exec. The shell sources
    // its profile, profile reads XLINGS_SUBOS_MODE and decorates PS1
    // with `<xsubos:<name>>` instead of the default `[xsubos:<name>]`.
    platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
    platform::set_env_variable("XLINGS_SUBOS_MODE", "sandbox");
    platform::set_env_variable("HOME", user_home);

    auto argv = sandbox_detail_::build_proot_argv_(
        *proot, subos_dir, p.homeDir, user, shell);
    std::vector<char*> c_argv;
    c_argv.reserve(argv.size() + 1);
    for (auto& s : argv) c_argv.push_back(const_cast<char*>(s.c_str()));
    c_argv.push_back(nullptr);

    ::execvp(c_argv[0], c_argv.data());
    log::error("failed to exec proot '{}': {}", proot->string(),
               std::strerror(errno));
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
                    bool sandbox = false)
{
    // V4: --sandbox is a `use`-time modifier. Dispatch to the proot
    // path when set; otherwise the regular env-spawn shell behavior
    // (this function's body) runs unchanged. Old V1.1-V1.3 detection
    // of the `sandbox-shell` field in .xlings.json is GONE — the user
    // explicitly opts into sandbox via the flag now.
    if (sandbox) return use_sandbox_mode_(name, stream);

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
        // Parse: xlings subos new <name>
        // (--sandbox-shell removed in 0.4.23 V4: sandbox is a `use` modifier
        // now, not a creation property — see .agents/docs/sandbox-v4-design.md)
        std::string name;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (!a.empty() && a[0] != '-' && name.empty()) {
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
        return create(name, {}, stream);
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
        return use_spawn_shell(name, stream, sandbox);
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
