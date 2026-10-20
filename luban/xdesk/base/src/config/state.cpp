// state.json and keys.json (state.cppm), and the JSON they are written in.
module xdesk.config;

import std;

namespace xdesk::config {

namespace fs = std::filesystem;

namespace {

struct Reader {
    std::string_view s;
    std::size_t at { 0 };

    void space() { while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) ++at; }
    bool eat(char c) {
        space();
        if (at < s.size() && s[at] == c) { ++at; return true; }
        return false;
    }
    std::expected<Json, std::string> fail(std::string_view what) const { return std::unexpected(std::format("{} at byte {}", what, at)); }

    std::expected<std::string, std::string> string() {
        if (!eat('"')) return std::unexpected(std::format("a string expected at byte {}", at));
        std::string out;
        while (at < s.size() && s[at] != '"') {
            char c = s[at++];
            if (c != '\\') { out.push_back(c); continue; }
            if (at >= s.size()) break;
            switch (const char e = s[at++]) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (at + 4 > s.size() || std::from_chars(s.data() + at, s.data() + at + 4, cp, 16).ec != std::errc{})
                        return std::unexpected(std::format("a bad \\u escape at byte {}", at));
                    at += 4;
                    if (cp < 0x80) out.push_back(static_cast<char>(cp));
                    else if (cp < 0x800) { out.push_back(static_cast<char>(0xc0 | cp >> 6)); out.push_back(static_cast<char>(0x80 | (cp & 0x3f))); }
                    else {
                        out.push_back(static_cast<char>(0xe0 | cp >> 12));
                        out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3f)));
                        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                    }
                    break;
                }
                default: out.push_back(e);
            }
        }
        if (at >= s.size()) return std::unexpected(std::string("a string without its end"));
        ++at;
        return out;
    }

    std::expected<Json, std::string> value(int depth = 0) {
        if (depth > 64) return fail("nested too deep");
        space();
        if (at >= s.size()) return fail("a value expected");
        const char c = s[at];
        if (c == '{') {
            ++at;
            Json::Object o;
            if (eat('}')) return Json{o};
            do {
                auto k = string();
                if (!k) return std::unexpected(k.error());
                if (!eat(':')) return fail("':' expected");
                auto v = value(depth + 1);
                if (!v) return v;
                o.emplace_back(std::move(*k), std::move(*v));
            } while (eat(','));
            if (!eat('}')) return fail("'}' expected");
            return Json{o};
        }
        if (c == '[') {
            ++at;
            Json::Array a;
            if (eat(']')) return Json{a};
            do {
                auto v = value(depth + 1);
                if (!v) return v;
                a.push_back(std::move(*v));
            } while (eat(','));
            if (!eat(']')) return fail("']' expected");
            return Json{a};
        }
        if (c == '"') {
            auto str = string();
            if (!str) return std::unexpected(str.error());
            return Json{std::move(*str)};
        }
        for (const auto& [word, val] : {std::pair<std::string_view, Json>{"true", Json{true}}, {"false", Json{false}}, {"null", Json{nullptr}}})
            if (s.substr(at, word.size()) == word) { at += word.size(); return val; }
        double d = 0;
        const auto [end, ec] = std::from_chars(s.data() + at, s.data() + s.size(), d);
        if (ec != std::errc{}) return fail("a value expected");
        at = static_cast<std::size_t>(end - s.data());
        return Json{d};
    }
};

