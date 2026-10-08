// `subos exec / start / cp`: running things in an instance from outside it,
// and the isolation flags every entry shares (module xlings.core.subos).
module xlings.core.subos;

import std;
import xlings.core.config;
import xlings.core.home_config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.runtime;
import xlings.core.utils;
import xlings.core.xself;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xim.commands;
import xlings.subos.gpu;
import xlings.subos.graphics;
import xlings.core.subos.sandbox;
import xlings.subos.manifest;
import xlings.cli.spec;
import xlings.i18n;
import xlings.core.confirm;
import xlings.core.destructive_log;
import xlings.subos.userdata;
import xlings.subos.model;
import xlings.core.subos.ports;
import xlings.subos.session;
import xlings.subos.policy;
import xlings.subos.policy_store;
import xlings.subos.broker;
import xlings.observe;
import xlings.core.home;
import xlings.libs.sha256;
import xlings.core.subos.root;
import xlings.subos.rootfs;
import xlings.subos.roles;
import xlings.core.version_order;

namespace xlings::subos {

std::optional<std::pair<std::string, std::string>> split_env_(std::string_view kv) {
    auto eq = kv.find('=');
    if (eq == std::string_view::npos || eq == 0) return std::nullopt;
    return std::pair{std::string(kv.substr(0, eq)), std::string(kv.substr(eq + 1))};
}

int parse_isolation_flag_(std::string_view a, int& i, int argc, char* argv[],
                          IsolationArgs& x, std::string& err) {
    auto value = [&](std::string_view flag) -> std::optional<std::string> {
        if (a.starts_with(std::string(flag) + "=")) return std::string(a.substr(flag.size() + 1));
        if (a == flag && i + 1 < argc) return std::string(argv[++i]);
        return std::nullopt;
    };
    if (a == "--sandbox") {
        x.sandbox = true;
        if (i + 1 < argc) {
            std::string_view next = argv[i + 1];
            if (next == "bwrap" || next == "proot" || next == "landlock") { x.backend = next; ++i; }
        }
        return 1;
    }
    if (a.starts_with("--sandbox=")) {
        x.sandbox = true;
        auto v = a.substr(10);
        if (v == "bwrap" || v == "proot" || v == "landlock") { x.backend = v; return 1; }
        auto p = policy::preset_from_string(v);
        if (!p || *p == policy::Preset::Legacy) {
            err = std::format("--sandbox={}: expected dev, private or locked (or a backend: bwrap, proot, landlock)", v);
            return -1;
        }
        x.preset = *p;
        return 1;
    }
    if (a == "--net" || a.starts_with("--net=")) {
        auto v = value("--net");
        auto n = v ? policy::net_from_string(*v) : std::nullopt;
        if (!n) { err = "--net expects host, nat, none or proxy"; return -1; }
        x.overrides.net = *n;
        x.sandbox = true;
        return 1;
    }
    if (a == "--fetch" || a.starts_with("--fetch=")) {
        auto v = value("--fetch");
        auto f = v ? policy::fetch_from_string(*v) : std::nullopt;
        if (!f) { err = "--fetch expects auto, ask or deny"; return -1; }
        x.overrides.fetch = *f;
        return 1;
    }
    if (a == "--allow" || a.starts_with("--allow=")) {
        auto v = value("--allow");
        if (!v) { err = "--allow expects a grant"; return -1; }
        std::string_view list(*v);
        while (!list.empty()) {
            auto comma = list.find(',');
            x.overrides.allow.insert(std::string(list.substr(0, comma)));
            if (comma == std::string_view::npos) break;
            list.remove_prefix(comma + 1);
        }
        x.sandbox = true;
        return 1;
    }
    if (a == "--mount" || a.starts_with("--mount=")) {
        auto v = value("--mount");
        if (!v) { err = "--mount expects <host>[:<inside>][:ro|rw]"; return -1; }
        std::error_code ec;
        auto m = policy::parse_mount(*v, utils::get_env_or_default("HOME"),
                                     fs::current_path(ec).generic_string());
        if (!m) { err = m.error(); return -1; }
        x.overrides.mounts.push_back(std::move(*m));
        x.sandbox = true;
        return 1;
    }
    if (a == "--publish" || a.starts_with("--publish=")) {
        auto v = value("--publish");
        if (!v || v->find(':') == std::string::npos) { err = "--publish expects HOST:SANDBOX ports, e.g. 8080:80"; return -1; }
        x.publish.push_back(*v);
        x.sandbox = true;
        return 1;
    }
    if (a == "--no-degrade") {
        x.overrides.no_degrade = true;
        x.sandbox = true;
        return 1;
    }
    return 0;
}



// `subos exec <name> [options] -- <argv...>` (design §12): one command in an
// instance from outside it, as argv (nothing to shell-escape). Joins the
// running session when there is one; otherwise starts one for the command
// (with --sandbox) or runs it with the instance's environment. Exit codes are
// the command's own, 125 when it never started, 126/127, 124, 128+n.
int run_exec_(int argc, char* argv[], EventStream& stream) {
    std::string name, cwd, from;
    std::map<std::string, std::string> env;
    std::optional<std::chrono::milliseconds> timeout;
    bool json = false, temp = false;
    IsolationArgs iso;
    std::vector<std::string> command;
    auto fail = [&](std::string message, std::string hint = {}) {
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput, .message = std::move(message),
                                .recoverable = false, .hint = std::move(hint) });
        if (json) std::println(std::cerr, "{}", nlohmann::json{{"exit", session::kExitSetup},
                                                            {"phase", "setup"}}.dump());
        return session::kExitSetup;
    };
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") { command.assign(argv + i + 1, argv + argc); break; }
        std::string err;
        if (auto r = parse_isolation_flag_(a, i, argc, argv, iso, err); r == 1) continue;
        else if (r < 0) return fail(err);
        if (a == "--json") json = true;
        else if (a == "--temp") temp = true;
        else if (a == "--from" && i + 1 < argc) from = argv[++i];
        else if (a == "--cwd" && i + 1 < argc) cwd = argv[++i];
        else if (a == "--env" && i + 1 < argc) {
            auto kv = split_env_(argv[++i]);
            if (!kv) return fail("--env expects K=V");
            env[kv->first] = kv->second;
        }
        else if (a == "--timeout" && i + 1 < argc) {
            auto d = model::parse_duration(argv[++i]);
            if (!d) return fail("--timeout expects seconds or 30s / 10m / 2h");
            timeout = std::chrono::seconds(*d);
        }
        else if (!a.empty() && a[0] != '-' && name.empty() && !temp) name = std::move(a);
        else return fail("unknown option for `xlings subos exec`: " + a,
                         "usage: xlings subos exec <name> [--sandbox] [--cwd D] [--env K=V] "
                         "[--timeout T] [--json] -- <command...>");
    }
    if (command.empty()) return fail("missing the command: xlings subos exec <name> -- <command...>");
    if (!from.empty() && !temp) return fail("--from needs --temp");

    // --temp: a throwaway instance, removed afterwards; its audit stays.
    std::string tempName;
    if (temp) {
        if (!name.empty()) return fail("--temp takes no name");
        std::random_device rd;
        tempName = std::format("tmp-{:06x}", rd() & 0xffffff);
        name = tempName;
        EventStream quiet;
        int rc = from.empty()
            ? create(name, {}, sandbox::StorageMode::Shared, "50G", "", /*yes=*/true, "--temp", quiet)
            : new_from(name, {}, sandbox::StorageMode::Shared, "50G", from, "", /*yes=*/true, quiet);
        if (rc != 0) return fail("could not create a temporary instance" +
                                 (from.empty() ? std::string{} : " from " + from));
    } else if (name.empty()) {
        return fail("missing <name> for: xlings subos exec (or --temp)");
    } else {
        auto resolved = resolve_candidate_(name);
        if (resolved.selected.empty() || resolved.autoSelected) {
            emit_candidates_(stream, resolved, name);
            return fail("no SubOS named '" + name + "'", "xlings subos list");
        }
    }

    const auto started = std::chrono::steady_clock::now();
    int rc = 0;
    std::string mode;
    if (iso.sandbox || iso.preset || session::find(home_view(), name)
        || enters_sandboxed_(name)) {
        mode = "sandbox";
        use_detail_::apply_subos_env_(name);
        auto declared = declared_env_(name);
        declared.insert(env.begin(), env.end());
        for (auto& [k, v] : env) declared[k] = v;
        EventStream quiet;
        EventStream& out = json ? static_cast<EventStream&>(quiet) : stream;
        rc = sandbox::enter(name, out, sandbox::EnterOptions{
            .backend = iso.backend, .argv = command, .cwd = cwd, .env = std::move(declared),
            .timeout = timeout, .exec_codes = true, .announce = false,
            .preset = iso.preset, .overrides = iso.overrides, .publish = iso.publish });
    } else {
        // An instance without a sandbox: its environment, this process's
        // stdio, no supervisor (design §16, "没有沙箱的实例").
        mode = "shell";
        if (!cwd.empty()) {
            std::error_code ec;
            fs::current_path(cwd, ec);
            if (ec) return fail("--cwd: " + cwd + ": " + ec.message());
        }
        auto& p = Config::paths();
        auto bin_dir = p.homeDir / "subos" / name / "bin";
        platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
        platform::set_env_variable("XLINGS_BIN", bin_dir.string());
        platform::set_env_variable("XLINGS_SUBOS_LIB", (p.homeDir / "subos" / name / "lib").string());
        platform::set_env_variable("PATH", use_detail_::rebuild_path_for_subos_(
            utils::get_env_or_default("PATH"), p.homeDir, bin_dir));
        use_detail_::apply_subos_env_(name);
        for (auto& [k, v] : env) platform::set_env_variable(k, v);
        observe::append(home_view().logs_dir(name) / "events.ndjson", observe::Event{
            .kind = observe::Kind::Ops,
            .fields = {{"event", "exec"}, {"instance", name}, {"mode", "shell"},
                       {"program", command[0]}, {"argc", command.size()}}});
        if (timeout && platform::is_windows)
            log::warn("--timeout needs --sandbox on Windows; running without it");
        rc = timeout && platform::is_posix ? platform::run_argv_with_timeout(command, *timeout)
                                           : platform::run_argv(command);
        observe::append(home_view().logs_dir(name) / "events.ndjson", observe::Event{
            .kind = observe::Kind::Ops,
            .fields = {{"event", "exec-end"}, {"instance", name}, {"mode", "shell"},
                       {"program", command[0]}, {"exit", rc}}});
    }

    if (!tempName.empty()) {
        // The instance this command created, removed by the same command: the
        // user asked for a throwaway with --temp, which is the confirmation.
        // `remove` with the --temp spelling as its yes: the one deletion
        // entry point, the registry entry, and a destructive-log line that
        // says how it was confirmed.
        EventStream quiet;
        if (remove(name, /*yes=*/true, "--temp", quiet) != 0)
            log::warn("could not remove the temporary instance {}", name);
    }
    if (json) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started).count();
        nlohmann::json result{{"instance", name}, {"exit", rc}, {"mode", mode}, {"ms", ms},
                              {"temp", temp}};
        if (rc == session::kExitSetup) result["phase"] = "setup";
        if (rc == session::kExitTimeout && timeout) result["timeout"] = true;
        std::println(std::cerr, "{}", result.dump());
    }
    return rc;
}

