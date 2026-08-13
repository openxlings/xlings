// xlings.core.mirror.forms — three URL-rewriting strategies that mirrors
// can declare via `form` in the registry JSON. Each strategy is a pure
// function so it's trivially unit-testable.

export module xlings.core.mirror.forms;

import std;
import xlings.core.mirror.types;

namespace xlings::mirror { // anonymous namespace

// Public entry point used by expand.cppm. Returns the rewritten URL, or
// nullopt when the mirror's form is incompatible with the input URL —
// the caller drops that mirror from the candidate list.
export std::optional<std::string> rewrite(std::string_view url,
                                          const Mirror& mirror,
                                          ResourceType type);

} // namespace xlings::mirror
