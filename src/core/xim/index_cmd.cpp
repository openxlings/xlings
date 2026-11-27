module xlings.core.xim.index_cmd;

import std;
import xlings.libs.json;
import xlings.core.config;
import xlings.core.home_config;
import xlings.core.log;
import xlings.core.version_order;
import xlings.core.xim.indexfetch;
import xlings.core.xim.repo;
import xlings.platform;
import xlings.runtime;
import mcpplibs.xpkg;
import mcpplibs.xpkg.check;
import xlings.core.xim.machine_rule;
import xlings.core.xim.build_paths;

namespace xlings::xim {

std::string installed_index_version(const std::filesystem::path& repoDir) {
    std::error_code ec;
    auto marker = repoDir / ".xlings-index-version";
    if (!std::filesystem::exists(marker, ec)) return {};
    try {
        auto v = platform::read_file_to_string(marker.string());
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' '))
            v.pop_back();
        return v;
    } catch (...) { return {}; }
}

std::vector<IndexSourceView> collect_index_sources() {
    std::vector<IndexSourceView> views;
    for (const auto& repo : Config::global_index_repos()) {
        IndexSourceView view;
        view.name = repo.name;
        view.pin  = repo.version;
        view.artifactBases = repo.artifactBases;
        const auto repoDir = effective_repo_dir(repo, false);
        view.installed = installed_index_version(repoDir);

        // Ask the pointer only about repos a pointer answers for -- the same
        // question, and the same answerer, the sync uses. Asking anyway said
        // "no pointer entry for 'dsh'" about a perfectly healthy git repo, and
        // for a name that IS in the combined pointer because it is also a
        // declared sub-index, it printed a permanent "run `xlings update` to
        // move to <version>" that the git path can never satisfy.
        if (!artifact_is_declared_for(repo, false)) {
            view.gitManaged = true;
            view.installed  = get_repo_revision_label(repoDir);
            views.push_back(std::move(view));
            continue;
        }

        auto source = artifact_source_for(repo);
        const auto& pointers = load_index_pointers(Config::mirror(),
                                                   source ? &*source : nullptr);
        const auto key = index_pointer_key(repo);
        const auto* manifest = select_manifest(pointers, key, source.has_value());
        if (!manifest) {
            view.error = pointers.empty()
                ? "pointer unavailable (offline, or this repo is git-only)"
                : std::format("no pointer entry for '{}'", key);
            views.push_back(std::move(view));
            continue;
        }
        view.hasPointer = true;
        view.snapshots  = snapshots_of(*manifest);
        view.truncated  = manifest->history_truncated;
        if (auto choice = choose_snapshot(*manifest, Info::VERSION, repo.version)) {
            view.current = choice->snapshot.index_version;
        } else {
            view.error = choice.error();
        }
        views.push_back(std::move(view));
    }
    return views;
}

nlohmann::json index_sources_json(const std::vector<IndexSourceView>& views) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& view : views) {
        nlohmann::json entry;
        entry["name"]      = view.name;
        entry["pinned"]    = view.pin;
        entry["current"]   = view.current;
        // What is on disk, which is not the same question as what would be
        // picked: between a pointer moving and the next `update`, the two differ
        // and only this one describes the tree the client is actually reading.
        entry["installed"] = view.installed;
        entry["git_managed"] = view.gitManaged;
        entry["artifact_bases"] = nlohmann::json::array();
        for (const auto& ab : view.artifactBases)
            entry["artifact_bases"].push_back({ {"region", ab.region}, {"url", ab.url} });
        entry["truncated"] = view.truncated;
        if (!view.error.empty()) entry["error"] = view.error;
        entry["snapshots"] = nlohmann::json::array();
        for (const auto& s : view.snapshots) {
            nlohmann::json row;
            row["index_version"] = s.index_version;
            row["generated_at"]  = s.generated_at;
            row["current"]       = s.index_version == view.current;
            row["installed"]     = !view.installed.empty()
                                   && s.index_version == view.installed;
            // Verbatim. Normalising here would make xlings an interpreter of
            // contracts it does not own.
            row["requires"]      = s.requirements;
            row["artifact"]      = { {"name", s.artifact_name},
                                     {"sha256", s.artifact_sha256},
                                     {"size", s.artifact_size} };
            entry["snapshots"].push_back(std::move(row));
        }
        out.push_back(std::move(entry));
    }
    return out;
}

