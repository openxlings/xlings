module luban.vm;

import std;
import xlings.subos.located;
import xlings.subos.root_etc;
import xlings.libs.json;
import xlings.observe;
import xlings.platform;
import xlings.platform.stream;
import xlings.image;
import xlings.platform.target;
import xlings.subos.home_view;
import xlings.subos.ports;
import xlings.subos.tools;
import xlings.carrier;
import luban.service;
import luban.vm.protocol;
import luban.vm.native.vz;

namespace luban::vm {

namespace {

// A machine's size, from its host's (design 2026-10-20 G28): a desktop draws
// with the CPU (llvmpipe, pixman), and 2 GB and 2 processors made LXQt and
// KDE slow. Half the host's processors (2 to 8), a quarter of its memory
// (2 to 8 GB); `luban config <n> --memory` still sets it.
std::string default_memory_mb_() {
    std::ifstream in("/proc/meminfo");
    std::string key;
    long long kb = 0;
    while (in >> key >> kb) {
        if (key == "MemTotal:") break;
        in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        kb = 0;
    }
    const long long quarter = kb / 1024 / 4;
    return std::to_string(std::clamp<long long>(quarter, 2048, 8192));
}
std::string default_cpus_() {
    const unsigned n = std::thread::hardware_concurrency();
    return std::to_string(std::clamp(n / 2, 2u, 8u));
}

using Json = nlohmann::json;
namespace platform = xlings::platform;
namespace proto = luban::vm::protocol;

constexpr char kEscape = 0x1d;   // Ctrl-]

fs::path state_file_(const fs::path& home, std::string_view name) { return run_dir(home, name) / "state.json"; }
fs::path client_socket_(const fs::path& home, std::string_view name) { return run_dir(home, name) / "luban.sock"; }

Json read_json_(const fs::path& f) {
    std::error_code ec;
    if (!fs::is_regular_file(f, ec)) return Json::object();
    auto j = Json::parse(platform::read_file_to_string(f.string()), nullptr, false);
    return j.is_object() ? j : Json::object();
}

void say_(const service::Context& c, service::Event e) {
    if (c.sink) c.sink(std::move(e));
}

std::string capture_(const std::vector<std::string>& argv) {
    std::string out;
    (void)platform::stream::run(argv, [&](std::string_view, std::string_view bytes) { out.append(bytes); });
    return out;
}

// The kernel a machine boots: the virt profile's (linux-kernel-virt).
std::optional<fs::path> kernel_(const fs::path& home) {
    std::error_code ec;
    const auto root = home / "data" / "xpkgs" / "xim-x-linux-kernel-virt";
    std::optional<fs::path> best;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename() == "vmlinuz" && it->is_regular_file(ec)) best = it->path();
    return best;
}

// One answer to "is there hardware virtualization here": the capability
// report's (luban status).
bool kvm_() { return service::kvm_usable(); }

void copy_tree_(const fs::path& from, const fs::path& to, const std::set<std::string>& skip, std::error_code& ec) {
    fs::create_directories(to, ec);
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (skip.contains(name)) {
            fs::create_directories(to / name, ec);
            continue;
        }
        fs::copy(it->path(), to / name, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec) return;
    }
}

// The static luban a machine runs from (its /init, its supervisor, its proxy
// relay): this binary when it is luban, else the luban beside it (the Luban OS
// app), else the home's.
fs::path luban_(const fs::path& home) {
    if (const char* forced = std::getenv("LUBAN_BIN"); forced && *forced) return forced;
    const auto self = platform::get_executable_path();
    std::error_code ec;
    if (self.filename().string().starts_with("luban") && !self.filename().string().starts_with("luban-os")) return self;
    const auto exe = std::string(platform::exe_suffix);
    for (const auto& candidate : {self.parent_path() / ("luban" + exe), home / "bin" / ("luban" + exe)})
        if (fs::is_regular_file(candidate, ec)) return candidate;
    return self;
}

// The initramfs: the static luban as /init and the mount points it needs.
std::expected<fs::path, std::string> initramfs_(const fs::path& home, std::string_view name) {
    const auto self = luban_(home);
    if (self.empty() || !is_static_elf(self))
        return std::unexpected("a machine starts from a static luban (a release build); this one needs a loader");
    const auto out = run_dir(home, name) / "initramfs.cpio";
    std::error_code ec;
    if (fs::exists(out, ec) && fs::last_write_time(out, ec) >= fs::last_write_time(self, ec)) return out;
    std::vector<xlings::image::CpioEntry> entries;
    for (const auto* d : {"dev", "proc", "sys", "newroot"})
        entries.push_back({.path = d, .kind = xlings::image::CpioEntry::Kind::Dir, .mode = 0755});
    std::ifstream in(self, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    entries.push_back({.path = "init", .kind = xlings::image::CpioEntry::Kind::File, .data = std::move(bytes), .mode = 0755});
    if (auto w = xlings::image::write_cpio(out, entries); !w) return std::unexpected(w.error());
    return out;
}

struct Network {
    std::vector<std::string> args;
    std::map<std::string, std::string> env;   // what a session inside needs to use it
    std::string summary;
};

// The machine's network is the environment's policy: its own NAT, only its
// proxy, or none.
Network network_(const service::Context& c, std::string_view name) {
    Network n{.args = {"-nic", "user"}, .summary = "network NAT"};
    auto iso = c.port.json({"subos", "config", std::string(name), "--json"});
    if (!iso || !iso->is_object()) return n;
    const auto in = iso->value("isolation", Json::object());
    const auto net = in.value("net", std::string("host"));
    if (net == "none") return {.args = {"-nic", "none"}, .summary = "no network"};
    if (net == "proxy") {
        const auto url = in.value("proxy", std::string{});
        const auto at = url.find("://");
        const auto endpoint = at == std::string::npos ? url : url.substr(at + 3);
        const auto colon = endpoint.rfind(':');
        const auto self = luban_(c.home).string();
        if (colon == std::string::npos || self.find(',') != std::string::npos) return {.args = {"-nic", "none"}, .summary = "no network (its proxy cannot be reached)"};
        n.args = {"-nic", std::format("user,restrict=on,guestfwd=tcp:10.0.2.100:1080-cmd:{} __pipe {} {}", self,
                                      endpoint.substr(0, colon), endpoint.substr(colon + 1))};
        for (const auto* k : {"ALL_PROXY", "all_proxy", "HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy"})
            n.env[k] = "socks5h://10.0.2.100:1080";
        n.summary = "only its proxy";
    } else {
        // Like the host: the host's proxy, its loopback the host's NAT address
        // (design 2026-10-19 §27.3).
        const auto host = platform::environment();
        n.env = service::host_proxy_env(host, service::kQemuGateway, service::kQemuNat);
        if (!n.env.empty()) n.summary += " · " + service::network_line(host, n.env).substr(std::string_view("like the host · ").size());
    }
    // Sound (design §9.4): with the audio grant, the guest's Pulse clients
    // reach this host's PipeWire / PulseAudio socket through a forwarded port.
    bool audio = false;
    if (auto g = in.find("grants"); g != in.end() && g->is_array())
        audio = std::ranges::any_of(*g, [](const Json& v) { return v.is_string() && v.get<std::string>() == "audio"; });
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const auto pulse = runtime ? fs::path(runtime) / "pulse" / "native" : fs::path{};
    if (auto sound = sound_forward(audio, pulse, luban_(c.home)); sound && n.args.size() == 2 && n.args[1].starts_with("user")) {
        n.args[1] += "," + sound->guestfwd;
        n.env["PULSE_SERVER"] = sound->pulse_server;
        n.summary += ", sound";
    }
    return n;
}

int connect_client_(const fs::path& home, std::string_view name) {
    return platform::unix_connect_stream(client_socket_(home, name));
}

// Reads frames from `fd` until `want` arrives or `deadline` passes; other
// frames are kept for the caller.
std::optional<proto::Frame> wait_for_(int fd, proto::Decoder& d, char want, std::chrono::steady_clock::time_point deadline,
                                      const std::function<void()>& tick = {}) {
    std::array<char, 65536> buf{};
    while (std::chrono::steady_clock::now() < deadline) {
        while (auto f = d.next()) {
            if (!*f) break;
            if ((*f)->type == want) return std::move(**f);
        }
        std::array<platform::PollFd, 1> p{{{.fd = fd}}};
        if (platform::poll_fds(p, 500) > 0) {
            if (p[0].closed && !p[0].readable) return std::nullopt;
            auto n = platform::read_some(fd, buf);
            if (!n || *n == 0) return std::nullopt;
            d.feed(std::string_view(buf.data(), *n));
        }
        if (tick) tick();
    }
    return std::nullopt;
}

std::string tail_(const fs::path& file, std::size_t lines) {
    std::ifstream in(file);
    std::deque<std::string> last;
    for (std::string l; std::getline(in, l);) {
        last.push_back(l);
        if (last.size() > lines) last.pop_front();
    }
    std::string out;
    for (const auto& l : last) out += "\n    " + l;
    return out;
}

// The first render node this host has (/dev/dri/renderD128 on).
std::optional<fs::path> render_node_() {
    std::error_code ec;
    for (int n = 128; n < 192; ++n)
        if (const fs::path p = std::format("/dev/dri/renderD{}", n); fs::exists(p, ec)) return p;
    return std::nullopt;
}

// Its kernel driver (nvidia, amdgpu, i915), as sysfs names it; empty when unread.
std::string render_driver_(const fs::path& node) {
    std::error_code ec;
    const auto link = fs::read_symlink(fs::path("/sys/class/drm") / node.filename() / "device" / "driver", ec);
    return ec ? std::string{} : link.filename().string();
}

// The GL qemu (WuXian §7.5) and what its graphics stack declares -- the EGL
// vendors, GBM backends and DRI drivers of the SubOS it is installed in (the
// Port's subos_env), which qemu needs to find the host's driver.
struct GlQemu {
    fs::path bin;
    Json env = Json::array();   // [{var, op, value}]
};

// Found, or installed the first time a machine may draw with this host's GPU
// (T1, as the qemu is). Why not, said as the machine's state says it.
std::expected<GlQemu, std::string> gl_qemu_(const service::Context& c, const xlings::subos::HomeView& view) {
    auto found = xlings::subos::tools::first("qemu-system-gl", view, xlings::subos::Ports{});
    if (!found) {
        say_(c, service::phase("gpu", "begin", "installing 3D for machines (qemu with OpenGL)"));
        const int rc = c.port.capability("install_packages", {{"targets", Json::array({"xim:qemu-system-gl"})}, {"yes", true}}, c.sink);
        found = xlings::subos::tools::first("qemu-system-gl", view, xlings::subos::Ports{});
        say_(c, service::phase("gpu", found ? "done" : "failed", "installing 3D for machines (qemu with OpenGL)"));
        if (!found)
            return std::unexpected(std::format("xim:qemu-system-gl {} (xlings install qemu-system-gl)",
                                               rc != 0 ? "could not be installed" : "is not installed"));
    }
    if (capture_({found->bin.string(), "-M", "none", "-device", "help"}).find("\"virtio-gpu-gl-pci\"") == std::string::npos)
        return std::unexpected(std::format("{} has no virtio-gpu-gl-pci", found->bin.string()));
    GlQemu gl{found->bin};
    (void)c.port.capability("subos_env", Json::object(), [&](const service::Event& e) {
        if (e.kind == service::Event::Kind::Data && e.data_kind == "subos_env" && e.data.is_object())
            gl.env = e.data.value("vars", Json::array());
    });
    return gl;
}

// What the supervisor runs qemu with: this environment and what the GL qemu's
// stack declares (`prepend` before the value here, `set` only when unset, as
// `subos use` applies them).
std::map<std::string, std::string> qemu_env_(const Json& declared) {
    auto env = platform::environment();
    for (const auto& v : declared) {
        if (!v.is_object()) continue;
        const auto var = v.value("var", std::string{});
        const auto value = v.value("value", std::string{});
        if (var.empty() || value.empty()) continue;
        if (v.value("op", std::string{}) == "prepend") env[var] = platform::prepend_to_list(var, value, env[var], ':');
        else env.try_emplace(var, value);
    }
    return env;
}

}  // namespace

