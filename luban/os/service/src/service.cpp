module luban.service;

import std;
import luban.compat;
import xlings.libs.json;
import opencaps;
import xlings.platform;
import xlings.observe;
import xlings.subos.home_view;
import xlings.subos.located;
import xlings.carrier;
import xlings.platform.stream;
import xlings.subos.edition;
import xlings.subos.policy;
import xlings.subos.policy_store;
import xlings.subos.root_etc;
import xlings.subos.rootfs;
import xlings.subos.session;
import luban.service.archive;
import xlings.store;

namespace luban::service {

namespace {

using Json = nlohmann::json;
using xlings::subos::HomeView;

Json read_json_(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return Json::object();
    auto j = Json::parse(xlings::platform::read_file_to_string(file.string()), nullptr, false);
    return j.is_object() ? j : Json::object();
}

// Whether a root has the program at `path` (/bin/sh, /usr/bin/fish), looked
// for in its /usr projection: the root's /bin and /usr are links that name
// the system's own paths (/xlings in a domain), which this host need not
// have. Merged /usr: /bin and /sbin are its bin.
bool root_has_(const Context& c, std::string_view name, const fs::path& path) {
    const auto usr = xlings::subos::rootfs::usr_of(xlings::subos::locate(HomeView{c.home}, name).home.instance(name));
    std::error_code ec;
    return fs::symlink_status(usr / "bin" / path.filename(), ec).type() != fs::file_type::not_found
        || fs::symlink_status(usr / "sbin" / path.filename(), ec).type() != fs::file_type::not_found;
}

bool exists_(const Context& c, std::string_view name) {
    std::error_code ec;
    return !name.empty() && fs::is_directory(HomeView{c.home}.instance(name), ec);
}

Outcome missing_(const Context& c, std::string_view name) {
    if (c.sink) c.sink(error(std::format("there is no environment '{}'", name), "luban ls"));
    return {.code = 1};
}

// Events of an xlings step, passed on; a step's own failure ends its phase.
int step_(const Context& c, std::string phase_name, std::string text, std::span<const std::string> argv, bool yes = false,
          std::string done = {}) {
    if (c.sink) c.sink(phase(phase_name, "begin", text));
    xlings::observe::ops::Span span(phase_name);
    const int rc = c.port.subos(argv, yes, c.sink);
    if (rc != 0) span.fail(std::format("exit {}", rc));
    if (c.sink) c.sink(phase(phase_name, rc == 0 ? "done" : "failed", rc == 0 && !done.empty() ? done : text));
    return rc;
}

// The self-check (design 2026-10-16 L §11), asked inside the system by its
// session's xlings: nullopt when it is usable, otherwise what failed. A
// check that cannot be asked is said and does not hold anything up.
std::optional<std::string> unusable_(const Context& c, std::string_view name, std::string_view phase_name,
                                     const std::string& ready) {
    if (c.sink) c.sink(phase(std::string(phase_name), "begin", std::format("checking {}", name)));
    const auto report = c.port.json({"subos", "exec", std::string(name), "--", "/run/xlings/xlings", "subos", "__check", "--json"});
    if (!report || !report->is_object() || !report->contains("ok")) {
        if (c.sink) c.sink(phase(std::string(phase_name), "done", ready + " (not checked)"));
        if (c.sink) c.sink(notice(std::format("its self-check did not answer: {}", report ? report->dump() : report.error()),
                                  std::format("luban status {} --check", name)));
        return std::nullopt;
    }
    std::string failed;
    for (const auto& item : report->value("items", Json::array()))
        if (item.value("critical", false) && !item.value("ok", true))
            failed += (failed.empty() ? "" : "; ") + item.value("what", std::string{}) + ": " + item.value("detail", std::string{});
    const bool ok = report->value("ok", false);
    if (c.sink) c.sink(phase(std::string(phase_name), ok ? "done" : "failed",
                             ok ? std::format("{} ({} checks, {} notes: luban status {} --check)", ready,
                                              report->value("items", Json::array()).size(), report->value("failed", 0), name)
                                : std::format("checking {}", name)));
    if (ok) return std::nullopt;
    return failed;
}

std::vector<std::string> argv_(std::initializer_list<std::string_view> words) {
    std::vector<std::string> out;
    for (auto w : words) out.emplace_back(w);
    return out;
}

// A Luban system is its own userland (part 3 §3.4): on Linux it is made in
// the system prefix domain (/xlings), so it shares no payload with this
// host's SubOS -- not the runtime they were bound to, not the host's user
// name in its paths. A carrier (WSL2, vz) makes it where it runs.
std::vector<std::string> new_argv_(std::string_view name, std::string_view edition, std::string_view carrier) {
    auto args = argv_({"new", name, "--from", edition});
    if (!carrier.empty()) args.insert(args.end(), {"--carrier", std::string(carrier)});
    else if constexpr (xlings::platform::is_linux) args.insert(args.end(), {"--rootfs", "--domain-if-possible", "/xlings"});
    return args;
}

const std::set<std::string, std::less<>>& isolation_keys_() {
    static const std::set<std::string, std::less<>> value{"policy", "proxy", "tz", "allow", "disallow",
                                                          "mount", "unmount", "fetch", "observe"};
    return value;
}

}  // namespace


std::expected<EditionAvailability, std::string> edition_availability(const Context& c, std::string_view edition) {
    std::vector<Json> data;
    std::string error;
    bool not_found = false;
    // Its short name is a name in the luban: namespace (or a published subos:luban-* one).
    const auto target = edition.find(':') != std::string_view::npos
        ? std::string(edition) : official_system(c.port, edition).value_or(std::format("luban:{}", edition));
    // Linux's build: an edition is one, wherever luban runs.
    const int rc = c.port.capability("package_info", Json{{"target", target}, {"platform", "linux"}},
                                     [&](const Event& e) {
                                         if (e.kind != Event::Kind::Error) return;
                                         not_found = not_found || e.code == "E_NOT_FOUND";
                                         if (error.empty()) error = e.text;
                                     }, &data);
    EditionAvailability out;
    for (const auto& d : data) {
        if (d.value("dataKind", "") != "info_panel" || !d.contains("payload")) continue;
        out.listed = true;
        for (const auto& f : d["payload"].value("fields", Json::array())) {
            const auto label = f.value("label", ""), value = f.value("value", "");
            if (label == "status") out.status = value;
            else if (label == "description") out.description = value;
            else if (label == "archs") {
                for (std::size_t at = 0; at < value.size();) {
                    const auto sp = value.find(' ', at);
                    if (sp != at) out.archs.push_back(value.substr(at, sp == std::string::npos ? std::string::npos : sp - at));
                    if (sp == std::string::npos) break;
                    at = sp + 1;
                }
            } else if (label == "aliases" && value.starts_with("latest -> ")) out.latest = value.substr(10);
        }
    }
    // Not listed is an answer; a failure that is not "no such package" is not.
    if (!out.listed && rc != 0 && !not_found) return std::unexpected(error.empty() ? std::string("the index could not be asked") : error);
    return out;
}


// A system on a carrier lives in that carrier's machine: what it is comes
// from the luban there, through the same interface (part 4 §7.4) -- the
// edition, packages, generation and shell it has. False when the carrier
// does not answer (a guest from before it carried luban): the record here
// stands, without them.
// What the luban there says of one system (its status --json fields).
void apply_guest_(Environment& e, const nlohmann::json& j) {
    e.kind = j.value("kind", e.kind);
    e.edition = j.value("edition", e.edition);
    e.packages = j.value("packages", e.packages);
    e.shell = j.value("shell", std::string{});
    e.running = j.value("running", false);
    if (j.contains("generation") && j["generation"].is_number_integer()) e.generation = j["generation"].get<int>();
    if (auto d = j.find("desktops"); d != j.end() && d->is_array()) {
        e.desktops.clear();
        for (const auto& id : *d)
            if (id.is_string()) e.desktops.push_back(id.get<std::string>());
    }
}

// `/xlings/bin/luban <args> --json` in the carrier's machine: its JSON lines.
std::vector<nlohmann::json> ask_guest_luban_(const Context& c, std::string_view carrier_name, std::vector<std::string> args) {
    const auto* carrier = xlings::carrier::find(carrier_name);
    if (!carrier) return {};
    auto at = carrier->ensure(HomeView{c.home});
    if (!at || at->launcher.empty()) return {};
    std::vector<std::string> argv = at->launcher;
    argv.push_back("/xlings/bin/luban");
    argv.insert(argv.end(), args.begin(), args.end());
    argv.push_back("--json");
    std::string out;
    if (xlings::platform::stream::run(argv, [&](std::string_view stream, std::string_view bytes) {
            if (stream == "stdout") out.append(bytes);
        }) != 0)
        return {};
    std::vector<nlohmann::json> lines;
    std::istringstream in(out);
    for (std::string line; std::getline(in, line);)
        if (auto one = nlohmann::json::parse(line, nullptr, false); one.is_object())
            lines.push_back(one.contains("result") && one["result"].is_object() ? one["result"] : one);
    return lines;
}

// A system on a carrier lives in that carrier's machine: what it is comes
// from the luban there, through the same interface (part 4 §7.4) -- the
// edition, packages, generation and shell it has. False when the carrier
// does not answer (a guest from before it carried luban): the record here
// stands, without them.
bool describe_in_carrier_(const Context& c, Environment& e) {
    for (const auto& j : ask_guest_luban_(c, e.carrier, {"status", e.name}))
        if (j.value("name", std::string{}) == e.name) {
            apply_guest_(e, j);
            return true;
        }
    return false;
}

namespace {

// What this home's record says, and what the system's own files say where
// this host can read them (here, its prefix domain, a Mac's host machine
// tree); `asked` when they are only in a carrier's machine (a PC's WSL).
std::expected<Environment, std::string> describe_(const Context& c, std::string_view name, bool& asked) {
    if (!exists_(c, name)) return std::unexpected(std::format("there is no environment '{}'", name));
    const auto at = xlings::subos::locate(HomeView{c.home}, name);
    const HomeView view = at.home;
    Environment e;
    e.name = std::string(name);
    const auto record = read_json_(HomeView{c.home}.instance_file(name));
    const auto instance = read_json_(view.instance_file(name));
    e.kind = instance.value("kind", "view") == "rootfs" ? "root" : "view";
    e.carrier = record.value("carrier", "local");
    asked = e.carrier != "local" && !at.domain;
    std::set<std::string> packages;
    if (auto record = xlings::subos::edition::read(instance)) {
        e.edition = record->ref;
        e.chain = record->chain;
        e.edition_index = record->index;
        e.chain_index = record->chain_index;
        for (const auto& [pkg, _] : record->packages) packages.insert(xlings::subos::edition::split(pkg).first);
    }
    const auto workspace = read_json_(view.instance(name) / ".xlings.json");
    e.manifest = workspace;
    // The packages it was given (its file's `packages`), not the names they
    // register (a glibc alone registers dozens).
    if (auto declared = workspace.find("packages"); declared != workspace.end() && declared->is_array())
        for (const auto& spec : *declared)
            if (spec.is_string()) packages.insert(xlings::subos::edition::split(spec.get<std::string>()).first);
    e.packages = packages.size();
    if (e.kind == "root") {
        e.generation = xlings::subos::rootfs::current(view.instance(name));
        if (auto root = root_of(c.home, name)) {
            // Its login shell, when the root has one (nano has none).
            e.shell = xlings::subos::root_etc::login_shell(*root);
            if (!root_has_(c, name, e.shell)) e.shell.clear();
        }
    }
    e.running = xlings::subos::session::find(view, name).has_value();
    if (auto usr = e.kind == "root" ? usr_of(c.home, name) : std::nullopt) {
        std::error_code ec;
        for (const auto& dir : {*usr / "local" / "share" / "wayland-sessions", *usr / "share" / "wayland-sessions"}) {
            std::vector<fs::path> entries;
            for (fs::directory_iterator it(xlings::subos::physical(at, dir), ec), end; !ec && it != end; it.increment(ec))
                entries.push_back(it->path());
            ec.clear();
            std::ranges::sort(entries);
            for (const auto& entry : entries) {
                if (entry.extension() != ".desktop" || std::ranges::find(e.desktops, entry.stem().string()) != e.desktops.end())
                    continue;
                e.desktops.push_back(entry.stem().string());
                std::ifstream in(entry);
                for (std::string line; std::getline(in, line);)
                    if (line.starts_with("X-Luban-Aliases=")) {
                        std::string_view rest(line);
                        rest.remove_prefix(16);
                        for (std::size_t at = 0; at <= rest.size();) {
                            const auto semi = rest.find_first_of(";,", at);
                            auto alias = std::string(rest.substr(at, semi == std::string_view::npos ? std::string_view::npos : semi - at));
                            if (!alias.empty()) e.aliases.try_emplace(alias, entry.stem().string());
                            if (semi == std::string_view::npos) break;
                            at = semi + 1;
                        }
                    }
            }
        }
    }
    if (e.kind == "root")
        if (auto root = root_of(c.home, name)) e.layout = xlings::subos::rootfs::layout_of(*root);
    if (auto luban = workspace.find("luban"); luban != workspace.end() && luban->is_object())
        for (const auto& need : luban->value("requires", nlohmann::json::array()))
            if (need.is_string()) e.needs.push_back(need.get<std::string>());
    auto settings = read_settings(c.home, name);
    if (!settings) return std::unexpected(settings.error());
    e.settings = *settings;
    return e;
}

}  // namespace

std::expected<Environment, std::string> describe(const Context& c, std::string_view name) {
    bool asked = false;
    auto e = describe_(c, name, asked);
    if (e && asked) describe_in_carrier_(c, *e);
    return e;
}

// Every system: those this host can read, read; those only a carrier's
// machine has, asked of the luban there once for all of them (one round
// trip per carrier, not one per system).
std::expected<std::vector<Environment>, std::string> list(const Context& c) {
    std::vector<Environment> out;
    std::map<std::string, std::vector<std::size_t>> by_carrier;
    std::error_code ec;
    for (fs::directory_iterator it(HomeView{c.home}.subos_root(), ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name == "default" || name == "current" || name.starts_with('.') || !it->is_directory(ec)) continue;
        bool asked = false;
        auto e = describe_(c, name, asked);
        if (!e) return std::unexpected(e.error());
        if (asked) by_carrier[e->carrier].push_back(out.size());
        out.push_back(std::move(*e));
    }
    for (const auto& [carrier, which] : by_carrier) {
        std::map<std::string, nlohmann::json> there;
        for (const auto& j : ask_guest_luban_(c, carrier, {"ls"}))
            for (const auto& one : j.value("environments", nlohmann::json::array()))
                if (one.is_object()) there[one.value("name", std::string{})] = one;
        for (const auto i : which)
            if (auto it = there.find(out[i].name); it != there.end()) apply_guest_(out[i], it->second);
    }
    std::ranges::sort(out, {}, &Environment::name);
    return out;
}

namespace {

// An edition's declared login shell, set when the root has it; said, and
// left to the root's best, when it does not.
void preset_login_shell_(const Context& c, std::string_view name, std::string shell = {}) {
    const auto usr = usr_of(c.home, name);
    const auto root = root_of(c.home, name);
    if (!usr || !root) return;
    if (shell.empty()) {
        const auto at = xlings::subos::locate(xlings::subos::HomeView{c.home}, name);
        const auto file = xlings::subos::physical(at, *usr / "share" / "luban" / "settings.json");
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) return;
        const auto d = nlohmann::json::parse(xlings::platform::read_file_to_string(file.string()), nullptr, false);
        if (!d.is_object() || !d.contains("login_shell") || !d["login_shell"].is_string()) return;
        shell = d["login_shell"].get<std::string>();
    }
    if (shell == xlings::subos::root_etc::login_shell(*root)) return;
    if (!root_has_(c, name, shell)) {
        if (c.sink) c.sink(notice(std::format("its edition names {} as the login shell, which {} does not have",
                                              shell, name),
                                  std::format("xlings install {} --subos {}", fs::path(shell).filename().string(), name)));
        return;
    }
    if (auto set = xlings::subos::root_etc::set_login_shell(*root / "etc", "root", shell); !set && c.sink)
        c.sink(notice(set.error()));
}

}  // namespace

