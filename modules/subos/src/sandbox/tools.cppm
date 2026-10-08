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
// (empty: none does); `install` is the command that brings the payload.
struct Tool {
    std::string_view name;
    std::string_view package;
};
std::span<const Tool> known();

// Every candidate for `tool`, in the table's order, that exists. Nothing is
// probed: whether a found binary WORKS is the caller's question (caps probes
// bwrap). An unknown name is a programming error and finds nothing.
std::vector<Found> candidates(std::string_view tool, const HomeView& home, const Ports& ports);
std::optional<Found> first(std::string_view tool, const HomeView& home, const Ports& ports);

// "xlings install <package>" or empty.
std::string install_hint(std::string_view tool);

}  // namespace xlings::subos::tools
