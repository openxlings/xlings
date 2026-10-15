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
// reported by the readers that need it, not guessed around. A system on a
// Mac's vz carrier is where the Mac sees its host machine's tree (part 4
// §6.2): <home>/carriers/vz/root/xlings, /xlings inside, when attached.
Located locate(const HomeView& owner, std::string_view name);

// `p` (under the instance's physical home) as this host reaches it: a link
// on the way that names the logical home (what the instance sees, /xlings)
// is followed into the physical one. What a root's /usr links lead to, read
// from outside it.
std::filesystem::path physical(const Located& at, const std::filesystem::path& p);

}  // namespace xlings::subos
