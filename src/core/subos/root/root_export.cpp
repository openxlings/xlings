// A root as something to carry (design part 2 §9, part 3): exported as a tree,
// an archive, a drive image or a live ISO, and packed.
module xlings.core.subos;

import std;
import xlings.core.config;
import xlings.core.home;
import xlings.core.home.prefix_domain;
import xlings.core.home.domain_producer;
import xlings.core.home.domain_producer_source;
import xlings.core.subos.store_closure;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.platform.stream;
import xlings.core.profile;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.libs.elfread;
import xlings.core.subos.root;
import xlings.subos.home_view;
import xlings.subos.policy_store;
import xlings.subos.policy;
import xlings.subos.rootfs;
import xlings.subos.root_etc;
import xlings.subos.root_check;
import xlings.subos.library_cache;
import xlings.subos.roles;
import xlings.subos.boot;
import xlings.observe;
import xlings.core.subos.ports;
import xlings.core.xself;
import xlings.core.xim.extract;
import xlings.core.xim.commands;
import xlings.core.xim.catalog;
import xlings.image;
import xlings.platform.target;
import xlings.subos.tools;
import xlings.subos.caps;
import xlings.subos.edition;
import xlings.core.version_order;
import xlings.core.confirm;
import :root_shared;

namespace xlings::subos {

using namespace rootcmd_;

namespace {
namespace rf = xlings::subos::rootfs;
namespace bt = xlings::subos::boot;
}  // namespace

// ── export ───────────────────────────────────────────────────────────

namespace {

bool same_bytes_(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (!fs::is_regular_file(a, ec) || !fs::is_regular_file(b, ec) || fs::file_size(a, ec) != fs::file_size(b, ec))
        return false;
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    std::vector<char> p(1 << 16), q(1 << 16);
    while (x && y) {
        x.read(p.data(), static_cast<std::streamsize>(p.size()));
        y.read(q.data(), static_cast<std::streamsize>(q.size()));
        if (x.gcount() != y.gcount() || !std::equal(p.begin(), p.begin() + x.gcount(), q.begin())) return false;
    }
    return true;
}

// What a directory of the image has mounted on it when the image runs
// (design 2026-10-16 L §6.2): /usr the generation recorded for it, and a
// generation's directory entry the directory its mounts.tsv names. Nothing
// for any other path.
std::optional<fs::path> mounted_at_(const fs::path& stage, const fs::path& inside) {
    if (inside == "/usr") return rf::usr_target_of(stage);
    for (auto generation = inside.parent_path(); generation.has_relative_path(); generation = generation.parent_path()) {
        if (generation.parent_path().filename() != rf::kGenerations) continue;
        const auto rel = inside.lexically_relative(generation);
        for (const auto& [entry, target] : rf::mounts_of(stage / generation.relative_path()))
            if (entry == rel) return target;
        return std::nullopt;
    }
    return std::nullopt;
}

// A path of the image as the image sees it, resolved component by component
// through the image's own links (an absolute link names a path of the
// image, not of this machine) and its mounts -- what the kernel does inside
// a chroot of the running image.
fs::path inside_stage_(const fs::path& stage, const fs::path& inside, int& hops) {
    fs::path current = "/";
    std::error_code ec;
    for (const auto& part : inside.relative_path()) {
        if (part == ".") continue;
        if (part == "..") { current = current.parent_path(); continue; }
        const auto next = current / part;
        const auto here = stage / next.relative_path();
        if (fs::is_symlink(here, ec) && ++hops < 40) {
            const auto target = fs::read_symlink(here, ec);
            if (ec) return next;
            current = inside_stage_(stage, (target.is_absolute() ? target : current / target).lexically_normal(), hops);
        } else if (const auto mounted = fs::is_directory(here, ec) && fs::is_empty(here, ec) ? mounted_at_(stage, next)
                                                                                            : std::nullopt;
                   mounted && mounted->is_absolute() && ++hops < 40) {
            current = inside_stage_(stage, mounted->lexically_normal(), hops);
        } else {
            current = next;
        }
    }
    return current;
}

fs::path in_stage_(const fs::path& stage, const fs::path& inside) {
    int hops = 0;
    return stage / inside_stage_(stage, inside, hops).relative_path();
}

// Where limine's boot files are: beside the limine this home or this
// machine has (the tool table), a limine payload, the machine's share.
std::optional<fs::path> limine_data_(const fs::path& home, EventStream& stream) {
    std::vector<fs::path> candidates;
    auto ports = subos::make_ports(stream);
    if (auto found = subos::tools::first("limine", home_view(), ports))
        candidates.push_back(found->bin.parent_path().parent_path() / "share" / "limine");
    std::error_code ec;
    for (fs::directory_iterator it(home / "data" / "xpkgs" / "xim-x-limine", ec), end; !ec && it != end; it.increment(ec))
        candidates.push_back(it->path() / "share" / "limine");
    candidates.insert(candidates.end(), {"/usr/share/limine", "/usr/local/share/limine"});
    // What this machine boots with: limine's UEFI loader and CD image, and
    // its BIOS stages where the machine has a BIOS.
    const auto& machine = xlings::image::native_arch();
    for (const auto& d : candidates) {
        bool all = fs::is_regular_file(d / machine.efi_loader, ec) && fs::is_regular_file(d / "limine-uefi-cd.bin", ec);
        if (machine.bios) all = all && fs::is_regular_file(d / "limine-bios-cd.bin", ec) && fs::is_regular_file(d / "limine-bios.sys", ec);
        if (all) return d;
    }
    return std::nullopt;
}

// The kernel an image boots: --kernel, or the root's own (followed through
// the image's links). None is refused with the route.
// "6.8.0-71-generic" / "5.10" -> {6, 8, 0} / {5, 10}: what is compared.
std::vector<int> version_parts_(std::string_view v) {
    std::vector<int> out;
    for (std::size_t i = 0; i < v.size() && out.size() < 3;) {
        int n = 0;
        auto [p, e] = std::from_chars(v.data() + i, v.data() + v.size(), n);
        if (e != std::errc{}) break;
        out.push_back(n);
        i = static_cast<std::size_t>(p - v.data());
        if (i >= v.size() || v[i] != '.') break;
        ++i;
    }
    return out;
}

// The kernel an image boots: --kernel, or the root's own (followed through
// the image's links). None is refused with the route -- the kernel its
// edition recommends (boot.kernel) when it names one -- and one older than
// what its userland needs (boot.kernel_min) is refused too (§A6).
std::expected<fs::path, std::pair<std::string, std::string>> kernel_of_(const fs::path& stage, const fs::path& given,
                                                                        const nlohmann::json& boot, std::string_view name) {
    std::error_code ec;
    fs::path kernel = given;
    std::string version;
    if (kernel.empty()) {
        // The boot profile's kernel when it names one (a root may hold more).
        if (const auto release = boot.is_object() ? boot.value("release", std::string()) : std::string(); !release.empty()) {
            const auto candidate = in_stage_(stage, fs::path("/usr/lib/modules") / release / "vmlinuz");
            if (fs::is_regular_file(candidate, ec)) { kernel = candidate; version = release; }
        }
    }
    if (kernel.empty()) {
        const auto modules = in_stage_(stage, "/usr/lib/modules");
        for (fs::directory_iterator it(modules, ec), end; !ec && it != end; it.increment(ec)) {
            const auto candidate = in_stage_(stage, fs::path("/usr/lib/modules") / it->path().filename() / "vmlinuz");
            if (fs::is_regular_file(candidate, ec)) { kernel = candidate; version = it->path().filename().string(); break; }
        }
    }
    if (kernel.empty() || !fs::is_regular_file(kernel, ec)) {
        const auto recommended = boot.is_object() ? boot.value("kernel", std::string("linux-kernel")) : std::string("linux-kernel");
        return std::unexpected(std::pair{
            std::string("a bootable image needs a kernel, and this root has none"),
            std::format("install one into it (xlings install {} --subos {}), or give --kernel <vmlinuz>", recommended, name)});
    }
    const auto floor = boot.is_object() ? boot.value("kernel_min", std::string()) : std::string();
    if (!floor.empty() && !version.empty() && version_parts_(version) < version_parts_(floor))
        return std::unexpected(std::pair{
            std::format("its kernel is {}, and its userland needs {} or newer", version, floor),
            std::format("a newer kernel: xlings install {} --subos {}",
                        boot.value("kernel", std::string("linux-kernel")), name)});
    return kernel;
}

// The console and the rest of a kernel command line an image adds to what it
// must say (its root, its init): the boot profile's, or a machine's default.
std::string kernel_args_(const nlohmann::json& boot) {
    std::string line;
    if (auto c = boot.is_object() ? boot.find("cmdline") : boot.end(); boot.is_object() && c != boot.end() && c->is_array())
        for (const auto& a : *c) if (a.is_string()) line += (line.empty() ? "" : " ") + a.get<std::string>();
    return line.empty() ? std::string("console=tty0 console=ttyS0") : line;
}

// "virt" -> xim:luban-boot-virt; a full reference stays as it is.
std::string boot_profile_ref_(std::string_view profile) {
    return profile.find(':') == std::string_view::npos ? std::format("xim:luban-boot-{}", profile) : std::string(profile);
}

// A boot profile (Luban OS design part 2 §2.4) is installed into the root,
// as a kernel is to make an image: its kernel and limine come with it, and
// its share/luban/boot.json says which kernel and what command line.
std::expected<nlohmann::json, std::pair<std::string, std::string>> apply_boot_profile_(
    const fs::path& home, const std::string& name, const std::string& ref, EventStream& stream) {
    log::info("boot profile {} ...", ref);
    const std::vector<std::string> targets{ref};
    if (xim::cmd_install_step(targets, name, stream, /*plumbing=*/true) != 0)
        return std::unexpected(std::pair{std::format("the boot profile {} could not be installed into '{}'", ref, name),
                                         std::string("--boot virt or --boot generic, or --kernel <vmlinuz>")});
    const auto key = ref.substr(0, ref.find('@'));
    const auto configured = read_json_(HomeView{home}.instance(name) / ".xlings.json").value("configured", nlohmann::json::object());
    for (auto it = configured.begin(); it != configured.end(); ++it) {
        const auto at = it.key().rfind('@');
        if (at == std::string::npos || it.key().substr(0, at) != key) continue;
        const auto colon = key.find(':');
        const auto file = home / "data" / "xpkgs"
            / xim::package_store_name(colon == std::string::npos ? "xim" : key.substr(0, colon),
                                      colon == std::string::npos ? key : key.substr(colon + 1))
            / it.key().substr(at + 1) / "share" / "luban" / "boot.json";
        auto profile = read_json_(file);
        if (!profile.is_object() || profile.empty())
            return std::unexpected(std::pair{std::format("{} has no share/luban/boot.json", ref), std::string{}});
        return profile;
    }
    return std::unexpected(std::pair{std::format("{} is not installed in '{}'", ref, name), std::string{}});
}

std::uint64_t size_bytes_(std::string_view size) {
    std::uint64_t n = 0;
    auto [p, e] = std::from_chars(size.data(), size.data() + size.size(), n);
    if (e != std::errc{}) return 0;
    switch (p < size.data() + size.size() ? std::toupper(static_cast<unsigned char>(*p)) : 'B') {
    case 'K': return n << 10;
    case 'M': return n << 20;
    case 'G': return n << 30;
    case 'T': return n << 40;
    default: return n;
    }
}

// A drive image of the image in `stage` (Luban design §A9): GPT, a FAT
// system partition with limine (UEFI, and BIOS when limine's tool is here),
// the kernel and limine.conf, and an ext4 root the kernel mounts by its
// PARTUUID -- a drive that boots on a machine as on qemu, and keeps what it
// writes. The GPT and the FAT are written in-process (xlings.image).
std::expected<void, std::pair<std::string, std::string>> write_drive_(
    const fs::path& stage, const fs::path& home, const fs::path& kernel_file, const std::string& size,
    const fs::path& scratch, const fs::path& output, const nlohmann::json& boot, std::string_view name,
    EventStream& stream) {
    auto kernel = kernel_of_(stage, kernel_file, boot, name);
    if (!kernel) return std::unexpected(kernel.error());
    auto limine = limine_data_(home, stream);
    std::error_code ec;
    if (!limine)
        return std::unexpected(std::pair{std::string("limine's boot files are not here"), subos::tools::install_hint("limine")});
    const auto& machine = xlings::image::native_arch();
    const std::uint64_t esp_bytes = 64ull << 20;
    const std::uint64_t root_bytes = size_bytes_(size);
    if (root_bytes < (64ull << 20)) return std::unexpected(std::pair{"--size " + size + ": too small for a root", std::string{}});
    const auto root_uuid = xlings::image::random_guid();
    // BIOS boot (limine's second stage) where the machine has a BIOS, the
    // system partition, the root.
    const std::uint64_t bios_bytes = 1ull << 20;
    std::vector<std::uint64_t> sizes;
    if (machine.bios) sizes.push_back(bios_bytes);
    sizes.insert(sizes.end(), {esp_bytes, root_bytes});
    const auto starts = xlings::image::layout(sizes);
    const auto esp_start = starts[machine.bios ? 1 : 0];
    // The root filesystem, as --disk makes it.
    auto mkfs = tool_("mkfs.ext4", stream);
    if (!mkfs) return std::unexpected(std::pair{std::string("mkfs.ext4 is not available"), subos::tools::install_hint("mkfs.ext4")});
    const auto rootfs = scratch / "root.ext4";
    auto argv = as_root_({*mkfs, "-q", "-F", "-L", "luban", "-d", stage.string(), rootfs.string(), size}, stream);
    if (!argv) return std::unexpected(std::pair{std::string("cannot write the root filesystem"), std::string{}});
    if (run_tool_(*argv, stream, "writing the root filesystem") != 0)
        return std::unexpected(std::pair{std::string("mkfs.ext4 failed"), std::string{}});
    // The system partition.
    // Stage-0 is luban-init: without it beside the client the image cannot
    // start (xlings-init only hands over to it), so it is not made.
    if (!fs::exists(stage / home.relative_path() / "boot" / "luban-init", ec))
        return std::unexpected(std::pair{std::format("no luban-init beside {}: a machine starts with it, so the "
                                                     "image would not boot", (home / "bin" / "xlings").string()),
                                         std::string("xlings self update")});
    const auto init = "luban-init";
    const std::string conf = std::format(
        "# A Luban drive (luban export <name> <file>.img)\ntimeout: 3\n\n/Luban\n    protocol: linux\n"
        "    path: boot():/boot/vmlinuz\n"
        "    cmdline: root=PARTUUID={} rw rootwait {} init={}/boot/{}\n",
        xlings::image::to_string(root_uuid), kernel_args_(boot), home.generic_string(), init);
    std::vector<xlings::image::File> files{
        {.path = "EFI/BOOT/" + std::string(machine.efi_loader), .from = *limine / machine.efi_loader},
        {.path = "boot/vmlinuz", .from = *kernel},
        {.path = "boot/limine/limine.conf", .content = conf},
    };
    if (machine.bios) files.push_back({.path = "boot/limine/limine-bios.sys", .from = *limine / "limine-bios.sys"});
    const auto esp = scratch / "esp.fat";
    if (auto w = xlings::image::write_fat(esp, esp_bytes, files, static_cast<std::uint32_t>(esp_start), "LUBANESP"); !w)
        return std::unexpected(std::pair{"the system partition: " + w.error(), std::string{}});
    std::vector<xlings::image::Partition> parts;
    if (machine.bios)
        parts.push_back({.name = "BIOS boot", .type = xlings::image::kBiosBootType,
                         .uuid = xlings::image::random_guid(), .content = {}, .bytes = bios_bytes});
    parts.push_back({.name = "EFI system", .type = xlings::image::kEspType,
                     .uuid = xlings::image::random_guid(), .content = esp, .bytes = esp_bytes});
    parts.push_back({.name = "luban", .type = xlings::image::kLinuxType, .uuid = root_uuid,
                     .content = rootfs, .bytes = root_bytes});
    if (auto w = xlings::image::write_gpt_disk(output, parts); !w)
        return std::unexpected(std::pair{"the drive: " + w.error(), std::string{}});
    fs::remove(esp, ec);
    fs::remove(rootfs, ec);
    if (!machine.bios) return {};
    auto ports = subos::make_ports(stream);
    if (auto tool = subos::tools::first("limine", home_view(), ports)) {
        if (run_tool_({tool->bin.string(), "bios-install", output.string()}, stream, "making it boot on BIOS too") != 0)
            log::warn("the drive boots with UEFI; BIOS boot needs `limine bios-install` to work here");
    } else {
        log::info("  boots with UEFI; on BIOS too with limine's tool ({})", subos::tools::install_hint("limine"));
    }
    return {};
}

// A live ISO of the image in `stage` (Luban design §A9): limine boots the
// kernel with the whole root as its initramfs -- the kernel unpacks it into
// memory and runs stage-0 there, nothing to mount and no module to load.
// Written in-process (the aarch64 design §4.2): ISO 9660 with a BIOS El
// Torito entry where the machine has a BIOS and a UEFI one always, and a
// hybrid GPT, so it boots from a disc and from a drive it is written to --
// on BIOS too once `limine bios-install` has run (x86_64).
std::expected<void, std::pair<std::string, std::string>> write_live_iso_(
    const fs::path& stage, const fs::path& home, const fs::path& kernel_file, const fs::path& scratch,
    const nlohmann::json& boot, std::string_view name, EventStream& stream) {
    std::error_code ec;
    auto found = kernel_of_(stage, kernel_file, boot, name);
    if (!found) return std::unexpected(found.error());
    const auto kernel = *found;
    auto limine = limine_data_(home, stream);
    if (!limine)
        return std::unexpected(std::pair{std::string("limine's boot files are not here"),
                                         subos::tools::install_hint("limine")});
    const auto& machine = xlings::image::native_arch();
    const auto iso = scratch / "iso";
    for (const auto* d : {"boot/limine", "EFI/BOOT"}) fs::create_directories(iso / d, ec);
    fs::copy_file(kernel, iso / "boot" / "vmlinuz", ec);
    if (ec) return std::unexpected(std::pair{"cannot copy the kernel: " + ec.message(), std::string{}});
    log::info("packing the root as the live system's memory image ...");
    if (auto packed = xim::write_archive(stage, iso / "boot" / "initramfs.img", xim::ArchiveFormat::CpioNewcGz); !packed)
        return std::unexpected(std::pair{"writing the live image: " + packed.error(), std::string{}});
    std::vector<std::string> boot_files{"limine-uefi-cd.bin"};
    if (machine.bios) boot_files.insert(boot_files.end(), {"limine-bios-cd.bin", "limine-bios.sys"});
    for (const auto& f : boot_files) {
        fs::copy_file(*limine / f, iso / "boot" / "limine" / f, ec);
        if (ec) return std::unexpected(std::pair{"cannot copy " + f + ": " + ec.message(), std::string{}});
    }
    fs::copy_file(*limine / machine.efi_loader, iso / "EFI" / "BOOT" / machine.efi_loader, ec);
    // Stage-0 is luban-init: without it beside the client the image cannot
    // start (xlings-init only hands over to it), so it is not made.
    if (!fs::exists(stage / home.relative_path() / "boot" / "luban-init", ec))
        return std::unexpected(std::pair{std::format("no luban-init beside {}: a machine starts with it, so the "
                                                     "image would not boot", (home / "bin" / "xlings").string()),
                                         std::string("xlings self update")});
    const auto init = "luban-init";
    {
        std::ofstream conf(iso / "boot" / "limine" / "limine.conf");
        conf << "# A live Luban system (luban export <name> <file>.iso)\n"
             << "timeout: 3\n\n/Luban\n    protocol: linux\n    path: boot():/boot/vmlinuz\n"
             << "    module_path: boot():/boot/initramfs.img\n"
             << std::format("    cmdline: {} rdinit={}/boot/{}\n", kernel_args_(boot), home.generic_string(), init);
        if (!conf) return std::unexpected(std::pair{std::string("cannot write limine.conf"), std::string{}});
    }
    const auto output = scratch / "output";
    // The El Torito default entry: BIOS where there is one (limine's CD
    // stage, a boot-info table patched in), else the EFI image itself.
    const auto default_boot = machine.bios ? xim::ArchiveOptions{.boot = "boot/limine/limine-bios-cd.bin"}
                                           : xim::ArchiveOptions{.boot = "boot/limine/limine-uefi-cd.bin", .boot_info_table = false};
    if (auto written = xim::write_archive(iso, output, xim::ArchiveFormat::Iso9660, default_boot); !written)
        return std::unexpected(std::pair{"writing the ISO: " + written.error(), std::string{}});
    auto efi = xlings::image::iso_find(output, "boot/limine/limine-uefi-cd.bin");
    if (!efi) return std::unexpected(std::pair{"the ISO: " + efi.error(), std::string{}});
    if (auto hybrid = xlings::image::make_iso_hybrid(output, *efi, machine.bios); !hybrid)
        return std::unexpected(std::pair{"making the ISO boot on UEFI: " + hybrid.error(), std::string{}});
    if (!machine.bios) return {};
    auto ports = subos::make_ports(stream);
    if (auto tool = subos::tools::first("limine", home_view(), ports)) {
        if (run_tool_({tool->bin.string(), "bios-install", output.string()}, stream, "making it boot from a drive on BIOS") != 0)
            log::warn("the ISO boots on UEFI and from a CD; from a drive on BIOS it needs `limine bios-install` to work here");
    } else {
        log::info("  boots on UEFI and from a CD; from a drive on BIOS too with limine's tool ({})",
                  subos::tools::install_hint("limine"));
    }
    return {};
}

}  // namespace

int run_export_(int argc, char* argv[], EventStream& stream, const UsageError& usageError) {
    std::string name;
    fs::path rootfs_dir, tarball, disk, drive, iso, qcow2, kernel_file;
    std::string size = "4G", boot_profile;
    bool with_data = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--rootfs" && i + 1 < argc) rootfs_dir = argv[++i];
        else if (a == "--tar" && i + 1 < argc) tarball = argv[++i];
        else if (a == "--disk" && i + 1 < argc) disk = argv[++i];
        else if (a == "--iso" && i + 1 < argc) iso = argv[++i];
        else if (a == "--drive" && i + 1 < argc) drive = argv[++i];
        else if (a == "--qcow2" && i + 1 < argc) qcow2 = argv[++i];
        else if (a == "--kernel" && i + 1 < argc) kernel_file = argv[++i];
        else if (a == "--boot" && i + 1 < argc) boot_profile = argv[++i];
        else if (a == "--size" && i + 1 < argc) size = argv[++i];
        else if (a == "--with-data") with_data = true;
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
        else { usageError("unknown option for `xlings subos export`: " + a); return 1; }
    }
    const int outputs = !rootfs_dir.empty() + !tarball.empty() + !disk.empty() + !drive.empty() + !iso.empty()
                        + !qcow2.empty();
    if (name.empty() || outputs != 1) {
        usageError("usage: xlings subos export <name> --rootfs <dir> | --tar <file> | --disk <file> | "
                   "--drive <file> | --qcow2 <file> | --iso <file> [--size 4G] [--boot <profile> | --kernel <vmlinuz>] [--with-data]");
        return 1;
    }
    // The flag and the file, whichever was asked for. A directory may be
    // written `dir/` (that is how luban tells the format): the name is `dir`.
    const auto [flag, given] = !rootfs_dir.empty() ? std::pair{"--rootfs", rootfs_dir}
        : !tarball.empty() ? std::pair{"--tar", tarball} : !disk.empty() ? std::pair{"--disk", disk}
        : !drive.empty() ? std::pair{"--drive", drive} : !iso.empty() ? std::pair{"--iso", iso}
        : std::pair{"--qcow2", qcow2};
    const fs::path target = given.has_filename() ? given : given.parent_path();
    auto domainScope = xlings::home::domain_producer::read_scope(home_dir_(), name);
    if (!domainScope) { error_(stream, domainScope.error()); return 1; }
    if (*domainScope) {
        const auto out = fs::absolute(target);
        if (auto fresh = require_new_output_(out); !fresh) { error_(stream, fresh.error()); return 1; }
        auto scratch = OwnedStage::create(out.parent_path());
        if (!scratch) { error_(stream, scratch.error()); return 1; }
        const fs::path guest = "/run/xlings-domain-output";
        std::vector<std::string> arguments{"subos", "export", name, flag, (guest / "output").string()};
        if (!disk.empty() || !drive.empty() || !qcow2.empty()) arguments.insert(arguments.end(), {"--size", size});
        if (with_data) arguments.push_back("--with-data");
        auto exported = xlings::home::domain_producer::run((**domainScope).domain, arguments,
            xlings::home::domain_producer::OutputBinding{scratch->path(), guest});
        if (!exported || *exported != 0) {
            error_(stream, exported ? std::format("namespace export failed (exit {})", *exported) : exported.error());
            return exported ? *exported : 1;
        }
        if (auto published = platform::rename_no_replace(scratch->path() / "output", out); !published) {
            error_(stream, published.error()); return 1;
        }
        return 0;
    }
    if (!exists_(name)) { error_(stream, "no SubOS named '" + name + "'"); return 1; }
    if (!role_allows_(roles::Op::Export, name, stream)) return 1;
    if constexpr (!platform::is_linux) {
        error_(stream, "exporting a root needs Linux", "export it on a Linux machine");
        return 1;
    }

