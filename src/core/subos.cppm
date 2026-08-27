module;

// System headers used by use_spawn_shell only. `import std;` doesn't
// pull these in, and we want execl/errno (POSIX) or CreateProcess
// (Win32) without #include in the named-module purview (which the
// standard forbids for headers that aren't importable units).
#include <cstdio>  // stderr (used by std::println(stderr, ...))
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// windows.h defines `min`/`max` as function-like macros, which turns any
// `std::min({a, b, c})` in this module into "too many arguments provided to
// function-like macro invocation" -- an error that names the call site and
// says nothing about where the macro came from. Every other module here that
// includes windows.h already defines this (platform.cppm, platform/windows.cppm,
// platform/target.cppm); this one was the exception, and it cost four
// consecutive red Windows runs.
#define NOMINMAX
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
import xlings.core.log;
import xlings.platform;
import xlings.runtime;
import xlings.core.utils;
import xlings.core.xself;
import xlings.core.xim.commands;  // auto_install_backend_ needs cmd_install
import xlings.core.subos.keeper;
import xlings.core.subos.gpu;
import xlings.core.subos.graphics;
import xlings.core.subos.sandbox;
import xlings.core.subos.manifest;
// Leaf module (std + json only). Same source for "is this a global option"
// that the CLI validator uses, so the two cannot disagree about `--yes`.
import xlings.cli.spec;

namespace xlings::subos {

namespace fs = std::filesystem;

export struct SubosInfo {
    std::string   name;
    fs::path      dir;
    bool          isActive;
    int           toolCount;
};

export struct SubosCandidateView {
    std::vector<SubosInfo> candidates;
};

export SubosCandidateView candidate_view(bool includeToolCount = true);

export std::vector<SubosInfo> list_all();

struct CandidateResolution_ {
    std::string selected;
    std::string reason;
    std::vector<SubosInfo> candidates;
    bool autoSelected { false };
};

struct UseNameResolution_ {
    std::string selected;
    int exitCode { 0 };
};

// Create a subos. V6: storage mode is a creation-time property
// (`--storage image|tmpfs|shared`). Non-shared modes force sandbox
// entry at use-time. The sandbox-private dirs are laid down lazily.
//
// `runtime` is the subos's declared runtime binding ("glibc@2.39"): what its
// binaries are built against. Empty means "resolve it" -- see
// resolve_default_runtime below. It is a creation-time property because
// changing it after the fact would invalidate every payload already installed.
export int create(const std::string& name, const fs::path& customDir,
                  sandbox::StorageMode storage, const std::string& imageSize,
                  const std::string& runtime,
                  EventStream& stream);

// What a subos gets when nobody named a runtime: the index's answer for
// manifest::DEFAULT_RUNTIME_PACKAGE, or the pinned fallback when the index
// cannot be read.
//
// It lives HERE and not in the manifest module because answering it needs the
// catalog, and that module is deliberately free of Config/xvm/catalog imports
// so its invariant checks stay testable without a home on disk. The catalog is
// read LocalOnly -- index files already on disk, no sync, no network -- which
// is the same access subos/sandbox.cpp already takes for backend availability.
//
// `resolved` distinguishes the two outcomes, and the caller must record it:
// a fallback binding and a resolved one are byte-identical in the manifest,
// and only one of them is a decision.
export struct DefaultRuntime {
    std::string binding;
    bool        resolved { false };   // false -> came from the pinned fallback
};
export DefaultRuntime resolve_default_runtime();

// Back-compat overloads. Callers that predate the runtime argument get the
// built-in default, and callers that predate storage get shared as before.
export int create(const std::string& name, const fs::path& customDir,
                  sandbox::StorageMode storage, const std::string& imageSize,
                  EventStream& stream);

export int create(const std::string& name, const fs::path& customDir,
                  EventStream& stream);

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

// Parse `[<ns>:]<name>[@<ver>]` → {ns, name, ver}. Empty ns if absent;
// empty ver if absent ("latest" semantics handled downstream).
struct PkgRef {
    std::string ns;
    std::string name;
    std::string ver;
};

} // namespace new_from_detail_

export int new_from(const std::string& name, const fs::path& customDir,
                    sandbox::StorageMode storage, const std::string& imageSize,
                    const std::string& fromSpec, const std::string& runtime,
                    EventStream& stream);

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
// All three share validation through the bounded candidate view. This keeps
// registered environments authoritative while allowing only the legacy
// default-manifest compatibility case.