int cmd_index_list(const std::string& filter, bool asJson, EventStream& stream) {
    auto views = collect_index_sources();
    if (!filter.empty()) {
        std::erase_if(views, [&](const auto& v) { return v.name != filter; });
        if (views.empty()) {
            log::error("no index source named '{}'", filter);
            return 1;
        }
    }

    if (asJson) {
        std::println("{}", index_sources_json(views).dump(2));
        return 0;
    }

    for (const auto& view : views) {
        log::println("");
        log::println("  ◆ {}{}", view.name,
                     view.pin.empty() ? "" : std::format("  (pinned: {})", view.pin));
        for (std::size_t i = 0; i < view.artifactBases.size(); ++i) {
            const auto& ab = view.artifactBases[i];
            log::println("    {} {}{}", i == 0 ? "artifact:" : "    then:",
                         ab.url, ab.region.empty() ? "" : "  [" + ab.region + "]");
        }
        if (view.gitManaged) {
            log::println("    git-managed — no published snapshots{}",
                         view.installed.empty()
                             ? std::string(" (not synced yet)")
                             : std::format(" (on disk: {})", view.installed));
            continue;
        }
        if (!view.error.empty()) {
            log::println("    {}", view.error);
            continue;
        }
        for (const auto& s : view.snapshots) {
            const auto req = requirement_for(s.requirements, "xlings");
            std::string note;
            if (req) {
                if (!req->min.empty()) note += ">= " + req->min;
                if (!req->max.empty()) note += (note.empty() ? "" : " ") + std::string("< ") + req->max;
                note = "  requires xlings " + note;
            }
            // `*` is the tree on disk -- the packages this client resolves
            // against right now. `>` is what the next `update` would fetch.
            // They differ whenever the pointer moved since the last update, and
            // reporting only the second would answer a question the user did
            // not ask while looking like it answered theirs.
            const bool onDisk = !view.installed.empty()
                                && s.index_version == view.installed;
            log::println("    {} {}{}{}",
                         onDisk ? "*" : (s.index_version == view.current ? ">" : " "),
                         s.index_version,
                         s.generated_at.empty() ? "" : "  " + s.generated_at,
                         note);
        }
        if (view.truncated) {
            log::println("      … older snapshots exist but are not listed");
        }
        if (view.installed.empty()) {
            log::println("      (no index on disk yet — run `xlings update`)");
        } else if (!view.current.empty() && view.current != view.installed) {
            log::println("      on disk: {} — run `xlings update` to move to {}",
                         view.installed, view.current);
        }
    }
    log::println("");
    return 0;
}

