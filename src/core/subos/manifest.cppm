export module xlings.core.subos.manifest;

import std;

import xlings.libs.json;

// `subos_info` — what a subos is, recorded in the subos's own `.xlings.json`.
//
// A subos already had a directory, a bin/, an xvm scope and a registry entry.
// What it did not have was a statement of *what it is*: which runtime its
// binaries were built against, and which environment its processes need. Both
// were implicit in whatever happened to be installed, which is why a subos
// could not be described, checked, or reproduced.
//
// Layer this closes: a program needs bootstrap (PT_INTERP + CRT + libc),
// discovery (PATH + RPATH), and configuration (env vars). xlings had the first
// two — glibc + elfpatch, xvm + shims — and nothing for the third. That is the
// gap behind mcpp-community/mcpp#352: a GLFW binary that links fine and exits
// 255 because no one told it where the GL drivers are.
//
// Placement is the per-subos `.xlings.json`, not a new file and not the home
// or project one. Those two already use the key `subos` for other things (a
// selector string at project level, a registry map at home level), and a subos
// describing itself belongs in the subos.
//
// This module is deliberately free of Config/xvm imports: it takes paths and a
// binding→dir resolver from the caller. That keeps invariant checking and
// placeholder expansion testable without a home on disk, and it is what lets
// the doctor reporter and the repairer share one predicate instead of
// describing the rules twice and drifting apart.

