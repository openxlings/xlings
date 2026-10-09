module xlings.subos.tools;

import std;
import xlings.platform;
import xlings.subos.home_view;
import xlings.subos.ports;

namespace xlings::subos::tools {

namespace {

// Where each tool may come from, in order. `kRootOwned` only for bwrap: what
// `self doctor --isolation --fix` installs, root's and writable by nobody
// else, with an AppArmor profile that grants it user namespaces.
struct Location {
    Source source;
    std::string_view where;   // a path (RootOwned, Host), a bin dir name (Payload)
};
struct Entry {
    Tool tool;
    std::string_view xpkgs_dir;   // data/xpkgs/<dir>
    std::vector<Location> order;
};

const std::vector<Entry>& table() {
    static const std::vector<Entry> t{
        {{"bwrap", "bwrap"}, "xim-x-bwrap",
         {{Source::RootOwned, "/usr/lib/xlings/bwrap"},
          {Source::Host, "/usr/bin/bwrap"}, {Source::Host, "/usr/local/bin/bwrap"},
          {Source::Payload, "bin"}}},
        {{"proot", "proot"}, "xim-x-proot",
         {{Source::Payload, "bin"}, {Source::Runtimedir, "proot"},
          {Source::Host, "/usr/bin/proot"}, {Source::Host, "/usr/local/bin/proot"}}},
        {{"pasta", "passt"}, "xim-x-passt",
         {{Source::Payload, "bin"}, {Source::Host, "/usr/bin/pasta"}, {Source::Host, "/usr/local/bin/pasta"}}},
        // The machine's copy, for its copy-on-write flags (--reflink,
        // APFS clonefile); a SubOS fork falls back to std::filesystem.
        {{"cp", ""}, "",
         {{Source::Host, "/bin/cp"}, {Source::Host, "/usr/bin/cp"}}},
        // The VMM the vz carrier drives (part 3 §5.4): a signed helper with
        // Virtualization.framework's entitlement, as a payload.
        {{"xlings-vm", "xlings-vm"}, "xim-x-xlings-vm",
         {{Source::Payload, "bin"}}},
        {{"mkfs.ext4", "e2fsprogs"}, "xim-x-e2fsprogs",
         {{Source::Payload, "sbin"}, {Source::Payload, "bin"},
          {Source::Host, "/usr/sbin/mkfs.ext4"}, {Source::Host, "/sbin/mkfs.ext4"},
          {Source::Host, "/usr/bin/mkfs.ext4"}}},
        // Where a proxy's exit is (identity tz = proxy, Luban design §C4):
        // asked THROUGH the proxy, so the answer names the exit and the
        // question does not leave from here.
        {{"curl", "curl"}, "xim-x-curl",
         {{Source::Payload, "bin"}, {Source::Host, "/usr/bin/curl"}, {Source::Host, "/usr/local/bin/curl"}}},
        // A Luban image, made and tried (Luban design §A9, §B3.5).
        {{"mksquashfs", "squashfs-tools"}, "xim-x-squashfs-tools",
         {{Source::Payload, "bin"}, {Source::Payload, "sbin"},
          {Source::Host, "/usr/bin/mksquashfs"}, {Source::Host, "/usr/sbin/mksquashfs"}}},
        {{"xorriso", "xorriso"}, "xim-x-xorriso",
         {{Source::Payload, "bin"}, {Source::Host, "/usr/bin/xorriso"}, {Source::Host, "/usr/local/bin/xorriso"}}},
        {{"limine", "limine"}, "xim-x-limine",
         {{Source::Payload, "bin"}, {Source::Host, "/usr/bin/limine"}, {Source::Host, "/usr/local/bin/limine"}}},
        {{"qemu-system-x86_64", "qemu"}, "xim-x-qemu",
         {{Source::Payload, "bin"}, {Source::Host, "/usr/bin/qemu-system-x86_64"},
          {Source::Host, "/usr/local/bin/qemu-system-x86_64"}}},
        {{"qemu-system-aarch64", "qemu"}, "xim-x-qemu",
         {{Source::Payload, "bin"}, {Source::Host, "/usr/bin/qemu-system-aarch64"},
          {Source::Host, "/usr/local/bin/qemu-system-aarch64"}}},
        {{"qemu-img", "qemu"}, "xim-x-qemu",
         {{Source::Payload, "bin"}, {Source::Host, "/usr/bin/qemu-img"}, {Source::Host, "/usr/local/bin/qemu-img"}}},
    };
    return t;
}

const Entry* entry(std::string_view name) {
    for (const auto& e : table()) if (e.tool.name == name) return &e;
    return nullptr;
}

// Sentinel iteration: a range-for over a directory_iterator through a BMI
// fails to link on the musl cross toolchain (see caps.cpp's history).
std::optional<fs::path> payload_bin(const fs::path& root, std::string_view dir, std::string_view bin) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return std::nullopt;
    std::vector<fs::path> versions;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != std::default_sentinel; it.increment(ec))
        versions.push_back(it->path());
    std::ranges::sort(versions);
    for (const auto& v : versions) {
        const auto candidate = v / std::string(dir) / std::string(bin);
        std::error_code fe;
        if (fs::is_regular_file(candidate, fe)) return candidate;
    }
    return std::nullopt;
}

}  // namespace

