module luban.vm;

import std;
import xlings.libs.json;
import xlings.observe;
import xlings.platform;
import xlings.platform.stream;
import xlings.image;
import xlings.platform.target;
import xlings.subos.home_view;
import xlings.subos.ports;
import xlings.subos.tools;
import luban.service;
import luban.vm.protocol;

namespace luban::vm {

namespace {

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

bool kvm_() {
    std::error_code ec;
    if (!fs::is_character_file("/dev/kvm", ec)) return false;
    std::fstream kvm("/dev/kvm", std::ios::in | std::ios::out);
    return kvm.is_open();
}

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

// Starts the machine's supervisor and waits for its agent.
int start_impl_(const service::Context& c, std::string_view name, const EnterOptions& o) {
    const auto& arch = xlings::image::native_arch();
    const xlings::subos::HomeView view{c.home};
    if (arch.qemu.empty()) {
        say_(c, service::error(std::format("a machine runs on x86_64 and aarch64 hosts (this one is {})", platform::build_arch())));
        return 1;
    }
    say_(c, service::phase("check", "begin", "checking this host"));
    // In the table's order, the first that can share a directory with the
    // machine (9p, asked of the device itself: its properties name the fsdev
    // it shares) -- a host's qemu without it does not hide the index's.
    const auto all = xlings::subos::tools::candidates(arch.qemu, view, xlings::subos::Ports{});
    if (all.empty()) {
        say_(c, service::phase("check", "failed", "checking this host"));
        say_(c, service::error("a machine needs a virtual machine monitor here", xlings::subos::tools::install_hint(arch.qemu)));
        return 1;
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
        say_(c, service::phase("check", "failed", "checking this host"));
        say_(c, service::error(refused, "xlings install qemu-system@latest (revision 3 or later)"));
        return 1;
    }
    auto kernel = kernel_(c.home);
    if (!kernel) {
        say_(c, service::phase("check", "done", "checking this host"));
        say_(c, service::phase("kernel", "begin", "installing the machine kernel"));
        const int rc = c.port.capability("install_packages", {{"targets", Json::array({"xim:linux-kernel-virt"})}, {"yes", true}}, c.sink);
        kernel = kernel_(c.home);
        say_(c, service::phase("kernel", kernel ? "done" : "failed", "installing the machine kernel"));
        if (rc != 0 || !kernel) return rc != 0 ? rc : 1;
    } else {
        say_(c, service::phase("check", "done", "checking this host"));
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
    const auto home = c.home.lexically_normal().string();
    if (home.find(',') != std::string::npos || root->string().find(',') != std::string::npos) {
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
    const bool kvm = kvm_();
    const auto memory = o.memory.empty() ? std::string("2048") : o.memory;
    std::vector<std::string> q{qemu->bin.string()};
    if (!arch.machine.empty()) q.insert(q.end(), {"-M", std::string(arch.machine)});
    q.insert(q.end(), {"-m", memory, "-smp", "2"});
    if (kvm) q.insert(q.end(), {"-enable-kvm", "-cpu", "host"});
    else if (!arch.cpu.empty()) q.insert(q.end(), {"-cpu", std::string(arch.cpu)});
    const std::string console = arch.name == "aarch64" ? "ttyAMA0" : "ttyS0";
    q.insert(q.end(), {"-kernel", kernel->string(), "-initrd", initrd->string(), "-append",
                       std::format("console={} quiet panic=-1 luban.home={} luban.name={} luban.userhome={}", console, home, name, userhome ? 1 : 0),
                       "-display", "none", "-no-reboot",
                       "-serial", std::format("file:{}", (log_dir(c.home, name) / "console.log").string()),
                       "-virtfs", std::format("local,path={},mount_tag=root,security_model=none,id=root", share.string()),
                       "-virtfs", std::format("local,path={},mount_tag=home,security_model=none,readonly=on,id=home", home),
                       "-device", "virtio-serial-pci",
                       "-chardev", std::format("socket,id=agent,path={},server=on,wait=off", (dir / "agent.sock").string()),
                       "-device", std::format("virtserialport,chardev=agent,name={}", proto::kPort),
                       "-qmp", std::format("unix:{},server=on,wait=off", (dir / "qmp.sock").string())});
    if (userhome)
        q.insert(q.end(), {"-virtfs", std::format("local,path={},mount_tag=userhome,security_model=none,id=userhome",
                                                  (*root / "home").string())});
    q.insert(q.end(), net.args.begin(), net.args.end());
    Json state{{"name", name}, {"qemu", q}, {"persist", persist}, {"temp", o.temp}, {"scratch", persist == "all" ? "" : share.string()},
               {"env", net.env}, {"started", xlings::observe::utc_now()}, {"accel", kvm ? "kvm" : "tcg"}, {"memory", memory}};
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
    // Up when its agent answers through the supervisor.
    const auto deadline = started + std::chrono::seconds(kvm ? 90 : 300);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!platform::process_alive(*pid)) break;
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

enum class Ending { Exited, Detached, PowerOff, Lost };

// One session of a running machine on this terminal.
std::pair<Ending, int> session_(const service::Context& c, std::string_view name) {
    const int fd = connect_client_(c.home, name);
    if (fd < 0) return {Ending::Lost, 1};
    const auto state = read_json_(state_file_(c.home, name));
    const char* term = std::getenv("TERM");
    auto size = platform::terminal_size(0).value_or(std::pair<unsigned short, unsigned short>{24, 80});
    Json open{{"rows", size.first}, {"cols", size.second}, {"term", term ? term : "xterm-256color"},
              {"env", state.value("env", Json::object())}};
    if (!platform::write_all(fd, proto::encode_json('O', 1, open))) return {Ending::Lost, 1};
    const auto saved = platform::make_terminal_raw(0);
    std::println(std::cerr, "\r{} -- Ctrl-] d leaves it running, Ctrl-] p powers it off\r", name);
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
                    ending = Ending::Exited;
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
    q.insert(q.end(), {"-m", o.memory.empty() ? std::string("2048") : o.memory, "-smp", "2"});
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

std::optional<SoundForward> sound_forward(bool granted, const fs::path& pulse, const fs::path& luban) {
    std::error_code ec;
    if (!granted || pulse.empty() || !fs::exists(pulse, ec)) return std::nullopt;
    if (pulse.string().find(',') != std::string::npos || luban.string().find(',') != std::string::npos) return std::nullopt;
    return SoundForward{std::format("guestfwd=tcp:10.0.2.101:4713-cmd:{} __pipe-unix {}", luban.string(), pulse.string()),
                        "tcp:10.0.2.101:4713"};
}

int start(const service::Context& c, std::string_view name, const EnterOptions& o) {
    if (running(c.home, name)) return 0;
    return start_impl_(c, name, o);
}

nlohmann::json session_env(const fs::path& home, std::string_view name) {
    return read_json_(state_file_(home, name)).value("env", Json::object());
}

std::expected<Client, std::string> Client::connect(const fs::path& home, std::string_view name) {
    const int fd = connect_client_(home, name);
    if (fd < 0) return std::unexpected(std::format("{} is not running", name));
    return Client(fd);
}

Client& Client::operator=(Client&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) { std::vector<int> fds{fd_}; platform::close_fds(fds); }
        fd_ = std::exchange(other.fd_, -1);
        buffer_ = std::move(other.buffer_);
        writing_ = std::move(other.writing_);
    }
    return *this;
}

bool Client::write_(std::string_view frame) {
    std::lock_guard l(*writing_);
    return platform::write_all(fd_, frame);
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

std::optional<std::vector<std::pair<char, std::pair<std::uint32_t, std::string>>>> Client::poll(int timeout_ms) {
    std::vector<std::pair<char, std::pair<std::uint32_t, std::string>>> out;
    std::array<platform::PollFd, 1> p{{{.fd = fd_}}};
    if (platform::poll_fds(p, timeout_ms) > 0) {
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

std::optional<int> running(const fs::path& home, std::string_view name) {
    const auto state = read_json_(state_file_(home, name));
    const int pid = state.value("supervisor", 0);
    if (pid > 0 && platform::process_alive(pid)) return pid;
    return std::nullopt;
}

int enter(const service::Context& c, std::string_view name_or_image, const EnterOptions& o) {
    if constexpr (!platform::is_linux) {
        say_(c, service::error("machine mode runs on Linux hosts in this version",
                               std::format("luban enter {} (the sandbox), or luban export {} {}.iso", name_or_image, name_or_image, name_or_image)));
        return 1;
    }
    std::error_code ec;
    if (fs::is_regular_file(name_or_image, ec)) return boot_image_(c, fs::absolute(name_or_image), o);
    const std::string name(name_or_image);
    if (o.display == "window") {
        const auto app = platform::get_executable_path().parent_path() / "luban-os";
        if (!fs::exists(app, ec)) {
            say_(c, service::error("a machine's window is the Luban OS app's", "xlings install luban-os"));
            return 1;
        }
        return platform::run_argv({app.string(), "--attach", name});
    }
    if (!running(c.home, name)) {
        if (const int rc = start_impl_(c, name, o); rc != 0) return rc;
    } else if (o.temp) {
        say_(c, service::notice(std::format("{} is already running and keeps what it writes; this session joins it", name)));
    }
    if (auto e = service::describe(c, name))
        for (const auto& line : service::welcome(*e, "machine", o.temp, service::chinese_locale())) std::println("{}", line);
    auto [ending, code] = session_(c, name);
    const auto state = read_json_(state_file_(c.home, name));
    const bool temp = state.value("temp", false) || state.value("persist", "all") != "all";
    if (ending == Ending::Detached) {
        std::println("{} keeps running: luban enter {} · luban stop {}", name, name, name);
        return 0;
    }
    if (ending == Ending::Lost && !running(c.home, name)) {
        std::println("{} is off", name);
        return code;
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

service::Outcome stop(const service::Context& c, std::string_view name) {
    auto pid = running(c.home, name);
    if (!pid) return {.code = 0};
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

int supervise(const fs::path& home, std::string_view name) {
    auto state = read_json_(state_file_(home, name));
    std::vector<std::string> q;
    for (const auto& a : state.value("qemu", Json::array())) q.push_back(a.get<std::string>());
    if (q.empty()) return 1;
    state["supervisor"] = platform::get_pid();
    platform::write_file_atomic(state_file_(home, name).string(), state.dump(2));
    const auto dir = run_dir(home, name);
    std::error_code ec;
    fs::remove(dir / "agent.sock", ec);
    platform::ignore_signals({platform::sig::pipe, platform::sig::hangup});
    const int qemu = platform::fork_process();
    if (qemu == 0) {
        platform::exec_path(q, platform::environment());
        platform::exit_now(127);
    }
    if (qemu < 0) return 1;
    int agent = -1;
    for (int i = 0; i < 100 && agent < 0; ++i) {
        agent = platform::unix_connect_stream(dir / "agent.sock");
        if (agent < 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const int listen = platform::unix_listen_stream(client_socket_(home, name));
    if (agent < 0 || listen < 0) {
        platform::send_signal(qemu, platform::sig::terminate);
        return 1;
    }
    platform::route_signals({platform::sig::terminate, platform::sig::interrupt}, {});
    struct Client { int fd; proto::Decoder d; std::map<std::uint32_t, std::uint32_t> channels; };   // local -> global
    std::vector<Client> clients;
    std::map<std::uint32_t, std::pair<int, std::uint32_t>> owners;   // global -> (client fd, local)
    std::uint32_t next = 1;
    bool up = false;
    proto::Decoder from_agent;
    std::array<char, 65536> buf{};
    auto powering_off = std::optional<std::chrono::steady_clock::time_point>{};
    for (;;) {
        if (auto ended = platform::wait_process(qemu, false)) break;
        for (int s : platform::pending_signals()) {
            if (!powering_off && (s == platform::sig::terminate || s == platform::sig::interrupt)) {
                (void)platform::write_all(agent, proto::encode('P', 0));
                powering_off = std::chrono::steady_clock::now();
            }
        }
        if (powering_off && std::chrono::steady_clock::now() - *powering_off > std::chrono::seconds(20))
            platform::send_signal(qemu, platform::sig::terminate);
        std::vector<platform::PollFd> p{{.fd = agent}, {.fd = listen}};
        for (const auto& cl : clients) p.push_back({.fd = cl.fd});
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
                    for (auto& cl : clients) (void)platform::write_all(cl.fd, proto::encode('H', 0, frame.payload));
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
                    if (up) (void)platform::write_all(cl.fd, proto::encode('H', 0));
                    continue;
                }
                if (frame.type == 'P') {
                    (void)platform::write_all(agent, proto::encode('P', 0));
                    if (!powering_off) powering_off = std::chrono::steady_clock::now();
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
    }
    (void)platform::wait_process(qemu, true);
    std::vector<int> fds{agent, listen};
    for (auto& cl : clients) if (cl.fd >= 0) fds.push_back(cl.fd);
    platform::close_fds(fds);
    if (const auto scratch = state.value("scratch", std::string{}); !scratch.empty() && scratch.starts_with(dir.string()))
        fs::remove_all(scratch, ec);
    fs::remove(client_socket_(home, name), ec);
    fs::remove(state_file_(home, name), ec);
    return 0;
}

}  // namespace luban::vm
