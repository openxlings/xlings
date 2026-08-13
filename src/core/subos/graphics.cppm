// What this subos will actually render GL through -- read, never measured.
//
// WHY A READER AND NOT A PROBE
//
// The graphics stack's failure mode is that it succeeds. glvnd dlopens each
// vendor library by name; when one fails to load it falls through to the next
// with no diagnostic, so a machine whose NVIDIA vendor cannot load still draws
// a window, still prints a GL_RENDERER, and still exits 0 -- on llvmpipe. The
// only difference a user can observe is frame rate.
//
// The stack's installer already answers this question: it has `readelf` and
// `patchelf` in hand at wiring time, probes every entry point each vendor
// ships, and writes the verdict next to what it wired (xim-pkgindex
// `graphics.record_wiring`). That record is the answer. This module reads it.
//
// It deliberately does NOT re-derive it. Two reasons, and the second is the
// one that bites:
//
//   * `xlings subos info` is a local query, and local queries answer instantly
//     (2026.8.10.1). Shelling out to readelf per vendor library would put a
//     dozen subprocesses behind a command that currently touches only the
//     filesystem.
//   * A second answerer to a question already answered is how this codebase
//     produces contradictions -- the doctor/repairer predicate drift, the
//     reporter that says one thing while the fixer does another. The installer
//     probed with the tools present; a reader that re-probes on a machine
//     where they are absent would report a DIFFERENT verdict for the same
//     wiring, and neither would be wrong.
//
// WHY THE ANCHOR IS `<subos>/lib/libGLX.so.0`
//
// The vendor directory lives inside libglvnd's payload, not inside the subos,
// and a home can hold several libglvnd payloads at once. Asking "which
// libglvnd is in the store" is the wrong question -- the answer is per-subos.
//
// So the anchor is the same edge the dynamic loader walks: a GL program in
// this subos loads `<subos>/lib/libGLX.so.0`, which is a symlink into exactly
// one payload, and glvnd finds its vendors through that library's own RPATH,
// which points at `<payload>/lib/glx-vendor`. Following the symlink is
// therefore not a lookup heuristic; it is the runtime's own resolution, done
// with `readlink` instead of `dlopen`.
//
// The subdirectory name is shared with the writer as
// `graphics.GLX_VENDOR_SUBDIR` in xim-pkgindex/libs/graphics.lua. If it ever
// moves, both sides move together, and the "no dispatch" branch below is what
// a mismatch looks like -- absence, not a wrong answer.
//
// THREE KINDS OF ABSENCE
//
// An unwritten record is not a healthy stack, and it is not one condition:
//
//   NoDispatch  -- no libGLX.so.0 in the farm. This subos does no GL at all;
//                  there is nothing to report and nothing wrong.
//   NoVendors   -- dispatch present, vendor directory empty. THIS is the
//                  software-rendering failure, and it is visible without any
//                  record at all.
//   Unrecorded  -- vendors present, no record. Wired by a graphics recipe
//                  older than the probe; the wiring may be fine, we cannot
//                  say. Saying "ok" here would be the silent-success bug in a
//                  new place.
//
// Refs: .agents/docs/2026-08-10-graphics-stack-architecture-review-and-plan.md
export module xlings.core.subos.graphics;

import std;

export namespace xlings::subos::graphics {

namespace fs = std::filesystem;

// The subdirectory of libglvnd's payload that holds the vendor libraries.
// Kept in one place because it is a cross-repository contract: the writer is
// `graphics.GLX_VENDOR_SUBDIR` in xim-pkgindex.
inline constexpr std::string_view kVendorSubdir = "glx-vendor";
inline constexpr std::string_view kRecordName   = ".wiring";

// The library a GL program in a subos loads first. Its directory is the
// libglvnd payload, and its RPATH is what finds the vendors.
inline constexpr std::string_view kDispatchLib = "libGLX.so.0";

struct VendorWiring {
    std::string soname;                 // libGLX_nvidia.so.0
    std::string state;                  // ok | native | broken | unverified
    std::string reason;                 // broken only, e.g. runpath-not-transitive
    std::vector<std::string> missing;   // broken only: unresolved SONAMEs
    // The payload directory this verdict was measured against. Written by the
    // index since graphics 0.1.5; EMPTY for every record written before that,
    // and an empty one must produce no verdict -- see stale below.
    std::string payload;
    // Derived HERE, not recorded: the record no longer describes what is on
    // disk. See recompute_staleness_ for the two local tests and why neither
    // of them asks the index anything.
    bool stale { false };
    std::string staleDetail;

    bool is_broken() const { return state == "broken"; }