opencaps::Provides luban_provides() {
    return {{"root-layout", xlings::subos::rootfs::kLayout}, {"luban-manifest", 1}};
}

std::vector<opencaps::Verdict> unmet_needs(const Environment& e, const opencaps::Provides& reader) {
    std::vector<opencaps::Need> needs;
    if (e.layout > 0) needs.push_back({"root-layout", e.layout});
    for (const auto& text : e.needs) {
        if (auto n = opencaps::parse_need(text)) needs.push_back(*n);
        // A need this side cannot read is a need of something newer: unmet, by its text.
        else needs.push_back({text, 1});
    }
    return opencaps::unmet(needs, reader);
}

EnterPlan enter_plan(const Environment& e) {
    const auto& setting = e.settings.desktop;
    // What going in shows first (design 2026-10-18 §8): a system set to its
    // console enters it whatever desktops it has; not set, the rule before --
    // its desktop when it has one.
    if (e.settings.enter == "console") return {};
    if (setting == "none" || (setting.empty() && e.desktops.empty())) return {};
    if (setting.empty()) return {.target = "desktop", .session = e.desktops.front()};
    if (std::ranges::find(e.desktops, setting) != e.desktops.end()) return {.target = "desktop", .session = setting};
    // A session renamed answers to its old name (labwc-desktop is labwc now).
    if (const auto renamed = e.aliases.find(setting); renamed != e.aliases.end())
        return {.target = "desktop", .session = renamed->second};
    std::string have;
    for (const auto& d : e.desktops) have += (have.empty() ? "" : ", ") + d;
    return {.why = have.empty()
                ? std::format("its desktop {} is not in it: xlings install <the desktop> --subos {}, or luban config {} --desktop none",
                              setting, e.name, e.name)
                : std::format("its desktop {} is not in it (it has {}): luban config {} --desktop <one of them>",
                              setting, have, e.name)};
}

