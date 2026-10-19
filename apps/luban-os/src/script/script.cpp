// What tests read and drive: the state as data, screenshots, and scripts that
// inject input between frames (--headless --script).
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

Json state_(const App& a) {
    static constexpr std::array<std::string_view, 9> names{"launcher", "quick-start", "job", "console", "off", "admin", "desktop", "import", "run"};
    Json j{{"view", std::string(names[static_cast<int>(a.view)])}, {"systems", Json::array()}};
    for (const auto& s : a.systems) j["systems"].push_back(s.name);
    j["recent"] = a.recent;
    if (a.countdown_until) j["countdown_to"] = a.countdown_target();
    j["selected"] = a.selected;
    j["filter"] = a.filter;
    j["menu"] = a.menu;
    j["close_dialog"] = a.close_dialog;
    j["close_choice"] = a.close_choice;
    j["confirm"] = a.confirm ? Json(a.confirm->title) : Json();
    j["notice"] = a.notice;
    j["current"] = a.current;
    j["title"] = a.window ? a.window->title() : std::string{};
    j["edition"] = std::string(kEditions[static_cast<std::size_t>(a.new_edition)].name);
    j["name"] = std::string(a.new_name);
    j["mode"] = a.new_mode == 1 ? "machine" : "sandbox";
    j["admin_tab"] = a.admin_tab;
    j["admin_page"] = a.admin_page;
    j["desktop_text"] = a.desktop_text;
    j["ended_why"] = a.ended_why;
    {
        auto ms = std::vector<double>(a.perf.frame_ms.begin(), a.perf.frame_ms.end());
        std::ranges::sort(ms);
        auto at = [&](double q) { return ms.empty() ? 0.0 : ms[std::min(ms.size() - 1, static_cast<std::size_t>(q * ms.size()))]; };
        j["perf"] = {{"frames", a.perf.frames}, {"presented", a.perf.presented}, {"renderer", a.perf.renderer},
                     {"frame_ms_p50", at(0.5)}, {"frame_ms_p95", at(0.95)},
                     {"frame_ms_max", ms.empty() ? 0.0 : ms.back()}, {"input_to_next_frame_ms", a.perf.input_to_next_frame_ms},
                     {"input_to_next_frame_ms_max", a.perf.input_to_next_frame_max}, {"wait_after_input_ms", a.perf.wait_after_input_ms},
                     {"idle_wait_ms", a.perf.idle_wait_ms}};
        auto pm = std::vector<double>(a.perf.present_ms.begin(), a.perf.present_ms.end());
        std::ranges::sort(pm);
        j["perf"]["present_ms_p50"] = pm.empty() ? 0.0 : pm[pm.size() / 2];
        // What the app itself spends (its layout and drawing, not the renderer's).
        std::vector<double> dm;
        for (std::size_t k = 0; k < a.perf.frame_ms.size(); ++k) dm.push_back(a.perf.frame_ms[k] - a.perf.present_ms[k]);
        std::ranges::sort(dm);
        j["perf"]["draw_ms_p95"] = dm.empty() ? 0.0 : dm[std::min(dm.size() - 1, dm.size() * 95 / 100)];
        j["perf"]["draw_ms_max"] = dm.empty() ? 0.0 : dm.back();
    }
    j["tray"] = {{"backend", a.tray_backend}, {"error", a.tray_error}, {"hidden", a.hidden}, {"said", a.tray_said},
                 {"pid", a.tray_pid}};
    j["scale"] = imkit::scale();
    j["unit"] = imkit::unit();
    j["display"] = Json::array({ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y});
    if (a.window) j["fonts"] = a.window->font_dir().string();
    if (a.view == View::QuickStart) {
        j["quick_start"] = {{"said", a.quick_said}, {"can_make", a.quick_can_make}, {"more", a.more}, {"offer", a.new_offer},
                            {"desktop", a.new_desktop}, {"card", chosen_card_(a)}, {"traditional", a.new_trad},
                            {"base", std::string(std::array<std::string_view, 3>{"tiny", "core", "desktop"}[static_cast<std::size_t>(std::clamp(a.new_base, 0, 2))])}};
        // The groups as shown, each its cards (design 2026-10-19 §5.1).
        Json groups = Json::array();
        for (const auto& g : quick_groups_(const_cast<App&>(a))) {
            Json cards = Json::array();
            for (const auto& c : g.cards) cards.push_back(c.id);
            groups.push_back({{"key", g.key}, {"cards", cards}});
        }
        j["quick_start"]["groups"] = groups;
        std::lock_guard l(const_cast<App&>(a).available_m);
        if (a.offers) {
            Json systems = Json::array(), desktops = Json::array();
            for (const auto& o : a.offers->systems) systems.push_back(o.ref);
            for (const auto& o : a.offers->desktops) desktops.push_back(o.ref);
            j["quick_start"]["offers"] = {{"systems", systems}, {"desktops", desktops}};
        }
    }
    if (a.import)
        j["import"] = {{"file", a.import->file.string()}, {"error", a.import->error}, {"edition", a.import->edition},
                       {"packages", a.import->packages}, {"wider", a.import->wider}, {"name", std::string(a.import->name)},
                       {"can_go", a.import->error.empty() && (a.import->wider.empty() || a.import->confirmed) && can_run_(const_cast<App&>(a))}};
    {
        const bool can = can_run_(const_cast<App&>(a));
        std::lock_guard l(const_cast<App&>(a).available_m);
        // And what the index said of every edition: the quick start reflows
        // when that arrives.
        bool editions = true;
        for (const auto& e : kEditions) {
            const auto it = a.available.find(std::string(e.name));
            editions = editions && it != a.available.end() && it->second.known;
        }
        j["host"] = {{"known", a.caps.has_value()}, {"can_run", can}, {"editions_known", editions}};
        if (a.caps) j["host"]["report"] = service::to_json(*a.caps);
    }
    {
        std::lock_guard l(const_cast<App&>(a).available_m);
        for (const auto& [name, v] : a.available)
            j["available"][name] = v.known ? (!v.listed ? "unlisted" : !v.here ? "not-here" : "yes") : "checking";
    }
    j["clipboard"] = a.window ? a.window->clipboard() : std::string{};
    if (a.window)
        j["window"] = {{"title", a.window->title()}, {"keyboard_grabbed", a.window->keyboard_grabbed()},
                       {"pointer_locked", a.window->pointer_locked()}};
    j["admin_selected"] = a.admin_selected < static_cast<int>(a.systems.size()) ? a.systems[static_cast<std::size_t>(a.admin_selected)].name : std::string{};
    if (a.console) {
        // What a console shows, as text: its lines, trailing blanks dropped.
        Json rows = Json::array();
        std::lock_guard l(a.console->lock());
        auto& screen = a.console->screen();
        for (int r = 0; r < screen.rows(); ++r) {
            std::string line;
            for (int c = 0; c < screen.cols(); ++c) {
                const auto& cell = screen.at(r, c);
                if (cell.continuation) continue;
                append_utf8_(line, cell.ch);
            }
            while (!line.empty() && line.back() == ' ') line.pop_back();
            rows.push_back(line);
        }
        while (!rows.empty() && rows.back().get<std::string>().empty()) rows.erase(rows.size() - 1);
        j["console"] = {{"rows", rows}, {"alive", a.console->alive()}, {"size", Json::array({screen.rows(), screen.cols()})},
                        {"cell", Json::array({a.grid.cell_w, a.grid.cell_h})}, {"font_size", a.console_size(a.current)},
                        {"scroll", a.console_scroll}};
    }
    if (a.view == View::Run) {
        Json lines = Json::array();
        std::lock_guard l(const_cast<App&>(a).run.m);
        for (const auto& line : a.run.lines) lines.push_back(line.text);
        j["run"] = {{"lines", lines}, {"busy", a.run.busy.load()}};
    }
    if (a.display) {
        j["desktop"] = {{"texture", Json::array({a.texture.width, a.texture.height})},
                        {"asked", Json::array({a.desktop_size.width, a.desktop_size.height})}};
        // The pointer: what the desktop says, and what the window shows now.
        const auto p = a.display->pointer();
        const auto [iw, ih] = a.window->pointer_image_size();
        j["pointer"] = {{"said", p.shape}, {"shape", a.window->pointer_shown()}, {"custom", p.image.has_value()},
                        {"size", Json::array({iw, ih})}, {"host_arrow", p.host_arrow}};
        if (a.display && a.display->shell()) {
            Json apps = Json::array(), windows = Json::array();
            for (const auto& app : a.display->applications()) apps.push_back(app.id);
            for (const auto& w : a.display->windows())
                windows.push_back({{"id", w.id}, {"app_id", w.app_id}, {"title", w.title}, {"focused", w.focused}});
            j["desktop"]["shell"] = {{"apps", apps}, {"windows", windows}, {"open", a.shell_apps}};
        }
    }
    if (a.job) {
        std::lock_guard l(a.job->m);
        Json phases = Json::array();
        for (const auto& ph : a.job->phases) phases.push_back(ph.text + ": " + ph.state);
        j["job"] = {{"title", a.job->title}, {"done", a.job->done.load()}, {"code", a.job->code}, {"current", a.job->current},
                    {"phases", phases}, {"errors", a.job->errors}, {"stage", a.job->stage}, {"percent", a.job->percent}};
    }
    j["anchors"] = Json::object();
    for (const auto& [k, r] : a.anchors) j["anchors"][k] = Json::array({r[0], r[1], r[2], r[3]});
    j["clipped"] = Json::array();
    for (const auto& k : a.clipped) j["clipped"].push_back(k);
    return j;
}

