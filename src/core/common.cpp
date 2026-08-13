module;

module xlings.core.common;

import std;
import xlings.core.config;
import xlings.core.xself;

namespace xlings::common {

void mirror_shim_to_global_bin(const fs::path& xlings_bin,
                                      const std::string& shim_name) {
    if (!Config::has_project_config()) return;
    auto global_bin = Config::global_subos_bin_dir();
    auto current_bin = Config::paths().binDir;
    if (global_bin == current_bin) return;
    fs::create_directories(global_bin);
    auto dst = global_bin / shim_name;
    if (!fs::exists(dst)) {
        xself::create_shim(xlings_bin, dst);
    }
}

}
