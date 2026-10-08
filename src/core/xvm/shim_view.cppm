export module xlings.core.xvm.shim_view;

import std;

import xlings.libs.json;
import xlings.core.xvm.types;
import xlings.core.xvm.db;

// The shim dispatch cache: what one dispatch needs, resolved once and kept
// beside the subos it was resolved for.
//
// Every `mcpp` on PATH is a new process, so nothing a process memoizes
// survives to the next invocation. This cache is the cross-process form of
// the same idea -- and it is DERIVED data, under the same discipline as the
// routing table it sits next to:
//
//   * It is never an authority. `load_shim_view` validates the caller's
//     fresh stats of every input file against the fingerprint the view was
//     stored with, and EVERY failure -- missing file, unreadable, malformed,
//     wrong program, any stat mismatch -- is a plain miss. A miss costs a
//     full resolution; it can never produce a different answer.
//
//   * Readers may store. After a full resolution succeeds, the dispatcher
//     stores what it resolved, stamped with the stats it actually used. A
//     racing writer is harmless: the store is an atomic replace, and a view
//     stored against soon-to-be-stale stats fails the NEXT load's
//     fingerprint check. Additive-only writes also mean no reader can ever
//     delete someone else's state -- the failure mode that shaped the
//     routing table's one-writer rule does not exist here.
//
//   * It can always be deleted. Nothing rebuilds FROM it, nothing refuses
//     to run without it; the worst case of a wiped directory is that every
//     dispatch resolves the slow way until it stores again.
export namespace xlings::xvm {

// What one dispatch consumes: the effective workspace (active versions),
// the effective installed[] sets, and the ONE target's entry with every
// layer merged exactly as merged_versions would merge it.
struct ShimView {
    Workspace workspace;
    WorkspaceInstalled installed;
    VersionDB slice;

#if defined(_MSC_VER)  // platform-if-ok: compiler, not platform: GCC/MSVC module special members
    ShimView() = default;
    ~ShimView() = default;
    ShimView(const ShimView&) = default;
    ShimView& operator=(const ShimView&) = default;
    ShimView(ShimView&&) = default;
    ShimView& operator=(ShimView&&) = default;
#else
    ShimView();
    ~ShimView();
    ShimView(const ShimView&);
    ShimView& operator=(const ShimView&);
    ShimView(ShimView&&);
    ShimView& operator=(ShimView&&);
#endif
};

#if !defined(_MSC_VER)  // platform-if-ok: compiler, not platform: GCC/MSVC module special members
// Out-of-line special members to work around GCC module boundary issues
// (same treatment as VData/VInfo in types.cppm).
xlings::xvm::ShimView::ShimView() = default;
xlings::xvm::ShimView::~ShimView() = default;
xlings::xvm::ShimView::ShimView(const ShimView&) = default;
xlings::xvm::ShimView& xlings::xvm::ShimView::operator=(const ShimView&) = default;
xlings::xvm::ShimView::ShimView(ShimView&&) = default;
xlings::xvm::ShimView& xlings::xvm::ShimView::operator=(ShimView&&) = default;
#endif

// A file identity: size plus mtime ticks of the file's own clock. Compared,
// never interpreted -- the cache is platform-local.
struct ShimViewStat {
    std::uintmax_t size {};
    std::int64_t mtime {};
    bool operator==(const ShimViewStat&) const = default;
};

// Stat a fingerprint input. nullopt means "cannot be stat'ed", which is
// itself an identity: a caller carries the nullopt so that an input
// vanishing between store and load reads as a mismatch, not as a match.
[[nodiscard]] std::optional<ShimViewStat>
shim_view_stat(const std::filesystem::path& p);

// Load the view for (subos, context, program). `fingerprint` is the
// CALLER'S freshly-taken stats of every input the view depends on; the
// stored fingerprint must equal it exactly -- same paths, same identities,
// nothing missing -- or the load is a miss.
[[nodiscard]] std::optional<ShimView>
load_shim_view(const std::filesystem::path& subosDir,
               std::string_view ctxTag,
               const std::string& program,
               const std::map<std::string, ShimViewStat>& fingerprint);

// Store a view under the caller's fingerprint. Best-effort by design:
// a failed store is logged and dropped, and the next dispatch pays a full
// resolution. That is the whole failure model -- this cache can only ever
// cost time, never correctness.
void store_shim_view(const std::filesystem::path& subosDir,
                     std::string_view ctxTag,
                     const std::string& program,
                     const std::map<std::string, ShimViewStat>& fingerprint,
                     const ShimView& view);

}  // namespace xlings::xvm
