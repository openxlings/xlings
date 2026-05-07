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

void sync_workspace_yaml_(const fs::path& envDir,
                          const std::map<std::string, std::string>& packages) {
    auto xvmDir = envDir / "xvm";
    fs::create_directories(xvmDir);
    auto yamlPath = xvmDir / ".workspace.xvm.yaml";

    std::string yaml = "versions:\n";
    for (auto& [name, ver] : packages) {
        yaml += "  " + name + ": " + ver + "\n";
    }
    platform::write_string_to_file(yamlPath.string(), yaml);
}

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

    sync_workspace_yaml_(envDir, packages);

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

export int rollback(const fs::path& envDir, int targetGen) {
    std::map<std::string, std::string> packages;

    if (targetGen > 0) {
        auto gensDir = envDir / "generations";
        auto genFile = gensDir / (([](int n) { char b[8]; std::snprintf(b, sizeof(b), "%03d", n); return std::string(b); })(targetGen) + ".json");
        if (!fs::exists(genFile)) {
            log::error("[xlings:profile] generation {} not found", targetGen);
            return 1;
        }
        try {
            auto content = platform::read_file_to_string(genFile.string());
            auto json = nlohmann::json::parse(content);
            for (auto it = json["packages"].begin(); it != json["packages"].end(); ++it)
                packages[it.key()] = it.value().get<std::string>();
        } catch (...) {
            log::error("[xlings:profile] failed to read generation {}", targetGen);
            return 1;
        }
    }

    sync_workspace_yaml_(envDir, packages);

    nlohmann::json profileJson = {
        {"current_generation", targetGen},
        {"packages",           packages}
    };
    platform::write_string_to_file((envDir / ".profile.json").string(),
                                profileJson.dump(2));

    std::println("[xlings:profile] rolled back to generation {}", targetGen);
    return 0;
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
    auto subosDir = xlingsHome / "subos";
    if (fs::exists(subosDir)) {
        for (auto& envEntry : platform::dir_entries(subosDir)) {
            if (!envEntry.is_directory()) continue;
            auto name = envEntry.path().filename().string();
            if (name == "current") continue; // symlink
            auto wsPath = envEntry.path() / ".xlings.json";
            if (!fs::exists(wsPath)) continue;
            try {
                auto content = platform::read_file_to_string(wsPath.string());
                auto json = nlohmann::json::parse(content, nullptr, false);
                if (!json.is_discarded() && json.contains("workspace"))
                    add_refs_from_subos_workspace(xvm::subos_workspace_from_json(json["workspace"]));
            } catch (...) {}
        }
    }

    return referenced;
}

// Find which subos environments reference a given package target name.
export std::vector<std::string> find_subos_referencing(
        const fs::path& xlingsHome, const std::string& target) {
    std::vector<std::string> result;
    auto subosDir = xlingsHome / "subos";
    if (!fs::exists(subosDir)) return result;

    for (auto& envEntry : platform::dir_entries(subosDir)) {
        if (!envEntry.is_directory()) continue;
        auto name = envEntry.path().filename().string();
        if (name == "current") continue;
        auto wsPath = envEntry.path() / ".xlings.json";
        if (!fs::exists(wsPath)) continue;
        try {
            auto content = platform::read_file_to_string(wsPath.string());
            auto json = nlohmann::json::parse(content, nullptr, false);
            if (json.is_discarded() || !json.contains("workspace")) continue;
            auto sws = xvm::subos_workspace_from_json(json["workspace"]);
            if (sws.active.contains(target) || sws.installed.contains(target))
                result.push_back(name);
        } catch (...) {}
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
