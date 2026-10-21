module xlings.core.xim.lua_boundary;
import std;
import mcpplibs.xpkg;
import mcpplibs.xpkg.loader;
import mcpplibs.xpkg.executor;
import xlings.libs.json;
import xlings.platform;
import xlings.platform.worker;
import xlings.platform.target;
import xlings.core.config;
import xlings.core.home;
import xlings.core.home.layers;
import xlings.libs.elfread;
import xlings.core.log;
import xlings.observe;
import xlings.core.xim.payload;
import xlings.core.xim.lua_protocol;
import xlings.core.xvm.db;
import xlings.core.xvm.owner;
import xlings.subos.home_view;
import xlings.subos.policy;
import xlings.subos.policy_store;
import xlings.subos.caps;
import xlings.subos.ports;
import xlings.subos.spec;
import xlings.confine.provider;
import xlings.confine;
import xlings.subos.network;
import xlings.subos.session;
namespace xlings::xim::lua_boundary {
namespace xpkg = mcpplibs::xpkg;
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {
std::expected<std::optional<subos::policy::Policy>, std::string> policy() {
    const auto& p = Config::paths();
    return subos::policy_store::read({p.homeDir}, p.activeSubos, Info::VERSION);
}
// What reading a recipe runs under. Inside a Luban system its policy is the
// one already around this process (design 2026-10-20 §4.13 V12): a worker
// under it again needs the home written, which a sandbox's /xlings never is
// -- a desktop's search found nothing. A read there is as in a home with no
// policy.
std::expected<std::optional<subos::policy::Policy>, std::string> read_policy() {
    auto p = policy();
    if (p && *p && Config::inside_root())
        p->reset();
    return p;
}
subos::policy::Policy minimum_layer_policy() {
    auto declared = subos::policy::preset(subos::policy::Preset::Dev);
    declared.needs["fs"] = subos::policy::Need::Must;
    declared.needs["pid"] = subos::policy::Need::Must;
    return declared;
}
std::expected<bool, std::string> needs_layer_boundary(const std::string& provider = {}) {
    auto layer = home::read_system_layer();
    if (!layer)
        return std::unexpected(layer.error());
    if (!*layer)
        return false;
    std::error_code ec;
    if (fs::equivalent(**layer, Config::paths().homeDir, ec))
        return false;
    auto snapshot = home::layers::read_snapshot(**layer);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    if (provider.empty())
        return true;
    for (const auto& [target, info] : snapshot->versions) {
        for (const auto& [key, data] : info.versions) {
            const auto owner = xvm::recorded_owner(snapshot->versions, target, key);
            if (!owner)
                continue;
            const auto canonical =
                owner->ns.empty() ? owner->package : owner->ns + ":" + owner->package;
            if (canonical == provider)
                return true;
        }
    }
    return false;
}
struct AuditState {
    fs::path path;
    bool required{false};
    std::atomic<bool> warning{false};
    std::atomic<bool> failed{false};
};
struct Worker {
    fs::path scratch;
    fs::path payload;
    fs::path shadow_parent;
    fs::path recipe_logs;
    xpkg::ExecutionContext effect_context;
    bool readonly_payload{false};
    std::set<fs::path> hook_logs;
    Json effects = Json::object();
    std::shared_ptr<AuditState> audit_state = std::make_shared<AuditState>();
    bool audit(Json event) {
        if (observe::append_checked(audit_state->path,
                                    {.kind = observe::Kind::Ops, .fields = std::move(event)}))
            return true;
        if (audit_state->required) {
            failure = "locked Lua worker audit could not be persisted";
            process.reset();
            return false;
        }
        if (!audit_state->warning.exchange(true)) {
            log::warn("Lua worker audit could not be persisted: {}", audit_state->path.string());
        }
        return true;
    }
    std::expected<void, std::string> validate_effects(xpkg::HookResponse& response,
                                                      bool pending_payload = false) const {
        try {
            std::vector<fs::path> roots{fs::weakly_canonical(scratch)};
            if (!effect_context.install_dir.empty())
                roots.push_back(fs::weakly_canonical(effect_context.install_dir));
            for (const auto& [spec, dep] : effect_context.resolved_deps)
                roots.push_back(fs::canonical(dep.install_dir));
            auto under = [](const fs::path& path, const fs::path& root) {
                const auto relative = path.lexically_relative(root);
                return path == root ||
                       (!relative.empty() && !relative.is_absolute() && *relative.begin() != "..");
            };
            auto permitted = [&](const fs::path& path) {
                const auto canonical = fs::weakly_canonical(path);
                return std::ranges::any_of(
                    roots, [&](const auto& root) { return under(canonical, root); });
            };
            // `walk`: a directory source is every file under it, checked; a
            // library's directory is only where its one file is (below).
            auto system_library = [&](const fs::path& link) {
                if constexpr (!platform::is_linux) return false;
                std::error_code ec;
                if (!fs::is_symlink(fs::symlink_status(link, ec))) return false;
                const auto target = fs::canonical(link, ec);
                if (ec || !fs::is_regular_file(target, ec)) return false;
                const bool in_system = std::ranges::any_of(
                    std::array<fs::path, 4>{"/usr/lib", "/usr/lib64", "/lib", "/lib64"},
                    [&](const fs::path& dir) { return under(target, dir); });
                const auto own = platform::file_ownership(target);
                // Not this user's (root's; nobody's in a user namespace, which
                // is root outside it) and not writable by others.
                return in_system && own && own->uid != platform::user_ids().euid && !(own->mode & 022);
            };
            auto source = [&](const std::string& text, const fs::path& base, bool walk = true) {
                fs::path path(xvm::expand_path(text, Config::paths().homeDir.string()));
                if (path.empty())
                    throw std::runtime_error("empty host effect source");
                for (const auto& component : path)
                    if (component == "..")
                        throw std::runtime_error("host effect source contains upward traversal");
                if (path.is_relative())
                    path = base / path;
                if (!pending_payload && under(path.lexically_normal(), scratch)) {
                    if (effect_context.install_dir.empty() || readonly_payload)
                        throw std::runtime_error(
                            "scratch host effects require a user-owned persistent payload");
                    const auto directory = effect_context.install_dir / ".xlings-hook-effects";
                    std::error_code ec;
                    if (!fs::create_directory(directory, ec) &&
                        (ec || !fs::is_directory(fs::symlink_status(directory))))
                        throw std::runtime_error(
                            "cannot prepare owned hook effect snapshot directory");
                    std::random_device random;
                    const auto stage = directory / std::format("{:x}-{:x}", random(), random());
                    if (!fs::create_directory(stage))
                        throw std::runtime_error("cannot create exclusive hook effect snapshot");
                    const auto captured = stage / "source";
                    const auto relative = path.lexically_relative(scratch);
                    auto copied = platform::copy_out_of_beneath(scratch, relative, captured);
                    if (!copied)
                        throw std::runtime_error("cannot snapshot scratch host effect: " +
                                                 copied.error());
                    path = captured;
                }
                auto lookup = path;
                if (pending_payload && !payload.empty() && under(path.lexically_normal(), payload))
                    lookup = shadow_parent / payload.filename() / path.lexically_relative(payload);
                if (!permitted(lookup))
                    throw std::runtime_error(
                        "host effect source escapes the recorded payload closure: " +
                        path.string());
                auto stable = [&](const fs::path& item) {
                    const auto canonical = fs::weakly_canonical(item);
                    const bool shadow = pending_payload && !payload.empty() &&
                                        under(canonical, shadow_parent / payload.filename());
                    return permitted(item) &&
                           (under(path, scratch) || !under(canonical, scratch) || shadow);
                };
                if (!stable(lookup))
                    throw std::runtime_error(
                        "host effect source points into mutable worker scratch");
                std::error_code ec;
                const auto status = fs::symlink_status(lookup, ec);
                if (ec && ec != std::errc::no_such_file_or_directory)
                    throw std::runtime_error("cannot inspect host effect source: " + ec.message());
                if (fs::is_symlink(status) && !fs::exists(lookup))
                    throw std::runtime_error("host effect source is a dangling symlink");
                if (walk && fs::is_directory(lookup)) {
                    for (const auto& entry : fs::recursive_directory_iterator(lookup)) {
                        if (!stable(entry.path()))
                            throw std::runtime_error(
                                "host effect source contains a symlink outside "
                                "the recorded payload closure: " +
                                entry.path().string());
                        if (entry.is_symlink() && !fs::exists(entry.path()))
                            throw std::runtime_error(
                                "host effect source contains a dangling symlink");
                    }
                }
                return path;
            };
            const auto coordinate =
                xvm::coordinate_from_payload_path(effect_context.install_dir.generic_string());
            const bool is_xlings = coordinate && coordinate->package == "xlings" &&
                                   (coordinate->ns == "xim" || coordinate->ns.empty());
            for (auto& operation : response.xvm_ops) {
                if (operation.op == "add") {
                    if (operation.name.empty() || operation.name == "." || operation.name == ".." ||
                        operation.name.find_first_of("/\\") != std::string::npos)
                        throw std::runtime_error("invalid host effect target name");
                    if (operation.name == "xlings" && !is_xlings)
                        throw std::runtime_error(
                            "only the xlings package may register the shared client");
                    if (effect_context.install_dir.empty())
                        throw std::runtime_error("host registration requires a package payload");
                    // What a registration exposes is its own file (a program,
                    // a library: bindir/filename) or asset (files: src, walked
                    // below) -- not everything beside it. A payload may hold
                    // links another package keeps there for this host
                    // (libglvnd's lib/glx-vendor, filled by the graphics
                    // stack), which are not its registrations' and refused
                    // them in a scope that has not recorded those payloads.
                    // Only a program run through its alias has its whole
                    // directory on PATH: that directory is walked.
                    const bool library = operation.type == "lib";
                    const bool on_path = !operation.alias.empty() && !library && operation.type != "group";
                    const auto base =
                        source(operation.bindir.empty() ? effect_context.install_dir.string()
                                                        : operation.bindir,
                               effect_context.install_dir, on_path);
                    operation.bindir = base.string();
                    if (operation.type == "files") {
                        const fs::path relative(operation.src);
                        if (relative.is_absolute())
                            throw std::runtime_error(
                                "file asset source must be relative to its payload");
                        // One file, the operating system's library (as below):
                        // nvidia-gl-host-link's GBM backend.
                        if (!system_library(base / relative)) (void)source(operation.src, base);
                    } else if (library) {
                        const fs::path relative(operation.filename.empty() ? operation.name
                                                                           : operation.filename);
                        if (relative.is_absolute())
                            throw std::runtime_error("artifact source name must be relative");
                        // A library of the operating system's own (T0: a host
                        // link sentinel's -- the NVIDIA driver's userspace):
                        // in a system library directory, not this user's to
                        // change. Not a user's data; what the boundary keeps
                        // out is that, and a recipe's own scratch.
                        if (!system_library(base / relative)) (void)source(relative.string(), base);
                    } else if (operation.type != "group" && operation.alias.empty()) {
                        const auto name =
                            operation.filename.empty() ? operation.name : operation.filename;
                        const fs::path relative(name);
                        if (relative.is_absolute())
                            throw std::runtime_error("artifact source name must be relative");
                        (void)source(name, base);
                    }
                } else if (operation.op == "headers" || operation.op == "remove_headers") {
                    operation.includedir =
                        source(operation.includedir, effect_context.install_dir).string();
                }
            }
            return {};
        } catch (const std::exception& error) {
            return std::unexpected("unsafe Lua host effect: " + std::string(error.what()));
        }
    }
    std::expected<void, std::string> refresh_payload() {
        if (payload.empty())
            return {};
        try {
            std::error_code status_error;
            const auto status = fs::symlink_status(payload, status_error);
            if (status_error && status_error != std::errc::no_such_file_or_directory)
                return std::unexpected(status_error.message());
            if (status.type() != fs::file_type::directory &&
                status.type() != fs::file_type::not_found)
                return std::unexpected("hook payload must be a real directory");
            const auto target = shadow_parent / payload.filename();
            std::error_code ec;
            fs::remove_all(target, ec);
            if (ec)
                return std::unexpected(ec.message());
            if (status.type() == fs::file_type::directory)
                fs::copy(payload, target,
                         fs::copy_options::recursive | fs::copy_options::copy_symlinks);
            else
                fs::create_directories(target);
            return {};
        } catch (const std::exception& e) {
            return std::unexpected(e.what());
        }
    }
    std::expected<void, std::string> publish_payload() {
        if (payload.empty())
            return {};
        try {
            std::error_code payload_error;
            const auto previous_status = fs::symlink_status(payload, payload_error);
            if (payload_error && payload_error != std::errc::no_such_file_or_directory)
                return std::unexpected("cannot inspect hook payload destination: " +
                                       payload_error.message());
            if (previous_status.type() != fs::file_type::directory &&
                previous_status.type() != fs::file_type::not_found)
                return std::unexpected("hook payload destination must be a real directory");
            const auto source = shadow_parent / payload.filename();
            std::error_code ec;
            const auto status = fs::symlink_status(source, ec);
            if (ec && ec != std::errc::no_such_file_or_directory)
                return std::unexpected(ec.message());
            if (status.type() != fs::file_type::directory &&
                status.type() != fs::file_type::not_found)
                return std::unexpected("hook returned a non-directory payload");
            if (status.type() == fs::file_type::not_found) {
                if (remove_payload_dir(payload) != RemoveOutcome::Removed)
                    return std::unexpected("cannot withdraw hook payload");
                return {};
            }
            std::random_device random;
            const auto trash = payload_trash_root(payload);
            if (trash.empty())
                return std::unexpected("hook payload is outside a managed store");
            fs::create_directories(trash);
            const auto stage = trash / std::format("hook-{:x}-{:x}", random(), random());
            if (!fs::create_directory(stage))
                return std::unexpected("cannot create hook publication staging directory");
            struct Cleanup {
                fs::path directory;
                ~Cleanup() {
                    std::error_code ignored;
                    fs::remove_all(directory, ignored);
                }
            } cleanup{stage};
            const auto ready = stage / "payload";
            auto copied = platform::copy_out_of_beneath(shadow_parent, payload.filename(), ready);
            if (!copied)
                return std::unexpected(copied.error());
            if (previous_status.type() == fs::file_type::not_found) {
                if (fs::is_empty(ready))
                    return {};
                return platform::rename_no_replace(ready, payload);
            }
            auto previous = set_aside_payload(payload);
            if (!previous)
                return std::unexpected(previous.error());
            if (remove_payload_dir(payload) != RemoveOutcome::Removed)
                return std::unexpected("cannot prepare hook payload destination");
            auto published = platform::rename_no_replace(ready, payload);
            if (!published)
                return std::unexpected(published.error());
            previous->commit();
            return {};
        } catch (const std::exception& e) {
            return std::unexpected(e.what());
        }
    }

    std::optional<platform::worker::Process> process;
    std::optional<std::string> failure;
    ~Worker() {
        process.reset();
        if (!scratch.empty()) {
            std::error_code ec;
            fs::remove_all(scratch, ec);
        }
    }
    std::expected<Json, std::string> call(Json request) {
        if (audit_state->failed.load()) {
            failure = "locked Lua worker exec audit could not be persisted";
            process.reset();
        }
        if (failure)
            return std::unexpected(*failure);
        const auto operation = request.at("operation").get<std::string>();
        const bool capture = operation != "executor" ||
                             request.at("invocation").at("action") == "load" ||
                             request.at("invocation").at("action") == "set_log_level";
        if (capture) {
            auto name = operation;
            if (request.contains("invocation"))
                name += "." + request.at("invocation").at("action").get<std::string>();
            request["capture_log"] = (scratch / (name + ".log")).generic_string();
        }
        Json event = {{"event", "recipe-worker-action"}, {"operation", request.at("operation")}};
        if (request.contains("package"))
            event["package"] = request.at("package");
        if (request.contains("invocation")) {
            event["package"] = request.at("invocation").at("package");
            event["action"] = request.at("invocation").at("action");
            event["hook"] = request.at("invocation").at("hook");
        }
        if (!audit(event))
            return std::unexpected(*failure);
        request["protocol"] = xpkg::kHookBoundaryProtocol;
        auto wire = process->exchange(request.dump());
        if (audit_state->failed.load()) {
            failure = "locked Lua worker exec audit could not be persisted";
            process.reset();
            return std::unexpected(*failure);
        }
        if (!wire) {
            failure = wire.error();
            return std::unexpected(*failure);
        }
        try {
            auto answer = Json::parse(*wire);
            if (answer.at("protocol").get<int>() != xpkg::kHookBoundaryProtocol)
                throw std::runtime_error("Lua worker protocol mismatch");
            if (capture) {
                const auto source = fs::path(request.at("capture_log").get<std::string>());
                auto captured = platform::worker::read_log(scratch, source.filename().string());
                if (captured) {
                    const auto& contents = *captured;
                    const auto published = recipe_logs / (scratch.filename().string() + "." +
                                                          source.filename().string());
                    try {
                        platform::write_file_atomic(published.string(), contents);
                    } catch (const std::exception& error) {
                        failure = "cannot publish recipe log " + published.string() + ": " +
                                  error.what();
                        return std::unexpected(*failure);
                    }
                    if (!answer.at("ok").get<bool>()) {
                        std::istringstream lines(contents);
                        std::deque<std::string> tail;
                        std::string line;
                        while (std::getline(lines, line)) {
                            tail.push_back(line);
                            if (tail.size() > 20)
                                tail.pop_front();
                        }
                        std::string message = answer.at("error").get<std::string>() +
                                              "\nrecipe log: " + published.string();
                        for (const auto& text : tail)
                            message += "\n" + text;
                        answer["error"] = std::move(message);
                    }
                } else if (answer.at("ok").get<bool>()) {
                    throw std::runtime_error("recipe output unavailable: " + captured.error());
                }
            }
            if (!answer.at("ok").get<bool>()) {
                failure = answer.at("error").get<std::string>();
                return std::unexpected(*failure);
            }
            event["event"] = "recipe-worker-result";
            if (!audit(event))
                return std::unexpected(*failure);
            return answer.at("value");
        } catch (const std::exception& e) {
            failure = "invalid Lua worker response: " + std::string(e.what());
            return std::unexpected(*failure);
        }
    }
};
std::expected<std::shared_ptr<Worker>, std::string>
launch(const subos::policy::Policy& declared, const fs::path& package,
       const xpkg::ExecutionContext& context, bool build = false,
       const std::vector<fs::path>& hook_logs = {}, bool readonly_payload = false,
       bool layer_only = false) {
    try {
        const auto& paths = Config::paths();
        subos::HomeView home{paths.homeDir};
        auto w = std::make_shared<Worker>();
        w->effect_context = context;
        w->readonly_payload = readonly_payload;
        w->audit_state->path = home.logs_dir(paths.activeSubos) / "events.ndjson";
        w->recipe_logs = fs::canonical(paths.homeDir);
        for (const auto directory : {"logs", "recipes"}) {
            w->recipe_logs /= directory;
            std::error_code error;
            const auto status = fs::symlink_status(w->recipe_logs, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return std::unexpected("cannot inspect recipe log directory: " + error.message());
            if (fs::exists(status) && !fs::is_directory(status))
                return std::unexpected("recipe log directory is not an owned directory: " +
                                       w->recipe_logs.string());
            if (!fs::exists(status))
                fs::create_directory(w->recipe_logs);
            if (fs::canonical(w->recipe_logs) != w->recipe_logs)
                return std::unexpected("recipe log directory traverses a symlink: " +
                                       w->recipe_logs.string());
        }
        w->audit_state->required = declared.preset == subos::policy::Preset::Locked;
        auto parent = home.run_dir(paths.activeSubos) / "lua";
        fs::create_directories(parent);
        // In a machine the home is the system's tree over 9p, where the
        // worker's private directory and its captured output cannot be made
        // as they must be (design 2026-10-20 WuXian §13.2, H26): the
        // session's own tmpfs instead.
        if (Config::inside_root() && platform::fs_caps(parent).type == "9p")
            if (const auto env = platform::environment(); env.contains("XDG_RUNTIME_DIR") && !env.at("XDG_RUNTIME_DIR").empty()) {
                const auto& runtime = env.at("XDG_RUNTIME_DIR");
                std::error_code ec;
                const auto local = fs::path(runtime) / "xlings-lua" / paths.activeSubos;
                fs::create_directories(local, ec);
                if (!ec) parent = local;
            }
        // The instance the worker is confined to: a view made a moment ago
        // has none yet (bwrap cannot bind what is not there).
        fs::create_directories(home.instance(paths.activeSubos));
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate =
                parent / std::format("{}-{:x}-{:x}", platform::get_pid(), random(), random());
            if (fs::create_directory(candidate)) {
                w->scratch = candidate;
                break;
            }
        }
        if (w->scratch.empty())
            return std::unexpected("cannot create private Lua worker directory");
        fs::permissions(w->scratch, fs::perms::owner_all, fs::perm_options::replace);
        auto executable = platform::get_executable_path();
        std::vector<std::string> command{executable.string(), "__xpkg-worker",
                                         std::string(platform::worker::kReadToken),
                                         std::string(platform::worker::kWriteToken)};
        const bool full_trace =
            platform::is_linux && declared.observe == subos::policy::Observe::Full;
        if (full_trace)
            command.push_back(std::string(platform::worker::kTraceToken));
        auto capabilities = subos::caps::probe(home, {});
        // Identity files belong to this worker, rather than changing user instance data.
        const auto identity = w->scratch / "etc";
        fs::create_directory(identity);
        const auto ids = platform::user_ids();
        const auto login = std::string("user");
        platform::write_string_to_file(
            (identity / "passwd").string(),
            std::format("{}:x:{}:{}::/home/{}:/bin/sh\n", login, ids.uid, ids.gid, login));
        platform::write_string_to_file((identity / "group").string(),
                                       std::format("{}:x:{}:\n", login, ids.gid));
        platform::write_string_to_file((identity / "hosts").string(),
                                       "127.0.0.1 localhost\n::1 localhost\n");
        platform::write_string_to_file((identity / "nsswitch.conf").string(),
                                       "passwd: files\ngroup: files\nhosts: files dns\n");

        auto p = declared;
        // A hook is an install action, never a second user session with desktop grants.
        p.grants.clear();
        p.grants_allowed.clear();
        p.mounts.clear();
        auto inherited = platform::environment();
        subos::spec::Request request{.instance = paths.activeSubos,
                                     .instance_dir = home.instance(paths.activeSubos),
                                     .user = "user",
                                     .argv = command,
                                     .host_env = inherited,
                                     .cwd = w->scratch.string()};
        // What the layer boundary (needs_layer_boundary, a borrowed payload)
        // asks of the filesystem is a write fence: a recipe does not write the
        // layer below this home or the payloads it borrows. Inside a Luban
        // system nested user namespaces are forbidden and bwrap cannot start;
        // there Landlock fences the writes, and the system's own pid namespace
        // already separates the worker from the host's processes. On a host
        // bwrap remains the only way, and a policy the user declared keeps
        // what it declared.
        const bool fence = platform::is_linux && layer_only && Config::inside_root() &&
                           !(capabilities.bwrap && capabilities.bwrap->usable) &&
                           capabilities.landlock_abi >= 1;
        if (fence) {
            for (const auto dimension : {"fs", "pid"})
                if (auto need = p.needs.find(dimension); need != p.needs.end())
                    need->second = subos::policy::Need::Should;
            // What the worker writes, outside the instance the fence allows
            // (design 2026-10-20 §4.10, G12): its private directory and the
            // captured output in it, the recipe and hook logs, and this home's
            // own store -- never the layer below or a borrowed payload.
            for (const auto& dir : {w->scratch, w->recipe_logs, paths.homeDir / "logs", paths.dataDir / "xpkgs"}) {
                std::error_code ec;
                fs::create_directories(dir, ec);
                if (!ec) p.mounts.push_back({.src = dir.string(), .dst = {}, .rw = true, .mode_given = true});
            }
            for (const auto& log : hook_logs)
                if (!log.empty()) p.mounts.push_back({.src = log.parent_path().string(), .dst = {}, .rw = true, .mode_given = true});
        }
        if (platform::is_linux)
            request.preferred = fence ? subos::spec::Backend::Landlock : subos::spec::Backend::Bwrap;
        auto compiled = xlings::confine::compile(p, home, capabilities, request);
        if (!compiled) {
            std::string reason = "Lua worker isolation unavailable";
            for (const auto& missing : compiled.error().missing)
                reason += "; " + missing.dimension + ": " + missing.reason;
            return std::unexpected(reason);
        }
        auto sandbox = std::move(*compiled);
        if (sandbox.backend != subos::spec::Backend::Bwrap &&
            sandbox.backend != subos::spec::Backend::Landlock)
            for (const auto& path : hook_logs)
                if (!path.empty())
                    w->hook_logs.insert(path.lexically_normal());
        // An owned hook log: a regular file in <home>/logs/hooks, reached
        // through no symlink, made before the worker starts.
        const auto log_directory = home.home / "logs" / "hooks";
        auto own_log = [&](const fs::path& path) -> std::expected<void, std::string> {
            if (path.parent_path().lexically_normal() != log_directory.lexically_normal())
                return std::unexpected("hook log is outside the owned hook log directory");
            fs::create_directories(log_directory);
            if (fs::canonical(log_directory) != fs::canonical(home.home) / "logs" / "hooks")
                return std::unexpected("hook log directory traverses a symlink");
            if (!platform::worker::prepare_log(path))
                return std::unexpected("hook log is not a writable regular file: " + path.string());
            w->hook_logs.insert(path.lexically_normal());
            return {};
        };
        if (sandbox.backend == subos::spec::Backend::Bwrap) {
            // No compiled instance mount is writable by default for a recipe.
            for (auto& mount : sandbox.mounts) {
                if (mount.kind == subos::spec::MountKind::Bind)
                    mount.kind = subos::spec::MountKind::RoBind;
                for (const auto file : {"passwd", "group", "hosts", "nsswitch.conf"})
                    if (mount.dst == std::string("/etc/") + file)
                        mount.src = (identity / file).generic_string();
                if (mount.dst == "/home" || mount.dst == "/tmp") {
                    auto private_dir = w->scratch / (mount.dst == "/home" ? "home" : "tmp");
                    fs::create_directories(private_dir);
                    mount.kind = subos::spec::MountKind::Bind;
                    mount.src = private_dir.generic_string();
                }
            }

            auto ro = [&](const fs::path& path) {
                if (path.empty() || !fs::exists(path))
                    return;
                const auto destination = fs::absolute(path).lexically_normal();
                const auto source = fs::canonical(destination);
                // The read-only home already exposes its index symlinks. Make
                // their exact canonical targets visible in private /tmp/home;
                // binding again through the alias cannot create a destination
                // beneath that read-only symlink.
                sandbox.mounts.push_back({subos::spec::MountKind::RoBind, source.generic_string(),
                                          source.generic_string()});
            };
            auto rw = [&](const fs::path& path) {
                if (path.empty())
                    return;
                fs::create_directories(path);
                auto canonical = fs::canonical(path);
                sandbox.mounts.push_back({subos::spec::MountKind::Bind, canonical.generic_string(),
                                          canonical.generic_string()});
            };
            auto runtime_ro = [&](const fs::path& path) {
                const auto destination = fs::absolute(path).lexically_normal();
                const auto source = fs::canonical(destination);
                ro(source);
                if (source == destination)
                    return;
                // Keep aliases already provided by a home/userland mount. A
                // masked runtime directory instead needs its literal INTERP
                // or RPATH spelling, as well as the canonical library target.
                for (const auto& mount : sandbox.mounts | std::views::reverse) {
                    const auto relative = destination.lexically_relative(fs::path(mount.dst));
                    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
                        continue;
                    if (mount.kind == subos::spec::MountKind::RoBind ||
                        mount.kind == subos::spec::MountKind::Bind) {
                        std::error_code error;
                        if (fs::equivalent(fs::path(mount.src) / relative, source, error))
                            return;
                    }
                    break;
                }
                sandbox.mounts.push_back({subos::spec::MountKind::RoBind, source.generic_string(),
                                          destination.generic_string()});
            };
            ro(executable);
            if (auto runtime = elfread::read(executable)) {
                std::set<fs::path> runtimeDirectories;
                if (!runtime->interpreter.empty()) {
                    if (!fs::is_regular_file(runtime->interpreter))
                        return std::unexpected("Lua worker ELF interpreter is unavailable: " +
                                               runtime->interpreter);
                    runtimeDirectories.insert(fs::path(runtime->interpreter).parent_path());
                }
                for (auto path : runtime->searchPaths) {
                    for (const auto token :
                         {std::string_view("${ORIGIN}"), std::string_view("$ORIGIN")}) {
                        std::size_t position{};
                        while ((position = path.find(token, position)) != std::string::npos) {
                            const auto origin = executable.parent_path().generic_string();
                            path.replace(position, token.size(), origin);
                            position += origin.size();
                        }
                    }
                    runtimeDirectories.insert(fs::path(path));
                }
                for (const auto& directory : runtimeDirectories) {
                    if (!directory.is_absolute() || !fs::is_directory(directory))
                        continue;
                    // A dev build's managed loader and libraries can be outside
                    // host_userland(), under the home that this worker masks.
                    runtime_ro(directory);
                }
                if (!runtime->interpreter.empty())
                    runtime_ro(runtime->interpreter);
            } else if (elfread::is_elf(executable)) {
                return std::unexpected("cannot read the Lua worker ELF runtime: " +
                                       executable.string());
            }
            ro(package);
            ro(context.xpkg_dir);
            ro(context.project_data_dir);
            if (!Config::project_dir().empty())
                ro(Config::project_dir());
            if (!context.pkgindex_dir.empty())
                ro(context.pkgindex_dir);
            for (const auto& path : context.dependency_store_roots)
                ro(path);
            rw(w->scratch);
            if (build)
                rw(package);
            else if (readonly_payload) {
                // A link entry (an upstream's payload) is seen through its
                // store; what it names is mounted at its own path.
                std::error_code rec;
                const auto real = fs::canonical(context.install_dir, rec);
                ro(rec ? context.install_dir : real);
            } else if (!context.install_dir.empty()) {
                w->payload = fs::absolute(context.install_dir).lexically_normal();
                const auto parent = w->payload.parent_path();
                const auto store = parent.parent_path();
                const bool declared_store = std::ranges::any_of(
                    context.dependency_store_roots, [&](const auto& root) {
                        return fs::absolute(root).lexically_normal() == store;
                    });
                if (!declared_store || store.filename() != "xpkgs" ||
                    !xvm::coordinate_from_payload_path(w->payload.string()))
                    return std::unexpected("hook payload is outside its declared managed store");
                const auto expected_parent = fs::weakly_canonical(store) / parent.filename();
                if (fs::weakly_canonical(parent) != expected_parent)
                    return std::unexpected("hook payload parent traverses a symlink");
                fs::create_directories(parent);
                if (fs::canonical(parent) != expected_parent)
                    return std::unexpected("hook payload parent traverses a symlink");
                w->shadow_parent = w->scratch / "payload";
                fs::create_directory(w->shadow_parent);
                auto refreshed = w->refresh_payload();
                if (!refreshed)
                    return std::unexpected(refreshed.error());
                sandbox.mounts.push_back({subos::spec::MountKind::Bind,
                                          w->shadow_parent.generic_string(),
                                          w->payload.parent_path().generic_string()});
            }
            for (const auto& path : hook_logs) {
                if (path.empty())
                    continue;
                if (auto owned = own_log(path); !owned)
                    return std::unexpected(owned.error());
                sandbox.mounts.push_back(
                    {subos::spec::MountKind::Bind, path.generic_string(), path.generic_string()});
            }
            command = xlings::confine::provider::bwrap_argv(sandbox);
        } else if (sandbox.backend == subos::spec::Backend::Landlock) {
            // The fence the worker puts up before it reads anything
            // (worker_main): it writes its scratch, the devices, its hook
            // logs and what it builds -- an index being built, or this
            // install's payload directory (its parent, which a hook may
            // replace it in; bwrap shows it a copy of that directory) --
            // and nothing else: not the layer below, not a borrowed payload.
            std::vector<fs::path> writable{w->scratch, "/dev"};
            if (build) {
                writable.push_back(package);
            } else if (!readonly_payload && !context.install_dir.empty()) {
                const auto parent = fs::absolute(context.install_dir).lexically_normal().parent_path();
                fs::create_directories(parent);
                writable.push_back(fs::canonical(parent));
            }
            for (const auto& path : hook_logs) {
                if (path.empty())
                    continue;
                if (auto owned = own_log(path); !owned)
                    return std::unexpected(owned.error());
                writable.push_back(path);
            }
            for (const auto directory : {"home", "tmp"})
                fs::create_directories(w->scratch / directory);
            const auto private_home = (w->scratch / "home").generic_string();
            sandbox.env["HOME"] = private_home;
            sandbox.env["TMPDIR"] = (w->scratch / "tmp").generic_string();
            sandbox.env["XDG_CONFIG_HOME"] = private_home + "/.config";
            sandbox.env["XDG_DATA_HOME"] = private_home + "/.local/share";
            sandbox.env["XDG_CACHE_HOME"] = private_home + "/.cache";
            sandbox.env["XDG_STATE_HOME"] = private_home + "/.local/state";
            sandbox.landlock_rw = std::move(writable);
            log::debug("Lua worker: a Landlock write fence (inside a Luban system; pid: the system's)");
        } else {
            // compile() refused all Must dimensions; this is the documented advisory
            // platform path, still an external worker with no in-process fallback.
            log::warn("Lua worker isolation is advisory on {}", capabilities.platform);
        }
        auto env = xlings::confine::provider::process_env(sandbox, inherited);
        env.erase("XLINGS_BROKER_SOCKET");
        env.erase("XLINGS_SUBOS_MODE");
        platform::worker::Network network;
        if (sandbox.net_nat) {
            network.pasta = xlings::confine::provider::pasta_args(sandbox);
            network.pid_file = w->scratch / "pasta.pid";
        }
        if (sandbox.net_proxy) {
            auto resolved = subos::network::resolve_proxy(sandbox.proxy_url);
            if (!resolved)
                return std::unexpected(resolved.error());
            auto relay = std::make_shared<subos::network::Relay>(std::move(*resolved));
            network.proxy = true;
            network.gateway_port = subos::network::GATEWAY_PORT;
            network.gateway = [](int channel, int listener) {
                return subos::network::gateway_run(channel, listener);
            };
            network.accept_client = [relay](int client) { return relay->add(client); };
            network.relay_fds = [relay] { return relay->fds(); };
            network.relay_tick = [relay, state = w->audit_state] {
                relay->tick([state](const subos::network::Event& event) {
                    bool stored = observe::append_checked(
                        state->path, {.kind = observe::Kind::Net,
                                      .fields = {{"event", event.event},
                                                 {"worker", "lua"},
                                                 {"connection", event.connection},
                                                 {"target", event.target},
                                                 {"port", event.port},
                                                 {"remote_dns", event.remote_dns},
                                                 {"reason", event.reason}}});
                    if (!stored && state->required) {
                        state->failed.store(true);
                        return false;
                    }
                    if (!stored && !state->warning.exchange(true))
                        log::warn("Lua proxy audit could not be persisted: {}",
                                  state->path.string());
                    return true;
                });
            };
        }
        platform::worker::Trace trace;
        trace.enabled = full_trace;
        trace.audit = [state = w->audit_state](int pid, std::string_view path) {
            const bool stored =
                observe::append_checked(state->path, {.kind = observe::Kind::Exec,
                                                      .fields = {{"event", "exec"},
                                                                 {"worker", "lua"},
                                                                 {"pid", pid},
                                                                 {"path", std::string(path)}}});
            if (stored)
                return true;
            if (state->required) {
                state->failed.store(true);
                return false;
            }
            if (!state->warning.exchange(true))
                log::warn("Lua worker exec audit could not be persisted: {}", state->path.string());
            return true;
        };
        trace.net_audit = [state =
                               w->audit_state](const platform::net_notify::Notice& notification) {
            const bool stored = observe::append_checked(
                state->path, {.kind = observe::Kind::Net,
                              .fields = {{"event", "net-attempt"},
                                         {"worker", "lua"},
                                         {"pid", notification.pid},
                                         {"syscall", notification.syscall},
                                         {"address", notification.address},
                                         {"port", notification.port},
                                         {"address_readable", notification.address_readable},
                                         {"result", "unknown"}}});
            if (stored)
                return true;
            if (state->required) {
                state->failed.store(true);
                return false;
            }
            if (!state->warning.exchange(true))
                log::warn("Lua worker network audit could not be persisted: {}",
                          state->path.string());
            return true;
        };
        auto child = platform::worker::Process::launch(command, env, network, trace);
        if (!child)
            return std::unexpected(child.error());
        w->process.emplace(std::move(*child));
        return w;
    } catch (const std::exception& e) {
        return std::unexpected("cannot launch Lua worker: " + std::string(e.what()));
    }
}
} // namespace
std::expected<xpkg::Package, std::string> metadata(const fs::path& package,
                                                   const xpkg::LoaderContext& context,
                                                   const std::string& formal_provider) {
    auto p = read_policy();
    if (!p)
        return std::unexpected(p.error());
    const auto layer_required = needs_layer_boundary(formal_provider);
    if (!layer_required)
        return std::unexpected(layer_required.error());
    if (!*p && !*layer_required)
        return xpkg::load_package(package, context);
    const bool layer_only = !*p;
    if (!*p)
        p->emplace(minimum_layer_policy());
    if (*layer_required) {
        (**p).needs["fs"] = subos::policy::Need::Must;
        (**p).needs["pid"] = subos::policy::Need::Must;
    }
    const auto absolute_package = fs::absolute(package);
    auto w = launch(**p, absolute_package, {}, false, {}, false, layer_only);
    if (!w)
        return std::unexpected(w.error());
    return xpkg::load_package(absolute_package, context,
                              [worker = *w](const xpkg::MetadataInvocation& invocation)
                                  -> std::expected<xpkg::Package, std::string> {
                                  auto answer = worker->call(
                                      {{"operation", "metadata"},
                                       {"package", invocation.package.generic_string()},
                                       {"platform", invocation.context.platform},
                                       {"arch", invocation.context.arch}});
                                  if (!answer)
                                      return std::unexpected(answer.error());
                                  try {
                                      return lua_protocol::package(*answer);
                                  } catch (const std::exception& e) {
                                      return std::unexpected(e.what());
                                  }
                              });
}
std::expected<xpkg::PackageIndex, std::string> build_index(const fs::path& repo,
                                                           const std::string& ns,
                                                           const xpkg::LoaderContext& context,
                                                           const xpkg::BuildOutput& output) {
    auto p = read_policy();
    if (!p)
        return std::unexpected(p.error());
    const auto layer_required = needs_layer_boundary();
    if (!layer_required)
        return std::unexpected(layer_required.error());
    if (!*p && !*layer_required)
        return xpkg::build_index(repo, ns, output);
    const bool layer_only = !*p;
    if (!*p)
        p->emplace(minimum_layer_policy());
    if (*layer_required) {
        (**p).needs["fs"] = subos::policy::Need::Must;
        (**p).needs["pid"] = subos::policy::Need::Must;
    }
    xpkg::LoaderBoundaries boundaries;
    boundaries.metadata = [&](const xpkg::MetadataInvocation& invocation) {
        return metadata(invocation.package, invocation.context);
    };
    boundaries.index_build =
        [&](const fs::path& directory,
            const xpkg::BuildOutput& sink) -> std::expected<void, std::string> {
        auto worker = launch(**p, directory, {}, true, {}, false, layer_only);
        if (!worker)
            return std::unexpected(worker.error());
        auto result = (*worker)->call({{"operation", "build_index"},
                                       {"package", directory.generic_string()},
                                       {"platform", context.platform},
                                       {"arch", context.arch}});
        if (!result)
            return std::unexpected(result.error());
        if (sink)
            sink(result->at("output").get<std::string>());
        return {};
    };
    return xpkg::build_index(repo, ns, context, boundaries, output);
}
std::expected<xpkg::PackageExecutor, std::string>
create_executor(const fs::path& package, xpkg::ExecutionContext& context,
                const std::vector<fs::path>& hook_logs, bool readonly_payload) {
    auto p = policy();
    if (!p)
        return std::unexpected(p.error());
    if (!*p && !readonly_payload)
        return xpkg::create_executor(package);
    const bool layer_only = !*p;
    if (readonly_payload) {
        if (!*p)
            p->emplace(minimum_layer_policy());
        (**p).needs["fs"] = subos::policy::Need::Must;
        (**p).needs["pid"] = subos::policy::Need::Must;
    }
    const auto absolute_package = fs::absolute(package);
    auto worker =
        launch(**p, absolute_package, context, false, hook_logs, readonly_payload, layer_only);
    if (!worker)
        return std::unexpected(worker.error());
    if (!context.install_file.empty()) {
        const auto private_file = (*worker)->scratch / context.install_file.filename();
        std::error_code ec;
        fs::copy_file(context.install_file, private_file, fs::copy_options::none, ec);
        if (ec)
            return std::unexpected("cannot stage hook archive: " + ec.message());
        context.install_file = private_file;
    }
    return xpkg::create_executor(
        absolute_package,
        [worker = *worker](const xpkg::HookInvocation& original)
            -> std::expected<xpkg::HookResponse, std::string> {
            auto invocation = original;
            if (!invocation.context.hook_log.empty() &&
                !worker->hook_logs.contains(invocation.context.hook_log.lexically_normal()))
                return std::unexpected("hook log was not declared before worker launch");
            if (!worker->readonly_payload && (invocation.action == xpkg::HookAction::RunHook ||
                                              invocation.action == xpkg::HookAction::Elfpatch)) {
                auto refreshed = worker->refresh_payload();
                if (!refreshed)
                    return std::unexpected(refreshed.error());
            }
            Json env = Json::object();
            for (const auto& [name, value] : platform::environment())
                if (name == "PATH" || name.starts_with("XLINGS_BUILDDEP_"))
                    env[name] = value;
            auto cwd = worker->scratch;
            if (invocation.action == xpkg::HookAction::RunHook &&
                invocation.hook == xpkg::HookType::Install &&
                !invocation.context.install_file.empty())
                cwd = invocation.context.install_file.parent_path();
            else if ((invocation.action == xpkg::HookAction::RunHook ||
                      invocation.action == xpkg::HookAction::Elfpatch) &&
                     !invocation.context.install_dir.empty())
                cwd = invocation.context.install_dir;
            else if (invocation.action == xpkg::HookAction::RunScript &&
                     !Config::project_dir().empty())
                cwd = Config::project_dir();
            auto answer = worker->call({{"operation", "executor"},
                                        {"invocation", lua_protocol::encode(invocation)},
                                        {"environment", std::move(env)},
                                        {"cwd", cwd.generic_string()}});
            if (!answer)
                return std::unexpected(answer.error());
            try {
                auto result = lua_protocol::response(*answer);
                const bool publishes =
                    !worker->readonly_payload && (invocation.action == xpkg::HookAction::RunHook ||
                                                  invocation.action == xpkg::HookAction::Elfpatch);
                if (auto safe = worker->validate_effects(result, publishes); !safe) {
                    worker->failure = safe.error();
                    return std::unexpected(safe.error());
                }
                if (invocation.action != xpkg::HookAction::RunScript) {
                    for (const auto field : {"xvm_ops", "install_requests"}) {
                        const auto& current = answer->at(field);
                        if (worker->effects.contains(field)) {
                            const auto& previous = worker->effects.at(field);
                            if (current.size() < previous.size())
                                throw std::runtime_error("hook effect snapshot regressed");
                            for (std::size_t i = 0; i < previous.size(); ++i)
                                if (current.at(i) != previous.at(i))
                                    throw std::runtime_error(
                                        "hook effect snapshot rewrote an applied operation");
                        }
                        worker->effects[field] = current;
                    }
                }
                if (publishes) {
                    auto published = worker->publish_payload();
                    if (!published)
                        return std::unexpected("cannot publish hook payload: " + published.error());
                }
                if (auto safe = worker->validate_effects(result); !safe) {
                    worker->failure = safe.error();
                    return std::unexpected(safe.error());
                }
                return result;
            } catch (const std::exception& e) {
                worker->failure = e.what();
                return std::unexpected(e.what());
            }
        });
}
int worker_main(int argc, char* argv[]) {
    if (argc != 4 && argc != 5)
        return 125;
    auto attached = platform::worker::attach(argv[2], argv[3]);
    if (!attached)
        return 125;
    auto channel = *attached;
    // A Landlock worker (launch, inside a Luban system) fences its writes
    // before it reads a request; a fence that cannot be put up ends it.
    if (const auto env = platform::environment();
        env.contains(std::string(subos::session::kLandlockRwEnv))) {
        const auto rw = env.find(std::string(subos::session::kLandlockRwEnv));
        std::vector<fs::path> paths;
        std::istringstream in{rw->second};
        for (std::string line; std::getline(in, line);)
            if (!line.empty())
                paths.emplace_back(line);
        platform::unset_env_variable(std::string(subos::session::kLandlockRwEnv));
        if (!platform::landlock::restrict_writes(paths, true)) {
            platform::worker::close(channel);
            return 125;
        }
    }
    if (argc == 5 && !platform::worker::install_trace(argv[4])) {
        platform::worker::close(channel);
        return 125;
    }
    xpkg::HookWorker worker;
    while (true) {
        auto input = platform::worker::receive(channel);
        if (!input)
            break;
        Json answer = {{"protocol", xpkg::kHookBoundaryProtocol}, {"ok", false}};
        std::optional<platform::worker::OutputCapture> capture;
        try {
            auto request = Json::parse(*input);
            if (request.at("protocol").get<int>() != xpkg::kHookBoundaryProtocol)
                throw std::runtime_error("unsupported Lua worker protocol");
            const auto operation = request.at("operation").get<std::string>();
            const bool capture_output = operation != "executor" ||
                                        request.at("invocation").at("action") == "load" ||
                                        request.at("invocation").at("action") == "set_log_level";
            if (capture_output)
                capture.emplace(fs::path(request.at("capture_log").get<std::string>()));
            if (operation == "metadata") {
                auto result =
                    xpkg::load_package(request.at("package").get<std::string>(),
                                       {.platform = request.at("platform").get<std::string>(),
                                        .arch = request.at("arch").get<std::string>()});
                if (!result)
                    throw std::runtime_error(result.error());
                answer["value"] = lua_protocol::encode(*result);
            } else if (operation == "build_index") {
                std::string output;
                auto result = xpkg::build_index(request.at("package").get<std::string>(), {},
                                                [&](std::string_view text) { output += text; });
                if (!result)
                    throw std::runtime_error(result.error());
                answer["value"] = {{"output", std::move(output)}};
            } else if (operation == "executor") {
                const auto& environment = request.at("environment");
                for (auto it = environment.begin(); it != environment.end(); ++it) {
                    const auto& name = it.key();
                    if (name != "PATH" && !name.starts_with("XLINGS_BUILDDEP_"))
                        throw std::runtime_error("invalid worker environment update");
                    platform::set_env_variable(name, it.value().get<std::string>());
                }
                fs::current_path(request.at("cwd").get<std::string>());
                auto result = worker.dispatch(lua_protocol::invocation(request.at("invocation")));
                if (!result)
                    throw std::runtime_error(result.error());
                answer["value"] = lua_protocol::encode(*result);
            } else
                throw std::runtime_error("unknown Lua worker operation");
            answer["ok"] = true;
        } catch (const std::exception& e) {
            answer["error"] = e.what();
        }
        capture.reset();
        if (!platform::worker::send(channel, answer.dump()))
            break;
    }
    platform::worker::close(channel);
    return 0;
}
} // namespace xlings::xim::lua_boundary
