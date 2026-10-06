module xlings.subos.policy;

import std;
import xlings.libs.json;

namespace xlings::subos::policy {

std::string_view to_string(Preset p) {
    switch (p) {
    case Preset::Dev:     return "dev";
    case Preset::Private: return "private";
    case Preset::Locked:  return "locked";
    default:              return "legacy";
    }
}

std::string_view to_string(Net n) {
    switch (n) {
    case Net::Nat:   return "nat";
    case Net::None:  return "none";
    case Net::Proxy: return "proxy";
    default:         return "host";
    }
}

std::string_view to_string(Fetch f) {
    switch (f) {
    case Fetch::Ask:   return "ask";
    case Fetch::Layer: return "layer";
    case Fetch::Deny:  return "deny";
    default:           return "auto";
    }
}

std::string_view to_string(Observe o) {
    switch (o) {
    case Observe::Off:      return "off";
    case Observe::Standard: return "standard";
    case Observe::Full:     return "full";
    default:                return "basic";
    }
}

std::optional<Preset> preset_from_string(std::string_view s) {
    for (auto p : {Preset::Legacy, Preset::Dev, Preset::Private, Preset::Locked})
        if (to_string(p) == s) return p;
    return std::nullopt;
}

std::optional<Net> net_from_string(std::string_view s) {
    for (auto n : {Net::Host, Net::Nat, Net::None, Net::Proxy})
        if (to_string(n) == s) return n;
    return std::nullopt;
}

std::optional<Fetch> fetch_from_string(std::string_view s) {
    for (auto f : {Fetch::Auto, Fetch::Ask, Fetch::Layer, Fetch::Deny})
        if (to_string(f) == s) return f;
    return std::nullopt;
}

std::optional<Observe> observe_from_string(std::string_view s) {
    for (auto o : {Observe::Off, Observe::Basic, Observe::Standard, Observe::Full})
        if (to_string(o) == s) return o;
    return std::nullopt;
}

Policy legacy() {
    Policy p;
    p.preset = Preset::Legacy;
    p.env_pass.assign(kDevEnvPass.begin(), kDevEnvPass.end());
    for (auto g : kGrants) p.grants_allowed.insert(std::string(g));
    return p;
}

bool env_name_matches(std::string_view name, std::span<const std::string> patterns) {
    for (const auto& p : patterns) {
        if (!p.empty() && p.back() == '*') {
            if (name.starts_with(std::string_view(p).substr(0, p.size() - 1))) return true;
        } else if (name == p) {
            return true;
        }
    }
    return false;
}

}  // namespace xlings::subos::policy

