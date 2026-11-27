module xlings.capabilities;

import std;
import xlings.runtime.event;
import xlings.runtime.event_stream;
import xlings.runtime.capability;
import xlings.libs.json;
import xlings.core.xim.commands;
import xlings.core.xim.overlay;
import xlings.core.xself;

// The verbs 2026.9.12.1 gave the command line only (#593, design 2026-11-27
// X10): the local recipes `config --add-xpkg` keeps, and `self doctor` with
// its scope. remove_package's `all` and `subos` are in capabilities.cpp.
namespace xlings::capabilities {

namespace {

std::string_view status_name(xim::overlay::Status::Kind kind) {
    switch (kind) {
        case xim::overlay::Status::Unique:    return "unique";
        case xim::overlay::Status::Identical: return "identical";
        case xim::overlay::Status::Modified:  return "modified";
        case xim::overlay::Status::Behind:    return "behind";
        case xim::overlay::Status::Missing:   return "missing";
    }
    return "unique";
}

}  // namespace

auto LocalRecipes::spec() const -> CapabilitySpec {
    return {
        .name = "local_recipes",
        .description = "The recipes `config --add-xpkg` keeps: list them with their status against the index "
                       "(unique, identical, modified, behind, missing), remove one, or clear all or the stale ones",
        .inputSchema = R"JSON({"type":"object","properties":{"action":{"type":"string","enum":["list","remove","clear"],"description":"list (default), remove one by name, clear a category"},"name":{"type":"string","description":"With remove: the recipe's package name"},"what":{"type":"string","enum":["all","stale"],"description":"With clear: every recipe, or those the index now has identically"}}})JSON",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto LocalRecipes::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    if (!json.is_object()) json = nlohmann::json::object();
    const auto action = json.value("action", std::string("list"));
    if (action == "remove") {
        const auto name = json.value("name", std::string{});
        if (name.empty()) {
            stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput, .message = "remove needs the recipe's name",
                                   .recoverable = false});
            return exit_result(2);
        }
        return exit_result(xim::cmd_remove_xpkg(name));
    }
    if (action == "clear") return exit_result(xim::cmd_clear_xpkg(json.value("what", std::string("stale"))));
    if (action != "list") {
        stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput,
                               .message = std::format("'{}' is not an action of local_recipes", action),
                               .recoverable = false, .hint = "list, remove or clear"});
        return exit_result(2);
    }
    const auto dir = xim::overlay::dir();
    auto discovered = xim::overlay::load_with_files(dir);
    std::ranges::sort(discovered, {}, [](const auto& d) -> const std::string& { return d.entry.name; });
    nlohmann::json recipes = nlohmann::json::array();
    for (const auto& item : discovered) {
        const auto status = xim::overlay::status_of(item.entry, item.path);
        recipes.push_back({{"name", item.entry.name},
                           {"version", item.entry.version},
                           {"status", status_name(status.kind)},
                           {"source", item.entry.source},
                           {"tracked", item.tracked},
                           {"path", item.path.generic_string()}});
    }
    stream.emit(DataEvent{"local_recipes", nlohmann::json{{"recipes", std::move(recipes)}}.dump()});
    return exit_result(0);
}

auto SelfDoctor::spec() const -> CapabilitySpec {
    return {
        .name = "self_doctor",
        .description = "Check this home (self doctor): what is wrong, each with its remedy; with fix, repair "
                       "the derived state; subos limits it to one subos, show_ok lists what passed",
        .inputSchema = R"JSON({"type":"object","properties":{"fix":{"type":"boolean","description":"Repair the derived state (routes, generations, registrations)"},"dry_run":{"type":"boolean","description":"With fix: say what would be repaired"},"deep":{"type":"boolean","description":"Also scan every payload's ELF files"},"show_ok":{"type":"boolean","description":"List the checks that passed too"},"subos":{"type":"string","description":"This subos only"}}})JSON",
        .outputSchema = R"({"type":"object","properties":{"exitCode":{"type":"integer"}}})",
        .destructive = true,
    };
}

auto SelfDoctor::execute(Params params, EventStream& stream) -> Result {
    auto json = nlohmann::json::parse(params, nullptr, false);
    if (!json.is_object()) json = nlohmann::json::object();
    std::optional<std::string> subos;
    if (const auto name = json.value("subos", std::string{}); !name.empty()) subos = name;
    return exit_result(xself::cmd_doctor(stream, json.value("fix", false), /*resetMetadata=*/false,
                                         json.value("dry_run", false), json.value("show_ok", false),
                                         json.value("deep", false), std::nullopt, subos));
}

}  // namespace xlings::capabilities