std::string_view to_string(Source s) {
    switch (s) {
    case Source::RootOwned: return "root-owned";
    case Source::Payload: return "payload";
    case Source::Runtimedir: return "runtimedir";
    // "system", as `self doctor --isolation --json` has always said it.
    case Source::Host: return "system";
    }
    return "system";
}

std::span<const Tool> known() {
    static const std::vector<Tool> tools = [] {
        std::vector<Tool> out;
        for (const auto& e : table()) out.push_back(e.tool);
        return out;
    }();
    return tools;
}

std::vector<Found> candidates(std::string_view tool, const HomeView& home, const Ports& ports) {
    std::vector<Found> out;
    const auto* e = entry(tool);
    if (!e) return out;
    std::error_code ec;
    // XLINGS_TOOLS_SEARCH=home: only what the home holds -- no root-owned
    // copy, no machine path. A test seam: a host whose own bwrap works can
    // still show what happens on one where none does.
    const char* search = std::getenv("XLINGS_TOOLS_SEARCH");
    const bool home_only = search && std::string_view(search) == "home";
    for (const auto& loc : e->order) {
        if (home_only && (loc.source == Source::RootOwned || loc.source == Source::Host)) continue;
        switch (loc.source) {
        case Source::RootOwned: {
            if constexpr (!platform::is_linux) break;
            const fs::path p(loc.where);
            if (auto own = platform::file_ownership(p); own && own->uid == 0 && !(own->mode & 022))
                out.push_back({std::string(tool), p, Source::RootOwned});
            break;
        }
        case Source::Payload:
            if (auto bin = payload_bin(home.home / "data" / "xpkgs" / std::string(e->xpkgs_dir), loc.where, tool))
                if (std::ranges::none_of(out, [&](const Found& f) { return f.bin == *bin; }))
                    out.push_back({std::string(tool), *bin, Source::Payload});
            break;
        case Source::Runtimedir: {
            const auto p = home.home / "runtimedir" / std::string(loc.where);
            if (fs::is_regular_file(p, ec)) out.push_back({std::string(tool), p, Source::Runtimedir});
            break;
        }
        case Source::Host: {
            const fs::path p(loc.where);
            if (!fs::is_regular_file(p, ec)) break;
            if (ports.shim_owner && ports.shim_owner(p)) break;   // another home's shim
            out.push_back({std::string(tool), p, Source::Host});
            break;
        }
        }
    }
    return out;
}

std::optional<Found> first(std::string_view tool, const HomeView& home, const Ports& ports) {
    auto all = candidates(tool, home, ports);
    if (all.empty()) return std::nullopt;
    return all.front();
}

std::string install_hint(std::string_view tool) {
    const auto* e = entry(tool);
    if (!e || e->tool.package.empty()) return {};
    return "xlings install " + std::string(e->tool.package);
}

}  // namespace xlings::subos::tools
