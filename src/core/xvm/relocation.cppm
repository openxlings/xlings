export module xlings.core.xvm.relocation;

import std;

import xlings.core.xvm.types;

// Where were these records written, and does what they name still exist here?
//
// A home records payload paths ABSOLUTELY -- measured on a real home, 2901 of
// 2908 version records share one prefix and the other 7 are self-managed tools
// outside the store (`~/.cargo/bin`, `/usr/bin`). Move the home and every one
// of those records names a directory that is gone, while the payload itself is
// sitting under the new root untouched.
//
// That difference is the whole module. `self doctor --fix` used to ask only
// "does the recorded path exist" and, on a home that had been moved, answered
// no 411 times: it deleted 1173 sysroot links and dropped 367 registrations
// while all 234 package directories were present under the new root, reporting
// `its payload is gone and nothing can restore it` about payloads that were
// there. The question it never asked is the one this module answers.
//
// WHAT THIS IS NOT. Detecting the move does not make a home relocatable, and
// nothing here pretends otherwise. `PT_INTERP` is read literally by the kernel,
// linker scripts name absolute paths, and payloads carry the root they were
// built against. Those are not repaired -- by this module or by anything that
// consumes it. What is repairable is the bookkeeping xlings owns: the versions
// database and the sysroot links it placed.
//
// Pure over (db, path strings) with existence injected, in the same spirit as
// xvm/owner.cppm: the whole detection order is reachable from unit tests
// without a filesystem.
export namespace xlings::xvm {

// The root a home's records were written for, when that is not where the home
// is now.
struct HomeRelocation {
    std::string oldRoot;        // the prefix the records carry
    std::string newRoot;        // where this home is now
    std::size_t entries{};      // version records still naming oldRoot
    std::size_t recoverable{};  // of those, payloads present under newRoot

    [[nodiscard]] bool empty() const { return oldRoot.empty(); }
};

// Split a recorded path at its `data/xpkgs` segment: {root, tail}.
//
// The FIRST occurrence, not the last. A payload that packages an xlings home
// of its own would contain a second one, and the outer store is the one that
// says where this record's home was.
//
// Both separators are accepted on every platform. A home written on Windows
// stores `C:\Users\me\.xlings\data\xpkgs\...`, and a record can carry mixed
// separators -- `C:/Users/me/.xlings\data\xpkgs\...` is a real shape, because
// the two halves are joined by different code.
std::optional<std::pair<std::string_view, std::string_view>>
split_store_path(std::string_view path);

// Do two path strings name the same location, ignoring separator spelling and
// a trailing separator? Textual only -- no symlink resolution, which is the
// point: a home reached through a symlink at the old path is a DIFFERENT
// string, and that is exactly the state this module has to notice.
bool same_path_text(std::string_view a, std::string_view b);

// `recorded` expressed under the current root. Empty when `recorded` is not
// under `oldRoot` -- a self-managed tool outside the store keeps its path.
//
// Plain prefix substitution. The tail is copied verbatim, separators included,
// so a pathological record survives translation unchanged rather than being
// quietly normalised into a different path.
std::string relocated_path(const HomeRelocation& reloc,
                           std::string_view recorded);

// Existence, injected.
using PathProbe = std::function<bool(const std::string&)>;

// Do these two paths name the same directory on disk, symlinks resolved?
//
// Separate from `same_path_text` and not a substitute for it. The text
// comparison is what NOTICES that the records name another root; this one is
// what tells the two reasons apart:
//
//   * a compensating symlink at the old path -- the workaround users reach for
//     -- resolves to this very home, and re-pointing away from it is a repair;
//   * a root that is a DIFFERENT live home (a project-scope `.xlings` under a
//     checkout, a second home on the same machine) is not this home's business.
//     Its records legitimately name it, and `Config::versions()` merges the two
//     scopes, so without this the project's payloads would be re-pointed into
//     the global store the moment the same package exists in both.
using SamePlaceProbe =
    std::function<bool(const std::string& a, const std::string& b)>;

// Was this home's database written for another root?
//
// Answers only when it can point at a payload: the modal foreign store prefix
// must have at least one record whose counterpart EXISTS under `homeDir`.
// Without that clause a home whose packages were genuinely deleted would be
// diagnosed as moved and its dead registrations kept forever -- the opposite
// failure, and a silent one.
//
// The MODE, not the longest common prefix. Records outside the store
// (`/usr/bin`, `~/.cargo/bin` -- 7 of 2908 on the measured home) would drag a
// common prefix down to `/` and make every path "under" it.
//
// And the old root must be GONE, or be this same home under another name. A
// root that still exists as a home of its own is not a move -- see
// SamePlaceProbe.
std::optional<HomeRelocation> detect_relocation(const VersionDB& db,
                                                std::string_view homeDir,
                                                const PathProbe& exists,
                                                const SamePlaceProbe& samePlace);

}  // namespace xlings::xvm
