module;

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

export module xlings.platform.target;

import std;

// "Which OS and architecture is this?" -- asked once, in one place.
//
// The question has TWO answers and the codebase had been giving one, under a
// name that claimed the other. `xim::host_architecture()` was a chain of
// `#if defined(__aarch64__)`, which is the architecture this binary was
// COMPILED for, not the one the machine has. They differ whenever emulation is
// involved: an x86_64 build under Rosetta 2 on Apple Silicon, under WOW64 on
// Windows ARM, under qemu-user or box64 on Linux.
//
// Both answers are needed, for different questions:
//
//   build()  the ABI of this process. Decides WHICH ARTIFACT TO INSTALL --
//            an x86_64 xlings must fetch x86_64 packages, because they are
//            what it can dynamically link against and exec. Getting this from
//            the hardware would install aarch64 binaries into an x86_64
//            process's world.
//
//   host()   what the kernel says the machine is. Decides WHETHER TO SAY
//            SOMETHING -- a user running the emulated build on native
//            hardware is paying an emulation tax for no reason and would like
//            to know a native release exists.
//
// Naming them apart is the point. Every `#if` that answers this question in
// its own file is a place the two can be confused again.
export namespace xlings::platform {

struct Target {
    std::string os;    // linux | macosx | windows
    std::string arch;  // x86_64 | aarch64 | arm64 (macOS) | x86 | unknown

    std::string str() const { return os + "-" + arch; }
};

// Written out rather than `= default`: a defaulted hidden-friend comparison in
// a module interface ICEs gcc 16.1 (segfault in the modules mapper), the same
// class of failure that keeps the xvm test files merged.
inline bool operator==(const Target& lhs, const Target& rhs) {
    return lhs.os == rhs.os && lhs.arch == rhs.arch;
}

// Per-OS arch spelling, matching release asset naming: Linux and Windows use
// the GNU/uname-m token `aarch64`, Apple uses its own `arm64`.
constexpr std::string_view build_os() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macosx";
#else
    return "linux";
#endif
}

constexpr std::string_view build_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
  #if defined(__APPLE__)
    return "arm64";
  #else
    return "aarch64";
  #endif
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

// The ABI of this process. Compile-time, and deliberately so.
const Target& build();

// What the kernel reports. Falls back to build() when it cannot be read, so a
// caller never has to handle "unknown machine" separately -- the interesting
// case is only ever "these two disagree".
const Target& host();

// True when this binary is running on hardware it was not built for -- Rosetta
// 2, WOW64, qemu-user, box64. Not an error: the emulated build works, and
// switching to a native one is the user's call.
bool is_emulated();

}  // namespace xlings::platform