bool needs_user(const NewPreview& p) {
    const auto* base = p.lineage.base();
    return base && (base->name() == "desktop" || base->name().starts_with("desktop-"));
}

Json NewPreview::value() const {
    Json lineage_json = Json::array();
    for (const auto& l : lineage.layers)
        lineage_json.push_back({{"ref", l.ref}, {"index", l.index}, {"official", l.official()}, {"name", l.name()},
                                {"version", l.version()}});
    Json v{{"system", system}, {"distribution", display_name(lineage)}, {"lineage", lineage_json}};
    if (const auto* b = lineage.base()) v["luban_base"] = std::format("{} {}", b->name(), b->version());
    if (const auto* p = lineage.parent()) v["parent"] = p->ref;
    Json more = Json::array();
    for (const auto& e : extras) more.push_back({{"package", e.ref()}, {"role", e.role()}, {"index", e.index}});
    v["extras"] = more;
    Json beyond_json = Json::array();
    for (const auto& b : beyond) beyond_json.push_back({{"what", b.what}, {"text", b.text}, {"from", b.from}});
    v["beyond"] = beyond_json;
    v["needs_confirmation"] = needs_confirmation();
    return v;
}

std::expected<NewPreview, int> preview_new(const Context& c, const NewOptions& o) {
    auto fail = [&](std::string text, std::string hint, int code) -> std::expected<NewPreview, int> {
        if (c.sink) c.sink(error(std::move(text), std::move(hint)));
        return std::unexpected(code);
    };
    NewPreview p;
    std::vector<Coordinate> systems;
    for (const auto& given : o.xpkgs) {
        auto coord = describe_coordinate(c.port, given);
        if (!coord) return fail(coord.error(), std::format("xlings search {}", given.substr(0, given.find('@'))), 1);
        (coord->role() == "system" ? systems : p.extras).push_back(*coord);
    }
    if (systems.size() > 1)
        return fail(std::format("{} and {} are both systems: --xpkg takes one system", systems[0].given, systems[1].given),
                    std::format("luban new {} --xpkg {}", o.name, systems[0].given), 2);
    if (!o.edition.empty() && !systems.empty())
        return fail(std::format("{} is a system, and so is --edition {}: one system", systems[0].given, o.edition),
                    std::format("luban new {} --xpkg {}", o.name, systems[0].given), 2);
    if (o.edition.find(':') != std::string::npos)
        return fail(std::format("--edition takes an official short name; {} is a coordinate", o.edition),
                    std::format("luban new {} --xpkg {}", o.name, o.edition), 2);
    if (!systems.empty()) {
        // As given: an unversioned coordinate is resolved by xlings, which
        // says when it took an older version than the newest (B22).
        p.system = systems.front().given;
    } else {
        const auto edition = o.edition.empty() ? std::string("core") : o.edition;
        if (auto ref = official_system(c.port, edition)) p.system = *ref;
        else {
            // A desktop given as a system: the right way to have it (design §5.3).
            const auto short_name = edition.substr(0, edition.find('@'));
            if (official_desktop(c.port, short_name))
                return fail(std::format("{} is a desktop, not a system", short_name),
                            std::format("luban new {} --edition core --desktop {}", o.name, short_name), 2);
            std::string known;
            for (const auto& e : official_systems(c.port)) known += (known.empty() ? "" : ", ") + e;
            return fail(std::format("no edition '{}'{}", short_name, known.empty() ? std::string{} : " (the editions: " + known + ")"),
                        std::format("a desktop or a package goes into a system: luban new {} --edition core --xpkg <it>", o.name), 2);
        }
    }
    // Made here, or in a carrier's machine (a PC's WSL, a Mac's vz: asked
    // for, or the only place a Linux system is made on this host).
    const bool here = xlings::platform::is_linux && o.carrier.empty();
    if (!here) {
        // Its chain is read there, by the xlings that makes it (a recipe for
        // Linux has no build for this host); here, what was asked and where
        // it comes from.
        auto top = describe_coordinate(c.port, p.system);
        if (!top) return fail(top.error(), {}, 1);
        p.lineage.layers.push_back({.ref = top->ref(), .index = top->index});
    } else {
        auto lineage = plan_system(c.port, o.name, p.system);
        if (!lineage) return fail(lineage.error(), {}, 1);
        p.lineage = std::move(*lineage);
    }
    // Built on Luban (design §4.4): its chain reaches an official edition
    // (read here; a carrier's xlings reads it where it makes it).
    if (here && !p.lineage.base())
        return fail(std::format("{} is not built on a Luban edition: its `from` chain reaches none", p.system),
                    "a Luban system's chain ends on an official edition (luban:core, luban:desktop, ...)", 1);
    // What its manifests need of this luban (design §9).
    std::vector<opencaps::Need> needs;
    for (const auto& text : p.lineage.needs()) {
        if (auto n = opencaps::parse_need(text)) needs.push_back(*n);
        else needs.push_back({text, 1});
    }
    if (const auto bad = opencaps::unmet(needs, luban_provides()); !bad.empty())
        return fail(std::format("{} needs what this luban does not have: {}", p.system, bad.front().why()),
                    "xlings update luban", 1);
    p.beyond = beyond_luban(p.lineage);
    p.confirm_key = confirmation_key(p.lineage, p.extras);
    if (!p.confirm_key.empty()) {
        auto done = is_confirmed(c.home, p.confirm_key);
        if (!done) return fail(done.error(), {}, 1);
        p.confirmed = *done;
    }
    return p;
}

