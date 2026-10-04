export module xlings.subos.policy;

import std;

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
};

// Named grants (design §21.2): each opens exactly one capability.
inline constexpr std::array<std::string_view, 7> kGrants{
    "display", "audio", "camera", "gpu", "ssh-agent", "dbus", "host-loopback"};

struct Policy {
    Preset preset { Preset::Legacy };
    Net net { Net::Host };
    std::string proxy;                       // for Net::Proxy
    Fetch fetch { Fetch::Auto };
    Fetch index_update { Fetch::Auto };
    Observe observe { Observe::Basic };
    Identity identity { Identity::Host };
    std::string tz;                          // neutral identity: default "UTC"

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
};

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

// Whether `name` matches an env_pass entry (exact, or `PREFIX*`).
bool env_name_matches(std::string_view name, std::span<const std::string> patterns);

}  // namespace xlings::subos::policy
