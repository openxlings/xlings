module;

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

module xlings.ui;

import std;
import xlings.platform;
import xlings.core.glyph;
import xlings.core.palette;

namespace xlings::ui::theme {

auto title()     -> Decorator { return ftxui::bold | ftxui::color(cyan()); }

auto success()   -> Decorator { return ftxui::bold | ftxui::color(green()); }

auto warning()   -> Decorator { return ftxui::bold | ftxui::color(amber()); }

auto error()     -> Decorator { return ftxui::bold | ftxui::color(red()); }

auto hint()      -> Decorator { return ftxui::dim | ftxui::color(dim_color()); }

auto highlight() -> Decorator { return ftxui::bold | ftxui::color(magenta()); }

auto label()     -> Decorator { return ftxui::color(dim_color()); }

auto body()      -> Decorator { return ftxui::color(text_color()); }

}
