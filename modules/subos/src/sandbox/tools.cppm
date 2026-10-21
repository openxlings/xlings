export module xlings.subos.tools;

import std;
import xlings.subos.home_view;
import xlings.subos.ports;

// Every external program the SubOS code runs, resolved in one place
// (SubOS design part 3 §6.4).
//
//   in-process  what xlings links: libarchive writes tarballs, the platform
//               layer enters namespaces -- such a tool is not in this table
//   payload     a package xlings installed into the home (data/xpkgs), pinned
//               by the index's sha256: what xlings ships for itself
//   host        a program the machine has, found only at the paths named
//               here, never an xlings shim (running another home's shim would
//               move the work into that home), and reported as the host's
//
// The table is the policy; nothing else spells a tool's path. xlings never
// installs a host tool through the host's package manager.
export namespace xlings::subos::tools {

namespace fs = std::filesystem;

enum class Source { RootOwned, Payload, Runtimedir, Host };
std::string_view to_string(Source s);

struct Found {
    std::string tool;
    fs::path bin;
    Source source { Source::Host };
};

// The tools this table knows. `package` is the xim package that provides it
// -- every tool has one, except what the operating system itself is: `os`
// says which and why (T0). `platform` limits a tool to one platform
// ("linux", "macos", "windows", or "linux-x86_64" for a package built for
// that architecture alone; empty: wherever it is asked for), and
// `planned` names the requirement (tests/requirements.toml) that tracks a
// package not yet in the index. tests/scripts/test_tools_provenance.py holds the table to the
// index: each package exists, for each architecture xlings is released for.
struct Tool {
    std::string_view name;
    std::string_view package;
    std::string_view os = {};
    std::string_view platform = {};
    std::string_view planned = {};
};
std::span<const Tool> known();

// Every candidate for `tool`, in the table's order, that exists. Nothing is
// probed: whether a found binary WORKS is the caller's question (caps probes
// bwrap). An unknown name is a programming error and finds nothing.
std::vector<Found> candidates(std::string_view tool, const HomeView& home, const Ports& ports);
std::optional<Found> first(std::string_view tool, const HomeView& home, const Ports& ports);

// "xlings install <package>" or empty.
std::string install_hint(std::string_view tool);

// A data file a package carries (firmware, boot files): <relative> under the
// newest installed version of xim:<package> in the home, if there is one.
std::optional<fs::path> payload_file(const HomeView& home, std::string_view package, const fs::path& relative);

}  // namespace xlings::subos::tools
