// `xlings self log` -- what an operation did, read back from the operation
// journal (xlings.observe ops; Luban OS architecture design §13).
//
//   self log                 the latest operations: when, trace, exit, time, command
//   self log <trace>         one operation: its processes, phases (with their
//                            milliseconds and failures), events and side files
//   self log <trace> --bundle [FILE]
//                            the operation and its side files in one file, with
//                            the home's path, the user's home and name taken out
//                            -- what a bug report carries
//   --json                   the journal's lines as they are
export module xlings.core.xself.oplog;

import std;

export namespace xlings::xself {

int cmd_log(const std::vector<std::string>& args);

// The bundle's text for an operation journal and its side files: every
// occurrence of `home`, the user's home directory and `user` replaced.
std::string redact_for_bundle(std::string text, const std::filesystem::path& home,
                              const std::filesystem::path& userHome, std::string_view user);

}  // namespace xlings::xself
