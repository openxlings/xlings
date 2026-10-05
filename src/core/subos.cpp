module xlings.core.subos;

import std;
import xlings.core.config;
import xlings.core.home_config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.runtime;
import xlings.core.utils;
import xlings.core.xself;
import xlings.core.xvm.types;
import xlings.core.xvm.db;
import xlings.core.xim.commands;
import xlings.subos.gpu;
import xlings.subos.graphics;
import xlings.core.subos.sandbox;
import xlings.subos.manifest;
import xlings.cli.spec;
import xlings.i18n;
import xlings.core.confirm;
import xlings.core.destructive_log;
import xlings.subos.userdata;
import xlings.subos.model;
import xlings.core.subos.ports;
import xlings.subos.session;
import xlings.subos.policy;
import xlings.subos.policy_store;
import xlings.subos.broker;
import xlings.observe;
import xlings.core.home;
import xlings.libs.sha256;
import xlings.core.version_order;

namespace xlings::subos {

nlohmann::json read_config_json_(const fs::path& path) {
    if (!fs::exists(path)) return nlohmann::json::object();
    try {
        auto content = platform::read_file_to_string(path.string());
        auto json = nlohmann::json::parse(content, nullptr, false);
        return json.is_discarded() ? nlohmann::json::object() : json;
    } catch (...) { return nlohmann::json::object(); }
}

void write_config_json_(const fs::path& path, const nlohmann::json& json) {
    platform::write_string_to_file(path.string(), json.dump(2));
}

// How many command names this subos routes, and how many packages back them.
//
// Two numbers because they answer two questions and one of them was silently
// standing in for the other. The command count is the size of the routing
// table; the package count is per RELEASE, from the workspace, which is the
// only place that knows a release exists. One llvm is one package and about
// forty commands.
struct SubosCounts {
    int commands { 0 };
    int packages { -1 };
};

SubosCounts count_subos_(const fs::path& dir, bool withPackages) {
    SubosCounts counts;

    auto binDir = dir / "bin";
    if (fs::exists(binDir)) {
        for (auto& e : platform::dir_entries(binDir)) {
            auto stem = e.path().stem().string();
            if (!xself::is_builtin_shim(stem) && stem != "xvm-alias")
                ++counts.commands;
        }
    }
    if (!withPackages) return counts;

    // Per release, not per name. A release is identified by its binding group
    // root; a target with no group is its own release.
    auto json = read_config_json_(dir / ".xlings.json");
    auto wsIt = json.find("workspace");
    if (wsIt == json.end() || !wsIt->is_object()) {
        counts.packages = 0;
        return counts;
    }
    auto active = xvm::subos_workspace_from_json(*wsIt).active;
    auto db = Config::versions();

    std::set<std::string> releases;
    for (const auto& [target, version] : active) {
        if (version.empty()) continue;
        const auto* vd = xvm::get_vdata(db, target, version);
        if (vd != nullptr && vd->bindingGroup.has_value()
            && !vd->bindingGroup->rootTarget.empty()) {
            releases.insert(vd->bindingGroup->rootTarget + "@"
                            + vd->bindingGroup->rootVersion);
        } else {
            releases.insert(target + "@" + version);
        }
    }
    counts.packages = static_cast<int>(releases.size());
    return counts;
}

SubosInfo candidate_info_(const std::string& name, bool includeToolCount) {
    auto& p = Config::paths();
    auto dir = Config::subos_dir(name);
    auto counts = includeToolCount ? count_subos_(dir, true) : SubosCounts{};
    return {name, dir, p.activeSubos == name, counts.commands, counts.packages};
}

SubosCandidateView candidate_view(bool includeToolCount) {
    auto& p = Config::paths();
    auto json = read_config_json_(p.homeDir / ".xlings.json");
    SubosCandidateView view;
    bool hasDefault = false;

    if (json.contains("subos") && json["subos"].is_object()) {
        for (auto it = json["subos"].begin(); it != json["subos"].end(); ++it) {
            auto name = it.key();
            hasDefault = hasDefault || name == "default";
            view.candidates.push_back(candidate_info_(name, includeToolCount));
        }
    }

    // Bounded compatibility for homes created before the registry existed.
    // Only the well-known default manifest is synthesized. Arbitrary
    // directories are never promoted into environments by a read-only query.
    std::error_code ec;
    const auto defaultManifest =
        Config::subos_dir("default") / ".xlings.json";
    if (!hasDefault && fs::is_regular_file(defaultManifest, ec)) {
        view.candidates.push_back(
            candidate_info_("default", includeToolCount));
    }

    std::ranges::sort(view.candidates, {}, &SubosInfo::name);
    return view;
}

std::vector<SubosInfo> list_all() {
    return candidate_view().candidates;
}

// The rules live in the SubOS core (xlings.subos.model) and work on names;
// this maps their answer back to the instances it is about.
CandidateResolution_ resolve_candidate_(std::string_view query) {
    auto all = candidate_view(false).candidates;
    std::vector<std::string> names;
    names.reserve(all.size());
    for (const auto& c : all) names.push_back(c.name);

    auto r = model::resolve_name(query, names);
    CandidateResolution_ out{ .selected = std::move(r.selected),
                              .reason = std::move(r.reason),
                              .autoSelected = r.autoSelected };
    for (const auto& name : r.matches) {
        if (auto it = std::ranges::find(all, name, &SubosInfo::name); it != all.end())
            out.candidates.push_back(*it);
    }
    return out;
}

void emit_candidates_(EventStream& stream,
                      const CandidateResolution_& resolution,
                      std::string_view query,
                      std::string_view hint = {}) {
    nlohmann::json candidates = nlohmann::json::array();
    for (const auto& candidate : resolution.candidates) {
        candidates.push_back({
            {"name", candidate.name},
            {"active", candidate.isActive},
            {"dir", candidate.dir.string()},
            {"commands", candidate.commandCount},
            {"packages", candidate.packageCount},
        });
    }
    nlohmann::json payload;
    payload["reason"] = resolution.reason;
    payload["query"] = query;
    payload["candidates"] = std::move(candidates);
    payload["auto_selected"] = resolution.autoSelected;
    if (!resolution.selected.empty()) payload["selected"] = resolution.selected;
    if (!hint.empty()) payload["hint"] = hint;
    stream.emit(DataEvent{"subos_candidates", payload.dump()});
}

UseNameResolution_ resolve_use_name_(std::string_view query,
                                    EventStream& stream) {
    auto resolution = resolve_candidate_(query);
    if (query.empty()) {
        // A list of every subos, followed by "now type one of these".
        //
        // That is the shape the picker exists for, and this is the command
        // that produces the longest list -- 45 on a working machine. Offered
        // when somebody can answer; the list below is unchanged for everyone
        // else, and is a complete answer rather than an error.
        if (!resolution.candidates.empty() && stream.interactive()) {
            PromptEvent pick;
            pick.id = "select_subos";
            pick.question = std::string(i18n::tr("ui.select_subos"));
            pick.kind = PromptEvent::Kind::Select;
            for (const auto& c : resolution.candidates) pick.options.push_back(c.name);
            pick.defaultValue = Config::paths().activeSubos;

            std::optional<UseNameResolution_> done;
            std::visit(EventStream::on{
                [&](EventStream::Chosen&& c) {
                    done = UseNameResolution_{.selected = std::move(c.value)};
                },
                [&](EventStream::Cancelled&&) {
                    log::println("cancelled");
                    done = UseNameResolution_{};
                },
                [&](EventStream::NobodyToAsk&&) {},   // fall through to the list
            }, stream.prompt(std::move(pick)));
            if (done) return *done;
        }
        const auto hint = resolution.candidates.empty()
            ? "Create: xlings subos new <name>"
            : "Use: xlings subos use <name>";
        emit_candidates_(stream, resolution, query, hint);
        return {};
    }
    if (!resolution.selected.empty()) {
        if (resolution.autoSelected) {
            emit_candidates_(stream, resolution, query);
        }
        return {.selected = std::move(resolution.selected)};
    }
    if (resolution.reason == "ambiguous") {
        emit_candidates_(stream, resolution, query);
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = std::format("subos name '{}' is ambiguous", query),
            .recoverable = true,
            .hint = "use one of the exact names listed above",
        });
        return {.exitCode = 2};
    }

    emit_candidates_(stream, resolution, query);
    std::string nearest;
    for (const auto& candidate : resolution.candidates) {
        if (!nearest.empty()) nearest += ", ";
        nearest += candidate.name;
    }
    stream.emit(ErrorEvent{
        .code = ErrorCode::NotFound,
        .message = std::format("subos '{}' not found", query),
        .recoverable = true,
        .hint = nearest.empty()
            ? "create it first: xlings subos new " + std::string(query)
            : "did you mean: " + nearest,
    });
    return {.exitCode = 1};
}

void update_current_symlink_(EventStream& stream,
                              const fs::path& homeDir,
                              const fs::path& targetDir) {
    auto linkPath = homeDir / "subos" / "current";
    std::error_code ec;
    fs::remove(linkPath, ec);
    // The same helper `self init` uses to create this link, rather than
    // `fs::create_directory_symlink` directly. On Windows a symlink needs
    // developer mode or elevation and a junction does not, so the two spellings
    // disagree exactly there: init would lay down a junction and every later
    // switch would fail to replace it, leaving `subos/current` pointing at
    // whatever was active the day the home was created -- while `subos list`
    // reported the switch as done.
    if (!platform::create_directory_link(linkPath, targetDir)) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Permission,
            .message = std::format("failed to update current symlink: {}",
                                   Config::display_path(linkPath)),
            .recoverable = true,
        });
    }
}

// ─────────────────────────────────────────────────────────────────────
// Sandbox subos helpers (0.4.23 V4 — see .agents/docs/sandbox-v4-design.md)
//
// V4 model: sandbox is NOT a creation property of subos. It's a `use`
// modifier — `xlings subos use <name> --sandbox` enters the same subos
// via proot fs-isolation, with sandbox-private $HOME / /tmp / /etc and
// host-shared ~/.xlings / /usr / /lib*. Real user identity (no fake
// root). Shell is whatever $SHELL is. Prompt switches from
// `[xsubos:<name>]` to `<xsubos:<name>>` to signal sandbox mode.
//
// Helpers here:
//   - sandbox::init_sandbox_dirs_ — lazy-init the per-subos
//     sandbox dirs (<subos>/{home/<user>, tmp, etc/...}) at first
//     `subos use --sandbox`. Idempotent.
//   - sandbox::locate_proot_ — search for the proot binary
//     (defined later, alongside build_proot_argv_).
//   - sandbox::build_proot_argv_ — assemble the proot CLI for
//     entering the sandbox (defined later).
//
// Note: V1.1-V1.3 (`--sandbox-shell <xpkg>`, `sandbox-shell` /
// `sandbox-shell-xpkg` config fields, eager shell install at
// create-time) was removed in V4. Old sandboxes still in user homes
// retain those fields; V4 silently ignores them, and `subos use
// --sandbox` works on them because init_sandbox_dirs_ is idempotent.
// ─────────────────────────────────────────────────────────────────────


// Give a subos directory a `subos_info` block, or leave the one it has.
//
// Idempotent, and called from every path that produces a subos directory
// (create, new_from, and the migration of a subos made before this block
// existed). A subos without it violates invariant I4 and cannot be described,
// checked or entered with its environment.
//
// The block is added even when `.xlings.json` already exists, which is the
// difference from the surrounding code: the file predates the block, so
// "the file is there" does not mean "the subos describes itself".
bool ensure_subos_info_(const fs::path& dir, manifest::Intent intent,
                        std::string_view requested) {
    // An unparseable manifest is not an empty one: rebuilding it from {} would
    // write a blank workspace over the subos's real one (home::read_json_for_update).
    auto read = home::read_json_for_update(dir / ".xlings.json");
    if (!read) {
        log::warn("{} -- not rewriting it", read.error());
        return false;
    }
    auto json = std::move(*read);
    if (!json.contains("workspace")) json["workspace"] = nlohmann::json::object();

    // Only replace a block that is absent or unusable. Rewriting a valid one
    // would discard the envs a package declared into it.
    if (manifest::validate_block(json).empty()) return true;

    // An unusable block can still carry a valid runtime binding, and that
    // binding — not the caller's request — is what the subos was declared
    // against. runtime_for keeps it through the rebuild.
    // The default is resolved rather than compiled in, for the same reason
    // create() resolves it: this constant and the index's `latest` were one
    // decision in two repositories. Silent here -- a rebuild is not the place
    // to teach someone about the index -- and the source is left unset,
    // because a recorded or observed binding outranks the default and this
    // call cannot see which step answered.
    // Resolved only for Create. Under Describe step 5 does not exist, so the
    // default is never consulted -- and asking would make every repair and
    // every rebuild rebuild the catalog for an answer it then discards.
    auto runtime = intent == manifest::Intent::Create
        ? manifest::runtime_for(dir, json, intent, requested,
                                resolve_default_runtime().binding)
        : manifest::runtime_for(dir, json, intent, requested);

    // Carry the recorded ABI across the rebuild when the binding did not
    // change. It is a creation-time fact -- what the package DECLARED it
    // provides -- and a repair has no evidence to re-derive it: asking the
    // index here is exactly what the comment above rules out. Dropping it
    // would let `doctor --fix` erase something only the original install
    // could know, which is the shape that once gave two different subos
    // byte-identical `created_at` values.
    const auto prior = manifest::parse(json);
    std::string carriedAbi;
    if (!prior.runtime_abi.empty() && prior.runtime == runtime)
        carriedAbi = prior.runtime_abi;

    json[std::string(manifest::BLOCK)] = manifest::make_block({
        .runtime    = std::move(runtime),
        .by         = std::format("xlings {}", Info::VERSION),
        .hostGlibc  = platform::host_glibc_version(),
        .intent     = intent,
        .runtimeAbi = std::move(carriedAbi),
    });
    try {
        write_config_json_(dir / ".xlings.json", json);
    } catch (const std::exception& e) {
        // std::string on both, not a bare const char*.
        //
        // clang instantiated log::error<std::string, const char*> down a path
        // that ends in formatter<const char*, wchar_t> -- deleted -- and gcc
        // did not. Whether it trips depends on what else the TU imports, so
        // it appeared here when an unrelated import was added.
        log::error("failed to write subos manifest {}: {}",
                   (dir / ".xlings.json").string(), std::string(e.what()));
        return false;
    }
    return true;
}

// What the runtime package says it provides, asked of the index rather than
// derived from the name.
//
// Empty is a legitimate answer and must not be replaced by a guess here: a
// hosted runtime has no recipe to ask, an older recipe may declare no `abi`,
// and an offline home cannot look. Readers fall back to family_of() and know
// that what they got was DERIVED -- which is a different fact from "the
// package told us", and worth being able to tell apart.
std::string runtime_abi_for(std::string_view binding) {
    if (manifest::runtime_is_hosted(binding)) return {};
    auto abi = xim::index_runtime_abi_of(manifest::runtime_query_for(binding));
    return abi ? *abi : std::string{};
}

DefaultRuntime resolve_default_runtime() {
    // Queried by coordinate, recorded by name -- see the two constants.
    const std::string pkg{manifest::DEFAULT_RUNTIME_PACKAGE};

    // The missing access argument is a decision, not an omission: every caller
    // of THIS function (`subos new`, fork, block rebuild) runs on a home that
    // has already been initialized, so the index is normally on disk and a
    // LocalOnly read answers without touching the network. Only `self init`
    // decides a binding on a home that has never had one -- it passes
    // InstallReady, and it has to resolve the same decision separately because
    // xself cannot import this module without closing a cycle (see the comment
    // at that call site).
    //
    // So: two copies of one decision, deliberately differing in exactly one
    // argument. If you unify them, keep the difference -- making every rebuild
    // and every fork able to sync would put a network round trip behind
    // frequent, previously offline operations.
    if (auto version = xim::index_version_of(manifest::DEFAULT_RUNTIME_QUERY))
        return DefaultRuntime{.binding = pkg + "@" + *version, .resolved = true};
    return DefaultRuntime{
        .binding = std::string(manifest::DEFAULT_RUNTIME_FALLBACK),
        .resolved = false,
    };
}

// A subos name has to name a subos.
//
// "" is not a subos, and every path this file builds from a name is
// `<home>/subos/<name>` -- so an empty one resolves to the subos ROOT. That is
// not a theoretical concern: on 2026.9.16.1, two malformed agent calls
//
//     xlings interface create_subos --args '{}'    -> exitCode 0
//     xlings interface remove_subos --args '{}'    -> exitCode 0
//
// registered an entry named "" and then removed `<home>/subos/` with it,
// deleting `default`, `current` and every other subos in the home, reporting
// success both times. The capability layer passed `json.value("name", "")`
// straight through; the CLI never could, because its parser demands the
// argument ("missing <name> for: xlings subos remove|rm").
//
// The interface dispatcher now refuses a params object that omits a declared
// `required` field, which stops that call before it reaches here. This is the
// operation's OWN precondition, which is a different statement: whatever asks,
// there is no subos called "", and nothing may be created at or removed from
// the path an empty name builds.