namespace xlings::subos::policy {

namespace {

int rank(Net n) {
    switch (n) {
    case Net::Host:  return 0;
    case Net::Nat:   return 1;
    case Net::Proxy: return 2;
    case Net::None:  return 3;
    }
    return 0;
}

int rank(Fetch f) {
    switch (f) {
    case Fetch::Auto:  return 0;
    case Fetch::Layer: return 1;
    case Fetch::Ask:   return 2;
    case Fetch::Deny:  return 3;
    }
    return 0;
}

int rank(Observe o) {
    switch (o) {
    case Observe::Off:      return 0;
    case Observe::Basic:    return 1;
    case Observe::Standard: return 2;
    case Observe::Full:     return 3;
    }
    return 0;
}

bool free_key(std::string_view k) { return k == "comment" || k.starts_with("x-"); }

std::expected<std::vector<std::string>, std::string> string_list(const nlohmann::json& j,
                                                                 std::string_view where) {
    if (!j.is_array()) return std::unexpected(std::format("{} must be a list of strings", where));
    std::vector<std::string> out;
    for (auto& e : j) {
        if (!e.is_string()) return std::unexpected(std::format("{} must be a list of strings", where));
        out.push_back(e.get<std::string>());
    }
    return out;
}

}  // namespace

std::string_view to_string(Action a) {
    switch (a) {
    case Action::Allow: return "allow";
    case Action::Ask:   return "ask";
    default:            return "deny";
    }
}

bool glob_match(std::string_view pat, std::string_view text) {
    std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
    while (t < text.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t])) { ++p; ++t; }
        else if (p < pat.size() && pat[p] == '*') { star = p++; mark = t; }
        else if (star != std::string_view::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::optional<std::uint64_t> parse_size(std::string_view text) {
    std::uint64_t mult = 1;
    auto upper = std::string(text);
    for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::string_view v(upper);
    for (auto [suffix, m] : std::initializer_list<std::pair<std::string_view, std::uint64_t>>{
             {"GB", 1ull << 30}, {"MB", 1ull << 20}, {"KB", 1ull << 10}, {"G", 1ull << 30},
             {"M", 1ull << 20}, {"K", 1ull << 10}, {"B", 1}}) {
        if (v.ends_with(suffix)) { mult = m; v.remove_suffix(suffix.size()); break; }
    }
    std::uint64_t n = 0;
    auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    if (ec != std::errc{} || ptr != v.data() + v.size() || v.empty()) return std::nullopt;
    return n * mult;
}

Policy preset(Preset which) {
    Policy p = legacy();
    p.preset = which;
    p.extends = std::string(to_string(which));
    switch (which) {
    case Preset::Legacy:
    case Preset::Dev:
        break;
    case Preset::Private:
        p.net = Net::Nat;
        p.fetch = Fetch::Ask;
        p.index_update = Fetch::Ask;
        p.observe = Observe::Standard;
        p.identity = Identity::Neutral;
        p.tz = "UTC";
        p.env_pass.assign({"http_proxy", "https_proxy", "HTTP_PROXY", "HTTPS_PROXY",
                           "no_proxy", "NO_PROXY", "all_proxy", "ALL_PROXY"});
        p.env_explicit_any = false;
        for (auto d : {"fs", "pid", "net"}) p.needs[d] = Need::Must;
        break;
    case Preset::Locked:
        p.net = Net::None;
        p.fetch = Fetch::Deny;
        p.index_update = Fetch::Deny;
        p.observe = Observe::Full;
        p.identity = Identity::Neutral;
        p.tz = "UTC";
        p.env_pass.clear();
        p.env_explicit_any = false;
        p.grants_allowed.clear();
        p.mounts_ro_default = true;
        p.disable_userns = true;
        for (auto d : {"fs", "pid", "net", "identity", "terminal"}) p.needs[d] = Need::Must;
        break;
    }
    return p;
}

std::expected<Policy, std::string> from_json(const nlohmann::json& doc) {
    if (!doc.is_object()) return std::unexpected("policy: not a JSON object");
    auto unknown = [](std::string_view where) {
        return std::unexpected(std::format(
            "policy: unknown field '{}' -- this version of xlings does not know it, and an "
            "isolation setting it cannot enforce is refused rather than ignored", where));
    };
    auto bad = [](std::string_view where, std::string_view value) {
        return std::unexpected(std::format("policy: {} = '{}' is not a value this version knows",
                                           where, value));
    };

    Policy p = preset(Preset::Dev);
    if (auto it = doc.find("extends"); it != doc.end()) {
        if (!it->is_string()) return std::unexpected("policy: extends must be a string");
        const auto ext = it->get<std::string>();
        if (is_package_ref(ext)) {
            // Selected from a package: the copy below is the policy; `resolved`
            // says where it came from and which preset that package built on.
            auto r = doc.find("resolved");
            if (r == doc.end() || !r->is_object() || !r->contains("from") || !r->contains("sha256"))
                return std::unexpected(std::format(
                    "policy: extends = '{}' without `resolved` -- select the package with "
                    "`xlings subos config <name> --sandbox {}`", ext, ext));
            auto base = preset_from_string(r->value("base", "dev"));
            if (!base || *base == Preset::Legacy) return bad("resolved.base", r->value("base", ""));
            p = preset(*base);
            p.extends = ext;
            p.package = Policy::Package{ r->value("from", ""), r->value("sha256", "") };
        } else {
            auto base = preset_from_string(ext);
            if (!base || *base == Preset::Legacy) return bad("extends", ext);
            p = preset(*base);
        }
    }
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        const auto& key = it.key();
        if (key == "extends" || key == "resolved" || free_key(key)) continue;
        const auto& v = it.value();
        if (key == "isolation") {
            if (!v.is_object()) return std::unexpected("policy: isolation must be an object");
            for (auto f = v.begin(); f != v.end(); ++f) {
                const auto& k = f.key();
                const auto& fv = f.value();
                const auto where = "isolation." + k;
                if (free_key(k)) continue;
                if (k == "net") {
                    auto n = fv.is_string() ? net_from_string(fv.get<std::string>()) : std::nullopt;
                    if (!n) return bad(where, fv.dump());
                    p.net = *n;
                } else if (k == "proxy") {
                    if (!fv.is_string()) return bad(where, fv.dump());
                    p.proxy = fv.get<std::string>();
                } else if (k == "identity") {
                    if (fv.is_string()) {
                        if (fv == "host") p.identity = Identity::Host;
                        else if (fv == "neutral") p.identity = Identity::Neutral;
                        else return bad(where, fv.dump());
                    } else if (fv.is_object()) {
                        p.identity = Identity::Neutral;
                        for (auto g = fv.begin(); g != fv.end(); ++g) {
                            if (g.key() == "tz" && g.value().is_string()) p.tz = g.value().get<std::string>();
                            else if (!free_key(g.key())) return unknown(where + "." + g.key());
                        }
                    } else {
                        return bad(where, fv.dump());
                    }
                } else if (k == "grants" || k == "grants_allowed") {
                    auto list = string_list(fv, where);
                    if (!list) return std::unexpected(list.error());
                    std::set<std::string, std::less<>> set;
                    for (auto& g : *list) {
                        if (std::ranges::find(kGrants, std::string_view(g)) == kGrants.end())
                            return bad(where, g);
                        set.insert(g);
                    }
                    (k == "grants" ? p.grants : p.grants_allowed) = std::move(set);
                } else if (k == "env_pass") {
                    auto list = string_list(fv, where);
                    if (!list) return std::unexpected(list.error());
                    p.env_pass = std::move(*list);
                } else if (k == "disable_userns" || k == "no_degrade") {
                    if (!fv.is_boolean()) return bad(where, fv.dump());
                    (k == "disable_userns" ? p.disable_userns : p.no_degrade) = fv.get<bool>();
                } else if (k == "needs") {
                    if (!fv.is_object()) return bad(where, fv.dump());
                    for (auto g = fv.begin(); g != fv.end(); ++g) {
                        if (g.value() == "must") p.needs[g.key()] = Need::Must;
                        else if (g.value() == "should") p.needs[g.key()] = Need::Should;
                        else return bad(where + "." + g.key(), g.value().dump());
                    }
                } else {
                    return unknown(where);
                }
            }
        } else if (key == "mounts") {
            if (!v.is_array()) return std::unexpected("policy: mounts must be a list");
            p.mounts.clear();
            for (auto& m : v) {
                if (!m.is_object() || !m.contains("src") || !m["src"].is_string())
                    return std::unexpected("policy: each mount needs a src");
                Mount mount{ .src = m["src"].get<std::string>(),
                             .dst = m.value("dst", std::string{}),
                             .rw = !p.mounts_ro_default };
                for (auto f = m.begin(); f != m.end(); ++f) {
                    if (f.key() == "src" || f.key() == "dst" || free_key(f.key())) continue;
                    if (f.key() != "mode") return unknown("mounts[]." + f.key());
                    if (f.value() == "ro") mount.rw = false;
                    else if (f.value() == "rw") mount.rw = true;
                    else return bad("mounts[].mode", f.value().dump());
                }
                p.mounts.push_back(std::move(mount));
            }
        } else if (key == "permissions") {
            if (!v.is_object()) return std::unexpected("policy: permissions must be an object");
            for (auto f = v.begin(); f != v.end(); ++f) {
                const auto& k = f.key();
                const auto& fv = f.value();
                if (free_key(k)) continue;
                if (k == "index_update") {
                    auto a = fv.is_string() ? fetch_from_string(fv.get<std::string>()) : std::nullopt;
                    if (!a) return bad("permissions.index_update", fv.dump());
                    p.index_update = *a;
                } else if (k == "fetch") {
                    if (fv.is_string()) {
                        auto a = fetch_from_string(fv.get<std::string>());
                        if (!a) return bad("permissions.fetch", fv.dump());
                        p.fetch = *a;
                        continue;
                    }
                    if (!fv.is_object()) return bad("permissions.fetch", fv.dump());
                    for (auto g = fv.begin(); g != fv.end(); ++g) {
                        if (free_key(g.key())) continue;
                        if (g.key() == "default") {
                            auto a = g.value().is_string() ? fetch_from_string(g.value().get<std::string>())
                                                           : std::nullopt;
                            if (!a) return bad("permissions.fetch.default", g.value().dump());
                            p.fetch = *a;
                        } else if (g.key() == "rules") {
                            if (!g.value().is_array()) return bad("permissions.fetch.rules", "not a list");
                            p.fetch_rules.clear();
                            for (auto& r : g.value()) {
                                if (!r.is_object()) return bad("permissions.fetch.rules[]", r.dump());
                                Policy::Rule rule;
                                for (auto h = r.begin(); h != r.end(); ++h) {
                                    const auto& rk = h.key();
                                    if (free_key(rk)) continue;
                                    if (rk == "match" && h.value().is_string()) rule.match = h.value().get<std::string>();
                                    else if (rk == "index" && h.value().is_string()) rule.index = h.value().get<std::string>();
                                    else if (rk == "size_gt" && h.value().is_string()) {
                                        auto sz = parse_size(h.value().get<std::string>());
                                        if (!sz) return bad("permissions.fetch.rules[].size_gt", h.value().dump());
                                        rule.size_gt = *sz;
                                    } else if (rk == "action" && h.value().is_string()) {
                                        auto a = fetch_from_string(h.value().get<std::string>());
                                        if (!a) return bad("permissions.fetch.rules[].action", h.value().dump());
                                        rule.action = *a;
                                    } else return unknown("permissions.fetch.rules[]." + rk);
                                }
                                p.fetch_rules.push_back(std::move(rule));
                            }
                        } else return unknown("permissions.fetch." + g.key());
                    }
                } else {
                    return unknown("permissions." + k);
                }
            }
        } else if (key == "observe") {
            if (!v.is_object()) return std::unexpected("policy: observe must be an object");
            for (auto f = v.begin(); f != v.end(); ++f) {
                if (free_key(f.key()) || f.key() == "retention" || f.key() == "redact") continue;
                if (f.key() != "level") return unknown("observe." + f.key());
                auto o = f.value().is_string() ? observe_from_string(f.value().get<std::string>())
                                               : std::nullopt;
                if (!o) return bad("observe.level", f.value().dump());
                p.observe = *o;
            }
        } else {
            return unknown(key);
        }
    }
    if (auto why = not_enforced(p)) return std::unexpected("policy: " + *why);
    return p;
}

