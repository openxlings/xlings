// xlings' own words, registered with the i18n mechanism as the domain
// "xlings" (design 2026-10-19 §7.2): called first thing in xlings' main.
export module xlings.ui.i18n;

import std;
import xlings.i18n;
import xlings.i18n.en;
import xlings.i18n.zh;

export namespace xlings::ui {

inline void register_catalogues() {
    i18n::register_catalogue("xlings", "en", i18n::en::kEntries);
    i18n::register_catalogue("xlings", "zh", i18n::zh::kEntries);
}

}  // namespace xlings::ui