bool reject_empty_subos_name_(std::string_view what, const std::string& name,
                              EventStream& stream) {
    if (!name.empty()) return false;
    stream.emit(ErrorEvent{
        .code = ErrorCode::InvalidInput,
        .message = std::string("a subos name is required to ") + std::string(what),
        .recoverable = false,
        .hint = "an empty name resolves to the subos root, not to a subos",
    });
    return true;
}

int create(const std::string& name, const fs::path& customDir,
                  sandbox::StorageMode storage, const std::string& imageSize,
                  const std::string& runtime,
                  EventStream& stream) {
    return create(name, customDir, storage, imageSize, runtime,
                  /*yes=*/false, "-y", stream);
}

int create(const std::string& name, const fs::path& customDir,
           sandbox::StorageMode storage, const std::string& imageSize,
           const std::string& runtime, bool yes, std::string_view yesSpelling,
           EventStream& stream) {
    auto& p = Config::paths();

    if (reject_empty_subos_name_("create a subos", name, stream)) return 1;

    if (name == "current") {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "'current' is a reserved subos name",
            .recoverable = false,
        });
        return 1;
    }

    // Checked before anything is laid down. A malformed runtime that only
    // surfaced at write time would leave a registered subos that cannot
    // satisfy its own invariants.
    std::string effectiveRuntime = runtime;
    std::string runtimeSource{manifest::RUNTIME_SOURCE_EXPLICIT};
    if (effectiveRuntime.empty()) {
        const auto def = resolve_default_runtime();
        effectiveRuntime = def.binding;
        runtimeSource = std::string(def.resolved ? manifest::RUNTIME_SOURCE_INDEX
                                                 : manifest::RUNTIME_SOURCE_FALLBACK);
        if (!def.resolved) {
            // Said out loud, because the alternative is a subos silently
            // pinned to whatever this build was compiled with while the index
            // has moved on -- and the failure that follows names a payload
            // directory, not a decision anybody remembers making.
            //
            // Both remedies, because they answer different situations: the
            // index has never been synced here, or the caller knew the answer
            // all along.
            stream.emit(LogEvent{
                .level = LogLevel::warn,
                .message = "index could not answer which "
                           + std::string(manifest::DEFAULT_RUNTIME_PACKAGE)
                           + " to bind to; using the built-in " + effectiveRuntime
                           + ". Run `xlings update` and recreate, or pass "
                             "`--runtime <package>@<version>`.",
            });
        }
    }
    if (!manifest::is_binding(effectiveRuntime)) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "invalid --runtime '" + effectiveRuntime
                       + "' (expected <package>@<version>, e.g. glibc@2.39)",
            .recoverable = false,
        });
        return 1;
    }

    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = "invalid subos name: '" + name
                           + "' (allowed: alphanumeric, underscore, dash)",
                .recoverable = false,
            });
            return 1;
        }
    }

    // Cheap pre-check so the common "already exists" mistake fails before we
    // lay down directories or run mkfs. It is advisory only -- the binding
    // check that actually decides is the one inside the locked commit below.
    if (read_home_config(p.homeDir).value("subos", nlohmann::json::object())
            .contains(name)) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "subos '" + name + "' already exists",
            .recoverable = false,
        });
        return 1;
    }

    auto dir = customDir.empty() ? (p.homeDir / "subos" / name) : customDir;

    // A directory that is already there but is not a registered subos holds
    // somebody's files: a subos whose registration was lost, or -- through
    // `dir` -- any directory at all. It used to be built into silently and
    // reported as "created", and a later `subos remove` then deleted files
    // xlings had never made. Taking it over is now the user's call.
    std::error_code existEc;
    bool adopting = fs::exists(dir, existEc) && !fs::is_empty(dir, existEc);
    if (adopting) {
        // A bare skeleton holds nothing of anyone's: no home files and no
        // file outside the entries xlings manages. That is what a create the
        // state lock refused leaves behind (its directories are laid down
        // before the locked commit), and retrying it must just work -- asking
        // permission to take over our own leftovers is noise, not safety.
        const auto held = userdata::census(dir);
        if (held.home.files == 0 && held.otherFiles == 0) adopting = false;
    }
    if (adopting) {
        const auto held = destructive_log::measure(dir);
        auto asked = confirm::ask(
            stream, "subos_adopt",
            std::format("{} already exists and is not a registered subos ({} in {} "
                        "file(s)). Adopt it as subos '{}', keeping everything in it?",
                        dir.string(), destructive_log::human_bytes(held.bytes),
                        held.files, name),
            yes, yesSpelling);
        if (asked.outcome == confirm::Outcome::Declined) {
            log::println("cancelled; nothing was created");
            return 0;
        }
        if (asked.outcome == confirm::Outcome::NobodyToAsk) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = std::format(
                    "{} already exists and is not a registered subos ({} in {} "
                    "file(s)); nothing was created",
                    dir.string(), destructive_log::human_bytes(held.bytes), held.files),
                .recoverable = true,
                .hint = yesSpelling == "yes:true"
                    ? std::string("adopting it registers that directory, contents and "
                                  "all, as this subos -- the user's decision. Call "
                                  "create_subos again with \"yes\": true to adopt it, "
                                  "or use another name")
                    : std::string("re-run with -y to adopt it as it is, or use another name"),
            });
            return 2;
        }
    }

    // What THIS run creates, so a rollback takes back exactly that and never
    // a file that was already there.
    std::vector<fs::path> createdHere;
    for (const auto* sub : {"bin", "lib", "usr", "generations"}) {
        const auto d = dir / sub;
        std::error_code probeEc;
        if (!fs::exists(d, probeEc)) createdHere.push_back(d);
        // The throwing overload on purpose: a read-only home must surface as
        // the permission error it is (the top-level handler says "not
        // writable"), not as a failed write three steps later that reads as
        // an internal bug.
        fs::create_directories(d);
    }
    if (!fs::exists(dir / ".xlings.json", existEc)) createdHere.push_back(dir / ".xlings.json");

    auto subosConfig = dir / ".xlings.json";
    if (!fs::exists(subosConfig)) {
        nlohmann::json j;
        j["workspace"] = nlohmann::json::object();
        if (storage != sandbox::StorageMode::Shared)
            j["storage"] = sandbox::storage_to_string_(storage);
        if (storage == sandbox::StorageMode::Image)
            j["imageSize"] = imageSize;
        auto newRuntime = manifest::runtime_for(
            dir, j, manifest::Intent::Create, effectiveRuntime);
        auto newRuntimeAbi = runtime_abi_for(newRuntime);
        j[std::string(manifest::BLOCK)] = manifest::make_block({
            .runtime    = std::move(newRuntime),
            .by         = std::format("xlings {}", Info::VERSION),
            .hostGlibc  = platform::host_glibc_version(),
            .intent     = manifest::Intent::Create,
            // Claimed only here. This is the one path that KNOWS: the value
            // was decided above, before runtime_for was asked, and step 2
            // returns it verbatim. The other two Create call sites hand
            // runtime_for a resolved default but cannot tell whether it was
            // the step that answered -- a recorded or observed binding
            // outranks it -- so they leave this empty, which reads as
            // "unknown" rather than as a fourth value.
            .runtimeSource = runtimeSource,
            .runtimeAbi    = std::move(newRuntimeAbi),
        });
        write_config_json_(subosConfig, j);
    } else if (!ensure_subos_info_(dir, manifest::Intent::Create,
                                   effectiveRuntime)) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = "failed to write the subos manifest for '" + name + "'",
            .recoverable = false,
            .hint = "check write permission on " + subosConfig.string(),
        });
        return 1;
    }

    // Image mode: create sparse ext4 image
    if (storage == sandbox::StorageMode::Image) {
        auto rc = sandbox::init_image_(dir / "home.img", imageSize);
        if (rc != 0) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Internal,
                .message = "failed to create home.img",
                .recoverable = false,
                .hint = "ensure mkfs.ext4 is available (e2fsprogs)",
            });
            return 1;
        }
        fs::create_directories(dir / ".mountpoint");
    }

    // Create shim hardlinks from xlings binary. `xlings_binary_in_home` is
    // the one answer to where that is: the spelling this replaced had no
    // `.exe`, so on Windows a new subos got no `xlings` shim at all.
    auto xlingsBin = xself::xlings_binary_in_home(p.homeDir);
    if (!xlingsBin.empty()) {
        if (xself::ensure_subos_shims(dir / "bin", xlingsBin, p.homeDir) != 0) {
            log::warn("some shims could not be written into {}; commands may "
                      "not resolve inside this subos", (dir / "bin").string());
        }
    }

    // Everything above this point -- mkfs.ext4 for image storage in
    // particular -- can take seconds. Reading the config before it and
    // writing it after would put back a document that predates any install
    // that finished meanwhile. Re-read under the lock and edit only our key.
    bool raced = false;
    auto committed = update_home_config(p.homeDir, [&](nlohmann::json& json) {
        if (!json.contains("subos") || !json["subos"].is_object()) {
            json["subos"] = nlohmann::json::object();
        }
        if (json["subos"].contains(name)) {
            raced = true;
            return false;
        }
        json["subos"][name] =
            {{"dir", customDir.empty() ? "" : customDir.string()}};
        return true;
    });
    if (!committed) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = "subos '" + name + "' was created on disk but could "
                       "not be recorded: " + committed.error(),
            .recoverable = true,
            .hint = "retry once the other xlings finishes; the directory at "
                    + dir.string() + " is reused as-is",
        });
        return 1;
    }
    if (raced) {
        // Another xlings registered this name while we were building. Its
        // entry is the one on disk; ours would overwrite a directory the
        // other command is using.
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "subos '" + name + "' already exists",
            .recoverable = false,
        });
        return 1;
    }

    // The subos is registered; check it can actually satisfy the invariants
    // before saying so. A creation that reports success and leaves a subos
    // that doctor immediately condemns is the failure mode this whole slice
    // exists to remove -- "it happened" and "it worked" must not look alike.
    if (auto findings = manifest::validate(dir); !findings.empty()) {
        std::string detail;
        for (const auto& f : findings) {
            if (!detail.empty()) detail += "; ";
            detail += std::string(manifest::describe(f.kind));
            if (!f.detail.empty()) detail += " (" + f.detail + ")";
        }
        // Roll back to the state before the command: the registry entry first,
        // since that is what makes the name unusable a second time.
        (void)update_home_config(p.homeDir, [&](nlohmann::json& json) {
            if (json.contains("subos") && json["subos"].is_object())
                json["subos"].erase(name);
            return true;
        });
        // Only what this run made. A directory that was already there keeps
        // everything it held; one this run created goes entirely.
        std::error_code rmec;
        if (adopting) {
            for (const auto& made : createdHere) fs::remove_all(made, rmec);  // subos-remove-all-ok: only entries this run created
        } else {
            fs::remove_all(dir, rmec);  // subos-remove-all-ok: created by this run
        }
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = "subos '" + name + "' did not come out valid: " + detail,
            .recoverable = false,
            .hint = rmec
                ? "rolled back the registry entry, but " + dir.string()
                  + " could not be removed -- delete it before retrying"
                : "nothing was left behind; retry, or report this",
        });
        return 1;
    }

    // An eager install of the declared runtime -- now an OPTIMISATION, not
    // the thing that makes the declaration true.
    //
    // It used to be load-bearing, and only on this branch. The failure it
    // described was real: "the first package to arrive decides the actual
    // glibc — a subos declaring 2.39 ends up on 2.44 because something's
    // `>=2.38` resolved higher." But it only ever guarded the path where
    // `--runtime` was passed EXPLICITLY, and the paths that take a default --
    // `subos new` bare, and the `subos/default` that `self init` creates, the
    // one every new user gets -- were left recording a runtime nobody
    // installed.
    //
    // Correctness now comes from resolution instead: a subos's declared
    // runtime outranks both what happens to be active and what the index
    // calls newest (`subos_version_of_` in xim/commands.cpp). That holds on
    // all three creation paths, so this branch no longer has to.
    //
    // Kept, because a user who names a runtime has said what they want and
    // should get it now rather than on first use. Still only when asked:
    // `subos new` without --runtime stays a local directory operation that
    // cannot fail on a network.
    if (!runtime.empty()) {
        // Both, and that is not belt-and-braces. The override is what
        // recomputes Config's cached paths, and XLINGS_ACTIVE_SUBOS is what
        // the activation path re-reads for itself. Setting only the override
        // put the payload in the right subos and then reported
        // "'glibc' is not installed in this subos" from the reader that had
        // not been told — a command that succeeds narrating a failure.
        auto prevEnv = utils::get_env_or_default("XLINGS_ACTIVE_SUBOS");
        platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
        auto prev = Config::set_active_subos_override(name);
        std::vector<std::string> targets{effectiveRuntime};
        // useAfterInstall stays FALSE. The payload is usually already in the
        // store — another subos has it — so cmd_install takes its
        // "already installed" path, and the activation that flag triggers
        // runs before the registration for THIS subos has landed. It fails,
        // prints three [error] lines and a [warn], and the subos ends up
        // correct anyway. A command that succeeds must not narrate a failure.
        const int rc = xim::cmd_install(targets, /*yes=*/true,
                                        /*noDeps=*/false, stream);
        (void)Config::set_active_subos_override(prev);
        platform::set_env_variable("XLINGS_ACTIVE_SUBOS", prevEnv);
        if (rc != 0) {
            // The subos exists and is valid; it just does not have what it
            // says it runs on. Reported rather than rolled back, because the
            // directory is usable and deleting it would throw away a
            // successful create over a failed download.
            stream.emit(ErrorEvent{
                .code = ErrorCode::Internal,
                .message = "subos '" + name + "' was created but its declared "
                           "runtime " + effectiveRuntime
                           + " could not be installed",
                .recoverable = true,
                .hint = "run: xlings subos use " + name
                        + " && xlings install " + effectiveRuntime,
            });
            return rc;
        }
    }

    nlohmann::json payload;
    payload["name"] = name;
    payload["dir"]  = dir.string();
    payload["storage"] = sandbox::storage_to_string_(storage);
    payload["runtime"] = effectiveRuntime;
    if (adopting) payload["adopted"] = true;
    stream.emit(DataEvent{"subos_created", payload.dump()});
    return 0;
}

int create(const std::string& name, const fs::path& customDir,
                  sandbox::StorageMode storage, const std::string& imageSize,
                  EventStream& stream) {
    return create(name, customDir, storage, imageSize, "", stream);
}

int create(const std::string& name, const fs::path& customDir,
                  EventStream& stream) {
    return create(name, customDir, sandbox::StorageMode::Shared, "50G", "", stream);
}

namespace new_from_detail_ {

bool is_pkg_spec_(const std::string& spec) {
    return spec.find(':') != std::string::npos || spec.find('@') != std::string::npos;
}

PkgRef parse_pkg_spec_(const std::string& spec) {
    PkgRef r;
    std::string rest = spec;
    if (auto colon = rest.find(':'); colon != std::string::npos) {
        r.ns = rest.substr(0, colon);
        rest = rest.substr(colon + 1);
    }
    if (auto at = rest.find('@'); at != std::string::npos) {
        r.name = rest.substr(0, at);
        r.ver  = rest.substr(at + 1);
    } else {
        r.name = rest;
    }
    return r;
}

// Recursive directory copy with reflink/clonefile preferred where the
// filesystem supports COW. Falls back to full byte-copy. Excludes the
// caller's xlings binary shims (those are minted fresh in the target).
int copy_tree_(const fs::path& src, const fs::path& dst,
               EventStream& stream) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = "failed to create fork target dir: " + ec.message(),
            .recoverable = false,
        });
        return 1;
    }

    // Use the system cp with reflink/clonefile flags; falls back to
    // full copy when the FS doesn't support it. Skip the bin/ subtree
    // here — shims are regenerated below.
    std::string copy_cmd;
    if constexpr (platform::is_linux) {
        // cp -a preserves mode/ownership/timestamps; --reflink=auto uses
        // COW where available (btrfs/xfs) and full copy otherwise.
        copy_cmd = std::format(
            "cp -a --reflink=auto '{}/.' '{}/'", src.string(), dst.string());
    } else if constexpr (platform::is_macos) {
        // APFS clonefile via /bin/cp -c
        copy_cmd = std::format("cp -ac '{}/.' '{}/'", src.string(), dst.string());
    }

    if (!copy_cmd.empty()) {
        auto rc = std::system(copy_cmd.c_str());
        if (rc != 0) {
            log::warn("cp -a/--reflink failed (rc={}), falling back to "
                      "std::filesystem::copy", rc);
            fs::copy(src, dst,
                     fs::copy_options::recursive |
                     fs::copy_options::overwrite_existing |
                     fs::copy_options::copy_symlinks, ec);
            if (ec) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::Internal,
                    .message = "fork copy failed: " + ec.message(),
                    .recoverable = false,
                });
                return 1;
            }
        }
    } else {
        // Windows / generic
        fs::copy(src, dst,
                 fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing |
                 fs::copy_options::copy_symlinks, ec);
        if (ec) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Internal,
                .message = "fork copy failed: " + ec.message(),
                .recoverable = false,
            });
            return 1;
        }
    }
    return 0;
}

