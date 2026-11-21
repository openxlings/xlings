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

// Inside a carrier's machine (`xlings __carrier-probe`, run there by the
// host): its userland brought to this release's revision (bring_userland),
// then what this machine gives a Luban system, as one JSON line --
// {"kvm": bool, "kernel": "<release>", "version": "<this xlings>",
// "userland": <revision>}. The host keeps it as the carrier's probe.json;
// `luban status` reads it (machine mode needs the KVM).
int guest_probe();

// The guest's userland -- what its image holds besides xlings -- by revision
// (design 2026-11-21-issue-724 §4.4), each step idempotent, the revision
// reached recorded in the guest home's state/carrier-userland.json:
//   1  busybox's applets linked where a Linux has them
//   2  the standard loader directory linked to the home's runtime
//      (/lib64 -> /xlings/abi/linux-x86_64-glibc) when it does not exist
// `root` is the machine's root, `home` its xlings home in it (/xlings). The
// revision reached.
inline constexpr int kUserlandRevision = 2;
std::expected<int, std::string> bring_userland(const fs::path& root, const fs::path& home, std::string_view arch);

// The release this client is (xlings and luban are released together): a
// guest machine reports its own in guest_probe.
void set_client_version(std::string version);
const std::string& client_version();

// The Linux build of this release the host holds for its guest: a directory
// with `xlings` (and `luban`), and the release it is -- the client's, or the
// latest published when the client's is not (a candidate, a dev build).
struct GuestBuild {
    fs::path dir;
    std::string version;
};
using GuestBuildSource = std::function<std::expected<GuestBuild, std::string>(const HomeView&)>;
void set_guest_build_source(GuestBuildSource source);

// Make a host directory visible in the guest under /grant/<name> (Carrier::grant).
using Grant = std::function<std::expected<std::string, std::string>(const Endpoint&, const fs::path& host, bool writable,
                                                                    std::string_view name)>;

// Before an endpoint is handed out: what the guest gives, asked again when
// `probe_file` is ten minutes old (or missing), and the guest brought to the
// host's Linux build when it reports another release -- the build granted
// read-only and adopted there (guest_adopt: its files replaced by rename),
// no network in the guest. A guest newer than the client is left as it is.
// A failure is the call's error, recorded in probe.json for luban status and
// tried again on the next call (never "tried once"). Kept in `probe_file`.
std::expected<void, std::string> bring_guest(const HomeView& home, const Endpoint& at, const fs::path& probe_file,
                                             const Grant& grant);

// `xlings __carrier-adopt <bin-dir>`, run in the guest by the build the host
// granted it: this program, and the luban beside it, put at <bin-dir>/xlings
// and <bin-dir>/luban, each copied beside its place and renamed over it (a
// process running the old file keeps it until it ends).
int guest_adopt(std::span<const std::string> args);

// Whether release `a` is older than `b` (YYYY.M.D.N, numerically).
bool older_release(std::string_view a, std::string_view b);

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

// One call of the NDJSON interface where the endpoint is, each line of its
// stdout handed to `line` as it comes (design 2026-11-21-issue-724 §4.2): the
// caller decodes them -- this module knows lines, not events. `env` is carried
// as `terminal` carries it; its stderr is kept in `diagnostics`. Ends when this
// process is asked to stop (the child is ended: ending the launcher ends what
// it started there). The exit code is the command's (125 when it never started).
int relay(const Endpoint& at, std::string_view capability, const nlohmann::json& args,
          const std::map<std::string, std::string>& env, const std::function<void(std::string_view)>& line,
          std::string* diagnostics = nullptr);

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
    bool kernel { false };       // the Linux machine kernel of this architecture (xim:linux-kernel-virt), here
};
// Returns the image tarball (root-owned entries) for `files` plus the Linux
// xlings at xlings/bin/xlings.
using ImageSource = std::function<std::expected<fs::path, std::string>(const HomeView&, std::span<const ImageFile>)>;
void set_image_source(ImageSource source);
const ImageSource& image_source();

}  // namespace xlings::carrier
