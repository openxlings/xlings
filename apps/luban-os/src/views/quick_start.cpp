// The quick start (design 2026-10-19 §5): what can be made, in four groups --
// consoles, xdesk, the traditional desktops, more systems -- and making it.
module luban.os;

import std;
import xlings.libs.json;
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
        // What this computer can do: the same report `luban status` gives.
        {
            auto caps = service::capabilities(service::gather(ctx.home, ctx.port));
            std::lock_guard l(app->available_m);
            app->caps = std::move(caps);
        }
        if (!stop.stop_requested() && app->window) app->window->wake();
        // What the indexes offer: systems and desktops, one search each. Which
        // package each short name is (luban:<name>, or a published
        // subos:luban-<name>) comes with it.
        auto offers = service::quick_start_offers(ctx.port);
        std::map<std::string, std::string> refs;
        for (const auto& o : offers.systems)
            if (o.official) refs[o.name] = o.ref;
        {
            std::lock_guard l(app->available_m);
            app->offers = std::move(offers);
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

// Makes the system the quick start describes, then goes in.
void make_(App& a, bool agreed) {
    const auto& chosen = kEditions[static_cast<std::size_t>(a.new_edition)];
    const auto offer = a.new_offer.empty() ? std::nullopt : offered_system_(a, a.new_offer);
    service::NewOptions o{.name = a.new_name, .edition = std::string(chosen.edition()), .proxy = a.new_proxy, .user = a.new_user,
                          .agreed = agreed};
    if (!offer && !chosen.session.empty()) o.settings.emplace_back("desktop", std::string(chosen.session));
    if (offer) {
        // An official one by its short name; anyone else's by its coordinate.
        o.edition = offer->official ? offer->name : std::string{};
        if (!offer->official) o.xpkgs.push_back(offer->ref);
    }
    // A desktop chosen: Luban's by its name (made with it, its default), another's installed.
    const auto desktop = offered_desktop_(a, a.new_desktop);
    if (desktop) {
        if (desktop->official) o.settings.emplace_back("desktop", desktop->name);
        else o.xpkgs.push_back(desktop->ref);
    }
    const bool third_party = (offer && !offer->official) || (desktop && !desktop->official);
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
    const bool machine = a.new_mode == 1, goes_in = offer ? true : chosen.goes_in;
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

// The chosen edition's panel: what it is, its settings, making it. At
// (x, y) in the current window, `w` wide; returns its height.
float new_panel_(App& a, float x, float y, float w, float min_h) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o(ImGui::GetWindowPos().x - ImGui::GetScrollX(), ImGui::GetWindowPos().y - ImGui::GetScrollY());
    const auto& chosen = kEditions[static_cast<std::size_t>(a.new_edition)];
    // A system from the indexes is said in its index's words, with where it comes from.
    const auto offer = a.new_offer.empty() ? std::nullopt : offered_system_(a, a.new_offer);
    const std::string badge_name = offer ? offer->ref : std::string(chosen.name);
    const std::string title = offer ? offer->name : std::string(tr(chosen.title));
    std::string sub = std::format("{} · {}", chosen.size, chosen.contents);
    if (offer) sub = offer->official ? std::string(tr("luban.os.quick.luban_s")) : std::vformat(tr("luban.os.quick.from"), std::make_format_args(offer->index));
    else if (chosen.desktop) {
        // What its desktop is, as the index says it (E15): not this window's guess.
        if (auto d = index_description_(a, chosen.name); !d.empty()) sub = std::format("{} · {}", chosen.size, d);
    }
    const float pad = px(18), fw = w - pad * 2;
    float cy = y + px(16);
    const float start = y;
    // The face first (drawn under everything), sized once its content is known: last frame's height.
    const float h = std::max(min_h, a.panel_h);
    imkit::panel(dl, ImVec2(o.x + x, o.y + y), ImVec2(o.x + x + w, o.y + y + h), imkit::Bevel::Raised, th_().panel_lo);
    const float cx = x + pad;
    art::badge(dl, art::badge_of(badge_name), badge_name, ImVec2(o.x + cx, o.y + cy), std::round(px(3)), imkit::beat(2, 0.45));
    imkit::text(dl, ImVec2(o.x + cx + px(60), o.y + cy + px(2)), imkit::fit(title, fw - px(60), 22, true), th_().text, 22, true);
    imkit::text(dl, ImVec2(o.x + cx + px(60), o.y + cy + px(30)), imkit::fit(sub, fw - px(60), 13), th_().text_faint, 13);
    cy += px(62);
    const auto why_not = offer ? cannot_run_(a) : unavailable_(a, chosen);
    const std::string detail = offer ? offer->description : std::string(tr(chosen.detail));
    a.quick_said = why_not.empty() ? detail : why_not;
    a.quick_can_make = why_not.empty();
    cy += paragraph_(dl, ImVec2(o.x + cx, o.y + cy), a.quick_said, fw, why_not.empty() ? th_().text_dim : th_().warn, 14) + px(12);
    // The name follows the edition until the person types one of their own.
    if (a.new_name[0] == 0 || a.auto_name == a.new_name) {
        auto base = offer ? offer->name : std::string(chosen.name);
        if (const auto colon = base.find(':'); colon != std::string::npos) base = base.substr(colon + 1);
        for (int k = 1; k < 100; ++k) {
            const auto candidate = std::format("{}-{}", base, k);
            if (std::ranges::none_of(a.systems, [&](const auto& s2) { return s2.name == candidate; })) {
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
        imkit::text_field("user", a.new_user, sizeof a.new_user, (fw / 2 - px(6)) / imkit::scale(), 0, "root");
        mark_(a, "user");
        ImGui::SetCursorPos(ImVec2(cx + fw / 2 + px(6), cy));
        imkit::text_field("password", a.new_password, sizeof a.new_password, (fw / 2 - px(6)) / imkit::scale(),
                          ImGuiInputTextFlags_Password, tr("luban.os.quick.password"));
        mark_(a, "password");
        cy += px(48);
        // The desktop it is made with: its own, or one the indexes offer
        // (luban-desktop), as many to a row as fit.
        std::vector<service::Offer> desktops;
        {
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
    ImGui::SetCursorPos(ImVec2(cx, cy + px(4)));
    const auto make_label = offer || chosen.goes_in ? tr("luban.os.quick.create_go") : tr("luban.os.quick.create");
    if (imkit::button("make", make_label, ImVec2(fwn, 44), imkit::Look::Primary, why_not.empty() && a.new_name[0], false, imkit::Icon::Run))
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
    a.panel_h = cy - start + px(8);
    return std::max(min_h, a.panel_h);
}

void quick_start_(App& a) {
    begin_full_("quick");
    ask_availability_(a);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    if (page_bar_(a, tr("luban.os.admin.new_system"), !a.systems.empty())) a.view = View::Launcher;
    // A mode chosen for the edition, until the person chooses: a desktop runs
    // as a machine (its own kernel, its own display), a console in a sandbox.
    if (a.new_mode_for != a.new_edition && a.new_offer.empty()) {
        a.new_mode = kEditions[static_cast<std::size_t>(a.new_edition)].desktop ? 1 : 0;
        a.new_mode_for = a.new_edition;
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
    const float inner = cardsW - ImGui::GetStyle().ScrollbarSize - px(4);   // a scrollbar may take the right edge
    const int perRow = std::max(1, static_cast<int>((inner + gap) / (minCard + gap)));
    const float cardW = std::floor(std::min(px(230), (inner - gap * (perRow - 1)) / perRow));
    const float cardH = px(156);
    ImGui::SetCursorPos(ImVec2(margin, top));
    ImGui::BeginChild("cards", ImVec2(side ? cardsW + px(8) : W - margin * 2 + px(8), H - top - px(12)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    auto* cdl = ImGui::GetWindowDrawList();
    const ImVec2 co(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y - ImGui::GetScrollY());
    float y = px(6);
    for (int group = 0; group < 2; ++group) {
        imkit::text(cdl, ImVec2(co.x, co.y + y),
                    group == 0 ? tr("luban.os.quick.console_small_yours_shape") : tr("luban.os.quick.desktop_ready_use"),
                    th_().text_faint, 13, true);
        y += px(26);
        int col = 0;
        for (std::size_t i = 0; i < kEditions.size(); ++i) {
            const auto& e = kEditions[i];
            if (e.desktop != (group == 1)) continue;
            if (col == perRow) { col = 0; y += cardH + gap; }
            ImGui::SetCursorPos(ImVec2(col * (cardW + gap), y));
            const bool on = a.new_offer.empty() && a.new_edition == static_cast<int>(i);
            // One that cannot be made here is still chosen to read why.
            const bool usable = unavailable_(a, e).empty();
            const auto desc = usable ? std::format("{}\n{} · {}", tr(e.about), e.size, e.contents)
                                     : std::format("{}\n{}", tr(e.about), tr("luban.os.quick.not_available_here"));
            if (imkit::option_card("card:" + std::string(e.name), art::picture(e.name, on ? imkit::beat(2, 0.45) : 0),
                                   tr(e.title), desc, on, ImVec2(cardW / imkit::scale(), cardH / imkit::scale()),
                                   on && a.keyboard, true)) {
                a.new_edition = static_cast<int>(i);
                a.new_offer.clear();
            }
            mark_(a, "card:" + std::string(e.name));
            ++col;
        }
        y += cardH + px(22);
    }
    // The rest the indexes offer: an official system this window has no card
    // for, and every one from an index someone added -- said where it comes from.
    std::vector<service::Offer> more;
    {
        std::lock_guard l(a.available_m);
        if (a.offers)
            for (const auto& o : a.offers->systems)
                if (!o.official || std::ranges::none_of(kEditions, [&](const Edition& e) { return e.name == o.name; })) more.push_back(o);
    }
    if (!more.empty()) {
        imkit::text(cdl, ImVec2(co.x, co.y + y), tr("luban.os.quick.more_indexes"), th_().text_faint, 13, true);
        y += px(26);
        int col = 0;
        for (const auto& o : more) {
            if (col == perRow) { col = 0; y += cardH + gap; }
            ImGui::SetCursorPos(ImVec2(col * (cardW + gap), y));
            const bool on = a.new_offer == o.ref;
            const auto from = o.official ? std::string(tr("luban.os.quick.luban_s")) : std::vformat(tr("luban.os.quick.from"), std::make_format_args(o.index));
            if (imkit::option_card("card:" + o.ref, art::picture(o.ref, on ? imkit::beat(2, 0.45) : 0), o.name,
                                   std::format("{}\n{}", o.description, from), on,
                                   ImVec2(cardW / imkit::scale(), cardH / imkit::scale()), on && a.keyboard, true))
                a.new_offer = o.ref;
            mark_(a, "card:" + o.ref);
            ++col;
        }
        y += cardH + px(22);
    }
    if (!side) {
        new_panel_(a, 0, y, panelW, 0);
        ImGui::SetCursorPos(ImVec2(0, y + a.panel_h + px(16)));
        ImGui::Dummy(ImVec2(1, 1));
    }
    ImGui::EndChild();
    if (side) new_panel_(a, margin * 2 + cardsW, top + px(6), panelW, H - top - px(28));
    ImGui::End();
}

}  // namespace luban::os
