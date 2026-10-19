// The window's state (App, core.cppm): what its members do. Out of the
// partition's interface, where a compiler had to generate them in every
// unit that imports it (and clang 20 on Windows crashed doing so).
module luban.os;

import std;
import luban.compat;
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

void App::refresh() {
    auto l = service::list(ctx);
    if (!l) { list_error = l.error(); systems.clear(); recent = 0; return; }
    list_error.clear();
    auto all = std::move(*l);
    std::vector<service::Environment> used;
    for (const auto& e : all)
        if (auto it = settings.last_used.find(e.name); it != settings.last_used.end() && !it->second.empty()) used.push_back(e);
    std::ranges::stable_sort(used, [&](const auto& a, const auto& b) { return settings.last_used[a.name] > settings.last_used[b.name]; });
    if (used.size() > 3) used.resize(3);
    std::vector<service::Environment> rest;
    for (auto& e : all)
        if (std::ranges::none_of(used, [&](const auto& u) { return u.name == e.name; })) rest.push_back(std::move(e));
    std::ranges::sort(rest, {}, &service::Environment::name);
    recent = static_cast<int>(used.size());
    systems = std::move(used);
    systems.insert(systems.end(), std::make_move_iterator(rest.begin()), std::make_move_iterator(rest.end()));
    selected = std::clamp(selected, 0, std::max(0, static_cast<int>(systems.size()) + 1));
}

std::string App::countdown_target() const {
    if (systems.empty()) return {};
    if (!settings.default_system.empty()
        && std::ranges::any_of(systems, [&](const auto& s) { return s.name == settings.default_system; }))
        return settings.default_system;
    return systems.front().name;
}

std::string App::title_for(std::string_view what) const {
    // A program alone in the window is its title.
    if (!once_app.empty()) {
        const auto first = once_app.substr(0, once_app.find(' '));
        return std::format("{} — {}", fs::path(first).filename().string(), what);
    }
    // One title for a system in every view (design 2026-10-19 §18.3, F17):
    // `Luban OS (<system>)`; the keys are said in the bar at the top.
    return std::format("Luban OS ({})", what);
}

void App::start_job(std::string title, std::function<void(Job&, service::Context)> body, std::function<void(App&)> then) {
    job = std::make_unique<Job>();
    job->title = std::move(title);
    job->cancel = std::make_shared<std::atomic<bool>>(false);
    job_told = false;
    job_subject.clear();
    auto* j = job.get();
    auto* w = window.get();
    service::Context c = ctx;
    c.port = c.port.cancellable(job->cancel);
    c.sink = [j, w](const service::Event& e) { j->take(e); w->wake(); };
    c.yes = true;
    after_job = then;
    job->retry = [this, title = job->title, body, then] { start_job(title, body, then); };
    job->worker = std::thread([j, w, c, body = std::move(body)] {
        body(*j, c);
        j->done = true;
        w->wake();
    });
    view = View::Job;
}

std::string App::edition_name(const service::Environment& e) {
    auto label = e.edition;
    label = std::string(luban::compat::without_frozen_prefix(label));
    if (const auto at = label.find('@'); at != std::string::npos) label = label.substr(0, at);
    return label;
}

void App::enter(const std::string& name) {
    // What it needs of the window that shows it (design 2026-10-18 §9, B14):
    // a system laid out in a way this app does not read is not opened wrongly
    // (E6: an older app saw an empty /usr and showed only the console).
    if (auto env = service::describe(ctx, name)) {
        if (const auto bad = service::unmet_needs(*env, service::luban_provides()); !bad.empty()) {
            const auto why = bad.front().why();
            notice = std::vformat(tr("luban.os.state.luban_os_app_older_than"),
                                  std::make_format_args(name, why));
            return;
        }
    }
    countdown_until.reset();
    settings.last_used[name] = xlings::observe::utc_now();
    write_app_settings(home, settings);
    current = name;
    auto env = service::describe(ctx, name);
    // nano has nothing to log into: its commands (design 2026-10-16 L §13.6).
    if (env && edition_name(*env) == "nano" && !vm::running(home, name)) {
        open_run(name);
        return;
    }
    const auto mode = vm::running(home, name) ? std::string("machine") : env ? env->settings.mode : std::string("sandbox");
    if (mode == "machine") {
        start_job(std::format("{} {}", tr("luban.os.state.starting"), name),
                  [name](Job& j, service::Context c) { j.code = vm::start(c, name, {}); },
                  [name](App& a) {
                      if (a.job->code != 0) return;
                      a.open_machine(name);
                  });
    } else {
        open_console(name, false);
    }
}

