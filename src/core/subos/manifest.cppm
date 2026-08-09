export module xlings.core.subos.manifest;

import std;

import xlings.libs.json;
import xlings.platform;

// `subos_info` — what a subos is, recorded in the subos's own `.xlings.json`.
//
// A subos already had a directory, a bin/, an xvm scope and a registry entry.
// What it did not have was a statement of *what it is*: which runtime its
// binaries were built against, and which environment its processes need. Both
// were implicit in whatever happened to be installed, which is why a subos
// could not be described, checked, or reproduced.
//
// Layer this closes: a program needs bootstrap (PT_INTERP + CRT + libc),
// discovery (PATH + RPATH), and configuration (env vars). xlings had the first
// two — glibc + elfpatch, xvm + shims — and nothing for the third. That is the
// gap behind mcpp-community/mcpp#352: a GLFW binary that links fine and exits
// 255 because no one told it where the GL drivers are.
//
// Placement is the per-subos `.xlings.json`, not a new file and not the home
// or project one. Those two already use the key `subos` for other things (a
// selector string at project level, a registry map at home level), and a subos
// describing itself belongs in the subos.
//
// This module is deliberately free of Config/xvm imports: it takes paths and a
// binding→dir resolver from the caller. That keeps invariant checking and
// placeholder expansion testable without a home on disk, and it is what lets
// the doctor reporter and the repairer share one predicate instead of
// describing the rules twice and drifting apart.

