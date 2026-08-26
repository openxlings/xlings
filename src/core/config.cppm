export module xlings.core.config;

import std;

import xlings.libs.json;
import xlings.libs.tinyhttps;
import xlings.core.xvm.types;
import xlings.core.xvm.db;

namespace xlings {

export struct Info {
    static constexpr std::string_view VERSION = "2026.8.27.1";
    static constexpr std::string_view REPO = "https://github.com/openxlings/xlings";
};

// XLINGS_HOME as the PROCESS WAS STARTED with -- what the USER named, not
// what xlings resolved.
//
// main.cpp exports XLINGS_HOME to the home it resolved, on every single run,
// so from that line onward `getenv("XLINGS_HOME")` returns xlings's own answer.
// Anything asking "did the caller name a home?" must read it before that.
//
// Found by an e2e that had never been run in its life: `self install`'s
// conflict guard used getenv() and refused a caller who had explicitly passed
// `env -u XLINGS_HOME` -- xlings had set the variable itself two frames
// earlier. One question, two answerers, and the second one was us.
//
// Captured EXPLICITLY from main.cpp rather than at static-init: the ordering
// argument for static-init is correct but unverifiable by a reader, and a
// guard whose input silently changes meaning is what this whole round is
// about. Absent (never captured, e.g. in a unit test) means no observation,
// so nothing that consumes this fires.
namespace detail_ {
inline std::optional<std::string> ambientHome_;
inline bool ambientHomeCaptured_ { false };
}  // namespace detail_

export void capture_ambient_home_env();

export const std::optional<std::string>& ambient_home_env();

export struct IndexRepo {
    std::string name;
    std::string url;
    std::string artifactBase;  // #377: resolved artifact base URL ("" = git-only)
    std::string source;        // #377: per-repo override: "" | "auto" | "artifact" | "git"
    // #476: pin this repo to one published index snapshot. Empty (or "latest")
    // means automatic routing -- the newest snapshot whose declared client
    // contract this xlings satisfies. A pin is an override for reproducing a
    // state, so it bypasses the contract check but never the sha256 check.
    std::string version;
};

// #377: parse index_repos entries. `artifact` is a flat string or a region
// object {"GLOBAL":..,"CN":..} (same shape as xim.index-base), resolved
// against `mirror` with GLOBAL fallback. `source` optionally overrides the
// global index source for this repo only.
export std::vector<IndexRepo> parse_index_repos_json(const nlohmann::json& json,
                                                     const std::string& mirror);

using MirrorServerMap = std::unordered_map<std::string, std::vector<std::string>>;

// Which subos a process is acting on: a name and the root of its tree.
//
// One value rather than two lookups. Every "which subos" question in the code
// base used to be answered by one of seven spellings, and the two that
// mattered disagreed (see Config::resolve_subos_scope_). Handing back both
// halves together removes the shape of that bug: a caller cannot take the
// name from one resolution and the root from another.
export struct SubosScope {
    std::string name;
    std::filesystem::path root;
};

export enum class ProjectSubosMode {
    None,
    Anonymous,
    Named,
};

export class Config {
public:
    struct PathInfo {
        std::filesystem::path homeDir;      // XLINGS_HOME (~/.xlings)
        std::filesystem::path dataDir;      // $homeDir/data - global shared
        std::filesystem::path subosDir;     // effective subos dir
        std::filesystem::path binDir;       // $subosDir/bin
        std::filesystem::path libDir;       // $subosDir/lib
        std::string           activeSubos;  // effective active subos name
        bool                  selfContained = false;
    };

    // Owner-anchored shim dispatch (0.4.48; see
    // .agents/docs/2026-06-04-shim-owner-anchoring-design.md): inject the
    // dispatch home BEFORE first Config use. main.cpp calls this for shim
    // invocations only — the xlings CLI keeps the env-first resolution.
    // Has no effect once the singleton is constructed.
    static void override_home(const std::filesystem::path& home);

private:
    static std::optional<std::filesystem::path>& home_override_();