    // The verdict that depends on WHO OPENS IT.
    //
    // `vendor_closure_gaps` judges the interposer in isolation: it carries
    // DT_RUNPATH, so the host driver behind it cannot reach our payloads. That
    // is true of the interposer alone and false of the process, because a
    // CONSUMER's DT_RPATH is transitive — when an executable stamped that way
    // opens this vendor, its search path covers the whole chain.
    //
    // Measured on one home, one interposer, changing only the consumer:
    // DT_RUNPATH cannot open libEGL_nvidia, DT_RPATH loads it and renders on
    // the GPU. Before libxpkg 0.0.57 nearly every installed executable was
    // DT_RUNPATH, so `broken` was accurate in practice; after it, installed
    // programs are DT_RPATH and `broken` under-reports. See #537.
    //
    // Under-reporting is the worse half: it sends someone to fix a problem
    // that is not there, and it hides the one that is — programs the user
    // builds are still DT_RUNPATH (#532), and this is exactly the state that
    // says so.
    bool needs_transitive_consumer() const {
        return state == "needs-transitive-consumer";
    }
    // `native` means the probe found no host driver behind this vendor -- it
    // is one of our own builds, so there is no host closure to complete. That
    // is a pass, and it is NOT the same fact as `ok`, which means a host
    // driver IS behind it and everything it needs is reachable.
    // FAILS CLOSED ON `stale`, for the reason stated one function down about
    // an unrecognized state: a verdict measured against a payload that is no
    // longer in place is not evidence that anything passes. Today the only
    // production consumer of this module renders `describe()` -- which already
    // replaces the verdict -- so nothing changes; the point is that the next
    // caller cannot read `ok` off a record that has expired.
    bool is_ok() const {
        return !stale && (state == "ok" || state == "native");
    }
};

enum class WiringStatus {
    NoDispatch,
    NoVendors,
    Unrecorded,
    Recorded,
};

struct GraphicsWiring {
    WiringStatus status { WiringStatus::NoDispatch };
    fs::path dispatchDir;               // the libglvnd payload this subos loads
    std::string recordedDispatch;       // `dispatch=` from the record, if any
    // The record was written against a different payload than this subos now
    // loads -- a farm rebuilt onto a new libglvnd without re-running the
    // assembler. The states below then describe libraries nobody will load.
    bool dispatchMismatch { false };
    int vendorFiles { 0 };              // entries in glx-vendor/, record excluded
    std::vector<VendorWiring> vendors;