Outcome create(const Context& c, const NewOptions& o, const NewPreview* given) {
    if (o.name.empty()) {
        if (c.sink) c.sink(error("an environment needs a name", "luban new <name> --edition tiny"));
        return {.code = 2};
    }
    // A name in use is said in luban's words before anything is fetched (U17).
    if (describe(c, o.name)) {
        if (c.sink) c.sink(error(std::format("'{}' already exists", o.name),
                                 std::format("luban rm {}, or choose another name", o.name)));
        return {.code = 2};
    }
    // What it would be, checked before anything is made (design §5.3): an
    // edition that is not one is said here, never by xlings (E14).
    std::optional<NewPreview> made_preview;
    if (!given) {
        auto p = preview_new(c, o);
        if (!p) return {.code = p.error()};
        made_preview = std::move(*p);
    }
    const NewPreview& preview = given ? *given : *made_preview;
    // A third party's part, never confirmed: only when the caller asked (design §5.4).
    if (preview.needs_confirmation()) {
        if (!c.yes && !o.agreed) {
            if (c.sink) c.sink(error(std::format("{} is not Luban's own: what it does is to be agreed to first",
                                                 display_name(preview.lineage)),
                                     std::format("luban new {} ... at a terminal shows it; --yes agrees", o.name)));
            return {.code = 2};
        }
        if (auto r = record_confirmed(c.home, preview.confirm_key, display_name(preview.lineage)); !r) {
            if (c.sink) c.sink(error(r.error()));
            return {.code = 1};
        }
    }
    auto args = new_argv_(o.name, preview.system, o.carrier);
    if (!o.proxy.empty()) args.insert(args.end(), {"--proxy", o.proxy});
    // Ready is what the self-check says, not that making it finished.
    const bool checked = xlings::platform::is_linux && o.carrier.empty();
    // An older version of the edition than the index's newest, because that
    // one needs a newer xlings: said, with the update (E8, B22).
    std::string passed_over;
    Context making = c;
    making.sink = [&](const Event& e) {
        if (e.kind == Event::Kind::Data && (e.data_kind == "passed_over" || e.data_kind == "installed_only")) {
            const auto d = e.data.is_string() ? nlohmann::json::parse(e.data.get<std::string>(), nullptr, false) : e.data;
            if (d.is_object() && e.data_kind == "passed_over") passed_over = d.value("message", std::string{});
            else if (d.is_object() && c.sink) c.sink(notice(d.value("message", std::string{}), "xlings update"));
            return;
        }
        if (c.sink) c.sink(e);
    };
    const auto shown = display_name(preview.lineage);
    const int rc = step_(making, "make", std::format("making {} ({})", o.name, shown), args, false,
                         o.name + (checked ? " is made" : " is ready"));
    if (!passed_over.empty() && c.sink) c.sink(notice(passed_over, "xlings self update"));
    if (rc != 0) return {.code = rc};
    // A system that names itself says so in its os-release (design §4.5).
    if (const auto release = os_release(preview.lineage); !release.empty())
        if (auto root = root_of(c.home, o.name)) {
            std::error_code ec;
            fs::remove(*root / "etc" / "os-release", ec);
            xlings::platform::write_file_atomic((*root / "etc" / "os-release").string(), release);
        }
    // What was given besides the system: its desktops and packages (design §5.1).
    for (const auto& extra : preview.extras) {
        if (c.sink) c.sink(phase("install", "begin", std::format("installing {}", extra.ref())));
        const int irc = c.port.terminal({"install", extra.ref(), "--subos", o.name, "-y", "--agent"});
        if (c.sink) c.sink(phase("install", irc == 0 ? "done" : "failed", std::format("installing {}", extra.ref())));
        if (irc != 0) {
            if (c.sink) c.sink(error(std::format("{} is made; {} was not installed into it", o.name, extra.ref()),
                                     std::format("xlings install {} --subos {}", extra.ref(), o.name)));
            return {.code = irc};
        }
    }
    if (checked) {
        if (const auto failed = unusable_(c, o.name, "check", std::format("{} is ready", o.name))) {
            if (c.sink) c.sink(error(std::format("{} is made, and not usable: {}", o.name, *failed),
                                     std::format("luban status {} --check", o.name)));
            return {.code = 1};
        }
    }
    // The login shell its edition declares (its manifest's luban.settings, or
    // usr/share/luban/settings.json), before its user copies root's.
    preset_login_shell_(c, o.name, preview.lineage.luban("settings").value("login_shell", std::string{}));
    // Its login user (part 3 §6.1): in its passwd, its group and wheel, the
    // password's hash in its shadow, and the one `luban enter` logs in as.
    // A desktop is never root's: made from the window (no question asked
    // there), it gets the default user, without a password.
    const auto user = o.user.empty() && needs_user(preview) ? std::string(kDefaultUser) : o.user;
    if (!user.empty()) {
        const auto root = root_of(c.home, o.name);
        if (!root) {
            if (c.sink) c.sink(error(std::format("{} has no root to give a user", o.name)));
            return {.code = 1};
        }
        const auto rootUser = xlings::subos::root_etc::find_user(*root / "etc", "root");
        auto made = xlings::subos::root_etc::add_user(*root, user, rootUser ? rootUser->shell : std::string("/bin/sh"),
                                                      o.password_hash, xlings::platform::user_ids().uid);
        if (!made) {
            if (c.sink) c.sink(error(made.error()));
            return {.code = 1};
        }
        if (auto login = xlings::subos::root_etc::set_login_user(*root / "etc", user); !login) {
            if (c.sink) c.sink(error(login.error()));
            return {.code = 1};
        }
        if (c.sink) c.sink(notice(std::format("{} logs in as {}{}", o.name, user,
                                              o.password_hash.empty() ? " (no password)" : "")));
    }
    // The desktop asked for (--desktop), when it does not have it: the
    // official one of that name, installed -- the one thing luban new
    // installs unasked, and says (design §5.1).
    std::string desktop;
    for (const auto& [k, v] : o.settings) if (k == "desktop") desktop = v;
    if (!desktop.empty() && desktop != "none") {
        auto e = describe(c, o.name);
        if (e && std::ranges::find(e->desktops, desktop) == e->desktops.end()) {
            auto found = official_desktop(c.port, desktop);
            std::expected<Coordinate, std::string> official = std::unexpected(std::string{});
            if (found) official = describe_coordinate(c.port, *found);
            if (!official) {
                if (c.sink) c.sink(error(std::format("{} has no desktop {}, and Luban has none of that name", o.name, desktop),
                                         std::format("xlings install <its package> --subos {}", o.name)));
                return {.code = 1};
            }
            if (c.sink) c.sink(phase("install", "begin", std::format("installing {} (its desktop)", official->ref())));
            const int drc = c.port.terminal({"install", official->ref(), "--subos", o.name, "-y", "--agent"});
            if (c.sink) c.sink(phase("install", drc == 0 ? "done" : "failed", std::format("installing {} (its desktop)", official->ref())));
            if (drc != 0) return {.code = drc};
            if (c.sink) c.sink(notice(std::format("{} has {} as its desktop ({})", o.name, desktop, official->ref())));
        }
    }
    // What luban config takes, given now (design §5.1): the same function.
    if (!o.settings.empty())
        if (auto set = configure(c, o.name, o.settings); !set.ok()) return {.code = set.code};
    auto e = describe(c, o.name);
    if (!e) return {.code = 0};
    Json value = preview.value();
    value.update(Json{{"name", e->name}, {"edition", e->edition}, {"packages", e->packages},
                      {"user", user.empty() ? std::string("root") : user},
                      {"generation", e->generation ? Json(*e->generation) : Json()}, {"shell", e->shell},
                      {"desktops", e->desktops}});
    const auto plan = enter_plan(*e);
    if (plan.target == "desktop") value["desktop"] = plan.session;
    if (!passed_over.empty()) value["passed_over"] = passed_over;
    return {.code = 0, .value = std::move(value)};
}

