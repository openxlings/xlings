module;

#include <cstdlib>

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

module xlings.core.xvm.shim;

import std;
import xlings.libs.json;
import xlings.core.config;
import xlings.core.log;
import xlings.core.diag;
import xlings.platform;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.errors;
import xlings.core.version_order;
// For `project_contributions()` -- the one answer to "which project
// declares this command". Implementation-unit import only; the xvm
// interface stays free of xself.
import xlings.core.xself.init;
import xlings.core.xvm.shim_table;

namespace xlings::xvm {

bool is_xlings_binary(std::string_view name) {
    return name == "xlings";
}

std::string extract_program_name(const char* argv0) {
    auto p = std::filesystem::path(argv0);
    return p.stem().string();
}

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
        log::warn("cannot read {} (permission denied)",
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

AliasResolution resolve_alias_program(const std::string& program_name,
                                      const std::string& path,
                                      const std::string& xlings_home,
                                      const std::filesystem::path& subos_bin,
                                      std::string_view searchPath) {
    namespace fs = std::filesystem;

    if (program_name.empty()) return {};

    // An absolute alias is not searched for; it either exists or it does not.
    // It is NOT a host command -- a recipe that pins a full path into its own
    // payload store writes one, and calling that "satisfied by the host" gets
    // the portability question exactly backwards.
    if (fs::path(program_name).is_absolute()) {
        std::error_code ec;
        if (fs::exists(program_name, ec)) {
            return {AliasOrigin::Absolute, fs::path(program_name)};
        }
        return {};
    }

    if (auto own = resolve_executable(program_name, path, xlings_home);
        !own.empty()) {
        return {AliasOrigin::Payload, std::move(own)};
    }

#if defined(_WIN32)
    constexpr std::string_view exts[] = {"", ".exe", ".bat", ".cmd"};
#else
    constexpr std::string_view exts[] = {""};
#endif

    const auto probe = [&](const fs::path& dir) -> fs::path {
        if (dir.empty()) return {};
        for (auto ext : exts) {
            auto candidate = dir / (program_name + std::string(ext));
            std::error_code ec;
            if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec)) {
                return candidate;
            }
        }
        return {};
    };

    if (auto sibling = probe(subos_bin); !sibling.empty()) {
        return {AliasOrigin::SubosBin, std::move(sibling)};
    }

