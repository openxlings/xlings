// The management panel (F2): each system's pages, the app's settings, activity.
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

// ── management ──────────────────────────────────────────────────────────

// A setting's row: its name, the control, and the command that sets it.
struct Rows_ {
    App& a;
    ImDrawList* dl;
    ImVec2 o;
    float x, y, width;
    void label(std::string_view s) {
        imkit::text(dl, ImVec2(o.x + x, o.y + y + px(10)), s, th_().text_dim, 13, true);
    }
    void command(std::string_view s) {
        imkit::text(dl, ImVec2(o.x + x + px(200), o.y + y + px(42)), imkit::fit(s, width - px(200), 12), th_().text_faint, 12);
    }
    // A choice among words; the index chosen.
    int choice(std::string_view id, std::string_view name, const std::vector<std::string>& words, int now,
               std::string_view cmd = {}) {
        label(name);
        ImGui::SetCursorPos(ImVec2(x + px(200), y));
        const int chosen = imkit::segmented(id, words, now, std::min(width - px(200), px(420)) / imkit::scale());
        if (!cmd.empty()) command(cmd);
        y += cmd.empty() ? px(52) : px(66);
        return chosen;
    }
};

void check_system_(App& a, const std::string& name);

void system_pages_(App& a, ImVec2 o, float x0, float y0, float width, float height) {
    auto* dl = ImGui::GetWindowDrawList();
    if (a.admin_selected >= static_cast<int>(a.systems.size())) return;
    auto& e = a.systems[static_cast<std::size_t>(a.admin_selected)];
    const auto edition = App::edition_name(e);
    const bool on = a.is_running(e.name) || e.running;
    // Its head: badge, name, edition, state.
    const float cell = std::round(px(3));
    art::badge(dl, art::badge_of(edition), edition, ImVec2(o.x + x0, o.y + y0), cell, imkit::beat(2, 0.45));
    imkit::text(dl, ImVec2(o.x + x0 + 16 * cell + px(14), o.y + y0 + px(2)), imkit::fit(e.name, width - px(200), 24, true), th_().text, 24, true);
    imkit::text(dl, ImVec2(o.x + x0 + 16 * cell + px(14), o.y + y0 + px(30)),
                std::format("{} · {} {} · {}", edition_label_(e), e.packages, tr("luban.os.admin.packages"),
                            on ? tr("luban.os.admin.running") : tr("luban.os.admin.stopped")),
                th_().text_dim, 14);
    ImGui::SetCursorPos(ImVec2(x0 + width - px(120), y0 + px(4)));
    if (imkit::button("admin:enter", tr("luban.os.admin.go"), ImVec2(120, 40), imkit::Look::Primary, true, false, imkit::Icon::Run)) a.enter(e.name);
    mark_(a, "admin:enter");
    float y = y0 + 16 * cell + px(18);
    ImGui::SetCursorPos(ImVec2(x0, y));
    const std::vector<std::string> pages{std::string(tr("luban.os.admin.overview")), std::string(tr("luban.os.admin.versions")),
                                         std::string(tr("luban.os.app.settings")), std::string(tr("luban.os.admin.export_remove"))};
    a.admin_page = imkit::tabs("page", pages, a.admin_page, {"page:overview", "page:versions", "page:settings", "page:danger"});
    y = ImGui::GetCursorPosY() + px(16);
    Rows_ r{a, dl, o, x0, y, width};
    if (a.admin_page == 0) {
        auto fact = [&](std::string_view k, const std::string& v) {
            r.label(k);
            imkit::text(dl, ImVec2(o.x + x0 + px(200), o.y + r.y + px(8)), imkit::fit(v, width - px(200)), th_().text);
            r.y += px(34);
        };
        fact(tr("luban.os.admin.edition"), e.edition.empty() ? std::string("—") : e.edition);
        fact(tr("luban.os.admin.generation"), e.generation ? std::to_string(*e.generation) : std::string("—"));
        fact(tr("luban.os.admin.runs"), e.settings.mode);
        fact(tr("luban.os.admin.keeps"), e.settings.persist);
        if (!e.shell.empty()) fact(tr("luban.os.admin.shell"), e.shell);
        fact(tr("luban.os.admin.carrier"), e.carrier);
        // Its self-check (design 2026-10-16 L §11), asked here.
        r.y += px(8);
        r.label(tr("luban.os.admin.self_check"));
        ImGui::SetCursorPos(ImVec2(x0 + px(200), r.y));
        bool checking;
        {
            std::lock_guard cl(a.check_m);
            checking = a.checking.contains(e.name);
        }
        if (imkit::button("admin:check", checking ? tr("luban.os.admin.checking") : tr("luban.os.admin.check"), ImVec2(140, 36),
                          imkit::Look::Normal, !checking, false, imkit::Icon::Check))
            check_system_(a, e.name);
        mark_(a, "admin:check");
        r.y += px(48);
        std::lock_guard l(a.check_m);
        if (auto it = a.checks.find(e.name); it != a.checks.end() && it->second.is_object()) {
            for (const auto& item : it->second.value("items", Json::array())) {
                const bool ok = item.value("ok", true), critical = item.value("critical", false);
                imkit::icon(dl, ok ? imkit::Icon::Check : critical ? imkit::Icon::Cross : imkit::Icon::Warn,
                            ImVec2(o.x + x0 + px(200), o.y + r.y + px(3)), std::max(1.0f, std::round(px(1.25f))),
                            ok ? th_().good : critical ? th_().bad : th_().warn);
                std::string line = item.value("what", std::string{});
                if (!ok && item.contains("detail")) line += " — " + item.value("detail", std::string{});
                imkit::text(dl, ImVec2(o.x + x0 + px(226), o.y + r.y), imkit::fit(line, width - px(230), 14), ok ? th_().text_dim : th_().text, 14);
                r.y += px(24);
                if (r.y > y0 + height - px(24)) break;
            }
        } else if (it != a.checks.end()) {
            imkit::text(dl, ImVec2(o.x + x0 + px(200), o.y + r.y), imkit::fit(it->second.dump(), width - px(200), 13), th_().warn, 13);
        }
    } else if (a.admin_page == 1) {
        if (a.history_for != e.name) {
            a.history_for = e.name;
            a.history = service::history(a.ctx, e.name).value;
            a.upgrade_plan.reset();
        }
        ImGui::SetCursorPos(ImVec2(x0, r.y));
        if (imkit::button("admin:upgrade-plan", tr("luban.os.admin.what_would_upgrade_do"), ImVec2(240, 38)))
            a.upgrade_plan = service::upgrade(a.ctx, e.name, "", true).value;
        mark_(a, "admin:upgrade-plan");
        ImGui::SetCursorPos(ImVec2(x0 + px(252), r.y));
        const auto name = e.name;
        if (imkit::button("admin:upgrade", tr("luban.os.admin.upgrade"), ImVec2(140, 38), imkit::Look::Primary))
            a.start_job(std::format("{} {}", tr("luban.os.admin.upgrading"), name),
                        [name](Job& j, service::Context c) { j.code = service::upgrade(c, name, "", false).code; },
                        [](App& app) { app.refresh(); app.history_for.clear(); app.view = View::Admin; });
        mark_(a, "admin:upgrade");
        r.y += px(54);
        if (a.upgrade_plan) {
            const auto said = a.upgrade_plan->value("up_to_date", false) ? std::string(tr("luban.os.admin.up_date"))
                                                                         : a.upgrade_plan->dump();
            r.y += paragraph_(dl, ImVec2(o.x + x0, o.y + r.y), imkit::fit(said, width * 3), width, th_().text_dim, 13) + px(10);
        }
        if (a.history)
            for (const auto& g : a.history->value("generations", Json::array())) {
                const int n = g.value("generation", 0);
                const bool current = g.value("current", false);
                imkit::text(dl, ImVec2(o.x + x0, o.y + r.y + px(8)), std::format("{} {:>3}", current ? "▶" : " ", n),
                            current ? th_().accent_hi : th_().text_dim, 15, true);
                imkit::text(dl, ImVec2(o.x + x0 + px(70), o.y + r.y + px(8)),
                            imkit::fit(std::format("{}  {}", g.value("created", std::string{}), g.value("reason", std::string{})), width - px(240), 14),
                            th_().text, 14);
                if (!current) {
                    ImGui::SetCursorPos(ImVec2(x0 + width - px(140), r.y));
                    const auto to = std::to_string(n);
                    if (imkit::button(std::format("admin:rollback:{}", n), tr("luban.os.admin.go_back"), ImVec2(140, 34)))
                        a.start_job(std::format("{} {}", tr("luban.os.admin.going_back"), name),
                                    [name, to](Job& j, service::Context c) { j.code = service::rollback(c, name, to).code; },
                                    [](App& app) { app.refresh(); app.history_for.clear(); app.view = View::Admin; });
                    mark_(a, std::format("admin:rollback:{}", n));
                }
                r.y += px(42);
                if (r.y > y0 + height - px(40)) break;
            }
    } else if (a.admin_page == 2) {
        // Applied now, the list read again after the frame: this page is
        // still drawing from it.
        auto set = [&](const char* key, const std::string& value) {
            (void)service::configure(a.ctx, e.name, {{key, value}});
            a.refresh_after_frame = true;
            a.page_cache_for.clear();
        };
        const std::vector<std::string> modes{"sandbox", "machine"}, keeps{"all", "home", "none"}, closes{"ask", "background", "poweroff"};
        auto index = [](const std::vector<std::string>& v, const std::string& now) {
            const auto it = std::ranges::find(v, now);
            return it == v.end() ? 0 : static_cast<int>(it - v.begin());
        };
        const auto name = e.name;
        int k = index(modes, e.settings.mode);
        if (int c = r.choice("setting:mode", tr("luban.os.admin.runs"), {std::string(tr("luban.os.app.sandbox")), std::string(tr("luban.os.app.machine"))}, k,
                             std::format("luban config {} --mode {}", name, modes[static_cast<std::size_t>(k)])); c != k)
            set("mode", modes[static_cast<std::size_t>(c)]);
        k = index(keeps, e.settings.persist);
        if (int c = r.choice("setting:persist", tr("luban.os.admin.keeps_2"), {std::string(tr("luban.os.admin.all")), std::string(tr("luban.os.admin.home")),
                                                                     std::string(tr("luban.os.admin.nothing"))}, k,
                             std::format("luban config {} --persist {}", name, keeps[static_cast<std::size_t>(k)])); c != k)
            set("persist", keeps[static_cast<std::size_t>(c)]);
        k = index(closes, e.settings.on_close);
        if (int c = r.choice("setting:on-close", tr("luban.os.admin.closing"), {std::string(tr("luban.os.admin.ask")), std::string(tr("luban.os.admin.background")),
                                                                         std::string(tr("luban.os.admin.power_off"))}, k,
                             std::format("luban config {} --on-close {}", name, closes[static_cast<std::size_t>(k)])); c != k)
            set("on-close", closes[static_cast<std::size_t>(c)]);
        if (a.page_cache_for != e.name || std::chrono::steady_clock::now() - a.page_at > std::chrono::seconds(2)) {
            a.page_cache_for = e.name;
            a.page_at = std::chrono::steady_clock::now();
            // The service's answer (read where the root's links lead), not a
            // walk of the root from here (E11: a root's /usr is a mount point).
            a.page_desktops = e.desktops;
            a.page_grant = a.clipboard_grant(e.name);
            a.page_gpu = a.gpu_grant(e.name);
        }
        {
            // The desktop: the sessions its root declares, or none (its console).
            std::vector<std::string> ids{"", "none"};
            for (const auto& id : a.page_desktops) ids.push_back(id);
            if (ids.size() > 2) {
                std::vector<std::string> words{std::string(tr("luban.os.admin.own")), std::string(tr("luban.os.admin.none"))};
                for (std::size_t i = 2; i < ids.size(); ++i) words.push_back(ids[i]);
                k = index(ids, e.settings.desktop);
                if (int c = r.choice("setting:desktop", tr("luban.os.admin.desktop"), words, k,
                                     std::format("luban config {} --desktop {}", name, ids[static_cast<std::size_t>(k)].empty() ? "''" : ids[static_cast<std::size_t>(k)]));
                    c != k)
                    set("desktop", ids[static_cast<std::size_t>(c)]);
            }
        }
        if (!e.shell.empty()) {
            const std::vector<std::string> shells{"fish", "bash", "sh"};
            const auto now = fs::path(e.shell).filename().string();
            k = index(shells, now);
            if (int c = r.choice("setting:shell", tr("luban.os.admin.shell"), shells, k, std::format("luban config {} --shell {}", name, now)); c != k)
                set("shell", shells[static_cast<std::size_t>(c)]);
        }
        {
            // What may cross the clipboard: its grants.
            bool copy = a.page_grant.copy, paste = a.page_grant.paste;
            r.label(tr("luban.os.admin.clipboard"));
            ImGui::SetCursorPos(ImVec2(x0 + px(200), r.y + px(4)));
            if (imkit::toggle("setting:clipboard", tr("luban.os.admin.copy_out"), copy))
                set(copy ? "allow" : "disallow", "clipboard");
            mark_(a, "setting:clipboard");
            ImGui::SetCursorPos(ImVec2(x0 + px(200) + px(220), r.y + px(4)));
            if (imkit::toggle("setting:clipboard-paste", tr("luban.os.admin.paste_into"), paste))
                set(paste ? "allow" : "disallow", "clipboard-paste");
            mark_(a, "setting:clipboard-paste");
            r.y += px(52);
        }
        if (!e.desktops.empty()) {
            // Its desktop drawn with this computer's GPU (WuXian §7.4).
            bool gpu = a.page_gpu;
            r.label(tr("luban.os.admin.gpu"));
            ImGui::SetCursorPos(ImVec2(x0 + px(200), r.y + px(4)));
            if (imkit::toggle("setting:gpu", tr("luban.os.admin.gpu_draws"), gpu)) set(gpu ? "allow" : "disallow", "gpu");
            mark_(a, "setting:gpu");
            r.y += px(52);
        }
        {
            r.label(tr("luban.os.admin.proxy"));
            ImGui::SetCursorPos(ImVec2(x0 + px(200), r.y));
            imkit::text_field("setting:proxy", a.proxy_edit, sizeof a.proxy_edit, std::min(px(300), width - px(340)) / imkit::scale(), 0,
                              tr("luban.os.admin.socks5h_host_port_off"));
            mark_(a, "setting:proxy");
            ImGui::SetCursorPos(ImVec2(x0 + px(200) + std::min(px(300), width - px(340)) + px(12), r.y));
            if (imkit::button("setting:proxy:apply", tr("luban.os.admin.apply"), ImVec2(110, 38), imkit::Look::Normal, a.proxy_edit[0] != 0))
                set("proxy", a.proxy_edit);
            mark_(a, "setting:proxy:apply");
            r.y += px(52);
        }
    } else {
        // Export, then the danger zone.
        r.label(tr("luban.os.admin.export"));
        ImGui::SetCursorPos(ImVec2(x0 + px(200), r.y));
        if (!a.export_path[0]) {
            const auto file = (fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / (e.name + ".luban")).string();
            std::snprintf(a.export_path, sizeof a.export_path, "%s", file.c_str());
        }
        const float fw = std::min(px(420), width - px(340));
        imkit::text_field("admin:export-path", a.export_path, sizeof a.export_path, fw / imkit::scale(), 0, "box.luban / box.img / box.iso");
        mark_(a, "admin:export-path");
        ImGui::SetCursorPos(ImVec2(x0 + px(200) + fw + px(12), r.y));
        const auto name = e.name;
        if (imkit::button("admin:export", tr("luban.os.admin.export_2"), ImVec2(110, 38), imkit::Look::Normal, a.export_path[0] != 0)) {
            const fs::path file = a.export_path;
            a.start_job(std::format("{} {}", tr("luban.os.admin.exporting"), name),
                        [name, file](Job& j, service::Context c) { j.code = service::export_environment(c, {.name = name, .file = file}).code; },
                        [](App& app) { app.view = View::Admin; app.notice = std::string(tr("luban.os.admin.exported")); });
        }
        mark_(a, "admin:export");
        r.command(std::format("luban export {} <file>  (.luban .tar .img .qcow2 .iso)", name));
        r.y += px(80);
        imkit::panel(dl, ImVec2(o.x + x0, o.y + r.y), ImVec2(o.x + x0 + width, o.y + r.y + px(120)), imkit::Bevel::Flat,
                     IM_COL32(60, 22, 30, 255));
        imkit::icon(dl, imkit::Icon::Warn, ImVec2(o.x + x0 + px(16), o.y + r.y + px(16)), std::round(px(1.5f)), th_().bad);
        imkit::text(dl, ImVec2(o.x + x0 + px(44), o.y + r.y + px(14)), tr("luban.os.admin.danger_zone"), th_().bad, 15, true);
        imkit::text(dl, ImVec2(o.x + x0 + px(16), o.y + r.y + px(44)),
                    imkit::fit(std::vformat(tr("luban.os.admin.removing_deletes_home_every_file"),
                                            std::make_format_args(name)), width - px(200), 14),
                    th_().text_dim, 14);
        ImGui::SetCursorPos(ImVec2(x0 + width - px(160), r.y + px(66)));
        if (on && imkit::button("admin:poweroff", tr("luban.os.app.power_off"), ImVec2(140, 38), imkit::Look::Normal, true, false, imkit::Icon::Power))
            a.power_off(name);
        ImGui::SetCursorPos(ImVec2(x0 + px(16), r.y + px(66)));
        if (imkit::button("admin:remove", tr("luban.os.admin.remove"), ImVec2(140, 38), imkit::Look::Danger)) {
            a.confirm = Confirm{std::vformat(tr("luban.os.app.remove"), std::make_format_args(name)),
                                std::string(tr("luban.os.app.home_every_file_are_deleted")),
                                std::string(tr("luban.os.app.remove_2")), true, [&a, name] {
                                    a.start_job(std::format("{} {}", tr("luban.os.app.removing"), name),
                                                [name](Job& j, service::Context c) { j.code = service::remove(c, name).code; },
                                                [](App& app) { app.refresh(); app.view = View::Admin; });
                                }};
        }
        mark_(a, "admin:remove");
    }
}

