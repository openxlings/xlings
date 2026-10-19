// The words of luban -- its command line and what its service says (design
// 2026-10-19 §7): the domain "luban", English; what this round added is by
// key. The CLI stays English (its output is asserted by docs/generated and the
// fresh suites; a translation is designed with them).
module luban.service;

import std;
import xlings.i18n;

namespace luban::service {

namespace {

constexpr xlings::i18n::Entry kEnglish[] = {
    // luban ls <pattern>
    {"luban.ls.no_match", "no environment matches '{}': luban ls lists them all"},
    // luban search
    {"luban.search.distributions", "distributions"},
    {"luban.search.distributions_hint", "make one: luban new <name> --edition <it>"},
    {"luban.search.desktops", "desktops"},
    {"luban.search.desktops_hint", "give one to a system: luban new <name> --desktop <it>, or luban config <name> --desktop <it>"},
    {"luban.search.not_rated", "not rated"},
    {"luban.search.gpu_better", "GPU better"},
    {"luban.search.gpu_needed", "GPU needed"},
    {"luban.search.official", "official"},
    {"luban.search.preview", "preview"},
    {"luban.search.ignored", "ignored: {}"},
    {"luban.search.unreadable", "could not read the index ({})"},
    {"luban.search.nothing", "nothing offered: the index could not be read, or it lists none (xlings update)"},
    // luban status
    {"luban.status.versions", "versions"},
    {"luban.status.network", "network"},
    {"luban.status.unlisted", "(this system's processes cannot be listed: older Luban OS windows are not looked for)"},
    {"luban.status.proxy_changed", "it runs with the proxy it started with ({}); the host's now is the next start's"},
    // What may be done, never forced (design 2026-10-19 §12.2).
    {"luban.notes.relayout", "keeps what it was before its move into its own userland ({} MB)"},
    {"luban.notes.no_cursor_theme", "its desktop has no cursor theme"},
};

}  // namespace

std::span<const xlings::i18n::Entry> words_en() { return kEnglish; }

void register_words() { xlings::i18n::register_catalogue("luban", "en", kEnglish); }

}  // namespace luban::service
