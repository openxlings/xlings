// The window's life (Luban OS architecture design §14): its loop, the status-area
// icon, and a file dropped on it.
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
import luban.compat;
import luban.desktop.host;
import opensal.wlr;
import :core;

namespace luban::os {

// ── the status-area icon (design 2026-10-16 L §13.12) ──────────────────

// The systems running now: a machine, or a sandbox's session.
std::vector<const service::Environment*> running_(const App& a) {
    std::vector<const service::Environment*> out;
    for (const auto& e : a.systems)
        if (e.running || const_cast<App&>(a).is_running(e.name)) out.push_back(&e);
    return out;
}

// The line to the tray process, made once the icon is wanted (design
// 2026-10-19 §3): the tray started when there is none. Where this desktop
// has no status area, why -- the tray says (its tray.json, its hello).
bool ensure_tray_(App& a) {
    if (!a.tray) {
        auto* w = a.window.get();
        a.tray = std::make_unique<TrayLink>(a.home, [w] { w->wake(); });
        a.tray_said.clear();
    }
    if (a.tray_pid == 0 && a.tray_error.empty()) {
        const auto asked = std::chrono::steady_clock::now();
        if (const auto st = a.tray->status(std::chrono::milliseconds(3000))) {
            a.latency["tray-ready"].push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - asked).count());
            a.tray_pid = st->value("pid", 0);
            a.tray_error = st->value("error", std::string{});
            a.tray_backend = st->value("backend", std::string{});
        } else if (auto why = a.tray->trouble(); !why.empty()) {
            a.tray_error = why;
        } else {
            a.tray_error = "the Luban OS tray did not answer";
        }
    }
    return a.tray_error.empty();
}

// What the tray is told of this window: what it shows and what it is doing
// (only what changed).
void tray_update_(App& a) {
    if (!a.tray) return;
    static constexpr std::array<std::string_view, 9> kViews{"launcher", "quick-start", "job", "console", "off", "admin", "desktop", "import", "run"};
    const bool in_system = a.view == View::Console || a.view == View::Desktop || a.view == View::Run || a.view == View::Off;
    nlohmann::json reg{{"attach", in_system ? a.current : std::string{}}, {"view", std::string(kViews[static_cast<std::size_t>(a.view)])},
                       {"hidden", a.hidden}};
    nlohmann::json job = nullptr;
    if (a.job) {
        std::lock_guard l(a.job->m);
        job = {{"title", a.job->title}, {"percent", a.job->percent}, {"done", a.job->done.load()}, {"code", a.job->code},
               {"error", a.job->errors.empty() ? std::string{} : a.job->errors.front()}};
    }
    const auto said = reg.dump() + job.dump();
    if (said == a.tray_said) return;
    a.tray_said = said;
    a.tray->send({{"register", reg}});
    a.tray->send({{"job", job}});
}

void show_(App& a) {
    a.hidden = false;
    a.window->show();
    tray_update_(a);
}

// The window goes (design 2026-10-19 §3.4): with a job under way it hides and
// finishes it, the tray showing how far; with none the window ends -- the
// machines run on, the tray shows them. Where there is no status area,
// minimized (design 2026-10-16 L §13.11-12).
void to_background_(App& a) {
    a.console.reset();
    a.close_display();
    a.view = View::Launcher;
    a.refresh();
    a.window->set_title("Luban OS");
    a.window->clear_close_request();
    if (ensure_tray_(a)) {
        if (a.job && !a.job->done) {
            a.hidden = true;
            a.window->hide();
            tray_update_(a);
        } else {
            a.hidden = true;
            tray_update_(a);
            a.quit = true;
        }
    } else {
        a.window->minimize();
    }
}

// Leaving the app: with systems running, asked whether they go too; then
// every window of the home ends, and the tray (quit-all).
void quit_(App& a) {
    auto all = [&a] {
        if (a.tray) a.tray->send({{"quit-all", nlohmann::json::object()}});
        a.quit = true;
    };
    const auto on = running_(a);
    if (on.empty()) { all(); return; }
    const auto count = on.size();
    show_(a);
    a.confirm = Confirm{std::string(tr("luban.os.app.quit_luban_os")),
                        std::vformat(tr("luban.os.app.systems_are_running_they_can"), std::make_format_args(count)),
                        std::string(tr("luban.os.app.keep_them_running")), false, all,
                        std::string(tr("luban.os.app.power_off_all")), [&a, all] {
                            for (const auto* e : running_(a))
                                if (vm::running(a.home, e->name)) (void)vm::stop(a.ctx, e->name);
                            all();
                        }};
}

