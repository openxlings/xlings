module xlings.core.xself.init;


import std;
import xlings.core.config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.core.xself.compat;
import xlings.core.xself.profile_resources;
import xlings.core.subos.manifest;
import xlings.core.xim.commands;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xvm.shim;
import xlings.core.xvm.shim_table;

namespace xlings::xself {

bool is_builtin_shim(std::string_view name) {
    for (auto n : SHIM_NAMES_BASE)
        if (n == name) return true;
    for (auto n : SHIM_NAMES_OPTIONAL)
        if (n == name) return true;
    return false;
}

bool is_bootstrap_home_root(const fs::path& root) {
    std::error_code ec;
    if (root.empty() || !fs::exists(root / ".xlings.json", ec)) return false;
    if (!fs::exists(root / "bin", ec) || !fs::is_directory(root / "bin", ec)) return false;
#ifdef _WIN32
    return fs::exists(root / "bin" / "xlings.exe", ec);
#else
    return fs::exists(root / "bin" / "xlings", ec);
#endif
}

fs::path xlings_binary_in_home(const fs::path& home_dir) {
#ifdef _WIN32
    auto bin = home_dir / "bin" / "xlings.exe";
#else
    auto bin = home_dir / "bin" / "xlings";
#endif
    if (fs::exists(bin)) return bin;
    return {};
}

LinkResult create_shim(const fs::path& source, const fs::path& target) {
    std::error_code ec;

    if (!fs::exists(source, ec)) return LinkResult::Failed;

    // Free the target path. `fs::remove` alone is not enough on Windows: it
    // cannot delete a running executable, and the shim being rewritten here is
    // frequently `xlings.exe` itself during `self update`. The failure was
    // discarded, so the code went on to hard-link and copy over a path that
    // was still occupied and reported "failed to create shim ... the process
    // cannot access the file because it is being used by another process"
    // (issue #473) -- with the update half-applied.
    //
    // displace_locked_file renames the occupant aside on Windows, which works
    // on a running image, and is a plain unlink on POSIX.
    if (!platform::displace_locked_file(target)) {
        log::error("[xlings:self]: cannot free shim path {} "
                   "(a process is holding it open)", target.string());
        return LinkResult::Failed;
    }
    ec.clear();

#if !defined(_WIN32)
    // Unix: prefer relative symlink
    auto rel = fs::relative(source, target.parent_path(), ec);
    if (!ec && !rel.empty()) {
        fs::create_symlink(rel, target, ec);
        if (!ec) return LinkResult::Symlink;
    }
    ec.clear();
    // Fallback: absolute symlink
    fs::create_symlink(source, target, ec);
    if (!ec) return LinkResult::Symlink;
    ec.clear();
#endif

    // Hardlink (Unix fallback / Windows primary)
    fs::create_hard_link(source, target, ec);
    if (!ec) return LinkResult::Hardlink;
    ec.clear();

    // Final fallback: copy
    fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
    if (!ec) return LinkResult::Copy;

    log::error("[xlings:self]: failed to create shim {} - {}",
        target.string(), ec.message());
    return LinkResult::Failed;
}

std::size_t ensure_subos_shims(const fs::path& target_bin_dir,
                               const fs::path& shim_src,
                               const fs::path& pkg_root) {
    if (!fs::exists(shim_src)) return 0;

    std::string ext = shim_src.extension().string();
    std::size_t failures = 0;

    for (auto name : SHIM_NAMES_BASE) {
        auto dst = target_bin_dir / (std::string(name) + ext);
        if (create_shim(shim_src, dst) == LinkResult::Failed) ++failures;
    }

    if (!pkg_root.empty()) {
        auto bin_dir = pkg_root / "bin";
        for (auto name : SHIM_NAMES_OPTIONAL) {
            auto opt_bin = bin_dir / (std::string(name) + ext);
            if (fs::exists(opt_bin)) {
                auto dst = target_bin_dir / (std::string(name) + ext);
                if (create_shim(shim_src, dst) == LinkResult::Failed) ++failures;
            }
        }
    }

    // COMPAT(0.4.8 → drop in 0.6.0): one-shot migration cleanup.
    compat::v0_4_8::cleanup_legacy_alias_shims(target_bin_dir, shim_src);

    platform::make_files_executable(target_bin_dir);
    return failures;
}

void ensure_parent_dirs_(const fs::path& file) {
    std::error_code ec;
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path(), ec);
}

