// xlings.agent.text_renderer — plain-text rendering for --agent mode.
//
// Converts DataEvent payloads to clean, unformatted text output.
// No ANSI codes, no TUI decorations, no progress bar animations.
// Designed to be easily readable by LLM agents.

export module xlings.agent.text_renderer;

import std;

import xlings.libs.json;
import xlings.runtime.event;

namespace xlings::agent {

// Render a DataEvent as plain text to stdout.
// Silently skips event kinds that are purely visual (e.g. download_progress).
export void render_data_event(const DataEvent& e) {
    auto json = nlohmann::json::parse(e.json, nullptr, false);
    if (json.is_discarded()) return;

    if (e.kind == "info_panel") {
        auto title = json.value("title", "");
        if (!title.empty()) std::println("{}", title);
        auto print_fields = [](const nlohmann::json& arr) {
            if (!arr.is_array()) return;
            for (auto& f : arr) {
                std::println("  {}: {}", f.value("label", ""), f.value("value", ""));
            }
        };
        if (json.contains("fields")) print_fields(json["fields"]);
        if (json.contains("extra_fields")) print_fields(json["extra_fields"]);
    }
    else if (e.kind == "tip") {
        std::println("hint: {}", json.value("message", ""));
    }
    else if (e.kind == "search_results") {
        if (json.contains("results") && json["results"].is_array()) {
            for (auto& r : json["results"]) {
                if (r.is_array() && r.size() >= 2) {
                    std::println("  {}  {}", r[0].get<std::string>(), r[1].get<std::string>());
                }
            }
        }
    }
    else if (e.kind == "table") {
        if (json.contains("headers") && json["headers"].is_array()) {
            for (auto& h : json["headers"]) {
                std::print("{}\t", h.get<std::string>());
            }
            std::println("");
        }
        if (json.contains("rows") && json["rows"].is_array()) {
            for (auto& row : json["rows"]) {
                if (row.is_array()) {
                    for (auto& cell : row) {
                        std::print("{}\t", cell.get<std::string>());
                    }
                    std::println("");
                }
            }
        }
    }
    else if (e.kind == "install_plan") {
        std::println("Install plan:");
        if (json.contains("packages") && json["packages"].is_array()) {
            for (auto& p : json["packages"]) {
                if (p.is_array() && p.size() >= 2) {
                    std::println("  {} {}", p[0].get<std::string>(), p[1].get<std::string>());
                }
            }
        }
    }
    else if (e.kind == "install_summary") {
        auto ok = json.value("success", 0);
        auto fail = json.value("failed", 0);
        if (fail > 0) {
            std::println("Install complete: {} succeeded, {} failed", ok, fail);
        } else {
            std::println("Install complete: {} succeeded", ok);
        }
    }
    else if (e.kind == "remove_plan") {
        std::println("Remove: {} {} (subos: {})",
            json.value("name", ""), json.value("version", ""), json.value("subos", ""));
    }
    else if (e.kind == "remove_summary") {
        auto name = json.value("name", json.value("target", ""));
        // Same distinction the TUI makes: a detach is not a removal, and an
        // agent reading this must not conclude the payload is gone (#443).
        if (json.value("detached", false)) {
            std::string others;
            if (json.contains("pinned_by") && json["pinned_by"].is_array()) {
                for (auto& n : json["pinned_by"]) {
                    if (!n.is_string()) continue;
                    if (!others.empty()) others += ", ";
                    others += n.get<std::string>();
                }
            }
            std::println("Detached: {} {} (subos: {}); payload kept, still used by {}",
                name, json.value("version", ""), json.value("subos", ""),
                others.empty() ? std::string("another subos") : others);
        } else {
            std::println("Removed: {} {} (subos: {})",
                name, json.value("version", ""), json.value("subos", ""));
        }
    }
    else if (e.kind == "styled_list") {
        auto title = json.value("title", "");
        if (!title.empty()) std::println("{}", title);
        if (json.contains("items") && json["items"].is_array()) {
            int idx = 1;
            bool numbered = json.value("numbered", false);
            for (auto& item : json["items"]) {
                if (item.is_array() && item.size() >= 2) {
                    if (numbered) {
                        std::println("  {}. {} {}", idx++, item[0].get<std::string>(), item[1].get<std::string>());
                    } else {
                        std::println("  {} {}", item[0].get<std::string>(), item[1].get<std::string>());
                    }
                } else if (item.is_string()) {
                    if (numbered) {
                        std::println("  {}. {}", idx++, item.get<std::string>());
                    } else {
                        std::println("  {}", item.get<std::string>());
                    }
                }
            }
        }
    }
    else if (e.kind == "subos_list") {
        if (json.contains("entries") && json["entries"].is_array()) {
            for (auto& s : json["entries"]) {
                auto name = s.value("name", "");
                auto active = s.value("active", false);
                auto pkgs = s.value("pkgCount", 0);
                std::println("  {}{} ({} packages)",
                    name, active ? " (active)" : "", pkgs);
            }
        }
    }
    else if (e.kind == "subos_created") {
        std::println("Created subos: {} ({})",
            json.value("name", ""), json.value("dir", ""));
    }
    else if (e.kind == "subos_switched") {
        std::println("Switched to subos: {}", json.value("name", ""));
    }
    else if (e.kind == "subos_entering") {
        std::println("Entering subos: {}", json.value("name", ""));
    }
    else if (e.kind == "subos_already_in") {
        std::println("Already in subos: {}", json.value("name", ""));
    }
    else if (e.kind == "subos_nesting") {
        std::println("Nesting subos: {} -> {}",
            json.value("from", ""), json.value("to", ""));
    }
    else if (e.kind == "subos_removed") {
        std::println("Removed subos: {}", json.value("name", ""));
    }
    else if (e.kind == "help") {
        auto name = json.value("name", "");
        auto desc = json.value("description", "");
        if (!name.empty()) std::println("  xlings {}", name);
        if (!desc.empty()) std::println("  {}", desc);
        if (json.contains("args") && json["args"].is_array()) {
            for (auto& a : json["args"]) {
                std::println("    {} — {}", a.value("name", ""), a.value("desc", ""));
            }
        }
        if (json.contains("opts") && json["opts"].is_array()) {
            for (auto& o : json["opts"]) {
                std::println("    {} — {}", o.value("name", ""), o.value("desc", ""));
            }
        }
    }
    // download_progress, system_info, env, repo_list, subos_shims:
    // these are either noisy (progress) or already covered by info_panel.
    // Skip silently — the log messages carry the essential info.
}

}  // namespace xlings::agent
