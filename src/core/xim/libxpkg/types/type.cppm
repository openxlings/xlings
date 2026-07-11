export module xlings.core.xim.libxpkg.types.type;

import std;

export namespace xlings::xim {

enum class PackageScope {
    Global,
    Project,
};

// Install phase for UI status tracking
enum class InstallPhase {
    Pending,
    Downloading,
    Extracting,
    Building,
    Installing,
    Configuring,
    Done,
    Failed
};

// Status of a single package during install
struct InstallStatus {
    std::string name;
    InstallPhase phase { InstallPhase::Pending };
    float progress { 0.0f };
    std::string message;
};

// How a node was reached in the dep graph. Determines whether the
// installer activates it in the subos workspace (Runtime) or merely
// places it in the xpkgs store for the consumer's install hook (Build).
enum class DepKind { Runtime, Build };

// What this package exposes — mirrors libxpkg's ExportsBlock but lives in
// xlings's plan so resolver/installer can pass it around without dragging
// the upstream header into every TU. All paths are RELATIVE to install_dir
// at this stage; xlings joins them with the actual install_dir before
// pushing into _RUNTIME so the Lua side gets absolute paths.
struct ExportsRuntime {
    std::string loader;                       // relative path, e.g. "lib64/ld-linux-x86-64.so.2"
    std::vector<std::string> libdirs;         // relative paths
    std::string abi;                          // e.g. "linux-x86_64-glibc"
};

// A node in the dependency-resolved install plan
struct PlanNode {
    std::string rawName;
    std::string name;
    std::string version;
    std::string namespaceName;
    std::string canonicalName;
    std::string repoName;
    std::filesystem::path pkgFile;
    std::filesystem::path storeRoot;
    // Effective union of runtime + build deps (kept for legacy callers
    // that don't yet distinguish; populated by resolver as `runtime ∪
    // build`).
    std::vector<std::string> deps;
    // The two kinds, populated when the package's xpm declares them
    // separately. For legacy packages where only `deps` is present in
    // the schema, both lists are equal to `deps` (loader-side fan-out).
    std::vector<std::string> runtime_deps;
    std::vector<std::string> build_deps;
    // How this node was added to the plan: Build = a transitive
    // build-only dep of some other node; Runtime = part of a consumer's
    // active workspace contract. Build nodes skip workspace activation.
    DepKind kind { DepKind::Runtime };
    // What this package itself exposes (parsed from xpm.<platform>.exports
    // by the resolver). Empty `loader` means "this package isn't a loader
    // provider"; predicate-driven elfpatch trigger reads this + dep nodes'
    // exports to decide whether and how to patch.
    ExportsRuntime exports;
    bool alreadyInstalled { false };
    bool isSystemPM { false };
    PackageScope scope { PackageScope::Global };
    int pkgType { 0 };  // 0=Package, 1=Script, 2=Template, 3=Config, 4=Subos

    // Explicit special members to work around GCC 15 module linker bug
    PlanNode() = default;
#if defined(_MSC_VER)
    ~PlanNode() = default;
    PlanNode(const PlanNode&) = default;
    PlanNode& operator=(const PlanNode&) = default;
    PlanNode(PlanNode&&) noexcept = default;
    PlanNode& operator=(PlanNode&&) noexcept = default;
#else
    ~PlanNode();
    PlanNode(const PlanNode&);
    PlanNode& operator=(const PlanNode&);
    PlanNode(PlanNode&&) noexcept;
    PlanNode& operator=(PlanNode&&) noexcept;
#endif
};

#if !defined(_MSC_VER)
// Out-of-line definitions force GCC to emit symbols in this TU
PlanNode::~PlanNode() = default;
PlanNode::PlanNode(const PlanNode&) = default;
PlanNode& PlanNode::operator=(const PlanNode&) = default;
PlanNode::PlanNode(PlanNode&&) noexcept = default;
PlanNode& PlanNode::operator=(PlanNode&&) noexcept = default;
#endif

// DAG install plan (topologically sorted)
struct InstallPlan {
    std::vector<PlanNode> nodes;
    std::vector<std::string> errors;

    bool has_errors() const { return !errors.empty(); }
    std::size_t pending_count() const {
        std::size_t n = 0;
        for (auto& nd : nodes)
            if (!nd.alreadyInstalled) ++n;
        return n;
    }

    InstallPlan() = default;
#if defined(_MSC_VER)
    ~InstallPlan() = default;
    InstallPlan(const InstallPlan&) = default;
    InstallPlan& operator=(const InstallPlan&) = default;
    InstallPlan(InstallPlan&&) noexcept = default;
    InstallPlan& operator=(InstallPlan&&) noexcept = default;
#else
    ~InstallPlan();
    InstallPlan(const InstallPlan&);
    InstallPlan& operator=(const InstallPlan&);
    InstallPlan(InstallPlan&&) noexcept;
    InstallPlan& operator=(InstallPlan&&) noexcept;
#endif
};

#if !defined(_MSC_VER)
InstallPlan::~InstallPlan() = default;
InstallPlan::InstallPlan(const InstallPlan&) = default;
InstallPlan& InstallPlan::operator=(const InstallPlan&) = default;
InstallPlan::InstallPlan(InstallPlan&&) noexcept = default;
InstallPlan& InstallPlan::operator=(InstallPlan&&) noexcept = default;
#endif

// Download task for a single resource
struct DownloadTask {
    std::string name;
    std::string url;
    std::string sha256;
    std::string cacheIdentity;
    std::filesystem::path destDir;
    std::vector<std::string> fallbackUrls;  // tried in order when url fails
};

// Result of a download
struct DownloadResult {
    std::string name;
    bool success { false };
    std::string error;
    std::filesystem::path localFile;
};

// Downloader configuration
struct DownloaderConfig {
    int maxConcurrency { 4 };
    std::string preferredMirror;    // "GLOBAL" | "CN"
    bool autoDetectMirror { true };
};

} // namespace xlings::xim
