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
    bool is_ok() const { return state == "ok" || state == "native"; }
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

ParsedRecord parse_wiring_record(std::string_view text) {
    ParsedRecord rec;

    for (auto rawLine : split_(text, '\n')) {
        auto line = trim_(rawLine);
        if (line.empty()) continue;

        // `dispatch=` takes the whole rest of the line: it is a path, and a
        // path may contain spaces. Every other key is token-separated.
        if (line.starts_with("dispatch=")) {
            rec.dispatch = std::string(trim_(line.substr(9)));
            continue;
        }
        if (!line.starts_with("vendor=")) continue;

        VendorWiring v;
        for (auto& tok : split_(line, ' ')) {
            auto t = trim_(tok);
            if (t.empty()) continue;
            auto eq = t.find('=');
            if (eq == std::string_view::npos) continue;
            auto key = t.substr(0, eq);
            auto val = t.substr(eq + 1);
            if (key == "vendor")      v.soname = std::string(val);
            else if (key == "state")  v.state  = std::string(val);
            else if (key == "reason") v.reason = std::string(val);
            else if (key == "missing") {
                for (auto& m : split_(val, ',')) {
                    auto mm = trim_(m);
                    if (!mm.empty()) v.missing.emplace_back(mm);
                }
            }
        }
        // A line naming no library describes nothing. Dropping it keeps a
        // truncated write (a crash mid-`io.writefile`) from rendering as a
        // vendor with an empty name and an unknown state.
        if (v.soname.empty()) continue;
        // An unrecognized state must not read as a pass. `is_ok()` already
        // fails closed, but naming it keeps the panel's text honest.
        if (v.state.empty()) v.state = "unverified";
        rec.vendors.push_back(std::move(v));
    }
    return rec;
}

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

