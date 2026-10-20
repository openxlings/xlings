// wuxian.json, version 1 (openwx.cppm, SPEC.md §4): read, written and
// checked. The JSON is read here, a subset enough for this shape (objects,
// arrays, strings, numbers, booleans, null): the specification depends on
// nothing but the standard library.
module openwx;

import std;

namespace openwx {

namespace {

struct Value {
    enum class Kind { null, boolean, number, string, array, object };
    Kind kind { Kind::null };
    bool b { false };
    double n { 0 };
    std::string s;
    std::vector<Value> a;
    std::vector<std::pair<std::string, Value>> o;
    std::string raw;   // the value as it was written

    const Value* get(std::string_view key) const {
        for (const auto& [k, v] : o)
            if (k == key) return &v;
        return nullptr;
    }
};

struct Reader {
    std::string_view s;
    std::size_t at { 0 };

    void space() { while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) ++at; }
    bool eat(char c) {
        space();
        if (at < s.size() && s[at] == c) { ++at; return true; }
        return false;
    }
    std::unexpected<std::string> fail(std::string_view what) const { return std::unexpected(std::format("{} at byte {}", what, at)); }

    static void utf8_(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xc0 | cp >> 6));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | cp >> 12));
            out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | cp >> 18));
            out.push_back(static_cast<char>(0x80 | (cp >> 12 & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    std::expected<std::uint32_t, std::string> hex4_() {
        std::uint32_t cp = 0;
        if (at + 4 > s.size() || std::from_chars(s.data() + at, s.data() + at + 4, cp, 16).ec != std::errc{})
            return fail("a bad \\u escape");
        at += 4;
        return cp;
    }

    std::expected<std::string, std::string> string() {
        if (!eat('"')) return fail("a string expected");
        std::string out;
        while (at < s.size() && s[at] != '"') {
            const char c = s[at++];
            if (static_cast<unsigned char>(c) < 0x20) return fail("a control character in a string");
            if (c != '\\') { out.push_back(c); continue; }
            if (at >= s.size()) break;
            switch (const char e = s[at++]) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    auto cp = hex4_();
                    if (!cp) return std::unexpected(cp.error());
                    // A surrogate pair is one character.
                    if (*cp >= 0xd800 && *cp < 0xdc00 && at + 1 < s.size() && s[at] == '\\' && s[at + 1] == 'u') {
                        at += 2;
                        auto low = hex4_();
                        if (!low) return std::unexpected(low.error());
                        if (*low < 0xdc00 || *low >= 0xe000) return fail("a lone surrogate");
                        *cp = 0x10000 + ((*cp - 0xd800) << 10) + (*low - 0xdc00);
                    }
                    utf8_(out, *cp);
                    break;
                }
                default: return fail(std::format("an unknown escape \\{}", e));
            }
        }
        if (at >= s.size()) return fail("a string without its end");
        ++at;
        return out;
    }

    std::expected<Value, std::string> value(int depth = 0) {
        if (depth > 32) return fail("nested too deep");
        space();
        if (at >= s.size()) return fail("a value expected");
        const auto from = at;
        Value v;
        const char c = s[at];
        if (c == '{') {
            ++at;
            v.kind = Value::Kind::object;
            if (!eat('}')) {
                do {
                    space();
                    auto k = string();
                    if (!k) return std::unexpected(k.error());
                    if (!eat(':')) return fail("':' expected");
                    auto item = value(depth + 1);
                    if (!item) return item;
                    v.o.emplace_back(std::move(*k), std::move(*item));
                } while (eat(','));
                if (!eat('}')) return fail("'}' expected");
            }
        } else if (c == '[') {
            ++at;
            v.kind = Value::Kind::array;
            if (!eat(']')) {
                do {
                    auto item = value(depth + 1);
                    if (!item) return item;
                    v.a.push_back(std::move(*item));
                } while (eat(','));
                if (!eat(']')) return fail("']' expected");
            }
        } else if (c == '"') {
            auto str = string();
            if (!str) return std::unexpected(str.error());
            v.kind = Value::Kind::string;
            v.s = std::move(*str);
        } else if (s.substr(at, 4) == "true" || s.substr(at, 5) == "false") {
            v.kind = Value::Kind::boolean;
            v.b = s[at] == 't';
            at += v.b ? 4 : 5;
        } else if (s.substr(at, 4) == "null") {
            at += 4;
        } else {
            const auto [end, ec] = std::from_chars(s.data() + at, s.data() + s.size(), v.n);
            if (ec != std::errc{} || !std::isfinite(v.n)) return fail("a value expected");
            at = static_cast<std::size_t>(end - s.data());
            v.kind = Value::Kind::number;
        }
        v.raw = std::string(s.substr(from, at - from));
        return v;
    }
};