void check_system_(App& a, const std::string& name) {
    {
        std::lock_guard l(a.check_m);
        a.checking.insert(name);
    }
    auto* app = &a;
    auto* w = a.window.get();
    a.check_jobs.push_back(std::make_unique<std::jthread>([app, w, name, port = a.ctx.port] {
        auto report = port.json({"subos", "exec", name, "--", "/run/xlings/xlings", "subos", "__check", "--json"});
        {
            std::lock_guard l(app->check_m);
            app->checks[name] = report ? *report : Json(report.error());
            app->checking.erase(name);
        }
        w->wake();
    }));
}

void app_settings_(App& a, ImVec2 o, float x0, float y0, float width) {
    auto* dl = ImGui::GetWindowDrawList();
    Rows_ r{a, dl, o, x0, y0, width};
    const std::vector<std::string> langs{"auto", "en", "zh"};
    int k = static_cast<int>(std::ranges::find(langs, a.settings.language) - langs.begin());
    if (int c = r.choice("app:language", tr("luban.os.admin.language"), {std::string(tr("luban.os.admin.system")), "English", "中文"}, k); c != k) {
        a.settings.language = langs[static_cast<std::size_t>(c)];
        set_language(a.settings.language);
        write_app_settings(a.home, a.settings);
    }
    const std::vector<float> sizes{14, 16, 18, 20};
    k = static_cast<int>(std::ranges::find(sizes, a.settings.console_size) - sizes.begin());
    if (int c = r.choice("app:console-size", tr("luban.os.admin.console_text"), {"14", "16", "18", "20"}, k); c != k) {
        a.settings.console_size = sizes[static_cast<std::size_t>(c)];
        write_app_settings(a.home, a.settings);
    }
    const std::vector<int> counts{0, 5, 10, 30};
    k = static_cast<int>(std::ranges::find(counts, a.settings.countdown) - counts.begin());
    if (int c = r.choice("app:countdown", tr("luban.os.admin.go_itself_after"), {std::string(tr("luban.os.admin.never")), "5 s", "10 s", "30 s"}, k);
        c != k) {
        a.settings.countdown = counts[static_cast<std::size_t>(c)];
        write_app_settings(a.home, a.settings);
    }
    auto toggle = [&](const char* id, std::string_view label, bool& value) {
        ImGui::SetCursorPos(ImVec2(x0 + px(200), r.y));
        if (imkit::toggle(id, label, value)) write_app_settings(a.home, a.settings);
        mark_(a, id);
        r.y += px(40);
    };
    r.label(tr("luban.os.admin.motion"));
    toggle("app:reduce-motion", tr("luban.os.admin.reduce_motion"), a.settings.reduce_motion);
    r.label(tr("luban.os.admin.keyboard"));
    toggle("app:lock-keyboard", tr("luban.os.admin.lock_keyboard"), a.settings.lock_keyboard);
    r.label(tr("luban.os.admin.tray"));
    toggle("app:minimize-to-tray", tr("luban.os.admin.minimize_tray"), a.settings.minimize_to_tray);
    toggle("app:keep-in-tray", tr("luban.os.admin.always_tray"), a.settings.keep_in_tray);
    r.y += px(8);
    r.label(tr("luban.os.admin.default_system"));
    imkit::text(dl, ImVec2(o.x + x0 + px(200), o.y + r.y + px(8)),
                a.settings.default_system.empty() ? std::string(tr("luban.os.admin.one_opened_last")) : a.settings.default_system,
                th_().text);
    r.y += px(40);
    r.label(tr("luban.os.admin.version"));
    imkit::text(dl, ImVec2(o.x + x0 + px(200), o.y + r.y + px(8)), std::format("luban-os {} · {}", service::kVersion,
                                                                              tr("luban.os.admin.update_xlings_self_update")),
                th_().text_dim, 14);
    r.y += px(40);
    r.label(tr("luban.os.admin.fonts"));
    imkit::text(dl, ImVec2(o.x + x0 + px(200), o.y + r.y + px(8)),
                imkit::fit(a.window->font_dir().empty() ? std::string(tr("luban.os.admin.computer_s_install_luban_fonts"))
                                                        : a.window->font_dir().string(), width - px(200), 13),
                th_().text_dim, 13);
}

