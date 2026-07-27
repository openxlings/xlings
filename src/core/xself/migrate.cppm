export module xlings.core.xself.migrate;

import std;

import xlings.core.config;
import xlings.core.home_config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;

namespace xlings::xself {

// `xlings self migrate` — one-shot migration of the legacy `data/` layout
// to `subos/default/`. Idempotent: a no-op once `subos/default/bin` exists.
export int cmd_migrate() {
    namespace fs = std::filesystem;
    auto& p = Config::paths();
    auto subosDir   = p.homeDir / "subos";
    auto defaultDir = subosDir / "default";

    if (fs::exists(defaultDir / "bin")) {
        log::info("already migrated (subos/default/bin exists)");
        return 0;
    }

    fs::create_directories(defaultDir);

    auto oldDataDir = p.homeDir / "data";
    bool moved = false;

    auto move_if_exists = [&](const std::string& name) {
        auto src = oldDataDir / name;
        auto dst = defaultDir / name;
        if (fs::exists(src)) {
            std::error_code ec;
            fs::rename(src, dst, ec);
            if (ec) {
                fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    log::error("failed to move {}: {}", name, ec.message());
                    return;
                }
                fs::remove_all(src, ec);
            }
            log::info("migrated data/{} -> subos/default/{}", name, name);
            moved = true;
        }
    };

    move_if_exists("bin");
    move_if_exists("lib");
    move_if_exists("xvm");

    fs::create_directories(defaultDir / "usr");
    fs::create_directories(defaultDir / "generations");

    // The data-tree move above is the slow part; commit the config edit
    // afterwards under the state lock rather than rewriting a document read
    // before it. See core/home_config.cppm.
    auto committed = update_home_config(p.homeDir, [](nlohmann::json& json) {
        json["activeSubos"] = "default";
        if (!json.contains("subos") || !json["subos"].is_object()) {
            json["subos"] = nlohmann::json::object();
        }
        json["subos"]["default"] = {{"dir", ""}};
        return true;
    });
    if (!committed) {
        log::error("migration moved the data tree but could not update "
                   "the config: {}", committed.error());
        return 1;
    }

    auto currentLink = subosDir / "current";
    std::error_code lec;
    fs::remove(currentLink, lec);
    fs::create_directory_symlink(defaultDir, currentLink, lec);

    if (moved) {
        log::info("migration complete");
    } else {
        log::info("no legacy data found; initialized subos/default");
    }
    return 0;
}

} // namespace xlings::xself
