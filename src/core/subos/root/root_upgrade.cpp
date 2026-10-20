// `subos` commands over a root's life (design part 2): upgrade along its edition,
// rollback to a generation, its self-check, a boot's record, and what differs.
module xlings.core.subos;

import std;
import xlings.core.config;
import xlings.core.home;
import xlings.core.home.prefix_domain;
import xlings.core.home.domain_producer;
import xlings.core.home.domain_producer_source;
import xlings.core.subos.store_closure;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.platform.stream;
import xlings.core.profile;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.libs.elfread;
import xlings.core.subos.root;
import xlings.subos.home_view;
import xlings.subos.policy_store;
import xlings.subos.policy;
import xlings.subos.rootfs;
import xlings.subos.root_etc;
import xlings.subos.root_check;
import xlings.subos.library_cache;
import xlings.subos.roles;
import xlings.subos.boot;
import xlings.observe;
import xlings.core.subos.ports;
import xlings.core.xself;
import xlings.core.xim.extract;
import xlings.core.xim.commands;
import xlings.core.xim.catalog;
import xlings.image;
import xlings.platform.target;
import xlings.subos.tools;
import xlings.subos.caps;
import xlings.subos.edition;
import xlings.core.version_order;
import xlings.core.confirm;
import :root_shared;

namespace xlings::subos {

using namespace rootcmd_;

namespace {
namespace rf = xlings::subos::rootfs;
namespace bt = xlings::subos::boot;
}  // namespace

// ── upgrade ──────────────────────────────────────────────────────────
//
// `subos upgrade <name> [--to <version>] [--dry-run] [-y]` (Luban OS design
// part 2 §3.3): an environment made from an edition moves to a newer version
// of it. One new generation (rollback undoes the packages); the packages the
// user moved stay theirs; the ones the edition dropped stay installed; the
// edition's policy is applied only when it allows nothing more than the one
// in force -- a looser one is the owner's explicit `subos config`.

namespace {

// A template's manifest and the `from` chain below it, as refs and
// manifests, top first -- read, never copied anywhere.
struct Chain {
    std::vector<std::pair<std::string, fs::path>> templates;   // ref, payload dir
    std::vector<std::string> packages;                          // merged, as declared
    std::string policy;
};

std::optional<Chain> chain_of_(const std::string& ref, EventStream& stream) {
    Chain c;
    std::map<std::string, std::string> by_key;
    std::vector<std::string> order;
    std::vector<nlohmann::json> manifests;
    for (std::string from = ref; !from.empty();) {
        if (c.templates.size() > 8) {
            error_(stream, "the `from` chain is longer than 8: a cycle?");
            return std::nullopt;
        }
        const auto dir = resolve_base_package_(from, stream);
        if (dir.empty()) return std::nullopt;
        std::string resolved = from;
        if (from.find('@') == std::string::npos) resolved += "@" + dir.filename().string();
        c.templates.emplace_back(resolved, dir);
        manifests.push_back(read_json_(dir / ".xlings.json"));
        from = manifests.back().value("from", std::string());
    }
    for (auto it = manifests.rbegin(); it != manifests.rend(); ++it)   // bottom first
        for (auto& p : it->value("packages", nlohmann::json::array())) {
            if (!p.is_string()) continue;
            const auto key = package_key_(p.get<std::string>());
            if (!by_key.contains(key)) order.push_back(key);
            by_key[key] = p.get<std::string>();
        }
    for (auto& k : order) c.packages.push_back(by_key[k]);
    for (const auto& m : manifests)
        if (auto p = m.find("policy"); p != m.end() && p->is_string()) { c.policy = p->get<std::string>(); break; }
    return c;
}

}  // namespace

// `subos new <name> --from <ref> --dry-run` (design 2026-10-18 §5.3): what a
// system made from `ref` would be -- each layer of its `from` chain with the
// index it resolves in and its manifest as written (a `luban` object
// included), the packages merged, the policy -- read from the templates (small
// xpkgs, installed when missing), nothing made. A program reads it as data
// (`subos_plan`); a person reads the layers.
int plan_from_(const std::string& ref, EventStream& stream) {
    auto chain = chain_of_(ref, stream);
    if (!chain) return 1;
    nlohmann::json layers = nlohmann::json::array();
    for (const auto& [resolved, dir] : chain->templates)
        layers.push_back({{"ref", resolved}, {"index", xim::index_repo_of(resolved).value_or("")},
                          {"manifest", read_json_(dir / ".xlings.json")}});
    nlohmann::json plan{{"layers", layers}, {"packages", chain->packages}};
    if (!chain->policy.empty()) plan["policy"] = chain->policy;
    stream.emit(DataEvent{"subos_plan", plan.dump()});
    for (const auto& l : layers)
        log::info("{}{}", l.value("ref", std::string{}),
                  l.value("index", std::string{}).empty() ? std::string{} : "  (" + l.value("index", std::string{}) + ")");
    log::info("{} package(s){}; nothing was made", chain->packages.size(),
              chain->policy.empty() ? std::string{} : ", policy " + chain->policy);
    return 0;
}

namespace {

std::vector<std::string> configured_keys_(const fs::path& home, const std::string& name) {
    std::vector<std::string> out;
    const auto configured = read_json_(HomeView{home}.instance(name) / ".xlings.json")
                                .value("configured", nlohmann::json::object());
    for (auto it = configured.begin(); it != configured.end(); ++it) out.push_back(it.key());
    return out;
}

}  // namespace

int run_upgrade_(int argc, char* argv[], EventStream& stream, const UsageError& usageError, bool yes) {
    std::string name, to;
    bool dry = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--to" && i + 1 < argc) to = argv[++i];
        else if (a == "--dry-run") dry = true;
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
        else { usageError("unknown option for `xlings subos upgrade`: " + a); return 1; }
    }
    if (name.empty()) { usageError("usage: xlings subos upgrade <name> [--to <version>|<ns:name[@version]>] [--dry-run] [-y]"); return 1; }
    if (!exists_(name)) { error_(stream, "no SubOS named '" + name + "'"); return 1; }
    if (!role_allows_(roles::Op::Packages, name, stream)) return 1;
    const auto home = home_dir_();
    const auto file = HomeView{home}.instance_file(name);
    auto instance = read_json_(file);
    const auto now = subos::edition::read(instance);
    if (!now) {
        error_(stream, std::format("'{}' has no edition to upgrade from: it was not made from one, or was made "
                                   "before xlings recorded it (2026.10.11.1)", name),
               std::format("xlings install <package> --subos {} updates a package; a new environment from the "
                           "edition has the newest of everything", name));
        return 1;
    }
    const auto [key, version] = subos::edition::split(now->ref);
    // The newest of the edition, or the version asked for -- or another
    // package that replaces it (`--to ns:name[@version]`; design 2026-10-18
    // §15.1). The index is whatever this home has: `xlings update` refreshes it.
    const bool successor = to.find(':') != std::string::npos;
    const auto target = to.empty() ? key : successor ? to : key + "@" + to;
    const auto chain = chain_of_(target, stream);
    if (!chain) return 1;
    const auto& top = chain->templates.front();
    if (successor && subos::edition::split(to).first != key) {
        // Only a package that says it replaces this one, in the same index: a
        // package elsewhere cannot take another's systems over.
        const auto manifest = read_json_(top.second / ".xlings.json");
        bool named = false;
        if (const auto r = manifest.find("replaces"); r != manifest.end()) {
            if (r->is_string()) named = r->get<std::string>() == key;
            else if (r->is_array()) named = std::ranges::any_of(*r, [&](const auto& v) { return v.is_string() && v.template get<std::string>() == key; });
        }
        const auto old_index = xim::index_repo_of(key), new_index = xim::index_repo_of(to);
        if (!named || !old_index || old_index != new_index) {
            error_(stream, !named ? std::format("{} does not replace {}: its manifest names no `replaces` of it", top.first, key)
                                  : std::format("{} replaces {} from another index: a replacement is honoured within one index",
                                                top.first, key),
                   std::format("xlings subos upgrade {} (its own newest)", name));
            return 1;
        }
    }
    if (auto refused = root_abi_refusal_(read_json_(top.second / ".xlings.json"))) {
        error_(stream, std::format("{} cannot run here: {}", top.first, *refused));
        return 1;
    }
    if (const auto m = read_json_(top.second / ".xlings.json").value("min_client", std::string()); !m.empty()) {
        const auto order = policy::compare_client_versions(Info::VERSION, m);
        if (!order || *order < 0) {
            error_(stream, std::format("{} needs xlings >= {}, this is {}", top.first, m, Info::VERSION),
                   "xlings self update");
            return 1;
        }
    }
    // An unpinned package (an agent) follows the index's newest: compared
    // as that version, so one already at it is not an upgrade.
    std::vector<std::string> declared;
    for (const auto& spec : chain->packages) {
        if (spec.find('@') == std::string::npos)
            if (auto v = xim::index_version_of(spec, xim::CatalogAccess::LocalOnly)) {
                declared.push_back(spec + "@" + *v);
                continue;
            }
        declared.push_back(spec);
    }
    const auto plan = subos::edition::plan(*now, declared, configured_keys_(home, name));

