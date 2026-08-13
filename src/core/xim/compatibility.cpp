module xlings.core.xim.compatibility;

import std;
import mcpplibs.xpkg;
import xlings.platform.target;

namespace xlings::xim {

std::string host_architecture() {
    return std::string(platform::build_arch());
}

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

}
