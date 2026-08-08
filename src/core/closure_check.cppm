module;

export module xlings.core.closure_check;

import std;
import xlings.platform;
import xlings.core.log;
import xlings.core.version_order;
import xlings.core.elf_same_source;

// Rule D at the second execution point: the client, at install time.
//
// The index CI runs dep-closure-check.sh over every changed package and fails
// hard when a payload anchored to OUR loader NEEDs a soname nothing provides.
// That predicate is right, and it runs in exactly one place -- a repository
// workflow the user's machine never sees. mcpp-community/mcpp#392 installed
// llvm on a real machine, llvm's toolchain put every user binary into form X,
// and nothing on that machine ever evaluated the closure it was now relying
// on. This module is the same predicate, evaluated where the payload actually
// lands.
//
// What it checks (form-X payload ELFs only -- host-anchored payloads resolve
// from the host by design, that is the documented arrangement, not a leak):
//
//   rule D   every DT_NEEDED soname is provided by the payload itself or by
//            some installed payload (symlinks count: the soname usually IS
//            the symlink). `*-host-link` packages install symlinks into a
//            payload, so the hardware exception passes without a special
//            case -- same property the CI script relies on.
//   rule A   the glibc payload the interpreter comes from must not be older
//            than the host's glibc recorded at subos creation. Host objects
//            can always enter a form-X process (dlopen, host-link), and an
//            older libc cannot serve their symbol versions.
//
// Rule B (loader and libc from one payload) is elfcheck's, already enforced
// hard earlier in the same install loop. This module deliberately does not
// re-state it.
//
// WARN-ONLY for now (C2.5 of the closure design: collect real gaps before
// turning the gate hard). A warning here names the exact soname and what it
// means; the alternative is a runtime failure on some other machine that
// names neither.
//
// Scope note: this runs on INDEX PACKAGES only -- the install pipeline is the
// only caller. User-built binaries are execution point 3 (the build tool's),
// and rule D deliberately does not apply to them at all: what a user links is
// the user's business (.agents/docs/2026-08-09-ecosystem-closure-design.md
// §0.2).
export namespace xlings::closurecheck {

namespace fs = std::filesystem;

// One soname a form-X ELF needs that nothing installed provides.
struct MissingSoname {
    std::string elf;      // absolute path of the ELF that needs it
    std::string soname;
};

// The interpreter's glibc payload is older than the host glibc.
struct VersionFloor {
    std::string elf;
    std::string oursVersion;   // version component of the interp payload
    std::string hostVersion;   // as recorded in the subos manifest
    std::string interpPayload;
};

struct Report {
    int scannedElves = 0;
    int formXElves   = 0;
    std::vector<MissingSoname>  missing;   // one entry per (first elf, soname)
    std::optional<VersionFloor> floor;     // one offender is enough to say it
};

// ── pure core ───────────────────────────────────────────────────────────

// The sonames in `needed` that neither the payload itself nor any installed
// payload provides. Pure so the decision -- not just the plumbing -- is unit
// testable.
inline std::vector<std::string>
unprovided(std::span<const std::string> needed,
           const std::set<std::string>& selfProvides,
           const std::set<std::string>& storeProvides) {
    std::vector<std::string> out;
    for (const auto& n : needed) {
        if (n.empty()) continue;
        if (selfProvides.contains(n)) continue;
        if (storeProvides.contains(n)) continue;
        out.push_back(n);
    }
    return out;
}

// ── store scanning ──────────────────────────────────────────────────────

// Every *.so* basename any installed payload offers. Symlinks count, and are
// the usual case: `libstdc++.so.6` is a symlink to `.so.6.0.33`, and the
// symlink is the name binaries actually NEED -- a files-only scan would
// report every properly-versioned library as missing (the CI script documents
// the same trap). The payload under test is excluded so it cannot vouch for
// itself twice.
inline std::set<std::string>
store_sonames(const fs::path& xpkgsRoot, const fs::path& excludePayload) {
    std::set<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(xpkgsRoot, ec)) return out;

    const auto excluded = excludePayload.lexically_normal();
    for (auto it = fs::recursive_directory_iterator(
             xpkgsRoot, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        // <store>/<version>/... : anything deeper than ~6 components is not
        // where sonames live; bounding the walk keeps a huge store cheap.
        if (it.depth() > 6) { it.disable_recursion_pending(); continue; }
        std::error_code fec;
        if (!it->is_regular_file(fec) && !it->is_symlink(fec)) continue;
        const auto name = it->path().filename().string();
        if (name.find(".so") == std::string::npos) continue;
        auto norm = it->path().lexically_normal();
        auto rel  = norm.lexically_relative(excluded);
        if (!rel.empty() && *rel.begin() != "..") continue;  // inside payload
        out.insert(name);
    }
    return out;
}

// The payload's own offering, by the same rule.
inline std::set<std::string> payload_sonames(const fs::path& payloadDir) {
    std::set<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             payloadDir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) && !it->is_symlink(fec)) continue;
        const auto name = it->path().filename().string();
        if (name.find(".so") != std::string::npos) out.insert(name);
    }
    return out;
}

