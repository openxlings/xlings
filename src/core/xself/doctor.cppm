export module xlings.core.xself.doctor;

import std;
import xlings.core.xself.init;   // create_shim, LinkResult
// Cross-version compat module (legacy alias names + safety predicate
// live under v0_4_8). See compact/xself.cppm.
import xlings.core.xself.compat;

import xlings.core.config;
import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.runtime;
import xlings.core.xvm.types;
import xlings.core.xvm.bindings;
import xlings.core.xvm.db;
import xlings.core.xvm.shim;
import xlings.core.xvm.inspect;
import xlings.core.xvm.lock;
import xlings.core.xself.repair;

namespace xlings::xself {

namespace fs = std::filesystem;

// `xlings self doctor` — verify the consistency of the program-registration
// state across xlings's state layers, and offer to repair the
// metadata-layer drift that's safe to mend in place.
//
// State layers:
//   [L1 workspace]    ws[name] = "<version>"
//   [L2 versions DB]  db[name].versions[<version>].path = "<bindir>"
//   [L3 shim file]    <binDir>/<name>
//   [L4 payload]      vdata.path directory + the actual executable inside it
//
// Checks (mixed scope is intentional, see each item):
//   1. `missing shim` — for every program in the active workspace, its
//      shim file at <binDir>/<name> must exist.  Scope: active versions
//      only (workspace by definition only references the active one per
//      name).
//   2. `orphan shim` — for every program-typed shim under binDir, the
//      active workspace must have a non-empty entry for that name.
//      Scope: active.
//   3. `broken payload` — for every (name, version) entry in the versions
//      DB, the registered payload must resolve to an executable on disk.
//      Scope: ALL versions (active + inactive). Reported now, not lazily,
//      so users get a heads-up before they `xlings use` an inactive
//      version that is actually broken.
//
// `--fix` policy (deliberately conservative):
//   - missing shim   → recreate from the bootstrap binary  (safe, local)
//   - orphan shim    → remove the file                     (safe, local)
//   - broken payload → DO NOTHING.  Print an actionable hint
//                      `xlings install <pkg>@<ver>` that the user can
//                      run themselves.  doctor never deletes payload
//                      metadata or pulls from the network.  Why: an
//                      auto reinstall would silently touch the network
//                      (failure-prone) and rerun install hooks (side
//                      effects); the user is the right party to decide
//                      to do that.  The recipe is just `install` —
//                      installer's xvm-DB shortcut verifies payload
//                      existence on disk now, so re-running the install
//                      hook is automatic when the payload is missing.
//   - alias warning  → not auto-fixed (could be intentional external).
//   - corrupt binding metadata → --reset-metadata only.  Why: it is the one
//                      repair that loses information (the release, its
//                      members and its header assets are discarded and the
//                      entry becomes a standalone version), so it must be
//                      asked for, not inherited from --fix.
export int cmd_doctor(EventStream& stream, bool fix,
                      bool resetMetadata = false) {
    auto& p   = Config::paths();
    auto db   = Config::versions();
    auto ws   = Config::effective_workspace();

#ifdef _WIN32
    constexpr std::string_view shim_ext = ".exe";
    auto xlings_bin = p.homeDir / "bin" / "xlings.exe";
#else
    constexpr std::string_view shim_ext = "";
    auto xlings_bin = p.homeDir / "bin" / "xlings";
#endif
    if (!fs::exists(xlings_bin)) {
        xlings_bin = p.homeDir / "xlings";
    }

    auto shim_filename = [&](const std::string& name) {
        std::string fn = name;
        if (!shim_ext.empty() && !fn.ends_with(shim_ext)) fn += shim_ext;
        return fn;
    };

    nlohmann::json fields = nlohmann::json::array();
    auto add_field = [&](std::string_view label, std::string value, bool hl = false) {
        fields.push_back({{"label", std::string(label)},
                          {"value", std::move(value)},
                          {"highlight", hl}});
    };

    int missing  = 0;
    int orphans  = 0;
    int broken   = 0;
    int warnings = 0;
    int healed   = 0;

    // Check 1: every workspace program has its shim.
    for (auto& [name, version] : ws) {
        if (version.empty()) continue;
        auto* vi = xvm::get_vinfo(db, name);
        if (!vi || vi->type != "program") continue;

        auto shim_path = p.binDir / shim_filename(name);
        if (fs::exists(shim_path) || fs::is_symlink(shim_path)) continue;

        ++missing;
        std::string detail = std::format("workspace[{}]={} but {} missing",
                                         name, version,
                                         Config::display_path(shim_path));
        if (fix && fs::exists(xlings_bin)) {
            std::error_code ec;
            fs::create_directories(p.binDir, ec);
            auto r = create_shim(xlings_bin, shim_path);
            if (r != LinkResult::Failed) {
                ++healed;
                detail += " — recreated";
            } else {
                detail += " — recreate failed";
            }
        }
        add_field("✗ missing shim", std::move(detail));
    }

    // Check 2: orphan shims (program shim file present, workspace doesn't
    // know about it). Only consider names that are registered as type
    // "program" in the version DB — random files under binDir aren't ours.
    if (fs::exists(p.binDir)) {
        for (auto& entry : platform::dir_entries(p.binDir)) {
            std::error_code ec;
            if (!entry.is_regular_file(ec) && !entry.is_symlink(ec)) continue;
            auto fname = entry.path().filename().string();
            std::string base = fname;
            if (!shim_ext.empty() && base.ends_with(shim_ext)) {
                base = base.substr(0, base.size() - shim_ext.size());
            }

            auto* vi = xvm::get_vinfo(db, base);
            if (!vi || vi->type != "program") continue;

            auto wit = ws.find(base);
            bool active_present = (wit != ws.end() && !wit->second.empty());
            if (active_present) continue;

            ++orphans;
            std::string detail = std::format(
                "{} exists but workspace has no active version for {}",
                Config::display_path(entry.path()), base);
            if (fix) {
                ec.clear();
                fs::remove(entry.path(), ec);
                if (!ec) {
                    ++healed;
                    detail += " — removed";
                } else {
                    detail += " — remove failed";
                }
            }
            add_field("✗ orphan shim", std::move(detail));
        }
    }

    // COMPAT(0.4.8 → drop in 0.6.0): Check 2.5 — legacy alias shims
    // (xim/xvm/xself/xsubos/xinstall).
    //
    // Names + predicate are owned by xself::compat. Doctor differs from
    // the silent cleanup helper only in that it reports each finding and
    // gates removal on `--fix`. Both share the safety predicate so they
    // agree on what counts as "safe to remove".
    //
    // When the compat module is removed, delete this entire block.
    if (fs::exists(p.binDir)) {
        std::error_code bec;
        auto canonical_bootstrap = fs::weakly_canonical(xlings_bin, bec);
        for (auto alias : compat::v0_4_8::LEGACY_ALIAS_NAMES) {
            auto path = p.binDir / shim_filename(std::string(alias));
            if (!compat::v0_4_8::is_legacy_alias_symlink_to_bootstrap(path,
                    canonical_bootstrap)) continue;

            ++orphans;
            std::string detail = std::format(
                "{} is a leftover symlink from older xlings (alias `{}` "
                "removed in 0.4.8)",
                Config::display_path(path), alias);
            if (fix) {
                std::error_code ec;
                fs::remove(path, ec);
                if (!ec) {
                    ++healed;
                    detail += " — removed";
                } else {
                    detail += " — remove failed";
                }
            }
            add_field("✗ legacy alias shim", std::move(detail));
        }
    }

    // Check 2.6: shim ownership anchoring (0.4.48). Every program-typed
    // shim under this home's binDir must anchor back to THIS home — a shim
    // that anchors elsewhere (or nowhere) would dispatch against a foreign
    // home's versions DB. Warning-only: the usual cause is a hand-copied
    // shim file or a damaged home layout, and the right fix depends on
    // which it is. Scoped to bin dirs physically inside the home: a project
    // subos binDir lives in the project tree, where shims intentionally
    // anchor to the global home via their symlink target (and on Windows,
    // where hardlinks don't carry the target path, via the dispatch-time
    // fallback chain) — not a defect.
    // See .agents/docs/2026-06-04-shim-owner-anchoring-design.md.
    std::error_code relEc;
    auto binRel = fs::relative(p.binDir, p.homeDir, relEc);
    bool binDirInsideHome = !relEc && !binRel.empty()
        && binRel.string().rfind("..", 0) != 0;
    if (binDirInsideHome && fs::exists(p.binDir)) {
        std::error_code hec;
        auto home_canon = fs::weakly_canonical(p.homeDir, hec);
        for (auto& entry : platform::dir_entries(p.binDir)) {
            std::error_code ec;
            if (!entry.is_regular_file(ec) && !entry.is_symlink(ec)) continue;
            auto fname = entry.path().filename().string();
            std::string base = fname;
            if (!shim_ext.empty() && base.ends_with(shim_ext)) {
                base = base.substr(0, base.size() - shim_ext.size());
            }
            if (xvm::is_xlings_binary(base)) continue;
            auto* vi = xvm::get_vinfo(db, base);
            if (!vi || vi->type != "program") continue;

            auto owner = xvm::resolve_owner_home(entry.path());
            std::error_code oec;
            bool anchored = owner
                && fs::weakly_canonical(*owner, oec) == home_canon;
            if (anchored) continue;

            ++warnings;
            add_field("⚠ shim anchor", std::format(
                "{} anchors to {} (expected this home); it will dispatch "
                "against that home's versions DB",
                Config::display_path(entry.path()),
                owner ? Config::display_path(*owner)
                      : std::string("no home (orphan)")));
        }
    }

    // Check 3: payload existence + executability for every (name, version)
    // in the versions DB. Reuses the same `resolve_executable` helper that
    // shim_dispatch uses at runtime so doctor's verdict matches what the
    // user will actually experience when they invoke the shim.
    //
    // Scope: ALL versions (active + inactive) — heads-up before users
    // `xlings use` an inactive version that's already broken.
    //
    // Repair policy: doctor reports broken payloads but never repairs
    // them. Each finding is followed by a copy-pasteable remediation
    // command. See the policy comment on cmd_doctor for the rationale.
    auto home_str = p.homeDir.string();

    // Findings the repair ladder can act on, collected during detection and
    // acted on afterwards. Repairing inline would mutate the database that
    // the rest of detection is still walking.
    std::vector<RepairTask> repairTasks;
    // Findings the repair pass owned and could not resolve. Distinct from
    // `issues`, which also counts things --fix is not responsible for.
    int repairFailed = 0;

    auto report_broken_payload = [&](const std::string& name,
                                     const std::string& version,
                                     std::string detail) {
        ++broken;
        if (fix) {
            repairTasks.push_back(RepairTask{
                .kind    = RepairKind::BrokenPayload,
                .target  = name,
                .version = version,
                .detail  = detail,
            });
        }
        auto wit = ws.find(name);
        bool is_active = (wit != ws.end() && wit->second == version);
        std::string label = is_active ? "✗ broken payload [active]"
                                      : "✗ broken payload";
        add_field(label, std::move(detail));
        // Actionable remediation, exactly as the user should run it.
        // doctor never runs this for them: install touches the network and
        // reruns install hooks, both of which are user decisions.
        // `xlings install <pkg>@<ver>` is sufficient on its own — the
        // installer's xvm-DB shortcut now verifies payload existence
        // before honoring it, so a broken-payload entry triggers a
        // re-run of the install hook automatically.
        add_field("  → run", std::format(
            "xlings install {}@{}", name, version));
    };

    for (auto& [name, vinfo] : db) {
        if (vinfo.type != "program") continue;
        for (auto& [version, vdata] : vinfo.versions) {
            if (vdata.path.empty()) continue;  // type-only stub; nothing to verify

            auto expanded = xvm::expand_path(vdata.path, home_str);
            std::error_code ec;

            // L4: payload directory must exist.
            if (!fs::is_directory(expanded, ec)) {
                report_broken_payload(name, version, std::format(
                    "{}@{} path {} missing", name, version,
                    Config::display_path(expanded)));
                continue;
            }

            // L5: the executable that shim_dispatch would actually exec
            // must resolve. Branch on alias mode to mirror runtime semantics.
            bool alias_mode = !vdata.alias.empty() && !vdata.alias[0].empty();
            if (!alias_mode) {
                auto exe = xvm::resolve_executable(name, vdata.path, home_str);
                if (!exe.empty()) continue;  // OK

                // A name that exists only to anchor a release is not a
                // broken payload. Library-only packages have no program of
                // their own, so their recipe registers the package name with
                // no bindir purely to have something for the libraries to
                // bind to; with `type` unset that entry defaults to
                // "program" and then fails this check forever. On a real
                // installation 31 entries were reported this way, and
                // `xlings install <pkg>@<ver>` -- the hint we printed --
                // cannot fix any of them, because nothing is wrong.
                //
                // Reported, but as what it is. Staying silent would hide the
                // rarer case of a genuine program whose payload directory
                // survived while its executable did not; that entry is also
                // a binding root, so it lands here too and the user still
                // sees the line.
                if (xvm::is_binding_root(db, name, version)) {
                    add_field("ⓘ release anchor", std::format(
                        "{}@{} registers no program of its own; it names the "
                        "release its libraries belong to", name, version));
                    continue;
                }

                report_broken_payload(name, version, std::format(
                    "{}@{} executable '{}' not found in {}",
                    name, version, name, Config::display_path(expanded)));
                continue;
            }

            // Alias mode: best-effort coverage. Parse the first token (the
            // command itself); absolute-path aliases are intentionally
            // external and skipped; relative aliases that don't resolve
            // locally are downgraded to a warning because they MIGHT be
            // system commands found via the runtime PATH.
            //
            // TODO(self-doctor): strengthen alias-mode handling. Known
            // limitations:
            //   - `${XLINGS_HOME}` placeholders in alias_prog aren't
            //     expanded before is_absolute()/resolve_executable() —
            //     rare in practice but a real coverage gap.
            //   - only alias[0] is inspected (matches runtime today; if
            //     multi-element fallback chains ever land they should be
            //     covered too).
            //   - "intentional system command" vs "misconfiguration"
            //     can't be told apart from inside doctor — users see
            //     warning either way. Acceptable for now since false-
            //     positive on alias is bounded by warning severity (no
            //     error, no exit-1, --fix doesn't touch).
            // For now the alias branch is a permissive heuristic: when
            // in doubt we skip rather than emit a false `broken payload`
            // error.
            const auto& alias_cmd = vdata.alias[0];
            auto sp = alias_cmd.find(' ');
            std::string alias_prog = (sp == std::string::npos)
                ? alias_cmd : alias_cmd.substr(0, sp);

            if (fs::path(alias_prog).is_absolute()) continue;

            auto exe = xvm::resolve_executable(alias_prog, vdata.path, home_str);
            if (!exe.empty()) continue;  // resolved within payload — OK

            ++warnings;
            std::string detail = std::format(
                "{}@{} alias '{}' not resolvable in {} (may be a system command)",
                name, version, alias_prog, Config::display_path(expanded));
            add_field("⚠ alias unresolved", std::move(detail));
        }
    }

    // Check 4: the binding state itself.
    //
    // Everything above looks at shims and payloads. None of it can see a
    // release whose members disagree about which release they are, or an
    // active toolchain whose members drifted apart -- and those are exactly
    // the states that make `xlings use` refuse. Without this, a user hitting
    // that refusal has nowhere to look but versions.json.
    //
    // Read-only for now: reporting is what removes the dead end. Repair
    // lands separately, because deactivating a group or dropping metadata
    // is a decision the user should see spelled out before it happens.
    auto bindingFindings = xvm::inspect_binding_state(db, ws);

    // Sysroot ownership. Reported only for destinations a package declares,
    // not for the whole tree: the subos carries the host image, so listing
    // every unmanaged entry would bury the two or three that matter under
    // hundreds that are simply not ours to manage. Drift on a declared
    // destination is the actionable case, and that is what this finds.
    {
        std::vector<xvm::SysrootEntry> entries;
        for (const auto& [target, version] : ws) {
            auto infoIt = db.find(target);
            if (infoIt == db.end()) continue;
            auto dataIt = infoIt->second.versions.find(version);
            if (dataIt == infoIt->second.versions.end()) continue;
            const auto& dst = dataIt->second.fileDst;
            if (dst.empty()) continue;
            const auto abs = p.subosDir / dst;
            std::error_code ec;
            if (!fs::exists(abs, ec) && !fs::is_symlink(abs, ec)) continue;
            xvm::SysrootEntry entry{.path = dst};
            if (fs::is_symlink(abs, ec)) {
                entry.linkTarget = fs::read_symlink(abs, ec).string();
                if (ec) entry.linkTarget.clear();
            }
            entries.push_back(std::move(entry));
        }
        auto ownership = xvm::inspect_sysroot_ownership(
            db, ws, entries, (p.dataDir / "xpkgs").string());
        bindingFindings.insert(bindingFindings.end(),
                               std::make_move_iterator(ownership.begin()),
                               std::make_move_iterator(ownership.end()));
    }

    for (const auto& finding : bindingFindings) {
        const bool notice =
            finding.severity == xvm::BindingSeverity::Notice;
        // A notice describes state the upgrade inherited rather than
        // created, so it does not colour the run red -- same reasoning as
        // the release anchors above. It still prints its remediation.
        if (!notice) ++broken;
        std::string detail = finding.summary;
        if (!finding.target.empty()) {
            detail += std::format(" [{}{}{}]", finding.target,
                                  finding.version.empty() ? "" : "@",
                                  finding.version);
        }
        if (!finding.field.empty()) {
            detail += std::format(" at {}", finding.field);
        }
        detail += std::format(" — {} — {}", finding.code, finding.hint);
        add_field(notice ? "ⓘ binding state" : "✗ binding state",
                  std::move(detail));
    }

    // --fix for the one binding problem that can be repaired without
    // guessing: an active release whose members disagree. Deactivate the
    // whole release and let the user re-select. Choosing a survivor here
    // would mean deciding which member was "right", which is exactly the
    // silent decision that produces incoherent state to begin with.
    //
    // Unresolvable entries are reported but not touched: repairing one means
    // deciding which member was "right". Corrupt metadata is repairable, but
    // only by discarding it, so it takes its own flag (--reset-metadata)
    // rather than riding along on one the user passed for shim repair.
    if (resetMetadata) {
        // Before the deactivation pass: an entry whose group is unreadable
        // makes its release unresolvable, which that pass would otherwise
        // report as a different problem.
        if (auto reset = xvm::plan_metadata_reset(db); !reset.empty()) {
            auto lock = xvm::acquire_state_lock(Config::paths().homeDir);
            if (!lock) {
                add_field("✗ binding state", std::format(
                    "cannot reset binding metadata: {}", lock.error()));
            } else {
                Config::reload_state();
                auto& mutableDb = Config::versions_mut();
                const auto replanned = xvm::plan_metadata_reset(mutableDb);
                const auto cleared =
                    xvm::apply_metadata_reset(mutableDb, replanned);
                if (cleared > 0) {
                    Config::save_versions();
                    healed += static_cast<int>(cleared);
                    for (const auto& entry : replanned.entries) {
                        std::string codes;
                        for (const auto& code : entry.codes) {
                            if (!codes.empty()) codes += ", ";
                            codes += code;
                        }
                        // Name what was discarded. This is the one repair
                        // that loses information, so a bare count would be
                        // the wrong report.
                        add_field("· metadata reset", std::format(
                            "{}@{} dropped its release metadata ({}) and is "
                            "now switchable on its own — reinstall it to "
                            "restore the release",
                            entry.target, entry.version, codes));
                    }
                    db = Config::versions();
                }
            }
        }
    }

    if (fix) {
        // Dangling pairwise edges first: they are repairable without
        // guessing, and leaving one in place keeps the release it names
        // unresolvable, which would make the deactivation pass below see a
        // problem that is really this one.
        if (auto pruning = xvm::plan_dangling_edge_pruning(db);
            !pruning.empty()) {
            auto lock = xvm::acquire_state_lock(Config::paths().homeDir);
            if (!lock) {
                add_field("✗ binding state", std::format(
                    "cannot drop dangling binding edges: {}", lock.error()));
            } else {
                Config::reload_state();
                auto& mutableDb = Config::versions_mut();
                const auto replanned =
                    xvm::plan_dangling_edge_pruning(mutableDb);
                const auto dropped =
                    xvm::apply_dangling_edge_pruning(mutableDb, replanned);
                if (dropped > 0) {
                    Config::save_versions();
                    healed += static_cast<int>(dropped);
                    for (const auto& edge : replanned.edges) {
                        add_field("· edge dropped", std::format(
                            "{}@{} no longer points at unregistered {}@{}",
                            edge.target, edge.version,
                            edge.peerTarget, edge.peerVersion));
                    }
                    // The database changed underneath the findings above.
                    db = Config::versions();
                }
            }
        }

        auto plan = xvm::plan_incoherent_deactivation(db, ws);
        if (!plan.targets.empty()) {
            auto lock = xvm::acquire_state_lock(Config::paths().homeDir);
            if (!lock) {
                add_field("✗ binding state", std::format(
                    "cannot deactivate incoherent releases: {}", lock.error()));
            } else {
                Config::reload_state();
                auto& mutableWs = Config::workspace_mut();
                std::size_t dropped = 0;
                for (const auto& [target, label] : plan.targets) {
                    if (mutableWs.erase(target) == 0) continue;
                    ++dropped;
                    add_field("· deactivated", std::format(
                        "{} (was part of {}) — run `xlings use {} <version>` "
                        "to select a release", target, label, target));
                }
                if (dropped > 0) {
                    Config::save_workspace();
                    healed += static_cast<int>(dropped);
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // Repair pass.
    //
    // `--fix` used to stop at the shim layer and print a copy-pasteable
    // `xlings install` for every broken payload. On a home carried over from
    // an older client that is not a handful of lines -- 0.4.69 records a
    // headers-only package as one program-typed entry with a payload path and
    // no executable, so every such package is reported, and the user has to
    // run the command by hand for each one. Measured on the upgrade
    // simulation (PR #434): the printed remedy IS the cure, because
    // re-running install re-runs config() and re-registers the package in the
    // current form. Doing it for them is the whole point of --fix.
    //
    // See .agents/docs/2026-07-28-self-repair-design.md for the ladder.
    if (fix && !repairTasks.empty()) {
        const CommandRunner run = [](const std::string& cmd) {
            return platform::exec(cmd);
        };

        // A finding names an xvm TARGET; the ladder installs a PACKAGE, and
        // the two are not the same thing. A broken llvm release reports
        // `ar@22.1.8`, `clang@22.1.8`, `cc@22.1.8` and forty more -- none of
        // which is installable, because they are programs the llvm package
        // registers. Nor is the binding ROOT reliably the package: gcc's root
        // target is `xim-gnu-gcc`, and `xlings install xim-gnu-gcc` is not a
        // thing.
        //
        // So the owner is looked for, in descending order of confidence, and
        // each candidate is confirmed against the index before it is used.
        // Nothing is repaired on a guess.
        std::map<std::pair<std::string, std::string>, bool> probed;
        auto resolves = [&](const std::string& name, const std::string& ver) {
            auto key = std::pair{name, ver};
            auto it = probed.find(key);
            if (it != probed.end()) return it->second;
            bool ok = probe_reinstallable(name, ver, run);
            probed.emplace(key, ok);
            return ok;
        };

        auto owning_package = [&](const std::string& target,
                                  const std::string& version)
            -> std::optional<std::pair<std::string, std::string>> {
            // 1. Recorded provider. Present on anything a current client
            //    registered, and authoritative when it is.
            if (const auto* vi = xvm::get_vinfo(db, target)) {
                if (auto it = vi->versions.find(version);
                    it != vi->versions.end() && it->second.bindingGroup) {
                    const auto& g = *it->second.bindingGroup;
                    if (resolves(g.provider, g.providerVersion)) {
                        return std::pair{g.provider, g.providerVersion};
                    }
                }
            }
            // 2. The target itself. This is the case that matters for a
            //    0.4.69 home: its owner-less anchor entries ARE package-named
            //    (`llvm@20.1.7`, `linux-headers@5.11.1`, `gcc@15.1.0`).
            if (resolves(target, version)) return std::pair{target, version};
            // 3. A binding root reachable from here, for a member whose own
            //    name means nothing to the index.
            if (auto sel = xvm::resolve_binding_selection(db, target, version)) {
                for (const auto& [m, v] : sel->members) {
                    if (m == target) continue;
                    if (!xvm::is_binding_root(db, m, v)) continue;
                    if (resolves(m, v)) return std::pair{m, v};
                }
            }
            return std::nullopt;
        };

        // Collapse the findings onto their owners: one install per release,
        // not one per program in it.
        std::map<std::pair<std::string, std::string>,
                 std::vector<RepairTask>> byOwner;
        std::vector<RepairTask> unowned;
        for (auto& task : repairTasks) {
            if (auto owner = owning_package(task.target, task.version)) {
                byOwner[*owner].push_back(task);
            } else {
                unowned.push_back(task);
            }
        }

        repairFailed += static_cast<int>(unowned.size());
        for (const auto& task : unowned) {
            add_field("✗ repair skipped", std::format(
                "{}@{} — no package in the index provides this entry; "
                "removing it could not be undone",
                task.target, task.version));
        }

        std::vector<std::pair<RepairTask, RepairResult>> outcomes;
        for (const auto& [owner, covered] : byOwner) {
            RepairTask task{
                .kind          = RepairKind::BrokenPayload,
                .target        = owner.first,
                .version       = owner.second,
                .detail        = std::format("{} broken entr{}",
                                             covered.size(),
                                             covered.size() == 1 ? "y" : "ies"),
                .reinstallable = true,   // confirmed by the probe above
            };
            auto result = repair_one(task, RepairPolicy{}, run);
            // Attribute the outcome to every finding the owner covers, so the
            // re-detect below checks the entries the user was shown.
            for (const auto& c : covered) outcomes.emplace_back(c, result);
        }

        // Re-detect rather than believe the ladder.
        //
        // A rung reporting success while the finding survives is this
        // codebase's recurring shape, and here it would be worse than usual:
        // `healed` feeds the exit code, so a lying rung turns a still-broken
        // home into `status OK`. The database is reloaded from disk and each
        // repaired entry re-checked, so "healed" means the finding is gone,
        // not that a subprocess exited 0.
        //
        // reload_state() first, and not as a precaution: the repairs ran in
        // SUBPROCESSES that wrote the state file, while this process still
        // holds the copy it read at startup. Without the reload, a cure that
        // is purely a re-registration -- which is what the ladder mostly does
        // -- reads as a failure, and only repairs that happened to restore a
        // directory on disk appear to work, because that branch stats the
        // filesystem. Measured on a real 0.4.69 home: 55 healed and one false
        // failure, the single entry whose cure was metadata-only.
        Config::reload_state();
        const auto after = Config::versions();
        for (const auto& [task, result] : outcomes) {
            const auto* vi = xvm::get_vinfo(after, task.target);
            const xvm::VData* vd = nullptr;
            if (vi) {
                if (auto it = vi->versions.find(task.version);
                    it != vi->versions.end()) {
                    vd = &it->second;
                }
            }

            bool cured = false;
            if (vd) {
                // Two ways to be cured, matching the two ways to be broken:
                // the payload resolves again, or re-registration made the
                // entry recognisable as a release anchor (a package that
                // ships no program of its own was never broken).
                cured = !xvm::resolve_executable(
                             task.target, vd->path, home_str).empty()
                     || xvm::is_binding_root(after, task.target, task.version);
            } else {
                // The entry is gone. Only a cure if the ladder took it out on
                // purpose and could not put it back -- which it reports as a
                // failure, not a cure.
                cured = false;
            }

            if (cured) {
                ++healed;
                // Not printed per entry: one repaired release accounts for
                // dozens of findings, and listing them all would bury the
                // failures. The count goes in the summary.
            } else {
                ++repairFailed;
                add_field("✗ repair failed", std::format(
                    "{}@{}{}{}", task.target, task.version,
                    result.note.empty() ? "" : " — ", result.note));
            }
        }
    }

    int issues = missing + orphans + broken;
    if (issues == 0 && warnings == 0) {
        add_field("status",
                  "OK — workspace, shims, and payloads are all consistent",
                  true);
    } else {
        if (missing  > 0) add_field("missing shims",   std::to_string(missing));
        if (orphans  > 0) add_field("orphan shims",    std::to_string(orphans));
        if (broken   > 0) add_field("broken payloads", std::to_string(broken));
        if (warnings > 0) add_field("warnings",        std::to_string(warnings));
        if (fix) {
            if (healed > 0) add_field("healed", std::to_string(healed), true);
            if (broken > healed) add_field("hint",
                "some payloads could not be repaired — see the reasons above",
                true);
        } else {
            if (missing > 0 || orphans > 0)
                add_field("hint", "rerun with `--fix` to repair shim-layer issues", true);
            if (broken > 0)
                add_field("hint", "broken payloads: run the listed `xlings install` commands to repair", true);
        }
    }

    // Exit non-zero only when issues remain after the (optional) fix pass.
    int unresolved = issues - (fix ? healed : 0);

    // Stamp the home with the client that just checked it.
    //
    // `.xlings.json:version` records which xlings set the home up. Only
    // `self install` ever wrote it, so `self update` -- which installs
    // xlings@latest as a package -- left it reading the old version forever.
    // That made the field useless for the one thing it is shaped for, which
    // is telling a user their packages predate their client.
    //
    // Stamped here it becomes the migration marker: the hint below appears
    // while the home is behind and stops once a --fix has actually migrated
    // the packages. Stamping on a failed pass would silence the hint while
    // leaving the state it points at.
    //
    // Gated on the REPAIR pass, not on `unresolved`. doctor also reports
    // things --fix is not responsible for -- on a real upgraded home,
    // active-group incoherence across two providers that both claim `cc` and
    // `c++` accounts for 190 findings on its own, measured. Requiring a
    // spotless home would mean the marker never lands and the hint nags
    // forever about a migration that already happened.
    if (fix && repairFailed == 0) {
        Config::record_client_version(std::string(Info::VERSION));
    }

    // The nudge, for the reader who ran plain `doctor` (or whose fix left
    // something behind) on a home an older client set up.
    if (auto hint = migration_hint(Config::recorded_client_version(),
                                   Info::VERSION)) {
        add_field("ⓘ migration", *hint);
    }

    nlohmann::json payload;
    payload["title"]  = "xlings self doctor";
    payload["fields"] = std::move(fields);
    stream.emit(DataEvent{"info_panel", payload.dump()});

    return unresolved == 0 ? 0 : 1;
}

} // namespace xlings::xself
