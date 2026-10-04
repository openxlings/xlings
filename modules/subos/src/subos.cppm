export module xlings.subos;

import std;

// The SubOS core's identity (design §23). Kept deliberately small: the parts
// live in their own modules (`xlings.subos.manifest`, `.policy`, `.spec`, ...)
// and are imported by name, so that importing the package's umbrella never
// pulls a dependency a caller did not ask for.
export namespace xlings::subos {

// The version of the SubOS core's contracts (policy schema, spec schema,
// session protocol). Bumped when one of them changes incompatibly.
inline constexpr std::string_view kCoreVersion = "1";

}  // namespace xlings::subos
