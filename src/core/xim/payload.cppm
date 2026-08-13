export module xlings.core.xim.payload;

import std;

import xlings.platform;
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

}  // namespace xlings::xim
