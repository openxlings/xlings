// The two loader fields, read in this process instead of by forking patchelf.
//
// WHY THIS FILE EXISTS
//
// `elfcheck::scan_payload` needs exactly two things per file: which interpreter
// the binary asks for (PT_INTERP) and which directories it will search for
// libraries (DT_RUNPATH, or DT_RPATH when there is no RUNPATH). Both are a few
// dozen bytes near the front of the file.
//
// Reading them by spawning `patchelf` cost two processes per ELF -- through
// `popen`, so a shell each -- and patchelf reads the WHOLE file to answer.
// Measured on a 363-payload / 73 GB home: 13,729 ELFs, 15,008 patchelf
// invocations, 56.54 GB read, and `xlings self doctor --deep` took 2m3s with
// over half of it in the kernel. `--fix` runs that audit up to eight times.
//
// The same walk done with header reads takes seconds, because the answer never
// required reading the file: the program header table says where PT_INTERP is,
// and only 1,279 of those 13,729 files have one at all. The other 12,450 are
// shared libraries and static binaries, for which the correct answer is "no
// interpreter" and the old path still read every byte to find that out.
//
// EQUIVALENCE IS THE CONTRACT, NOT THE SPEED
//
// This replaces a reader whose results a repair acts on, so "faster" is worth
// nothing if it is not the same answer. Two behaviours of `patchelf` are
// reproduced deliberately and are pinned by tests:
//
//   * `--print-rpath` prints DT_RUNPATH when the tag is present and DT_RPATH
//     otherwise -- not both, and not concatenated. Measured against real
//     payloads in this home: node carries RUNPATH, godot carries RPATH, and
//     patchelf prints each one's own tag.
//   * `--print-interpreter` FAILS on a file with no `.interp`, and the caller
//     turned a failed run into an empty string. So "static or shared library"
//     and "unreadable" arrived as the same value, and both meant "skip". An
//     empty interpreter here means the same thing.
//
// `patchelf` is therefore no longer a dependency of the audit. It stays in the
// tests as the ORACLE: tests/unit/test_elfread.cpp runs both readers over the
// same files and requires them to agree. That inverts the old risk -- the tool
// used to be something the audit needed installed to work at all, and its
// absence silently produced an empty scan.
export module xlings.core.elfread;

import std;

export namespace xlings::elfread {

// The fields the loader itself would read.
struct DynamicInfo {
    // PT_INTERP. Empty for a shared library or a static executable -- neither
    // chooses a loader, and neither is a defect.
    std::string interpreter;

    // DT_RUNPATH if the tag is present, otherwise DT_RPATH. Already split on
    // ':' with empty components dropped, which is what the caller wanted from
    // patchelf's colon-joined line.
    std::vector<std::string> searchPaths;

    // Which tag supplied `searchPaths`. Not used by the same-source check, but
    // the distinction is load-bearing elsewhere in this repo (DT_RUNPATH does
    // not apply to a dependency's own dependencies, DT_RPATH does), so reading
    // it and discarding it would be throwing away the interesting half.
    bool fromRunpath { false };
};

// True when `file` begins with the ELF magic. The cheap gate: a payload is
// mostly headers, scripts and data, and this rejects those with a 4-byte read.
bool is_elf(const std::filesystem::path& file);

// The two fields, or nullopt when the file is not an ELF this can parse.
//
// nullopt and a DynamicInfo with an empty interpreter are DIFFERENT: the first
// means "not something we read", the second means "read, and it has no
// interpreter". The audit treats both as "nothing to pair", but a caller that
// wants to report unreadable files can tell them apart.
std::optional<DynamicInfo> read(const std::filesystem::path& file);

}  // namespace xlings::elfread