void activity_(App& a, ImVec2 o, float x0, float y0, float width) {
    auto* dl = ImGui::GetWindowDrawList();
    // The operations' logs, read every few seconds, not every frame.
    if (std::chrono::steady_clock::now() - a.activity_at > std::chrono::seconds(3)) {
        a.activity_at = std::chrono::steady_clock::now();
        a.activity_rows.clear();
        a.activity_refs.clear();
        std::vector<fs::path> files;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(a.home / "logs" / "ops", ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".ndjson") files.push_back(it->path());
        std::ranges::sort(files, [](const auto& x, const auto& y) {
            std::error_code e1, e2;
            return fs::last_write_time(x, e1) > fs::last_write_time(y, e2);
        });
        for (std::size_t k = 0; k < std::min<std::size_t>(files.size(), 40); ++k) {
            std::ifstream in(files[k]);
            std::string first;
            std::getline(in, first);
            auto j = Json::parse(first, nullptr, false);
            // "2026-10-12T20:12:09Z" as a person reads it: the date and the time, in UTC.
            auto when = j.is_object() ? j.value("ts", std::string{}) : std::string{};
            if (when.size() >= 16 && when[10] == 'T') when = when.substr(0, 10) + "  " + when.substr(11, 5) + " UTC";
            std::string cmd = j.is_object() ? j.value("client", "") : "";
            if (j.is_object()) for (const auto& w : j.value("argv", Json::array())) if (w.is_string()) cmd += " " + w.get<std::string>();
            a.activity_rows.emplace_back(when, cmd);
            a.activity_refs.push_back(std::format("xlings self log {}", files[k].stem().string().substr(0, 12)));
        }
    }
    float y = y0;
    if (a.activity_rows.empty()) imkit::text(dl, ImVec2(o.x + x0, o.y + y), tr("luban.os.admin.nothing_done_yet"), th_().text_faint);
    for (std::size_t k = 0; k < a.activity_rows.size(); ++k) {
        const auto& [when, cmd] = a.activity_rows[k];
        if (k % 2 == 0) dl->AddRectFilled(ImVec2(o.x + x0, o.y + y), ImVec2(o.x + x0 + width, o.y + y + px(30)), th_().bg_deep);
        imkit::text(dl, ImVec2(o.x + x0 + px(8), o.y + y + px(6)), when, th_().text_dim, 13);
        imkit::text(dl, ImVec2(o.x + x0 + px(190), o.y + y + px(6)), imkit::fit(cmd, width - px(420), 13), th_().text, 13);
        const auto& ref = a.activity_refs[k];
        imkit::text(dl, ImVec2(o.x + x0 + width - imkit::text_size(ref, 12).x - px(8), o.y + y + px(7)), ref, th_().text_faint, 12);
        y += px(30);
        if (y > ImGui::GetWindowHeight() - px(40)) break;
    }
}

