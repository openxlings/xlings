module;

#include <ctime>
#include <cstdio>

module xlings.core.profile;

import std;
import xlings.core.destructive_log;
import xlings.core.config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.core.utils;
import xlings.core.xvm.db;
import xlings.core.xvm.types;

namespace xlings::profile {

std::string utc_now_iso_() {
    auto now   = std::chrono::system_clock::now();
    auto nowTT = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&nowTT));
    return buf;
}

int next_gen_number_(const fs::path& gensDir) {
    int maxNum = 0;
    if (!fs::exists(gensDir)) return 1;
    for (auto& entry : platform::dir_entries(gensDir)) {
        auto stem = entry.path().stem().string();
        try { maxNum = std::max(maxNum, std::stoi(stem)); }
        catch (...) {}
    }
    return maxNum + 1;
}

// NOTE: `.workspace.xvm.yaml` used to be written here and by rollback().
// Nothing in the tree ever read it -- which is what made `profile rollback`
// a no-op with respect to version selection: it recorded an intent in a file
// no reader consulted, and the live workspace kept whatever it had. Rollback
// now returns the selection so the caller can apply it through the real
// switch path, and the file is gone rather than left as a decoy.
std::uintmax_t dir_size_(const fs::path& dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != std::default_sentinel; ++it) {
        if (it->is_regular_file())
            total += it->file_size();
    }
    return total;
}

Generation load_current(const fs::path& envDir) {
    auto profilePath = envDir / ".profile.json";
    if (!fs::exists(profilePath)) return {0, "", "init", {}};
    try {
        auto content = platform::read_file_to_string(profilePath.string());
        auto json = nlohmann::json::parse(content);
        Generation gen;
        gen.number  = json.value("current_generation", 0);
        gen.created = json.value("created", "");
        gen.reason  = json.value("reason", "");
        if (json.contains("packages") && json["packages"].is_object()) {
            for (auto it = json["packages"].begin(); it != json["packages"].end(); ++it)
                gen.packages[it.key()] = it.value().get<std::string>();
        }
        return gen;
    } catch (...) { return {0, "", "init", {}}; }
}

int commit(const fs::path& envDir,
                  std::map<std::string, std::string> packages,
                  const std::string& reason) {
    auto gensDir = envDir / "generations";
    fs::create_directories(gensDir);

    int nextGen = next_gen_number_(gensDir);
    auto now = utc_now_iso_();

    nlohmann::json genJson = {
        {"generation", nextGen},
        {"created",    now},
        {"reason",     reason},
        {"packages",   packages}
    };
    auto genFile = gensDir / (([](int n) { char b[8]; std::snprintf(b, sizeof(b), "%03d", n); return std::string(b); })(nextGen) + ".json");
    platform::write_string_to_file(genFile.string(), genJson.dump(2));

    nlohmann::json profileJson = {
        {"current_generation", nextGen},
        {"packages",           packages}
    };
    platform::write_string_to_file((envDir / ".profile.json").string(),
                                profileJson.dump(2));

    return 0;
}

std::vector<Generation> list_generations(const fs::path& envDir) {
    std::vector<Generation> result;
    auto gensDir = envDir / "generations";
    if (!fs::exists(gensDir)) return result;

    for (auto& entry : platform::dir_entries(gensDir)) {
        if (!entry.is_regular_file()) continue;
        try {
            auto content = platform::read_file_to_string(entry.path().string());
            auto json = nlohmann::json::parse(content);
            Generation gen;
            gen.number  = json.value("generation", 0);
            gen.created = json.value("created", "");
            gen.reason  = json.value("reason", "");
            if (json.contains("packages") && json["packages"].is_object())
                for (auto it = json["packages"].begin(); it != json["packages"].end(); ++it)
                    gen.packages[it.key()] = it.value().get<std::string>();
            result.push_back(gen);
        } catch (...) {}
    }

    std::ranges::sort(result, {}, &Generation::number);
    return result;
}

std::expected<std::map<std::string, std::string>, std::string>
rollback(const fs::path& envDir, int targetGen) {
    std::map<std::string, std::string> packages;

    if (targetGen > 0) {
        auto gensDir = envDir / "generations";
        auto genFile = gensDir / (([](int n) { char b[8]; std::snprintf(b, sizeof(b), "%03d", n); return std::string(b); })(targetGen) + ".json");
        if (!fs::exists(genFile)) {
            return std::unexpected(
                std::format("generation {} not found", targetGen));
        }
        try {
            auto content = platform::read_file_to_string(genFile.string());
            auto json = nlohmann::json::parse(content);
            for (auto it = json["packages"].begin(); it != json["packages"].end(); ++it)
                packages[it.key()] = it.value().get<std::string>();
        } catch (...) {
            return std::unexpected(
                std::format("failed to read generation {}", targetGen));
        }
    }

    nlohmann::json profileJson = {
        {"current_generation", targetGen},
        {"packages",           packages}
    };
    platform::write_string_to_file((envDir / ".profile.json").string(),
                                profileJson.dump(2));

    return packages;
}