// ── reading the shape ────────────────────────────────────────────────

std::expected<double, std::string> number_(const Value& v, std::string_view what) {
    if (v.kind != Value::Kind::number) return std::unexpected(std::format("{} is not a number", what));
    return v.n;
}

std::expected<int, std::string> integer_(const Value& v, std::string_view what) {
    auto n = number_(v, what);
    if (!n) return std::unexpected(n.error());
    if (*n != std::floor(*n) || std::abs(*n) > 1e9) return std::unexpected(std::format("{} is not an integer", what));
    return static_cast<int>(*n);
}

std::expected<std::string, std::string> string_(const Value& v, std::string_view what) {
    if (v.kind != Value::Kind::string) return std::unexpected(std::format("{} is not a string", what));
    return v.s;
}

std::expected<std::vector<double>, std::string> numbers_(const Value& v, std::size_t n, std::string_view what) {
    if (v.kind != Value::Kind::array || v.a.size() != n) return std::unexpected(std::format("{} is not {} numbers", what, n));
    std::vector<double> out;
    for (const auto& x : v.a) {
        auto d = number_(x, what);
        if (!d) return std::unexpected(d.error());
        out.push_back(*d);
    }
    return out;
}

std::expected<std::uint32_t, std::string> color_(const Value& v, std::string_view what) {
    auto s = string_(v, what);
    if (!s) return std::unexpected(s.error());
    std::uint32_t rgb = 0;
    if (s->size() != 7 || (*s)[0] != '#' || std::from_chars(s->data() + 1, s->data() + 7, rgb, 16).ec != std::errc{})
        return std::unexpected(std::format("{} is not a colour #rrggbb", what));
    return rgb;
}

std::expected<Workspace, std::string> workspace_(const Value& v, std::size_t k, std::string& layout) {
    const auto what = [k](std::string_view field) { return std::format("workspaces[{}].{}", k, field); };
    if (v.kind != Value::Kind::object) return std::unexpected(std::format("workspaces[{}] is not an object", k));
    Workspace w;
    const auto* id = v.get("id");
    if (!id) return std::unexpected(std::format("workspaces[{}] has no id", k));
    auto n = integer_(*id, what("id"));
    if (!n) return std::unexpected(n.error());
    w.id = *n;
    if (const auto* x = v.get("name")) {
        auto s = string_(*x, what("name"));
        if (!s) return std::unexpected(s.error());
        w.name = std::move(*s);
    }
    w.color = color_for(w.id);
    if (const auto* x = v.get("color")) {
        auto c = color_(*x, what("color"));
        if (!c) return std::unexpected(c.error());
        w.color = *c;
    }
    if (const auto* x = v.get("rule")) {
        auto s = string_(*x, what("rule"));
        if (!s) return std::unexpected(s.error());
        const auto r = rule_named(*s);
        if (!r) return std::unexpected(std::format("{} is '{}', not tile or stack", what("rule"), *s));
        w.rule = *r;
    }
    if (const auto* x = v.get("flow")) {
        auto s = string_(*x, what("flow"));
        if (!s) return std::unexpected(s.error());
        const auto f = flow_named(*s);
        if (!f) return std::unexpected(std::format("{} is '{}', not horizontal or vertical", what("flow"), *s));
        w.flow = *f;
    }
    if (const auto* x = v.get("origin")) {
        auto p = numbers_(*x, 2, what("origin"));
        if (!p) return std::unexpected(p.error());
        w.origin = {(*p)[0], (*p)[1]};
    }
    if (const auto* x = v.get("scale")) {
        auto d = number_(*x, what("scale"));
        if (!d) return std::unexpected(d.error());
        w.scale = *d;
    }
    if (const auto* x = v.get("z")) {
        auto z = integer_(*x, what("z"));
        if (!z) return std::unexpected(z.error());
        w.z = *z;
    }
    if (const auto* x = v.get("layout"); x && x->kind != Value::Kind::null) layout = x->raw;
    return w;
}

