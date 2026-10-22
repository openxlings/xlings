module xlings.subos.situation;

import std;
import xlings.platform;

namespace xlings::subos::situation {

namespace {

// Where xlings inside a system reaches its broker (xlings.subos.broker): a
// sandbox's bind mount or a machine's agent at the inside path; a sandbox
// without a view of its own (Landlock) names the host path.
constexpr std::string_view kSocketInside = "/run/xlings/broker.sock";
constexpr std::string_view kSocketEnv = "XLINGS_BROKER_SOCKET";

}  // namespace

std::optional<RootRecord> parse_root_record(std::string_view line) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.remove_suffix(1);
    // A Linux root's path, read the same on every host (a test on Windows).
    if (!line.starts_with('/')) return std::nullopt;
    const auto usr = fs::path(line).lexically_normal();
    if (usr.filename() != "usr" || usr.parent_path().filename() != "root") return std::nullopt;
    const auto instance = usr.parent_path().parent_path();
    if (instance.parent_path().filename() != "subos" || instance.filename().empty()) return std::nullopt;
    return RootRecord{instance.parent_path().parent_path(), instance.filename().string()};
}

std::optional<RootRecord> root_record(const fs::path& etc) {
    if constexpr (!platform::is_linux) return std::nullopt;
    std::ifstream in(etc / "xlings" / "usr");
    std::string line;
    if (!std::getline(in, line)) return std::nullopt;
    return parse_root_record(line);
}

bool in_machine() {
    if constexpr (!platform::is_linux) return false;
    std::ifstream in("/proc/cmdline");
    std::string word;
    while (in >> word)
        if (word.starts_with("luban.home=")) return true;
    return false;
}

Situation from(const Facts& f, const fs::path& home) {
    Situation s;
    s.broker = f.broker;
    if (f.record) {
        s.system_home = f.record->home;
        s.system = f.record->name;
    }
    if (f.mode == "sandbox") {
        // The session acts for the system; its home is read-only unless it is
        // one of the sandbox's own (a test's, mcpp's registry).
        s.place = Place::Sandbox;
        s.who = Who::Admin;
        s.store = f.writable ? Store::Own : Store::Shared;
        if (s.system_home.empty()) s.system_home = home;
        return s;
    }
    if (!f.root_instance && !f.record) return s;   // a host
    s.place = f.machine ? Place::Machine : Place::System;
    s.who = f.root ? Who::Admin : Who::User;
    // The store the system reads: this computer's, written by the host, when
    // it is mounted read-only here (or root cannot write it); else its own.
    s.store = f.read_only || (f.root && !f.writable) ? Store::Shared : Store::Own;
    return s;
}

Situation measure(const fs::path& home) {
    const auto env = platform::environment();
    const auto get = [&](std::string_view k) {
        const auto it = env.find(std::string(k));
        return it == env.end() ? std::string{} : it->second;
    };
    Facts f;
    f.mode = get("XLINGS_SUBOS_MODE");
    f.root_instance = !get("XLINGS_ROOT_INSTANCE").empty();
    f.record = root_record();
    if (f.mode != "sandbox" && !f.root_instance && !f.record) return from(f, home);   // a host: nothing more to ask
    f.machine = f.mode != "sandbox" && in_machine();
    f.root = platform::is_root();
    std::error_code ec;
    // Written as the lock is: a home this process cannot add to is not its
    // to write (a Landlock fence says so only when tried).
    f.writable = !fs::exists(home, ec) || static_cast<bool>(std::ofstream(home / ".xlings.lock", std::ios::app));
    f.read_only = fs::exists(home, ec) && platform::read_only_mount(home).value_or(false);
    if constexpr (platform::is_linux) {
        // A sandbox names its socket or has it inside; a machine's agent has
        // it inside, root's alone -- and `sudo` clears XLINGS_*, so a machine
        // is known by its kernel, not by a variable.
        auto socket = get(kSocketEnv);
        if (socket.empty()) socket = std::string(kSocketInside);
        if (f.mode == "sandbox" || f.machine) f.broker = fs::exists(socket, ec);
    }
    return from(f, home);
}

Way route(const Situation& s, Touch t) {
    if (t == Touch::Read || s.place == Place::Host) return Way::Here;
    if (s.who == Who::User) {
        // A user's view: their own packages, versions and settings are theirs
        // (a payload the store lacks is added through the administrator, by
        // the install itself); the index is every user's.
        return t == Touch::Index ? Way::Admin : Way::Here;
    }
    if (s.store == Store::Own) return Way::Here;
    if (t == Touch::Home) return Way::Owner;
    return s.broker ? Way::Broker : Way::NoBroker;
}

std::string_view to_string(Place p) {
    switch (p) {
        case Place::Host: return "host";
        case Place::Sandbox: return "sandbox";
        case Place::Machine: return "machine";
        case Place::System: return "system";
    }
    return "host";
}

std::string_view to_string(Who w) {
    switch (w) {
        case Who::Owner: return "owner";
        case Who::Admin: return "admin";
        case Who::User: return "user";
    }
    return "owner";
}

std::string_view to_string(Store s) {
    switch (s) {
        case Store::Personal: return "personal";
        case Store::Shared: return "shared";
        case Store::Own: return "own";
    }
    return "personal";
}

}  // namespace xlings::subos::situation