std::optional<std::string> not_enforced(const Policy& p) {
    (void)p;
    return std::nullopt;
}

bool fetches_into_layer(const Policy& p) {
    bool layer = p.fetch == Fetch::Layer || p.index_update == Fetch::Layer;
    for (const auto& r : p.fetch_rules) layer = layer || r.action == Fetch::Layer;
    return layer;
}

bool is_package_ref(std::string_view extends) {
    return extends.find(':') != std::string_view::npos;
}

std::expected<Policy, std::string> from_package(const nlohmann::json& doc, std::string_view ref,
                                                Policy::Package resolved) {
    if (!doc.is_object()) return std::unexpected("not a JSON object");
    if (doc.contains("resolved")) return std::unexpected("a package's policy carries no `resolved`");
    if (auto it = doc.find("extends"); it != doc.end() && it->is_string()
        && is_package_ref(it->get<std::string>()))
        return std::unexpected(std::format(
            "it extends another package ({}); a package extends a built-in preset or nothing",
            it->get<std::string>()));
    auto p = from_json(doc);
    if (!p) return p;
    p->extends = std::string(ref);
    p->package = std::move(resolved);
    return p;
}

nlohmann::json to_json(const Policy& p) {
    nlohmann::json j;
    j["extends"] = p.extends.empty() ? std::string(to_string(p.preset)) : p.extends;
    if (p.package)
        j["resolved"] = {{"from", p.package->from}, {"sha256", p.package->sha256},
                         {"base", std::string(to_string(p.preset))}};
    nlohmann::json iso;
    iso["net"] = std::string(to_string(p.net));
    if (!p.proxy.empty()) iso["proxy"] = p.proxy;
    iso["identity"] = p.identity == Identity::Neutral
        ? nlohmann::json{{"tz", p.tz.empty() ? "UTC" : p.tz}} : nlohmann::json("host");
    iso["grants"] = std::vector<std::string>(p.grants.begin(), p.grants.end());
    iso["grants_allowed"] = std::vector<std::string>(p.grants_allowed.begin(), p.grants_allowed.end());
    iso["env_pass"] = p.env_pass;
    iso["disable_userns"] = p.disable_userns;
    iso["no_degrade"] = p.no_degrade;
    if (!p.needs.empty()) {
        nlohmann::json needs = nlohmann::json::object();
        for (auto& [d, n] : p.needs) needs[d] = n == Need::Must ? "must" : "should";
        iso["needs"] = needs;
    }
    j["isolation"] = iso;
    j["mounts"] = nlohmann::json::array();
    for (auto& m : p.mounts) {
        nlohmann::json e{{"src", m.src}, {"mode", m.rw ? "rw" : "ro"}};
        if (!m.dst.empty()) e["dst"] = m.dst;
        j["mounts"].push_back(e);
    }
    nlohmann::json fetch{{"default", std::string(to_string(p.fetch))}};
    if (!p.fetch_rules.empty()) {
        fetch["rules"] = nlohmann::json::array();
        for (auto& r : p.fetch_rules) {
            nlohmann::json e{{"match", r.match}, {"action", std::string(to_string(r.action))}};
            if (!r.index.empty()) e["index"] = r.index;
            if (r.size_gt) e["size_gt"] = std::to_string(r.size_gt);
            fetch["rules"].push_back(e);
        }
    }
    j["permissions"] = {{"fetch", fetch}, {"index_update", std::string(to_string(p.index_update))}};
    j["observe"] = {{"level", std::string(to_string(p.observe))}};
    return j;
}