    const auto out = fs::absolute(target);
    if (!kernel_file.empty() && iso.empty() && drive.empty() && qcow2.empty()) {
        error_(stream, "--kernel names the kernel an ISO or a drive boots; a --disk boots the kernel given to it");
        return 1;
    }
    if (!boot_profile.empty() && (iso.empty() && drive.empty() && qcow2.empty())) {
        error_(stream, "--boot names how an ISO or a drive boots");
        return 1;
    }
    if (!boot_profile.empty() && !kernel_file.empty()) {
        error_(stream, "--boot and --kernel both say which kernel: give one");
        return 1;
    }
    if (auto fresh = require_new_output_(out); !fresh) {
        error_(stream, fresh.error(), "choose a new output path");
        return 1;
    }

    const auto home = home_dir_();
    const auto instance = HomeView{home}.instance(name);
    // How an image boots: --boot, else the edition's profile; else the
    // root's own kernel (or --kernel). Applied before the root is read.
    auto boot = read_json_(HomeView{home}.instance_file(name)).value("boot", nlohmann::json::object());
    if (boot_profile.empty() && kernel_file.empty() && (!iso.empty() || !drive.empty() || !qcow2.empty()))
        boot_profile = boot.value("profile", std::string());
    if (!boot_profile.empty()) {
        auto profile = apply_boot_profile_(home, name, boot_profile_ref_(boot_profile), stream);
        if (!profile) { error_(stream, profile.error().first, profile.error().second); return 1; }
        for (const auto* k : {"release", "cmdline"})
            if (profile->contains(k)) boot[k] = (*profile)[k];
    }
    // The image's xlings runs with no host under it: the static release build.
    fs::path entry = home / "bin" / "xlings";
    std::error_code ec;
    auto check_io = [&]() {
        if (!ec) return true;
        error_(stream, "cannot construct exported root: " + ec.message(), {}, ErrorCode::Internal);
        return false;
    };
    auto write_json = [&](const fs::path& path, const nlohmann::json& value) {
        const auto written = write_json_(path, value);
        if (written) return true;
        error_(stream, written.error(), {}, ErrorCode::Internal);
        return false;
    };
    if (!fs::exists(entry, ec)) entry = platform::get_executable_path();
    entry = fs::weakly_canonical(entry, ec);
    if (auto info = elfread::read(entry); !info || !info->interpreter.empty()) {
        error_(stream, entry.string() + " is not a static build; an image's xlings must run with no "
                                        "host libraries",
               std::format("use the release build of xlings (xlings install xlings), or `mcpp build --target {}`",
                           xlings::image::native_arch().triple));
        return 1;
    }
    auto ws = workspace_of_(name);
    if (!ws) { error_(stream, "the workspace of '" + name + "' is unreadable"); return 1; }
    if (auto r = subos_root::refresh(home, name, "export"); r && !*r) {
        error_(stream, r->error(), {}, ErrorCode::Internal);
        return 1;
    }