void write_if_missing_(const fs::path& path, std::string_view content) {
    if (fs::exists(path)) return;
    ensure_parent_dirs_(path);
    platform::write_string_to_file(path.string(), std::string(content));
}

// Give a subos directory a valid `subos_info`, preserving everything else in
// the file. Idempotent: a block that already validates is left alone, so the
// envs packages declared into it survive.
void ensure_subos_manifest_(const fs::path& subos_dir) {
    namespace mf = xlings::subos::manifest;
    auto path = subos_dir / ".xlings.json";

    // Which of the two things this run is doing, decided by the one fact that
    // distinguishes them.
    //
    // `ensure_home_layout` calls this on install AND on update, so it is a
    // CREATION on a fresh home and a DESCRIPTION on every home after that.
    // Getting it wrong in either direction is a defect with a name:
    //
    //   always Describe  a brand-new home's `default` records no runtime at
    //                    all, even though this run is the one making it and
    //                    the default is exactly what "nobody said otherwise"
    //                    means. Caught by E2E-67/S1.
    //   always Create    an upgraded home gets re-declared against whatever
    //                    the default is today, which is #547.
    //
    // Same test `subos.cpp` applies at its own fork: the config file's
    // existence, sampled BEFORE anything below writes one.
    const bool creating = !fs::exists(path);

    nlohmann::json json = nlohmann::json::object();
    if (!creating) {
        try {
            auto parsed = nlohmann::json::parse(
                platform::read_file_to_string(path.string()), nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                // Unreadable, not absent. Overwriting would throw away a
                // workspace we cannot see; leave it for doctor to report.
                log::warn("[xlings:self]: {} is not readable JSON; leaving it "
                          "alone (run `xlings self doctor`)",
                          Config::display_path(path));
                return;
            }
            json = std::move(parsed);
        } catch (...) { return; }
    }
    if (!json.contains("workspace")) json["workspace"] = nlohmann::json::object();
    if (mf::validate_block(json).empty()) return;

    const auto by = std::format("xlings {}", Info::VERSION);
    // The default runtime comes from the index, not from a constant compiled
    // into this binary. THIS path matters more than it looks: `self init` is
    // what lays down `subos/default`, and that is the manifest mcpp reads on a
    // first build in a fresh home -- the exact block that said `glibc@2.44`
    // while the only payload on disk was 2.44.2 (xim-pkgindex#692).
    //
    // Resolved here rather than through subos::resolve_default_runtime because
    // this file cannot import xlings.core.subos: subos imports xself, so that
    // edge would close a cycle. The index query lives one layer down, where
    // both callers can reach it.
    if (creating) {
        // Only on the creating side. describe_block never consults a default,
        // and resolving one anyway would rebuild the catalog on every repair
        // for an answer that is then discarded.
        struct DefaultRuntime { std::string binding; bool resolved; };
        const DefaultRuntime def = [] -> DefaultRuntime {
            // Queried by coordinate, recorded by name -- see the two
            // constants in manifest.cppm.
            const std::string pkg{mf::DEFAULT_RUNTIME_PACKAGE};

            // InstallReady, not LocalOnly, and this is the whole point of
            // the change: `self init` is where a subos gets its runtime
            // binding, and that binding is a CREATION-TIME property nothing
            // later rewrites. On a fresh home the index has never been
            // materialized, so a LocalOnly query cannot answer -- and the
            // fallback constant became, in practice, the value every new user
            // got. Measured on a first `mcpp build` in a fresh MCPP_HOME with
            // xlings 2026.8.27.3: `runtime_source` was `fallback`, and it
            // worked only because the constant happened to equal what the
            // index offered that day.
            //
            // "Answer from disk" is the wrong question when the answer is
            // about to be written down permanently. InstallReady makes the
            // index usable first -- and it degrades exactly the way this call
            // needs: a home that already has an index answers WITHOUT
            // touching the network, and a sync that cannot happen still ends
            // in nullopt, so the fallback below is unchanged for anyone
            // offline.
            //
            // Not writing anything is not an alternative. Downstream, an
            // absent runtime means "no declared runtime to bind to", and the
            // link falls back to the HOST -- loud, but not hermetic. Create
            // must produce a value; the choice is only between looking it up
            // and guessing it.
            if (auto v = xlings::xim::index_version_of(
                    mf::DEFAULT_RUNTIME_QUERY,
                    xlings::xim::CatalogAccess::InstallReady))
                return {pkg + "@" + *v, true};
            return {std::string(mf::DEFAULT_RUNTIME_FALLBACK), false};
        }();

        // Said out loud, matching what `subos new` already does on the same
        // fallback (subos.cpp). Both spellings of the remedy, because they
        // answer different situations -- the index has never been synced on
        // this machine, or the user knew the binding all along. Without this,
        // the only trace is `runtime_source` in a file nobody opens, and the
        // failure that eventually follows names a payload DIRECTORY rather
        // than a decision anyone remembers making.
        //
        // `log::` rather than an event: init runs before the stream exists.
        if (!def.resolved) {
            log::warn("index could not answer which {} to bind to; using the "
                      "built-in {}. Run `xlings update`, or create the subos "
                      "explicitly with `--runtime <package>@<version>`.",
                      mf::DEFAULT_RUNTIME_PACKAGE, def.binding);
        }

        // Provenance is recorded HERE, unlike the rebuild paths in subos.cpp,
        // because this call can tell: the block is being written from nothing,
        // so `def` is what step 5 answered with -- and if some earlier step
        // outranked it, the values differ and nothing is claimed.
        auto runtime = mf::runtime_for(subos_dir, json, mf::Intent::Create,
                                       {}, def.binding);
        // Asked of the index, not derived from the name -- and only when the
        // index could answer at all. On the fallback path the query already
        // failed once; asking again would be a second round trip for the same
        // silence, and an ABI invented from the name is exactly the second
        // derivation this field exists to remove.
        std::string runtimeAbi;
        if (def.resolved && !mf::runtime_is_hosted(runtime)) {
            if (auto abi = xlings::xim::index_runtime_abi_of(
                    mf::runtime_query_for(runtime)))
                runtimeAbi = *abi;
        }

        json[std::string(mf::BLOCK)] = mf::make_block({
            .runtime   = runtime,
            .by        = by,
            .hostGlibc = platform::host_glibc_version(),
            .intent    = mf::Intent::Create,
            .runtimeSource = runtime == def.binding
                ? std::string(def.resolved ? mf::RUNTIME_SOURCE_INDEX
                                           : mf::RUNTIME_SOURCE_FALLBACK)
                : std::string{},
            .runtimeAbi = std::move(runtimeAbi),
        });
    } else {
        json[std::string(mf::BLOCK)] =
            mf::describe_block(subos_dir, json, by,
                               platform::host_glibc_version());
    }
    ensure_parent_dirs_(path);
    platform::write_string_to_file(path.string(), json.dump(2));
}

