module;

export module xlings.core.elf_same_source;

import std;
import xlings.platform;
import xlings.core.elfread;
import xlings.core.log;
import xlings.core.version_order;

// A binary's loader and its libc must come from the same payload.
//
// They are not two dependencies that happen to be related: `ld.so` and
// `libc.so.6` are two halves of one build, talking to each other over symbols
// versioned GLIBC_PRIVATE, which promise nothing across versions. Pair a 2.44
// libc with a 2.39 loader and the process dies before `main` with
//
//     undefined symbol: __pointer_chk_guard, version GLIBC_PRIVATE
//
// which names neither version nor package. This module states the invariant
// that makes that unreachable, and it exists as a module rather than as two
// copies because the pair "doctor reports it / --fix acts on it" has drifted
// three times in this repo, each time showing up as a finding that repairing
// does not clear.
//
// Spec, including the six edge cases:
// .agents/docs/2026-08-05-dependency-resolution-single-source.md §6.4
export namespace xlings::elfcheck {

namespace fs = std::filesystem;

// Store coordinates may arrive from std::filesystem using either native path
// separator. Normalize only for parsing; callers keep the original spelling
// for diagnostics and for opening the path on its host platform.
inline std::string store_parse_path_(std::string_view p) {
    std::string normalized(p);
    for (auto& ch : normalized) {
        if (ch == '\\') ch = '/';
    }
    return normalized;
}

// `<store>/xpkgs/<provider>/<version>` for any path inside a payload, or
// empty when the path does not live in a store at all ($ORIGIN, a subos
// directory, a system path).
inline std::string payload_of(std::string_view p) {
    constexpr std::string_view marker = "/xpkgs/";
    const auto normalized = store_parse_path_(p);
    const std::string_view parsed(normalized);
    auto pos = parsed.rfind(marker);
    if (pos == std::string_view::npos) return {};
    auto rest = parsed.substr(pos + marker.size());
    // provider/version — two components, and both must be there.
    auto slash1 = rest.find('/');
    if (slash1 == std::string_view::npos) return {};
    auto slash2 = rest.find('/', slash1 + 1);
    auto len = (slash2 == std::string_view::npos) ? rest.size() : slash2;
    return std::string(p.substr(0, pos + marker.size() + len));
}

// The `<provider>` component of a payload directory.
inline std::string provider_of(std::string_view payload) {
    constexpr std::string_view marker = "/xpkgs/";
    const auto normalized = store_parse_path_(payload);
    const std::string_view parsed(normalized);
    auto pos = parsed.rfind(marker);
    if (pos == std::string_view::npos) return {};
    auto rest = parsed.substr(pos + marker.size());
    auto slash = rest.find('/');
    return std::string(slash == std::string_view::npos ? rest
                                                       : rest.substr(0, slash));
}

// Filesystem aliases are not different runtime builds. macOS exposes ordinary
// paths through aliases such as /var -> /private/var and the Data volume; a
// SubOS may likewise reach one payload through a symlinked store root. Compare
// existing payload directories by their canonical identity while preserving
// the original paths in diagnostics.
inline std::string payload_identity_(std::string_view payload) {
    if (payload.empty()) return {};
    fs::path path(payload);
    std::error_code ec;
    auto resolved = fs::canonical(path, ec);
    return ec ? path.lexically_normal().string() : resolved.string();
}

struct Finding {
    enum class Reason {
        None,
        PayloadMismatch,
        HostLoaderPayloadCore,
    };