VirtiofsPlan virtiofs_plan(const fs::path& virtiofsd, const std::vector<Share>& shares, const fs::path& run, std::string_view memory_mb) {
    VirtiofsPlan p;
    // `-m`'s size: a plain number is MiB there, bytes here.
    const bool plain = !memory_mb.empty() && std::ranges::all_of(memory_mb, [](char ch) { return ch >= '0' && ch <= '9'; });
    p.qemu = {"-object", std::format("memory-backend-memfd,id=mem,size={}{},share=on", memory_mb, plain ? "M" : ""), "-numa",
              "node,memdev=mem"};
    for (const auto& s : shares) {
        const auto sock = (run / std::format("vfs-{}.sock", s.tag)).generic_string();
        // Its own namespaces (unprivileged); a host that refuses them gets `none` (supervise).
        // `auto` for all: the home's generations do not change, but the host
        // installs into the same share and the machine sees it at once.
        std::vector<std::string> d{virtiofsd.generic_string(), "--socket-path", sock, "--shared-dir", s.dir.generic_string(), "--sandbox", "namespace",
                                   "--cache", "auto"};
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
        out.insert(out.end(), {"-virtfs", std::format("local,path={},mount_tag={},security_model=none,{}id={}", s.dir.generic_string(), s.tag,
                                                       s.read_only ? "readonly=on," : "", s.tag)});
    return out;
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

namespace {

// Who it is and when, before it boots (part 2 J43): its machine-id, and this
// computer's time zone -- a machine cannot read the host's /etc.
void stamp_identity_(const service::Context& c, std::string_view name) {
    if (const auto root = service::root_of(c.home, name))
        for (auto written : {xlings::subos::root_etc::ensure_machine_id(*root / "etc"),
                             xlings::subos::root_etc::ensure_localtime(*root / "etc")})
            if (!written) xlings::observe::ops::event("identity", {{"system", std::string(name)}, {"error", written.error()}});
}

// Starts the machine's supervisor and waits for its agent.
int start_impl_(const service::Context& c, std::string_view name, const EnterOptions& o) {
    const auto& arch = xlings::image::native_arch();
    const xlings::subos::HomeView view{c.home};
    stamp_identity_(c, name);
    if (arch.qemu.empty()) {
        say_(c, service::error(std::format("a machine runs on x86_64 and aarch64 hosts (this one is {})", platform::build_arch())));
        return 1;
    }
    say_(c, service::phase("preflight", "begin", "checking this host"));
    // In the table's order, the first that can share a directory with the
    // machine (9p, asked of the device itself: its properties name the fsdev
    // it shares) -- a host's qemu without it does not hide the index's.
    auto all = xlings::subos::tools::candidates(arch.qemu, view, xlings::subos::Ports{});
    if (all.empty()) {
        // Its monitor is a package of the index, installed the first time as
        // its kernel is (a Windows PC's host machine has nobody to type it).
        say_(c, service::phase("preflight", "done", "checking this host"));
        say_(c, service::phase("vmm", "begin", "installing the virtual machine monitor"));
        const int rc = c.port.capability("install_packages", {{"targets", Json::array({"xim:qemu-system"})}, {"yes", true}}, c.sink);
        all = xlings::subos::tools::candidates(arch.qemu, view, xlings::subos::Ports{});
        say_(c, service::phase("vmm", all.empty() ? "failed" : "done", "installing the virtual machine monitor"));
        if (rc != 0 || all.empty()) {
            say_(c, service::error("a machine needs a virtual machine monitor here", xlings::subos::tools::install_hint(arch.qemu)));
            return 1;
        }
        say_(c, service::phase("preflight", "begin", "checking this host"));
    }
    std::optional<xlings::subos::tools::Found> qemu;
    std::string refused;
    for (const auto& found : all) {
        const auto probe = capture_({found.bin.string(), "-M", "none", "-device", "virtio-9p-pci,help"});
        if (probe.find("fsdev") != std::string::npos) { qemu = found; break; }
        if (refused.empty()) {
            const auto first = probe.substr(0, probe.find('\n'));
            refused = std::format("{} cannot share a directory with a machine (no 9p{})", found.bin.string(),
                                  first.empty() ? std::string{} : ": " + first);
        }
    }
    if (!qemu) {
        say_(c, service::phase("preflight", "failed", "checking this host"));
        say_(c, service::error(refused, "xlings install qemu-system@latest (revision 3 or later)"));
        return 1;
    }
    auto kernel = kernel_(c.home);
    if (!kernel) {
        say_(c, service::phase("preflight", "done", "checking this host"));
        say_(c, service::phase("kernel", "begin", "installing the machine kernel"));
        const int rc = c.port.capability("install_packages", {{"targets", Json::array({"xim:linux-kernel-virt"})}, {"yes", true}}, c.sink);
        kernel = kernel_(c.home);
        say_(c, service::phase("kernel", kernel ? "done" : "failed", "installing the machine kernel"));
        if (rc != 0 || !kernel) return rc != 0 ? rc : 1;
    } else {
        say_(c, service::phase("preflight", "done", "checking this host"));
    }
    std::error_code ec;
    const auto dir = run_dir(c.home, name);
    fs::create_directories(dir, ec);
    fs::create_directories(log_dir(c.home, name), ec);
    auto initrd = initramfs_(c.home, name);
    if (!initrd) {
        say_(c, service::error(initrd.error(), "use a release luban: xlings install xlings@latest"));
        return 1;
    }
    auto root = service::root_of(c.home, name);
    if (!root) {
        say_(c, service::error(std::format("'{}' is a view of this host: it has no root of its own to boot", name),
                               "an environment from an edition is a root: luban new <name> --edition tiny"));
        return 2;
    }
    // The home the root's links name: a system in a prefix domain sees its
    // domain's home at the logical path (/xlings); the files are elsewhere.
    const auto at = xlings::subos::locate(xlings::subos::HomeView{c.home}, name);
    const auto home = at.logical.lexically_normal().string();
    const auto home_files = at.home.home.lexically_normal().string();
    if (home_files.find(',') != std::string::npos || root->string().find(',') != std::string::npos) {
        say_(c, service::error("a machine cannot share a path with a comma in it", c.home.string()));
        return 1;
    }
    auto settings = service::read_settings(c.home, name);
    const auto persist = o.temp ? std::string("none") : settings ? settings->persist : std::string("all");
    // What the machine writes: the environment's own tree, or a copy of it that
    // goes when the machine does (persist none: all of it; home: all but /home).
    fs::path share = *root;
    bool userhome = false;
    if (persist != "all") {
        say_(c, service::phase("copy", "begin", persist == "home" ? "a fresh copy of its system" : "a fresh copy of it"));
        share = dir / "scratch";
        fs::remove_all(share, ec);
        ec.clear();
        copy_tree_(*root, share, persist == "home" ? std::set<std::string>{"home"} : std::set<std::string>{}, ec);
        say_(c, service::phase("copy", ec ? "failed" : "done", persist == "home" ? "a fresh copy of its system" : "a fresh copy of it"));
        if (ec) {
            say_(c, service::error(std::format("cannot copy {}: {}", root->string(), ec.message())));
            return 1;
        }
        userhome = persist == "home";
    }
    const auto net = network_(c, name);
    // What its init and its login shells read for this boot (/run/luban/network.env,
    // design 2026-10-19 §27.3): under the home it mounts, never in its tree.
    {
        const auto file = dir / "network.env";
        fs::remove(file, ec);
        ec.clear();
        std::string text;
        for (const auto& [k, v] : net.env)
            if (!k.starts_with("PULSE")) text += std::format("{}={}\n", k, v);
        if (!text.empty()) {
            platform::write_file_atomic(file.string(), text);
            fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, ec);
        }
    }
    const bool kvm = kvm_();
    const auto memory = o.memory.empty() ? default_memory_mb_() : o.memory;
    // Its GPU (WuXian §7.5): virgl -- the GL qemu, drawing on this host's
    // render node -- for a system that may draw with this computer's GPU
    // (the `gpu` grant, a desktop's); otherwise the qemu above, and no GPU.
    const auto node = render_node_();
    std::expected<GlQemu, std::string> gl = std::unexpected(std::string("it may not use this computer's GPU (luban config ") +
                                                            std::string(name) + " --allow gpu)");
    if (node && arch.name == "x86_64" && service::gpu_granted(c.home, name)) gl = gl_qemu_(c, view);
    else if (node && arch.name != "x86_64") gl = std::unexpected(std::string("3D in a machine is on x86_64 hosts"));
    const auto gpu = machine_gpu(node, gl ? std::expected<void, std::string>{} : std::unexpected(gl.error()),
                                 node ? render_driver_(*node) : std::string{}, !node && [] { std::error_code ec; return fs::exists("/dev/dxg", ec); }());
    std::vector<std::string> q{gpu.virgl ? gl->bin.string() : qemu->bin.string()};
    if (!arch.machine.empty()) q.insert(q.end(), {"-M", std::string(arch.machine)});
    q.insert(q.end(), {"-m", memory, "-smp", default_cpus_()});
    if (kvm) q.insert(q.end(), {"-enable-kvm", "-cpu", "host"});
    else if (!arch.cpu.empty()) q.insert(q.end(), {"-cpu", std::string(arch.cpu)});
    const std::string console = arch.name == "aarch64" ? "ttyAMA0" : "ttyS0";
    q.insert(q.end(), {"-kernel", kernel->string(), "-initrd", initrd->string(), "-append",
                       std::format("console={} quiet panic=-1 luban.home={} luban.name={} luban.userhome={}{}", console, home, name,
                                   userhome ? 1 : 0,
                                   // Its network is the kernel's own DHCP on the NAT, and its
                                   // name the system's (design 2026-10-19 §27.2): nothing in
                                   // the system has to bring either up, whatever its init is.
                                   net.args.size() == 2 && net.args[1] != "none" ? std::format(" ip=::::{}::dhcp", name) : std::string{}),
                       "-no-reboot",
                       "-serial", std::format("file:{}", (log_dir(c.home, name) / "console.log").string()),
                       "-device", "virtio-serial-pci",
                       "-chardev", std::format("socket,id=agent,path={},server=on,wait=off", (dir / "agent.sock").string()),
                       "-device", std::format("virtserialport,chardev=agent,name={}", proto::kPort),
                       "-qmp", std::format("unix:{},server=on,wait=off", (dir / "qmp.sock").string())});
    // Its files (design 2026-10-21 §4): 9p, or virtio-fs when asked and its
    // daemon is here (installed the first time); the 9p arguments kept, the
    // supervisor's way back should a daemon not start.
    std::vector<Share> shares{{"root", share, false}, {"home", home_files, true}};
    if (userhome) shares.push_back({"userhome", *root / "home", false});
    const auto nine_p = nine_p_args(shares);
    std::optional<VirtiofsPlan> vfs;
    std::string files = "9p";
    // An emulated machine (no KVM) stays on 9p: programs read over
    // vhost-user crashed its init (aarch64 TCG, CI), and the speed is KVM's.
    const bool want_virtiofs = !settings || settings->files == "virtiofs";
    if (want_virtiofs && !kvm) files = "9p (an emulated machine shares its files over 9p)";
    if (want_virtiofs && kvm) {
        auto found = xlings::subos::tools::first("virtiofsd", view, xlings::subos::Ports{});
        // Installed the first time; one that could not be (an index without
        // it, a host it is not built for) is tried again a day later, not at
        // every start.
        const auto tried = log_dir(c.home, name) / "virtiofsd-install-failed";
        const auto recently = [&] {
            std::error_code e;
            const auto t = fs::last_write_time(tried, e);
            return !e && fs::file_time_type::clock::now() - t < std::chrono::hours(24);
        };
        if (!found && !recently()) {
            say_(c, service::phase("files", "begin", "installing virtio-fs for machines"));
            (void)c.port.capability("install_packages", {{"targets", Json::array({"xim:virtiofsd"})}, {"yes", true}}, c.sink);
            found = xlings::subos::tools::first("virtiofsd", view, xlings::subos::Ports{});
            say_(c, service::phase("files", found ? "done" : "failed", "installing virtio-fs for machines"));
            if (!found) platform::write_file_atomic(tried.string(), xlings::observe::utc_now() + "\n");
        }
        if (found) {
            vfs = virtiofs_plan(found->bin, shares, dir, memory);
            files = "virtiofs";
        } else {
            files = "9p (virtiofsd is not installed: xlings install virtiofsd)";
        }
    }
    if (vfs) {
        q.insert(q.end(), vfs->qemu.begin(), vfs->qemu.end());
        // The guest mounts its shares with virtio-fs: on the kernel's command line.
        for (std::size_t k = 0; k + 1 < q.size(); ++k)
            if (q[k] == "-append") { q[k + 1] += " luban.fs=virtiofs"; break; }
    } else {
        q.insert(q.end(), nine_p.begin(), nine_p.end());
    }
    // Without its GPU, should the GL qemu not start (its driver refuses the
    // render node): the same machine, drawn in software (supervise).
    auto software = q;
    software.front() = qemu->bin.string();
    software.insert(software.end(), {"-display", "none"});
    software.insert(software.end(), net.args.begin(), net.args.end());
    q.insert(q.end(), gpu.args.begin(), gpu.args.end());
    q.insert(q.end(), net.args.begin(), net.args.end());
    Json state{{"name", name}, {"qemu", q}, {"persist", persist}, {"temp", o.temp}, {"scratch", persist == "all" ? "" : share.string()},
               {"env", net.env}, {"started", xlings::observe::utc_now()}, {"accel", kvm ? "kvm" : "tcg"}, {"memory", memory},
               {"gpu", gpu.summary}, {"files", files}};
    if (vfs) {
        // Its way back, should a daemon not start: the same arguments, 9p's instead.
        state["virtiofsd"] = vfs->daemons;
        state["files_virtiofs"] = vfs->qemu;
        state["files_9p"] = nine_p;
    }
    if (gpu.virgl) {
        state["gpu_env"] = gl->env;
        state["qemu_software"] = software;
    }
    platform::write_file_atomic(state_file_(c.home, name).string(), state.dump(2));
    fs::remove(client_socket_(c.home, name), ec);
    const auto self = luban_(c.home).string();
    const auto started = std::chrono::steady_clock::now();
    const auto text = std::format("starting {} ({} MB, {}, {})", name, memory, kvm ? "hardware virtualization" : "emulated, slowly", net.summary);
    say_(c, service::phase("boot", "begin", text));
    auto pid = platform::spawn_detached({self, "__vm-supervise", c.home.string(), std::string(name)},
                                        log_dir(c.home, name) / "supervisor.log");
    if (!pid) {
        say_(c, service::phase("boot", "failed", text));
        say_(c, service::error("the machine's supervisor could not be started"));
        return 1;
    }
    // Up when its agent answers through the supervisor. Meanwhile, how far it
    // is (design 2026-10-20 G23): the time against its last boot's (or a
    // first guess), and the last line its console printed.
    const auto deadline = started + std::chrono::seconds(kvm ? 90 : 300);
    const auto timing = log_dir(c.home, name) / "boot-ms";
    double expected_ms = kvm ? 20000 : 90000;
    if (std::ifstream in(timing); in) in >> expected_ms;
    expected_ms = std::clamp(expected_ms, 2000.0, 300000.0);
    auto said_at = started;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!platform::process_alive(*pid)) break;
        if (const auto now = std::chrono::steady_clock::now(); now - said_at >= std::chrono::milliseconds(800)) {
            said_at = now;
            const double spent = std::chrono::duration<double, std::milli>(now - started).count();
            auto line = tail_(log_dir(c.home, name) / "console.log", 1);
            while (!line.empty() && (line.front() == '\n' || line.front() == ' ')) line.erase(line.begin());
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (line.size() > 120) line = line.substr(0, 117) + "...";
            say_(c, service::Event{.kind = service::Event::Kind::Progress, .phase = "boot",
                                   .text = line.empty() ? text : line, .percent = std::min(0.95, spent / expected_ms)});
        }
        const int fd = connect_client_(c.home, name);
        if (fd >= 0) {
            proto::Decoder d;
            (void)platform::write_all(fd, proto::encode('Q', 0));
            auto hello = wait_for_(fd, d, 'H', deadline, [&] { (void)platform::write_all(fd, proto::encode('Q', 0)); });
            std::vector<int> close{fd};
            platform::close_fds(close);
            if (hello) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                say_(c, service::phase("boot", "done", std::format("{} is up ({:.1f} s)", name, ms / 1000.0)));
                std::ofstream(timing, std::ios::trunc) << ms;
                return 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    say_(c, service::phase("boot", "failed", text));
    say_(c, service::error(std::format("{} did not come up; the end of its console:{}", name,
                                       tail_(log_dir(c.home, name) / "console.log", 15)),
                           std::format("the whole of it: {}", (log_dir(c.home, name) / "console.log").string())));
    if (platform::process_alive(*pid)) platform::send_signal(*pid, platform::sig::terminate);
    return 1;
}

// Killed: its shell ended by a signal -- the machine powering itself off.
enum class Ending { Exited, Killed, Detached, PowerOff, Lost };

// One session of a running machine on this terminal.
// What the welcome says of a machine that only its host knows: the kernel
// package it boots, how it is accelerated, its size.
service::WelcomeFacts machine_facts_(const service::Context& c, std::string_view name) {
    service::WelcomeFacts f;
    if (const auto k = kernel_(c.home)) {
        for (auto it = k->begin(); it != k->end(); ++it)
            if (it->string().find("linux-kernel") != std::string::npos && std::next(it) != k->end()) {
                f.kernel = "Linux " + std::next(it)->string();
                break;
            }
    }
    const auto state = read_json_(state_file_(c.home, name));
    int mb = 2048;
    try { mb = std::stoi(state.value("memory", std::string("2048"))); } catch (...) {}
    f.machine = std::format("{} · 2 CPU · {}{}", state.value("accel", "kvm") == "kvm" ? "KVM" : "emulated",
                            mb % 1024 == 0 ? std::format("{} GiB", mb / 1024) : std::format("{} MiB", mb),
                            state.value("gpu", std::string{}).starts_with("virgl") ? " · GPU (virgl)" : "");
    // Its network as it was started (design 2026-10-19 §27.2).
    if (const auto started = started_env(c.home, name)) f.network = service::network_line(platform::environment(), *started);
    return f;
}

std::pair<Ending, int> session_(const service::Context& c, std::string_view name, std::string_view user = {}) {
    const int fd = connect_client_(c.home, name);
    if (fd < 0) return {Ending::Lost, 1};
    const auto state = read_json_(state_file_(c.home, name));
    const char* term = std::getenv("TERM");
    auto size = platform::terminal_size(0).value_or(std::pair<unsigned short, unsigned short>{24, 80});
    // Where it is, as a sandbox says it (part 3 §7.2): its kind, what it
    // keeps, the prompt; the root it is, for what lists its siblings.
    auto env = state.value("env", Json::object());
    const auto persist = state.value("persist", std::string("all"));
    const char* no_color = std::getenv("NO_COLOR");
    env["LUBAN_KIND"] = "machine";
    env["LUBAN_PERSIST"] = persist;
    env["XLINGS_ROOT_INSTANCE"] = std::string(name);
    env["PS1"] = xlings::subos::root_etc::prompt("machine", persist, term ? term : "xterm-256color", no_color && *no_color);
    Json open{{"rows", size.first}, {"cols", size.second}, {"term", term ? term : "xterm-256color"}, {"env", env}};
    if (!user.empty()) open["user"] = std::string(user);
    if (!platform::write_all(fd, proto::encode_json('O', 1, open))) return {Ending::Lost, 1};
    const auto saved = platform::make_terminal_raw(0);

    proto::Decoder d;
    std::array<char, 65536> buf{};
    bool escape = false;
    Ending ending = Ending::Lost;
    int code = 0;
    auto last_resize = std::chrono::steady_clock::now();
    for (bool done = false; !done;) {
        std::array<platform::PollFd, 2> p{{{.fd = 0}, {.fd = fd}}};
        platform::poll_fds(p, 100);
        if (p[1].readable || p[1].closed) {
            auto n = platform::read_some(fd, buf);
            if (!n || *n == 0) { ending = Ending::Lost; break; }
            d.feed(std::string_view(buf.data(), *n));
            for (;;) {
                auto f = d.next();
                if (!f || !*f) break;
                const auto& frame = **f;
                if (frame.type == 'D') (void)platform::write_all(1, frame.payload);
                else if (frame.type == 'X') {
                    auto j = Json::parse(frame.payload, nullptr, false);
                    code = j.is_object() ? j.value("code", 0) : 0;
                    ending = j.is_object() && j.contains("signal") ? Ending::Killed : Ending::Exited;
                    done = true;
                }
            }
        }
        if (p[0].readable) {
            auto n = platform::read_some(0, buf);
            if (!n || *n == 0) { ending = Ending::Detached; break; }
            std::string out;
            for (std::size_t k = 0; k < *n; ++k) {
                const char ch = buf[k];
                if (escape) {
                    escape = false;
                    if (ch == 'd' || ch == 'D') { ending = Ending::Detached; done = true; break; }
                    if (ch == 'p' || ch == 'P') { ending = Ending::PowerOff; done = true; break; }
                    out += kEscape;
                    if (ch != kEscape) out += ch;
                    continue;
                }
                if (ch == kEscape) { escape = true; continue; }
                out += ch;
            }
            if (!out.empty()) (void)platform::write_all(fd, proto::encode('D', 1, out));
        }
        if (std::chrono::steady_clock::now() - last_resize > std::chrono::milliseconds(100)) {
            last_resize = std::chrono::steady_clock::now();
            if (auto now = platform::terminal_size(0); now && *now != size) {
                size = *now;
                (void)platform::write_all(fd, proto::encode_json('R', 1, Json{{"rows", size.first}, {"cols", size.second}}));
            }
        }
    }
    if (saved) platform::restore_terminal(0, *saved);
    std::println("");
    std::vector<int> close{fd};
    platform::close_fds(close);
    return {ending, code};
}

int boot_image_(const service::Context& c, const fs::path& image, const EnterOptions& o) {
    const auto& arch = xlings::image::native_arch();
    const xlings::subos::HomeView view{c.home};
    auto qemu = xlings::subos::tools::first(arch.qemu, view, xlings::subos::Ports{});
    if (!qemu) {
        say_(c, service::error("booting an image needs a virtual machine monitor here", xlings::subos::tools::install_hint(arch.qemu)));
        return 1;
    }
    auto firmware = xlings::subos::tools::payload_file(view, "edk2-firmware", fs::path("share/edk2") / arch.firmware);
    if (!firmware)
        for (const auto& t : xlings::subos::tools::known())
            if (t.name == arch.qemu && !t.package.empty())
                firmware = xlings::subos::tools::payload_file(view, t.package, fs::path("share/qemu") / arch.firmware);
    if (arch.firmware_required && !firmware) {
        say_(c, service::error(std::format("booting on {} needs UEFI firmware", arch.name), "xlings install edk2-firmware"));
        return 1;
    }
    std::error_code ec;
    const auto dir = fs::temp_directory_path() / std::format("luban-image-{}", platform::get_pid());
    fs::create_directories(dir, ec);
    const auto console = dir / "console.sock";
    std::vector<std::string> q{qemu->bin.string()};
    if (!arch.machine.empty()) q.insert(q.end(), {"-M", std::string(arch.machine)});
    q.insert(q.end(), {"-m", o.memory.empty() ? default_memory_mb_() : o.memory, "-smp", default_cpus_()});
    if (kvm_()) q.insert(q.end(), {"-enable-kvm", "-cpu", "host"});
    else if (!arch.cpu.empty()) q.insert(q.end(), {"-cpu", std::string(arch.cpu)});
    if (arch.firmware_required) q.insert(q.end(), {"-bios", firmware->string()});
    const auto ext = image.extension().string();
    if (ext == ".iso" && arch.ide_cdrom) q.insert(q.end(), {"-cdrom", image.string(), "-boot", "d"});
    else if (ext == ".iso")
        q.insert(q.end(), {"-device", "virtio-scsi-pci,id=scsi", "-device", "scsi-cd,drive=cd", "-drive",
                           std::format("if=none,id=cd,media=cdrom,readonly=on,format=raw,file={}", image.string())});
    else q.insert(q.end(), {"-drive", std::format("file={},format={},if=virtio,snapshot=on", image.string(), ext == ".qcow2" ? "qcow2" : "raw")});
    q.insert(q.end(), {"-display", "none", "-no-reboot", "-nic", "user",
                       "-chardev", std::format("socket,id=con,path={},server=on,wait=off", console.string()),
                       "-serial", "chardev:con"});
    say_(c, service::phase("boot", "begin", std::format("booting {}", image.filename().string())));
    auto pid = platform::spawn_detached(q, dir / "qemu.log");
    if (!pid) {
        say_(c, service::phase("boot", "failed", std::format("booting {}", image.filename().string())));
        return 1;
    }
    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; ++i) {
        fd = platform::unix_connect_stream(console);
        if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (fd < 0) {
        say_(c, service::phase("boot", "failed", std::format("booting {}", image.filename().string())));
        say_(c, service::error("its console never opened", tail_(dir / "qemu.log", 10)));
        platform::send_signal(*pid, platform::sig::terminate);
        return 1;
    }
    say_(c, service::phase("boot", "done", std::format("{}: its console is this terminal; Ctrl-] q ends it", image.filename().string())));
    const auto saved = platform::make_terminal_raw(0);
    std::array<char, 65536> buf{};
    bool escape = false;
    bool input = true;   // standard input that ends (not a terminal) only stops being read
    for (bool done = false; !done && platform::process_alive(*pid);) {
        std::array<platform::PollFd, 2> p{{{.fd = input ? 0 : -1}, {.fd = fd}}};
        platform::poll_fds(p, 200);
        if (p[1].readable || p[1].closed) {
            auto n = platform::read_some(fd, buf);
            if (!n || *n == 0) break;
            (void)platform::write_all(1, std::string_view(buf.data(), *n));
        }
        if (input && p[0].readable) {
            auto n = platform::read_some(0, buf);
            if (!n || *n == 0) {
                if (saved) break;
                input = false;
                continue;
            }
            std::string out;
            for (std::size_t k = 0; k < *n; ++k) {
                if (escape) {
                    escape = false;
                    if (buf[k] == 'q' || buf[k] == 'p' || buf[k] == 'd') { done = true; break; }
                    out += kEscape;
                }
                if (buf[k] == kEscape) { escape = true; continue; }
                out += buf[k];
            }
            if (!out.empty()) (void)platform::write_all(fd, out);
        }
    }
    if (saved) platform::restore_terminal(0, *saved);
    std::println("");
    std::vector<int> close{fd};
    platform::close_fds(close);
    if (platform::process_alive(*pid)) platform::send_signal(*pid, platform::sig::terminate);
    fs::remove_all(dir, ec);
    return 0;
}

bool ask_(std::string_view question) {
    std::print("{}", question);
    std::cout.flush();
    std::string answer;
    std::getline(std::cin, answer);
    return !answer.empty() && (answer[0] == 'p' || answer[0] == 'P');
}

}  // namespace

fs::path run_dir(const fs::path& home, std::string_view name) { return home / "run" / "machines" / name; }
fs::path relayed_marker_(const fs::path& home, std::string_view name) { return run_dir(home, name) / "in-host-machine"; }

std::optional<SoundForward> sound_forward(bool granted, const fs::path& pulse, const fs::path& luban) {
    std::error_code ec;
    if (!granted || pulse.empty() || !fs::exists(pulse, ec)) return std::nullopt;
    if (pulse.string().find(',') != std::string::npos || luban.string().find(',') != std::string::npos) return std::nullopt;
    return SoundForward{std::format("guestfwd=tcp:10.0.2.101:4713-cmd:{} __pipe-unix {}", luban.string(), pulse.string()),
                        "tcp:10.0.2.101:4713"};
}

int start_vz_(const service::Context& c, std::string_view name, const EnterOptions& o);

// On a PC: the luban in the Luban host machine starts it there (its KVM),
// its events relayed here as they come.
int start_in_carrier_(const service::Context& c, std::string_view name, const EnterOptions& o) {
    const auto caps = service::capabilities(service::gather(c.home, c.port));
    const auto* m = caps.mode("machine");
    if (!m || !m->ready) {
        say_(c, service::error(std::format("{} cannot run as a machine on this computer: {}", name,
                                           m ? m->reason : std::string("this computer did not say")), m ? m->route : std::string{}));
        return 1;
    }
    const auto* carrier = xlings::carrier::find("wsl2");
    auto at = carrier ? carrier->ensure(xlings::subos::HomeView{c.home}) : std::unexpected(std::string("no wsl2 carrier"));
    if (!at) {
        say_(c, service::error("the Luban host machine did not start: " + at.error(), "luban status"));
        return 1;
    }
    auto argv = at->launcher;
    argv.insert(argv.end(), {"/xlings/bin/luban", "--json", "__machine-start", std::string(name)});
    if (o.temp) argv.push_back("--temp");
    std::string pending;
    const int rc = platform::stream::run(argv, [&](std::string_view stream, std::string_view bytes) {
        if (stream != "stdout") return;
        pending.append(bytes);
        for (auto nl = pending.find('\n'); nl != std::string::npos; nl = pending.find('\n')) {
            const auto j = Json::parse(pending.substr(0, nl), nullptr, false);
            pending.erase(0, nl + 1);
            if (!j.is_object() || !j.contains("event")) continue;
            service::Event e;
            const auto kind = j.value("event", std::string{});
            e.kind = kind == "phase" ? service::Event::Kind::Phase : kind == "progress" ? service::Event::Kind::Progress
                   : kind == "error" ? service::Event::Kind::Error : kind == "notice" ? service::Event::Kind::Notice
                   : kind == "data" ? service::Event::Kind::Data : service::Event::Kind::Log;
            e.phase = j.value("phase", std::string{});
            e.state = j.value("state", std::string{});
            e.text = j.value("text", std::string{});
            e.hint = j.value("hint", std::string{});
            e.level = j.value("level", std::string{});
            if (j.contains("percent") && j["percent"].is_number()) e.percent = j["percent"].get<double>();
            say_(c, std::move(e));
        }
    });
    if (rc == 0) {
        std::error_code ec;
        fs::create_directories(run_dir(c.home, name), ec);
        std::ofstream(relayed_marker_(c.home, name)) << "started in the Luban host machine\n";
    }
    return rc;
}

int start(const service::Context& given, std::string_view name, const EnterOptions& o) {
    // Its one progress: the stage of starting (design 2026-10-20 WuXian §10).
    service::Context c = given;
    c.sink = service::with_overall(given.sink, false, true, false);
    if constexpr (platform::is_windows) return start_in_carrier_(c, name, o);
    if (running(c.home, name)) return 0;
    if constexpr (platform::is_macos) return start_vz_(c, name, o);
    return start_impl_(c, name, o);
}

nlohmann::json session_env(const fs::path& home, std::string_view name) {
    return read_json_(state_file_(home, name)).value("env", Json::object());
}

std::optional<std::map<std::string, std::string>> started_env(const fs::path& home, std::string_view name) {
    if (!running(home, name)) return std::nullopt;
    std::map<std::string, std::string> out;
    const auto env = session_env(home, name);
    for (auto it = env.begin(); it != env.end(); ++it)
        if (it.value().is_string()) out[it.key()] = it.value().get<std::string>();
    return out;
}

std::optional<std::string> started_gpu(const fs::path& home, std::string_view name) {
    if (!running(home, name)) return std::nullopt;
    const auto gpu = read_json_(state_file_(home, name)).value("gpu", std::string{});
    if (gpu.empty()) return std::nullopt;
    return gpu;
}

std::optional<std::string> started_files(const fs::path& home, std::string_view name) {
    if (!running(home, name)) return std::nullopt;
    const auto files = read_json_(state_file_(home, name)).value("files", std::string{});
    if (files.empty()) return std::nullopt;
    return files;
}

std::expected<Client, std::string> Client::connect(const fs::path& home, std::string_view name) {
    if constexpr (platform::is_windows) {
        const auto* carrier = xlings::carrier::find("wsl2");
        auto at = carrier ? carrier->ensure(xlings::subos::HomeView{home}) : std::unexpected(std::string("no wsl2 carrier"));
        if (!at) return std::unexpected(at.error());
        auto argv = at->launcher;
        argv.insert(argv.end(), {"/xlings/bin/luban", "__pipe-unix", std::format("/xlings/run/machines/{}/luban.sock", name)});
        auto c = through(argv, log_dir(home, name) / "relay.log");
        if (!c) return c;
        // Up when the supervisor there answers.
        (void)c->write_(proto::encode('Q', 0));
        for (int i = 0; i < 30; ++i) {
            auto frames = c->poll(100);
            if (!frames) break;
            for (const auto& [type, _] : *frames)
                if (type == 'H') return c;
        }
        std::error_code ec;
        fs::remove(relayed_marker_(home, name), ec);
        return std::unexpected(std::format("{} is not running", name));
    }
    const int fd = connect_client_(home, name);
    if (fd < 0) return std::unexpected(std::format("{} is not running", name));
    return Client(fd);
}

std::expected<Client, std::string> Client::through(const std::vector<std::string>& argv, const fs::path& log) {
    std::string error;
    std::error_code ec;
    if (!log.empty()) fs::create_directories(log.parent_path(), ec);
    auto child = xlings::platform::duplex::Child::start(argv, log, error);
    if (!child) return std::unexpected(error);
    Client c(-1);
    c.child_ = std::move(child);
    return c;
}

Client& Client::operator=(Client&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) { std::vector<int> fds{fd_}; platform::close_fds(fds); }
        fd_ = std::exchange(other.fd_, -1);
        child_ = std::move(other.child_);
        buffer_ = std::move(other.buffer_);
        writing_ = std::move(other.writing_);
    }
    return *this;
}

