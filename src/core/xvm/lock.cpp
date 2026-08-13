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

std::filesystem::path state_lock_owner_path(const std::filesystem::path& home) {
    return home / ".xlings.lock.owner";
}

namespace {

std::string gCommandHint_;

// One line, written plainly with an ordinary stream.
//
// NOT platform::write_string_to_file: that replaces by rename, and this file
// lives beside an flock'd one whose whole hazard is inode replacement. Using
// the rename writer here would be safe today (different path) and would become
// the bug the moment somebody pointed it at the lock itself. Writing it the
// obvious way keeps the two files obviously different.
void write_owner_(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return;
    out << platform::get_pid() << '\n' << gCommandHint_ << '\n';
}

}  // namespace

void set_lock_command_hint(std::string command) {
    // Bounded: this ends up inside a one-line message, and a command line can
    // be arbitrarily long (a plan with fifty targets, a hook's full argv).
    constexpr std::size_t kMax = 120;
    if (command.size() > kMax) {
        command.resize(kMax);
        command += "…";
    }
    gCommandHint_ = std::move(command);
}

std::string lock_holder_hint(const std::filesystem::path& home) {
    std::ifstream in(state_lock_owner_path(home));
    if (!in) return {};
    std::string pidText;
    std::string command;
    if (!std::getline(in, pidText)) return {};
    std::getline(in, command);
    if (pidText.empty()) return {};

    int pid = 0;
    const auto* first = pidText.data();
    const auto* last = first + pidText.size();
    if (std::from_chars(first, last, pid).ec != std::errc{}) return {};

    // Whether the holder is still running is the most useful half of this.
    // A sidecar left behind by a killed process would otherwise name a pid
    // that means nothing, and the reader would have no way to tell.
    if (!platform::is_process_alive(pid)) {
        return std::format("pid {} (no longer running)", pid);
    }
    if (command.empty()) return std::format("pid {}", pid);
    return std::format("pid {}, `{}`", pid, command);
}

std::expected<StateLock, std::string> acquire_state_lock(const std::filesystem::path& home, std::chrono::milliseconds timeout) {
    StateLock lock;

    // The env override wins over the caller's argument on purpose: the callers
    // pass the default, and a machine that does long installs needs one place
    // to say so rather than twenty.
    if (const auto configured =
            utils::get_env_or_default(std::string(lock_timeout_env()));
        !configured.empty()) {
        long long seconds = 0;
        const auto* first = configured.data();
        const auto* last = first + configured.size();
        if (auto [ptr, ec] = std::from_chars(first, last, seconds);
            ec == std::errc{} && ptr == last && seconds >= 0) {
            timeout = std::chrono::seconds{seconds};
        }
    }

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

    // The wait is announced, and the holder is named when it can be.
    //
    // The identity is read ONCE, on the first tick: re-reading it every second
    // would turn a queued command into a file-poller, and the answer cannot
    // usefully change while we are still waiting for the same holder.
    std::string holder;
    bool holderRead = false;
    const auto onWait = [&](std::chrono::milliseconds elapsed) {
        if (!holderRead) {
            holderRead = true;
            holder = lock_holder_hint(home);
        }
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        if (seconds <= 0) {
            log::info("waiting for another xlings to finish with this home{}",
                      holder.empty() ? std::string{}
                                     : std::format(" ({})", holder));
        } else {
            log::info("still waiting for another xlings{} — {}s",
                      holder.empty() ? std::string{}
                                     : std::format(" ({})", holder),
                      seconds);
        }
    };

    const auto path = state_lock_path(home);
    std::string error;
    if (!lock.lock_.acquire(path, timeout, {}, error, onWait)) {
        return std::unexpected(std::format(
            "another xlings process is changing this installation{}; waited "
            "{}s. Wait for it to finish, raise {} (seconds, 0 = wait "
            "indefinitely), or set {}=1 to proceed anyway (which can discard "
            "one of the two commands' changes)",
            holder.empty() ? std::string{} : std::format(" ({})", holder),
            std::chrono::duration_cast<std::chrono::seconds>(timeout).count(),
            lock_timeout_env(), no_lock_env()));
    }

    // Who we are, for the next process that has to wait on us. Written after
    // the lock is held, so the file only ever describes an actual holder.
    lock.ownerFile_ = state_lock_owner_path(home);
    write_owner_(lock.ownerFile_);

    // Children spawned from here on inherit this and skip locking -- but
    // only for this home.
    lock.previousMarker_ = marker;
    platform::set_env_variable(std::string(reentry_env()), scope);
    lock.ownsMarker_ = true;
    lock.held_ = true;
    return lock;
}

}


// ── out-of-line class members ──────────────────────────────────

namespace xlings::xvm {

StateLock::StateLock(StateLock&& other) noexcept : lock_(std::move(other.lock_)),
      held_(std::exchange(other.held_, false)),
      bypassed_(std::exchange(other.bypassed_, false)),
      inherited_(std::exchange(other.inherited_, false)),
      ownsMarker_(std::exchange(other.ownsMarker_, false)),
      previousMarker_(std::move(other.previousMarker_)),
      // Moved, not copied: two StateLocks naming one sidecar would have the
      // first destructor delete the live holder's own record.
      ownerFile_(std::exchange(other.ownerFile_, {})) {}

StateLock& StateLock::operator=(StateLock&& other) noexcept {
    if (this != &other) {
        clear_marker_();
        clear_owner_();
        lock_ = std::move(other.lock_);
        held_ = std::exchange(other.held_, false);
        bypassed_ = std::exchange(other.bypassed_, false);
        inherited_ = std::exchange(other.inherited_, false);
        ownsMarker_ = std::exchange(other.ownsMarker_, false);
        previousMarker_ = std::move(other.previousMarker_);
        ownerFile_ = std::exchange(other.ownerFile_, {});
    }
    return *this;
}

StateLock::~StateLock() { clear_marker_(); clear_owner_(); }

void StateLock::clear_owner_() {
    if (ownerFile_.empty()) return;
    std::error_code ec;
    std::filesystem::remove(ownerFile_, ec);   // best effort, see the header
    ownerFile_.clear();
}

[[nodiscard]] bool StateLock::held() const { return held_; }

[[nodiscard]] bool StateLock::bypassed() const { return bypassed_; }

[[nodiscard]] bool StateLock::inherited() const { return inherited_; }

void StateLock::clear_marker_() {
    if (!ownsMarker_) return;
    // Restore whatever was there rather than clearing outright: locking
    // a second home while holding the first must not erase the first
    // one's marker, or a later child would try to re-lock it and hang.
    platform::set_env_variable(std::string(reentry_env()), previousMarker_);
    ownsMarker_ = false;
}

} // namespace xlings::xvm
