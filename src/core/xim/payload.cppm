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

// The fingerprint a hookless install used to leave when it staged from the
// shared download directory instead of its own archive (mcpp-community/
// mcpp#636, fixed by xpkg-manifest-v1 §6 / installer.cpp's private
// extraction): a download-cache sidecar, or another package's archive,
// sitting at the top level of what should be one package's own files.
//
// Anchored on the lock file, not on any filename by itself: a package may
// legitimately ship its OWN top-level "setup.exe", "data.zip" or
// "notes.meta" as part of its own archive, and none of those names alone
// is evidence of anything. What only the downloader ever writes, and only
// inside runtimedir (downloader.cpp), is a zero-length "<name>.lock" held
// for the download's duration -- a genuine payload has no reason to carry
// one. So every candidate is a zero-length regular file "<name>.lock",
// and it counts as the fingerprint only when one of three further, non-
// recursive facts about "<name>" also holds (measured shapes, any one
// sufficient):
//
//   * "<name>" exists as a sibling here too -- the archive beside its own
//     lock, e.g. "hooked.tar.gz" + "hooked.tar.gz.lock";
//   * "<name>" itself ends in one of the extensions the downloader names
//     its destination file with -- true even when the archive side was
//     already moved or the download never finished, e.g. an orphan
//     "glibc-2.44.2-linux-x86_64.tar.gz.lock";
//   * "<name>.meta" exists as a sibling -- the downloader's per-file
//     sidecar, corroborating a non-archive download such as
//     "LICENSE.TXT" + "LICENSE.TXT.lock".
//
// Returns the lock file's own name (the evidence), or "" when the top
// level carries no zero-length lock file satisfying any of the three.
std::string swept_payload_marker(const std::filesystem::path& dir);

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

// The packaging revision the install that wrote this stamp recorded
// (openxlings/xlings#620).
//
// A recipe may state `revision = N` on a version entry: a change to what it
// installs that keeps the upstream version. The stamp records the revision
// the payload was built from, and `payload_revision_verdict`
// (install_state.cppm) compares the two.
//
// Three answers, and the difference between the last two is deliberate:
//   * no stamp at all            -> kRevisionUnrecorded: no observation, and
//                                   so no verdict (a pre-stamp payload is not
//                                   reinstalled on this account);
//   * a stamp without the field  -> 0: the install happened, before revisions
//                                   existed, and a recipe that states none is
//                                   revision 0. A recipe that states 1 or more
//                                   therefore reaches every older payload;
//   * the field                  -> its value.
constexpr int kRevisionUnrecorded = -1;

int stamped_revision(const std::filesystem::path& dir);

// Record what this platform installed, so the next run does not have to guess.
//
// `registered` is the count of xvm nodes the install produced. It is what
// makes "the install finished" checkable instead of merely asserted: a stamp
// alone says a run reached the end, which a run that registered nothing also
// does. Measured on a real home: 29 payloads carried a stamp, reported
// `installed`, and had no ledger entry at all -- the whole graphics stack
// among them. See .agents/docs/2026-08-11-five-issues-triage-and-plan.md.
//
// `revision` is the recipe's packaging revision for this version; see
// `stamped_revision`. It is always written, 0 included, so a stamp from this
// client never relies on the absent-field reading.
void write_payload_stamp(const std::filesystem::path& dir,
                         std::string_view version,
                         int registered = kRegisteredUnrecorded,
                         int revision = 0);

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

// ── replacing a payload that is installed but stale ──────────────────
//
// A payload whose recipe revision moved on (#620) is replaced, and the
// replacement must never leave a window in which the version has no payload:
// a failed reinstall that had already deleted the old tree turns "outdated"
// into "absent", for a package other payloads may link against.
//
// The new payload is installed into the REAL path, never into a temporary
// directory renamed afterwards: recipes write install_dir() into the files
// they install (gcc's specs, pkg-config files, rpaths), so a payload built
// anywhere else names a directory that does not hold it. The old tree is
// moved out of the way instead, and that move is what this type owns:
//
//   set_aside_payload   renames the old tree to its parking place and leaves
//                       an empty directory at the real path;
//   commit              the reinstall succeeded: the parked tree is deleted;
//   rollback            it did not: the partial tree is deleted and the
//                       parked one renamed back. The destructor does the same
//                       when neither was called, so every early exit of an
//                       install step restores the old payload.
//
// Files another tool wrote into the old tree (mcpp's `.mcpp_ok` and
// `.mcpp-fixup.json`) are deleted with it on commit. They describe the old
// payload, and carrying them over would assert them of the new one.
//
// The parking place is `<data>/stale/`, beside `xpkgs` and so on the same
// filesystem. Not a sibling of the version directory: every reader of a store
// takes each subdirectory of `xpkgs/<pkg>/` for a version (see
// `payload_trash_root`). Not the trash root either: `remove` sweeps it, and a
// recipe may request a removal between its install and config hooks, which
// is inside this window.
class PayloadReplacement {
public:
    PayloadReplacement(std::filesystem::path payloadDir,
                       std::filesystem::path parkedDir);
    PayloadReplacement(PayloadReplacement&& other) noexcept;
    PayloadReplacement& operator=(PayloadReplacement&&) = delete;
    PayloadReplacement(const PayloadReplacement&) = delete;
    PayloadReplacement& operator=(const PayloadReplacement&) = delete;
    ~PayloadReplacement();