std::expected<WindowState, std::string> window_(const Value& v, std::size_t k) {
    const auto what = [k](std::string_view field) { return std::format("windows[{}].{}", k, field); };
    if (v.kind != Value::Kind::object) return std::unexpected(std::format("windows[{}] is not an object", k));
    WindowState w;
    if (const auto* m = v.get("match")) {
        if (m->kind != Value::Kind::object) return std::unexpected(std::format("{} is not an object", what("match")));
        if (const auto* x = m->get("app_id")) {
            auto s = string_(*x, what("match.app_id"));
            if (!s) return std::unexpected(s.error());
            w.app_id = std::move(*s);
        }
        if (const auto* x = m->get("title")) {
            auto s = string_(*x, what("match.title"));
            if (!s) return std::unexpected(s.error());
            w.title = std::move(*s);
        }
    }
    if (const auto* x = v.get("in"); x && x->kind != Value::Kind::null) {
        auto n = integer_(*x, what("in"));
        if (!n) return std::unexpected(n.error());
        w.in = *n;
    }
    if (const auto* x = v.get("free"); x && x->kind != Value::Kind::null) {
        auto r = numbers_(*x, 4, what("free"));
        if (!r) return std::unexpected(r.error());
        w.free = Rect{(*r)[0], (*r)[1], (*r)[2], (*r)[3]};
    }
    if (const auto* x = v.get("z")) {
        auto z = integer_(*x, what("z"));
        if (!z) return std::unexpected(z.error());
        w.z = *z;
    }
    if (const auto* x = v.get("flags")) {
        if (x->kind != Value::Kind::array) return std::unexpected(std::format("{} is not an array", what("flags")));
        for (const auto& f : x->a) {
            auto s = string_(f, what("flags"));
            if (!s) return std::unexpected(s.error());
            w.flags.push_back(std::move(*s));
        }
    }
    return w;
}

// ── writing ──────────────────────────────────────────────────────────

std::string quoted_(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                else out.push_back(c);
        }
    }
    return out + "\"";
}

std::string number_text_(double d) {
    if (d == std::floor(d) && std::abs(d) < 1e15) return std::format("{}", static_cast<long long>(d));
    return std::format("{}", d);
}

}  // namespace

std::expected<State, std::string> parse(std::string_view json) {
    Reader r{json};
    auto root = r.value();
    if (!root) return std::unexpected("wuxian.json: " + root.error());
    r.space();
    if (r.at != json.size()) return std::unexpected(std::format("wuxian.json: more after the state at byte {}", r.at));
    if (root->kind != Value::Kind::object) return std::unexpected(std::string("wuxian.json: not a JSON object"));
    const auto* version = root->get("wuxian");
    if (!version) return std::unexpected(std::string("wuxian.json: not a WuXian state (no \"wuxian\" version)"));
    auto v = integer_(*version, "wuxian");
    if (!v) return std::unexpected("wuxian.json: " + v.error());
    if (*v != kStateVersion)
        return std::unexpected(std::format("wuxian.json is version {}; this reads version {}", *v, kStateVersion));
    State s;
    auto fail = [](const std::string& e) { return std::unexpected("wuxian.json: " + e); };
    if (const auto* x = root->get("view")) {
        auto t = string_(*x, "view");
        if (!t) return fail(t.error());
        const auto view = view_named(*t);
        if (!view) return fail(std::format("view is '{}', not workspace or wuxian", *t));
        s.view = *view;
    }
    if (const auto* x = root->get("current")) {
        auto n = integer_(*x, "current");
        if (!n) return fail(n.error());
        s.current = *n;
    }
    if (const auto* c = root->get("camera")) {
        if (c->kind != Value::Kind::object) return fail("camera is not an object");
        for (const auto& [key, field] : {std::pair<std::string_view, double*>{"x", &s.camera.at.x}, {"y", &s.camera.at.y}, {"zoom", &s.camera.zoom}})
            if (const auto* x = c->get(key)) {
                auto d = number_(*x, std::format("camera.{}", key));
                if (!d) return fail(d.error());
                *field = *d;
            }
    }
    if (const auto* ws = root->get("workspaces")) {
        if (ws->kind != Value::Kind::array) return fail("workspaces is not an array");
        for (std::size_t k = 0; k < ws->a.size(); ++k) {
            std::string layout;
            auto w = workspace_(ws->a[k], k, layout);
            if (!w) return fail(w.error());
            if (!layout.empty()) s.layouts[w->id] = std::move(layout);
            s.workspaces.push_back(std::move(*w));
        }
    }
    if (const auto* ws = root->get("windows")) {
        if (ws->kind != Value::Kind::array) return fail("windows is not an array");
        for (std::size_t k = 0; k < ws->a.size(); ++k) {
            auto w = window_(ws->a[k], k);
            if (!w) return fail(w.error());
            s.windows.push_back(std::move(*w));
        }
    }
    return s;
}