export namespace xlings::subos::manifest {

namespace fs = std::filesystem;

inline constexpr int             SCHEMA_VERSION  = 1;
inline constexpr std::string_view BLOCK          = "subos_info";
// The runtime a subos gets when the caller names none. A constant rather than
// a lookup: slice 1 ships one runtime, and inventing a "pick the newest libc
// present" rule would make two homes with the same command produce different
// subos.
//
// 2.44 since 2026.8.9.1 (C1 of the closure contract). The 2.39 floor was a
// systematic error, not a conservative default: `*-host-link` packages carry
// host-built libraries into our closure permanently, those need the host
// glibc's symbol versions, so `our_glibc >= host_glibc` is a standing
// constraint -- and 2.39 loses it on every distro newer than Ubuntu 24.04
// (mcpp-community/mcpp#392 is that failure verbatim). Backward compatibility
// makes the move safe: every 2.39-built payload runs under 2.44.
//
// Scope: NEW subos only. The binding is a creation-time property persisted in
// each subos's own manifest, and rebuild paths preserve a valid recorded
// binding (see preserved_runtime), so no existing subos changes runtime by
// this constant moving.
inline constexpr std::string_view DEFAULT_RUNTIME = "glibc@2.44";

inline constexpr std::string_view OP_SET     = "set";
inline constexpr std::string_view OP_PREPEND = "prepend";

// ── data ────────────────────────────────────────────────────────────────

// One variable a package asks its subos to export.
struct EnvDecl {
    std::string var;
    std::string op;     // OP_SET | OP_PREPEND
    std::string value;  // may contain ${...} placeholders
};

// A provider's whole section. Keyed by binding so uninstalling the package
// removes exactly what it added — the same provider-scoped ownership xvm.add
// and xvm.files already use.
struct Provider {
    std::string          binding;   // "<name>@<version>"
    std::vector<EnvDecl> decls;
};

struct Info {
    int                   schema_version = 0;
    std::string           runtime;
    std::vector<Provider> envs;      // sorted by binding; see resolve()
    std::string           created_at;
    std::string           created_by;
    // Host glibc version ("2.39") probed when the block was written. Empty =
    // unknown: pre-C1 manifests, non-glibc hosts, failed probe. Rule A's
    // right-hand side; a reader must treat unknown as unprovable, not as 0.
    std::string           host_glibc;
};

// ── runtime family ──────────────────────────────────────────────────────

// The runtime string is self-describing: "glibc@2.39" says Linux/glibc without
// a second field to disagree with it. Families are derived here rather than
// stored, so a manifest cannot claim a family its runtime contradicts.
//
// Slice 1 needs only the glibc row. The rest are listed to make the shape of
// the mapping explicit — a new OS adds a row, not a schema field.
std::string family_of(std::string_view runtime, std::string_view arch = "x86_64") {
    const auto at   = runtime.find('@');
    const auto name = runtime.substr(0, at == std::string_view::npos
                                        ? runtime.size() : at);
    if (name == "glibc")     return std::format("linux-{}-glibc", arch);
    if (name == "musl")      return std::format("linux-{}-musl", arch);
    if (name == "wasi-libc") return "wasm32-wasi";
    if (name == "macos_sdk") return std::format("darwin-{}", arch);
    if (name == "ucrt")      return std::format("windows-{}-ucrt", arch);
    return "unknown";
}

// "<name>@<version>", both halves non-empty. Used for `runtime` and for every
// envs key.
bool is_binding(std::string_view s) {
    const auto at = s.find('@');
    return at != std::string_view::npos && at > 0 && at + 1 < s.size();
}

std::string_view binding_name(std::string_view binding) {
    const auto at = binding.find('@');
    return at == std::string_view::npos ? binding : binding.substr(0, at);
}

std::string_view binding_version(std::string_view binding) {
    const auto at = binding.find('@');
    return at == std::string_view::npos ? std::string_view{}
                                        : binding.substr(at + 1);
}

// ── the subos layer's "exactly one" ─────────────────────────────────────
//
// One package, one version, per subos. The three-layer model has always said
// so -- the xpkg store holds many versions by design, each consumer freezes one
// into its own RPATH/INTERP, and the subos sysroot in between is the layer that
// is supposed to hold exactly one -- but nothing anywhere enforced it.
//
// Measured on a real home: `xlings list` showed mesa@25.0.7 and mesa@25.0.7.1
// both bound in `default`, both contributing to __EGL_VENDOR_LIBRARY_DIRS, and
// EGL duly enumerated the device twice. `xlings self doctor` said nothing.
//
// This is P1 wearing a different hat. "What is in this subos" has two records
// -- the xvm registration and this manifest's envs section -- and installing a
// second version appends to both. They agree, which is why nothing complained;
// they agree on an answer the model forbids.
//
// ONE function, used by the report and by --fix. Every previous report/repair
// pair in this repo has drifted, and the shape it takes is a finding that
// repairing does not clear, so the predicate lives here rather than in doctor.
struct DuplicateBinding {
    std::string              name;
    std::vector<std::string> bindings;   // every binding for that name, sorted
};

std::vector<DuplicateBinding> duplicate_bindings(const Info& info) {
    std::map<std::string, std::vector<std::string>> byName;
    for (const auto& p : info.envs) {
        if (!is_binding(p.binding)) continue;
        byName[std::string(binding_name(p.binding))].push_back(p.binding);
    }
    std::vector<DuplicateBinding> out;
    for (auto& [name, bindings] : byName) {
        if (bindings.size() < 2) continue;
        std::ranges::sort(bindings);
        bindings.erase(std::ranges::unique(bindings).begin(), bindings.end());
        if (bindings.size() < 2) continue;   // one version listed twice is not two
        out.push_back({.name = name, .bindings = std::move(bindings)});
    }
    return out;
}

// Which version of each package THIS subos has active.
//
// Read from the same file as the declarations. `subos/<name>/.xlings.json`
// holds both `workspace` (name -> active/installed) and `subos_info.envs`
// (binding -> declarations) -- two records of "what is in this subos", in one
// file. That is the whole of P1 in eleven lines of JSON.
std::map<std::string, std::string> active_versions(const nlohmann::json& doc) {
    std::map<std::string, std::string> out;
    if (!doc.contains("workspace") || !doc["workspace"].is_object()) return out;
    for (auto it = doc["workspace"].begin(); it != doc["workspace"].end(); ++it) {
        if (!it.value().is_object()) continue;
        auto active = it.value().value("active", std::string{});
        if (!active.empty()) out[it.key()] = std::move(active);
    }
    return out;
}

// True when `version` is the one `active` names, allowing for the `<ns>:<ver>`
// form a namespaced install records.
inline bool version_is_active(std::string_view version, std::string_view active) {
    if (version == active) return true;
    const auto colon = active.find(':');
    return colon != std::string_view::npos && active.substr(colon + 1) == version;
}

// The manifest and XVM workspace are two records of the same SubOS runtime.
// The global payload store may contain any number of versions; this predicate
// only asks whether THIS SubOS has exactly the version it declares active and
// whether that declaration still resolves to a payload on disk.
struct RuntimeActivationMismatch {
    std::string declared;
    std::string active;
    bool payloadMissing = false;
};

std::optional<RuntimeActivationMismatch>
check_runtime_activation(const Info& info,
                         std::string_view activeVersion,
                         bool payloadExists) {
    if (!is_binding(info.runtime)) return std::nullopt;

    const auto declaredVersion = binding_version(info.runtime);
    if (version_is_active(declaredVersion, activeVersion) && payloadExists) {
        return std::nullopt;
    }

    std::string active;
    if (!activeVersion.empty()) {
        auto displayVersion = activeVersion;
        if (const auto colon = displayVersion.find(':');
            colon != std::string_view::npos) {
            displayVersion.remove_prefix(colon + 1);
        }
        active = std::format("{}@{}", binding_name(info.runtime),
                             displayVersion);
    }
    return RuntimeActivationMismatch{
        .declared = info.runtime,
        .active = std::move(active),
        .payloadMissing = !payloadExists,
    };
}

// The providers that actually take effect, out of everything the manifest
// records.
//
// A package installed at two versions keeps BOTH provider sections -- the
// dormant one has to survive so that `xlings use pkg@<older>` restores its
// environment without reinstalling. What must not happen is both contributing
// at once, which is how one GPU came to be enumerated as two.
//
// Which one is live is not a decision made here: xvm already made it, and its
// answer is in this same file. Re-deriving it (highest version? last
// installed?) would be a second answerer to a question that has one -- the
// exact defect this function exists to remove.
//
// A package with NO active version keeps every provider, and that default
// matters more than it looks: filtering on a record that turns out to be absent
// would silently delete a package's whole environment, which is the failure
// mode this file exists to prevent, arrived at from the other side. Measured
// before choosing it -- a bare `xvm.add(name)` does record an active version,
// so this is the salvage path for a manifest whose workspace record was lost
// (pruned, copied between homes, hand-edited), not the common case. Where
// several versions are declared with no active one, nothing can say which is
// meant; that state is reported rather than guessed at.
Info select_effective(const Info& info,
                      const std::map<std::string, std::string>& active) {
    Info out = info;
    out.envs.clear();
    for (const auto& p : info.envs) {
        if (is_binding(p.binding)) {
            const auto it = active.find(std::string(binding_name(p.binding)));
            if (it != active.end()
                && !version_is_active(binding_version(p.binding), it->second)) {
                continue;
            }
        }
        out.envs.push_back(p);
    }
    return out;
}

// A package bound at several versions with nothing able to say which is meant.
//
// NOT the same as duplicate_bindings: two versions where one is active is
// ordinary -- the dormant declarations are how `xlings use` can switch back.
// This is the subset that has no active version -- every one of them
// contributes, so the subos exports each variable several times over. Reaching
// it takes a lost workspace record, not an ordinary install.
std::vector<DuplicateBinding>
contested_bindings(const Info& info,
                   const std::map<std::string, std::string>& active) {
    std::vector<DuplicateBinding> out;
    for (auto& dup : duplicate_bindings(info)) {
        if (active.contains(dup.name)) continue;
        out.push_back(std::move(dup));
    }
    return out;
}

// ── invariants ──────────────────────────────────────────────────────────

enum class Defect {
    DirMissing,          // I1
    ConfigMissing,       // I2
    ConfigUnreadable,    // I2
    BlockMissing,        // I4
    SchemaUnsupported,   // I5
    RuntimeMalformed,    // I6
    EnvsMalformed,       // I7
    EnvDeclMalformed,    // I7
    ProvenanceMissing,   // I8
};

struct Finding {
    Defect      kind;
    std::string detail;
};

std::string_view describe(Defect d) {
    switch (d) {
        case Defect::DirMissing:        return "subos directory is missing";
        case Defect::ConfigMissing:     return "subos has no .xlings.json";
        case Defect::ConfigUnreadable:  return ".xlings.json is not readable JSON";
        case Defect::BlockMissing:      return "no subos_info block";
        case Defect::SchemaUnsupported: return "unsupported subos_info schema_version";
        case Defect::RuntimeMalformed:  return "runtime is not <name>@<version>";
        case Defect::EnvsMalformed:     return "envs is not an object of provider sections";
        case Defect::EnvDeclMalformed:  return "env declaration is malformed";
        case Defect::ProvenanceMissing: return "created_at / created_by missing";
    }
    return "unknown defect";
}

fs::path config_path(const fs::path& subosDir) {
    return subosDir / ".xlings.json";
}

// Read the whole document. An unreadable or malformed file yields nullopt, and
// the caller must not paper over it with an empty object: for the home config
// "corrupt means absent" is the historical behavior, but here it would let
// `--fix` rewrite a file it never managed to read.
std::optional<nlohmann::json> read_document(const fs::path& subosDir) {
    const auto path = config_path(subosDir);
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return std::nullopt;
    try {
        auto content = platform::read_file_to_string(path.string());
        auto parsed  = nlohmann::json::parse(content, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
        return parsed;
    } catch (...) { return std::nullopt; }
}

// I4–I8, over a document already in hand. Split out from validate() so a
// caller that has just built a document in memory can check it before writing.
std::vector<Finding> validate_block(const nlohmann::json& doc) {
    std::vector<Finding> out;

    if (!doc.contains(std::string(BLOCK)) || !doc[std::string(BLOCK)].is_object()) {
        out.push_back({Defect::BlockMissing, std::string(BLOCK)});
        return out;   // nothing below can be checked
    }
    const auto& b = doc[std::string(BLOCK)];

    if (!b.contains("schema_version") || !b["schema_version"].is_number_integer()
        || b["schema_version"].get<int>() != SCHEMA_VERSION) {
        out.push_back({Defect::SchemaUnsupported,
                       b.contains("schema_version") ? b["schema_version"].dump()
                                                    : "absent"});
    }

    const auto runtime = b.value("runtime", std::string{});
    if (!is_binding(runtime)) {
        out.push_back({Defect::RuntimeMalformed,
                       runtime.empty() ? "absent" : runtime});
    }

    // "An empty collection is not a missing one": `envs` is written as {} at
    // creation and stays {} when the last provider is removed, so no reader
    // has to treat absent and empty as the same thing.
    if (!b.contains("envs") || !b["envs"].is_object()) {
        out.push_back({Defect::EnvsMalformed,
                       b.contains("envs") ? b["envs"].type_name() : "absent"});
    } else {
        for (auto it = b["envs"].begin(); it != b["envs"].end(); ++it) {
            if (!is_binding(it.key())) {
                out.push_back({Defect::EnvsMalformed,
                               std::format("provider key '{}' is not <name>@<version>",
                                           it.key())});
                continue;
            }
            if (!it.value().is_array()) {
                out.push_back({Defect::EnvsMalformed,
                               std::format("provider '{}' is not an array", it.key())});
                continue;
            }
            for (const auto& d : it.value()) {
                const auto var = d.is_object() ? d.value("var", std::string{})
                                               : std::string{};
                const auto op  = d.is_object() ? d.value("op", std::string{})
                                               : std::string{};
                if (var.empty() || (op != OP_SET && op != OP_PREPEND)) {
                    out.push_back({Defect::EnvDeclMalformed,
                                   std::format("{}: {}", it.key(), d.dump())});
                }
            }
        }
    }

    if (b.value("created_at", std::string{}).empty()
        || b.value("created_by", std::string{}).empty()) {
        out.push_back({Defect::ProvenanceMissing, ""});
    }
    return out;
}

// I1–I8 for a subos on disk. The single predicate the doctor reporter and the
// repairer both call — two descriptions of the same rule is how a reporter
// ends up flagging what `--fix` will not touch.
std::vector<Finding> validate(const fs::path& subosDir) {
    std::error_code ec;
    if (!fs::is_directory(subosDir, ec) || ec)
        return {{Defect::DirMissing, subosDir.string()}};
    if (!fs::exists(config_path(subosDir), ec) || ec)
        return {{Defect::ConfigMissing, config_path(subosDir).string()}};

    auto doc = read_document(subosDir);
    if (!doc) return {{Defect::ConfigUnreadable, config_path(subosDir).string()}};
    return validate_block(*doc);
}

// ── parse / build ───────────────────────────────────────────────────────

// Providers come back sorted by binding. That ordering is the resolution order
// (see resolve) and it is derived from the data rather than from install
// history, so two homes holding the same manifest resolve it identically.
Info parse(const nlohmann::json& doc) {
    Info info;
    if (!doc.contains(std::string(BLOCK)) || !doc[std::string(BLOCK)].is_object())
        return info;
    const auto& b = doc[std::string(BLOCK)];

    info.schema_version = b.value("schema_version", 0);
    info.runtime        = b.value("runtime", std::string{});
    info.created_at     = b.value("created_at", std::string{});
    info.created_by     = b.value("created_by", std::string{});
    info.host_glibc     = b.value("host_glibc", std::string{});

    if (b.contains("envs") && b["envs"].is_object()) {
        for (auto it = b["envs"].begin(); it != b["envs"].end(); ++it) {
            if (!it.value().is_array()) continue;
            Provider p{.binding = it.key()};
            for (const auto& d : it.value()) {
                if (!d.is_object()) continue;
                EnvDecl e{
                    .var   = d.value("var", std::string{}),
                    .op    = d.value("op", std::string{}),
                    .value = d.value("value", std::string{}),
                };
                if (e.var.empty()) continue;
                if (e.op != OP_SET && e.op != OP_PREPEND) continue;
                p.decls.push_back(std::move(e));
            }
            info.envs.push_back(std::move(p));
        }
    }
    std::ranges::sort(info.envs, {}, &Provider::binding);
    return info;
}

std::string utc_now_iso() {
    const auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    return buf;
}

// The block a freshly created subos gets. `envs` is an explicit empty object.
// `hostGlibc` is written only when known — an absent key is the documented
// spelling of "unknown", so a pre-C1 manifest and a failed probe read the
// same way.
nlohmann::json make_block(std::string_view runtime, std::string_view createdBy,
                          std::string_view hostGlibc = {}) {
    nlohmann::json b;
    b["schema_version"] = SCHEMA_VERSION;
    b["runtime"]        = std::string(runtime);
    b["envs"]           = nlohmann::json::object();
    b["created_at"]     = utc_now_iso();
    b["created_by"]     = std::string(createdBy);
    if (!hostGlibc.empty()) b["host_glibc"] = std::string(hostGlibc);
    return b;
}

// What a subos is OBSERVED to run, out of the workspace in its own manifest.
//
// A subos that predates `subos_info` recorded no binding, but it is not silent
// about its runtime: its workspace names an active version for the runtime
// family. Reading it is not a guess and not a scan -- it is the same file, one
// key over, and it is the record `use` itself maintains.
//
// The family comes from the caller's fallback rather than a literal, so this
// says nothing about which family a platform uses.
std::string observed_runtime(const nlohmann::json& doc,
                             std::string_view family) {
    if (family.empty()) return {};
    if (!doc.contains("workspace") || !doc["workspace"].is_object()) return {};
    const auto& ws = doc["workspace"];
    const std::string key(family);
    if (!ws.contains(key) || !ws[key].is_object()) return {};
    auto active = ws[key].value("active", std::string{});
    // Workspace versions may carry the provider namespace (`xim:2.39`); the
    // binding never does.
    if (const auto colon = active.find(':'); colon != std::string::npos) {
        active = active.substr(colon + 1);
    }
    if (active.empty()) return {};
    const auto binding = std::format("{}@{}", family, active);
    return is_binding(binding) ? binding : std::string{};
}

// The runtime a REBUILT block should carry. A block can be invalid for
// reasons that have nothing to do with its runtime (schema_version, envs,
// provenance), and every rebuild path used to reset the runtime to the
// caller's fallback as a side effect. After a default bump that side effect
// silently re-declares an existing subos against a libc its payloads were
// never built for — the exact "changed underneath you" C1 forbids.
//
// Three sources, most authoritative first:
//
//   1. a valid recorded binding — the subos said what it is;
//   2. the workspace's active runtime — the subos never said, but it is
//      demonstrably running something, and declaring it against anything else
//      is the same re-declaration as (1) guards, reached from the
//      never-recorded side instead of the invalid-block side;
//   3. the caller's fallback — nothing is known, so a new subos's default is
//      the only answer left.
//
// (2) is what makes the upgrade seamless for every home created before
// `subos_info` existed. Without it those homes are declared against the
// current default the first time anything rebuilds their block, and then
// `self doctor` calls them broken and `use` refuses to activate the runtime
// they were already on.
std::string preserved_runtime(const nlohmann::json& doc,
                              std::string_view fallback) {
    if (doc.contains(std::string(BLOCK)) && doc[std::string(BLOCK)].is_object()) {
        const auto r = doc[std::string(BLOCK)].value("runtime", std::string{});
        if (is_binding(r)) return r;
    }
    if (auto observed = observed_runtime(doc, binding_name(fallback));
        !observed.empty()) {
        return observed;
    }
    return std::string(fallback);
}

// ── env declarations ────────────────────────────────────────────────────

// Record one declaration under its provider. Idempotent on the whole
// (var, op, value) triple: config() runs again on every dependent install, and
// a re-run must not grow the section.
//
// Returns whether the document changed, so a caller can skip a write.
bool add_env(nlohmann::json& doc, std::string_view binding, const EnvDecl& decl) {
    auto& b = doc[std::string(BLOCK)];
    if (!b.is_object()) b = nlohmann::json::object();
    if (!b.contains("envs") || !b["envs"].is_object())
        b["envs"] = nlohmann::json::object();

    auto& section = b["envs"][std::string(binding)];
    if (!section.is_array()) section = nlohmann::json::array();

    for (const auto& existing : section) {
        if (existing.value("var", std::string{}) == decl.var
            && existing.value("op", std::string{}) == decl.op
            && existing.value("value", std::string{}) == decl.value) {
            return false;
        }
    }
    section.push_back({{"var", decl.var}, {"op", decl.op}, {"value", decl.value}});
    return true;
}

// Replace a provider's whole section with what it declared THIS run.
//
// add_env alone cannot express this, and the gap is not theoretical. Its
// contract is idempotence per (var, op, value) triple, which holds a re-run of
// the SAME declarations steady -- and silently accumulates two generations when
// the recipe's declarations CHANGE. Measured while moving mesa's discovery paths
// from `${pkgdir}` to `${subosdir}`: the section ended up holding both, so
// LIBGL_DRIVERS_PATH resolved to the new subos directory followed by a stale
// payload path, and __EGL_VENDOR_LIBRARY_DIRS listed the shared vendor directory
// twice. Nothing reported it, and once that payload is collected the stale entry
// is a directory the loader walks past.
//
// A recipe is the sole owner of its binding's section -- that is the ownership
// rule the whole `envs` design rests on, and the reason uninstall needs no
// cleanup code in the recipe. Owning it means being able to REMOVE a
// declaration, not only add one.
//
// Returns whether the document changed, so a caller can skip a write. Order is
// preserved as declared: `prepend` composition depends on it.
bool set_env_section(nlohmann::json& doc, std::string_view binding,
                     const std::vector<EnvDecl>& decls) {
    auto& b = doc[std::string(BLOCK)];
    if (!b.is_object()) b = nlohmann::json::object();
    if (!b.contains("envs") || !b["envs"].is_object())
        b["envs"] = nlohmann::json::object();

    auto next = nlohmann::json::array();
    for (const auto& d : decls) {
        // Within one run a package may legitimately compute the same
        // declaration twice (a shared helper called from two places); recording
        // it twice would double the value in the resolved variable.
        bool dup = false;
        for (const auto& existing : next) {
            if (existing.value("var", std::string{}) == d.var
                && existing.value("op", std::string{}) == d.op
                && existing.value("value", std::string{}) == d.value) {
                dup = true;
                break;
            }
        }
        if (!dup)
            next.push_back({{"var", d.var}, {"op", d.op}, {"value", d.value}});
    }

    auto& section = b["envs"][std::string(binding)];
    if (section == next) return false;
    section = std::move(next);
    return true;
}

// Drop a provider's whole section — the uninstall counterpart of add_env. The
// package never writes cleanup code for this; ownership is by binding, so
// removing the key removes exactly what that package added and nothing else.
//
// The `envs` object itself stays, empty, per the invariant above.
bool remove_provider(nlohmann::json& doc, std::string_view binding) {
    if (!doc.contains(std::string(BLOCK))) return false;
    auto& b = doc[std::string(BLOCK)];
    if (!b.is_object() || !b.contains("envs") || !b["envs"].is_object())
        return false;
    return b["envs"].erase(std::string(binding)) > 0;
}

// Every provider whose package name matches, regardless of version. Uninstall
// knows the name and version it removed, but a home that installed the same
// package twice under different versions can hold a stale section from the
// earlier one; matching on name is what lets doctor see it.
std::vector<std::string> providers_named(const nlohmann::json& doc,
                                         std::string_view name) {
    std::vector<std::string> out;
    if (!doc.contains(std::string(BLOCK))) return out;
    const auto& b = doc[std::string(BLOCK)];
    if (!b.is_object() || !b.contains("envs") || !b["envs"].is_object()) return out;
    for (auto it = b["envs"].begin(); it != b["envs"].end(); ++it)
        if (binding_name(it.key()) == name) out.push_back(it.key());
    return out;
}

// ── placeholders ────────────────────────────────────────────────────────

// What a value may refer to. `${pkgdir}` differs per provider, so it arrives
// as a resolver rather than a path — and the resolver is the caller's, which
// is what keeps this module independent of the version database.
struct Placeholders {
    fs::path subosdir;
    fs::path home;
    fs::path xlings_home;
    std::function<fs::path(std::string_view binding)> pkgdir_of;
};

// Expand ${...} in a declared value.
//
// Placeholders are why a manifest is portable at all: a value carrying
// /home/alice/... describes one machine, and a subos description that only
// works on the machine that wrote it is not a description.
//
// An unknown or unresolvable placeholder is left verbatim rather than replaced
// with an empty string. Empty would turn "${pkgdir}/lib/dri" into "/lib/dri" —
// a real path, on the host, outside the subos. Leaving the text intact makes
// the failure visible to doctor D3 instead of pointing a driver search at /.
std::string expand(std::string_view value, std::string_view binding,
                   const Placeholders& ph) {
    std::string out;
    out.reserve(value.size());

    for (std::size_t i = 0; i < value.size();) {
        if (value[i] != '$' || i + 1 >= value.size() || value[i + 1] != '{') {
            out += value[i++];
            continue;
        }
        const auto close = value.find('}', i + 2);
        if (close == std::string_view::npos) {           // unterminated: verbatim
            out += value.substr(i);
            break;
        }
        const auto name = value.substr(i + 2, close - i - 2);
        fs::path resolved;
        bool known = true;
        if      (name == "subosdir")    resolved = ph.subosdir;
        else if (name == "home")        resolved = ph.home;
        else if (name == "xlings_home") resolved = ph.xlings_home;
        else if (name == "pkgdir")      resolved = ph.pkgdir_of ? ph.pkgdir_of(binding)
                                                                : fs::path{};
        else known = false;

        if (!known || resolved.empty()) out += value.substr(i, close - i + 1);
        else                            out += resolved.string();
        i = close + 1;
    }
    return out;
}

// True once expansion left any `${...}` behind — i.e. something could not be
// resolved. Doctor D3 reports on this rather than on the directory existing,
// because an unexpanded value is a defect regardless of what is on disk.
bool has_unresolved(std::string_view expanded) {
    return expanded.find("${") != std::string_view::npos;
}

// ── privileged declarations ─────────────────────────────────────────────
//
// A `subos.env` declaration whose value points at OUR payload is one of two
// very different things, and the difference is not "is it process-global":
//
//   causes CODE to be loaded    LD_LIBRARY_PATH, LD_PRELOAD,
//     into someone's process    __EGL_VENDOR_LIBRARY_DIRS, LIBGL_DRIVERS_PATH,
//                               VK_ICD_FILENAMES, and every future variable
//                               some library invents for finding its plugins
//   causes DATA to be found     XDG_DATA_DIRS, MANPATH, PKG_CONFIG_PATH
//
// The first class is dangerous because every child of the subos shell inherits
// it, and most of those children are HOST binaries under the HOST loader.
// Measured: a host binary linked against the host's libEGL drops from the
// NVIDIA GPU to llvmpipe under our declarations, and LD_DEBUG shows it loading
// OUR libm, libgcc_s, libstdc++ and libxcb into a process running on the host's
// libc. On this machine the host glibc happened to match; on an older one that
// is `version 'GLIBC_2.xx' not found`. The second class is ordinary — subos
// supplies a default, the user can override it, that is how Linux works (AD-3).
// `PATH` is a third thing again: it does not inject code into an existing
// process, it decides which executable runs, and it is governed by R6/AD-1
// rather than by this guard.
//
// The list below is the BENIGN one -- named for what it asserts, since PATH is
// on it and PATH plainly does not name only data. The check is default-deny. Listing the
// dangerous set instead would be a hand-written list of "what we happened to
// think of" — the exact anti-pattern R7 names, and the one that already cost us
// five missing entries in nvidia-gl-host-link's dependency table. A variable
// nobody has classified reads as privileged, which fails toward a report.
//
// Adding to this list is a deliberate act: it asserts the variable cannot cause
// code to enter a process.
inline bool never_loads_code(std::string_view var) {
    return var == "XDG_DATA_DIRS"   || var == "XDG_CONFIG_DIRS"
        || var == "XDG_DATA_HOME"   || var == "XDG_CONFIG_HOME"
        || var == "XDG_CACHE_HOME"  || var == "XDG_STATE_HOME"
        || var == "MANPATH"         || var == "INFOPATH"
        || var == "PKG_CONFIG_PATH" || var == "PKG_CONFIG_LIBDIR"
        || var == "ACLOCAL_PATH"    || var == "TERMINFO"
        || var == "FONTCONFIG_PATH" || var == "FONTCONFIG_FILE"
        || var == "SSL_CERT_FILE"   || var == "SSL_CERT_DIR"
        || var == "GIT_SSL_CAINFO"  || var == "CURL_CA_BUNDLE"
        || var == "LOCPATH"         || var == "TZDIR"
        || var == "PATH";  // R6/AD-1's business, not this guard's
}

// A declaration is privileged when it can put code from our payload into a
// process we do not own.
//
// The placeholders are checked as well as an expanded store path, because at
// install time -- the moment this most needs to be reported -- the value has
// not been expanded yet.
//
// `${subosdir}` counts. The subos sysroot is a VIEW onto our payloads, made of
// symlinks into them, so a directory under it on a loader search path delivers
// our libraries just as surely as the store path does. Checking only
// `${pkgdir}` would have let the same declaration through in its other spelling
// -- one hazard with two names, which is the shape this whole review is about.
inline bool is_privileged_env(std::string_view var, std::string_view value) {
    if (never_loads_code(var)) return false;
    for (const auto* needle : {"${pkgdir}", "${subosdir}", "${xlings_home}",
                               "/xpkgs/"}) {
        if (value.find(needle) != std::string_view::npos) return true;
    }
    return false;
}

// ── resolution ──────────────────────────────────────────────────────────

// One variable as it will actually be exported.
struct Resolved {
    std::string              var;
    std::string              op;         // the winning op
    std::string              value;      // expanded, prepends already joined
    std::vector<std::string> providers;  // every binding that declared this var
    bool                     conflicted = false;
    bool                     unresolved = false;
};

// Fold the manifest into the variables to export.
//
// Conflict rules, when more than one provider names the same variable:
//   * several `set`      — the last provider wins, and it is a conflict
//   * several `prepend`  — all contribute, later providers land nearer the front
//   * `set` and `prepend` mixed — `set` wins, the prepends are dropped, conflict
//
// "Later" means later in binding order, not install order. Install order is
// not recorded in the manifest and recording it would add a field whose only
// job is to make the outcome depend on history — two homes with byte-identical
// manifests would then export different values. Sorting by binding keeps the
// manifest the whole answer. Every conflict is reported (doctor D4) rather than
// resolved quietly.
std::vector<Resolved> resolve(const Info& info, const Placeholders& ph) {
    struct Acc {
        std::string              setValue;
        bool                     hasSet = false;
        std::vector<std::string> prepends;   // front-most last
        std::vector<std::string> providers;
        bool                     unresolved = false;
        std::size_t              order = 0;  // first appearance, for stable output
    };
    std::map<std::string, Acc> acc;
    std::size_t seen = 0;

    for (const auto& p : info.envs) {          // already sorted by binding
        for (const auto& d : p.decls) {
            auto expanded = expand(d.value, p.binding, ph);
            auto& a = acc[d.var];
            if (a.providers.empty()) a.order = seen++;
            a.providers.push_back(p.binding);
            if (has_unresolved(expanded)) a.unresolved = true;
            if (d.op == OP_SET) { a.setValue = std::move(expanded); a.hasSet = true; }
            else                { a.prepends.push_back(std::move(expanded)); }
        }
    }

    std::vector<Resolved> out;
    out.reserve(acc.size());
    for (auto& [var, a] : acc) {
        Resolved r{.var = var, .providers = a.providers, .unresolved = a.unresolved};
        // More than one declaration for a variable is a conflict unless they
        // are all prepends, which compose by construction.
        r.conflicted = a.providers.size() > 1
                       && !(!a.hasSet && !a.prepends.empty());
        if (a.hasSet) {
            r.op    = std::string(OP_SET);
            r.value = std::move(a.setValue);
            if (!a.prepends.empty()) r.conflicted = true;
        } else {
            r.op = std::string(OP_PREPEND);
            // Later provider nearer the front, matching "the newest thing
            // installed is found first".
            //
            // De-duplicated, because two providers naming the same directory is
            // not a mistake to preserve -- it is the intended arrangement. mesa
            // and nvidia-gl-host-link both declare the SHARED glvnd vendor
            // directory precisely so that either package being absent does not
            // remove it for the other, and joining them verbatim put that path
            // on __EGL_VENDOR_LIBRARY_DIRS twice.
            //
            // The shim side already did this (xvm/shim.cppm
            // merge_shim_env_value, added when a doubled GIT_SSL_CAINFO broke
            // every HTTPS git transport). Two answers to one question; this is
            // the second one agreeing with the first.
            std::set<std::string> seen;
            for (auto it = a.prepends.rbegin(); it != a.prepends.rend(); ++it) {
                if (!seen.insert(*it).second) continue;
                if (!r.value.empty()) r.value += ':';
                r.value += *it;
            }
        }
        out.push_back(std::move(r));
    }
    // Stable, and stable for a reason: this list is echoed to the user and
    // diffed in tests, so it must not reorder because a map rehashed.
    std::ranges::sort(out, {}, &Resolved::var);
    return out;
}

} // namespace xlings::subos::manifest
