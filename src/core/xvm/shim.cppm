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
bool is_xlings_binary(std::string_view name);

// Extract basename from argv[0], stripping path and extension
std::string extract_program_name(const char* argv0);

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
bool is_home_root(const std::filesystem::path& dir);

// Find the home that owns the invoked shim by walking parents upward.
// Tries the shim file as invoked first (only when argv[0] carries a
// path — a bare name came from PATH search and is not a location),
// then the running executable path (Windows: the hardlinked shim path;
// Unix: the symlink-resolved real binary inside its home — both land
// inside the owning home). The walk meets the INNERMOST home root
// first, so nested homes resolve to the nearest owner.
std::optional<std::filesystem::path>
resolve_owner_home(const std::filesystem::path& invoked);

// Lightweight probe: does <home>'s version DB register `program` (with
// at least one version)? Reads <home>/.xlings.json directly — no Config
// singleton involvement, so candidate homes can be probed before the
// dispatch home is chosen. A registered-but-broken entry counts as a
// hit: the error is then reported against that home instead of silently
// jumping homes (genuine breakage must not be masked).
bool home_knows_program(const std::filesystem::path& home,
                        const std::string& program);

// Choose the dispatch home for a shim invocation: owner → env →
// default (§3 of the design doc). Returns the home to inject via
// Config::override_home(), or nullopt to leave Config's legacy
// resolution untouched (legacy mode, or no candidate knows better).
std::optional<std::filesystem::path>
resolve_dispatch_home(const std::string& program, const char* argv0);

// Resolve the real executable path for a shim target
std::filesystem::path resolve_executable(const std::string& program_name,
                                         const std::string& path,
                                         const std::string& xlings_home);

// Where an alias's program was found, in the order the runtime looks.
//
// The alias path below does not call resolve_executable at all in the normal
// case: it PREPENDS `<payload>`, `<payload>/bin` and the subos bin dir to
// PATH and hands the command to a shell (see the alias branch of
// shim_dispatch). So an alias naming a sibling xlings command -- thirty of
// mcpp's short commands name `mcpp`, which lives in a different package --
// resolves at the third position, and one naming a host tool resolves at the
// fourth.
//
// This mattered because `self doctor` was asking resolve_executable, which
// only knows the first two, and reporting everything past them as
// unresolvable. Thirty-four such warnings on one measured home, every one of
// them a command that runs correctly. The point of the enum is that the
// caller can tell those apart from an alias that really has nothing to run:
// they were a single warning level before, so a genuine break was
// indistinguishable from the noise around it.
enum class AliasOrigin {
    Absolute,     // the alias named a full path and it is there
    Payload,      // <payload>/ or <payload>/bin -- the package's own
    SubosBin,     // a sibling shim in the active subos: still xlings's
    SystemPath,   // inherited PATH: works, but the host is providing it
    Nowhere,      // nothing to exec
};

struct AliasResolution {
    AliasOrigin origin { AliasOrigin::Nowhere };
    std::filesystem::path path;
};

// Resolve an alias program the way the alias branch of shim_dispatch will.
//
// `searchPath` is the PATH to search at the fourth position; callers pass the
// live environment, tests pass their own. Empty means "do not look" -- which
// is not the same as "found nothing", so a caller that cannot see a
// representative PATH gets Nowhere and should say so rather than claim the
// alias is broken.
AliasResolution resolve_alias_program(const std::string& program_name,
                                      const std::string& path,
                                      const std::string& xlings_home,
                                      const std::filesystem::path& subos_bin,
                                      std::string_view searchPath);

// Merge a shim-injected env value into the existing one. PATH-style vars
// prepend (new first), but a value already present — as the whole value or as
// one separator-delimited component — must NOT be appended again: the blind
// re-append corrupted single-value vars (GIT_SSL_CAINFO="x" became "x:x",
// which curl reads as one nonexistent filename and every HTTPS git transport
// dies; hit when both compact::git's CA pin (#378) and the xim git.lua envs
// (xim-pkgindex#406) resolve the same bundle path). Returns the value to set,
// or nullopt for "leave the env untouched".
std::optional<std::string> merge_shim_env_value(const std::string& expanded,
                                                const std::string& existing);

// Set environment variables for a program before exec.
//
// Returns the name of a `${XLINGS_*}` reference that would have reached the
// child as an empty string, or nullopt when everything resolved. See
// vanishing_xlings_reference for why that is the failure worth refusing.
std::optional<std::string> setup_envs(const VData& vdata,
                const std::string& resolved_path,
                const std::string& xlings_home,
                const std::string& active_subos_dir);

// One message for both ways a `${XLINGS_*}` reference can vanish, because the
// user's next move differs and only one of the two is their problem.
void report_vanishing_reference_(const std::string& program_name,
                                 const std::string& name);

// Main shim dispatch: called when argv[0] is a tool name (not xlings/xim)
int shim_dispatch(const std::string& program_name, int argc, char* argv[]);

} // namespace xlings::xvm