// `subos start <name> [--sandbox[=backend]] [--ttl 30m]`: a session that runs
// without a terminal, for a series of `subos exec` (design §12.1).
int run_start_(int argc, char* argv[], EventStream& stream,
               const std::function<void(std::string_view)>& usageError) {
    std::string name;
    IsolationArgs iso;     // a session is a sandbox, whatever was said
    int ttl = 0;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        std::string err;
        if (auto r = parse_isolation_flag_(a, i, argc, argv, iso, err); r == 1) continue;
        else if (r < 0) { usageError(err); return 1; }
        if (a == "--ttl" && i + 1 < argc) {
            auto d = model::parse_duration(argv[++i]);
            if (!d) { usageError("--ttl expects seconds or 30s / 10m / 2h"); return 1; }
            ttl = static_cast<int>(*d);
        }
        else if (!a.empty() && a[0] != '-' && name.empty()) name = std::move(a);
        else { usageError("unknown option for `xlings subos start`: " + a); return 1; }
    }
    if (name.empty()) { usageError("missing <name> for: xlings subos start"); return 1; }
    auto resolved = resolve_use_name_(name, stream);
    if (resolved.selected.empty()) return resolved.exitCode;
    name = resolved.selected;
    if (auto live = session::find(home_view(), name)) {
        log::info("'{}' is already running (session {})", name, live->id);
        return 0;
    }
    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;
    use_detail_::apply_subos_env_(name);
    auto rc = sandbox::enter(name, stream, sandbox::EnterOptions{
        .backend = iso.backend, .env = declared_env_(name), .ttl = ttl, .detached = true,
        .preset = iso.preset, .overrides = iso.overrides, .publish = iso.publish });
    if (rc != 0) return rc;
    if (auto live = session::find(home_view(), name)) {
        log::info("started session {} for '{}'{}", live->id, name,
                  ttl > 0 ? std::format(" (ends after {}s idle)", ttl) : std::string(" (until `subos stop`)"));
        return 0;
    }
    return 1;
}

