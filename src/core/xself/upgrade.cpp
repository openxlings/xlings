module xlings.core.xself.upgrade;

import std;
import xlings.libs.json;
import xlings.core.config;
import xlings.core.semver;
import xlings.core.home_health;
import xlings.core.xvm.db;
import xlings.core.xvm.lock;
import xlings.core.xself.init;
import xlings.core.xself.doctor;
import xlings.observe;
import xlings.platform;

namespace xlings::xself::upgrade {

namespace fs = std::filesystem;

namespace {

std::string now_utc() {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

bool installed_(std::string_view version) {
    const auto db = Config::versions();
    const auto* info = xvm::get_vinfo(db, "xlings");
    if (!info) return false;
    for (const auto& [key, _] : info->versions)
        if (xvm::strip_namespace(key) == version) return true;
    return false;
}

}  // namespace

std::optional<Outcome> on_first_command() {
    try {
        if (const char* mode = std::getenv("XLINGS_SUBOS_MODE"); mode && *mode) return std::nullopt;
        // A command an xlings runs for itself (a hook's, `self update`'s
        // steps): the outermost one upgrades the home, after it is done,
        // never in the middle of its transaction.
        if (const char* held = std::getenv("XLINGS_STATE_LOCK_HELD"); held && *held) return std::nullopt;
        const std::string now(Info::VERSION);
        const auto record = Config::recorded_upgrade();
        std::string last = record.value("to", std::string{});
        if (last.empty()) last = Config::recorded_verified_version();
        if (last.empty()) last = Config::recorded_client_version();
        // `self install` records "v<version>".
        if (last.size() > 1 && last[0] == 'v' && std::isdigit(static_cast<unsigned char>(last[1])))
            last.erase(0, 1);
        if (last == now) return std::nullopt;
        const auto home = Config::paths().homeDir;
        if (last.empty()) {
            // Nothing to upgrade from (a home `self init` just made): this
            // version is where the record starts.
            (void)Config::record_upgrade({{"to", now}, {"at", now_utc()}});
            return std::nullopt;
        }
        Outcome out{.from = last, .to = now};
        if (semver::compare(last, now) > 0) {
            out.downgrade = true;
            out.canGoBack = installed_(last);
            return out;
        }

        observe::Timed timed(std::format("upgrade {} -> {}", last, now),
                             std::format("checking this home after the upgrade to {}", now));
        const auto pend = [&](std::string what, std::string command) {
            if (std::ranges::none_of(out.pending, [&](const Pending& p) { return p.what == what; }))
                out.pending.push_back({std::move(what), std::move(command)});
        };

        // 1. The home config: everything else reads it.
        std::vector<home_health::Problem> problems;
        try {
            problems = home_health::problems(home);
        } catch (...) {
            problems.push_back({"", "could not be read", "settings fall back to defaults"});
        }
        if (!problems.empty()) pend(home_health::summary(home, problems), "xlings self doctor --fix");

        // 2. Derived state, rebuilt from the records (routes, a root's
        //    generation): idempotent, so redone rather than migrated. Under
        //    the home's lock; another xlings holding it means the next
        //    command does this, so nothing is recorded.
        if (problems.empty()) {
            auto lock = xvm::acquire_state_lock(home, std::chrono::seconds(2));
            if (!lock) return std::nullopt;
            try {
                Config::reload_state();
                if (const auto synced = sync_shim_tables(); !synced.root_error.empty())
                    pend(synced.root_error, "xlings self doctor --fix");
            } catch (const std::exception& e) {
                pend(std::format("rebuilding the command routes failed: {}", e.what()),
                     "xlings self doctor --fix");
            }
        }

        // 3. What it cannot do by itself: the doctor's errors, each with its
        //    command. Read-only.
        if (problems.empty()) {
            try {
                for (auto& step : upgrade_check()) pend(std::move(step.what), std::move(step.command));
            } catch (const std::exception& e) {
                pend(std::format("checking the home failed: {}", e.what()), "xlings self doctor");
            }
        }

        out.canGoBack = installed_(last);
        nlohmann::json pending = nlohmann::json::array();
        for (const auto& p : out.pending) pending.push_back({{"what", p.what}, {"command", p.command}});
        const nlohmann::json entry{{"from", last}, {"to", now}, {"at", now_utc()}, {"pending", pending}};
        (void)Config::record_upgrade(entry);
        try {
            fs::create_directories(home / "logs");
            std::ofstream(home / "logs" / "upgrade.ndjson", std::ios::app) << entry.dump() << '\n';
        } catch (...) {
        }
        return out;
    } catch (...) {
        // An upgrade step must never be why a command did not run.
        return std::nullopt;
    }
}

}  // namespace xlings::xself::upgrade