// What the tray asks of this window: to come forward on a view, to ask
// whether to quit, to end. A script's `tray <id>` goes to the tray as if
// chosen at the icon, and comes back the same way.
void tray_take_(App& a) {
    for (auto& what : std::exchange(a.tray_injected, {})) {
        ensure_tray_(a);
        if (a.tray) a.tray->send({{"choose", what}});
        a.tray_chose_at = std::chrono::steady_clock::now();
    }
    if (!a.tray) return;
    for (const auto& m : a.tray->take()) {
        if (m.contains("close")) { a.quit = true; continue; }
        if (m.contains("confirm-quit")) { quit_(a); continue; }
        if (!m.contains("raise")) continue;
        const auto& r = m["raise"];
        const auto view = r.value("view", std::string{"launcher"}), name = r.value("system", std::string{});
        // Chosen at the icon (or as if): until the frame that shows it.
        if (a.tray_chose_at) a.latency_pending["tray-raise"] = *std::exchange(a.tray_chose_at, std::nullopt);
        show_(a);
        if (view == "launcher") {
            const auto on = running_(a);
            if (r.value("activate", false) && on.size() == 1 && a.view == View::Launcher) a.enter(on.front()->name);
        } else if (view == "quick-start") a.view = View::QuickStart;
        else if (view == "settings") { a.before_admin = a.view; a.view = View::Admin; a.admin_tab = "settings"; }
        else if (view == "enter") a.enter(name);
        else if (view == "console") { a.current = name; a.open_console(name, true); }
        else if (view == "manage") {
            a.before_admin = a.view;
            a.view = View::Admin;
            a.admin_tab = "systems";
            for (int i = 0; i < static_cast<int>(a.systems.size()); ++i)
                if (a.systems[static_cast<std::size_t>(i)].name == name) a.admin_selected = i;
        }
        a.tray_said.clear();
        tray_update_(a);
    }
}

// A file dropped on a system's window (design 2026-10-16 L §13.9): copied into
// its login user's ~/Downloads -- the system's tree, which a sandbox shows and
// a machine shares (virtiofs) -- and said. Where it went, or why not.
std::string drop_into_(App& a, const fs::path& file) {
    const auto root = service::root_of(a.home, a.current);   // env-read-ok: a write into its home, a directory here (not its /usr)
    if (!root) return std::string(tr("luban.os.app.system_has_no_tree_own"));
    const auto user = xlings::subos::root_etc::login_user(*root / "etc");
    const auto who = xlings::subos::root_etc::find_user(*root / "etc", user);
    const fs::path inside = who && !who->home.empty() ? fs::path(who->home) : fs::path("/root");
    const auto dir = *root / inside.relative_path() / "Downloads";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto target = dir / file.filename();
    for (int k = 2; fs::exists(target, ec) && k < 100; ++k)
        target = dir / std::format("{} ({}){}", file.stem().string(), k, file.extension().string());
    fs::copy(file, target, fs::copy_options::recursive, ec);
    if (ec) return std::format("{}: {}", file.filename().string(), ec.message());
    const auto shown = file.filename().string(), where = (inside / "Downloads").generic_string();
    return std::vformat(tr("luban.os.app.downloads"), std::make_format_args(shown, where));
}

