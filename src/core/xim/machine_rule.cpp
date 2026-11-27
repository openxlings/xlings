module xlings.core.xim.machine_rule;

import std;
import mcpplibs.xpkg;
import xlings.libs.elfread;
import xlings.platform;
import xlings.platform.target;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.shim;
import xlings.core.xvm.registration;

namespace xlings::xim::machine_rule {

namespace fs = std::filesystem;

namespace {

std::string name_of(std::uint16_t machine, int bits) {
    if (const auto* arch = mcpplibs::xpkg::find_arch_by_elf(machine, bits)) return std::string(arch->name);
    return std::format("e_machine {} ({}-bit)", machine, bits);
}

void add_file(Surface& surface, const fs::path& file) {
    if (!file.empty()) surface.files.push_back(file);
}

// What a machine of `host` runs: its own, and on x86_64 also x86 (32-bit),
// which the x86_64 kernel runs natively (syslinux's programs). Nothing else
// is assumed: an aarch64 kernel may have no AArch32, and many have none.
bool runs_(const mcpplibs::xpkg::ArchIdentity& host, std::uint16_t machine, int bits) {
    if (machine == host.elf_machine && bits == host.elf_bits) return true;
    const auto* x86 = mcpplibs::xpkg::find_arch("x86");
    return host.name == "x86_64" && x86 && machine == x86->elf_machine && bits == x86->elf_bits;
}

}  // namespace

Surface of_registration(const fs::path& payload, const xvm::RegistrationBatch& batch,
                        std::string_view loader, const std::vector<std::string>& libdirs,
                        const std::string& home) {
    Surface surface;
    for (const auto& node : batch.nodes) {
        if (node.kind == "program") {
            // An anchor has no command behind it; an alias runs something else.
            if (!node.runnable || !node.alias.empty()) continue;
            add_file(surface, xvm::resolve_executable(
                node.sourceName.empty() ? node.target : node.sourceName, node.path, home));
        } else if (node.kind == "lib" && !node.sourceName.empty()) {
            add_file(surface, fs::path(xvm::expand_path(node.path, home)) / node.sourceName);
        }
    }
    if (!loader.empty()) add_file(surface, payload / loader);
    if (libdirs.empty()) {
        for (const auto* dir : {"lib64", "lib"}) surface.libdirs.push_back(payload / dir);
    } else {
        for (const auto& dir : libdirs) {
            const fs::path d(dir);
            surface.libdirs.push_back(d.is_absolute() ? d : payload / d);
        }
    }
    return surface;
}

Surface of_database_entry(const xvm::VInfo& info, const xvm::VData& data,
                          const std::string& target, const std::string& home) {
    Surface surface;
    if (data.path.empty()) return surface;
    const auto kind = xvm::effective_kind(info, data);
    const auto source = xvm::effective_source_name(target, info, data, kind);
    const auto path = xvm::expand_path(data.path, home);
    if (kind == "program" && data.alias.empty()) {
        add_file(surface, xvm::resolve_executable(source.empty() ? target : source, path, home));
    } else if (kind == "lib" && !source.empty()) {
        add_file(surface, fs::path(path) / source);
    }
    return surface;
}

std::vector<Foreign> foreign(const Surface& surface) {
    if constexpr (!platform::is_linux) return {};
    // A build of an architecture the table lacks: nothing to compare with.
    return foreign(surface, platform::build_arch());
}

Surface of_payload(const fs::path& payload) {
    Surface s;
    for (const auto* dir : {"bin", "sbin"}) {
        std::error_code ec;
        std::vector<fs::path> files;
        for (auto it = fs::directory_iterator(payload / dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            files.push_back(it->path());
        std::ranges::sort(files);
        s.files.insert(s.files.end(), files.begin(), files.end());
    }
    s.libdirs = { payload / "lib64", payload / "lib" };
    return s;
}

std::vector<Foreign> foreign(const Surface& surface, std::string_view machine) {
    std::vector<Foreign> out;
    const auto* host = mcpplibs::xpkg::find_arch(machine);
    if (!host) return out;
    std::set<fs::path> seen;
    const auto judge = [&](const fs::path& file) {
        std::error_code ec;
        const auto real = fs::canonical(file, ec);
        if (ec || !fs::is_regular_file(real, ec) || !seen.insert(real).second) return;
        const auto info = elfread::read(real);
        if (!info) return;   // not ELF -- a script, an archive, data: nothing to judge
        if (!runs_(*host, info->machine, info->bits))
            out.push_back({ .file = file.string(), .machine = name_of(info->machine, info->bits) });
    };
    for (const auto& file : surface.files) judge(file);
    for (const auto& dir : surface.libdirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        std::vector<fs::path> entries;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            entries.push_back(it->path());
        std::ranges::sort(entries);
        for (const auto& entry : entries) judge(entry);
    }
    return out;
}

std::string host_machine() {
    if (const auto* host = mcpplibs::xpkg::find_arch(platform::build_arch())) return std::string(host->name);
    return std::string(platform::build_arch());
}

std::string describe(std::string_view package, const std::vector<Foreign>& files) {
    if (files.empty()) return {};
    std::string out = std::format("{}: {} is for {}, and this machine is {}", package, files.front().file,
                                  files.front().machine, host_machine());
    for (std::size_t i = 1; i < files.size() && i < 3; ++i)
        out += std::format("; {} is for {}", files[i].file, files[i].machine);
    if (files.size() > 3) out += std::format("; and {} more", files.size() - 3);
    return out + " -- the package's entry gives this machine another machine's payload (rule M)";
}

std::string hint() {
    return "nothing was registered and the payload was not kept; the index's entry for this machine "
           "is what needs fixing: https://github.com/openxlings/xim-pkgindex/issues";
}

}  // namespace xlings::xim::machine_rule