void dump_(const Json& j, std::string& out, int indent, int level) {
    const auto nl = [&](int l) {
        if (indent <= 0) return;
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent * l), ' ');
    };
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) out += "null";
        else if constexpr (std::is_same_v<T, bool>) out += x ? "true" : "false";
        else if constexpr (std::is_same_v<T, double>) {
            if (x == std::floor(x) && std::abs(x) < 1e15) out += std::format("{}", static_cast<long long>(x));
            else out += std::format("{}", x);
        } else if constexpr (std::is_same_v<T, std::string>) {
            out.push_back('"');
            for (const char c : x) {
                if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
                else if (c == '\n') out += "\\n";
                else if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<int>(c));
                else out.push_back(c);
            }
            out.push_back('"');
        } else if constexpr (std::is_same_v<T, Json::Array>) {
            out.push_back('[');
            for (std::size_t i = 0; i < x.size(); ++i) {
                if (i) out.push_back(',');
                nl(level + 1);
                dump_(x[i], out, indent, level + 1);
            }
            if (!x.empty()) nl(level);
            out.push_back(']');
        } else {
            out.push_back('{');
            for (std::size_t i = 0; i < x.size(); ++i) {
                if (i) out.push_back(',');
                nl(level + 1);
                dump_(Json{x[i].first}, out, indent, level + 1);
                out += indent > 0 ? ": " : ":";
                dump_(x[i].second, out, indent, level + 1);
            }
            if (!x.empty()) nl(level);
            out.push_back('}');
        }
    }, j.v);
}

fs::path config_home_(const std::map<std::string, std::string>& env) {
    if (const auto it = env.find("XDG_CONFIG_HOME"); it != env.end() && !it->second.empty()) return it->second;
    if (const auto it = env.find("HOME"); it != env.end() && !it->second.empty()) return fs::path(it->second) / ".config";
    return {};
}

std::expected<std::optional<Json>, std::string> read_json_(const fs::path& file) {
    std::error_code ec;
    if (file.empty() || !fs::exists(file, ec)) return std::optional<Json>{};
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::unexpected(std::format("{}: cannot be read", file.string()));
    const std::string text((std::istreambuf_iterator<char>(in)), {});
    auto j = Json::parse(text);
    if (!j) return std::unexpected(std::format("{}: {}", file.string(), j.error()));
    if (!j->is_object()) return std::unexpected(std::format("{}: not an object", file.string()));
    return std::optional<Json>{std::move(*j)};
}

}  // namespace

std::expected<Json, std::string> Json::parse(std::string_view text) {
    Reader r{text};
    auto v = r.value();
    if (!v) return v;
    r.space();
    if (r.at != text.size()) return r.fail("something after the value");
    return v;
}

std::string Json::dump(int indent) const {
    std::string out;
    dump_(*this, out, indent, 0);
    return out;
}

const Json* Json::get(std::string_view key) const {
    if (const auto* o = std::get_if<Object>(&v))
        for (const auto& [k, val] : *o)
            if (k == key) return &val;
    return nullptr;
}

void Json::set(std::string_view key, Json value) {
    if (!is_object()) v = Object{};
    auto& o = std::get<Object>(v);
    for (auto& [k, val] : o)
        if (k == key) { val = std::move(value); return; }
    o.emplace_back(std::string(key), std::move(value));
}

std::string Json::string_or(std::string_view fallback) const {
    if (const auto* s = std::get_if<std::string>(&v)) return *s;
    return std::string(fallback);
}
double Json::number_or(double fallback) const {
    if (const auto* d = std::get_if<double>(&v)) return *d;
    return fallback;
}
bool Json::bool_or(bool fallback) const {
    if (const auto* b = std::get_if<bool>(&v)) return *b;
    return fallback;
}

fs::path state_file(const std::map<std::string, std::string>& env) {
    const auto home = config_home_(env);
    return home.empty() ? fs::path{} : home / "xdesk" / "state.json";
}
fs::path keys_file(const std::map<std::string, std::string>& env) {
    const auto home = config_home_(env);
    return home.empty() ? fs::path{} : home / "xdesk" / "keys.json";
}