// Locate xpkgs/<ns>-x-<name>/<ver>/ for a parsed pkg ref. Returns empty
// path if no matching install exists.
fs::path locate_base_pkg_(const PkgRef& ref) {
    auto& p = Config::paths();
    auto storeName = ref.ns.empty() ? ref.name : (ref.ns + "-x-" + ref.name);
    auto base = p.dataDir / "xpkgs" / storeName;
    if (!fs::is_directory(base)) return {};

    // Specific version requested
    if (!ref.ver.empty()) {
        auto candidate = base / ref.ver;
        return fs::is_directory(candidate) ? candidate : fs::path{};
    }

    // No version → take the highest-sorted installed version directory.
    fs::path latest;
    std::error_code ec;
    for (auto it = fs::directory_iterator(base, ec);
         !ec && it != std::default_sentinel; it.increment(ec)) {
        if (it->is_directory(ec)) latest = it->path();
    }
    return latest;
}

}

int new_from(const std::string& name, const fs::path& customDir,
                    sandbox::StorageMode storage, const std::string& imageSize,
                    const std::string& fromSpec, const std::string& runtime,
                    bool yes, EventStream& stream) {
    auto& p = Config::paths();

    fs::path baseDir;

    if (new_from_detail_::is_pkg_spec_(fromSpec)) {
        // ── pkg-spec path: locate or install the base xpkg ────────────
        auto ref = new_from_detail_::parse_pkg_spec_(fromSpec);
        baseDir = new_from_detail_::locate_base_pkg_(ref);

        if (baseDir.empty()) {
            // Auto-install (E5a): invoke `xlings install <spec>` so the
            // base lands at xpkgs/<ns>-x-<name>/<ver>/. We use the host
            // xlings binary (same process binary) so the install runs
            // with the same context (XLINGS_HOME, mirror config, etc.).
            log::info("base subos pkg '{}' not installed; auto-installing...",
                      fromSpec);
            auto xlings_bin = xself::xlings_binary_in_home(p.homeDir);

            auto cmd = std::format("{} install -y {}",
                                   xlings_bin.string(), fromSpec);
            auto rc = std::system(cmd.c_str());
            if (rc != 0) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::Internal,
                    .message = "auto-install of base '" + fromSpec
                               + "' failed",
                    .recoverable = true,
                    .hint = "run manually: xlings install " + fromSpec,
                });
                return 1;
            }
            baseDir = new_from_detail_::locate_base_pkg_(ref);
        }

        if (baseDir.empty()) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "couldn't locate base pkg payload for '" + fromSpec
                           + "' after install",
                .recoverable = false,
            });
            return 1;
        }
    } else {
        // ── local fork path: source is an existing subos by name ──────
        baseDir = p.homeDir / "subos" / fromSpec;
        if (!fs::is_directory(baseDir)) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::NotFound,
                .message = "source subos '" + fromSpec + "' not found",
                .recoverable = true,
                .hint = "list available: xlings subos list",
            });
            return 1;
        }
    }

    // Validate base shape: must contain .xlings.json for fork to make sense
    if (!fs::is_regular_file(baseDir / ".xlings.json")) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "source '" + fromSpec
                       + "' has no .xlings.json (not a valid fork source)",
            .recoverable = false,
        });
        return 1;
    }

    // Create target subos via standard `create`. This sets up
    // bin/lib/usr/generations, writes initial .xlings.json, optionally
    // creates home.img, and registers the subos.
    if (auto rc = create(name, customDir, storage, imageSize, runtime, yes, "-y", stream);
        rc != 0) {
        return rc;
    }

    auto dstDir = customDir.empty() ? (p.homeDir / "subos" / name) : customDir;

    // Overlay base content on top — workspace (.xlings.json), any
    // templates/static files. We re-issue create()'s file writes
    // afterwards for storage/imageSize keys so the new subos's own
    // storage choice wins over the base's. The base's .xlings.json
    // workspace map is the data we want to inherit.
    if (auto rc = new_from_detail_::copy_tree_(baseDir, dstDir, stream); rc != 0) {
        return rc;
    }

    // Restore storage/imageSize fields in target's .xlings.json since
    // copy_tree_ overwrote it with base's version (base usually has no
    // explicit storage key — it inherits at fork time).
    auto subosCfgPath = dstDir / ".xlings.json";
    auto subosCfg = read_config_json_(subosCfgPath);
    if (storage != sandbox::StorageMode::Shared)
        subosCfg["storage"] = sandbox::storage_to_string_(storage);
    else
        subosCfg.erase("storage");
    if (storage == sandbox::StorageMode::Image)
        subosCfg["imageSize"] = imageSize;
    else
        subosCfg.erase("imageSize");
    // Same restoration, same reason. copy_tree_ replaced the manifest create()
    // wrote with the base's, and a base built before subos_info existed has
    // none -- which would leave the fork registered and failing its own
    // invariants. A base that does carry one keeps it: it describes the very
    // content that was just copied in, envs included.
    if (!subosCfg.contains("workspace"))
        subosCfg["workspace"] = nlohmann::json::object();
    if (!manifest::validate_block(subosCfg).empty()) {
        auto forkRuntime = manifest::runtime_for(
            dstDir, subosCfg, manifest::Intent::Create, runtime,
            resolve_default_runtime().binding);
        auto forkAbi = runtime_abi_for(forkRuntime);
        subosCfg[std::string(manifest::BLOCK)] = manifest::make_block({
            .runtime    = std::move(forkRuntime),
            .by         = std::format("xlings {}", Info::VERSION),
            .hostGlibc  = platform::host_glibc_version(),
            .intent     = manifest::Intent::Create,
            .runtimeAbi = std::move(forkAbi),
        });
    }
    // `--runtime` loses to what the fork actually carries, and that is right:
    // copy_tree_ just brought the base's payloads across, and declaring them
    // against a libc they were not built for would be a lie the fork cannot
    // satisfy. What was NOT right is doing it in silence -- the flag parsed,
    // was accepted, and vanished. Say so at the moment it stops mattering.
    if (!runtime.empty()) {
        const auto effective =
            manifest::parse(subosCfg).runtime;
        if (!effective.empty() && effective != runtime) {
            log::warn("--runtime {} ignored: '{}' inherits {} from its base, "
                      "which is what the payloads it just copied were built "
                      "against", runtime, name, effective);
            log::warn("  to choose a runtime, create without --from: "
                      "xlings subos new {} --runtime {}", name, runtime);
        }
    }
    write_config_json_(subosCfgPath, subosCfg);

    // Re-mint subos shims (they may have been clobbered by copy_tree_
    // if base happened to ship its own bin/ — defensive).
    auto xlingsBin = xself::xlings_binary_in_home(p.homeDir);
    if (!xlingsBin.empty()) {
        if (xself::ensure_subos_shims(dstDir / "bin", xlingsBin, p.homeDir) != 0) {
            log::warn("some shims could not be written into {}; commands may "
                      "not resolve inside this subos", (dstDir / "bin").string());
        }
    }

    nlohmann::json payload;
    payload["name"]    = name;
    payload["from"]    = fromSpec;
    payload["base"]    = baseDir.string();
    payload["storage"] = sandbox::storage_to_string_(storage);
    stream.emit(DataEvent{"subos_forked", payload.dump()});
    return 0;
}

 // namespace use_detail_

// Internal — not exported. `xlings subos use --global <name>` and
// the back-compat single-arg `use()` both route here.
int use_global(const std::string& name, EventStream& stream) {
    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;

    auto& p = Config::paths();
    // The window here is short, but a full-document rewrite is a full-document
    // rewrite: an install committing between the read and the write loses its
    // `versions` entry all the same.
    auto committed = update_home_config(p.homeDir, [&](nlohmann::json& json) {
        json["activeSubos"] = name;
        return true;
    });
    if (!committed) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = "failed to switch subos: " + committed.error(),
            .recoverable = true,
        });
        return 1;
    }

    auto dir = Config::subos_dir(name);
    update_current_symlink_(stream, p.homeDir, dir);

    nlohmann::json payload;
    payload["name"] = name;
    payload["dir"]  = dir.string();
    stream.emit(DataEvent{"subos_switched", payload.dump()});
    return 0;
}

// Internal — not exported. Powers the hidden `--shell <kind>` flag,
// kept available for tests and power users that want eval-able output
// without a sub-shell layer. The default user-facing path is
// use_spawn_shell; --shell is intentionally not in the help text.
// Shell-level entry never activates storage isolation (V4 orthogonality
// — see use_spawn_shell). Emit a single hint to stderr if the subos
// was created with image/tmpfs storage so the user understands the
// attribute is dormant in this entry. Writes to stderr (not stdout)
// so the --shell <kind> path stays eval-safe.
// `--sandbox` without `--gpu` on a machine that has one, for a subos that does
// graphics: say so, once, before entering.
//
// bwrap's `--dev` builds a fresh /dev from a hard-coded whitelist that does not
// include /dev/nvidia*, /dev/dri or /dev/dxg, so a sandbox without `--gpu` is a
// software-rendering environment by construction. That default is CORRECT --
// device passthrough should be a decision someone takes, not something a tool
// does quietly -- and `--gpu` genuinely restores it: measured, GLX and Vulkan
// come back byte-identical to the unsandboxed subos.
//
// So the gap is not capability, it is silence. Without the flag the user gets
// "runs, draws a window, exits 0", indistinguishable from the GPU case except
// in frame rate. That is this stack's whole failure mode: succeeding at the
// wrong thing without saying so.
//
// THREE conditions, and the narrowness is the point. `warn_storage_dormant_on_
// shell_` a few lines below is a hint that fired on every entry of its kind,
// became noise, and is now commented out -- a hint that cannot be acted on is
// worse than none. This one can only fire where acting on it changes the
// outcome:
//
//   * the subos has a GL dispatch -- a subos that does no graphics is not
//     missing anything (read, not probed: same state file `subos info` reads)
//   * the host actually has GPU device nodes -- on a machine with no GPU,
//     `--gpu` would expose nothing and the advice would be false
//   * `--gpu` was not passed -- the user who asked does not need telling
void warn_sandbox_without_gpu_(const std::string& name, bool gpu,
                               EventStream& stream) {
    if (gpu) return;
    auto w = xlings::subos::graphics::read_graphics_wiring(Config::subos_dir(name));
    if (!w.has_dispatch()) return;
    if (!xlings::subos::gpu::host_has_gpu_devices()) return;
    stream.emit(DataEvent{"tip", nlohmann::json{
        {"message",
         "this subos has a GL stack but the sandbox exposes no GPU device; "
         "GL will render in software. Add --gpu to pass the host's GPU through."}
    }.dump()});
}

void warn_storage_dormant_on_shell_(const std::string& name) {
    auto& p = Config::paths();
    auto storage = sandbox::read_storage_mode_(p.homeDir / "subos" / name);
    if (storage == sandbox::StorageMode::Shared) return;
    // Hint disabled: wording was ambiguous ("use --sandbox to activate"
    // reads as either the verb or the `subos use` command) and it fired
    // on every shell-level entry for image/tmpfs subos, becoming noise
    // once the user already knows the layout. Revisit with a clearer
    // one-time / opt-in form before re-enabling.
    // std::println(std::cerr,
    //              "[xlings] storage={} is sandbox-only; entering "
    //              "shell-level (use --sandbox to activate)",
    //              sandbox::storage_to_string_(storage));
    (void)storage;
}

int use_emit_shell(const std::string& name,
                          std::string_view shell_kind,
                          EventStream& stream) {
    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;
    warn_storage_dormant_on_shell_(name);

    auto& p = Config::paths();
    auto bin_dir = p.homeDir / "subos" / name / "bin";

    bool is_fish = (shell_kind == "fish");
    bool is_pwsh = (shell_kind == "pwsh" || shell_kind == "powershell" ||
                    shell_kind == "ps1" || shell_kind == "ps");

    // The subos's own declared environment (GL driver paths, EGL vendor dirs,
    // and whatever else a package needs a *user's* binary to see). Emitted
    // after the xvm/PATH lines so a declaration cannot displace them.
    //
    // UC-1 -- a variable the user already exported wins. The emitted code
    // tests the live variable rather than what this process happens to see:
    // `--shell` output is frequently captured once and eval'd later, in a
    // shell whose environment has moved on.
    const auto envVars = use_detail_::subos_env_for_(name);
    use_detail_::report_injected_env_(name, envVars);

    if (is_fish) {
        std::println(std::cout, R"(set -gx XLINGS_ACTIVE_SUBOS "{}";)", name);
        std::println(std::cout, R"(set -gx XLINGS_BIN "{}";)", bin_dir.string());
        // Strip any old subos bin segments from PATH, then prepend the new
        // bin. fish's $PATH is a list, so we use string match -v.
        std::println(std::cout, R"(set -gx PATH "{}" (string match -v -r "^{}/subos/[^/]+/bin$" -- $PATH);)",
                     bin_dir.string(), p.homeDir.string());
        for (const auto& v : envVars) {
            if (v.unresolved) continue;
            // R"SH(...)SH": the fish source below contains `)"`, which ends a
            // plain R"(...)" literal early -- and the truncation compiles,
            // because what is left is still a valid string.
            if (v.op == manifest::OP_PREPEND) {
                std::println(std::cout, 
                    R"SH(if set -q {0}; set -gx {0} "{1}:${0}"; else; set -gx {0} "{1}"; end;)SH",
                    v.var, v.value);
            } else {
                std::println(std::cout, R"SH(if not set -q {0}; set -gx {0} "{1}"; end;)SH",
                             v.var, v.value);
            }
        }
        return 0;
    }
    if (is_pwsh) {
        std::println(std::cout, R"($env:XLINGS_ACTIVE_SUBOS = '{}')", name);
        std::println(std::cout, R"($env:XLINGS_BIN = '{}')", bin_dir.string());
        std::println(std::cout, R"($env:Path = '{}' + ';' + (($env:Path -split ';') -notmatch '^{}\\subos\\[^\\]+\\bin$' -join ';'))",
                     bin_dir.string(), p.homeDir.string());
        for (const auto& v : envVars) {
            if (v.unresolved) continue;
            // ';' rather than ':' -- these are path lists, and on Windows the
            // separator is the one the platform's own tools split on.
            if (v.op == manifest::OP_PREPEND) {
                std::println(std::cout, 
                    R"($env:{0} = if ($env:{0}) {{ '{1}' + ';' + $env:{0} }} else {{ '{1}' }})",
                    v.var, v.value);
            } else {
                // `$null -eq`, not `-not`: PowerShell's `-not` is true for an
                // empty string too, which would overwrite a value the user
                // deliberately set to "".
                std::println(std::cout, R"(if ($null -eq $env:{0}) {{ $env:{0} = '{1}' }})",
                             v.var, v.value);
            }
        }
        return 0;
    }
    // POSIX (sh/bash/zsh) default
    auto orig_path = utils::get_env_or_default("PATH");
    auto new_path  = use_detail_::rebuild_path_for_subos_(
        orig_path, p.homeDir, bin_dir);
    std::println(std::cout, R"(export XLINGS_ACTIVE_SUBOS="{}";)", name);
    std::println(std::cout, R"(export XLINGS_BIN="{}";)", bin_dir.string());
    std::println(std::cout, R"(export PATH="{}";)", new_path);
    for (const auto& v : envVars) {
        if (v.unresolved) continue;
        if (v.op == manifest::OP_PREPEND) {
            // ${VAR:+:$VAR} appends the separator only when VAR is non-empty,
            // so an unset variable does not become a trailing ':' -- which an
            // empty PATH-list element reads as "the current directory".
            std::println(std::cout, R"(export {0}="{1}${{{0}:+:${0}}}";)", v.var, v.value);
        } else {
            // `${VAR=v}`, NOT `${VAR:=v}`. The colon form also assigns when VAR
            // is set-but-empty, which would overwrite a value the user chose.
            std::println(std::cout, R"(: "${{{0}={1}}}"; export {0};)", v.var, v.value);
        }
    }
    return 0;
}

// The subos's DECLARED variables, as they stand after apply_subos_env_, for
// a sandbox's environment. The sandbox passes an allow-list, and a variable
// the instance itself declares (LIBGL_DRIVERS_PATH for its GL stack, #352)
// is the instance's, not the host's: it enters by name.
std::map<std::string, std::string> declared_env_(const std::string& name) {
    std::map<std::string, std::string> env;
    for (const auto& v : use_detail_::subos_env_for_(name)) {
        if (v.unresolved) continue;
        if (auto value = utils::get_env_or_default(v.var); !value.empty()) env[v.var] = value;
    }
    return env;
}

