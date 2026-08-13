export module xlings.core.xim.compatibility;

import std;
import mcpplibs.xpkg;
import xlings.platform.target;

// Whether a package can be installed on this OS and architecture.
//
// The obvious source for that answer is `package.archs`, and it is the wrong
// one. `archs` is a PACKAGE-level union while the download resources are
// grouped BY OS: `go.lua` declares `archs = {"x86_64"}` and its macOS entry is
// a `darwin-arm64` tarball, because on macOS the only artifact anyone ships is
// arm64. Refusing on the union alone rejects the very archive the recipe was
// about to fetch.
//
// It is also unreliable in a way that is not evenly distributed. `archs` went
// unenforced for the whole of spec V1, so it is widely under-declared: of the
// 99 recipes in xim-pkgindex that set it, the great majority say `{"x86_64"}`
// and mean "this is the arch I happened to test". Enforcing that field against
// every recipe turns "we do not know" into "no".
//
// So the question asked here is not "what does the author claim" but "what
// does this resource entry actually carry":
//
//   Strong  the entry enumerates its architectures -- a per-arch map, per-arch
//           checksums, or per-arch upstream aliases. The author is describing
//           artifacts, not intentions, and a missing arch is a real answer.
//   Open    the entry substitutes ${arch} into a URL, or is an XLINGS_RES
//           resource. Arch-parametric by construction; nothing here can bound
//           the set, and the download reports the truth.
//   Weak    a single artifact, and `package.archs` is the only clue. A
//           mismatch is an advisory, never a refusal.
//   None    no claim at all.
//
// This reproduces the intent of the `spec >= 2` gate it replaces -- V2 authors
// are the ones who write per-arch resources -- but keys off a fact in the
// entry rather than a version number the author promised to honour. An index
// that upgrades a recipe to per-arch artifacts gets the strict check without
// anyone shipping a new client.
export namespace xlings::xim {

enum class ArchEvidence {
    None,    // package declares no archs and the entry carries none
    Weak,    // single artifact; package-level `archs` is the only signal
    Open,    // ${arch} template or XLINGS_RES: parametric, set unbounded
    Strong,  // entry enumerates the architectures it ships
};

struct TargetCompatibility {
    bool supported { true };
    ArchEvidence evidence { ArchEvidence::None };
    std::string target;                        // "<os>-<arch>"
    std::vector<std::string> supportedTargets;
    // Set when package-level `archs` does not list this arch but the evidence
    // is too weak to refuse. Surfaced once, never fatal.
    std::string advisory;
};

// The architecture PACKAGES must match: this process's ABI, not the machine's.
// An x86_64 build running under Rosetta has to install x86_64 artifacts,
// because those are what it can link against and exec. Kept under the old name
// so callers do not all move at once; `platform::Target` documents the split.
std::string host_architecture();

// The `xpm[os][version]` resource, with version aliases (`ref`) followed.
// nullptr when this OS or version has no entry -- the caller then has nothing
// but the package-level claim to go on.
const mcpplibs::xpkg::PlatformResource* find_entry(
    const mcpplibs::xpkg::Package& package,
    std::string_view os,
    std::string_view version);

// Architectures this entry names outright, canonicalised.
std::set<std::string> entry_architectures(
    const mcpplibs::xpkg::PlatformResource& entry);

bool is_arch_templated(const mcpplibs::xpkg::PlatformResource& entry);

ArchEvidence classify_arch_evidence(
    const mcpplibs::xpkg::Package& package,
    const mcpplibs::xpkg::PlatformResource* entry);

// `entry` is the resolved `xpm[os][version]` resource, with `ref` already
// followed. Pass nullptr when no entry could be selected -- the answer then
// falls back to the package-level claim, which can only ever warn.
TargetCompatibility check_target_compatibility(
    const mcpplibs::xpkg::Package& package,
    const mcpplibs::xpkg::PlatformResource* entry,
    std::string_view os,
    std::string_view arch);

std::string compatibility_error(std::string_view packageName,
                                const TargetCompatibility& compatibility);

}  // namespace xlings::xim
