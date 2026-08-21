module xlings.core.xself.config;

import std;
import xlings.core.config;
import xlings.core.uimode;
import xlings.libs.json;
import xlings.runtime;

namespace xlings::xself {

int cmd_config(EventStream& stream) {
    auto& p = Config::paths();
    nlohmann::json fieldsJson = nlohmann::json::array();
    auto addField = [&](const std::string& label, const std::string& value, bool hl = false) {
        fieldsJson.push_back({{"label", label}, {"value", value}, {"highlight", hl}});
    };
    addField("XLINGS_HOME", p.homeDir.string());
    addField("XLINGS_DATA", p.dataDir.string());
    addField("XLINGS_SUBOS", p.subosDir.string());
    addField("active subos", p.activeSubos, true);
    addField("bin", p.binDir.string());

    auto mirror = Config::mirror();
    if (!mirror.empty()) addField("mirror", mirror);
    auto lang = Config::lang();
    if (!lang.empty()) addField("lang", lang);
    addField("ui mode", std::string(ui::to_string(ui::current_mode())));
    if (auto theme = Config::theme(); !theme.empty()) addField("theme", theme);
    if (auto inter = Config::tui_interactive())
        addField("tui.interactive", *inter ? "true" : "false");

    auto& repos = Config::global_index_repos();
    for (auto& repo : repos) {
        addField("index-repo", repo.name + " : " + repo.url);
    }

    if (Config::has_project_config()) {
        addField("project data", Config::project_data_dir().string());
        auto& projectRepos = Config::project_index_repos();
        for (auto& repo : projectRepos) {
            addField("project repo", repo.name + " : " + repo.url);
        }
    }

    nlohmann::json payload;
    payload["title"] = "xlings config";
    payload["fields"] = std::move(fieldsJson);
    stream.emit(DataEvent{"info_panel", payload.dump()});
    return 0;
}

}