    // The policy: applied when it allows nothing more than the one in force.
    std::vector<std::string> loosened;
    bool apply_policy = false;
    if (!chain->policy.empty() && chain->policy != now->policy) {
        auto current = policy_store::read(HomeView{home}, name);
        auto next = select_policy_package_(chain->policy, /*upgrade=*/false);
        if (!next) { error_(stream, next.error().second); return next.error().first; }
        const auto base = current && *current ? **current : policy::legacy();
        loosened = policy::loosened(base, *next);
        apply_policy = loosened.empty();
    }

    const bool same = top.first == now->ref;
    // The plan is data, rendered by the frontend (xlings' terminal, luban):
    // a system in a prefix domain answers through a relay that carries
    // events, so text printed here would never reach its terminal.
    {
        nlohmann::json data{{"name", name}, {"from", now->ref}, {"to", top.first}, {"dry_run", dry},
                            {"up_to_date", plan.empty() && same && !apply_policy}};
        auto& rows = data["upgrade"] = nlohmann::json::array();
        for (const auto& s : plan.upgrade)
            rows.push_back({{"key", s.key}, {"from", s.from},
                            {"to", subos::edition::split(s.to).second.empty() ? "newest" : subos::edition::split(s.to).second}});
        auto& added = data["add"] = nlohmann::json::array();
        for (const auto& s : plan.add) added.push_back(s.to);
        auto& kept = data["keep"] = nlohmann::json::array();
        for (const auto& s : plan.kept) kept.push_back({{"key", s.key}, {"from", s.from}, {"edition", s.to}});
        for (const auto& k : plan.dropped) kept.push_back({{"key", k}, {"dropped", true}});
        if (!chain->policy.empty() && chain->policy != now->policy)
            data["policy"] = {{"from", now->policy}, {"to", chain->policy}, {"applied", apply_policy},
                              {"loosened", loosened}};
        stream.emit(DataEvent{"subos_upgrade_plan", data.dump()});
    }
    if (plan.empty() && same && !apply_policy) return 0;
    if (dry) return 0;
    auto asked = confirm::ask(stream, "subos_upgrade", std::format("upgrade '{}'?", name), yes, "-y");
    if (asked.outcome == confirm::Outcome::Declined) return 0;
    if (asked.outcome == confirm::Outcome::NobodyToAsk) {
        stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput,
            .message = std::format("upgrading '{}' needs confirmation; nothing changed", name),
            .recoverable = true, .hint = std::format("xlings subos upgrade {} -y", name)});
        return 2;
    }

    // The templates' files under usr/ (the factory /etc and the rest an
    // edition ships): derived data, the edition's, so the new one's replace
    // them; the top's win. Nothing outside usr/ is touched -- home/ and the
    // machine's state are the user's. The manifest keys an edition owns
    // follow it; the scope's own keys (workspace, configured, subos_info) stay.
    const auto dir = HomeView{home}.instance(name);
    std::error_code ec;
    for (auto t = chain->templates.rbegin(); t != chain->templates.rend(); ++t) {
        for (fs::recursive_directory_iterator it(t->second, ec), end; !ec && it != end; it.increment(ec)) {
            const auto rel = it->path().lexically_relative(t->second);
            if (rel.empty() || *rel.begin() != "usr") continue;
            std::error_code sec;
            if (it->is_directory(sec) && !it->is_symlink(sec)) { fs::create_directories(dir / rel, sec); continue; }
            fs::remove(dir / rel, sec);
            fs::copy(it->path(), dir / rel, fs::copy_options::copy_symlinks, sec);
        }
    }
    {
        auto manifest = read_json_(dir / ".xlings.json");
        const auto next = read_json_(top.second / ".xlings.json");
        for (const auto* k : {"subos_kind", "from", "packages", "abi", "boot", "policy", "min_client", "replaces"}) {
            if (next.contains(k)) manifest[k] = next[k];
            else manifest.erase(k);
        }
        // Whatever else the template declares (a `luban` object, a
        // distribution's own), as it writes it: xlings keeps what it does not
        // read. What is the instance's own is not the template's to replace.
        for (auto it = next.begin(); it != next.end(); ++it)
            if (it.key() != "workspace" && it.key() != "subos_info" && it.key() != "configured")
                manifest[it.key()] = it.value();
        if (auto w = write_json_(dir / ".xlings.json", manifest); !w) { error_(stream, w.error(), {}, ErrorCode::Internal); return 1; }
    }
    std::vector<std::string> specs;
    for (const auto& s : plan.upgrade) specs.push_back(s.to);
    for (const auto& s : plan.add) specs.push_back(s.to);
    if (!specs.empty() && xim::cmd_install_step(specs, name, stream, /*plumbing=*/false) != 0) {
        log::info("'{}' is partly upgraded: `xlings subos upgrade {}` again finishes it, "
                  "`xlings subos rollback {}` goes back", name, name, name);
        return 1;
    }
    if (apply_policy) {
        std::vector<std::string> words{"xlings", "subos", "config", name, "--sandbox", chain->policy};
        std::vector<char*> args;
        for (auto& w : words) args.push_back(w.data());
        args.push_back(nullptr);
        if (auto r = run_config_(static_cast<int>(words.size()), args.data(), stream, usageError); r != 0) return r;
    }
    // The record: what the edition brought now, as installed.
    subos::edition::Record next{.ref = top.first, .chain = {}, .packages = {},
                                .policy = apply_policy || now->policy.empty() ? chain->policy : now->policy};
    for (std::size_t i = 1; i < chain->templates.size(); ++i) {
        next.chain.push_back(chain->templates[i].first);
        next.chain_index.push_back(xim::index_repo_of(chain->templates[i].first).value_or(""));
    }
    next.index = xim::index_repo_of(top.first).value_or(now->index);
    const auto configured = configured_keys_(home, name);
    for (const auto& spec : chain->packages) {
        const auto k = subos::edition::split(spec).first;
        std::string installed;
        for (const auto& c : configured) {
            auto [ck, cv] = subos::edition::split(c);
            if (ck == k && (installed.empty() || version_order::compare(cv, installed) > 0)) installed = cv;
        }
        // The user's version of a package they moved is theirs: the record
        // keeps what the edition last brought.
        if (std::ranges::any_of(plan.kept, [&](const auto& s) { return s.key == k; }))
            installed = now->packages.contains(k) ? now->packages.at(k) : std::string();
        next.packages[k] = installed;
    }
    instance = read_json_(file);
    instance["edition"] = subos::edition::to_json(next);
    if (auto w = write_json_(file, instance); !w) { error_(stream, w.error(), {}, ErrorCode::Internal); return 1; }
    auto r = subos_root::refresh(home, name, "subos upgrade");
    if (r && !*r) {
        error_(stream, "the root of '" + name + "' could not be laid out: " + r->error(), {}, ErrorCode::Internal);
        return 1;
    }
    observe::append(HomeView{home}.logs_dir(name) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Lifecycle,
        .fields = {{"event", "upgrade"}, {"instance", name}, {"from", now->ref}, {"to", top.first}}});
    log::info("'{}' is {} (generation {}); `xlings subos rollback {}` goes back", name, top.first,
              r && *r ? (*r)->generation : 0, name);
    return 0;
}

