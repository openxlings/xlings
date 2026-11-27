// What the home contracts of 2026.9.12, once shell scripts, share as C++
// tests (#592): a home whose default index (`xim`) is a directory of the
// test's own recipes, a command run in a named subos, and the checks they all
// make -- a command survives a damaged home, a removal leaves nothing behind.
// Included after the imports of xlings.testkit, xlings.libs.json,
// xlings.core.config and std, with tk, fs and Json named as those files name
// them.
#pragma once

namespace home_contract {

// A program recipe: install() lays out bin/<name>, a script answering
// `<name>@<version>`; config() registers it; uninstall() withdraws it.
// `guard` runs first in install() (a `return false` there fails it), `after`
// last; `uninstall` replaces that hook's body.
struct Program {
    std::string name;
    std::vector<std::string> versions { "1.0.0" };
    std::string guard;
    std::string after;
    std::string uninstall;
    bool programs { false };
};

inline std::string recipe(const Program& p) {
    std::string keys;
    for (const auto& v : p.versions) keys += std::format("[\"{}\"] = {{}}, ", v);
    const auto uninstall = p.uninstall.empty()
        ? std::format("xvm.remove(\"{}\")\n    return true", p.name) : p.uninstall;
    const auto programs = p.programs ? std::format("\n    programs = {{\"{}\"}},", p.name) : std::string{};
    return std::format(R"LUA(package = {{
    spec = "1", name = "{0}", description = "a fixture of a home contract",
    type = "package", archs = {{"x86_64", "aarch64"}}, status = "stable",{1}
    xpm = {{ linux = {{ {2}}}, macosx = {{ {2}}}, windows = {{ {2}}} }},
}}
import("xim.libxpkg.pkginfo")
import("xim.libxpkg.xvm")
function install()
    {3}
    local bindir = path.join(pkginfo.install_dir(), "bin")
    os.tryrm(pkginfo.install_dir())
    os.mkdir(bindir)
    io.writefile(path.join(bindir, "{0}"), "#!/bin/sh\necho {0}@" .. pkginfo.version() .. "\n")
    if os.host() ~= "windows" then os.exec("chmod +x " .. path.join(bindir, "{0}")) end
    {4}
    return true
end
function config()
    xvm.add("{0}", {{ bindir = path.join(pkginfo.install_dir(), "bin") }})
    return true
end
function uninstall()
    {5}
end
)LUA", p.name, programs, keys, p.guard, p.after, uninstall);
}

// Where a recipe of the default index lives: pkgs/<first letter>/<name>.lua.
inline fs::path recipe_file(const fs::path& index, std::string_view name) {
    return index / "pkgs" / std::string(name.substr(0, 1)) / (std::string(name) + ".lua");
}

// The index a seeded home reads: <root>/index.
inline fs::path index_of(const tk::Home& home) { return home.root() / "index"; }

// A home whose default index holds `recipes` (name -> text), its entry in
// bin/, initialised, with no sub-index to fetch.
inline tk::Home seeded(std::string_view name, const std::map<std::string, std::string>& recipes) {
    auto home = tk::Home::isolated(name);
    const auto index = index_of(home);
    tk::write_file(index / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    fs::create_directories(index / "pkgs");
    for (const auto& [pkg, text] : recipes) tk::write_file(recipe_file(index, pkg), text);
    Json config = Json::object();
    config["mirror"] = "GLOBAL";
    config["xim"] = Json::object({{"index-repo", index.generic_string()}});
    Json repo = Json::object();
    repo["name"] = std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME);
    repo["url"] = index.generic_string();
    repo["source"] = "git";
    config["index_repos"] = Json::array({repo});
    tk::write_file(home.dir() / ".xlings.json", config.dump(2));
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"));
    const auto init = home.xlings({"self", "init"});
    EXPECT_EQ(init.exit_code, 0) << init.transcript();
    tk::write_file(home.dir() / "data" / "xim-index-repos" / "xim-indexrepos.json", "{}\n");
    return home;
}

// `xlings <args>` with `subos` the active one.
inline tk::RunResult in(const tk::Home& home, std::string_view subos, std::vector<std::string> args,
                        std::map<std::string, std::string> env = {}) {
    env["XLINGS_ACTIVE_SUBOS"] = std::string(subos);
    return home.xlings(std::move(args), std::move(env));
}

// What a terminal shows, without its colour and cursor sequences.
inline std::string plain(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            std::size_t j = i + 2;
            while (j < s.size() && !std::isalpha(static_cast<unsigned char>(s[j]))) ++j;
            i = j;
            continue;
        }
        out += s[i];
    }
    return out;
}

inline bool has(std::string_view text, std::string_view needle) {
    return plain(text).find(needle) != std::string::npos;
}

