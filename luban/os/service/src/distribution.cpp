// What a Luban system is made of and where it comes from (design 2026-10-18
// §4, §5): coordinates, lineages, names, confirmations (service.cppm).
module luban.service;

import std;
import luban.compat;
import xlings.libs.json;
import xlings.libs.sha256;
import xlings.platform;
import xlings.platform.target;
import opencaps;

namespace luban::service {

namespace {

using Json = nlohmann::json;

// A data event's payload: an object, or the JSON text of one.
Json payload_(const Json& d) {
    if (!d.contains("payload")) return Json::object();
    const auto& p = d["payload"];
    if (p.is_string()) {
        auto parsed = Json::parse(p.get<std::string>(), nullptr, false);
        return parsed.is_discarded() ? Json::object() : parsed;
    }
    return p;
}

// "luban:desktop@2026.10.19.1" -> {"luban", "desktop", "2026.10.19.1"}
std::array<std::string, 3> split_ref_(std::string_view ref) {
    std::string ns, name(ref), version;
    if (const auto at = name.find('@'); at != std::string::npos) { version = name.substr(at + 1); name.resize(at); }
    if (const auto colon = name.find(':'); colon != std::string::npos) { ns = name.substr(0, colon); name = name.substr(colon + 1); }
    return {ns, name, version};
}

// An official edition's short name: luban:core -> core, subos:luban-core -> core.
std::string short_of_(std::string_view ns, std::string_view name) {
    if (ns == "subos" && name.starts_with("luban-")) return std::string(name.substr(6));
    return std::string(name);
}

// A manifest's `luban` object (an empty one when it has none, or is no object).
Json luban_of_(const Json& manifest) {
    if (!manifest.is_object()) return Json::object();
    const auto l = manifest.find("luban");
    return l != manifest.end() && l->is_object() ? *l : Json::object();
}

std::string lower_(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

}  // namespace

std::string Coordinate::role() const {
    if (type == "subos") return "system";
    if (std::ranges::find(categories, "luban-desktop") != categories.end()) return "desktop";
    return "package";
}

std::expected<Coordinate, std::string> describe_coordinate(const Port& port, std::string_view coord) {
    std::vector<Json> data;
    std::string error;
    // A Luban system and what goes into it are Linux's, wherever luban runs
    // (a PC's or a Mac's is made in its carrier's machine).
    port.capability("package_info", Json{{"target", std::string(coord)}, {"platform", "linux"}},
                    [&](const Event& e) { if (e.kind == Event::Kind::Error && error.empty()) error = e.text; }, &data);
    for (const auto& d : data) {
        if (d.value("dataKind", "") != "info_panel") continue;
        const auto p = payload_(d);
        const auto pkg = p.value("package", Json::object());
        if (!pkg.is_object() || pkg.empty()) continue;
        Coordinate c{.given = std::string(coord), .package = pkg.value("package", std::string{}),
                     .ns = pkg.value("namespace", std::string{}), .name = pkg.value("name", std::string{}),
                     .version = pkg.value("version", std::string{}), .index = pkg.value("index", std::string{}),
                     .type = pkg.value("type", std::string{"package"}),
                     .description = pkg.value("description", std::string{}),
                     .passed_over = pkg.value("passed_over", std::string{})};
        for (const auto& cat : pkg.value("categories", Json::array()))
            if (cat.is_string()) c.categories.push_back(cat.get<std::string>());
        // The version asked for, when one was.
        if (const auto at = coord.find('@'); at != std::string_view::npos) c.version = std::string(coord.substr(at + 1));
        return c;
    }
    return std::unexpected(error.empty() ? std::format("{} is in no index this home has", coord) : error);
}

namespace {

// The official index's packages a search finds in `category` (one call; a
// search, unlike a lookup that misses, never refreshes the index).
std::vector<Json> official_in_(const Port& port, std::string_view keyword, std::string_view category) {
    std::vector<Json> data, out;
    port.capability("search_packages", Json{{"keyword", std::string(keyword)}, {"category", std::string(category)}},
                    nullptr, &data);
    for (const auto& d : data) {
        if (d.value("dataKind", "") != "packages") continue;
        for (const auto& p : payload_(d))
            if (p.is_object() && p.value("index", std::string{}) == kOfficialIndex) out.push_back(p);
    }
    return out;
}

}  // namespace

std::optional<std::string> official_system(const Port& port, std::string_view short_name) {
    // The package's own name of a published edition (luban-core) names it too.
    if (short_name.starts_with("luban-")) short_name.remove_prefix(6);
    const auto at = short_name.find('@');
    const auto name = std::string(short_name.substr(0, at));
    const auto version = at == std::string_view::npos ? std::string_view{} : short_name.substr(at);
    std::optional<std::string> frozen;
    for (const auto& p : official_in_(port, name, "distribution")) {
        const auto ns = p.value("namespace", std::string{}), n = p.value("name", std::string{});
        if (ns == "luban" && n == name) return std::format("luban:{}{}", name, version);
        if (luban::compat::frozen_package(ns, n, name)) frozen = luban::compat::frozen_ref(name, version);
    }
    return frozen;
}

std::optional<std::string> official_desktop(const Port& port, std::string_view name) {
    for (const auto& p : official_in_(port, name, "luban-desktop"))
        if (p.value("namespace", std::string{}) == "luban" && p.value("name", std::string{}) == name)
            return std::format("luban:{}", name);
    return std::nullopt;
}

std::map<std::string, std::string> official_system_refs(const Port& port) {
    std::map<std::string, std::string> out;
    for (const auto& p : official_in_(port, "", "distribution")) {
        const auto ns = p.value("namespace", std::string{}), n = p.value("name", std::string{});
        if (ns == "luban") out[n] = "luban:" + n;   // the luban: one wins
        else if (ns == "subos" && n.starts_with("luban-")) out.try_emplace(n.substr(6), "subos:" + n);
    }
    return out;
}

namespace {

// A table of languages ({en = "...", zh = "..."}) as text; nothing when it is not one.
std::map<std::string, std::string> texts_(const Json& j) {
    std::map<std::string, std::string> out;
    if (!j.is_object()) return out;
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.value().is_string() && !it.value().get<std::string>().empty()) out[it.key()] = it.value().get<std::string>();
    return out;
}

// What the index says of a package besides its name (design 2026-10-19 §5.3,
// §5.4): its version, status, source, and its extensions.luban -- each value
// in range taken, each other one named in `ignored`.
void read_offer_(const Json& p, Offer& o) {
    o.version = p.value("version", std::string{});
    o.passed_over = p.value("passed_over", std::string{});
    o.status = p.value("status", std::string{});
    o.source = o.official || o.index == kOfficialIndex ? "official" : o.index == "local" ? "local" : "community";
    if (auto k = p.find("keywords"); k != p.end() && k->is_array())
        for (const auto& w : *k)
            if (w.is_string()) o.keywords.push_back(w.get<std::string>());
    if (auto a = p.find("archs"); a != p.end() && a->is_array() && !a->empty()) {
        auto here = std::string(xlings::platform::build_arch());
        if (here == "arm64") here = "aarch64";
        bool found = false;
        for (const auto& arch : *a)
            if (arch.is_string()) {
                auto v = arch.get<std::string>();
                if (v == "arm64") v = "aarch64";
                if (v == "amd64" || v == "x64") v = "x86_64";
                found = found || v == here;
            }
        if (!found) {
            o.available = false;
            o.reason = std::format("not made for {}", here);
        }
    }
    const auto ext = p.value("extensions", Json::object());
    const auto luban = ext.is_object() ? ext.value("luban", Json::object()) : Json::object();
    if (!luban.is_object()) return;
    if (auto t = luban.find("tier"); t != luban.end()) {
        if (t->is_number_integer() && t->get<int>() >= 1 && t->get<int>() <= 4) o.tier = t->get<int>();
        else o.ignored.push_back(std::format("tier {}", t->dump()));
    }
    if (auto g = luban.find("gpu"); g != luban.end()) {
        if (g->is_string() && (g->get<std::string>() == "better" || g->get<std::string>() == "needed")) o.gpu = g->get<std::string>();
        else o.ignored.push_back(std::format("gpu {}", g->dump()));
    }
    if (auto t = luban.find("title"); t != luban.end()) {
        o.title = texts_(*t);
        if (o.title.empty()) o.ignored.push_back("title (not a table of languages)");
    }
    if (auto t = luban.find("summary"); t != luban.end()) {
        o.summary = texts_(*t);
        if (o.summary.empty()) o.ignored.push_back("summary (not a table of languages)");
    }
    if (auto h = luban.find("highlights"); h != luban.end() && h->is_array())
        for (const auto& x : *h)
            if (x.is_string()) o.highlights.push_back(x.get<std::string>());
    if (auto z = luban.find("size"); z != luban.end()) {
        if (z->is_number_unsigned() || (z->is_number_integer() && z->get<std::int64_t>() > 0)) o.size = z->get<std::uint64_t>();
        else o.ignored.push_back(std::format("size {}", z->dump()));
    }
}

}  // namespace

Json offer_json(const Offer& o) {
    Json j{{"ref", o.ref}, {"name", o.name}, {"version", o.version}, {"index", o.index}, {"source", o.source},
           {"status", o.status}, {"description", o.description}, {"available", o.available}};
    if (o.tier) j["tier"] = *o.tier;
    if (!o.gpu.empty()) j["gpu"] = o.gpu;
    if (!o.title.empty()) j["title"] = o.title;
    if (!o.summary.empty()) j["summary"] = o.summary;
    if (!o.highlights.empty()) j["highlights"] = o.highlights;
    if (o.size) j["size"] = *o.size;
    if (!o.passed_over.empty()) j["passed_over"] = o.passed_over;
    if (!o.available) j["reason"] = o.reason;
    if (!o.ignored.empty()) j["ignored"] = o.ignored;
    return j;
}

QuickStartOffers quick_start_offers(const Port& port) {
    QuickStartOffers out;
    // One search a category, both at once (each is an xlings call; the quick
    // start waits on them, design 2026-10-19 §4.1).
    struct Found { std::vector<Json> packages; std::string error; };
    auto search = [&port](std::string category) {
        Found f;
        std::vector<Json> data;
        const int code = port.capability("search_packages", Json{{"keyword", ""}, {"category", category}}, nullptr, &data);
        if (code != 0) {
            f.error = "the index could not be read";
            for (const auto& d : data)
                if (d.value("kind", "") == "error" || d.contains("message")) {
                    f.error = d.value("message", f.error);
                    break;
                }
        }
        for (const auto& d : data)
            if (d.value("dataKind", "") == "packages")
                for (const auto& p : payload_(d))
                    if (p.is_object()) f.packages.push_back(p);
        return f;
    };
    auto systems_found = std::async(std::launch::async, search, std::string("distribution"));
    auto desktops_found = std::async(std::launch::async, search, std::string("luban-desktop"));
    std::map<std::string, Found, std::less<>> results;
    results.emplace("distribution", systems_found.get());
    results.emplace("luban-desktop", desktops_found.get());
    for (const auto& [category, f] : results)
        if (!f.error.empty() && out.error.empty()) out.error = f.error;
    auto in = [&](std::string_view category) { return results.find(category)->second.packages; };
    auto offer = [](const Json& p) {
        const auto ns = p.value("namespace", std::string{}), name = p.value("name", std::string{});
        Offer o{.ref = ns + ":" + name, .name = name, .description = p.value("description", std::string{}),
                .index = p.value("index", std::string{})};
        o.official = o.index == kOfficialIndex && (ns == "luban" || (ns == "subos" && name.starts_with("luban-")));
        if (o.official) o.name = short_of_(ns, name);
        read_offer_(p, o);
        return o;
    };
    // Systems: an official short name once (luban: before a frozen subos:luban-*).
    for (const auto& p : in("distribution")) {
        auto o = offer(p);
        auto same = std::ranges::find_if(out.systems, [&](const Offer& x) { return x.official && o.official && x.name == o.name; });
        if (same == out.systems.end()) out.systems.push_back(std::move(o));
        else if (o.ref.starts_with("luban:")) *same = std::move(o);
    }
    for (const auto& p : in("luban-desktop")) out.desktops.push_back(offer(p));
    // A deprecated one is not offered.
    for (auto* list : {&out.systems, &out.desktops})
        std::erase_if(*list, [](const Offer& o) { return o.status == "deprecated"; });
    // The official first, then by name; desktops by tier first, the unrated
    // last (design 2026-10-19 §5.1).
    std::ranges::stable_sort(out.systems, [](const Offer& x, const Offer& y) {
        return std::tie(y.official, x.name) < std::tie(x.official, y.name);
    });
    std::ranges::stable_sort(out.desktops, [](const Offer& x, const Offer& y) {
        const int tx = x.tier.value_or(9), ty = y.tier.value_or(9);
        return std::tie(tx, y.official, x.name) < std::tie(ty, x.official, y.name);
    });
    return out;
}

std::vector<std::string> official_systems(const Port& port) {
    std::vector<Json> data;
    port.capability("search_packages", Json{{"keyword", ""}, {"category", "distribution"}}, nullptr, &data);
    std::vector<std::string> out;
    for (const auto& d : data) {
        if (d.value("dataKind", "") != "packages") continue;
        for (const auto& p : payload_(d)) {
            if (!p.is_object() || p.value("index", std::string{}) != kOfficialIndex) continue;
            const auto ns = p.value("namespace", std::string{}), name = p.value("name", std::string{});
            if (ns != "luban" && !(ns == "subos" && name.starts_with("luban-"))) continue;
            auto s = short_of_(ns, name);
            if (std::ranges::find(out, s) == out.end()) out.push_back(std::move(s));
        }
    }
    std::ranges::sort(out);
    return out;
}

bool Layer::official() const {
    const auto [ns, name, _] = split_ref_(ref);
    return index == kOfficialIndex && (ns == "luban" || (ns == "subos" && name.starts_with("luban-")));
}

std::string Layer::name() const {
    if (auto os = luban_of_(manifest).value("os", Json::object()); os.is_object() && os.contains("name") && os["name"].is_string())
        return os["name"].get<std::string>();
    const auto [ns, name, _] = split_ref_(ref);
    return official() ? short_of_(ns, name) : name;
}

std::string Layer::version() const { return split_ref_(ref)[2]; }

const Layer* Lineage::base() const {
    for (const auto& l : layers)
        if (l.official()) return &l;
    return nullptr;
}

const Layer* Lineage::parent() const { return layers.size() > 1 ? &layers[1] : nullptr; }

nlohmann::json Lineage::luban(std::string_view key) const {
    Json merged = Json::object();
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {   // bottom first; the upper one wins
        const auto l = luban_of_(it->manifest);
        if (!l.contains(key)) continue;
        if (l[key].is_object()) for (auto e = l[key].begin(); e != l[key].end(); ++e) merged[e.key()] = e.value();
        else merged = l[key];
    }
    return merged;
}

std::vector<std::string> Lineage::needs() const {
    std::vector<std::string> out;
    for (const auto& l : layers)
        for (const auto& n : luban_of_(l.manifest).value("requires", Json::array()))
            if (n.is_string() && std::ranges::find(out, n.get<std::string>()) == out.end()) out.push_back(n.get<std::string>());
    return out;
}

std::expected<Lineage, std::string> plan_system(const Port& port, std::string_view name, std::string_view ref) {
    Lineage l;
    std::string error;
    bool planned = false;
    const std::vector<std::string> argv{"new", std::string(name), "--from", std::string(ref), "--dry-run"};
    const int rc = port.subos(argv, false, [&](const Event& e) {
        if (e.kind == Event::Kind::Error && error.empty()) error = e.text;
        if (e.kind == Event::Kind::Data && e.data_kind == "passed_over") {
            const auto d = e.data.is_string() ? Json::parse(e.data.get<std::string>(), nullptr, false) : e.data;
            if (d.is_object()) l.passed_over = d.value("message", std::string{});
            return;
        }
        if (e.kind != Event::Kind::Data || e.data_kind != "subos_plan") return;
        const auto d = e.data.is_string() ? Json::parse(e.data.get<std::string>(), nullptr, false) : e.data;
        if (!d.is_object()) return;
        planned = true;
        for (const auto& layer : d.value("layers", Json::array()))
            l.layers.push_back({layer.value("ref", std::string{}), layer.value("index", std::string{}),
                                layer.value("manifest", Json::object())});
        for (const auto& p : d.value("packages", Json::array()))
            if (p.is_string()) l.packages.push_back(p.get<std::string>());
        l.policy = d.value("policy", std::string{});
    });
    if (rc != 0 || !planned)
        return std::unexpected(error.empty() ? std::format("{} could not be read (exit {})", ref, rc) : error);
    return l;
}

Lineage lineage_of(const Environment& e) {
    Lineage l;
    if (e.edition.empty()) return l;
    // A record from before the indexes were kept: a published Luban name is official.
    auto index_of = [](const std::string& ref, const std::string& recorded) {
        if (!recorded.empty()) return recorded;
        return ref.starts_with("luban:") || luban::compat::frozen_edition(ref) ? std::string(kOfficialIndex) : std::string{};
    };
    l.layers.push_back({e.edition, index_of(e.edition, e.edition_index), e.manifest});
    for (std::size_t i = 0; i < e.chain.size(); ++i)
        l.layers.push_back({e.chain[i], index_of(e.chain[i], i < e.chain_index.size() ? e.chain_index[i] : std::string{}),
                            Json::object()});
    return l;
}

std::string display_name(const Lineage& l) {
    if (l.layers.empty()) return {};
    const auto& top = l.layers.front();
    if (top.official()) return std::format("Luban {} {}", top.name(), top.version());
    const auto* base = l.base();
    return base ? std::format("{} {} (Luban {})", top.name(), top.version(), base->name())
                : std::format("{} {}", top.name(), top.version());
}

std::string os_release(const Lineage& l) {
    if (l.layers.empty() || l.layers.front().official()) return {};
    const auto& top = l.layers.front();
    const auto os = l.luban("os");
    const auto name = os.value("name", top.name());
    auto id = lower_(os.value("id", name));
    std::ranges::replace(id, ' ', '-');
    const auto* base = l.base();
    std::string out = std::format("NAME=\"{}\"\nID={}\nID_LIKE=luban\nVERSION_ID={}\nPRETTY_NAME=\"{}\"\n",
                                  name, id, top.version(), display_name(l));
    if (base) out += std::format("LUBAN_BASE=\"{} {}\"\n", base->name(), base->version());
    return out;
}

std::vector<Beyond> beyond_luban(const Lineage& l) {
    std::vector<Beyond> out;
    for (const auto& layer : l.layers) {
        if (layer.official()) continue;
        if (!layer.manifest.is_object()) continue;
        if (auto p = layer.manifest.find("policy"); p != layer.manifest.end() && p->is_string())
            out.push_back({"policy", std::format("its isolation is {}", p->get<std::string>()), layer.ref, true});
        if (const auto s = luban_of_(layer.manifest).value("settings", Json::object()); s.is_object() && !s.empty()) {
            std::string text;
            for (auto e = s.begin(); e != s.end(); ++e)
                text += (text.empty() ? "" : ", ") + e.key() + " " + (e.value().is_string() ? e.value().get<std::string>() : e.value().dump());
            out.push_back({"settings", text, layer.ref, false});
        }
        const auto packages = layer.manifest.value("packages", Json::array());
        if (!packages.empty())
            out.push_back({"packages", std::format("{} package(s) of its own", packages.size()), layer.ref, false});
    }
    std::ranges::stable_sort(out, [](const Beyond& a, const Beyond& b) { return a.attention > b.attention; });
    return out;
}

std::string confirmation_key(const Lineage& l, std::span<const Coordinate> extra) {
    Json what = Json::array();
    for (const auto& layer : l.layers)
        if (!layer.official()) what.push_back({{"ref", layer.ref}, {"manifest", layer.manifest}});
    for (const auto& c : extra)
        if (!c.official()) what.push_back({{"ref", c.ref()}, {"index", c.index}});
    if (what.empty()) return {};
    return xlings::sha256::hex(what.dump());
}

namespace {
fs::path confirmed_file_(const fs::path& home) { return home / "config" / "luban" / "confirmed.json"; }
}

std::expected<bool, std::string> is_confirmed(const fs::path& home, std::string_view key) {
    const auto file = confirmed_file_(home);
    std::error_code ec;
    if (!fs::exists(file, ec)) return false;
    const auto j = Json::parse(xlings::platform::read_file_to_string(file.string()), nullptr, false);
    if (!j.is_object())
        return std::unexpected(std::format("{} cannot be read; fix it or remove it (what was confirmed is asked again)", file.string()));
    return j.contains(std::string(key));
}

std::expected<void, std::string> record_confirmed(const fs::path& home, std::string_view key, std::string_view label) {
    const auto file = confirmed_file_(home);
    std::error_code ec;
    Json j = Json::object();
    if (fs::exists(file, ec)) {
        j = Json::parse(xlings::platform::read_file_to_string(file.string()), nullptr, false);
        if (!j.is_object()) return std::unexpected(std::format("{} cannot be read; fix it or remove it", file.string()));
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    j[std::string(key)] = Json{{"what", std::string(label)}, {"at", std::format("{:%Y-%m-%dT%H:%M:%SZ}", now)}};
    fs::create_directories(file.parent_path(), ec);
    xlings::platform::write_file_atomic(file.string(), j.dump(2) + "\n");
    return {};
}

}  // namespace luban::service