int run(int argc, char* argv[]) {
    // The tray of this home, no window of its own (design 2026-10-19 §3).
    if (argc >= 2 && std::string_view(argv[1]) == "__tray") {
        register_words();
        service::register_words();
        return run_tray(argc, argv);
    }
    // What this app can do, for whoever is about to rely on it (design
    // 2026-10-19 §10.2): answered before anything opens a display or reads
    // the home, so it works over SSH and in CI.
    register_words();
    service::register_words();
    if (argc >= 2 && std::string_view(argv[1]) == "__capabilities") {
        auto provides = service::luban_provides();
        provides["host-gpu"] = 1;
        provides["app-capabilities"] = 1;
        std::println("{}", service::app_capabilities_json(service::kVersion, provides, {}).dump());
        return 0;
    }
    // Quit from anywhere (design 2026-10-20 §3.1, G20): the tray and every
    // window of this home end; the systems keep running. Not the icon's menu's
    // to be the only way.
    if (argc >= 2 && std::string_view(argv[1]) == "--quit") {
        const auto home = home_dir_();
        auto link = platform::LocalStream::connect(tray_dir(home) / "tray.sock", std::chrono::milliseconds(500));
        if (!link) { std::println("luban-os: not running (no tray for {})", home.string()); return 0; }
        if (!link->send_all(nlohmann::json{{"quit-all", nlohmann::json::object()}}.dump() + "\n", std::chrono::seconds(2))) {
            std::println(std::cerr, "luban-os: the tray did not take the request");
            return 1;
        }
        std::println("luban-os: the tray and its windows are closing; running systems keep running (luban ls)");
        return 0;
    }
    App a;
    host::Options o;
    int frames = -1;
    std::string screenshot;
    std::optional<Script> script;
    std::vector<std::string> desktop_exec;   // tests: a desktop from any program speaking opensal.wire
    std::string desktop_session_name;        // tests: the session it stands for
    bool quit_asked = false, console_asked = false;
    std::string manage_system;
    std::vector<std::string> console_exec;   // tests: a console of any program, on a pseudo-terminal
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--attach" && i + 1 < argc) a.attach = argv[++i];
        else if (arg == "--desktop" && i + 1 < argc) a.once_desktop = argv[++i];
        else if (arg == "--app" && i + 1 < argc) a.once_app = argv[++i];
        else if (arg == "--headless") {
            o.hidden = true;
            platform::set_env_variable("SDL_VIDEODRIVER", "offscreen");
        }
        else if (arg == "--density" && i + 1 < argc) a.density_override = std::atof(argv[++i]);   // tests: a screen's density
        else if (arg == "--scale" && i + 1 < argc) o.scale = static_cast<float>(std::atof(argv[++i]));   // tests: a display's scale
        else if (arg == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (arg == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else if (arg == "--dump-state") a.dump_state = true;
        else if (arg == "--input" && i + 1 < argc) a.input = argv[++i];
        else if (arg == "--script" && i + 1 < argc) {
            script = read_script_(argv[++i]);
            if (!script) { std::println(std::cerr, "luban-os: cannot read the script {}", argv[i]); return 2; }
        }
        else if (arg == "--desktop-session" && i + 1 < argc) desktop_session_name = argv[++i];
        else if (arg == "--desktop-exec" && i + 1 < argc) { desktop_exec.assign(argv + i + 1, argv + argc); break; }
        else if (arg == "--console-exec" && i + 1 < argc) { console_exec.assign(argv + i + 1, argv + argc); break; }
        else if (arg == "--view" && i + 1 < argc) {
            const std::string_view v = argv[++i];
            if (v == "admin") { a.before_admin = View::Launcher; a.view = View::Admin; }
            else if (v == "quick-start") a.view = View::QuickStart;
            else if (v == "settings") { a.before_admin = View::Launcher; a.view = View::Admin; a.admin_tab = "settings"; }
            // Started by the tray for what it has no window for (design 2026-10-19 §3.3).
            else if (v == "quit") quit_asked = true;
            else if (v == "console") console_asked = true;
            else if (v == "manage") { a.before_admin = View::Launcher; a.view = View::Admin; a.admin_tab = "systems"; }
        }
        else if (arg == "--system" && i + 1 < argc) manage_system = argv[++i];
        else if (arg == "--version") { std::println("luban-os {}", service::kVersion); return 0; }
        else if (arg == "--help" || arg == "-h") {
            std::println("luban-os [--attach <name> [--desktop <session> | --app <command>]]   Luban OS: the systems of this home, in a window");
            std::println("luban-os --quit   the tray and every window of this home end (the systems keep running)");
            return 0;
        }
    }
    set_language(a.settings.language);
    auto window = host::Window::open(o);
    if (!window) {
        std::println(std::cerr, "luban-os: {}", window.error());
        return 1;
    }
    a.window = std::move(*window);
    a.window->set_title("Luban OS");
    // What draws it. SDL falls back to its software renderer without a word
    // when no GPU is reached (E13: luban-os installed without the graphics
    // stack took half a core for a moving pointer): said, with the remedy,
    // unless software was asked for.
    a.perf.renderer = a.window->renderer();
    if (const char* asked = std::getenv("SDL_RENDER_DRIVER");
        a.perf.renderer == "software" && (!asked || std::string_view(asked) != "software")) {
        a.notice = platform::is_linux
            ? std::string(tr("luban.os.app.drawing_without_gpu_no_graphics"))
            : std::string(tr("luban.os.app.drawing_without_gpu"));
        std::println(std::cerr, "luban-os: the renderer is SDL's software one (no GPU reached)");
    }
    // The app's look: its theme on imkit, its background behind every page.
    imkit::set_theme(art::theme(a.window->ui_font(), a.window->title_font()));
    {
        auto& colors = ImGui::GetStyle().Colors;
        colors[ImGuiCol_WindowBg] = ImGui::ColorConvertU32ToFloat4(imkit::theme().bg);
        colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_TextSelectedBg] = ImGui::ColorConvertU32ToFloat4(imkit::theme().accent_lo);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_ScrollbarGrab] = ImGui::ColorConvertU32ToFloat4(imkit::theme().panel);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImGui::ColorConvertU32ToFloat4(imkit::theme().panel_hi);
        colors[ImGuiCol_ScrollbarGrabActive] = ImGui::ColorConvertU32ToFloat4(imkit::theme().accent_lo);
        auto& style = ImGui::GetStyle();
        style.ScrollbarSize = std::round(8 * a.window->ui_scale());
        style.ScrollbarRounding = 0;
    }
    imkit::on_anchor([&a](std::string_view name, ImVec2 mn, ImVec2 mx) { a.anchors[std::string(name)] = {mn.x, mn.y, mx.x, mx.y}; },
                     [&a](std::string_view name) { a.clipped.emplace(name); });
    if (auto refused = a.ctx.port.handshake()) a.notice = *refused;
    a.refresh();
    if (!console_exec.empty()) {
        std::string error;
        auto* win = a.window.get();
        a.current = "console";
        a.console = Console::process(console_exec, platform::environment(), a.current, [win] { win->wake(); }, error);
        if (!a.console) { std::println(std::cerr, "luban-os: {}", error); return 1; }
        a.view = View::Console;
        a.window->text_input(true);
    } else if (!desktop_exec.empty()) {
        std::string error;
        const auto [w, h] = a.window->size();
        a.desktop_size = a.device_size_(w, h);
        auto* win = a.window.get();
        a.current = "desktop";
        a.display = Display::process(desktop_exec, platform::environment(), a.home / "logs" / "luban-os.log", a.current,
                                     a.desktop_size, {.copy = true, .paste = true}, [win] { win->wake(); }, error,
                                     desktop_session_name);
        if (!a.display) { std::println(std::cerr, "luban-os: {}", error); return 1; }
        a.view = View::Desktop;
        a.window->set_title(a.title_for(a.current));
    } else if (!a.attach.empty() && console_asked) {
        a.current = a.attach;
        a.open_console(a.attach, true);
    } else if (!a.attach.empty()) a.enter(a.attach);
    else if (a.view == View::Admin || a.view == View::QuickStart) {}
    else if (a.systems.empty()) a.view = View::QuickStart;
    else if (a.settings.countdown > 0) a.countdown_until = std::chrono::steady_clock::now() + std::chrono::seconds(a.settings.countdown);
    if (!manage_system.empty())
        for (int i = 0; i < static_cast<int>(a.systems.size()); ++i)
            if (a.systems[static_cast<std::size_t>(i)].name == manage_system) a.admin_selected = i;
    if (quit_asked) quit_(a);
    // This window lives while it holds its lock (the tray and the others tell it so).
    {
        std::error_code ec;
        fs::create_directories(tray_dir(a.home) / "windows", ec);
        std::string why;
        (void)a.alive.try_acquire(tray_dir(a.home) / "windows" / std::format("{}.lock", platform::current_pid()), why);
    }
    // Luban OS of before, each window with an icon of its own (design
    // 2026-10-19 §3.6): said, never ended for the person.
    if (a.notice.empty()) {
        std::vector<int> registered{platform::current_pid()};
        std::error_code ec;
        for (fs::directory_iterator it(tray_dir(a.home) / "windows", ec), end; !ec && it != end; it.increment(ec)) {
            platform::FileLock probe;
            std::string why;
            if (!probe.try_acquire(it->path(), why)) {
                registered.push_back(std::atoi(it->path().stem().string().c_str()));   // held: a live window
            } else {
                probe.release();
                fs::remove(it->path(), ec);
            }
        }
        std::ifstream tray_json(tray_dir(a.home) / "tray.json");
        if (auto j = nlohmann::json::parse(tray_json, nullptr, false); j.is_object()) registered.push_back(j.value("pid", 0));
        // An old one of this home runs from it (its package store); another home's are its own.
        auto old = luban::compat::unregistered_luban_os(registered);
        std::erase_if(old.found, [&](const auto& w) { return !w.exe.string().starts_with(a.home.string()); });
        if (old.looked && !old.found.empty()) a.notice = std::string(tr("luban.os.app.old_luban_os_running"));
    }
    const auto started = std::chrono::steady_clock::now();
    // Drawn while anything happens (design 2026-10-16 L §13.3): after any
    // input a few frames more, while something animates or runs at 60 fps;
    // only a still window waits long.
    int pending = 3;
    double wake = -1;   // when the last frame's animations next change
    for (int frame = 0; (frames < 0 || frame < frames) && !a.quit; ++frame) {
        if (script && script->finished()) break;
        if (script && std::chrono::steady_clock::now() - started > std::chrono::minutes(10)) {
            script->failure = "the script did not finish in 10 minutes";
            break;
        }
        const bool busy = a.countdown_until || (a.job && !a.job->done) || a.resize_at || a.run.busy;
        // What the scheduler chooses (tests run their own beat, and are told this).
        // Still: until the next step of an animation, at most a second.
        // A shaken pointer growing or shrinking is an animation too.
        int scheduled = (pending > 0 || busy || a.window->pointer_animating() || (wake >= 0 && wake < 0.016)) ? 16
                        : wake < 0 ? 1000 : std::clamp(static_cast<int>(wake * 1000), 16, 1000);
        if (a.toast_until)
            scheduled = std::clamp(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       *a.toast_until - std::chrono::steady_clock::now()).count()) + 1, 16, scheduled);
        // Wanted and not held: asked again (the window tries at most every 500 ms).
        if (a.keyboard_wanted && a.view == View::Desktop && !a.window->keyboard_grabbed()) scheduled = std::min(scheduled, 500);
        if (a.notice_until)   // a notice over a system goes when its time is up
            scheduled = std::clamp(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       *a.notice_until - std::chrono::steady_clock::now()).count()) + 1, 16, scheduled);
        // Without a GPU every frame is the CPU's: what animates runs at 30 a
        // second (the frame after an input still comes at once).
        if (a.perf.renderer == "software" && pending == 0) scheduled = std::max(scheduled, 33);
        // Done well: "ready" shown its moment, then what comes next.
        if (a.view == View::Job && a.job && a.job->done && a.job->code == 0 && a.after_job && !a.job->ready_shown())
            scheduled = std::clamp(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       a.job->done_at + std::chrono::milliseconds(700) - std::chrono::steady_clock::now()).count()) + 1,
                                   1, scheduled);
        if (a.present_due)   // a desktop is to be told its frame was shown
            scheduled = std::clamp(static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       *a.present_due - std::chrono::steady_clock::now()).count()) + 1, 1, scheduled);
        const int wait_ms = script ? 30 : frames >= 0 ? 0 : scheduled;
        if (a.perf.input_at && a.perf.frames_since_input == 1) a.perf.wait_after_input_ms = scheduled;
        // The longest a still window waits since the last input: a stepped
        // animation (a badge's beat) makes the wait before its next step
        // short, the one after it long; resting is the long one.
        if (pending == 0 && !busy) a.perf.idle_wait_ms = std::max(a.perf.idle_wait_ms, scheduled);
        if (!a.window->begin_frame(wait_ms) && !a.close_dialog) {
            // The window is to close: a running machine's on-close decides, and
            // a desktop on the bus's (it runs without the window, design
            // 2026-10-24 §4.3). A sandbox's console ends with its window.
            if ((a.view == View::Console && a.console && a.console->is_machine())
                || (a.view == View::Desktop && a.display && (a.display->is_machine() || a.display->on_bus()))) {
                auto env = service::describe(a.ctx, a.current);
                const auto on_close = env ? env->settings.on_close : std::string("ask");
                if (on_close == "ask") {
                    a.close_dialog = true;
                    a.close_temp = env && env->settings.persist == "none";
                    a.close_choice = a.settings.close_choice;
                    a.close_remember = false;
                } else if (on_close == "poweroff") {
                    if (a.display && a.display->on_bus() && !a.display->is_machine())
                        (void)vm::desktop(a.home, a.current, {{"op", "end"}, {"by", "its window closed (on-close poweroff)"}});
                    else
                        (void)vm::stop(a.ctx, a.current);
                    break;
                } else {
                    to_background_(a);
                }
            } else if (a.settings.keep_in_tray || !running_(a).empty()) {
                // Something still runs, or the app is to stay: the tray.
                to_background_(a);
            } else {
                break;
            }
        }
        if (a.settings.keep_in_tray && !a.tray && a.tray_error.empty()) ensure_tray_(a);
        tray_take_(a);
        if (a.tray && std::chrono::steady_clock::now() - a.tray_checked > std::chrono::seconds(2)) {
            a.tray_checked = std::chrono::steady_clock::now();
            if (a.hidden) a.refresh();
            tray_update_(a);
        }
        // An operation that ended while nobody looked: said by the system.
        if (a.job && a.job->done && !std::exchange(a.job_told, true) && (a.hidden || !a.window->focused()) && ensure_tray_(a)) {
            std::lock_guard l(a.job->m);
            const auto body = a.job->code == 0 ? std::string(tr("luban.os.app.done"))
                                               : (a.job->errors.empty() ? std::string(tr("luban.os.app.failed")) : a.job->errors.front());
            a.tray->send({{"notify", {{"title", a.job->title}, {"body", body},
                                      {"action", a.job->code == 0 && !a.job_subject.empty() ? "made:" + a.job_subject : std::string("open")}}}});
            // Hidden for it, done with it: the window ends; the tray and its notification stay.
            if (a.hidden) a.quit = true;
        }
        const auto frame_start = std::chrono::steady_clock::now();
        if (a.present_due && frame_start >= *a.present_due) {
            a.present_due.reset();
            if (a.display) a.display->send(opensal::FramePresented{});
            a.presented_at = frame_start;
        }
        const bool input = !a.window->events().empty() || !a.window->keys().empty();
        // Over a desktop the pointer moving changes nothing here: it is sent
        // on, and what it changes there comes back as a frame (E13: every
        // move redrew the whole window).
        const bool over_desktop = a.view == View::Desktop && !a.menu && !a.close_dialog && !a.confirm;
        const bool only_motion = input && a.window->keys().empty()
            && std::ranges::all_of(a.window->events(), [](const auto& e) { return std::holds_alternative<opensal::PointerMotion>(e); });
        const auto view_before = a.view;
        if (input && !(over_desktop && only_motion)) {
            pending = 3;
            a.perf.input_at = frame_start;
            a.perf.frames_since_input = 0;
            a.perf.idle_wait_ms = 0;
        } else if (pending > 0) --pending;
        imkit::begin_frame(a.window->ui_scale(), ImGui::GetTime(), a.settings.reduce_motion);
        // Any input cancels the countdown.
        if (a.countdown_until) {
            const bool input = std::ranges::any_of(a.window->events(), [](const auto& e) {
                return std::holds_alternative<opensal::Key>(e) || std::holds_alternative<opensal::PointerButton>(e)
                    || std::holds_alternative<opensal::PointerMotion>(e);
            });
            if (input) a.countdown_until.reset();
            else if (std::chrono::steady_clock::now() >= *a.countdown_until && !a.systems.empty()) a.enter(a.countdown_target());
        }
        if (a.window->host_key() && (a.view == View::Console || a.view == View::Off || a.view == View::Desktop || a.view == View::Run))
            a.menu = !a.menu, a.menu_focus = 0;
        // The keyboard (design 2026-10-19 §16.1): held while a desktop is
        // shown and the window has the focus -- every key its, the host's
        // chords too -- as the setting says when it is entered; Ctrl+Alt+K
        // holds it or lets it go, the pointer too when the desktop held it.
        {
            for (const auto& e : a.window->events())
                if (const auto* f = std::get_if<opensal::Focus>(&e)) a.focus_out = !f->in;
            const bool in_desktop = a.view == View::Desktop && a.display && a.display->alive();
            if (in_desktop && !a.was_in_desktop) a.keyboard_wanted = a.settings.lock_keyboard;
            a.was_in_desktop = in_desktop;
            if (in_desktop) {
                if (const auto asked = a.display->take_pointer_lock()) a.pointer_lock_asked = *asked;
                // Wanted or not: what is held is the host's answer (design 2026-10-20 §5).
                if (a.window->lock_key()) {
                    const bool release = a.keyboard_wanted || a.window->pointer_locked();
                    a.keyboard_wanted = !release;
                    if (release) a.pointer_lock_asked = false;
                    a.keyboard_told = true;
                }
            }
            const bool grab = in_desktop && a.keyboard_wanted && !a.focus_out;
            const auto held = a.window->grab_keyboard(grab);
            if (held != a.keyboard_shown) {
                // Said as it changes: the title while held; a moment at the top
                // when the person asked (Ctrl+Alt+K, the menu) or the host let
                // it go -- not on entering, nor on a focus change (a still
                // desktop draws nothing).
                using G = host::Window::Grab;
                const bool refused = held == G::HeldByAnother || held == G::NotShown || held == G::Refused;
                if ((held == G::Held && a.keyboard_told) || (held == G::Released && a.keyboard_shown == G::Held && !a.focus_out)
                    || (refused && a.keyboard_told)) {
                    const std::string why(tr(held == G::NotShown        ? "luban.os.app.keyboard_not_shown"
                                             : held == G::HeldByAnother ? "luban.os.app.keyboard_host_holds"
                                                                        : "luban.os.app.keyboard_host_refused"));
                    a.toast(held == G::Held        ? std::string(tr("luban.os.app.keyboard_held"))
                            : held == G::Released ? std::string(tr("luban.os.app.keyboard_let_go"))
                                                  : std::vformat(tr("luban.os.app.keyboard_refused"), std::make_format_args(why)));
                }
                if (!refused) a.keyboard_told = false;   // a refusal is tried again: its success is said too
                a.keyboard_shown = held;
                if (in_desktop) a.window->set_title(a.title_for(a.current));
            }
            const bool lock = in_desktop && a.pointer_lock_asked && !a.focus_out;
            if (lock != a.window->pointer_locked()) a.window->lock_pointer(lock);
            // A Super key the host kept (the keyboard not held): Ctrl+Alt+K
            // said, once for this person (design 2026-10-21 §8.10).
            if (a.window->take_super_unlocked() && in_desktop && !a.settings.super_hint_said) {
                a.toast(std::string(tr("luban.os.app.super_needs_hold")));
                a.settings.super_hint_said = true;
                write_app_settings(a.home, a.settings);
            }
        }
        // The menu lists the system's desktops: read when it opens, not each frame.
        if (a.menu && a.menu_desktops.empty())
            if (auto env = service::describe(a.ctx, a.current)) a.menu_desktops = env->desktops;
        if (!a.menu) a.menu_desktops.clear();
        const bool overlay = a.menu || a.close_dialog || a.confirm;
        const bool typing = ImGui::GetIO().WantTextInput;
        for (const auto& k : a.window->keys())
            if (k.name == "F2" && !overlay && a.view != View::Console && a.view != View::Desktop && a.view != View::Run) {
                a.before_admin = a.view;
                a.view = View::Admin;
                if (a.selected < static_cast<int>(a.systems.size())) a.admin_selected = a.selected;
            }
        if (a.view == View::Launcher && !overlay) {
            const auto shown = shown_(a);
            const int rows = static_cast<int>(shown.size()) + 1;
            for (const auto& k : a.window->keys()) {
                const bool ctrl = k.key.mods & opensal::Ctrl;
                if (k.name == "Down") { a.selected = (a.selected + 1) % rows; a.keyboard = a.follow = true; }
                else if (k.name == "Up") { a.selected = (a.selected + rows - 1) % rows; a.keyboard = a.follow = true; }
                else if (k.name == "Enter") {
                    if (a.selected >= static_cast<int>(shown.size())) a.view = View::QuickStart;
                    else a.enter(a.systems[static_cast<std::size_t>(shown[static_cast<std::size_t>(a.selected)])].name);
                } else if (k.name == "Delete" && a.selected < static_cast<int>(shown.size())) {
                    const auto name = a.systems[static_cast<std::size_t>(shown[static_cast<std::size_t>(a.selected)])].name;
                    a.confirm = Confirm{std::vformat(tr("luban.os.app.remove"), std::make_format_args(name)),
                                        std::string(tr("luban.os.app.home_every_file_are_deleted")),
                                        std::string(tr("luban.os.app.remove_2")), true, [&a, name] {
                                            a.start_job(std::format("{} {}", tr("luban.os.app.removing"), name),
                                                        [name](Job& j, service::Context c) { j.code = service::remove(c, name).code; },
                                                        [](App& app) { app.refresh(); app.view = View::Launcher; });
                                        }};
                } else if (k.name == "Escape") { a.filter.clear(); a.selected = 0; }
                else if (k.name == "Backspace" && !a.filter.empty()) {
                    a.filter.pop_back();
                    while (!a.filter.empty() && (static_cast<unsigned char>(a.filter.back()) & 0xC0) == 0x80) a.filter.pop_back();
                    if ((static_cast<unsigned char>(a.filter.empty() ? 0 : a.filter.back()) & 0x80) && !a.filter.empty()) a.filter.pop_back();
                    a.selected = 0;
                } else if (ctrl && (k.name == "o" || k.name == "O"))
                    a.window->pick_file(std::string(tr("luban.os.app.import_luban_system")), "Luban", "luban");
            }
            // Typed on the home: the names shown.
            for (const auto& e : a.window->events())
                if (const auto* t = std::get_if<opensal::TextCommit>(&e); t && !t->text.empty() && t->text != " ") {
                    a.filter += t->text;
                    a.selected = 0;
                    a.countdown_until.reset();
                }
        }
        if (a.view == View::QuickStart && !overlay && !typing) quick_keys_(a);
        if (a.view == View::Admin && !overlay && !typing)
            for (const auto& k : a.window->keys())
                if (k.name == "Escape") { a.view = a.display ? View::Desktop : a.console ? View::Console : View::Launcher; a.refresh(); }
        if (a.view == View::Job && a.job && !a.job->done && !overlay)
            for (const auto& k : a.window->keys())
                if (k.name == "Escape" && a.job->cancel) a.job->cancel->store(true);
        // A pointer that moved: its focus is drawn no more until the keys move it.
        for (const auto& e : a.window->events())
            if (std::holds_alternative<opensal::PointerButton>(e)) a.keyboard = false;
        if (!a.input.empty() && a.view == View::Console && a.console) {
            bool shown = false;
            {
                std::lock_guard l(a.console->lock());
                shown = a.console->screen().cursor_col() > 0 || a.console->screen().cursor_row() > 0;
            }
            if (shown) {
                std::string text;
                for (std::size_t k = 0; k < a.input.size(); ++k) {
                    if (a.input[k] == '\\' && k + 1 < a.input.size() && a.input[k + 1] == 'n') { text += '\r'; ++k; }
                    else text += a.input[k];
                }
                a.console->write(text);
                a.input.clear();
            }
        }
        // Tests: typed into the desktop once it shows something -- characters,
        // `\n` for Return, `{Alt+Return}`-style chords -- a second apart per chord.
        if (!a.input.empty() && a.view == View::Desktop && a.display && a.texture.handle) {
            if (!a.input_at) a.input_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            if (std::chrono::steady_clock::now() >= *a.input_at) {
                a.input = type_desktop_(*a.display, a.input);
                a.input_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
        }
        // A .luban file -- picked in the dialog, or dropped on the window --
        // opens its preview (not over a system in use); any other file
        // dropped on a system goes into it.
        {
            std::optional<fs::path> file = a.window->picked();
            if (const auto why = a.window->pick_error(); !why.empty())
                a.notice = std::vformat(tr("luban.os.app.no_file_dialog_here_drop"), std::make_format_args(why));
            if (file && file->empty()) file.reset();   // the dialog was cancelled
            const bool in_system = a.view == View::Console || a.view == View::Desktop || a.view == View::Run;
            for (const auto& d : a.window->dropped()) {
                if (d.extension() == ".luban" && !in_system) file = d;
                else if (in_system) {
                    a.notice = drop_into_(a, d);
                    a.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                }
            }
            const bool free = a.view == View::Launcher || a.view == View::QuickStart || a.view == View::Admin || a.view == View::Import;
            if (file && free) {
                std::vector<std::string> taken;
                for (const auto& sys : a.systems) taken.push_back(sys.name);
                a.import = preview_import_(*file, taken);
                a.countdown_until.reset();
                a.view = View::Import;
            }
        }
        if (a.view == View::Job && a.job && a.job->done && a.job->code == 0 && a.after_job && a.job->ready_shown()) {
            auto then = std::move(a.after_job);
            a.after_job = nullptr;
            then(a);
        }
        a.anchors_before = std::move(a.anchors);
        a.anchors.clear();
        a.clipped.clear();
        // The quick start opened: until its first frame; its index's word, until it shows.
        if (a.view == View::QuickStart && a.view_drawn != View::QuickStart) {
            a.latency_pending["quick-start"] = frame_start;
            if (!a.quick_opened_at) a.quick_opened_at = frame_start;
        }
        a.view_drawn = a.view;
        switch (a.view) {
        case View::Launcher: launcher_(a); break;
        case View::QuickStart: quick_start_(a); break;
        case View::Job: job_(a); break;
        case View::Console: console_view_(a); break;
        case View::Off: off_(a); break;
        case View::Admin: admin_(a); break;
        case View::Desktop: desktop_view_(a); break;
        case View::Import: import_view_(a); break;
        case View::Run: run_view_(a); break;
        }
        // A notice over a system: a line at the bottom, for a moment.
        if (a.notice_until && (a.view == View::Console || a.view == View::Desktop || a.view == View::Run)) {
            if (std::chrono::steady_clock::now() < *a.notice_until) {
                auto* dl = ImGui::GetForegroundDrawList();
                const auto* vp = ImGui::GetMainViewport();
                const auto ts = imkit::text_size(a.notice, 14);
                const ImVec2 p(std::round(vp->Pos.x + (vp->Size.x - ts.x) / 2 - px(12)), std::round(vp->Pos.y + vp->Size.y - ts.y - px(28)));
                imkit::panel(dl, p, ImVec2(p.x + ts.x + px(24), p.y + ts.y + px(12)), imkit::Bevel::Raised, imkit::theme().panel);
                imkit::text(dl, ImVec2(p.x + px(12), p.y + px(6)), a.notice, imkit::theme().text, 14);
            } else {
                a.notice_until.reset();
                a.notice.clear();
            }
        }
        // A moment's word at the top (the keyboard held or let go).
        if (a.toast_until) {
            if (std::chrono::steady_clock::now() < *a.toast_until) {
                auto* dl = ImGui::GetForegroundDrawList();
                const auto* vp = ImGui::GetMainViewport();
                const auto ts = imkit::text_size(a.toast_text, 14);
                const ImVec2 p(std::round(vp->Pos.x + (vp->Size.x - ts.x) / 2 - px(12)), std::round(vp->Pos.y + px(40)));
                imkit::panel(dl, p, ImVec2(p.x + ts.x + px(24), p.y + ts.y + px(12)), imkit::Bevel::Raised, imkit::theme().panel);
                imkit::text(dl, ImVec2(p.x + px(12), p.y + px(6)), a.toast_text, imkit::theme().text, 14);
            } else {
                a.toast_until.reset();
            }
        }
        if (a.menu) host_menu_(a);
        if (a.close_dialog) close_dialog_(a);
        if (a.confirm) confirm_(a);
        wake = imkit::wake_in();
        const auto drawn = std::chrono::steady_clock::now();
        // A desktop frame that shows what the last one showed is not drawn
        // again: no new picture, no input but the pointer's, nothing over it.
        const bool still = over_desktop && a.view == View::Desktop && view_before == View::Desktop && !a.desktop_changed
                           && (!input || only_motion) && !a.notice_until && !a.toast_until && !a.resize_at && a.perf.presented > 0;
        a.window->end_frame(!still);
        if (!still) {
            ++a.perf.presented;
            const auto shown = std::chrono::steady_clock::now();
            for (const auto& [what, since] : std::exchange(a.latency_pending, {}))
                a.latency[what].push_back(std::chrono::duration<double, std::milli>(shown - since).count());
        }
        {
            // This frame's time; and, the frame after an input, how long until
            // what the input did is on the screen (the input is taken in one
            // frame and shown by the next).
            const auto end = std::chrono::steady_clock::now();
            a.perf.frame_ms.push_back(std::chrono::duration<double, std::milli>(end - frame_start).count());
            a.perf.present_ms.push_back(std::chrono::duration<double, std::milli>(end - drawn).count());
            if (a.perf.frame_ms.size() > 600) { a.perf.frame_ms.pop_front(); a.perf.present_ms.pop_front(); }
            ++a.perf.frames;
            if (a.perf.input_at && ++a.perf.frames_since_input == 2) {
                a.perf.input_to_next_frame_ms = std::chrono::duration<double, std::milli>(end - *a.perf.input_at).count();
                a.perf.input_to_next_frame_max = std::max(a.perf.input_to_next_frame_max, a.perf.input_to_next_frame_ms);
            }
        }
        if (auto later = std::exchange(a.later, nullptr)) later();
        if (std::exchange(a.refresh_after_frame, false)) a.refresh();
        if (script) script_step_(a, *script);
    }
    if (a.dump_state) std::println("{}", dump_(a));
    if (!screenshot.empty()) write_screenshot_(*a.window, screenshot);
    if (script && !script->failure.empty()) {
        std::println(std::cerr, "luban-os script: {}", script->failure);
        return 3;
    }
    return a.exit_code;
}

}  // namespace luban::os