std::string format(const State& s) {
    std::string out = std::format("{{\n  \"wuxian\": {},\n  \"view\": {},\n  \"current\": {},\n", s.version, quoted_(name(s.view)), s.current);
    out += std::format("  \"camera\": {{\"x\": {}, \"y\": {}, \"zoom\": {}}},\n", number_text_(s.camera.at.x), number_text_(s.camera.at.y),
                       number_text_(s.camera.zoom));
    out += "  \"workspaces\": [";
    for (std::size_t k = 0; k < s.workspaces.size(); ++k) {
        const auto& w = s.workspaces[k];
        out += k ? ",\n    " : "\n    ";
        out += std::format("{{\"id\": {}, \"name\": {}, \"color\": \"#{:06x}\", \"rule\": {}, \"flow\": {}, \"origin\": [{}, {}], "
                           "\"scale\": {}, \"z\": {}",
                           w.id, quoted_(w.name), w.color & 0xffffff, quoted_(name(w.rule)), quoted_(name(w.flow)), number_text_(w.origin.x),
                           number_text_(w.origin.y), number_text_(w.scale), w.z);
        if (const auto it = s.layouts.find(w.id); it != s.layouts.end() && !it->second.empty()) out += ", \"layout\": " + it->second;
        out += "}";
    }
    out += s.workspaces.empty() ? "],\n" : "\n  ],\n";
    out += "  \"windows\": [";
    for (std::size_t k = 0; k < s.windows.size(); ++k) {
        const auto& w = s.windows[k];
        out += k ? ",\n    " : "\n    ";
        out += std::format("{{\"match\": {{\"app_id\": {}", quoted_(w.app_id));
        if (!w.title.empty()) out += std::format(", \"title\": {}", quoted_(w.title));
        out += "}";
        if (w.in) out += std::format(", \"in\": {}", *w.in);
        if (w.free)
            out += std::format(", \"free\": [{}, {}, {}, {}]", number_text_(w.free->x), number_text_(w.free->y), number_text_(w.free->width),
                               number_text_(w.free->height));
        if (w.z != 0) out += std::format(", \"z\": {}", w.z);
        if (!w.flags.empty()) {
            out += ", \"flags\": [";
            for (std::size_t f = 0; f < w.flags.size(); ++f) out += (f ? ", " : "") + quoted_(w.flags[f]);
            out += "]";
        }
        out += "}";
    }
    out += s.windows.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

std::vector<std::string> validate(const State& s) {
    std::vector<std::string> out;
    if (s.version != kStateVersion) out.push_back(std::format("version {}, not {}", s.version, kStateVersion));
    if (s.workspaces.empty()) out.push_back("no workspace: the plane has at least one");
    std::set<int> ids;
    for (const auto& w : s.workspaces) {
        if (w.id <= 0) out.push_back(std::format("workspace {}: a number is a positive integer", w.id));
        if (!ids.insert(w.id).second) out.push_back(std::format("workspace {}: its number is used twice", w.id));
        if (!(w.scale >= kMinScale && w.scale <= kMaxScale))
            out.push_back(std::format("workspace {}: scale {} is outside {}-{}", w.id, w.scale, kMinScale, kMaxScale));
        if (w.color > 0xffffff) out.push_back(std::format("workspace {}: its colour is not #rrggbb", w.id));
    }
    if (!s.workspaces.empty() && !ids.contains(s.current)) out.push_back(std::format("current workspace {} is not there", s.current));
    if (!(s.camera.zoom >= kMinZoom && s.camera.zoom <= kMaxZoom))
        out.push_back(std::format("camera zoom {} is outside {}-{}", s.camera.zoom, kMinZoom, kMaxZoom));
    for (const auto& [id, _] : s.layouts)
        if (!ids.contains(id)) out.push_back(std::format("a layout for workspace {}, which is not there", id));
    for (std::size_t k = 0; k < s.windows.size(); ++k) {
        const auto& w = s.windows[k];
        if (w.in.has_value() == w.free.has_value())
            out.push_back(std::format("window {} ({}): in a workspace or on the plane, one of them", k, w.app_id));
        if (w.in && !ids.contains(*w.in)) out.push_back(std::format("window {} ({}): in workspace {}, which is not there", k, w.app_id, *w.in));
        if (w.free && (w.free->width <= 0 || w.free->height <= 0))
            out.push_back(std::format("window {} ({}): its frame on the plane has no size", k, w.app_id));
    }
    return out;
}

}  // namespace openwx