std::string dump_(const App& a) { return state_(a).dump(); }

// A screenshot: .bmp (24-bit, for people) or PPM (for tests to read).
bool write_screenshot_(const host::Window& w, const fs::path& file) {
    int width = 0, height = 0;
    const auto px = w.read_pixels(width, height);
    if (px.empty()) return false;
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    if (file.extension() == ".bmp") {
        const std::uint32_t row = (static_cast<std::uint32_t>(width) * 3 + 3) & ~3u, size = row * static_cast<std::uint32_t>(height);
        auto u16 = [&](std::uint16_t v) { out.put(static_cast<char>(v & 0xff)); out.put(static_cast<char>(v >> 8)); };
        auto u32 = [&](std::uint32_t v) { for (int k = 0; k < 4; ++k) out.put(static_cast<char>((v >> (8 * k)) & 0xff)); };
        out << "BM";
        u32(54 + size); u32(0); u32(54);
        u32(40); u32(static_cast<std::uint32_t>(width)); u32(static_cast<std::uint32_t>(height)); u16(1); u16(24);
        u32(0); u32(size); u32(2835); u32(2835); u32(0); u32(0);
        for (int y = height - 1; y >= 0; --y) {
            for (int x = 0; x < width; ++x) {
                const auto* p = px.data() + (std::size_t(y) * width + x) * 4;
                out.put(static_cast<char>(p[0])); out.put(static_cast<char>(p[1])); out.put(static_cast<char>(p[2]));
            }
            for (std::uint32_t k = static_cast<std::uint32_t>(width) * 3; k < row; ++k) out.put(0);
        }
    } else {
        out << std::format("P6\n{} {}\n255\n", width, height);
        for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            out << static_cast<char>(px[i + 2]) << static_cast<char>(px[i + 1]) << static_cast<char>(px[i]);
    }
    return static_cast<bool>(out);
}


