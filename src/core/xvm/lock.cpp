module xlings.core.xvm.lock;

import std;
import xlings.platform;
import xlings.core.log;
import xlings.core.utils;

namespace xlings::xvm {

std::string lock_scope_key(const std::filesystem::path& home) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(home, ec);
    return (ec ? home : canonical).string();
}

std::filesystem::path state_lock_path(const std::filesystem::path& home) {
    return home / ".xlings.lock";
}

std::expected<StateLock, std::string> acquire_state_lock(const std::filesystem::path& home, std::chrono::milliseconds timeout) {
    StateLock lock;

    if (utils::get_env_or_default(std::string(no_lock_env())) == "1") {
        log::warn(
            "{}=1 — running without the state lock; a concurrent xlings can "
            "silently discard this command's changes",
            no_lock_env());
        lock.bypassed_ = true;
        lock.held_ = true;
        return lock;
    }

    const auto scope = lock_scope_key(home);
    const auto marker = utils::get_env_or_default(std::string(reentry_env()));
    if (!marker.empty() && marker == scope) {
        // An ancestor already holds this home. Waiting would deadlock: the
        // holder cannot finish until this child does.
        lock.inherited_ = true;
        lock.held_ = true;
        return lock;
    }

    std::error_code ec;
    std::filesystem::create_directories(home, ec);

    const auto path = state_lock_path(home);
    std::string error;
    if (!lock.lock_.acquire(path, timeout, {}, error)) {
        return std::unexpected(std::format(
            "another xlings process is changing this installation; waited "
            "{}s. Wait for it to finish, or set {}=1 to proceed anyway "
            "(which can discard one of the two commands' changes)",
            std::chrono::duration_cast<std::chrono::seconds>(timeout).count(),
            no_lock_env()));
    }

    // Children spawned from here on inherit this and skip locking -- but
    // only for this home.
    lock.previousMarker_ = marker;
    platform::set_env_variable(std::string(reentry_env()), scope);
    lock.ownsMarker_ = true;
    lock.held_ = true;
    return lock;
}

}