GraphicsWiring read_graphics_wiring(const fs::path& subosDir) {
    GraphicsWiring w;

    std::error_code ec;
    auto dispatchLib = subosDir / "lib" / fs::path(std::string(kDispatchLib));
    // `symlink_status`, not `exists`: a farm entry pointing at a payload that
    // has been removed is a DANGLING symlink. `exists` follows it and says
    // no, which would report "this subos does no GL" about a subos that is
    // wired to something that is gone -- a broken stack read as an absent one.
    auto st = fs::symlink_status(dispatchLib, ec);
    if (ec || st.type() == fs::file_type::not_found) return w;

    auto resolved = fs::weakly_canonical(dispatchLib, ec);
    if (ec) resolved = dispatchLib;
    w.dispatchDir = resolved.parent_path().parent_path();   // <payload>/lib/x -> <payload>

    auto vendorDir = resolved.parent_path() / fs::path(std::string(kVendorSubdir));

    w.status = WiringStatus::NoVendors;
    if (fs::is_directory(vendorDir, ec)) {
        for (auto it = fs::directory_iterator(vendorDir, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            auto name = it->path().filename().string();
            if (name.starts_with(".")) continue;   // the record itself
            ++w.vendorFiles;
        }
    }

    std::string text;
    {
        std::ifstream in(vendorDir / fs::path(std::string(kRecordName)),
                         std::ios::binary);
        if (in) {
            in.seekg(0, std::ios::end);
            auto sz = in.tellg();
            if (sz > 0) {
                text.resize(static_cast<std::size_t>(sz));
                in.seekg(0, std::ios::beg);
                in.read(text.data(), sz);
                text.resize(static_cast<std::size_t>(in.gcount()));
            }
        }
    }

    if (text.empty()) {
        // No record. Which absence it is depends on the directory, not on the
        // file: an empty vendor directory is the software-rendering failure
        // whether or not anything wrote a record.
        if (w.vendorFiles > 0) w.status = WiringStatus::Unrecorded;
        return w;
    }

    auto rec = parse_wiring_record(text);
    w.recordedDispatch = rec.dispatch;
    w.vendors = std::move(rec.vendors);
    w.dispatchMismatch = !same_path_(rec.dispatch, w.dispatchDir);
    // A record listing no vendor is not a record of a wired stack. Fall back
    // to what the directory says rather than rendering an empty vendor table
    // under a "recorded" heading.
    w.status = w.vendors.empty()
                   ? (w.vendorFiles > 0 ? WiringStatus::Unrecorded
                                        : WiringStatus::NoVendors)
                   : WiringStatus::Recorded;
    return w;
}

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
DriverStamp read_driver_stamp(const fs::path& vendorDir) {
    DriverStamp d;
    std::error_code ec;
    for (auto it = fs::directory_iterator(vendorDir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        auto name = it->path().filename().string();
        if (name.find("nvidia") == std::string::npos) continue;
        auto real = fs::weakly_canonical(it->path(), ec);
        if (ec) continue;
        // <payload>/lib/<soname> -> <payload>
        auto payload = real.parent_path().parent_path();
        auto stamp = payload / ".host-driver-version";
        if (!fs::exists(stamp, ec)) continue;
        d.known = true;
        d.builtFor = read_trimmed_(stamp);
        break;
    }
    if (d.known) d.hostNow = read_trimmed_("/sys/module/nvidia/version");
    return d;
}

// ─── display ───────────────────────────────────────────────────────────────
//
// Presentation, not re-derivation: the SONAME is the record's key, and this
// only decides how to spell it for a human. A soname that matches none of the
// four entry points is shown verbatim rather than guessed at.

struct VendorLabel {
    std::string vendor;   // nvidia
    std::string api;      // GLX
};

std::optional<VendorLabel> label_for(std::string_view soname) {
    struct Pat { std::string_view prefix, suffix, api; };
    static constexpr Pat kPats[] = {
        {"libGLX_",       ".so.0", "GLX"},
        {"libEGL_",       ".so.0", "EGL"},
        {"libGLESv1_CM_", ".so.1", "GLESv1"},
        {"libGLESv2_",    ".so.2", "GLESv2"},
    };
    for (auto& p : kPats) {
        if (soname.starts_with(p.prefix) && soname.ends_with(p.suffix)) {
            auto mid = soname.substr(
                p.prefix.size(), soname.size() - p.prefix.size() - p.suffix.size());
            if (mid.empty()) continue;
            return VendorLabel{std::string(mid), std::string(p.api)};
        }
    }
    return std::nullopt;
}

std::string describe(const VendorWiring& v) {
    if (v.state == "native")
        return "ok  (built by xlings — no host driver behind it)";
    if (v.state == "ok")
        return "ok  (the host driver behind it is reachable)";
    if (v.state == "needs-transitive-consumer")
        return "ok for installed programs — but a program you build here "
               "cannot load it: your build gets DT_RUNPATH, which is not "
               "transitive (see xlings#532). Installed packages get DT_RPATH "
               "and reach the GPU through it";
    if (v.state == "unverified")
        return "unverified — the installer could not read this library; "
               "if it is broken, GL renders elsewhere and says nothing";
    if (v.state == "broken") {
        std::string why = v.reason == "runpath-not-transitive"
            ? "it carries DT_RUNPATH, which is not transitive, so the host "
              "driver behind it reaches none of our payloads"
            : (!v.missing.empty()
                   ? "the host driver behind it needs " +
                     [&] {
                         std::string s;
                         for (auto& m : v.missing) {
                             if (!s.empty()) s += ", ";
                             s += m;
                         }
                         return s;
                     }() +
                     ", which nothing on its search path provides"
                   : (v.reason.empty() ? "the installer did not say why"
                                       : v.reason));
        return "BROKEN — " + why +
               ". GL still renders, on a different driver, without saying so";
    }
    // A verdict this client has never heard of. The index ships independently
    // of the client, so a newer recipe WILL reach an older xlings, and showing
    // the bare word ("quarantined") would read as a state that is fine. Say
    // that this xlings cannot interpret it, and what to do about that.
    return "state '" + v.state + "' is newer than this xlings — upgrade to "
           "read it; until then this vendor is unassessed";
}

} // namespace xlings::subos::graphics