    PathInfo paths_;
    std::string mirror_;
    std::string indexBase_;   // xim.index-base override (region-resolved); empty = default xlings-res
    std::string lang_;
    // Frontend preferences. Flat keys next to `mirror`/`lang` because they
    // answer the same class of question ("which source / language / frontend /
    // colours"), and because `lang` is already flat, already in the schema and
    // already asserted flat by an e2e test -- moving it into a nested block to
    // look tidier would cost a compat layer and buy nothing.
    std::string uiMode_;        // "cli" | "tui" | "auto"/"" 
    std::string theme_;         // path reference, or a shipped theme's name
    std::optional<bool> tuiInteractive_;
    std::string globalActiveSubos_ = "default";
    xvm::VersionDB globalVersions_;
    xvm::VersionDB projectVersions_;
    xvm::Workspace globalWorkspace_;
    xvm::Workspace projectWorkspace_;       // from project .xlings.json
    xvm::Workspace projectSubosWorkspace_;  // from project-local subos file
    // Per-subos installed[] sets, paired with the matching workspace map.
    // Only subos files (global subos and project subos) carry installed[];
    // the project manifest's workspace is intent-only and has no
    // corresponding installed map (Plan 3 of the C2 schema design).
    xvm::WorkspaceInstalled globalInstalled_;
    xvm::WorkspaceInstalled projectSubosInstalled_;
    bool hasProjectConfig_ = false;
    std::string activeSubosOverride_;
    bool forceGlobalScope_ = false;
    std::filesystem::path projectDir_;      // directory containing project .xlings.json
    std::vector<IndexRepo> globalIndexRepos_;
    std::vector<IndexRepo> projectIndexRepos_;
    MirrorServerMap globalResourceServers_;
    MirrorServerMap projectResourceServers_;
    ProjectSubosMode projectSubosMode_ = ProjectSubosMode::None;
    std::string projectSubosName_;
    mutable std::mutex resourceServerMutex_;
    mutable std::unordered_map<std::string, std::string> selectedResourceServerCache_;

    static constexpr std::string_view DEFAULT_INDEX_REPO_NAME = "xim";
    static constexpr std::string_view DEFAULT_INDEX_REPO_DIR = "xim-pkgindex";

    static std::vector<IndexRepo> default_global_index_repos_(const std::string& mirror);

    static MirrorServerMap default_resource_servers_();

    // Resolve xim.index-base from a config json. Accepts a flat string or a
    // region object {"GLOBAL":"...","CN":"..."}. Lets a deployment point the
    // index pointer+artifact at a self-hosted server without code changes.
    static std::string resolve_index_base_(const nlohmann::json& json, const std::string& mirror);

    static std::vector<std::string> parse_server_list_(const nlohmann::json& value);

    static void merge_resource_servers_into_(MirrorServerMap& dst, const MirrorServerMap& src);

    static void load_resource_servers_from_json_(const nlohmann::json& json,
                                                 MirrorServerMap& out);

    // Frontend preferences, read from whichever layer is being loaded.
    // Non-static because they land on the instance being populated.
    void load_ui_prefs_from_json_(const nlohmann::json& json);

    // Read a subos `.xlings.json` workspace section. Returns the new
    // SubosWorkspace bundle so callers get both `active` (Workspace) and
    // `installed[]` (WorkspaceInstalled). Legacy string-form values still
    // parse cleanly — installed[] just stays empty until the next save
    // re-emits the file in C2 form.
    static xvm::SubosWorkspace load_workspace_from_file_(const std::filesystem::path& path);

    static std::string load_project_subos_name_(const nlohmann::json& json);

    static void merge_versions_into_(xvm::VersionDB& dst, const xvm::VersionDB& src);

    static void merge_workspace_into_(xvm::Workspace& dst, const xvm::Workspace& src);

    static std::string effective_mirror_name_(std::string_view mirror,
                                              std::string_view fallback);

