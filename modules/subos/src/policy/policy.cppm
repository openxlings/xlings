export module xlings.subos.policy;

import std;
import xlings.libs.json;

// The policy model (design §7): what an instance is allowed, as data.
//
// Four layers produce one Policy -- a preset (dev / private / locked), the
// overrides a user sets, ordered rules, and policy packages -- and the
// compiler (xlings.subos.spec) turns it, with the host's capabilities, into
// the sandbox that actually runs. This module holds the TYPES and the values
// every layer starts from; reading policy.json, merging and decide() build on
// them.
export namespace xlings::subos::policy {

enum class Preset {
    Legacy,    // an instance nobody declared a policy for: today's behaviour
               // plus the S0 fixes (design §25 "无感升级")
    Dev,       // host files and other instances invisible, xlings tamper-proof
    Private,   // + network, desktop and identity isolation
    Locked,    // + no network, mounts read-only by default
};

enum class Net { Host, Nat, None, Proxy };
enum class Fetch { Auto, Ask, Layer, Deny };
enum class Observe { Off, Basic, Standard, Full };
enum class Identity { Host, Neutral };

// How hard a requirement is. A missing `Must` refuses entry (exit 125); a
// missing `Should` degrades and says so -- unless the call asked for
// --no-degrade, which turns every Should into a Must.
enum class Need { Must, Should };

struct Mount {
    std::string src;     // host path, `~` expanded by the caller
    std::string dst;     // path inside; empty = same as src
    bool rw { true };
    bool mode_given { false };   // `:ro` / `:rw` was written; otherwise the policy's default
};

// `--mount <host>[:<inside>][:ro|rw]` (design §11, docker -v): a second segment
// that is exactly `ro` or `rw` is the mode (`~/.gitconfig:ro`). `home` expands
// a leading `~`; a relative host path is taken from `cwd`.
std::expected<Mount, std::string> parse_mount(std::string_view spec, std::string_view home,
                                              std::string_view cwd);

// Named grants (design §21.2): each opens exactly one capability.
inline constexpr std::array<std::string_view, 7> kGrants{
    "display", "audio", "camera", "gpu", "ssh-agent", "dbus", "host-loopback"};

// First client that implements this policy schema; new files carry this floor.
inline constexpr std::string_view kPolicyMinClient = "2026.10.10.1";

struct Policy {
    Preset preset { Preset::Legacy };
    Net net { Net::Host };
    std::string proxy;                       // for Net::Proxy
    Fetch fetch { Fetch::Auto };
    Fetch index_update { Fetch::Auto };
    Observe observe { Observe::Basic };
    Identity identity { Identity::Host };
    // Neutral identity's time zone: an IANA name, "UTC", or "proxy" -- the
    // proxy's exit, asked through it (Luban design §C4). Empty: "proxy" when
    // the network is a proxy, UTC otherwise.
    std::string tz;
    std::string geo_lookup;                  // where "proxy" asks; empty: https://ipinfo.io/timezone

    // What isolation each dimension needs; dimensions not listed are Should.
    std::map<std::string, Need, std::less<>> needs;

    // Environment variables that may enter, by name or `PREFIX*`.
    std::vector<std::string> env_pass;
    bool env_inherit { false };              // Legacy: pass everything (pre-#640)
    // Whether `--env K=V` may name any variable, or only env_pass ones.
    bool env_explicit_any { true };

    std::vector<Mount> mounts;
    std::set<std::string, std::less<>> grants;          // granted now
    std::set<std::string, std::less<>> grants_allowed;  // may be granted per call