// ── rollback ─────────────────────────────────────────────────────────

int run_rollback_(int argc, char* argv[], EventStream& stream, const UsageError& usageError) {
    std::string name;
    std::optional<int> to;
    bool list = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--to" && i + 1 < argc) {
            int k = 0;
            std::string v = argv[++i];
            auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), k);
            if (ec != std::errc{} || p != v.data() + v.size()) { usageError("--to expects a generation number"); return 1; }
            to = k;
        } else if (a == "--list") list = true;
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
        else { usageError("unknown option for `xlings subos rollback`: " + a); return 1; }
    }
    if (!name.empty()) {
        std::vector<std::string> domainArgs{"subos", "rollback"};
        domainArgs.insert(domainArgs.end(), argv + 3, argv + argc);
        if (auto result = run_domain_operation_(name, domainArgs, stream)) return *result;
    }
    if (!exists_(name)) { usageError("usage: xlings subos rollback <name> [--to <generation>] [--list]"); return 1; }
    if (!role_allows_(roles::Op::Rollback, name, stream)) return 1;
    const auto dir = HomeView{home_dir_()}.instance(name);
    const auto gens = rf::generations(dir);
    const auto now = rf::current(dir);
    if (list) {
        nlohmann::json rows = nlohmann::json::array();
        for (int k : gens) {
            auto g = rf::info(dir, k);
            rows.push_back({{"generation", k}, {"current", now && *now == k},
                            {"created", g ? g->created : ""}, {"reason", g ? g->reason : ""},
                            {"links", g ? g->links : 0}});
            log::println("{} {:>3}  {}  {}", now && *now == k ? "*" : " ", k,
                         g ? g->created : "", g ? g->reason : "");
        }
        stream.emit(DataEvent{"subos_generations", rows.dump()});
        return 0;
    }
    if (!to) {
        auto it = now ? std::ranges::find(gens, *now) : gens.end();
        if (it == gens.end() || it == gens.begin()) {
            error_(stream, "'" + name + "' has no earlier generation", "xlings subos rollback " + name + " --list");
            return 1;
        }
        to = *std::prev(it);
    }
    if (auto sw = rf::switch_to(dir, *to); !sw) {
        error_(stream, sw.error(), "xlings subos rollback " + name + " --list");
        return 1;
    }
    // Its packages too, or the next entry rebuilds the root from today's.
    if (auto restored = subos_root::restore_packages(home_dir_(), name, *to); !restored) {
        error_(stream, "cannot restore the packages of generation " + std::to_string(*to) + ": " + restored.error());
        if (now) (void)rf::switch_to(dir, *now, rf::Flush::Durable, rf::Verify::Tree);
        return 1;
    } else if (!*restored) {
        log::warn("generation {} was made before generations recorded their packages: its root is back, and "
                  "the next entry rebuilds it from the packages '{}' has now", *to, name);
    }
    if (auto cache = subos_root::refresh_cache(home_dir_(), name); !cache) {
        if (now) {
            if (auto restored = rf::switch_to(dir, *now, rf::Flush::Durable, rf::Verify::Tree); !restored)
                error_(stream, "cannot restore prior generation: " + restored.error());
        }
        error_(stream, cache.error());
        return 1;
    }
    observe::append(HomeView{home_dir_()}.logs_dir(name) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Lifecycle,
        .fields = {{"event", "rollback"}, {"instance", name}, {"from", now ? *now : 0}, {"to", *to}}});
    log::info("'{}': root generation {} (was {})", name, *to, now ? std::to_string(*now) : "none");
    return 0;
}

