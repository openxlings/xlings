export module xlings.core.xvm.types;

import std;

export namespace xlings::xvm {

struct BindingGroupRef {
    std::string provider;
    std::string providerVersion;
    std::string group;
    std::string rootTarget;
    std::string rootVersion;
};

struct HeaderAsset {
    std::string sourceDir;
    std::string destinationPrefix;
};

struct BindingIntegrityIssue {
    std::string code;
    std::string path;
};

struct VData {
    std::string path;
    std::string kind;
    std::string sourceName;
    std::string destinationName;
    std::string includedir;  // source header directory (e.g., xpkgs/openssl/3.1.5/include)
    std::string libdir;      // source library directory (e.g., xpkgs/glibc/2.39/lib64)

    // kind = "files": one asset the package places into the subos.
    //
    // Both ends are relative and must stay that way. A payload is shared
    // between subos and reference-counted, so an absolute destination
    // recorded against it would be right for the subos that installed it and
    // wrong for every other one. `fileSrc` is relative to the payload root,
    // `fileDst` to the subos root.
    std::string fileSrc;
    std::string fileDst;
    std::vector<std::string> alias;

    // This entry wants `--sysroot=<the active subos>` on every invocation.
    //
    // The same rule as fileSrc/fileDst above, finally applied to the alias:
    // **this database is shared by every subos in the home**, so a subos path
    // stored in it is right for the subos that installed the package and
    // wrong for all the others. A recipe writes the flag with a concrete path
    // because that is all `xvm.add` can express; registration lifts it out of
    // the alias into this boolean, and the shim -- which is the layer that
    // actually knows which subos is active -- puts it back at exec time.
    //
    // What is stored is therefore the INTENT ("this compiler needs the FHS
    // view") rather than one subos's answer to it, and the record is correct
    // read from anywhere. Legacy entries that still carry the path keep
    // working: normalize_subos_paths rewrites them on the way out, and
    // `self doctor --fix` migrates them to this form.
    bool sysroot { false };

    std::map<std::string, std::string> envs;
    std::optional<BindingGroupRef> bindingGroup;
    std::map<std::string, std::string> bindingMembers;
    std::vector<HeaderAsset> bindingHeaders;
    bool bindingMembersDeclared { false };
    bool bindingHeadersDeclared { false };
    std::vector<BindingIntegrityIssue> bindingIntegrityIssues;

