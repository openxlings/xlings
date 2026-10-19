// A system shown: its console, a system without a shell (run), its desktop.
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

// The console's pointer (design 2026-10-18 §12, E16). A program that takes
// the mouse (mouse_mode) is sent its buttons, wheel and motion as xterm
// does; with Shift held, or a program that does not, it is the window's: a
// drag selects (copied when it ends), two clicks a word, three a line, the
// middle button pastes the last selection, the right one opens Copy / Paste,
// and the wheel scrolls back.
void console_mouse_(App& a, int mouse, bool sgr) {
    if (!ImGui::IsWindowHovered() && !a.selecting && a.mouse_held < 0) return;
    const auto m = ImGui::GetMousePos();
    const int col = static_cast<int>(std::floor((m.x - a.grid.x) / a.grid.cell_w));
    const int row = static_cast<int>(std::floor((m.y - a.grid.y) / a.grid.cell_h));
    const bool inside = row >= 0 && col >= 0 && row < a.grid.rows && col < a.grid.cols;
    const int r = std::clamp(row, 0, std::max(0, a.grid.rows - 1)), c = std::clamp(col, 0, std::max(0, a.grid.cols - 1));
    const auto& io = ImGui::GetIO();
    std::uint32_t mods = 0;
    if (io.KeyShift) mods |= opensal::Shift;
    if (io.KeyCtrl) mods |= opensal::Ctrl;
    if (io.KeyAlt) mods |= opensal::Alt;
    // The program's, unless Shift says the window's.
    if (mouse != 0 && !io.KeyShift && !a.selecting && !a.console_menu) {
        using term::MouseEvent;
        constexpr std::array<int, 3> buttons{ImGuiMouseButton_Left, ImGuiMouseButton_Middle, ImGuiMouseButton_Right};
        for (int b = 0; b < 3; ++b) {
            if (ImGui::IsMouseClicked(buttons[static_cast<std::size_t>(b)]) && inside) {
                a.console->write(term::encode_mouse(MouseEvent::Press, b, r, c, mods, sgr));
                a.mouse_held = b;
                a.mouse_cell = {r, c};
            }
            if (ImGui::IsMouseReleased(buttons[static_cast<std::size_t>(b)]) && a.mouse_held == b) {
                a.console->write(term::encode_mouse(MouseEvent::Release, b, r, c, mods, sgr));
                a.mouse_held = -1;
            }
        }
        // Motion: with a button held (1002), or any (1003); once a cell.
        if (std::pair(r, c) != a.mouse_cell && (mouse == 1003 || (mouse == 1002 && a.mouse_held >= 0)) && (inside || a.mouse_held >= 0)) {
            a.console->write(term::encode_mouse(MouseEvent::Motion, a.mouse_held, r, c, mods, sgr));
            a.mouse_cell = {r, c};
        }
        if (inside && io.MouseWheel != 0) {
            const auto ev = io.MouseWheel > 0 ? MouseEvent::WheelUp : MouseEvent::WheelDown;
            for (int k = 0; k < std::min(3, static_cast<int>(std::ceil(std::abs(io.MouseWheel)))); ++k)
                a.console->write(term::encode_mouse(ev, 0, r, c, mods, sgr));
        }
        return;
    }
    if (io.MouseWheel != 0 && ImGui::IsWindowHovered()) a.console_scroll = std::max(0, a.console_scroll + static_cast<int>(io.MouseWheel * 3));
    auto copy = [&] {
        if (!a.selection) return;
        a.primary = console_text(*a.console, a.console_scroll, *a.selection);
        a.window->set_clipboard(a.primary);
    };
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inside && !a.console_menu) {
        // Clicks on one cell in quick succession: a word, then its line. A
        // click a frame or two after the last came as quickly, however long
        // the frames took (a busy machine's queued input).
        const auto now = std::chrono::steady_clock::now();
        const int frame = ImGui::GetFrameCount();
        const bool quick = now - a.clicked_at < std::chrono::milliseconds(400) || frame - a.clicked_frame <= 2;
        a.clicks = quick && a.clicked_cell == std::pair(r, c) ? a.clicks + 1 : 1;
        a.clicked_at = now;
        a.clicked_frame = frame;
        a.clicked_cell = {r, c};
        if (a.clicks == 2) { a.selection = console_word(*a.console, a.console_scroll, r, c); copy(); }
        else if (a.clicks >= 3) { a.selection = std::array<int, 4>{r, 0, r, a.grid.cols - 1}; copy(); a.clicks = 0; }
        else { a.selecting = true; a.selection = std::array<int, 4>{r, c, r, c}; }
    } else if (a.selecting && ImGui::IsMouseDown(ImGuiMouseButton_Left) && a.selection) {
        (*a.selection)[2] = r, (*a.selection)[3] = c;
    }
    if (a.selecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        a.selecting = false;
        if (a.selection && ((*a.selection)[0] != (*a.selection)[2] || (*a.selection)[1] != (*a.selection)[3])) copy();
        else a.selection.reset();
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && inside) {
        const auto text = a.primary.empty() ? a.window->clipboard() : a.primary;
        bool bracketed;
        {
            std::lock_guard l(a.console->lock());
            bracketed = a.console->screen().bracketed_paste();
        }
        a.console->write(term::encode_paste(text, bracketed));
        a.console_scroll = 0;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && inside) a.console_menu = m;
}

