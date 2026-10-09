// luban: the Luban OS management tool (Luban design §B3).
//
// One noun -- an environment -- and a handful of verbs, shown by level (help
// lists the common ones). Everything an environment IS stays in xlings: each
// command here is an xlings command, run on the same terminal (so there is
// one implementation, and `--help` names the xlings command for whoever
// needs more). What is luban's own: where you are, the overview, the
// edition names, the image formats, and the machine (stage-0, boot).
export module luban.cli;

import std;
import xlings.cli.model;

export namespace luban::cli {

const xlings::cli::spec::CommandSpec& tree();

// Where this luban runs.
enum class Place { Host, Environment, Machine };
Place where();
std::string_view to_string(Place p);

// `core` -> `subos:luban-core`; a full reference (`ns:name[@v]`) as given.
std::string edition_ref(std::string_view edition);

// What `luban export` writes, from the file name: iso, img, qcow2, tar, dir;
// empty when the name says nothing it knows.
std::string export_format(std::string_view file);

// The xlings command a luban command is. `args` is argv after `luban`, the
// global options already taken out. A usage error is the message.
std::expected<std::vector<std::string>, std::string> to_xlings(std::span<const std::string> args);

// The xlings this luban drives: $LUBAN_XLINGS, next to this binary,
// $XLINGS_HOME/bin, then PATH.
std::filesystem::path xlings_path();

// luban is released with the xlings it drives: the same date version
// (YYYY.M.D.N). tests/unit/test_luban_cli.cpp holds the two equal.
inline constexpr std::string_view kVersion = "2026.10.9.2";

// The lowest xlings interface protocol this luban speaks.
inline constexpr std::string_view kMinProtocol = "1.6";

int run(int argc, char* argv[]);

}  // namespace luban::cli
