export module xlings.subos.home_view;

import std;

// Where a SubOS's things live in a home (design §6). The one answer to "which
// path", handed to the SubOS core by xlings core's HomeContext -- the SubOS
// core never reads Config.
//
// The layout separates each instance from what governs and records it:
//
//   <home>/subos/<n>/            the instance (the sandbox writes here)
//   <home>/config/subos/<n>/     its policy      -- read-only inside, owner-written
//   <home>/logs/subos/<n>/       its audit       -- invisible inside, supervisor-written
//   <home>/run/subos/<n>/        its sockets     -- invisible inside
//   <home>/state/                host facts      -- the capability cache
//
// A sandbox that could rewrite its own policy or audit would make both
// decorative; keeping them outside the instance is what makes the read-only
// mount topology enough to protect them.
export namespace xlings::subos {

namespace fs = std::filesystem;

struct HomeView {
    fs::path home;

    fs::path subos_root() const { return home / "subos"; }
    fs::path instance(std::string_view name) const { return subos_root() / name; }
    fs::path config_dir(std::string_view name) const { return home / "config" / "subos" / name; }
    fs::path policy_file(std::string_view name) const { return config_dir(name) / "policy.json"; }
    fs::path logs_dir(std::string_view name) const { return home / "logs" / "subos" / name; }
    fs::path run_dir(std::string_view name) const { return home / "run" / "subos" / name; }
    fs::path state_dir() const { return home / "state"; }
    fs::path caps_cache() const { return state_dir() / "isolation-caps.json"; }
    // Requests waiting for the owner's approval (design §8.2), hidden inside.
    fs::path requests_dir(std::string_view name) const { return state_dir() / "subos" / name / "requests"; }
    fs::path broker_socket(std::string_view name) const { return run_dir(name) / "broker.sock"; }
};

}  // namespace xlings::subos