std::optional<Script> read_script_(const fs::path& file) {
    std::ifstream in(file);
    if (!in) return std::nullopt;
    Script sc;
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        sc.lines.push_back(line);
    }
    return sc;
}

std::optional<Json> path_(const Json& root, std::string_view path) {
    const Json* node = &root;
    while (!path.empty()) {
        const auto dot = path.find('.');
        const auto part = path.substr(0, dot);
        if (node->is_object()) {
            auto it = node->find(std::string(part));
            if (it == node->end()) return std::nullopt;
            node = &*it;
        } else if (node->is_array()) {
            std::size_t k = 0;
            if (std::from_chars(part.data(), part.data() + part.size(), k).ec != std::errc{} || k >= node->size()) return std::nullopt;
            node = &(*node)[k];
        } else {
            return std::nullopt;
        }
        path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
    }
    return std::optional<Json>{std::in_place, *node};
}

bool matches_(const std::optional<Json>& node, std::string_view want) {
    if (want == "present") return node.has_value();
    if (!node) return want == "absent";
    return node->is_string() ? node->get<std::string>() == want : node->dump() == want;
}

void script_step_(App& a, Script& sc) {
    if (!sc.pending.empty()) {
        auto act = std::move(sc.pending.front());
        sc.pending.pop_front();
        act(a);
        return;
    }
    if (sc.at >= sc.lines.size()) return;
    const auto& line = sc.lines[sc.at];
    std::istringstream words(line);
    std::string cmd;
    words >> cmd;
    auto fail = [&](std::string why) {
        sc.failure = std::format("line {} (`{}`): {}", sc.at + 1, line, why);
        a.quit = true;
    };
    auto next = [&] { ++sc.at; sc.deadline.reset(); };
    auto waiting = [&](double seconds) {
        if (!sc.deadline) sc.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
        return std::chrono::steady_clock::now() < *sc.deadline;
    };
    if (cmd.empty() || cmd.starts_with('#')) { next(); return; }
    if (cmd == "wait" || cmd == "waitfor") {
        std::string path = "view", want;
        if (cmd == "waitfor") words >> path;
        words >> want;
        double seconds = 30;
        words >> seconds;
        const auto got = path_(state_(a), path);
        if (matches_(got, want)) next();
        else if (!waiting(seconds)) fail(std::format("{} is {}, not {}", path, got ? got->dump() : "absent", want));
    } else if (cmd == "see") {
        std::string text;
        std::getline(words >> std::ws, text);
        double seconds = 60;
        if (const auto sp = text.find(' '); sp != std::string::npos && sp > 1 && text[sp - 1] == 's'
            && std::ranges::all_of(text.substr(0, sp - 1), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            seconds = std::stod(text.substr(0, sp - 1));
            text = text.substr(sp + 1);
        }
        const auto st = state_(a);
        std::string shown;
        if (st.contains("console"))
            for (const auto& row : st["console"]["rows"])
                if (row.is_string()) shown += row.get<std::string>() + "\n";
        if (st.contains("job")) shown += st["job"].dump();
        shown += st.value("notice", std::string{});
        if (shown.find(text) != std::string::npos) next();
        else if (!waiting(seconds)) fail(std::format("`{}` is not shown; shown:\n{}", text, shown));
    } else if (cmd == "expect") {
        std::string path, want;
        words >> path;
        std::getline(words >> std::ws, want);
        const auto got = path_(state_(a), path);
        if (!matches_(got, want)) fail(std::format("{} is {}, not {}", path, got ? got->dump() : "absent", want));
        else next();
    } else if (cmd == "key") {
        std::string spec;
        words >> spec;
        std::vector<std::string> keys;
        for (std::size_t from = 0; from <= spec.size();) {
            const auto plus = spec.find('+', from);
            auto k = spec.substr(from, plus == std::string::npos ? std::string::npos : plus - from);
            if (!k.empty()) {
                k[0] = static_cast<char>(k.size() > 1 ? std::toupper(static_cast<unsigned char>(k[0])) : k[0]);
                keys.push_back(k);
            }
            if (plus == std::string::npos) break;
            from = plus + 1;
        }
        for (const auto& k : keys)
            if (!a.window->inject_key(k, true)) { fail("no key " + k); return; }
        sc.pending.push_back([keys](App& app) {
            for (auto it = keys.rbegin(); it != keys.rend(); ++it) app.window->inject_key(*it, false);
        });
        next();
    } else if (cmd == "keydown" || cmd == "keyup") {
        // A key held down, or let go: what is pressed meanwhile is pressed with it.
        std::string k;
        words >> k;
        if (k.size() > 1) k[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(k[0])));
        const bool down = cmd == "keydown";
        sc.pending.push_back([k, down](App& app) { app.window->inject_key(k, down); });
        next();
    } else if (cmd == "drop") {
        // A file dropped on the window, as a file manager drops it.
        std::string file;
        std::getline(words >> std::ws, file);
        sc.pending.push_back([file](App& app) { app.window->inject_drop(file); });
        next();
    } else if (cmd == "commit" || cmd == "clipboard") {
        // Text as the host's input method commits it; the host's clipboard.
        std::string text;
        std::getline(words >> std::ws, text);
        if (cmd == "commit") sc.pending.push_back([text](App& app) { app.window->inject_text(text); });
        else sc.pending.push_back([text](App& app) { app.window->set_clipboard(text); });
        next();
    } else if (cmd == "type") {
        std::string text;
        std::getline(words >> std::ws, text);
        for (char ch : text)
            sc.pending.push_back([ch](App& app) {
                const std::string one(1, ch);
                const bool upper = std::isupper(static_cast<unsigned char>(ch)) != 0;
                if (upper) app.window->inject_key("Shift", true);
                app.window->inject_key(ch == ' ' ? std::string("Space") : one, true);
                app.window->inject_text(one);
                app.window->inject_key(ch == ' ' ? std::string("Space") : one, false);
                if (upper) app.window->inject_key("Shift", false);
            });
        next();
    } else if (cmd == "click" || cmd == "move") {
        std::string first;
        words >> first;
        float x = 0, y = 0;
        if (first.empty()) { fail("where?"); return; }
        if (std::isdigit(static_cast<unsigned char>(first[0]))) {
            x = std::stof(first);
            words >> y;
        } else {
            // Clicked once it holds still -- laid out, not moving -- as a person would.
            auto it = a.anchors.find(first);
            auto before = a.anchors_before.find(first);
            if (it == a.anchors.end() || before == a.anchors_before.end() || before->second != it->second) {
                if (!waiting(10)) fail("no control " + first + " holding still on screen");
                return;
            }
            x = (it->second[0] + it->second[2]) / 2;
            y = (it->second[1] + it->second[3]) / 2;
        }
        // `click <x> <y> right`: the other button.
        std::string which;
        words >> which;
        const int button = which == "right" ? 3 : which == "middle" ? 2 : 1;
        sc.pending.push_back([x, y](App& app) { app.window->inject_pointer(x, y); });
        if (cmd == "click") {
            sc.pending.push_back([x, y, button](App& app) { app.window->inject_button(x, y, button, true); });
            sc.pending.push_back([x, y, button](App& app) { app.window->inject_button(x, y, button, false); });
        }
        next();
    } else if (cmd == "cell") {
        int row = 0, col = 0;
        std::string how = "left";
        words >> row >> col >> how;
        if (!a.console || a.grid.cell_w <= 0) { if (!waiting(10)) fail("no console"); return; }
        const float x = a.grid.x + (static_cast<float>(col) + 0.5f) * a.grid.cell_w;
        const float y = a.grid.y + (static_cast<float>(row) + 0.5f) * a.grid.cell_h;
        const int button = how == "middle" ? 2 : how == "right" ? 3 : 1;
        const int times = how == "double" ? 2 : how == "triple" ? 3 : 1;
        // The move goes with the first press: clicks on one cell from two
        // commands are as close as clicks from one (a frame apart each).
        for (int k = 0; k < times; ++k) {
            sc.pending.push_back([x, y, button, k](App& app) {
                if (k == 0) app.window->inject_pointer(x, y);
                app.window->inject_button(x, y, button, true);
            });
            sc.pending.push_back([x, y, button](App& app) { app.window->inject_button(x, y, button, false); });
        }
        next();
    } else if (cmd == "frames") {
        int n = 1;
        words >> n;
        for (int k = 0; k < n; ++k) sc.pending.push_back([](App&) {});
        next();
    } else if (cmd == "sleep") {
        double ms = 0;
        words >> ms;
        if (!waiting(ms / 1000.0)) next();
    } else if (cmd == "resize") {
        int w = 0, h = 0;
        words >> w >> h;
        a.window->set_size(w, h);
        next();
    } else if (cmd == "perf") {
        // `perf reset`: what follows is measured alone.
        a.perf = {.renderer = a.perf.renderer};
        next();
    } else if (cmd == "tray") {
        // As if chosen at the status-area icon.
        std::string what;
        words >> what;
        a.tray_injected.push_back(what);
        next();
    } else if (cmd == "close") {
        a.window->inject_close();
        next();
    } else if (cmd == "quit") {
        a.quit = true;
        next();
    } else if (cmd == "screenshot") {
        std::string file;
        words >> file;
        if (!write_screenshot_(*a.window, file)) fail("cannot write " + file);
        else next();
    } else if (cmd == "state") {
        std::string file;
        words >> file;
        std::ofstream(file) << state_(a).dump(2) << "\n";
        next();
    } else {
        fail("not a script command");
    }
}

}  // namespace luban::os