// Extract the value following `# xlings-profile-version: ` on any line of
// `text`. Returns an empty string when the marker is absent, which we
// interpret as "legacy v1" — anything older than the time we started
// shipping a version marker.
std::string extract_profile_version_(std::string_view text) {
    constexpr std::string_view marker = "# xlings-profile-version:";
    auto pos = text.find(marker);
    if (pos == std::string_view::npos) return {};
    auto value_start = pos + marker.size();
    while (value_start < text.size() &&
           (text[value_start] == ' ' || text[value_start] == '\t')) {
        ++value_start;
    }
    auto eol = text.find_first_of("\r\n", value_start);
    auto end = (eol == std::string_view::npos) ? text.size() : eol;
    auto value = std::string{text.substr(value_start, end - value_start)};
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

// Profile-aware writer. Behavior:
//
//   1. Path doesn't exist  → write fresh.
//   2. Path exists, version matches `target_version` → leave alone (we
//      respect any user edits made after install).
//   3. Path exists with older or missing version marker → overwrite, log
//      the upgrade. This is how shell-level subos switching reaches users
//      who installed before v2 of the profile shipped.
//
// The version of the bytes we're shipping comes from
// xlings::xself::profile_resources::kVersion; the on-disk value comes from
// the marker line we inject as the first comment of every profile.
void write_or_upgrade_profile_(const fs::path& path,
                                      std::string_view content,
                                      std::string_view target_version) {
    if (!fs::exists(path)) {
        ensure_parent_dirs_(path);
        platform::write_string_to_file(path.string(), std::string(content));
        return;
    }

    auto existing = platform::read_file_to_string(path.string());
    auto existing_version = extract_profile_version_(existing);
    if (existing_version == target_version) return;

    log::debug("upgrading {} (was: {}, now: {})",
               path.filename().string(),
               existing_version.empty() ? "<no marker, treated as v1>"
                                        : existing_version,
               target_version);
    platform::write_string_to_file(path.string(), std::string(content));
}

// Read-modify-writes `<home>/.xlings.json` without taking the state lock
// itself. Both callers reach it through `ensure_home_layout`, and the only
// caller of that is `xself::cmd_install`, which holds the lock for its whole
// home-rewriting stretch. Acquiring here as well would be harmless (the lock
// is re-entrant within a process) but would suggest this is safe to call
// from somewhere unlocked, which it is not.
void ensure_home_config_defaults_(const fs::path& home_dir) {
    auto config_path = home_dir / ".xlings.json";
    nlohmann::json json = nlohmann::json::object();

    if (fs::exists(config_path)) {
        try {
            auto content = platform::read_file_to_string(config_path.string());
            auto parsed = nlohmann::json::parse(content, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) json = std::move(parsed);
        } catch (...) {}
    }

    if (!json.contains("activeSubos") || !json["activeSubos"].is_string() ||
        json["activeSubos"].get<std::string>().empty()) {
        json["activeSubos"] = "default";
    }
    if (!json.contains("subos") || !json["subos"].is_object()) {
        json["subos"] = nlohmann::json::object();
    }
    if (!json["subos"].contains("default") || !json["subos"]["default"].is_object()) {
        json["subos"]["default"] = {{"dir", ""}};
    }

    ensure_parent_dirs_(config_path);
    platform::write_string_to_file(config_path.string(), json.dump(2));
}

bool ensure_home_layout(const fs::path& home_dir) {
    std::error_code ec;
    if (home_dir.empty()) return false;

    auto default_subos = home_dir / "subos" / "default";
    auto dirs = {
        home_dir,
        home_dir / "bin",
        home_dir / "config" / "shell",
        home_dir / "data" / "xpkgs",
        home_dir / "data" / "runtimedir",
        home_dir / "data" / "xim-index-repos",
        home_dir / "data" / "local-indexrepo",
        default_subos / "bin",
        default_subos / "lib",
        default_subos / "usr",
        default_subos / "generations",
    };

    for (auto& dir : dirs) {
        fs::create_directories(dir, ec);
        if (ec) {
            log::error("[xlings:self]: failed to create {} - {}",
                       Config::display_path(dir), ec.message());
            return false;
        }
    }

    auto current_link = home_dir / "subos" / "current";
    platform::create_directory_link(current_link, default_subos);

    // Not write_if_missing_: every home that predates `subos_info` already has
    // this file, so "missing" is exactly the case that never fires on the
    // subos that matters most. `default` is where an ordinary user installs
    // everything, and a default without the block would make the whole
    // configuration layer inert on upgrade while looking installed.
    //
    // This is also the migration: `self init` runs on install and update, so an
    // old home repairs its default subos on the next either. Other subos in
    // that home are `subos doctor --fix`'s job -- init does not enumerate them.
    ensure_subos_manifest_(default_subos);
    write_if_missing_(home_dir / "data" / "xim-index-repos" / "xim-indexrepos.json", "{}");
    // Profile content lives in xlings.core.xself.profile_resources. We use
    // the version-aware writer so users who installed an older xlings get
    // their profile auto-upgraded on the next `self init` / `self update`,
    // while users on the current version preserve any local edits.
    write_or_upgrade_profile_(home_dir / "config" / "shell" / "xlings-profile.sh",
                              profile_resources::bash_sh,
                              profile_resources::kVersion);
    write_or_upgrade_profile_(home_dir / "config" / "shell" / "xlings-profile.fish",
                              profile_resources::fish,
                              profile_resources::kVersion);
    write_or_upgrade_profile_(home_dir / "config" / "shell" / "xlings-profile.ps1",
                              profile_resources::pwsh,
                              profile_resources::kVersion);


    ensure_home_config_defaults_(home_dir);

    auto xlings_bin = xlings_binary_in_home(home_dir);
    if (!xlings_bin.empty()) {
        // A home whose shims could not be written is not a laid-out home. It
        // used to report success anyway, which is how a failed Windows update
        // produced "healed" alongside the state that made it fail.
        if (auto failures = ensure_subos_shims(default_subos / "bin",
                                               xlings_bin, home_dir);
            failures != 0) {
            log::error("[xlings:self]: {} shim(s) could not be written into {}",
                       failures, (default_subos / "bin").string());
            return false;
        }
    }

    return true;
}

int cmd_init() {
    auto& p = Config::paths();
    if (!ensure_home_layout(p.homeDir)) return 1;
    log::info("init ok");
    return 0;
}

// ─── The routing table's single writer ──────────────────────────────

namespace {

std::vector<std::string> reserved_shim_names_() {
    std::vector<std::string> names;
    for (auto n : SHIM_NAMES_BASE) names.emplace_back(n);
    for (auto n : SHIM_NAMES_OPTIONAL) names.emplace_back(n);
    return names;
}

} // namespace

std::vector<xvm::ProjectContribution> project_contributions() {
    std::vector<xvm::ProjectContribution> out;

    for (const auto& root : Config::known_projects()) {
        xvm::ProjectContribution contribution{ .root = root };

        auto statePath = root / ".xlings" / ".xlings.json";
        std::error_code ec;
        if (!fs::is_regular_file(statePath, ec)) {
            contribution.readable = false;
            out.push_back(std::move(contribution));
            continue;
        }

        nlohmann::json state;
        try {
            auto content = platform::read_file_to_string(statePath.string());
            state = nlohmann::json::parse(content, nullptr, false);
        } catch (...) { state = nlohmann::json{}; }

        if (state.is_discarded() || !state.is_object()) {
            contribution.readable = false;
            out.push_back(std::move(contribution));
            continue;
        }

        xvm::VersionDB db;
        if (auto it = state.find("versions");
            it != state.end() && it->is_object()) {
            db = xvm::versions_from_json(*it);
        }
        xvm::Workspace active;
        if (auto it = state.find("workspace");
            it != state.end() && it->is_object()) {
            active = xvm::subos_workspace_from_json(*it).active;
        }

        // Same rule as a subos's own names, applied to the project's own
        // state -- one predicate, two callers, so a project cannot contribute
        // a name a subos would have rejected.
        auto names = xvm::compute_desired(db, active, {});

        contribution.commands.reserve(names.size());
        for (const auto& n : names) contribution.commands.push_back(n);

        out.push_back(std::move(contribution));
    }
    return out;
}

xvm::TableDiff plan_shim_table(
        const fs::path& subos_dir,
        const xvm::Workspace& active,
        const xvm::VersionDB& db,
        const std::vector<xvm::ProjectContribution>& projects) {
    auto home = Config::paths().homeDir;
    auto entry = xlings_binary_in_home(home);

    auto desired = xvm::compute_desired(db, active, projects);
    auto actual = xvm::scan_actual(subos_dir / "bin", entry);
    return xvm::plan_table(desired, actual, reserved_shim_names_());
}

xvm::TableReport apply_shim_table(const fs::path& subos_dir,
                                  const xvm::TableDiff& diff) {
    auto entry = xlings_binary_in_home(Config::paths().homeDir);
    return xvm::apply_table(diff, subos_dir / "bin", entry);
}

void sync_shim_tables() {
    auto entry = xlings_binary_in_home(Config::paths().homeDir);
    std::error_code ec;
    if (entry.empty() || !fs::exists(entry, ec)) {
        // Pre-`self init` bootstrap: there is nothing to link to yet, and
        // ensure_home_layout will build the table when it lands.
        log::debug("[shim-table] no entry binary yet; skipping sync");
        return;
    }

    // Register the project BEFORE gathering contributions, so the very first
    // install into a project already reaches the global table. Done here
    // rather than at the install call sites because this is the one function
    // every workspace change goes through -- a registration hung off one of
    // the three call sites would be missed by the other two, which is how the
    // thing it replaces went wrong in the first place.
    if (Config::has_project_config() && !Config::project_dir().empty()) {
        Config::register_known_project(Config::project_dir());
    }

    auto projects = project_contributions();
    auto db = Config::versions();

    const auto sync_one = [&](const fs::path& subosDir,
                              const xvm::Workspace& active,
                              std::string_view label) {
        if (subosDir.empty()) return;
        auto diff = plan_shim_table(subosDir, active, db, projects);
        if (diff.empty()) return;
        auto report = apply_shim_table(subosDir, diff);
        log::debug("[shim-table] {}: +{} -{} (failed {})", label,
                   report.added.size(), report.removed.size(),
                   report.failed.size());
        for (const auto& [name, why] : report.failed) {
            log::warn("could not update shim '{}' in {}: {}", name,
                      Config::display_path(subosDir / "bin"), why);
        }
    };

    // The scope that was just written.
    sync_one(Config::xvm_artifact_subos_dir(), Config::workspace(), "scope");

    // In project scope, the global active subos too: the project's bin is
    // never on PATH, so its command names must also exist in the directory
    // that is. `project_contributions()` is what carries them there.
    if (Config::has_project_config()) {
        auto globalDir = Config::global_subos_dir();
        if (globalDir != Config::xvm_artifact_subos_dir()) {
            sync_one(globalDir, Config::global_workspace(), "global");
        }
    }
}

}
