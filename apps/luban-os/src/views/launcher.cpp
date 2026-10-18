// The list of systems, the quick start (making one), importing an archive, a
// job under way, and a system that is off.
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

void launcher_(App& a) {
    begin_full_("launcher");
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    // The bar: the craftsman and the name; the gear and "new" at the right.
    dl->AddRectFilled(o, ImVec2(o.x + W, o.y + px(72)), th_().bg_deep);
    art::badge(dl, art::Badge::Craftsman, "", ImVec2(o.x + px(20), o.y + px(12)), std::max(1.0f, std::round(px(3))),
               imkit::beat(2, 0.5));
    imkit::text(dl, ImVec2(o.x + px(76), o.y + px(22)), "Luban OS", th_().text, 26, true);
    ImGui::SetCursorPos(ImVec2(W - px(20) - px(48), px(16)));
    if (imkit::button("gear", "", ImVec2(48, 40), imkit::Look::Ghost, true, false, imkit::Icon::Gear)) {
        a.before_admin = a.view;
        a.view = View::Admin;
    }
    mark_(a, "gear");
    ImGui::SetCursorPos(ImVec2(W - px(20) - px(48) - px(12) - px(150), px(16)));
    if (imkit::button("new", tr("New system", "新建系统"), ImVec2(150, 40), imkit::Look::Primary, true, false, imkit::Icon::Plus))
        a.view = View::QuickStart;
    mark_(a, "new");

    // The list: recent first, then the rest by name; a new one, one from a file.
    const float listW = std::min(px(820), W - px(48));
    const float x0 = std::round((W - listW) / 2);
    const float top = px(92), bottom = H - px(84);
    const auto shown = shown_(a);
    if (!a.filter.empty()) {
        imkit::text(dl, ImVec2(o.x + x0, o.y + top - px(4)),
                    std::vformat(tr("Showing names with “{}” · Esc shows all", "名字含“{}”的系统 · Esc 显示全部"),
                                 std::make_format_args(a.filter)),
                    th_().warn, 14);
    }
    ImGui::SetCursorPos(ImVec2(x0, top + px(20)));
    ImGui::BeginChild("systems", ImVec2(listW, std::max(px(120), bottom - top - px(20))), ImGuiChildFlags_None);
    const float rowH = 68, rowW = ImGui::GetContentRegionAvail().x;
    auto* ldl = ImGui::GetWindowDrawList();
    auto group = [&](std::string_view label) {
        ImGui::Dummy(ImVec2(0, px(6)));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        imkit::text(ldl, at, label, th_().text_faint, 13, true);
        ImGui::Dummy(ImVec2(0, imkit::text_size(label, 13, true).y + px(6)));
    };
    const int n = static_cast<int>(shown.size());
    for (int k = 0; k < n; ++k) {
        const int i = shown[static_cast<std::size_t>(k)];
        if (a.filter.empty() && k == 0 && a.recent > 0) group(tr("RECENT", "最近"));
        if (a.filter.empty() && i == a.recent) group(tr("ALL SYSTEMS", "全部系统"));
        const auto& e = a.systems[static_cast<std::size_t>(i)];
        const bool chosen = a.selected == k;
        if (imkit::list_row("row:" + e.name, ImVec2(rowW, rowH), chosen, chosen && a.keyboard)) {
            a.selected = k;
            a.countdown_until.reset();
            a.enter(e.name);
        }
        mark_(a, "row:" + e.name);
        if (chosen && a.follow) ImGui::SetScrollHereY(0.5f);
        const auto mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
        const auto edition = App::edition_name(e);
        const float cell = std::max(1.0f, std::round(px(3)));
        art::badge(ldl, art::badge_of(edition), edition, ImVec2(mn.x + px(14), mn.y + (mx.y - mn.y - 16 * cell) / 2), cell,
                   chosen ? imkit::beat(2, 0.45) : 0);
        const float tx = mn.x + px(14) + 16 * cell + px(16);
        imkit::text(ldl, ImVec2(tx, mn.y + px(12)), imkit::fit(e.name, px(260), 18, true), th_().text, 18, true);
        const bool running = e.running || a.is_running(e.name);
        std::string second = edition_label_(e);
        if (edition == "nano") second += std::string(" · ") + std::string(tr("no shell · runs commands", "无 shell · 运行命令"));
        imkit::text(ldl, ImVec2(tx, mn.y + px(40)), imkit::fit(second, mx.x - tx - px(260), 14), th_().text_dim, 14);
        // The right side: its mode, whether it runs, when it was last opened.
        float rx = mx.x - px(16);
        if (auto it = a.settings.last_used.find(e.name); it != a.settings.last_used.end()) {
            const auto when = ago_(it->second);
            const auto ts = imkit::text_size(when, 13);
            rx -= ts.x;
            imkit::text(ldl, ImVec2(rx, mn.y + px(40)), when, th_().text_faint, 13);
        }
        const std::string mode = e.settings.mode == "machine" ? std::string(tr("machine", "机器")) : std::string(tr("sandbox", "沙箱"));
        const std::string state = running ? std::string(tr("running", "运行中")) : std::string(tr("stopped", "已停止"));
        const auto sw = imkit::text_size(state, 13).x + px(12), mw = imkit::text_size(mode, 13).x + px(12);
        chip_(ldl, ImVec2(mx.x - px(16) - sw, mn.y + px(12)), state, running ? th_().ink : th_().text_dim,
              running ? th_().good : th_().panel_lo);
        chip_(ldl, ImVec2(mx.x - px(16) - sw - px(8) - mw, mn.y + px(12)), mode, th_().text, art::edition_color(edition, -1));
    }
    if (n == 0 && a.systems.empty()) {
        // Nothing yet: the craftsman, and the first step.
        ImGui::Dummy(ImVec2(0, px(40)));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float cell = std::round(px(5));
        art::badge(ldl, art::Badge::Craftsman, "", ImVec2(at.x + (rowW - 16 * cell) / 2, at.y), cell, imkit::beat(2, 0.5));
        ImGui::Dummy(ImVec2(0, 16 * cell + px(16)));
    }
    if (n == 0 && !a.systems.empty()) group(tr("NOTHING MATCHES", "没有匹配的系统"));
    else ImGui::Dummy(ImVec2(0, px(10)));
    {
        const bool chosen = a.selected == n;
        if (imkit::list_row("row:new", ImVec2(rowW, 52), chosen, chosen && a.keyboard)) a.view = View::QuickStart;
        mark_(a, "row:new");
        if (chosen && a.follow) ImGui::SetScrollHereY(0.5f);
        const auto mn = ImGui::GetItemRectMin();
        const float cell = std::max(1.0f, std::round(px(2)));
        imkit::icon(ldl, imkit::Icon::Plus, ImVec2(mn.x + px(20), mn.y + px(26) - 6 * cell), cell, th_().accent);
        imkit::text(ldl, ImVec2(mn.x + px(52), mn.y + px(16)), tr("New system", "新建系统"), th_().text, 16);
    }
    {
        if (imkit::list_row("row:import", ImVec2(rowW, 52), false, false))
            a.window->pick_file(std::string(tr("Import a Luban system", "导入 Luban 系统")), "Luban", "luban");
        mark_(a, "row:import");
        const auto mn = ImGui::GetItemRectMin();
        const float cell = std::max(1.0f, std::round(px(2)));
        imkit::icon(ldl, imkit::Icon::Import, ImVec2(mn.x + px(20), mn.y + px(26) - 6 * cell), cell, th_().text_dim);
        imkit::text(ldl, ImVec2(mn.x + px(52), mn.y + px(16)), tr("Import from a file (.luban)…", "从文件导入（.luban）…"),
                    th_().text_dim, 16);
    }
    a.follow = false;
    ImGui::EndChild();
    // What is going on: the countdown, a notice, why the list is empty.
    float y = bottom + px(4);
    if (a.countdown_until && !a.systems.empty()) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(*a.countdown_until - std::chrono::steady_clock::now()).count() + 1;
        const auto target = a.countdown_target();
        centred_(dl, y, std::vformat(tr("{} s, then {} · any key stops it", "{} 秒后进入 {} · 按任意键取消"),
                                     std::make_format_args(left, target)), th_().accent_hi, 15);
        y += px(22);
    }
    if (!a.notice.empty()) { centred_(dl, y, a.notice, th_().warn, 14); y += px(20); }
    if (!a.list_error.empty()) centred_(dl, y, a.list_error, th_().bad, 14);
    centred_(dl, H - px(26), tr("↑ ↓ choose · Enter go in · type to find · Del remove · F2 manage",
                                "↑ ↓ 选择 · Enter 进入 · 直接输入查找 · Del 删除 · F2 管理"), th_().text_faint, 13);
    ImGui::End();
}

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
    std::string out(tr("This computer cannot run Luban systems yet: ", "这台电脑还不能运行 Luban 系统："));
    const auto* s = a.caps ? a.caps->mode("sandbox") : nullptr;
    if (!s) return out + std::string(tr("they are Linux systems and need a way to run one here.", "它们是 Linux 系统，这里还没有运行它们的方式。"));
    out += s->reason;
    if (!s->route.empty()) out += std::string(tr(" -- do it: ", "——处理：")) + s->route;
    return out;
}

