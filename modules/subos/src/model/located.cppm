export module xlings.subos.located;

import std;
import xlings.subos.home_view;

// Where an instance's own files are. A Luban system lives in a prefix domain
// (part 3 §3.4): the owner home holds its control record, and its tree, its
// workspace, its generations and its sessions live in the domain's private
// home, seen inside as `logical` (/xlings). An instance outside a domain is
// where it always was. Every reader that looks at an instance's files asks
// this, so none of them has its own idea of where a system is.
export namespace xlings::subos {

struct Located {
    HomeView home;                  // the home whose subos/<name> is the instance
    std::filesystem::path logical;  // that home as the instance sees it
    bool domain { false };
};

// Unreadable or absent records answer "here": a broken control record is
// reported by the readers that need it, not guessed around.
Located locate(const HomeView& owner, std::string_view name);

}  // namespace xlings::subos