void App::open_run(const std::string& name) {
    current = name;
    {
        std::lock_guard l(run.m);
        run.lines.clear();
        run.lines.push_back({std::vformat(tr("luban.os.state.has_no_shell_each_line"),
                                          std::make_format_args(name, name))});
    }
    view = View::Run;
    window->set_title(title_for(name));
    window->text_input(true);
}

void App::open_machine(const std::string& name) {
    auto env = service::describe(ctx, name);
    // A desktop set and not there is said over the console (E20).
    if (env) if (const auto plan = service::enter_plan(*env); !plan.why.empty()) notice = plan.why;
    if (auto session = env ? session_now(*env) : std::nullopt) {
        std::string error;
        const auto [w, h] = window->size();
        desktop_size = device_size_(w, h);
        auto* win = window.get();
        display = Display::machine(home, name, *session, desktop_size, clipboard_grant(name), [win] { win->wake(); }, error);
        if (display) {
            shown_desktop = *session;
            view = View::Desktop;
            window->set_title(title_for(name));
            window->text_input(false);
            return;
        }
        notice = error;
    }
    if (!once_app.empty()) { app_failed_(notice.empty() ? std::string("it cannot be described") : notice); return; }
    open_console(name, true);
}

double App::density() const {
    if (density_override > 0) return density_override;
    const double d = window ? window->density() : 1.0;
    return d > 0 ? d : 1.0;
}

double App::pixel_density() const {
    if (density_override > 0) return density_override;
    return window ? window->pixel_density() : 1.0;
}

opensal::Size App::device_size_(int w, int h) const {
    const double d = pixel_density();
    return {static_cast<std::uint32_t>(std::lround(w * d)), static_cast<std::uint32_t>(std::lround(h * d))};
}

ClipboardGrant App::clipboard_grant(const std::string& name) const {
    auto p = xlings::subos::policy_store::read(xlings::subos::HomeView{home}, name);
    if (!p) return {};
    if (!*p) return {.copy = true, .paste = true};
    return {.copy = (*p)->grants.contains("clipboard"), .paste = (*p)->grants.contains("clipboard-paste")};
}

void App::close_display() {
    shown_desktop.clear();
    desktop_text = false;
    desktop_buttons = 0;
    shell_apps = false;
    sent_density = 1;
    paste_on_open = true;
    pasted.clear();
    display.reset();
    if (texture.handle) window->destroy(texture);
    texture = {};
}

fs::path App::luban_binary(const std::string& name) const {
    std::error_code ec;
    if (std::ifstream in(xlings::subos::HomeView{home}.instance_file(name)); in) {
        const auto doc = nlohmann::json::parse(in, nullptr, false);
        if (doc.is_object() && doc.value("carrier", std::string("local")) != "local") return fs::path("/xlings/bin/luban");
    }
    if (const auto at = xlings::subos::locate(xlings::subos::HomeView{home}, name); at.domain)
        return fs::is_regular_file(at.home.home / "bin" / "luban", ec) ? at.logical / "bin" / "luban" : fs::path{};
    for (const auto& p : {platform::get_executable_path().parent_path() / "luban", home / "bin" / "luban"})
        if (fs::is_regular_file(p, ec)) return p;
    return {};
}

