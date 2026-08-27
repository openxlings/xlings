module;

// System headers used by use_spawn_shell only. `import std;` doesn't
// pull these in, and we want execl/errno (POSIX) or CreateProcess
// (Win32) without #include in the named-module purview (which the
// standard forbids for headers that aren't importable units).
#include <cstdio>

// stdout / stderr as FILE*, for std::println.
//
// EVERY std::println here names its stream, and that is load-bearing rather
// than tidy: without one, clang deduces the format string itself as the first
// ARGUMENT and instantiates formatter<basic_format_string<...>>, which is
// deleted. gcc accepts the same code. Whether it trips is decided by what
// else the translation unit imports, so it cannot be predicted from this
// file -- adding one unrelated import to this TU is exactly what surfaced it.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// windows.h defines `min`/`max` as function-like macros, which turns any
// `std::min({a, b, c})` in this module into "too many arguments provided to
// function-like macro invocation" -- an error that names the call site and
// says nothing about where the macro came from. Every other module here that
// includes windows.h already defines this (platform.cppm, platform/windows.cppm,
// platform/target.cppm); this one was the exception, and it cost four
// consecutive red Windows runs.
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#endif

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
import xlings.core.xim.commands;
import xlings.core.subos.keeper;
import xlings.core.subos.gpu;
import xlings.core.subos.graphics;
import xlings.core.subos.sandbox;
import xlings.core.subos.manifest;
import xlings.cli.spec;
import xlings.i18n;

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

SubosInfo candidate_info_(const std::string& name, bool includeToolCount) {
    auto& p = Config::paths();
    auto dir = Config::subos_dir(name);
    int toolCount = 0;
    auto binDir = dir / "bin";
    if (includeToolCount && fs::exists(binDir)) {
        for (auto& e : platform::dir_entries(binDir)) {
            auto stem = e.path().stem().string();
            if (!xself::is_builtin_shim(stem) && stem != "xvm-alias")
                ++toolCount;
        }
    }
    return {name, dir, p.activeSubos == name, toolCount};
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

std::string lowercase_name_(std::string_view name) {
    std::string lowered(name);
    for (auto& ch : lowered) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return lowered;
}

std::size_t edit_distance_(std::string_view lhs, std::string_view rhs) {
    std::vector<std::size_t> prev(rhs.size() + 1);
    std::vector<std::size_t> next(rhs.size() + 1);
    std::iota(prev.begin(), prev.end(), std::size_t{0});
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        next[0] = i + 1;
        for (std::size_t j = 0; j < rhs.size(); ++j) {
            const auto replace = prev[j] + (lhs[i] == rhs[j] ? 0u : 1u);
            next[j + 1] = std::min({prev[j + 1] + 1, next[j] + 1, replace});
        }
        std::swap(prev, next);
    }
    return prev.back();
}

std::vector<SubosInfo> suggestions_(
        std::string_view query,
        std::span<const SubosInfo> candidates) {
    struct Scored {
        int substringRank;
        std::size_t distance;
        SubosInfo candidate;
    };
    const auto loweredQuery = lowercase_name_(query);
    std::vector<Scored> scored;
    scored.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        const auto lowered = lowercase_name_(candidate.name);
        const bool related = lowered.contains(loweredQuery)
            || loweredQuery.contains(lowered);
        scored.push_back({related ? 0 : 1,
                          edit_distance_(loweredQuery, lowered), candidate});
    }
    std::ranges::sort(scored, [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.substringRank, lhs.distance, lhs.candidate.name)
             < std::tie(rhs.substringRank, rhs.distance, rhs.candidate.name);
    });
    std::vector<SubosInfo> result;
    for (std::size_t i = 0; i < std::min<std::size_t>(3, scored.size()); ++i) {
        result.push_back(std::move(scored[i].candidate));
    }
    return result;
}

