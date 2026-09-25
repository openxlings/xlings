export module xlings.core.subos.userdata;

import std;
import xlings.core.confirm;
import xlings.core.destructive_log;

// SubOS user data.
//
// The rule (AGENTS.md, "SubOS user data"): what a reinstall can put back --
// payload links, shims, generations -- any flow may delete. A SubOS's home and
// every other file in it that xlings cannot prove it owns only a deletion the
// USER initiated may remove, and only after they confirmed (a terminal answer,
// `-y`, or `"yes": true`). The one deletion entry point below takes a
// confirm::UserConfirmed, which nothing but confirm::ask() can produce.
//
// Its own module because two sides need it that cannot import each other:
// `subos remove` (xlings.core.subos) and `self install` / `self uninstall`
// (xlings.core.xself, which xlings.core.subos imports).
export namespace xlings::subos::userdata {

struct Census {
    destructive_log::Size home;       // home/ and a home.img
    std::uintmax_t otherFiles { 0 };  // regular files outside them, not links into packages
};

// What deleting `dir` takes that nothing can put back.
[[nodiscard]] Census census(const std::filesystem::path& dir);

// "home 2.5 GB in 30256 file(s), plus 12 other file(s) outside home/"
[[nodiscard]] std::string describe(const Census& c);

// Delete one SubOS directory. Refused unless `dir` is a direct child of
// `<XLINGS_HOME>/subos` (never the root, never `current`, never a symlink),
// and on Linux while anything is mounted under it. Recorded in the
// destructive log with how it was confirmed.
[[nodiscard]] std::expected<void, std::string>
delete_subos(const std::filesystem::path& dir, std::string_view op,
             const confirm::UserConfirmed& confirmed);

// The hint for a refusal that lacked confirmation, worded for who asked:
// an agent ("yes:true") is told the decision is the user's.
[[nodiscard]] std::string needs_confirmation_hint(std::string_view yesSpelling,
                                                  std::string_view capability);

}  // namespace xlings::subos::userdata
