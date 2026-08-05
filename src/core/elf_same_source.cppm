module;

export module xlings.core.elf_same_source;

import std;
import xlings.platform;
import xlings.core.log;

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

    // `command -v`, the same way the rest of this file locates tools. PATH
    // already has the subos bin dir prepended by the caller.
    auto trim = [](std::string v) {
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        return v;
    };
    auto [rcWhich, whichOut] =
        platform::run_command_capture("command -v patchelf 2>/dev/null");
    if (rcWhich != 0) return out;
    const auto patchelf = trim(whichOut);
    if (patchelf.empty()) return out;

    // run_command_capture merges stderr into stdout, and `patchelf` here is
    // usually an xlings shim that prints its own advisory lines. Taking the
    // whole capture as the value put a log message where a path belongs. The
    // value is the last line that looks like one; everything a shim emits is
    // bracketed log output, and it comes first.
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