    auto closureInputs = subos_root::store_closure::read_scope(home, name, subos_root::tree_of(home, name));
    if (!closureInputs) { error_(stream, closureInputs.error()); return 1; }
    auto checkedClosure = subos_root::store_closure::collect(*closureInputs);
    if (!checkedClosure) { error_(stream, checkedClosure.error()); return 1; }
    auto sourceMapping = xlings::home::domain_producer_source::read();
    if (!sourceMapping) { error_(stream, sourceMapping.error()); return 1; }
    const auto originalVersions = Config::versions();
    auto exportedVersions = originalVersions;
    const auto selectedKey = [&](const std::string& target, const std::string& key) {
        const auto installed = ws->installed.find(target);
        const auto active = ws->active.find(target);
        return (installed != ws->installed.end() && std::ranges::find(installed->second, key) != installed->second.end()) ||
               (active != ws->active.end() && active->second == key);
    };
    const auto imagePath = [&](const fs::path& path) -> std::expected<fs::path, std::string> {
        const auto normalized = path.lexically_normal();
        if (!normalized.is_absolute()) return normalized;
        // The environment's runtime (part 3 §4): bound through <home>/abi,
        // which the image's home has too -- the runtime its root carries.
        if (const auto abi = normalized.lexically_relative(home / "abi"); !abi.empty() && *abi.begin() != "..")
            return (home / "abi" / abi).lexically_normal();
        for (const auto& mount : checkedClosure->mounts) {
            const auto slot = mount.source.lexically_relative(mount.home);
            std::vector<fs::path> sources{mount.source, mount.destination};
            if (*sourceMapping && mount.home == (**sourceMapping).recordedHome)
                sources.push_back((**sourceMapping).physicalHome / slot);
            for (const auto& source : sources) {
                const auto relative = normalized.lexically_relative(source);
                if (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..") {
                    auto mapped = (home / slot / relative).lexically_normal();
                    if (mapped.filename().empty() && mapped != mapped.root_path()) mapped = mapped.parent_path();
                    return mapped;
                }
            }
        }
        return std::unexpected(normalized.string() + ": export path is outside the checked payload closure");
    };
    for (auto target = exportedVersions.begin(); target != exportedVersions.end();) {
        for (auto version = target->second.versions.begin(); version != target->second.versions.end();) {
            if (!selectedKey(target->first, version->first)) {
                version = target->second.versions.erase(version); continue;
            }
            auto& data = version->second;
            const auto mapField = [&](std::string& text) -> bool {
                if (text.empty()) return true;
                auto mapped = imagePath(fs::path(xvm::expand_path(text, home.string())));
                if (!mapped) { error_(stream, mapped.error()); return false; }
                text = mapped->generic_string(); return true;
            };
            if (!mapField(data.path) || !mapField(data.includedir) || !mapField(data.libdir) || !mapField(data.fileSrc)) return 1;
            for (auto& header : data.bindingHeaders) if (!mapField(header.sourceDir)) return 1;
            data.sourceHome.clear(); data.sourceScope.clear(); data.layerMetadata.clear();
            ++version;
        }
        if (target->second.versions.empty()) target = exportedVersions.erase(target);
        else ++target;
    }

