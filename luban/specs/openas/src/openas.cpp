module openas;

import std;

namespace openas {

namespace {

bool name_char_(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '@';
}

std::string short_(const Actor& a) {
    return a.name.empty() ? std::string(name_of(a.kind)) : std::format("{}:{}", name_of(a.kind), a.name);
}

std::optional<Actor> parse_one_(std::string_view text) {
    Actor a;
    const auto colon = text.find(':');
    auto k = kind_from(text.substr(0, colon));
    if (!k) return std::nullopt;
    a.kind = *k;
    if (colon != std::string_view::npos) {
        a.name = std::string(text.substr(colon + 1));
        if (a.name.empty() || !std::ranges::all_of(a.name, name_char_)) return std::nullopt;
    }
    return a;
}

}  // namespace

std::string format(const Actor& a) {
    auto out = short_(a);
    if (!a.via.empty()) {
        out += ";via=";
        for (std::size_t i = 0; i < a.via.size(); ++i) out += (i ? "," : "") + a.via[i];
    }
    return out;
}

std::optional<Actor> parse(std::string_view text) {
    const auto semi = text.find(';');
    auto a = parse_one_(text.substr(0, semi));
    if (!a) return std::nullopt;
    if (semi == std::string_view::npos) return a;
    auto rest = text.substr(semi + 1);
    if (!rest.starts_with("via=")) return std::nullopt;
    rest.remove_prefix(4);
    // One or more actors, comma-separated: never an empty one.
    for (;;) {
        const auto comma = rest.find(',');
        const auto one = rest.substr(0, comma);
        if (!parse_one_(one)) return std::nullopt;
        a->via.emplace_back(one);
        if (comma == std::string_view::npos) return a;
        rest.remove_prefix(comma + 1);
    }
}

Actor delegate(const Actor& from, Kind kind, std::string name) {
    Actor to{kind, std::move(name), from.via};
    to.via.push_back(short_(from));
    return to;
}

bool acts_for(const Actor& a, const Actor& owner) {
    if (a == owner) return true;
    const auto chain = owner.via;
    if (a.via.size() < chain.size() + 1) return false;
    for (std::size_t i = 0; i < chain.size(); ++i)
        if (a.via[i] != chain[i]) return false;
    return a.via[chain.size()] == short_(owner);
}

}  // namespace openas
