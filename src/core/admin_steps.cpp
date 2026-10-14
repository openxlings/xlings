module xlings.core.admin_steps;

import std;
import xlings.libs.json;
import xlings.runtime;
import xlings.platform;
import xlings.core.log;
import xlings.core.confirm;
import xlings.subos.elevation;
import xlings.subos.home_view;

namespace xlings::admin_steps {

namespace fs = std::filesystem;

namespace {
fs::path file_(const fs::path& home) { return home / "state" / "admin-steps.json"; }
}  // namespace

std::optional<Step> parse(std::string_view target) {
    std::vector<std::string> words;
    for (std::string_view rest = target;;) {
        const auto at = rest.find('\x1f');
        words.emplace_back(rest.substr(0, at));
        if (at == std::string_view::npos) break;
        rest.remove_prefix(at + 1);
    }
    if (words.size() < 2 || words[1].empty()) return std::nullopt;
    return Step{words.front(), std::vector<std::string>(words.begin() + 1, words.end())};
}

std::string command(const Step& s) {
    std::string out;
    for (const auto& w : s.argv) out += (out.empty() ? "" : " ") + platform::shell_quote(w);
    return out;
}

std::vector<Step> pending(const fs::path& home) {
    std::vector<Step> out;
    std::ifstream in(file_(home));
    if (!in) return out;
    const auto doc = nlohmann::json::parse(in, nullptr, false);
    if (!doc.is_array()) return out;
    for (const auto& j : doc) {
        if (!j.is_object() || !j.contains("argv") || !j["argv"].is_array()) continue;
        Step s{j.value("why", std::string{}), {}};
        for (const auto& w : j["argv"]) if (w.is_string()) s.argv.push_back(w.get<std::string>());
        if (!s.argv.empty()) out.push_back(std::move(s));
    }
    return out;
}

void record(const fs::path& home, const std::vector<Step>& steps) {
    std::error_code ec;
    if (steps.empty()) { fs::remove(file_(home), ec); return; }
    fs::create_directories(file_(home).parent_path(), ec);
    nlohmann::json doc = nlohmann::json::array();
    for (const auto& s : steps) doc.push_back({{"why", s.why}, {"argv", s.argv}});
    platform::write_file_atomic(file_(home).string(), doc.dump(2) + "\n");
}

std::size_t settle(const fs::path& home, std::vector<Step> steps, bool yes, EventStream& stream) {
    auto waiting = pending(home);
    for (auto& s : steps)
        if (std::ranges::find(waiting, s) == waiting.end()) waiting.push_back(std::move(s));
    if (waiting.empty()) return 0;
    if (!subos::elevation::can_ask()) {
        record(home, waiting);
        log::println("{} step(s) need administrator rights; nothing was asked here:", waiting.size());
        for (const auto& s : waiting) log::println("  {}: {}", s.why, command(s));
        log::println("  at a terminal: xlings self doctor --fix (or run them as root)");
        return waiting.size();
    }
    log::println("{} step(s) need administrator rights:", waiting.size());
    for (const auto& s : waiting) log::println("  {}: {}", s.why, command(s));
    auto asked = confirm::ask(stream, "admin_steps", "run them now (sudo)?", yes, "-y");
    if (asked.outcome != confirm::Outcome::Confirmed) {
        record(home, waiting);
        log::println("  kept for later: xlings self doctor --fix");
        return waiting.size();
    }
    std::vector<Step> failed;
    for (const auto& s : waiting) {
        const int rc = subos::elevation::run(subos::HomeView{home}, s.argv, s.why);
        if (rc != 0) {
            log::warn("{}: `{}` exited {}", s.why, command(s), rc);
            failed.push_back(s);
        }
    }
    record(home, failed);
    return failed.size();
}

}  // namespace xlings::admin_steps
