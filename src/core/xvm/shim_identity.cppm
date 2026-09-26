// What a file in a subos bin directory IS -- the one answer.
//
// A shim used to be "ours" when it was the same file object as the entry
// binary, and "current" for the same reason. POSIX shims are symlinks to the
// entry's PATH, so both answers were right there by accident. Windows shims
// are hard links to the entry's FILE OBJECT, and every upgrade replaces that
// object on purpose (a running image cannot be overwritten in place) -- so
// after `self update` every shim in the home was, at once, "not ours" and left
// running the previous client forever (#615).
//
// The two questions are answered from two different facts here:
//
//   is it ours     the file IS an xlings build -- it carries
//                  `kMulticallMarker`, or (builds before this module) the
//                  legacy fingerprint. What it links to is irrelevant.
//   is it current  it is the entry's file object, or byte-identical to it
//                  (the copy fallback, on a filesystem without hard links).
//
// A file that cannot be read is Unknown, never Stale: "could not read" is not
// evidence of anything, and a Stale file is one xlings will replace.
//
// Pure of Config on purpose, like shim_table: the whole decision is reachable
// from a unit test with a temp directory.
//
// See .agents/docs/2026-09-26-issue615-shim-identity-design.md.
export module xlings.core.xvm.shim_identity;

import std;

import xlings.platform;

export namespace xlings::xvm {

namespace fs = std::filesystem;

// Embedded in every xlings build that contains this module. Its presence is
// the proof that a file is an xlings multicall binary, whatever version, and
// that it performs the startup handoff (`caps=handoff`), so a stale copy of it
// still runs the entry's code.
//
// Never reword it: a build is recognised by these exact bytes. A future ABI
// adds a second marker rather than changing this one.
inline constexpr std::string_view kMulticallMarker =
    "xlings:multicall-shim:abi=1:caps=handoff:f615c0de";

enum class ShimState {
    Current,   // the entry binary's file object, or byte-identical to it
    Stale,     // an xlings build, but not the entry's -- runs older code
    Foreign,   // not an xlings build: somebody's real program; never touched
    Unknown,   // could not be read; never touched, reported
};

struct ShimIdentity {
    ShimState state { ShimState::Foreign };
    // Stale only: the file carries `kMulticallMarker`, so running it hands
    // off to the entry anyway. False for a legacy build, which runs its own
    // (old) dispatcher -- the state #615 reported.
    bool handoffCapable { false };
};

// One classification pass. Holds a cache keyed by FILE OBJECT, not by path:
// a home's stale shims are typically forty hard links to one old file, and
// reading that file forty times is the cost this avoids.
class ShimClassifier {
public:
    explicit ShimClassifier(fs::path entryBinary);

    ShimIdentity classify(const fs::path& path);

    [[nodiscard]] const fs::path& entry() const { return entry_; }

private:
    enum class Content { Unreadable, NotXlings, Legacy, Marked };

    Content content_of_(const fs::path& path, const platform::FileIdentity& id);
    bool same_bytes_as_entry_(const fs::path& path,
                              const platform::FileIdentity& id);

    fs::path entry_;
    std::optional<platform::FileIdentity> entryId_;
    std::uintmax_t entrySize_ { 0 };
    std::map<platform::FileIdentity, Content> content_;
    std::map<platform::FileIdentity, bool> sameAsEntry_;
};

// Byte-for-byte equality of two regular files. False when either cannot be
// read -- a caller deciding whether to skip a write must not skip it on a
// read error.
bool same_bytes(const fs::path& a, const fs::path& b);

} // namespace xlings::xvm