// ── the scan ────────────────────────────────────────────────────────────

// `hostGlibc` comes from the subos manifest (recorded at creation); empty
// means unknown, and unknown is unprovable -- rule A is then not evaluated,
// never assumed to pass or fail.
inline Report scan_payload(const fs::path& payloadDir,
                           const fs::path& xpkgsRoot,
                           std::string_view hostGlibc = {}) {
    Report rep;
    std::error_code ec;
    if (!fs::is_directory(payloadDir, ec)) return rep;

    const auto patchelf = elfcheck::locate_patchelf(payloadDir);
    if (patchelf.empty()) return rep;   // unverifiable, not clean; stay silent

    auto run_lines = [&](const std::string& args) -> std::vector<std::string> {
        auto [rc, o] = platform::run_command_capture(
            std::format("\"{}\" {}", patchelf, args));
        std::vector<std::string> lines;
        if (rc != 0) return lines;
        for (const auto lineView : std::views::split(o, '\n')) {
            std::string line(lineView.begin(), lineView.end());
            while (!line.empty()
                   && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            // Shim advisory lines start with '['; patchelf output never does.
            if (line.empty() || line.front() == '[') continue;
            lines.push_back(std::move(line));
        }
        return lines;
    };

    std::optional<std::set<std::string>> store;   // built on first form-X ELF
    std::set<std::string> selfProvides = payload_sonames(payloadDir);
    std::set<std::string> reported;               // dedup sonames across ELFs

    for (auto it = fs::recursive_directory_iterator(
             payloadDir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code fec;
        if (!it->is_regular_file(fec) || it->is_symlink(fec)) continue;
        const auto path = it->path().string();

        std::ifstream f(path, std::ios::binary);
        char magic[4] = {};
        if (!f.read(magic, 4)) continue;
        if (!(magic[0] == '\x7f' && magic[1] == 'E'
              && magic[2] == 'L' && magic[3] == 'F')) continue;
        rep.scannedElves++;

        const auto interpLines =
            run_lines(std::format("--print-interpreter \"{}\"", path));
        if (interpLines.empty()) continue;        // library or static ELF
        const auto& interp = interpLines.back();
        const auto interpPayload = elfcheck::payload_of(interp);
        if (interpPayload.empty()) continue;      // host-anchored: by design
        rep.formXElves++;

        // rule A -- once per payload; the first form-X ELF names it.
        if (!rep.floor && !hostGlibc.empty()) {
            const auto ours = fs::path(interpPayload).filename().string();
            if (version_order::compare(ours, std::string(hostGlibc)) < 0) {
                rep.floor = VersionFloor{
                    .elf = path,
                    .oursVersion = ours,
                    .hostVersion = std::string(hostGlibc),
                    .interpPayload = interpPayload,
                };
            }
        }

        // rule D
        if (!store) store = store_sonames(xpkgsRoot, payloadDir);
        const auto needed = run_lines(std::format("--print-needed \"{}\"", path));
        for (auto& soname : unprovided(needed, selfProvides, *store)) {
            if (!reported.insert(soname).second) continue;
            rep.missing.push_back({ .elf = path, .soname = std::move(soname) });
        }
    }
    return rep;
}

// ── rendering ───────────────────────────────────────────────────────────

// The message must let the reader act without running LD_DEBUG themselves:
// name the soname, say what form X means for it, and say where exceptions go.
inline std::string describe_missing(const MissingSoname& m) {
    return std::format(
        "closure gap: {} is anchored to our loader but NEEDs {}, which no "
        "installed payload provides.\n"
        "  A form-X binary cannot fall back to host libraries (our ld.so has "
        "no usable ld.so.cache); at runtime this dies with an error that "
        "names neither the package nor the cause. Host hardware libraries "
        "enter through a *-host-link package, everything else belongs in the "
        "index. (rule D, warn-only)",
        fs::path(m.elf).filename().string(), m.soname);
}

inline std::string describe_floor(const VersionFloor& v) {
    return std::format(
        "version floor: {} runs on glibc {} while this subos recorded the "
        "host at glibc {}.\n"
        "  Host objects can always enter a form-X process (dlopen, "
        "*-host-link), and a {} libc cannot serve symbols a host-built "
        "library compiled against {} may require -- the failure appears "
        "later, inside whatever loads the host object, and blames it. "
        "(rule A, warn-only)",
        fs::path(v.elf).filename().string(), v.oursVersion, v.hostVersion,
        v.oursVersion, v.hostVersion);
}

} // namespace xlings::closurecheck
