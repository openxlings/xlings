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
import xlings.core.xvm.db;
import xlings.core.xvm.shim;

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
export int cmd_doctor(EventStream& stream, bool fix) {
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
                                         name, version, shim_path.string());
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
                entry.path().string(), base);
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
                path.string(), alias);
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
                entry.path().string(),
                owner ? owner->string() : "no home (orphan)"));
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

    auto report_broken_payload = [&](const std::string& name,
                                     const std::string& version,
                                     std::string detail) {
        ++broken;
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
                    "{}@{} path {} missing", name, version, expanded));
                continue;
            }

            // L5: the executable that shim_dispatch would actually exec
            // must resolve. Branch on alias mode to mirror runtime semantics.
            bool alias_mode = !vdata.alias.empty() && !vdata.alias[0].empty();
            if (!alias_mode) {
                auto exe = xvm::resolve_executable(name, vdata.path, home_str);
                if (!exe.empty()) continue;  // OK
                report_broken_payload(name, version, std::format(
                    "{}@{} executable '{}' not found in {}",
                    name, version, name, expanded));
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
                name, version, alias_prog, expanded);
            add_field("⚠ alias unresolved", std::move(detail));
        }
    }

    // Note: there is intentionally no --fix loop for broken payloads here.
    // Hints inline above each broken finding tell the user exactly what
    // to run.

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
            if (broken > 0) add_field("hint",
                "broken payloads not auto-fixed — run the listed `xlings install` commands",
                true);
        } else {
            if (missing > 0 || orphans > 0)
                add_field("hint", "rerun with `--fix` to repair shim-layer issues", true);
            if (broken > 0)
                add_field("hint", "broken payloads: run the listed `xlings install` commands to repair", true);
        }
    }

    nlohmann::json payload;
    payload["title"]  = "xlings self doctor";
    payload["fields"] = std::move(fields);
    stream.emit(DataEvent{"info_panel", payload.dump()});

    // Exit non-zero only when issues remain after the (optional) fix pass.
    int unresolved = issues - (fix ? healed : 0);
    return unresolved == 0 ? 0 : 1;
}

} // namespace xlings::xself
