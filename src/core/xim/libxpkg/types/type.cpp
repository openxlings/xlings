module xlings.core.xim.libxpkg.types.type;

import std;


// ── out-of-line class members ──────────────────────────────────

namespace xlings::xim {

bool InstallPlan::has_errors() const { return !errors.empty() || !unsupported.empty(); }

std::string InstallPlan::first_error() const {
    if (!errors.empty()) return errors.front();
    if (unsupported.empty()) return {};
    const auto& u = unsupported.front();
    return std::format("{}: {} has no {} artifact", u.chain, u.package, u.target);
}

std::size_t InstallPlan::pending_count() const {
    std::size_t n = 0;
    for (auto& nd : nodes)
        if (!nd.alreadyInstalled) ++n;
    return n;
}

} // namespace xlings::xim
