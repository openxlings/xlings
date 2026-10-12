// The first command of a new client on a home: the upgrade, done once.
//
// What other package managers and long-lived tools converge on (Debian's
// maintainer scripts, Android's onUpgrade, database migrations): the new
// version brings its own ordered, idempotent steps; they run by themselves
// the first time it meets old state; they are recorded; and what cannot be
// done by itself is said once, with the exact command, never as "something
// is wrong, run the doctor". Each step is its own failure domain: one that
// fails becomes a pending item and stops nothing else -- neither the other
// steps nor the command the user asked for.
//
// Design and spec: .agents/docs/2026-10-12-seamless-upgrade-design.md,
// docs/specs/upgrade-and-compatibility.md.
export module xlings.core.xself.upgrade;

import std;

export namespace xlings::xself::upgrade {

struct Pending {
    std::string what;
    std::string command;
};

struct Outcome {
    std::string from;
    std::string to;
    // This client is OLDER than the one that last upgraded the home: nothing
    // is changed, and the way back to the newer one is said.
    bool downgrade { false };
    std::vector<Pending> pending;
    // The version it came from is still installed, so `xlings use xlings
    // <from>` goes back to it.
    bool canGoBack { false };
};

// nullopt when there is nothing to do or say: the home was already upgraded
// to this version, it is a fresh home, or this runs inside a SubOS session
// (the owner's home is the owner's to upgrade). Never throws.
std::optional<Outcome> on_first_command();

}  // namespace xlings::xself::upgrade