// ── check ────────────────────────────────────────────────────────────

// `subos __check [--fast] [--json]`: the self-check of the system this
// process runs in (design 2026-10-16 L §11), reached by `luban status
// --check` -- inside, or from the host through `subos exec`. Its exit is 1
// when a critical item failed.
int run_check_(int argc, char* argv[], EventStream& stream, const UsageError& usageError) {
    bool json = false, fast = false;
    for (int i = 3; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--fast") fast = true;
        else { usageError("unknown option for `luban status --check`: " + std::string(a)); return 2; }
    }
    const auto env = [](const char* name) { const char* v = std::getenv(name); return std::string(v ? v : ""); };
    if (std::error_code ec; env("XLINGS_ROOT_INSTANCE").empty() && !fs::exists("/etc/xlings/usr", ec)) {
        stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput, .message = "this is not a Luban system",
                               .recoverable = false, .hint = "from the host: luban status <name> --check"});
        return 2;
    }
    namespace rc = xlings::subos::root_check;
    auto report = rc::check("/", fast ? rc::Depth::Fast : rc::Depth::Full, {.term = env("TERM")});
    // Its users read the system's package index (design 2026-10-20 §4): the
    // catalog loaded -- built and kept here by root when it is not -- so a
    // user's view, which cannot write it, finds it ready.
    if (!fast) {
        auto& catalog = xim::get_catalog();
        rc::Item item{.id = "package-index", .what = "its package index can be read (its users' too)"};
        if (!catalog.is_loaded()) (void)catalog.rebuild();
        item.ok = catalog.is_loaded();
        if (!item.ok) item.detail = "xlings update fetches it again";
        report.items.push_back(std::move(item));
    }
    if (json) {
        std::println("{}", rc::to_json(report).dump());
    } else {
        for (const auto& item : report.items) {
            const auto mark = item.ok ? "✓" : item.critical ? "✗" : "!";
            std::println("{} {}{}", mark, item.what, item.ok || item.detail.empty() ? std::string{} : " -- " + item.detail);
        }
        std::println("{}", report.ok() ? std::format("usable ({} of {} passed)", report.items.size() - report.failed(),
                                                     report.items.size())
                                       : "not usable: a critical item failed");
    }
    return report.ok() ? 0 : 1;
}