// The right button's menu over the console: Copy (what is selected), Paste.
void console_menu_(App& a) {
    if (!a.console_menu) return;
    const ImVec2 at = *a.console_menu;
    const ImVec2 wo = ImGui::GetWindowPos();
    auto* dl = ImGui::GetWindowDrawList();
    const float w = px(140), h = px(84);
    imkit::panel(dl, at, ImVec2(at.x + w, at.y + h), imkit::Bevel::Raised, th_().panel_lo);
    bool done = false;
    ImGui::SetCursorPos(ImVec2(at.x - wo.x + px(6), at.y - wo.y + px(6)));
    if (imkit::button("console:copy", tr("luban.os.system.copy"), ImVec2((w - px(12)) / imkit::scale(), 32), imkit::Look::Ghost, a.selection.has_value())) {
        if (a.selection) a.window->set_clipboard(console_text(*a.console, a.console_scroll, *a.selection));
        done = true;
    }
    mark_(a, "console:copy");
    ImGui::SetCursorPos(ImVec2(at.x - wo.x + px(6), at.y - wo.y + px(44)));
    if (imkit::button("console:paste", tr("luban.os.system.paste"), ImVec2((w - px(12)) / imkit::scale(), 32), imkit::Look::Ghost)) {
        bool bracketed;
        {
            std::lock_guard l(a.console->lock());
            bracketed = a.console->screen().bracketed_paste();
        }
        a.console->write(term::encode_paste(a.window->clipboard(), bracketed));
        a.console_scroll = 0;
        done = true;
    }
    mark_(a, "console:paste");
    const bool over = ImGui::IsMouseHoveringRect(at, ImVec2(at.x + w, at.y + h));
    if (done || (!over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) a.console_menu.reset();
}

void console_view_(App& a) {
    begin_full_("console");
    if (!a.console) { ImGui::End(); return; }
    const float bar = system_bar_(a, a.console->is_machine() ? tr("luban.os.app.machine") : tr("luban.os.app.sandbox"));
    // Keys the program gets: text from the input method; named keys and
    // control chords as xterm sends them. The size keys are the window's.
    if (!a.menu && !a.close_dialog) {
        bool app_cursor, bracketed, sgr;
        int mouse = 0;
        {
            std::lock_guard l(a.console->lock());
            app_cursor = a.console->screen().application_cursor();
            bracketed = a.console->screen().bracketed_paste();
            mouse = a.console->screen().mouse_mode();
            sgr = a.console->screen().mouse_sgr();
        }
        for (const auto& e : a.window->events())
            if (auto* t = std::get_if<opensal::TextCommit>(&e)) { a.console->write(t->text); a.console_scroll = 0; a.selection.reset(); }
        for (const auto& k : a.window->keys()) {
            const auto mods = k.key.mods;
            const bool ctrl = mods & opensal::Ctrl, shift = mods & opensal::Shift;
            if (ctrl && !shift && (k.name == "=" || k.name == "+" || k.name == "-" || k.name == "0")) {
                float size = a.console_size(a.current);
                size = k.name == "0" ? a.settings.console_size : std::clamp(size + (k.name == "-" ? -1.0f : 1.0f), 8.0f, 40.0f);
                a.settings.console_sizes[a.current] = size;
                write_app_settings(a.home, a.settings);
                continue;
            }
            if (ctrl && shift && (k.name == "V" || k.name == "v")) {
                a.console->write(term::encode_paste(a.window->clipboard(), bracketed));
                continue;
            }
            if (ctrl && shift && (k.name == "C" || k.name == "c")) {
                if (a.selection) a.window->set_clipboard(console_text(*a.console, a.console_scroll, *a.selection));
                continue;
            }
            if (shift && (k.name == "PageUp" || k.name == "PageDown")) {
                a.console_scroll = std::max(0, a.console_scroll + (k.name == "PageUp" ? 1 : -1) * std::max(1, a.grid.rows - 1));
                continue;
            }
            const bool printable = k.name.size() == 1;
            if (printable && !(mods & (opensal::Ctrl | opensal::Alt))) continue;   // came as text
            a.console->write(term::encode_key(k.name, mods & (opensal::Shift | opensal::Ctrl | opensal::Alt), app_cursor));
            a.console_scroll = 0;
        }
        console_mouse_(a, mouse, sgr);
        // What the program put on the clipboard (OSC 52), when it may.
        std::optional<std::string> wanted;
        {
            std::lock_guard l(a.console->lock());
            wanted = a.console->screen().take_clipboard();
        }
        if (wanted && a.clipboard_grant(a.current).copy) a.window->set_clipboard(*wanted);
    }
    ImGui::SetCursorPos(ImVec2(0, bar));
    a.grid = draw_console(*a.console, {.font = a.window->mono_font(), .bold = a.window->mono_bold_font(),
                                       .size = a.console_size(a.current), .padding = imkit::unit(),
                                       .focused = !a.menu, .scroll = a.console_scroll, .selection = a.selection});
    mark_(a, "console");
    console_menu_(a);
    a.console->resize(a.grid.rows, a.grid.cols);
    ImGui::End();
    if (!a.console->alive()) {
        const bool machine = a.console->is_machine();
        // A session that failed says why on the way out: its last lines are
        // the reason shown, not a blank "off".
        a.ended_why.clear();
        if (a.console->exit_code() != 0) {
            std::lock_guard l(a.console->lock());
            auto& screen = a.console->screen();
            std::vector<std::string> lines;
            for (int row = 0; row < screen.rows(); ++row) {
                std::string line;
                for (int col = 0; col < screen.cols(); ++col) {
                    const auto& cell = screen.at(row, col);
                    if (cell.continuation) continue;
                    append_utf8_(line, cell.ch ? cell.ch : U' ');
                }
                while (!line.empty() && line.back() == ' ') line.pop_back();
                if (!line.empty()) lines.push_back(line);
            }
            if (lines.size() > 6) lines.erase(lines.begin(), lines.end() - 6);
            for (const auto& l2 : lines) a.ended_why += l2 + "\n";
            a.ended_code = a.console->exit_code();
        }
        a.console.reset();
        if (machine && vm::running(a.home, a.current)) {
            a.notice = std::vformat(tr("luban.os.system.keeps_running"), std::make_format_args(a.current));
            a.back_home();
        } else {
            a.view = View::Off;
        }
        a.window->set_title("Luban OS");
    }
}

// nano's view: one line a command, run in it, its output kept.
void run_view_(App& a) {
    begin_full_("run");
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    const float bar = system_bar_(a, tr("luban.os.system.commands"));
    const float inputH = px(56);
    // The output.
    ImGui::SetCursorPos(ImVec2(0, bar));
    dl->AddRectFilled(ImVec2(o.x, o.y + bar), ImVec2(o.x + W, o.y + H), IM_COL32(19, 21, 26, 255));
    ImGui::SetCursorPos(ImVec2(imkit::unit(), bar + imkit::unit()));
    ImGui::BeginChild("output", ImVec2(W - imkit::unit() * 2, H - bar - inputH - imkit::unit() * 2), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    {
        std::lock_guard l(a.run.m);
        auto* odl = ImGui::GetWindowDrawList();
        auto* mono = static_cast<ImFont*>(a.window->mono_font());
        const float size = a.console_size(a.current) * imkit::scale();
        const float lh = std::round(size * 1.25f);
        for (const auto& line : a.run.lines) {
            const ImU32 c = line.command ? IM_COL32(115, 239, 247, 255) : line.error ? IM_COL32(239, 125, 87, 255) : IM_COL32(216, 222, 233, 255);
            std::string_view rest = line.text;
            for (;;) {
                const auto nl = rest.find('\n');
                const auto part = rest.substr(0, nl);
                odl->AddText(mono, size, ImGui::GetCursorScreenPos(), c, part.data(), part.data() + part.size());
                ImGui::Dummy(ImVec2(0, lh));
                if (nl == std::string_view::npos) break;
                rest.remove_prefix(nl + 1);
            }
        }
        if (a.run.scroll_down) { ImGui::SetScrollHereY(1.0f); a.run.scroll_down = false; }
    }
    ImGui::EndChild();
    mark_(a, "run:output");
    // The line to type into.
    const float y = H - inputH + px(8);
    ImGui::SetCursorPos(ImVec2(imkit::unit(), y));
    const bool busy = a.run.busy;
    imkit::text(dl, ImVec2(o.x + imkit::unit(), o.y + y + px(10)), busy ? "…" : ">", th_().accent_hi, 16);
    ImGui::SetCursorPos(ImVec2(imkit::unit() + px(20), y));
    if (!ImGui::GetIO().WantTextInput && !a.menu) ImGui::SetKeyboardFocusHere();
    const bool entered = imkit::text_field("run:input", a.run.input, sizeof a.run.input, (W - imkit::unit() * 2 - px(20)) / imkit::scale(),
                                           ImGuiInputTextFlags_EnterReturnsTrue,
                                           tr("luban.os.system.command_e_g_xlings_list"));
    mark_(a, "run:input");
    ImGui::End();
    for (const auto& k : a.window->keys()) {
        if (k.name == "Escape") { a.back_home(); return; }
        if ((k.name == "Up" || k.name == "Down") && !a.run.history.empty()) {
            const int n = static_cast<int>(a.run.history.size());
            a.run.history_at = k.name == "Up" ? (a.run.history_at < 0 ? n - 1 : std::max(0, a.run.history_at - 1))
                                              : (a.run.history_at < 0 ? -1 : a.run.history_at + 1 >= n ? -1 : a.run.history_at + 1);
            const auto& text = a.run.history_at < 0 ? std::string{} : a.run.history[static_cast<std::size_t>(a.run.history_at)];
            std::snprintf(a.run.input, sizeof a.run.input, "%s", text.c_str());
        }
    }
    if (entered && !busy && a.run.input[0]) {
        const std::string line = a.run.input;
        a.run.input[0] = 0;
        a.run.history.push_back(line);
        a.run.history_at = -1;
        auto argv = platform::split_plain_words(line);
        std::lock_guard l(a.run.m);
        a.run.lines.push_back({"> " + line, true});
        a.run.scroll_down = true;
        if (!argv || argv->empty()) {
            a.run.lines.push_back({std::string(tr("luban.os.system.plain_words_only_there_no")), false, true});
            return;
        }
        a.run.busy = true;
        auto* app = &a;
        auto* w = a.window.get();
        a.run.worker = std::make_unique<std::jthread>([app, w, argv = *argv, name = a.current, port = a.ctx.port] {
            std::string pending_err;
            const int rc = port.capability("subos_exec", Json{{"name", name}, {"argv", argv}}, [&](const service::Event& e) {
                if (e.kind == service::Event::Kind::Data && e.data_kind == "subos_exec_output" && e.data.is_object()) {
                    auto text = e.data.value("data", std::string{});
                    if (e.data.value("encoding", std::string{}) == "base64") text = "(binary output)";
                    const bool err = e.data.value("stream", std::string{}) == "stderr";
                    std::lock_guard l(app->run.m);
                    while (!text.empty() && text.back() == '\n') text.pop_back();
                    if (!text.empty()) app->run.lines.push_back({text, false, err});
                    app->run.scroll_down = true;
                } else if (e.kind == service::Event::Kind::Error) {
                    std::lock_guard l(app->run.m);
                    app->run.lines.push_back({e.hint.empty() ? e.text : e.text + " -- " + e.hint, false, true});
                }
                w->wake();
            });
            {
                std::lock_guard l(app->run.m);
                if (rc != 0) app->run.lines.push_back({std::format("(exit {})", rc), false, true});
                app->run.scroll_down = true;
            }
            app->run.busy = false;
            w->wake();
        });
    }
}

// Reads an archive for its preview: what it is and what it may do.
ImportPreview preview_import_(const fs::path& file, const std::vector<std::string>& taken) {
    ImportPreview p;
    p.file = file;
    auto manifest = luban::service::archive::read(file);
    if (!manifest) { p.error = manifest.error(); return p; }
    const auto& m = *manifest;
    if (m.contains("edition") && m["edition"].is_object()) p.edition = m["edition"].value("ref", "");
    if (m.contains("packages") && m["packages"].is_object()) p.packages = m["packages"].size();
    if (m.contains("policy") && m["policy"].is_object()) {
        if (auto pol = xlings::subos::policy::from_json(m["policy"])) {
            p.preset = std::string(xlings::subos::policy::to_string(pol->preset));
            if (pol->preset == xlings::subos::policy::Preset::Legacy)
                p.wider.push_back(std::string(tr("luban.os.system.no_isolation_preset_environment_made")));
            // A grant as what it lets the system do.
            static constexpr std::array<std::array<std::string_view, 2>, 6> kGrants{{
                {"clipboard", "luban.os.system.grant.clipboard"},
                {"clipboard-paste", "luban.os.system.grant.clipboard_paste"},
                {"display", "luban.os.system.grant.display"},
                {"audio", "luban.os.system.grant.audio"},
                {"gpu", "luban.os.system.grant.gpu"},
                {"ssh-agent", "luban.os.system.grant.ssh_agent"},
            }};
            for (const auto& g : pol->grants) {
                const auto known = std::ranges::find_if(kGrants, [&](const auto& k) { return k[0] == g; });
                p.wider.push_back(known != kGrants.end() ? std::string(tr((*known)[1]))
                                                         : std::vformat(tr("luban.os.system.granted"), std::make_format_args(g)));
            }
            for (const auto& mnt : pol->mounts)
                p.wider.push_back(std::format("{} {} ({})", tr("luban.os.system.mounts_host_s"), mnt.src, mnt.rw ? "rw" : "ro"));
        } else {
            p.wider.push_back(std::vformat(tr("luban.os.system.policy_version_cannot_read"),
                                           std::make_format_args(pol.error())));
        }
    }
    // A name: the file's, made unique.
    std::string base = file.stem().string();
    std::ranges::replace_if(base, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_'; }, '-');
    if (base.empty()) base = "imported";
    std::string name = base;
    for (int k = 2; std::ranges::find(taken, name) != taken.end() && k < 100; ++k) name = std::format("{}-{}", base, k);
    std::snprintf(p.name, sizeof p.name, "%s", name.c_str());
    return p;
}

// Tests: sends `input` up to its next chord (or all of it) to the desktop as
// key presses; returns what is left.
std::string type_desktop_(Display& d, std::string_view input) {
    auto press = [&](std::uint32_t sym, std::uint32_t mods) {
        d.send(opensal::Key{true, sym, mods});
        d.send(opensal::Key{false, sym, mods});
    };
    static constexpr std::array<std::pair<std::string_view, std::uint32_t>, 6> names{{
        {"Return", 0xff0d}, {"Tab", 0xff09}, {"Escape", 0xff1b}, {"BackSpace", 0xff08}, {"Up", 0xff52}, {"Down", 0xff54}}};
    std::size_t k = 0;
    if (input.starts_with('{')) {
        const auto end = input.find('}');
        if (end == std::string_view::npos) return {};
        auto chord = input.substr(1, end - 1);
        std::uint32_t mods = 0;
        std::vector<std::uint32_t> held;
        for (auto [name, bit, sym] : {std::tuple{"Alt+", opensal::Alt, 0xffe9u}, {"Ctrl+", opensal::Ctrl, 0xffe3u},
                                      {"Shift+", opensal::Shift, 0xffe1u}, {"Super+", opensal::Super, 0xffebu}})
            if (chord.starts_with(name)) { mods |= bit; held.push_back(sym); chord.remove_prefix(std::string_view(name).size()); }
        for (auto sym : held) d.send(opensal::Key{true, sym, mods});
        std::uint32_t key = chord.size() == 1 ? static_cast<unsigned char>(chord[0]) : 0;
        for (const auto& [n, sym] : names) if (chord == n) key = sym;
        if (key) press(key, mods);
        for (auto sym : held) d.send(opensal::Key{false, sym, 0});
        return std::string(input.substr(end + 1));
    }
    for (; k < input.size() && input[k] != '{'; ++k) {
        if (input[k] == '\\' && k + 1 < input.size() && input[k + 1] == 'n') { press(0xff0d, 0); ++k; continue; }
        const auto c = static_cast<unsigned char>(input[k]);
        press(c, std::isupper(c) ? opensal::Shift : 0);
    }
    return std::string(input.substr(k));
}

// A desktop without a shell of its own (`X-Luban-Shell=none`; design
// 2026-10-18 §7, T1): the window's bar under it is its shell -- the
// applications its session lists, to start, and its windows, to bring
// forward. True while the pointer is the bar's (or its list's), not the desktop's.
bool shell_bar_(App& a, Display& d, ImVec2 at, float width, float height) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 wo = ImGui::GetWindowPos();
    dl->AddRectFilled(at, ImVec2(at.x + width, at.y + height), th_().bg_deep);
    dl->AddRectFilled(at, ImVec2(at.x + width, at.y + imkit::unit() / 3), th_().line);
    const float button_h = (height - px(8)) / imkit::scale();
    ImGui::SetCursorPos(ImVec2(at.x - wo.x + px(6), at.y - wo.y + px(4)));
    if (imkit::button("shell:apps", tr("luban.os.system.applications"), ImVec2(120, button_h), a.shell_apps ? imkit::Look::Primary : imkit::Look::Normal))
        a.shell_apps = !a.shell_apps;
    mark_(a, "shell:apps");
    // Its windows, the focused one raised; as many as fit.
    const auto windows = d.windows();
    float x = px(6) + px(128);
    const float each = std::clamp((width - x - px(6)) / std::max<std::size_t>(1, windows.size()), px(80), px(220));
    for (const auto& w : windows) {
        if (x + each > width) break;
        ImGui::SetCursorPos(ImVec2(at.x - wo.x + x, at.y - wo.y + px(4)));
        const std::string id = std::format("window:{}", w.id);
        const auto label = imkit::fit(w.title.empty() ? w.app_id : w.title, each - px(16), 13);
        if (imkit::button(id, label, ImVec2((each - px(4)) / imkit::scale(), button_h), w.focused ? imkit::Look::Primary : imkit::Look::Ghost))
            d.send(opensal::Activate{w.id});
        mark_(a, id);
        x += each;
    }
    bool over = ImGui::IsMouseHoveringRect(at, ImVec2(at.x + width, at.y + height));
    if (!a.shell_apps) return over;
    // The list above the bar, in the session's order (by name).
    const auto apps = d.applications();
    const float row = px(34), list_w = std::min(px(320), width - px(12));
    const float list_h = std::min(row * static_cast<float>(std::max<std::size_t>(1, apps.size())) + px(24), at.y - wo.y - px(12));
    const ImVec2 top(at.x + px(6), at.y - list_h - px(4));
    ImGui::SetCursorScreenPos(top);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, th_().panel_lo);
    ImGui::BeginChild("shell:list", ImVec2(list_w, list_h), ImGuiChildFlags_Borders);
    if (apps.empty()) ImGui::TextUnformatted(tr("luban.os.system.nothing_can_start_yet").data());
    for (const auto& app : apps) {
        const std::string id = "app:" + app.id;
        if (imkit::button(id, imkit::fit(app.name, list_w - px(40), 14), ImVec2((list_w - px(32)) / imkit::scale(), (row - px(6)) / imkit::scale()),
                          imkit::Look::Ghost)) {
            d.send(opensal::Launch{app.id});
            a.shell_apps = false;
        }
        mark_(a, id);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    over = over || ImGui::IsMouseHoveringRect(top, ImVec2(top.x + list_w, top.y + list_h));
    // A click anywhere else closes it.
    if (!over && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) a.shell_apps = false;
    return over;
}

// A machine's desktop: its surface drawn whole (1:1 when it fits), and the
// window's input given to it -- the pointer in the surface's pixels.
void desktop_view_(App& a) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    // No background of its own: the desktop's picture is under it (backdrop).
    begin_full_("desktop", true);
    ImGui::PopStyleVar();
    if (!a.display) { ImGui::End(); return; }
    auto& d = *a.display;
    // A mirror keeps the source's shape; the primary any (design 2026-10-20 §11.2).
    if (const auto r = d.take_role()) {
        if (r->kind == opensal::DisplayRole::Kind::mirror) a.window->keep_shape(static_cast<int>(r->size.width), static_cast<int>(r->size.height));
        else a.window->free_shape();
        a.window->set_title(a.title_for(a.current));
    }
    std::vector<Display::Update> updates;
    const auto size = d.take(updates);
    a.desktop_changed = !updates.empty();
    a.perf.desktop_frames += static_cast<int>(updates.size());
    if (size.width && size.height
        && (a.texture.width != static_cast<int>(size.width) || a.texture.height != static_cast<int>(size.height))) {
        if (a.texture.handle) a.window->destroy(a.texture);
        a.texture = a.window->texture(static_cast<int>(size.width), static_cast<int>(size.height));
        a.desktop_changed = true;
    }
    for (const auto& u : updates)
        if (a.texture.handle && u.rect.x + u.rect.width <= static_cast<std::uint32_t>(a.texture.width)
            && u.rect.y + u.rect.height <= static_cast<std::uint32_t>(a.texture.height))
            a.window->update(a.texture, u.rect, u.rows);
    // Pacing: what it sent is shown with this frame; it may draw the next --
    // without a GPU at most 30 times a second (the main loop says it when due).
    if (!updates.empty()) {
        const auto now = std::chrono::steady_clock::now();
        if (a.perf.renderer == "software" && now - a.presented_at < std::chrono::milliseconds(33))
            a.present_due = a.presented_at + std::chrono::milliseconds(33);
        else {
            d.send(opensal::FramePresented{});
            a.presented_at = now;
        }
    }
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float shell_h = d.shell() ? std::round(px(40)) : 0.0f;
    avail.y -= shell_h;
    bool on_shell = false;
    if (a.texture.handle) {
        // Its device pixels at the screen's pixel density (1:1), smaller when it does not fit.
        const float density = static_cast<float>(a.pixel_density());
        const float scale = std::min({1.0f / density, avail.x / a.texture.width, avail.y / a.texture.height});
        const ImVec2 shown(a.texture.width * scale, a.texture.height * scale);
        const ImVec2 at(std::round(origin.x + (avail.x - shown.x) / 2), std::round(origin.y + (avail.y - shown.y) / 2));
        ImGui::SetCursorScreenPos(at);
        ImGui::Dummy(shown);
        // Under the interface, copied as one rectangle (design 2026-10-20
        // §13.2): imgui's textured triangles were most of the window's work
        // on a frame, and on the software renderer most of its CPU.
        a.window->backdrop(a.texture, at.x, at.y, shown.x, shown.y);
        mark_(a, "desktop");
        // Over its picture the pointer is the desktop's: drawn by the host
        // as it says (design 2026-10-19 §15), or none where it draws its own
        // into the frames; elsewhere -- the bars around a picture smaller
        // than the window -- the interface's.
        if (ImGui::IsItemHovered()) {
            const auto p = d.pointer();
            a.window->surface_pointer(p.shape, p.image ? &*p.image : nullptr);
        }
        if (shell_h > 0) on_shell = shell_bar_(a, d, ImVec2(origin.x, origin.y + avail.y), avail.x, shell_h);
        // Its caret: the host's input method writes there (in window pixels).
        if (auto t = d.take_text_input()) {
            int on = 0, x = 0, y = 0, w = 1, h = 1;
            std::istringstream in(*t);
            in >> on >> x >> y >> w >> h;
            a.desktop_text = on == 1;
            if (a.desktop_text)
                a.window->text_input(true, static_cast<int>(at.x + x * scale), static_cast<int>(at.y + y * scale),
                                     std::max(1, static_cast<int>(w * scale)), std::max(1, static_cast<int>(h * scale)));
            else a.window->text_input(false);
        }
        // What it copied is the host's clipboard (it is not pasted back).
        if (auto c = d.take_clipboard()) {
            a.window->set_clipboard(*c);
            a.pasted = *c;
        }
        // The host's clipboard into it, when it is granted: as it opens,
        // whenever it changes, and whenever the window comes back to it.
        auto paste = [&] {
            if (!d.clipboard_grant().paste) return;
            auto text = a.window->clipboard();
            if (text.empty() || text == a.pasted) return;
            a.pasted = text;
            d.send(opensal::ClipboardText{std::move(text)});
        };
        if (std::exchange(a.paste_on_open, false)) paste();
        // Whether it can be seen, whatever is over it (it draws nothing while not).
        for (const auto& e : a.window->events())
            if (std::holds_alternative<opensal::Shown>(e)) d.send(e);
        if (!a.menu && !a.close_dialog) {
            for (const auto& e : a.window->events()) {
                if (const auto* m = std::get_if<opensal::PointerMotion>(&e)) {
                    d.send(opensal::PointerMotion{(m->x - at.x) / scale, (m->y - at.y) / scale});
                } else if (const auto* k = std::get_if<opensal::Key>(&e)) {
                    // While it takes text, a character is the input method's
                    // to give (as text), not a key's.
                    const bool chord = k->mods & (opensal::Ctrl | opensal::Alt | opensal::Super);
                    const bool character = (k->keysym >= 0x20 && k->keysym <= 0x7e) || (k->keysym >= 0xa0 && k->keysym <= 0xff)
                                           || k->keysym >= 0x01000000;
                    if (!(a.desktop_text && character && !chord)) d.send(e);
                } else if (std::holds_alternative<opensal::TextCommit>(e) || std::holds_alternative<opensal::Preedit>(e)) {
                    if (a.desktop_text) d.send(e);
                } else if (const auto* f = std::get_if<opensal::Focus>(&e)) {
                    d.send(e);
                    if (f->in) paste();
                } else if (std::holds_alternative<opensal::ClipboardText>(e)) {
                    paste();    // the host's clipboard changed
                } else if (std::holds_alternative<opensal::PointerButton>(e) || std::holds_alternative<opensal::Wheel>(e)) {
                    // The shell's bar and list are the window's; a button pressed on
                    // the desktop is let go there wherever the pointer is.
                    const auto* b = std::get_if<opensal::PointerButton>(&e);
                    const auto bit = b ? 1u << static_cast<std::uint32_t>(b->button) : 0u;
                    if (b && !b->down && (a.desktop_buttons & bit)) {
                        a.desktop_buttons &= ~bit;
                        d.send(e);
                    } else if (!on_shell) {
                        if (b && b->down) a.desktop_buttons |= bit;
                        if (!b || b->down) d.send(e);
                    }
                }
            }
        }
    } else {
        auto* dl = ImGui::GetWindowDrawList();
        const float cell = std::round(px(4));
        art::badge(dl, art::Badge::Craftsman, "", ImVec2(origin.x + (avail.x - 16 * cell) / 2, origin.y + avail.y * 0.32f), cell,
                   imkit::beat(4, 0.15));
        centred_(dl, avail.y * 0.32f + 16 * cell + px(20),
                 std::vformat(tr("luban.os.system.starting_desktop"), std::make_format_args(a.current)), th_().text_dim, 16);
    }
    // The window's size is the desktop's, once it has held still a moment.
    // The screen's scale, said once and whenever it changes (a screen with
    // another scale); the size it wants, in device pixels.
    if (const double density = a.density(); std::abs(density - a.sent_density) > 0.01) {
        d.send(opensal::DensityChanged{density});
        a.sent_density = density;
    }
    const opensal::Size want = a.device_size_(static_cast<int>(avail.x), static_cast<int>(avail.y));
    if (want != a.desktop_size) { a.desktop_size = want; a.resize_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(300); }
    if (a.resize_at && std::chrono::steady_clock::now() >= *a.resize_at) { d.send(opensal::Resized{want}); a.resize_at.reset(); }
    ImGui::End();
    if (!d.alive()) {
        // A program alone in the window: the window was its, and goes with it.
        if (!a.once_app.empty()) {
            if (const auto why = d.error(); !why.empty()) a.app_failed_(why);
            a.quit = true;
            return;
        }
        // A desktop that could not start (or ended) leaves the machine's console.
        const auto why = d.error();
        const bool machine = d.is_machine();
        a.close_display();
        if (machine && !vm::running(a.home, a.current)) {
            a.view = View::Off;
        } else if (machine) {
            a.notice = why;
            a.open_console(a.current, true);
        } else {
            // A sandbox's desktop that ended is the end of that session.
            a.notice = why;
            a.back_home();
        }
    }
}

}  // namespace luban::os