    bool has_dispatch() const { return status != WiringStatus::NoDispatch; }
    int broken_count() const {
        return static_cast<int>(std::ranges::count_if(
            vendors, [](const VendorWiring& v) { return v.is_broken(); }));
    }
};

// ─── record parsing ────────────────────────────────────────────────────────
//
// Plain `key=value` tokens, one entity per line. The format has no escaping
// and no nesting on purpose: it crosses a repository boundary and is read by
// a different language than wrote it, so a format that needs a parser is a
// format that can fail to parse. Anything unrecognized is skipped rather than
// rejected -- a newer writer must be able to add a key without making this
// reader report an empty stack.

struct ParsedRecord {
    std::string dispatch;
    std::vector<VendorWiring> vendors;
};

inline std::vector<std::string> split_(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

inline std::string_view trim_(std::string_view s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && ws(s.back()))  s.remove_suffix(1);
    return s;
}

ParsedRecord parse_wiring_record(std::string_view text);

// ─── filesystem read ───────────────────────────────────────────────────────

// Resolve both sides before comparing.
//
// One-sided resolution has produced a whole class of false findings here: the
// record holds the path the installer saw, this holds the path the farm
// resolves to, and either may run through a symlinked prefix (`/tmp` on
// macOS, a bind-mounted home) that the other does not. Comparing the strings
// would report a mismatch on every such machine.
inline bool same_path_(const fs::path& a, const fs::path& b) {
    if (a.empty() || b.empty()) return true;   // nothing to disagree about
    std::error_code ec1, ec2;
    auto ca = fs::weakly_canonical(a, ec1);
    auto cb = fs::weakly_canonical(b, ec2);
    if (ec1 || ec2) return a == b;
    return ca == cb;
}

// Has each verdict outlived the payload it was measured against?
//
// The record is written by ONE package (`graphics`) and every line in it is
// about ANOTHER (mesa, libglvnd, nvidia-gl-host-link). Those upgrade
// independently, and nothing made the record expire when they did. Measured on
// a real home: the record was written 57 seconds BEFORE the nvidia payload it
// speaks for.
//
// TWO LOCAL TESTS, no index, no probe.
//
//   * a vendor wired into `glx-vendor/` -- follow the symlink. It is absolute
//     and points into a payload; if it no longer lands inside the recorded
//     `payload=`, that vendor was upgraded and the assembler was not re-run.
//     This is exactly what an upgrade changes and what a re-run would fix.
//   * a vendor NOT in that directory (EGL and GLES are found through the
//     glvnd JSON, not through the GLX directory) -- the recorded payload must
//     still exist and still hold that soname.
//
// WHY NOT "is this the current version of that package": because the reader
// cannot answer it. `subos info` answers from local state by contract -- an
// instant local query, no index parsing (2026.8.10.1) -- so a criterion that
// needs the index is a criterion that would be silently skipped. The plan for
// this check originally said "compare versions", and that is where it would
// have died.
//
// NO `payload=` -> NO VERDICT. Every record written before graphics 0.1.5
// lacks the field, and calling those stale would mark every existing home's
// stack as suspect on the strength of a field nobody wrote. Same rule as
// `registered` in xim/install_state.cppm: an absent observation is not an
// observation of absence.
inline void recompute_staleness_(std::vector<VendorWiring>& vendors,
                                 const fs::path& vendorDir) {
    for (auto& v : vendors) {
        if (v.payload.empty()) continue;          // no observation
        std::error_code ec;
        const auto wired = vendorDir / v.soname;
        const auto st = fs::symlink_status(wired, ec);
        if (!ec && st.type() != fs::file_type::not_found) {
            auto target = fs::weakly_canonical(wired, ec);
            if (ec) continue;                     // unreadable: no verdict
            auto recorded = fs::weakly_canonical(fs::path(v.payload), ec);
            if (ec) continue;
            // lexically_relative rather than a string prefix: `/a/b` must not
            // read as a parent of `/a/bc`.
            //
            // `generic_string()`, not `native()`: path::string_type is
            // std::wstring on Windows, so comparing it against a narrow
            // literal does not compile there. This module only describes a
            // Linux stack, but it is still built on every platform.
            const auto rel = target.lexically_relative(recorded).generic_string();
            if (rel.empty() || rel.starts_with("..")) {
                v.stale = true;
                v.staleDetail = "wired to " + target.parent_path().string()
                              + ", recorded against " + v.payload;
            }
            continue;
        }
        // Not in glx-vendor/ -- an EGL or GLES entry point. The record is
        // still about a file, so ask whether that file is still there.
        if (!fs::exists(fs::path(v.payload) / "lib" / v.soname, ec)) {
            v.stale = true;
            v.staleDetail = v.payload + " no longer holds " + v.soname;
        }
    }
}

GraphicsWiring read_graphics_wiring(const fs::path& subosDir);

// ─── host driver drift ─────────────────────────────────────────────────────
//
// The one piece of this stack we do not own is the NVIDIA userspace driver: it
// is in lockstep with the host's kernel module (550.144.03 userspace talks to
// 550.144.03 `nvidia.ko` and to nothing else), so the stack links to the
// host's files rather than shipping them. That makes the host driver a version
// that moves under us — a distribution update replaces it, the versioned
// SONAMEs our payload symlinks to change, and the wiring recorded at install
// time describes a driver that is no longer there.
//
// The detector already exists and works: `xlings-gl-doctor`, shipped by the
// nvidia-gl-host-link package, compares the version stamped at install against
// the one the kernel module reports now. What it lacks is a way to reach the
// user — it has to be remembered and run. This reads the same two files it
// reads, so `subos info` can say it without anyone remembering.
//
// Two plain file reads, no subprocess: the local-query-answers-instantly
// contract (2026.8.10.1) applies here as much as to the wiring record, and the
// values are already written down. Re-deriving the driver version by running
// something would make this the second answerer to a question the installer
// answered.
struct DriverStamp {
    bool known { false };        // the payload recorded a version at install
    std::string builtFor;        // <nvidia payload>/.host-driver-version
    std::string hostNow;         // /sys/module/nvidia/version
    bool drifted() const {
        // Only a DISAGREEMENT is drift. An unknown on either side is not:
        // a machine whose module is not loaded right now (`hostNow` empty)
        // has not changed driver, it has no driver running, and reporting
        // that as drift would cry wolf on every laptop with the GPU asleep.
        return known && !builtFor.empty() && !hostNow.empty()
               && builtFor != hostNow;
    }
};

inline std::string read_trimmed_(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::string s;
    std::getline(in, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// `vendorDir` is the dispatch's `glx-vendor/`. The NVIDIA entry there is a
// symlink into the nvidia payload, so the payload root — and the stamp beside
// it — is reached the same way everything else here is reached: by following
// the link the loader would follow, not by searching the store.
DriverStamp read_driver_stamp(const fs::path& vendorDir);

// ─── display ───────────────────────────────────────────────────────────────
//
// Presentation, not re-derivation: the SONAME is the record's key, and this
// only decides how to spell it for a human. A soname that matches none of the
// four entry points is shown verbatim rather than guessed at.

struct VendorLabel {
    std::string vendor;   // nvidia
    std::string api;      // GLX
};

std::optional<VendorLabel> label_for(std::string_view soname);

std::string describe(const VendorWiring& v);

} // namespace xlings::subos::graphics