std::expected<Policy, std::string> apply(Policy p, const Overrides& o) {
    if (o.net) {
        if (rank(*o.net) < rank(p.net))
            return std::unexpected(std::format("--net {} would loosen this instance's policy (net {})",
                                               to_string(*o.net), to_string(p.net)));
        p.net = *o.net;
    }
    if (o.fetch) {
        if (rank(*o.fetch) < rank(p.fetch))
            return std::unexpected(std::format("--fetch {} would loosen this instance's policy (fetch {})",
                                               to_string(*o.fetch), to_string(p.fetch)));
        p.fetch = *o.fetch;
    }
    if (o.observe && rank(*o.observe) > rank(p.observe)) p.observe = *o.observe;
    for (const auto& g : o.allow) {
        if (std::ranges::find(kGrants, std::string_view(g)) == kGrants.end())
            return std::unexpected(std::format("--allow {}: not a grant (one of: display, audio, "
                                               "camera, gpu, ssh-agent, dbus, host-loopback)", g));
        if (!p.grants.contains(g) && !p.grants_allowed.contains(g))
            return std::unexpected(std::format("--allow {}: this instance's policy does not allow "
                                               "granting it (grants_allowed)", g));
        p.grants.insert(g);
    }
    for (auto m : o.mounts) {
        if (!m.mode_given && p.mounts_ro_default) m.rw = false;
        if (m.rw && p.mounts_ro_default && p.preset == Preset::Locked)
            return std::unexpected(std::format("--mount {}: a locked instance maps read-only", m.src));
        p.mounts.push_back(m);
    }
    if (o.no_degrade) p.no_degrade = true;
    if (auto why = not_enforced(p)) return std::unexpected(*why);
    return p;
}