    static std::vector<std::string> workspace_targets_from_workspace_(const xvm::Workspace& ws);

    [[nodiscard]] std::filesystem::path project_data_dir_() const;

    [[nodiscard]] std::filesystem::path project_home_dir_() const;

    [[nodiscard]] std::filesystem::path project_state_path_() const;

    [[nodiscard]] std::filesystem::path project_manifest_path_() const;

    [[nodiscard]] std::filesystem::path project_subos_dir_() const;

    [[nodiscard]] std::filesystem::path global_subos_dir_() const;

    [[nodiscard]] static std::vector<std::string>
    lookup_resource_servers_in_(const MirrorServerMap& source, std::string_view mirror);

    [[nodiscard]] std::vector<std::string>
    candidate_resource_servers_for_(std::string_view mirror) const;

    // Every resource server worth trying: this region's candidates first, then
    // every other region's, deduplicated.
    //
    // The regional buckets are a routing preference, not a partition of the
    // content -- both hosts mirror the same releases. Treating them as a
    // partition is what turned a mirroring gap into an outage: `xlings-res`
    // publishes to GitHub and then mirrors to GitCode, that second step is
    // manual for large assets (the GitHub runner cannot push them across the
    // border), and until it runs a CN client's candidate list is
    // `{gitcode}` -- one host, no alternative. `2026.7.30.1` shipped with the
    // four tarballs missing from GitCode and CN users got a flat HTTP 404
    // while the same files sat on GitHub, reachable, unlisted.
    //
    // Ordering keeps the preference intact: the cross-region entries are only
    // ever tried after every same-region one has failed, so nobody in CN
    // starts pulling from GitHub while GitCode is serving.
    [[nodiscard]] std::vector<std::string>
    all_resource_servers_for_(std::string_view mirror) const;

    static double probe_resource_server_latency_(const std::string& server);

    [[nodiscard]] std::string selected_resource_server_for_(std::string_view mirror) const;

    // The ONE answer to "which subos is this process acting on".
    //
    // Resolution priority:
    //   1. Project-mode (Named or Anonymous) — strongest, "this directory
    //      asks for subos X" overrides everything else. Unless `-g` was
    //      passed, which is exactly what forceGlobalScope_ means: act on the
    //      home, not on this project.
    //   2. $XLINGS_ACTIVE_SUBOS env var — shell-level override; the shell
    //      profile honors the same priority so PATH and `xpkg install`
    //      target stay in sync.
    //   3. globalActiveSubos_ from ~/.xlings.json — the persistent default
    //      that new shells inherit when no env override is set.
    //
    // Everything that needs a subos goes through here. There used to be two
    // implementations of this question that disagreed: `update_effective_paths_`
    // (which ignored forceGlobalScope_ and was computed once at construction)
    // and `xvm_artifact_subos_dir()` (which honored it and recomputed). They
    // diverged on exactly one case -- `xlings install -g` run inside a project
    // -- and the three paths that matter had picked different ones: install
    // resolved artifacts with the second, while `use` and the removal path
    // read `paths()`, i.e. the first. Installing into one subos and cleaning
    // another is not a bug that can be fixed at a call site; it is what having
    // two authorities means.
    [[nodiscard]] SubosScope resolve_subos_scope_() const;

    void update_effective_paths_();

    Config();

    void load_project_config_from_dir_(const std::filesystem::path& dir);

    void load_project_config_();

    void load_global_versions_from_json_(const nlohmann::json& json);

    void load_global_workspace_();

    // Re-read the mutable state layers from disk.
    //
    // Config loads state during construction, and main.cpp touches it before
    // dispatching a command, so by the time a mutating command starts it is
    // already holding a snapshot taken outside any lock. Serializing writes
    // alone would not prevent a lost update: both processes would still have
    // read the same old state and the second write would erase the first.
    // Commands re-read through here *after* taking the state lock.
    //
    // Deliberately narrow: versions, workspace and project state only.
    // Mirror, language and index-repo resolution stay as they were resolved
    // at startup -- re-deriving those mid-command would change behavior
    // under the user rather than protect it.
    void reload_state_();