inline bool has_nocase(std::string_view text, std::string_view needle) {
    auto lower = [](std::string_view s) {
        std::string out(s);
        for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    return lower(plain(text)).find(lower(needle)) != std::string::npos;
}

inline Json read_json(const fs::path& file) { return Json::parse(tk::read_file(file), nullptr, false); }
inline void write_json(const fs::path& file, const Json& json) { tk::write_file(file, json.dump(2)); }

// A hand-edit of the versions database: the config's `versions`, the copy
// every client writes, with the derived data/versions.json dropped so no
// reader answers from a copy without the edit.
inline void edit_versions(const tk::Home& home, const std::function<void(Json&)>& edit) {
    const auto config = home.dir() / ".xlings.json";
    auto json = read_json(config);
    if (!json.contains("versions") || !json["versions"].is_object()) json["versions"] = Json::object();
    edit(json["versions"]);
    write_json(config, json);
    std::error_code ec;
    fs::remove(home.dir() / "data" / "versions.json", ec);
}

// A subos's workspace file, edited (`subos/<name>/.xlings.json`).
inline void edit_workspace(const tk::Home& home, std::string_view subos, const std::function<void(Json&)>& edit) {
    const auto file = home.dir() / "subos" / std::string(subos) / ".xlings.json";
    auto json = fs::exists(file) ? read_json(file) : Json::object();
    if (!json.is_object()) json = Json::object();
    if (!json.contains("workspace") || !json["workspace"].is_object()) json["workspace"] = Json::object();
    edit(json["workspace"]);
    write_json(file, json);
}

// Whether the versions database (either copy) records <name>@<version>.
inline bool registered(const tk::Home& home, std::string_view name, std::string_view version) {
    for (const auto& file : {home.dir() / ".xlings.json", home.dir() / "data" / "versions.json"}) {
        if (!fs::exists(file)) continue;
        const auto json = read_json(file);
        if (!json.is_object() || !json.contains("versions")) continue;
        const auto& versions = json["versions"];
        const std::string key(name);
        if (versions.is_object() && versions.contains(key) && versions[key].is_object()
            && versions[key].contains("versions") && versions[key]["versions"].is_object()
            && versions[key]["versions"].contains(std::string(version)))
            return true;
    }
    return false;
}

inline bool has_shim(const fs::path& bin, std::string_view name) {
    std::error_code ec;
    return fs::exists(bin / std::string(name), ec) || fs::exists(bin / (std::string(name) + ".exe"), ec);
}

inline std::string verified_by(const tk::Home& home) {
    const auto json = read_json(home.dir() / ".xlings.json");
    return json.is_object() ? json.value("verifiedBy", std::string{}) : std::string{};
}

// Nothing of <name>@<version> anywhere state can hide: the versions database,
// every subos's workspace and program shim, the payload directory.
inline void expect_gone(const tk::Home& home, std::string_view name, std::string_view version,
                        std::string_view label) {
    const std::string n(name), v(version);
    EXPECT_FALSE(registered(home, n, v)) << label << ": " << n << "@" << v << " is still in the versions database";
    std::error_code ec;
    for (const auto& subos : fs::directory_iterator(home.dir() / "subos", ec)) {
        const auto ws = read_json(subos.path() / ".xlings.json");
        if (ws.is_object() && ws.contains("workspace") && ws["workspace"].is_object()
            && ws["workspace"].contains(n)) {
            const auto& entry = ws["workspace"][n];
            if (entry.is_object()) {
                EXPECT_NE(entry.value("active", std::string{}), v) << label << ": active in " << subos.path();
                if (entry.contains("installed") && entry["installed"].is_array())
                    for (const auto& i : entry["installed"])
                        EXPECT_NE(i, v) << label << ": installed in " << subos.path();
            } else if (entry.is_string()) {
                EXPECT_NE(entry.get<std::string>(), v) << label << ": named in " << subos.path();
            }
        }
        EXPECT_FALSE(has_shim(subos.path() / "bin", n)) << label << ": a shim is left in " << subos.path();
    }
    for (const auto& store : fs::directory_iterator(home.dir() / "data" / "xpkgs", ec))
        if (store.path().filename().string().ends_with("-x-" + n))
            EXPECT_FALSE(fs::exists(store.path() / v)) << label << ": the payload is left at " << store.path() / v;
}

// A command against a damaged home ends with an exit code a script can act
// on (0 clean, 1 issues found, 2 a refusal), not a signal, and no crash text.
inline void expect_survives(std::string_view label, const tk::RunResult& r) {
    EXPECT_TRUE(r.exit_code >= 0 && r.exit_code <= 2)
        << label << ": exit " << r.exit_code << ", signal " << r.signal << "\n" << r.transcript();
    for (const auto* crash : {"terminate called", "Segmentation", "Aborted"})
        EXPECT_FALSE(has(r.transcript(), crash)) << label << ": " << crash << "\n" << r.transcript();
}

}  // namespace home_contract
