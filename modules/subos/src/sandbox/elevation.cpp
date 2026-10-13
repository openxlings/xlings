module xlings.subos.elevation;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.observe;
import xlings.subos.home_view;

namespace xlings::subos::elevation {

bool can_ask() {
    if (const char* agent = std::getenv("XLINGS_AGENT_MODE"); agent && std::string_view(agent) == "1") return false;
    if constexpr (platform::is_windows) return true;  // UAC asks in its own window
    return platform::stdin_is_terminal() && platform::in_terminal_foreground();
}

int run(const HomeView& home, const std::vector<std::string>& argv, std::string_view purpose) {
    const bool already = platform::is_elevated();
    const bool ask = can_ask();
    int rc = kNobodyToAsk;
    if (already || ask || platform::elevation_needs_no_question()) rc = platform::run_elevated(argv);
    nlohmann::json line{{"ts", observe::utc_now()}, {"purpose", std::string(purpose)}, {"argv", argv},
                        {"exit", rc}, {"already_elevated", already}, {"could_ask", ask}};
    if (rc == kNobodyToAsk && !already && !ask) line["refused"] = "nobody to ask";
    observe::append_json(home.home / "logs" / "elevation.ndjson", line);
    return rc;
}

std::string nobody_to_ask(std::string_view purpose, std::string_view command) {
    return std::format("{} needs administrator rights, and nobody can be asked here; run `{}` at a terminal",
                       purpose, command);
}

}  // namespace xlings::subos::elevation
