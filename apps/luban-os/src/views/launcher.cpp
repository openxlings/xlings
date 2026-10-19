// The list of systems, importing an archive, a job under way, and a system
// that is off.
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
               imkit::beat(4, 0.25));
    imkit::text(dl, ImVec2(o.x + px(76), o.y + px(22)), "Luban OS", th_().text, 26, true);
    ImGui::SetCursorPos(ImVec2(W - px(20) - px(48), px(16)));
    if (imkit::button("gear", "", ImVec2(48, 40), imkit::Look::Ghost, true, false, imkit::Icon::Gear)) {
        a.before_admin = a.view;
        a.view = View::Admin;
    }
    mark_(a, "gear");
    ImGui::SetCursorPos(ImVec2(W - px(20) - px(48) - px(12) - px(150), px(16)));
    if (imkit::button("new", tr("luban.os.admin.new_system"), ImVec2(150, 40), imkit::Look::Primary, true, false, imkit::Icon::Plus))
        a.view = View::QuickStart;
    mark_(a, "new");

    // The list: recent first, then the rest by name; a new one, one from a file.
    const float listW = std::min(px(820), W - px(48));
    const float x0 = std::round((W - listW) / 2);
    const float top = px(92), bottom = H - px(84);
    const auto shown = shown_(a);
    if (!a.filter.empty()) {
        imkit::text(dl, ImVec2(o.x + x0, o.y + top - px(4)),
                    std::vformat(tr("luban.os.quick.showing_names_esc_shows_all"),
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
        if (a.filter.empty() && k == 0 && a.recent > 0) group(tr("luban.os.quick.recent"));
        if (a.filter.empty() && i == a.recent) group(tr("luban.os.quick.all_systems"));
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
        if (edition == "nano") second += std::string(" · ") + std::string(tr("luban.os.quick.no_shell_runs_commands"));
        imkit::text(ldl, ImVec2(tx, mn.y + px(40)), imkit::fit(second, mx.x - tx - px(260), 14), th_().text_dim, 14);
        // The right side: its mode, whether it runs, when it was last opened.
        float rx = mx.x - px(16);
        if (auto it = a.settings.last_used.find(e.name); it != a.settings.last_used.end()) {
            const auto when = ago_(it->second);
            const auto ts = imkit::text_size(when, 13);
            rx -= ts.x;
            imkit::text(ldl, ImVec2(rx, mn.y + px(40)), when, th_().text_faint, 13);
        }
        const std::string mode = e.settings.mode == "machine" ? std::string(tr("luban.os.app.machine")) : std::string(tr("luban.os.app.sandbox"));
        const std::string state = running ? std::string(tr("luban.os.admin.running")) : std::string(tr("luban.os.admin.stopped"));
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
        art::badge(ldl, art::Badge::Craftsman, "", ImVec2(at.x + (rowW - 16 * cell) / 2, at.y), cell, imkit::beat(4, 0.25));
        ImGui::Dummy(ImVec2(0, 16 * cell + px(16)));
    }
    if (n == 0 && !a.systems.empty()) group(tr("luban.os.quick.nothing_matches"));
    else ImGui::Dummy(ImVec2(0, px(10)));
    {
        const bool chosen = a.selected == n;
        if (imkit::list_row("row:new", ImVec2(rowW, 52), chosen, chosen && a.keyboard)) a.view = View::QuickStart;
        mark_(a, "row:new");
        if (chosen && a.follow) ImGui::SetScrollHereY(0.5f);
        const auto mn = ImGui::GetItemRectMin();
        const float cell = std::max(1.0f, std::round(px(2)));
        imkit::icon(ldl, imkit::Icon::Plus, ImVec2(mn.x + px(20), mn.y + px(26) - 6 * cell), cell, th_().accent);
        imkit::text(ldl, ImVec2(mn.x + px(52), mn.y + px(16)), tr("luban.os.admin.new_system"), th_().text, 16);
    }
    {
        if (imkit::list_row("row:import", ImVec2(rowW, 52), false, false))
            a.window->pick_file(std::string(tr("luban.os.app.import_luban_system")), "Luban", "luban");
        mark_(a, "row:import");
        const auto mn = ImGui::GetItemRectMin();
        const float cell = std::max(1.0f, std::round(px(2)));
        imkit::icon(ldl, imkit::Icon::Import, ImVec2(mn.x + px(20), mn.y + px(26) - 6 * cell), cell, th_().text_dim);
        imkit::text(ldl, ImVec2(mn.x + px(52), mn.y + px(16)), tr("luban.os.quick.import_file_luban"),
                    th_().text_dim, 16);
    }
    a.follow = false;
    ImGui::EndChild();
    // What is going on: the countdown, a notice, why the list is empty.
    float y = bottom + px(4);
    if (a.countdown_until && !a.systems.empty()) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(*a.countdown_until - std::chrono::steady_clock::now()).count() + 1;
        const auto target = a.countdown_target();
        centred_(dl, y, std::vformat(tr("luban.os.quick.s_then_any_key_stops"),
                                     std::make_format_args(left, target)), th_().accent_hi, 15);
        y += px(22);
    }
    if (!a.notice.empty()) { centred_(dl, y, a.notice, th_().warn, 14); y += px(20); }
    if (!a.list_error.empty()) centred_(dl, y, a.list_error, th_().bad, 14);
    centred_(dl, H - px(26), tr("luban.os.quick.choose_enter_go_type_find"), th_().text_faint, 13);
    ImGui::End();
}

