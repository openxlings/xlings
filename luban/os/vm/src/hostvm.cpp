// The Luban host machine on a Mac (part 4 §6.2): `luban __vm <verb>`, the
// vz carrier's helper -- the contract modules/carrier/src/vz.cppm names:
//
//   probe                                   0: this Mac can run it (stdout: what)
//   status --name N                         0 running, 3 stopped, 4 absent
//   create --name N --dir D --image T       its tree from the carrier's image
//   start  --name N | stop --name N
//   share  --name N --host P --tag G --mode rw|ro     stdout: the path inside
//   exec   --name N -- <argv...>            stdio attached, the exit code passed
//
// Its tree is on a case-sensitive APFS volume (a sparse image under D, made
// and attached with the system's hdiutil): Linux payloads need case, links
// and modes as a Linux file system keeps them, and the Mac sees the same
// files, so a machine of one of its systems boots from them directly. The
// machine is a Luban machine (luban.vm) run by Virtualization.framework: the
// Linux luban of its tree as the initramfs's /init, the tree shared by
// virtio-fs, the agent its only process, grants under /grant.
module luban.vm;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.platform.stream;
import xlings.observe;
import xlings.image;
import xlings.subos.home_view;
import xlings.subos.ports;
import xlings.subos.tools;
import luban.service;
import luban.vm.protocol;
import luban.vm.native.vz;