int use_spawn_shell(const std::string& name, EventStream& stream, bool sandbox, const std::string& sandbox_backend, bool gpu, const std::string& cmd,
                    std::optional<policy::Preset> preset, const policy::Overrides& overrides)
{
    // A declared instance is entered under its policy however it is entered
    // (design §10: the secure default belongs to the instance, not the flag).
    if (!sandbox && (preset || policy_store::has_file(home_view(), name))) sandbox = true;
    // V5: --sandbox [backend] is a `use`-time modifier. Dispatch to the
    // sandbox path when set; auto-detect backend (bwrap preferred, proot
    // fallback) or use the explicitly requested one.
    //
    // Storage mode (image/tmpfs) and sandbox are orthogonal axes per
    // V4 design: shell-level entry only swaps env/PATH and never
    // mounts. Image / tmpfs only take effect when `--sandbox` is
    // explicitly passed — earlier V6 auto-upgrade fused the two axes
    // and made `subos use <image-subos>` silently require root, bwrap,
    // and a working mount namespace just to switch shells.
    //
    // M3: `cmd` non-empty switches to non-interactive single-command
    // execution — `shell -c <cmd>` instead of an interactive shell.
    // Useful for scripts and agent workflows.
    if (sandbox) {
        // Before entering, not inside: the sandbox module cannot import this
        // one (this one imports it), and proot/bwrap pass our environment
        // through to the shell anyway. The home is bound at its own absolute
        // path, so the payload paths in these values mean the same thing on
        // both sides of the boundary.
        if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;
        use_detail_::apply_subos_env_(name);
        warn_sandbox_without_gpu_(name, gpu, stream);
        return sandbox::enter(name, stream, sandbox::EnterOptions{
            .backend = sandbox_backend, .gpu = gpu, .cmd = cmd, .env = declared_env_(name),
            .preset = preset, .overrides = overrides });
    }
    warn_storage_dormant_on_shell_(name);

    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;

    auto already = utils::get_env_or_default("XLINGS_ACTIVE_SUBOS");
    if (already == name) {
        nlohmann::json p; p["name"] = name;
        stream.emit(DataEvent{"subos_already_in", p.dump()});
        return 0;
    }
    if (!already.empty()) {
        nlohmann::json p; p["from"] = already; p["to"] = name;
        stream.emit(DataEvent{"subos_nesting", p.dump()});
    }

    auto& p = Config::paths();
    auto bin_dir = p.homeDir / "subos" / name / "bin";

    auto orig_path = utils::get_env_or_default("PATH");
    auto new_path  = use_detail_::rebuild_path_for_subos_(
        orig_path, p.homeDir, bin_dir);

    // Set env BEFORE spawning so the child shell inherits it. The profile
    // sourced by the child reads XLINGS_ACTIVE_SUBOS and re-computes
    // XLINGS_BIN; XLINGS_BIN we set here is mostly defensive (covers the
    // case where the child shell is started with --norc and never sources
    // the profile).
    platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
    platform::set_env_variable("XLINGS_BIN", bin_dir.string());
    // E2a: the subos's library farm, DECLARED rather than derivable.
    //
    // Two consumers need it and neither should have to guess. The linker
    // wrapper in the binutils payload writes it into `-rpath-link` (and, when
    // not opted out, `-rpath`) so a program built in here can find the
    // libraries it just linked against; mcpp reads it to know which directory
    // to strip back out at pack time. Both reading one declaration is the
    // difference between a contract and two derivations of the same decision
    // that drift.
    //
    // `XLINGS_BIN` + "/../lib" would work today and is exactly the coincidence
    // this replaces: it is true because of how the farm happens to be laid
    // out, not because anything promised it.
    platform::set_env_variable(
        "XLINGS_SUBOS_LIB",
        (p.homeDir / "subos" / name / "lib").string());
    platform::set_env_variable("PATH", new_path);

    // The subos's declared environment, applied to this process before it is
    // replaced -- so the shell (or the single `--cmd`) inherits it, and so
    // does every user binary run inside. This is the path that matters for
    // issue #352: nothing xlings wraps needs LIBGL_DRIVERS_PATH, the user's
    // own GL program does.
    //
    // UC-1 -- a variable already set in this environment is the user's, and
    // `set` leaves it alone. `prepend` still contributes, since composing is
    // what prepend means.
    use_detail_::apply_subos_env_(name);

    nlohmann::json payload;
    payload["name"] = name;
    payload["mode"] = "spawn";
    stream.emit(DataEvent{"subos_entering", payload.dump()});

    // Flush std streams before exec/CreateProcess — buffered output isn't
    // preserved across execve(2), and on Windows the child shell may start
    // writing before the parent's pending output drains; in either case
    // CI capture (where stdout is a pipe rather than a TTY) loses any
    // block-buffered bytes that didn't get flushed.
    std::cout.flush();
    std::cerr.flush();

    return platform::run_shell(cmd, cmd.empty());
}

int use(const std::string& name, EventStream& stream) {
    auto resolved = resolve_use_name_(name, stream);
    if (resolved.selected.empty()) return resolved.exitCode;
    return use_global(resolved.selected, stream);
}

int remove(const std::string& name, EventStream& stream) {
    return remove(name, /*yes=*/false, "-y", stream);
}

int remove(const std::string& name, bool yes, std::string_view yesSpelling,
           EventStream& stream) {
    if (reject_empty_subos_name_("remove a subos", name, stream)) return 1;

    if (name == "default") {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "cannot remove the 'default' subos",
            .recoverable = false,
        });
        return 1;
    }

    auto& p = Config::paths();
    if (p.activeSubos == name) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = "cannot remove the active subos '" + name + "'",
            .recoverable = true,
            .hint = "switch first: xlings subos use default",
        });
        return 1;
    }

    if (!read_home_config(p.homeDir).value("subos", nlohmann::json::object())
             .contains(name)) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::NotFound,
            .message = "subos '" + name + "' not found",
            .recoverable = true,
        });
        return 1;
    }

    auto dir = Config::subos_dir(name);
    if (fs::exists(dir)) {
        // A SubOS's home is where its user works -- an agent's clone, a
        // shell's history, whatever was built there -- and nothing can put it
        // back. So removing one is the user's decision, asked for and said
        // out loud: the size of what goes, and a default of no.
        const auto census = userdata::census(dir);
        const auto what = userdata::describe(census);
        auto asked = confirm::ask(
            stream, "subos_remove",
            std::format("remove subos '{}'? This deletes {} ({}) and cannot be undone",
                        name, dir.string(), what),
            yes, yesSpelling);
        if (asked.outcome == confirm::Outcome::Declined) {
            log::println("cancelled; subos '{}' was not removed", name);
            return 0;
        }
        if (asked.outcome == confirm::Outcome::NobodyToAsk) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = std::format(
                    "removing subos '{}' deletes {} ({}); nothing was removed",
                    name, dir.string(), what),
                .recoverable = true,
                .hint = userdata::needs_confirmation_hint(yesSpelling, "remove_subos"),
            });
            return 2;
        }

        // V6: image-storage subos has an ext4 mount at <subos>/.mountpoint.
        // remove_all would either (a) hit EBUSY at the mountpoint, leaving
        // a half-cleaned tree behind, or (b) silently recurse into the
        // live mount and erase the image's contents before EBUSY surfaces.
        // Detect and umount first.
        auto mountpoint = dir / ".mountpoint";
        if (platform::is_linux && fs::exists(mountpoint)
            && sandbox::is_mounted_(mountpoint))
        {
            if (sandbox::unmount_image_(mountpoint) != 0) {
                stream.emit(ErrorEvent{
                    .code = ErrorCode::Permission,
                    .message = "failed to unmount " + mountpoint.string()
                               + " (image storage subos); refusing to "
                                 "remove to avoid corrupting the live "
                                 "filesystem",
                    .recoverable = true,
                    .hint = "ensure no shell is inside this subos, then "
                            "retry — or manually: sudo umount "
                            + mountpoint.string(),
                });
                return 1;
            }
        }
        if (auto removed = userdata::delete_subos(home_view(), dir, "subos-remove", *asked.token);
            !removed) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Permission,
                .message = removed.error(),
                .recoverable = false,
            });
            return 1;
        }
    }

    // remove_all above walks the whole subos tree and can run for a long
    // time on a big one. Deleting our key out of a document read before that
    // walk would resurrect every other key's pre-walk value.
    auto committed = update_home_config(p.homeDir, [&](nlohmann::json& json) {
        if (!json.contains("subos") || !json["subos"].is_object()) return false;
        return json["subos"].erase(name) > 0;
    });
    if (!committed) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = "subos '" + name + "' was deleted from disk but its "
                       "entry could not be removed: " + committed.error(),
            .recoverable = true,
            .hint = "retry once the other xlings finishes",
        });
        return 1;
    }

    nlohmann::json payload;
    payload["name"] = name;
    stream.emit(DataEvent{"subos_removed", payload.dump()});
    return 0;
}

std::optional<SubosInfo> info(const std::string& name) {
    auto& p = Config::paths();
    auto dir = Config::subos_dir(name);
    if (!fs::exists(dir)) return std::nullopt;

    auto counts = count_subos_(dir, true);
    return SubosInfo{name, dir, p.activeSubos == name,
                     counts.commands, counts.packages};
}

int run_list_(EventStream& stream) {
    auto all = candidate_view().candidates;
    std::vector<std::tuple<std::string, std::string, int, int, bool>> entries;
    for (auto& s : all) {
        entries.emplace_back(s.name, s.dir.string(), s.commandCount,
                             s.packageCount, s.isActive);
    }
    nlohmann::json entriesJson = nlohmann::json::array();
    for (auto& [n, d, commands, packages, active] : entries) {
        entriesJson.push_back({{"name", n}, {"dir", d},
                               {"commands", commands},
                               {"packages", packages},
                               {"active", active}});
    }
    nlohmann::json payload;
    payload["entries"] = std::move(entriesJson);
    stream.emit(DataEvent{"subos_list", payload.dump()});
    return 0;
}

// The graphics section of `xlings subos info`.
//
// This is the ONLY channel the wiring verdict has. It was designed as the
// second one: the graphics recipe logs a warning for every vendor it finds
// broken, and that seemed sufficient. Measured on a real install — the config
// hook's log does not surface on the success path at all, not the new
// warnings and not the stack's own long-standing "GL renders on the GPU"
// banner. A user whose NVIDIA vendor cannot load is told nothing, anywhere,
// unless they run this command.
//
// Empty when the subos has no GL dispatch, so `subos info` is unchanged for
// every subos that does no graphics — which is most of them.
nlohmann::json graphics_fields_(const fs::path& subosDir) {
    namespace gfx = xlings::subos::graphics;
    auto w = gfx::read_graphics_wiring(subosDir);
    nlohmann::json out = nlohmann::json::array();
    if (!w.has_dispatch()) return out;

    auto row = [&](std::string label, std::string value, bool alert) {
        out.push_back({{"label", std::move(label)}, {"value", std::move(value)},
                       {"highlight", false}, {"alert", alert}});
    };

    row("GL dispatch", w.dispatchDir.string(), false);

    switch (w.status) {
    case gfx::WiringStatus::NoVendors:
        // The failure this whole mechanism exists to remove, and the one case
        // that needs no record to detect: glvnd with nothing to dispatch to
        // falls back to software rendering and reports success.
        row("vendors",
            "NONE REGISTERED — every GL program in this subos falls back to "
            "software rendering. Run 'xlings install graphics' to wire it.",
            true);
        return out;
    case gfx::WiringStatus::Unrecorded:
        row("vendors",
            std::to_string(w.vendorFiles) + " registered, but this stack was "
            "wired before xlings recorded whether they load. Re-run "
            "'xlings install graphics' to check them.",
            true);
        return out;
    default:
        break;
    }

    // The one part of this stack we do not own is the host's NVIDIA driver,
    // and it moves: a distribution update replaces it, the versioned SONAMEs
    // our payload links to change, and the wiring below describes a driver
    // that is no longer there. The detector already existed and worked
    // (`xlings-gl-doctor`); what it lacked was a way to reach the user without
    // being remembered. Reported before the per-vendor rows because when it
    // fires, every row under it is about the old driver.
    if (auto d = gfx::read_driver_stamp(w.dispatchDir / "lib" /
                                        std::string(gfx::kVendorSubdir));
        d.drifted()) {
        row("host driver",
            "CHANGED — this stack was wired for " + d.builtFor +
            " and the host is now running " + d.hostNow +
            ". Re-run 'xlings install graphics'; until then the states below "
            "describe a driver that is no longer loaded.",
            true);
    }

    if (w.dispatchMismatch) {
        // The record describes libraries nobody in this subos will load.
        // Reporting their states as this subos's would be a confident wrong
        // answer, so say that first and keep the states clearly attributed.
        row("stale wiring",
            "recorded against " + w.recordedDispatch + ", which is not the "
            "dispatch this subos loads. The states below may describe a "
            "different stack — re-run 'xlings install graphics'.",
            true);
    }

    for (const auto& v : w.vendors) {
        auto lbl = gfx::label_for(v.soname);
        auto label = lbl ? lbl->vendor + " " + lbl->api : v.soname;
        // `needs-transitive-consumer` is marked too: it is not a failure for
        // installed programs, but it IS the answer to "why does the GL app I
        // just compiled render in software", and that question is why someone
        // reads this panel.
        row(std::move(label), gfx::describe(v),
            v.stale || v.is_broken() || v.needs_transitive_consumer());
    }
    return out;
}

int run_info_(const std::string& name, EventStream& stream) {
    auto& p = Config::paths();
    auto target = name.empty() ? p.activeSubos : name;
    auto si = info(target);
    if (!si) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::NotFound,
            .message = "subos '" + target + "' not found",
            .recoverable = true,
        });
        return 1;
    }
    nlohmann::json fieldsJson = nlohmann::json::array();
    fieldsJson.push_back({{"label", "active"}, {"value", si->isActive ? "yes" : "no"}, {"highlight", si->isActive}});
    fieldsJson.push_back({{"label", "dir"}, {"value", si->dir.string()}, {"highlight", false}});
    fieldsJson.push_back({{"label", "commands"}, {"value", std::to_string(si->commandCount)}, {"highlight", false}});
    if (si->packageCount >= 0) {
        fieldsJson.push_back({{"label", "packages"}, {"value", std::to_string(si->packageCount)}, {"highlight", false}});
    }
    nlohmann::json payload;
    payload["title"] = si->name;
    payload["fields"] = std::move(fieldsJson);
    auto gfx = graphics_fields_(si->dir);
    if (!gfx.empty()) payload["extra_fields"] = std::move(gfx);
    stream.emit(DataEvent{"info_panel", payload.dump()});
    return 0;
}

// "which subos" when the command needs one and none was given.
//
// Every one of these printed `missing <name> for: xlings subos <sub>` -- true,
// and it makes the user run `xlings subos list`, read 45 names and type one
// back. The list is already known; offering it is the same move
// `resolve_use_name_` makes for `use`.
//
// Returns empty when the caller should stop; `*rc` carries the exit code.
// Non-interactive keeps the old message and the old exit code exactly, so
// scripts see no change.
// `report` is `run()`'s own usage-error lambda, passed in rather than
// reimplemented: it carries the exit path and formatting the rest of that
// function already uses.
std::string pick_subos_or_fail_(std::string_view verb, EventStream& stream,
                                const std::function<void(const std::string&)>& report,
                                int* rc) {
    auto resolution = resolve_candidate_("");
    if (!resolution.candidates.empty() && stream.interactive()) {
        PromptEvent pick;
        pick.id = "select_subos";
        pick.question = std::string(i18n::tr("ui.select_subos"));
        pick.kind = PromptEvent::Kind::Select;
        for (const auto& c : resolution.candidates) pick.options.push_back(c.name);
        pick.defaultValue = Config::paths().activeSubos;

        std::string chosen;
        std::visit(EventStream::on{
            [&](EventStream::Chosen&& c) { chosen = std::move(c.value); },
            [&](EventStream::Cancelled&&) { log::println("cancelled"); *rc = 0; },
            [&](EventStream::NobodyToAsk&&) {},   // fall through
        }, stream.prompt(std::move(pick)));
        if (!chosen.empty()) return chosen;
        if (*rc == 0) return {};
    }
    report(std::format("missing <name> for: xlings subos {}", verb));
    *rc = 1;
    return {};
}

