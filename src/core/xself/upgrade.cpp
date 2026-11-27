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
import xlings.home.format;
import xlings.core.home;
import xlings.core.home_identity;
import xlings.subos.manifest;

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

// A subos whose declared runtime is a version the store does not hold while
// its view serves another (design 2026-11-27 §7.2): what `subos runtime` with
// a version line used to leave (#607 item 1). Its directory, and the binding
// the view serves. A declared version that IS installed is left alone: that
// disagreement is a decision (doctor's runtime drift), not an older format.
std::vector<std::pair<fs::path, std::string>> runtime_declarations_to_bring_(const fs::path& home) {
    namespace mf = xlings::subos::manifest;
    std::vector<std::pair<fs::path, std::string>> out;
    std::error_code ec;
    if (!fs::is_directory(home / "subos", ec)) return out;
    const auto db = Config::versions();
    const auto installed = [&](std::string_view binding) {
        const auto* vi = xvm::get_vinfo(db, std::string(mf::binding_name(binding)));
        if (!vi) return false;
        for (const auto& [version, data] : vi->versions) {
            if (!mf::version_is_active(mf::binding_version(binding), version) || data.path.empty()) continue;
            std::error_code e;
            if (fs::exists(xvm::expand_path(data.path, home.string()), e)) return true;
        }
        return false;
    };
    for (const auto& entry : fs::directory_iterator(home / "subos", ec)) {
        if (!entry.is_directory(ec)) continue;
        const auto doc = mf::read_document(entry.path());
        if (!doc) continue;
        const auto declared = mf::parse(*doc).runtime;
        if (!mf::is_binding(declared) || mf::runtime_is_hosted(declared)) continue;
        const auto served = mf::sysroot_runtime(entry.path());
        if (served.empty() || served == declared || mf::binding_name(served) != mf::binding_name(declared))
            continue;
        if (installed(declared) || !installed(served)) continue;
        out.emplace_back(entry.path(), served);
    }
    return out;
}

}  // namespace

std::span<const home::format::Step> format_steps() {
    static const std::array<home::format::Step, 3> value{{
        // The directories and the marker every home of this layout has
        // (ensure_home_layout, idempotent): a home an older client made lacks
        // the newer ones.
        {"home-layout",
         [](const fs::path& h) {
             if (!home_identity::has_marker(h)) return true;
             if (home::describe(h, home::Source::Default).layout < home::format::kLayout) return true;
             std::error_code ec;
             for (const auto& d : {"bin", "config/shell", "data/xpkgs", "subos/default/bin", "subos/default/generations"})
                 if (!fs::is_directory(h / d, ec)) return true;
             return false;
         },
         [](const fs::path& h) -> std::expected<std::string, std::string> {
             if (!ensure_home_layout(h)) return std::unexpected("the home's directories could not be made");
             return std::string("the home's directories and marker are this layout's");
         }},
        // Stage-0 is luban-init (it was xlings itself under the name
        // xlings-init): a machine booting `init=<home>/boot/xlings-init` boots
        // the luban-init beside it from now on. xlings still hands the old
        // name over (main.cpp), so this is the fast path, not the only one.
        {"boot-stage0",
         [](const fs::path& h) {
             std::error_code ec;
             const auto link = h / "boot" / "xlings-init";
             if (!fs::is_symlink(link, ec) || !fs::exists(h / "bin" / "luban-init", ec)) return false;
             return fs::read_symlink(link, ec).filename() != "luban-init";
         },
         [](const fs::path& h) -> std::expected<std::string, std::string> {
             std::error_code ec;
             const auto link = h / "boot" / "xlings-init";
             const auto staged = h / "boot" / ".xlings-init.new";
             fs::remove(staged, ec);
             fs::create_symlink("../bin/luban-init", staged, ec);
             if (!ec) fs::rename(staged, link, ec);
             if (ec) return std::unexpected("boot/xlings-init could not be pointed at luban-init: " + ec.message());
             return std::string("boot/xlings-init is luban-init");
         }},
        // A declaration names the version installed and active (§7.2); one
        // that names a version the store lacks while the view serves another
        // becomes what is served, the old text kept as the request. Not a
        // layout change: an older client reads it and does not undo it.
        {"runtime-declaration",
         [](const fs::path& h) { return !runtime_declarations_to_bring_(h).empty(); },
         [](const fs::path& h) -> std::expected<std::string, std::string> {
             namespace mf = xlings::subos::manifest;
             std::string said;
             for (const auto& [dir, served] : runtime_declarations_to_bring_(h)) {
                 auto doc = mf::read_document(dir);
                 if (!doc) return std::unexpected(dir.string() + ": the manifest could not be read");
                 auto& block = (*doc)[std::string(mf::BLOCK)];
                 const auto was = block.value("runtime", std::string{});
                 block["runtime"] = served;
                 if (!block.contains("runtime_request")) block["runtime_request"] = was;
                 try {
                     platform::write_file_atomic(mf::config_path(dir).string(), doc->dump(2));
                 } catch (const std::exception& e) {
                     return std::unexpected(mf::config_path(dir).string() + ": " + e.what());
                 }
                 said += std::format("{}subos '{}' declares {} (was {}, not installed)", said.empty() ? "" : "; ",
                                     dir.filename().string(), served, was);
             }
             return said;
         }},
    }};
    return value;
}

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
        const bool firstRecord = last.empty();
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
        Outcome out{.from = last, .to = now, .firstRecord = firstRecord};
        if (semver::compare(last, now) > 0) {
            out.downgrade = true;
            out.canGoBack = installed_(last);
            return out;
        }

        observe::Timed timed(std::format("upgrade {} -> {}", last, now),
                             std::format("checking this home after the upgrade to {}", now));
        const auto pend = [&](std::string what, std::string command, std::string group = {}) {
            if (std::ranges::none_of(out.pending, [&](const Pending& p) { return p.what == what; }))
                out.pending.push_back({std::move(what), std::move(command), std::move(group)});
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
            const auto format = xlings::home::format::run(home, format_steps());
            for (const auto& [step, why] : format.failed)
                pend(std::format("bringing the home to layout {} ({}) failed: {}", xlings::home::format::kLayout, step, why),
                     "xlings self doctor --fix");
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
                for (auto& step : upgrade_check())
                    pend(std::move(step.what), std::move(step.command), std::move(step.group));
            } catch (const std::exception& e) {
                pend(std::format("checking the home failed: {}", e.what()), "xlings self doctor");
            }
        }

        out.canGoBack = installed_(last);
        nlohmann::json pending = nlohmann::json::array();
        for (const auto& p : out.pending)
            pending.push_back({{"what", p.what}, {"command", p.command}, {"group", p.group}});
        nlohmann::json entry{{"from", last}, {"to", now}, {"at", now_utc()}, {"pending", pending}};
        entry["layout"] = xlings::home::format::kLayout;
        if (firstRecord) entry["first_record"] = true;
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