bool Client::write_(std::string_view frame) {
    std::lock_guard l(*writing_);
    if (child_) return child_->write(frame);
    return platform::write_all(fd_, frame);
}

bool Client::open_command(std::uint32_t channel, const std::vector<std::string>& argv, int rows, int cols,
                          const nlohmann::json& env, bool pipes, std::string_view user) {
    Json open{{"argv", argv}, {"env", env}};
    if (!user.empty()) open["user"] = std::string(user);
    if (pipes) open["pipes"] = true;
    else {
        const char* term = std::getenv("TERM");
        open["rows"] = rows;
        open["cols"] = cols;
        open["term"] = term ? term : "xterm-256color";
    }
    return write_(proto::encode_json('O', channel, open));
}

bool Client::end_input(std::uint32_t channel) { return write_(proto::encode('F', channel)); }

std::expected<Client, std::string> host_machine_agent(const fs::path& home) {
    if constexpr (!platform::is_windows) return std::unexpected(std::string("only a PC reaches its host machine through a relay"));
    const auto* carrier = xlings::carrier::find("wsl2");
    auto at = carrier ? carrier->ensure(xlings::subos::HomeView{home}) : std::unexpected(std::string("no wsl2 carrier"));
    if (!at) return std::unexpected(at.error());
    auto argv = at->launcher;
    argv.insert(argv.end(), {"/xlings/bin/luban", "__agent-stdio"});
    auto c = Client::through(argv, home / "logs" / "luban-os-relay.log");
    if (!c) return c;
    // The agent there says hello first.
    for (int i = 0; i < 100; ++i) {
        auto frames = c->poll(100);
        if (!frames) return std::unexpected(std::string("the Luban host machine's agent did not answer (an older host machine: luban status)"));
        for (const auto& [type, _] : *frames)
            if (type == 'H') return c;
    }
    return std::unexpected(std::string("the Luban host machine's agent did not answer"));
}