int cmd_index_use(const std::string& name, const std::string& version,
                  EventStream& stream) {
    if (name.empty()) {
        log::error("usage: xlings index use <name> <version|latest>");
        return 2;
    }
    const bool clearing = version.empty() || version == "latest";

    // Refuse a version the pointer does not offer -- accepting it would leave a
    // config that only fails on the next update, far from the command that
    // wrote it.
    if (!clearing) {
        auto views = collect_index_sources();
        const auto hit = std::ranges::find_if(views,
            [&](const auto& v) { return v.name == name; });
        if (hit == views.end()) {
            log::error("no index source named '{}'", name);
            return 1;
        }
        const bool known = std::ranges::any_of(hit->snapshots,
            [&](const auto& s) { return s.index_version == version; });
        if (!known) {
            log::error("index '{}' has no published snapshot '{}'", name, version);
            std::string list;
            for (const auto& s : hit->snapshots) {
                if (!list.empty()) list += ", ";
                list += s.index_version;
            }
            log::error("  available: {}", list.empty() ? "(none)" : list);
            return 1;
        }
    }

    auto applied = update_home_config(Config::paths().homeDir,
        [&](nlohmann::json& json) {
            if (!json.contains("index_repos") || !json["index_repos"].is_array()) {
                json["index_repos"] = nlohmann::json::array();
            }
            for (auto& entry : json["index_repos"]) {
                if (!entry.is_object() || !entry.contains("name")) continue;
                if (entry["name"].get<std::string>() != name) continue;
                if (clearing) entry.erase("version");
                else entry["version"] = version;
                return true;
            }
            // The source exists in the effective config but not in the file --
            // it is a built-in default. Materialise the entry so the pin has
            // somewhere to live.
            //
            // insert(begin), not push_back: Config materialises a missing
            // default at the FRONT of the effective list (config.cpp), so
            // appending here made the two writers of "where does the default
            // entry go" disagree. `xlings index use xim <ver>` therefore moved
            // the default index out of position 0 -- and position used to
            // decide which entry was the default index at all. Position no
            // longer carries meaning, but two writers with opposite answers is
            // a defect either way, and it still reorders `xlings index`.
            for (const auto& repo : Config::global_index_repos()) {
                if (repo.name != name) continue;
                nlohmann::json entry;
                entry["name"] = repo.name;
                entry["url"]  = repo.url;
                if (!clearing) entry["version"] = version;
                json["index_repos"].insert(json["index_repos"].begin(), entry);
                return true;
            }
            return false;
        });

    if (!applied) {
        log::error("{}", applied.error());
        return 1;
    }
    if (!*applied) {
        log::error("no index source named '{}'", name);
        return 1;
    }
    if (clearing) {
        log::println("index '{}' follows the newest compatible snapshot", name);
    } else {
        log::println("index '{}' pinned to {}", name, version);
    }
    log::println("  run `xlings update` to apply");
    return 0;
}