std::vector<SubosSnapshot> load_subos_snapshots(const fs::path& xlingsHome,
                                                 std::vector<std::string>* unreadable) {
    std::vector<SubosSnapshot> snapshots;
    auto subosDir = xlingsHome / "subos";
    std::error_code ec;
    if (!fs::is_directory(subosDir, ec)) return snapshots;

    for (auto& entry : platform::dir_entries(subosDir)) {
        std::error_code dec;
        if (!entry.is_directory(dec)) continue;
        auto name = entry.path().filename().string();
        if (name == "current") continue;
        auto wsPath = entry.path() / ".xlings.json";
        std::error_code fec;
        if (!fs::exists(wsPath, fec)) continue;
        auto mark_unreadable = [&] { if (unreadable) unreadable->push_back(name); };
        try {
            auto content = platform::read_file_to_string(wsPath.string());
            auto json = nlohmann::json::parse(content, nullptr, false);
            if (json.is_discarded() || !json.is_object()) {
                mark_unreadable();
                continue;
            }
            if (!json.contains("workspace") || !json["workspace"].is_object()) {
                mark_unreadable();
                continue;
            }
            snapshots.push_back({
                .name = name,
                .dir = entry.path(),
                .workspace = xvm::subos_workspace_from_json(json["workspace"]),
            });
        } catch (...) {
            mark_unreadable();
        }
    }
    std::ranges::sort(snapshots, [](const auto& a, const auto& b) {
        return a.name < b.name;
    });
    return snapshots;
}

bool save_subos_workspace(const fs::path& subosRoot,
                                 const xvm::SubosWorkspace& workspace) {
    const auto wsPath = subosRoot / ".xlings.json";
    nlohmann::json json = nlohmann::json::object();
    std::error_code ec;
    if (fs::exists(wsPath, ec)) {
        try {
            auto content = platform::read_file_to_string(wsPath.string());
            auto parsed = nlohmann::json::parse(content, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) {
                json = std::move(parsed);
            } else {
                // Refuse rather than replace: an unreadable file is not an
                // empty one, and overwriting it would turn "we could not read
                // your subos" into "your subos is now blank".
                return false;
            }
        } catch (...) {
            return false;
        }
    }
    json["workspace"] = xvm::subos_workspace_to_json(workspace);
    try {
        platform::write_string_to_file(wsPath.string(), json.dump(2));
    } catch (...) {
        return false;
    }
    return true;
}

// Build a set of referenced xpkg "dirname/version" keys from all subos workspaces
// by mapping workspace target names to xpkg directory paths via the versions DB.
// Every `<store>/<version>` some subos still references -- or why that cannot
// be answered. "Could not read" is not "references nothing": an unreadable
// subos config or version DB used to contribute no references, and every
// payload only it used was then collected (measured with `self clean
// --dry-run`: one bad subos config, its package listed for removal). So a
// reader that cannot see everything refuses, and says which part it could not
// see. The refusal is binary on purpose -- see AGENTS.md on derived tables.
std::expected<std::set<std::string>, std::string>
collect_subos_references_(const fs::path& xlingsHome) {
    std::set<std::string> referenced;

    // Load global versions DB
    auto configPath = xlingsHome / ".xlings.json";
    xvm::VersionDB globalDB;
    if (fs::exists(configPath)) {
        nlohmann::json json;
        try {
            json = nlohmann::json::parse(
                platform::read_file_to_string(configPath.string()), nullptr, false);
        } catch (...) {
            json = nlohmann::json::value_t::discarded;
        }
        if (json.is_discarded() || !json.is_object()) {
            return std::unexpected(std::format(
                "{} could not be read, so which packages are in use is unknown",
                configPath.string()));
        }
        if (json.contains("versions"))
            globalDB = xvm::versions_from_json(json["versions"]);
    }

    // Build a map: xpkg_dir_name/version -> key, from versions DB paths
    // e.g., "xim-x-gcc/15.1.0" from path "/home/.../.xlings/data/xpkgs/xim-x-gcc/15.1.0/bin"
    //
    // Pin every payload of every target name that appears in either the
    // active or installed[] map. (The lambda is intentionally coarse:
    // currently a target name in workspace pins ALL versions of that
    // target; finer-grained per-version pinning is a separate
    // refactor.) The 0.4.19 fix here is to also walk installed[] —
    // pre-fix, a target only pinned in installed[] of some subos (e.g.
    // after `xlings remove` cleared its active but the user kept
    // installed[]) was eligible for GC.
    auto add_refs_from_subos_workspace = [&](const xvm::SubosWorkspace& sws) {
        std::set<std::string> targetNames;
        for (auto& [target, _] : sws.active) targetNames.insert(target);
        for (auto& [target, _] : sws.installed) targetNames.insert(target);
        for (auto& target : targetNames) {
            auto it = globalDB.find(target);
            if (it == globalDB.end()) continue;
            for (auto& [verKey, vdata] : it->second.versions) {
                if (vdata.path.empty()) continue;
                // `.../xpkgs/<store>/<version>/...`, by path component: a
                // substring search for "xpkgs/" missed every record written
                // with backslashes.
                const fs::path recorded(vdata.path);
                for (auto part = recorded.begin(); part != recorded.end(); ++part) {
                    if (part->string() != "xpkgs") continue;
                    auto store = std::next(part);
                    if (store == recorded.end()) break;
                    auto version = std::next(store);
                    if (version == recorded.end()) break;
                    referenced.insert(store->string() + "/" + version->string());
                    break;
                }
            }
        }
    };

    // Scan all subos workspace files
    std::vector<std::string> unreadable;
    for (auto& snapshot : load_subos_snapshots(xlingsHome, &unreadable)) {
        add_refs_from_subos_workspace(snapshot.workspace);
    }
    if (!unreadable.empty()) {
        std::string names;
        for (const auto& n : unreadable) names += (names.empty() ? "" : ", ") + n;
        return std::unexpected(std::format(
            "the config of subos {} could not be read, so the packages it uses "
            "are unknown", names));
    }

    return referenced;
}

