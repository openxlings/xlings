module;

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

module xlings.platform.target;

import std;

namespace xlings::platform {

const Target& build() {
    static const Target value{std::string(build_os()), std::string(build_arch())};
    return value;
}

const Target& host() {
    static const Target value = [] {
        Target target{std::string(build_os()), std::string(build_arch())};
#if defined(_WIN32)
        SYSTEM_INFO info{};
        ::GetNativeSystemInfo(&info);
        switch (info.wProcessorArchitecture) {
            case PROCESSOR_ARCHITECTURE_ARM64: target.arch = "aarch64"; break;
            case PROCESSOR_ARCHITECTURE_AMD64: target.arch = "x86_64"; break;
            case PROCESSOR_ARCHITECTURE_INTEL: target.arch = "x86"; break;
            default: break;
        }
#else
        struct utsname info{};
        if (::uname(&info) == 0) {
            const std::string_view machine{info.machine};
            if (machine == "aarch64" || machine == "arm64") {
                target.arch = build_os() == "macosx" ? "arm64" : "aarch64";
            } else if (machine == "x86_64" || machine == "amd64") {
                target.arch = "x86_64";
            } else if (machine == "i386" || machine == "i686") {
                target.arch = "x86";
            }
        }
#endif
        return target;
    }();
    return value;
}

bool is_emulated() { return build().arch != host().arch; }

}


// ── out-of-line class members ──────────────────────────────────

namespace xlings::platform {

std::string Target::str() const { return os + "-" + arch; }

} // namespace xlings::platform
