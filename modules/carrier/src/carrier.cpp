module xlings.carrier;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.platform.stream;
import xlings.subos.home_view;

namespace xlings::carrier {

namespace {

Carrier local() {
    Carrier c;
    c.name = "local";
    c.probe = [](const HomeView&) { return Probe{.supported = true, .reason = "this machine's kernel"}; };
    c.ensure = [](const HomeView&) -> std::expected<Endpoint, std::string> {
        return Endpoint{.carrier = "local", .xlings = platform::get_executable_path().string()};
    };
    c.stop = [](const Endpoint&) -> std::expected<void, std::string> { return {}; };
    c.grant = [](const Endpoint&, const fs::path& host, bool, std::string_view) -> std::expected<std::string, std::string> {
        return host.string();   // it is already here
    };
    return c;
}

std::vector<std::string> command(const Endpoint& at, std::span<const std::string> args,
                                 const std::map<std::string, std::string>& env) {
    std::vector<std::string> argv = at.launcher;
    if (!at.launcher.empty() && !env.empty()) {
        // There is a Linux userland (a WSL2 distribution, a VM): env(1)
        // carries the variables across the launcher, which may not.
        argv.push_back("/usr/bin/env");
        for (const auto& [k, v] : env) argv.push_back(k + "=" + v);
    }
    argv.push_back(at.xlings);
    argv.insert(argv.end(), args.begin(), args.end());
    return argv;
}

}  // namespace

std::span<const Carrier> carriers() {
    static const std::vector<Carrier> all{local()};
    return all;
}

const Carrier* find(std::string_view name) {
    for (const auto& c : carriers())
        if (c.name == name) return &c;
    return nullptr;
}

std::vector<std::string_view> carriers_of(std::string_view platform) {
    if (platform == "windows") return {"local", "wsl2"};
    if (platform == "macos") return {"local", "vz"};
    return {"local"};
}

int terminal(const Endpoint& at, std::span<const std::string> args, const std::map<std::string, std::string>& env) {
    if (at.launcher.empty())
        for (const auto& [k, v] : env) platform::set_env_variable(k, v);
    return platform::run_argv(command(at, args, env));
}

std::expected<Reply, std::string> control(const Endpoint& at, std::string_view capability, const nlohmann::json& args) {
    const std::vector<std::string> call{"interface", std::string(capability), "--args", args.dump()};
    std::string out, err;
    const int rc = platform::stream::run(command(at, call, {}), [&](std::string_view stream, std::string_view bytes) {
        (stream == "stderr" ? err : out).append(bytes);
    });
    Reply reply;
    reply.diagnostics = std::move(err);
    std::istringstream lines(out);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        if (j.value("kind", "") == "result") reply.result = std::move(j);
        else reply.events.push_back(std::move(j));
    }
    if (!reply.result.is_object()) {
        return std::unexpected(std::format("{} carrier: `xlings interface {}` answered no result (exit {}){}{}",
                                           at.carrier, capability, rc, reply.diagnostics.empty() ? "" : ": ",
                                           reply.diagnostics));
    }
    reply.exit_code = reply.result.value("exitCode", rc);
    return reply;
}

std::expected<Choice, Refused> choose(std::string_view platform, const Want& want,
                                      const std::function<Probe(std::string_view)>& probe) {
    const auto available = carriers_of(platform);
    auto checked = [&](std::string_view name, std::string why) -> std::expected<Choice, Refused> {
        const auto p = probe(name);
        if (!p.supported) return std::unexpected(Refused{p.reason, p.route});
        return Choice{std::string(name), std::move(why)};
    };
    if (!want.requested.empty() && want.requested != "auto") {
        if (std::ranges::find(available, want.requested) == available.end())
            return std::unexpected(Refused{std::format("the {} carrier does not exist on {}", want.requested, platform),
                                           std::format("--carrier {}", available.front())});
        if (want.requested == "local" && platform != "linux" && (want.abi == "linux" || want.root))
            return std::unexpected(Refused{std::format("a Linux {} cannot run on {}'s own kernel",
                                                       want.root ? "root" : "SubOS", platform),
                                           std::format("--carrier {}", available.back())});
        return checked(want.requested, "asked for");
    }
    if (available.size() == 1) return checked(available.front(), "this machine's kernel");
    if (want.abi == "linux") return checked(available.back(), "its packages are Linux programs");
    if (want.root) return checked(available.back(), "a root is a Linux userland");
    if (want.strong) return checked(available.back(), "its policy needs isolation this kernel cannot give here");
    return checked(available.front(), "this machine's own programs");
}

}  // namespace xlings::carrier