    static Config& instance_() {
        static Config inst;
        return inst;
    }

public:
    [[nodiscard]] static std::vector<std::string> workspace_install_targets(const xvm::Workspace& ws);

    [[nodiscard]] static xvm::VersionDB merged_versions(const xvm::VersionDB& globalVersions,
                                                        const xvm::VersionDB& projectVersions);

    [[nodiscard]] static xvm::Workspace merged_workspace(const xvm::Workspace& globalWorkspace,
                                                         const xvm::Workspace& projectWorkspace,
                                                         const xvm::Workspace& projectSubosWorkspace,
                                                         ProjectSubosMode mode);

    // Re-read versions/workspace/project state from disk. Call after taking
    // the state lock so a command operates on what is actually stored rather
    // than on the snapshot taken at process start.
    static void reload_state();

    [[nodiscard]] static const PathInfo& paths();

    // Render a path for a human: anything under the xlings home shows as
    // `@xlings/...` instead of its absolute form.
    //
    // Output is full of these -- doctor lists payload and shim paths, config
    // prints the layout, install and error hints quote destinations -- and an
    // absolute home path is both noise and, in shared logs or issue reports,
    // the user's account name. `@xlings` says the one thing that matters
    // about the prefix: it is inside the installation.
    //
    // Display only. `${XLINGS_HOME}` remains the *storage* placeholder in the
    // version database (see expand_path in xvm/db.cppm); the two must not be
    // confused -- one is expanded on read, this one is never read back.
    //
    // A path outside the home is returned unchanged: substituting a prefix
    // that does not apply would be a lie about where the file is.
    [[nodiscard]] static std::string display_path(
            const std::filesystem::path& p);

    [[nodiscard]] static std::string display_path(const std::string& p);
    [[nodiscard]] static const std::string& mirror();
    [[nodiscard]] static const std::string& lang();

    // "cli" | "tui" | "" (auto). Empty means detect; see ui::resolve.
    [[nodiscard]] static const std::string& ui_mode();

    // A path reference to a theme file, or a shipped theme's bare name.
    // Empty means the built-in default compiled into the binary.
    [[nodiscard]] static const std::string& theme();

    // The theme file `theme()` points at, or empty for the built-in default.
    //
    // A bare name is sugar for `config/themes/<name>.json`; anything with a
    // separator or a suffix is a path, resolved against the project root when
    // it came from a project config and against the home otherwise -- the same
    // rule local index repo sources already use, so a project can ship its own
    // colours exactly the way it can ship its own index.
    [[nodiscard]] static std::filesystem::path resolve_theme_path();

    // The same resolution, applied to a candidate value rather than to the
    // stored one. `xlings config --theme <x>` uses it to refuse a value that
    // resolves to nothing -- and it must be THIS function, not a second
    // lookup, or the setter and the reader can disagree about what a name
    // means and the disagreement only shows up on the next command.
    [[nodiscard]] static std::filesystem::path resolve_theme_path_for(std::string value);

    // nullopt = not configured; the default is ON for terminals (gated by
    // ui::capabilities_of, which knows whether anyone is actually there).
    [[nodiscard]] static std::optional<bool> tui_interactive();

    // One-shot hints, remembered ACROSS RUNS.
    //
    // `xself::print_migration_hint_once` is a `static bool` -- per process, so
    // reusing it for "show this the first time ever" would show it every time.
    // An array rather than a bool per hint: these will not stay singular, and
    // a list means adding one does not change the schema.
    [[nodiscard]] static bool hint_seen(std::string_view id);

