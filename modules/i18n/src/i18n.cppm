// xlings.i18n: the i18n kit (luban/kits/i18n) as xlings' programs use it --
// the same mechanism as every program (design 2026-10-19 §7), with xlings'
// own words the default domain ("xlings": its keys predate dotted domains)
// and the system's language read through xlings.platform (Windows' user
// locale too).
export module xlings.i18n;

import std;
export import i18n;
import xlings.platform;

export namespace xlings::i18n {

using ::i18n::Entry;
using ::i18n::Term;
using ::i18n::register_catalogue;
using ::i18n::domains;
using ::i18n::catalogue;
using ::i18n::speaks;
using ::i18n::set_override_env;
using ::i18n::set_language;
using ::i18n::language;
using ::i18n::tr;
using ::i18n::trf;
using ::i18n::check;
using ::i18n::pick;
using ::i18n::register_glossary;
using ::i18n::glossary;
using ::i18n::explain;
using ::i18n::normalize;
using ::i18n::fallback_chain;

// Every key xlings' English catalogue defines (the domain "xlings").
[[nodiscard]] std::span<const Entry> english_catalogue();

}  // namespace xlings::i18n

// In the interface: its initializer runs before any importer's, and it links
// or the program does not.
namespace {
// Before anything asks: xlings' domain is the default one, the system's
// language is the platform's.
[[maybe_unused]] const bool installed_ = [] {
    ::i18n::set_default_domain("xlings");
    ::i18n::set_system_language_source([] { return xlings::platform::get_system_language(); });
    return true;
}();
}  // namespace
