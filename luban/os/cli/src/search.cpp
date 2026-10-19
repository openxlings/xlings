// What the index offers (design 2026-10-19 §6): `luban search`, the same
// answer the Luban OS quick start shows (luban.service::quick_start_offers).
module luban.cli;

import std;
import xlings.libs.json;
import xlings.i18n;
import luban.service;

namespace luban::cli {

namespace {

namespace svc = luban::service;
using Json = nlohmann::json;
using xlings::i18n::tr;

std::string lower_(std::string_view s) {
    std::string out(s);
    for (auto& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

bool offer_matches_(const svc::Offer& o, std::string_view keyword) {
    if (keyword.empty()) return true;
    const auto k = lower_(keyword);
    auto has = [&](std::string_view text) { return lower_(text).find(k) != std::string::npos; };
    if (has(o.ref) || has(o.name) || has(o.description)) return true;
    for (const auto& [_, t] : o.title) if (has(t)) return true;
    for (const auto& [_, t] : o.summary) if (has(t)) return true;
    for (const auto& w : o.keywords) if (has(w)) return true;
    return false;
}

std::string text_(const std::map<std::string, std::string>& texts, std::string_view fallback) {
    std::map<std::string, std::string, std::less<>> t(texts.begin(), texts.end());
    auto picked = xlings::i18n::pick(t);
    return picked.empty() ? std::string(fallback) : picked;
}

std::string shown_ref_(const svc::Offer& o) { return o.official && !o.name.empty() ? o.name : o.ref; }

}  // namespace

bool name_matches(std::string_view name, std::string_view pattern) {
    if (pattern.empty()) return true;
    const auto n = lower_(name), p = lower_(pattern);
    if (n.find(p) != std::string::npos) return true;
    std::size_t at = 0;
    for (const char ch : n)
        if (at < p.size() && ch == p[at]) ++at;
    return at == p.size();
}

int search(const svc::Context& c, std::string_view keyword, std::string_view group, bool json, bool verbose) {
    const auto offers = svc::quick_start_offers(c.port);
    struct Group { std::string_view key, title, hint; const std::vector<svc::Offer>* list; };
    const std::array<Group, 2> groups{{
        {"distributions", tr("luban.search.distributions"), tr("luban.search.distributions_hint"), &offers.systems},
        {"desktops", tr("luban.search.desktops"), tr("luban.search.desktops_hint"), &offers.desktops},
    }};
    // Could not read is never nothing offered: said, with its fix.
    if (!offers.error.empty()) {
        if (json) std::println("{}", Json{{"error", offers.error}, {"next", "xlings update"}}.dump());
        else std::println(std::cerr, "luban: {}\n  next: xlings update", xlings::i18n::trf("luban.search.unreadable", offers.error));
        return 1;
    }
    if (json) {
        Json out = Json::array();
        for (const auto& g : groups) {
            if (!group.empty() && g.key != group) continue;
            Json items = Json::array();
            for (const auto& o : *g.list)
                if (offer_matches_(o, keyword)) items.push_back(svc::offer_json(o));
            out.push_back({{"group", g.key}, {"items", std::move(items)}});
        }
        std::println("{}", Json{{"groups", std::move(out)}}.dump());
        return 0;
    }
    bool any = false;
    for (const auto& g : groups) {
        if (!group.empty() && g.key != group) continue;
        std::vector<const svc::Offer*> shown;
        for (const auto& o : *g.list)
            if (offer_matches_(o, keyword)) shown.push_back(&o);
        if (shown.empty()) continue;
        if (any) std::println("");
        any = true;
        std::println("{}  {}", g.title, g.hint);
        for (const auto* o : shown) {
            const auto tier = o->tier ? std::format("T{}", *o->tier) : std::string("-");
            std::string note;
            // Off the T1-T4 axis: an official one (xdesk) says what it is; another is not rated.
            if (g.key == "desktops" && !o->tier && o->source != "official") note = tr("luban.search.not_rated");
            if (o->gpu == "better") note += (note.empty() ? "" : " · ") + std::string(tr("luban.search.gpu_better"));
            if (o->gpu == "needed") note += (note.empty() ? "" : " · ") + std::string(tr("luban.search.gpu_needed"));
            if (!o->available) note += (note.empty() ? "" : " · ") + o->reason;
            const auto summary = text_(o->summary, {});
            if (note.empty() && !summary.empty()) note = summary;
            const auto source = o->source == "official" ? std::string(tr("luban.search.official")) : std::format("{} ({})", o->source, o->index);
            const auto status = o->status == "dev" ? std::string(tr("luban.search.preview")) : o->status;
            std::println("  {:<3} {:<16} {:<14} {:<10} {:<8} {}", tier, shown_ref_(*o), o->version.empty() ? "-" : o->version,
                         source, status, note);
            if (verbose)
                for (const auto& why : o->ignored) std::println("      {}", xlings::i18n::trf("luban.search.ignored", why));
            if (verbose && !o->passed_over.empty()) std::println("      {}", o->passed_over);
        }
    }
    if (!any) {
        if (keyword.empty()) std::println("{}", tr("luban.search.nothing"));
        else std::println("nothing matches '{}': luban search lists everything", keyword);
    }
    return 0;
}

}  // namespace luban::cli
