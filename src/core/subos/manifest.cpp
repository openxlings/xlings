module xlings.core.subos.manifest;

import std;
import xlings.libs.json;
import xlings.platform;

namespace xlings::subos::manifest {

std::string family_of(std::string_view runtime, std::string_view arch) {
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

nlohmann::json make_block(std::string_view runtime, std::string_view createdBy, std::string_view hostGlibc) {
    nlohmann::json b;
    b["schema_version"] = SCHEMA_VERSION;
    b["runtime"]        = std::string(runtime);
    b["envs"]           = nlohmann::json::object();
    b["created_at"]     = utc_now_iso();
    b["created_by"]     = std::string(createdBy);
    if (!hostGlibc.empty()) b["host_glibc"] = std::string(hostGlibc);
    return b;
}

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

bool remove_provider(nlohmann::json& doc, std::string_view binding) {
    if (!doc.contains(std::string(BLOCK))) return false;
    auto& b = doc[std::string(BLOCK)];
    if (!b.is_object() || !b.contains("envs") || !b["envs"].is_object())
        return false;
    return b["envs"].erase(std::string(binding)) > 0;
}

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

bool has_unresolved(std::string_view expanded) {
    return expanded.find("${") != std::string_view::npos;
}

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

}
