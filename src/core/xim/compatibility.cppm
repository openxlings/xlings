export module xlings.core.xim.compatibility;

import std;
import mcpplibs.xpkg;

// Whether a package can be installed on this OS and architecture.
//
// The obvious source for that answer is `package.archs`, and it is the wrong
// one. `archs` is a PACKAGE-level union while the download resources are
// grouped BY OS: `go.lua` declares `archs = {"x86_64"}` and its macOS entry is
// a `darwin-arm64` tarball, because on macOS the only artifact anyone ships is
// arm64. Refusing on the union alone rejects the very archive the recipe was
// about to fetch.
//
// It is also unreliable in a way that is not evenly distributed. `archs` went
// unenforced for the whole of spec V1, so it is widely under-declared: of the
// 99 recipes in xim-pkgindex that set it, the great majority say `{"x86_64"}`
// and mean "this is the arch I happened to test". Enforcing that field against
// every recipe turns "we do not know" into "no".
//
// So the question asked here is not "what does the author claim" but "what
// does this resource entry actually carry":
//
//   Strong  the entry enumerates its architectures -- a per-arch map, per-arch
//           checksums, or per-arch upstream aliases. The author is describing
//           artifacts, not intentions, and a missing arch is a real answer.
//   Open    the entry substitutes ${arch} into a URL, or is an XLINGS_RES
//           resource. Arch-parametric by construction; nothing here can bound
//           the set, and the download reports the truth.
//   Weak    a single artifact, and `package.archs` is the only clue. A
//           mismatch is an advisory, never a refusal.
//   None    no claim at all.
//
// This reproduces the intent of the `spec >= 2` gate it replaces -- V2 authors
// are the ones who write per-arch resources -- but keys off a fact in the
// entry rather than a version number the author promised to honour. An index
// that upgrades a recipe to per-arch artifacts gets the strict check without
// anyone shipping a new client.
export namespace xlings::xim {

enum class ArchEvidence {
    None,    // package declares no archs and the entry carries none
    Weak,    // single artifact; package-level `archs` is the only signal
    Open,    // ${arch} template or XLINGS_RES: parametric, set unbounded
    Strong,  // entry enumerates the architectures it ships
};

struct TargetCompatibility {
    bool supported { true };
    ArchEvidence evidence { ArchEvidence::None };
    std::string target;                        // "<os>-<arch>"
    std::vector<std::string> supportedTargets;
    // Set when package-level `archs` does not list this arch but the evidence
    // is too weak to refuse. Surfaced once, never fatal.
    std::string advisory;
};

std::string host_architecture() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

// The `xpm[os][version]` resource, with version aliases (`ref`) followed.
// nullptr when this OS or version has no entry -- the caller then has nothing
// but the package-level claim to go on.
const mcpplibs::xpkg::PlatformResource* find_entry(
    const mcpplibs::xpkg::Package& package,
    std::string_view os,
    std::string_view version) {
    const auto platform = package.xpm.entries.find(std::string(os));
    if (platform == package.xpm.entries.end()) return nullptr;
    std::string key(version);
    // `latest -> 1.2.3 -> ...`. Bounded so a recipe with a cyclic alias
    // returns "no entry" rather than spinning.
    for (int hop = 0; hop < 8; ++hop) {
        const auto found = platform->second.find(key);
        if (found == platform->second.end()) return nullptr;
        if (found->second.ref.empty()) return &found->second;
        key = found->second.ref;
    }
    return nullptr;
}

// Architectures this entry names outright, canonicalised.
std::set<std::string> entry_architectures(
    const mcpplibs::xpkg::PlatformResource& entry) {
    std::set<std::string> archs;
    for (const auto& pair : entry.archs) {
        archs.insert(mcpplibs::xpkg::normalize_arch(pair.first));
    }
    for (const auto& pair : entry.sha256_by_arch) {
        archs.insert(mcpplibs::xpkg::normalize_arch(pair.first));
    }
    for (const auto& pair : entry.arch_alias) {
        archs.insert(mcpplibs::xpkg::normalize_arch(pair.first));
    }
    return archs;
}

bool is_arch_templated(const mcpplibs::xpkg::PlatformResource& entry) {
    const auto templated = [](std::string_view url) {
        return url.find("${arch}") != std::string_view::npos
            || url.find("${arch_alias}") != std::string_view::npos;
    };
    if (templated(entry.url)) return true;
    for (const auto& pair : entry.mirrors) {
        if (templated(pair.second)) return true;
    }
    return false;
}

ArchEvidence classify_arch_evidence(
    const mcpplibs::xpkg::Package& package,
    const mcpplibs::xpkg::PlatformResource* entry) {
    if (entry != nullptr) {
        if (!entry_architectures(*entry).empty()) return ArchEvidence::Strong;
        // `res = true` is XLINGS_RES with per-arch checksums; when the loader
        // has not populated them the resource is still arch-parametric.
        if (entry->is_res || is_arch_templated(*entry)) {
            return ArchEvidence::Open;
        }
    }
    return package.archs.empty() ? ArchEvidence::None : ArchEvidence::Weak;
}

// `entry` is the resolved `xpm[os][version]` resource, with `ref` already
// followed. Pass nullptr when no entry could be selected -- the answer then
// falls back to the package-level claim, which can only ever warn.
TargetCompatibility check_target_compatibility(
    const mcpplibs::xpkg::Package& package,
    const mcpplibs::xpkg::PlatformResource* entry,
    std::string_view os,
    std::string_view arch) {
    TargetCompatibility result;
    const auto normalizedArch = mcpplibs::xpkg::normalize_arch(arch);
    result.target = std::string(os) + "-" + normalizedArch;
    result.evidence = classify_arch_evidence(package, entry);

    const auto label = [&](const std::string& value) {
        return std::string(os) + "-" + value;
    };

    switch (result.evidence) {
        case ArchEvidence::Strong: {
            const auto archs = entry_architectures(*entry);
            result.supported = archs.contains(normalizedArch);
            for (const auto& value : archs) {
                result.supportedTargets.push_back(label(value));
            }
            break;
        }
        case ArchEvidence::Weak: {
            const auto declared = std::ranges::any_of(package.archs,
                [&](const auto& packageArch) {
                    return mcpplibs::xpkg::arch_matches(packageArch,
                                                        normalizedArch);
                });
            // Deliberately still supported. This is the V1 under-declaration
            // case: a single artifact whose `archs` was never enforced and is
            // routinely wrong for exactly this OS.
            result.supported = true;
            if (!declared) {
                std::string declaredTargets;
                for (const auto& packageArch : package.archs) {
                    if (!declaredTargets.empty()) declaredTargets += ", ";
                    declaredTargets +=
                        label(mcpplibs::xpkg::normalize_arch(packageArch));
                }
                result.advisory = std::format(
                    "{}: recipe declares {} but ships one artifact for {}; "
                    "installing it. Add per-arch resources to the recipe to "
                    "make this checkable.",
                    package.name, declaredTargets, result.target);
            }
            break;
        }
        case ArchEvidence::Open:
        case ArchEvidence::None:
            break;
    }
    return result;
}

std::string compatibility_error(std::string_view packageName,
                                const TargetCompatibility& compatibility) {
    std::string supported;
    for (const auto& target : compatibility.supportedTargets) {
        if (!supported.empty()) supported += ", ";
        supported += target;
    }
    if (supported.empty()) supported = "none";
    return std::format(
        "E_UNSUPPORTED_TARGET: {} has no {} artifact; supported targets: {}",
        packageName, compatibility.target, supported);
}

}  // namespace xlings::xim