Outcome configure(const Context& c, std::string_view name, const std::vector<std::pair<std::string, std::string>>& changes) {
    if (!exists_(c, name)) return missing_(c, name);
    std::vector<std::string> isolation;
    for (const auto& [key, value] : changes) {
        if (key == "mode" || key == "persist" || key == "desktop" || key == "on-close" || key == "enter") {
            auto s = set_setting(c.home, name, key, value);
            if (!s) {
                if (c.sink) c.sink(error(s.error()));
                return {.code = 2};
            }
            if (key == "mode" && value == "machine" && c.sink)
                c.sink(notice(std::format("{} now starts as a machine: luban enter {}", name, name)));
        } else if (key == "shell") {
            auto root = root_of(c.home, name);
            if (!root) {
                if (c.sink) c.sink(error(std::format("'{}' is a view of this host: its shell is yours ($SHELL)", name)));
                return {.code = 2};
            }
            std::string shell = value;
            if (shell.find('/') == std::string::npos) shell = "/usr/bin/" + shell;
            if (!root_has_(c, name, shell)) {
                if (c.sink) c.sink(error(std::format("{} has no {}", name, shell),
                                         std::format("xlings install {} --subos {}", fs::path(shell).filename().string(), name)));
                return {.code = 1};
            }
            if (auto set = xlings::subos::root_etc::set_login_shell(*root / "etc", "root", shell); !set) {
                if (c.sink) c.sink(error(set.error()));
                return {.code = 1};
            }
            if (const auto e = describe(c, name); e && e->running && c.sink)
                c.sink(notice(std::format("{} is running: a terminal that enters it now starts {}; one already "
                                          "in it keeps its shell (luban stop {} to restart everything)",
                                          name, shell, name)));
        } else if (key == "login") {
            // Who `luban enter` logs in as: root, or one of its users.
            const auto root = root_of(c.home, name);
            if (!root) {
                if (c.sink) c.sink(error(std::format("'{}' is a view of this host: you are you", name)));
                return {.code = 2};
            }
            if (auto set = xlings::subos::root_etc::set_login_user(*root / "etc", value); !set) {
                if (c.sink) c.sink(error(set.error(), std::format("luban config {} --login root", name)));
                return {.code = 1};
            }
        } else if (key == "runtime") {
            // Its default runtime is the one its generation carries (part 3
            // §4): that version made active in it, a new generation.
            if (value.find('@') == std::string::npos) {
                if (c.sink) c.sink(error(std::format("'{}' names no version", value), "luban config " + std::string(name) + " --runtime glibc@2.44.3"));
                return {.code = 2};
            }
            const auto spec = value.find(':') == std::string::npos ? "xim:" + value : value;
            if (c.sink) c.sink(phase("runtime", "begin", std::format("moving {} to {}", name, value)));
            const int rc = c.port.terminal({"install", spec, "--subos", std::string(name), "-y"});
            if (c.sink) c.sink(phase("runtime", rc == 0 ? "done" : "failed", std::format("moving {} to {}", name, value)));
            if (rc != 0) return {.code = rc};
        } else if (isolation_keys_().contains(key)) {
            if (key == "proxy") {
                if (value == "off" || value == "none") isolation.insert(isolation.end(), {"--net", "host"});
                else isolation.insert(isolation.end(), {"--net", "proxy", "--proxy", value});
            } else if (key == "policy") {
                isolation.insert(isolation.end(), {"--sandbox", value});
            } else {
                isolation.insert(isolation.end(), {"--" + key, value});
            }
        } else {
            if (c.sink) c.sink(error(std::format("no setting '{}'", key), "luban config --help"));
            return {.code = 2};
        }
    }
    if (!isolation.empty()) {
        auto args = argv_({"config", name});
        args.insert(args.end(), isolation.begin(), isolation.end());
        if (const int rc = step_(c, "config", std::format("changing how {} is isolated", name), args); rc != 0)
            return {.code = rc};
    }
    auto e = describe(c, name);
    if (!e) return {.code = 1};
    Json value{{"name", e->name}, {"mode", e->settings.mode}, {"persist", e->settings.persist},
               {"desktop", e->settings.desktop}, {"on_close", e->settings.on_close}, {"shell", e->shell}};
    if (auto iso = c.port.json(argv_({"subos", "config", name, "--json"})); iso) value["isolation"] = *iso;
    return {.code = 0, .value = value};
}

namespace {

// A root made before 2026.10.14 lives in this home's own prefix (its payloads
// shared with the host's SubOS, the host's user name in its paths). Its
// upgrade first moves it into the system domain (part 3 §9): carried as an
// archive -- its /etc, /home, /root, /var, /opt, /srv, its policy, settings
// and the packages it had beyond its edition -- removed, and made again
// there. The archive is kept until the new one stands, and named on failure.
// Where a move found this host could not build the domain: not tried again.
fs::path stays_here_(const Context& c, std::string_view name) {
    return c.home / "state" / "luban" / "relayout" / std::format("{}.stays", name);
}

bool old_layout_(const Context& c, std::string_view name) {
    if constexpr (!xlings::platform::is_linux) return false;
    std::error_code ec;
    if (fs::exists(stays_here_(c, name), ec)) return false;
    const auto e = describe(c, name);
    return e && e->kind == "root" && e->carrier == "local" && !e->edition.empty()
        && !xlings::subos::locate(HomeView{c.home}, name).domain;
}

int relayout_(const Context& c, std::string_view name) {
    if (const auto e = describe(c, name); e && e->running) {
        if (c.sink) c.sink(error(std::format("{} is running; it moves to its own userland when stopped", name),
                                 std::format("luban stop {}", name)));
        return 2;
    }
    const auto keep = c.home / "state" / "luban" / "relayout" / std::format("{}.luban", name);
    std::error_code ec;
    fs::create_directories(keep.parent_path(), ec);
    fs::remove(keep, ec);
    const auto text = std::format("moving {} into its own userland (/xlings)", name);
    if (c.sink) c.sink(phase("relayout", "begin", text));
    const auto quiet = Context{.home = c.home, .port = c.port, .sink = c.sink, .yes = true};
    if (export_environment(quiet, {.name = std::string(name), .file = keep, .format = "luban"}).code != 0
        || remove(quiet, name).code != 0) {
        if (c.sink) c.sink(phase("relayout", "failed", text));
        return 1;
    }
    if (const auto made = import_environment(quiet, keep, std::string(name)); made.code != 0) {
        if (c.sink) {
            c.sink(error(std::format("{} was carried to {} and could not be made again", name, keep.string()),
                         std::format("luban import {} --name {} -y", keep.string(), name)));
            c.sink(phase("relayout", "failed", text));
        }
        return made.code;
    }
    fs::remove(keep, ec);
    if (!xlings::subos::locate(HomeView{c.home}, name).domain) {
        // Made again in this home's prefix: this host cannot build the domain.
        std::ofstream(stays_here_(c, name)) << "this host could not build the system domain\n";
        if (c.sink) c.sink(notice(std::format("{} stays in this home's prefix: this host cannot build it its own "
                                              "(xlings self doctor --isolation)", name)));
    }
    if (c.sink) c.sink(phase("relayout", "done", text));
    return 0;
}

}  // namespace

namespace {

// The official edition whose manifest says it replaces `ref`'s package.
std::optional<std::string> successor_(const Context& c, std::string_view name, std::string_view ref) {
    const auto old = std::string(ref.substr(0, ref.find('@')));
    for (const auto& [short_name, candidate] : official_system_refs(c.port)) {
        if (!candidate.starts_with("luban:")) continue;
        auto lineage = plan_system(c.port, name, candidate);
        if (!lineage || lineage->layers.empty()) continue;
        const auto replaces = lineage->layers.front().manifest.value("replaces", Json());
        if ((replaces.is_string() && replaces.get<std::string>() == old)
            || (replaces.is_array() && std::ranges::find(replaces, Json(old)) != replaces.end()))
            return candidate;
    }
    return std::nullopt;
}

}  // namespace

Outcome upgrade(const Context& c, std::string_view name, std::string_view to, bool dry_run) {
    if (!exists_(c, name)) return missing_(c, name);
    if (old_layout_(c, name)) {
        if (dry_run) {
            if (c.sink) c.sink(notice(std::format("{} first moves into its own userland (/xlings): its /etc, /home, "
                                                  "/root, /var and its own packages come along", name)));
        } else if (!c.yes) {
            if (c.sink) c.sink(error(std::format("{} moves into its own userland (/xlings) first; its files come along",
                                                 name), std::format("luban upgrade {} -y", name)));
            return {.code = 2};
        } else if (const int rc = relayout_(c, name); rc != 0) {
            return {.code = rc};
        }
    }
    // What it shows and how it runs now, where the person did not choose:
    // kept when its new edition's defaults differ (design 2026-10-18 §15.2).
    const auto before = read_settings(c.home, name);
    auto args = argv_({"upgrade", name});
    std::string target(to);
    // A published name that is frozen (subos:luban-*) moves to the official
    // edition that says it replaces it (design §15.1): luban asks the index.
    if (target.empty())
        if (const auto e = describe(c, name); e && luban::compat::frozen_edition(e->edition))
            if (auto next = successor_(c, name, e->edition)) {
                if (c.sink) c.sink(notice(std::format("{} moves from {} to {}, which replaces it", name, e->edition, *next)));
                target = *next;
            }
    if (!target.empty()) args.insert(args.end(), {"--to", target});
    if (dry_run) args.push_back("--dry-run");
    // The plan comes back as data (subos_upgrade_plan); the rest goes on.
    Json plan = Json::object();
    const Sink watched = [&](const Event& e) {
        if (e.kind == Event::Kind::Data && e.data_kind == "subos_upgrade_plan") plan = e.data;
        else if (c.sink) c.sink(e);
    };
    const auto text = dry_run ? std::format("planning {}'s upgrade", name)
                              : std::format("moving {} to its edition's newest", name);
    if (c.sink) c.sink(phase("upgrade", "begin", text));
    xlings::observe::ops::Span span("upgrade");
    const int rc = c.port.subos(args, c.yes, watched);
    if (rc != 0) span.fail(std::format("exit {}", rc));
    const bool upToDate = rc == 0 && plan.value("up_to_date", false);
    const auto done = upToDate ? std::format("{} is up to date", name) : text;
    if (c.sink) c.sink(phase("upgrade", rc == 0 ? "done" : "failed", done));
    // A new generation that is not usable is not kept (L §11): the previous
    // one is current again.
    if (rc == 0 && !dry_run && !upToDate && xlings::platform::is_linux) {
        if (const auto failed = unusable_(c, name, "check", std::format("{} is upgraded", name))) {
            step_(c, "rollback", std::format("going back to {}'s previous generation", name), argv_({"rollback", name}));
            if (c.sink) c.sink(error(std::format("{}'s new generation is not usable ({}); it stays on the previous one",
                                                 name, *failed), std::format("luban history {}", name)));
            return {.code = 1, .value = plan};
        }
    }
    if (rc == 0 && !dry_run && !upToDate && before)
        if (const auto after = read_settings(c.home, name)) {
            const std::array<std::tuple<std::string_view, std::string_view, std::string, std::string>, 3> keys{{
                {"desktop", "desktop", before->desktop, after->desktop},
                {"enter", "enter", before->enter, after->enter},
                {"mode", "mode", before->mode, after->mode}}};
            for (const auto& [key, json_key, was, now] : keys) {
                if (was == now || was.empty() || before->document.contains(std::string(json_key))) continue;
                if (auto kept = set_setting(c.home, name, key, was); kept && c.sink)
                    c.sink(notice(std::format("{} keeps {} {} (its new edition's default is {}): luban config {} --{} {}",
                                              name, key, was, now.empty() ? "none" : now, name, key, now.empty() ? "none" : now)));
            }
        }
    return {.code = rc, .value = plan};
}