// A keeper process an xlings before 2026.10 left behind (sessions replaced
// it): `subos stop` still ends it. COMPAT: drop in 2027.4.
void stop_legacy_keeper_(const std::string& name) {
    const auto pid_file = Config::paths().homeDir / "subos" / name / ".keeper.pid";
    std::error_code ec;
    if (!fs::exists(pid_file, ec)) return;
    int pid = 0;
    std::ifstream(pid_file) >> pid;
    if (pid > 0 && platform::is_process_alive(pid)) {
        platform::send_signal(pid, platform::sig::terminate);
        for (int i = 0; i < 20 && platform::is_process_alive(pid); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (platform::is_process_alive(pid)) platform::send_signal(pid, platform::sig::kill);
    }
    fs::remove(pid_file, ec);
    fs::remove(pid_file.parent_path() / ".keeper.lastused", ec);
}

// `subos cp <src> <dst>`, one side `<name>:<path>` (design §12.1). Paths
// inside are the instance's own: its home and its /tmp.
int run_cp_(int argc, char* argv[], EventStream& stream,
            const std::function<void(std::string_view)>& usageError) {
    std::vector<std::string> paths;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (!a.empty() && a[0] == '-') { usageError("unknown option for `xlings subos cp`: " + a); return 1; }
        paths.push_back(std::move(a));
    }
    if (paths.size() != 2) { usageError("usage: xlings subos cp <src> <name>:<dst>  |  <name>:<src> <dst>"); return 1; }
    auto split = [](const std::string& p) -> std::optional<std::pair<std::string, std::string>> {
        auto colon = p.find(':');
        // A Windows drive letter (C:\x) is a host path, not an instance.
        if (colon == std::string::npos || colon < 2) return std::nullopt;
        return std::pair{p.substr(0, colon), p.substr(colon + 1)};
    };
    auto src = split(paths[0]);
    auto dst = split(paths[1]);
    if (static_cast<bool>(src) == static_cast<bool>(dst)) {
        usageError("exactly one side of `subos cp` names an instance as <name>:<path>");
        return 1;
    }
    const auto& inst = src ? *src : *dst;
    auto resolved = resolve_use_name_(inst.first, stream);
    if (resolved.selected.empty()) return resolved.exitCode;
    auto user = utils::get_env_or_default(platform::is_windows ? "USERNAME" : "USER");
    if (user.empty()) user = "user";
    if (!role_allows_(roles::Op::Copy, resolved.selected, stream)) return 1;
    // A rootfs instance's own tree is everything but what the projection and
    // the kernel provide (part 2 §6.1); a view's is its home and /tmp.
    const auto kind = subos_root::read_kind(Config::paths().homeDir, resolved.selected);
    if (!kind) {
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput, .message = kind.error(),
                                .recoverable = false });
        return 1;
    }
    const bool rootfs = *kind == roles::Kind::Rootfs;
    auto mapped = rootfs
        ? model::inside_to_root(Config::subos_dir(resolved.selected) / std::string(rootfs::kTree),
                                inst.second, Config::paths().homeDir)
        : model::inside_to_host(Config::subos_dir(resolved.selected), user, inst.second);
    if (!mapped) {
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput,
            .message = rootfs ? inst.second + " is not the root's own (/usr, /proc, /sys, /dev, /run and "
                                              "the xlings home are provided, not copied into)"
                              : inst.second + " is not the instance's own (only /home/" + user + " and /tmp are)",
            .recoverable = false });
        return 1;
    }
    // Paths inside are resolved beneath the instance's root and never
    // through a link it made (platform::copy_into_beneath).
    const auto root = rootfs ? Config::subos_dir(resolved.selected) / std::string(rootfs::kTree)
                             : Config::subos_dir(resolved.selected);
    const auto rel = mapped->lexically_relative(root);
    std::expected<void, std::string> done;
    if (src) {
        fs::path to = paths[1];
        std::error_code ec;
        if (fs::is_directory(to, ec)) to /= rel.filename();
        done = platform::copy_out_of_beneath(root, rel, to);
    } else {
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(paths[0], ec))) {
            stream.emit(ErrorEvent{ .code = ErrorCode::NotFound, .message = paths[0] + ": not found",
                                    .recoverable = false });
            return 1;
        }
        done = platform::copy_into_beneath(paths[0], root, rel);
    }
    if (!done) {
        stream.emit(ErrorEvent{ .code = ErrorCode::Internal,
            .message = "copy failed: " + done.error(), .recoverable = false });
        return 1;
    }
    observe::append(home_view().logs_dir(resolved.selected) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Fs,
        .fields = {{"event", "cp"}, {"instance", resolved.selected},
                   {"direction", src ? "out" : "in"}, {"path", inst.second}}});
    return 0;
}

}  // namespace xlings::subos