Client::~Client() {
    if (fd_ >= 0) { std::vector<int> fds{fd_}; platform::close_fds(fds); }
}

bool Client::open(std::uint32_t channel, int rows, int cols, const nlohmann::json& env) {
    const char* term = std::getenv("TERM");
    return write_(proto::encode_json('O', channel, Json{{"rows", rows}, {"cols", cols},
                                                                          {"term", term ? term : "xterm-256color"}, {"env", env}}));
}
bool Client::open_display(std::uint32_t channel, std::string_view session, int width, int height) {
    return write_(proto::encode_json('O', channel, Json{{"display", Json{{"session", std::string(session)},
                                                                                           {"width", width}, {"height", height}}}}));
}
bool Client::send(std::uint32_t channel, std::string_view bytes) { return write_(proto::encode('D', channel, bytes)); }
bool Client::resize(std::uint32_t channel, int rows, int cols) {
    return write_(proto::encode_json('R', channel, Json{{"rows", rows}, {"cols", cols}}));
}
bool Client::close(std::uint32_t channel) { return write_(proto::encode('C', channel)); }
bool Client::power_off() { return write_(proto::encode('P', 0)); }
bool Client::send_frame(char type, std::uint32_t channel, std::string_view payload) { return write_(proto::encode(type, channel, payload)); }