std::vector<std::string> diff(const Policy& a, const Policy& b) {
    // Objects are walked; a list is one value (a reordered or extended grant
    // list reads as one change, not as one line per shifted index).
    std::map<std::string, nlohmann::json> fa, fb;
    std::function<void(const nlohmann::json&, const std::string&, std::map<std::string, nlohmann::json>&)> flat =
        [&](const nlohmann::json& j, const std::string& path, std::map<std::string, nlohmann::json>& out) {
            if (j.is_object()) {
                for (auto it = j.begin(); it != j.end(); ++it) flat(it.value(), path + "/" + it.key(), out);
            } else {
                out[path] = j;
            }
        };
    flat(to_json(a), "", fa);
    flat(to_json(b), "", fb);
    std::vector<std::string> out;
    for (auto& [k, v] : fb) {
        auto it = fa.find(k);
        if (it == fa.end()) out.push_back(std::format("{}: (unset) -> {}", k, v.dump()));
        else if (it->second != v) out.push_back(std::format("{}: {} -> {}", k, it->second.dump(), v.dump()));
    }
    for (auto& [k, v] : fa)
        if (!fb.contains(k)) out.push_back(std::format("{}: {} -> (unset)", k, v.dump()));
    return out;
}

Decision decide(const Policy& p, const Op& op) {
    const auto inst = op.instance.empty() ? std::string("<name>") : op.instance;
    if (op.kind == "policy_change" || op.kind == "instance_admin") {
        if (op.from_inside)
            return {Action::Deny, "only the owner, outside the sandbox, changes this",
                    "xlings subos config " + inst};
        return {Action::Allow, "owner"};
    }
    if (op.kind == "grant") {
        if (p.grants.contains(op.target)) return {Action::Allow, "granted by the policy"};
        if (p.grants_allowed.contains(op.target) && !op.from_inside)
            return {Action::Allow, "grants_allowed"};
        return {Action::Deny, "not granted by this instance's policy",
                std::format("xlings subos config {} --allow {}", inst, op.target)};
    }
    auto to_decision = [&](Fetch f, std::string why) -> Decision {
        const auto owner = std::format("xlings install {} --subos {}", op.target, inst);
        switch (f) {
        case Fetch::Auto:  return {Action::Allow, std::move(why)};
        case Fetch::Layer: return {Action::Allow, why + " (into the instance's own layer)"};
        case Fetch::Ask:   return {Action::Ask, std::move(why), owner};
        default:           return {Action::Deny, std::move(why), owner};
        }
    };
    if (op.kind == "index_update")
        return to_decision(p.index_update, std::format("index_update = {}", to_string(p.index_update)));
    if (op.kind == "fetch") {
        for (std::size_t i = 0; i < p.fetch_rules.size(); ++i) {
            const auto& r = p.fetch_rules[i];
            if (!glob_match(r.match, op.target)) continue;
            if (!r.index.empty() && r.index != op.index) continue;
            if (r.size_gt && !(op.size > r.size_gt)) continue;
            return to_decision(r.action, std::format("fetch rule {} ({} -> {})", i + 1, r.match,
                                                     to_string(r.action)));
        }
        return to_decision(p.fetch, std::format("fetch = {}", to_string(p.fetch)));
    }
    return {Action::Deny, "unknown operation '" + op.kind + "'"};
}

}  // namespace xlings::subos::policy

