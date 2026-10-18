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
                      std::string_view hint) {
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


 // namespace use_detail_

// Internal — not exported. `xlings subos use --global <name>` and
// the back-compat single-arg `use()` both route here.
int use_global(const std::string& name, EventStream& stream) {
    if (auto rc = use_detail_::validate_subos_(name, stream); rc != 0) return rc;
    if (reject_unrouted_domain_(name, "global activation", stream)) return 1;

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
                    std::optional<policy::Preset> preset, const policy::Overrides& overrides, std::string_view persist)
{
    const std::string persisted(persist.empty() ? "all" : persist);
    if (persisted != "all" && persisted != "home" && persisted != "none") {
        stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput,
                               .message = std::format("--persist {}: all, home or none", persist), .recoverable = false});
        return 2;
    }
    if (persisted != "all") {
        const auto kind = subos_root::read_kind(Config::paths().homeDir, name);
        if (!kind || *kind != roles::Kind::Rootfs) {
            stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput,
                                   .message = std::format("--persist {} keeps a root's state from a session; '{}' is not a root", persist, name),
                                   .recoverable = false});
            return 2;
        }
        if (session::find(home_view(), name)) {
            stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput,
                                   .message = std::format("'{}' has a running session, which keeps what it writes; "
                                                          "`xlings subos stop {}` first", name, name),
                                   .recoverable = true});
            return 2;
        }
        sandbox = true;
    }
    // A declared instance is entered under its policy however it is entered
    // (design §10: the secure default belongs to the instance, not the flag).
    if (!sandbox && (preset || enters_sandboxed_(name))) sandbox = true;
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
        // A `--cmd` run is a script: its standard output is the command's alone
        // (a stream a program reads, as the Luban OS window reads a desktop's).
        return sandbox::enter(name, stream, sandbox::EnterOptions{
            .backend = sandbox_backend, .gpu = gpu, .cmd = cmd, .env = declared_env_(name),
            .announce = cmd.empty(), .preset = preset, .overrides = overrides, .persist = persisted });
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

    auto scope = xlings::home::domain_producer::read_scope(p.homeDir, name);
    if (!scope) {
        stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput, .message = scope.error(), .recoverable = false });
        return 1;
    }
    if (*scope) {
        const auto& actual = **scope;
        if (!role_allows_at_(actual.domain.physicalHome, roles::Op::Remove, name, stream)) return 1;
        auto preflight = xlings::home::domain_producer::check_control_removal(actual, name);
        if (!preflight) {
            stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput, .message = preflight.error(), .recoverable = false });
            return 1;
        }
        const auto what = userdata::describe(userdata::census(actual.producerInstance));
        auto asked = confirm::ask(stream, "subos_remove",
            std::format("remove subos '{}'? This deletes {} ({}) and cannot be undone", name,
                        actual.producerInstance.string(), what), yes, yesSpelling);
        if (asked.outcome == confirm::Outcome::Declined) return 0;
        if (asked.outcome == confirm::Outcome::NobodyToAsk) {
            stream.emit(ErrorEvent{ .code = ErrorCode::InvalidInput,
                .message = "removing '" + name + "' requires confirmation; nothing was removed", .recoverable = true,
                .hint = userdata::needs_confirmation_hint(yesSpelling, "remove_subos") });
            return 2;
        }
        const std::vector<std::string> arguments{"subos", "remove", name, "-y"};
        auto removed = xlings::home::domain_producer::run(actual.domain, arguments, std::nullopt, true);
        if (!removed) {
            stream.emit(ErrorEvent{ .code = ErrorCode::Internal, .message = removed.error(), .recoverable = true });
            return 1;
        }
        if (*removed != 0) return *removed;
        auto cleaned = xlings::home::domain_producer::remove_control(actual, name);
        if (!cleaned) {
            stream.emit(ErrorEvent{ .code = ErrorCode::Internal, .message = cleaned.error(), .recoverable = true });
            return 1;
        }
        stream.emit(DataEvent{"subos_removed", nlohmann::json{{"name", name}}.dump()});
        return 0;
    }

    if (!role_allows_(roles::Op::Remove, name, stream)) return 1;
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

    // Its record (config/subos/<n>: what it is, its policy, its persona,
    // luban's settings) goes with it: a record of nothing is what a later
    // `subos new` of the same name -- a Luban system moving into its prefix
    // domain -- found and refused as "output already exists".
    {
        std::error_code ec;
        const auto record = home_view().config_dir(name);
        for (fs::directory_iterator it(record, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) fs::remove(it->path(), ec);
        fs::remove(record, ec);
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

int run_info_(const std::string& name, EventStream& stream) {
    auto& p = Config::paths();
    auto target = name.empty() ? p.activeSubos : name;
    if (!name.empty()) {
        const std::vector<std::string> domainArgs{"subos", "info", name};
        if (auto result = run_domain_operation_(name, domainArgs, stream)) return *result;
    }
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

// The declaration a new instance starts with: the preset `new --sandbox`
// named, else -- forked from a local instance -- that instance's policy, so
// a copy is isolated as its source was. Written outside the instance and
// audited, as `subos config` does.
// A template's `policy` (`"policy": "xim:agent-private@2026.10.10.1"`) and
// `subos new --proxy`: `subos config <name> --sandbox <policy> --net proxy
// --proxy <url>`, in this process.
int apply_declared_policy_(const std::string& name, const std::string& proxy, EventStream& stream,
                           const std::function<void(std::string_view)>& usageError) {
    std::ifstream in(home_view().instance(name) / ".xlings.json");
    auto manifest = nlohmann::json::parse(in, nullptr, false);
    const std::string declared = manifest.is_object() && manifest.contains("policy") && manifest["policy"].is_string()
        ? manifest["policy"].get<std::string>() : std::string{};
    if (declared.empty() && proxy.empty()) return 0;
    std::vector<std::string> words{"xlings", "subos", "config", name};
    if (!declared.empty()) words.insert(words.end(), {"--sandbox", declared});
    if (!proxy.empty()) words.insert(words.end(), {"--net", "proxy", "--proxy", proxy});
    std::vector<char*> argv;
    for (auto& w : words) argv.push_back(w.data());
    argv.push_back(nullptr);
    return run_config_(static_cast<int>(words.size()), argv.data(), stream, usageError);
}

int declare_isolation_at_creation_(const std::string& name, std::optional<policy::Preset> preset,
                                   const std::string& from, EventStream& stream) {
    const auto home = home_view();
    std::optional<policy::Policy> pol;
    std::string origin;
    if (preset) {
        pol = policy::preset(*preset);
        origin = "subos new --sandbox";
    } else if (!from.empty() && from.find_first_of(":@") == std::string::npos
               && policy_store::has_file(home, from)) {
        auto src = policy_store::read(home, from);
        if (src && *src) { pol = **src; origin = "subos new --from " + from; }
    }
    if (!pol) return 0;
    if (auto w = policy_store::write(home, name, *pol); !w) {
        stream.emit(ErrorEvent{ .code = ErrorCode::Internal, .message = w.error(), .recoverable = true });
        return 1;
    }
    observe::append(home.logs_dir(name) / "events.ndjson", observe::Event{
        .kind = observe::Kind::Lifecycle,
        .fields = {{"event", "policy-change"}, {"instance", name}, {"by", origin},
                   {"diff", policy::diff(policy::legacy(), *pol)}}});
    log::info("'{}' is declared {}: `xlings subos use {}` and `subos exec` enter it that way",
              name, policy::to_string(pol->preset), name);
    return 0;
}


// A command for a system in a prefix domain runs where the system lives
// (part 3 §B), and what it says comes back (design 2026-10-16 L §5): at a
// terminal it speaks there itself and asks there; asked through the
// interface (luban, the window), it runs as `interface subos_command` and its
// events are relayed. The one path for every routed command and for making a
// system -- making one used to bypass it, and `luban new` showed one line for
// minutes. `words` is the subos command without the leading `subos`.
std::expected<int, std::string> run_domain_relayed_(const xlings::home::prefix_domain::Domain& domain,
                                                   std::vector<std::string> words,
                                                   std::optional<xlings::home::domain_producer::OutputBinding> output,
                                                   bool yes, EventStream& stream, bool runtime) {
    if (stream.interactive()) {
        std::vector<std::string> args{"subos"};
        args.insert(args.end(), words.begin(), words.end());
        if (yes) args.push_back("-y");
        return xlings::home::domain_producer::run(domain, args, output, runtime);
    }
    nlohmann::json call{{"argv", words}};
    if (yes) call["yes"] = true;
    const std::vector<std::string> args{"interface", "subos_command", "--args", call.dump()};
    int answered = -1;
    auto rc = xlings::home::domain_producer::run(domain, args, output, runtime,
        [&](std::string_view text) {
            auto j = nlohmann::json::parse(text, nullptr, false);
            if (!j.is_object()) return;
            const auto kind = j.value("kind", "");
            if (kind == "result") answered = j.value("exitCode", 1);
            else if (kind == "data")
                stream.emit(DataEvent{j.value("dataKind", ""), j.value("payload", nlohmann::json()).dump()});
            else if (kind == "progress")
                stream.emit(ProgressEvent{j.value("phase", ""), j.value("percent", 0.0f), j.value("message", "")});
            else if (kind == "log") {
                const auto level = j.value("level", "info");
                stream.emit(LogEvent{level == "warn" ? LogLevel::warn : level == "error" ? LogLevel::error
                                     : level == "debug" ? LogLevel::debug : LogLevel::info,
                                     j.value("message", "")});
            } else if (kind == "error")
                stream.emit(ErrorEvent{error_code_from_wire(j.value("code", "")), j.value("message", ""),
                                       j.value("recoverable", false), j.value("hint", "")});
            else if (kind == "prompt")
                stream.emit(ErrorEvent{ErrorCode::InvalidInput, j.value("question", ""), true,
                                       "answer it with -y"});
        });
    if (rc && answered >= 0) rc = answered;
    return rc;
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
    // A name already taken is not a usage mistake: it says so, and what to do
    // (U17: it used to read "control registry is invalid or already claims
    // this name" followed by the usage line).
    auto taken_or_usage = [&](std::string_view detail) {
        if (detail.ends_with("already exists")) {
            const auto first = detail.find('\'');
            const auto last = detail.rfind('\'');
            const auto taken = first != std::string_view::npos && last > first
                ? std::string(detail.substr(first + 1, last - first - 1)) : std::string{};
            stream.emit(ErrorEvent{
                .code = ErrorCode::InvalidInput,
                .message = std::string(detail),
                .recoverable = true,
                .hint = std::format("remove it first (xlings subos remove {}), or choose another name", taken),
            });
            return;
        }
        usageError(detail);
    };

    // A SubOS on another carrier: its xlings answers there (part 3 §5).
    static const std::set<std::string, std::less<>> kNamed{
        "use", "exec", "start", "stop", "remove", "info", "config", "status", "doctor", "cp", "log",
        "report", "requests", "approve", "deny", "rollback", "boot", "__boot", "export", "__export", "diff", "pack"};
    if (kNamed.contains(sub)) {
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--") break;
            if (a.empty() || a[0] == '-' || !fs::exists(Config::subos_dir(a))) continue;
            const auto read = carrier_of_(a);
            if (!read) {
                stream.emit(ErrorEvent{.code = ErrorCode::InvalidInput, .message = read.error(), .recoverable = true,
                                       .hint = "xlings self doctor"});
                return 1;
            }
            const auto& where = *read;
            if (where.carrier == "local") break;
            std::vector<std::string> args{"subos", sub};
            for (int k = 3; k < argc; ++k) args.emplace_back(argv[k]);
            if (yesGiven) args.push_back("-y");
            const int rc = forward_to_carrier_(where, args, stream);
            if (sub == "remove" && rc == 0) {
                // There it is gone; here only its name and record remain.
                std::error_code ec;
                fs::remove(home_view().instance_file(a), ec);
                fs::remove(home_view().config_dir(a), ec);
                fs::remove(Config::subos_dir(a), ec);
                (void)update_home_config(Config::paths().homeDir, [&](nlohmann::json& json) {
                    if (!json.contains("subos") || !json["subos"].is_object()) return false;
                    return json["subos"].erase(a) > 0;
                });
            }
            return rc;
        }
    }

    // A Luban system in a prefix domain is answered where it lives (part 3
    // §3.4): the same command runs again in its domain's namespace, where
    // /xlings is the home and the system an ordinary root. A file it writes
    // for the owner goes through the namespace's one output binding. Asked
    // through the interface (luban, the window), what it says comes back as
    // events; at a terminal it speaks there itself, and asks there.
    if (static const std::set<std::string_view> routed{"upgrade", "boot", "__boot", "export", "__export"};
        routed.contains(sub) && argc > 3 && argv[3][0] != '-') {
        auto scope = xlings::home::domain_producer::read_scope(Config::paths().homeDir, argv[3]);
        if (!scope) { usageError(scope.error()); return 1; }
        if (*scope) {
            std::vector<std::string> words;
            for (int i = 2; i < argc; ++i) words.emplace_back(argv[i]);
            std::optional<xlings::home::domain_producer::OutputBinding> output;
            static const std::set<std::string_view> outputs{"--rootfs", "--tar", "--disk", "--iso", "--drive", "--qcow2"};
            for (std::size_t i = 2; (sub == "export" || sub == "__export") && i + 1 < words.size(); ++i) {
                if (!outputs.contains(words[i])) continue;
                const fs::path file = fs::absolute(words[i + 1]);
                const bool dir = file.filename().empty();
                const auto leaf = dir ? file.parent_path().filename() : file.filename();
                output = xlings::home::domain_producer::OutputBinding{
                    dir ? file.parent_path().parent_path() : file.parent_path(), "/run/xlings-domain-output"};
                words[i + 1] = (fs::path("/run/xlings-domain-output") / leaf).string() + (dir ? "/" : "");
                break;
            }
            auto rc = run_domain_relayed_((**scope).domain, words, output, yesGiven, stream);
            if (!rc) {
                stream.emit(ErrorEvent{.code = ErrorCode::Internal, .message = rc.error(), .recoverable = true});
                return 1;
            }
            return *rc;
        }
    }

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
        // --sandbox[=dev|private|locked]: how isolated the instance is, declared
        // once here -- every later `use` / `exec` enters it that way without
        // being told (design §7). Bare `--sandbox` is dev.
        std::optional<policy::Preset> declared;
        // --rootfs: the instance can be presented as `/` (design part 2 §3.1).
        bool rootfs = false;
        std::string domain;
        bool domainIfPossible = false;
        // --carrier / --abi (design part 3 §5.6): where it runs. Default:
        // this machine's kernel unless what it is needs another one.
        std::string requestedCarrier, abi = "native";
        // --proxy <URL> (Luban design §C3): its network goes only through
        // this SOCKS5h proxy, from the first time it is entered.
        std::string proxy;
        bool dryRun = false;                    // --dry-run: what --from would make, nothing made
        std::vector<std::string> forwardArgs;   // what the carrier's xlings is asked
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            // Everything this argument took -- an option and its value --
            // goes to the carrier as it was given, except where it runs.
            struct Forward {
                std::vector<std::string>& out;
                char** argv;
                int first;
                const int& last;
                bool keep { true };
                ~Forward() {
                    if (keep)
                        for (int k = first; k <= last; ++k) out.emplace_back(argv[k]);
                }
            } forward{forwardArgs, argv, i, i};
            if (a == "--proxy" && i + 1 < argc) { proxy = argv[++i]; continue; }
            if (a.starts_with("--proxy=")) { proxy = a.substr(8); continue; }
            if ((a == "--carrier" || a == "--abi") && i + 1 < argc) {
                (a == "--carrier" ? requestedCarrier : abi) = argv[++i];
                forward.keep = false;
                continue;
            }
            if (a.starts_with("--carrier=")) { requestedCarrier = a.substr(10); forward.keep = false; continue; }
            if (a.starts_with("--abi=")) { abi = a.substr(6); forward.keep = false; continue; }
            if (a == "--rootfs") { rootfs = true; continue; }
            if (a == "--dry-run") { dryRun = true; continue; }
            if (a == "--domain" || a == "--domain-if-possible") {
                if (i + 1 >= argc || !domain.empty()) { usageError(std::string(a) + " expects one absolute home path"); return 1; }
                domainIfPossible = a == "--domain-if-possible";
                domain = argv[++i];
                if (domain.empty() || domain[0] != '/') { usageError("--domain expects an absolute home path"); return 1; }
                continue;
            }
            if (a == "--sandbox" || a.starts_with("--sandbox=")) {
                std::string v = a == "--sandbox" ? std::string("dev") : a.substr(10);
                if (a == "--sandbox" && i + 1 < argc) {
                    const std::string_view next = argv[i + 1];
                    if (policy::preset_from_string(next)) v = argv[++i];
                    else if (next == "bwrap" || next == "proot" || next == "landlock") v = argv[++i];
                }
                if (v == "bwrap" || v == "proot" || v == "landlock") {
                    usageError(std::format("--sandbox {}: a backend is chosen when the instance is "
                                           "entered (`subos use <name> --sandbox {}`); `new` declares "
                                           "dev, private or locked", v, v));
                    return 1;
                }
                declared = policy::preset_from_string(v);
                if (!declared || *declared == policy::Preset::Legacy) {
                    usageError("--sandbox expects dev, private or locked");
                    return 1;
                }
            }
            else if (a == "--runtime" && i + 1 < argc) {
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
        if (dryRun) {
            if (fromSpec.find_first_of(":@") == std::string::npos) {
                usageError("--dry-run reads a template: --from <namespace:name[@version]>");
                return 1;
            }
            return plan_from_(fromSpec, stream);
        }
        if (abi != "native" && abi != "linux") { usageError("--abi expects native or linux"); return 1; }
        const auto platformName = std::string(caps::platform_name());
        if (abi == "linux" && platformName == "linux") abi = "native";
        auto choice = carrier::choose(platformName,
            carrier::Want{.requested = requestedCarrier, .abi = abi, .root = rootfs}, probe_carrier_);
        if (!choice && platformName == "windows" && choice.error().route.starts_with(carrier::wsl2::kSetupCommand)) {
            // A Linux SubOS on Windows: the carrier's one-time setup, asked
            // here rather than handed back as a command to type.
            const int ready = sandbox::prepare_carrier_host(yesGiven, stream);
            if (ready == 3) {
                log::info("restart Windows once, then run this again: xlings subos new {}", name);
                return 3;
            }
            if (ready != 0) return ready;
            choice = carrier::choose(platformName,
                carrier::Want{.requested = requestedCarrier, .abi = abi, .root = rootfs}, probe_carrier_);
        }
        if (!choice) {
            stream.emit(ErrorEvent{.code = ErrorCode::NotFound, .message = choice.error().reason,
                                   .recoverable = true, .hint = choice.error().route});
            return 125;
        }
        if (choice->carrier != "local") {
            if (fs::exists(Config::subos_dir(name))) { usageError("subos '" + name + "' already exists"); return 1; }
            std::vector<std::string> args{"subos", "new"};
            args.insert(args.end(), forwardArgs.begin(), forwardArgs.end());
            if (yesGiven) args.push_back("-y");
            const CarrierBinding_ binding{choice->carrier, abi};
            log::info("'{}' runs on the {} carrier ({})", name, choice->carrier, choice->why);
            const int rc = forward_to_carrier_(binding, args, stream);
            if (rc != 0) return rc;
            // Here: the name, and where it lives. Its content is there.
            std::error_code ec;
            fs::create_directories(Config::subos_dir(name), ec);
            if (auto recorded = record_carrier_(name, binding); !recorded) {
                stream.emit(ErrorEvent{.code = ErrorCode::Internal, .message = recorded.error(), .recoverable = true});
                return 1;
            }
            auto registered = update_home_config(Config::paths().homeDir, [&](nlohmann::json& json) {
                if (!json.contains("subos") || !json["subos"].is_object()) json["subos"] = nlohmann::json::object();
                json["subos"][name] = nlohmann::json{{"dir", ""}};
                return true;
            });
            if (!registered) {
                stream.emit(ErrorEvent{.code = ErrorCode::Internal, .message = registered.error(), .recoverable = true});
                return 1;
            }
            return 0;
        }
        if (!fromSpec.empty() && fromSpec.find_first_of(":@") == std::string::npos) {
            auto source = xlings::home::domain_producer::read_scope(Config::paths().homeDir, fromSpec);
            if (!source) { usageError(source.error()); return 1; }
            if (*source) {
                const auto& sourceDomain = (**source).domain;
                if (!domain.empty() && fs::path(domain).lexically_normal() != sourceDomain.logicalHome) {
                    usageError("a different prefix domain requires rebuilding from package coordinates; an instance's absolute payload paths cannot be copied across prefixes");
                    return 1;
                }
                domain = sourceDomain.logicalHome.string();
                const auto fresh = preflight_domain_at_creation_(name, true, domain, {});
                if (!fresh) { taken_or_usage(fresh.error()); return 1; }
                std::vector<std::string> arguments{"subos", "new"};
                for (int i = 3; i < argc; ++i) {
                    if (std::string_view(argv[i]) == "--domain") { ++i; continue; }
                    arguments.emplace_back(argv[i]);
                }
                if (!rootfs) arguments.push_back("--rootfs");
                if (yesGiven) arguments.push_back("-y");
                auto forked = produce_domain_at_creation_(name, true, domain, arguments, stream);
                if (!forked) { usageError("private domain fork did not select its namespace producer"); return 1; }
                return *forked;
            }
        }
        const auto domain_ready = preflight_domain_at_creation_(name, rootfs, domain, fromSpec);
        if (!domain_ready) { taken_or_usage(domain_ready.error()); return 1; }
        if (!domain.empty()) {
            std::vector<std::string> producerArgs{"subos", "new"};
            for (int i = 3; i < argc; ++i) {
                if (std::string_view(argv[i]) == "--domain" || std::string_view(argv[i]) == "--domain-if-possible") { ++i; continue; }
                producerArgs.emplace_back(argv[i]);
            }
            bool fellBack = false;
            if (auto result = produce_domain_at_creation_(name, rootfs, domain, producerArgs, stream,
                                                          domainIfPossible ? &fellBack : nullptr))
                return *result;
            // This host cannot build in a private namespace: in this home's
            // prefix instead, as before, said once (--domain-if-possible).
            if (fellBack) domain.clear();
        }
        // The same command again, for an environment a failure left
        // unfinished: it finishes it -- the edition's packages that are not
        // there yet, then its record. Only the edition it was made from; any
        // other is a different request and `already exists` stays the answer.
        if (!fromSpec.empty() && fs::exists(Config::subos_dir(name))) {
            const auto file = home_view().instance_file(name);
            std::error_code fec;
            auto instance = fs::is_regular_file(file, fec)
                ? nlohmann::json::parse(platform::read_file_to_string(file.string()), nullptr, false)
                : nlohmann::json();
            if (instance.is_object() && instance.value("making", std::string{}) == fromSpec) {
                log::info("finishing '{}' ({})", name, fromSpec);
                if (auto r = declare_root_at_creation_(name, true, fromSpec, stream, domain); r != 0) return r;
                nlohmann::json payload{{"name", name}, {"from", fromSpec},
                                       {"dir", Config::subos_dir(name).string()}};
                stream.emit(DataEvent{"subos_forked", payload.dump()});
                return 0;
            }
            if (instance.is_object() && instance.contains("making")) {
                usageError(std::format("'{}' was being made from {}; `xlings subos new {} --from {}` "
                                       "finishes it, or remove it first", name,
                                       instance.value("making", std::string{}), name,
                                       instance.value("making", std::string{})));
                return 1;
            }
        }
        // A root is checked for before it costs a download (`--rootfs`; a
        // template that declares one is checked once it is read, in new_from).
        if (rootfs && fromSpec.empty())
            if (auto r = sandbox::prepare_root_host(yesGiven, stream); r != 0) return r;
        // "created" is said once, last: after the root and what it declares
        // are in place. Said first, it was followed by the template chain and
        // the package plan, and a failure there read as a SubOS that exists.
        std::vector<Event> report;
        EventStream made;
        made.set_can_confirm(stream.can_confirm());
        made.set_can_select(stream.interactive());
        made.on_event([&](const Event& e) {
            if (const auto* d = std::get_if<DataEvent>(&e);
                d && (d->kind == "subos_created" || d->kind == "subos_forked")) {
                report.push_back(e);
                return;
            }
            stream.emit(e);
        });
        const int rc = !fromSpec.empty()
            ? new_from(name, {}, storage, imageSize, fromSpec, runtime, yesGiven, made)
            : create(name, {}, storage, imageSize, runtime, yesGiven, "-y", made);
        if (rc != 0) return rc;
        if (auto r = declare_isolation_at_creation_(name, declared, fromSpec, stream); r != 0) return r;
        if (auto r = declare_root_at_creation_(name, rootfs, fromSpec, stream, domain); r != 0) return r;
        // The policy its template declares, and the proxy asked for here:
        // selected and locked before the first entry, through the same code
        // as `subos config` -- so a workspace that is private is private from
        // the start, with no window between "made" and "made private".
        if (auto r = apply_declared_policy_(name, proxy, stream, usageError); r != 0) return r;
        for (auto& e : report) stream.emit(std::move(e));
        if (!requestedCarrier.empty() || abi != "native") {
            if (auto recorded = record_carrier_(name, {"local", abi}); !recorded) {
                stream.emit(ErrorEvent{.code = ErrorCode::Internal, .message = recorded.error(), .recoverable = true});
                return 1;
            }
        }
        return 0;
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
        std::string persist;
        std::vector<std::pair<int, std::string>> normalizedMounts;
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
                     a.starts_with("--sandbox") || a.starts_with("--net") || a.starts_with("--proxy") || a.starts_with("--observe") || a.starts_with("--allow")
                     || a.starts_with("--fetch") || a.starts_with("--publish") || a.starts_with("--mount")
                     || a == "--no-degrade") {
                auto r = parse_isolation_flag_(a, i, argc, argv, iso, err);
                if (r < 0) { usageError(err); return 1; }
                if (r == 0) { usageError("unknown option for `xlings subos use`: " + a); return 1; }
                if (a == "--mount" || a.starts_with("--mount=")) {
                    const auto& mount = iso.overrides.mounts.back();
                    auto spec = mount.src + (mount.dst.empty() ? "" : ":" + mount.dst);
                    if (mount.mode_given) spec += mount.rw ? ":rw" : ":ro";
                    normalizedMounts.emplace_back(i, a == "--mount" ? spec : "--mount=" + spec);
                }
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
            else if (a == "--persist" && i + 1 < argc) {
                persist = argv[++i];
            }
            else if (a == "--user" && i + 1 < argc) {
                // Who logs in to a root this once (part 3 §6.1).
                platform::set_env_variable("XLINGS_ROOT_LOGIN", argv[++i]);
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

        std::vector<std::string> domainArgs{"subos", "use"};
        domainArgs.insert(domainArgs.end(), argv + 3, argv + argc);
        for (const auto& [index, spec] : normalizedMounts) domainArgs[index - 1] = spec;
        if (auto result = run_domain_operation_(name, domainArgs, stream)) return *result;

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
        // Wherever there is a session host (Linux, macOS); on Windows --keep /
        // --ttl change nothing and the shell below is the whole entry.
        if (!platform::is_windows && sandbox && !no_keep && (keep_forever || ttl_sec > 0)
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
                               iso.preset, iso.overrides, persist);
    }
    if (sub == "list") {
        bool all = false;
        for (int i = 3; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--all") all = true;
            else { usageError(std::format("unknown option '{}' (xlings subos list [--all])", argv[i])); return 1; }
        }
        return run_list_(stream, all);
    }
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
        std::vector<std::string> domainArgs{"subos", "stop", target};
        if (argc > 4) domainArgs.insert(domainArgs.end(), argv + 4, argv + argc);
        if (auto result = run_domain_operation_(target, domainArgs, stream)) return *result;
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
    if (sub == "ps") {
        std::string name;
        bool json = false, valid = true;
        for (int i = 3; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--json") json = true;
            else if (!arg.empty() && arg[0] != '-' && name.empty()) name = arg;
            else valid = false;
        }
        if (name.empty()) return run_ps_(argc, argv, stream);
        if (!valid) { usageError("usage: xlings subos ps [name] [--json]"); return 1; }
        auto resolved = resolve_use_name_(name, stream);
        if (resolved.selected.empty()) return resolved.exitCode;
        name = resolved.selected;
        std::vector<std::string> domainArgs{"subos", "ps", name};
        if (json) domainArgs.push_back("--json");
        if (auto result = run_domain_operation_(name, domainArgs, stream)) return *result;
        const auto live = session::find(home_view(), name);
        if (json) {
            if (live) std::println(std::cout, "{}", session::to_json(*live).dump());
        } else if (!live) log::info("'{}' has no running session", name);
        else {
            nlohmann::json table{{"headers", {"SUBOS", "SESSION", "BACKEND", "STARTED", "PID", "MODE"}},
                {"rows", nlohmann::json::array({{live->instance, live->id, live->backend, live->started,
                    std::to_string(live->supervisor_pid), live->detached ? std::format("detached, ttl {}s", live->ttl) : "attached"}})}};
            stream.emit(DataEvent{"table", table.dump()});
        }
        return 0;
    }
    if (sub == "exec") return run_exec_(argc, argv, stream);
    if (sub == "report") return run_report_(argc, argv, stream, usageError);
    if (sub == "requests" || sub == "approve" || sub == "deny")
        return run_requests_(sub, argc, argv, stream, usageError);
    if (sub == "config") return run_config_(argc, argv, stream, usageError);
    if (sub == "status") return run_status_(argc, argv, stream, usageError);
    if (sub == "doctor") return run_doctor_(argc, argv, stream, usageError);
    if (sub == "start") return run_start_(argc, argv, stream, usageError);
    if (sub == "cp") return run_cp_(argc, argv, stream, usageError);
    if (sub == "log") {
        std::string name;
        bool valid = true;
        for (int i = 3; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--json" || arg == "-f" || arg == "--follow") continue;
            if (arg == "--kind" || arg == "--session" || arg == "-n" || arg == "--lines") {
                if (++i >= argc) { valid = false; break; }
                if (arg == "-n" || arg == "--lines") {
                    try { (void)std::stoul(argv[i]); } catch (...) { valid = false; break; }
                }
            } else if (!arg.empty() && arg[0] != '-' && name.empty()) name = arg;
            else { valid = false; break; }
        }
        if (valid && !name.empty()) {
            std::vector<std::string> domainArgs{"subos", "log"};
            domainArgs.insert(domainArgs.end(), argv + 3, argv + argc);
            if (auto result = run_domain_operation_(name, domainArgs, stream)) return *result;
        }
        return run_log_(argc, argv, stream, usageError);
    }
    if (sub == "rollback") return run_rollback_(argc, argv, stream, usageError);
    if (sub == "upgrade") return run_upgrade_(argc, argv, stream, usageError, yesGiven);
    // A machine's boot entries and the images a machine boots are Luban's
    // (design §2): `luban boot`, `luban export`. Their implementation is here,
    // reached by luban through the plumbing names `subos __boot` /
    // `subos __export`; the user-facing spellings say where they went.
    // A machine's boot entries and the images a machine boots are Luban's
    // (design §2): `luban boot`, `luban export`, which reach this
    // implementation through the plumbing names (cli.cpp says where the
    // user-facing spellings went).
    if (sub == "__boot") return run_boot_(argc, argv, stream, usageError);
    if (sub == "__check") return run_check_(argc, argv, stream, usageError);
    if (sub == "__export" || sub == "export") return run_export_(argc, argv, stream, usageError);
    if (sub == "diff") return run_diff_(argc, argv, stream, usageError);
    if (sub == "pack") return run_pack_(argc, argv, stream, usageError);

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
        std::vector<std::string> domainArgs{"subos", "runtime", binding, target};
        if (argc > 5) domainArgs.insert(domainArgs.end(), argv + 5, argv + argc);
        if (auto result = run_domain_operation_(target, domainArgs, stream)) return *result;
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
