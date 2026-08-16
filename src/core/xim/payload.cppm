export module xlings.core.xim.payload;

import std;

import xlings.core.config;

export namespace xlings::xim {

// ── payload platform identity ────────────────────────────────────────
//
// "Already installed" used to mean "the directory exists and is not empty".
// A payload left behind by a run that targeted ANOTHER platform passes that
// test perfectly, so the install hook -- the only code that unpacks the right
// tarball -- was skipped, while the config hook ran and registered whatever
// was lying there. The measured case: a May-era Windows llvm@20.1.7 in a
// Linux store, which registered `clang.exe` … `libomp.dll` as programs, then
// warned six times that `cc -> clang` could not be found, and reported
// success. The payload is stamped on install so the question is answerable;
// payloads installed before the stamp existed are classified by magic number.
enum class PayloadPlatform {
    Host,      // provably this platform
    Foreign,   // provably some other platform
    Unknown,   // nothing conclusive -- scripts, data, empty
};

constexpr std::string_view kPayloadStampFile = ".xpkg-install.json";

std::string_view host_platform_tag();

// Classify one executable by its first bytes. Returns "" when unrecognized.
std::string_view executable_format_(const std::filesystem::path& file);

// Whether the payload holds anything besides our own bookkeeping.
//
// The emptiness probe ("directory exists and is not empty") is what lets a
// broken payload be reinstalled at all, and a stamp file would satisfy it on
// its own -- turning the record of an install into evidence of one. The
// `.xim-installed` marker is deliberately NOT excluded: wrapper packages
// whose real payload lives in a dependency use it to mean exactly "installed,
// nothing here".
bool payload_has_content(const std::filesystem::path& dir);

// What the FILES say, ignoring any stamp. Separate from the function below
// because "should this payload be stamped" must never be answered by reading
// the stamp: a run that wrote one over a payload it did not actually install
// would then confirm its own claim forever.
PayloadPlatform classify_payload_content(const std::filesystem::path& dir);

PayloadPlatform classify_payload_platform(const std::filesystem::path& dir);

// How many xvm nodes the install that wrote this stamp registered.
//
// `-1` means the stamp predates the field, and that is NOT the same fact as
// zero. Zero is a package DECLARING it registers nothing -- wrapper and meta
// packages legitimately do -- and can be checked against the ledger. Absent is
// an install we never observed, and a check that treats it as zero would
// declare something about a run that recorded nothing. A payload whose stamp
// cannot say what it registered gets no verdict, not a convenient default.
constexpr int kRegisteredUnrecorded = -1;

// Whether the last install of this payload is known to have failed.
//
// A failure has to leave a record of ITSELF, because the alternative -- infer
// it from the payload -- cannot work. "No stamp" was the obvious candidate and
// it is wrong: measured on a real home, the payloads with no stamp are old
// installs written before the stamp existed (`linux-headers` with a full
// `include/` tree, twenty-five superseded `xlings` versions), not failures.
// Treating those as incomplete would reinstall them forever, which is the
// non-convergence this repo already has a postmortem about.
//
// So the moment we know -- the hook returned false -- we write it down.
bool stamped_incomplete(const std::filesystem::path& dir);

int stamped_registration_count(const std::filesystem::path& dir);

// Record what this platform installed, so the next run does not have to guess.
//
// `registered` is the count of xvm nodes the install produced. It is what
// makes "the install finished" checkable instead of merely asserted: a stamp
// alone says a run reached the end, which a run that registered nothing also
// does. Measured on a real home: 29 payloads carried a stamp, reported
// `installed`, and had no ledger entry at all -- the whole graphics stack
// among them. See .agents/docs/2026-08-11-five-issues-triage-and-plan.md.
void write_payload_stamp(const std::filesystem::path& dir,
                         std::string_view version,
                         int registered = kRegisteredUnrecorded);

// Mark this payload as the residue of an install that did not finish.
//
// Deliberately written into the stamp file and not a new one: the stamp is the
// single artifact every reader already consults, and `payload_has_content`
// ignores it by name -- so recording a failure never makes an empty directory
// look like a payload, which is the exact confusion that made the failure
// unrecoverable in the first place.
//
// Unlike write_payload_stamp this does NOT skip an empty directory. A hook
// that failed before unpacking anything is precisely the case that must be
// retryable, and it is the case that leaves nothing behind.
void write_payload_failure_marker(const std::filesystem::path& dir,
                                  std::string_view version,
                                  std::string_view reason);

// ── removing a payload something may be holding ──────────────────────
//
// Windows, and only Windows, has three ways to refuse a delete:
//
//   read-only attributes   payloads come out of .vsix/.msi archives that
//                          carry the bit; POSIX only needs the DIRECTORY
//                          writable to unlink a child, so this never shows
//                          up on Linux or macOS.
//   an open FILE           `cl.exe` leaves `vctip.exe` and `mspdbsrv.exe`
//                          running INSIDE the toolset it was launched from,
//                          for tens of seconds after it exits. (Visible in
//                          our own Windows CI: "Terminate orphan process:
//                          pid (8696) (vctip)".)
//   an open DIRECTORY      a process whose current directory is in the tree.
//
// The first is cleared, the second is worked around by MOVING the files
// (renaming an open file is allowed on Windows -- that is how an updater
// replaces a running .exe -- while deleting it is not), and the third is
// accepted: a payload with no files left is not installed, which is what
// uninstall promises.
//
// Lives here rather than in installer.cpp for two reasons that turned out to
// be the same reason: `xlings subos remove` and `xlings self uninstall` have
// the identical problem, and a function in a .cpp's anonymous namespace
// cannot be unit tested -- which is why its rollback path shipped inverted.
//
// Two outcomes, because there is no third one to have.
//
// "Roll back to untouched" is not reachable and never was. The fast path is
// `remove_all`, which is not atomic: on a tree where one file is held it
// deletes everything it can reach and then reports failure, so by the time we
// know something is holding a file, other files are already gone. Probing
// first -- trying a delete to find the holder -- makes the diagnosis a second
// act of destruction, which is the shape this is trying to avoid.
//
// So the question is not "did we damage it" (we did, that was the request)
// but "will the NEXT install repair it". A leftover payload is dangerous for
// exactly one reason: `payload_has_content` is true, so the package reads as
// installed and a reinstall adopts the wreckage instead of replacing it.
// `Partial` therefore stamps the directory incomplete, which install_state
// checks before anything else -- the same marker a failed install leaves.
enum class RemoveOutcome {
    Removed,   // no regular files left; the package is not installed
    Partial,   // some files could not be removed -- stamped incomplete so
               // that `xlings install` rebuilds it rather than adopting it
};

// Where displaced files are parked, derived from the store the payload lives
// in.
//
// Two constraints, and together they leave exactly one place:
//   * same filesystem — the whole strategy is `rename`, and a cross-device
//     rename fails;
//   * outside `xpkgs` — seven places in this tree read every subdirectory of
//     `xpkgs/<pkg>/` as a version and every subdirectory of `xpkgs/` as a
//     package, and NOT ONE of them skips dotfiles. A `.trash-22.17.1` beside
//     the version directories becomes a version called `.trash-22.17.1` in
//     `xlings list`, in doctor's payload audit and in the reference count.
//
// So: `<...>/data/trash`, a sibling of `<...>/data/xpkgs`. Empty when the
// payload is not inside a store, in which case the move strategy is skipped
// rather than guessing a location something else reads as content.
std::filesystem::path payload_trash_root(const std::filesystem::path& payloadDir);

// Try hard to remove a payload directory. See RemoveOutcome.
//
// `version` is only used to stamp the leftovers on a Partial outcome; pass
// what the caller is uninstalling.
RemoveOutcome remove_payload_dir(const std::filesystem::path& root,
                                 std::string_view version = {});

// The verdict, once every removal strategy has been tried: Removed when no
// regular files are left, Partial otherwise -- and Partial STAMPS, which is
// the part that matters.
//
// Separated from `remove_payload_dir` because it is the only half of this
// that can be tested off Windows. Nothing in a tree we own can resist us on
// POSIX: `clear_readonly_` chmods any directory back to writable, and an open
// file descriptor does not prevent unlink -- so the Partial BRANCH is
// genuinely unreachable on Linux and macOS, and a test that claimed to cover
// it would be asserting on a path it never entered. This function is real
// production code driven with real leftovers, not a stub standing in for one.
RemoveOutcome settle_removal(const std::filesystem::path& root,
                             std::string_view version);

// Clear whatever earlier removals had to park. Cheap, idempotent, and the
// answer to "removed on a later run" -- which the first version of this code
// promised without anything anywhere doing it.
//
// Returns how many entries are still held, so a caller can say so instead of
// implying the store is clean.
int sweep_payload_trash(const std::filesystem::path& trashRoot);

}  // namespace xlings::xim
