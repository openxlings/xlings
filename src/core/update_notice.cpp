module xlings.core.update_notice;

import std;

namespace xlings::update_notice {

namespace {
std::mutex& mu_() { static std::mutex m; return m; }
std::optional<std::string>& slot_() { static std::optional<std::string> v; return v; }
}  // namespace

void offer(std::string version) {
    std::scoped_lock lock(mu_());
    slot_() = std::move(version);
}

std::optional<std::string> pending() {
    std::scoped_lock lock(mu_());
    return slot_();
}

}  // namespace xlings::update_notice