// Why this edition cannot be made here, or nothing.
std::string unavailable_(App& a, const Edition& e) {
    if (auto why = cannot_run_(a); !why.empty()) return why;
    std::lock_guard l(a.available_m);
    const auto it = a.available.find(std::string(e.name));
    if (it == a.available.end() || !it->second.known) return {};
    if (!it->second.listed) return std::string(tr("Not published yet.", "尚未发布。"));
    if (!it->second.here)
        return std::vformat(tr("Not built for {} yet.", "尚未提供 {} 版本。"), std::make_format_args(it->second.arch));
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
        a.confirm = Confirm{std::string(tr("Not from Luban", "不是 Luban 官方的")),
                            std::vformat(tr("{} comes from an index you added, not from Luban. Luban does not check what it does.",
                                            "{} 来自你添加的 index，不是 Luban 官方的；Luban 不检查它做什么。"),
                                         std::make_format_args(what)),
                            std::string(tr("Make it", "创建")), false, [&a] { make_(a, true); }};
        return;
    }
    if (a.new_password[0]) o.password_hash = xlings::sha512::crypt(a.new_password, xlings::sha512::random_salt());
    const bool machine = a.new_mode == 1, goes_in = offer ? true : chosen.goes_in;
    static constexpr std::array<std::string_view, 3> kPersist{"all", "home", "none"};
    const std::string persist(kPersist[static_cast<std::size_t>(std::clamp(a.new_persist, 0, 2))]);
    const std::string name = o.name;
    a.start_job(std::format("{} {}", tr("Making", "创建"), name),
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
    const std::string title = offer ? offer->name : std::string(tr(chosen.title_en, chosen.title_zh));
    std::string sub = std::format("{} · {}", chosen.size, chosen.contents);
    if (offer) sub = offer->official ? std::string(tr("Luban's", "Luban 官方")) : std::vformat(tr("from {}", "来自 {}"), std::make_format_args(offer->index));
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
    const std::string detail = offer ? offer->description : std::string(tr(chosen.detail_en, chosen.detail_zh));
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
    label(tr("NAME", "名字"));
    ImGui::SetCursorPos(ImVec2(cx, cy));
    imkit::text_field("name", a.new_name, sizeof a.new_name, fwn);
    mark_(a, "name");
    cy += px(48);
    label(tr("RUNS AS", "运行方式"));
    ImGui::SetCursorPos(ImVec2(cx, cy));
    a.new_mode = imkit::segmented("mode", {std::string(tr("sandbox", "沙箱")), std::string(tr("machine", "机器"))}, a.new_mode, fwn,
                                  {"mode:sandbox", "mode:machine"});
    cy += px(46);
    ImGui::SetCursorPos(ImVec2(cx, cy));
    imkit::toggle("more", tr("More options", "更多选项"), a.more);
    mark_(a, "more");
    cy += px(36);
    if (a.more) {
        label(tr("KEEPS", "保留修改"));
        ImGui::SetCursorPos(ImVec2(cx, cy));
        a.new_persist = imkit::segmented("persist", {std::string(tr("all", "全部")), std::string(tr("/home", "仅 /home")),
                                                     std::string(tr("nothing", "不保留"))},
                                         a.new_persist, fwn);
        cy += px(46);
        label(tr("PROXY (SOCKS5h, optional)", "代理（SOCKS5h，可选）"));
        ImGui::SetCursorPos(ImVec2(cx, cy));
        imkit::text_field("proxy", a.new_proxy, sizeof a.new_proxy, fwn, 0, "socks5h://127.0.0.1:1080");
        mark_(a, "proxy");
        cy += px(48);
        label(tr("LOGS IN AS (optional)", "登录用户（可选）"));
        ImGui::SetCursorPos(ImVec2(cx, cy));
        imkit::text_field("user", a.new_user, sizeof a.new_user, (fw / 2 - px(6)) / imkit::scale(), 0, "root");
        mark_(a, "user");
        ImGui::SetCursorPos(ImVec2(cx + fw / 2 + px(6), cy));
        imkit::text_field("password", a.new_password, sizeof a.new_password, (fw / 2 - px(6)) / imkit::scale(),
                          ImGuiInputTextFlags_Password, tr("password", "密码"));
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
            label(tr("DESKTOP", "桌面"));
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
            choice(-1, std::string(tr("its own", "自带的")), "desktop:own");
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
    const auto make_label = offer || chosen.goes_in ? tr("Create and go in", "创建并进入") : tr("Create", "创建");
    if (imkit::button("make", make_label, ImVec2(fwn, 44), imkit::Look::Primary, why_not.empty() && a.new_name[0], false, imkit::Icon::Run))
        make_(a);
    mark_(a, "make");
    cy += px(58);
    ImGui::SetCursorPos(ImVec2(cx, cy));
    if (imkit::button("import", tr("Import from a file (.luban)…", "从文件导入（.luban）…"), ImVec2(fwn, 36),
                      imkit::Look::Ghost, true, false, imkit::Icon::Import))
        a.window->pick_file(std::string(tr("Import a Luban system", "导入 Luban 系统")), "Luban", "luban");
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
    if (page_bar_(a, tr("New system", "新建系统"), !a.systems.empty())) a.view = View::Launcher;
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
                    group == 0 ? tr("CONSOLE · small, yours to shape", "控制台 · 精简，自己定制") : tr("DESKTOP · ready to use", "桌面 · 开箱即用"),
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
            const auto desc = usable ? std::format("{}\n{} · {}", tr(e.about_en, e.about_zh), e.size, e.contents)
                                     : std::format("{}\n{}", tr(e.about_en, e.about_zh), tr("Not available here", "这里不可用"));
            if (imkit::option_card("card:" + std::string(e.name), art::picture(e.name, on ? imkit::beat(2, 0.45) : 0),
                                   tr(e.title_en, e.title_zh), desc, on, ImVec2(cardW / imkit::scale(), cardH / imkit::scale()),
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
        imkit::text(cdl, ImVec2(co.x, co.y + y), tr("MORE · from the indexes", "更多 · 来自 index"), th_().text_faint, 13, true);
        y += px(26);
        int col = 0;
        for (const auto& o : more) {
            if (col == perRow) { col = 0; y += cardH + gap; }
            ImGui::SetCursorPos(ImVec2(col * (cardW + gap), y));
            const bool on = a.new_offer == o.ref;
            const auto from = o.official ? std::string(tr("Luban's", "Luban 官方")) : std::vformat(tr("from {}", "来自 {}"), std::make_format_args(o.index));
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

void import_view_(App& a) {
    begin_full_("import");
    auto& p = *a.import;
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth();
    const auto file_name = p.file.filename().string();
    if (page_bar_(a, std::vformat(tr("Import {}", "导入 {}"), std::make_format_args(file_name)), false)) {}
    const float width = std::min(px(620), W - px(48)), x = std::round((W - width) / 2);
    float y = px(96);
    imkit::panel(dl, ImVec2(o.x + x - px(16), o.y + y - px(12)), ImVec2(o.x + x + width + px(16), o.y + ImGui::GetWindowHeight() - px(24)),
                 imkit::Bevel::Raised, th_().panel_lo);
    if (!p.error.empty()) {
        y += paragraph_(dl, ImVec2(o.x + x, o.y + y), std::vformat(tr("It cannot be read: {}", "无法读取：{}"), std::make_format_args(p.error)),
                        width, th_().bad) + px(16);
    } else {
        auto row = [&](std::string_view k, const std::string& v) {
            imkit::text(dl, ImVec2(o.x + x, o.y + y), k, th_().text_faint, 13, true);
            imkit::text(dl, ImVec2(o.x + x + px(140), o.y + y), imkit::fit(v, width - px(140)), th_().text);
            y += px(28);
        };
        row(tr("EDITION", "EDITION"), p.edition.empty() ? std::string("—") : p.edition);
        row(tr("PACKAGES", "软件包"), std::to_string(p.packages));
        row(tr("ISOLATION", "隔离"), p.preset.empty() ? std::string(tr("the default", "默认")) : p.preset);
        if (!p.wider.empty()) {
            // More than a new environment may do: said before, agreed to.
            y += px(8);
            imkit::icon(dl, imkit::Icon::Warn, ImVec2(o.x + x, o.y + y), std::round(px(1.5f)), th_().warn);
            imkit::text(dl, ImVec2(o.x + x + px(26), o.y + y), tr("It may do more than a new system:", "它能做的比新系统更多："), th_().warn);
            y += px(26);
            for (const auto& w : p.wider) {
                imkit::text(dl, ImVec2(o.x + x + px(16), o.y + y), imkit::fit("· " + w, width - px(16), 14), th_().text_dim, 14);
                y += px(22);
            }
            ImGui::SetCursorPos(ImVec2(x, y + px(4)));
            imkit::checkbox("import:confirm", tr("I know; import it as it is", "我知道了，按原样导入"), p.confirmed);
            mark_(a, "import:confirm");
            y += px(40);
        }
        y += px(8);
        imkit::text(dl, ImVec2(o.x + x, o.y + y), tr("NAME", "名字"), th_().text_dim, 13, true);
        y += px(20);
        ImGui::SetCursorPos(ImVec2(x, y));
        imkit::text_field("import:name", p.name, sizeof p.name, width / imkit::scale());
        mark_(a, "import:name");
        y += px(56);
        if (const auto why = cannot_run_(a); !why.empty()) y += paragraph_(dl, ImVec2(o.x + x, o.y + y), why, width, th_().warn, 14) + px(8);
        ImGui::SetCursorPos(ImVec2(x, y));
        const bool go = imkit::button("import:go", tr("Import and go in", "导入并进入"), ImVec2(width / imkit::scale(), 44),
                                      imkit::Look::Primary, (p.wider.empty() || p.confirmed) && can_run_(a));
        mark_(a, "import:go");
        y += px(56);
        if (go) {
            const std::string name = p.name;
            const fs::path file = p.file;
            a.import.reset();
            a.start_job(std::format("{} {}", tr("Importing", "导入"), name),
                        [name, file](Job& j, service::Context c) { j.code = service::import_environment(c, file, name).code; },
                        [name](App& app) {
                            if (app.job->code != 0) return;
                            app.refresh();
                            app.enter(name);
                        });
            ImGui::End();
            return;
        }
    }
    ImGui::SetCursorPos(ImVec2(x, y));
    if (imkit::button("import:cancel", tr("Cancel", "取消"), ImVec2(width / imkit::scale(), 36), imkit::Look::Ghost)) {
        a.import.reset();
        a.view = a.systems.empty() ? View::QuickStart : View::Launcher;
    }
    mark_(a, "import:cancel");
    ImGui::End();
}

// The stages of making a system, as a person reads them, and which one the
// events are in.
constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kStages{{
    {"Resolve", "解析"}, {"Download", "下载"}, {"Unpack", "解压"}, {"Configure", "配置"}, {"Check", "自检"}}};
int stage_of_(const Job& j) {
    const auto& s = j.stage;
    if (s == "check") return 4;
    if (s.find("config") != std::string::npos || s == "hook" || s == "configure") return 3;
    if (s.find("extract") != std::string::npos || s == "unpack") return 2;
    if (s.find("download") != std::string::npos) return 1;
    return 0;
}

void job_(App& a) {
    begin_full_("job");
    auto& j = *a.job;
    std::lock_guard l(j.m);
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    const float width = std::min(px(680), W - px(48)), x = std::round((W - width) / 2);
    float y = std::max(px(40), H * 0.12f);
    const bool failed = j.done && j.code != 0;
    // The craftsman at work, or standing when it failed.
    const float cell = std::round(px(5));
    art::badge(dl, art::Badge::Craftsman, "", ImVec2(o.x + (W - 16 * cell) / 2, o.y + y), cell, failed ? 0 : imkit::beat(2, 0.3));
    y += 16 * cell + px(16);
    centred_(dl, y, j.title, th_().text, 26, true);
    y += px(44);
    // Its stages, the one it is in lit.
    const int at = j.done && j.code == 0 ? 5 : stage_of_(j);
    const float chipW = std::floor((width - px(8) * 4) / 5);
    for (int k = 0; k < 5; ++k) {
        const ImVec2 c0(o.x + x + k * (chipW + px(8)), o.y + y), c1(c0.x + chipW, c0.y + px(30));
        const bool done = k < at, now = k == at && !failed;
        imkit::panel(dl, c0, c1, now ? imkit::Bevel::Raised : imkit::Bevel::Flat,
                     done ? imkit::theme().accent_lo : now ? imkit::theme().panel : imkit::theme().bg_deep);
        const std::string label(tr(kStages[static_cast<std::size_t>(k)].first, kStages[static_cast<std::size_t>(k)].second));
        const auto ts = imkit::text_size(label, 13);
        imkit::text(dl, ImVec2(c0.x + (chipW - ts.x) / 2, c0.y + (px(30) - ts.y) / 2), label,
                    done || now ? th_().text : th_().text_faint, 13);
        if (done) imkit::icon(dl, imkit::Icon::Check, ImVec2(c0.x + px(6), c0.y + px(8)), 1.0f * std::max(1.0f, std::round(px(1))), th_().good);
    }
    y += px(46);
    imkit::progress(dl, ImVec2(o.x + x, o.y + y), ImVec2(o.x + x + width, o.y + y + px(22)), failed ? 0 : j.done ? 1.0 : j.percent);
    y += px(30);
    if (j.percent >= 0 && !j.done) {
        const auto pct = std::format("{:.0f} %", j.percent * 100);
        imkit::text(dl, ImVec2(o.x + x + width - imkit::text_size(pct, 13).x, o.y + y), pct, th_().text_dim, 13);
    }
    imkit::text(dl, ImVec2(o.x + x, o.y + y), imkit::fit(j.current, width - px(70), 14), th_().text_dim, 14);
    y += px(30);
    for (const auto& e : j.errors) y += paragraph_(dl, ImVec2(o.x + x, o.y + y), e, width, th_().bad, 14) + px(6);
    y += px(8);
    ImGui::SetCursorPos(ImVec2(x, y));
    if (!j.done) {
        if (imkit::button("job:cancel", tr("Cancel", "取消"), ImVec2(140, 38), imkit::Look::Normal, j.cancel && !j.cancel->load(),
                          false, imkit::Icon::Cross))
            if (j.cancel) j.cancel->store(true);
        mark_(a, "job:cancel");
    } else if (failed) {
        bool retry = imkit::button("job:retry", tr("Try again", "重试"), ImVec2(140, 38), imkit::Look::Primary);
        mark_(a, "job:retry");
        ImGui::SetCursorPos(ImVec2(x + px(152), y));
        bool back = imkit::button("job:back", tr("Back to the list", "返回列表"), ImVec2(180, 38), imkit::Look::Ghost);
        mark_(a, "job:back");
        if (back) a.view = View::Launcher;
        if (retry && j.retry) a.later = j.retry;   // after the frame: it replaces this job
    }
    // What it said, quietly, at the bottom.
    y += px(52);
    const float logH = std::max(0.0f, H - y - px(24));
    if (logH > px(60)) {
        ImGui::SetCursorPos(ImVec2(x, y));
        imkit::panel(dl, ImVec2(o.x + x, o.y + y), ImVec2(o.x + x + width, o.y + y + logH), imkit::Bevel::Sunken, th_().bg_deep);
        ImGui::SetCursorPos(ImVec2(x + px(8), y + px(6)));
        ImGui::BeginChild("log", ImVec2(width - px(16), logH - px(12)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        auto* ldl = ImGui::GetWindowDrawList();
        const float lh = imkit::text_size("Ag", 13).y + px(2);
        const std::size_t first = j.log.size() > 400 ? j.log.size() - 400 : 0;
        for (std::size_t k = first; k < j.log.size(); ++k) {
            imkit::text(ldl, ImGui::GetCursorScreenPos(), imkit::fit(j.log[k], width - px(24), 13), th_().text_faint, 13);
            ImGui::Dummy(ImVec2(0, lh));
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - px(4)) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::End();
}


void off_(App& a) {
    begin_full_("off");
    auto* dl = ImGui::GetWindowDrawList();
    const float H = ImGui::GetWindowHeight();
    float y = H * 0.3f;
    centred_(dl, y, std::vformat(tr("{} is off", "{} 已关机"), std::make_format_args(a.current)), th_().text, 26, true);
    y += px(40);
    centred_(dl, y, tr("Enter starts it again · Esc goes back to the list", "Enter 重新启动 · Esc 返回列表"), th_().text_dim, 15);
    if (!a.ended_why.empty()) {
        // Why: what the session said last, as it said it.
        y += px(40);
        centred_(dl, y, std::vformat(tr("It stopped (exit {}). It said:", "它停止了（退出码 {}）。最后的输出："), std::make_format_args(a.ended_code)),
                 th_().warn, 14);
        y += px(26);
        const float width = std::min(px(720), ImGui::GetWindowWidth() - px(80));
        const float x = (ImGui::GetWindowWidth() - width) / 2;
        ImGui::SetCursorPos(ImVec2(x, y));
        ImGui::BeginChild("why", ImVec2(width, px(130)), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(a.ended_why.c_str());
        ImGui::EndChild();
        mark_(a, "why");
    }
    ImGui::End();
    for (const auto& k : a.window->keys()) {
        if (k.name == "Enter") a.enter(a.current);
        else if (k.name == "Escape") a.back_home();
    }
}

// A layer over the window: dim, and a panel in the middle; the panel's top-left.

}  // namespace luban::os
