// xlings.core.mirror.registry — load + cache the mirror list.
//
// Resolution order (first hit wins):
//   1. ~/.xlings/data/github-mirrors.json (user override; full replacement)
//   2. compiled-in DEFAULT_MIRRORS_JSON
//
// Loaded once on first call to all() / filtered(), then memoized for the
// process lifetime. Failure to parse a user override is logged at warn
// level and the default list is used instead.

export module xlings.core.mirror.registry;

import std;

import xlings.core.config;
import xlings.core.mirror.types;

namespace xlings::mirror {

namespace fs = std::filesystem; // anonymous namespace

// All mirrors, in priority order. Read-only view; the underlying storage
// is process-wide and immutable after first load.
export std::span<const Mirror> all();

// Mirrors filtered by resource type and (optionally) by `expected_size`.
// Expected size of 0 means "size unknown" — limit_bytes filter is skipped
// because we can't know whether the mirror will refuse the request.
export std::vector<Mirror> filtered(ResourceType type,
                                     std::size_t expected_size = 0);

} // namespace xlings::mirror
