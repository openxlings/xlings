module xlings.core.xvm.relocation;

import std;
import xlings.core.xvm.types;

namespace xlings::xvm {

namespace {

constexpr bool is_sep_(char c) { return c == '/' || c == '\\'; }

// Separator spelling is not identity, and on Windows neither is case.
//
// The records and `Config::paths().homeDir` are produced by different code
// paths -- one round-trips through JSON, the other through std::filesystem --
// so `C:\Users\me\.xlings` and `C:/Users/me/.xlings` name one home. Treating
// them as two would diagnose every Windows home as relocated, which is the
// most expensive false positive this module could produce.
constexpr char fold_(char c) {
    if (is_sep_(c)) return '/';
#ifdef _WIN32
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
#endif
    return c;
}

std::string_view trim_trailing_seps_(std::string_view p) {
    while (p.size() > 1 && is_sep_(p.back())) p.remove_suffix(1);
    return p;
}

}  // namespace

bool same_path_text(std::string_view a, std::string_view b) {
    a = trim_trailing_seps_(a);
    b = trim_trailing_seps_(b);
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (fold_(a[i]) != fold_(b[i])) return false;
    }
    return true;
}

std::optional<std::pair<std::string_view, std::string_view>>
split_store_path(std::string_view path) {
    constexpr std::string_view kData  = "data";
    constexpr std::string_view kXpkgs = "xpkgs";
    // <root>/data/xpkgs -- the shortest match needs a separator before `data`,
    // one between the two components, and both names.
    const std::size_t need = 1 + kData.size() + 1 + kXpkgs.size();
    if (path.size() < need) return std::nullopt;

    for (std::size_t i = 1; i + need - 1 <= path.size(); ++i) {
        if (!is_sep_(path[i - 1])) continue;
        if (path.compare(i, kData.size(), kData) != 0) continue;
        std::size_t j = i + kData.size();
        if (j >= path.size() || !is_sep_(path[j])) continue;
        ++j;
        if (j + kXpkgs.size() > path.size()) return std::nullopt;
        if (path.compare(j, kXpkgs.size(), kXpkgs) != 0) continue;
        const std::size_t k = j + kXpkgs.size();
        if (k != path.size() && !is_sep_(path[k])) continue;
        return std::pair{path.substr(0, i - 1), path.substr(i)};
    }
    return std::nullopt;
}

std::string relocated_path(const HomeRelocation& reloc,
                           std::string_view recorded) {
    if (reloc.empty() || recorded.size() < reloc.oldRoot.size()) return {};
    const std::string_view root{reloc.oldRoot};
    for (std::size_t i = 0; i < root.size(); ++i) {
        if (fold_(recorded[i]) != fold_(root[i])) return {};
    }
    // A prefix must end on a boundary: `<old>-backup/...` is not under
    // `<old>/`.
    if (recorded.size() > root.size() && !is_sep_(recorded[root.size()])) {
        return {};
    }
    std::string out = reloc.newRoot;
    out.append(recorded.substr(root.size()));
    return out;
}

std::optional<HomeRelocation> detect_relocation(const VersionDB& db,
                                                std::string_view homeDir,
                                                const PathProbe& exists,
                                                const SamePlaceProbe& samePlace) {
    if (homeDir.empty() || !exists || !samePlace) return std::nullopt;

    // Ordered, so a tie between two foreign roots resolves the same way on
    // every run and on every platform. A doctor that reports a different root
    // each time it is asked is worse than one that reports none.
    std::map<std::string, std::size_t> roots;
    for (const auto& [target, info] : db) {
        for (const auto& [version, data] : info.versions) {
            if (data.path.empty()) continue;
            const auto split = split_store_path(data.path);
            if (!split) continue;                       // not a store path
            if (same_path_text(split->first, homeDir)) continue;
            ++roots[std::string(split->first)];
        }
    }
    if (roots.empty()) return std::nullopt;

    const auto best = std::ranges::max_element(
        roots, [](const auto& a, const auto& b) { return a.second < b.second; });

    // A root that is still a live home of its own is not a move. Without this
    // clause, a project scope -- whose records name `<checkout>/.xlings` while
    // `homeDir` is the global home, and which `Config::versions()` merges into
    // one view -- would be diagnosed as relocated and its payload paths
    // re-pointed into the global store the moment the same package exists in
    // both. The one exception is the workaround: a symlink at the old path
    // that resolves to this home.
    if (exists(best->first) && !samePlace(best->first, std::string(homeDir))) {
        return std::nullopt;
    }

    HomeRelocation reloc{
        .oldRoot = best->first,
        .newRoot = std::string(trim_trailing_seps_(homeDir)),
        .entries = best->second,
    };

    // Confirm against the disk, deduplicated by payload directory: 2908
    // records on the measured home resolve to ~700 directories, and the
    // question is per directory.
    std::map<std::string, bool> probed;
    for (const auto& [target, info] : db) {
        for (const auto& [version, data] : info.versions) {
            if (data.path.empty()) continue;
            auto here = relocated_path(reloc, data.path);
            if (here.empty()) continue;
            auto it = probed.find(here);
            if (it == probed.end()) it = probed.emplace(here, exists(here)).first;
            if (it->second) ++reloc.recoverable;
        }
    }

    // Nothing to point at is not a relocation. A home whose packages were
    // genuinely removed must keep reaching the prune.
    if (reloc.recoverable == 0) return std::nullopt;
    return reloc;
}

}  // namespace xlings::xvm