namespace luban::vm {

namespace {

using Json = nlohmann::json;
namespace platform = xlings::platform;
namespace proto = luban::vm::protocol;

constexpr int kStopped = 3;
constexpr int kAbsent = 4;

struct Args {
    std::string verb, name, dir, image, host, tag, mode;
    std::vector<std::string> argv;
};

Args parse_(std::span<const std::string> args) {
    Args a;
    if (!args.empty()) a.verb = args[0];
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& w = args[i];
        const auto value = [&](std::string& into) { if (i + 1 < args.size()) into = args[++i]; };
        if (w == "--") { a.argv.assign(args.begin() + static_cast<std::ptrdiff_t>(i + 1), args.end()); break; }
        if (w == "--name") value(a.name);
        else if (w == "--dir") value(a.dir);
        else if (w == "--image") value(a.image);
        else if (w == "--host") value(a.host);
        else if (w == "--tag") value(a.tag);
        else if (w == "--mode") value(a.mode);
    }
    return a;
}

fs::path carrier_dir_(const fs::path& home) { return home / "carriers" / "vz"; }
fs::path tree_(const fs::path& home) { return carrier_dir_(home) / "root"; }
fs::path volume_(const fs::path& home) { return carrier_dir_(home) / "root.sparseimage"; }

// A tool of the operating system (T0), from the one table.
std::optional<fs::path> os_tool_(const fs::path& home, std::string_view name) {
    if (auto found = xlings::subos::tools::first(std::string(name), xlings::subos::HomeView{home}, xlings::subos::Ports{}))
        return found->bin;
    return std::nullopt;
}

int run_tool_(const fs::path& home, std::string_view tool, std::vector<std::string> args, std::string* out = nullptr) {
    auto bin = os_tool_(home, tool);
    if (!bin) {
        std::println(std::cerr, "luban __vm: {} is not here", tool);
        return 127;
    }
    args.insert(args.begin(), bin->string());
    if (out) {
        std::string got;
        const int rc = platform::stream::run(args, [&](std::string_view, std::string_view b) { got.append(b); });
        *out = got;
        return rc;
    }
    return platform::run_argv(args);
}

// The volume attached at its tree (attached once; a Mac restarts with it detached).
bool attached_(const fs::path& home) {
    std::error_code ec;
    if (fs::exists(tree_(home) / "xlings", ec)) return true;
    if (!fs::exists(volume_(home), ec)) return false;
    fs::create_directories(tree_(home), ec);
    return run_tool_(home, "hdiutil", {"attach", "-nobrowse", "-quiet", "-mountpoint", tree_(home).string(), volume_(home).string()}) == 0
        && fs::exists(tree_(home) / "xlings", ec);
}

int connect_(const fs::path& home, std::string_view name) {
    return platform::unix_connect_stream(run_dir(home, name) / "luban.sock");
}

std::optional<proto::Frame> await_(int fd, proto::Decoder& d, char want, std::chrono::seconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::array<char, 65536> buf{};
    while (std::chrono::steady_clock::now() < deadline) {
        for (;;) {
            auto f = d.next();
            if (!f || !*f) break;
            if ((**f).type == want) return **f;
        }
        std::array<platform::PollFd, 1> p{{{.fd = fd}}};
        platform::poll_fds(p, 200);
        if (p[0].readable || p[0].closed) {
            auto n = platform::read_some(fd, buf);
            if (!n || *n == 0) return std::nullopt;
            d.feed(std::string_view(buf.data(), *n));
        }
    }
    return std::nullopt;
}

int probe_() {
    const auto why = vz::unsupported();
    if (!why.empty()) {
        std::println("{}", why);
        return 1;
    }
    std::println("Virtualization.framework");
    return 0;
}

int status_(const fs::path& home, const Args& a) {
    if (running(home, a.name)) return 0;
    std::error_code ec;
    return fs::exists(volume_(home), ec) ? kStopped : kAbsent;
}

int create_(const fs::path& home, const Args& a) {
    std::error_code ec;
    if (fs::exists(volume_(home), ec)) {
        std::println(std::cerr, "luban __vm: {} exists", volume_(home).string());
        return 1;
    }
    fs::create_directories(carrier_dir_(home), ec);
    // Sparse: it takes the space its files take, up to its size.
    if (const int rc = run_tool_(home, "hdiutil", {"create", "-quiet", "-size", "256g", "-type", "SPARSE", "-fs", "Case-sensitive APFS",
                                                   "-volname", "luban-host", (carrier_dir_(home) / "root").string()});
        rc != 0)
        return rc;
    if (!attached_(home) && run_tool_(home, "hdiutil", {"attach", "-nobrowse", "-quiet", "-mountpoint", tree_(home).string(),
                                                        volume_(home).string()}) != 0)
        return 1;
    return run_tool_(home, "tar", {"-xzf", a.image, "-C", tree_(home).string()});
}

int start_(const fs::path& home, const Args& a) {
    if (running(home, a.name)) return 0;
    if (!attached_(home)) {
        std::println(std::cerr, "luban __vm: the host machine's volume does not attach ({})", volume_(home).string());
        return 1;
    }
    const auto tree = tree_(home);
    std::error_code ec;
    const auto kernel = tree / "boot" / "vmlinuz";
    const auto guest_luban = tree / "xlings" / "bin" / "luban";
    if (!fs::exists(kernel, ec) || !is_static_elf(guest_luban)) {
        std::println(std::cerr, "luban __vm: the host machine's tree has no kernel (boot/vmlinuz) or no static Linux luban");
        return 1;
    }
    const auto dir = run_dir(home, a.name);
    fs::create_directories(dir, ec);
    fs::create_directories(log_dir(home, a.name), ec);
    // The initramfs: the tree's Linux luban as /init.
    std::vector<xlings::image::CpioEntry> entries;
    for (const auto* d : {"dev", "proc", "sys", "newroot"})
        entries.push_back({.path = d, .kind = xlings::image::CpioEntry::Kind::Dir, .mode = 0755});
    std::ifstream in(guest_luban, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {});
    entries.push_back({.path = "init", .kind = xlings::image::CpioEntry::Kind::File, .data = std::move(bytes), .mode = 0755});
    if (auto w = xlings::image::write_cpio(dir / "initramfs.cpio", entries); !w) {
        std::println(std::cerr, "luban __vm: {}", w.error());
        return 1;
    }
    const Json state{{"name", a.name}, {"hypervisor", "vz"}, {"started", xlings::observe::utc_now()},
                     {"vz", {{"kernel", kernel.string()}, {"initrd", (dir / "initramfs.cpio").string()},
                             {"cmdline", "console=hvc0 quiet panic=-1 luban.carrier=1 luban.fs=virtiofs"},
                             {"cpus", 2}, {"memory", 2048}, {"grants", true}, {"network", true},
                             {"shares", Json::array({{{"tag", "root"}, {"dir", tree.string()}, {"read_only", false}}})}}}};
    platform::write_file_atomic((dir / "state.json").string(), state.dump(2));
    auto pid = platform::spawn_detached({platform::get_executable_path().string(), "__vm-supervise", home.string(), a.name},
                                        log_dir(home, a.name) / "supervisor.log");
    if (!pid) return 1;
    // Up when its agent answers through the supervisor.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline && platform::process_alive(*pid)) {
        if (const int fd = connect_(home, a.name); fd >= 0) {
            proto::Decoder d;
            (void)platform::write_all(fd, proto::encode('Q', 0));
            const auto hello = await_(fd, d, 'H', std::chrono::seconds(2));
            std::vector<int> close{fd};
            platform::close_fds(close);
            if (hello) return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::println(std::cerr, "luban __vm: the host machine did not come up (its console: {})",
                 (log_dir(home, a.name) / "console.log").string());
    return 1;
}

int stop_(const fs::path& home, const Args& a) {
    const int fd = connect_(home, a.name);
    if (fd < 0) return 0;
    (void)platform::write_all(fd, proto::encode('P', 0));
    std::vector<int> close{fd};
    platform::close_fds(close);
    for (int i = 0; i < 300 && running(home, a.name); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return running(home, a.name) ? 1 : 0;
}

int share_(const fs::path& home, const Args& a) {
    const int fd = connect_(home, a.name);
    if (fd < 0) return kStopped;
    proto::Decoder d;
    (void)platform::write_all(fd, proto::encode_json('S', 0, Json{{"grants", {{a.tag, Json::array({a.host, a.mode == "ro"})}}}}));
    const auto reply = await_(fd, d, 'S', std::chrono::seconds(10));
    std::vector<int> close{fd};
    platform::close_fds(close);
    const auto j = reply ? Json::parse(reply->payload, nullptr, false) : Json();
    if (!j.is_object() || !j.value("ok", false)) return 1;
    std::println("/grant/{}", a.tag);
    return 0;
}

// The command, there: on a terminal when this has one (its size, raw), else
// on pipes -- what a program reads, as it was written.
int exec_(const fs::path& home, const Args& a) {
    if (a.argv.empty()) return 2;
    const int fd = connect_(home, a.name);
    if (fd < 0) return 125;
    const bool tty = platform::stdin_is_terminal() && platform::stdout_is_terminal();
    Json open{{"argv", a.argv}};
    if (tty) {
        const auto size = platform::terminal_size(0).value_or(std::pair<unsigned short, unsigned short>{24, 80});
        const char* term = std::getenv("TERM");
        open["rows"] = size.first;
        open["cols"] = size.second;
        open["term"] = term ? term : "xterm-256color";
    } else {
        open["pipes"] = true;
    }
    if (!platform::write_all(fd, proto::encode_json('O', 1, open))) return 125;
    const auto saved = tty ? platform::make_terminal_raw(0) : std::nullopt;
    proto::Decoder d;
    std::array<char, 65536> buf{};
    bool input = true;
    int code = 125;
    for (bool done = false; !done;) {
        std::array<platform::PollFd, 2> p{{{.fd = input ? 0 : -1}, {.fd = fd}}};
        platform::poll_fds(p, 200);
        if (p[1].readable || p[1].closed) {
            auto n = platform::read_some(fd, buf);
            if (!n || *n == 0) break;
            d.feed(std::string_view(buf.data(), *n));
            for (;;) {
                auto f = d.next();
                if (!f || !*f) break;
                const auto& frame = **f;
                if (frame.type == 'D') (void)platform::write_all(1, frame.payload);
                else if (frame.type == 'E') (void)platform::write_all(2, frame.payload);
                else if (frame.type == 'X') {
                    const auto j = Json::parse(frame.payload, nullptr, false);
                    code = j.is_object() ? j.value("code", 0) : 0;
                    if (j.is_object() && j.contains("error")) std::println(std::cerr, "{}", j.value("error", std::string{}));
                    done = true;
                }
            }
        }
        if (input && (p[0].readable || p[0].closed)) {
            auto n = platform::read_some(0, buf);
            if (!n || *n == 0) {
                input = false;
                if (!tty) (void)platform::write_all(fd, proto::encode('F', 1));
            } else {
                (void)platform::write_all(fd, proto::encode('D', 1, std::string_view(buf.data(), *n)));
            }
        }
    }
    if (saved) platform::restore_terminal(0, *saved);
    std::vector<int> close{fd};
    platform::close_fds(close);
    return code;
}

}  // namespace

int host_machine(const fs::path& home, std::span<const std::string> args) {
    const auto a = parse_(args);
    if (a.verb == "probe") return probe_();
    if (a.name.empty()) {
        std::println(std::cerr, "luban __vm {}: --name is needed", a.verb);
        return 2;
    }
    if (a.verb == "status") return status_(home, a);
    if (a.verb == "create") return create_(home, a);
    if (a.verb == "start") return start_(home, a);
    if (a.verb == "stop") return stop_(home, a);
    if (a.verb == "share") return share_(home, a);
    if (a.verb == "exec") return exec_(home, a);
    std::println(std::cerr, "luban __vm: no verb {}", a.verb);
    return 2;
}

}  // namespace luban::vm
