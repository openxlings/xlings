export module xlings.subos.library_cache;

import std;
import xlings.subos.home_view;

export namespace xlings::subos::library_cache {

namespace fs = std::filesystem;

// Only ldconfig's output staging directory is writable. The root, its
// configuration and its payloads remain read-only during cache generation.
std::vector<std::string> command(const fs::path& root, const HomeView& home,
                                 const fs::path& scratch, const fs::path& bwrap);

// Missing ldconfig means a root has no glibc cache to generate. An existing
// malformed/unowned cache or failed generator is an error. A write-ahead
// digest record proves ownership without treating unreadable state as empty.
std::expected<bool, std::string> refresh(const fs::path& root, const HomeView& home,
                                        std::string_view instance);

}  // namespace xlings::subos::library_cache