void admin_(App& a) {
    begin_full_("admin");
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    if (page_bar_(a, tr("luban.os.app.manage"), true, "admin:back")) {
        a.view = a.display ? View::Desktop : a.console ? View::Console : View::Launcher;
        a.refresh();
        ImGui::End();
        return;
    }
    // The top tabs, at the bar's right.
    static const std::vector<std::string> keys{"systems", "activity", "settings"};
    int tab = static_cast<int>(std::ranges::find(keys, a.admin_tab) - keys.begin());
    ImGui::SetCursorPos(ImVec2(std::max(px(260), W - px(460)), px(22)));
    tab = imkit::tabs("top", {std::string(tr("luban.os.admin.systems")), std::string(tr("luban.os.admin.activity")), std::string(tr("luban.os.admin.app_settings"))},
                      tab, {"tab:systems", "tab:activity", "tab:settings"});
    a.admin_tab = keys[static_cast<std::size_t>(std::clamp(tab, 0, 2))];
    const float top = px(84);
    if (a.admin_tab == "systems") {
        // The systems on the left; the chosen one's pages on the right.
        const float side = std::min(px(260), W * 0.3f);
        imkit::panel(dl, ImVec2(o.x + px(16), o.y + top), ImVec2(o.x + px(16) + side, o.y + H - px(16)), imkit::Bevel::Sunken, th_().bg_deep);
        ImGui::SetCursorPos(ImVec2(px(22), top + px(6)));
        ImGui::BeginChild("list", ImVec2(side - px(12), H - top - px(28)), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        auto* ldl = ImGui::GetWindowDrawList();
        for (int i = 0; i < static_cast<int>(a.systems.size()); ++i) {
            const auto& e = a.systems[static_cast<std::size_t>(i)];
            if (imkit::list_row("admin:" + e.name, ImVec2(side - px(16), 46), a.admin_selected == i)) {
                a.admin_selected = i;
                a.history_for.clear();
                a.proxy_edit[0] = 0;
                a.export_path[0] = 0;
            }
            mark_(a, "admin:" + e.name);
            const auto mn = ImGui::GetItemRectMin();
            const auto ed = App::edition_name(e);
            const float cell = std::max(1.0f, std::round(px(1.75f)));
            art::badge(ldl, art::badge_of(ed), ed, ImVec2(mn.x + px(10), mn.y + px(23) - 8 * cell), cell);
            imkit::text(ldl, ImVec2(mn.x + px(10) + 16 * cell + px(10), mn.y + px(13)), imkit::fit(e.name, side - px(90), 15), th_().text, 15);
        }
        if (imkit::list_row("admin:new", ImVec2(side - px(16), 46), false)) a.view = View::QuickStart;
        mark_(a, "admin:new");
        imkit::icon(ldl, imkit::Icon::Plus, ImVec2(ImGui::GetItemRectMin().x + px(14), ImGui::GetItemRectMin().y + px(11)),
                    std::round(px(2)), th_().accent);
        imkit::text(ldl, ImVec2(ImGui::GetItemRectMin().x + px(48), ImGui::GetItemRectMin().y + px(13)), tr("luban.os.admin.new_system"), th_().text_dim, 15);
        ImGui::EndChild();
        if (a.systems.empty())
            imkit::text(dl, ImVec2(o.x + side + px(48), o.y + top + px(16)), tr("luban.os.admin.no_system_yet"), th_().text_dim);
        else
            system_pages_(a, o, side + px(48), top + px(8), W - side - px(80), H - top - px(24));
    } else if (a.admin_tab == "activity") {
        activity_(a, o, px(24), top + px(8), W - px(48));
    } else {
        app_settings_(a, o, px(24), top + px(8), std::min(W - px(48), px(900)));
    }
    if (!a.notice.empty()) imkit::text(dl, ImVec2(o.x + px(24), o.y + H - px(28)), imkit::fit(a.notice, W - px(48), 13), th_().warn, 13);
    ImGui::End();
}

// What a test asserts on: the view and its state, the window's title, and
// where each named control was drawn.

}  // namespace luban::os
