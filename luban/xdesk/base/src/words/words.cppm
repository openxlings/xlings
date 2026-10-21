// xdesk.words: xdesk's words in a person's language, through the i18n kit
// (luban/kits/i18n) -- the mechanism every program shares. The desktop's
// text is written in English where it is used (i18n::tr("Apps")), each a key
// that is its own English; the other languages are tables here. The domain
// is "xdesk", the desktop's default.
export module xdesk.words;

import std;
export import i18n;

export namespace xdesk::words {

// Once, before anything is drawn: the catalogues, the default domain.
void register_words();
// The language resolved is a Chinese one (fonts that have its glyphs).
bool chinese();

}  // namespace xdesk::words