namespace {

// "K=V" -> {K, V}; nullopt without '='.
std::optional<std::pair<std::string, std::string>> split_env_(std::string_view kv) {
    auto eq = kv.find('=');
    if (eq == std::string_view::npos || eq == 0) return std::nullopt;
    return std::pair{std::string(kv.substr(0, eq)), std::string(kv.substr(eq + 1))};
}

// The isolation flags every entry shares (design §10): `--sandbox`,
// `--sandbox=<dev|private|locked>`, `--sandbox[=| ]<bwrap|proot>` (a backend),
// and the tighten-only overrides `--net`, `--fetch`, `--allow`, `--no-degrade`.
struct IsolationArgs {
    bool sandbox { false };
    std::string backend;
    std::optional<policy::Preset> preset;
    policy::Overrides overrides;
    std::vector<std::string> publish;    // --publish HOST:SANDBOX
};

// 1 = consumed, 0 = not ours, -1 = malformed (`err` says why).
int parse_isolation_flag_(std::string_view a, int& i, int argc, char* argv[],
                          IsolationArgs& x, std::string& err) {
    auto value = [&](std::string_view flag) -> std::optional<std::string> {
        if (a.starts_with(std::string(flag) + "=")) return std::string(a.substr(flag.size() + 1));
        if (a == flag && i + 1 < argc) return std::string(argv[++i]);
        return std::nullopt;
    };
    if (a == "--sandbox") {
        x.sandbox = true;
        if (i + 1 < argc) {
            std::string_view next = argv[i + 1];
            if (next == "bwrap" || next == "proot" || next == "landlock") { x.backend = next; ++i; }
        }
        return 1;
    }
    if (a.starts_with("--sandbox=")) {
        x.sandbox = true;
        auto v = a.substr(10);
        if (v == "bwrap" || v == "proot" || v == "landlock") { x.backend = v; return 1; }
        auto p = policy::preset_from_string(v);
        if (!p || *p == policy::Preset::Legacy) {
            err = std::format("--sandbox={}: expected dev, private or locked (or a backend: bwrap, proot, landlock)", v);
            return -1;
        }
        x.preset = *p;
        return 1;
    }
    if (a == "--net" || a.starts_with("--net=")) {
        auto v = value("--net");
        auto n = v ? policy::net_from_string(*v) : std::nullopt;
        if (!n) { err = "--net expects host, nat, none or proxy"; return -1; }
        x.overrides.net = *n;
        x.sandbox = true;
        return 1;
    }
    if (a == "--fetch" || a.starts_with("--fetch=")) {
        auto v = value("--fetch");
        auto f = v ? policy::fetch_from_string(*v) : std::nullopt;
        if (!f) { err = "--fetch expects auto, ask or deny"; return -1; }
        x.overrides.fetch = *f;
        return 1;
    }
    if (a == "--allow" || a.starts_with("--allow=")) {
        auto v = value("--allow");
        if (!v) { err = "--allow expects a grant"; return -1; }
        std::string_view list(*v);
        while (!list.empty()) {
            auto comma = list.find(',');
            x.overrides.allow.insert(std::string(list.substr(0, comma)));
            if (comma == std::string_view::npos) break;
            list.remove_prefix(comma + 1);
        }
        x.sandbox = true;
        return 1;
    }
    if (a == "--mount" || a.starts_with("--mount=")) {
        auto v = value("--mount");
        if (!v) { err = "--mount expects <host>[:<inside>][:ro|rw]"; return -1; }
        std::error_code ec;
        auto m = policy::parse_mount(*v, utils::get_env_or_default("HOME"),
                                     fs::current_path(ec).generic_string());
        if (!m) { err = m.error(); return -1; }
        x.overrides.mounts.push_back(std::move(*m));
        x.sandbox = true;
        return 1;
    }
    if (a == "--publish" || a.starts_with("--publish=")) {
        auto v = value("--publish");
        if (!v || v->find(':') == std::string::npos) { err = "--publish expects HOST:SANDBOX ports, e.g. 8080:80"; return -1; }
        x.publish.push_back(*v);
        x.sandbox = true;
        return 1;
    }
    if (a == "--no-degrade") {
        x.overrides.no_degrade = true;
        x.sandbox = true;
        return 1;
    }
    return 0;
}


}  // namespace

// `subos exec <name> [options] -- <argv...>` (design §12): one command in an
// instance from outside it, as argv (nothing to shell-escape). Joins the
// running session when there is one; otherwise starts one for the command
// (with --sandbox) or runs it with the instance's environment. Exit codes are
// the command's own, 125 when it never started, 126/127, 124, 128+n.
int run_exec_(int argc, char* argv[], EventStream& stream) {
    std::string name, cwd, from;
    std::map<std::string, std::string> env;
    std::optional<std::chrono::milliseconds> timeout;
    bool json = false, temp = false;
    IsolationArgs iso;
    std::vector<std::string> command;
    auto fail = [&](std::string message, std::string hint = {}) {
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput, .message = std::move(message),
                                .recoverable = false, .hint = std::move(hint) });
        if (json) std::println(std::cerr, "{}", nlohmann::json{{"exit", session::kExitSetup},
                                                            {"phase", "setup"}}.dump());
        return session::kExitSetup;
    };
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") { command.assign(argv + i + 1, argv + argc); break; }
        std::string err;
        if (auto r = parse_isolation_flag_(a, i, argc, argv, iso, err); r == 1) continue;
        else if (r < 0) return fail(err);
        if (a == "--json") json = true;
        else if (a == "--temp") temp = true;
        else if (a == "--from" && i + 1 < argc) from = argv[++i];
        else if (a == "--cwd" && i + 1 < argc) cwd = argv[++i];
        else if (a == "--env" && i + 1 < argc) {
            auto kv = split_env_(argv[++i]);
            if (!kv) return fail("--env expects K=V");
            env[kv->first] = kv->second;
        }
        else if (a == "--timeout" && i + 1 < argc) {
            auto d = model::parse_duration(argv[++i]);
            if (!d) return fail("--timeout expects seconds or 30s / 10m / 2h");
            timeout = std::chrono::seconds(*d);
        }
        else if (!a.empty() && a[0] != '-' && name.empty() && !temp) name = std::move(a);
        else return fail("unknown option for `xlings subos exec`: " + a,
                         "usage: xlings subos exec <name> [--sandbox] [--cwd D] [--env K=V] "
                         "[--timeout T] [--json] -- <command...>");
    }
    if (command.empty()) return fail("missing the command: xlings subos exec <name> -- <command...>");
    if (!from.empty() && !temp) return fail("--from needs --temp");

    // --temp: a throwaway instance, removed afterwards; its audit stays.
    std::string tempName;
    if (temp) {
        if (!name.empty()) return fail("--temp takes no name");
        std::random_device rd;
        tempName = std::format("tmp-{:06x}", rd() & 0xffffff);
        name = tempName;
        EventStream quiet;
        int rc = from.empty()
            ? create(name, {}, sandbox::StorageMode::Shared, "50G", "", /*yes=*/true, "--temp", quiet)
            : new_from(name, {}, sandbox::StorageMode::Shared, "50G", from, "", /*yes=*/true, quiet);
        if (rc != 0) return fail("could not create a temporary instance" +
                                 (from.empty() ? std::string{} : " from " + from));
    } else if (name.empty()) {
        return fail("missing <name> for: xlings subos exec (or --temp)");
    } else {
        auto resolved = resolve_candidate_(name);
        if (resolved.selected.empty() || resolved.autoSelected) {
            emit_candidates_(stream, resolved, name);
            return fail("no SubOS named '" + name + "'", "xlings subos list");
        }
    }

    const auto started = std::chrono::steady_clock::now();
    int rc = 0;
    std::string mode;
    if (iso.sandbox || iso.preset || session::find(home_view(), name)
        || policy_store::has_file(home_view(), name)) {
        mode = "sandbox";
        use_detail_::apply_subos_env_(name);
        auto declared = declared_env_(name);
        declared.insert(env.begin(), env.end());
        for (auto& [k, v] : env) declared[k] = v;
        EventStream quiet;
        EventStream& out = json ? static_cast<EventStream&>(quiet) : stream;
        rc = sandbox::enter(name, out, sandbox::EnterOptions{
            .backend = iso.backend, .argv = command, .cwd = cwd, .env = std::move(declared),
            .timeout = timeout, .exec_codes = true, .announce = false,
            .preset = iso.preset, .overrides = iso.overrides, .publish = iso.publish });
    } else {
        // An instance without a sandbox: its environment, this process's
        // stdio, no supervisor (design §16, "没有沙箱的实例").
        mode = "shell";
        if (!cwd.empty()) {
            std::error_code ec;
            fs::current_path(cwd, ec);
            if (ec) return fail("--cwd: " + cwd + ": " + ec.message());
        }
        auto& p = Config::paths();
        auto bin_dir = p.homeDir / "subos" / name / "bin";
        platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
        platform::set_env_variable("XLINGS_BIN", bin_dir.string());
        platform::set_env_variable("XLINGS_SUBOS_LIB", (p.homeDir / "subos" / name / "lib").string());
        platform::set_env_variable("PATH", use_detail_::rebuild_path_for_subos_(
            utils::get_env_or_default("PATH"), p.homeDir, bin_dir));
        use_detail_::apply_subos_env_(name);
        for (auto& [k, v] : env) platform::set_env_variable(k, v);
        observe::append(home_view().logs_dir(name) / "events.ndjson", observe::Event{
            .kind = observe::Kind::Ops,
            .fields = {{"event", "exec"}, {"instance", name}, {"mode", "shell"},
                       {"program", command[0]}, {"argc", command.size()}}});
        if (timeout && platform::is_windows)
            log::warn("--timeout needs --sandbox on Windows; running without it");
        rc = timeout && platform::is_posix ? platform::run_argv_with_timeout(command, *timeout)
                                           : platform::run_argv(command);
        observe::append(home_view().logs_dir(name) / "events.ndjson", observe::Event{
            .kind = observe::Kind::Ops,
            .fields = {{"event", "exec-end"}, {"instance", name}, {"mode", "shell"},
                       {"program", command[0]}, {"exit", rc}}});
    }

    if (!tempName.empty()) {
        // The instance this command created, removed by the same command: the
        // user asked for a throwaway with --temp, which is the confirmation.
        // `remove` with the --temp spelling as its yes: the one deletion
        // entry point, the registry entry, and a destructive-log line that
        // says how it was confirmed.
        EventStream quiet;
        if (remove(name, /*yes=*/true, "--temp", quiet) != 0)
            log::warn("could not remove the temporary instance {}", name);
    }
    if (json) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started).count();
        nlohmann::json result{{"instance", name}, {"exit", rc}, {"mode", mode}, {"ms", ms},
                              {"temp", temp}};
        if (rc == session::kExitSetup) result["phase"] = "setup";
        if (rc == session::kExitTimeout && timeout) result["timeout"] = true;
        std::println(std::cerr, "{}", result.dump());
    }
    return rc;
}

// `subos start <name> [--sandbox[=backend]] [--ttl 30m]`: a session that runs
// without a terminal, for a series of `subos exec` (design §12.1).
int run_start_(int argc, char* argv[], EventStream& stream,
               const std::function<void(std::string_view)>& usageError) {
    std::string name;
    IsolationArgs iso;     // a session is a sandbox, whatever was said
    int ttl = 0;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        std::string err;
        if (auto r = parse_isolation_flag_(a, i, argc, argv, iso, err); r == 1) continue;
        else if (r < 0) { usageError(err); return 1; }
        if (a == "--ttl" && i + 1 < argc) {
            auto d = model::parse_duration(argv[++i]);
            if (!d) { usageError("--ttl expects seconds or 30s / 10m / 2h"); return 1; }
            ttl = static_cast<int>(*d);
        }
        else if (!a.empty() && a[0] != '-' && name.empty()) name = std::move(a);
        else { usageError("unknown option for `xlings subos start`: " + a); return 1; }
    }
    if (name.empty()) { usageError("missing <name> for: xlings subos start"); return 1; }
    auto resolved = resolve_use_name_(name, stream);
    if (resolved.selected.empty()) return resolved.exitCode;
    name = resolved.selected;
    if (auto live = session::find(home_view(), name)) {
        log::info("'{}' is already running (session {})", name, live->id);
        return 0;
    }
    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;
    use_detail_::apply_subos_env_(name);
    auto rc = sandbox::enter(name, stream, sandbox::EnterOptions{
        .backend = iso.backend, .env = declared_env_(name), .ttl = ttl, .detached = true,
        .preset = iso.preset, .overrides = iso.overrides, .publish = iso.publish });
    if (rc != 0) return rc;
    if (auto live = session::find(home_view(), name)) {
        log::info("started session {} for '{}'{}", live->id, name,
                  ttl > 0 ? std::format(" (ends after {}s idle)", ttl) : std::string(" (until `subos stop`)"));
        return 0;
    }
    return 1;
}

// `subos cp <src> <dst>`, one side `<name>:<path>` (design §12.1). Paths
// inside are the instance's own: its home and its /tmp.
// A keeper process an xlings before 2026.10 left behind (sessions replaced
// it): `subos stop` still ends it. COMPAT: drop in 2027.4.
void stop_legacy_keeper_(const std::string& name) {
    const auto pid_file = Config::paths().homeDir / "subos" / name / ".keeper.pid";
    std::error_code ec;
    if (!fs::exists(pid_file, ec)) return;
    int pid = 0;
    std::ifstream(pid_file) >> pid;
    if (pid > 0 && platform::is_process_alive(pid)) {
        platform::send_signal(pid, platform::sig::terminate);
        for (int i = 0; i < 20 && platform::is_process_alive(pid); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (platform::is_process_alive(pid)) platform::send_signal(pid, platform::sig::kill);
    }
    fs::remove(pid_file, ec);
    fs::remove(pid_file.parent_path() / ".keeper.lastused", ec);
}

int run_cp_(int argc, char* argv[], EventStream& stream,
            const std::function<void(std::string_view)>& usageError) {
    std::vector<std::string> paths;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (!a.empty() && a[0] == '-') { usageError("unknown option for `xlings subos cp`: " + a); return 1; }
        paths.push_back(std::move(a));
    }
    if (paths.size() != 2) { usageError("usage: xlings subos cp <src> <name>:<dst>  |  <name>:<src> <dst>"); return 1; }
    auto split = [](const std::string& p) -> std::optional<std::pair<std::string, std::string>> {
        auto colon = p.find(':');
        // A Windows drive letter (C:\x) is a host path, not an instance.
        if (colon == std::string::npos || colon < 2) return std::nullopt;
        return std::pair{p.substr(0, colon), p.substr(colon + 1)};
    };
    auto src = split(paths[0]);
    auto dst = split(paths[1]);
    if (static_cast<bool>(src) == static_cast<bool>(dst)) {
        usageError("exactly one side of `subos cp` names an instance as <name>:<path>");
        return 1;
    }
    const auto& inst = src ? *src : *dst;
    auto resolved = resolve_use_name_(inst.first, stream);
    if (resolved.selected.empty()) return resolved.exitCode;
    auto user = utils::get_env_or_default(platform::is_windows ? "USERNAME" : "USER");
    if (user.empty()) user = "user";
    auto mapped = model::inside_to_host(Config::subos_dir(resolved.selected), user, inst.second);
    if (!mapped) {
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput,
            .message = inst.second + " is not the instance's own (only /home/" + user + " and /tmp are)",
            .recoverable = false });
        return 1;
    }
    // Paths inside are resolved beneath the instance's root and never
    // through a link it made (platform::copy_into_beneath).
    const auto root = Config::subos_dir(resolved.selected);
    const auto rel = mapped->lexically_relative(root);
    std::expected<void, std::string> done;
    if (src) {
        fs::path to = paths[1];
        std::error_code ec;
        if (fs::is_directory(to, ec)) to /= rel.filename();
        done = platform::copy_out_of_beneath(root, rel, to);
    } else {
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(paths[0], ec))) {
            stream.emit(ErrorEvent{ .code = ErrorCode::NotFound, .message = paths[0] + ": not found",
                                    .recoverable = false });
            return 1;
        }
        done = platform::copy_into_beneath(paths[0], root, rel);
    }
    if (!done) {
        stream.emit(ErrorEvent{ .code = ErrorCode::Internal,
            .message = "copy failed: " + done.error(), .recoverable = false });
        return 1;
    }
    observe::append(home_view().logs_dir(resolved.selected) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Fs,
        .fields = {{"event", "cp"}, {"instance", resolved.selected},
                   {"direction", src ? "out" : "in"}, {"path", inst.second}}});
    return 0;
}

// `subos config <name> [changes]`: the owner's declaration of what the
// instance may do (design §7). Written outside the instance; every change is
// audited with its diff. No change prints the policy in force.
// A policy package (design §7.3): `ns:name[@version]`, an xpkg whose payload
// carries policy.json. Installed when absent (always, for an upgrade), then
// read from the newest matching version and locked by sha256. On a system
// install, /etc/xlings/config.json's `subos_policy_sources` (globs) limits
// which packages an owner may select.
std::expected<policy::Policy, std::pair<int, std::string>>
select_policy_package_(const std::string& ref, bool upgrade) {
    const auto sys = home::read_system_config();
    if (auto it = sys.find("subos_policy_sources"); it != sys.end() && it->is_array()) {
        bool allowed = false;
        for (auto& g : *it)
            if (g.is_string() && policy::glob_match(g.get<std::string>(), ref)) allowed = true;
        if (!allowed)
            return std::unexpected(std::pair{13, std::format(
                "E_PERMISSION: {} is not among the policy packages {} allows ({})",
                ref, home::system_config_path().string(), it->dump())});
    }
    const auto colon = ref.find(':');
    const auto at = ref.find('@', colon);
    const auto ns = ref.substr(0, colon);
    const auto name = ref.substr(colon + 1, at == std::string::npos ? std::string::npos : at - colon - 1);
    const auto want = at == std::string::npos ? std::string{} : ref.substr(at + 1);
    // Each part becomes a path component under data/xpkgs: names only.
    auto plain = [](std::string_view v) {
        return v.find('/') == std::string_view::npos && v.find('\\') == std::string_view::npos
            && v != "." && v != ".." && v.find("..") == std::string_view::npos;
    };
    if (ns.empty() || name.empty() || !plain(ns) || !plain(name) || !plain(want))
        return std::unexpected(std::pair{2, std::format("'{}': a policy package is ns:name[@version]", ref)});
    const auto root = Config::paths().homeDir / "data" / "xpkgs" / (ns + "-x-" + name);

    auto newest = [&]() -> std::optional<fs::path> {
        std::optional<fs::path> best;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) return best;
        for (auto& e : platform::dir_entries(root)) {
            const auto v = e.path().filename().string();
            if (!want.empty() && v != want && !v.starts_with(want + ".")) continue;
            if (!fs::is_regular_file(e.path() / "policy.json", ec)) continue;
            if (!best || version_order::compare(v, best->filename().string()) > 0) best = e.path();
        }
        return best;
    };
    auto dir = newest();
    if (!dir || upgrade) {
        log::info("installing the policy package {}...", ref);
        const auto rc = platform::run_argv({platform::get_executable_path().string(),
                                            "install", ref, "-y"});
        if (rc != 0)
            return std::unexpected(std::pair{rc, std::format("could not install {} (exit {})", ref, rc)});
        dir = newest();
    }
    if (!dir)
        return std::unexpected(std::pair{2, std::format(
            "{} has no policy.json in its payload: not a subos-policy package", ref)});
    const auto file = *dir / "policy.json";
    const auto sha = sha256::hex_file(file);
    std::ifstream in(file, std::ios::binary);
    auto doc = nlohmann::json::parse(in, nullptr, false);
    if (!sha || doc.is_discarded())
        return std::unexpected(std::pair{2, std::format("{}: not readable JSON", file.string())});
    const auto from = std::format("{}:{}@{}", ns, name, dir->filename().string());
    auto p = policy::from_package(doc, ref, {from, *sha});
    if (!p) return std::unexpected(std::pair{2, std::format("{}: {}", from, p.error())});
    return std::move(*p);
}

