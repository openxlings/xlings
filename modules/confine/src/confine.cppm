export module xlings.confine;

import std;
export import xlings.confine.implementation;
import xlings.subos.home_view;
import xlings.subos.caps;
import xlings.subos.policy;
import xlings.subos.spec;

// Policy + host capabilities -> a sandbox (SubOS design part 3 §6.1-6.2).
//
//   intent::lower      the policy and the call, as backend-free decisions
//   select             which implementation runs it here (or why none can)
//   compile            the implementation's plan, with the policy's
//                      semantics kept here: what is Must refuses, what is
//                      Should degrades and says so
//   launch_argv        the command that starts it (empty for one that starts
//                      in-process)
//
// The spec is pure data, so isolation is testable without a sandbox; the
// golden (INTENT-EQ) pins it.
export namespace xlings::confine {

// Every implementation, in the order the selector prefers them.
std::span<const Implementation> implementations();
const Implementation* find(sp::Backend backend);

std::expected<sp::SandboxSpec, sp::Refusal> compile(const subos::policy::Policy& policy,
                                                    const HomeView& home, const Caps& caps,
                                                    const sp::Request& request);

// The command that starts `s` (`seccomp_fd`: a terminal-injection filter the
// caller has open). Landlock has no view to start a program in: its argv is
// `s.argv` run as `self` (this binary), which fences itself before it starts
// anything. Empty for an implementation that starts nothing (home redirect,
// fake).
std::vector<std::string> launch_argv(const sp::SandboxSpec& s, std::optional<int> seccomp_fd = std::nullopt,
                                     const std::string& self = {});

}  // namespace xlings::confine
