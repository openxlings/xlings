#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

// A configured theme that is not there is said once, not on every command;
// another value asks again; inside an environment (a SubOS view, a Luban
// root) the default is used without a word -- the home's theme files are not
// projected there, and it is not where a theme is configured.
XTEST(ThemeNotice, AMissingThemeIsSaidOnceAndNeverInsideAnEnvironment, .area = "cli",
      .covers = {"UX-THEME-ONCE"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("theme-once");
    ASSERT_EQ(home.xlings({"self", "init"}).exit_code, 0);
    const auto config = home.dir() / ".xlings.json";
    auto set_theme = [&](std::string_view value) {
        auto doc = nlohmann::json::parse(tk::read_file(config));
        doc["theme"] = value;
        tk::write_file(config, doc.dump());
    };
    constexpr std::string_view said = "configured theme file is not there";

    set_theme("solarized");
    auto first = home.xlings({"config"});
    EXPECT_NE(first.transcript().find(said), std::string::npos) << first.transcript();
    auto second = home.xlings({"config"});
    EXPECT_EQ(second.transcript().find(said), std::string::npos) << second.transcript();

    set_theme("another-missing");
    auto inside = home.xlings({"config"}, {{"XLINGS_SUBOS_MODE", "overlay"}});
    EXPECT_EQ(inside.transcript().find(said), std::string::npos) << inside.transcript();
    auto outside = home.xlings({"config"});
    EXPECT_NE(outside.transcript().find(said), std::string::npos) << outside.transcript();
}