    // Verbatim JSON text of every top-level binding field this build could
    // not read, keyed by field name.
    //
    // A field we failed to parse is a field we cannot re-emit from the parsed
    // model: rewriting the entry from `bindingGroup` / `bindingMembers` /
    // `bindingHeaders` alone silently drops whatever did not fit. Keeping the
    // original text makes load/save lossless even for a corrupt entry, which
    // is the precondition for offering to discard it -- a discard the user
    // asked for is a repair, the same discard as a side effect of saving is
    // data loss.
    //
    // Text rather than a JSON object so this stays a plain value type with no
    // dependency on the JSON library.
    std::map<std::string, std::string> bindingUnreadable;

#if defined(_MSC_VER)
    VData() = default;
    ~VData() = default;
    VData(const VData&) = default;
    VData& operator=(const VData&) = default;
    VData(VData&&) = default;
    VData& operator=(VData&&) = default;
#else
    VData();
    ~VData();
    VData(const VData&);
    VData& operator=(const VData&);
    VData(VData&&);
    VData& operator=(VData&&);
#endif
};

struct VInfo {
    std::string type;      // "program" | "lib"
    std::string filename;
    std::map<std::string, VData> versions;
    std::map<std::string, std::map<std::string, std::string>> bindings;

#if defined(_MSC_VER)
    VInfo() = default;
    ~VInfo() = default;
    VInfo(const VInfo&) = default;
    VInfo& operator=(const VInfo&) = default;
    VInfo(VInfo&&) = default;
    VInfo& operator=(VInfo&&) = default;
#else
    VInfo();
    ~VInfo();
    VInfo(const VInfo&);
    VInfo& operator=(const VInfo&);
    VInfo(VInfo&&);
    VInfo& operator=(VInfo&&);
#endif
};

using VersionDB = std::map<std::string, VInfo>;
using Workspace = std::map<std::string, std::string>;  // target -> active version

// Per-subos installed-version sets, sibling to Workspace.
//
// Workspace answers "which version is currently active for target T?"
// WorkspaceInstalled answers "which versions has this subos opted into for T?"
//
// The two maps are kept side-by-side rather than fused into one struct so
// that every existing reader of `Workspace` (shim dispatch, project-mode
// resolution, list/use, GC's by-target check) continues to compile and
// behave identically — `installed[]` is an additive concept that only
// surfaces in subos-aware code paths.
//
// Project-mode workspace (the user-authored .xlings.json under a project
// root) intentionally does NOT carry an installed[] set: project files
// declare intent ("we want gcc 15.1.0" / "{linux: ..., windows: ...}"),
// not runtime state. Only subos workspace files carry WorkspaceInstalled.
using WorkspaceInstalled = std::map<std::string, std::vector<std::string>>;

// Bundle for the subos `.xlings.json` workspace section: active version
// per target plus installed[] per target. Used by subos_workspace_from_json
// / subos_workspace_to_json. Kept distinct from Workspace so that callers
// that only ever needed the active version do not see the new field.
struct SubosWorkspace {
    Workspace active;
    WorkspaceInstalled installed;
};

// How an entry is actually materialized, once defaults are applied.
//
// Three fields decide it, and each has a fallback the entry may rely on:
//
//   kind             VData::kind, else the target-level VInfo::type. Entries
//                    written before 0.4.70 have no per-version kind at all --
//                    on a real installation every one of 372 entries was
//                    missing it -- so the target-level fallback is not a
//                    nicety, it is the only thing that types legacy state.
//   sourceName       what to read out of the payload directory
//   destinationName  what to call it in the sysroot
//
// These live here, next to the types they read, because two subsystems have
// to agree on them exactly: registration writes entries and the switch
// planner materializes them. They used to exist only inside registration's
// detail namespace, so the planner could not see them -- which is part of why
// libraries were registered correctly and then never switched.
std::string effective_kind(const VInfo& info, const VData& data) {
    return data.kind.empty() ? info.type : data.kind;
}

std::string effective_source_name(const std::string& target,
                                  const VInfo& info,
                                  const VData& data,
                                  std::string_view kind) {
    // `group` and `files` name no artifact to dispatch.
    if (kind == "group" || kind == "files") return {};
    if (!data.sourceName.empty()) return data.sourceName;
    return info.filename.empty() ? target : info.filename;
}

std::string effective_destination_name(const std::string& target,
                                       const VData& data,
                                       std::string_view kind,
                                       std::string_view sourceName) {
    if (kind == "group" || kind == "files") return {};
    if (!data.destinationName.empty()) return data.destinationName;
    if (kind == "program") return target;
    return std::string(sourceName);
}

// effective_kind for a (target, version) resolved out of the database.
//
// Several call sites that decide "does this name own a shim" read
// `VInfo::type` directly. That is the FALLBACK, not the authority: a
// per-version `kind` overrides it, and entries written before 0.4.70 have no
// per-version kind at all -- which is why the fallback exists and why reading
// it where the authority is available gets the answer wrong in exactly the
// cases the two disagree. A `lib` or `files` entry could be handed a shim it
// can never dispatch, and a `program` could be denied one.
std::string effective_kind_of(const VersionDB& db,
                              const std::string& target,
                              const std::string& version) {
    auto it = db.find(target);
    if (it == db.end()) return {};
    auto vit = it->second.versions.find(version);
    if (vit == it->second.versions.end()) return it->second.type;
    return effective_kind(it->second, vit->second);
}

// Whether ANY version of this target materializes as a program.
//
// For the checks that start from a shim FILE and have no version in hand. A
// shim belongs to whichever version is active, and widening to "any version"
// is the conservative direction here: it can only make a check report fewer
// orphans, never fabricate one.
bool has_program_kind(const VersionDB& db, const std::string& target) {
    auto it = db.find(target);
    if (it == db.end()) return false;
    if (it->second.versions.empty()) return it->second.type == "program";
    for (const auto& [version, data] : it->second.versions) {
        if (effective_kind(it->second, data) == "program") return true;
    }
    return false;
}

} // namespace xlings::xvm

#if !defined(_MSC_VER)
// Out-of-line special members to work around GCC module boundary issues
xlings::xvm::VData::VData() = default;
xlings::xvm::VData::~VData() = default;
xlings::xvm::VData::VData(const VData&) = default;
xlings::xvm::VData& xlings::xvm::VData::operator=(const VData&) = default;
xlings::xvm::VData::VData(VData&&) = default;
xlings::xvm::VData& xlings::xvm::VData::operator=(VData&&) = default;

xlings::xvm::VInfo::VInfo() = default;
xlings::xvm::VInfo::~VInfo() = default;
xlings::xvm::VInfo::VInfo(const VInfo&) = default;
xlings::xvm::VInfo& xlings::xvm::VInfo::operator=(const VInfo&) = default;
xlings::xvm::VInfo::VInfo(VInfo&&) = default;
xlings::xvm::VInfo& xlings::xvm::VInfo::operator=(VInfo&&) = default;
#endif
