module xlings.subos.policy;

import std;

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
    p.env_inherit = true;
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
