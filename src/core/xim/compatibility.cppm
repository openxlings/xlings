export module xlings.core.xim.compatibility;

import std;
import mcpplibs.xpkg;

export namespace xlings::xim {

struct TargetCompatibility {
    bool supported { true };
    std::string target;
    std::vector<std::string> supportedTargets;
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

TargetCompatibility check_target_compatibility(
    const mcpplibs::xpkg::Package& package,
    std::string_view os,
    std::string_view arch) {
    TargetCompatibility result;
    const auto normalizedArch = mcpplibs::xpkg::normalize_arch(arch);
    result.target = std::string(os) + "-" + normalizedArch;
    if (package.archs.empty()) return result;

    for (const auto& packageArch : package.archs) {
        const auto normalized = mcpplibs::xpkg::normalize_arch(packageArch);
        result.supportedTargets.push_back(std::string(os) + "-" + normalized);
        if (mcpplibs::xpkg::arch_matches(normalized, normalizedArch)) {
            result.supported = true;
            return result;
        }
    }
    result.supported = false;
    std::ranges::sort(result.supportedTargets);
    result.supportedTargets.erase(
        std::unique(result.supportedTargets.begin(), result.supportedTargets.end()),
        result.supportedTargets.end());
    return result;
}

std::string compatibility_error(std::string_view packageName,
                                const TargetCompatibility& compatibility) {
    std::string supported;
    for (const auto& target : compatibility.supportedTargets) {
        if (!supported.empty()) supported += ", ";
        supported += target;
    }
    return std::format(
        "E_UNSUPPORTED_TARGET: {} has no {} artifact; supported targets: {}",
        packageName, compatibility.target, supported);
}

}  // namespace xlings::xim