namespace xlings::subos::policy {

std::expected<Mount, std::string> parse_mount(std::string_view spec, std::string_view home,
                                              std::string_view cwd) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= spec.size()) {
        auto c = spec.find(':', start);
        if (c == std::string_view::npos) c = spec.size();
        parts.emplace_back(spec.substr(start, c - start));
        start = c + 1;
    }
    if (parts.empty() || parts[0].empty() || parts.size() > 3)
        return std::unexpected(std::format("--mount {}: expected <host>[:<inside>][:ro|rw]", spec));
    Mount m;
    auto is_mode = [](const std::string& x) { return x == "ro" || x == "rw"; };
    m.src = parts[0];
    if (parts.size() == 2 && is_mode(parts[1])) {
        m.rw = parts[1] == "rw";
        m.mode_given = true;
    } else if (parts.size() >= 2) {
        m.dst = parts[1];
        if (parts.size() == 3) {
            if (!is_mode(parts[2]))
                return std::unexpected(std::format("--mount {}: the mode is ro or rw", spec));
            m.rw = parts[2] == "rw";
            m.mode_given = true;
        }
    }
    if (m.src == "~" || m.src.starts_with("~/")) m.src = std::string(home) + m.src.substr(1);
    else if (m.src.front() != '/') m.src = std::string(cwd) + "/" + m.src;
    m.src = std::filesystem::path(m.src).lexically_normal().generic_string();
    if (m.dst.starts_with("~/")) m.dst.clear();   // inside, ~ is not the host's home
    return m;
}

}  // namespace xlings::subos::policy