Outcome rollback(const Context& c, std::string_view name, std::string_view to) {
    if (!exists_(c, name)) return missing_(c, name);
    auto args = argv_({"rollback", name});
    if (!to.empty()) args.insert(args.end(), {"--to", std::string(to)});
    return {.code = step_(c, "rollback", std::format("going back a generation of {}", name), args)};
}

Outcome history(const Context& c, std::string_view name) {
    if (!exists_(c, name)) return missing_(c, name);
    Json rows = Json::array();
    bool answered = false;
    const Sink keep = [&](const Event& e) {
        if (e.kind == Event::Kind::Data && e.data_kind == "subos_generations") rows = e.data, answered = true;
        else if (e.kind == Event::Kind::Error && c.sink) c.sink(e);
    };
    int rc = c.port.subos(argv_({"rollback", name, "--list"}), false, keep);
    // "Could not read" is never "none": an answer without the list (an
    // xlings that printed it instead, design 2026-10-19 F13) is said.
    if (rc == 0 && !answered) {
        if (c.sink) c.sink(error(std::format("could not read the generations of {}: xlings answered without them", name),
                                 std::format("xlings subos rollback {} --list", name)));
        rc = 1;
    }
    return {.code = rc, .value = {{"generations", rows}}};
}

Outcome remove(const Context& c, std::string_view name) {
    if (!exists_(c, name)) return missing_(c, name);
    if (!c.yes) {
        if (c.sink) c.sink(error(std::format("removing {} deletes its home and every file in it", name),
                                 std::format("luban rm {} -y", name)));
        return {.code = 2};
    }
    const int rc = step_(c, "remove", std::format("removing {}", name), argv_({"remove", name}), true);
    if (rc == 0) {
        std::error_code ec;
        fs::remove(settings_file(c.home, name), ec);
    }
    return {.code = rc};
}

Outcome stop(const Context& c, std::string_view name) {
    if (!exists_(c, name)) return missing_(c, name);
    return {.code = step_(c, "stop", std::format("stopping {}", name), argv_({"stop", name}))};
}

Outcome boot(const Context& c, std::span<const std::string> args) {
    std::vector<std::string> argv{"__boot"};
    argv.insert(argv.end(), args.begin(), args.end());
    Json entries;
    const Sink keep = [&](const Event& e) {
        if (e.kind == Event::Kind::Data && e.data_kind == "subos_boot") entries = e.data;
        else if (c.sink) c.sink(e);
    };
    const int rc = c.port.subos(argv, false, keep);
    return {.code = rc, .value = entries.is_object() ? entries : Json::object()};
}

namespace {

// What a system runs on, for an archive made again without the network
// (design 2026-10-19 §9.3): every payload its generation links into and the
// editions it was made from, as <package dir>/<version> of the store -- and
// the index snapshot of the home it lives in.
std::vector<archive::Tree> carried_(const xlings::subos::Located& at, std::string_view name, const Json& instance) {
    std::map<std::string, fs::path> payloads;
    const auto add = [&](const fs::path& root) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) return;
        payloads.emplace((root.parent_path().filename() / root.filename()).generic_string(), root);
    };
    std::error_code ec;
    const auto usr = xlings::subos::rootfs::usr_of(at.home.instance(name));
    for (fs::recursive_directory_iterator it(usr, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_symlink(ec)) continue;
        auto target = fs::read_symlink(it->path(), ec);
        if (ec) { ec.clear(); continue; }
        if (target.is_relative()) target = (it->path().parent_path() / target).lexically_normal();
        if (auto root = xlings::store::payload_root(xlings::subos::physical(at, target))) add(*root);
    }
    // The editions it was made from, and the packages its templates declared
    // (a package with no program or library has no link in the generation).
    std::vector<std::string> refs;
    if (auto record = xlings::subos::edition::read(instance)) {
        refs = record->chain;
        refs.push_back(record->ref);
    }
    if (const auto declared = read_json_(at.home.instance(name) / ".xlings.json").value("packages", Json::array());
        declared.is_array())
        for (const auto& p : declared)
            if (p.is_string()) refs.push_back(p.get<std::string>());
    {
        for (const auto& ref : refs) {
            const auto colon = ref.find(':'), at_ = ref.rfind('@');
            if (colon == std::string::npos || at_ == std::string::npos || at_ < colon) continue;
            add(at.home.home / "data" / "xpkgs" / std::format("{}-x-{}", ref.substr(0, colon), ref.substr(colon + 1, at_ - colon - 1))
                / ref.substr(at_ + 1));
        }
    }
    std::vector<archive::Tree> trees;
    for (const auto& [key, dir] : payloads) trees.push_back({"payloads/" + key, dir});
    // The indexes: the default, its sub-indexes, and each one the home names
    // (index_repos keeps its data under its own name).
    std::set<std::string> indexes{"xim-pkgindex", "xim-index-repos"};
    if (const auto repos = read_json_(at.home.home / ".xlings.json").value("index_repos", Json::array()); repos.is_array())
        for (const auto& r : repos)
            if (r.is_object() && r.value("name", "") != "xim" && !r.value("name", "").empty()) indexes.insert(r.value("name", ""));
    // An index kept as a link to a checkout (a local index) is carried as
    // its content.
    for (const auto& index : indexes)
        if (fs::is_directory(at.home.home / "data" / index, ec))
            trees.push_back({"index/" + index, fs::canonical(at.home.home / "data" / index, ec)});
    return trees;
}

}  // namespace

std::string export_format(std::string_view file) {
    const auto ends = [&](std::string_view s) { return file.ends_with(s); };
    if (ends("/")) return "dir";
    if (ends(".iso")) return "iso";
    if (ends(".img") || ends(".raw")) return "img";
    if (ends(".qcow2")) return "qcow2";
    if (ends(".tar") || ends(".tar.gz") || ends(".tgz") || ends(".tar.zst") || ends(".tar.xz")) return "tar";
    if (ends(".luban")) return "luban";
    return {};
}

