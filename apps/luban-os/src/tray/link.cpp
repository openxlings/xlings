// A window's line to the tray process (core.cppm; design 2026-10-19 §3.3):
// one thread connects, reads what the tray says and wakes the window; the
// window writes. The tray started when nobody holds its lock; when it keeps
// ending (three times in a minute), not again -- said, with its log.
module luban.os;

import std;
import xlings.libs.json;
import xlings.platform;
import luban.service;
import :core;

namespace luban::os {

namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

}  // namespace

fs::path tray_dir(const fs::path& home) { return home / "run" / "luban-os"; }

struct TrayLink::State {
    fs::path home;
    std::function<void()> wake;
    std::mutex m;
    platform::LocalStream stream;
    bool connected { false };
    std::map<std::string, Json> kept;        // register, job: said again after a reconnect
    std::vector<Json> received;
    std::optional<Json> status;
    std::condition_variable got_status;
    std::string trouble;
    std::deque<std::chrono::steady_clock::time_point> started;   // the trays this window started

    bool write_(const Json& m) {
        if (!connected) return false;
        if (!stream.send_all(m.dump() + "\n", 500ms)) {
            connected = false;
            stream.close();
            return false;
        }
        return true;
    }
};

TrayLink::TrayLink(fs::path home, std::function<void()> wake) : s_(std::make_shared<State>()) {
    s_->home = std::move(home);
    s_->wake = std::move(wake);
    io_ = std::jthread([s = s_](std::stop_token stop) {
        const auto dir = tray_dir(s->home);
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string buffer;
        std::array<char, 4096> chunk{};
        while (!stop.stop_requested()) {
            bool connected = false;
            {
                std::lock_guard l(s->m);
                connected = s->connected;
            }
            if (!connected) {
                auto stream = platform::LocalStream::connect(dir / "tray.sock", 300ms);
                if (!stream) {
                    // Nobody holds the tray's lock: it is not there -- start it,
                    // unless it keeps ending.
                    platform::FileLock probe;
                    std::string why;
                    if (probe.try_acquire(dir / "tray.lock", why)) {
                        probe.release();
                        std::lock_guard l(s->m);
                        const auto now = std::chrono::steady_clock::now();
                        while (!s->started.empty() && now - s->started.front() > 1min) s->started.pop_front();
                        if (s->started.size() >= 3) {
                            if (s->trouble.empty()) {
                                s->trouble = std::format("the Luban OS tray keeps ending: see {}", (s->home / "logs" / "luban-os-tray.log").string());
                                if (s->wake) s->wake();
                            }
                        } else {
                            s->started.push_back(now);
                            (void)platform::spawn_detached({platform::get_executable_path().string(), "__tray"},
                                                           s->home / "logs" / "luban-os-tray.log");
                        }
                    }
                    std::this_thread::sleep_for(200ms);
                    continue;
                }
                std::lock_guard l(s->m);
                s->stream = std::move(*stream);
                s->connected = true;
                buffer.clear();
                // Who this is, and what it said of itself before.
                s->write_({{"hello", {{"role", "window"}, {"version", std::string(service::kVersion)}, {"pid", platform::current_pid()},
                                      {"exe", platform::get_executable_path().string()},
                                      {"provides", Json::array({std::format("tray-protocol {}", kTrayProtocol)})}}}});
                for (const auto& [k, v] : s->kept) s->write_({{k, v}});
                continue;
            }
            std::intptr_t handle = -1;
            {
                std::lock_guard l(s->m);
                handle = s->stream.native();
            }
            const std::array<std::intptr_t, 1> handles{handle};
            if (platform::wait_readable(handles, 200ms).empty()) continue;
            std::lock_guard l(s->m);
            auto n = s->stream.poll_receive(chunk, 0ms);
            if (!n || (*n && **n == 0)) {
                // The tray went: connect again (and start it again if it is gone).
                s->connected = false;
                s->stream.close();
                continue;
            }
            if (!*n) continue;
            buffer.append(chunk.data(), **n);
            bool woke = false;
            for (auto nl = buffer.find('\n'); nl != std::string::npos; nl = buffer.find('\n')) {
                auto j = Json::parse(buffer.substr(0, nl), nullptr, false);
                buffer.erase(0, nl + 1);
                if (!j.is_object()) continue;
                if (j.contains("tray")) {
                    s->status = j["tray"];
                    s->got_status.notify_all();
                    continue;
                }
                s->received.push_back(std::move(j));
                woke = true;
            }
            if (woke && s->wake) s->wake();
        }
    });
}

TrayLink::~TrayLink() {
    io_.request_stop();
    if (io_.joinable()) io_.join();
    std::lock_guard l(s_->m);
    s_->stream.close();
}

void TrayLink::send(const Json& message) {
    std::lock_guard l(s_->m);
    for (const auto* k : {"register", "job"})
        if (message.contains(k)) s_->kept[k] = message[k];
    s_->write_(message);
}

std::vector<Json> TrayLink::take() {
    std::lock_guard l(s_->m);
    return std::exchange(s_->received, {});
}

std::optional<Json> TrayLink::status(std::chrono::milliseconds wait) {
    std::unique_lock l(s_->m);
    if (!s_->status && wait.count() > 0) s_->got_status.wait_for(l, wait, [&] { return s_->status.has_value() || !s_->trouble.empty(); });
    return s_->status;
}

std::string TrayLink::trouble() const {
    std::lock_guard l(s_->m);
    return s_->trouble;
}

}  // namespace luban::os