std::expected<State, std::string> read_state(const std::map<std::string, std::string>& env) {
    auto j = read_json_(state_file(env));
    if (!j) return std::unexpected(j.error());
    State s;
    if (!*j) return s;
    const auto& doc = **j;
    if (const auto* a = doc.get("arrangements"))
        if (const auto* list = std::get_if<Json::Array>(&a->v))
            for (const auto& x : *list) s.arrangements.push_back(x.string_or(""));
    if (const auto* l = doc.get("last")) s.last = l->string_or("");
    if (const auto* h = doc.get("hinted")) s.hinted = h->bool_or(false);
    if (const auto* k = doc.get("keys_seen")) s.keys_seen = static_cast<int>(k->number_or(0));
    if (const auto* r = doc.get("rules"))
        if (const auto* o = std::get_if<Json::Object>(&r->v))
            for (const auto& [app, rule] : *o) {
                Rule out;
                if (const auto* f = rule.get("floating")) out.floating = f->bool_or(false);
                if (const auto* w = rule.get("workspace"); w && std::holds_alternative<double>(w->v)) out.workspace = static_cast<int>(w->number_or(0));
                if (const auto* rect = rule.get("rect"))
                    if (const auto* xs = std::get_if<Json::Array>(&rect->v); xs && xs->size() == 4) {
                        std::array<int, 4> r4{};
                        for (std::size_t k = 0; k < 4; ++k) r4[k] = static_cast<int>((*xs)[k].number_or(0));
                        out.rect = r4;
                    }
                s.rules[app] = out;
            }
    return s;
}

std::expected<void, std::string> write_state(const std::map<std::string, std::string>& env, const State& s) {
    const auto file = state_file(env);
    if (file.empty()) return std::unexpected(std::string("no home to keep the desktop's state in"));
    Json doc;
    Json::Array arr;
    for (const auto& a : s.arrangements) arr.push_back(Json{a});
    doc.set("arrangements", Json{arr});
    doc.set("last", Json{s.last});
    doc.set("hinted", Json{s.hinted});
    doc.set("keys_seen", Json{static_cast<double>(s.keys_seen)});
    Json rules{Json::Object{}};
    for (const auto& [app, r] : s.rules) {
        Json one{Json::Object{}};
        one.set("floating", Json{r.floating});
        if (r.workspace) one.set("workspace", Json{static_cast<double>(*r.workspace)});
        if (r.rect) one.set("rect", Json{Json::Array{Json{double((*r.rect)[0])}, Json{double((*r.rect)[1])}, Json{double((*r.rect)[2])},
                                                     Json{double((*r.rect)[3])}}});
        rules.set(app, std::move(one));
    }
    doc.set("rules", std::move(rules));
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const auto tmp = fs::path(file.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << doc.dump(1) << "\n";
        if (!out) return std::unexpected(std::format("{}: cannot be written", tmp.string()));
    }
    fs::rename(tmp, file, ec);
    if (ec) return std::unexpected(std::format("{}: {}", file.string(), ec.message()));
    return {};
}

std::expected<std::map<std::string, std::vector<std::string>>, std::string> read_keys(const std::map<std::string, std::string>& env) {
    auto j = read_json_(keys_file(env));
    if (!j) return std::unexpected(j.error());
    std::map<std::string, std::vector<std::string>> out;
    if (!*j) return out;
    for (const auto& [action, chords] : std::get<Json::Object>((**j).v)) {
        std::vector<std::string> list;
        if (const auto* a = std::get_if<Json::Array>(&chords.v))
            for (const auto& c : *a) list.push_back(c.string_or(""));
        else if (const auto* one = std::get_if<std::string>(&chords.v)) list.push_back(*one);
        std::erase(list, std::string{});
        out[action] = std::move(list);
    }
    return out;
}

std::expected<void, std::string> write_keys(const std::map<std::string, std::string>& env,
                                            const std::map<std::string, std::vector<std::string>>& keys) {
    const auto file = keys_file(env);
    if (file.empty()) return std::unexpected(std::string("no home to keep the keys in"));
    Json doc{Json::Object{}};
    for (const auto& [action, chords] : keys) {
        if (action == "set") {
            if (!chords.empty()) doc.set("set", Json{chords.front()});
            continue;
        }
        Json::Array list;
        for (const auto& c : chords) list.push_back(Json{c});
        doc.set(action, Json{list});
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const auto tmp = fs::path(file.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << doc.dump(1) << "\n";
        if (!out) return std::unexpected(std::format("{}: cannot be written", tmp.string()));
    }
    fs::rename(tmp, file, ec);
    if (ec) return std::unexpected(std::format("{}: {}", file.string(), ec.message()));
    return {};
}

}  // namespace xdesk::config
