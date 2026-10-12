export module xlings.subos.root_etc;

import std;

// A root's own /etc (SubOS design part 2 §6.4, part 3 §8): what a root
// needs as a system -- in a sandbox view or as a machine: factory files linked in where it has
// none, users and groups its packages declare. Machine state is user data:
// nothing that exists is replaced.
export namespace xlings::subos::root_etc {

namespace fs = std::filesystem;

// Factory /etc (design part 2 §6.4): every file under `factory` that `etc`
// does not have is linked to it -- a link the user may replace with a file of
// their own. Nothing that exists is touched. passwd/group are private regular
// copies for sysusers; directory symlinks and write/read failures are errors.
std::expected<std::vector<std::string>, std::string> fill_etc(const fs::path& etc, const fs::path& factory);

// A root's machine /etc from what its SubOS provides: the projection's
// usr/share/factory/etc, and the SubOS's sysroot /etc (certificates a package
// placed) minus the files that stand in for a host in a sandbox view.
// fill_etc's rule: only what is missing. Returns what it added.
std::expected<std::vector<std::string>, std::string> fill_machine_etc(const fs::path& etc, const fs::path& subos);

// sysusers.d (systemd's format, without systemd): `u name uid "gecos" home
// shell` and `g name gid` lines from `<usr>/lib/sysusers.d/*.conf`. Users and
// groups missing from etc/passwd and etc/group are appended; root always
// exists. Nothing is rewritten. Returns the names added.
std::expected<std::vector<std::string>, std::string> apply_sysusers(const fs::path& etc, const fs::path& usr);

// The login shell (Luban OS architecture design §7): the environment answers,
// not the command that enters it. A root made now gives root the best shell its
// /usr has (fish, then bash, else sh); `luban config <n> --shell` changes it;
// installing fish later does not (a distribution does not chsh for you).
std::string best_shell(const fs::path& usr);
// The shell `user`'s passwd entry names in this root, when the root has it;
// otherwise /bin/sh.
std::string login_shell(const fs::path& root, std::string_view user = "root");
// Rewrite `user`'s shell in etc/passwd -- the root's own regular file, never a
// link to a factory one.
std::expected<void, std::string> set_login_shell(const fs::path& etc, std::string_view user, std::string_view shell);

}  // namespace xlings::subos::root_etc