export namespace xlings::subos::manifest {

namespace fs = std::filesystem;

inline constexpr int             SCHEMA_VERSION  = 1;
inline constexpr std::string_view BLOCK          = "subos_info";
// WHICH RUNTIME A SUBOS GETS WHEN THE CALLER NAMES NONE.
//
// A NAME here, and the version resolved from the index by the caller. The
// previous shape was a pinned constant, and the reason given for it was:
//
//     A constant rather than a lookup: ... inventing a "pick the newest libc
//     PRESENT" rule would make two homes with the same command produce
//     different subos.
//
// That reasoning is sound and it is not what this does. It rejects scanning
// the MACHINE -- two homes have different things installed, so that answer is
// per-machine. Reading the index's `latest` is the opposite: two homes on the
// same index snapshot get the same answer, and it is the answer every
// `xlings install` already gets. The resolved value is still persisted into
// the subos's own manifest at creation, so the binding remains a
// creation-time property; only where the number comes from has moved.
//
// WHY IT HAD TO MOVE. The pin and the index's `latest` were one decision
// written in two repositories, and xim-pkgindex#692 made them disagree:
//
//   error: selected RuntimeBinding glibc@2.44 requires payload
//          '<home>/registry/data/xpkgs/xim-x-glibc/2.44', but it is not
//          installed; mcpp will not fall back to another directory entry
//
// measured on a first `mcpp build` in a fresh home. `latest` resolved to
// 2.44.2 and installed it; this constant said 2.44; payload directories are
// named after the version, so nothing on disk answered to the binding. Every
// NEW subos was affected and no existing one was -- the shape that is easy to
// miss, because a developer machine full of older subos stays green.
//
// It survived until now because 2.39 -> 2.44 is an UPSTREAM version move:
// rare, visible, and someone is watching. 2.44 -> 2.44.2 is a packaging
// revision of the same upstream release, which moves whenever an artifact has
// to be rebuilt. The revision suffix is what first made drift possible.
//
// The policy the old comment carried -- `our_glibc >= host_glibc`, because
// `*-host-link` packages permanently bring host-built libraries into our
// closure -- now lives in the index, where glibc.lua states that `latest`
// TRACKS the highest glibc of any distribution we support. Keeping a second
// copy here is what let the two drift; this is the copy that goes.
inline constexpr std::string_view DEFAULT_RUNTIME_PACKAGE = "glibc";

// The same package, spelled as an INDEX COORDINATE rather than as a binding.
//
// Two constants because they are answers to different questions, and using one
// for both is a bug that hides:
//
//   DEFAULT_RUNTIME_PACKAGE  names the RUNTIME. It is what goes in the binding
//                            (`glibc@2.44.2`) and what the payload directory is
//                            called, so it cannot carry a namespace.
//   DEFAULT_RUNTIME_QUERY    asks the INDEX. A bare name there is AMBIGUOUS the
//                            moment a second repo publishes it -- and one does:
//                            `scode` ships glibc too, so on any home with the
//                            default sub-indexes
//
//                              [error] package 'glibc' is ambiguous, candidates:
//                                1. scode:glibc@2.44.2
//                                2. xim:glibc@2.44.2
//
//                            and the resolver returns an error, which the
//                            caller reads as "the index cannot answer" and
//                            falls back. Measured in a sandbox on a released
//                            client: the binding came out RIGHT and
//                            `runtime_source` said `fallback`, because the
//                            pinned value and the index's answer happened to
//                            agree. The provenance field is the only reason
//                            that was visible at all.
//
// `xim` and not "whichever repo answers": the primary index is where the
// runtime contract lives (glibc.lua states the `latest` policy there), and a
// sub-index shadowing the libc the whole closure is built against is not a
// tie to break by priority.
inline constexpr std::string_view DEFAULT_RUNTIME_QUERY = "xim:glibc";

// Used ONLY when the index cannot answer -- see resolve_default_runtime() in
// subos.cpp, which is the single place allowed to decide that. It is a real
// version because a subos must not be created against a binding nobody can
// satisfy, and it is deliberately the same value the index's `latest` had
// when this line was last touched.
//
// A subos created from this rather than from the index records
// `runtime_source: "fallback"`, so a stale pin can never be mistaken for a
// resolved fact. Nothing else may read it: substituting a constant for an
// answer under Describe is exactly the defect this file already fixed once.
inline constexpr std::string_view DEFAULT_RUNTIME_FALLBACK = "glibc@2.44.2";

inline constexpr std::string_view OP_SET     = "set";
inline constexpr std::string_view OP_PREPEND = "prepend";

// ── data ────────────────────────────────────────────────────────────────

// One variable a package asks its subos to export.
struct EnvDecl {
    std::string var;
    std::string op;     // OP_SET | OP_PREPEND
    std::string value;  // may contain ${...} placeholders
};

// A provider's whole section. Keyed by binding so uninstalling the package
// removes exactly what it added — the same provider-scoped ownership xvm.add
// and xvm.files already use.
struct Provider {
    std::string          binding;   // "<name>@<version>"
    std::vector<EnvDecl> decls;
};

struct Info {
    int                   schema_version = 0;
    // May be EMPTY, and empty is a statement: "we looked and could not tell".
    // Distinct from the block being absent, which means "written before this
    // block existed". A reader must not substitute a default for either --
    // see `runtime_for` for why a constant here is a fabricated record.
    std::string           runtime;
    std::vector<Provider> envs;      // sorted by binding; see resolve()
    // Provenance. Exactly one pair is written, and which one says what
    // happened: a subos that was CREATED carries created_*, one that was
    // merely DESCRIBED after the fact carries described_*. Writing created_at
    // on a backfill dates a months-old subos to the moment someone ran
    // `doctor --fix` -- measured on a real home, where two different subos
    // carried a byte-identical `created_at` because they were described in
    // the same run, not created in the same second.
    std::string           created_at;
    std::string           created_by;
    std::string           described_at;
    std::string           described_by;
    // Host glibc version ("2.39") probed when the block was written. Empty =
    // unknown: pre-C1 manifests, non-glibc hosts, failed probe. Rule A's
    // right-hand side; a reader must treat unknown as unprovable, not as 0.
    std::string           host_glibc;
    // See BlockSpec::runtimeSource. Empty = the block predates the field, or
    // was written by a path that has nothing to say about provenance. A
    // reader must treat empty as UNKNOWN, never as any of the three values.
    std::string           runtime_source;
};

// ── who is asking, and therefore what may be invented ────────────────────
//
// Six places write this block. Five of them routed through one function and
// one wrote `DEFAULT_RUNTIME` directly, which is how a subos whose workspace
// plainly recorded glibc@2.39 came to declare glibc@2.44. The fix is not a
// seventh writer: it is that all of them ask the same question, and that the
// ONE thing they legitimately disagree about becomes a parameter.
//
// That one thing is whether a constant may stand in for an answer:
//
//   Create    someone is making a new subos and could have said `--runtime`.
//             `DEFAULT_RUNTIME` is what "they didn't say" means, so it is
//             legitimate -- and legitimate ONLY here.
//   Describe  the subos already exists and is already running something.
//             Nobody was asked and nobody can be. A constant here does not
//             record a default, it records a guess as a fact.
//
// `xlings install` has no `--runtime` flag anywhere in the tree (the only
// parser for it is `subos new`), which is what makes the installer's two
// sites Describe rather than Create however much they look like creation.
enum class Intent {
    Create,
    Describe,
};

// What to write. A struct rather than five positional arguments because the
// provenance pair and the runtime key are both conditional on `intent`, and
// callers kept getting the argument order right for the wrong reason.
struct BlockSpec {
    std::string runtime;    // empty -> the `runtime` key is OMITTED
    std::string by;         // "xlings <version>"
    std::string hostGlibc;  // empty -> the key is omitted
    Intent      intent = Intent::Create;
    // Where `runtime` came from. Empty -> the key is omitted, which is what
    // every pre-existing manifest looks like and is read back as "unknown".
    //
    // It exists because the three answers are otherwise INDISTINGUISHABLE on
    // disk, and they are not equally trustworthy: `index` is what the index
    // said at creation, `fallback` is a pin used because the index could not
    // be read, `explicit` is a human. A subos sitting on an old glibc could
    // be any of the three, and without this nobody -- doctor included -- can
    // tell a recorded decision from a guess that had nowhere else to go.
    std::string runtimeSource;  // RUNTIME_SOURCE_* below
};

// Values for BlockSpec::runtimeSource / Info::runtime_source.
inline constexpr std::string_view RUNTIME_SOURCE_EXPLICIT = "explicit";
inline constexpr std::string_view RUNTIME_SOURCE_INDEX    = "index";
inline constexpr std::string_view RUNTIME_SOURCE_FALLBACK = "fallback";

// The package names that can serve as a subos's C runtime.
//
// Derived from the same set `family_of` maps rather than a second list, so a
// new OS adds one row in one place. Order is the tie-break when a workspace
// somehow records two: deterministic and documented beats "whichever the map
// iterated first".
inline constexpr std::array<std::string_view, 5> RUNTIME_PACKAGES = {
    "glibc", "musl", "wasi-libc", "macos_sdk", "ucrt",
};

// ── runtime family ──────────────────────────────────────────────────────

// The runtime string is self-describing: "glibc@2.39" says Linux/glibc without
// a second field to disagree with it. Families are derived here rather than
// stored, so a manifest cannot claim a family its runtime contradicts.
//
// Slice 1 needs only the glibc row. The rest are listed to make the shape of
// the mapping explicit — a new OS adds a row, not a schema field.
std::string family_of(std::string_view runtime, std::string_view arch = "x86_64");

// "<name>@<version>", both halves non-empty. Used for `runtime` and for every
// envs key.
bool is_binding(std::string_view s);

std::string_view binding_name(std::string_view binding);

std::string_view binding_version(std::string_view binding);

// ── the subos layer's "exactly one" ─────────────────────────────────────
//
// One package, one version, per subos. The three-layer model has always said
// so -- the xpkg store holds many versions by design, each consumer freezes one
// into its own RPATH/INTERP, and the subos sysroot in between is the layer that
// is supposed to hold exactly one -- but nothing anywhere enforced it.
//
// Measured on a real home: `xlings list` showed mesa@25.0.7 and mesa@25.0.7.1
// both bound in `default`, both contributing to __EGL_VENDOR_LIBRARY_DIRS, and
// EGL duly enumerated the device twice. `xlings self doctor` said nothing.
//
// This is P1 wearing a different hat. "What is in this subos" has two records
// -- the xvm registration and this manifest's envs section -- and installing a
// second version appends to both. They agree, which is why nothing complained;
// they agree on an answer the model forbids.
//
// ONE function, used by the report and by --fix. Every previous report/repair
// pair in this repo has drifted, and the shape it takes is a finding that
// repairing does not clear, so the predicate lives here rather than in doctor.
struct DuplicateBinding {
    std::string              name;
    std::vector<std::string> bindings;   // every binding for that name, sorted
};

std::vector<DuplicateBinding> duplicate_bindings(const Info& info);

// Which version of each package THIS subos has active.
//
// Read from the same file as the declarations. `subos/<name>/.xlings.json`
// holds both `workspace` (name -> active/installed) and `subos_info.envs`
// (binding -> declarations) -- two records of "what is in this subos", in one
// file. That is the whole of P1 in eleven lines of JSON.
std::map<std::string, std::string> active_versions(const nlohmann::json& doc);

// True when `version` is the one `active` names, allowing for the `<ns>:<ver>`
// form a namespaced install records.
inline bool version_is_active(std::string_view version, std::string_view active) {
    if (version == active) return true;
    const auto colon = active.find(':');
    return colon != std::string_view::npos && active.substr(colon + 1) == version;
}

// The manifest and XVM workspace are two records of the same SubOS runtime.
// The global payload store may contain any number of versions; this predicate
// only asks whether THIS SubOS has exactly the version it declares active and
// whether that declaration still resolves to a payload on disk.
struct RuntimeActivationMismatch {
    std::string declared;
    std::string active;
    bool payloadMissing = false;
};

std::optional<RuntimeActivationMismatch>
check_runtime_activation(const Info& info,
                         std::string_view activeVersion,
                         bool payloadExists);

// The providers that actually take effect, out of everything the manifest
// records.
//
// A package installed at two versions keeps BOTH provider sections -- the
// dormant one has to survive so that `xlings use pkg@<older>` restores its
// environment without reinstalling. What must not happen is both contributing
// at once, which is how one GPU came to be enumerated as two.
//
// Which one is live is not a decision made here: xvm already made it, and its
// answer is in this same file. Re-deriving it (highest version? last
// installed?) would be a second answerer to a question that has one -- the
// exact defect this function exists to remove.
//
// A package with NO active version keeps every provider, and that default
// matters more than it looks: filtering on a record that turns out to be absent
// would silently delete a package's whole environment, which is the failure
// mode this file exists to prevent, arrived at from the other side. Measured
// before choosing it -- a bare `xvm.add(name)` does record an active version,
// so this is the salvage path for a manifest whose workspace record was lost
// (pruned, copied between homes, hand-edited), not the common case. Where
// several versions are declared with no active one, nothing can say which is
// meant; that state is reported rather than guessed at.
Info select_effective(const Info& info,
                      const std::map<std::string, std::string>& active);

// A package bound at several versions with nothing able to say which is meant.
//
// NOT the same as duplicate_bindings: two versions where one is active is
// ordinary -- the dormant declarations are how `xlings use` can switch back.
// This is the subset that has no active version -- every one of them
// contributes, so the subos exports each variable several times over. Reaching
// it takes a lost workspace record, not an ordinary install.
std::vector<DuplicateBinding>
contested_bindings(const Info& info,
                   const std::map<std::string, std::string>& active);

// ── invariants ──────────────────────────────────────────────────────────

enum class Defect {
    DirMissing,          // I1
    ConfigMissing,       // I2
    ConfigUnreadable,    // I2
    BlockMissing,        // I4
    SchemaUnsupported,   // I5
    RuntimeMalformed,    // I6
    EnvsMalformed,       // I7
    EnvDeclMalformed,    // I7
    ProvenanceMissing,   // I8
};

struct Finding {
    Defect      kind;
    std::string detail;
};

std::string_view describe(Defect d);

fs::path config_path(const fs::path& subosDir);

// Read the whole document. An unreadable or malformed file yields nullopt, and
// the caller must not paper over it with an empty object: for the home config
// "corrupt means absent" is the historical behavior, but here it would let
// `--fix` rewrite a file it never managed to read.
std::optional<nlohmann::json> read_document(const fs::path& subosDir);

// I4–I8, over a document already in hand. Split out from validate() so a
// caller that has just built a document in memory can check it before writing.
std::vector<Finding> validate_block(const nlohmann::json& doc);

// I1–I8 for a subos on disk. The single predicate the doctor reporter and the
// repairer both call — two descriptions of the same rule is how a reporter
// ends up flagging what `--fix` will not touch.
std::vector<Finding> validate(const fs::path& subosDir);

// ── parse / build ───────────────────────────────────────────────────────

// Providers come back sorted by binding. That ordering is the resolution order
// (see resolve) and it is derived from the data rather than from install
// history, so two homes holding the same manifest resolve it identically.
Info parse(const nlohmann::json& doc);

std::string utc_now_iso();

// The block a subos gets. `envs` is an explicit empty object.
//
// Three keys are conditional, and each absence is a statement:
//   * `runtime`    absent  — we looked and could not tell (spec.runtime empty)
//   * `host_glibc` absent  — the probe found nothing / not a glibc host
//   * `created_*` vs `described_*` — created, or merely described afterwards
//
// An absent key is the documented spelling of "unknown", so a pre-C1 manifest
// and a failed probe read the same way, and neither reads as a claim.
nlohmann::json make_block(const BlockSpec& spec);

// The block for a subos that ALREADY EXISTS -- the one call every Describe
// site makes.
//
// Four sites were repeating the same five-line spec, and the three things
// they all have to get right are exactly the three that were easy to get
// wrong: Describe intent, the runtime from `runtime_for` rather than a
// constant, and carrying a genuine creation record across the rebuild.
//
// That last one matters for the same reason as everything else here. A
// rebuild is triggered by the block being INVALID, which can be true for
// reasons that have nothing to do with provenance (a corrupted `envs`), and
// a subos that really was created by `subos new` has a real `created_at`.
// Replacing it with `described_at` alone would delete a fact -- the mirror
// image of inventing one.
nlohmann::json describe_block(const fs::path& subosDir,
                              const nlohmann::json& doc,
                              std::string_view by,
                              std::string_view hostGlibc = {});

// What a subos is OBSERVED to run, out of the workspace in its own manifest.
//
// A subos that predates `subos_info` recorded no binding, but it is not silent
// about its runtime: its workspace names an active version for the runtime
// family. Reading it is not a guess and not a scan -- it is the same file, one
// key over, and it is the record `use` itself maintains.
//
// The family comes from the caller's fallback rather than a literal, so this
// says nothing about which family a platform uses.
std::string observed_runtime(const nlohmann::json& doc,
                             std::string_view family);

// What the SYSROOT is actually serving, out of the symlink farm in the subos.
//
// This is the only source that is an OBSERVATION rather than a record. Every
// other answer to "which libc is this" is something a previous run wrote
// down; this one is what the loader will actually open. It is what makes the
// difference between a declaration being checkable and being merely stated.
//
// The family comes from the STORE PATH the link lands in
// (`.../xpkgs/xim-x-glibc/2.39/...` -> `glibc@2.39`), never from the file
// name — a filename->family table would be a second place to be wrong about
// what a payload is.
//
// Empty when there is no such link, or it does not point into a store.
//
// `fromLink`, when given, receives the link that answered. A finding that
// says "lib/libc.so.6 points into X" while it actually read
// `lib64/ld-musl-x86_64.so.1` names a file it never opened -- and this whole
// change is about messages that state more than they checked. The probe list
// spans four directories and seven names, so the caller cannot infer it.
std::string sysroot_runtime(const fs::path& subosDir,
                            fs::path* fromLink = nullptr);

// The runtime a block should carry. THE answer — every writer calls this.
//
// A block can be invalid for reasons that have nothing to do with its runtime
// (schema_version, envs, provenance), and every rebuild path used to reset
// the runtime to a caller-supplied fallback as a side effect. After a default
// bump that side effect silently re-declares an existing subos against a libc
// its payloads were never built for — the exact "changed underneath you" C1
// forbids.
//
// Sources, most authoritative first:
//
//   1. a valid recorded binding — the subos said what it is;
//   2. `requested` (`--runtime`) — a human said what it should be.
//      CREATE ONLY: there is no way to say it anywhere else;
//   3. the workspace's active runtime — the subos never said, but it is
//      demonstrably running something, and declaring it against anything else
//      is the same re-declaration as (1) guards, reached from the
//      never-recorded side instead of the invalid-block side;
//   4. the sysroot observation — no record anywhere, but the payload behind
//      `lib/libc.so.6` is not an opinion;
//   5. `defaultRuntime`, the caller's answer to "they didn't say". CREATE
//      ONLY. Under Describe this step does not exist and the result is EMPTY,
//      which `make_block` writes as an absent `runtime` key — "we looked and
//      could not tell", which a reader can act on, rather than a constant it
//      cannot distinguish from a real answer.
//
//      A PARAMETER, not a constant read here, because answering it needs the
//      index and this module is deliberately free of Config/xvm/catalog
//      imports (see the header). Callers that can resolve pass the resolved
//      value; the default keeps the signature usable from a test with no home
//      on disk. This is the same rule the header already states -- everyone
//      asks one question, and the ONE thing they legitimately disagree about
//      becomes a parameter.
//
// (3) and (4) are what make the upgrade seamless for every home created
// before `subos_info` existed. Without them those homes are declared against
// the current default the first time anything rebuilds their block, and then
// `self doctor` calls them broken and `use` refuses to activate the runtime
// they were already on.
//
// (3) outranks (4) deliberately, and disagreement between them is NOT
// resolved quietly — doctor reports it (SubosRuntimeDrift). Two answers to
// one question is the defect; picking one and staying silent would hide it.
std::string runtime_for(const fs::path& subosDir,
                        const nlohmann::json& doc,
                        Intent intent,
                        std::string_view requested = {},
                        std::string_view defaultRuntime = DEFAULT_RUNTIME_FALLBACK);

// ── env declarations ────────────────────────────────────────────────────

// Record one declaration under its provider. Idempotent on the whole
// (var, op, value) triple: config() runs again on every dependent install, and
// a re-run must not grow the section.
//
// Returns whether the document changed, so a caller can skip a write.
bool add_env(nlohmann::json& doc, std::string_view binding, const EnvDecl& decl);

// Replace a provider's whole section with what it declared THIS run.
//
// add_env alone cannot express this, and the gap is not theoretical. Its
// contract is idempotence per (var, op, value) triple, which holds a re-run of
// the SAME declarations steady -- and silently accumulates two generations when
// the recipe's declarations CHANGE. Measured while moving mesa's discovery paths
// from `${pkgdir}` to `${subosdir}`: the section ended up holding both, so
// LIBGL_DRIVERS_PATH resolved to the new subos directory followed by a stale
// payload path, and __EGL_VENDOR_LIBRARY_DIRS listed the shared vendor directory
// twice. Nothing reported it, and once that payload is collected the stale entry
// is a directory the loader walks past.
//
// A recipe is the sole owner of its binding's section -- that is the ownership
// rule the whole `envs` design rests on, and the reason uninstall needs no
// cleanup code in the recipe. Owning it means being able to REMOVE a
// declaration, not only add one.
//
// Returns whether the document changed, so a caller can skip a write. Order is
// preserved as declared: `prepend` composition depends on it.
bool set_env_section(nlohmann::json& doc, std::string_view binding,
                     const std::vector<EnvDecl>& decls);

// Drop a provider's whole section — the uninstall counterpart of add_env. The
// package never writes cleanup code for this; ownership is by binding, so
// removing the key removes exactly what that package added and nothing else.
//
// The `envs` object itself stays, empty, per the invariant above.
bool remove_provider(nlohmann::json& doc, std::string_view binding);

// Every provider whose package name matches, regardless of version. Uninstall
// knows the name and version it removed, but a home that installed the same
// package twice under different versions can hold a stale section from the
// earlier one; matching on name is what lets doctor see it.
std::vector<std::string> providers_named(const nlohmann::json& doc,
                                         std::string_view name);

// ── placeholders ────────────────────────────────────────────────────────

// What a value may refer to. `${pkgdir}` differs per provider, so it arrives
// as a resolver rather than a path — and the resolver is the caller's, which
// is what keeps this module independent of the version database.
struct Placeholders {
    fs::path subosdir;
    fs::path home;
    fs::path xlings_home;
    std::function<fs::path(std::string_view binding)> pkgdir_of;
};

// Expand ${...} in a declared value.
//
// Placeholders are why a manifest is portable at all: a value carrying
// /home/alice/... describes one machine, and a subos description that only
// works on the machine that wrote it is not a description.
//
// An unknown or unresolvable placeholder is left verbatim rather than replaced
// with an empty string. Empty would turn "${pkgdir}/lib/dri" into "/lib/dri" —
// a real path, on the host, outside the subos. Leaving the text intact makes
// the failure visible to doctor D3 instead of pointing a driver search at /.
std::string expand(std::string_view value, std::string_view binding,
                   const Placeholders& ph);

// True once expansion left any `${...}` behind — i.e. something could not be
// resolved. Doctor D3 reports on this rather than on the directory existing,
// because an unexpanded value is a defect regardless of what is on disk.
bool has_unresolved(std::string_view expanded);

// ── privileged declarations ─────────────────────────────────────────────
//
// A `subos.env` declaration whose value points at OUR payload is one of two
// very different things, and the difference is not "is it process-global":
//
//   causes CODE to be loaded    LD_LIBRARY_PATH, LD_PRELOAD,
//     into someone's process    __EGL_VENDOR_LIBRARY_DIRS, LIBGL_DRIVERS_PATH,
//                               VK_ICD_FILENAMES, and every future variable
//                               some library invents for finding its plugins
//   causes DATA to be found     XDG_DATA_DIRS, MANPATH, PKG_CONFIG_PATH
//
// The first class is dangerous because every child of the subos shell inherits
// it, and most of those children are HOST binaries under the HOST loader.
// Measured: a host binary linked against the host's libEGL drops from the
// NVIDIA GPU to llvmpipe under our declarations, and LD_DEBUG shows it loading
// OUR libm, libgcc_s, libstdc++ and libxcb into a process running on the host's
// libc. On this machine the host glibc happened to match; on an older one that
// is `version 'GLIBC_2.xx' not found`. The second class is ordinary — subos
// supplies a default, the user can override it, that is how Linux works (AD-3).
// `PATH` is a third thing again: it does not inject code into an existing
// process, it decides which executable runs, and it is governed by R6/AD-1
// rather than by this guard.
//
// The list below is the BENIGN one -- named for what it asserts, since PATH is
// on it and PATH plainly does not name only data. The check is default-deny. Listing the
// dangerous set instead would be a hand-written list of "what we happened to
// think of" — the exact anti-pattern R7 names, and the one that already cost us
// five missing entries in nvidia-gl-host-link's dependency table. A variable
// nobody has classified reads as privileged, which fails toward a report.
//
// Adding to this list is a deliberate act: it asserts the variable cannot cause
// code to enter a process.
inline bool never_loads_code(std::string_view var) {
    return var == "XDG_DATA_DIRS"   || var == "XDG_CONFIG_DIRS"
        || var == "XDG_DATA_HOME"   || var == "XDG_CONFIG_HOME"
        || var == "XDG_CACHE_HOME"  || var == "XDG_STATE_HOME"
        || var == "MANPATH"         || var == "INFOPATH"
        || var == "PKG_CONFIG_PATH" || var == "PKG_CONFIG_LIBDIR"
        || var == "ACLOCAL_PATH"    || var == "TERMINFO"
        || var == "FONTCONFIG_PATH" || var == "FONTCONFIG_FILE"
        || var == "SSL_CERT_FILE"   || var == "SSL_CERT_DIR"
        || var == "GIT_SSL_CAINFO"  || var == "CURL_CA_BUNDLE"
        || var == "LOCPATH"         || var == "TZDIR"
        || var == "PATH";  // R6/AD-1's business, not this guard's
}

// A declaration is privileged when it can put code from our payload into a
// process we do not own.
//
// The placeholders are checked as well as an expanded store path, because at
// install time -- the moment this most needs to be reported -- the value has
// not been expanded yet.
//
// `${subosdir}` counts. The subos sysroot is a VIEW onto our payloads, made of
// symlinks into them, so a directory under it on a loader search path delivers
// our libraries just as surely as the store path does. Checking only
// `${pkgdir}` would have let the same declaration through in its other spelling
// -- one hazard with two names, which is the shape this whole review is about.
inline bool is_privileged_env(std::string_view var, std::string_view value) {
    if (never_loads_code(var)) return false;
    for (const auto* needle : {"${pkgdir}", "${subosdir}", "${xlings_home}",
                               "/xpkgs/"}) {
        if (value.find(needle) != std::string_view::npos) return true;
    }
    return false;
}

// ── resolution ──────────────────────────────────────────────────────────

// One variable as it will actually be exported.
struct Resolved {
    std::string              var;
    std::string              op;         // the winning op
    std::string              value;      // expanded, prepends already joined
    std::vector<std::string> providers;  // every binding that declared this var
    bool                     conflicted = false;
    bool                     unresolved = false;
};

// Fold the manifest into the variables to export.
//
// Conflict rules, when more than one provider names the same variable:
//   * several `set`      — the last provider wins, and it is a conflict
//   * several `prepend`  — all contribute, later providers land nearer the front
//   * `set` and `prepend` mixed — `set` wins, the prepends are dropped, conflict
//
// "Later" means later in binding order, not install order. Install order is
// not recorded in the manifest and recording it would add a field whose only
// job is to make the outcome depend on history — two homes with byte-identical
// manifests would then export different values. Sorting by binding keeps the
// manifest the whole answer. Every conflict is reported (doctor D4) rather than
// resolved quietly.
std::vector<Resolved> resolve(const Info& info, const Placeholders& ph);

} // namespace xlings::subos::manifest
