// The programs the desktop started (server.cppm; design 2026-10-24 §6.1): a
// thread of its own reads what each says -- never the loop -- within a budget
// (1 MiB a second each; past it, dropped and counted, the program never
// held back), into its log (8 MiB at most) and its last mebibyte, and to
// whoever follows its log on the bus. The loop reaps them: an end is an
// event (app-exit: its code or signal, how long it ran, its log, its last
// lines), and an unexpected one -- a signal, or failing within ten seconds --
// is said on the desktop.
module xdesk.compositor;

import std;
import xdesk.os;
import xdesk.words;
import openbus;
import xbus.node;
import :server;

namespace xdesk::compositor {

namespace os = xdesk::os;

namespace {

constexpr std::size_t kTail = 1u << 20;
constexpr double kBudget = 1u << 20;           // bytes a second
constexpr std::uint64_t kLogLimit = 8u << 20;

std::string last_lines_(std::string_view text, int n) {
    std::size_t at = text.size();
    while (n-- > 0 && at > 0) {
        const auto nl = text.rfind('\n', at > 1 ? at - 2 : 0);
        if (nl == std::string_view::npos) { at = 0; break; }
        at = nl + 1;
    }
    return std::string(text.substr(at));
}

}  // namespace

void Server::apps_read(std::stop_token stop) {
    std::array<char, 65536> buf{};
    while (!stop.stop_requested()) {
        std::vector<os::PollFd> fds;
        {
            std::lock_guard l(apps_m);
            for (const auto& a : app_runs)
                if (a.out >= 0) fds.push_back({.fd = a.out});
        }
        if (fds.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        os::poll(fds, 200);
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard l(apps_m);
        for (const auto& p : fds) {
            if (!p.readable && !p.closed) continue;
            auto it = std::ranges::find(app_runs, p.fd, &AppRun::out);
            if (it == app_runs.end()) continue;
            auto& a = *it;
            const auto n = os::read_some(a.out, buf);
            if (!n || *n == 0) {
                os::close(a.out);
                a.out = -1;
                if (a.dropped) {
                    const auto note = std::format("\n[xdesk: {} bytes it said past 1 MiB a second were dropped]\n", a.dropped);
                    if (a.log_fd >= 0) (void)os::write_all(a.log_fd, note);
                }
                for (auto& f : a.followers) f.close();
                a.followers.clear();
                continue;
            }
            // The budget, refilled with the time that passed.
            a.budget = std::min(kBudget, a.budget + kBudget * std::chrono::duration<double>(now - a.refilled).count());
            a.refilled = now;
            std::string_view got(buf.data(), *n);
            if (got.size() > a.budget) {
                a.dropped += got.size() - static_cast<std::size_t>(a.budget);
                got = got.substr(0, static_cast<std::size_t>(a.budget));
            }
            a.budget -= static_cast<double>(got.size());
            if (got.empty()) continue;
            if (a.log_fd >= 0 && a.logged < kLogLimit) {
                const auto room = std::min<std::uint64_t>(got.size(), kLogLimit - a.logged);
                (void)os::write_all(a.log_fd, got.substr(0, room));
                a.logged += room;
                if (a.logged >= kLogLimit) (void)os::write_all(a.log_fd, "\n[xdesk: its log stops at 8 MiB]\n");
            }
            a.tail.append(got);
            if (a.tail.size() > kTail) a.tail.erase(0, a.tail.size() - kTail);
            std::erase_if(a.followers, [&](xbus::Stream& f) { return !f.write(got, std::chrono::milliseconds(200)); });
        }
    }
}

void Server::apps_reap() {
    std::erase_if(children, [this](int pid) {
        const auto code = os::exited(pid);
        if (!code) return false;
        std::optional<AppRun> ended;
        {
            std::lock_guard l(apps_m);
            if (auto it = std::ranges::find(app_runs, pid, &AppRun::pid); it != app_runs.end()) {
                it->ended = true;
                it->code = *code;
                ended = AppRun{.pid = it->pid, .id = it->id, .name = it->name, .log = it->log, .started = it->started, .tail = it->tail};
                if (it->log_fd >= 0) { os::close(it->log_fd); it->log_fd = -1; }
            }
            // The last 32 that ended are kept for who asks (their logs stay on disk).
            std::size_t ended_count = 0;
            for (const auto& a : app_runs) ended_count += a.ended && a.out < 0 ? 1 : 0;
            for (auto it = app_runs.begin(); ended_count > 32 && it != app_runs.end();) {
                if (it->ended && it->out < 0 && it->followers.empty()) { it = app_runs.erase(it); --ended_count; }
                else ++it;
            }
        }
        if (!ended) return true;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ended->started).count();
        const bool signal = *code > 128;
        publish("app-exit", {{"id", ended->id}, {"pid", std::to_string(pid)}, {"code", std::to_string(*code)},
                             {"signal", signal ? std::to_string(*code - 128) : std::string("0")}, {"ms", std::to_string(ms)},
                             {"log", ended->log.string()}, {"last", last_lines_(ended->tail, 20)}});
        if (bus) bus->withdraw(std::format("service/xdesk/apps/{}/log", pid));
        // Unexpected: a signal, or failing soon -- said, with where its log is.
        if (signal || (*code != 0 && ms < 10000)) {
            const auto how = signal ? std::format("{} {}", i18n::tr("signal"), *code - 128) : std::format("{} {}", i18n::tr("exit"), *code);
            show_hint(std::format("{} {} ({}) -- {}", ended->name, i18n::tr("ended unexpectedly"), how, ended->log.string()), 8000);
            log(std::format("{} ended unexpectedly ({}), its log: {}", ended->name, how, ended->log.string()));
        }
        return true;
    });
}

std::string Server::apps_json() {
    std::lock_guard l(apps_m);
    std::string out = "[";
    for (const auto& a : app_runs) {
        auto q = [](std::string_view s) {
            std::string o = "\"";
            for (char c : s) {
                if (c == '"' || c == '\\') o += '\\';
                if (static_cast<unsigned char>(c) < 0x20) { o += std::format("\\u{:04x}", static_cast<unsigned char>(c)); continue; }
                o += c;
            }
            return o + "\"";
        };
        out += std::format("{}{{\"pid\":{},\"id\":{},\"name\":{},\"log\":{},\"running\":{},\"dropped\":{},\"code\":{}}}",
                           out.size() > 1 ? "," : "", a.pid, q(a.id), q(a.name), q(a.log.string()), a.ended ? "false" : "true", a.dropped,
                           a.code ? std::to_string(*a.code) : std::string("null"));
    }
    return out + "]";
}

}  // namespace xdesk::compositor