Outcome export_environment(const Context& c, const ExportOptions& o) {
    if (!exists_(c, o.name)) return missing_(c, o.name);
    // A system in the Luban host machine (a Mac, a PC) is written out there,
    // not here, in this version: said before anything is made.
    if (const auto instance = read_json_(HomeView{c.home}.instance_file(o.name));
        instance.value("carrier", std::string("local")) != "local") {
        if (c.sink) c.sink(error(std::format("'{}' lives in the Luban host machine; exporting it from this computer is not done in this version", o.name),
                                 "export it on a Linux computer (luban export there), or carry it with luban export on Linux and luban import"));
        return {.code = 2};
    }
    const auto format = o.format.empty() ? export_format(o.file.generic_string()) : o.format;
    if (format.empty()) {
        if (c.sink) c.sink(error(std::format("cannot tell the format from '{}'", o.file.string()),
                                 "name it .iso, .img, .qcow2, .tar(.zst), .luban or end it with / (a directory), or give --format"));
        return {.code = 2};
    }
    if (format == "luban") {
        auto root = root_of(c.home, o.name);
        if (!root) {
            if (c.sink) c.sink(error(std::format("'{}' is a view of this host; an archive carries a root", o.name)));
            return {.code = 2};
        }
        if (c.sink) c.sink(phase("archive", "begin", std::format("writing {}", o.file.string())));
        const auto at = xlings::subos::locate(HomeView{c.home}, o.name);
        const HomeView& view = at.home;
        const auto instance = read_json_(view.instance_file(o.name));
        Json manifest{{"format", std::string(archive::kFormat)}, {"version", archive::kVersion},
                      {"name", o.name}, {"kind", "root"}};
        if (auto record = xlings::subos::edition::read(instance)) manifest["edition"] = xlings::subos::edition::to_json(*record);
        const auto workspace = read_json_(view.instance(o.name) / ".xlings.json");
        manifest["packages"] = workspace.contains("workspace") ? workspace["workspace"] : Json::object();
        if (auto pol = xlings::subos::policy_store::read(view, o.name); pol && pol->has_value())
            manifest["policy"] = xlings::subos::policy::to_json(**pol);
        if (auto s = read_settings(c.home, o.name)) manifest["settings"] = s->document;
        // What it runs on and the index naming it: made again offline.
        const auto trees = carried_(at, o.name, instance);
        Json carries{{"payloads", Json::array()}, {"index", false}};
        for (const auto& t : trees) {
            if (t.prefix.starts_with("payloads/")) carries["payloads"].push_back(t.prefix.substr(9));
            else carries["index"] = true;
        }
        manifest["carries"] = carries;
        auto written = archive::write(o.file, manifest, *root, at.logical, trees);
        if (!written) {
            if (c.sink) {
                c.sink(error(written.error()));
                c.sink(phase("archive", "failed"));
            }
            return {.code = 1};
        }
        if (c.sink) c.sink(phase("archive", "done", std::format("{} ({} entries)", o.file.string(), *written)));
        return {.code = 0, .value = {{"file", o.file.string()}, {"entries", *written}}};
    }
    static const std::map<std::string, std::string, std::less<>> flag_of{
        {"iso", "--iso"}, {"img", "--drive"}, {"qcow2", "--qcow2"}, {"tar", "--tar"}, {"dir", "--rootfs"}};  // tool-ok: format names
    auto f = flag_of.find(format);
    if (f == flag_of.end()) {
        if (c.sink) c.sink(error(std::format("no format '{}'", format), "iso, img, qcow2, tar, dir or luban"));
        return {.code = 2};
    }
    const bool machine_image = format == "iso" || format == "img" || format == "qcow2";
    auto args = argv_({machine_image ? "__export" : "export", o.name, f->second, o.file.string()});
    if (!o.size.empty()) args.insert(args.end(), {"--size", o.size});
    if (!o.boot.empty()) args.insert(args.end(), {"--boot", o.boot});
    const int rc = step_(c, "export", std::format("making {} of {}", o.file.string(), o.name), args);
    return {.code = rc, .value = {{"file", o.file.string()}, {"format", format}}};
}

Outcome import_environment(const Context& c, const fs::path& file, std::string name) {
    auto manifest = archive::read(file);
    if (!manifest) {
        if (c.sink) c.sink(error(manifest.error()));
        return {.code = 1};
    }
    if (name.empty()) name = manifest->value("name", "");
    if (exists_(c, name)) {
        if (c.sink) c.sink(error(std::format("there is already an environment '{}'", name),
                                 std::format("luban import {} --name <another>", file.string())));
        return {.code = 2};
    }
    std::string edition;
    if (auto e = manifest->find("edition"); e != manifest->end() && e->is_object()) edition = e->value("ref", "");
    if (edition.empty()) {
        if (c.sink) c.sink(error(std::format("{} names no edition to make it from", file.string())));
        return {.code = 1};
    }
    // Its isolation, before anything is made: never looser than the edition's
    // without the user's word (an upgrade never loosens either).
    std::optional<xlings::subos::policy::Policy> wanted;
    if (auto p = manifest->find("policy"); p != manifest->end() && p->is_object()) {
        auto parsed = xlings::subos::policy::from_json(*p);
        if (!parsed) {
            if (c.sink) c.sink(error(std::format("{}: its policy cannot be read: {}", file.string(), parsed.error())));
            return {.code = 1};
        }
        wanted = *parsed;
    }
    // What it carries -- its payloads, the index naming them -- is where it
    // is made before anything is fetched (design 2026-10-19 §9.3): an
    // archive is imported without the network.
    auto args = new_argv_(name, edition, {});
    fs::path carried;
    if (auto carries = manifest->find("carries"); carries != manifest->end() && carries->is_object()) {
        carried = c.home / "tmp" / std::format("import-{}-{}", name, xlings::platform::current_pid());
        std::error_code ec;
        fs::remove_all(carried, ec);
        auto payloads = archive::extract_tree(file, "payloads", carried / "payloads");
        auto index = payloads ? archive::extract_tree(file, "index", carried / "index") : payloads;
        if (!payloads || !index) {
            if (c.sink) c.sink(error(std::format("{}: what it carries cannot be unpacked: {}", file.string(),
                                                 (!payloads ? payloads : index).error())));
            fs::remove_all(carried, ec);
            return {.code = 1};
        }
        args.insert(args.end(), {"--offline-from", carried.string()});
    }
    const int made = step_(c, "make", std::format("making {} ({})", name, edition), args);
    if (!carried.empty()) {
        std::error_code ec;
        fs::remove_all(carried, ec);
    }
    if (made != 0) return {.code = made};
    const HomeView view = xlings::subos::locate(HomeView{c.home}, name).home;
    if (wanted) {
        auto now = xlings::subos::policy_store::read(view, name);
        const auto base = now && now->has_value() ? **now : xlings::subos::policy::legacy();
        const auto looser = xlings::subos::policy::loosened(base, *wanted);
        if (!looser.empty() && !c.yes) {
            std::string what;
            for (const auto& l : looser) what += (what.empty() ? "" : ", ") + l;
            if (c.sink) c.sink(error(std::format("{} asks for less isolation than its edition ({}); {} was made with the "
                                                 "edition's", file.string(), what, name),
                                     std::format("luban import {} --name {} -y (after `luban rm {} -y`)", file.string(), name, name)));
        } else if (auto w = xlings::subos::policy_store::write(view, name, *wanted); !w && c.sink) {
            c.sink(error(w.error()));
        }
    }
    // The packages it had beyond its edition's, at their versions.
    if (auto packages = manifest->find("packages"); packages != manifest->end() && packages->is_object()) {
        const auto have = read_json_(view.instance(name) / ".xlings.json").value("workspace", Json::object());
        for (auto it = packages->begin(); it != packages->end(); ++it) {
            const auto pkg = it.key();
            const auto& version = it.value();
            if (!version.is_string() || (have.contains(pkg) && have[pkg] == version)) continue;
            const auto spec = std::format("{}@{}", pkg, version.get<std::string>());
            if (c.sink) c.sink(phase("install", "begin", std::format("installing {}", spec)));
            std::vector<std::string> install{"install", spec, "--subos", name, "-y", "--agent"};
            const int rc = c.port.terminal(install);
            if (c.sink) c.sink(phase("install", rc == 0 ? "done" : "failed", std::format("installing {}", spec)));
        }
    }
    // Its own state, over the fresh one; the persona stays the new one's.
    if (auto root = root_of(c.home, name)) {
        if (c.sink) c.sink(phase("state", "begin", "placing its /etc, /home and the rest"));
        auto placed = archive::extract_state(file, *root);
        if (!placed) {
            if (c.sink) {
                c.sink(error(placed.error()));
                c.sink(phase("state", "failed", "placing its /etc, /home and the rest"));
            }
            return {.code = 1};
        }
        if (c.sink) c.sink(phase("state", "done", std::format("{} entries placed", *placed)));
    }
    if (auto s = manifest->find("settings"); s != manifest->end() && s->is_object()) {
        for (auto it = s->begin(); it != s->end(); ++it)
            if (it.value().is_string())
                (void)set_setting(c.home, name, it.key() == "on_close" ? "on-close" : it.key(), it.value().get<std::string>());
    }
    return {.code = 0, .value = {{"name", name}, {"edition", edition}}};
}