std::vector<std::string> find_subos_referencing(
        const fs::path& xlingsHome, const std::string& target,
        std::vector<std::string>* unreadable) {
    std::vector<std::string> result;
    for (auto& snapshot : load_subos_snapshots(xlingsHome, unreadable)) {
        if (snapshot.workspace.active.contains(target)
            || snapshot.workspace.installed.contains(target)) {
            result.push_back(snapshot.name);
        }
    }
    return result;
}

std::vector<std::string> find_subos_pinning_version(
        const fs::path& xlingsHome,
        const std::string& target,
        const std::string& version,
        std::vector<std::string>* unreadable) {
    // Both spellings of a key name the same record; see
    // xvm::version_key_matches for which spellings those are.
    auto matches = [&](std::string_view stored) {
        return xvm::version_key_matches(version, stored);
    };

    std::vector<std::string> result;
    for (auto& snapshot : load_subos_snapshots(xlingsHome, unreadable)) {
        const auto& ws = snapshot.workspace;
        bool pinned = false;
        if (auto it = ws.active.find(target);
            it != ws.active.end() && matches(it->second)) {
            pinned = true;
        }
        if (!pinned) {
            // installed[] pins the payload just as hard as active does — a
            // subos that opted into the version but is not currently using it
            // still breaks if the payload goes.
            if (auto it = ws.installed.find(target); it != ws.installed.end()) {
                for (auto& v : it->second) {
                    if (matches(v)) { pinned = true; break; }
                }
            }
        }
        if (pinned) result.push_back(snapshot.name);
    }
    return result;
}

int gc(const fs::path& xlingsHome, bool dryRun) {
    auto references = collect_subos_references_(xlingsHome);
    if (!references) {
        std::println("[xlings:store] gc refused: {}", references.error());
        std::println("  nothing was removed; `xlings self doctor --fix` repairs an "
                     "unreadable subos config, then run this again");
        return 1;
    }
    const auto& referenced = *references;

    auto pkgDir = xlingsHome / "data" / "xpkgs";
    if (!fs::exists(pkgDir)) {
        std::println("[xlings:store] xpkgs not found, nothing to gc");
        return 0;
    }

    std::uintmax_t freedBytes = 0;
    int removedCount = 0;

    for (auto& pkgEntry : platform::dir_entries(pkgDir)) {
        if (!pkgEntry.is_directory()) continue;
        auto pkgName = pkgEntry.path().filename().string();
        for (auto& verEntry : platform::dir_entries(pkgEntry)) {
            if (!verEntry.is_directory()) continue;
            auto ver = verEntry.path().filename().string();
            auto key = pkgName + "/" + ver;
            if (!referenced.count(key)) {
                auto size = dir_size_(verEntry.path());
                if (dryRun) {
                    auto mb = static_cast<int>(static_cast<double>(size) / 1e5) / 10.0;
                    std::println("  would remove xpkgs/{}/{} ({} MB)", pkgName, ver, mb);
                } else {
                    std::error_code ec;
                    const auto held = destructive_log::measure(verEntry.path());
                    fs::remove_all(verEntry.path(), ec);
                    destructive_log::record({
                        .op = "gc", .path = verEntry.path(), .bytes = held.bytes,
                        .files = held.files, .confirmedBy = "self clean",
                        .detail = ec ? "incomplete: " + ec.message() : std::string{},
                    });
                    if (!ec)
                        std::println("[xlings:store] removed xpkgs/{}/{}", pkgName, ver);
                }
                freedBytes += size;
                ++removedCount;
            }
        }
    }

    auto totalMb = static_cast<int>(static_cast<double>(freedBytes) / 1e5) / 10.0;
    if (dryRun) {
        std::println("[xlings:store] gc dry-run: {} packages, {} MB would be freed",
            removedCount, totalMb);
    } else {
        std::println("[xlings:store] gc: {} packages removed, {} MB freed",
            removedCount, totalMb);
    }
    return 0;
}

}
