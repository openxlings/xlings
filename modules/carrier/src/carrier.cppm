export module xlings.carrier;

import std;
import xlings.libs.json;
import xlings.subos.home_view;

// A carrier: where a SubOS runs, and how this xlings reaches it
// (SubOS design part 3 §5).
export namespace xlings::carrier {

namespace fs = std::filesystem;
using xlings::subos::HomeView;

// How to run something where the SubOS is.
struct Endpoint {
    std::string carrier;                    // local | wsl2 | vz
    std::string instance;                   // the guest's name (a WSL distribution); empty: local
    std::vector<std::string> launcher;      // argv prefix that runs a command there; empty: here
    std::string xlings;                     // the xlings to run there, as a path there
};

// Whether this machine can use a carrier, and the route when it cannot.
struct Probe {
    bool supported { false };
    std::string reason;
    std::string route;                      // a command or a step that brings it
    std::string evidence;                   // what was measured (a version line, a probe's output)
};

struct Carrier {
    std::string_view name;
    // Measured, never assumed.
    std::function<Probe(const HomeView&)> probe;
    // The endpoint for this home, created or started when needed.
    std::function<std::expected<Endpoint, std::string>(const HomeView&)> ensure;
    // Stop it (idle); a no-op for local.
    std::function<std::expected<void, std::string>(const Endpoint&)> stop;
    // Make a host directory visible there under `name`; its path there.
    std::function<std::expected<std::string, std::string>(const Endpoint&, const fs::path& host, bool writable,
                                                           std::string_view name)> grant;
};

std::span<const Carrier> carriers();
const Carrier* find(std::string_view name);

// `xlings <args>` where the endpoint is, this terminal attached, `env` added.
// The exit code is the command's (125 when it never started).
int terminal(const Endpoint& at, std::span<const std::string> args,
             const std::map<std::string, std::string>& env = {});

// One call of the NDJSON interface where the endpoint is.
struct Reply {
    int exit_code { 125 };
    std::vector<nlohmann::json> events;     // every line before the result
    nlohmann::json result;                  // the `result` line
    std::string diagnostics;                // stderr, for a failure message
};
std::expected<Reply, std::string> control(const Endpoint& at, std::string_view capability,
                                          const nlohmann::json& args);

// Which carrier a SubOS gets (part 3 §5.6). `requested`: "" or "auto" lets
// this decide; a name is honoured or refused. `abi`: "native" or "linux".
// `root`: it is a root (`--rootfs`). `strong`: its policy requires an
// isolation this machine's own kernel cannot give (a Must).
struct Want {
    std::string requested;
    std::string abi { "native" };
    bool root { false };
    bool strong { false };
};
struct Choice {
    std::string carrier;
    std::string why;                         // for `subos status`
};
struct Refused {
    std::string reason;
    std::string route;
};
std::expected<Choice, Refused> choose(std::string_view platform, const Want& want,
                                      const std::function<Probe(std::string_view)>& probe);

// The carriers a platform has, in preference order (the first is native).
std::vector<std::string_view> carriers_of(std::string_view platform);

// ── a guest's image ──────────────────────────────────────────────────
//
// What a guest carrier imports the first time: the Linux build of this
// release at /xlings/bin/xlings (a static binary: no userland needed to run
// it, and it is the home's package manager there), and the few machine files
// a carrier declares. Built by the xlings core (it knows the index and the
// downloader), declared here (the carrier knows its machine).
struct ImageFile {
    std::string path;            // inside the image, relative ("etc/wsl.conf")
    std::string content;         // a regular file's bytes
    unsigned mode { 0644 };
    std::string link;            // non-empty: a symbolic link to this target
    bool directory { false };
};
// Returns the image tarball (root-owned entries) for `files` plus the Linux
// xlings at xlings/bin/xlings.
using ImageSource = std::function<std::expected<fs::path, std::string>(const HomeView&, std::span<const ImageFile>)>;
void set_image_source(ImageSource source);
const ImageSource& image_source();

}  // namespace xlings::carrier
