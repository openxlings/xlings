module luban.vm;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.subos.root_etc;
import luban.vm.protocol;
import luban.desktop.wlr;

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
};

void close_session_(Session& s) {
    std::vector<int> fds{s.read};
    if (s.write != s.read) fds.push_back(s.write);
    platform::close_fds(fds);
}

std::map<std::string, std::string> session_env_(const Json& open, const std::string& shell) {
    std::map<std::string, std::string> env{
        {"HOME", "/root"}, {"USER", "root"}, {"LOGNAME", "root"}, {"SHELL", shell},
        {"PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"},
        {"TERM", open.value("term", std::string("xterm-256color"))}, {"LANG", "C.UTF-8"}};
    if (auto e = open.find("env"); e != open.end() && e->is_object())
        for (auto it = e->begin(); it != e->end(); ++it)
            if (it.value().is_string()) env[it.key()] = it.value().get<std::string>();
    return env;
}

// A desktop for the Luban OS window: the session `display.session` names
// (or the first one declared), headless, bridged on two pipes (opensal-wlr).
std::expected<Session, std::string> desktop_(const Json& display, std::map<std::string, std::string> env,
                                             std::vector<int> inherited) {
    namespace wlr = luban::desktop::wlr;
    const auto dirs = wlr::session_dirs(env);
    const auto id = display.value("session", std::string{});
    std::optional<wlr::Session> session;
    if (!id.empty()) session = wlr::find_session(id, dirs);
    else if (auto all = wlr::sessions(dirs); !all.empty()) session = all.front();
    if (!session)
        return std::unexpected(id.empty() ? std::string("this environment declares no desktop session (share/wayland-sessions)")
                                          : std::format("this environment has no desktop session {}", id));
    auto to = platform::make_pipe(), from = platform::make_pipe();
    if (!to || !from) return std::unexpected("no pipe for the desktop");
    wlr::SessionOptions o;
    o.size = {display.value("width", 1280u), display.value("height", 800u)};
    o.env = std::move(env);
    std::error_code ec;
    fs::create_directories("/var/log/luban", ec);
    o.log = "/var/log/luban/desktop.log";
    const int pid = platform::fork_process();
    if (pid == 0) {
        // Nothing of the agent's stays open in the bridge (a shell's pty, the port).
        inherited.insert(inherited.end(), {(*to)[1], (*from)[0]});
        platform::close_fds(inherited);
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

[[noreturn]] void agent_() {
    platform::new_session();
    std::optional<fs::path> dev;
    for (int i = 0; i < 300 && !dev; ++i) {
        dev = port_();
        if (!dev) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!dev) { console_("no luban.agent port: this machine has no sessions"); platform::exit_now(1); }
    const int port = platform::open_device(*dev);
    if (port < 0) { console_("cannot open " + dev->string()); platform::exit_now(1); }
    (void)platform::write_all(port, proto::encode_json('H', 0, Json{{"agent", 1}}));
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
                    (void)platform::write_all(port, proto::encode_json('H', 0, Json{{"agent", 1}}));
                } else if (frame.type == 'O') {
                    auto open = Json::parse(frame.payload, nullptr, false);
                    if (!open.is_object()) open = Json::object();
                    if (auto display = open.find("display"); display != open.end() && display->is_object()) {
                        std::vector<int> inherited{port};
                        for (const auto& [ch, other] : sessions) {
                            inherited.push_back(other.read);
                            if (other.write != other.read) inherited.push_back(other.write);
                        }
                        // Its terminals open the root's login shell, as a session's does.
                        auto s = desktop_(*display, session_env_(open, xlings::subos::root_etc::login_shell("/")), std::move(inherited));
                        if (!s) {
                            (void)platform::write_all(port, proto::encode_json('X', frame.channel, Json{{"code", 127}, {"error", s.error()}}));
                            continue;
                        }
                        sessions[frame.channel] = *s;
                        continue;
                    }
                    auto pty = platform::open_pty();
                    if (!pty) {
                        console_(std::format("no pty for a session: {}", platform::error_text(platform::last_error())));
                        (void)platform::write_all(port, proto::encode_json('X', frame.channel, Json{{"code", 125}}));
                        continue;
                    }
                    platform::set_terminal_size((*pty)[0], open.value("rows", 24), open.value("cols", 80));
                    const auto shell = xlings::subos::root_etc::login_shell("/");
                    const int pid = platform::fork_process();
                    if (pid == 0) {
                        platform::new_session();
                        const std::array<int, 3> stdio{(*pty)[1], (*pty)[1], (*pty)[1]};
                        platform::redirect_stdio(stdio);
                        platform::take_controlling_terminal();
                        std::error_code ec;
                        fs::current_path("/root", ec);
                        std::vector<std::string> argv{shell, "-l"};
                        if (auto a = open.find("argv"); a != open.end() && a->is_array() && !a->empty()) {
                            argv.clear();
                            for (const auto& w : *a) argv.push_back(w.get<std::string>());
                        }
                        platform::exec_program(argv, session_env_(open, shell));
                        platform::exit_now(127);
                    }
                    std::vector<int> slave{(*pty)[1]};
                    platform::close_fds(slave);
                    sessions[frame.channel] = {(*pty)[0], (*pty)[0], pid};
                } else if (frame.type == 'D') {
                    if (auto s = sessions.find(frame.channel); s != sessions.end()) (void)platform::write_all(s->second.write, frame.payload);
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
                    // busybox init powers off on SIGUSR2; anything else in 10 s.
                    platform::send_signal(1, platform::sig::user2);
                    if (!powering_off) powering_off = std::chrono::steady_clock::now();
                }
            }
        }
        for (std::size_t k = 0; k < order.size(); ++k) {
            const auto& pf = p[1 + k];
            if (!pf.readable && !pf.closed) continue;
            auto s = sessions.find(order[k]);
            if (s == sessions.end()) continue;
            auto n = platform::read_some(s->second.read, buf);
            if (n && *n > 0) {
                (void)platform::write_all(port, proto::encode('D', order[k], std::string_view(buf.data(), *n)));
                continue;
            }
            int code = 0;
            if (auto st = platform::wait_process(s->second.pid, true)) code = platform::exit_code(*st);
            (void)platform::write_all(port, proto::encode_json('X', order[k], Json{{"code", code}}));
            close_session_(s->second);
            sessions.erase(s);
        }
    }
}

}  // namespace

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
    const auto home = params.contains("luban.home") ? fs::path(params.at("luban.home")) : fs::path{};
    if (home.empty() || !home.is_absolute()) give_up_("no luban.home on the kernel's command line");
    if (!platform::mount_9p("root", "/newroot", false)) give_up_("the environment's tree (9p tag root) did not mount");
    // A machine's /run and /tmp are its own and go with it, as on any system
    // (the tree would keep them: a desktop's sockets, owned by the host's user
    // there, refuse the next session). Before the home, which may be under one.
    if (!platform::mount_tmpfs("/newroot/run", 0755)) console_("cannot mount /run");
    if (!platform::mount_tmpfs("/newroot/tmp", 01777)) console_("cannot mount /tmp");
    if (!platform::mount_9p("home", fs::path("/newroot") / home.relative_path(), true))
        give_up_("the home (9p tag home) did not mount");
    if (params.contains("luban.userhome") && params.at("luban.userhome") == "1"
        && !platform::mount_9p("userhome", "/newroot/home", false))
        give_up_("its /home (9p tag userhome) did not mount");
    for (const auto* at : {"/dev", "/proc", "/sys"})
        if (!platform::move_mount(at, fs::path("/newroot") / fs::path(at).relative_path()))
            give_up_(std::format("cannot move {} into the environment", at));
    if (!platform::switch_root("/newroot")) give_up_("cannot make the environment /");
    // The agent's sessions are ptys before the environment's own start mounts
    // anything (its rcS mounting devpts again is harmless: already mounted).
    if (!platform::mount_kernel_fs("devpts", "/dev/pts")) console_("cannot mount devpts: sessions will fail");

    // Its name: the machine's own /etc/hostname, else the environment's.
    {
        std::ifstream in("/etc/hostname");
        std::string name;
        std::getline(in, name);
        if (name.empty() && params.contains("luban.name")) name = params.at("luban.name");
        if (!name.empty()) (void)platform::set_hostname(name);
    }
    if (platform::fork_process() == 0) agent_();
    std::error_code ec;
    for (const auto* init : {"/sbin/init", "/usr/sbin/init", "/usr/bin/init", "/bin/init"}) {
        if (fs::symlink_status(init, ec).type() == fs::file_type::not_found) continue;
        platform::exec_path({init}, {{"PATH", "/usr/sbin:/usr/bin:/sbin:/bin"}, {"TERM", "linux"}});
    }
    console_("the environment has no init; a shell on the console instead");
    platform::exec_path({"/bin/sh"}, {{"PATH", "/usr/sbin:/usr/bin:/sbin:/bin"}});
    give_up_("nothing to run");
}

}  // namespace luban::vm
