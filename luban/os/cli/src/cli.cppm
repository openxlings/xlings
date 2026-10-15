// luban: the Luban OS command (Luban OS architecture design §6).
//
// One noun -- an environment -- and the verbs on it, shown by level (help
// lists the common ones). The operations are luban.service's; this module is
// the command tree and the terminal's rendering of their events: one line per
// phase, the conclusion and the next step, `-v` for the steps, `--json` for the
// events themselves. A machine (an environment booted with its own kernel) is
// luban.vm's.
export module luban.cli;

import std;
import xlings.cli.model;
import xlings.libs.json;
import openas;
import luban.service;

export namespace luban::cli {

const xlings::cli::spec::CommandSpec& tree();

// Where this luban runs.
enum class Place { Host, Environment, Machine };
Place where();
std::string_view to_string(Place p);

// Where a Luban edition (a Linux root) runs from this host when no carrier is
// named: empty on Linux (here), wsl2 on Windows, vz on macOS.
std::string default_carrier();

// What a luban command line asks for, before anything runs: the command, its
// positional words and its options (`--x v`, `--x=v`, flags). A usage error is
// the message.
struct Parsed {
    std::string command;
    std::vector<std::string> words;
    std::vector<std::pair<std::string, std::string>> options;   // in order; a flag's value is ""
    std::vector<std::string> after;                             // after `--`
    std::optional<std::string> option(std::string_view name) const;
    bool flag(std::string_view name) const;
};
std::expected<Parsed, std::string> parse(std::span<const std::string> args);

// The luban command's version: Luban's (luban.service).
inline constexpr std::string_view kVersion = luban::service::kVersion;

// A drive `luban write` may write (Linux): what it is, read from sysfs and the
// mount table (parameters, so a test can hand it a made-up machine).
struct Drive {
    std::string name;            // sdb, nvme0n1, loop3
    std::filesystem::path dev;   // /dev/sdb
    std::uintmax_t bytes { 0 };
    std::string model;
    std::string serial;          // what --serial must say; the name when it reports none
    std::string refused;         // non-empty: why it must not be written
};
Drive inspect_drive(const std::filesystem::path& dev, const std::filesystem::path& sys = "/sys",
                    const std::filesystem::path& mounts = "/proc/self/mounts");

// The verbs as openas describes them (part 4 §7): every command with an
// effect, its arguments, its options (the global ones too) and what answers
// its questions -- what `luban interface --describe` prints.
std::vector<openas::Verb> verbs();
nlohmann::json describe_json();
// The command line a verb's JSON arguments stand for (`luban interface
// <verb> --args`): arguments by position, a repeated last one after `--`,
// options by name; an unknown name is refused.
std::expected<std::vector<std::string>, std::string> argv_of(const openas::Verb& v, const nlohmann::json& args);

int run(int argc, char* argv[]);

}  // namespace luban::cli