    static void mark_hint_seen(std::string_view id);
    [[nodiscard]] static std::vector<std::string> resource_servers(std::string_view mirror = {});
    // Same list, extended with the other regions' servers as a last resort.
    // Use this for download fallbacks; `resource_servers()` remains the
    // "where should this region be pointed" answer used for selection.
    [[nodiscard]] static std::vector<std::string>
    resource_servers_with_cross_region(std::string_view mirror = {});
    [[nodiscard]] static std::string resource_server(std::string_view mirror = {});
    // xim.index-base override (env XLINGS_INDEX_BASE_URL takes precedence in the
    // caller). Empty => default xlings-res raw-pointer + release-artifact path.
    [[nodiscard]] static std::string index_base();

    // Returns BY VALUE -- global and project state are merged into a fresh
    // map, so there is no long-lived object to hand out a reference to.
    //
    // Bind it to a named local before taking any pointer or reference into
    // it. `get_vinfo(Config::versions(), name)` compiles, and returns a
    // pointer into a temporary that dies at the end of that full expression;
    // the next line then reads freed memory. That was a real crash --
    // `xlings remove glibc` SIGSEGV'd on the musl-static release build and
    // threw bad_alloc on a glibc one, reported 2026-07-28 and present since
    // the fallback was written.
    [[nodiscard]] static xvm::VersionDB versions();
    [[nodiscard]] static xvm::VersionDB& versions_mut();
    [[nodiscard]] static const xvm::VersionDB& global_versions();
    [[nodiscard]] static const xvm::VersionDB& project_versions();

    // Effective data dir: project-local if project config exists, otherwise global
    [[nodiscard]] static std::filesystem::path global_data_dir();

    [[nodiscard]] static std::filesystem::path project_data_dir();

    [[nodiscard]] static std::filesystem::path project_home_dir();

    [[nodiscard]] static std::filesystem::path project_state_path();

    [[nodiscard]] static std::filesystem::path project_manifest_path();

    [[nodiscard]] static std::filesystem::path effective_data_dir();

    [[nodiscard]] static const std::vector<IndexRepo>& global_index_repos();

    [[nodiscard]] static const std::vector<IndexRepo>& project_index_repos();

    [[nodiscard]] static const std::vector<IndexRepo>& index_repos();

    [[nodiscard]] static std::filesystem::path repo_dir_for(const IndexRepo& repo,
                                                            bool projectScope);

    [[nodiscard]] static std::filesystem::path resolve_repo_source(const IndexRepo& repo,
                                                                   bool projectScope);

    [[nodiscard]] static bool is_local_repo_source(const IndexRepo& repo,
                                                   bool projectScope);

    [[nodiscard]] static const std::filesystem::path& project_dir();

    [[nodiscard]] static ProjectSubosMode project_subos_mode();

    [[nodiscard]] static const std::string& project_subos_name();

    // Get effective workspace: project overrides subos
    [[nodiscard]] static xvm::Workspace effective_workspace();

    // WHO ASKED FOR THIS VERSION.
    //
    // `effective_workspace()` merges three layers and hands back the winner
    // with no record of which layer it came from. That is fine right up until
    // the answer is wrong: a project `.xlings.json` pinning
    // `"mcpp": "2026.99.9.9"` made every `mcpp` invocation fail with
    //
    //     xlings: version '2026.99.9.9' not found for 'mcpp'
    //
    // and nothing anywhere named the file. The user is handed a version string
    // they never typed and whose origin is unsearchable without grepping the
    // disk.
    //
    // Returns the file and field that set `target`, plus WHICH LAYER won --
    // the two halves of the same answer, so that a caller cannot get one of
    // them from here and reconstruct the other from a proxy. `source` is
    // display form (`@xlings/...`, project paths relative to the project
    // root) because it goes straight into a diagnostic, and is empty when no
    // layer claims the target.
    struct VersionOrigin {
        std::string source;
        // The PROJECT MANIFEST (`./.xlings.json -> workspace.<target>`) is
        // the winning layer, which is exactly the condition under which
        // `xlings install` with no arguments fixes the problem: that command
        // walks up for a `.xlings.json` and installs the `workspace` it
        // declares.
        //
        // Deliberately narrower than "a project config exists". The other two
        // layers also produce a non-empty `source`, and for both of them a
        // bare `xlings install` installs something ELSE and exits 0:
        //   - the global pin lives in the active subos's file, which that
        //     command never reads;
        //   - the project-subos file is not read by it either.
        // Pointing a user at a command that succeeds without fixing anything
        // is the failure this whole diagnostic exists to stop.
        bool fromProjectManifest { false };
    };
    [[nodiscard]] static VersionOrigin version_origin(const std::string& target);

