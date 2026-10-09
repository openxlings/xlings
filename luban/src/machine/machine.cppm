export module luban.machine;

import std;

// A machine's own /etc (SubOS design part 2 §6.4, part 3 §8): what a root
// needs when it is a machine -- factory files linked in where the machine has
// none, users and groups its packages declare. Machine state is user data:
// nothing that exists is replaced.
export namespace luban::machine {

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

}  // namespace luban::machine