bool chinese_locale() {
    for (const char* key : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (const char* v = std::getenv(key); v && *v) return std::string_view(v).starts_with("zh");
    return false;
}

namespace {

// Columns a line takes: a wide (CJK) character two, everything else one.
std::size_t columns_(std::string_view s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        char32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < len && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        n += (cp >= 0x1100 && (cp <= 0x115F || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3)
                               || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F)
                               || (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6))) ? 2 : 1;
        i += static_cast<std::size_t>(len);
    }
    return n;
}

// At most `width` columns: the middle of a long value goes, its ends stay.
std::string fit_(std::string s, std::size_t width, bool utf8) {
    if (columns_(s) <= width) return s;
    const std::string dots = utf8 ? "…" : "...";
    const std::size_t keep = width > 4 ? (width - (utf8 ? 1 : 3)) / 2 : 1;
    return s.substr(0, keep) + dots + s.substr(s.size() - keep);
}

}  // namespace

WelcomeStyle terminal_style() {
    WelcomeStyle st;
    st.zh = chinese_locale();
    std::string locale;
    for (const char* key : {"LC_ALL", "LC_CTYPE", "LANG"})
        if (const char* v = std::getenv(key); v && *v) { locale = v; break; }
    std::ranges::transform(locale, locale.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    st.utf8 = locale.empty() || locale.find("utf-8") != std::string::npos || locale.find("utf8") != std::string::npos;
    const char* no = std::getenv("NO_COLOR");
    const char* lc = std::getenv("LUBAN_COLOR");
    const std::string term = std::getenv("TERM") ? std::getenv("TERM") : "";
    const bool known = term.starts_with("xterm") || term.starts_with("screen") || term.starts_with("tmux")
                    || term == "linux" || term.starts_with("rxvt") || term == "vt220" || term.find("color") != std::string::npos
                    || term == "alacritty" || term == "foot" || term.starts_with("kitty") || term == "wezterm";
    st.color = known && !(no && *no) && !(lc && std::string_view(lc) == "0") && xlings::platform::stdout_is_terminal();
    return st;
}

std::string kind_mark(std::string_view mode, std::string_view persist) {
    const auto keep = persist == "none" ? std::string(":temp") : persist == "home" ? std::string(":home") : std::string();
    return mode == "machine" ? "<luban" + keep + ">" : "[luban" + keep + "]";
}

std::vector<std::string> welcome(const Environment& e, std::string_view mode, bool temp,
                                 const WelcomeStyle& style, const WelcomeFacts& facts) {
    constexpr std::size_t kWidth = 72, kLabel = 9;
    const bool zh = style.zh;
    const auto paint = [&](std::string_view code, const std::string& text) {
        return style.color ? std::format("\x1b[{}m{}\x1b[0m", code, text) : text;
    };
    std::string edition = e.edition;
    edition = std::string(luban::compat::without_frozen_prefix(edition));
    if (const auto at = edition.find('@'); at != std::string::npos) edition[at] = ' ';
    const auto persist = temp ? std::string("none") : e.settings.persist;
    const auto mark = kind_mark(mode, persist);
    const auto mark_color = persist != "all" ? (persist == "none" ? "31" : "33") : mode == "machine" ? "35" : "36";
    std::vector<std::string> out;
    out.push_back(std::format(" {} · {} {}", paint("1", std::format("Luban OS {}", kVersion)), paint(mark_color, mark), e.name));
    const std::string rule = " " + [&] {
        std::string r;
        for (std::size_t i = 0; i < 58; ++i) r += style.utf8 ? "─" : "-";
        return r;
    }();
    out.push_back(rule);
    const auto row = [&](std::string_view en, std::string_view cn, std::string value) {
        std::string label(zh ? cn : en);
        const auto pad = kLabel > columns_(label) ? kLabel - columns_(label) : 1;
        label = paint("2", label) + std::string(pad, ' ');
        out.push_back(" " + label + fit_(std::move(value), kWidth - kLabel - 1, style.utf8));
    };
    std::string system = edition.empty() ? std::string(zh ? "(未知的版本)" : "(no edition)") : edition;
    if (e.generation) system += (zh ? std::format(" · 第 {} 代", *e.generation) : std::format(" · generation {}", *e.generation));
    system += (zh ? std::format(" · {} 个包", e.packages) : std::format(" · {} packages", e.packages));
    row("system", "系统", system);
    if (mode == "machine") {
        auto kernel = facts.kernel.empty() ? std::string("Linux") : facts.kernel;
        if (!facts.machine.empty()) kernel += ", " + facts.machine;
        row("runs as", "运行", (zh ? std::format("机器：自己的内核（{}）", kernel) : std::format("machine: its own kernel ({})", kernel)));
    } else {
        const auto kernel = facts.kernel.empty() ? std::string("Linux") : facts.kernel;
        row("runs as", "运行", (zh ? std::format("沙箱：与本机共用内核（{}）", kernel) : std::format("sandbox: shares this host's kernel ({})", kernel)));
    }
    row("keeps", "保留", persist == "all" ? std::string(zh ? "所有改动" : "everything")
                       : persist == "home" ? std::string(zh ? "只有 /home" : "only /home")
                                           : std::string(zh ? "什么也不留：停下后写入的都会消失" : "nothing: what you write is gone when it stops"));
    row("user", "用户", facts.user + " · " + facts.user_home);
    row("xlings", "xlings", zh ? "只管理这个系统（R）" : "manages this system only (R)");
    const auto now = std::chrono::floor<std::chrono::minutes>(std::chrono::system_clock::now());
    row("time", "时间", std::format("{:%Y-%m-%d %H:%M} UTC", now));
    if (!facts.network.empty()) row("network", "网络", facts.network);
    out.push_back(rule);
    std::string next = mode == "machine"
        ? std::string(zh ? "exit 离开 · Ctrl-] d 让它继续运行 · Ctrl-] p 关机" : "exit to leave · Ctrl-] d leaves it running · Ctrl-] p powers it off")
        : (zh ? std::format("exit 离开 · luban status {}", e.name) : std::format("exit to leave · luban status {}", e.name));
    out.push_back(" " + next);
    out.push_back("");
    // A locale that is not UTF-8 gets ASCII only.
    if (!style.utf8)
        for (auto& line : out)
            for (std::size_t at; (at = line.find("·")) != std::string::npos;) line.replace(at, std::string_view("·").size(), "-");
    return out;
}

int enter_sandbox(const Context& c, std::string_view name, bool temp, std::string_view user) {
    std::vector<std::string> args{"subos", "use", std::string(name)};
    if (!user.empty()) args.insert(args.end(), {"--user", std::string(user)});
    auto settings = read_settings(c.home, name);
    const auto persist = temp ? std::string("none") : settings ? settings->persist : std::string("all");
    if (persist != "all") args.insert(args.end(), {"--persist", persist});
    if (auto e = describe(c, name)) {
        WelcomeFacts facts;
        if (const auto release = xlings::platform::kernel_release(); !release.empty()) facts.kernel = "Linux " + release;
        if (const auto root = root_of(c.home, name)) {
            const auto who = user.empty() ? xlings::subos::root_etc::login_user(*root / "etc") : std::string(user);
            if (const auto u = xlings::subos::root_etc::find_user(*root / "etc", who)) {
                facts.user = u->name;
                facts.user_home = u->home;
            }
        }
        for (const auto& line : welcome(*e, "sandbox", temp, terminal_style(), facts)) std::println("{}", line);
        xlings::platform::set_env_variable("XLINGS_ENTER_QUIET", "1");
    }
    return c.port.terminal(args);
}

int run_command(const Context& c, std::string_view name, std::span<const std::string> argv, std::string_view user) {
    std::vector<std::string> args{"subos", "exec", std::string(name)};
    if (!user.empty()) args.insert(args.end(), {"--user", std::string(user)});
    args.push_back("--");
    args.insert(args.end(), argv.begin(), argv.end());
    return c.port.terminal(args);
}

}  // namespace luban::service