    bool        violated = false;
    Reason      reason = Reason::None;
    std::string binary;
    std::string interpreter;
    std::string provider;
    std::string interpPayload;   // where the loader comes from
    std::string rpathPayload;    // where that provider's libdir points instead
    std::string offendingPath;   // resolved RUNPATH directory inspected
    std::string corePath;        // resolved libc/loader entry inside it
};

inline bool is_core_runtime_filename_(std::string_view name) {
    if (name == "libc.so.6") return true;
    const auto loader_name = [&](std::string_view prefix) {
        return name.starts_with(prefix)
            && name.find(".so.", prefix.size()) != std::string_view::npos;
    };
    return loader_name("ld-linux-") || loader_name("ld-musl-");
}

// glibc and musl are not each other's core runtime.
//
// "A loader and the libc it loads are one build" is a statement about ONE
// runtime family. A glibc-interpreted binary asks for `libc.so.6`, which a musl
// libdir does not contain under any name -- so a musl loader sitting on its
// RUNPATH is not a libc it could ever load, and pairing them is not the defect
// this rule is about.
//
// Without this, any binary produced by one toolchain while linking the other's
// runtime trips the guard. That is not hypothetical: a musl cross-compiler
// bakes its own libdir into RUNPATH, and the resulting glibc binaries then
// carry a musl loader on a path they never resolve through.
enum class CoreFamily { Unknown, Glibc, Musl };

inline CoreFamily core_family_of_(std::string_view filename) {
    if (filename == "libc.so.6" || filename.starts_with("ld-linux-")) {
        return CoreFamily::Glibc;
    }
    if (filename.starts_with("ld-musl-")
        || filename.starts_with("libc.musl-")) {
        return CoreFamily::Musl;
    }
    return CoreFamily::Unknown;
}

inline CoreFamily core_family_of_path_(const fs::path& path) {
    return core_family_of_(path.filename().string());
}

// A core runtime file, and which family it belongs to.
//
// The family comes from the DIRECTORY ENTRY's name, not the resolved target's.
// On musl the loader and the libc are ONE FILE: a real payload ships
// `ld-musl-x86_64.so.1` as a symlink to `libc.so`. Resolve it and the name that
// declared the family is gone -- `libc.so` matches neither the glibc nor the
// musl pattern, so it classifies as Unknown, and the family guard's deliberate
// "an unknown family still compares" rule then fires on exactly the case the
// guard exists to exclude.
//
// Measured on a real home: every glibc binary whose RUNPATH reached a subos lib
// farm containing musl was reported as a loader/libc split, and the
// install-time guard REFUSED the install.
//
// The resolved path is still what identifies the payload and what the error
// message must name; only the family question is answered by the entry.
struct CoreSource {
    fs::path   source;   // resolved -- the payload identity, and what we print
    CoreFamily family;   // from the entry name, which is what declares it
};

// Every core runtime file in a directory, resolved through symlinks.
// Inspecting all of them is essential: a SubOS libdir can itself be split,
// with its loader link pointing into payload A and libc.so.6 into payload B.
// directory_iterator order is unspecified, so choosing one entry makes the
// verdict depend on filesystem history.
inline std::vector<CoreSource> core_runtime_sources_(const fs::path& dir) {
    std::vector<CoreSource> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return out;
    for (auto it = fs::directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const auto entryName = it->path().filename().string();
        if (!is_core_runtime_filename_(entryName)) continue;

        std::error_code rec;
        auto resolved = fs::weakly_canonical(it->path(), rec);
        auto source = rec ? it->path().lexically_normal() : std::move(resolved);

        // Entry name first. Fall back to the resolved name only when the entry
        // itself says nothing -- which `is_core_runtime_filename_` makes
        // impossible today, but the fallback keeps the two functions from
        // having to agree by accident.
        auto family = core_family_of_(entryName);
        if (family == CoreFamily::Unknown) {
            family = core_family_of_path_(source);
        }
        out.push_back({ std::move(source), family });
    }
    std::ranges::sort(out, {}, [](const CoreSource& e) {
        return e.source.string();
    });
    out.erase(std::ranges::unique(out, {}, [](const CoreSource& e) {
                  return e.source.string();
              }).begin(),
              out.end());
    return out;
}

inline bool directory_contains_core_runtime(const fs::path& dir) {
    return !core_runtime_sources_(dir).empty();
}

using CoreDirProbe = std::function<bool(const fs::path&)>;

inline fs::path resolve_rpath_entry_(std::string_view binary,
                                    std::string_view entry) {
    std::string expanded(entry);
    const auto origin = fs::path(binary).parent_path().string();
    const auto replace_all = [&](std::string_view token) {
        std::size_t pos = 0;
        while ((pos = expanded.find(token, pos)) != std::string::npos) {
            expanded.replace(pos, token.size(), origin);
            pos += origin.size();
        }
    };
    replace_all("${ORIGIN}");
    replace_all("$ORIGIN");

    fs::path path(expanded);
    if (path.is_relative()) path = fs::path(binary).parent_path() / path;
    std::error_code ec;
    // canonical() deliberately requires the complete path to exist. For
    // synthetic/unmaterialized entries, retaining the lexical spelling avoids
    // canonicalising only an OS alias prefix and then comparing two spellings
    // of the same nonexistent path.
    auto resolved = fs::canonical(path, ec);
    return ec ? path.lexically_normal() : resolved;
}

// The invariant, over the two fields as read from one ELF.
//
// PASSES when: there is no interpreter (a shared library or a static binary —
// neither chooses a loader), or no RUNPATH directory is proven to contain a
// core runtime. Every proven core-runtime entry must agree: a later directory
// may supply the loader or another core component even when libc.so.6 was
// found earlier, so one same-source entry cannot wash out another mismatch.
inline Finding check(std::string_view binary,
                     std::string_view interp,
                     std::span<const std::string> rpathEntries,
                     const CoreDirProbe& coreDirProbe) {
    Finding f;
    f.binary = std::string(binary);
    f.interpreter = std::string(interp);
    if (interp.empty()) return f;                 // no INTERP: nothing to pair
    f.interpPayload = payload_of(interp);
    if (f.interpPayload.empty()) {
        // A PT_INTERP inside a SubOS lib dir is OUR loader reached through a
        // view, not the host's: that directory is a symlink farm into the
        // payload, and payload_of is documented to return empty for it. The
        // RUNPATH side already resolves through symlinks (core_runtime_sources_)
        // for exactly this reason; doing it on only one side classified every
        // migrated binary as "host loader with a payload libc".
        //
        // Measured on a real home: this accounted for 143 of 145 findings the
        // reverse rule produced, all of them a payload loader and a payload
        // libc from the same payload. Resolving both sides is what makes the
        // rule mean what it says.
        std::error_code ec;
        const auto resolved = fs::weakly_canonical(fs::path(interp), ec);
        if (!ec) f.interpPayload = payload_of(resolved.string());
    }
    f.provider = provider_of(f.interpPayload);
    const auto interpIdentity = payload_identity_(f.interpPayload);

    for (const auto& e : rpathEntries) {
        const auto dir = resolve_rpath_entry_(binary, e);
        if (!coreDirProbe(dir)) continue;

        // A SubOS libdir commonly contains several symlinks into shared
        // payloads. Preserve every resolved source so a loader from A cannot
        // hide a libc from B merely by being enumerated first.
        auto sources = core_runtime_sources_(dir);
        if (sources.empty()) {
            // Injectable probes let unit tests exercise synthetic store paths
            // without building real runtimes. The directory is the identity
            // only in that path; production probes always produce sources.
            const auto payload = payload_of(dir.string());
            // Unknown family on purpose: this is the injected-probe path, and
            // a synthetic directory declares nothing. Unknown keeps the
            // fail-loud comparison the guard was written with.
            if (!payload.empty()) {
                sources.push_back({ dir, CoreFamily::Unknown });
            }
        }

        const auto interpFamily = core_family_of_path_(fs::path(interp));
        for (const auto& entry : sources) {
            const auto& source = entry.source;
            // Only compare within one runtime family. An unknown family on
            // either side still compares, so a payload this does not recognise
            // is never silently excused.
            const auto sourceFamily = entry.family;
            if (interpFamily != CoreFamily::Unknown
                && sourceFamily != CoreFamily::Unknown
                && interpFamily != sourceFamily) {
                continue;
            }
            auto payload = payload_of(source.string());

            if (f.interpPayload.empty()) {
                // The reverse rule is deliberately narrower: a host loader
                // may search host core directories, but never a payload core.
                if (payload.empty()) continue;
                f.violated = true;
                f.reason = Finding::Reason::HostLoaderPayloadCore;
                f.rpathPayload = std::move(payload);
                f.offendingPath = dir.string();
                f.corePath = source.string();
                return f;
            }

            // A copied core file outside the store is not proven same-source
            // either. Use its resolved path as the identity so the error
            // names what must be repaired.
            const auto sourceIdentity = payload.empty()
                ? source.string() : payload;
            if (!payload.empty()
                && payload_identity_(payload) == interpIdentity) {
                continue;
            }
            f.violated = true;
            f.reason = Finding::Reason::PayloadMismatch;
            f.rpathPayload = sourceIdentity;
            f.offendingPath = dir.string();
            f.corePath = source.string();
            return f;
        }
    }
    return f;
}

inline Finding check(std::string_view binary,
                     std::string_view interp,
                     std::span<const std::string> rpathEntries) {
    return check(binary, interp, rpathEntries, directory_contains_core_runtime);
}

// Human-readable, and deliberately naming both paths: the whole point is that
// the reader should not have to run LD_DEBUG to find out which two.
inline std::string describe(const Finding& f) {
    if (f.reason == Finding::Reason::HostLoaderPayloadCore) {
        return std::format(
            "{}: its host interpreter would search a payload core runtime\n"
            "    interpreter -> {}\n    RUNPATH     -> {}\n"
            "    core file   -> {}\n    payload     -> {}\n"
            "  A host loader and a payload libc are different builds; this "
            "can crash before main with no useful message.",
            f.binary, f.interpreter, f.offendingPath,
            f.corePath.empty() ? f.offendingPath : f.corePath,
            f.rpathPayload);
    }
    return std::format(
        "{}: its interpreter and its {} libraries come from different "
        "payloads\n    interpreter -> {}\n    RUNPATH     -> {}\n"
        "    core file   -> {}\n    payloads    -> {} vs {}\n"
        "  A loader and the libc it loads are one build; pairing two is a "
        "crash before main with no useful message.",
        f.binary, f.provider, f.interpreter,
        f.offendingPath.empty() ? f.rpathPayload : f.offendingPath,
        f.corePath.empty() ? f.rpathPayload : f.corePath,
        f.interpPayload, f.rpathPayload);
}

// The store root (`.../xpkgs`) containing a path, or empty.
//
// Derived from the scanned directory rather than from XLINGS_HOME: the two
// agree in the default configuration and diverge exactly when it matters --
// a shim rewrites XLINGS_HOME to the home that owns it, so a helper invoked
// through one sees the real home rather than the isolated one under test.
// The directory being scanned cannot lie about which store it is in.
// Normalization is for FINDING the split point only. The result is sliced out
// of the caller's own spelling, exactly as payload_of does -- the two are
// compared against each other, so they have to make the same choice. Returning
// a normalized slice from one and an original slice from the other makes a
// single store two different strings on Windows, and that comparison fails
// silently and only there. `PayloadOf.AcceptsWindowsSeparators` pins it.
inline std::string store_root_of(std::string_view p) {
    constexpr std::string_view marker = "/xpkgs/";
    const auto normalized = store_parse_path_(p);
    auto pos = std::string_view(normalized).rfind(marker);
    if (pos == std::string_view::npos) return {};
    return std::string(p.substr(0, pos + marker.size() - 1));
}

// The patchelf that stamped these fields, resolved from the payload.
//
// R6 (see .agents/docs/2026-08-06-subos-architecture-proposal.md §1.5): when
// xlings itself needs a tool it resolves the payload, never the view. This
// used to be `command -v patchelf`, which inside a subos session hits one of
// our shims -- re-entering xlings and anchoring to whichever home owns it --
// and outside one hits whatever `/usr/bin/patchelf` the machine happens to
// have. patchelf versions differ in how they grow the dynamic segment and in
// `--force-rpath` semantics, so a reader that is not the writer can report a
// mismatch that does not exist, or miss one that does.
//
// Highest installed version, which is the same choice libxpkg's resolver
// makes for an unpinned dependency -- these two have to agree, and agreeing by
// construction is the only way that holds.
//
// Returns empty when there is no payload and no host tool. The caller treats
// that as "unverifiable", not as "clean".
inline std::string locate_patchelf(const std::filesystem::path& scanned) {
    namespace fs = std::filesystem;
    std::error_code ec;

    auto store = store_root_of(scanned.string());
    if (!store.empty()) {
        std::string bestVer, bestPath;
        for (auto it = fs::directory_iterator(fs::path(store), ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const auto name = it->path().filename().string();
            if (name != "patchelf" && !name.ends_with("-x-patchelf")) continue;
            std::error_code vec;
            for (auto vit = fs::directory_iterator(it->path(), vec);
                 !vec && vit != fs::directory_iterator(); vit.increment(vec)) {
                // A separate error_code: sharing the outer loop's would let a
                // single unreadable entry set it and end the iteration, which
                // reads as "no payload here" and silently falls through to the
                // host tool.
                std::error_code fec;
                auto candidate = vit->path() / "bin" / "patchelf";
                if (!fs::is_regular_file(candidate, fec)) continue;
                const auto ver = vit->path().filename().string();
                if (bestVer.empty()
                    || version_order::compare(ver, bestVer) > 0) {
                    bestVer = ver;
                    bestPath = candidate.string();
                }
            }
        }
        if (!bestPath.empty()) return bestPath;
    }

    // No payload in this store. Fall back so that an older home stays
    // verifiable, and say so -- landing here means the fields are being read
    // by a tool that did not write them.
    auto [rc, out] = platform::run_command_capture(
        "command -v patchelf 2>/dev/null");
    if (rc != 0) return {};
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    if (!out.empty()) {
        log::debug("elfcheck: no patchelf payload under {}; reading with {}",
                   store.empty() ? scanned.string() : store, out);
    }
    return out;
}

// A payload that is really another home's store.
//
// `xim-x-mcpp/<v>/registry/data/xpkgs/...` is a complete xlings store carried
// inside an mcpp payload -- mcpp installs its own packages there. Walking into
// it audits a store this home does not own, cannot repair, and re-audits once
// per installed mcpp version. Measured on a real home: 4,845 ELFs and 26.93 GB,
// 48% of the entire audit's bytes, none of it actionable.
//
// Keyed on the directory NAME plus a marker rather than on the full path, so it
// holds for any package that vendors a store, not only for mcpp.
inline bool is_nested_store_root_(const std::filesystem::path& dir) {
    if (dir.filename() != "xpkgs") return false;
    const auto parent = dir.parent_path();
    if (parent.filename() != "data") return false;
    // `<something>/data/xpkgs` is the store layout. Requiring the grandparent
    // to exist keeps this from matching a payload that merely has a `data`
    // directory at its root -- which IS the shape of our own store root, and
    // is why the caller never passes one.
    return !parent.parent_path().empty();
}

// Every ELF under DIR whose loader and libc come from different payloads.
//
// Reads the two fields in-process (see xlings.core.elfread), which is what
// makes this affordable to run over a whole store: the previous reader forked
// `patchelf` twice per ELF through a shell and patchelf read each file in full,
// so a single pass over a real 73 GB home moved 56.54 GB and took two minutes.
//
// The fields are the same fields, deliberately: elfread reproduces patchelf's
// RUNPATH-over-RPATH precedence and its "no .interp means no answer" behaviour,
// and tests/unit/test_elfread.cpp cross-checks the two readers against real
// files. patchelf remains the writer of these fields; it is no longer required
// to be installed in order to READ them, which used to make a missing tool
// indistinguishable from a clean scan.
//
// Shared with doctor deliberately. Report and repair have drifted three times
// in this repo; the shape it takes is a finding that fixing does not clear.
inline std::vector<Finding>
scan_payload(const std::filesystem::path& dir) {
    std::vector<Finding> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;

    auto it = std::filesystem::recursive_directory_iterator(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;

        if (it->is_directory(ec) && is_nested_store_root_(it->path())) {
            // Announced, not silent. A scan that quietly covers less than it
            // says is this repository's recurring failure -- "never happened"
            // and "succeeded" printing the same thing.
            log::debug("elfcheck: skipping nested store {}", it->path().string());
            it.disable_recursion_pending();
            continue;
        }

        if (!it->is_regular_file(ec) || it->is_symlink(ec)) continue;

        auto info = elfread::read(it->path());
        if (!info) continue;                    // not an ELF we can read
        if (info->interpreter.empty()) continue; // library or static: nothing to pair

        auto finding = check(it->path().string(), info->interpreter,
                             info->searchPaths);
        if (finding.violated) out.push_back(std::move(finding));
    }
    return out;
}

// One command's worth of scan results, keyed on whether the payload changed.
//
// WHY A CACHE AT ALL
//
// `self doctor --fix` re-detects after every repair phase -- six times
// unconditionally, eight when it prunes -- because the payload repairs run in
// SUBPROCESSES that write the state file, and a stale in-process copy makes a
// successful cure read as a failure. That re-detection is correct and must
// stay. What is wasteful is that it re-walks every payload's bytes, including
// the overwhelming majority that no phase could possibly have touched.
//
// WHY NOT JUST SKIP THE LATE PASSES
//
// The obvious alternative is to re-audit only after the phases that can change
// payload bytes (the reinstall ladder) and skip it after the metadata-only
// ones. That is faster still and it is a WORSE trade: it is a claim about what
// each phase does, enforced nowhere, and if a phase ever gains a side effect
// the audit stops seeing it and says nothing. This cache makes no such claim.
// Every pass still asks about every payload; a payload whose key moved is
// re-read no matter which phase moved it.
//
// THE KEY, and why the caller supplies part of it
//
// A payload's install stamp is rewritten by every install, and it is written
// through a rename -- which changes the containing DIRECTORY's write time as
// well. So the directory's own mtime already moves on any install, extraction
// or repair. The stamp is included on top of it because rewriting an existing
// file in place would leave the directory untouched, and elfpatch does exactly
// that during an install.
//
// The stamp's FILENAME belongs to xim (payload.cppm owns the constant), and
// this module sits below xim. Rather than spell it a second time here -- the
// shape that has already cost this repository a reporter and a repairer
// disagreeing about the same rule -- the caller passes the stamp path in.
//
// PROCESS-SCOPED ON PURPOSE. Persisting it across runs would make it a claim
// about what happens between commands -- a user running `patchelf` by hand, an
// installer from another home -- which nothing here can see. One command is
// exactly the window in which "we are the only writer" is true.
class PayloadScanCache {
public:
    // `stampName` is the install marker's filename, e.g. xim's
    // kPayloadStampFile. Empty means "no marker to consult", which degrades to
    // keying on the directory alone rather than to caching nothing.
    explicit PayloadScanCache(std::string stampName)
        : stampName_(std::move(stampName)) {}

    std::vector<Finding> scan(const std::filesystem::path& dir) {
        const auto key = key_of_(dir);
        const auto path = dir.string();
        if (auto it = entries_.find(path); it != entries_.end()) {
            if (it->second.key == key) {
                ++hits_;
                return it->second.findings;
            }
        }
        ++misses_;
        auto findings = scan_payload(dir);
        entries_.insert_or_assign(path, Entry{key, findings});
        return findings;
    }

    [[nodiscard]] std::size_t hits() const { return hits_; }
    [[nodiscard]] std::size_t misses() const { return misses_; }

private:
    struct Key {
        std::int64_t stampTime { 0 };
        std::uint64_t stampSize { 0 };
        std::int64_t dirTime { 0 };
        bool operator==(const Key&) const = default;
    };

    struct Entry {
        Key key;
        std::vector<Finding> findings;
    };

    static std::int64_t write_time_(const std::filesystem::path& p) {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(p, ec);
        if (ec) return 0;
        return t.time_since_epoch().count();
    }

    Key key_of_(const std::filesystem::path& dir) const {
        Key key;
        key.dirTime = write_time_(dir);
        if (stampName_.empty()) return key;
        const auto stamp = dir / stampName_;
        std::error_code ec;
        if (std::filesystem::is_regular_file(stamp, ec)) {
            key.stampTime = write_time_(stamp);
            const auto size = std::filesystem::file_size(stamp, ec);
            key.stampSize = ec ? 0 : size;
        }
        return key;
    }

    std::string stampName_;
    std::map<std::string, Entry> entries_;
    std::size_t hits_ { 0 };
    std::size_t misses_ { 0 };
};

} // namespace xlings::elfcheck
