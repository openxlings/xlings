module luban.vm;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.subos.root_etc;
import xlings.subos.rootfs;
import xlings.subos.root_check;
import luban.vm.protocol;
import opensal.wlr;

namespace luban::vm {

namespace {

using Json = nlohmann::json;
namespace platform = xlings::platform;
namespace proto = luban::vm::protocol;

void console_(std::string_view line) {
    std::println(std::cerr, "luban init: {}", line);
}

std::map<std::string, std::string> cmdline_() {
    std::map<std::string, std::string> out;
    std::ifstream in("/proc/cmdline");
    std::string word;
    while (in >> word) {
        const auto eq = word.find('=');
        if (word.starts_with("luban.")) out[word.substr(0, eq)] = eq == std::string::npos ? "" : word.substr(eq + 1);
    }
    return out;
}

[[noreturn]] void give_up_(std::string_view why) {
    console_(why);
    console_("powering off in 10 seconds");
    std::this_thread::sleep_for(std::chrono::seconds(10));
    platform::power_off();
}

// The virtio-serial port the host named luban.agent, as a device node.
std::optional<fs::path> port_() {
    std::error_code ec;
    for (fs::directory_iterator it("/sys/class/virtio-ports", ec), end; !ec && it != end; it.increment(ec)) {
        std::ifstream name(it->path() / "name");
        std::string n;
        std::getline(name, n);
        if (n == proto::kPort) return fs::path("/dev") / it->path().filename();
    }
    return std::nullopt;
}

// A session: a shell on a pty (`read` and `write` the master), or a desktop
// bridged on two pipes.
struct Session {
    int read { -1 };
    int write { -1 };
    int pid { -1 };
    bool desktop { false };
    int err { -1 };             // a command on pipes: its standard error, sent as E frames
};

void close_session_(Session& s) {
    std::vector<int> fds{s.read};
    if (s.write != s.read && s.write >= 0) fds.push_back(s.write);
    if (s.err >= 0) fds.push_back(s.err);
    platform::close_fds(fds);
}


// Who a session logs in as (part 3 §6.1): the --user it was opened with,
// else the root's etc/luban/login, else root; its passwd says the rest.
xlings::subos::root_etc::User login_(const Json& open) {
    auto name = open.value("user", std::string{});
    if (name.empty()) name = xlings::subos::root_etc::login_user("/etc");
    if (auto u = xlings::subos::root_etc::find_user("/etc", name)) return *u;
    return {"root", 0, 0, "/root", xlings::subos::root_etc::login_shell("/")};
}

// In a session's child, before its program: that user, at its home.
void as_login_(const xlings::subos::root_etc::User& u) {
    if (u.uid != 0 && !platform::become_user(u.uid, u.gid)) platform::exit_now(126);
    std::error_code ec;
    fs::current_path(u.home, ec);
}

std::map<std::string, std::string> session_env_(const Json& open, const std::string& shell,
                                                const xlings::subos::root_etc::User& u) {
    std::map<std::string, std::string> env{
        {"HOME", u.home}, {"USER", u.name}, {"LOGNAME", u.name}, {"SHELL", shell},
        {"PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"},
        {"TERM", open.value("term", std::string("xterm-256color"))}, {"LANG", "C.UTF-8"}};
    if (auto e = open.find("env"); e != open.end() && e->is_object())
        for (auto it = e->begin(); it != e->end(); ++it)
            if (it.value().is_string()) env[it.key()] = it.value().get<std::string>();
    return env;
}

// A command on pipes, not a terminal (`"pipes": true` with its argv): what a
// program reads -- an NDJSON stream must not be echoed or have its line ends
// changed. Its output comes back as D frames, its standard error as E.
std::optional<Session> piped_(const Json& open, const std::vector<int>& inherited) {
    auto in = platform::make_pipe(), out = platform::make_pipe(), err = platform::make_pipe();
    if (!in || !out || !err) return std::nullopt;
    const auto who = login_(open);
    const auto shell = xlings::subos::root_etc::login_shell("/", who.name);
    std::vector<std::string> argv;
    for (const auto& w : open.value("argv", Json::array()))
        if (w.is_string()) argv.push_back(w.get<std::string>());
    if (argv.empty()) return std::nullopt;
    const int pid = platform::fork_process();
    if (pid == 0) {
        platform::new_session();
        const std::array<int, 3> stdio{(*in)[0], (*out)[1], (*err)[1]};
        platform::redirect_stdio(stdio);
        // Its standard streams are its own now: the agent's (0 and 1 on a
        // relay) are not closed under it.
        auto rest = inherited;
        rest.insert(rest.end(), {(*in)[1], (*out)[0], (*err)[0]});
        std::erase_if(rest, [](int fd) { return fd <= 2; });
        platform::close_fds(rest);
        as_login_(who);
        platform::exec_program(argv, session_env_(open, shell, who));
        platform::exit_now(127);
    }
    std::vector<int> theirs{(*in)[0], (*out)[1], (*err)[1]};
    platform::close_fds(theirs);
    if (pid < 0) {
        std::vector<int> mine{(*in)[1], (*out)[0], (*err)[0]};
        platform::close_fds(mine);
        return std::nullopt;
    }
    return Session{.read = (*out)[0], .write = (*in)[1], .pid = pid, .err = (*err)[0]};
}

// A desktop for the Luban OS window: the session `display.session` names
// (or the first one declared), headless, bridged on two pipes (opensal-wlr).
std::expected<Session, std::string> desktop_(const Json& display, std::map<std::string, std::string> env,
                                         std::vector<int> inherited, const xlings::subos::root_etc::User& who) {
    namespace wlr = opensal::wlr;
    const auto dirs = wlr::session_dirs(env);
    const auto id = display.value("session", std::string{});
    std::optional<wlr::Session> session;
    if (!id.empty()) session = wlr::resolve_session(id, dirs);
    else if (auto all = wlr::sessions(dirs); !all.empty()) session = all.front();
    if (!session)
        return std::unexpected(id.empty() ? std::string("this environment declares no desktop session (share/wayland-sessions)")
                               : id.starts_with(wlr::kAppSession)
                                   ? std::string("nothing here runs one application in a window (luban:compositor does: "
                                                 "xlings install luban:compositor in it)")
                                   : std::format("this environment has no desktop session {}", id));
    auto to = platform::make_pipe(), from = platform::make_pipe();
    if (!to || !from) return std::unexpected("no pipe for the desktop");
    wlr::SessionOptions o;
    o.size = {display.value("width", 1280u), display.value("height", 800u)};
    o.env = std::move(env);
    std::error_code ec;
    fs::create_directories("/var/log/luban", ec);
    o.log = "/var/log/luban/desktop.log";
    // The user's own runtime directory and log: made here, as root, theirs
    // before the session drops to them.
    if (who.uid != 0) {
        o.runtime_root = fs::path("/run/user") / std::to_string(who.uid);
        fs::create_directories(o.runtime_root, ec);
        (void)platform::give_to(o.runtime_root, who.uid, who.gid);
        { std::ofstream touch(o.log, std::ios::app); }
        (void)platform::give_to(o.log, who.uid, who.gid);
    }
    const int pid = platform::fork_process();
    if (pid == 0) {
        // Nothing of the agent's stays open in the bridge (a shell's pty, the port).
        inherited.insert(inherited.end(), {(*to)[1], (*from)[0]});
        std::erase_if(inherited, [](int fd) { return fd <= 2; });
        platform::close_fds(inherited);
        // A desktop is its user's (part 3 §6.1): a browser or an Electron
        // application refuses root, or runs without its sandbox.
        as_login_(who);
        platform::exit_now(wlr::run_session(*session, (*to)[0], (*from)[1], o));
    }
    std::vector<int> mine{(*to)[0], (*from)[1]};
    platform::close_fds(mine);
    if (pid < 0) {
        std::vector<int> rest{(*to)[1], (*from)[0]};
        platform::close_fds(rest);
        return std::unexpected("cannot start the desktop");
    }
    console_(std::format("desktop {} ({})", session->id, session->exec));
    return Session{(*from)[0], (*to)[1], pid, true};
}

[[noreturn]] void serve_(int port, int out, bool init, bool machine);

[[noreturn]] void agent_(bool init = false) {
    if (!init) platform::new_session();
    std::optional<fs::path> dev;
    for (int i = 0; i < 300 && !dev; ++i) {
        dev = port_();
        if (!dev) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!dev) { console_("no luban.agent port: this machine has no sessions"); platform::exit_now(1); }
    const int port = platform::open_device(*dev);
    if (port < 0) { console_("cannot open " + dev->string()); platform::exit_now(1); }
    serve_(port, port, init, true);
}

// What the agent says when it is up (design 2026-10-18 §9): that it is one,
// and what it provides (opencaps names: luban/specs/opencaps/SPEC.md).
Json hello_() { return Json{{"agent", 1}, {"provides", Json{{"agent-display", 1}}}}; }

// The agent's loop on its port: sessions, commands, a desktop, power off.
[[noreturn]] void serve_(int port, int out, bool init, bool machine) {
    (void)platform::write_all(out, proto::encode_json('H', 0, hello_()));
    std::map<std::uint32_t, Session> sessions;
    std::vector<int> ended;     // closed by the host, not yet reaped
    proto::Decoder d;
    std::array<char, 65536> buf{};
    std::optional<std::chrono::steady_clock::time_point> powering_off;
    for (;;) {
        if (powering_off && std::chrono::steady_clock::now() - *powering_off > std::chrono::seconds(10)) platform::power_off();
        std::vector<platform::PollFd> p{{.fd = port}};
        std::vector<std::uint32_t> order;
        for (const auto& [ch, s] : sessions) { p.push_back({.fd = s.read}); order.push_back(ch); }
        // A piped command's standard error, after every session's output.
        std::vector<std::uint32_t> errs;
        for (const auto& [ch, s] : sessions)
            if (s.err >= 0) { p.push_back({.fd = s.err}); errs.push_back(ch); }
        std::erase_if(ended, [](int pid) { return platform::wait_process(pid, false).has_value(); });
        platform::poll_fds(p, 500);
        if (p[0].readable) {
            auto n = platform::read_some(port, buf);
            if (n && *n > 0) d.feed(std::string_view(buf.data(), *n));
            else std::this_thread::sleep_for(std::chrono::milliseconds(100));   // the host is not connected
            for (;;) {
                auto f = d.next();
                if (!f) { d = proto::Decoder{}; break; }
                if (!*f) break;
                const auto& frame = **f;
                if (frame.type == 'Q') {
                    (void)platform::write_all(out, proto::encode_json('H', 0, hello_()));
                } else if (frame.type == 'O') {
                    auto open = Json::parse(frame.payload, nullptr, false);
                    if (!open.is_object()) open = Json::object();
                    if (auto display = open.find("display"); display != open.end() && display->is_object()) {
                        std::vector<int> inherited{port, out};
                        for (const auto& [ch, other] : sessions) {
                            inherited.push_back(other.read);
                            if (other.write != other.read) inherited.push_back(other.write);
                        }
                        // Its terminals open the root's login shell, as a session's does.
                        const auto who = login_(open);
                        auto s = desktop_(*display, session_env_(open, xlings::subos::root_etc::login_shell("/", who.name), who),
                                          std::move(inherited), who);
                        if (!s) {
                            (void)platform::write_all(out, proto::encode_json('X', frame.channel, Json{{"code", 127}, {"error", s.error()}}));
                            continue;
                        }
                        sessions[frame.channel] = *s;
                        continue;
                    }
                    if (open.value("pipes", false)) {
                        std::vector<int> inherited{port, out};
                        for (const auto& [ch, other] : sessions) {
                            inherited.push_back(other.read);
                            if (other.write != other.read && other.write >= 0) inherited.push_back(other.write);
                            if (other.err >= 0) inherited.push_back(other.err);
                        }
                        if (auto piped = piped_(open, inherited)) sessions[frame.channel] = *piped;
                        else (void)platform::write_all(out, proto::encode_json('X', frame.channel, Json{{"code", 125}, {"error", "the command could not start"}}));
                        continue;
                    }
                    auto pty = platform::open_pty();
                    if (!pty) {
                        console_(std::format("no pty for a session: {}", platform::error_text(platform::last_error())));
                        (void)platform::write_all(out, proto::encode_json('X', frame.channel, Json{{"code", 125}}));
                        continue;
                    }
                    platform::set_terminal_size((*pty)[0], open.value("rows", 24), open.value("cols", 80));
                    const auto who = login_(open);
                    const auto shell = xlings::subos::root_etc::login_shell("/", who.name);
                    const int pid = platform::fork_process();
                    if (pid == 0) {
                        platform::new_session();
                        const std::array<int, 3> stdio{(*pty)[1], (*pty)[1], (*pty)[1]};
                        platform::redirect_stdio(stdio);
                        platform::take_controlling_terminal();
                        as_login_(who);
                        std::vector<std::string> argv{shell, "-l"};
                        if (auto a = open.find("argv"); a != open.end() && a->is_array() && !a->empty()) {
                            argv.clear();
                            for (const auto& w : *a) argv.push_back(w.get<std::string>());
                        }
                        platform::exec_program(argv, session_env_(open, shell, who));
                        platform::exit_now(127);
                    }
                    std::vector<int> slave{(*pty)[1]};
                    platform::close_fds(slave);
                    sessions[frame.channel] = {(*pty)[0], (*pty)[0], pid};
                } else if (frame.type == 'D') {
                    if (auto s = sessions.find(frame.channel); s != sessions.end()) (void)platform::write_all(s->second.write, frame.payload);
                } else if (frame.type == 'F') {
                    // The end of a piped command's input.
                    if (auto s = sessions.find(frame.channel); s != sessions.end() && s->second.err >= 0 && s->second.write >= 0) {
                        std::vector<int> in{s->second.write};
                        platform::close_fds(in);
                        s->second.write = -1;
                    }
                } else if (frame.type == 'R') {
                    auto j = Json::parse(frame.payload, nullptr, false);
                    if (auto s = sessions.find(frame.channel); s != sessions.end() && j.is_object() && !s->second.desktop)
                        platform::set_terminal_size(s->second.read, j.value("rows", 24), j.value("cols", 80));
                } else if (frame.type == 'C') {
                    if (auto s = sessions.find(frame.channel); s != sessions.end()) {
                        // A desktop ends on its stream's end (and stops its compositor); a shell is hung up on.
                        if (!s->second.desktop) platform::send_signal(s->second.pid, platform::sig::hangup);
                        close_session_(s->second);
                        ended.push_back(s->second.pid);
                        sessions.erase(s);
                    }
                } else if (frame.type == 'P') {
                    // A relay's agent (a PC's host machine, on stdio) powers nothing off.
                    if (!machine) continue;
                    // The agent as PID 1 (the host machine) powers off itself.
                    if (init) platform::power_off();
                    // busybox init powers off on SIGUSR2; anything else in 10 s.
                    platform::send_signal(1, platform::sig::user2);
                    if (!powering_off) powering_off = std::chrono::steady_clock::now();
                }
            }
        }
        for (std::size_t k = 0; k < errs.size(); ++k) {
            const auto& pf = p[1 + order.size() + k];
            if (!pf.readable && !pf.closed) continue;
            auto s = sessions.find(errs[k]);
            if (s == sessions.end() || s->second.err < 0) continue;
            auto n = platform::read_some(s->second.err, buf);
            if (n && *n > 0) (void)platform::write_all(out, proto::encode('E', errs[k], std::string_view(buf.data(), *n)));
            else {
                std::vector<int> gone{s->second.err};
                platform::close_fds(gone);
                s->second.err = -1;
            }
        }
        for (std::size_t k = 0; k < order.size(); ++k) {
            const auto& pf = p[1 + k];
            if (!pf.readable && !pf.closed) continue;
            auto s = sessions.find(order[k]);
            if (s == sessions.end()) continue;
            auto n = platform::read_some(s->second.read, buf);
            if (n && *n > 0) {
                (void)platform::write_all(out, proto::encode('D', order[k], std::string_view(buf.data(), *n)));
                continue;
            }
            int code = 0;
            if (auto st = platform::wait_process(s->second.pid, true)) code = platform::exit_code(*st);
            (void)platform::write_all(out, proto::encode_json('X', order[k], Json{{"code", code}}));
            close_session_(s->second);
            sessions.erase(s);
        }
    }
}

// The Luban host machine's first process, after /dev, /proc and /sys: its
// tree from the share, the granted directories at /grant, and the agent as
// PID 1 -- it runs what the host asks (`luban __vm exec`), nothing else.
[[noreturn]] void host_machine_(const std::function<bool(std::string_view, const fs::path&, bool)>& share) {
    if (!share("root", "/newroot", false)) give_up_("the host machine's tree (tag root) did not mount");
    if (!platform::mount_tmpfs("/newroot/run", 0755)) console_("cannot mount /run");
    if (!platform::mount_tmpfs("/newroot/tmp", 01777)) console_("cannot mount /tmp");
    for (const auto* at : {"/dev", "/proc", "/sys"})
        if (!platform::move_mount(at, fs::path("/newroot") / fs::path(at).relative_path()))
            give_up_(std::format("cannot move {} into the host machine", at));
    if (!platform::switch_root("/newroot")) give_up_("cannot make the host machine's tree /");
    if (!platform::mount_kernel_fs("devpts", "/dev/pts")) console_("cannot mount devpts: sessions will fail");
    // What the host grants later appears under /grant (one share, a directory
    // per grant); a host that grants nothing has none.
    if (!share("grant", "/grant", false)) console_("no granted directories (tag grant)");
    (void)platform::set_hostname("luban-host");
    agent_(true);
}


// The name servers of the kernel's lease (/proc/net/pnp: "nameserver a.b.c.d"
// lines), in /run/luban/resolv.conf, bound over /etc/resolv.conf. A link
// there (a distribution's /run/systemd/... one) is left alone.
void root_owned_sudo_() {
    std::error_code ec;
    if (!fs::exists("/usr/bin/sudo", ec)) return;
    const fs::path dir = "/run/luban/root";
    fs::create_directories(dir, ec);
    const auto place = [&](const fs::path& from, const fs::path& copy, unsigned mode) {
        std::error_code e;
        fs::copy(fs::canonical(from, e), copy, fs::copy_options::overwrite_existing | fs::copy_options::recursive, e);
        if (e) return console_(std::format("sudo: cannot copy {}: {}", from.string(), e.message()));
        if (fs::is_directory(copy, e)) {
            for (const auto& f : fs::recursive_directory_iterator(copy, e)) (void)platform::own_by_root(f.path(), 0440);
            (void)platform::own_by_root(copy, 0750);
        } else if (!platform::own_by_root(copy, mode)) {
            return console_(std::format("sudo: cannot make {} root's", from.string()));
        }
        if (!platform::bind_mount(copy, from)) console_(std::format("sudo: cannot place {}", from.string()));
    };
    // Only sudo itself: a program reached through a shim is not bound over
    // (that would replace the shim's program).
    if (std::error_code e; fs::canonical("/usr/bin/sudo", e).filename() != "sudo")
        return console_("sudo: /usr/bin/sudo is not the sudo program; left as it is");
    place("/usr/bin/sudo", dir / "sudo", 04755);
    if (fs::exists("/etc/sudoers", ec)) place("/etc/sudoers", dir / "sudoers", 0440);
    if (fs::is_directory("/etc/sudoers.d", ec)) place("/etc/sudoers.d", dir / "sudoers.d", 0750);
}

void resolv_from_lease_() {
    std::ifstream pnp("/proc/net/pnp");
    std::string text, line;
    while (std::getline(pnp, line))
        if (line.starts_with("nameserver") || line.starts_with("domain") || line.starts_with("search")) text += line + "\n";
    if (text.find("nameserver") == std::string::npos) return;
    std::error_code ec;
    fs::create_directories("/run/luban", ec);
    {
        std::ofstream out("/run/luban/resolv.conf", std::ios::trunc);
        out << text;
        if (!out) { console_("cannot write /run/luban/resolv.conf"); return; }
    }
    const auto at = fs::symlink_status("/etc/resolv.conf", ec);
    if (at.type() == fs::file_type::symlink) {
        console_("/etc/resolv.conf is a link: the lease's name servers are in /run/luban/resolv.conf");
        return;
    }
    if (at.type() == fs::file_type::not_found) std::ofstream("/etc/resolv.conf");   // a mount point
    if (!platform::bind_mount("/run/luban/resolv.conf", "/etc/resolv.conf"))
        console_("cannot place the lease's name servers at /etc/resolv.conf");
}


}  // namespace

void serve_agent(int fd) { serve_(fd, fd, false, true); }
void serve_stdio() { serve_(0, 1, false, false); }

int guest_init(int, char*[]) {
    if (!platform::is_pid1()) {
        std::println(std::cerr, "luban init: a Luban machine's first process; this is not one");
        return 1;
    }
    // The initramfs has no /dev/console, so the kernel handed PID 1 no
    // standard streams: the console once /dev is there.
    if (platform::mount_kernel_fs("devtmpfs", "/dev"))
        if (const int console = platform::open_device("/dev/console"); console >= 0) {
            const std::array<int, 3> stdio{console, console, console};
            platform::redirect_stdio(stdio);
        }
    for (const auto& [type, at] : {std::pair{"proc", "/proc"}, {"sysfs", "/sys"}})
        if (!platform::mount_kernel_fs(type, at)) give_up_(std::format("cannot mount {} at {}", type, at));
    const auto params = cmdline_();
    // How the host shares a directory: 9p (qemu), or virtio-fs (a Mac's
    // Virtualization.framework, part 4 §6.6) -- the same tags either way.
    const bool virtiofs = params.contains("luban.fs") && params.at("luban.fs") == "virtiofs";
    const auto share = [&](std::string_view tag, const fs::path& at, bool ro) {
        return virtiofs ? platform::mount_virtiofs(tag, at, ro) : platform::mount_9p(tag, at, ro);
    };
    // The Luban host machine (part 4 §6.2): its whole tree is the share, its
    // home is /xlings in it, and the agent is all it runs.
    if (params.contains("luban.carrier")) host_machine_(share);
    const auto home = params.contains("luban.home") ? fs::path(params.at("luban.home")) : fs::path{};
    if (home.empty() || !home.is_absolute()) give_up_("no luban.home on the kernel's command line");
    if (!share("root", "/newroot", false)) give_up_("the environment's tree (tag root) did not mount");
    // A machine's /run and /tmp are its own and go with it, as on any system
    // (the tree would keep them: a desktop's sockets, owned by the host's user
    // there, refuse the next session). Before the home, which may be under one.
    if (!platform::mount_tmpfs("/newroot/run", 0755)) console_("cannot mount /run");
    if (!platform::mount_tmpfs("/newroot/tmp", 01777)) console_("cannot mount /tmp");
    if (!share("home", fs::path("/newroot") / home.relative_path(), true))
        give_up_("the home (tag home) did not mount");
    if (params.contains("luban.userhome") && params.at("luban.userhome") == "1"
        && !share("userhome", "/newroot/home", false))
        give_up_("its /home (tag userhome) did not mount");
    for (const auto* at : {"/dev", "/proc", "/sys"})
        if (!platform::move_mount(at, fs::path("/newroot") / fs::path(at).relative_path()))
            give_up_(std::format("cannot move {} into the environment", at));
    if (!platform::switch_root("/newroot")) give_up_("cannot make the environment /");
    // Its /usr is the generation, mounted, not a link into the store (design
    // 2026-10-16 L §6.2): a root from before is laid out so once, here.
    if (!home.empty() && params.contains("luban.name")) {
        const auto subos = home / "subos" / params.at("luban.name");
        if (auto laid = xlings::subos::rootfs::lay_out("/", xlings::subos::rootfs::usr_of(subos), home); !laid)
            console_("its /usr: " + laid.error());
        else if (auto mounted = xlings::subos::rootfs::mount_usr("/", subos); !mounted)
            console_("its /usr: " + mounted.error());
    }
    // Its own default runtime is the home's runtime indirection here (part 3
    // §4), read through the /usr just mounted: what a payload bound through <home>/abi runs on.
    if (std::error_code rec; fs::is_directory("/usr/lib/xlings-abi", rec) && fs::exists(home / "abi", rec)
        && !platform::bind_mount("/usr/lib/xlings-abi", home / "abi"))
        console_("cannot place its runtime at " + (home / "abi").string());
    // The fast self-check (design 2026-10-16 L §11), said on the console: the
    // host chose this system, so it starts, and what is wrong is in the log.
    for (const auto& item : xlings::subos::root_check::check("/", xlings::subos::root_check::Depth::Fast, {.run = false}).items)
        if (item.critical && !item.ok) console_(std::format("not usable: {}: {}", item.what, item.detail));
    // The agent's sessions are ptys before the environment's own start mounts
    // anything (its rcS mounting devpts again is harmless: already mounted).
    if (!platform::mount_kernel_fs("devpts", "/dev/pts")) console_("cannot mount devpts: sessions will fail");

    // sudo is root's (design 2026-10-20 §4.5): its program root-owned and
    // setuid, its rules root-owned -- a share shows the host's owner, which
    // sudo refuses. This boot's copies, on its /run, bound over them.
    root_owned_sudo_();

    // Its name servers: the kernel's lease (ip=dhcp, /proc/net/pnp), in this
    // boot's /run and bound over /etc/resolv.conf -- the host's NAT answers
    // them -- never written into the system's tree (design 2026-10-19 §27.2).
    // Without a lease (no network) nothing is bound and the system's own
    // stays.
    resolv_from_lease_();
    // The prompt's mark for every login shell, for a system made before a
    // shell's script existed (written when missing; design 2026-10-19 F26).
    (void)xlings::subos::root_etc::ensure_prompt_script("/etc");

    // Its name: the machine's own /etc/hostname, else the environment's.
    {
        std::ifstream in("/etc/hostname");
        std::string name;
        std::getline(in, name);
        // The edition's placeholder is no name: the system's own is (part 3 §6.2).
        // It is written into /etc/hostname too: the environment's own init
        // sets the name again from that file, and a placeholder left there
        // turned `pcore` into `luban` a moment after the first prompt.
        if ((name.empty() || name == "luban") && params.contains("luban.name")) {
            name = params.at("luban.name");
            // The placeholder is a link into the read-only factory /etc: a
            // write through it fails, and the system's own init later named
            // the machine `luban` from it (design 2026-10-19 F27). Its own
            // file instead, with its name.
            std::error_code ec;
            if (fs::is_symlink(fs::symlink_status("/etc/hostname", ec))) fs::remove("/etc/hostname", ec);
            std::ofstream out("/etc/hostname", std::ios::trunc);
            out << name << '\n';
            out.close();
            if (!out) console_("cannot write /etc/hostname: its name is the kernel's only");
        }
        if (!name.empty()) (void)platform::set_hostname(name);
    }
    // The host's proxy for this boot (design 2026-10-19 §27.3): what the host
    // left under the home, in this boot's /run for login shells, and in the
    // environment of its init (its services, its desktop session).
    std::map<std::string, std::string> network_env;
    if (!home.empty() && params.contains("luban.name")) {
        std::ifstream in(home / "run" / "machines" / params.at("luban.name") / "network.env");
        std::string text, line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            network_env[line.substr(0, eq)] = line.substr(eq + 1);
            text += line + "\n";
        }
        if (!text.empty()) {
            std::error_code mk;
            fs::create_directories("/run/luban", mk);
            std::ofstream("/run/luban/network.env", std::ios::trunc) << text;
        }
    }
    if (platform::fork_process() == 0) agent_();
    std::error_code ec;
    for (const auto* init : {"/sbin/init", "/usr/sbin/init", "/usr/bin/init", "/bin/init"}) {
        if (fs::symlink_status(init, ec).type() == fs::file_type::not_found) continue;
        // Every process of the machine -- its services, its desktop session --
        // knows which system it is (L §9), not only the agent's sessions.
        std::map<std::string, std::string> initEnv{{"PATH", "/usr/sbin:/usr/bin:/sbin:/bin"}, {"TERM", "linux"}};
        if (params.contains("luban.name")) initEnv["XLINGS_ROOT_INSTANCE"] = params.at("luban.name");
        initEnv.insert(network_env.begin(), network_env.end());
        platform::exec_path({init}, initEnv);
    }
    console_("the environment has no init; a shell on the console instead");
    platform::exec_path({"/bin/sh"}, {{"PATH", "/usr/sbin:/usr/bin:/sbin:/bin"}});
    give_up_("nothing to run");
}

}  // namespace luban::vm
