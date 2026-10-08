module xlings.subos.elevation;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.observe;
import xlings.subos.home_view;

namespace xlings::subos::elevation {

int run(const HomeView& home, const std::vector<std::string>& argv, std::string_view purpose) {
    const bool already = platform::is_elevated();
    const int rc = platform::run_elevated(argv);
    observe::append_json(home.home / "logs" / "elevation.ndjson",
                         nlohmann::json{{"ts", observe::utc_now()}, {"purpose", std::string(purpose)},
                                        {"argv", argv}, {"exit", rc}, {"already_elevated", already}});
    return rc;
}

}  // namespace xlings::subos::elevation