std::optional<std::vector<std::pair<char, std::pair<std::uint32_t, std::string>>>> Client::poll(int timeout_ms) {
    std::vector<std::pair<char, std::pair<std::uint32_t, std::string>>> out;
    std::array<platform::PollFd, 1> p{{{.fd = fd_}}};
    if (child_) {
        auto got = child_->read(timeout_ms);
        if (!got) return std::nullopt;
        buffer_ += *got;
    } else if (platform::poll_fds(p, timeout_ms) > 0) {
        std::array<char, 65536> buf{};
        auto n = platform::read_some(fd_, buf);
        if (!n || *n == 0) return std::nullopt;
        buffer_.append(buf.data(), *n);
    }
    // Complete frames out; an incomplete one stays for the next poll.
    while (buffer_.size() >= 9) {
        std::uint32_t len = 0, ch = 0;
        for (int k = 0; k < 4; ++k) ch = (ch << 8) | static_cast<unsigned char>(buffer_[1 + k]);
        for (int k = 0; k < 4; ++k) len = (len << 8) | static_cast<unsigned char>(buffer_[5 + k]);
        if (len > proto::kMaxPayload) return std::nullopt;
        if (buffer_.size() < 9 + len) break;
        out.push_back({buffer_[0], {ch, buffer_.substr(9, len)}});
        buffer_.erase(0, 9 + len);
    }
    return out;
}
fs::path log_dir(const fs::path& home, std::string_view name) { return home / "logs" / "machines" / name; }

bool is_static_elf(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::array<unsigned char, 64> h{};
    if (!in.read(reinterpret_cast<char*>(h.data()), 64)) return false;
    if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' || h[4] != 2 || h[5] != 1) return false;
    const auto u64 = [&](std::size_t at) { std::uint64_t v = 0; for (int k = 7; k >= 0; --k) v = (v << 8) | h[at + k]; return v; };
    const auto u16 = [&](std::size_t at) { return static_cast<std::uint16_t>(h[at] | (h[at + 1] << 8)); };
    const auto phoff = u64(32);
    const auto phentsize = u16(54), phnum = u16(56);
    for (std::uint16_t k = 0; k < phnum; ++k) {
        in.seekg(static_cast<std::streamoff>(phoff + std::uint64_t(k) * phentsize));
        std::array<unsigned char, 4> type{};
        if (!in.read(reinterpret_cast<char*>(type.data()), 4)) return false;
        if (type[0] == 3 && type[1] == 0 && type[2] == 0 && type[3] == 0) return false;   // PT_INTERP
    }
    return true;
}

// On a PC a machine runs in the Luban host machine; this side keeps that it
// was started (a marker, cleared when it is stopped or found gone), so the
// window's list does not ask the host machine on every frame.

std::optional<int> running(const fs::path& home, std::string_view name) {
    if constexpr (platform::is_windows) {
        std::error_code ec;
        return fs::exists(relayed_marker_(home, name), ec) ? std::optional<int>(1) : std::nullopt;
    }
    const auto state = read_json_(state_file_(home, name));
    const int pid = state.value("supervisor", 0);
    if (pid > 0 && platform::process_alive(pid)) return pid;
    return std::nullopt;
}

// On Windows a machine runs in the Luban host machine (WSL 2), on its KVM:
// the same luban, the same qemu and the same guest as on Linux, started
// there (part 4 §8.2). The terminal is this one, through wsl.exe.
int enter_in_carrier_(const service::Context& c, std::string_view name, const EnterOptions& o) {
    const auto caps = service::capabilities(service::gather(c.home, c.port));
    const auto* m = caps.mode("machine");
    if (!m || !m->ready) {
        say_(c, service::error(std::format("{} cannot run as a machine on this computer: {}", name,
                                           m ? m->reason : std::string("this computer did not say")),
                               m && !m->route.empty() ? m->route : std::format("luban enter {} (the sandbox)", name)));
        return 1;
    }
    const auto* carrier = xlings::carrier::find("wsl2");
    if (!carrier) {
        say_(c, service::error("this luban has no wsl2 carrier"));
        return 1;
    }
    auto at = carrier->ensure(xlings::subos::HomeView{c.home});
    if (!at) {
        say_(c, service::error("the Luban host machine did not start: " + at.error(), "luban status"));
        return 1;
    }
    if (o.display == "window") {
        // The window reaches the machine through the host machine (the app's
        // relay); the app starts it when it is not running.
        const auto app = service::window_app(c, name);
        if (!app) {
            say_(c, app.error());
            return 1;
        }
        return platform::run_argv({app->string(), "--attach", std::string(name)});
    }
    std::vector<std::string> argv = at->launcher;
    argv.insert(argv.end(), {"/xlings/bin/luban", "enter", std::string(name), "--mode", "machine"});
    if (o.temp) argv.push_back("--temp");
    // wsl.exe carries only the variables WSLENV names: the operation's trace
    // and actor, and the terminal's type.
    platform::set_env_variable("WSLENV", "TERM/u:XLINGS_TRACE_ID/u:XLINGS_ACTOR/u:XLINGS_OPS_PARENT/u:XLINGS_AGENT_MODE/u");
    return platform::run_argv(argv);
}

