module;

#include <cstdlib>

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

export module xlings.core.xvm.shim;

import std;

import xlings.libs.json;
import xlings.core.config;
import xlings.core.log;
import xlings.platform;
import xlings.core.xvm.types;
import xlings.core.xvm.db;

export namespace xlings::xvm {

// Check if a program name is the xlings binary itself (not a shim target).
//
// xlings is the single canonical entry point. Earlier releases also accepted
// `xim` / `xvm` as multicall aliases, but they have been removed (see
// main.cpp's deprecated-alias path for the migration error). The set is
// closed and intentional: only the literal name `xlings`.
bool is_xlings_binary(std::string_view name) {
    return name == "xlings";
}

// Extract basename from argv[0], stripping path and extension
std::string extract_program_name(const char* argv0) {
    auto p = std::filesystem::path(argv0);
    return p.stem().string();
}

// ─── Owner-anchored dispatch home (0.4.48) ──────────────────────────
//
// Design: .agents/docs/2026-06-04-shim-owner-anchoring-design.md
//
// A shim is an artifact of exactly one home; dispatch resolves against
// that home FIRST ("which shim file PATH hits" is the selector, ambient
// env is not). env XLINGS_HOME stays as a deprecated lower-priority
// fallback ("borrowing") for one transition window; ~/.xlings covers
// orphan shims copied out of a home. `XLINGS_SHIM_ANCHOR=legacy` (or
// `0`/`off`) restores the pre-0.4.48 env-first behavior as release
// insurance.

// Structural home-root signature. Deliberately structural (no JSON
// content sniffing): a real home has all three of `.xlings.json`,
// `bin/xlings[.exe]`, and a `subos/` directory. A subos dir
// (`<home>/subos/current`) has no `subos/` inside it, and a project
// state dir (`<project>/.xlings`) has no `bin/xlings` — so neither can
// false-match. Project shims must anchor to the GLOBAL home (their
// payloads live there); the project state dir is excluded by design.
bool is_home_root(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(dir / ".xlings.json", ec)) return false;
    constexpr std::string_view bin_name =
        (platform::OS_NAME == "windows") ? "xlings.exe" : "xlings";
    if (!fs::exists(dir / "bin" / bin_name, ec)) return false;
    if (!fs::is_directory(dir / "subos", ec)) return false;
    return true;
}