int run_config_(int argc, char* argv[], EventStream& stream,
                const std::function<void(std::string_view)>& usageError) {
    std::string name;
    bool json = false, reset = false;
    std::optional<policy::Preset> preset;
    std::optional<std::string> package;          // --sandbox ns:name[@version]
    bool upgrade = false;                        // --policy-upgrade
    std::optional<policy::Net> net;
    std::optional<policy::Fetch> fetch, index_update;
    std::optional<policy::Observe> observe;
    std::optional<bool> no_degrade;
    std::set<std::string> allow, disallow, env_pass;
    std::optional<std::set<std::string>> grants_allowed;
    std::vector<policy::Mount> mounts;
    std::set<std::string> unmounts;
    bool changed = false;
    auto value_of = [&](int& i, std::string_view a, std::string_view flag) -> std::optional<std::string> {
        if (a.starts_with(std::string(flag) + "=")) return std::string(a.substr(flag.size() + 1));
        if (a == flag && i + 1 < argc) return std::string(argv[++i]);
        return std::nullopt;
    };
    auto split = [](std::string_view list) {
        std::set<std::string> out;
        while (!list.empty()) {
            auto c = list.find(',');
            if (auto item = list.substr(0, c); !item.empty()) out.insert(std::string(item));
            if (c == std::string_view::npos) break;
            list.remove_prefix(c + 1);
        }
        return out;
    };
    for (int i = 3; i < argc; ++i) {
        std::string_view a = argv[i];
        std::optional<std::string> v;
        if (a == "--json") json = true;
        else if (a == "--reset") { reset = true; changed = true; }
        else if ((v = value_of(i, a, "--sandbox"))) {
            if (policy::is_package_ref(*v)) package = *v;
            else {
                preset = policy::preset_from_string(*v);
                if (!preset || *preset == policy::Preset::Legacy) {
                    usageError("--sandbox expects dev, private, locked or a policy package (ns:name[@version])");
                    return 1;
                }
            }
            changed = true;
        }
        else if (a == "--policy-upgrade") { upgrade = true; changed = true; }
        else if ((v = value_of(i, a, "--net"))) {
            net = policy::net_from_string(*v);
            if (!net) { usageError("--net expects host, nat, none or proxy"); return 1; }
            changed = true;
        }
        else if ((v = value_of(i, a, "--fetch"))) {
            fetch = policy::fetch_from_string(*v);
            if (!fetch) { usageError("--fetch expects auto, ask or deny"); return 1; }
            changed = true;
        }
        else if ((v = value_of(i, a, "--index-update"))) {
            index_update = policy::fetch_from_string(*v);
            if (!index_update) { usageError("--index-update expects auto, ask or deny"); return 1; }
            changed = true;
        }
        else if ((v = value_of(i, a, "--observe"))) {
            observe = policy::observe_from_string(*v);
            if (!observe) { usageError("--observe expects off, basic, standard or full"); return 1; }
            changed = true;
        }
        else if ((v = value_of(i, a, "--allow"))) { auto g = split(*v); allow.insert(g.begin(), g.end()); changed = true; }
        else if ((v = value_of(i, a, "--disallow"))) { auto g = split(*v); disallow.insert(g.begin(), g.end()); changed = true; }
        else if ((v = value_of(i, a, "--grants-allowed"))) { grants_allowed = split(*v); changed = true; }
        else if ((v = value_of(i, a, "--env-pass"))) { auto e = split(*v); env_pass.insert(e.begin(), e.end()); changed = true; }
        else if ((v = value_of(i, a, "--mount"))) {
            std::error_code ec;
            auto m = policy::parse_mount(*v, utils::get_env_or_default("HOME"),
                                         fs::current_path(ec).generic_string());
            if (!m) { usageError(m.error()); return 1; }
            mounts.push_back(std::move(*m));
            changed = true;
        }
        else if ((v = value_of(i, a, "--unmount"))) { unmounts.insert(*v); changed = true; }
        else if (a == "--no-degrade") { no_degrade = true; changed = true; }
        else if (a == "--degrade") { no_degrade = false; changed = true; }
        else if (!a.empty() && a[0] != '-' && name.empty()) name = std::string(a);
        else { usageError("unknown option for `xlings subos config`: " + std::string(a)); return 1; }
    }
    if (name.empty()) { usageError("missing <name> for: xlings subos config"); return 1; }
    auto resolved = resolve_use_name_(name, stream);
    if (resolved.selected.empty()) return resolved.exitCode;
    name = resolved.selected;
    const auto home = home_view();

    auto current = policy_store::read(home, name);
    if (!current) {
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput, .message = current.error(),
                                .recoverable = true, .hint = "fix the file, or: xlings subos config " + name + " --reset" });
        if (!reset) return 1;
    }
    const policy::Policy before = current && *current ? **current : policy::legacy();

    if (!changed) {
        auto doc = policy::to_json(before);
        doc["source"] = current && *current ? "file" : "default";
        std::println(std::cout, "{}", json ? doc.dump() : doc.dump(2));
        return 0;
    }

    // Only the owner, outside the sandbox (design §8.1): the instance does not
    // get to rewrite what it is allowed.
    const bool inside = utils::get_env_or_default("XLINGS_SUBOS_MODE") == "sandbox";
    auto decision = policy::decide(before, {.kind = "policy_change", .from_inside = inside, .instance = name});
    if (decision.action != policy::Action::Allow) {
        stream.emit(ErrorEvent{ .code = ErrorCode::Permission,
            .message = "E_PERMISSION: " + decision.reason, .recoverable = false,
            .hint = "outside the sandbox: " + decision.owner_command });
        return 13;
    }

    if (reset) {
        std::error_code ec;
        fs::remove(home.policy_file(name), ec);
        observe::append(home.logs_dir(name) / "events.ndjson", observe::Event{
            .kind = observe::Kind::Lifecycle,
            .fields = {{"event", "policy-reset"}, {"instance", name}}});
        log::info("'{}' has no policy file now (an undeclared instance)", name);
        return 0;
    }

    if (upgrade && !package) {
        if (!before.package) {
            usageError(std::format("'{}' does not use a policy package", name));
            return 1;
        }
        package = before.extends;
    }
    std::optional<policy::Policy> selected;
    if (package) {
        auto p = select_policy_package_(*package, upgrade);
        if (!p) {
            stream.emit(ErrorEvent{ .code = p.error().first == 13 ? ErrorCode::Permission : ErrorCode::InvalidInput,
                                    .message = p.error().second, .recoverable = false });
            return p.error().first;
        }
        selected = std::move(*p);
        // What the owner is choosing, against the preset it builds on: a
        // package can loosen as well as tighten, and that is shown, not hidden.
        if (!json) {
            const auto base = policy::preset(selected->preset);
            log::info("{} ({}), compared with the built-in {}:", *package,
                      selected->package->from, policy::to_string(selected->preset));
            for (auto& c : policy::diff(base, *selected))
                if (!c.starts_with("/extends") && !c.starts_with("/resolved")) log::info("  {}", c);
        }
    }

    policy::Policy after = selected ? *selected : preset ? policy::preset(*preset) : before;
    if (!selected && !preset && before.preset == policy::Preset::Legacy) {
        // The first declaration of an undeclared instance starts from dev.
        after = policy::preset(policy::Preset::Dev);
    }
    if (net) after.net = *net;
    if (fetch) after.fetch = *fetch;
    if (index_update) after.index_update = *index_update;
    if (observe) after.observe = *observe;
    if (no_degrade) after.no_degrade = *no_degrade;
    if (grants_allowed) {
        after.grants_allowed.clear();
        for (auto& g : *grants_allowed) after.grants_allowed.insert(g);
    }
    for (auto& g : allow) {
        if (std::ranges::find(policy::kGrants, std::string_view(g)) == policy::kGrants.end()) {
            usageError("--allow " + g + ": not a grant"); return 1;
        }
        after.grants.insert(g);
    }
    for (auto& g : disallow) { after.grants.erase(g); after.grants_allowed.erase(g); }
    for (auto& e : env_pass)
        if (std::ranges::find(after.env_pass, e) == after.env_pass.end()) after.env_pass.push_back(e);
    std::erase_if(after.mounts, [&](const policy::Mount& m) {
        return unmounts.contains(m.src) || unmounts.contains(m.dst);
    });
    for (auto m : mounts) {
        if (!m.mode_given && after.mounts_ro_default) m.rw = false;
        std::erase_if(after.mounts, [&](const policy::Mount& x) { return x.src == m.src; });
        after.mounts.push_back(std::move(m));
    }

    if (auto why = policy::not_enforced(after)) { usageError(*why); return 1; }
    if (auto w = policy_store::write(home, name, after); !w) {
        stream.emit(ErrorEvent{ .code = ErrorCode::Internal, .message = w.error(), .recoverable = true });
        return 1;
    }
    auto changes = policy::diff(before, after);
    observe::append(home.logs_dir(name) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Lifecycle,
        .fields = {{"event", "policy-change"}, {"instance", name}, {"diff", changes}}});
    if (json) {
        std::println(std::cout, "{}", nlohmann::json{{"instance", name}, {"diff", changes},
                                          {"policy", policy::to_json(after)}}.dump());
    } else {
        log::info("'{}' policy ({}):", name, policy::to_string(after.preset));
        for (auto& c : changes) log::info("  {}", c);
        if (session::find(home, name))
            log::info("the running session keeps its isolation until `xlings subos stop {}`", name);
    }
    return 0;
}

// `subos status <name>`: what the instance asks for, what this host gives it,
// and why not when it does not (design §14).
// `subos doctor [<name>] [--json] [--fix]` (design §14, C25): is each
// instance able to do what it declares, on this host? Findings, not repairs:
// `--fix` only re-installs a selected policy package's missing payload
// (derived data). A record of a session whose supervisor died is cleaned by
// reading it -- that is what every reader of it does -- and is reported.
int run_doctor_(int argc, char* argv[], EventStream& stream,
                const std::function<void(std::string_view)>& usageError) {
    std::string only;
    bool json = false, fix = false;
    for (int i = 3; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--fix") fix = true;
        else if (!a.empty() && a[0] != '-' && only.empty()) only = std::string(a);
        else { usageError("unknown option for `xlings subos doctor`: " + std::string(a)); return 1; }
    }
    std::vector<std::string> names;
    if (!only.empty()) {
        auto resolved = resolve_use_name_(only, stream);
        if (resolved.selected.empty()) return resolved.exitCode;
        names.push_back(resolved.selected);
    } else {
        for (auto& n : Config::list_subos_names()) if (n != "current") names.push_back(n);
    }
    const auto home = home_view();
    nlohmann::json report{{"instances", nlohmann::json::array()}};
    int errors = 0;
    for (const auto& name : names) {
        nlohmann::json findings = nlohmann::json::array();
        auto add = [&](std::string check, std::string level, std::string detail, std::string fix_hint = {}) {
            if (level == "error") ++errors;
            findings.push_back({{"check", std::move(check)}, {"level", std::move(level)},
                                {"detail", std::move(detail)}, {"fix", std::move(fix_hint)}});
        };
        // The manifest: what the instance holds. Unreadable is not empty.
        {
            const auto manifest = Config::paths().homeDir / "subos" / name / ".xlings.json";
            std::error_code ec;
            if (fs::exists(manifest, ec)) {
                std::ifstream in(manifest, std::ios::binary);
                if (nlohmann::json::parse(in, nullptr, false).is_discarded())
                    add("manifest", "error", manifest.string() + " does not parse",
                        "restore it; nothing rewrites a file it cannot read");
            }
        }
        // The policy: declared, readable, enforceable by this version.
        policy::Policy pol = policy::legacy();
        auto file = policy_store::read(home, name);
        if (!file) {
            add("policy", "error", file.error(), "xlings subos config " + name + " --reset");
        } else if (*file) {
            pol = **file;
            add("policy", "ok", std::format("{} ({})", home.policy_file(name).string(), policy::to_string(pol.preset)));
        } else {
            add("policy", "ok", "none declared: the instance enters as it always has");
        }
        // Can it enter here, as declared? Not at all while the policy
        // cannot be read: entry refuses rather than guess (fail closed).
        EventStream quiet;
        const auto eff = sandbox::preview(name, pol, quiet);
        if (!file) {
            add("enters", "error", "refused until the policy reads", "");
        } else if (eff.value("enters", false)) {
            std::string degraded;
            for (auto& d : eff["spec"]["degraded"])
                degraded += (degraded.empty() ? "" : "; ") + d.value("dimension", "") + ": " + d.value("reason", "");
            // The probe is the entry itself (design §18): the same spec, the
            // same supervisor, with `true` for the command.
            std::string cmd = platform::shell_quote(platform::get_executable_path().string());
            for (const auto* a : {"subos", "exec", name.c_str(), "--sandbox", "--"})
                cmd += " " + platform::shell_quote(a);
            cmd += platform::is_windows ? " cmd /c exit 0" : " true";
            auto [status, output] = platform::run_command_capture(cmd + " 2>&1");
            if (status != 0) {
                while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
                add("enters", "error", std::format("backend {} -- entering failed: {}",
                    eff["spec"].value("backend", "?"), output.empty() ? std::format("status {}", status) : output),
                    "xlings self doctor --isolation");
            } else {
                add("enters", degraded.empty() ? "ok" : "warn",
                    std::format("backend {}, entered{}", eff["spec"].value("backend", "?"),
                                degraded.empty() ? std::string{} : " -- not in effect: " + degraded));
            }
        } else {
            std::string why, fixes;
            for (auto& m : eff["missing"]) {
                why += (why.empty() ? "" : "; ") + m.value("dimension", "") + ": " + m.value("reason", "");
                if (auto f = m.value("fix", ""); !f.empty() && fixes.find(f) == std::string::npos)
                    fixes += (fixes.empty() ? "" : "; ") + f;
            }
            add("enters", "warn", "cannot enter on this host: " + why, fixes);
        }
        // A selected policy package: still the payload it was locked to?
        if (pol.package) {
            const auto& from = pol.package->from;
            const auto colon = from.find(':'), at = from.rfind('@');
            const auto dir = Config::paths().homeDir / "data" / "xpkgs"
                / (from.substr(0, colon) + "-x-" + from.substr(colon + 1, at - colon - 1)) / from.substr(at + 1);
            auto sha = sha256::hex_file(dir / "policy.json");
            if (!sha && fix) {
                (void)platform::run_argv({platform::get_executable_path().string(), "install", from, "-y"});
                sha = sha256::hex_file(dir / "policy.json");
            }
            if (!sha)
                add("package", "warn", from + " is not installed (the instance keeps its copy of the policy)",
                    "xlings subos doctor " + name + " --fix");
            else if (*sha != pol.package->sha256)
                add("package", "warn", from + " changed since it was selected (sha256 " + sha->substr(0, 12) + "...)",
                    "xlings subos config " + name + " --policy-upgrade");
            else
                add("package", "ok", from + " (sha256 matches)");
        }
        // Sessions: a record whose supervisor is gone is removed by reading it.
        {
            std::error_code ec;
            const bool recorded = fs::exists(home.run_dir(name) / "session.json", ec);
            auto live = session::find(home, name);
            if (live) add("session", "ok", "running (" + live->id + ")");
            else if (recorded) add("session", "ok", "a record of a session that had ended was removed");
        }
        report["instances"].push_back({{"instance", name}, {"findings", findings}});
    }
    {
        EventStream quiet;
        report["gates"] = sandbox::preview(names.empty() ? std::string("default") : names.front(),
                                           policy::legacy(), quiet)["gates"];
    }
    report["errors"] = errors;
    if (json) {
        std::println(std::cout, "{}", report.dump());
        return errors ? 1 : 0;
    }
    std::println(std::cout, "this host:");
    for (auto& g : report["gates"])
        std::println(std::cout, "  {:<14} {:<9} {}", g.value("gate", ""),
                     g.value("supported", false) ? g.value("enforced", "") : "no", g.value("reason", ""));
    for (auto& inst : report["instances"]) {
        std::println(std::cout, "subos {}", inst.value("instance", ""));
        for (auto& f : inst["findings"]) {
            const auto level = f.value("level", "");
            const char* mark = level == "ok" ? "✓" : level == "warn" ? "!" : "✗";
            std::println(std::cout, "  {} {:<9} {}{}", mark, f.value("check", ""), f.value("detail", ""),
                         f.value("fix", "").empty() ? "" : "\n              -> " + f.value("fix", ""));
        }
    }
    return errors ? 1 : 0;
}

