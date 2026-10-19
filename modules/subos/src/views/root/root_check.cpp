module xlings.subos.root_check;

import std;
import xlings.libs.json;
import xlings.libs.elfread;
import xlings.platform;
import xlings.subos.rootfs;
import xlings.subos.root_etc;

namespace xlings::subos::root_check {

namespace {

bool exists_(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool nonempty_dir_(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec) && !fs::is_empty(p, ec);
}

bool executable_(const fs::path& p) {
    std::error_code ec;
    const auto s = fs::status(p, ec);
    return !ec && fs::is_regular_file(s)
        && (s.permissions() & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
}

std::string first_line_(const fs::path& p) {
    std::ifstream in(p);
    std::string line;
    std::getline(in, line);
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    return line;
}

Item item_(std::string id, std::string what, bool critical, bool ok, std::string detail = {}) {
    return Item{std::move(id), std::move(what), critical, ok, std::move(detail)};
}

// ── programs ─────────────────────────────────────────────────────────

struct Ran {
    int code { -1 };
    bool timed_out { false };
    std::string output;
};

// `argv` for nobody: its own session, the null device as input, stdout and
// stderr into one file read back; ended whole when late.
Ran run_(const std::vector<std::string>& argv, std::chrono::seconds limit) {
    static std::atomic<unsigned> counter { 0 };
    const auto log = fs::temp_directory_path()
        / std::format("luban-check-{}-{}", platform::user_ids().uid,
                      std::chrono::steady_clock::now().time_since_epoch().count() + counter++);
    Ran ran;
    const int out = platform::open_for_append(log);
    const int in = platform::open_null();
    if (out < 0 || in < 0) {
        std::vector<int> fds{out, in};
        platform::close_fds(fds);
        ran.output = "cannot capture its output";
        return ran;
    }
    const int pid = platform::fork_process();
    if (pid == 0) {
        platform::new_session();
        platform::reset_signals();
        const std::array<int, 3> stdio{in, out, out};
        platform::redirect_stdio(stdio);
        platform::exec_program(argv, platform::environment());
        platform::exit_now(127);
    }
    {
        std::vector<int> fds{out, in};
        platform::close_fds(fds);
    }
    if (pid < 0) {
        ran.output = "cannot start it";
        return ran;
    }
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::optional<platform::ExitStatus> status;
    while (!(status = platform::wait_process(pid, false))) {
        if (std::chrono::steady_clock::now() >= deadline) {
            platform::send_signal_group(pid, platform::sig::kill);
            status = platform::wait_process(pid, true);
            ran.timed_out = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (status) ran.code = platform::exit_code(*status);
    {
        std::ifstream f(log, std::ios::binary);
        ran.output.assign(std::istreambuf_iterator<char>(f), {});
    }
    std::error_code ec;
    fs::remove(log, ec);
    return ran;
}

bool loader_failed_(const Ran& r) { return !started(r.code, r.timed_out, r.output); }

std::string tail_(std::string_view text, std::size_t keep = 200) {
    std::string s(text.size() > keep ? text.substr(text.size() - keep) : text);
    std::ranges::replace(s, '\n', ' ');
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// ── the runtime closure ──────────────────────────────────────────────

// Where the loader looks when an ELF says nothing: ld.so.conf (and what it
// includes), musl's path file, LD_LIBRARY_PATH, and the usual directories.
std::vector<fs::path> default_library_dirs_() {
    std::vector<fs::path> dirs;
    std::function<void(const fs::path&, int)> conf = [&](const fs::path& file, int depth) {
        if (depth > 4) return;
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) {
            if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
            std::size_t start = 0;
            while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) ++start;
            line.erase(0, start);
            if (line.empty()) continue;
            if (line.starts_with("include")) {
                fs::path pattern = line.substr(7);
                auto text = pattern.string();
                text.erase(0, text.find_first_not_of(" \t"));
                pattern = text;
                if (pattern.is_relative()) pattern = file.parent_path() / pattern;
                std::error_code ec;
                const auto dir = pattern.parent_path();
                const auto glob = pattern.filename().string();
                const auto suffix = glob.starts_with("*") ? glob.substr(1) : std::string{};
                std::vector<fs::path> files;
                for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                    const auto name = it->path().filename().string();
                    if (glob.starts_with("*") ? name.ends_with(suffix) : name == glob) files.push_back(it->path());
                }
                std::ranges::sort(files);
                for (const auto& f : files) conf(f, depth + 1);
                continue;
            }
            dirs.emplace_back(line);
        }
    };
    conf("/etc/ld.so.conf", 0);
    {
        std::error_code ec;
        for (fs::directory_iterator it("/etc", ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().string();
            if (!name.starts_with("ld-musl-") || !name.ends_with(".path")) continue;
            std::ifstream in(it->path());
            for (std::string word; in >> word;)
                for (std::size_t at = 0; at <= word.size();) {
                    const auto colon = word.find(':', at);
                    if (const auto part = word.substr(at, colon == std::string::npos ? std::string::npos : colon - at);
                        !part.empty())
                        dirs.emplace_back(part);
                    if (colon == std::string::npos) break;
                    at = colon + 1;
                }
        }
    }
    if (const char* env = std::getenv("LD_LIBRARY_PATH"); env && *env) {
        std::string_view rest = env;
        for (;;) {
            const auto colon = rest.find(':');
            if (const auto part = rest.substr(0, colon); !part.empty()) dirs.emplace_back(std::string(part));
            if (colon == std::string_view::npos) break;
            rest.remove_prefix(colon + 1);
        }
    }
    for (const auto* d : {"/lib", "/usr/lib", "/lib64", "/usr/lib64", "/usr/local/lib"}) dirs.emplace_back(d);
    std::error_code ec;
    for (fs::directory_iterator it("/usr/lib", ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename().string().ends_with("-linux-gnu")) dirs.push_back(it->path());
    return dirs;
}

std::pair<Item, Item> closure_(const fs::path& usrBin) {
    const auto defaults = default_library_dirs_();
    std::set<fs::path> seen;
    std::vector<std::string> noLoader;
    std::vector<std::string> missing;
    std::size_t elfs = 0;
    std::error_code ec;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(usrBin, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(it->path());
    std::ranges::sort(entries);
    for (const auto& entry : entries) {
        std::error_code cec;
        const auto real = fs::canonical(entry, cec);
        if (cec || !seen.insert(real).second || !fs::is_regular_file(real, cec)) continue;
        if (!elfread::is_elf(real)) continue;
        const auto info = elfread::read(real);
        if (!info) continue;
        ++elfs;
        if (!info->interpreter.empty() && !exists_(info->interpreter)) {
            noLoader.push_back(std::format("{} (needs {})", entry.filename().string(), info->interpreter));
            continue;
        }
        for (const auto& lib : info->needed) {
            bool found = false;
            for (auto dir : info->searchPaths) {
                for (const auto token : {std::string_view("${ORIGIN}"), std::string_view("$ORIGIN")})
                    for (std::size_t at; (at = dir.find(token)) != std::string::npos;)
                        dir.replace(at, token.size(), real.parent_path().string());
                if (exists_(fs::path(dir) / lib)) { found = true; break; }
            }
            if (!found)
                for (const auto& dir : defaults)
                    if (exists_(dir / lib)) { found = true; break; }
            if (!found) missing.push_back(std::format("{} ({})", entry.filename().string(), lib));
        }
    }
    const auto list = [](const std::vector<std::string>& v) {
        std::string s;
        for (std::size_t i = 0; i < v.size() && i < 8; ++i) s += (i ? ", " : "") + v[i];
        if (v.size() > 8) s += std::format(", and {} more", v.size() - 8);
        return s;
    };
    return {item_("loader", std::format("every program in /usr/bin has its loader ({} ELF files)", elfs), true,
                  noLoader.empty(), list(noLoader)),
            item_("libraries", "every program in /usr/bin finds the libraries it names", false, missing.empty(),
                  list(missing))};
}

// ── declared and default program checks ─────────────────────────────

std::vector<Item> declared_(const fs::path& root, const Options& o, std::set<std::string>& declaredIds) {
    std::vector<Item> out;
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(root / kDeclaredDir, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".json") files.push_back(it->path());
    std::ranges::sort(files);
    for (const auto& file : files) {
        std::ifstream in(file);
        const auto doc = nlohmann::json::parse(in, nullptr, false);
        const auto checks = doc.is_object() ? doc.find("checks") : doc.end();
        if (!doc.is_object() || checks == doc.end() || !checks->is_array()) {
            out.push_back(item_("declared:" + file.stem().string(), file.filename().string() + " is readable", false,
                                false, "not {\"checks\": [...]}"));
            continue;
        }
        for (const auto& c : *checks) {
            if (!c.is_object() || !c.contains("run") || !c["run"].is_string()) continue;
            const auto id = c.value("id", file.stem().string());
            declaredIds.insert(id);
            const auto what = c.value("what", id);
            const bool critical = c.value("critical", false);
            if (!o.run) {
                out.push_back(item_(id, what, critical, true, "not run here"));
                continue;
            }
            const auto limit = std::chrono::seconds(c.value("timeout", 120));
            const auto r = run_({"/bin/sh", "-c", c["run"].get<std::string>()}, limit);
            const bool ok = !r.timed_out && r.code == 0;
            out.push_back(item_(id, what, critical, ok,
                                ok ? std::string{}
                                   : r.timed_out ? std::format("did not finish in {} s", limit.count())
                                                 : std::format("exit {}: {}", r.code, tail_(r.output))));
        }
    }
    return out;
}

// Each package's programs start: one of its programs in /usr/bin, asked for
// its version. Not whether it answers 0 -- some have no --version -- but
// whether it runs at all (its loader, its libraries).
std::vector<Item> starts_(const fs::path& usrBin, const Options& o, const std::set<std::string>& declaredIds) {
    struct Candidate { std::string package; fs::path program; };
    std::map<fs::path, Candidate> byPayload;
    std::error_code ec;
    std::vector<fs::path> entries;
    for (fs::directory_iterator it(usrBin, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(it->path());
    std::ranges::sort(entries);
    for (const auto& entry : entries) {
        std::error_code lec;
        if (!fs::is_symlink(entry, lec)) continue;
        const auto target = fs::read_symlink(entry, lec).generic_string();
        const auto at = target.find("/xpkgs/");
        if (lec || at == std::string::npos) continue;
        const auto rest = std::string_view(target).substr(at + 7);
        const auto slash = rest.find('/');
        const auto slash2 = slash == std::string_view::npos ? slash : rest.find('/', slash + 1);
        if (slash2 == std::string_view::npos) continue;
        const fs::path payload = target.substr(0, at + 7 + slash2);
        const auto dir = std::string(rest.substr(0, slash));
        const auto x = dir.find("-x-");
        const auto package = x == std::string::npos ? dir : dir.substr(x + 3);
        auto& c = byPayload[payload];
        const auto name = entry.filename().string();
        if (c.program.empty() || (name == package && c.program.filename() != package)) c = {package, entry};
    }
    std::vector<Item> out;
    for (const auto& [payload, c] : byPayload) {
        if (declaredIds.contains(c.package) || !executable_(c.program)) continue;
        const auto r = run_({c.program.string(), "--version"}, o.program_timeout);
        const bool ok = !loader_failed_(r);
        out.push_back(item_("starts:" + c.package, std::format("{} starts ({})", c.package, c.program.filename().string()),
                            false, ok, ok ? std::string{} : std::format("exit {}: {}", r.code, tail_(r.output))));
    }
    return out;
}

// ── facts of the root ────────────────────────────────────────────────

bool has_terminfo_(const fs::path& root, std::string_view term) {
    if (term.empty()) return true;
    const std::string t(term);
    char hex[3];
    std::snprintf(hex, sizeof hex, "%02x", static_cast<unsigned char>(t[0]));
    for (const auto* base : {"/usr/share/terminfo", "/usr/lib/terminfo", "/etc/terminfo", "/lib/terminfo"}) {
        const auto dir = rootfs::in_root(root, base);
        if (exists_(dir / std::string(1, t[0]) / t) || exists_(dir / hex / t)) return true;
    }
    return false;
}

std::optional<std::string> effective_capabilities_() {
    std::ifstream in("/proc/self/status");
    for (std::string line; std::getline(in, line);)
        if (line.starts_with("CapEff:")) {
            auto v = line.substr(7);
            v.erase(0, v.find_first_not_of(" \t"));
            return v;
        }
    return std::nullopt;
}

}  // namespace

bool Report::ok() const {
    return std::ranges::none_of(items, [](const Item& i) { return i.critical && !i.ok; });
}

std::size_t Report::failed() const {
    return static_cast<std::size_t>(std::ranges::count_if(items, [](const Item& i) { return !i.ok; }));
}

Report check(const fs::path& root, Depth depth, const Options& o) {
    Report r;
    const bool here = root == fs::path("/");
    const auto usrBin = rootfs::in_root(root, "/usr/bin");
    r.items.push_back(item_("usr", "/usr is the system's generation", true, nonempty_dir_(usrBin),
                            nonempty_dir_(usrBin) ? std::string{} : "/usr/bin is missing or empty"));
    {
        std::ifstream in(root / "etc" / "passwd");
        bool rootUser = false;
        for (std::string line; std::getline(in, line);)
            if (line.starts_with("root:")) rootUser = true;
        r.items.push_back(item_("passwd", "/etc/passwd names its users", true, rootUser,
                                rootUser ? std::string{} : "no root entry in /etc/passwd"));
    }
    {
        const auto shell = root_etc::login_shell(root);
        const bool there = executable_(rootfs::in_root(root, shell));
        r.items.push_back(item_("shell", std::format("its login shell {} runs", shell), false, there,
                                there ? std::string{} : shell + " is not there (an edition without a shell has none)"));
    }
    {
        const auto name = first_line_(root / "etc" / "hostname");
        r.items.push_back(item_("hostname", "it has a host name", false, !name.empty(),
                                name.empty() ? "/etc/hostname is empty" : name));
    }
    if (!o.term.empty())
        r.items.push_back(item_("terminfo", std::format("the terminal {} is described", o.term), false,
                                has_terminfo_(root, o.term),
                                has_terminfo_(root, o.term) ? std::string{} : "no terminfo entry: TERM=xterm-256color works"));
    if (here && platform::user_ids().euid != 0) {
        const auto caps = effective_capabilities_();
        const bool none = !caps || caps->find_first_not_of('0') == std::string::npos;
        r.items.push_back(item_("capabilities", "its user's processes hold no capabilities", true, none,
                                none ? std::string{} : "CapEff " + caps.value_or("?")));
    }
    if (depth == Depth::Fast) return r;

    r.items.push_back(item_("machine-id", "it has a machine id", false, !first_line_(root / "etc/machine-id").empty()));
    r.items.push_back(item_("timezone", "it has a time zone", false, exists_(root / "etc/localtime")));
    {
        bool certs = false;
        for (const auto* f : {"etc/ssl/certs/ca-certificates.crt", "etc/ssl/cert.pem", "etc/pki/tls/certs/ca-bundle.crt"})
            certs = certs || exists_(root / f);
        r.items.push_back(item_("certificates", "it trusts the usual certificate authorities", false, certs,
                                certs ? std::string{} : "no CA bundle in /etc/ssl"));
    }
    {
        const auto usrLib = rootfs::in_root(root, "/usr/lib");
        bool glibc = exists_(usrLib / "libc.so.6") || exists_(rootfs::in_root(root, "/usr/lib64") / "libc.so.6");
        std::error_code ec;
        for (fs::directory_iterator it(usrLib, ec), end; !glibc && !ec && it != end; it.increment(ec))
            glibc = it->path().filename().string().ends_with("-linux-gnu") && exists_(it->path() / "libc.so.6");
        const bool locale = !glibc || exists_(usrLib / "locale/C.utf8") || exists_(usrLib / "locale/C.UTF-8")
                         || exists_(usrLib / "locale/locale-archive");
        r.items.push_back(item_("locale", "a UTF-8 locale is there", false, locale,
                                locale ? std::string{} : "no C.UTF-8 under /usr/lib/locale"));
    }
    const auto share = rootfs::in_root(root, "/usr/share");
    if (nonempty_dir_(share / "wayland-sessions") || nonempty_dir_(share / "xsessions")) {
        r.items.push_back(item_("applications", "its desktop lists applications", false,
                                nonempty_dir_(share / "applications"),
                                nonempty_dir_(share / "applications") ? std::string{} : "/usr/share/applications is empty"));
        bool cursors = exists_(share / "icons/default/index.theme");
        std::error_code ec;
        for (fs::directory_iterator it(share / "icons", ec), end; !cursors && !ec && it != end; it.increment(ec))
            cursors = nonempty_dir_(it->path() / "cursors");
        r.items.push_back(item_("cursors", "its desktop has a cursor theme", false, cursors,
                                cursors ? std::string{} : "no theme with cursors under /usr/share/icons"));
    }
    if (!here) return r;
    auto [loader, libraries] = closure_(usrBin);
    r.items.push_back(std::move(loader));
    r.items.push_back(std::move(libraries));
    // Its network (design 2026-10-19 §27.4): a default route and a name
    // server. Not critical -- a system works offline -- but said, so "the
    // index could not be fetched" inside a machine has an answer.
    {
        bool route = false, nameserver = false;
        std::ifstream routes("/proc/net/route");
        std::string line;
        std::getline(routes, line);   // the header
        while (!route && std::getline(routes, line)) {
            std::istringstream fields(line);
            std::string iface, destination;
            fields >> iface >> destination;
            route = destination == "00000000";
        }
        std::ifstream resolv("/etc/resolv.conf");
        while (!nameserver && std::getline(resolv, line)) nameserver = line.starts_with("nameserver");
        const bool ok = route && nameserver;
        r.items.push_back(item_("network", "it has a network: a default route and a name server", false, ok,
                                ok ? std::string{} : !route ? "no default route" : "no name server in /etc/resolv.conf"));
    }
    std::set<std::string> declaredIds;
    for (auto& i : declared_(root, o, declaredIds)) r.items.push_back(std::move(i));
    if (o.run)
        for (auto& i : starts_(usrBin, o, declaredIds)) r.items.push_back(std::move(i));
    return r;
}

nlohmann::json to_json(const Report& report) {
    auto items = nlohmann::json::array();
    for (const auto& i : report.items) {
        nlohmann::json j{{"id", i.id}, {"what", i.what}, {"critical", i.critical}, {"ok", i.ok}};
        if (!i.detail.empty()) j["detail"] = i.detail;
        items.push_back(std::move(j));
    }
    return {{"ok", report.ok()}, {"failed", report.failed()}, {"items", std::move(items)}};
}


// What a loader says when a program cannot start: glibc, musl, the kernel.
// BusyBox called by the name of an applet it was not built with answers
// "applet not found" and 127: it ran, which is all this asks (design
// 2026-10-19 F10).
bool started(int code, bool timed_out, std::string_view output) {
    if (timed_out) return true;
    if (output.find("applet not found") != std::string_view::npos) return true;
    if (code == 126 || code == 127 || code > 128) return false;
    for (const auto* said : {"error while loading shared libraries", "Error loading shared library",
                             "Error relocating", "cannot execute binary file", "Exec format error"})
        if (output.find(said) != std::string_view::npos) return false;
    return true;
}

}  // namespace xlings::subos::root_check
