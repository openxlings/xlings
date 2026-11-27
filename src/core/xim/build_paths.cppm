export module xlings.core.xim.build_paths;

import std;

// Rule P at install (design 2026-11-27 §8.2): a published payload names no
// path of the machine it was built on -- a build's temporary directory,
// another home -- in what a consumer reads to find files: an ELF's DT_RPATH or
// DT_RUNPATH, a pkg-config file, a libtool archive. Install reports it as a
// finding (the index refuses it at publication); what a recipe relocates at
// install points into this home and is fine.
//
// A path is the build machine's when it is absolute and under none of: the
// payload, this home and its stores, the operating system's directories, the
// reserved placeholder a build pads its prefix with (/nonexistent/...). A
// value with a variable left in it ($ORIGIN, ${prefix}) is not judged.
export namespace xlings::xim::build_paths {

struct Found {
    std::string file;   // the payload's file that names it
    std::string path;   // the path it names
};

struct Report {
    std::size_t files { 0 };          // files that name one
    std::vector<Found> examples;      // the first few
};

Report scan(const std::filesystem::path& payload, const std::vector<std::filesystem::path>& allowed);

// One finding's text: what, where, how many more.
std::string describe(const Report& report, const std::filesystem::path& payload);

}  // namespace xlings::xim::build_paths