    [[nodiscard]] const std::filesystem::path& parked() const;

    void commit();

    // Empty when the old payload is back at its path; otherwise a sentence
    // naming where it was left.
    std::string rollback();

private:
    std::filesystem::path payloadDir_;
    std::filesystem::path parkedDir_;
    bool settled_ { false };
};

// Where `payloadDir` is parked while it is replaced: under `<data>/stale/`
// for a payload inside an `xpkgs` store, a `.stale-<pid>` sibling otherwise.
// Unique within this process.
std::filesystem::path stale_payload_parking(const std::filesystem::path& payloadDir);

// Moves the payload aside and recreates its directory empty. Refused, with the
// payload untouched, when the move is not possible (a held file on Windows, a
// store split across filesystems): a reinstall that cannot keep the old
// payload recoverable does not start.
std::expected<PayloadReplacement, std::string>
set_aside_payload(const std::filesystem::path& payloadDir);

// ── recovering a parked payload after a crash ─────────────────────────
//
// PayloadReplacement's rollback is a destructor: it runs when the process
// that called `set_aside_payload` unwinds, by any path, without `commit()`.
// A kill (SIGKILL, power loss) between the rename in `set_aside_payload` and
// that destructor never runs it, and nothing else ever looks under
// `<data>/stale/` again -- `sweep_payload_trash` walks a different root
// (`<data>/trash`, `payload_trash_root`), for a different leftover
// (something `remove` displaced, not something an install parked). Every
// crash at that point leaks one directory, forever.
//
// The fix is a record written before the fact, because there is nothing to
// read after the fact: once the process is gone, the only thing that still
// says a parked directory was ever meant to go somewhere is whatever was
// written down while it still could be. `write_parked_payload_marker` is
// called from `set_aside_payload`, before the rename that creates the
// parked directory, so a crash in the smallest possible window after it
// still leaves the marker findable. It is written atomically
// (`platform::write_file_atomic`: temp file + rename), so a crash WHILE
// writing it leaves either the old marker (absent, the first time) or the
// new one -- never a torn file a later reader could misparse as valid.
struct ParkedPayloadMarker {
    std::filesystem::path payloadDir;  // where the parked directory came from
    int pid { 0 };                     // the process that parked it
};

// The marker's path for a given parked directory: a sibling of it (never a
// child), so writing or removing the marker is never mistaken for a change
// to the parked payload itself, and the marker survives exactly as long as
// the question it answers is still open.
std::filesystem::path parked_marker_path(const std::filesystem::path& parkedDir);

// Written before `parkedDir` exists. `pid` is the parking process's own,
// e.g. `platform::get_pid()`.
void write_parked_payload_marker(const std::filesystem::path& parkedDir,
                                 const std::filesystem::path& payloadDir,
                                 int pid);

// `std::nullopt` when there is no marker, or what is there cannot be read
// as one -- treated identically by `recover_parked_payloads`: nothing here
// says it is safe to touch a directory whose marker cannot be read.
std::optional<ParkedPayloadMarker> read_parked_payload_marker(
    const std::filesystem::path& parkedDir);

// Idempotent: removing an already-absent marker is not an error.
void remove_parked_payload_marker(const std::filesystem::path& parkedDir);

// Finishes what a crashed reinstall left parked under `staleRoot`
// (`<data>/stale`), for every entry whose marker names a process that is no
// longer running:
//
//   * the original payload path is missing, or is the empty directory
//     `set_aside_payload` recreates right after parking the old one --
//     nothing replaced it, so the parked payload moves back;
//   * anything else is already at the original path -- a later install
//     that finished cleanly, or a leftover from the SAME interrupted
//     replacement that this same rule already discarded once -- so the
//     parked payload and its marker are removed rather than guessed about
//     a second time.
//
// An entry with no readable marker, or whose owning process is still alive,
// is left exactly as found: the first because nothing says it is ours to
// touch, the second because that process may yet run its own rollback.
//
// Meant to run once per command, at the point installation first touches
// the store -- not once per package, and not on a dry run, which never
// touches the store at all. Returns how many entries were resolved, so a
// caller can log a count instead of nothing.
int recover_parked_payloads(const std::filesystem::path& staleRoot);

}  // namespace xlings::xim
