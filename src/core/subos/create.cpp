// Making a SubOS: an empty one (create), or one from a template or another SubOS
// (new_from, `--from`), and what each needs before it is used -- its subos_info,
// its runtime binding, a name that names a subos.
module xlings.core.subos;

import xlings.subos.roles;
import xlings.core.subos.root;
import std;
import xlings.core.config;
import xlings.core.home_config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.subos.tools;
import xlings.subos.ports;
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
import xlings.carrier;
import xlings.carrier.wsl2;
import xlings.core.subos.carrier_image;
import xlings.subos.caps;
import xlings.subos.model;
import xlings.core.subos.ports;
import xlings.subos.session;
import xlings.subos.policy;
import xlings.subos.policy_store;
import xlings.subos.broker;
import xlings.observe;
import xlings.core.home;
import xlings.core.home.domain_producer;
import xlings.core.home.domain_producer_source;
import xlings.libs.sha256;
import xlings.core.version_order;

namespace xlings::subos {

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

bool reject_unrouted_domain_(const std::string& name, std::string_view operation, EventStream& stream) {
    auto scope = xlings::home::domain_producer::read_scope(Config::paths().homeDir, name);
    if (scope && !*scope) return false;
    stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput,
        .message = scope ? std::format("{} of a private prefix-domain instance is not supported yet", operation)
                         : scope.error(), .recoverable = false });
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
    if (reject_unrouted_domain_(name, "replacement", stream)) return 1;

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

    // In process: each file cloned where the filesystem shares blocks
    // (btrfs, XFS, APFS), copied where it does not; links stay links, modes
    // and times are kept. Shims are regenerated below.
    if (auto copied = platform::copy_tree(src, dst); !copied) {
        stream.emit(ErrorEvent{
            .code = ErrorCode::Internal,
            .message = "fork copy failed: " + copied.error(),
            .recoverable = false,
        });
        return 1;
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

    // No version and none in the index: the newest installed one.
    fs::path latest;
    std::error_code ec;
    for (auto it = fs::directory_iterator(base, ec);
         !ec && it != std::default_sentinel; it.increment(ec)) {
        if (it->is_directory(ec)
            && (latest.empty() || version_order::compare(it->path().filename().string(),
                                                         latest.filename().string()) > 0))
            latest = it->path();
    }
    return latest;
}

}

// A making resolves its base more than once (the lineage, then the top): what
// it says about the choice is said once.
bool said_once_(std::string key) {
    static std::set<std::string> said;
    return said.insert(std::move(key)).second;
}

fs::path resolve_base_package_(const std::string& fromSpec, EventStream& stream) {
    auto& p = Config::paths();
    // ── pkg-spec path: locate or install the base xpkg ────────────
    auto ref = new_from_detail_::parse_pkg_spec_(fromSpec);
    // No version: the index's (its `latest`), not whichever is installed --
    // an edition installed once would otherwise be the newest forever.
    if (ref.ver.empty()) {
        std::string passedOver;
        if (auto v = xim::index_version_of(fromSpec, xim::CatalogAccess::LocalOnly, &passedOver)) ref.ver = *v;
        // An older version because the newest needs a newer xlings: said as
        // data too, for whoever asked for it (luban new says it; E8).
        if (!passedOver.empty() && said_once_("passed_over:" + fromSpec))
            stream.emit(DataEvent{"passed_over", nlohmann::json{{"package", fromSpec}, {"version", ref.ver},
                                                               {"message", passedOver}}.dump()});
        // The index could not be read: never whichever version is installed,
        // silently (design 2026-10-19 §9.2) -- "could not read" is not "none".
        if (ref.ver.empty() && !xim::get_catalog(xim::CatalogAccess::LocalOnly).is_loaded()) {
            stream.emit(ErrorEvent{.code = ErrorCode::NotFound,
                .message = std::format("the package index could not be read, so which version of {} to use is not known", fromSpec),
                .recoverable = true, .hint = "xlings update, or name a version: " + fromSpec + "@<version>"});
            return {};
        }
    }
    auto baseDir = new_from_detail_::locate_base_pkg_(ref);
    // The index has no such package and one is installed: that one, said
    // (the same shape as passed_over, for whoever asked).
    if (ref.ver.empty() && !baseDir.empty() && said_once_("installed_only:" + fromSpec)) {
        const auto version = baseDir.filename().string();
        stream.emit(DataEvent{"installed_only", nlohmann::json{{"package", fromSpec}, {"version", version},
            {"message", std::format("{} is not in the index; using version {} installed here", fromSpec, version)}}.dump()});
    }

    if (!baseDir.empty()) {
        auto borrowed = xlings::home::domain_producer_source::borrowed_mount(baseDir);
        if (!borrowed) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = borrowed.error(),
                .recoverable = false,
            });
            return {};
        }
        if (*borrowed) {
            const std::vector<std::string> targets{fromSpec};
            if (xim::cmd_install(targets, /*yes=*/true, /*noDeps=*/false, stream) != 0)
                return {};
            baseDir = new_from_detail_::locate_base_pkg_(ref);
        }
    }

    if (baseDir.empty()) {
        // Auto-install (E5a): the template is plumbing -- the user named a
        // SubOS, not a package -- so it is installed in this process, its
        // progress and errors shown, its own plan and summary not.
        log::debug("base subos pkg '{}' not installed; installing it", fromSpec);
        const std::vector<std::string> targets{fromSpec};
        if (xim::cmd_install_step(targets, "", stream, /*plumbing=*/true) != 0) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Internal,
                .message = "auto-install of base '" + fromSpec
                           + "' failed",
                .recoverable = true,
                .hint = "run manually: xlings install " + fromSpec,
            });
            return {};
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
    }
    return baseDir;
}

