module xlings.core.xself.clean;

import std;
import xlings.core.config;
import xlings.core.log;
import xlings.platform;
import xlings.core.profile;

namespace xlings::xself {

int cmd_clean(bool dryRun) {
    namespace fs = std::filesystem;
    auto& p = Config::paths();

    // A legacy cache directory, removed by NAME -- so first make sure the
    // name means that here. With XLINGS_HOME set to the user's home directory
    // `<home>/.xlings` is a whole xlings home (every subos, every package),
    // and a directory holding an xlings home's own markers is one wherever it
    // sits. Neither is a cache.
    auto cachedir = p.homeDir / ".xlings";
    std::error_code sameEc;
    const auto userHome = platform::get_home_dir();
    const bool isUserHome = !userHome.empty()
        && fs::equivalent(p.homeDir, fs::path(userHome), sameEc);
    const bool looksLikeHome = fs::exists(cachedir / ".xlings.json")
        || fs::exists(cachedir / "subos") || fs::exists(cachedir / "data");
    if (fs::is_directory(cachedir) && (isUserHome || looksLikeHome)) {
        log::println("  kept {}: it is an xlings home, not a cache", cachedir.string());
    } else if (fs::exists(cachedir) && fs::is_directory(cachedir)) {
        if (dryRun) {
            log::println("  would remove cache: {}", cachedir.string());
        } else {
            std::error_code ec;
            fs::remove_all(cachedir, ec);
            if (ec) {
                log::error("failed to remove {}: {}", cachedir.string(), ec.message());
                return 1;
            }
            log::debug("cleaned cache: {}", cachedir.string());
        }
    }

    profile::gc(p.homeDir, dryRun);

    if (!dryRun) log::info("clean ok");
    return 0;
}

}
