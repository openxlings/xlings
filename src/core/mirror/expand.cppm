// xlings.core.mirror.expand — top-level URL expansion logic.
//
// `expand(url, opts)` is the single entry point used by HTTP / git / index
// download paths. Given an input URL it returns an ordered list of URLs
// to try in sequence — original first, then mirror variants — based on
// the current Mode, the resource type, and the registered mirror list.

export module xlings.core.mirror.expand;

import std;

import xlings.core.config;
import xlings.core.mirror.types;
import xlings.core.mirror.registry;
import xlings.core.mirror.forms;

namespace xlings::mirror { // anonymous namespace

// Mode accessor. Resolved lazily on first read.
export Mode current_mode();

// Override the process-wide mode. Primarily for tests.
export void set_mode(Mode mode);

// Heuristic URL classification. Bare github.com URLs that are ambiguous
// between web view and git clone get classified as Unknown — the git
// caller passes ResourceType::Git via ExpandOptions to disambiguate.
export ResourceType classify(std::string_view url);

export bool is_github_url(std::string_view url);

// Main entry point. Returns the ordered list of URLs to try.
//
// Empty input is preserved as-is; callers handle empty URL elsewhere.
// Non-GitHub URLs are returned unmodified (no expansion). Mode::Off
// always returns just the original.
export std::vector<std::string> expand(std::string_view url,
                                       const ExpandOptions& opts = {});

} // namespace xlings::mirror
