export module xlings.core.xim.machine_rule;

import std;
import xlings.core.xvm.types;
import xlings.core.xvm.registration;

// Rule M (design 2026-11-27 §5.4): the files of a payload the host itself runs
// or loads are ELF files of the host's machine -- e_machine and the ELF class
// of the home's architecture (platform::build_arch()).
//
// The surface is what the host reaches without the recipe's help: the programs
// and libraries a registration publishes, the shared libraries directly in the
// runtime library directories the package exports (lib64 and lib when it
// exports none), and its exported loader. Anything else in the payload is the
// recipe's, and the rule says nothing about it: a cross toolchain's target
// libraries, a sysroot, prebuilt modules for several machines, firmware.
//
// Only ELF: macOS and Windows each have one supported architecture. Rule M is
// hard, without a switch -- a false positive is fixed in the recipe (a
// directory of another machine's files is not exported to the host), which an
// index publishes in minutes; a payload of the wrong machine registered on a
// host is what it replaces (an aarch64 host given x86_64 libraries, #1442).
export namespace xlings::xim::machine_rule {

struct Surface {
    std::vector<std::filesystem::path> files;    // read as they are
    std::vector<std::filesystem::path> libdirs;  // the files directly in them
};

struct Foreign {
    std::string file;
    std::string machine;   // its architecture's name, or "e_machine <n> (<bits>-bit)"
};

// The surface of a payload being installed: what `batch` registers (programs
// through the same resolution the shim uses, libraries at their path), and
// what the recipe exports, `loader` and `libdirs` relative to `payload`.
Surface of_registration(const std::filesystem::path& payload,
                        const xvm::RegistrationBatch& batch,
                        std::string_view loader,
                        const std::vector<std::string>& libdirs,
                        const std::string& home);

// The surface an installed release registers in `db`: its programs and
// libraries (what `self doctor` judges, design §5.4).
Surface of_database_entry(const xvm::VInfo& info, const xvm::VData& data,
                          const std::string& target, const std::string& home);

// The files of `surface` that are ELF of another machine than this host's.
// Empty when every one is the host's, and where the rule does not apply.
std::vector<Foreign> foreign(const Surface& surface);

// The host's machine, as the identity table names it.
std::string host_machine();

// `<pkg>: <file> is for <machine>, and this machine is <host>` -- up to three
// files, then how many more.
std::string describe(std::string_view package, const std::vector<Foreign>& files);

// What to do about it: the index's entry is what gives this machine the file.
std::string hint();

}  // namespace xlings::xim::machine_rule