    std::size_t start = 0;
    while (start <= searchPath.size() && !searchPath.empty()) {
        auto end = searchPath.find(platform::PATH_SEPARATOR, start);
        auto part = searchPath.substr(
            start, end == std::string_view::npos ? std::string_view::npos
                                                 : end - start);
        if (!part.empty()) {
            if (auto hit = probe(fs::path(part)); !hit.empty()) {
                return {AliasOrigin::SystemPath, std::move(hit)};
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }

    return {};
}

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

std::optional<std::string> setup_envs(const VData& vdata,
                const std::string& resolved_path,
                const std::string& xlings_home,
                const std::string& active_subos_dir) {
    // The subos library farm, declared by the layer that resolved WHICH subos
    // this process runs in.
    //
    // The same contract the shell profile, `subos spawn` and `--sandbox` each
    // declare (E2a, 2026.8.11.1) -- but those three cover entry into a subos,
    // and a shim is the fourth way a program starts: `gcc` invoked from a
    // plain shell that never sourced the profile, or from a build system that
    // sanitises the environment. The `ld` wrapper in the binutils payload
    // reads this variable, so a path where nobody sets it is a path where the
    // wrapper silently does nothing -- which is indistinguishable from it
    // working.
    //
    // Set unconditionally rather than only-if-absent: this layer resolved the
    // ACTIVE subos for this process (project > env > home), and the profile's
    // value is the home's default, which is the wrong one inside a project
    // subos. The most specific answerer wins.
    if (!active_subos_dir.empty()) {
        platform::set_env_variable(
            "XLINGS_SUBOS_LIB",
            (std::filesystem::path(active_subos_dir) / "lib").string());
    }

    // Set envs from VData
    for (auto& [key, value] : vdata.envs) {
        // Same hazard as the alias: an env value recorded at install time can
        // name that install's subos. Re-point it at the active one.
        auto expanded = expand_subos_placeholder(
            normalize_subos_paths(expand_path(value, xlings_home),
                                  xlings_home, active_subos_dir),
            active_subos_dir);
        if (auto vanishing = vanishing_xlings_reference(expanded)) {
            return vanishing;
        }
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
    return std::nullopt;
}

void report_vanishing_reference_(const std::string& program_name,
                                 const std::string& name) {
    if (name == "XLINGS_DYNAMIC_SUBOS_DIR") {
        log::error("xlings: '{}' needs the active subos, and none resolved",
                   program_name);
        log::error("  hint: xlings subos list   (then `xlings subos use <name>`)");
        return;
    }
    log::error("xlings: '{}' is registered against '{}', which this client "
               "does not provide", program_name, name);
    log::error("  this record was written by a newer index than the xlings "
               "running it");
    log::error("  hint: xlings self update   (entry: {})",
               std::string(Info::VERSION));
}

// ─── Host passthrough ───────────────────────────────────────────────
//
// A shim exists in a bin directory because some scope needs the NAME on PATH.
// When the scope actually being resolved makes no claim on that name, the file
// must not change anything: it hands the name back to PATH.
//
// Every comparable tool does this and every one of them excludes its own
// directories while doing it -- proto skips its shims/bin dirs and any
// directory holding a `registry.json`, aqua skips anything resolving to
// `aqua-proxy`, pyenv strips its shims dir from PATH first. rustup does not,
// and pays for it by re-exec'ing itself about twenty times before a counter
// stops it.

// Would this candidate just send us back into xlings?
bool is_own_shim_(const std::filesystem::path& candidate,
                  const std::filesystem::path& entryBinary,
                  const std::filesystem::path& home) {
    std::error_code ec;
    if (std::filesystem::equivalent(candidate, entryBinary, ec) && !ec) {
        return true;
    }
    // Path containment as well as file identity. A shim in ANOTHER home is a
    // different file, so `equivalent` says no -- but exec'ing it would land
    // us in that home's dispatch, one hop from the same question. The depth
    // guard would eventually stop it; not starting is better.
    ec.clear();
    auto canonCandidate = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) return false;
    std::error_code hec;
    auto canonHome = std::filesystem::weakly_canonical(home, hec);
    if (hec || canonHome.empty()) return false;
    auto c = canonCandidate.string();
    auto h = canonHome.string();
    return c.starts_with(h + "/") || c.starts_with(h + "\\");
}

// Was this shim reached by PATH, or named directly?
//
// The distinction decides whether passing through is honest. A shim found by
// PATH under its bare name is a ROUTING entry: the user typed a name, PATH
// happened to hit xlings's file, and if this scope has no opinion the right
// answer is whatever would have run without that file. A shim invoked by an
// explicit PATH -- `<project>/.xlings/subos/_/bin/node` -- is not that. The
// caller pointed at one installation; running a different program instead
// would be substituting for the exact thing they named.
//
// argv[0] without a path component came from a PATH search and is not a
// location (the same reading `resolve_owner_home` uses), so it is routing.
// With a path, it is routing only if the file lives in this home -- a project
// subos bin is never on PATH, so a shim there was always named deliberately.
bool invoked_as_routing_entry_(const char* argv0,
                               const std::filesystem::path& home) {
    if (argv0 == nullptr || *argv0 == '\0') return true;
    std::filesystem::path invoked(argv0);
    if (!invoked.has_parent_path() || invoked.parent_path().empty()) {
        return true;                      // bare name: came from PATH
    }
    // The DIRECTORY, never the file.
    //
    // A shim IS a symlink to the entry binary, and `weakly_canonical` follows
    // it -- so canonicalising the invoked path turns
    // `<project>/.xlings/subos/_/bin/node` into `<home>/bin/xlings` and every
    // shim on the machine looks like it lives in the home. Resolving the
    // parent directory is both correct and what the question actually is:
    // which bin directory was this reached through.
    std::error_code ec;
    auto canonDir = std::filesystem::weakly_canonical(invoked.parent_path(), ec);
    if (ec) return false;                 // cannot tell: do not substitute
    std::error_code hec;
    auto canonHome = std::filesystem::weakly_canonical(home, hec);
    if (hec || canonHome.empty()) return false;
    auto i = canonDir.string();
    auto h = canonHome.string();
    return i.starts_with(h + "/") || i.starts_with(h + "\\");
}

// The first executable on PATH with this name that is not one of ours.
std::filesystem::path find_host_passthrough_(const std::string& program_name,
                                             const std::string& xlings_home) {
    const char* rawPath = std::getenv("PATH");
    if (rawPath == nullptr || *rawPath == '\0') return {};

    std::filesystem::path home(xlings_home);
    auto entry = home / "bin" / (platform::OS_NAME == "windows"
                                     ? "xlings.exe" : "xlings");

#if defined(_WIN32)
    constexpr std::string_view exts[] = {"", ".exe", ".bat", ".cmd"};
#else
    constexpr std::string_view exts[] = {""};
#endif

    std::string_view search(rawPath);
    std::size_t start = 0;
    while (start <= search.size()) {
        auto end = search.find(platform::PATH_SEPARATOR, start);
        auto part = search.substr(
            start, end == std::string_view::npos ? std::string_view::npos
                                                 : end - start);
        if (!part.empty()) {
            std::filesystem::path dir{std::string(part)};
            for (auto ext : exts) {
                auto candidate = dir / (program_name + std::string(ext));
                std::error_code ec;
                if (!std::filesystem::exists(candidate, ec) || ec) continue;
                if (std::filesystem::is_directory(candidate, ec)) continue;
                if (is_own_shim_(candidate, entry, home)) continue;
                return candidate;
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return {};
}

// Say so -- but only where a person is watching.
//
// Interactively this is the difference between "my project setup is fine" and
// "I am in the wrong directory". In a pipeline, a Makefile or CI it is noise
// on the one path that is supposed to look exactly like plain PATH resolution,
// so it stays quiet there.
void report_passthrough_(const std::string& program_name,
                         const std::filesystem::path& host) {
    if (!platform::stderr_is_terminal()) return;
    // `warn`, and NOT `info`, for a reason that has nothing to do with
    // severity: `log::info` writes to STDOUT (log.cppm), so it would land in
    // the piped output of the command being passed through --
    // `node --version | grep` would read this line as node's answer. `warn`
    // writes to stderr, which is the stream the guard above tests.
    log::warn("{}: no version in subos '{}'; running {}",
              program_name, Config::subos_scope().name, host.string());
}

int exec_host_program_(const std::filesystem::path& host,
                       int argc, char* argv[]) {
    auto hostStr = host.string();
    std::vector<const char*> newArgv;
    newArgv.push_back(hostStr.c_str());
    for (int i = 1; i < argc; ++i) newArgv.push_back(argv[i]);
    newArgv.push_back(nullptr);

#if defined(__linux__) || defined(__APPLE__)
    execvp(hostStr.c_str(), const_cast<char* const*>(newArgv.data()));
    log::error("xlings: failed to exec '{}'", hostStr);
    return 127;
#else
    std::string cmd = platform::shell_quote(hostStr);
    for (int i = 1; i < argc; ++i) {
        cmd += " ";
        cmd += platform::shell_quote(argv[i]);
    }
    return platform::exec(cmd);
#endif
}

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
            // Nothing in this scope claims this name -- not active, not even
            // opted into `installed[]`. So the file on PATH is here for some
            // OTHER scope's sake: a project declared it, and a project's own
            // bin is never on PATH, so its command names have to live in the
            // directory that is.
            //
            // That file must not change what happens outside the project.
            // Hand the name back to PATH: run whatever would have run if
            // xlings had never put a file there.
            //
            // ONLY in this branch. The other two -- `installed[]` has the
            // name but nothing is active, or a version is pinned and not
            // installed -- are CLAIMS this scope makes and cannot satisfy,
            // and running the host's copy instead would be the silent
            // substitution pyenv still ships (a pinned-but-missing version
            // whose command exists on the system runs the system one and
            // prints nothing). A claim we cannot meet is an error.
            //
            // And only for a shim PATH found, not one the caller named by
            // path -- see invoked_as_routing_entry_.
            if (invoked_as_routing_entry_(argc > 0 ? argv[0] : nullptr,
                                          cfg.homeDir)) {
              if (auto host = find_host_passthrough_(program_name, xlings_home);
                  !host.empty()) {
                report_passthrough_(program_name, host);
                platform::set_env_variable("XLINGS_SHIM_DEPTH",
                                           std::to_string(depth + 1));
                return exec_host_program_(host, argc, argv);
              }
            }

            // Same state, same wording, same code as `xlings use` reports --
            // the shim used to say "xlings: '{}' is not installed in current
            // subos" while `use` said "in this subos ({})", so the two halves
            // of one product described one condition differently.
            // Which project, if any, is the reason this name is on PATH at
            // all. Without it the remedy was `xlings install <name>` for a
            // package the user never asked for -- and one that need not even
            // be a valid package name.
            std::vector<std::string> providers;
            for (const auto& contribution : xself::project_contributions()) {
                if (!contribution.readable) continue;
                if (std::ranges::find(contribution.commands,
                                      xvm::shim_filename(program_name))
                    == contribution.commands.end()) {
                    continue;
                }
                providers.push_back(contribution.root.string());
            }

            const auto origin = Config::version_origin(program_name);
            diag::emit(not_in_subos({
                .target             = program_name,
                .subos              = Config::subos_scope().name,
                .versionsElsewhere  = get_all_versions(db, program_name),
                .providedByProjects = std::move(providers),
                .source             = origin.source,
                .fromProject        = origin.fromProjectManifest,
            }));
        } else {
            // Genuinely different state: opted into this subos, but nothing is
            // pointed at. Different code, because the way out is different.
            diag::Diagnostic d {
                .code    = "xvm.no_active_version",
                .summary = std::format(
                    "{} is installed in this subos, but no version is active",
                    program_name),
                .facts   = { diag::candidates("installed here", here, 6) },
            };
            auto newest = here;
            version_order::sort_desc(newest);
            d.actions.push_back({ "pick one",
                std::format("xlings use {} {}", program_name, newest.front()) });
            diag::emit(d);
        }
        return 1;
    }

    // Resolve version (fuzzy match)
    auto resolved_version = match_version(db, program_name, version);
    if (resolved_version.empty()) {
        // The user did not type this version. Something pinned it -- most
        // often a project `.xlings.json` -- and before `version_source` there
        // was no way for them to find out which file, because this layer only
        // ever saw the merged answer.
        //
        // The old form printed the bare failure plus every version in
        // std::map order and no way out at all: measured on a real home, one
        // 877-character line of 94 versions with `0.0.100` ahead of `0.0.24`.
        //
        // WARN when the project manifest is what asked, error otherwise.
        //
        // The command did fail either way -- nothing ran, and the exit code
        // below stays non-zero so a script still sees that. But a project that
        // simply has not been set up yet is one documented command away from
        // working, and xlings knows which command; red bold "[error]" for
        // "your project needs installing" reads as a fault in the tool.
        //
        // The gate is which LAYER pinned it, not whether a project config
        // exists -- a project can sit in a directory whose pin for THIS
        // program came from the global subos file, and there `xlings install`
        // would install the project's other packages and exit 0 having left
        // this exactly as broken as it was.
        const auto origin = Config::version_origin(program_name);
        diag::Diagnostic d {
            .level   = origin.fromProjectManifest ? diag::Level::Warn
                                                  : diag::Level::Error,
            .code    = "xvm.pinned_version_missing",
            .summary = origin.fromProjectManifest
                ? std::format("{}@{} is the version this project asks for, "
                              "and it is not installed yet",
                              program_name, version)
                : std::format("{}@{} is selected here, and no such version "
                              "is installed", program_name, version),
            .source  = origin.source,
        };
        auto all = get_all_versions(db, program_name);
        if (!all.empty()) {
            // Two, not five. The point of this row is "you do have some of
            // these" -- the reader is going to run the action, not shop the
            // list, and a long row pushes the action off the screen.
            d.facts.push_back(diag::candidates(
                "installed", all, 2,
                std::format("xlings list {}", program_name)));
        }
        if (origin.fromProjectManifest) {
            // `xlings install` with no arguments reads the project manifest
            // and installs everything it declares. Naming the coordinate by
            // hand instead would make the user do what the project file
            // already says, and get it wrong when there is more than one
            // declared dependency.
            d.actions.push_back({ "set this project up", "xlings install" });
        } else {
            d.actions.push_back({ "install the pinned one",
                std::format("xlings install {}@{}", program_name, version) });
            if (!d.source.empty()) {
                // Deliberately does NOT repeat the source string: it is
                // already on the `from` row two lines up, and an action row
                // that restates its own context is how these blocks get long
                // enough that nobody finishes reading them.
                d.actions.push_back({ "or change the pin",
                                      "edit the file above" });
            }
        }
        diag::emit(d);
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
        if (auto vanishing =
                setup_envs(*vdata, "", xlings_home, active_subos_dir)) {
            report_vanishing_reference_(program_name, *vanishing);
            return 1;
        }

        // COMPAT: an alias written before the placeholder existed carries the
        // absolute path of whatever subos was active at install time. Re-point
        // it at the subos THIS process resolves to -- project, env and global
        // selection all land on xvm_artifact_subos_dir(). A record written by
        // a current xlings has no such path, so this is a no-op for it.
        //
        // Kept for records written before the placeholder existed. New
        // records carry `${XLINGS_SUBOS}` and have no path here to rewrite.
        std::string alias_cmd = normalize_subos_paths(
            vdata->alias[0], xlings_home, active_subos_dir);

        // The record stored a marker rather than one subos's answer, and this
        // is the only layer that knows which subos this process resolved to
        // -- which is exactly why the answer was not in the database.
        alias_cmd = expand_subos_placeholder(alias_cmd, active_subos_dir);

        // Refuse rather than hand a vanishing reference to the shell.
        //
        // This is the guard the 0.4.51 incident needed: that client predated
        // the marker, passed it through, and `sh` turned `--sysroot=<marker>`
        // into `--sysroot=` -- a valid flag meaning "use the host root". Every
        // build in the home silently stopped being self-contained and the
        // first visible symptom was a missing crt1.o three layers down.
        //
        // Checked AFTER setup_envs, so a variable the alias legitimately
        // defers to (`${XLINGS_SUBOS_LIB}`) has already been exported and does
        // not trip this.
        if (auto vanishing = vanishing_xlings_reference(alias_cmd)) {
            report_vanishing_reference_(program_name, *vanishing);
            return 1;
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
    if (auto vanishing = setup_envs(*vdata, exe_path.string(), xlings_home,
                                    active_subos_dir)) {
        report_vanishing_reference_(program_name, *vanishing);
        return 1;
    }

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

}
