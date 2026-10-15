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

// Users of a root (part 3 §6.1): its passwd answers who logs in.
struct User {
    std::string name;
    unsigned uid { 0 };
    unsigned gid { 0 };
    std::string home;
    std::string shell;
};
// A name `useradd` would take: [a-z_][a-z0-9_-]*, at most 32.
bool valid_user_name(std::string_view name);
std::optional<User> find_user(const fs::path& etc, std::string_view name);
// A login user: a passwd line (the first free uid from 1000, its own group),
// membership in wheel, a shadow line (`hash`, or none: logs in without one),
// and its home under `tree`. An existing name is an error.
// `prefer`: the uid to take when it is free and at least 1000 -- the host
// user's, so a machine's 9p share (which shows the host's owners) lets the
// user write its own home.
std::expected<User, std::string> add_user(const fs::path& tree, std::string_view name, std::string_view shell,
                                          std::string_view hash, std::optional<unsigned> prefer = std::nullopt);
// Replaces `name`'s shadow hash ("" = no password).
std::expected<void, std::string> set_password(const fs::path& etc, std::string_view name, std::string_view hash);
// Who `luban enter` logs in as: etc/luban/login, else root. Machine state.
std::string login_user(const fs::path& etc);
std::expected<void, std::string> set_login_user(const fs::path& etc, std::string_view name);

// Who a root is to its own processes (part 3 §6.2): its /etc/hostname, unless
// that is empty or the edition's placeholder `luban` -- then the system's name.
std::string hostname(const fs::path& etc, std::string_view name);

// Where you are, in the prompt (part 3 §7.2): `[luban]` for a system that
// shares the host's kernel, `<luban>` for one with its own, `:temp` / `:home`
// when it keeps less than everything, then `user@host:dir$`. ANSI's 16
// colours only, and only on a terminal known to show them (`term`); never
// when `no_color`. A PS1 sh, bash and busybox read.
std::string prompt(std::string_view kind, std::string_view persist, std::string_view term, bool no_color);

// The same, for a login shell and after `su -`: etc/profile.d/luban.sh, from
// LUBAN_KIND and LUBAN_PERSIST, written when missing (machine state: an edit
// stays). Returns whether it was written.
std::expected<bool, std::string> ensure_prompt_script(const fs::path& etc);
// A regular user's own packages before the system's (design 2026-10-16 L
// §8): profile.d/xlings-user.sh and fish/conf.d/xlings-user.fish, written
// once. The ones written, relative to etc.
std::expected<std::vector<std::string>, std::string> ensure_user_layer_scripts(const fs::path& etc);

}  // namespace xlings::subos::root_etc