int cmd_index_check(const std::string& path, bool asJson, EventStream& stream) {
    (void)stream;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target = path.empty() ? fs::current_path(ec) : fs::path(path);
    std::map<std::string, std::vector<mcpplibs::xpkg::Finding>> found;
    std::size_t checked = 0;
    if (fs::is_regular_file(target, ec)) {
        if (auto f = mcpplibs::xpkg::check_recipe(target); !f.empty()) found[target.string()] = std::move(f);
        checked = 1;
    } else if (fs::is_directory(target / "pkgs", ec)) {
        found = mcpplibs::xpkg::check_index(target);
        for (fs::recursive_directory_iterator it(target / "pkgs", ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file() && it->path().extension() == ".lua") ++checked;
    } else {
        log::error("{} is neither a recipe nor an index (a directory with pkgs/)", target.string());
        log::error("  `xlings index check <recipe.lua | index dir>`");
        return 2;
    }

    std::size_t errors = 0, warnings = 0, notes = 0;
    for (const auto& [_, findings] : found)
        for (const auto& f : findings)
            (f.severity == "error" ? errors : f.severity == "warning" ? warnings : notes) += 1;

    if (asJson) {
        auto list = nlohmann::json::array();
        for (const auto& [file, findings] : found)
            for (const auto& f : findings)
                list.push_back({{"file", file}, {"line", f.line}, {"code", f.code},
                                {"severity", f.severity}, {"message", f.message}});
        nlohmann::json out = nlohmann::json::object();
        out["checked"] = checked;
        out["errors"] = errors;
        out["warnings"] = warnings;
        out["notes"] = notes;
        out["findings"] = std::move(list);
        std::println("{}", out.dump(2));
        return errors > 0 ? 1 : 0;
    }

    const bool verbose = log::get_level() <= log::Level::Debug;
    const auto shown = [&](const std::string& rel) {
        return rel.empty() ? target.string() : rel;
    };
    for (const auto& [file, findings] : found) {
        const auto rel = file.empty() ? (target / "index-compat.json").string()
                                      : fs::path(file).lexically_relative(fs::is_directory(target) ? target : target.parent_path()).generic_string();
        for (const auto& f : findings) {
            if (f.severity == "info" && !verbose) continue;
            const auto where = f.line > 0 ? std::format("{}:{}", shown(rel), f.line) : shown(rel);
            log::println("{}: {} {}: {}", where, f.severity, f.code, f.message);
        }
    }
    if (errors == 0 && warnings == 0)
        log::println("{} recipe(s) checked: the xpkg format's rules hold{}", checked,
                     notes > 0 ? std::format(" ({} note(s); -v lists them)", notes) : "");
    else
        log::println("{} recipe(s) checked: {} error(s), {} warning(s){}; docs/specs/xpkg.md §8.4 says what each one means",
                     checked, errors, warnings, notes > 0 && !verbose ? std::format(", {} note(s) with -v", notes) : "");
    return errors > 0 ? 1 : 0;
}


nlohmann::json xpkg_interface_json() {
    const auto& m = mcpplibs::xpkg::interface_manifest();
    auto modules = nlohmann::json::array();
    for (const auto& mod : m.modules) {
        auto names = nlohmann::json::array();
        for (const auto& n : mod.names)
            names.push_back({{"name", n.name}, {"kind", n.kind}, {"signature", n.signature},
                             {"summary", n.summary}, {"since", n.since}, {"platforms", n.platforms}});
        modules.push_back({{"name", mod.name}, {"summary", mod.summary}, {"names", std::move(names)}});
    }
    auto compat = nlohmann::json::array();
    for (const auto& [name, instead] : m.compat) compat.push_back({{"name", name}, {"instead", instead}});
    nlohmann::json out = nlohmann::json::object();
    out["modules"] = std::move(modules);
    out["stdlib"] = {{"globals", m.stdlib_globals}, {"libraries", m.stdlib_libraries}, {"os", m.stdlib_os}};
    out["compat"] = std::move(compat);
    out["host"] = m.host;
    return out;
}


int cmd_payload_check(const std::string& dir, const std::string& arch, bool asJson) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path payload(dir);
    if (dir.empty() || !fs::is_directory(payload, ec)) {
        log::error("__payload-check: {} is no directory", dir);
        return 2;
    }
    const auto machine = arch.empty() ? machine_rule::host_machine() : arch;
    const auto* identity = mcpplibs::xpkg::find_arch(machine);
    if (!identity) {
        log::error("__payload-check: {} is no architecture of the format (docs/specs/xpkg.md §4.2)", machine);
        return 2;
    }
    const auto foreign = machine_rule::foreign(machine_rule::of_payload(payload), identity->name);
    const auto paths = build_paths::scan(payload, {});
    const bool ok = foreign.empty() && paths.files == 0;
    if (asJson) {
        auto m = nlohmann::json::array();
        for (const auto& f : foreign) m.push_back({{"file", f.file}, {"machine", f.machine}});
        auto examples = nlohmann::json::array();
        for (const auto& e : paths.examples) examples.push_back({{"file", e.file}, {"path", e.path}});
        nlohmann::json out = nlohmann::json::object();
        out["arch"] = std::string(identity->name);
        out["ok"] = ok;
        out["M"] = std::move(m);
        out["P"] = {{"files", paths.files}, {"examples", std::move(examples)}};
        std::println("{}", out.dump(2));
        return ok ? 0 : 1;
    }
    if (!foreign.empty()) {
        std::string line = std::format("rule M: {} file(s) the host runs or loads are not {}: {} is for {}",
                                       foreign.size(), identity->name, foreign.front().file, foreign.front().machine);
        for (std::size_t i = 1; i < foreign.size() && i < 3; ++i)
            line += std::format("; {} is for {}", foreign[i].file, foreign[i].machine);
        log::println("{}", line);
    }
    if (paths.files > 0) log::println("{}", build_paths::describe(paths, payload));
    if (ok) log::println("{}: rules M and P hold for {}", payload.string(), identity->name);
    return ok ? 0 : 1;
}

}
