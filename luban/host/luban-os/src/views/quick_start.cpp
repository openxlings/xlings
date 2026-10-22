// The quick start (design 2026-10-19 §5): what can be made, in four groups --
// consoles, xdesk, the traditional desktops, more systems -- and making it.
module luban.os;

import std;
import xlings.libs.json;
import xlings.i18n;
import imkit;
import tray;
import luban.service;
import luban.vm;
import xdesk.term;
import opensal;
import opensal.wire;
import xlings.subos.home_view;
import xlings.subos.located;
import xlings.subos.policy_store;
import xlings.subos.policy;
import xlings.subos.root_etc;
import luban.service.archive;
import xlings.libs.sha512;
import xlings.observe;
import xlings.platform;
import xlings.platform.target;
import luban.desktop.host;
import opensal.wlr;
import :core;

namespace luban::os {

// Asks the index, once, what of each edition can be had here.
void ask_availability_(App& a) {
    if (a.availability_job) return;
    auto* app = &a;
    a.availability_job = std::make_unique<std::jthread>([app, ctx = a.ctx](std::stop_token stop) {
        // What the indexes offer first -- what the page waits on (design
        // 2026-10-19 §4.1: within a second): systems and desktops, one search
        // each. Which package each short name is (luban:<name>, or a
        // published subos:luban-<name>) comes with it.
        auto offers = service::quick_start_offers(ctx.port);
        std::map<std::string, std::string> refs;
        for (const auto& o : offers.systems)
            if (o.official) refs[o.name] = o.ref;
        {
            std::lock_guard l(app->available_m);
            app->offers = std::move(offers);
        }
        if (!stop.stop_requested() && app->window) app->window->wake();
        // What this computer can do: the same report `luban status` gives.
        {
            auto caps = service::capabilities(service::gather(ctx.home, ctx.port));
            std::lock_guard l(app->available_m);
            app->caps = std::move(caps);
        }
        if (!stop.stop_requested() && app->window) app->window->wake();
        const std::string arch(platform::build_arch());
        for (const auto& e : kEditions) {
            if (stop.stop_requested()) return;   // the window is closing
            Availability v{.known = true, .arch = arch};
            const auto it = refs.find(std::string(e.edition()));
            if (auto got = service::edition_availability(ctx, it != refs.end() ? it->second : "luban:" + std::string(e.edition()))) {
                v.listed = got->listed;
                v.here = !got->listed || got->archs.empty() || std::ranges::find(got->archs, arch) != got->archs.end();
            }   // the index could not be asked: offered, and making it says why
            std::lock_guard l(app->available_m);
            app->available[std::string(e.name)] = v;
        }
        if (!stop.stop_requested() && app->window) app->window->wake();
    });
}

// Whether this computer can run a Luban system (a sandbox or a machine is
// ready), as the capability report said; until it has, what this platform
// could always do.
bool can_run_(App& a) {
    std::lock_guard l(a.available_m);
    if (!a.caps) return platform::is_linux;
    const auto* s = a.caps->mode("sandbox");
    const auto* m = a.caps->mode("machine");
    return (s && s->ready) || (m && m->ready);
}

// Why this computer cannot run a Luban system, with the next step; nothing
// when it can, or while that is not known yet.
std::string cannot_run_(App& a) {
    if (can_run_(a)) return {};
    std::lock_guard l(a.available_m);
    std::string out(tr("luban.os.quick.computer_cannot_run_luban_systems"));
    const auto* s = a.caps ? a.caps->mode("sandbox") : nullptr;
    if (!s) return out + std::string(tr("luban.os.quick.they_are_linux_systems_need"));
    out += s->reason;
    if (!s->route.empty()) out += std::string(tr("luban.os.quick.do")) + s->route;
    return out;
}

// Why this edition cannot be made here, or nothing.
std::string unavailable_(App& a, const Edition& e) {
    if (auto why = cannot_run_(a); !why.empty()) return why;
    std::lock_guard l(a.available_m);
    const auto it = a.available.find(std::string(e.name));
    if (it == a.available.end() || !it->second.known) return {};
    if (!it->second.listed) return std::string(tr("luban.os.quick.not_published_yet"));
    if (!it->second.here)
        return std::vformat(tr("luban.os.quick.not_built_yet"), std::make_format_args(it->second.arch));
    return {};
}

// What the indexes offer of a system (by its ref) or a desktop (by its place), once they have answered.
std::optional<service::Offer> offered_system_(App& a, std::string_view ref) {
    std::lock_guard l(a.available_m);
    if (!a.offers) return std::nullopt;
    auto it = std::ranges::find(a.offers->systems, ref, &service::Offer::ref);
    if (it == a.offers->systems.end()) return std::nullopt;
    return *it;
}
std::optional<service::Offer> offered_desktop_(App& a, int k) {
    std::lock_guard l(a.available_m);
    if (!a.offers || k < 0 || k >= static_cast<int>(a.offers->desktops.size())) return std::nullopt;
    return a.offers->desktops[static_cast<std::size_t>(k)];
}
// An official edition's description as its index says it (the cards' own words otherwise).
std::string index_description_(App& a, std::string_view edition) {
    std::lock_guard l(a.available_m);
    if (!a.offers) return {};
    for (const auto& o : a.offers->systems)
        if (o.official && o.name == edition) return o.description;
    return {};
}

// ── the cards ────────────────────────────────────────────────────────

namespace {

// The bases a traditional desktop is made on (design 2026-10-19 §5.2).
constexpr std::array<std::string_view, 3> kBases{"tiny", "core", "desktop"};

// A data text in this language (a recipe's extensions.luban title, summary).
std::string pick_(const std::map<std::string, std::string>& texts, std::string_view otherwise) {
    const std::map<std::string, std::string, std::less<>> t(texts.begin(), texts.end());
    auto out = xlings::i18n::pick(t);
    return out.empty() ? std::string(otherwise) : out;
}

std::string source_of_(const service::Offer& o) {
    if (o.source == "official" || (o.source.empty() && o.official)) return std::string(tr("luban.os.quick.official"));
    const auto ns = o.ref.substr(0, o.ref.find(':'));
    return std::format("{} · {}", tr(o.source == "local" ? "luban.os.quick.local" : "luban.os.quick.community"), ns);
}

// The line of chips under a desktop's summary: how much it brings, where it
// comes from, how settled it is, what it wants of the GPU, its version.
std::string chips_(const service::Offer& o) {
    std::string out = o.tier ? std::format("T{}", *o.tier) : std::string(tr("luban.os.quick.not_rated"));
    out += " · " + source_of_(o);
    out += std::format(" · {}", tr(o.status == "dev" ? "luban.os.quick.preview" : "luban.os.quick.stable"));
    if (o.gpu == "better") out += std::format(" · {}", tr("luban.os.quick.gpu_better"));
    if (o.gpu == "needed") out += std::format(" · {}", tr("luban.os.quick.gpu_needed"));
    if (!o.version.empty()) out += " · " + o.version;
    return out;
}

// The term a card's title is, when the glossary explains it.
std::optional<std::string_view> explained_(std::string_view title) {
    for (const auto& t : xlings::i18n::glossary("luban.os"))
        if (title.find(t.term) != std::string_view::npos) return xlings::i18n::explain(t.term);
    return std::nullopt;
}

}  // namespace

std::vector<QuickGroup> quick_groups_(App& a) {
    std::vector<QuickGroup> out{{.key = "console"}, {.key = "xdesk"}, {.key = "desktops"}, {.key = "more"}};
    for (std::size_t i = 0; i < kEditions.size(); ++i)
        out[kEditions[i].desktop ? 1 : 0].cards.push_back({.kind = QuickCard::Kind::Edition, .edition = static_cast<int>(i),
                                                            .id = "card:" + std::string(kEditions[i].name)});
    std::lock_guard l(a.available_m);
    if (a.offers) {
        // Every desktop of the luban-desktop category but xdesk (its group is above), by tier.
        for (const auto& d : a.offers->desktops)
            if (!(d.official && d.name == "xdesk")) out[2].cards.push_back({.kind = QuickCard::Kind::Desktop, .offer = d, .id = "card:" + d.ref});
        // The systems this window has no card for: the official new ones first, then by name.
        for (const auto& o : a.offers->systems)
            if (!o.official || std::ranges::none_of(kEditions, [&](const Edition& e) { return e.name == o.name; }))
                out[3].cards.push_back({.kind = QuickCard::Kind::System, .offer = o, .id = "card:" + o.ref});
    }
    return out;
}

std::string chosen_card_(const App& a) {
    if (!a.new_trad.empty()) return "card:" + a.new_trad;
    if (!a.new_offer.empty()) return "card:" + a.new_offer;
    return "card:" + std::string(kEditions[static_cast<std::size_t>(a.new_edition)].name);
}

void choose_card_(App& a, const QuickCard& c) {
    a.new_trad.clear();
    a.new_offer.clear();
    a.trad_heading = false;
    if (c.kind == QuickCard::Kind::Desktop) a.trad_open = true;
    if (c.kind == QuickCard::Kind::Edition) a.new_edition = c.edition;
    else if (c.kind == QuickCard::Kind::Desktop) a.new_trad = c.offer.ref;
    else a.new_offer = c.offer.ref;
}

// ←→ in a group, ↑↓ between groups, Enter makes it, Escape goes back.
void quick_keys_(App& a) {
    const auto groups = quick_groups_(a);
    const auto id = chosen_card_(a);
    std::size_t g = 0, k = 0;
    for (std::size_t i = 0; i < groups.size(); ++i)
        for (std::size_t j = 0; j < groups[i].cards.size(); ++j)
            if (groups[i].cards[j].id == id) g = i, k = j;
    // The folded traditional desktops: one stop, its heading (WuXian §12.1).
    const auto folded = [&](std::size_t i) { return groups[i].key == "desktops" && !a.trad_open && !groups[i].cards.empty(); };
    if (a.trad_heading)
        for (std::size_t i = 0; i < groups.size(); ++i)
            if (groups[i].key == "desktops") g = i, k = 0;
    for (const auto& key : a.window->keys()) {
        if (a.trad_heading && (key.name == "Right" || key.name == "Enter")) {
            // Opened, its first card chosen.
            a.trad_open = true;
            choose_card_(a, groups[g].cards.front());
            a.keyboard = true;
        } else if (key.name == "Right" || key.name == "Left") {
            const auto n = groups[g].cards.size();
            if (n == 0 || a.trad_heading) continue;
            k = (k + (key.name == "Right" ? 1 : n - 1)) % n;
            choose_card_(a, groups[g].cards[k]);
            a.keyboard = true;
        } else if (key.name == "Down" || key.name == "Up") {
            // The next group that has cards, at the same place (or its last);
            // a folded one stops on its heading.
            for (std::size_t step = 1; step < groups.size(); ++step) {
                const auto h = key.name == "Down" ? (g + step) % groups.size() : (g + groups.size() - step) % groups.size();
                if (groups[h].cards.empty()) continue;
                g = h;
                a.keyboard = true;
                if (folded(h)) {
                    a.trad_heading = true;
                    break;
                }
                k = std::min(k, groups[h].cards.size() - 1);
                choose_card_(a, groups[g].cards[k]);
                break;
            }
        } else if (key.name == "Enter" && a.quick_can_make && a.new_name[0]) {
            make_(a);
        } else if (key.name == "Escape" && !a.systems.empty()) {
            a.view = View::Launcher;
        }
    }
}

// Makes the system the quick start describes, then goes in.
void make_(App& a, bool agreed) {
    const auto& chosen = kEditions[static_cast<std::size_t>(a.new_edition)];
    const auto offer = a.new_offer.empty() ? std::nullopt : offered_system_(a, a.new_offer);
    std::optional<service::Offer> trad;
    if (!a.new_trad.empty()) {
        std::lock_guard l(a.available_m);
        if (a.offers)
            if (auto it = std::ranges::find(a.offers->desktops, a.new_trad, &service::Offer::ref); it != a.offers->desktops.end()) trad = *it;
    }
    service::NewOptions o{.name = a.new_name, .edition = std::string(chosen.edition()), .proxy = a.new_proxy, .user = a.new_user,
                          .agreed = agreed, .from_window = true};
    if (trad) {
        // A traditional desktop on its base (design 2026-10-19 §5.2): the
        // same as `luban new --edition <base> --desktop <it>`, entered as a desktop.
        o.edition = std::string(kBases[static_cast<std::size_t>(std::clamp(a.new_base, 0, 2))]);
        if (trad->official) o.settings.emplace_back("desktop", trad->name);
        else o.xpkgs.push_back(trad->ref);
        o.settings.emplace_back("enter", "desktop");
    } else {
        if (!offer && !chosen.session.empty()) o.settings.emplace_back("desktop", std::string(chosen.session));
        if (offer) {
            // An official one by its short name; anyone else's by its coordinate.
            o.edition = offer->official ? offer->name : std::string{};
            if (!offer->official) o.xpkgs.push_back(offer->ref);
        }
        // A desktop chosen: Luban's by its name (made with it, its default), another's installed.
        if (const auto desktop = offered_desktop_(a, a.new_desktop)) {
            if (desktop->official) o.settings.emplace_back("desktop", desktop->name);
            else o.xpkgs.push_back(desktop->ref);
        }
    }
    const bool third_party = !o.xpkgs.empty();
    if (third_party && !agreed) {
        // A third party's part is agreed to first (design §4.5): what it is and where it comes from.
        std::string what;
        for (const auto& r : o.xpkgs) what += (what.empty() ? "" : ", ") + r;
        a.confirm = Confirm{std::string(tr("luban.os.quick.not_luban")),
                            std::vformat(tr("luban.os.quick.comes_index_you_added_not"),
                                         std::make_format_args(what)),
                            std::string(tr("luban.os.quick.make")), false, [&a] { make_(a, true); }};
        return;
    }
    if (a.new_password[0]) o.password_hash = xlings::sha512::crypt(a.new_password, xlings::sha512::random_salt());
    const bool machine = a.new_mode == 1, goes_in = offer || trad ? true : chosen.goes_in;
    static constexpr std::array<std::string_view, 3> kPersist{"all", "home", "none"};
    const std::string persist(kPersist[static_cast<std::size_t>(std::clamp(a.new_persist, 0, 2))]);
    const std::string name = o.name;
    a.start_job(std::format("{} {}", tr("luban.os.quick.making"), name),
                [o, machine, persist](Job& j, service::Context c) {
                    auto out = service::create(c, o);
                    j.code = out.code;
                    std::vector<std::pair<std::string, std::string>> changes;
                    if (machine) changes.emplace_back("mode", "machine");
                    if (persist != "all") changes.emplace_back("persist", persist);
                    if (out.ok() && !changes.empty()) (void)service::configure(c, o.name, changes);
                },
                [name, goes_in](App& app) {
                    if (app.job->code != 0) return;
                    app.refresh();
                    // Made is entered (design 2026-10-16 L §13.5): a console or
                    // a desktop; nano, its commands.
                    if (goes_in) { app.enter(name); return; }
                    app.settings.last_used[name] = xlings::observe::utc_now();
                    write_app_settings(app.home, app.settings);
                    app.open_run(name);
                });
    a.job_subject = name;
}

// The chosen card's panel, what it is and its settings (`panel_body_`), and
// making it (`panel_actions_`): at (x, y) in the current window, `w` wide;
// each returns its height.
float panel_body_(App& a, float x, float y, float w) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o(ImGui::GetWindowPos().x - ImGui::GetScrollX(), ImGui::GetWindowPos().y - ImGui::GetScrollY());
    const auto& chosen = kEditions[static_cast<std::size_t>(a.new_edition)];
    // A system from the indexes is said in its index's words, with where it comes from.
    const auto offer = a.new_offer.empty() ? std::nullopt : offered_system_(a, a.new_offer);
    std::optional<service::Offer> trad;
    {
        std::lock_guard l(a.available_m);
        if (a.offers && !a.new_trad.empty())
            if (auto it = std::ranges::find(a.offers->desktops, a.new_trad, &service::Offer::ref); it != a.offers->desktops.end()) trad = *it;
    }
    const auto base = std::string(kBases[static_cast<std::size_t>(std::clamp(a.new_base, 0, 2))]);
    // The version made is the version shown (design 2026-10-19 §5.6, §9.1).
    const auto system_offer = offer ? offer : offered_system_(a, "luban:" + (trad ? base : std::string(chosen.edition())));
    const std::string badge_name = trad ? std::string("desktop") : offer ? offer->ref : std::string(chosen.name);
    std::string title = offer ? offer->name : std::string(tr(chosen.title));
    std::vector<std::string> subs{std::format("{} · {}", chosen.size, chosen.contents)};
    if (trad) {
        title = pick_(trad->title, trad->name);
        subs = {std::vformat(tr("luban.os.quick.system_line"), std::make_format_args(base, system_offer ? system_offer->version : std::string("-"))),
                std::vformat(tr("luban.os.quick.desktop_line"), std::make_format_args(title, trad->version))};
    } else if (offer) {
        subs = {offer->official ? std::string(tr("luban.os.quick.luban_s")) : std::vformat(tr("luban.os.quick.from"), std::make_format_args(offer->index))};
    } else if (chosen.desktop) {
        // What its desktop is, as the index says it (E15): not this window's guess.
        if (auto d = index_description_(a, chosen.name); !d.empty()) subs = {std::format("{} · {}", chosen.size, d)};
    }
    if (!trad && system_offer && !system_offer->version.empty()) subs.front() = system_offer->version + " · " + subs.front();
    if (system_offer && !system_offer->passed_over.empty()) subs.push_back(system_offer->passed_over);
    const float pad = px(18), fw = w - pad * 2;
    float cy = y + px(16);
    const float start = y;
    const float cx = x + pad;
    art::badge(dl, art::badge_of(badge_name), badge_name, ImVec2(o.x + cx, o.y + cy), std::round(px(3)), imkit::beat(2, 0.45));
    imkit::text(dl, ImVec2(o.x + cx + px(60), o.y + cy + px(2)), imkit::fit(title, fw - px(60), 22, true), th_().text, 22, true);
    float sy = cy + px(30);
    for (const auto& line : subs) {
        imkit::text(dl, ImVec2(o.x + cx + px(60), o.y + sy), imkit::fit(line, fw - px(60), 13), th_().text_faint, 13);
        sy += px(18);
    }
    cy = std::max(cy + px(62), sy + px(8));
    std::string why_not = offer ? cannot_run_(a) : unavailable_(a, chosen);
    if (trad) {
        why_not = cannot_run_(a);
        if (why_not.empty() && !trad->available) why_not = trad->reason;
    }
    const std::string detail = trad ? pick_(trad->summary, trad->description) : offer ? offer->description : std::string(tr(chosen.detail));
    a.quick_said = why_not.empty() ? detail : why_not;
    a.quick_can_make = why_not.empty();
    cy += paragraph_(dl, ImVec2(o.x + cx, o.y + cy), a.quick_said, fw, why_not.empty() ? th_().text_dim : th_().warn, 14) + px(12);
    // The name follows the card until the person types one of their own.
    if (a.new_name[0] == 0 || a.auto_name == a.new_name) {
        auto stem = trad ? trad->name : offer ? offer->name : std::string(chosen.name);
        if (const auto colon = stem.find(':'); colon != std::string::npos) stem = stem.substr(colon + 1);
        for (int k = 1; k < 100; ++k) {
            const auto candidate = std::format("{}-{}", stem, k);
            // Not one there is, nor one being made now or left half made (design 2026-10-20 WuXian §11).
            if (std::ranges::none_of(a.systems, [&](const auto& s2) { return s2.name == candidate; })
                && !service::name_reserved(a.ctx.home, candidate) && !service::name_left_over(a.ctx.home, candidate)) {
                std::snprintf(a.new_name, sizeof a.new_name, "%s", candidate.c_str());
                a.auto_name = candidate;
                break;
            }
        }
    }
    auto label = [&](std::string_view s2) {
        imkit::text(dl, ImVec2(o.x + cx, o.y + cy), s2, th_().text_dim, 13, true);
        cy += px(20);
    };
    const float fwn = fw / imkit::scale();
    label(tr("luban.os.quick.name"));
    ImGui::SetCursorPos(ImVec2(cx, cy));
    imkit::text_field("name", a.new_name, sizeof a.new_name, fwn);
    mark_(a, "name");
    cy += px(48);
    label(tr("luban.os.admin.runs"));
    ImGui::SetCursorPos(ImVec2(cx, cy));
    a.new_mode = imkit::segmented("mode", {std::string(tr("luban.os.app.sandbox")), std::string(tr("luban.os.app.machine"))}, a.new_mode, fwn,
                                  {"mode:sandbox", "mode:machine"});
    cy += px(46);
    ImGui::SetCursorPos(ImVec2(cx, cy));
    imkit::toggle("more", tr("luban.os.quick.more_options"), a.more);
    mark_(a, "more");
    cy += px(36);
    if (a.more) {
        if (trad) {
            // What the desktop is made on: tiny, core (the default), or desktop.
            label(tr("luban.os.quick.base"));
            ImGui::SetCursorPos(ImVec2(cx, cy));
            a.new_base = imkit::segmented("base", {"tiny", "core", "desktop"}, a.new_base, fwn, {"base:tiny", "base:core", "base:desktop"});
            cy += px(46);
        }
        label(tr("luban.os.admin.keeps_2"));
        ImGui::SetCursorPos(ImVec2(cx, cy));
        a.new_persist = imkit::segmented("persist", {std::string(tr("luban.os.admin.all")), std::string(tr("luban.os.admin.home")),
                                                     std::string(tr("luban.os.admin.nothing"))},
                                         a.new_persist, fwn);
        cy += px(46);
        label(tr("luban.os.quick.proxy_socks5h_optional"));
        ImGui::SetCursorPos(ImVec2(cx, cy));
        imkit::text_field("proxy", a.new_proxy, sizeof a.new_proxy, fwn, 0, "socks5h://127.0.0.1:1080");
        mark_(a, "proxy");
        cy += px(48);
        label(tr("luban.os.quick.logs_optional"));
        ImGui::SetCursorPos(ImVec2(cx, cy));
        // Who logs in when none is given: a system with a desktop has the
        // default user (design 2026-10-20 G33), a console root.
        const auto& card = kEditions[static_cast<std::size_t>(a.new_edition)];
        const bool with_desktop = !a.new_trad.empty() || a.new_desktop >= 0 || !card.session.empty()
                                  || card.edition() == std::string_view("desktop");
        imkit::text_field("user", a.new_user, sizeof a.new_user, (fw / 2 - px(6)) / imkit::scale(), 0,
                          with_desktop ? service::kDefaultUser : std::string_view("root"));
        mark_(a, "user");
        ImGui::SetCursorPos(ImVec2(cx + fw / 2 + px(6), cy));
        imkit::text_field("password", a.new_password, sizeof a.new_password, (fw / 2 - px(6)) / imkit::scale(),
                          ImGuiInputTextFlags_Password, tr("luban.os.quick.password"));
        mark_(a, "password");
        cy += px(48);
        // The desktop a console or xdesk card is made with: its own, or one
        // the indexes offer (luban-desktop), as many to a row as fit. A
        // traditional desktop's card is that choice already.
        std::vector<service::Offer> desktops;
        if (!trad) {
            std::lock_guard l(a.available_m);
            if (a.offers) desktops = a.offers->desktops;
        }
        if (!desktops.empty()) {
            label(tr("luban.os.admin.desktop"));
            float dx = 0;
            auto choice = [&](int k, const std::string& name, const std::string& id) {
                const float bw = std::min(fw, imkit::text_size(name, 14).x + px(28));
                if (dx > 0 && dx + bw > fw) { dx = 0; cy += px(40); }
                ImGui::SetCursorPos(ImVec2(cx + dx, cy));
                if (imkit::button(id, name, ImVec2(bw / imkit::scale(), 32), a.new_desktop == k ? imkit::Look::Primary : imkit::Look::Ghost))
                    a.new_desktop = k;
                mark_(a, id);
                dx += bw + px(6);
            };
            choice(-1, std::string(tr("luban.os.quick.own")), "desktop:own");
            for (int k = 0; k < static_cast<int>(desktops.size()); ++k) {
                const auto& d = desktops[static_cast<std::size_t>(k)];
                choice(k, d.official ? d.name : d.ref, "desktop:" + d.ref);
            }
            cy += px(40);
            if (const auto d = offered_desktop_(a, a.new_desktop); d && !d->description.empty())
                cy += paragraph_(dl, ImVec2(o.x + cx, o.y + cy), d->description, fw, th_().text_faint, 13) + px(8);
        }
    }
    return cy - start;
}

