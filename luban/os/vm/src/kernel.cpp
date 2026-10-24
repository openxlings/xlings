// What a machine is started with (vm.cppm; design 2026-10-24 §7): its
// kernel and what that kernel can do, how its files are shared (virtio-fs or
// 9p), whether this host lets virtiofsd make namespaces, whether its /home
// has users of their own, and its GPU.
module luban.vm;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.subos.home_view;
import xlings.subos.located;
import xlings.subos.root_etc;
import luban.service;

namespace luban::vm {

namespace {

using Json = nlohmann::json;
namespace platform = xlings::platform;

Json read_json_(const fs::path& f) {
    std::error_code ec;
    if (!fs::is_regular_file(f, ec)) return Json::object();
    auto j = Json::parse(platform::read_file_to_string(f.string()), nullptr, false);
    return j.is_object() ? j : Json::object();
}

// A kernel package's payload: its vmlinuz and the config beside it.
std::optional<fs::path> kernel_payload_(const fs::path& home, std::string_view version) {
    std::error_code ec;
    const auto modules = home / "data" / "xpkgs" / "xim-x-linux-kernel-virt" / std::string(version) / "lib" / "modules";
    for (fs::directory_iterator it(modules, ec), end; !ec && it != end; it.increment(ec))
        if (fs::is_regular_file(it->path() / "vmlinuz", ec)) return it->path() / "vmlinuz";
    return std::nullopt;
}

// The version a kernel ref names: "xim:linux-kernel-virt@6.12.112-r3" is 6.12.112-r3.
std::string kernel_version_(std::string_view ref) {
    const auto at = ref.rfind('@');
    return at == std::string_view::npos ? std::string{} : std::string(ref.substr(at + 1));
}

}  // namespace

std::set<std::string> kernel_caps(std::string_view config) {
    std::set<std::string> on;
    for (std::size_t at = 0; at < config.size();) {
        auto end = config.find('\n', at);
        if (end == std::string_view::npos) end = config.size();
        const auto line = config.substr(at, end - at);
        at = end + 1;
        if (line.starts_with("CONFIG_") && line.ends_with("=y")) on.emplace(line.substr(7, line.size() - 9));
    }
    std::set<std::string> caps;
    const auto all = [&](std::initializer_list<std::string_view> names) {
        return std::ranges::all_of(names, [&](std::string_view n) { return on.contains(std::string(n)); });
    };
    if (all({"FUSE_FS", "VIRTIO_FS"})) caps.insert("virtio-fs");
    if (all({"NET_9P_VIRTIO", "9P_FS"})) caps.insert("virtio-9p");
    if (all({"DRM_VIRTIO_GPU"})) caps.insert("virtio-gpu");
    if (all({"UDMABUF"})) caps.insert("udmabuf");
    if (all({"PCI", "SYSFS"})) caps.insert("pci-sysfs-mmap");
    if (all({"VIRTIO_VSOCKETS"})) caps.insert("virtio-vsock");
    return caps;
}

std::expected<MachineKernel, std::string> machine_kernel(const fs::path& home, std::string_view name) {
    MachineKernel k;
    const auto settings = service::read_settings(home, name);
    if (settings && !settings->kernel.empty()) {
        k.version = settings->kernel;
        k.overridden = true;
    } else {
        // Its version's own manifest (what luban upgrade rewrites), else what
        // its edition's chain declared when it was made (instance.json).
        const auto at = xlings::subos::locate(xlings::subos::HomeView{home}, name);
        for (const auto& file : {at.home.instance(name) / ".xlings.json", at.home.instance_file(name)}) {
            const auto boot = read_json_(file).value("boot", Json::object());
            if (const auto ref = boot.is_object() ? boot.value("virt", std::string{}) : std::string{}; !ref.empty()) {
                k.version = kernel_version_(ref);
                break;
            }
        }
        if (k.version.empty())
            return std::unexpected(std::format("'{}' does not say which kernel it boots as a machine (its version is older than "
                                               "systems carrying their own)", name));
    }
    k.ref = "xim:linux-kernel-virt@" + k.version;
    if (const auto v = kernel_payload_(home, k.version)) {
        k.vmlinuz = *v;
        std::error_code ec;
        if (const auto config = v->parent_path() / "config"; fs::is_regular_file(config, ec))
            k.caps = kernel_caps(platform::read_file_to_string(config.string()));
    }
    return k;
}

bool unprivileged_userns() {
    // Ubuntu's switch says it at once; a host without it is asked.
    std::ifstream in("/proc/sys/kernel/apparmor_restrict_unprivileged_userns");
    if (std::string v; in >> v && v == "1") return false;
    return platform::userns_can_mount();
}

VirtiofsPlan virtiofs_plan(const fs::path& virtiofsd, const std::vector<Share>& shares, const fs::path& run, std::string_view memory_mb,
                           bool sandboxed) {
    VirtiofsPlan p;
    // `-m`'s size: a plain number is MiB there, bytes here.
    const bool plain = !memory_mb.empty() && std::ranges::all_of(memory_mb, [](char ch) { return ch >= '0' && ch <= '9'; });
    p.qemu = {"-object", std::format("memory-backend-memfd,id=mem,size={}{},share=on", memory_mb, plain ? "M" : ""), "-numa",
              "node,memdev=mem"};
    for (const auto& s : shares) {
        if (s.mapped) continue;   // 9p's, always
        const auto sock = (run / std::format("vfs-{}.sock", s.tag)).generic_string();
        // Its own namespaces (unprivileged); a host that refuses them
        // (unprivileged_userns, design 2026-10-24 §7.3) gets `none` at once.
        // `auto` for all: the home's generations do not change, but the host
        // installs into the same share and the machine sees it at once.
        std::vector<std::string> d{virtiofsd.generic_string(), "--socket-path", sock, "--shared-dir", s.dir.generic_string(), "--sandbox",
                                   sandboxed ? "namespace" : "none", "--cache", "auto"};
        if (s.read_only) d.push_back("--readonly");
        p.daemons.push_back(std::move(d));
        p.qemu.insert(p.qemu.end(), {"-chardev", std::format("socket,id=vfs-{},path={}", s.tag, sock), "-device",
                                     std::format("vhost-user-fs-pci,chardev=vfs-{},tag={}", s.tag, s.tag)});
    }
    return p;
}

std::vector<std::string> nine_p_args(const std::vector<Share>& shares) {
    std::vector<std::string> out;
    for (const auto& s : shares)
        out.insert(out.end(), {"-virtfs", std::format("local,path={},mount_tag={},security_model={},{}id={}", s.dir.generic_string(), s.tag,
                                                       s.mapped ? "mapped-xattr" : "none", s.read_only ? "readonly=on," : "", s.tag)});
    return out;
}

bool users_of_their_own(const fs::path& etc) {
    const auto own = xlings::subos::root_etc::login_user(etc);
    std::ifstream in(etc / "passwd");
    for (std::string line; std::getline(in, line);) {
        std::vector<std::string> f;
        for (std::size_t at = 0;;) {
            const auto colon = line.find(':', at);
            f.push_back(line.substr(at, colon == std::string::npos ? std::string::npos : colon - at));
            if (colon == std::string::npos) break;
            at = colon + 1;
        }
        if (f.size() < 7 || f[0] == own || !f[5].starts_with("/home/")) continue;
        unsigned uid = 0;
        if (std::from_chars(f[2].data(), f[2].data() + f[2].size(), uid).ec != std::errc{}) continue;
        if (uid >= 1000 && uid < 65534) return true;
    }
    return false;
}

MachineGpu machine_gpu(const std::optional<fs::path>& render_node, const std::expected<void, std::string>& gl,
                       std::string_view driver, bool dxg) {
    if (!render_node && dxg)
        return {.args = {"-display", "none"},
                .summary = "no GPU: WSL's GPU (/dev/dxg, Mesa's d3d12) has no render node, which qemu's virgl draws on -- software"};
    if (!render_node) return {.args = {"-display", "none"}, .summary = "no GPU: this host has no render node"};
    if (!gl) return {.args = {"-display", "none"}, .summary = "no GPU: " + gl.error()};
    return {.virgl = true,
            .args = {"-device", "virtio-gpu-gl-pci", "-display", std::format("egl-headless,rendernode={}", render_node->string())},
            .summary = driver.empty() ? std::format("virgl on {}", render_node->string())
                                      : std::format("virgl on {} ({})", render_node->string(), driver)};
}

}  // namespace luban::vm
