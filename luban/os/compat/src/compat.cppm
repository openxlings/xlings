// luban.compat: what exists only for a released older version (design
// 2026-10-19 §15.3). Each entry says the version it serves, the version it
// came in, and the condition for removing it; docs/specs/
// upgrade-and-compatibility.md lists them, and removing one removes its test.
export module luban.compat;

import std;
export import :editions;
export import :pointer;
export import :processes;

export namespace luban::compat {

struct Entry {
    std::string_view name;
    std::string_view serves;        // the released versions it is for
    std::string_view since;         // the Luban version it came in
    std::string_view remove_when;
};

inline constexpr std::array<Entry, 3> kEntries{{
    {"frozen_edition_names", "systems made from subos:luban-* (Luban 2026.10.17.1 and before)", "2026.10.18.1",
     "the migration period of design 2026-10-18 §15.1 ends: no official index lists a subos:luban-* edition"},
    {"legacy_xdesk_pointer", "xdesk 2026.10.18.1 and before (no Cursor in its hello; draws no pointer)", "2026.10.19.1",
     "no official index offers a desktop edition with xdesk 2026.10.18.1 or before, and luban status finds none here"},
    {"unregistered_luban_os", "Luban OS 2026.10.18.1 and before (a tray icon per window, no registration)", "2026.10.19.1",
     "the next .0 release after 2026.10.19.1"},
}};

}  // namespace luban::compat