float panel_actions_(App& a, float x, float y, float w) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o(ImGui::GetWindowPos().x - ImGui::GetScrollX(), ImGui::GetWindowPos().y - ImGui::GetScrollY());
    const float pad = px(18), fw = w - pad * 2, fwn = fw / imkit::scale(), cx = x + pad;
    float cy = y + px(4);
    const auto& chosen = kEditions[static_cast<std::size_t>(a.new_edition)];
    const bool goes_in = !a.new_offer.empty() || !a.new_trad.empty() || chosen.goes_in;
    ImGui::SetCursorPos(ImVec2(cx, cy));
    if (imkit::button("make", goes_in ? tr("luban.os.quick.create_go") : tr("luban.os.quick.create"), ImVec2(fwn, 44), imkit::Look::Primary,
                      a.quick_can_make && a.new_name[0], false, imkit::Icon::Run))
        make_(a);
    mark_(a, "make");
    cy += px(58);
    ImGui::SetCursorPos(ImVec2(cx, cy));
    if (imkit::button("import", tr("luban.os.quick.import_file_luban"), ImVec2(fwn, 36),
                      imkit::Look::Ghost, true, false, imkit::Icon::Import))
        a.window->pick_file(std::string(tr("luban.os.app.import_luban_system")), "Luban", "luban");
    mark_(a, "import");
    cy += px(44);
    if (!a.notice.empty()) cy += paragraph_(dl, ImVec2(o.x + cx, o.y + cy), a.notice, fw, th_().warn, 13) + px(6);
    return cy - y;
}

