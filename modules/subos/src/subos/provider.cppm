export module xlings.subos.provider;

import std;
import xlings.subos.spec;

// Providers translate a SandboxSpec and decide nothing (design §16-§17).
//
// Each one is a pure function of the spec, so the argv a sandbox runs with is
// a golden-testable value. A capability the spec asks for that a backend
// cannot express is the COMPILER's problem (it reports it as unmet); a
// provider that silently dropped one would be a second, invisible policy.
export namespace xlings::subos::provider {

// `bwrap ... -- <argv>`. `seccomp_fd` is a filter the caller has open
// (--seccomp <fd>), when the spec blocks terminal injection.
std::vector<std::string> bwrap_argv(const spec::SandboxSpec& s,
                                    std::optional<int> seccomp_fd = std::nullopt);

// `proot -r <root> --bind=... --cwd=... <argv>`. proot has no read-only bind
// and no namespaces: it is a view, not a boundary.
std::vector<std::string> proot_argv(const spec::SandboxSpec& s);

// The environment the backend process is started with: exactly spec.env
// when the spec clears it, otherwise `inherited` with spec.env on top.
std::map<std::string, std::string> process_env(const spec::SandboxSpec& s,
                                               const std::map<std::string, std::string>& inherited);

// `pasta --config-net ... ` without its target PID (session::host appends
// it): the nat options the spec asks for.
std::vector<std::string> pasta_args(const spec::SandboxSpec& s);

}  // namespace xlings::subos::provider