int new_from(const std::string& name, const fs::path& customDir,
                    sandbox::StorageMode storage, const std::string& imageSize,
                    const std::string& fromSpec, const std::string& runtime,
                    bool yes, EventStream& stream) {
    auto& p = Config::paths();

    fs::path baseDir;

    if (new_from_detail_::is_pkg_spec_(fromSpec)) {
        baseDir = resolve_base_package_(fromSpec, stream);
        if (baseDir.empty()) return 1;
    } else {
        // ── local fork path: source is an existing subos by name ──────
        if (reject_unrouted_domain_(fromSpec, "forking", stream)) return 1;
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

    // A template that declares a root: the host is checked now, before the
    // packages it declares are fetched.
    if (new_from_detail_::is_pkg_spec_(fromSpec)) {
        std::ifstream manifestIn(baseDir / ".xlings.json");
        auto manifest = nlohmann::json::parse(manifestIn, nullptr, false);
        // An edition that uses what a newer client reads says so (Luban
        // design part 2 §2.6): refused before anything is fetched.
        if (auto m = manifest.is_object() ? manifest.find("min_client") : manifest.end();
            !manifest.is_discarded() && manifest.is_object() && m != manifest.end() && m->is_string()) {
            const auto order = policy::compare_client_versions(Info::VERSION, m->get<std::string>());
            if (!order || *order < 0) {
                stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput,
                    .message = std::format("{} needs xlings >= {}, this is {}", fromSpec, m->get<std::string>(),
                                           Info::VERSION),
                    .recoverable = true, .hint = "xlings self update"});
                return 1;
            }
        }
        if (auto refused = root_abi_refusal_(manifest.is_discarded() ? nlohmann::json() : manifest)) {
            stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput,
                                   .message = std::format("'{}' cannot be made here from {}: {}", name, fromSpec, *refused),
                                   .recoverable = false,
                                   .hint = "an edition built for this machine"});
            return 1;
        }
        if (!manifest.is_discarded() && manifest.is_object() && manifest.value("subos_kind", "") == "rootfs")
            if (auto r = sandbox::prepare_root_host(yes, stream); r != 0) return r;
    }

    // Create target subos via standard `create`. This sets up
    // bin/lib/usr/generations, writes initial .xlings.json, optionally
    // creates home.img, and registers the subos. Its own "created" report is
    // not shown: this command reports once, at the end, with the base it
    // came from (subos_forked) -- two reports of one creation, the second
    // naming the template's directory as if it were the subos's, read as two
    // subos.
    EventStream inner;
    inner.set_can_confirm(stream.can_confirm());
    inner.set_can_select(stream.interactive());
    inner.on_event([&](const Event& e) {
        if (const auto* d = std::get_if<DataEvent>(&e); d && d->kind == "subos_created") return;
        stream.emit(e);
    });
    if (auto rc = create(name, customDir, storage, imageSize, runtime, yes, "-y", inner);
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
    payload["dir"]     = dstDir.string();
    payload["storage"] = sandbox::storage_to_string_(storage);
    stream.emit(DataEvent{"subos_forked", payload.dump()});
    return 0;
}

}  // namespace xlings::subos