// A Mac's machine (part 4 §8.1): one of the Luban host machine's systems,
// booted by Virtualization.framework from the files the Mac sees on the host
// machine's volume -- its tree (or a copy that goes, with --temp), the host
// machine's home read-only, the same Linux luban as /init, virtio-fs for 9p.
int start_vz_(const service::Context& c, std::string_view name, const EnterOptions& o) {
    if (const auto why = vz::unsupported(); !why.empty()) {
        say_(c, service::error(why, std::format("luban enter {} (the sandbox)", name)));
        return 1;
    }
    std::error_code ec;
    const auto tree = c.home / "carriers" / "vz" / "root";
    const auto guest_home = tree / "xlings";
    if (!fs::exists(guest_home, ec)) {
        say_(c, service::error("the Luban host machine's volume is not attached", "luban status (it starts with any system)"));
        return 1;
    }
    auto root = service::root_of(guest_home, name);
    if (!root) {
        say_(c, service::error(std::format("'{}' has no root of its own to boot", name), "a system from an edition is one"));
        return 2;
    }
    const auto kernel = tree / "boot" / "vmlinuz";
    const auto luban = guest_home / "bin" / "luban";
    if (!fs::exists(kernel, ec) || !is_static_elf(luban)) {
        say_(c, service::error("the Luban host machine's tree has no machine kernel or no Linux luban", "luban status"));
        return 1;
    }
    const auto dir = run_dir(c.home, name);
    fs::create_directories(dir, ec);
    fs::create_directories(log_dir(c.home, name), ec);
    std::vector<xlings::image::CpioEntry> entries;
    for (const auto* d : {"dev", "proc", "sys", "newroot"})
        entries.push_back({.path = d, .kind = xlings::image::CpioEntry::Kind::Dir, .mode = 0755});
    std::ifstream in(luban, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    entries.push_back({.path = "init", .kind = xlings::image::CpioEntry::Kind::File, .data = std::move(bytes), .mode = 0755});
    if (auto w = xlings::image::write_cpio(dir / "initramfs.cpio", entries); !w) {
        say_(c, service::error(w.error()));
        return 1;
    }
    auto settings = service::read_settings(c.home, name);
    const auto persist = o.temp ? std::string("none") : settings ? settings->persist : std::string("all");
    fs::path share = *root;
    if (persist != "all") {
        say_(c, service::phase("copy", "begin", "a fresh copy of it"));
        share = dir / "scratch";
        fs::remove_all(share, ec);
        ec.clear();
        copy_tree_(*root, share, persist == "home" ? std::set<std::string>{"home"} : std::set<std::string>{}, ec);
        say_(c, service::phase("copy", ec ? "failed" : "done", "a fresh copy of it"));
        if (ec) return 1;
    }
    const auto memory = o.memory.empty() ? std::string("2048") : o.memory;
    const Json state{{"name", name}, {"hypervisor", "vz"}, {"persist", persist}, {"temp", o.temp},
                     {"scratch", persist == "all" ? "" : share.string()}, {"env", Json::object()},
                     {"started", xlings::observe::utc_now()}, {"accel", "vz"}, {"memory", memory},
                     {"vz", {{"kernel", kernel.string()}, {"initrd", (dir / "initramfs.cpio").string()},
                             {"cmdline", std::format("console=hvc0 quiet panic=-1 luban.home=/xlings luban.name={} luban.userhome=0 luban.fs=virtiofs", name)},
                             {"cpus", 2}, {"memory", std::stoull(memory)}, {"network", true},
                             {"shares", Json::array({{{"tag", "root"}, {"dir", share.string()}, {"read_only", false}},
                                                     {{"tag", "home"}, {"dir", guest_home.string()}, {"read_only", true}}})}}}};
    platform::write_file_atomic(state_file_(c.home, name).string(), state.dump(2));
    const auto text = std::format("starting {} ({} MB, Virtualization.framework, NAT)", name, memory);
    say_(c, service::phase("boot", "begin", text));
    const auto started = std::chrono::steady_clock::now();
    auto pid = platform::spawn_detached({luban_(c.home).string(), "__vm-supervise", c.home.string(), std::string(name)},
                                        log_dir(c.home, name) / "supervisor.log");
    if (!pid) {
        say_(c, service::phase("boot", "failed", text));
        return 1;
    }
    const auto deadline = started + std::chrono::seconds(90);
    while (std::chrono::steady_clock::now() < deadline && platform::process_alive(*pid)) {
        if (const int fd = connect_client_(c.home, name); fd >= 0) {
            proto::Decoder d;
            (void)platform::write_all(fd, proto::encode('Q', 0));
            auto hello = wait_for_(fd, d, 'H', deadline, [&] { (void)platform::write_all(fd, proto::encode('Q', 0)); });
            std::vector<int> close{fd};
            platform::close_fds(close);
            if (hello) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
                say_(c, service::phase("boot", "done", std::format("{} is up ({:.1f} s)", name, ms / 1000.0)));
                return 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    say_(c, service::phase("boot", "failed", text));
    say_(c, service::error(std::format("{} did not come up; the end of its console:{}", name, tail_(log_dir(c.home, name) / "console.log", 15))));
    return 1;
}

namespace {

// The commands in a machine (`luban run`): each holds a lease while it runs
// (<run>/commands/<pid>); a machine started for commands (<run>/for-commands)
// powers off when the last of them ends -- not under another that joined it.
// `luban enter` makes it a machine someone keeps running.
fs::path commands_dir_(const fs::path& home, std::string_view name) { return run_dir(home, name) / "commands"; }
fs::path for_commands_(const fs::path& home, std::string_view name) { return run_dir(home, name) / "for-commands"; }

void lease_(const fs::path& home, std::string_view name) {
    std::error_code ec;
    fs::create_directories(commands_dir_(home, name), ec);
    platform::write_file_atomic((commands_dir_(home, name) / std::to_string(platform::get_pid())).string(), "");
}

// This command's lease given back; whether it was the last one alive.
bool release_(const fs::path& home, std::string_view name) {
    std::error_code ec;
    fs::remove(commands_dir_(home, name) / std::to_string(platform::get_pid()), ec);
    bool others = false;
    for (const auto& e : fs::directory_iterator(commands_dir_(home, name), ec)) {
        int pid = 0;
        const auto file = e.path().filename().string();
        if (std::from_chars(file.data(), file.data() + file.size(), pid).ec != std::errc{} || !platform::is_process_alive(pid)) {
            fs::remove(e.path(), ec);   // a command that ended without giving it back
            continue;
        }
        others = true;
    }
    return !others;
}

}  // namespace

int enter(const service::Context& c, std::string_view name_or_image, const EnterOptions& o) {
    if constexpr (platform::is_windows) {
        std::error_code ec;
        if (!fs::is_regular_file(name_or_image, ec)) return enter_in_carrier_(c, name_or_image, o);
    }
    if constexpr (!platform::is_linux) {
        std::error_code ec;
        if (platform::is_windows || fs::is_regular_file(name_or_image, ec)) {
            say_(c, service::error(platform::is_windows ? "an image file boots on a Linux host in this version"
                                                        : "on a Mac an image file does not boot in this version",
                                   std::format("luban enter {} (the sandbox), or luban export {} {}.iso", name_or_image, name_or_image, name_or_image)));
            return 1;
        }
    }
    std::error_code ec;
    if (fs::is_regular_file(name_or_image, ec)) return boot_image_(c, fs::absolute(name_or_image), o);
    const std::string name(name_or_image);
    if (o.display == "window") {
        const auto app = service::window_app(c, name);
        if (!app) {
            say_(c, app.error());
            return 1;
        }
        return platform::run_argv({app->string(), "--attach", name});
    }
    // Entered: a machine someone keeps running, not one started for commands.
    {
        std::error_code rm;
        fs::remove(for_commands_(c.home, name), rm);
    }
    if (!running(c.home, name)) {
        if (const int rc = platform::is_macos ? start_vz_(c, name, o) : start_impl_(c, name, o); rc != 0) return rc;
    } else if (o.temp) {
        say_(c, service::notice(std::format("{} is already running and keeps what it writes; this session joins it", name)));
    }
    if (auto e = service::describe(c, name)) {
        auto facts = machine_facts_(c, name);
        if (const auto root = service::root_of(c.home, name)) {
            const auto who = o.user.empty() ? xlings::subos::root_etc::login_user(*root / "etc") : o.user;
            if (const auto u = xlings::subos::root_etc::find_user(*root / "etc", who)) {
                facts.user = u->name;
                facts.user_home = u->home;
            }
        }
        for (const auto& line : service::welcome(*e, "machine", o.temp, service::terminal_style(), facts))
            std::println("{}", line);
    }
    auto [ending, code] = session_(c, name, o.user);
    const auto state = read_json_(state_file_(c.home, name));
    const bool temp = state.value("temp", false) || state.value("persist", "all") != "all";
    if (ending == Ending::Detached) {
        std::println("{} keeps running: luban enter {} · luban stop {}", name, name, name);
        return 0;
    }
    // A machine powering itself off (`poweroff` inside) ends the session
    // first -- its shell killed, or the connection gone -- while it is still
    // going down: wait for it to be off rather than ask whether to keep
    // running what is stopping.
    if (ending == Ending::Lost || ending == Ending::Killed) {
        for (int k = 0; k < 100 && running(c.home, name); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!running(c.home, name)) {
            std::println("{} is off", name);
            return ending == Ending::Lost ? code : 0;
        }
    }
    bool off = ending == Ending::PowerOff;
    if (!off && state.value("temp", false)) off = true;
    if (!off) {
        auto settings = service::read_settings(c.home, name);
        const auto on_close = settings ? settings->on_close : std::string("ask");
        if (on_close == "poweroff") off = true;
        else if (on_close == "ask" && platform::stdin_is_terminal() && !std::getenv("XLINGS_AGENT_MODE")) {
            const auto line = std::format("{} is still running. (b)ackground or (p)ower off{}? B or P also makes it the default [b] ",
                                          name, temp ? " (what it wrote goes)" : "");
            std::print("{}", line);
            std::cout.flush();
            std::string answer;
            std::getline(std::cin, answer);
            off = !answer.empty() && (answer[0] == 'p' || answer[0] == 'P');
            if (!answer.empty() && (answer[0] == 'B' || answer[0] == 'P'))
                (void)service::set_setting(c.home, name, "on-close", off ? "poweroff" : "background");
        }
    }
    if (off) return vm::stop(c, name).code ? 1 : code;
    std::println("{} keeps running: luban enter {} · luban stop {}", name, name, name);
    return code;
}


int run(const service::Context& c, std::string_view name, std::span<const std::string> argv, std::string_view user) {
    if (argv.empty()) return 2;
    // Whether it runs and taking a lease, or giving one back and powering it
    // off, each under one lock: a command joining as another leaves would
    // otherwise find it running and lose it to that one's power-off.
    const auto decide = [&] {
        auto lock = std::make_unique<platform::FileLock>();
        std::error_code ec;
        fs::create_directories(run_dir(c.home, name), ec);
        std::string why;
        (void)lock->acquire(run_dir(c.home, name) / "commands.lock", std::chrono::minutes(5), {}, why);
        return lock;
    };
    {
        const auto lock = decide();
        if (!running(c.home, name).has_value()) {
            if (const int rc = start(c, name, EnterOptions{}); rc != 0) return rc;
            platform::write_file_atomic(for_commands_(c.home, name).string(), "");
        }
        lease_(c.home, name);
    }
    // The last command out powers off a machine started for commands.
    const auto done_with = [&] {
        const auto lock = decide();
        std::error_code ec;
        if (release_(c.home, name) && fs::exists(for_commands_(c.home, name), ec)) (void)vm::stop(c, name);
    };
    auto client = Client::connect(c.home, name);
    if (!client || client->fd() < 0) {
        say_(c, service::error(std::format("{} does not answer: {}", name, client ? std::string("no connection") : client.error()),
                               std::format("luban status {}", name)));
        done_with();
        return 125;
    }
    // A terminal on both ends: the command on one there (its size, raw), so
    // what asks a question can; otherwise pipes, its output as it was written.
    const bool tty = platform::stdin_is_terminal() && platform::stdout_is_terminal();
    const auto size = platform::terminal_size(0).value_or(std::pair<unsigned short, unsigned short>{24, 80});
    const std::vector<std::string> words(argv.begin(), argv.end());
    int code = 125;
    if (client->open_command(1, words, size.first, size.second, session_env(c.home, name), !tty, user)) {
        const auto saved = tty ? platform::make_terminal_raw(0) : std::nullopt;
        std::array<char, 65536> buf{};
        bool input = true;
        for (bool done = false; !done;) {
            std::array<platform::PollFd, 2> p{{{.fd = input ? 0 : -1}, {.fd = client->fd()}}};
            platform::poll_fds(p, 200);
            if (p[1].readable || p[1].closed) {
                auto frames = client->poll(0);
                if (!frames) {
                    // The machine went away under the command: said, never a silent end.
                    say_(c, service::error(std::format("{} stopped while {} ran in it", name, words.front()),
                                           std::format("luban status {} --log", name)));
                    break;
                }
                for (const auto& [type, body] : *frames) {
                    if (body.first != 1) continue;
                    if (type == 'D') (void)platform::write_all(1, body.second);
                    else if (type == 'E') (void)platform::write_all(2, body.second);
                    else if (type == 'X') {
                        const auto j = Json::parse(body.second, nullptr, false);
                        code = j.is_object() ? j.value("code", 0) : 0;
                        if (j.is_object() && j.contains("error"))
                            say_(c, service::error(std::format("{}: {}", words.front(), j.value("error", std::string{}))));
                        done = true;
                    }
                }
            }
            if (input && (p[0].readable || p[0].closed)) {
                auto n = platform::read_some(0, buf);
                if (!n || *n == 0) {
                    input = false;
                    if (!tty) (void)client->end_input(1);
                } else {
                    (void)client->send(1, std::string_view(buf.data(), *n));
                }
            }
        }
        if (saved) platform::restore_terminal(0, *saved);
    }
    done_with();
    return code;
}

service::Outcome stop(const service::Context& c, std::string_view name) {
    auto pid = running(c.home, name);
    if (!pid) return {.code = 0};
    if constexpr (platform::is_windows) {
        say_(c, service::phase("stop", "begin", std::format("powering off {}", name)));
        if (auto client = Client::connect(c.home, name)) {
            (void)client->power_off();
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        std::error_code ec;
        fs::remove(relayed_marker_(c.home, name), ec);
        say_(c, service::phase("stop", "done", std::format("{} is off", name)));
        return {.code = 0};
    }
    say_(c, service::phase("stop", "begin", std::format("powering off {}", name)));
    const int fd = connect_client_(c.home, name);
    if (fd >= 0) {
        (void)platform::write_all(fd, proto::encode('P', 0));
        std::vector<int> close{fd};
        platform::close_fds(close);
    } else {
        platform::send_signal(*pid, platform::sig::terminate);
    }
    for (int i = 0; i < 300 && platform::process_alive(*pid); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (platform::process_alive(*pid)) platform::send_signal(*pid, platform::sig::terminate);
    for (int i = 0; i < 50 && platform::process_alive(*pid); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool off = !platform::process_alive(*pid);
    say_(c, service::phase("stop", off ? "done" : "failed", off ? std::format("{} is off", name) : std::format("powering off {}", name)));
    return {.code = off ? 0 : 1};
}

// ── the supervisor ──────────────────────────────────────────────────────

namespace {

// What a supervisor does whatever runs the machine: the agent's frames to
// the client that opened the channel, a client's to the agent; `alive` says
// whether the machine still runs, `kill` ends it when a power-off is not
// heard; a client's `S` frame ({"grants": {name: [path, read-only]}}) is
// handed to `grants` (a hypervisor that shares directories while running).
// A broker request from root inside (design 2026-10-21 §3.3): xlings's
// `subos __broker-serve <name>` -- the verdict and the run, recorded -- on a
// terminal made here when the caller is at one, on pipes otherwise; its bytes
// relayed on the guest's channel.
struct Brokered {
    int pid { -1 };
    int in { -1 };
    int out { -1 };
    int err { -1 };
    bool tty { false };
};

std::optional<Brokered> broker_start_(std::string_view name, const Json& req) {
    std::vector<std::string> argv{service::xlings_path().string(), "subos", "__broker-serve", std::string(name)};
    if (const auto actor = req.value("actor", std::string{}); !actor.empty()) argv.insert(argv.end(), {"--actor", actor});
    argv.push_back("--");
    for (const auto& w : req.value("argv", Json::array()))
        if (w.is_string()) argv.push_back(w.get<std::string>());
    Brokered b{.tty = req.value("tty", false)};
    std::vector<int> child, mine;
    if (b.tty) {
        auto pty = platform::open_pty();
        if (!pty) return std::nullopt;
        platform::set_terminal_size((*pty)[0], req.value("rows", 24), req.value("cols", 80));
        b.in = b.out = (*pty)[0];
        child = {(*pty)[1], (*pty)[1], (*pty)[1]};
        mine = {(*pty)[1]};
    } else {
        auto in = platform::make_pipe(), out = platform::make_pipe(), err = platform::make_pipe();
        if (!in || !out || !err) return std::nullopt;
        b.in = (*in)[1];
        b.out = (*out)[0];
        b.err = (*err)[0];
        child = {(*in)[0], (*out)[1], (*err)[1]};
        mine = child;
    }
    b.pid = platform::fork_process();
    if (b.pid == 0) {
        platform::reset_signals();
        platform::new_session();
        platform::redirect_stdio(child);
        if (b.tty) (void)platform::take_controlling_terminal();
        platform::exec_path(argv, platform::environment());
        platform::exit_now(127);
    }
    platform::close_fds(mine);
    if (b.pid < 0) {
        std::vector<int> rest{b.in};
        if (b.out != b.in) rest.push_back(b.out);
        if (b.err >= 0) rest.push_back(b.err);
        platform::close_fds(rest);
        return std::nullopt;
    }
    return b;
}

void bridge_(const fs::path& home, std::string_view name, int agent, int listen, const std::function<bool()>& alive,
             const std::function<void()>& kill, const std::function<bool(const Json&)>& grants) {
    platform::route_signals({platform::sig::terminate, platform::sig::interrupt}, {});
    struct Client { int fd; proto::Decoder d; std::map<std::uint32_t, std::uint32_t> channels; };   // local -> global
    std::vector<Client> clients;
    std::map<std::uint32_t, std::pair<int, std::uint32_t>> owners;   // global -> (client fd, local)
    std::uint32_t next = 1;
    bool up = false;
    std::string hello;   // what the agent said it is and provides (its 'H'), passed on as said
    proto::Decoder from_agent;
    std::array<char, 65536> buf{};
    auto powering_off = std::optional<std::chrono::steady_clock::time_point>{};
    std::map<std::uint32_t, Brokered> brokered;   // the guest's channel -> its command here
    // The machine's openbus link (design 2026-10-22 §6.4): the guest's router
    // speaks to this home's, its bytes carried as L frames; this side only
    // moves them. -1: no link now.
    int bus = -1;
    const auto end_bus = [&](bool tell_guest) {
        if (bus >= 0) {
            std::vector<int> b{bus};
            platform::close_fds(b);
            bus = -1;
        }
        if (tell_guest) (void)platform::write_all(agent, proto::encode('L', 0));
    };
    const auto close_brokered = [](Brokered& b) {
        std::vector<int> fds;
        if (b.in >= 0) fds.push_back(b.in);
        if (b.out >= 0 && b.out != b.in) fds.push_back(b.out);
        if (b.err >= 0) fds.push_back(b.err);
        platform::close_fds(fds);
        b.in = b.out = b.err = -1;
    };
    for (;;) {
        if (!alive()) break;
        for (int s : platform::pending_signals()) {
            if (!powering_off && (s == platform::sig::terminate || s == platform::sig::interrupt)) {
                (void)platform::write_all(agent, proto::encode('P', 0));
                powering_off = std::chrono::steady_clock::now();
            }
        }
        if (powering_off && std::chrono::steady_clock::now() - *powering_off > std::chrono::seconds(20)) {
            kill();
            powering_off = std::chrono::steady_clock::now();
        }
        std::vector<platform::PollFd> p{{.fd = agent}, {.fd = listen}};
        for (const auto& cl : clients) p.push_back({.fd = cl.fd});
        // A brokered command's output (a terminal's, or its two pipes).
        std::vector<std::pair<std::uint32_t, char>> outputs;
        for (const auto& [ch, b] : brokered) {
            if (b.out >= 0) { p.push_back({.fd = b.out}); outputs.emplace_back(ch, 'D'); }
            if (b.err >= 0) { p.push_back({.fd = b.err}); outputs.emplace_back(ch, 'E'); }
        }
        const std::size_t bus_at = p.size();
        if (bus >= 0) p.push_back({.fd = bus});
        platform::poll_fds(p, 200);
        if (p[0].readable || p[0].closed) {
            auto n = platform::read_some(agent, buf);
            if (!n || *n == 0) break;
            from_agent.feed(std::string_view(buf.data(), *n));
            for (;;) {
                auto f = from_agent.next();
                if (!f || !*f) break;
                const auto& frame = **f;
                if (frame.type == 'H') {
                    up = true;
                    hello = frame.payload;
                    // This side's own: the broker (design 2026-10-21 §3.3), and the
                    // host's bus, under which the machine's is (design 2026-10-22 §6.4).
                    end_bus(false);   // an agent that says hello again links again
                    (void)platform::write_all(agent, proto::encode_json('A', 0, Json{{"provides", Json{
                        {"broker", 1}, {"openbus", Json{{"root", system_bus_root(name)}}}}}}));
                    for (auto& cl : clients) (void)platform::write_all(cl.fd, proto::encode('H', 0, frame.payload));
                    continue;
                }
                if (frame.type == 'L') {
                    // The guest's router: its first bytes open the link to the host's.
                    if (frame.payload.empty()) { end_bus(false); continue; }
                    if (bus < 0) {
                        auto fd = connect_bus(home);
                        if (!fd) {
                            std::println(std::cerr, "luban: openbus: {}", fd.error());
                            end_bus(true);
                            continue;
                        }
                        bus = *fd;
                    }
                    if (!platform::write_all(bus, frame.payload)) end_bus(true);
                    continue;
                }
                if (frame.channel & proto::kGuestChannel) {
                    if (frame.type == 'B') {
                        const auto req = Json::parse(frame.payload, nullptr, false);
                        auto b = req.is_object() ? broker_start_(name, req) : std::nullopt;
                        if (b) brokered[frame.channel] = *b;
                        else (void)platform::write_all(agent, proto::encode_json('X', frame.channel,
                                                           Json{{"code", 125}, {"error", "the host's broker could not start"}}));
                        continue;
                    }
                    auto b = brokered.find(frame.channel);
                    if (b == brokered.end()) continue;
                    if (frame.type == 'D' && b->second.in >= 0) (void)platform::write_all(b->second.in, frame.payload);
                    else if (frame.type == 'F' && !b->second.tty && b->second.in >= 0) {
                        std::vector<int> in{b->second.in};
                        platform::close_fds(in);
                        b->second.in = -1;
                    } else if (frame.type == 'R' && b->second.tty) {
                        const auto j = Json::parse(frame.payload, nullptr, false);
                        if (j.is_object()) platform::set_terminal_size(b->second.out, j.value("rows", 24), j.value("cols", 80));
                    } else if (frame.type == 'C') {
                        // The caller hung up: as a terminal's hang-up ends what runs on it.
                        platform::send_signal_group(b->second.pid, platform::sig::hangup);
                    }
                    continue;
                }
                auto owner = owners.find(frame.channel);
                if (owner == owners.end()) continue;
                (void)platform::write_all(owner->second.first, proto::encode(frame.type, owner->second.second, frame.payload));
                if (frame.type == 'X') owners.erase(owner);
            }
        }
        const auto polled = clients.size();
        if (p[1].readable) {
            const int fd = platform::unix_accept(listen);
            if (fd >= 0) clients.push_back({.fd = fd});
        }
        for (std::size_t k = 0; k < polled; ++k) {
            auto& cl = clients[k];
            const auto& pf = p[2 + k];
            if (!pf.readable && !pf.closed) continue;
            auto n = platform::read_some(cl.fd, buf);
            if (!n || *n == 0) {
                for (const auto& [local, global] : cl.channels) {
                    (void)platform::write_all(agent, proto::encode('C', global));
                    owners.erase(global);
                }
                std::vector<int> gone{cl.fd};
                platform::close_fds(gone);
                cl.fd = -1;
                continue;
            }
            cl.d.feed(std::string_view(buf.data(), *n));
            for (;;) {
                auto f = cl.d.next();
                if (!f || !*f) break;
                auto& frame = **f;
                if (frame.type == 'Q') {
                    if (up) (void)platform::write_all(cl.fd, proto::encode('H', 0, hello));
                    continue;
                }
                if (frame.type == 'P') {
                    (void)platform::write_all(agent, proto::encode('P', 0));
                    if (!powering_off) powering_off = std::chrono::steady_clock::now();
                    continue;
                }
                if (frame.type == 'S') {
                    const auto j = Json::parse(frame.payload, nullptr, false);
                    const bool ok = j.is_object() && grants && grants(j);
                    (void)platform::write_all(cl.fd, proto::encode_json('S', 0, Json{{"ok", ok}}));
                    continue;
                }
                std::uint32_t global;
                if (frame.type == 'O') {
                    global = next++;
                    cl.channels[frame.channel] = global;
                    owners[global] = {cl.fd, frame.channel};
                } else if (auto it = cl.channels.find(frame.channel); it != cl.channels.end()) {
                    global = it->second;
                } else {
                    continue;
                }
                (void)platform::write_all(agent, proto::encode(frame.type, global, frame.payload));
            }
        }
        std::erase_if(clients, [](const Client& cl) { return cl.fd < 0; });
        for (std::size_t k = 0; k < outputs.size(); ++k) {
            const auto& pf = p[2 + polled + k];
            if (!pf.readable && !pf.closed) continue;
            auto b = brokered.find(outputs[k].first);
            if (b == brokered.end()) continue;
            int& fd = outputs[k].second == 'D' ? b->second.out : b->second.err;
            if (fd < 0) continue;
            auto n = platform::read_some(fd, buf);
            if (n && *n > 0) {
                (void)platform::write_all(agent, proto::encode(outputs[k].second, b->first, std::string_view(buf.data(), *n)));
                continue;
            }
            if (outputs[k].second == 'E') {
                std::vector<int> e{fd};
                platform::close_fds(e);
                fd = -1;
                continue;
            }
            // Its output ended: the command's end, said with its code.
            int code = 125;
            if (auto st = platform::wait_process(b->second.pid, true)) code = platform::exit_code(*st);
            if (b->second.err >= 0)   // what it said last on standard error, first
                while (auto rest = platform::read_some(b->second.err, buf)) {
                    if (*rest == 0) break;
                    (void)platform::write_all(agent, proto::encode('E', b->first, std::string_view(buf.data(), *rest)));
                }
            close_brokered(b->second);
            (void)platform::write_all(agent, proto::encode_json('X', b->first, Json{{"code", code}}));
            brokered.erase(b);
        }
        if (bus >= 0 && bus_at < p.size() && (p[bus_at].readable || p[bus_at].closed)) {
            auto n = platform::read_some(bus, buf);
            if (n && *n > 0) (void)platform::write_all(agent, proto::encode('L', 0, std::string_view(buf.data(), *n)));
            else end_bus(true);   // the host's router went: the guest links again
        }
    }
    end_bus(false);
    for (auto& [ch, b] : brokered) {
        platform::send_signal_group(b.pid, platform::sig::hangup);
        close_brokered(b);
    }
    std::vector<int> fds{agent, listen};
    for (auto& cl : clients) if (cl.fd >= 0) fds.push_back(cl.fd);
    platform::close_fds(fds);
}

void cleanup_(const fs::path& home, std::string_view name, const Json& state) {
    std::error_code ec;
    const auto dir = run_dir(home, name);
    if (const auto scratch = state.value("scratch", std::string{}); !scratch.empty() && scratch.starts_with(dir.string()))
        fs::remove_all(scratch, ec);
    fs::remove(client_socket_(home, name), ec);
    fs::remove(state_file_(home, name), ec);
    // What its commands held is this boot's (vm::run); so are its files' daemons' sockets.
    fs::remove(dir / "for-commands", ec);
    fs::remove_all(dir / "commands", ec);
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.path().filename().string().starts_with("vfs-")) fs::remove(e.path(), ec);
}

// A Mac's machine (part 4 §6.6): Virtualization.framework in this process,
// its loop on the main thread, the bridge on another; the agent's port is a
// socket pair, the kernel's console a file.
int supervise_vz_(const fs::path& home, std::string_view name, Json state) {
    const auto& v = state["vz"];
    vz::Plan plan;
    plan.kernel = v.value("kernel", std::string{});
    plan.initrd = v.value("initrd", std::string{});
    plan.cmdline = v.value("cmdline", std::string{});
    plan.cpus = v.value("cpus", 2u);
    plan.memory_mb = v.value("memory", 2048ull);
    plan.grants = v.value("grants", false);
    plan.network = v.value("network", true);
    for (const auto& sh : v.value("shares", Json::array()))
        plan.shares.push_back({sh.value("tag", std::string{}), sh.value("dir", std::string{}), sh.value("read_only", false)});
    auto pair = platform::unix_stream_pair();
    const auto console = log_dir(home, name) / "console.log";
    { std::ofstream truncate(console, std::ios::trunc); }
    const int console_fd = platform::open_device(console);
    const int listen = platform::unix_listen_stream(client_socket_(home, name));
    if (!pair || listen < 0) return 1;
    plan.agent_fd = (*pair)[1];
    plan.console_fd = console_fd;
    state["supervisor"] = platform::get_pid();
    platform::write_file_atomic(state_file_(home, name).string(), state.dump(2));
    platform::ignore_signals({platform::sig::pipe, platform::sig::hangup});
    std::atomic<bool> alive{true};
    std::map<std::string, std::pair<fs::path, bool>> granted;
    std::mutex granting;
    std::thread relay([&] {
        bridge_(home, name, (*pair)[0], listen, [&] { return alive.load(); }, [] { vz::stop(); },
                [&](const Json& j) {
                    std::lock_guard l(granting);
                    const auto asked = j.value("grants", Json::object());
                    for (auto it = asked.begin(); it != asked.end(); ++it)
                        if (it.value().is_array() && it.value().size() == 2)
                            granted[it.key()] = {it.value()[0].get<std::string>(), it.value()[1].get<bool>()};
                    vz::set_grants(granted);
                    return true;
                });
        vz::stop();   // the agent went away: the machine with it
    });
    const int rc = vz::run(plan, [] {});
    alive = false;
    relay.join();
    std::vector<int> rest{(*pair)[1], console_fd};
    platform::close_fds(rest);
    cleanup_(home, name, state);
    return rc;
}

}  // namespace

int supervise(const fs::path& home, std::string_view name) {
    auto state = read_json_(state_file_(home, name));
    if (state.value("hypervisor", std::string("qemu")) == "vz") return supervise_vz_(home, name, std::move(state));
    std::vector<std::string> q;
    for (const auto& a : state.value("qemu", Json::array())) q.push_back(a.get<std::string>());
    if (q.empty()) return 1;
    state["supervisor"] = platform::get_pid();
    platform::write_file_atomic(state_file_(home, name).string(), state.dump(2));
    const auto dir = run_dir(home, name);
    std::error_code ec;
    fs::remove(dir / "agent.sock", ec);
    platform::ignore_signals({platform::sig::pipe, platform::sig::hangup});
    const auto launch = [](const std::vector<std::string>& argv, const std::map<std::string, std::string>& env) {
        const int pid = platform::fork_process();
        if (pid == 0) {
            platform::exec_path(argv, env);
            platform::exit_now(127);
        }
        return pid;
    };
    // Its files' daemons first (virtio-fs): each listening before qemu
    // connects, and each serves one qemu -- a second (the software one)
    // gets its own. One that does not start is 9p instead.
    std::vector<int> daemons;
    const auto stop_daemons = [&] {
        for (const int d : daemons) (void)platform::send_signal(d, platform::sig::terminate);
        for (const int d : daemons) (void)platform::wait_process(d, true);
        daemons.clear();
    };
    // One daemon, listening (its socket there) within 3 s.
    const auto start_one = [&](const std::vector<std::string>& a) {
        const fs::path sock = a.size() > 2 ? fs::path(a[2]) : fs::path{};
        fs::remove(sock, ec);
        const int pid = launch(a, platform::environment());
        if (pid < 0) return false;
        for (int k = 0; k < 60; ++k) {
            if (fs::exists(sock, ec)) {
                daemons.push_back(pid);
                return true;
            }
            if (platform::wait_process(pid, false)) return false;   // it ended: nothing to wait for
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        (void)platform::send_signal(pid, platform::sig::terminate);
        (void)platform::wait_process(pid, true);
        return false;
    };
    const auto start_daemons = [&] {
        for (const auto& argv : state.value("virtiofsd", Json::array())) {
            std::vector<std::string> a;
            for (const auto& w : argv) a.push_back(w.get<std::string>());
            if (start_one(a)) continue;
            // A host that does not give it namespaces (user namespaces restricted): without its sandbox.
            const auto how = std::ranges::find(a, "namespace");
            if (how == a.end()) return false;
            *how = "none";
            std::println(std::cerr, "luban: virtiofsd did not start in its namespaces; {} again without its sandbox", a.size() > 4 ? a[4] : "");
            if (!start_one(a)) return false;
        }
        return true;
    };
    const auto on_9p = [&](const Json& args) {
        std::vector<std::string> out;
        for (const auto& a : args) out.push_back(a.get<std::string>());
        const auto vfs = state.value("files_virtiofs", Json::array());
        if (vfs.empty()) return out;
        for (std::size_t k = 0; k + vfs.size() <= out.size(); ++k)
            if (std::equal(vfs.begin(), vfs.end(), out.begin() + static_cast<std::ptrdiff_t>(k),
                           [](const Json& v, const std::string& a) { return v.get<std::string>() == a; })) {
                out.erase(out.begin() + static_cast<std::ptrdiff_t>(k), out.begin() + static_cast<std::ptrdiff_t>(k + vfs.size()));
                for (const auto& n : state.value("files_9p", Json::array()))
                    out.insert(out.begin() + static_cast<std::ptrdiff_t>(k++), n.get<std::string>());
                break;
            }
        for (auto& a : out)
            if (const auto at = a.find(" luban.fs=virtiofs"); at != std::string::npos) a.erase(at, std::string_view(" luban.fs=virtiofs").size());
        return out;
    };
    if (state.contains("virtiofsd") && !start_daemons()) {
        std::println(std::cerr, "luban: a virtiofsd did not start; {} shares its files over 9p", name);
        stop_daemons();
        q = on_9p(state["qemu"]);
        state["qemu"] = q;
        if (state.contains("qemu_software")) state["qemu_software"] = on_9p(state["qemu_software"]);
        state.erase("virtiofsd");
        state["files"] = "9p (virtiofsd did not start: supervisor.log)";
        platform::write_file_atomic(state_file_(home, name).string(), state.dump(2));
    }
    int qemu = launch(q, qemu_env_(state.value("gpu_env", Json::array())));
    if (qemu < 0) { stop_daemons(); return 1; }
    int agent = -1;
    for (int i = 0; i < 100 && agent < 0; ++i) {
        agent = platform::unix_connect_stream(dir / "agent.sock");
        if (agent >= 0) break;
        // The GL qemu ended before its machine was up -- its display could
        // not draw on the render node (qemu said why, above in this log):
        // the machine in software, its state saying so.
        if (auto ended = platform::wait_process(qemu, false); ended && state.contains("qemu_software")) {
            std::println(std::cerr, "luban: the GL qemu ended ({}); starting {} without its GPU", platform::exit_code(*ended), name);
            q.clear();
            for (const auto& a : state["qemu_software"]) q.push_back(a.get<std::string>());
            state.erase("qemu_software");
            state.erase("gpu_env");
            state["qemu"] = q;
            state["gpu"] = "no GPU: the GL qemu could not draw on this host's render node (supervisor.log)";
            platform::write_file_atomic(state_file_(home, name).string(), state.dump(2));
            if (state.contains("virtiofsd")) {
                stop_daemons();
                if (!start_daemons()) {
                    stop_daemons();
                    return 1;
                }
            }
            qemu = launch(q, platform::environment());
            if (qemu < 0) { stop_daemons(); return 1; }
            i = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const int listen = platform::unix_listen_stream(client_socket_(home, name));
    if (agent < 0 || listen < 0) {
        platform::send_signal(qemu, platform::sig::terminate);
        (void)platform::wait_process(qemu, true);
        stop_daemons();
        return 1;
    }
    bridge_(home, name, agent, listen, [&] { return !platform::wait_process(qemu, false).has_value(); },
            [&] { platform::send_signal(qemu, platform::sig::terminate); }, {});
    (void)platform::wait_process(qemu, true);
    stop_daemons();
    cleanup_(home, name, state);
    return 0;
}

}  // namespace luban::vm