CandidateResolution_ resolve_candidate_(std::string_view query) {
    auto all = candidate_view(false).candidates;
    if (query.empty()) {
        return {.reason = "missing_name", .candidates = std::move(all)};
    }

    if (auto exact = std::ranges::find(all, query, &SubosInfo::name);
        exact != all.end()) {
        return {.selected = exact->name,
                .reason = "exact",
                .candidates = {*exact}};
    }

    const auto loweredQuery = lowercase_name_(query);
    std::vector<SubosInfo> matches;
    for (const auto& candidate : all) {
        if (lowercase_name_(candidate.name) == loweredQuery) {
            matches.push_back(candidate);
        }
    }
    if (matches.size() == 1) {
        return {.selected = matches.front().name,
                .reason = "case_insensitive_exact",
                .candidates = std::move(matches),
                .autoSelected = true};
    }
    if (matches.size() > 1) {
        return {.reason = "ambiguous", .candidates = std::move(matches)};
    }

    for (const auto& candidate : all) {
        if (lowercase_name_(candidate.name).starts_with(loweredQuery)) {
            matches.push_back(candidate);
        }
    }
    if (matches.size() == 1) {
        return {.selected = matches.front().name,
                .reason = "unique_prefix",
                .candidates = std::move(matches),
                .autoSelected = true};
    }
    if (matches.size() > 1) {
        return {.reason = "ambiguous", .candidates = std::move(matches)};
    }

    return {.reason = "not_found",
            .candidates = suggestions_(query, all)};
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
            {"pkgCount", candidate.toolCount},
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
    auto json = read_config_json_(dir / ".xlings.json");
    if (!json.is_object()) json = nlohmann::json::object();
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

int create(const std::string& name, const fs::path& customDir,
                  sandbox::StorageMode storage, const std::string& imageSize,
                  const std::string& runtime,
                  EventStream& stream) {
    auto& p = Config::paths();

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

    fs::create_directories(dir / "bin");
    fs::create_directories(dir / "lib");
    fs::create_directories(dir / "usr");
    fs::create_directories(dir / "generations");

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

    // Create shim hardlinks from xlings binary
    auto xlingsBin = p.homeDir / "xlings";
    if (!fs::exists(xlingsBin))
        xlingsBin = p.homeDir / "bin" / "xlings";
    if (fs::exists(xlingsBin)) {
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
        std::error_code rmec;
        fs::remove_all(dir, rmec);
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
#if defined(__linux__)
    // cp -a preserves mode/ownership/timestamps; --reflink=auto uses
    // COW where available (btrfs/xfs) and full copy otherwise.
    copy_cmd = std::format(
        "cp -a --reflink=auto '{}/.' '{}/'", src.string(), dst.string());
#elif defined(__APPLE__)
    // APFS clonefile via /bin/cp -c
    copy_cmd = std::format("cp -ac '{}/.' '{}/'", src.string(), dst.string());
#endif

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
                    EventStream& stream) {
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
            auto xlings_bin = p.homeDir / "xlings";
            if (!fs::exists(xlings_bin))
                xlings_bin = p.homeDir / "bin" / "xlings";

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
    if (auto rc = create(name, customDir, storage, imageSize, runtime, stream);
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
    auto xlingsBin = p.homeDir / "xlings";
    if (!fs::exists(xlingsBin))
        xlingsBin = p.homeDir / "bin" / "xlings";
    if (fs::exists(xlingsBin)) {
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
    // std::println(stderr,
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
        std::println(stdout, R"(set -gx XLINGS_ACTIVE_SUBOS "{}";)", name);
        std::println(stdout, R"(set -gx XLINGS_BIN "{}";)", bin_dir.string());
        // Strip any old subos bin segments from PATH, then prepend the new
        // bin. fish's $PATH is a list, so we use string match -v.
        std::println(stdout, R"(set -gx PATH "{}" (string match -v -r "^{}/subos/[^/]+/bin$" -- $PATH);)",
                     bin_dir.string(), p.homeDir.string());
        for (const auto& v : envVars) {
            if (v.unresolved) continue;
            // R"SH(...)SH": the fish source below contains `)"`, which ends a
            // plain R"(...)" literal early -- and the truncation compiles,
            // because what is left is still a valid string.
            if (v.op == manifest::OP_PREPEND) {
                std::println(stdout, 
                    R"SH(if set -q {0}; set -gx {0} "{1}:${0}"; else; set -gx {0} "{1}"; end;)SH",
                    v.var, v.value);
            } else {
                std::println(stdout, R"SH(if not set -q {0}; set -gx {0} "{1}"; end;)SH",
                             v.var, v.value);
            }
        }
        return 0;
    }
    if (is_pwsh) {
        std::println(stdout, R"($env:XLINGS_ACTIVE_SUBOS = '{}')", name);
        std::println(stdout, R"($env:XLINGS_BIN = '{}')", bin_dir.string());
        std::println(stdout, R"($env:Path = '{}' + ';' + (($env:Path -split ';') -notmatch '^{}\\subos\\[^\\]+\\bin$' -join ';'))",
                     bin_dir.string(), p.homeDir.string());
        for (const auto& v : envVars) {
            if (v.unresolved) continue;
            // ';' rather than ':' -- these are path lists, and on Windows the
            // separator is the one the platform's own tools split on.
            if (v.op == manifest::OP_PREPEND) {
                std::println(stdout, 
                    R"($env:{0} = if ($env:{0}) {{ '{1}' + ';' + $env:{0} }} else {{ '{1}' }})",
                    v.var, v.value);
            } else {
                // `$null -eq`, not `-not`: PowerShell's `-not` is true for an
                // empty string too, which would overwrite a value the user
                // deliberately set to "".
                std::println(stdout, R"(if ($null -eq $env:{0}) {{ $env:{0} = '{1}' }})",
                             v.var, v.value);
            }
        }
        return 0;
    }
    // POSIX (sh/bash/zsh) default
    auto orig_path = utils::get_env_or_default("PATH");
    auto new_path  = use_detail_::rebuild_path_for_subos_(
        orig_path, p.homeDir, bin_dir);
    std::println(stdout, R"(export XLINGS_ACTIVE_SUBOS="{}";)", name);
    std::println(stdout, R"(export XLINGS_BIN="{}";)", bin_dir.string());
    std::println(stdout, R"(export PATH="{}";)", new_path);
    for (const auto& v : envVars) {
        if (v.unresolved) continue;
        if (v.op == manifest::OP_PREPEND) {
            // ${VAR:+:$VAR} appends the separator only when VAR is non-empty,
            // so an unset variable does not become a trailing ':' -- which an
            // empty PATH-list element reads as "the current directory".
            std::println(stdout, R"(export {0}="{1}${{{0}:+:${0}}}";)", v.var, v.value);
        } else {
            // `${VAR=v}`, NOT `${VAR:=v}`. The colon form also assigns when VAR
            // is set-but-empty, which would overwrite a value the user chose.
            std::println(stdout, R"(: "${{{0}={1}}}"; export {0};)", v.var, v.value);
        }
    }
    return 0;
}

int use_spawn_shell(const std::string& name, EventStream& stream, bool sandbox, const std::string& sandbox_backend, bool gpu, const std::string& cmd)
{
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
        return sandbox::enter(name, stream, sandbox_backend, gpu, cmd);
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
        // V6: image-storage subos has an ext4 mount at <subos>/.mountpoint.
        // remove_all would either (a) hit EBUSY at the mountpoint, leaving
        // a half-cleaned tree behind, or (b) silently recurse into the
        // live mount and erase the image's contents before EBUSY surfaces.
        // Detect and umount first.
#if defined(__linux__)
        auto mountpoint = dir / ".mountpoint";
        if (fs::exists(mountpoint)
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
#endif
        std::error_code ec;
        fs::remove_all(dir, ec);
        if (ec) {
            stream.emit(ErrorEvent{
                .code = ErrorCode::Permission,
                .message = "failed to remove " + dir.string() + ": " + ec.message(),
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

    int toolCount = 0;
    auto binDir = dir / "bin";
    if (fs::exists(binDir)) {
        for (auto& e : platform::dir_entries(binDir)) {
            auto stem = e.path().stem().string();
            if (!xself::is_builtin_shim(stem) && stem != "xvm-alias")
                ++toolCount;
        }
    }
    return SubosInfo{name, dir, p.activeSubos == name, toolCount};
}

int run_list_(EventStream& stream) {
    auto all = candidate_view().candidates;
    std::vector<std::tuple<std::string, std::string, int, bool>> entries;
    for (auto& s : all) {
        entries.emplace_back(s.name, s.dir.string(), s.toolCount, s.isActive);
    }
    nlohmann::json entriesJson = nlohmann::json::array();
    for (auto& [n, d, tc, active] : entries) {
        entriesJson.push_back({{"name", n}, {"dir", d}, {"pkgCount", tc}, {"active", active}});
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
    fieldsJson.push_back({{"label", "tools"}, {"value", std::to_string(si->toolCount)}, {"highlight", false}});
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

int run(int argc, char* argv[], EventStream& stream) {
    // Drop the options root publishes as valid on every command before any
    // subcommand's argv loop sees them. `subos new` and `subos use` end their
    // loops with a catch-all `usageError`, so a documented global flag such as
    // `--yes` -- which an agent is instructed to always pass -- would come
    // back as "unknown option" from a command that has nothing to confirm.
    std::vector<char*> filtered;
    filtered.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        if (i >= 3 && cli::spec::is_global_option(argv[i])) continue;
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
            return new_from(name, {}, storage, imageSize, fromSpec, runtime, stream);
        }
        return create(name, {}, storage, imageSize, runtime, stream);
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
        // M3: --cmd <string> runs a single command non-interactively
        // and exits with the command's exit code. Works in both shell-
        // level and sandbox modes. Internally routed to `sh -c <cmd>`
        // (POSIX) or `pwsh -Command <cmd>` / `cmd /c <cmd>` (Windows).
        std::string cmd;
        // M5: explicit keeper policy overrides (D9). The runtime auto-
        // default (storage=image|tmpfs + sandbox + Linux → keeper on,
        // TTL=5min) is encoded in keeper::should_auto_keeper. These
        // flags let the user override per call:
        //   --no-keep      force disable (one-shot, even if auto would)
        //   --keep         never-expiring (use until `subos stop`)
        //   --ttl <sec>    custom idle TTL
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
            else if (a == "--sandbox") {
                sandbox = true;
                // Optional backend argument: --sandbox bwrap | --sandbox proot
                if (i + 1 < argc) {
                    std::string next = argv[i + 1];
                    if (next == "bwrap" || next == "proot") {
                        sandbox_backend = next;
                        ++i;
                    }
                }
            }
            else if (a == "--cmd" && i + 1 < argc) {
                cmd = argv[++i];
            }
            else if (a.rfind("--cmd=", 0) == 0) {
                cmd = a.substr(6);
            }
            // M5 keeper policy flags. The runtime spawning of the keeper
            // is wired separately (see keeper.cppm); these flags are
            // accepted on the CLI and threaded through for forward
            // compatibility. Auto-default (D9) is governed by
            // should_auto_keeper() at runtime.
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
        return use_spawn_shell(name, stream, sandbox, sandbox_backend, gpu, cmd);
    }
    if (sub == "list")   return run_list_(stream);
    if (sub == "remove") {
        std::string target = argc > 3 ? argv[3] : std::string{};
        if (target.empty()) {
            int rc = 0;
            target = pick_subos_or_fail_("remove|rm", stream, usageError, &rc);
            if (target.empty()) return rc;
        }
        return remove(target, stream);
    }
    if (sub == "info")   return run_info_(argc > 3 ? argv[3] : "", stream);
    if (sub == "stop") {
        // M4/D9: stop the auto-keeper for a sandboxed subos.
        // Safe to invoke even when no keeper is running — it's a no-op.
        std::string target = argc > 3 ? argv[3] : std::string{};
        if (target.empty()) {
            int rc = 0;
            target = pick_subos_or_fail_("stop", stream, usageError, &rc);
            if (target.empty()) return rc;
        }
        return keeper::stop_keeper(target);
    }

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