int run_status_(int argc, char* argv[], EventStream& stream,
                const std::function<void(std::string_view)>& usageError) {
    std::string name;
    bool json = false;
    for (int i = 3; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (!a.empty() && a[0] != '-' && name.empty()) name = std::string(a);
        else { usageError("unknown option for `xlings subos status`: " + std::string(a)); return 1; }
    }
    if (name.empty()) {
        int rc = 0;
        name = pick_subos_or_fail_("status", stream, usageError, &rc);
        if (name.empty()) return rc;
    }
    auto resolved = resolve_use_name_(name, stream);
    if (resolved.selected.empty()) return resolved.exitCode;
    name = resolved.selected;
    const auto home = home_view();
    auto file = policy_store::read(home, name);
    nlohmann::json out{{"instance", name}};
    policy::Policy pol = policy::legacy();
    if (!file) {
        out["policy_error"] = file.error();
    } else if (*file) {
        pol = **file;
        out["policy_source"] = "file";
    } else {
        out["policy_source"] = "default";
    }
    out["requested"] = policy::to_json(pol);
    EventStream quiet;
    out["effective"] = sandbox::preview(name, pol, quiet);
    if (auto live = session::find(home, name)) out["session"] = session::to_json(*live);
    if (json) {
        std::println(std::cout, "{}", out.dump());
        return 0;
    }
    const auto& eff = out["effective"];
    std::println(std::cout, "subos {}  ({})", name, out.value("policy_source", "invalid policy"));
    if (out.contains("policy_error")) std::println(std::cout, "  policy: {}", out["policy_error"].get<std::string>());
    std::println(std::cout, "  requested  preset={} net={} fetch={} observe={} identity={}",
                 policy::to_string(pol.preset), policy::to_string(pol.net),
                 policy::to_string(pol.fetch), policy::to_string(pol.observe),
                 pol.identity == policy::Identity::Neutral ? "neutral" : "host");
    if (eff.value("enters", false)) {
        const auto& sp = eff["spec"];
        std::println(std::cout, "  effective  backend={} pid={} net={} hostname={}", sp.value("backend", "?"),
                     sp["unshare"].value("pid", false) ? "private" : "host",
                     sp["unshare"].value("net", false) ? "private" : "host",
                     sp.value("hostname", "host"));
        for (auto& d : sp["degraded"])
            std::println(std::cout, "  ! {} not in effect: {}", d.value("dimension", ""), d.value("reason", ""));
    } else {
        std::println(std::cout, "  cannot enter on this host:");
        for (auto& m : eff["missing"])
            std::println(std::cout, "  \u2717 {}: {}{}", m.value("dimension", ""), m.value("reason", ""),
                         m.value("fix", "").empty() ? "" : "  (" + m.value("fix", "") + ")");
    }
    std::println(std::cout, "  platform:");
    for (auto& g : eff["gates"]) {
        std::println(std::cout, "    {:<14} {:<9} {}{}", g.value("gate", ""),
                     g.value("supported", false) ? g.value("enforced", "") : "no",
                     g.value("reason", ""),
                     g.value("route", "").empty() || g.value("supported", false) ? "" : "  -> " + g.value("route", ""));
    }
    if (out.contains("session"))
        std::println(std::cout, "  session    {} ({})", out["session"].value("id", ""),
                     out["session"].value("detached", false) ? "detached" : "attached");
    return 0;
}

// `subos requests / approve / deny <name> [id]`: what a sandbox asked for
// under fetch=ask (design §8.2). Only from outside -- the sandbox cannot see
// the queue, and approval never travels through a channel it can forge.
int run_requests_(std::string_view sub, int argc, char* argv[], EventStream& stream,
                  const std::function<void(std::string_view)>& usageError) {
    std::vector<std::string> pos;
    bool json = false;
    for (int i = 3; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (!a.empty() && a[0] != '-') pos.emplace_back(a);
        else { usageError(std::format("unknown option for `xlings subos {}`: {}", sub, a)); return 1; }
    }
    const std::size_t want = sub == "requests" ? 1 : 2;
    if (pos.size() != want) {
        usageError(sub == "requests" ? "usage: xlings subos requests <name>"
                                     : std::format("usage: xlings subos {} <name> <id>", sub));
        return 1;
    }
    if (utils::get_env_or_default("XLINGS_SUBOS_MODE") == "sandbox") {
        stream.emit(ErrorEvent{ .code = ErrorCode::Permission,
            .message = "E_PERMISSION: requests are answered by the owner, outside the sandbox",
            .recoverable = false });
        return 13;
    }
    const auto home = home_view();
    const auto& name = pos[0];
    if (sub == "requests") {
        auto reqs = subos::broker::pending(home, name);
        if (json) {
            for (auto& r : reqs)
                std::println(std::cout, "{}", nlohmann::json{{"id", r.id}, {"argv", r.argv}, {"created", r.created},
                                                  {"reason", r.reason}}.dump());
            return 0;
        }
        if (reqs.empty()) { log::info("'{}' has no pending requests", name); return 0; }
        for (auto& r : reqs) {
            std::string line;
            for (auto& a : r.argv) line += " " + a;
            std::println(std::cout, "{}  {}  xlings{}   ({})", r.id, r.created, line, r.reason);
        }
        return 0;
    }
    auto r = subos::broker::take(home, name, pos[1]);
    if (!r) {
        stream.emit(ErrorEvent{ .code = ErrorCode::NotFound,
            .message = std::format("no pending request {} for '{}'", pos[1], name),
            .recoverable = false, .hint = "xlings subos requests " + name });
        return 1;
    }
    const auto logfile = home.logs_dir(name) / "events.ndjson";
    if (sub == "deny") {
        observe::append(logfile, observe::Event{ .kind = observe::Kind::Perm,
            .fields = {{"event", "request-denied"}, {"instance", name}, {"request", r->id},
                       {"program", r->argv.empty() ? "" : r->argv[0]}}});
        log::info("denied {}", r->id);
        return 0;
    }
    // Approved: run it for that instance, as the broker would have.
    observe::append(logfile, observe::Event{ .kind = observe::Kind::Perm,
        .fields = {{"event", "request-approved"}, {"instance", name}, {"request", r->id},
                   {"program", r->argv.empty() ? "" : r->argv[0]}}});
    std::vector<std::string> full{ platform::get_executable_path().string() };
    full.insert(full.end(), r->argv.begin(), r->argv.end());
    if (std::ranges::find(full, std::string("-y")) == full.end()) full.push_back("-y");
    platform::set_env_variable("XLINGS_ACTIVE_SUBOS", name);
    const int rc = platform::run_argv(full);
    observe::append(logfile, observe::Event{ .kind = observe::Kind::Perm,
        .fields = {{"event", "request-done"}, {"instance", name}, {"request", r->id}, {"exit", rc}}});
    return rc;
}

// `subos report <name> [--session ID] [--json]`: the audit, summarised
// (design §22) -- what an agent did in an instance, in one screen.
int run_report_(int argc, char* argv[], EventStream& stream,
                const std::function<void(std::string_view)>& usageError) {
    std::string name, only;
    bool json = false;
    for (int i = 3; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--session" && i + 1 < argc) only = argv[++i];
        else if (!a.empty() && a[0] != '-' && name.empty()) name = std::string(a);
        else { usageError("unknown option for `xlings subos report`: " + std::string(a)); return 1; }
    }
    if (name.empty()) {
        int rc = 0;
        name = pick_subos_or_fail_("report", stream, usageError, &rc);
        if (name.empty()) return rc;
    }
    struct Session {
        std::string id, started, backend;
        long long ms = -1;
        std::optional<int> exit;
        bool trace = false;
        std::map<std::string, int> programs;
        int execs = 0, commands = 0;
        std::map<std::string, int> perm;
        std::vector<std::string> denied, changed;
    };
    std::map<std::string, Session> sessions;
    std::vector<std::string> order, policy_changes;
    for (auto& e : observe::read(home_view().logs_dir(name) / "events.ndjson")) {
        const auto ev = e.value("event", "");
        if (ev == "policy-change" || ev == "policy-reset") {
            policy_changes.push_back(e.value("ts", "") + " " + ev);
            continue;
        }
        const auto id = e.value("session", "");
        if (id.empty() || (!only.empty() && id != only)) continue;
        if (!sessions.contains(id)) order.push_back(id);
        auto& s = sessions[id];
        s.id = id;
        const auto kind = e.value("kind", "");
        if (ev == "session-start") {
            s.started = e.value("ts", "");
            s.backend = e.value("backend", "");
            s.trace = e.value("exec_trace", false);
        } else if (ev == "session-end") {
            s.ms = e.value("ms", -1LL);
            s.exit = e.value("exit", 0);
        } else if (kind == "exec") {
            ++s.execs;
            ++s.programs[e.value("path", "?")];
        } else if (kind == "ops" && ev == "exec") {
            ++s.commands;
        } else if (kind == "perm" && ev == "decision") {
            auto action = e.value("action", "");
            ++s.perm[action];
            if (action == "deny") s.denied.push_back(e.value("program", "") + ": " + e.value("reason", ""));
        } else if (kind == "fs") {
            for (auto& f : e.value("files", nlohmann::json::array()))
                s.changed.push_back(e.value("mount", "") + "/" + f.get<std::string>());
        }
    }
    if (json) {
        nlohmann::json out{{"instance", name}, {"sessions", nlohmann::json::array()},
                           {"policy_changes", policy_changes}};
        for (auto& id : order) {
            auto& s = sessions[id];
            nlohmann::json top = nlohmann::json::object();
            for (auto& [p, n] : s.programs) top[p] = n;
            out["sessions"].push_back({{"id", s.id}, {"started", s.started}, {"backend", s.backend},
                                       {"ms", s.ms}, {"exit", s.exit ? nlohmann::json(*s.exit) : nlohmann::json(nullptr)},
                                       {"exec_traced", s.trace}, {"executions", s.execs}, {"programs", top},
                                       {"commands", s.commands}, {"permissions", s.perm},
                                       {"denied", s.denied}, {"changed", s.changed}});
        }
        std::println(std::cout, "{}", out.dump());
        return 0;
    }
    std::println(std::cout, "subos {}: {} session(s)", name, order.size());
    for (auto& id : order) {
        auto& s = sessions[id];
        std::println(std::cout, "\n  session {}  {}  {}  {}", s.id, s.started, s.backend,
                     s.exit ? std::format("exit {} after {} ms", *s.exit, s.ms) : std::string("running"));
        std::println(std::cout, "    commands joined: {}", s.commands);
        if (s.trace) {
            std::vector<std::pair<int, std::string>> top;
            for (auto& [p, n] : s.programs) top.emplace_back(n, p);
            std::ranges::sort(top, std::greater<>{});
            std::println(std::cout, "    programs executed: {}", s.execs);
            for (std::size_t i = 0; i < std::min<std::size_t>(10, top.size()); ++i)
                std::println(std::cout, "      {:>5}  {}", top[i].first, top[i].second);
        } else {
            std::println(std::cout, "    programs executed: not traced (observe < full)");
        }
        if (!s.perm.empty()) {
            std::string line;
            for (auto& [a, n] : s.perm) line += std::format(" {}={}", a, n);
            std::println(std::cout, "    permission decisions:{}", line);
            for (auto& d : s.denied) std::println(std::cout, "      denied  {}", d);
        }
        if (!s.changed.empty()) {
            std::println(std::cout, "    files changed in rw mounts: {}", s.changed.size());
            for (std::size_t i = 0; i < std::min<std::size_t>(20, s.changed.size()); ++i)
                std::println(std::cout, "      {}", s.changed[i]);
        }
    }
    for (auto& c : policy_changes) std::println(std::cout, "\n  policy  {}", c);
    return 0;
}

// `subos ps`: the running sessions (design §22).
int run_ps_(int argc, char* argv[], EventStream& stream) {
    bool json = false;
    for (int i = 3; i < argc; ++i) if (std::string_view(argv[i]) == "--json") json = true;
    auto sessions = session::list(home_view());
    if (json) {
        for (auto& i : sessions) std::println(std::cout, "{}", session::to_json(i).dump());
        return 0;
    }
    if (sessions.empty()) {
        log::info("no running sessions");
        return 0;
    }
    nlohmann::json table;
    table["headers"] = {"SUBOS", "SESSION", "BACKEND", "STARTED", "PID", "MODE"};
    table["rows"] = nlohmann::json::array();
    for (auto& i : sessions) {
        table["rows"].push_back({i.instance, i.id, i.backend, i.started,
                                 std::to_string(i.supervisor_pid),
                                 i.detached ? std::format("detached, ttl {}s", i.ttl)
                                            : std::string("attached")});
    }
    stream.emit(DataEvent{"table", table.dump()});
    return 0;
}

// `subos log <name>`: the instance's audit, read from outside the sandbox
// where the supervisor wrote it (design §22).
int run_log_(int argc, char* argv[], EventStream& stream,
             const std::function<void(std::string_view)>& usageError) {
    std::string name;
    std::set<std::string> kinds;
    std::string sessionId;
    std::size_t limit = 50;
    bool json = false, follow = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") json = true;
        else if (a == "-f" || a == "--follow") follow = true;
        else if (a == "--kind" && i + 1 < argc) kinds.insert(argv[++i]);
        else if (a == "--session" && i + 1 < argc) sessionId = argv[++i];
        else if ((a == "-n" || a == "--lines") && i + 1 < argc) {
            try { limit = static_cast<std::size_t>(std::stoul(argv[++i])); }
            catch (...) { usageError("-n expects a number"); return 1; }
        }
        else if (!a.empty() && a[0] != '-' && name.empty()) name = std::move(a);
        else { usageError("unknown option for `xlings subos log`: " + a); return 1; }
    }
    if (name.empty()) {
        int rc = 0;
        name = pick_subos_or_fail_("log", stream, usageError, &rc);
        if (name.empty()) return rc;
    }
    const auto file = home_view().logs_dir(name) / "events.ndjson";
    auto keep = [&](const nlohmann::json& e) {
        if (!kinds.empty() && !kinds.contains(e.value("kind", ""))) return false;
        if (!sessionId.empty() && e.value("session", "") != sessionId) return false;
        return true;
    };
    auto print = [&](const nlohmann::json& e) {
        if (json) { std::println(std::cout, "{}", e.dump()); return; }
        std::string detail;
        for (std::string k : {"program", "path", "exit", "signal", "backend", "error", "count", "ms"}) {
            if (!e.contains(k)) continue;
            detail += std::format(" {}={}", k, e[k].is_string() ? e[k].get<std::string>() : e[k].dump());
        }
        std::println(std::cout, "{} {:<9} {:<14} {}{}", e.value("ts", ""), e.value("kind", ""),
                     e.value("event", ""), e.value("session", ""), detail);
    };
    auto events = observe::read(file);
    std::vector<nlohmann::json> shown;
    for (auto& e : events) if (keep(e)) shown.push_back(std::move(e));
    const auto from = shown.size() > limit ? shown.size() - limit : 0;
    for (auto i = from; i < shown.size(); ++i) print(shown[i]);
    if (!follow) return 0;
    // -f: keep reading what the supervisor appends.
    std::error_code ec;
    auto offset = fs::exists(file, ec) ? fs::file_size(file, ec) : 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto size = fs::exists(file, ec) ? fs::file_size(file, ec) : 0;
        if (size < offset) offset = 0;      // rotated
        if (size == offset) continue;
        std::ifstream in(file, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(offset));
        std::string line;
        while (std::getline(in, line)) {
            auto e = nlohmann::json::parse(line, nullptr, false);
            if (!e.is_discarded() && e.is_object() && keep(e)) print(e);
        }
        offset = size;
    }
}

