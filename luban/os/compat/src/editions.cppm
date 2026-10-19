// Frozen edition names (design 2026-10-18 §15.1): the editions were published
// as subos:luban-<name> before the luban: namespace. Systems made from them
// keep working and are moved to luban:<name> on upgrade.
export module luban.compat:editions;

import std;

export namespace luban::compat {

inline constexpr std::string_view kFrozenPrefix = "subos:luban-";

// "subos:luban-core@2026.10.15.1" is a frozen name.
[[nodiscard]] constexpr bool frozen_edition(std::string_view ref) { return ref.starts_with(kFrozenPrefix); }

// A frozen name without its namespace: "subos:luban-core@1" -> "core@1"; any
// other reference unchanged.
[[nodiscard]] constexpr std::string_view without_frozen_prefix(std::string_view ref) {
    return frozen_edition(ref) ? ref.substr(kFrozenPrefix.size()) : ref;
}

// Whether index entry `ns:n` is the frozen name of the official edition `name`.
[[nodiscard]] inline bool frozen_package(std::string_view ns, std::string_view n, std::string_view name) {
    return ns == "subos" && n.starts_with("luban-") && n.substr(6) == name;
}

// The frozen name of an official edition, "core" and "@v" -> "subos:luban-core@v".
[[nodiscard]] inline std::string frozen_ref(std::string_view name, std::string_view version_suffix = {}) {
    return std::format("{}{}{}", kFrozenPrefix, name, version_suffix);
}

// What a frozen name moved to: "subos:luban-core@1" -> "luban:core"; nullopt
// for a name that is not frozen.
[[nodiscard]] inline std::optional<std::string> successor(std::string_view ref) {
    if (!frozen_edition(ref)) return std::nullopt;
    auto name = without_frozen_prefix(ref);
    if (const auto at = name.find('@'); at != std::string_view::npos) name = name.substr(0, at);
    return std::format("luban:{}", name);
}

}  // namespace luban::compat
