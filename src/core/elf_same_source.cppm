module;

export module xlings.core.elf_same_source;

import std;
import xlings.platform;
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

// `<store>/xpkgs/<provider>/<version>` for any path inside a payload, or
// empty when the path does not live in a store at all ($ORIGIN, a subos
// directory, a system path).
inline std::string payload_of(std::string_view p) {
    const auto marker = std::string("/xpkgs/");
    auto pos = p.rfind(marker);
    if (pos == std::string_view::npos) return {};
    auto rest = p.substr(pos + marker.size());
    // provider/version — two components, and both must be there.
    auto slash1 = rest.find('/');
    if (slash1 == std::string_view::npos) return {};
    auto slash2 = rest.find('/', slash1 + 1);
    auto len = (slash2 == std::string_view::npos) ? rest.size() : slash2;
    return std::string(p.substr(0, pos + marker.size() + len));
}

// The `<provider>` component of a payload directory.
inline std::string provider_of(std::string_view payload) {
    const auto marker = std::string("/xpkgs/");
    auto pos = payload.rfind(marker);
    if (pos == std::string_view::npos) return {};
    auto rest = payload.substr(pos + marker.size());
    auto slash = rest.find('/');
    return std::string(slash == std::string_view::npos ? rest
                                                       : rest.substr(0, slash));
}

struct Finding {
    bool        violated = false;
    std::string binary;
    std::string provider;
    std::string interpPayload;   // where the loader comes from
    std::string rpathPayload;    // where that provider's libdir points instead
};

// The invariant, over the two fields as read from one ELF.
//
// PASSES when: there is no interpreter (a shared library or a static binary —
// neither chooses a loader), the interpreter is not in a payload, or the
// RUNPATH names no entry from the same provider. A provider appearing several
// times passes if ANY entry is same-source, because the loader takes the first
// match and same-source being present is what matters.
inline Finding check(std::string_view binary,
                     std::string_view interp,
                     std::span<const std::string> rpathEntries) {
    Finding f;
    f.binary = std::string(binary);
    if (interp.empty()) return f;                 // no INTERP: nothing to pair
    f.interpPayload = payload_of(interp);
    if (f.interpPayload.empty()) return f;        // interpreter outside a store
    f.provider = provider_of(f.interpPayload);
    if (f.provider.empty()) return f;

    std::string mismatch;
    for (const auto& e : rpathEntries) {
        auto payload = payload_of(e);
        if (payload.empty()) continue;            // $ORIGIN, subos dir, ...
        if (provider_of(payload) != f.provider) continue;
        if (payload == f.interpPayload) return f; // same-source found: pass
        if (mismatch.empty()) mismatch = payload;
    }
    if (!mismatch.empty()) {
        f.violated = true;
        f.rpathPayload = std::move(mismatch);
    }
    return f;
}

// Human-readable, and deliberately naming both paths: the whole point is that
// the reader should not have to run LD_DEBUG to find out which two.
inline std::string describe(const Finding& f) {
    return std::format(
        "{}: its interpreter and its {} libraries come from different "
        "payloads\n    interpreter -> {}\n    RUNPATH     -> {}\n"
        "  A loader and the libc it loads are one build; pairing two is a "
        "crash before main with no useful message.",
        f.binary, f.provider, f.interpPayload, f.rpathPayload);
}

// The store root (`.../xpkgs`) containing a path, or empty.
//
// Derived from the scanned directory rather than from XLINGS_HOME: the two
// agree in the default configuration and diverge exactly when it matters --
// a shim rewrites XLINGS_HOME to the home that owns it, so a helper invoked
// through one sees the real home rather than the isolated one under test.
// The directory being scanned cannot lie about which store it is in.
inline std::string store_root_of(std::string_view p) {
    const auto marker = std::string("/xpkgs/");
    auto pos = p.rfind(marker);
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
                auto candidate = vit->path() / "bin" / "patchelf";
                if (!fs::is_regular_file(candidate, ec)) continue;
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

// Every ELF under DIR whose loader and libc come from different payloads.
//
// Reads the two fields with patchelf, which is the same tool that wrote them
// and is already a hard dependency of the install path. When patchelf is
// missing the scan yields nothing rather than failing the install: an
// unverifiable install is not the same as a bad one, and refusing here would
// turn a missing tool into a broken package manager.
//
// Shared with doctor deliberately. Report and repair have drifted three times
// in this repo; the shape it takes is a finding that fixing does not clear.
inline std::vector<Finding>
scan_payload(const std::filesystem::path& dir) {
    std::vector<Finding> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;

    auto trim = [](std::string v) {
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        return v;
    };
    const auto patchelf = locate_patchelf(dir);
    if (patchelf.empty()) return out;

    // run_command_capture merges stderr into stdout. On the payload path
    // `patchelf` is the real binary and prints only its answer; on the
    // fallback path it can still be one of our shims, which emit their own
    // bracketed advisory lines first, and taking the whole capture as the
    // value put a log message where a path belongs. The value is therefore the
    // last line that looks like one -- correct for both.
    auto run = [&](const std::string& args) -> std::string {
        auto [rc, o] = platform::run_command_capture(
            std::format("\"{}\" {}", patchelf, args));
        if (rc != 0) return {};
        std::string value;
        for (const auto lineView : std::views::split(o, '\n')) {
            auto line = trim(std::string(lineView.begin(), lineView.end()));
            if (line.empty() || line.front() == '[') continue;
            value = std::move(line);
        }
        return value;
    };

    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || it->is_symlink(ec)) continue;
        const auto path = it->path().string();

        // Cheap gate first: only real ELFs are worth two patchelf calls.
        std::ifstream f(path, std::ios::binary);
        char magic[4] = {};
        if (!f.read(magic, 4)) continue;
        if (!(magic[0] == '\x7f' && magic[1] == 'E'
              && magic[2] == 'L' && magic[3] == 'F')) continue;

        const auto interp = run(std::format("--print-interpreter \"{}\"", path));
        if (interp.empty()) continue;   // library or static: nothing to pair

        const auto rpath = run(std::format("--print-rpath \"{}\"", path));
        std::vector<std::string> entries;
        for (const auto part : std::views::split(rpath, ':')) {
            std::string e(part.begin(), part.end());
            if (!e.empty()) entries.push_back(std::move(e));
        }

        auto finding = check(path, interp, entries);
        if (finding.violated) out.push_back(std::move(finding));
    }
    return out;
}

} // namespace xlings::elfcheck
