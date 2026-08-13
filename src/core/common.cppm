module;

export module xlings.core.common;

import std;

import xlings.core.config;
import xlings.core.xself;

namespace xlings::common {

namespace fs = std::filesystem;

// Mirror a shim to global subos bin when in project context.
// Project subos bin is not in PATH; global subos/current/bin is.
// Only creates if missing — never overwrites existing global shims.
export void mirror_shim_to_global_bin(const fs::path& xlings_bin,
                                      const std::string& shim_name);

} // namespace xlings::common