    auto scratch = OwnedStage::create(out.parent_path());
    if (!scratch) { error_(stream, scratch.error(), {}, ErrorCode::Internal); return 1; }
    const auto stage = scratch->path() / "rootfs";
    const auto image_home = stage / home.relative_path();
    const auto image_default = image_home / "subos" / "default";
    log::info("exporting '{}' ...", name);

    // The image's own system home: this instance is its `default`.
    std::set<fs::path> copiedSlots;
    for (const auto& mount : checkedClosure->mounts) {
        const auto relative = mount.source.lexically_relative(mount.home);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." ||
            !copiedSlots.insert(relative).second) {
            error_(stream, "export closure has conflicting or unproved payload destinations"); return 1;
        }
        const auto copied = image_home / relative;
        if (auto c = copy_tree_(mount.source, copied); !c) {
            error_(stream, c.error(), {}, ErrorCode::Internal); return 1;
        }
        // ELF interpreters and recipe aliases retain their logical source
        // prefix. Only the checked slot is exposed there; bytes live once in
        // the image's owned store and no external source home is needed.
        const auto ownedSlot = home / relative;
        if (mount.destination != ownedSlot) {
            const auto logicalSlot = stage / mount.destination.relative_path();
            fs::create_directories(logicalSlot.parent_path(), ec);
            if (!check_io()) return 1;
            fs::create_directory_symlink(ownedSlot, logicalSlot, ec);
            if (!check_io()) return 1;
        }
        {
            auto evidence = xlings::home::read_json_for_update(copied / ".xlings-resolution.json");
            if (!evidence) { error_(stream, evidence.error()); return 1; }
            if (!evidence->contains("deps") || !(*evidence)["deps"].is_array()) {
                error_(stream, "exported payload has no checked runtime evidence"); return 1;
            }
            for (auto& dependency : (*evidence)["deps"]) {
                const auto mapField = [&](nlohmann::json& field) -> bool {
                    if (!field.is_string()) { error_(stream, "invalid exported runtime dependency path"); return false; }
                    const fs::path path(field.get<std::string>());
                    if (!path.is_absolute()) return true;
                    auto mapped = imagePath(path);
                    if (!mapped) { error_(stream, mapped.error()); return false; }
                    field = mapped->generic_string(); return true;
                };
                // Bound through the environment's runtime: whichever runtime
                // the image's root carries answers it (its install_dir names
                // the provider at install time, which may be another system's).
                if (dependency.value("source", std::string{}) == "scope") continue;
                if (!dependency.contains("install_dir") || !mapField(dependency["install_dir"])) return 1;
                if (dependency.contains("libdirs")) {
                    if (!dependency["libdirs"].is_array()) { error_(stream, "invalid exported runtime libdirs"); return 1; }
                    for (auto& libdir : dependency["libdirs"]) if (!mapField(libdir)) return 1;
                }
            }
            if (!write_json(copied / ".xlings-resolution.json", *evidence)) return 1;
        }
    }
    const bool has_data = fs::exists(home / "data", ec);
    if (!check_io()) return 1;
    for (fs::directory_iterator it(has_data ? home / "data" : instance, ec), end;
         has_data && !ec && it != end; it.increment(ec)) {
        const auto& e = *it;
        const auto n = e.path().filename().string();
        if (n == "xpkgs" || n == "runtimedir" || n == "stale" || n.starts_with(".")) continue;
        if (auto c = copy_tree_(e.path(), image_home / "data" / n); !c) {
            error_(stream, c.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }
    if (!check_io()) return 1;
    fs::create_directories(image_default, ec);
    if (!check_io()) return 1;
    for (fs::directory_iterator it(instance, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& e = *it;
        const auto n = e.path().filename().string();
        if (n == rf::kTree || n == rf::kPointer || n == rf::kGenerations || n == "home" || n == "tmp"
            || n == "home.img" || n == ".mountpoint")
            continue;
        if (auto c = copy_tree_(e.path(), image_default / n); !c) {
            error_(stream, c.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }
    if (!check_io()) return 1;
    // The projection, re-planned for its new name: same payloads, its
    // sysroot now at subos/default.
    {
        auto plan = rf::plan(subos_root::inputs(home, instance, ws->active, exportedVersions));
        const auto from = instance.generic_string(), to = (home / "subos" / "default").generic_string();
        for (auto& l : plan.links) {
            auto t = l.target.generic_string();
            if (t.starts_with(from + "/") || t == from) l.target = to + t.substr(from.size());
        }
        // A tarball or a directory is started by an init that mounts nothing:
        // its directories are links (design 2026-10-16 L §6.2).
        const bool linked = !tarball.empty() || !rootfs_dir.empty();
        if (auto c = rf::commit(image_default, plan, "export of " + name,
                                linked ? rf::Directories::Linked : rf::Directories::Mounted); !c) {
            error_(stream, c.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }
    fs::create_directories(image_home / "subos", ec);
    if (!check_io()) return 1;
    fs::create_directory_symlink("default", image_home / "subos" / "current", ec);
    if (!check_io()) return 1;
    fs::create_directories(image_home / "bin", ec);
    if (!check_io()) return 1;
    fs::copy_file(entry, image_home / "bin" / "xlings", fs::copy_options::overwrite_existing, ec);
    if (!check_io()) return 1;
    fs::permissions(image_home / "bin" / "xlings", fs::perms::owner_all | fs::perms::group_read
                    | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace, ec);
    if (!check_io()) return 1;
    // Stage-0's own binary and the Luban tool travel with the client they
    // belong to (part 3 §8; Luban design §A1).
    for (const auto* companion : {"luban", "luban-init"}) {
        const auto from = entry.parent_path() / companion;
        if (!fs::is_regular_file(from, ec)) { ec.clear(); continue; }
        // One binary under two names (Luban design §A1.1): in an image --
        // a live one is held in memory -- the second name is a link.
        if (std::string_view(companion) == "luban-init" && same_bytes_(from, entry.parent_path() / "luban")) {
            fs::create_symlink("luban", image_home / "bin" / companion, ec);
            if (!check_io()) return 1;
            continue;
        }
        fs::copy_file(from, image_home / "bin" / companion, fs::copy_options::overwrite_existing, ec);
        if (!check_io()) return 1;
        fs::permissions(image_home / "bin" / companion, fs::perms::owner_all | fs::perms::group_read
                        | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                        fs::perm_options::replace, ec);
        if (!check_io()) return 1;
    }
    ec.clear();
    fs::create_directories(image_home / "boot", ec);
    if (!check_io()) return 1;
    {
        const bool luban_init = fs::exists(image_home / "bin" / "luban-init");
        if (!luban_init)
            stream.emit(LogEvent{LogLevel::warn, std::format("no luban-init beside {}: this root runs its tools, "
                                                             "but a machine cannot boot it (xlings self update)",
                                                             entry.string())});
        fs::create_symlink(luban_init ? "../bin/luban-init" : "../bin/xlings", image_home / "boot" / "xlings-init", ec);
        if (!ec && luban_init) fs::create_symlink("../bin/luban-init", image_home / "boot" / "luban-init", ec);
    }
    if (!check_io()) return 1;
    {
        // The home config: this instance's packages, nothing of the builder's.
        auto j = nlohmann::json::parse(read_text_(home / ".xlings.json"), nullptr, false);
        if (!j.is_object()) j = nlohmann::json::object();
        nlohmann::json versions = nlohmann::json::object();
        if (j.contains("versions") && j["versions"].is_object()) {
            for (auto target = j["versions"].begin(); target != j["versions"].end(); ++target) {
                const auto normalizedTarget = exportedVersions.find(target.key());
                if (normalizedTarget == exportedVersions.end()) continue;
                auto encodedTarget = target.value();
                auto& entries = encodedTarget["versions"];
                for (auto version = entries.begin(); version != entries.end();) {
                    const auto normalized = normalizedTarget->second.versions.find(version.key());
                    if (normalized == normalizedTarget->second.versions.end()) {
                        version = entries.erase(version); continue;
                    }
                    const auto encoded = xvm::vdata_to_json(normalized->second);
                    for (const auto* field : {"path", "includedir", "libdir", "fileSrc", "bindingHeaders"})
                        if (encoded.contains(field)) version.value()[field] = encoded[field];
                    version.value().erase("layer");
                    ++version;
                }
                versions[target.key()] = std::move(encodedTarget);
            }
        }
        j["versions"] = versions;
        j.erase("dbIndex");
        fs::remove(image_home / "data/versions.json", ec);
        if (!check_io()) return 1;
        j["activeSubos"] = "default";
        for (auto k : {"knownProjects", "subos"}) j.erase(k);
        if (!write_json(image_home / ".xlings.json", j)) return 1;
        const bool multi = home == fs::path("/xlings");
        nlohmann::json marker{{"layout", multi ? "multi" : "single"}, {"mode", "root"}, {"root_layout", multi ? "multi" : "single"}};
        if (!write_json(image_home / ".xlings-home", marker)) return 1;
        fs::create_directories(image_home / "config" / "subos" / "default", ec);
        if (!check_io()) return 1;
        if (!write_json(image_home / "config" / "subos" / "default" / "instance.json",
                        nlohmann::json{{"kind", "rootfs"}})) return 1;
        bt::Config boot;
        if (auto saved = bt::save(image_home / "boot.json", boot); !saved) {
            error_(stream, saved.error(), {}, ErrorCode::Internal);
            return 1;
        }
    }

    // The image's <home>/abi is its root's runtime (what a session binds
    // there: sandbox.cpp), so a payload bound through it runs.
    if (const auto runtime = rf::usr_of(image_default).parent_path() / std::string(rf::kRuntimeDir);
        fs::is_directory(runtime, ec)) {
        fs::create_directory_symlink(runtime.lexically_relative(image_home), image_home / "abi", ec);
        if (!check_io()) return 1;
    }
    ec.clear();

    // The root around it. An image that boots through stage-0 (a drive, an
    // ISO, a disk) has /usr as the mount point stage-0 mounts; a tarball or a
    // directory is started by another init (WSL, a container) and keeps the
    // link (design 2026-10-16 L §6.2).
    const auto usrForm = (!tarball.empty() || !rootfs_dir.empty()) ? rf::UsrForm::Link : rf::UsrForm::Mount;
    {
        const auto tree = subos_root::tree_of(home, name);
        // Machine state: /etc always (factory links become the files they
        // name -- they pointed into this home's copy of the instance), the
        // rest only when asked for.
        fs::copy(tree / "etc", stage / "etc", fs::copy_options::recursive
                 | fs::copy_options::overwrite_existing, ec);
        if (!check_io()) return 1;
        if (with_data) {
            for (auto d : {"root", "home", "var", "srv", "opt"}) {
                if (auto copied = copy_tree_(tree / d, stage / d); !copied) {
                    error_(stream, copied.error(), {}, ErrorCode::Internal);
                    return 1;
                }
            }
        }
        // After the machine's /etc: its record of what its /usr presents is
        // this host's, and the image's is the image's own default.
        if (auto l = rf::lay_out(stage, rf::usr_of(home / "subos" / "default"), home, usrForm); !l) {
            error_(stream, l.error(), {}, ErrorCode::Internal);
            return 1;
        }
        fs::create_directories(stage / "etc" / "xlings", ec);
        if (!check_io()) return 1;
        if (!write_json(stage / "etc" / "xlings" / "root.json",
                        nlohmann::json{{"home", home.generic_string()}})) return 1;
        const bool has_resolver = fs::exists(stage / "etc" / "resolv.conf", ec);
        if (!check_io()) return 1;
        if (!has_resolver) {
            std::ofstream resolver(stage / "etc" / "resolv.conf");
            resolver.close();
            if (!resolver) { error_(stream, "cannot create resolver configuration"); return 1; }
        }
    }

    int rc = 0;
    if (!tarball.empty()) {
        // In-process, root-owned entries: no host tar, no user namespace.
        if (auto written = xim::write_tar_gz(stage, scratch->path() / "output", xim::ArchiveOwner::Root);
            !written) {
            error_(stream, "writing the tarball: " + written.error(), {}, ErrorCode::Internal);
            return 1;
        }
    } else if (!disk.empty()) {
        auto mkfs = tool_("mkfs.ext4", stream);
        if (!mkfs) return 1;
        auto argv = as_root_({*mkfs, "-q", "-F", "-L", "luban", "-d", stage.string(),
                              (scratch->path() / "output").string(), size}, stream);
        if (!argv) return 1;
        rc = run_tool_(*argv, stream, "writing the disk image");
    } else if (!drive.empty() || !qcow2.empty()) {
        const auto raw = scratch->path() / (drive.empty() ? "drive.raw" : "output");
        if (auto made = write_drive_(stage, home, kernel_file, size, scratch->path(), raw, boot, name, stream); !made) {
            error_(stream, made.error().first, made.error().second);
            return 1;
        }
        if (!qcow2.empty()) {
            if (auto q = xlings::image::write_qcow2(raw, scratch->path() / "output"); !q) {
                error_(stream, "writing the qcow2: " + q.error(), {});
                return 1;
            }
        }
    } else if (!iso.empty()) {
        if (auto made = write_live_iso_(stage, home, kernel_file, scratch->path(), boot, name, stream); !made) {
            error_(stream, made.error().first, made.error().second);
            return 1;
        }
    }
    if (rc != 0) return 1;
    if (!rootfs_dir.empty()) {
        if (auto published = platform::rename_no_replace(stage, out); !published) {
            error_(stream, published.error(), {}, ErrorCode::Internal);
            return 1;
        }
    } else {
        // Publish a completed file without ever replacing an existing name.
        if (auto published = publish_file_(scratch->path() / "output", out); !published) {
            error_(stream, published.error());
            return 1;
        }
    }
    observe::append(HomeView{home}.logs_dir(name) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Lifecycle,
        .fields = {{"event", "export"}, {"instance", name}, {"to", fs::absolute(out).string()},
                   {"with_data", with_data}}});
    log::info("'{}' exported to {} (its system home: {})", name, fs::absolute(out).string(),
              home.string());
    if (!tarball.empty())
        log::info("  docker import {} luban:{}   |   wsl --import <distro> <dir> {}", out.string(), name,
                  out.string());
    if (!disk.empty())
        log::info("  boots with init={}/boot/{} root=/dev/vda", home.string(),
                  fs::exists(home / "bin" / "luban-init") ? "luban-init" : "xlings-init");
    if (!drive.empty() || !qcow2.empty())
        log::info("  a drive: `luban write {} <device>` puts it on one, `luban try {}` boots it here",
                  out.string(), out.string());
    if (!iso.empty())
        log::info("  a live system: it runs from memory; write it to a drive with `luban write {} <device>`, "
                  "try it with `luban try {}`", out.string(), out.string());
    return 0;
}

// ── pack ─────────────────────────────────────────────────────────────

int run_pack_(int argc, char* argv[], EventStream& stream, const UsageError& usageError) {
    std::string name, as;
    fs::path out_dir = ".";
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--as" && i + 1 < argc) as = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
        else { usageError("unknown option for `xlings subos pack`: " + a); return 1; }
    }
    const auto at = as.find('@');
    if (name.empty() || at == std::string::npos) {
        usageError("usage: xlings subos pack <name> --as <ns:pkg@version> [--out <dir>]");
        return 1;
    }
    if (!exists_(name)) { error_(stream, "no SubOS named '" + name + "'"); return 1; }
    auto ws = workspace_of_(name);
    if (!ws) { error_(stream, "the workspace of '" + name + "' is unreadable"); return 1; }
    const auto colon = as.find(':');
    const auto pkg = as.substr(colon == std::string::npos ? 0 : colon + 1,
                               at - (colon == std::string::npos ? 0 : colon + 1));
    const auto ver = as.substr(at + 1);
    if (!safe_component_(pkg) || !safe_component_(ver) || as.find('@', at + 1) != std::string::npos
        || (colon != std::string::npos && (colon >= at || !safe_component_(as.substr(0, colon))))) {
        usageError("--as expects a package name and version without path separators");
        return 1;
    }
    const auto home = home_dir_();
    const auto kind = subos_root::read_kind(home, name);
    if (!kind) { error_(stream, kind.error()); return 1; }
    const auto file = fs::absolute(out_dir) / std::format("{}-{}.tar.gz", pkg, ver);
    if (auto fresh = require_new_output_(file); !fresh) {
        error_(stream, fresh.error(), "choose a new package version or output directory");
        return 1;
    }
    auto scratch = OwnedStage::create(fs::absolute(out_dir));
    if (!scratch) { error_(stream, scratch.error(), {}, ErrorCode::Internal); return 1; }
    const auto stage = scratch->path() / std::format("{}-{}", pkg, ver);
    std::error_code ec;
    fs::create_directory(stage, ec);
    if (ec) { error_(stream, "cannot create package staging: " + ec.message()); return 1; }
    // What a subos-type xpkg carries: the workspace it declares and, for a
    // root, its kind. Payloads are not packed: they come from the index.
    nlohmann::json j;
    j["workspace"] = nlohmann::json::object();
    for (auto& [t, v] : ws->active) j["workspace"][t] = v;
    if (*kind == roles::Kind::Rootfs) j["subos_kind"] = "rootfs";
    {
        std::ofstream config(stage / ".xlings.json");
        config << j.dump(2) << "\n";
        config.close();
        if (!config) { error_(stream, "cannot write package configuration"); return 1; }
    }
    const auto archive = scratch->path() / "output.tar.gz";
    if (auto written = xim::write_tar_gz(stage, archive, xim::ArchiveOwner::AsOnDisk,
                                         stage.filename().string());
        !written) {
        error_(stream, "packing: " + written.error(), {}, ErrorCode::Internal);
        return 1;
    }
    if (auto published = publish_file_(archive, file); !published) {
        error_(stream, published.error());
        return 1;
    }
    log::info("packed '{}' as {}: {}", name, as, file.string());
    log::info("  a recipe: package = {{ name = \"{}\", type = \"subos\", xpm = {{ linux = {{ [\"{}\"] = "
              "{{ url = \"<where you publish it>\", sha256 = \"...\" }} }} }} }}", pkg, ver);
    return 0;
}


}  // namespace xlings::subos