int run(int argc, char* argv[], EventStream& stream) {
    // Drop the options root publishes as valid on every command before any
    // subcommand's argv loop sees them. `subos new` and `subos use` end their
    // loops with a catch-all `usageError`, so a documented global flag such as
    // `--yes` -- which an agent is instructed to always pass -- would come
    // back as "unknown option" from a command that has nothing to confirm.
    //
    // `-y` is recorded before it is dropped: for the commands that delete or
    // take over a directory (`remove`, and `new` onto one that exists) it is
    // the user's explicit answer to the question they would otherwise be asked.
    bool yesGiven = false;
    std::vector<char*> filtered;
    filtered.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        if (i >= 3) {
            const std::string_view a{argv[i]};
            if (a == "-y" || a == "--yes") yesGiven = true;
            if (cli::spec::is_global_option(argv[i])) continue;
        }
        filtered.push_back(argv[i]);
    }
    argc = static_cast<int>(filtered.size());
    argv = filtered.data();

    if (argc < 3) return run_list_(stream);

    std::string sub = argv[2];
    if (sub == "ls") sub = "list";
    if (sub == "rm") sub = "remove";
    if (sub == "i")  sub = "info";

    auto usageError = [&](std::string_view detail) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::InvalidInput,
            .message = std::string(detail),
            .recoverable = false,
            .hint = "usage: xlings subos <new|use|list|ls|remove|rm|info|i|stop|runtime> [name]",
        });
    };

    if (sub == "new") {
        if (argc < 4) { usageError("missing <name> for: xlings subos new"); return 1; }
        // Parse: xlings subos new <name> [--storage <mode>] [--image-size <size>] [--from <spec>]
        std::string name;
        sandbox::StorageMode storage = sandbox::StorageMode::Shared;
        std::string imageSize = "50G";
        // M2: --from <spec> creates the new subos by forking an existing
        // source. Spec containing `:` or `@` is treated as a pkg-spec
        // (auto-installs the base xpkg if missing); bare name is treated
        // as a local subos to fork from.
        std::string fromSpec;
        // --runtime <binding>: what this subos's binaries are built against
        // ("glibc@2.39"). Creation-time, because changing it later would
        // invalidate every payload already installed. Absent → the built-in
        // default, so existing invocations keep working unchanged.
        std::string runtime;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--runtime" && i + 1 < argc) {
                runtime = argv[++i];
            }
            else if (a.rfind("--runtime=", 0) == 0) {
                runtime = a.substr(10);
            }
            else if (a == "--storage" && i + 1 < argc) {
                auto s = std::string(argv[++i]);
                if (s == "image") storage = sandbox::StorageMode::Image;
                else if (s == "tmpfs") storage = sandbox::StorageMode::Tmpfs;
                else if (s == "shared") storage = sandbox::StorageMode::Shared;
                else {
                    usageError("unknown storage mode: " + s
                               + " (valid: shared, image, tmpfs)");
                    return 1;
                }
            }
            else if (a == "--image-size" && i + 1 < argc) {
                imageSize = argv[++i];
            }
            else if (a == "--from" && i + 1 < argc) {
                fromSpec = argv[++i];
            }
            else if (a.rfind("--from=", 0) == 0) {
                fromSpec = a.substr(7);
            }
            else if (!a.empty() && a[0] != '-' && name.empty()) {
                name = std::move(a);
            }
            else {
                usageError("unknown option for `xlings subos new`: " + a);
                return 1;
            }
        }
        if (name.empty()) {
            usageError("missing <name> for: xlings subos new");
            return 1;
        }
        if (!fromSpec.empty()) {
            return new_from(name, {}, storage, imageSize, fromSpec, runtime, yesGiven, stream);
        }
        return create(name, {}, storage, imageSize, runtime, yesGiven, "-y", stream);
    }
    if (sub == "use") {
        // Flags supported:
        //   --global         persist the choice into ~/.xlings.json + symlink
        //                    (legacy behavior; affects every shell)
        //   --shell <kind>   emit shell code on stdout for the user to
        //                    eval/Invoke-Expression. <kind> ∈ {sh,bash,zsh,
        //                    fish,pwsh}. Defaults to "sh" if not provided.
        //   --sandbox        Linux-only: enter via proot fs-isolation. $HOME,
        //                    /tmp, /etc/passwd are sandbox-private; ~/.xlings,
        //                    /usr, /lib*, /etc/{resolv.conf,ld.so.cache} are
        //                    bound from host. Same shell (`$SHELL`) as outside.
        //                    Prompt switches from `[xsubos:<name>]` to
        //                    `<xsubos:<name>>` so the user can tell at a glance.
        //   (no flag)        spawn a fresh interactive shell with
        //                    XLINGS_ACTIVE_SUBOS=<name> in env. Per-shell.
        std::string name;
        std::string mode = "spawn";        // default
        std::string shell_kind = "sh";
        bool sandbox = false;
        std::string sandbox_backend;       // "" = auto, "bwrap", "proot"
        IsolationArgs iso;                 // --sandbox=<preset>, --net, --allow, ...
        // M3: --cmd <string> runs a single command non-interactively
        // and exits with the command's exit code. Works in both shell-
        // level and sandbox modes. Internally routed to `sh -c <cmd>`
        // (POSIX) or `pwsh -Command <cmd>` / `cmd /c <cmd>` (Windows).
        std::string cmd;
        // The session's lifetime (design §16, the keeper's replacement):
        //   --no-keep      the session ends with the shell
        //   --keep         it stays until `subos stop`
        //   --ttl <sec>    it ends after this long idle
        bool no_keep = false;
        bool keep_forever = false;
        int  ttl_sec = 0;                  // 0 = use default
        // --gpu: opt-in NVIDIA + DRM device passthrough for bwrap
        // sandbox. Missing devices are silently skipped. Requires
        // --sandbox; ignored when the backend resolves to proot (proot
        // already passes /dev and /sys through wholesale).
        // Ref: .agents/docs/2026-05-22-subos-sandbox-gpu-passthrough.md
        bool gpu = false;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--global") { mode = "global"; }
            else if (a == "--shell") {
                mode = "shell";
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    shell_kind = argv[++i];
                }
            }
            else if (a.rfind("--shell=", 0) == 0) {
                mode = "shell";
                shell_kind = a.substr(8);
            }
            else if (std::string err;
                     a.starts_with("--sandbox") || a.starts_with("--net") || a.starts_with("--allow")
                     || a.starts_with("--fetch") || a.starts_with("--publish") || a.starts_with("--mount")
                     || a == "--no-degrade") {
                auto r = parse_isolation_flag_(a, i, argc, argv, iso, err);
                if (r < 0) { usageError(err); return 1; }
                if (r == 0) { usageError("unknown option for `xlings subos use`: " + a); return 1; }
                sandbox = sandbox || iso.sandbox;
                if (!iso.backend.empty()) sandbox_backend = iso.backend;
            }
            else if (a == "--cmd" && i + 1 < argc) {
                cmd = argv[++i];
            }
            else if (a.rfind("--cmd=", 0) == 0) {
                cmd = a.substr(6);
            }
            // The session's lifetime (see the declarations above).
            else if (a == "--no-keep") {
                no_keep = true;
            }
            else if (a == "--keep") {
                keep_forever = true;
            }
            else if (a == "--ttl" && i + 1 < argc) {
                try { ttl_sec = std::stoi(argv[++i]); }
                catch (...) {
                    usageError("--ttl expects an integer (seconds)");
                    return 1;
                }
            }
            else if (a.rfind("--ttl=", 0) == 0) {
                try { ttl_sec = std::stoi(std::string(a.substr(6))); }
                catch (...) {
                    usageError("--ttl=<sec> expects an integer");
                    return 1;
                }
            }
            else if (a == "--gpu") {
                gpu = true;
            }
            else if (!a.empty() && a[0] != '-' && name.empty()) {
                name = std::move(a);
            }
            else {
                usageError("unknown option for `xlings subos use`: " + a);
                return 1;
            }
        }
        if (no_keep && keep_forever) {
            usageError("--no-keep and --keep are mutually exclusive");
            return 1;
        }

        if (gpu && !sandbox) {
            usageError("--gpu requires --sandbox "
                       "(GPU passthrough only applies to bwrap-sandboxed sessions)");
            return 1;
        }

        // No name is a request to SEE the candidates -- but only when the rest
        // of the command asked for nothing else. Every other flag here names an
        // effect: run this command, emit this shell code, persist this choice,
        // enter this sandbox. Listing candidates and exiting 0 in their place
        // reports success for work that never happened, and `--shell` makes it
        // worse than a no-op: that stdout is eval'd by the caller, so a table
        // lands where shell code was expected.
        //
        // The acceptance path for this whole release is
        // `subos use <name> --sandbox --cmd ...`; a typo there has to fail, not
        // print a list and return 0.
        const bool discoveryOnly = mode == "spawn" && cmd.empty()
            && !sandbox && !gpu && !no_keep && !keep_forever && ttl_sec == 0;
        if (name.empty() && !discoveryOnly) {
            usageError("missing <name> for: xlings subos use "
                       "(run `xlings subos use` with no other arguments to "
                       "list candidates)");
            return 1;
        }

        auto resolved = resolve_use_name_(name, stream);
        if (resolved.selected.empty()) return resolved.exitCode;
        name = std::move(resolved.selected);

        if (mode == "global") {
            if (!cmd.empty()) {
                usageError("--cmd is incompatible with --global "
                           "(--global persists the active subos but doesn't spawn a shell)");
                return 1;
            }
            return use_global(name, stream);
        }
        if (mode == "shell") {
            if (!cmd.empty()) {
                usageError("--cmd is incompatible with --shell <kind> "
                           "(--shell emits env code; use plain `subos use --cmd` for non-interactive exec)");
                return 1;
            }
            return use_emit_shell(name, shell_kind, stream);
        }
        // --keep / --ttl: the session outlives this shell (design §16, the
        // keeper's replacement). Start it detached, then join it like any
        // later command would.
        // Linux only, like the keeper it replaces: elsewhere --keep / --ttl
        // change nothing and the shell below is the whole entry.
        if (platform::is_linux && sandbox && !no_keep && (keep_forever || ttl_sec > 0)
            && !session::find(home_view(), name)) {
            if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;
            use_detail_::apply_subos_env_(name);
            auto rc = sandbox::enter(name, stream, sandbox::EnterOptions{
                .backend = sandbox_backend, .gpu = gpu, .env = declared_env_(name),
                .ttl = keep_forever ? 0 : ttl_sec, .detached = true,
                // The join below announces the entry; once is enough.
                .announce = false,
                .preset = iso.preset, .overrides = iso.overrides });
            if (rc != 0) return rc;
        }
        return use_spawn_shell(name, stream, sandbox, sandbox_backend, gpu, cmd,
                               iso.preset, iso.overrides);
    }
    if (sub == "list")   return run_list_(stream);
    if (sub == "remove") {
        std::string target = argc > 3 ? argv[3] : std::string{};
        if (target.empty()) {
            int rc = 0;
            target = pick_subos_or_fail_("remove|rm", stream, usageError, &rc);
            if (target.empty()) return rc;
        }
        return remove(target, yesGiven, "-y", stream);
    }
    if (sub == "info")   return run_info_(argc > 3 ? argv[3] : "", stream);
    if (sub == "stop") {
        // Ends the instance's session (design §12.1). Safe when none runs.
        // The pre-session keeper's files are cleared too, for homes that
        // still carry them.
        std::string target = argc > 3 ? argv[3] : std::string{};
        if (target.empty()) {
            int rc = 0;
            target = pick_subos_or_fail_("stop", stream, usageError, &rc);
            if (target.empty()) return rc;
        }
        const bool had = session::find(home_view(), target).has_value();
        if (had && !session::stop(home_view(), target)) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Internal,
                .message = "the session of '" + target + "' did not stop",
                .recoverable = true,
            });
            return 1;
        }
        stop_legacy_keeper_(target);
        if (had) log::info("stopped the session of '{}'", target);
        else log::info("'{}' has no running session", target);
        return 0;
    }
    if (sub == "ps") return run_ps_(argc, argv, stream);
    if (sub == "exec") return run_exec_(argc, argv, stream);
    if (sub == "report") return run_report_(argc, argv, stream, usageError);
    if (sub == "requests" || sub == "approve" || sub == "deny")
        return run_requests_(sub, argc, argv, stream, usageError);
    if (sub == "config") return run_config_(argc, argv, stream, usageError);
    if (sub == "status") return run_status_(argc, argv, stream, usageError);
    if (sub == "doctor") return run_doctor_(argc, argv, stream, usageError);
    if (sub == "start") return run_start_(argc, argv, stream, usageError);
    if (sub == "cp") return run_cp_(argc, argv, stream, usageError);
    if (sub == "log") return run_log_(argc, argv, stream, usageError);

    // xlings subos runtime <binding> [name]
    //
    // Changing what a subos runs on is a DELIBERATE ACT, and until now it was
    // one the tool could not perform. `--runtime` was creation-time only, for
    // a stated reason -- "changing it later would invalidate every payload
    // already installed" -- and that reason is real. But the consequence was
    // that a runtime with a fix in it could never reach an existing subos by
    // any supported route, and nothing said so.
    //
    // So the answer is not to make it silent or automatic. An index update
    // must NEVER move a subos onto a different libc: that is precisely the
    // hazard of an INTERP and a RUNPATH coming from two different runtimes.
    // It is to make the deliberate act available, and to be honest at the
    // moment it happens about what it does not do.
    if (sub == "runtime") {
        if (argc < 4) {
            usageError("missing <binding> for: xlings subos runtime "
                       "<package@version> [name]");
            return 1;
        }
        const std::string binding = argv[3];
        if (!manifest::is_binding(binding)) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = "invalid runtime '" + binding
                           + "' (expected <package>@<version>, e.g. glibc@2.39)",
                .recoverable = false,
            });
            return 1;
        }

        std::string target = argc > 4 ? argv[4] : std::string{};
        if (target.empty()) {
            int rc = 0;
            target = pick_subos_or_fail_("runtime", stream, usageError, &rc);
            if (target.empty()) return rc;
        }
        const auto dir = Config::subos_dir(target);
        auto doc = manifest::read_document(dir);
        if (!doc) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = "subos '" + target + "' has no manifest to rebind",
                .recoverable = false,
            });
            return 1;
        }

        const auto previous = manifest::parse(*doc).runtime;
        if (previous == binding) {
            log::info("subos '{}' already declares {}", target, binding);
            return 0;
        }

        // Installed into THAT subos, not the active one. Both the override and
        // the env var, for the reason create() documents: one recomputes the
        // cached paths and the other is what the activation path re-reads,
        // and setting only one puts the payload in the right place while
        // reporting that it is not there.
        if (!manifest::runtime_is_hosted(binding)) {
            auto prevEnv = utils::get_env_or_default("XLINGS_ACTIVE_SUBOS");
            platform::set_env_variable("XLINGS_ACTIVE_SUBOS", target);
            auto prev = Config::set_active_subos_override(target);
            std::vector<std::string> targets{binding};
            // useAfterInstall TRUE here, unlike create().
            //
            // Rebinding is the one operation whose whole purpose is to change
            // what this subos runs on, so the new payload has to become the
            // ACTIVE one. Leaving it false installs the payload and leaves the
            // old version active -- declared and active then disagree, which
            // is precisely the state this change exists to make impossible,
            // arrived at by the command meant to fix it.
            //
            // create() passes false for a reason that does not apply: there,
            // activation would run before the registration for a subos being
            // created has landed. This subos already exists and is registered.
            const int rc = xim::cmd_install(targets, /*yes=*/true,
                                            /*noDeps=*/false, stream,
                                            /*forceGlobal=*/false,
                                            /*cancel=*/nullptr,
                                            /*dryRun=*/false,
                                            /*useAfterInstall=*/true);
            (void)Config::set_active_subos_override(prev);
            platform::set_env_variable("XLINGS_ACTIVE_SUBOS", prevEnv);
            if (rc != 0) {
                // Nothing is rewritten. A subos still declaring a runtime it
                // has is strictly better than one declaring a runtime that
                // was never installed -- which is the exact state this whole
                // change exists to make impossible.
                stream.emit(ErrorEvent{
                    .code = ErrorCode::Internal,
                    .message = "could not install " + binding
                               + "; subos '" + target + "' still declares "
                               + (previous.empty() ? "nothing" : previous),
                    .recoverable = true,
                    .hint = "xlings install " + binding,
                });
                return rc;
            }
        }

        auto runtimeAbi = runtime_abi_for(binding);
        (*doc)[std::string(manifest::BLOCK)] = manifest::make_block({
            .runtime   = binding,
            .by        = std::format("xlings {}", Info::VERSION),
            .hostGlibc = platform::host_glibc_version(),
            .intent    = manifest::Intent::Create,
            // A human asked for this one by name. That is what `explicit`
            // means, and it is true here in a way it is not on any path that
            // took a default.
            .runtimeSource = std::string(manifest::RUNTIME_SOURCE_EXPLICIT),
            .runtimeAbi    = std::move(runtimeAbi),
        });
        write_config_json_(dir / ".xlings.json", *doc);

        log::info("subos '{}' now declares {}{}", target, binding,
                  previous.empty() ? "" : std::format(" (was {})", previous));
        // Said at the moment it stops being true, not buried in a document.
        // Binaries already built in this subos have the OLD loader in their
        // INTERP and the old payload in their RUNPATH; nothing here rewrites
        // them, and they will keep running against the runtime they were
        // linked to until they are rebuilt.
        log::warn("programs already built in this subos still point at the "
                  "previous runtime -- rebuild them to pick up {}", binding);
        return 0;
    }

    usageError("unknown subcommand: " + sub);
    return 1;
}

}