// ── boot ─────────────────────────────────────────────────────────────

int run_boot_(int argc, char* argv[], EventStream& stream, const UsageError& usageError) {
    const auto home = home_dir_();
    const auto file = HomeView{home}.boot_file();
    std::string name;
    bool once = false, mark_good = false, fallback = false, now = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--once") once = true;
        else if (a == "--now") now = true;
        else if (a == "--fallback") fallback = true;
        else if (a == "--mark-good") mark_good = true;
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
        else { usageError("unknown option for `luban boot`: " + a); return 1; }
    }
    auto cfg = bt::load(file);
    if (!cfg) { error_(stream, cfg.error()); return 1; }
    if (mark_good) {
        if (auto s = bt::save(file, bt::mark_good(*cfg)); !s) { error_(stream, s.error()); return 1; }
        log::info("boot of '{}' marked good", cfg->booted.empty() ? "?" : cfg->booted);
        return 0;
    }
    if (name.empty()) {
        stream.emit(DataEvent{"subos_boot", bt::to_json(*cfg).dump()});
        log::println("default   {}", cfg->default_entry);
        log::println("fallback  {}", cfg->fallback);
        if (cfg->once) log::println("next boot {} (once)", *cfg->once);
        if (!cfg->booted.empty())
            log::println("this boot {} via {}{}", cfg->booted, cfg->via, cfg->good ? ", marked good" : "");
        return 0;
    }
    if (!exists_(name)) { error_(stream, "no SubOS named '" + name + "'"); return 1; }
    if (!role_allows_(roles::Op::Boot, name, stream)) return 1;
    // --now (part 2 §8.1): hand / to it without restarting the kernel. Only
    // an init that can re-exec stage-0 can do that -- busybox init with a
    // `::restart:` entry naming stage-0 (/usr/bin/luban-init, or xlings-init
    // in luban-tiny 0.1.0), on the
    // machine this home is the root of. Anything else would leave the
    // running processes on one SubOS and new ones on another: refused, and
    // a reboot does it.
    if (now) {
        const auto host = subos_root::running_host(home);
        std::error_code ec;
        const auto init = fs::read_symlink("/proc/1/exe", ec).filename().string();
        const auto inittab = read_text_("/etc/inittab");
        const bool restartable = host && init == "busybox"
            && (inittab.find("::restart:/usr/bin/luban-init") != std::string::npos
                || inittab.find("::restart:/usr/bin/xlings-init") != std::string::npos);
        if (!restartable) {
            error_(stream, !host ? "this home is not the root of the running machine"
                                 : "this machine's init cannot hand / to another SubOS without a reboot",
                   std::format("luban boot {} --once, then reboot", name));
            return 1;
        }
        cfg->once = name;
        if (auto s = bt::save(file, *cfg); !s) { error_(stream, s.error()); return 1; }
        log::info("switching user space to '{}' now", name);
        std::cout.flush();
        if (!platform::send_signal(1, platform::sig::quit)) {
            error_(stream, "init did not take the signal: " + platform::error_text(platform::last_error()));
            return 1;
        }
        return 0;
    }
    if (once) cfg->once = name;
    else if (fallback) cfg->fallback = name;
    else { cfg->default_entry = name; cfg->tries[name] = bt::kTries; }
    if (auto s = bt::save(file, *cfg); !s) { error_(stream, s.error()); return 1; }
    observe::append(HomeView{home}.logs_dir(name) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Lifecycle,
        .fields = {{"event", "boot-entry"}, {"instance", name},
                   {"as", once ? "once" : fallback ? "fallback" : "default"}}});
    log::info("'{}' {}; it takes effect when the machine boots", name,
              once ? "boots next, once" : fallback ? "is the fallback" : "is the default boot");
    return 0;
}