void import_view_(App& a) {
    begin_full_("import");
    auto& p = *a.import;
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth();
    const auto file_name = p.file.filename().string();
    if (page_bar_(a, std::vformat(tr("luban.os.quick.import"), std::make_format_args(file_name)), false)) {}
    const float width = std::min(px(620), W - px(48)), x = std::round((W - width) / 2);
    float y = px(96);
    imkit::panel(dl, ImVec2(o.x + x - px(16), o.y + y - px(12)), ImVec2(o.x + x + width + px(16), o.y + ImGui::GetWindowHeight() - px(24)),
                 imkit::Bevel::Raised, th_().panel_lo);
    if (!p.error.empty()) {
        y += paragraph_(dl, ImVec2(o.x + x, o.y + y), std::vformat(tr("luban.os.quick.cannot_read"), std::make_format_args(p.error)),
                        width, th_().bad) + px(16);
    } else {
        auto row = [&](std::string_view k, const std::string& v) {
            imkit::text(dl, ImVec2(o.x + x, o.y + y), k, th_().text_faint, 13, true);
            imkit::text(dl, ImVec2(o.x + x + px(140), o.y + y), imkit::fit(v, width - px(140)), th_().text);
            y += px(28);
        };
        row(tr("luban.os.admin.edition"), p.edition.empty() ? std::string("—") : p.edition);
        row(tr("luban.os.quick.packages"), std::to_string(p.packages));
        row(tr("luban.os.quick.isolation"), p.preset.empty() ? std::string(tr("luban.os.quick.default")) : p.preset);
        if (!p.wider.empty()) {
            // More than a new environment may do: said before, agreed to.
            y += px(8);
            imkit::icon(dl, imkit::Icon::Warn, ImVec2(o.x + x, o.y + y), std::round(px(1.5f)), th_().warn);
            imkit::text(dl, ImVec2(o.x + x + px(26), o.y + y), tr("luban.os.quick.may_do_more_than_new"), th_().warn);
            y += px(26);
            for (const auto& w : p.wider) {
                imkit::text(dl, ImVec2(o.x + x + px(16), o.y + y), imkit::fit("· " + w, width - px(16), 14), th_().text_dim, 14);
                y += px(22);
            }
            ImGui::SetCursorPos(ImVec2(x, y + px(4)));
            imkit::checkbox("import:confirm", tr("luban.os.quick.i_know_import"), p.confirmed);
            mark_(a, "import:confirm");
            y += px(40);
        }
        y += px(8);
        imkit::text(dl, ImVec2(o.x + x, o.y + y), tr("luban.os.quick.name"), th_().text_dim, 13, true);
        y += px(20);
        ImGui::SetCursorPos(ImVec2(x, y));
        imkit::text_field("import:name", p.name, sizeof p.name, width / imkit::scale());
        mark_(a, "import:name");
        y += px(56);
        if (const auto why = cannot_run_(a); !why.empty()) y += paragraph_(dl, ImVec2(o.x + x, o.y + y), why, width, th_().warn, 14) + px(8);
        ImGui::SetCursorPos(ImVec2(x, y));
        const bool go = imkit::button("import:go", tr("luban.os.quick.import_go"), ImVec2(width / imkit::scale(), 44),
                                      imkit::Look::Primary, (p.wider.empty() || p.confirmed) && can_run_(a));
        mark_(a, "import:go");
        y += px(56);
        if (go) {
            const std::string name = p.name;
            const fs::path file = p.file;
            a.import.reset();
            a.start_job(std::format("{} {}", tr("luban.os.quick.importing"), name),
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
    if (imkit::button("import:cancel", tr("luban.os.common.cancel"), ImVec2(width / imkit::scale(), 36), imkit::Look::Ghost)) {
        a.import.reset();
        a.view = a.systems.empty() ? View::QuickStart : View::Launcher;
    }
    mark_(a, "import:cancel");
    ImGui::End();
}

// The stages of making a system, as a person reads them, and which one the
// events are in.
// Six steps (design 2026-10-19 §5.7): preparing (the plan, what is agreed
// to, the system's own home), then the payload's four, then done.
constexpr std::array<std::string_view, 6> kStages{{
    "luban.os.quick.stage.prepare", "luban.os.quick.stage.download", "luban.os.quick.stage.unpack",
    "luban.os.quick.stage.configure", "luban.os.quick.stage.check", "luban.os.quick.stage.done"}};
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
    art::badge(dl, art::Badge::Craftsman, "", ImVec2(o.x + (W - 16 * cell) / 2, o.y + y), cell, failed ? 0 : imkit::beat(4, 0.15));
    y += 16 * cell + px(16);
    centred_(dl, y, j.title, th_().text, 26, true);
    y += px(44);
    // Its stages, the one it is in lit.
    const int at = j.done && j.code == 0 ? 6 : stage_of_(j);
    const float chipW = std::floor((width - px(8) * 5) / 6);
    for (int k = 0; k < 6; ++k) {
        const ImVec2 c0(o.x + x + k * (chipW + px(8)), o.y + y), c1(c0.x + chipW, c0.y + px(30));
        const bool done = k < at, now = k == at && !failed;
        imkit::panel(dl, c0, c1, now ? imkit::Bevel::Raised : imkit::Bevel::Flat,
                     done ? imkit::theme().accent_lo : now ? imkit::theme().panel : imkit::theme().bg_deep);
        const std::string label(tr(kStages[static_cast<std::size_t>(k)]));
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
        if (imkit::button("job:cancel", tr("luban.os.common.cancel"), ImVec2(140, 38), imkit::Look::Normal, j.cancel && !j.cancel->load(),
                          false, imkit::Icon::Cross))
            if (j.cancel) j.cancel->store(true);
        mark_(a, "job:cancel");
    } else if (failed) {
        bool retry = imkit::button("job:retry", tr("luban.os.quick.try_again"), ImVec2(140, 38), imkit::Look::Primary);
        mark_(a, "job:retry");
        ImGui::SetCursorPos(ImVec2(x + px(152), y));
        bool back = imkit::button("job:back", tr("luban.os.quick.back_list"), ImVec2(180, 38), imkit::Look::Ghost);
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
    centred_(dl, y, std::vformat(tr("luban.os.quick.off"), std::make_format_args(a.current)), th_().text, 26, true);
    y += px(40);
    centred_(dl, y, tr("luban.os.quick.enter_starts_again_esc_goes"), th_().text_dim, 15);
    if (!a.ended_why.empty()) {
        // Why: what the session said last, as it said it.
        y += px(40);
        centred_(dl, y, std::vformat(tr("luban.os.quick.stopped_exit_said"), std::make_format_args(a.ended_code)),
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
