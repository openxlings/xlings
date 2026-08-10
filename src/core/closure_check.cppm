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
//   rule E   a form-X EXECUTABLE's search path must be in DT_RPATH, not
//            DT_RUNPATH. DT_RUNPATH is consulted only for the object carrying
//            it, so an executable that dlopens two or three levels deep -- any
//            GL program does -- cannot reach the bottom however correct the
//            path is. Libraries are deliberately out of scope: the transitive
//            tag on a LIBRARY is measured harmful (xim-pkgindex#593).
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

// A form-X executable whose search path is in the non-transitive tag.
//
// rule E. DT_RUNPATH is consulted only for the object that carries it;
// DT_RPATH is consulted for every dlopen anywhere in the process. An
// executable that dlopens something three levels deep -- which is what any GL
// program does, since glvnd dlopens a vendor, the vendor dlopens its platform
// modules, and those dlopen their own dependencies -- cannot reach the bottom
// through DT_RUNPATH however correct the path itself is.
//
// This rule exists for the same reason as rule D, one layer up. Rule D's
// comment says it: a predicate that is right but "runs in exactly one place --
// a repository workflow the user's machine never sees". The TAG was in a worse
// place than that -- it lived in each recipe author's head. Measured on a real
// home: 1 of 73 installed executables carried DT_RPATH, 68 carried DT_RUNPATH,
// and 55 of those 68 already carried the correct PATH. The one that was right
// was right because that package's author found the problem independently and
// fixed it locally; nothing carried the finding to the other 68.
struct NonTransitiveTag {
    std::string elf;
    std::string rpath;      // the value, which is usually correct
};

// Which of the two search-path tags an ELF carries.
//
// Parsed here rather than shelled out because there is no tool to shell out
// TO: `patchelf --print-rpath` prints the value of whichever tag exists and
// does not say which, and `readelf` is not something a payload is guaranteed
// to have. It is also the right shape for a check that runs over every ELF of
// every install -- no process per file.
enum class PathTag { None, Rpath, Runpath };

inline PathTag path_tag_of(const fs::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return PathTag::None;
    std::string buf((std::istreambuf_iterator<char>(f)), {});
    if (buf.size() < 64) return PathTag::None;
    const auto* p = reinterpret_cast<const unsigned char*>(buf.data());
    if (!(p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F'))
        return PathTag::None;

    const bool is64 = p[4] == 2;
    // Little-endian only. Every target this check runs on is LE, and a wrong
    // guess here would misreport rather than fail loudly, so it declines
    // instead.
    if (p[5] != 1) return PathTag::None;

    auto rd = [&](std::size_t off, int width) -> std::uint64_t {
        if (off + static_cast<std::size_t>(width) > buf.size()) return 0;
        std::uint64_t v = 0;
        for (int i = width - 1; i >= 0; --i) v = (v << 8) | p[off + static_cast<std::size_t>(i)];
        return v;
    };

    const std::uint64_t phoff     = is64 ? rd(32, 8) : rd(28, 4);
    const std::uint64_t phentsize = is64 ? rd(54, 2) : rd(42, 2);
    const std::uint64_t phnum     = is64 ? rd(56, 2) : rd(44, 2);
    if (phoff == 0 || phentsize == 0) return PathTag::None;

    constexpr std::uint64_t PT_DYNAMIC = 2;
    constexpr std::uint64_t DT_NULL = 0, DT_RPATH = 15, DT_RUNPATH = 29;

    for (std::uint64_t i = 0; i < phnum; ++i) {
        const std::uint64_t ph = phoff + i * phentsize;
        if (rd(ph, 4) != PT_DYNAMIC) continue;
        const std::uint64_t dynoff = is64 ? rd(ph + 8, 8) : rd(ph + 4, 4);
        const int w = is64 ? 8 : 4;
        // The WHOLE dynamic section, not the first hit. When both tags are
        // present the loader IGNORES DT_RPATH and uses DT_RUNPATH, so an ELF
        // carrying both behaves as Runpath -- and returning whichever
        // happened to come first in the table would report the opposite on
        // half of them.
        bool sawRpath = false;
        for (std::uint64_t d = dynoff; d + 2 * static_cast<std::uint64_t>(w) <= buf.size();
             d += 2 * static_cast<std::uint64_t>(w)) {
            const std::uint64_t tag = rd(d, w);
            if (tag == DT_NULL) break;
            if (tag == DT_RUNPATH) return PathTag::Runpath;
            if (tag == DT_RPATH)   sawRpath = true;
        }
        return sawRpath ? PathTag::Rpath : PathTag::None;
    }
    return PathTag::None;
}

struct Report {
    int scannedElves = 0;
    int formXElves   = 0;
    std::vector<MissingSoname>  missing;   // one entry per (first elf, soname)
    std::optional<VersionFloor> floor;     // one offender is enough to say it
    // rule E. One offender names the payload: every executable in a payload is
    // stamped by the same pass, so listing all of them would repeat one fact.
    std::optional<NonTransitiveTag> nonTransitive;
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

        // rule E -- the same set rule A uses: this ELF has PT_INTERP (it is
        // an executable, not a library) and that interpreter is a payload
        // (form X). Libraries are deliberately out of scope: forcing the
        // transitive tag on a LIBRARY is measured harmful, because
        // transitivity runs downward too and the library's path then enters
        // every lookup beneath it (xim-pkgindex#593).
        if (!rep.nonTransitive
            && path_tag_of(path) == PathTag::Runpath) {
            const auto rp = run_lines(std::format("--print-rpath \"{}\"", path));
            rep.nonTransitive = NonTransitiveTag{
                .elf = path,
                .rpath = rp.empty() ? std::string{} : rp.back(),
            };
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

// Names the file, the tag, and the consequence -- because the consequence is
// the part nobody would guess. The path is usually right, so a message that
// only said "wrong tag" would read as pedantry rather than as the reason GL
// renders in software.
inline std::string describe_non_transitive(const NonTransitiveTag& t) {
    return std::format(
        "non-transitive search path: {} carries DT_RUNPATH, not DT_RPATH.\n"
        "  Its path ({}) is consulted for this binary's own libraries and for "
        "NOTHING it dlopens. A GL program reaches its driver through three "
        "levels of dlopen -- glvnd opens a vendor, the vendor opens its "
        "platform modules, those open their own dependencies -- so with this "
        "tag it silently renders in software however correct the path is. "
        "elfpatch stamps DT_RPATH on executables since libxpkg 0.0.57; a "
        "payload installed before that keeps the old tag until it is "
        "reinstalled. (rule E, warn-only)",
        fs::path(t.elf).filename().string(),
        t.rpath.empty() ? "empty" : t.rpath);
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