namespace use_detail_ {

inline int validate_subos_(const std::string& name, EventStream& stream) {
    auto candidates = candidate_view(false).candidates;
    if (std::ranges::find(candidates, name, &SubosInfo::name)
        == candidates.end()) {
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

// Where a provider's payload lives, for `${pkgdir}`.
//
// A binding is `<name>@<version>` and carries no namespace, while the store
// directory is `<ns>-x-<name>` (or a bare `<name>` for the primary index). So
// the bare spelling is tried first and the namespaced ones are found by scan.
// The scan is over the handful of bindings that actually appear in `envs`,
// not over the whole store.
//
// Returning empty is meaningful: expansion then leaves `${pkgdir}` in place
// rather than collapsing the value to a host path, and doctor D3 reports it.
inline fs::path pkgdir_for_binding_(std::string_view binding) {
    const auto at = binding.find('@');
    if (at == std::string_view::npos) return {};
    const std::string name(binding.substr(0, at));
    const std::string version(binding.substr(at + 1));
    if (name.empty() || version.empty()) return {};

    const auto store = Config::paths().dataDir / "xpkgs";
    std::error_code ec;

    if (auto direct = store / name / version; fs::is_directory(direct, ec))
        return direct;

    if (!fs::is_directory(store, ec)) return {};
    const auto suffix = "-x-" + name;
    for (const auto& entry : platform::dir_entries(store)) {
        if (!entry.is_directory(ec)) continue;
        if (!entry.path().filename().string().ends_with(suffix)) continue;
        if (auto candidate = entry.path() / version;
            fs::is_directory(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

inline manifest::Placeholders placeholders_for_(const fs::path& subosDir) {
    return manifest::Placeholders{
        .subosdir    = subosDir,
        .home        = platform::get_home_dir(),
        .xlings_home = Config::paths().homeDir,
        .pkgdir_of   = pkgdir_for_binding_,
    };
}

// Is this the name of a library that is welded to a particular ld.so?
//
// ld.so and libc.so.6 are two halves of one build and talk to each other over
// GLIBC_PRIVATE. Pairing halves from different builds does not fail to load --
// it segfaults before main, naming nothing.
//
// This list is deliberately just those two, and not the rest of the glibc set.
// The argument for a wider list is that libm and friends come from the same
// build; the argument against is measured. nvidia-gl-host-link has to offer
// the vendor library libpthread/librt/libdl, which since glibc 2.34 are
// compatibility stubs with 27, 13 and 9 defined symbols -- their
// implementations moved into libc.so.6 -- and without them the NVIDIA device
// disappears from EGL enumeration entirely. Flagging those would break a
// working configuration to prevent nothing. libm has real surface (1203
// symbols), but a host binary that picks up a mismatched libm dies with
// "version `GLIBC_2.38' not found" and names the file. Loud and diagnosable
// is not what this guard is for; it is for the failure that names nothing.
inline bool is_loader_coupled_soname_(std::string_view file) {
    return file == "libc.so.6"
        || file.starts_with("libc.musl-")
        || file.starts_with("ld-linux")
        || file.starts_with("ld-musl");
}

// Refuse to put a libc on a process-global search path.
//
// LD_LIBRARY_PATH and LD_PRELOAD are inherited by every child, and most
// children in a subos are HOST binaries running under the HOST loader. A
// directory of ours holding libc.so.6 hands them our half of a pair whose
// other half is the host's, which is the loader/libc split the rest of this
// codebase exists to prevent -- arrived at from the one direction the
// same-source assertion cannot see, because nothing we installed is wrong.
// `xlings subos use` returned a /bin/bash that died of SIGSEGV before
// printing a character, on a host whose glibc was the same upstream version
// as ours and merely a different build.
//
// Dropping the offending entry rather than the whole variable: the other
// directories on it are usually the point of the declaration, and a package
// that gathers dependencies into one directory has no way to ask for "all of
// these except the libc" today.
inline void drop_loader_coupled_dirs_(std::vector<manifest::Resolved>& vars) {
    for (auto& v : vars) {
        const bool is_preload = v.var == "LD_PRELOAD"
                             || v.var == "DYLD_INSERT_LIBRARIES";
        if (v.var != "LD_LIBRARY_PATH" && v.var != "DYLD_LIBRARY_PATH"
            && !is_preload) continue;

        std::vector<std::string> kept, dropped;
        std::error_code ec;
        // Hand-rolled split/join. `views::split | ranges::to<std::string>`
        // reads better and makes gcc 16 fail the whole module with "Bad file
        // data" pointing at an unrelated TU; gcc 15 compiles it. Not worth a
        // toolchain investigation for four lines.
        for (std::size_t pos = 0; pos <= v.value.size(); ) {
            const auto sep = v.value.find(':', pos);
            const auto len = (sep == std::string::npos ? v.value.size() : sep) - pos;
            std::string entry = v.value.substr(pos, len);
            pos = (sep == std::string::npos ? v.value.size() : sep) + 1;
            if (entry.empty()) { if (sep == std::string::npos) break; continue; }

            bool offends = false;
            if (is_preload) {
                offends = is_loader_coupled_soname_(
                    fs::path(entry).filename().string());
            } else if (fs::is_directory(entry, ec)) {
                for (const auto& e : platform::dir_entries(entry)) {
                    if (is_loader_coupled_soname_(e.path().filename().string())) {
                        offends = true;
                        break;
                    }
                }
            }
            (offends ? dropped : kept).push_back(std::move(entry));
            if (sep == std::string::npos) break;
        }
        if (dropped.empty()) continue;

        v.value.clear();
        for (const auto& k : kept) {
            if (!v.value.empty()) v.value += ':';
            v.value += k;
        }
        for (const auto& d : dropped) {
            std::println(stderr,
                "[xlings]   {} — dropped {} (declared by {}): it holds a libc, "
                "and this variable is inherited by host binaries running under "
                "the host loader",
                v.var, d,
                v.providers.empty() ? std::string{"?"} : v.providers.front());
        }
    }
    std::erase_if(vars, [](const manifest::Resolved& v) {
        return v.value.empty() && !v.unresolved;
    });
}

// The variables a subos exports, resolved and ready to apply.
//
// Empty for a subos with no declarations, which is every subos until a package
// makes one -- so both call sites below stay silent in the common case.
inline std::vector<manifest::Resolved> subos_env_for_(const std::string& name) {
    const auto dir = Config::subos_dir(name);
    auto doc = manifest::read_document(dir);
    if (!doc) return {};
    // C2: only the providers that can take effect. A package installed at two
    // versions keeps both sections -- the dormant one is what lets `xlings use`
    // switch back without a reinstall -- but only the active one contributes.
    // Which is active is xvm's answer, recorded in this same file; nothing here
    // re-derives it. See manifest::select_effective.
    auto info = manifest::select_effective(manifest::parse(*doc),
                                           manifest::active_versions(*doc));
    auto vars = manifest::resolve(info, placeholders_for_(dir));
    drop_loader_coupled_dirs_(vars);
    return vars;
}

// UC-2: say what was injected.
//
// To stderr, always -- the `--shell` path's stdout is eval'd by the caller's
// shell, and this is a report, not code. Without it the user pipes an opaque
// blob into `eval` and cannot tell which package changed what.
inline void report_injected_env_(const std::string& subosName,
                                 const std::vector<manifest::Resolved>& vars) {
    if (vars.empty()) return;
    std::set<std::string> providers;
    for (const auto& v : vars)
        providers.insert(v.providers.begin(), v.providers.end());

    std::println(stderr, "[xlings] subos {}: {} env var(s) from {} package(s)",
                 subosName, vars.size(), providers.size());
    for (const auto& v : vars) {
        if (v.unresolved) {
            std::println(stderr, "[xlings]   {} — unresolved path, skipped "
                                 "(run `xlings self doctor`)", v.var);
        } else if (v.conflicted) {
            std::println(stderr, "[xlings]   {} — declared by {} packages, "
                                 "conflicting", v.var, v.providers.size());
        }
    }
}

// Put a subos's declared variables into THIS process's environment.
//
// Shared by the shell-spawn path and the sandbox path. Both hand their
// environment to a child -- `run_shell` to a shell, the sandbox to
// proot/bwrap, which pass it through -- so a variable only one of them sets
// makes the same subos two different environments depending on how it was
// entered. It did: `subos use` had LIBGL_DRIVERS_PATH and
// __EGL_VENDOR_LIBRARY_DIRS, `subos use --sandbox` had neither, and a GL
// program inside the sandbox fell back to whatever the host offered without
// anything saying so.
//
// UC-1 -- a variable already set in this environment is the user's, and `set`
// leaves it alone. `prepend` still contributes, since composing is what
// prepend means.
inline void apply_subos_env_(const std::string& name) {
    const auto envVars = subos_env_for_(name);
    report_injected_env_(name, envVars);
    for (const auto& v : envVars) {
        if (v.unresolved) continue;
        const auto existing = utils::get_env_or_default(v.var);
        if (v.op == manifest::OP_PREPEND) {
            platform::set_env_variable(
                v.var, existing.empty() ? v.value : v.value + ":" + existing);
        } else if (!utils::env_is_set(v.var)) {
            // Not `existing.empty()`: an exported-but-empty variable is a value
            // the user chose, and `set` yields to the user's value.
            platform::set_env_variable(v.var, v.value);
        }
    }
}

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
                    const std::string& cmd = "");

// Back-compat single-arg entry point: keeps existing callers (anyone who
// imports xlings::subos::use directly) on the legacy global behavior.
// CLI dispatch goes through `run()` which uses the flag-aware path below.
export int use(const std::string& name, EventStream& stream);

export int remove(const std::string& name, EventStream& stream);

export std::optional<SubosInfo> info(const std::string& name);

export int run(int argc, char* argv[], EventStream& stream);

} // namespace xlings::subos
