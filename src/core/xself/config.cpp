module xlings.core.xself.config;

import std;
import xlings.core.config;
import xlings.core.uimode;
import xlings.libs.json;
import xlings.runtime;
import xlings.i18n;

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
    // Labels go through `tr`; the values do not. A path, a version and a
    // package name are data, and translating data is how a copied command
    // stops working.
    //
    // XLINGS_HOME / XLINGS_DATA / XLINGS_SUBOS above are environment variable
    // names -- typeable, therefore untranslated.
    const auto L = [](std::string_view key) { return std::string(i18n::tr(key)); };

    addField(L("config.active_subos"), p.activeSubos, true);
    addField(L("config.bin"), p.binDir.string());

    // EVERY row shows the EFFECTIVE value, and says when it was not chosen.
    //
    // Four of these used to be printed only when explicitly configured, while
    // `ui mode` always printed what was in force. One table, two meanings --
    // and the rows that vanished are exactly the ones a user comes here to
    // check. "I never set a language" and "this command is answering in
    // English because it resolved to en" produced identical output: no row.
    //
    // `(default)` / `auto (xx)` is the marker. It costs one word and removes
    // the ambiguity entirely.
    const auto shown = [](const std::string& set, std::string_view effective) {
        if (!set.empty()) return set;
        return std::format("{} ({})", effective, i18n::tr("config.default_marker"));
    };

    auto mirror = Config::mirror();
    if (!mirror.empty()) addField(L("config.mirror"), mirror);

    // `auto` is a real, chosen setting -- it means "follow the system" -- so
    // it is shown with what it resolved TO. Unset is the same behaviour, and
    // reads the same way.
    const auto lang = Config::lang();
    const auto effectiveLang = i18n::language();
    addField(L("config.lang"), (lang.empty() || lang == "auto")
                                   ? std::format("auto ({})", effectiveLang)
                                   : lang);

    addField(L("config.ui_mode"), std::string(ui::to_string(ui::current_mode())));
    addField(L("config.theme"), shown(Config::theme(), "default"));
    addField(L("config.interactive"),
             Config::tui_interactive()
                 ? std::string(*Config::tui_interactive() ? "true" : "false")
                 : std::format("false ({})", i18n::tr("config.default_marker")));

    auto& repos = Config::global_index_repos();
    for (auto& repo : repos) {
        addField(L("config.index_repo"), repo.name + " : " + repo.url);
    }

    if (Config::has_project_config()) {
        addField(L("config.project_data"), Config::project_data_dir().string());
        auto& projectRepos = Config::project_index_repos();
        for (auto& repo : projectRepos) {
            addField(L("config.project_repo"), repo.name + " : " + repo.url);
        }
    }

    nlohmann::json payload;
    payload["title"] = "xlings config";
    payload["fields"] = std::move(fieldsJson);
    stream.emit(DataEvent{"info_panel", payload.dump()});
    return 0;
}

}