    // INVARIANT: a reader and its writer must resolve to the SAME map.
    //
    // They did not. `workspace_mut()` honored forceGlobalScope_ and
    // `workspace()` did not, so under `-g` a package was written into the
    // home's workspace and looked up in the project's -- present and absent
    // at the same time, depending on which half you asked. `install -g`
    // registered it and `remove -g` reported "not installed in current
    // subos", with the shim plainly on disk.
    //
    // The two bodies are therefore identical on purpose. If one grows a
    // condition, the other has to grow it too.
    [[nodiscard]] static const xvm::Workspace& workspace();
    [[nodiscard]] static xvm::Workspace& workspace_mut();

    // Read access to per-subos installed[] sets, paired with the
    // workspace returned by workspace()/workspace_mut(). Only meaningful
    // for subos-side writes (project manifest path returns an empty
    // installed map since the project file format has no installed).
    // Same invariant as workspace()/workspace_mut() above, same reason.
    [[nodiscard]] static const xvm::WorkspaceInstalled& workspace_installed();
    [[nodiscard]] static xvm::WorkspaceInstalled& workspace_installed_mut();
    [[nodiscard]] static bool has_project_config();

    // Force all version/workspace writes to go to global scope.
    // Used by `install -g` to ensure tools are available outside project context.
    // Recomputes the cached paths: `-g` changes which subos this process acts
    // on, and paths_ is derived from that. Without the recompute the flag was
    // honored by one reader and ignored by another for the rest of the run.
    static void set_force_global_scope(bool force);

    // Act on a named subos for the rest of this call, then hand back the
    // previous override so the caller can restore it. Same recompute as the
    // scope flag above and for the same reason: paths_ is derived state, and
    // a setter that does not refresh it is honored by one reader and ignored
    // by the next.
    static std::string set_active_subos_override(std::string name);

    static std::filesystem::path subos_dir(const std::string& name);

    [[nodiscard]] static std::filesystem::path global_subos_dir();

    [[nodiscard]] static std::filesystem::path global_subos_bin_dir();

    // Kept as a name because ~40 call sites read it, but it is no longer a
    // second implementation -- it is the single resolver, live. See
    // resolve_subos_scope_ for why there is exactly one.
    [[nodiscard]] static std::filesystem::path
    xvm_artifact_subos_dir();

    // The same answer as a value: name and root together, so a caller that
    // needs both cannot pick them from two different resolutions.
    [[nodiscard]] static SubosScope subos_scope();

    static std::vector<std::string> list_subos_names();

    // Save .xlings.json versions section (project-local if project config exists)
    static void save_versions();

    // Which xlings set this home up.
    //
    // Always the HOME config, never a project one: this describes the
    // installed client, not a workspace. Historically only `self install`
    // wrote it (xself/install.cppm), so `self update` -- which installs
    // xlings@latest as an ordinary package -- left it reading the previous
    // version forever. The field is what tells a user their packages predate
    // their client, so a stale one is worse than none.
    [[nodiscard]] static std::string recorded_client_version();

    // Read-modify-write of the single field, deliberately: the file also
    // holds the xvm versions DB and the user's config, and this is called
    // from a command that has not necessarily loaded either.
    static void record_client_version(const std::string& version);

    // Save current subos workspace (project-local if project config exists)
    static void save_workspace();

    static void print_paths();
};

} // namespace xlings
