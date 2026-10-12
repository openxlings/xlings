module luban.service;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.observe;
import xlings.subos.home_view;
import xlings.subos.edition;
import xlings.subos.policy;
import xlings.subos.policy_store;
import xlings.subos.root_etc;
import xlings.subos.rootfs;
import xlings.subos.session;
import luban.service.archive;

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

std::vector<std::string> argv_(std::initializer_list<std::string_view> words) {
    std::vector<std::string> out;
    for (auto w : words) out.emplace_back(w);
    return out;
}

const std::set<std::string, std::less<>>& isolation_keys_() {
    static const std::set<std::string, std::less<>> value{"policy", "proxy", "tz", "allow", "disallow",
                                                          "mount", "unmount", "fetch", "observe"};
    return value;
}

}  // namespace

std::span<const std::string_view> official_editions() {
    static constexpr std::array<std::string_view, 7> value{"nano", "tiny", "tiny-musl", "core", "desktop", "desktop-xdesk", "agent-workspace"};
    return value;
}

std::expected<EditionAvailability, std::string> edition_availability(const Context& c, std::string_view edition) {
    std::vector<Json> data;
    std::string error;
    bool not_found = false;
    const int rc = c.port.capability("package_info", Json{{"target", edition_ref(edition)}},
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

std::string edition_ref(std::string_view edition) {
    if (edition.empty()) return "subos:luban-core";
    if (edition.find(':') != std::string_view::npos) return std::string(edition);
    const auto at = edition.find('@');
    const auto name = edition.substr(0, at);
    const auto version = at == std::string_view::npos ? std::string_view{} : edition.substr(at);
    if (std::ranges::find(official_editions(), name) != official_editions().end())
        return std::format("subos:luban-{}{}", name, version);
    if (name.starts_with("luban-")) return std::format("subos:{}{}", name, version);
    return std::string(edition);
}

std::expected<Environment, std::string> describe(const Context& c, std::string_view name) {
    if (!exists_(c, name)) return std::unexpected(std::format("there is no environment '{}'", name));
    const HomeView view{c.home};
    Environment e;
    e.name = std::string(name);
    const auto instance = read_json_(view.instance_file(name));
    e.kind = instance.value("kind", "view") == "rootfs" ? "root" : "view";
    e.carrier = instance.value("carrier", "local");
    std::set<std::string> packages;
    if (auto record = xlings::subos::edition::read(instance)) {
        e.edition = record->ref;
        for (const auto& [pkg, _] : record->packages) packages.insert(xlings::subos::edition::split(pkg).first);
    }
    const auto workspace = read_json_(view.instance(name) / ".xlings.json");
    // The packages it was given (its file's `packages`), not the names they
    // register (a glibc alone registers dozens).
    if (auto declared = workspace.find("packages"); declared != workspace.end() && declared->is_array())
        for (const auto& spec : *declared)
            if (spec.is_string()) packages.insert(xlings::subos::edition::split(spec.get<std::string>()).first);
    e.packages = packages.size();
    if (e.kind == "root") {
        e.generation = xlings::subos::rootfs::current(view.instance(name));
        if (auto root = root_of(c.home, name)) e.shell = xlings::subos::root_etc::login_shell(*root);
    }
    e.running = xlings::subos::session::find(view, name).has_value();
    auto settings = read_settings(c.home, name);
    if (!settings) return std::unexpected(settings.error());
    e.settings = *settings;
    return e;
}

std::expected<std::vector<Environment>, std::string> list(const Context& c) {
    std::vector<Environment> out;
    std::error_code ec;
    for (fs::directory_iterator it(HomeView{c.home}.subos_root(), ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name == "default" || name == "current" || name.starts_with('.') || !it->is_directory(ec)) continue;
        auto e = describe(c, name);
        if (!e) return std::unexpected(e.error());
        out.push_back(std::move(*e));
    }
    std::ranges::sort(out, {}, &Environment::name);
    return out;
}

Outcome create(const Context& c, const NewOptions& o) {
    if (o.name.empty()) {
        if (c.sink) c.sink(error("an environment needs a name", "luban new <name> --edition tiny"));
        return {.code = 2};
    }
    auto args = argv_({"new", o.name, "--from", edition_ref(o.edition)});
    if (!o.proxy.empty()) args.insert(args.end(), {"--proxy", o.proxy});
    if (!o.carrier.empty()) args.insert(args.end(), {"--carrier", o.carrier});
    const int rc = step_(c, "make", std::format("making {} ({})", o.name, o.edition), args, false,
                         std::format("{} is ready", o.name));
    if (rc != 0) return {.code = rc};
    auto e = describe(c, o.name);
    if (!e) return {.code = 0};
    return {.code = 0, .value = {{"name", e->name}, {"edition", e->edition}, {"packages", e->packages},
                                 {"generation", e->generation ? Json(*e->generation) : Json()}, {"shell", e->shell}}};
}

Outcome configure(const Context& c, std::string_view name, const std::vector<std::pair<std::string, std::string>>& changes) {
    if (!exists_(c, name)) return missing_(c, name);
    std::vector<std::string> isolation;
    for (const auto& [key, value] : changes) {
        if (key == "mode" || key == "persist" || key == "desktop" || key == "on-close") {
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
            std::error_code ec;
            if (fs::symlink_status(*root / fs::path(shell).relative_path(), ec).type() == fs::file_type::not_found) {
                if (c.sink) c.sink(error(std::format("{} has no {}", name, shell),
                                         std::format("xlings install {} --subos {}", fs::path(shell).filename().string(), name)));
                return {.code = 1};
            }
            if (auto set = xlings::subos::root_etc::set_login_shell(*root / "etc", "root", shell); !set) {
                if (c.sink) c.sink(error(set.error()));
                return {.code = 1};
            }
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

Outcome upgrade(const Context& c, std::string_view name, std::string_view to, bool dry_run) {
    if (!exists_(c, name)) return missing_(c, name);
    auto args = argv_({"upgrade", name});
    if (!to.empty()) args.insert(args.end(), {"--to", std::string(to)});
    if (dry_run) args.push_back("--dry-run");
    return {.code = step_(c, "upgrade", std::format("moving {} to its edition's newest", name), args, c.yes)};
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
    std::vector<Json> data;
    const Sink keep = [&](const Event& e) {
        if (e.kind == Event::Kind::Data && e.data_kind == "subos_generations") rows = e.data;
        else if (e.kind == Event::Kind::Error && c.sink) c.sink(e);
    };
    const int rc = c.port.subos(argv_({"rollback", name, "--list"}), false, keep);
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
        const HomeView view{c.home};
        const auto instance = read_json_(view.instance_file(o.name));
        Json manifest{{"format", std::string(archive::kFormat)}, {"version", archive::kVersion},
                      {"name", o.name}, {"kind", "root"}};
        if (auto record = xlings::subos::edition::read(instance)) manifest["edition"] = xlings::subos::edition::to_json(*record);
        const auto workspace = read_json_(view.instance(o.name) / ".xlings.json");
        manifest["packages"] = workspace.contains("workspace") ? workspace["workspace"] : Json::object();
        if (auto pol = xlings::subos::policy_store::read(view, o.name); pol && pol->has_value())
            manifest["policy"] = xlings::subos::policy::to_json(**pol);
        if (auto s = read_settings(c.home, o.name)) manifest["settings"] = s->document;
        auto written = archive::write(o.file, manifest, *root);
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
    if (const int rc = step_(c, "make", std::format("making {} ({})", name, edition), argv_({"new", name, "--from", edition}));
        rc != 0)
        return {.code = rc};
    const HomeView view{c.home};
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

int enter_sandbox(const Context& c, std::string_view name, bool temp) {
    std::vector<std::string> args{"subos", "use", std::string(name)};
    auto settings = read_settings(c.home, name);
    const auto persist = temp ? std::string("none") : settings ? settings->persist : std::string("all");
    if (persist != "all") args.insert(args.end(), {"--persist", persist});
    return c.port.terminal(args);
}

int run_command(const Context& c, std::string_view name, std::span<const std::string> argv) {
    std::vector<std::string> args{"subos", "exec", std::string(name), "--"};
    args.insert(args.end(), argv.begin(), argv.end());
    return c.port.terminal(args);
}

}  // namespace luban::service
