module xlings.core.xim.compatibility;

import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.compat;
import xlings.platform.target;

namespace xlings::xim {

std::string host_architecture() {
    return std::string(platform::build_arch());
}

const mcpplibs::xpkg::PlatformResource* find_entry(
    const mcpplibs::xpkg::Package& package,
    std::string_view os,
    std::string_view version) {
    return mcpplibs::xpkg::find_entry(package.xpm, os, version);
}

bool declares_no_download_source(
    const mcpplibs::xpkg::Package& package,
    const mcpplibs::xpkg::PlatformResource* entry,
    std::string_view os) {
    return mcpplibs::xpkg::declares_no_download_source(package.xpm, entry, os);
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
            // A template (type = "subos": an edition) ships no artifact: its
            // `archs` is the only statement of where it runs, and the truth --
            // an aarch64 host given an x86_64 edition was half-made, then
            // failed on its first package.
            if (package.type == mcpplibs::xpkg::PackageType::Subos) {
                result.supported = declared;
                for (const auto& packageArch : package.archs)
                    result.supportedTargets.push_back(label(mcpplibs::xpkg::normalize_arch(packageArch)));
                break;
            }
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
