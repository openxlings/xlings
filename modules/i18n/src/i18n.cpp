module xlings.i18n;

import std;
import i18n;
import xlings.platform;

namespace xlings::i18n {

std::span<const Entry> english_catalogue() { return ::i18n::catalogue("xlings", "en"); }

}  // namespace xlings::i18n
