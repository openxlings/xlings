module xlings.core.xim.libxpkg.types.type;

import std;


// ── out-of-line class members ──────────────────────────────────

namespace xlings::xim {

bool InstallPlan::has_errors() const { return !errors.empty(); }

std::size_t InstallPlan::pending_count() const {
    std::size_t n = 0;
    for (auto& nd : nodes)
        if (!nd.alreadyInstalled) ++n;
    return n;
}

} // namespace xlings::xim