// Find the home that owns the invoked shim by walking parents upward.
// Tries the shim file as invoked first (only when argv[0] carries a
// path — a bare name came from PATH search and is not a location),
// then the running executable path (Windows: the hardlinked shim path;
// Unix: the symlink-resolved real binary inside its home — both land
// inside the owning home). The walk meets the INNERMOST home root
// first, so nested homes resolve to the nearest owner.
std::optional<std::filesystem::path>
resolve_owner_home(const std::filesystem::path& invoked) {
    namespace fs = std::filesystem;

    auto walk_up = [](fs::path p) -> std::optional<fs::path> {
        std::error_code ec;
        auto canon = fs::weakly_canonical(p, ec);
        if (!ec && !canon.empty()) p = canon;
        for (auto dir = p.parent_path(); !dir.empty();) {
            if (is_home_root(dir)) return dir;
            auto parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        return std::nullopt;
    };

    if (invoked.has_parent_path() && !invoked.parent_path().empty()) {
        std::error_code ec;
        auto abs = fs::absolute(invoked, ec);
        if (!ec) {
            if (auto h = walk_up(abs)) return h;
        }
    }

    auto exe = platform::get_executable_path();
    if (!exe.empty()) {
        if (auto h = walk_up(exe)) return h;
    }
    return std::nullopt;
}

// Lightweight probe: does <home>'s version DB register `program` (with
// at least one version)? Reads <home>/.xlings.json directly — no Config
// singleton involvement, so candidate homes can be probed before the
// dispatch home is chosen. A registered-but-broken entry counts as a
// hit: the error is then reported against that home instead of silently
// jumping homes (genuine breakage must not be masked).
bool home_knows_program(const std::filesystem::path& home,
                        const std::string& program) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto cfg = home / ".xlings.json";
    if (!fs::is_regular_file(cfg, ec)) return false;

    // Distinguish "unreadable due to ownership" from "genuinely not
    // registered". A sudo-installed home is root-owned; a later non-root
    // invocation can't read its .xlings.json and would otherwise get a
    // misleading "<program>: not installed" instead of a permissions hint.
    if (std::ifstream probe(cfg, std::ios::binary); !probe.is_open()) {
        log::warn("[xlings] cannot read {} (permission denied)",
                  Config::display_path(cfg));
        log::warn("  this home may be owned by another user — avoid mixing "
                  "sudo and non-sudo xlings on the same home");
        return false;
    }

    try {
        auto content = platform::read_file_to_string(cfg.string());
        auto j = nlohmann::json::parse(content, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return false;
        if (!j.contains("versions") || !j["versions"].is_object()) return false;
        const auto& versions = j["versions"];

        auto has_versions = [](const nlohmann::json& e) {
            return e.is_object() && e.contains("versions")
                && e["versions"].is_object() && !e["versions"].empty();
        };

        if (versions.contains(program) && has_versions(versions[program]))
            return true;

        // The shim file name may differ from the DB key (VInfo::filename).
        for (auto it = versions.begin(); it != versions.end(); ++it) {
            const auto& e = it.value();
            if (!e.is_object()) continue;
            if (!e.contains("filename") || !e["filename"].is_string()) continue;
            auto fn = e["filename"].get<std::string>();
            if (!fn.empty() && fs::path(fn).stem().string() == program
                && has_versions(e)) {
                return true;
            }
        }
    } catch (...) {
        return false;
    }
    return false;
}

// Choose the dispatch home for a shim invocation: owner → env →
// default (§3 of the design doc). Returns the home to inject via
// Config::override_home(), or nullopt to leave Config's legacy
// resolution untouched (legacy mode, or no candidate knows better).
std::optional<std::filesystem::path>
resolve_dispatch_home(const std::string& program, const char* argv0) {
    namespace fs = std::filesystem;

    auto env_or_empty = [](const char* key) -> std::string {
        const char* v = std::getenv(key);
        return v ? std::string(v) : std::string();
    };

    auto mode = env_or_empty("XLINGS_SHIM_ANCHOR");
    if (mode == "0" || mode == "legacy" || mode == "off") {
        log::debug("shim home: anchoring disabled (XLINGS_SHIM_ANCHOR={})", mode);
        return std::nullopt;
    }

    auto owner = resolve_owner_home(fs::path(argv0 ? argv0 : ""));
    auto envHomeStr = env_or_empty("XLINGS_HOME");
    fs::path envHome = envHomeStr.empty() ? fs::path{} : fs::path(envHomeStr);

    auto same = [](const fs::path& a, const fs::path& b) {
        if (a.empty() || b.empty()) return false;
        std::error_code ec;
        auto ca = fs::weakly_canonical(a, ec);
        std::error_code ec2;
        auto cb = fs::weakly_canonical(b, ec2);
        return (ec || ec2) ? (a == b) : (ca == cb);
    };

    // hop 1: owner home (the shim's own home) — the contract.
    if (owner && home_knows_program(*owner, program)) {
        if (!envHome.empty() && !same(*owner, envHome)
            && home_knows_program(envHome, program)) {
            log::warn("shim '{}' owned by {} is also resolvable in XLINGS_HOME={}; "
                      "using the owner home (env-based shim switching is deprecated "
                      "— put that home's subos bin on PATH instead)",
                      program, owner->string(), envHome.string());
        }
        log::debug("shim home: owner={} (hop 1)", owner->string());
        return owner;
    }

    // hop 2: env XLINGS_HOME — DEPRECATED compat fallback ("borrowing").
    if (!envHome.empty() && !same(owner.value_or(fs::path{}), envHome)
        && home_knows_program(envHome, program)) {
        log::warn("shim '{}' is not installed in its owning home{}; falling back "
                  "to XLINGS_HOME={} (deprecated, will be removed in a future "
                  "release)",
                  program,
                  owner ? (" " + owner->string()) : std::string(""),
                  envHome.string());
        log::debug("shim home: env={} (hop 2)", envHome.string());
        return envHome;
    }

    // hop 3: default ~/.xlings — covers orphan shims copied out of a home.
    fs::path defaultHome = fs::path(platform::get_home_dir()) / ".xlings";
    if (!same(owner.value_or(fs::path{}), defaultHome)
        && !same(envHome, defaultHome)
        && home_knows_program(defaultHome, program)) {
        log::debug("shim home: default={} (hop 3)", defaultHome.string());
        return defaultHome;
    }

    // No candidate knows the program: bind to the owner so the error is
    // reported against the home this shim belongs to; otherwise leave
    // Config's legacy resolution (env/default) in place.
    if (owner) {
        log::debug("shim home: owner={} (no candidate knows '{}')",
                   owner->string(), program);
        return owner;
    }
    return std::nullopt;
}

// Resolve the real executable path for a shim target
std::filesystem::path resolve_executable(const std::string& program_name,
                                         const std::string& path,
                                         const std::string& xlings_home) {
    namespace fs = std::filesystem;

    auto expanded = expand_path(path, xlings_home);
    auto base = fs::path(expanded);

    // Try direct: path/program_name, then path/bin/program_name
    auto candidate1 = base / program_name;
    if (fs::exists(candidate1)) return candidate1;
    auto candidate2 = base / "bin" / program_name;
    if (fs::exists(candidate2)) return candidate2;

#if defined(_WIN32)
    // Windows: try .exe / .bat / .cmd (bare name already tried above)
    constexpr std::string_view win_exts[] = {".exe", ".bat", ".cmd"};
    for (auto ext : win_exts) {
        auto p1 = base / (program_name + std::string(ext));
        if (fs::exists(p1)) return p1;
        auto p2 = base / "bin" / (program_name + std::string(ext));
        if (fs::exists(p2)) return p2;
    }
#endif

    return {};
}

// Merge a shim-injected env value into the existing one. PATH-style vars
// prepend (new first), but a value already present — as the whole value or as
// one separator-delimited component — must NOT be appended again: the blind
// re-append corrupted single-value vars (GIT_SSL_CAINFO="x" became "x:x",
// which curl reads as one nonexistent filename and every HTTPS git transport
// dies; hit when both compact::git's CA pin (#378) and the xim git.lua envs
// (xim-pkgindex#406) resolve the same bundle path). Returns the value to set,
// or nullopt for "leave the env untouched".
std::optional<std::string> merge_shim_env_value(const std::string& expanded,
                                                const std::string& existing) {
    if (existing.empty()) return expanded;
    std::size_t start = 0;
    while (start <= existing.size()) {
        auto end = existing.find(platform::PATH_SEPARATOR, start);
        auto part = existing.substr(start, end == std::string::npos
                                           ? std::string::npos : end - start);
        if (part == expanded) return std::nullopt;  // already present
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return expanded + platform::PATH_SEPARATOR + existing;
}

// Set environment variables for a program before exec
void setup_envs(const VData& vdata,
                const std::string& resolved_path,
                const std::string& xlings_home,
                const std::string& active_subos_dir) {
    // Set envs from VData
    for (auto& [key, value] : vdata.envs) {
        // Same hazard as the alias: an env value recorded at install time can
        // name that install's subos. Re-point it at the active one.
        auto expanded = normalize_subos_paths(
            expand_path(value, xlings_home), xlings_home, active_subos_dir);
        auto existing = std::string(std::getenv(key.c_str()) ? std::getenv(key.c_str()) : "");
        if (auto merged = merge_shim_env_value(expanded, existing))
            platform::set_env_variable(key, *merged);
    }

    // Prepend resolved program's directory to PATH
    if (!resolved_path.empty()) {
        auto dir = std::filesystem::path(resolved_path).parent_path().string();
        auto existing_path = std::string(std::getenv("PATH") ? std::getenv("PATH") : "");
        auto bin_dir = Config::paths().binDir.string();
        std::string new_path;
        if (!dir.empty()) new_path = dir;
        if (!bin_dir.empty()) {
            if (!new_path.empty()) new_path += platform::PATH_SEPARATOR;
            new_path += bin_dir;
        }
        if (!existing_path.empty()) {
            if (!new_path.empty()) new_path += platform::PATH_SEPARATOR;
            new_path += existing_path;
        }
        platform::set_env_variable("PATH", new_path);
    }
}

// Main shim dispatch: called when argv[0] is a tool name (not xlings/xim)
int shim_dispatch(const std::string& program_name, int argc, char* argv[]) {
    // Recursion guard: detect infinite shim re-invocation
    constexpr int MAX_SHIM_DEPTH = 8;
    constexpr int FALLBACK_DEPTH = MAX_SHIM_DEPTH + 2;
    auto depth_str = std::getenv("XLINGS_SHIM_DEPTH");
    int depth = depth_str ? std::atoi(depth_str) : 0;
    if (depth >= FALLBACK_DEPTH) {
        log::error("xlings: shim recursion detected for '{}' (depth={})", program_name, depth);
        log::error("  hint: the real '{}' binary may not be installed", program_name);
        return 1;
    }

    log::debug("shim dispatch: program={}, depth={}", program_name, depth);

    auto& cfg = Config::paths();
    auto xlings_home = cfg.homeDir.string();
    // The one resolution point that is correct in all three selection modes
    // (project subos > XLINGS_ACTIVE_SUBOS > the home's activeSubos field).
    auto active_subos_dir = Config::xvm_artifact_subos_dir().string();

    // Get effective workspace (project > subos > global)
    auto workspace = Config::effective_workspace();

    // Look up active version for this program
    auto version = get_active_version(workspace, program_name);
    auto db = Config::versions();
    if (version.empty()) {
        // Tri-state diagnostic, scoped to the CURRENT subos's installed[]
        // (NOT the global versions DB — pre-0.4.19 the "available" list
        // showed every version every subos had ever installed, which was
        // misleading from inside a fresh / pruned subos):
        //
        //   * installed[] empty + global DB empty       → never installed
        //   * installed[] empty + global DB has entries → installed in
        //     another subos, current subos has no view of it (suggest
        //     `xlings install` — payload is shared, but this subos
        //     needs to opt in)
        //   * installed[] non-empty                     → some versions
        //     are opted-in but no active pointer; list THOSE versions
        //     and tell the user to `xlings use` one of them
        const auto& subos_installed = Config::workspace_installed();
        std::vector<std::string> here;
        if (auto it = subos_installed.find(program_name); it != subos_installed.end()) {
            here = it->second;
        }

        if (here.empty()) {
            auto global_all = get_all_versions(db, program_name);
            if (global_all.empty()) {
                log::error("xlings: '{}' is not installed", program_name);
                log::error("  hint: xlings install {}", program_name);
            } else {
                log::error("xlings: '{}' is not installed in current subos", program_name);
                log::error("  hint: xlings install {}", program_name);
            }
        } else {
            log::error("xlings: no active version of '{}' in current subos", program_name);
            std::string avail;
            for (auto& v : here) {
                if (!avail.empty()) avail += " ";
                avail += v;
            }
            log::error("  available: {}", avail);
            log::error("  hint: xlings use {} <version>", program_name);
        }
        return 1;
    }

    // Resolve version (fuzzy match)
    auto resolved_version = match_version(db, program_name, version);
    if (resolved_version.empty()) {
        log::error("xlings: version '{}' not found for '{}'", version, program_name);
        auto all = get_all_versions(db, program_name);
        if (!all.empty()) {
            std::string avail = "  available:";
            for (auto& v : all) avail += " " + v;
            log::error("{}", avail);
        }
        return 1;
    }

    log::debug("resolved version: {} -> {}", version, resolved_version);

    auto vdata = get_vdata(db, program_name, resolved_version);
    if (!vdata) {
        log::error("xlings: no path info for {} {}", program_name, resolved_version);
        return 1;
    }

    // Each target (including binding targets) has its own workspace version
    // and vdata, so use program_name directly for execution.
    std::string exec_name = program_name;

    if (!vdata->alias.empty()) {
        // Env alias fallback: prepend bindir to PATH, run alias via system
        auto expanded_path = expand_path(vdata->path, xlings_home);
        auto bin_path = (std::filesystem::path(expanded_path) / "bin").string();
        auto existing_path = std::string(std::getenv("PATH") ? std::getenv("PATH") : "");
        auto cfg_bin = Config::paths().binDir.string();

        std::string new_path;
        if (!expanded_path.empty() && std::filesystem::exists(expanded_path))
            new_path = expanded_path;
        if (std::filesystem::exists(bin_path)) {
            if (!new_path.empty()) new_path += platform::PATH_SEPARATOR;
            new_path += bin_path;
        }
        if (!cfg_bin.empty()) {
            if (!new_path.empty()) new_path += platform::PATH_SEPARATOR;
            new_path += cfg_bin;
        }
        if (!existing_path.empty()) {
            if (!new_path.empty()) new_path += platform::PATH_SEPARATOR;
            new_path += existing_path;
        }
        platform::set_env_variable("PATH", new_path);

        // Setup custom envs
        setup_envs(*vdata, "", xlings_home, active_subos_dir);

        // The alias may carry the absolute path of whatever subos was active
        // when the package was installed (gcc.lua bakes `--sysroot=<subos>`).
        // Re-point it at the subos THIS process resolves to -- project, env
        // and global selection all land on xvm_artifact_subos_dir().
        //
        // Kept for records written before the flag below existed, and for any
        // subos path an alias carries for some other reason. New records have
        // nothing here to rewrite.
        std::string alias_cmd = normalize_subos_paths(
            vdata->alias[0], xlings_home, active_subos_dir);

        // The entry asked for the active subos's sysroot rather than storing
        // one. This is where that request is answered -- the shim is the only
        // layer that knows which subos this process resolved to, which is
        // exactly why the answer is not in the database. See VData::sysroot.
        if (vdata->sysroot && !active_subos_dir.empty()) {
            alias_cmd += " --sysroot=" + active_subos_dir;
        }

        if (depth >= MAX_SHIM_DEPTH) {
            // Fallback: resolve alias command to full path to break recursion
            auto expanded_pkg = expand_path(vdata->path, xlings_home);
            log::debug("shim alias fallback (depth={}): program={}, alias='{}'", depth, program_name, alias_cmd);
            log::debug("  package path: {}", expanded_pkg);
            log::debug("  PATH: {}", std::getenv("PATH") ? std::getenv("PATH") : "(null)");

            auto first_space = alias_cmd.find(' ');
            std::string alias_prog = (first_space != std::string::npos)
                ? alias_cmd.substr(0, first_space) : alias_cmd;

            auto alias_exe = resolve_executable(alias_prog, vdata->path, xlings_home);
            if (!alias_exe.empty()) {
                std::string alias_rest = (first_space != std::string::npos)
                    ? alias_cmd.substr(first_space) : "";
                alias_cmd = platform::shell_quote(alias_exe.string()) + alias_rest;
                log::debug("  resolved: {} -> {}", alias_prog, alias_exe.string());
            } else if (alias_prog == program_name) {
                log::error("xlings: alias for '{}' references itself but real binary not found", program_name);
                log::error("  path: {}", expanded_pkg);
                return 1;
            }
        }

        platform::set_env_variable("XLINGS_SHIM_DEPTH", std::to_string(depth + 1));

        // Build command: resolved alias + original args, run via platform::exec
        std::string cmd = alias_cmd;
        for (int i = 1; i < argc; ++i) {
            cmd += " ";
            cmd += platform::shell_quote(argv[i]);
        }
        return platform::exec(cmd);
    }

    auto exe_path = resolve_executable(exec_name, vdata->path, xlings_home);
    if (exe_path.empty()) {
        log::error("xlings: executable '{}' not found", exec_name);
        log::error("  path: {}", expand_path(vdata->path, xlings_home));
        log::error("  hint: install with xlings install {}@{}", program_name, resolved_version);
        return 1;
    }

    log::debug("exe path: {}", exe_path.string());

    // Setup environment
    setup_envs(*vdata, exe_path.string(), xlings_home, active_subos_dir);

    // Build argv for execvp
    auto exe_str = exe_path.string();
    std::vector<const char*> new_argv;
    new_argv.push_back(exe_str.c_str());
    for (int i = 1; i < argc; ++i) {
        new_argv.push_back(argv[i]);
    }
    new_argv.push_back(nullptr);

    // Increment shim depth before exec so child processes see it
    platform::set_env_variable("XLINGS_SHIM_DEPTH", std::to_string(depth + 1));

#if defined(__linux__) || defined(__APPLE__)
    execvp(exe_path.c_str(), const_cast<char* const*>(new_argv.data()));
    // If execvp returns, it failed
    log::error("xlings: failed to exec '{}'",
               Config::display_path(exe_path));
    return 1;
#else
    // Fallback for platforms without execvp
    std::string cmd = platform::shell_quote(exe_path.string());
    for (int i = 1; i < argc; ++i) {
        cmd += " ";
        cmd += platform::shell_quote(argv[i]);
    }
    return platform::exec(cmd);
#endif
}

} // namespace xlings::xvm