// The panel in one column (a narrow window): its face, its body, its actions.
float new_panel_(App& a, float x, float y, float w, float min_h) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o(ImGui::GetWindowPos().x - ImGui::GetScrollX(), ImGui::GetWindowPos().y - ImGui::GetScrollY());
    // The face first (drawn under everything), sized once its content is known: last frame's height.
    const float h = std::max(min_h, a.panel_h);
    imkit::panel(dl, ImVec2(o.x + x, o.y + y), ImVec2(o.x + x + w, o.y + y + h), imkit::Bevel::Raised, th_().panel_lo);
    float cy = y + panel_body_(a, x, y, w);
    cy += panel_actions_(a, x, cy, w);
    a.panel_h = cy - y + px(8);
    return std::max(min_h, a.panel_h);
}

void quick_start_(App& a) {
    begin_full_("quick");
    ask_availability_(a);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    if (page_bar_(a, tr("luban.os.admin.new_system"), !a.systems.empty())) a.view = View::Launcher;
    const auto groups = quick_groups_(a);
    const auto chosen_id = chosen_card_(a);
    // A mode chosen for the card, until the person chooses: a desktop runs as
    // a machine (its own kernel, its own display), a console in a sandbox.
    if (a.new_mode_for != chosen_id) {
        a.new_mode = !a.new_trad.empty() || (a.new_offer.empty() && kEditions[static_cast<std::size_t>(a.new_edition)].desktop) ? 1 : 0;
        a.new_mode_for = chosen_id;
    }
    const float margin = px(24), gap = px(12);
    float top = px(80);
    if (const auto why = cannot_run_(a); !why.empty()) {
        // Said first, not discovered by a failure.
        imkit::text(dl, ImVec2(o.x + margin, o.y + top), imkit::fit(why, W - margin * 2, 14), th_().warn, 14);
        top += px(24);
    }
    // Wide: the cards, and the panel beside them. Narrow: one column that
    // scrolls, the panel under the cards.
    const bool side = W >= px(860);
    const float panelW = side ? std::clamp(W * 0.32f, px(300), px(380)) : W - margin * 2;
    const float cardsW = side ? W - margin * 3 - panelW : W - margin * 2;
    const float minCard = px(168);
    const float indent = px(12);                                            // room for the xdesk group's bar
    const float inner = cardsW - ImGui::GetStyle().ScrollbarSize - px(4) - indent;   // a scrollbar may take the right edge
    const int perRow = std::max(1, static_cast<int>((inner + gap) / (minCard + gap)));
    const float cardW = std::floor(std::min(px(230), (inner - gap * (perRow - 1)) / perRow));
    const float cardH = px(156);
    ImGui::SetCursorPos(ImVec2(margin, top));
    ImGui::BeginChild("cards", ImVec2(side ? cardsW + px(8) : W - margin * 2 + px(8), H - top - px(12)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    auto* cdl = ImGui::GetWindowDrawList();
    const ImVec2 co(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y - ImGui::GetScrollY());
    std::optional<std::string> index_error, reading;
    {
        std::lock_guard l(a.available_m);
        // What the index offers, shown: how long since the quick start opened (design 2026-10-19 §4.1).
        if (a.offers && a.quick_opened_at && !a.latency.contains("quick-start-index"))
            a.latency["quick-start-index"].push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - *a.quick_opened_at).count());
        if (!a.offers) reading = std::string(tr("luban.os.quick.reading_index"));
        else if (!a.offers->error.empty()) index_error = std::vformat(tr("luban.os.quick.index_unreadable"), std::make_format_args(a.offers->error));
    }
    float y = px(6);
    for (const auto& g : groups) {
        // More systems only when there are some; the traditional desktops
        // say the index is being read, or could not be (never an empty group).
        if (g.key == "more" && g.cards.empty()) continue;
        if (g.key == "desktops" && g.cards.empty() && !reading && !index_error) continue;
        const float x0 = indent;
        const std::string_view head = g.key == "console" ? tr("luban.os.quick.console_small_yours_shape")
                                      : g.key == "xdesk" ? tr("luban.os.quick.group.xdesk")
                                      : g.key == "desktops" ? tr("luban.os.quick.group.desktops") : tr("luban.os.quick.group.more");
        if (g.key == "desktops" && !g.cards.empty()) {
            // Folded until opened: "Traditional desktops · N ▸" (WuXian §12.1).
            const auto label = std::format("{} · {} {}", head, g.cards.size(), a.trad_open ? "▾" : "▸");
            const auto ts = imkit::text_size(label, 13, true);
            ImGui::SetCursorPos(ImVec2(x0, y - px(2)));
            if (ImGui::InvisibleButton("group:desktops", ImVec2(std::max(1.0f, ts.x + px(8)), ts.y + px(4)))) {
                a.trad_open = !a.trad_open;
                a.trad_heading = !a.trad_open;
            }
            mark_(a, "group:desktops");
            const bool lit = a.trad_heading || ImGui::IsItemHovered();
            if (a.trad_heading && a.keyboard)
                cdl->AddRect(ImVec2(co.x + x0 - px(4), co.y + y - px(3)), ImVec2(co.x + x0 + ts.x + px(6), co.y + y + ts.y + px(2)), th_().accent, 0, 0,
                             std::max(1.0f, std::round(px(1.5f))));
            imkit::text(cdl, ImVec2(co.x + x0, co.y + y), label, lit ? th_().text : th_().text_faint, 13, true);
        } else {
            imkit::text(cdl, ImVec2(co.x + x0, co.y + y), head, th_().text_faint, 13, true);
        }
        if (g.key == "desktops" && (a.trad_open || g.cards.empty())) {
            const auto scale = std::string(tr("luban.os.quick.light_to_full"));
            imkit::text(cdl, ImVec2(co.x + x0 + inner - imkit::text_size(scale, 12).x, co.y + y + px(1)), scale, th_().text_faint, 12);
        }
        const float group_top = y;
        y += px(26);
        if (g.key == "desktops" && g.cards.empty()) {
            imkit::text(cdl, ImVec2(co.x + x0, co.y + y), index_error ? *index_error : *reading, index_error ? th_().warn : th_().text_dim, 14);
            y += px(40);
            continue;
        }
        if (g.key == "desktops" && !a.trad_open) {
            y += px(8);
            continue;
        }
        int col = 0;
        for (const auto& c : g.cards) {
            if (col == perRow) { col = 0; y += cardH + gap; }
            ImGui::SetCursorPos(ImVec2(x0 + col * (cardW + gap), y));
            const bool on = c.id == chosen_id;
            std::string title, desc, art_name;
            bool usable = true;
            if (c.kind == QuickCard::Kind::Edition) {
                const auto& e = kEditions[static_cast<std::size_t>(c.edition)];
                // One that cannot be made here is still chosen to read why.
                usable = unavailable_(a, e).empty();
                title = std::string(tr(e.title));
                art_name = std::string(e.name);
                desc = usable ? std::format("{}\n{} · {}", tr(e.about), e.size, e.contents)
                              : std::format("{}\n{}", tr(e.about), tr("luban.os.quick.not_available_here"));
            } else if (c.kind == QuickCard::Kind::Desktop) {
                title = pick_(c.offer.title, c.offer.name);
                art_name = "desktop";
                desc = std::format("{}\n{}", pick_(c.offer.summary, c.offer.description), c.offer.available ? chips_(c.offer) : c.offer.reason);
            } else {
                title = c.offer.name;
                art_name = c.offer.ref;
                desc = std::format("{}\n{}", c.offer.description, source_of_(c.offer));
            }
            if (imkit::option_card(c.id, art::picture(art_name, on ? imkit::beat(2, 0.45) : 0), title, desc, on,
                                   ImVec2(cardW / imkit::scale(), cardH / imkit::scale()), on && a.keyboard, true))
                choose_card_(a, c);
            // A technical name explained (design 2026-10-19 §5.4, §7.5).
            if (ImGui::IsItemHovered())
                if (const auto why = explained_(title)) ImGui::SetTooltip("%.*s", static_cast<int>(why->size()), why->data());
            if (c.kind == QuickCard::Kind::Desktop && c.offer.tier) {
                // How much it brings: four marks in the card's top right
                // corner, inside its padding, on a translucent rounded ground,
                // the tier's filled -- the same on every card that has a tier;
                // they rise with the card when it is chosen (design 2026-10-21
                // §8.13, J29: they were placed by the picture's width and sat
                // against the icon).
                const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
                const float u = imkit::unit() / 3.0f;
                const float s = std::round(px(5)), sp = std::round(px(2)), bw = 4 * s + 3 * sp + px(8), bh = s * 1.6f + px(8);
                const ImVec2 b0(r1.x - px(12) - bw, r0.y + u * 2 + (on ? -u * 2 : 0) + px(10));
                cdl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + bh), (th_().bg_deep & 0x00ffffffu) | 0xb0000000u, px(4));
                for (int k = 0; k < 4; ++k) {
                    const ImVec2 p0(b0.x + px(4) + k * (s + sp), b0.y + px(4));
                    cdl->AddRectFilled(p0, ImVec2(p0.x + s, p0.y + s * 1.6f), k < *c.offer.tier ? th_().accent : th_().text_faint);
                }
            }
            mark_(a, c.id);
            ++col;
        }
        y += cardH;
        if (g.key == "xdesk") {
            // Luban's own desktop: its colour down the group, its name beside the title.
            cdl->AddRectFilled(ImVec2(co.x, co.y + group_top), ImVec2(co.x + std::round(px(4)), co.y + y), art::edition_color("desktop-xdesk"));
            const char* mark = "xdesk";
            imkit::text(cdl, ImVec2(co.x + x0 + inner - imkit::text_size(mark, 12, true).x, co.y + group_top + px(1)), mark,
                        art::edition_color("desktop-xdesk", 1), 12, true);
        }
        y += px(22);
    }
    if (!side) {
        new_panel_(a, indent, y, panelW - indent, 0);
        ImGui::SetCursorPos(ImVec2(0, y + a.panel_h + px(16)));
        ImGui::Dummy(ImVec2(1, 1));
    }
    ImGui::EndChild();
    if (side) {
        // The panel beside: its settings scroll, making it stays at its foot
        // (design 2026-10-19 §5.5).
        const float px0 = margin * 2 + cardsW, py0 = top + px(6), ph = H - top - px(28);
        imkit::panel(dl, ImVec2(o.x + px0, o.y + py0), ImVec2(o.x + px0 + panelW, o.y + py0 + ph), imkit::Bevel::Raised, th_().panel_lo);
        const float actions = a.panel_actions_h > 0 ? a.panel_actions_h : px(110);
        ImGui::SetCursorPos(ImVec2(px0, py0));
        ImGui::BeginChild("panel", ImVec2(panelW, std::max(px(120), ph - actions)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        const float body = panel_body_(a, 0, 0, panelW);
        ImGui::SetCursorPos(ImVec2(0, body));
        ImGui::Dummy(ImVec2(1, 1));
        ImGui::EndChild();
        a.panel_actions_h = panel_actions_(a, px0, py0 + std::max(px(120), ph - actions), panelW);
    }
    ImGui::End();
}

}  // namespace luban::os