// ── diff ─────────────────────────────────────────────────────────────

int run_diff_(int argc, char* argv[], EventStream& stream, const UsageError& usageError) {
    if (argc < 5) { usageError("usage: xlings subos diff <a> <b>"); return 1; }
    const std::string a = argv[3], b = argv[4];
    auto wa = workspace_of_(a), wb = workspace_of_(b);
    if (!wa || !wb) { error_(stream, "no SubOS named '" + (!wa ? a : b) + "'"); return 1; }
    nlohmann::json rows = nlohmann::json::array();
    std::set<std::string> names;
    for (auto& [t, _] : wa->active) names.insert(t);
    for (auto& [t, _] : wb->active) names.insert(t);
    for (auto& t : names) {
        auto ia = wa->active.find(t), ib = wb->active.find(t);
        const std::string va = ia == wa->active.end() ? "" : ia->second;
        const std::string vb = ib == wb->active.end() ? "" : ib->second;
        if (va == vb) continue;
        const char* mark = va.empty() ? "+" : vb.empty() ? "-" : "~";
        rows.push_back({{"target", t}, {a, va}, {b, vb}});
        log::println("{} {:<24} {:<14} {}", mark, t, va.empty() ? "-" : va, vb.empty() ? "-" : vb);
    }
    stream.emit(DataEvent{"subos_diff", rows.dump()});
    if (rows.empty()) log::info("'{}' and '{}' have the same packages", a, b);
    return 0;
}


}  // namespace xlings::subos
