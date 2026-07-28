module;

#include <ctime>
#include <cstdio>

export module xlings.core.profile;

import std;

import xlings.core.config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.core.utils;
import xlings.core.xvm.db;
import xlings.core.xvm.types;

namespace xlings::profile {

namespace fs = std::filesystem;

export struct Generation {
    int                                    number;
    std::string                            created;
    std::string                            reason;
    std::map<std::string, std::string>     packages;
};

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

export Generation load_current(const fs::path& envDir) {
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

export int commit(const fs::path& envDir,
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

export std::vector<Generation> list_generations(const fs::path& envDir) {
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

// Roll the recorded selection back to a generation, and hand it to the
// caller to apply.
//
// Applying is deliberately not done here. Putting a version back means
// running the same switch `xlings use` runs -- resolving the release,
// moving shims, libraries, headers and declared file assets -- and that
// lives in xvm::commands, which imports xself, which imports this module.
// Returning the selection keeps the cycle out of the module graph and keeps
// one implementation of "make this version active" rather than two.
//
// This used to write the selection into `<envDir>/xvm/.workspace.xvm.yaml`
// and stop. Nothing read that file, so rollback changed no version at all.
export std::expected<std::map<std::string, std::string>, std::string>
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

// One subos's workspace file, as read off disk.
//
// The unit every cross-subos question is answered in: "who else references
// this version", "which subos owns this finding", "is any other subos still
// pointing at something the DB no longer has". Those used to be three
// separate walks of ~/.xlings/subos/*/.xlings.json with three slightly
// different notions of what counts as a readable file; this is the one walk.
export struct SubosSnapshot {
    std::string         name;
    // The subos root, so a caller can tell "the subos named x under this home"
    // from "the project subos that happens to be named x" without guessing
    // from the name.
    fs::path            dir;
    xvm::SubosWorkspace workspace;
};

// Every subos under a home, in name order.
//
// Unreadable and malformed files are SKIPPED, not reported: this feeds
// read-only inspection and reference counting, and a subos whose config a
// user hand-edited into invalid JSON must not take down an unrelated
// `remove`. `current` is a symlink to the active one and would double-count.
export std::vector<SubosSnapshot> load_subos_snapshots(const fs::path& xlingsHome) {
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
        try {
            auto content = platform::read_file_to_string(wsPath.string());
            auto json = nlohmann::json::parse(content, nullptr, false);
            if (json.is_discarded() || !json.is_object()) continue;
            if (!json.contains("workspace") || !json["workspace"].is_object()) continue;
            snapshots.push_back({
                .name = name,
                .dir = entry.path(),
                .workspace = xvm::subos_workspace_from_json(json["workspace"]),
            });
        } catch (...) {}
    }
    std::ranges::sort(snapshots, [](const auto& a, const auto& b) {
        return a.name < b.name;
    });
    return snapshots;
}

// Build a set of referenced xpkg "dirname/version" keys from all subos workspaces
// by mapping workspace target names to xpkg directory paths via the versions DB.
std::set<std::string> collect_subos_references_(const fs::path& xlingsHome) {
    std::set<std::string> referenced;
    auto xpkgsDir = xlingsHome / "data" / "xpkgs";

    // Load global versions DB
    auto configPath = xlingsHome / ".xlings.json";
    xvm::VersionDB globalDB;
    if (fs::exists(configPath)) {
        try {
            auto content = platform::read_file_to_string(configPath.string());
            auto json = nlohmann::json::parse(content, nullptr, false);
            if (!json.is_discarded() && json.contains("versions"))
                globalDB = xvm::versions_from_json(json["versions"]);
        } catch (...) {}
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
                // Extract xpkg dir from path: .../xpkgs/<dirname>/<version>/...
                auto xpkgsPos = vdata.path.find("xpkgs/");
                if (xpkgsPos == std::string::npos) continue;
                auto rel = vdata.path.substr(xpkgsPos + 6); // after "xpkgs/"
                // rel is like "xim-x-gcc/15.1.0/bin" or "xim-x-gcc/15.1.0"
                // Extract first two path components: dirname/version
                auto slash1 = rel.find('/');
                if (slash1 == std::string::npos) continue;
                auto slash2 = rel.find('/', slash1 + 1);
                auto dirAndVer = (slash2 != std::string::npos)
                    ? rel.substr(0, slash2)
                    : rel;
                referenced.insert(dirAndVer);
            }
        }
    };

    // Scan all subos workspace files
    for (auto& snapshot : load_subos_snapshots(xlingsHome)) {
        add_refs_from_subos_workspace(snapshot.workspace);
    }

    return referenced;
}

// Find which subos environments reference a given package target name.
export std::vector<std::string> find_subos_referencing(
        const fs::path& xlingsHome, const std::string& target) {
    std::vector<std::string> result;
    for (auto& snapshot : load_subos_snapshots(xlingsHome)) {
        if (snapshot.workspace.active.contains(target)
            || snapshot.workspace.installed.contains(target)) {
            result.push_back(snapshot.name);
        }
    }
    return result;
}

// Which subos still pin an EXACT version of a target.
//
// The version-exact counterpart to find_subos_referencing(). `remove` keeps a
// payload alive when any OTHER subos still references the exact version being
// removed (installer.cppm's stillReferenced branch); naming those subos is
// what turns "✓ removed" from a claim the state contradicts into a next step.
//
// Version-exact and not name-only, because on a multi-version home the
// name-only answer names subos that pin a different version and therefore
// cannot explain why this payload was kept.
//
// Namespace handling mirrors is_version_referenced_anywhere_: stored values
// are namespaced for non-primary index repos (`local:0.0.1`), and the two must
// agree or this would omit a subos that really is holding the payload down.
export std::vector<std::string> find_subos_pinning_version(
        const fs::path& xlingsHome,
        const std::string& target,
        const std::string& version) {
    auto matches = [&](std::string_view stored) {
        return stored == version
            || xvm::strip_namespace(std::string(stored)) == version;
    };

    std::vector<std::string> result;
    for (auto& snapshot : load_subos_snapshots(xlingsHome)) {
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

export int gc(const fs::path& xlingsHome, bool dryRun = false) {
    auto referenced = collect_subos_references_(xlingsHome);

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
                    fs::remove_all(verEntry.path(), ec);
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

} // namespace xlings::profile