    bool disable_userns { false };           // forbid nested user namespaces
    bool no_degrade { false };
    bool mounts_ro_default { false };        // locked: --mount defaults to ro
    // Ordered fetch rules (design §7.3): the first that matches decides.
    struct Rule {
        std::string match { "*" };           // package glob, e.g. "xim:*"
        std::string index;                   // only packages from this index
        std::uint64_t size_gt { 0 };         // only packages larger than this
        Fetch action { Fetch::Ask };
    };
    std::vector<Rule> fetch_rules;
    std::string extends;                     // what the file said it extends
    // A policy package (design §7.3) the owner selected: the payload's file
    // the instance's copy was made from, locked by sha256. The instance file
    // carries the whole policy, so a package update changes nothing until
    // the owner asks (`subos config <s> --policy-upgrade`).
    struct Package {
        std::string from;                    // ns:name@version, as resolved
        std::string sha256;                  // of the payload's policy.json
    };
    std::optional<Package> package;
    std::string min_client;                  // empty only in legacy policy files
};

// `ns:name[@version]`: a policy package, where a preset is one word.
bool is_package_ref(std::string_view extends);

// A policy package's own file (the payload's policy.json) as the policy an
// instance selecting it gets. A package extends a built-in preset or
// nothing; one extending another package is refused.
std::expected<Policy, std::string> from_package(const nlohmann::json& doc, std::string_view ref,
                                                Policy::Package resolved);

std::string_view to_string(Preset p);
std::string_view to_string(Net n);
std::string_view to_string(Fetch f);
std::string_view to_string(Observe o);
std::optional<Preset> preset_from_string(std::string_view s);
std::optional<Net> net_from_string(std::string_view s);
std::optional<Fetch> fetch_from_string(std::string_view s);
std::optional<Observe> observe_from_string(std::string_view s);

// The environment every sandbox starts from, whatever the policy says: the
// variables a shell and the toolchains need to work and that name nothing
// about the host. Everything else must be passed by name.
inline constexpr std::array<std::string_view, 9> kBaseEnvPass{
    "TERM", "COLORTERM", "LANG", "LC_*", "TZ", "NO_COLOR", "XLINGS_AGENT_MODE",
    "XLINGS_NON_INTERACTIVE", "XLINGS_TRACE"};

// What dev (and an undeclared instance) passes on top of the base: the
// variables people rely on for a working toolchain and network that name no
// credential. A secret, a socket into the desktop or an agent, and the X
// authority are exactly what is NOT here (#640 F3) -- they enter only when
// named in env_pass or through a grant.
inline constexpr std::array<std::string_view, 17> kDevEnvPass{
    "http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY", "no_proxy", "NO_PROXY",
    "all_proxy", "ALL_PROXY", "EDITOR", "VISUAL", "PAGER", "COLUMNS", "LINES",
    "SSL_CERT_FILE", "SSL_CERT_DIR", "XLINGS_RELEASE_MIRROR", "XLINGS_MIRROR"};

// The policy of an instance nobody declared one for: what it had before, plus
// the S0 fixes every sandbox gets (environment allow-list, pid / ipc / uts
// namespaces, no terminal injection).
Policy legacy();

// Shell-style wildcard match: `*` and `?`.
bool glob_match(std::string_view pattern, std::string_view text);

// "2GB", "512MB", "100KB", "123" -> bytes; nullopt otherwise.
std::optional<std::uint64_t> parse_size(std::string_view text);

// Whether `name` matches an env_pass entry (exact, or `PREFIX*`).
bool env_name_matches(std::string_view name, std::span<const std::string> patterns);

// ── Presets (design §10) ─────────────────────────────────────────────
//
//   dev      host files and other instances invisible, xlings tamper-proof,
//            network as usual; what this host cannot do is reported, not fatal
//   private  + network (nat), desktop and identity isolation; the isolation
//            it promises is required -- missing, it does not enter
//   locked   + no network, mounts read-only by default, fetch denied, every
//            execution observed
Policy preset(Preset p);

// ── The policy file (design §7.2) ────────────────────────────────────
//
// <home>/config/subos/<name>/policy.json -- outside the instance, read-only
// inside it. Parsing FAILS CLOSED: a field this version does not know, or a
// value it does not know (`"net": "vpn"`), is an error and the instance is
// not entered -- never ignored, because ignoring a stricter setting is
// loosening it. Writers keep keys they do not know (design §25); executors
// refuse them. Keys starting with "x-" and "comment" are free text.
std::expected<Policy, std::string> from_json(const nlohmann::json& doc);
nlohmann::json to_json(const Policy& p);

// Numeric three-part semver (including prerelease/build) or four-part date versions.
// Invalid input is refused, never compared lexicographically.
std::expected<int, std::string> compare_client_versions(std::string_view lhs, std::string_view rhs);
std::expected<void, std::string> check_client(const Policy& p, std::string_view version);

// ── Per-call overrides (design §7.3) ─────────────────────────────────
//
// A single call may only TIGHTEN what the policy allows, or grant from
// `grants_allowed`. Anything else is refused with the reason.
struct Overrides {
    std::optional<Net> net;
    std::optional<std::string> proxy;
    std::optional<Fetch> fetch;
    std::optional<Observe> observe;
    std::set<std::string, std::less<>> allow;    // --allow <grant>
    std::vector<Mount> mounts;                   // --mount
    bool no_degrade { false };
};

std::expected<Policy, std::string> apply(Policy p, const Overrides& o);

// A value this version parses but does not enforce yet, named; nullopt when
// there is none (fail closed, design §7.3).
std::optional<std::string> not_enforced(const Policy& p);

// `fetch = layer` (design part 2 §3.3): what the xlings inside installs goes
// into the root's own system scope -- which only a rootfs SubOS has. A view
// of the host has no layer of its own, and granting it there would install
// into the shared home instead: such an instance refuses entry.
bool fetches_into_layer(const Policy& p);

// Field-by-field differences, for the audit of a policy change.
std::vector<std::string> diff(const Policy& before, const Policy& after);

// ── The one decision (design §9) ─────────────────────────────────────

enum class Action { Allow, Ask, Deny };
std::string_view to_string(Action a);

struct Op {
    // fetch | index_update | grant | policy_change | instance_admin
    std::string kind;
    std::string target;          // a package spec, a grant name
    std::string index;           // for fetch: the index it comes from
    std::uint64_t size { 0 };    // for fetch: bytes, when known
    bool from_inside { false };  // asked by the xlings inside the sandbox
    std::string instance;        // for the owner command in a refusal
};

struct Decision {
    Action action { Action::Deny };
    std::string reason;
    // The command a person outside runs to do it anyway, when there is one.
    std::string owner_command;
};

// The client inside (for an early, friendly answer) and the broker outside
// (for the answer that counts) call THIS, with the same policy file.
Decision decide(const Policy& p, const Op& op);

}  // namespace xlings::subos::policy
