module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

module xlings.ui;

import std;
import xlings.platform;
import xlings.core.glyph;
import xlings.core.palette;

namespace xlings::ui::theme {

namespace style {

auto title()     -> Decorator { return ftxui::bold | ftxui::color(theme::accent()); }

auto success()   -> Decorator { return ftxui::bold | ftxui::color(theme::success()); }

auto warning()   -> Decorator { return ftxui::bold | ftxui::color(theme::warn()); }

auto error()     -> Decorator { return ftxui::bold | ftxui::color(theme::error()); }

auto hint()      -> Decorator { return ftxui::dim | ftxui::color(theme::muted()); }

auto highlight() -> Decorator { return ftxui::bold | ftxui::color(theme::alt()); }

auto label()     -> Decorator { return ftxui::color(theme::muted()); }

auto body()      -> Decorator { return ftxui::color(theme::text()); }

}  // namespace style

}
