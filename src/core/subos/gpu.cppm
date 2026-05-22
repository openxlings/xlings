// GPU device passthrough for bwrap sandbox (`--gpu`).
//
// bwrap's `--dev /dev` creates a fresh tmpfs at /dev with only a hard-
// coded minimal whitelist (null/zero/random/tty/...). Custom character
// devices on the host — notably NVIDIA's /dev/nvidia*, /dev/nvidia-uvm,
// /dev/nvidiactl, /dev/nvidia-modeset and DRM's /dev/dri/* — are NOT
// part of that whitelist and therefore invisible inside the sandbox.
//
// When the user passes `--gpu`, this module returns the argv fragment
// that re-exposes those nodes via `--dev-bind` (the only flag bwrap
// accepts for character devices), plus a read-only `/sys` bind so
// libcuda / nvml can enumerate GPUs via /sys/bus/pci/devices.
//
// Missing nodes are silently skipped — "--gpu on a host with no GPU"
// is allowed and just degrades to a /sys-only addition.
//
// Refs: .agents/docs/2026-05-22-subos-sandbox-gpu-passthrough.md
module;

#include <cctype>

export module xlings.core.subos.gpu;

import std;

export namespace xlings::subos::gpu {

// Build the argv fragment to append to a bwrap invocation when GPU
// passthrough is requested. `exists_fn` is injected for unit testing;
// production calls the no-arg overload which uses std::filesystem.
inline std::vector<std::string> passthrough_args(
    std::function<bool(const std::string&)> exists_fn)
{
    std::vector<std::string> out;

    auto add_dev = [&](const std::string& path) {
        if (exists_fn(path)) {
            out.push_back("--dev-bind");
            out.push_back(path);
            out.push_back(path);
        }
    };

    // NVIDIA control + UVM + modeset nodes (single-instance)
    add_dev("/dev/nvidiactl");
    add_dev("/dev/nvidia-uvm");
    add_dev("/dev/nvidia-uvm-tools");
    add_dev("/dev/nvidia-modeset");

    // Per-GPU character devices. 16 covers typical 8-GPU H100/A100 nodes
    // with headroom; bump if multi-tenant 16+-GPU boxes become common.
    for (int i = 0; i < 16; ++i) {
        add_dev("/dev/nvidia" + std::to_string(i));
    }

    // DRM (covers AMD/Intel render nodes too, plus NVIDIA display).
    add_dev("/dev/dri");

    // /sys is required for libcuda / nvml PCI enumeration. Unconditional
    // when --gpu is set — /sys always exists on a Linux userspace host.
    out.push_back("--ro-bind");
    out.push_back("/sys");
    out.push_back("/sys");

    return out;
}

inline std::vector<std::string> passthrough_args() {
    return passthrough_args([](const std::string& p) {
        std::error_code ec;
        return std::filesystem::exists(p, ec);
    });
}

} // namespace xlings::subos::gpu