void App::open_console(const std::string& name, bool machine) {
    std::string error;
    auto* w = window.get();
    auto env = service::describe(ctx, name);
    // A sandbox shows its desktop the same way a machine does: the
    // session, bridged, on two pipes instead of a channel.
    if (!machine) {
        const auto luban = luban_binary(name);
        if (env) if (const auto plan = service::enter_plan(*env); !plan.why.empty()) notice = plan.why;
        if (auto session = env ? session_now(*env) : std::nullopt; session && !luban.empty()) {
            const auto [ww, wh] = window->size();
            desktop_size = device_size_(ww, wh);
            display = Display::sandbox(ctx.port.xlings(), luban, home, name, *session, env ? env->settings.persist : "all",
                                       desktop_size, clipboard_grant(name), [w] { w->wake(); }, error);
            if (display) {
                shown_desktop = *session;
                view = View::Desktop;
                window->set_title(title_for(name));
                window->text_input(false);
                return;
            }
            notice = error;
        }
        if (!once_app.empty()) { app_failed_(error.empty() ? std::string("it has no luban to run it with") : error); return; }
    }
    // The welcome a console says first, the same as `luban enter`'s.
    std::string greeting;
    if (env)
        for (const auto& line : service::welcome(*env, machine ? "machine" : "sandbox", false,
                                                 {.zh = xlings::i18n::language() == "zh", .color = true, .utf8 = true}))
            greeting += line + "\r\n";
    console = machine ? Console::machine(home, name, [w] { w->wake(); }, error, greeting)
                      : Console::sandbox(ctx.port.xlings(), home, name, env ? env->settings.persist : "all", [w] { w->wake(); },
                                         error, greeting);
    if (!console) {
        notice = error;
        view = View::Launcher;
        return;
    }
    console_scroll = 0;
    selection.reset();
    view = View::Console;
    // Why it is the console and not the desktop, said over it for a while
    // (E7: the reason was cleared as the console opened).
    if (!notice.empty()) notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    window->set_title(title_for(name));
    window->text_input(true);
}

// A program alone in the window that could not be shown: said where it was asked, and the window goes.
void App::app_failed_(const std::string& why) {
    std::println(std::cerr, "luban-os: {} could not be shown: {}", once_app, why);
    exit_code = 1;
    quit = true;
}

void App::power_off(const std::string& name) {
    start_job(std::format("{} {}", tr("luban.os.state.powering_off"), name),
              [name](Job& j, service::Context c) { j.code = vm::stop(c, name).code; },
              [](App& a) { a.view = View::Off; });
}

bool App::is_running(const std::string& name) {
    const auto now = std::chrono::steady_clock::now();
    if (now - running_at > std::chrono::seconds(1)) {
        running_cache.clear();
        running_at = now;
    }
    auto it = running_cache.find(name);
    if (it == running_cache.end()) it = running_cache.emplace(name, vm::running(home, name).has_value()).first;
    return it->second;
}

float App::console_size(const std::string& name) const {
    if (auto it = settings.console_sizes.find(name); it != settings.console_sizes.end()) return it->second;
    return settings.console_size;
}

void App::back_home() {
    once_desktop.clear();
    once_console = false;
    menu = false;
    console.reset();
    close_display();
    view = View::Launcher;
    refresh();
    window->set_title("Luban OS");
}

fs::path home_dir_() {
    if (const char* h = std::getenv("XLINGS_HOME"); h && *h) return h;
    if (const char* u = std::getenv("HOME"); u && *u) return fs::path(u) / ".xlings";
    return ".xlings";
}

int edition_index_(std::string_view name) {
    for (std::size_t i = 0; i < kEditions.size(); ++i) if (kEditions[i].name == name) return static_cast<int>(i);
    return 3;   // core
}

void append_utf8_(std::string& out, char32_t ch) {
    if (ch < 0x80) out += static_cast<char>(ch);
    else if (ch < 0x800) { out += static_cast<char>(0xC0 | (ch >> 6)); out += static_cast<char>(0x80 | (ch & 0x3F)); }
    else if (ch < 0x10000) {
        out += static_cast<char>(0xE0 | (ch >> 12)); out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (ch & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (ch >> 18)); out += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F)); out += static_cast<char>(0x80 | (ch & 0x3F));
    }
}

std::optional<std::string> App::session_now(const service::Environment& e) const {
    if (!once_app.empty()) return "app:" + once_app;
    if (once_console) return std::nullopt;
    if (!once_desktop.empty() && std::ranges::find(e.desktops, once_desktop) != e.desktops.end()) return once_desktop;
    return desktop_session(e);
}

}  // namespace luban::os
