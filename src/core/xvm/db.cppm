export module xlings.core.xvm.db;

import std;

import xlings.core.xvm.types;
import xlings.libs.json;
import xlings.platform;

export namespace xlings::xvm {

// Parse "ns:ver" into (namespace, version). Plain "ver" → ("", "ver").
std::pair<std::string, std::string> parse_ns_version(const std::string& s) {
    auto pos = s.find(':');
    if (pos == std::string::npos) return {"", s};
    return {s.substr(0, pos), s.substr(pos + 1)};
}

// Build "ns:ver" from namespace and version. Empty namespace → plain "ver".
std::string make_ns_version(const std::string& ns, const std::string& ver) {
    if (ns.empty()) return ver;
    return ns + ":" + ver;
}

// Strip namespace prefix, returning bare version.
std::string strip_namespace(const std::string& s) {
    return parse_ns_version(s).second;
}

// Get namespace prefix (empty if none).
std::string get_namespace(const std::string& s) {
    return parse_ns_version(s).first;
}

// How a (target, version-key) pair is written for a human, and for a shell.
//
// The two forms are REVERSED and that is the whole reason this exists. The
// database keys a version as "ns:ver" -- the namespace rides on the version.
// Everything a user types keys it as "ns:target@ver" -- the namespace rides on
// the front of the coordinate. Code that formats a finding as "{}@{}" produces
// `mcpp@local:0.0.27`, which reads like a version called `local:0.0.27` and
// parses as one too: `xlings install mcpp@local:0.0.27` looks up a version that
// does not exist. Every renderer goes through here instead.
//
//   ("mcpp", "local:0.0.27")  -> "local:mcpp@0.0.27"
//   ("llvm", "20.1.7")        -> "llvm@20.1.7"
//   ("gcc",  "")              -> "gcc"
std::string display_coordinate(std::string_view target,
                               std::string_view versionKey) {
    if (versionKey.empty()) return std::string(target);
    const auto [ns, version] = parse_ns_version(std::string(versionKey));
    if (ns.empty()) return std::format("{}@{}", target, version);
    return std::format("{}:{}@{}", ns, target, version);
}

// Add or update a version entry in the database.
// If ns is non-empty, the version key becomes "ns:version".
void add_version(VersionDB& db,
                 const std::string& target,
                 const std::string& version,
                 const std::string& path,
                 const std::string& type = "program",
                 const std::string& filename = "",
                 const std::string& alias = "",
                 const std::string& ns = "",
                 const std::string& binding = "") {
    auto& info = db[target];
    if (info.type.empty()) info.type = type;
    if (info.filename.empty() && !filename.empty()) info.filename = filename;

    auto ver_key = make_ns_version(ns, version);
    VData vdata;
    vdata.path = path;
    vdata.kind = type;
    if (type != "group") {
        vdata.sourceName = filename.empty() ? target : filename;
        vdata.destinationName =
            type == "program" ? target : vdata.sourceName;
    }
    if (!alias.empty()) vdata.alias.push_back(alias);
    info.versions[ver_key] = std::move(vdata);

    // Establish bidirectional binding relationship
    if (!binding.empty()) {
        auto at = binding.find('@');
        if (at != std::string::npos) {
            auto peer = binding.substr(0, at);
            auto peer_ver = make_ns_version(ns, binding.substr(at + 1));
            // Bidirectional: peer records target, target records peer
            db[peer].bindings[target][peer_ver] = ver_key;
            db[target].bindings[peer][ver_key] = peer_ver;
        }
    }
}

// Remove a version from the database.
// Namespaced input must match exactly. Bare input resolves only when exactly
// one stored key has that bare version.
enum class RemovalErrorKind {
    VersionNotFound,
    AmbiguousVersion,
    AsymmetricEdge,
    SelectionInvalid,
    ProviderRequired,
    ProviderMismatch,
    ProviderVersionNotFound,
    VersionMismatch,
};

struct RemovalError {
    RemovalErrorKind kind { RemovalErrorKind::VersionNotFound };
    std::string target;
    std::string version;
    std::string peerTarget;
    std::string peerVersion;
    std::string message;
};

std::expected<std::string, RemovalError>
resolve_exact_version_key(const VersionDB& db,
                          const std::string& target,
                          const std::string& version) {
    auto it = db.find(target);
    if (it == db.end() || version.empty()) {
        return std::unexpected(RemovalError{
            .kind = RemovalErrorKind::VersionNotFound,
            .target = target,
            .version = version,
            .message = "removal version is missing",
        });
    }

    if (version.find(':') != std::string::npos) {
        if (it->second.versions.contains(version)) return version;
        return std::unexpected(RemovalError{
            .kind = RemovalErrorKind::VersionNotFound,
            .target = target,
            .version = version,
            .message = "exact removal version is not registered",
        });
    }

    std::vector<std::string> matches;
    for (const auto& [storedVersion, _] : it->second.versions) {
        if (strip_namespace(storedVersion) == version) {
            matches.push_back(storedVersion);
        }
    }
    if (matches.size() == 1) return matches.front();
    if (matches.empty()) {
        return std::unexpected(RemovalError{
            .kind = RemovalErrorKind::VersionNotFound,
            .target = target,
            .version = version,
            .message = "removal version is not registered",
        });
    }
    return std::unexpected(RemovalError{
        .kind = RemovalErrorKind::AmbiguousVersion,
        .target = target,
        .version = version,
        .message = std::format(
            "bare removal version '{}' matches {} stored versions",
            version, matches.size()),
    });
}

std::expected<std::string, RemovalError>
remove_version(VersionDB& db,
               const std::string& target,
               const std::string& version) {
    auto exactVersionResult =
        resolve_exact_version_key(db, target, version);
    if (!exactVersionResult) {
        return std::unexpected(std::move(exactVersionResult.error()));
    }
    const auto& exactVersion = *exactVersionResult;

    auto it = db.find(target);
    auto& vers = it->second.versions;

    std::vector<std::pair<std::string, std::string>> edges;
    for (const auto& [peerTarget, versions] : it->second.bindings) {
        if (auto edgeIt = versions.find(exactVersion);
            edgeIt != versions.end()) {
            edges.emplace_back(peerTarget, edgeIt->second);
        }
    }

    for (const auto& [peerTarget, peerVersion] : edges) {
        auto peerIt = db.find(peerTarget);
        const auto reciprocal =
            peerIt != db.end()
            && peerIt->second.versions.contains(peerVersion)
            && peerIt->second.bindings.contains(target)
            && peerIt->second.bindings.at(target).contains(peerVersion)
            && peerIt->second.bindings.at(target).at(peerVersion)
                == exactVersion;
        if (!reciprocal) {
            return std::unexpected(RemovalError{
                .kind = RemovalErrorKind::AsymmetricEdge,
                .target = target,
                .version = exactVersion,
                .peerTarget = peerTarget,
                .peerVersion = peerVersion,
                .message = "removal binding edge is not reciprocal",
            });
        }
    }

    for (const auto& [peerTarget, peerInfo] : db) {
        auto incomingIt = peerInfo.bindings.find(target);
        if (incomingIt == peerInfo.bindings.end()) continue;
        for (const auto& [peerVersion, targetVersion] : incomingIt->second) {
            if (targetVersion != exactVersion) continue;
            const auto reciprocal =
                peerInfo.versions.contains(peerVersion)
                && it->second.bindings.contains(peerTarget)
                && it->second.bindings.at(peerTarget).contains(exactVersion)
                && it->second.bindings.at(peerTarget).at(exactVersion)
                    == peerVersion;
            if (!reciprocal) {
                return std::unexpected(RemovalError{
                    .kind = RemovalErrorKind::AsymmetricEdge,
                    .target = target,
                    .version = exactVersion,
                    .peerTarget = peerTarget,
                    .peerVersion = peerVersion,
                    .message = "incoming removal binding edge is not reciprocal",
                });
            }
        }
    }

    for (const auto& [peerTarget, peerVersion] : edges) {
        auto bindingIt = it->second.bindings.find(peerTarget);
        bindingIt->second.erase(exactVersion);
        if (bindingIt->second.empty()) {
            it->second.bindings.erase(bindingIt);
        }

        auto peerIt = db.find(peerTarget);
        if (peerIt == db.end()) continue;
        auto reverseIt = peerIt->second.bindings.find(target);
        if (reverseIt == peerIt->second.bindings.end()) continue;
        reverseIt->second.erase(peerVersion);
        if (reverseIt->second.empty()) {
            peerIt->second.bindings.erase(reverseIt);
        }
    }

    vers.erase(exactVersion);
    if (vers.empty() && it->second.bindings.empty()) db.erase(it);
    return exactVersion;
}

// Pick the highest semver key from a version map (descending by dotted numeric components,
// then by component count). Namespace prefix is stripped before comparison, but the
// original key is returned so callers can write it back to the workspace as-is.
// Returns empty string if the map is empty.
// Order two version keys highest-first: dotted numeric components descending,
// then the longer component list, then the raw key as a total tiebreak.
//
// The raw-key tiebreak is what makes this a strict weak ordering. Without it
// two keys that differ only by namespace ("15.1.0" vs "musl:15.1.0") compare
// equal in both directions, so which one a sort leaves in front depends on the
// input order. Callers that pick "the highest" would then return different
// answers for the same set depending on how it happened to be built.
bool version_key_greater(const std::string& lhs, const std::string& rhs) {
    const auto split = [](const std::string& s) {
        std::vector<std::string> parts;
        std::istringstream iss(s);
        std::string part;
        while (std::getline(iss, part, '.')) parts.push_back(part);
        return parts;
    };
    const auto pa = split(strip_namespace(lhs));
    const auto pb = split(strip_namespace(rhs));
    for (std::size_t i = 0; i < std::min(pa.size(), pb.size()); ++i) {
        int na = 0, nb = 0;
        std::from_chars(pa[i].data(), pa[i].data() + pa[i].size(), na);
        std::from_chars(pb[i].data(), pb[i].data() + pb[i].size(), nb);
        if (na != nb) return na > nb;
    }
    if (pa.size() != pb.size()) return pa.size() > pb.size();
    return lhs < rhs;
}

std::string pick_highest_version(const std::map<std::string, VData>& versions) {
    if (versions.empty()) return {};

    std::vector<std::string> keys;
    keys.reserve(versions.size());
    for (auto& [k, _] : versions) keys.push_back(k);

    std::ranges::sort(keys, version_key_greater);

    return keys.front();
}

// Fuzzy version match: "15" -> "15.1.0", "ns:14.2" -> "ns:14.2.0"
// Matching priority:
//   1. Exact match
//   2. If namespace specified: prefix-match only within that namespace
//   3. If no namespace: prefer bare (unqualified) versions, then fallback to any namespace
std::string match_version(const VersionDB& db,
                          const std::string& target,
                          const std::string& prefix) {
    auto it = db.find(target);
    if (it == db.end()) return {};

    auto& versions = it->second.versions;

    // Exact match first
    if (versions.contains(prefix)) return prefix;

    auto [input_ns, input_ver] = parse_ns_version(prefix);

    auto split = [](const std::string& s) -> std::vector<std::string> {
        std::vector<std::string> parts;
        std::istringstream iss(s);
        std::string part;
        while (std::getline(iss, part, '.')) {
            parts.push_back(part);
        }
        return parts;
    };

    auto sort_desc = [&](std::vector<std::string>& candidates) {
        std::ranges::sort(candidates, [&](const std::string& a, const std::string& b) {
            auto pa = split(strip_namespace(a));
            auto pb = split(strip_namespace(b));
            for (std::size_t i = 0; i < std::min(pa.size(), pb.size()); ++i) {
                int na = 0, nb = 0;
                std::from_chars(pa[i].data(), pa[i].data() + pa[i].size(), na);
                std::from_chars(pb[i].data(), pb[i].data() + pb[i].size(), nb);
                if (na != nb) return na > nb;
            }
            return pa.size() > pb.size();
        });
    };

    auto input_parts = split(input_ver);

    auto prefix_matches = [&](const std::string& bare_ver) -> bool {
        auto ver_parts = split(bare_ver);
        if (ver_parts.size() < input_parts.size()) return false;
        for (std::size_t i = 0; i < input_parts.size(); ++i) {
            if (input_parts[i] != ver_parts[i]) return false;
        }
        return true;
    };

    // If namespace specified, only match within that namespace
    if (!input_ns.empty()) {
        std::vector<std::string> candidates;
        for (auto& [ver, _] : versions) {
            auto [ver_ns, bare_ver] = parse_ns_version(ver);
            if (ver_ns == input_ns && prefix_matches(bare_ver))
                candidates.push_back(ver);
        }
        if (candidates.empty()) return {};
        sort_desc(candidates);
        return candidates[0];
    }

    // No namespace: prefer bare (unqualified) versions first
    std::vector<std::string> bare_candidates;
    std::vector<std::string> ns_candidates;
    for (auto& [ver, _] : versions) {
        auto [ver_ns, bare_ver] = parse_ns_version(ver);
        if (prefix_matches(bare_ver)) {
            if (ver_ns.empty())
                bare_candidates.push_back(ver);
            else
                ns_candidates.push_back(ver);
        }
    }

    if (!bare_candidates.empty()) {
        sort_desc(bare_candidates);
        return bare_candidates[0];
    }
    if (!ns_candidates.empty()) {
        sort_desc(ns_candidates);
        return ns_candidates[0];
    }
    return {};
}

// Get the active version for a target from a workspace
std::string get_active_version(const Workspace& workspace,
                               const std::string& target) {
    auto it = workspace.find(target);
    return it != workspace.end() ? it->second : std::string{};
}

// Check if a target has any versions registered
bool has_target(const VersionDB& db, const std::string& target) {
    return db.contains(target);
}

// Check if a specific version exists
bool has_version(const VersionDB& db,
                 const std::string& target,
                 const std::string& version) {
    auto it = db.find(target);
    if (it == db.end()) return false;
    return it->second.versions.contains(version);
}

// Get all version strings for a target
std::vector<std::string> get_all_versions(const VersionDB& db,
                                          const std::string& target) {
    std::vector<std::string> result;
    auto it = db.find(target);
    if (it == db.end()) return result;
    for (auto& [ver, _] : it->second.versions) {
        result.push_back(ver);
    }
    return result;
}

// Get VData for a specific version
const VData* get_vdata(const VersionDB& db,
                       const std::string& target,
                       const std::string& version) {
    auto it = db.find(target);
    if (it == db.end()) return nullptr;
    auto vit = it->second.versions.find(version);
    if (vit == it->second.versions.end()) return nullptr;
    return &vit->second;
}

// Get VInfo for a target
const VInfo* get_vinfo(const VersionDB& db, const std::string& target) {
    auto it = db.find(target);
    if (it == db.end()) return nullptr;
    return &it->second;
}

// Both accessors above hand back a pointer INTO the map, so passing a
// temporary map is a use-after-free every time -- the map dies at the end of
// the full expression and the caller is left holding a pointer into freed
// storage.
//
// That is not hypothetical. `Config::versions()` and
// `Config::effective_workspace()` return BY VALUE (global and project state
// are merged into a fresh map), and
//
//     const auto* vinfo = xvm::get_vinfo(Config::versions(), name);
//
// compiled cleanly, shipped in three releases, and SIGSEGV'd in the field
// (#432) when the next line walked the freed map and read a stack address as
// a string length.
//
// A `const&` parameter binds to a temporary, so nothing stopped it. Deleting
// the rvalue overload does: the same line is now a compile error at every
// call site, present and future. `const VersionDB&&` rather than
// `VersionDB&&` so a const temporary is caught too.
//
// The point fix in #432 corrected one call site; this is what makes the class
// of bug unwritable instead of a comment someone has to remember to read.
const VData* get_vdata(const VersionDB&&, const std::string&,
                       const std::string&) = delete;
const VInfo* get_vinfo(const VersionDB&&, const std::string&) = delete;

// Get binding for a target (e.g., g++ -> g++-15 for gcc version 15.1.0)
std::string get_binding(const VersionDB& db,
                        const std::string& target,
                        const std::string& binding_name,
                        const std::string& version) {
    auto it = db.find(target);
    if (it == db.end()) return {};
    auto bit = it->second.bindings.find(binding_name);
    if (bit == it->second.bindings.end()) return {};
    auto vit = bit->second.find(version);
    if (vit == bit->second.end()) return {};
    return vit->second;
}

// ── subos-relative alias/env normalization ───────────────────────────
//
// `gcc.lua` bakes `--sysroot=<subos_sysrootdir()>` -- the ABSOLUTE path of
// whichever subos was active at install time -- into the alias. The versions
// DB is shared by the entire home and `subos use` rewrites nothing, so that
// path outlives every switch: the user switches to `default` and their g++
// keeps compiling against `dev-hello`, against a sysroot that may not even
// exist any more. Re-point such paths at the subos THIS process resolves to,
// at the moment the alias is executed. Doing it here rather than at install
// time is what makes existing homes correct without a reinstall.
//
// Only provably-ours paths are touched: the segment before /subos/ must be the
// home itself, or end in `.xlings` (which is how a PROJECT subos --
// <projectDir>/.xlings/subos/<name>, not under homeDir at all -- is caught).
// A user's own /opt/subos/foo, the flags around the path, and all quoting come
// through byte-identical.
std::string normalize_subos_paths(const std::string& text,
                                  const std::string& xlings_home,
                                  const std::string& active_subos_dir) {
    if (active_subos_dir.empty() || text.empty()) return text;

    static constexpr std::string_view kPosix = "/subos/";
    static constexpr std::string_view kWin   = "\\subos\\";

    auto is_sep = [](char c) { return c == '/' || c == '\\'; };
    // Where a path token can start: whitespace, plus the punctuation that
    // glues a path onto a flag (`--sysroot=`, `PATH=a:b`, quotes).
    auto is_boundary = [](char c) {
        return c == ' ' || c == '\t' || c == '=' || c == ':' || c == ';'
            || c == ',' || c == '"' || c == '\'';
    };
    auto is_alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    auto path_equal = [](std::string_view a, std::string_view b) {
#if defined(_WIN32)
        // Windows compares paths case-insensitively and treats / and \ alike.
        if (a.size() != b.size()) return false;
        auto fold = [](char c) -> char {
            if (c == '\\') return '/';
            if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
            return c;
        };
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (fold(a[i]) != fold(b[i])) return false;
        }
        return true;
#else
        return a == b;
#endif
    };

    std::string out;
    out.reserve(text.size());
    std::size_t cursor = 0;

    while (cursor < text.size()) {
        auto p1 = text.find(kPosix, cursor);
        auto p2 = text.find(kWin, cursor);
        auto hit = std::min(p1, p2);   // npos is max, so min() picks the real one
        if (hit == std::string::npos) break;

        // Widen left to the start of the path token.
        std::size_t start = hit;
        while (start > cursor && !is_boundary(text[start - 1])) --start;
        // ':' is a boundary (PATH=a:b), which would cut the drive letter off
        // `--sysroot=C:\Users\...`: the prefix becomes `\Users\u\.xlings` and
        // the replacement splices a second drive spec onto the surviving `C:`.
        // Step back over a drive letter that is itself token-initial.
        if (start >= cursor + 2 && text[start - 1] == ':'
            && is_alpha(text[start - 2])
            && (start < cursor + 3 || is_boundary(text[start - 3]))) {
            start -= 2;
        }
        // A flag can sit flush against the path with no boundary between them
        // (`-B/h/.xlings/subos/a`), and the walk above happily swallows it.
        // Skip forward to where the path actually begins -- a separator, or a
        // drive letter -- so the replacement never eats the flag.
        const bool drive =
            start + 1 < hit && is_alpha(text[start]) && text[start + 1] == ':';
        if (!drive) {
            while (start < hit && !is_sep(text[start])) {
                if (start + 1 < hit && is_alpha(text[start])
                    && text[start + 1] == ':') {
                    break;   // `-BC:\...`
                }
                ++start;
            }
        }
        std::string_view prefix(text.data() + start, hit - start);

        // Right: the subos NAME segment only. Everything after it (`/usr/
        // include`, …) is the caller's business and is preserved.
        std::size_t nameStart = hit + kPosix.size();
        std::size_t nameEnd = nameStart;
        while (nameEnd < text.size()
               && !is_sep(text[nameEnd]) && !is_boundary(text[nameEnd])) {
            ++nameEnd;
        }

        const bool ours =
            path_equal(prefix, xlings_home)
            || (prefix.size() >= 7
                && path_equal(prefix.substr(prefix.size() - 7), ".xlings"));

        if (!ours || nameEnd == nameStart) {
            out.append(text, cursor, nameEnd - cursor);   // passthrough
        } else {
            out.append(text, cursor, start - cursor);
            out.append(active_subos_dir);
        }
        cursor = nameEnd;
    }
    out.append(text, cursor, std::string::npos);
    return out;
}

// The marker a recipe writes when it needs the ACTIVE subos, and its
// expansion at execution time.
//
// `${XLINGS_HOME}` (expand_path, below) has always been how this codebase
// keeps a home-relative path out of a stored record. This is the same
// mechanism one level down, and it exists for the same reason: the versions
// database is shared by every subos in the home, so a value naming one of
// them is right for the subos that installed the package and wrong for all
// the others -- the rule VData::fileSrc/fileDst has carried since the `files`
// kind was added.
//
// **The core knows this marker and nothing else.** It does not know
// `--sysroot`, `-isysroot` or `--gcc-toolchain=`; how a tool wants to be told
// is the recipe's business, and a new toolchain with a new spelling needs no
// change here. What xlings owns is the dictionary of runtime FACTS a recipe
// can reference by name; what a recipe owns is the syntax it renders them
// into. An earlier attempt inverted that -- registration recognised
// `--sysroot` and the shim re-emitted it -- which put one compiler's flag
// inside a generic version manager and still did nothing for `envs`.
//
// Measured before choosing: of 184 `xvm.add` calls in the package index,
// exactly one (gcc.lua) puts a subos path into an alias. musl-gcc.lua also
// writes absolute paths there, but they point into the PAYLOAD and are
// therefore correct from every subos -- which is why the rule is "no concrete
// SUBOS path", not "no absolute path".
inline constexpr std::string_view kSubosPlaceholder =
    "${XLINGS_DYNAMIC_SUBOS_DIR}";

// Rewrite every concrete subos path of this home to the marker.
//
// A REPAIR primitive, not an ingestion filter: registration stores what the
// recipe wrote, verbatim. This is what `self doctor --fix` applies to a
// record that pinned one subos, and what the detection side asks ("would this
// change?") so that detection and repair cannot drift.
//
// Implemented as normalize_subos_paths with the marker as the destination --
// literally the same traversal, so the two can never disagree about which
// paths are ours.
std::string pin_subos_paths(const std::string& text,
                            const std::string& xlings_home) {
    return normalize_subos_paths(text, xlings_home,
                                 std::string(kSubosPlaceholder));
}

std::string expand_subos_placeholder(const std::string& text,
                                     const std::string& subos_dir) {
    if (subos_dir.empty() || text.empty()) return text;
    std::string result = text;
    std::size_t pos = 0;
    while ((pos = result.find(kSubosPlaceholder, pos)) != std::string::npos) {
        result.replace(pos, kSubosPlaceholder.size(), subos_dir);
        pos += subos_dir.size();
    }
    return result;
}

// Expand ${XLINGS_HOME} in a path string
std::string expand_path(const std::string& path, const std::string& xlings_home) {
    std::string result = path;
    const std::string placeholder = "${XLINGS_HOME}";
    std::size_t pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
        result.replace(pos, placeholder.size(), xlings_home);
        pos += xlings_home.size();
    }
    return result;
}

// --- JSON serialization ---

nlohmann::json vdata_to_json(const VData& vdata) {
    nlohmann::json j;
    j["path"] = vdata.path;
    if (!vdata.kind.empty()) j["kind"] = vdata.kind;
    if (!vdata.sourceName.empty()) j["sourceName"] = vdata.sourceName;
    if (!vdata.destinationName.empty())
        j["destinationName"] = vdata.destinationName;
    if (!vdata.fileSrc.empty()) j["fileSrc"] = vdata.fileSrc;
    if (!vdata.fileDst.empty()) j["fileDst"] = vdata.fileDst;
    if (!vdata.includedir.empty()) j["includedir"] = vdata.includedir;
    if (!vdata.libdir.empty()) j["libdir"] = vdata.libdir;
    if (!vdata.alias.empty()) {
        j["alias"] = vdata.alias;
    }
    if (!vdata.envs.empty()) {
        nlohmann::json envs_j = nlohmann::json::object();
        for (auto it = vdata.envs.begin(); it != vdata.envs.end(); ++it) {
            envs_j[it->first] = it->second;
        }
        j["envs"] = envs_j;
    }
    if (vdata.bindingGroup) {
        j["bindingGroup"] = {
            {"provider", vdata.bindingGroup->provider},
            {"version", vdata.bindingGroup->providerVersion},
            {"group", vdata.bindingGroup->group},
            {"rootTarget", vdata.bindingGroup->rootTarget},
            {"rootVersion", vdata.bindingGroup->rootVersion},
        };
    }
    if (vdata.bindingMembersDeclared || !vdata.bindingMembers.empty()) {
        nlohmann::json members = nlohmann::json::object();
        for (const auto& [target, version] : vdata.bindingMembers) {
            members[target] = version;
        }
        j["bindingMembers"] = std::move(members);
    }
    if (vdata.bindingHeadersDeclared || !vdata.bindingHeaders.empty()) {
        nlohmann::json headers = nlohmann::json::array();
        for (const auto& header : vdata.bindingHeaders) {
            headers.push_back({
                {"sourceDir", header.sourceDir},
                {"destinationPrefix", header.destinationPrefix},
            });
        }
        j["bindingHeaders"] = std::move(headers);
    }
    if (!vdata.bindingIntegrityIssues.empty()) {
        nlohmann::json issues = nlohmann::json::array();
        for (const auto& issue : vdata.bindingIntegrityIssues) {
            issues.push_back({
                {"code", issue.code},
                {"path", issue.path},
            });
        }
        j["bindingIntegrityIssues"] = std::move(issues);
    }
    // Preserved originals win over the reconstructed view.
    //
    // For a field we could not read, the parsed model holds at best a subset
    // -- the members that happened to be well-formed, a group with the
    // unreadable keys blanked. Writing that back is the silent rewrite this
    // whole mechanism exists to stop, so the original text is restored last
    // and overwrites whatever the blocks above produced for that key.
    for (const auto& [field, text] : vdata.bindingUnreadable) {
        try {
            j[field] = nlohmann::json::parse(text);
        } catch (const std::exception&) {
            // Unreachable in practice: the text came from dump(). If it ever
            // is, keeping the reconstructed value beats dropping the field.
        }
    }
    return j;
}

VData vdata_from_json(const nlohmann::json& j) {
    VData vdata;
    const auto recordIssue =
        [&vdata](std::string code, std::string path) {
            const auto duplicate = std::ranges::any_of(
                vdata.bindingIntegrityIssues,
                [&](const BindingIntegrityIssue& issue) {
                    return issue.code == code && issue.path == path;
                });
            if (duplicate) return;
            vdata.bindingIntegrityIssues.push_back({
                .code = std::move(code),
                .path = std::move(path),
            });
        };
    const auto pointerToken = [](std::string_view token) {
        std::string escaped;
        escaped.reserve(token.size());
        for (char ch : token) {
            if (ch == '~') {
                escaped += "~0";
            } else if (ch == '/') {
                escaped += "~1";
            } else {
                escaped += ch;
            }
        }
        return escaped;
    };

    if (j.contains("path") && j["path"].is_string())
        vdata.path = j["path"].get<std::string>();
    if (j.contains("kind") && j["kind"].is_string())
        vdata.kind = j["kind"].get<std::string>();
    if (j.contains("sourceName") && j["sourceName"].is_string())
        vdata.sourceName = j["sourceName"].get<std::string>();
    if (j.contains("destinationName") && j["destinationName"].is_string())
        vdata.destinationName = j["destinationName"].get<std::string>();
    if (j.contains("fileSrc") && j["fileSrc"].is_string())
        vdata.fileSrc = j["fileSrc"].get<std::string>();
    if (j.contains("fileDst") && j["fileDst"].is_string())
        vdata.fileDst = j["fileDst"].get<std::string>();
    if (j.contains("includedir") && j["includedir"].is_string())
        vdata.includedir = j["includedir"].get<std::string>();
    if (j.contains("libdir") && j["libdir"].is_string())
        vdata.libdir = j["libdir"].get<std::string>();
    if (j.contains("alias") && j["alias"].is_array()) {
        for (auto& a : j["alias"]) {
            if (a.is_string()) vdata.alias.push_back(a.get<std::string>());
        }
    }
    if (j.contains("envs") && j["envs"].is_object()) {
        auto& envs = j["envs"];
        for (auto it = envs.begin(); it != envs.end(); ++it) {
            if (it.value().is_string())
                vdata.envs[it.key()] = it.value().get<std::string>();
        }
    }
    if (j.contains("bindingIntegrityIssues")) {
        if (!j["bindingIntegrityIssues"].is_array()) {
            recordIssue(
                "binding-integrity-issues-not-array",
                "/bindingIntegrityIssues");
        } else {
            std::size_t index = 0;
            for (const auto& issueJson : j["bindingIntegrityIssues"]) {
                const auto issuePath =
                    "/bindingIntegrityIssues/" + std::to_string(index++);
                if (!issueJson.is_object()) {
                    recordIssue(
                        "binding-integrity-issue-not-object",
                        issuePath);
                    continue;
                }

                const auto validCode =
                    issueJson.contains("code")
                    && issueJson["code"].is_string()
                    && !issueJson["code"].get_ref<
                        const std::string&>().empty();
                const auto validPath =
                    issueJson.contains("path")
                    && issueJson["path"].is_string()
                    && !issueJson["path"].get_ref<
                        const std::string&>().empty();
                if (!validCode) {
                    recordIssue(
                        "binding-integrity-issue-field-invalid",
                        issuePath + "/code");
                }
                if (!validPath) {
                    recordIssue(
                        "binding-integrity-issue-field-invalid",
                        issuePath + "/path");
                }
                if (validCode && validPath) {
                    recordIssue(
                        issueJson["code"].get<std::string>(),
                        issueJson["path"].get<std::string>());
                }
            }
        }
    }
    if (j.contains("bindingGroup")) {
        if (!j["bindingGroup"].is_object()) {
            recordIssue(
                "binding-group-not-object", "/bindingGroup");
        } else {
            const auto& group = j["bindingGroup"];
            BindingGroupRef ref;
            const auto parseGroupField =
                [&](std::string_view name, std::string& destination) {
                    const auto key = std::string(name);
                    if (group.contains(key)
                        && group[key].is_string()
                        && !group[key].get_ref<
                            const std::string&>().empty()) {
                        destination = group[key].get<std::string>();
                    } else {
                        recordIssue(
                            "binding-group-field-invalid",
                            "/bindingGroup/" + key);
                    }
                };
            parseGroupField("provider", ref.provider);
            parseGroupField("version", ref.providerVersion);
            parseGroupField("group", ref.group);
            parseGroupField("rootTarget", ref.rootTarget);
            parseGroupField("rootVersion", ref.rootVersion);
            vdata.bindingGroup = std::move(ref);
        }
    }
    if (j.contains("bindingMembers")) {
        vdata.bindingMembersDeclared = true;
        if (!j["bindingMembers"].is_object()) {
            recordIssue(
                "binding-members-not-object", "/bindingMembers");
        } else {
            const auto& members = j["bindingMembers"];
            for (auto it = members.begin(); it != members.end(); ++it) {
                const auto validTarget = !it.key().empty();
                if (!validTarget) {
                    recordIssue(
                        "binding-member-target-empty",
                        "/bindingMembers/");
                }
                if (!it.value().is_string()) {
                    recordIssue(
                        "binding-member-version-not-string",
                        "/bindingMembers/" + pointerToken(it.key()));
                } else if (it.value().get_ref<
                               const std::string&>().empty()) {
                    recordIssue(
                        "binding-member-version-empty",
                        "/bindingMembers/" + pointerToken(it.key()));
                } else if (validTarget) {
                    vdata.bindingMembers[it.key()] =
                        it.value().get<std::string>();
                }
            }
        }
    }
    if (j.contains("bindingHeaders")) {
        vdata.bindingHeadersDeclared = true;
        if (!j["bindingHeaders"].is_array()) {
            recordIssue(
                "binding-headers-not-array", "/bindingHeaders");
        } else {
            std::size_t index = 0;
            for (const auto& headerJson : j["bindingHeaders"]) {
                const auto headerPath =
                    "/bindingHeaders/" + std::to_string(index++);
                if (!headerJson.is_object()) {
                    recordIssue("binding-header-not-object", headerPath);
                    continue;
                }
                if (!headerJson.contains("sourceDir")
                    || !headerJson["sourceDir"].is_string()) {
                    recordIssue(
                        "binding-header-source-dir-not-string",
                        headerPath + "/sourceDir");
                    continue;
                }
                if (headerJson["sourceDir"].get_ref<
                        const std::string&>().empty()) {
                    recordIssue(
                        "binding-header-source-dir-empty",
                        headerPath + "/sourceDir");
                    continue;
                }
                if (headerJson.contains("destinationPrefix")
                    && !headerJson["destinationPrefix"].is_string()) {
                    recordIssue(
                        "binding-header-destination-prefix-not-string",
                        headerPath + "/destinationPrefix");
                    continue;
                }

                HeaderAsset header{
                    .sourceDir =
                        headerJson["sourceDir"].get<std::string>(),
                };
                if (headerJson.contains("destinationPrefix")) {
                    header.destinationPrefix =
                        headerJson["destinationPrefix"].get<std::string>();
                }
                vdata.bindingHeaders.push_back(std::move(header));
            }
        }
    }
    if ((j.contains("bindingMembers") || j.contains("bindingHeaders"))
        && !vdata.bindingGroup) {
        recordIssue("binding-group-missing", "/bindingGroup");
    }

    // Keep the original text of every binding field we could not fully read.
    //
    // Without this the next save rewrites the entry from the parsed model and
    // whatever did not fit is gone -- the marker survives, the data does not.
    // That is why `--reset-metadata` could not be offered before: there was
    // nothing left to reset from, and the destruction had already happened
    // silently on the first save after the corruption.
    //
    // Whole-field granularity: a field with one bad member is re-emitted
    // entire, because a partial re-emission is exactly the lossy rewrite this
    // is here to prevent.
    for (const auto& issue : vdata.bindingIntegrityIssues) {
        static constexpr std::string_view fields[] = {
            "bindingGroup", "bindingMembers",
            "bindingHeaders", "bindingIntegrityIssues",
        };
        for (const auto field : fields) {
            const auto prefix = "/" + std::string(field);
            if (!issue.path.starts_with(prefix)) continue;
            // `binding-group-missing` names a field that is not there; there
            // is nothing to preserve and nothing was lost.
            const auto key = std::string(field);
            if (!j.contains(key)) break;
            vdata.bindingUnreadable.emplace(key, j[key].dump());
            break;
        }
    }
    return vdata;
}

nlohmann::json vinfo_to_json(const VInfo& info) {
    nlohmann::json j;
    if (!info.type.empty()) j["type"] = info.type;
    if (!info.filename.empty()) j["filename"] = info.filename;

    nlohmann::json vers = nlohmann::json::object();
    for (auto it = info.versions.begin(); it != info.versions.end(); ++it) {
        vers[it->first] = vdata_to_json(it->second);
    }
    j["versions"] = vers;

    if (!info.bindings.empty()) {
        nlohmann::json binds = nlohmann::json::object();
        for (auto it = info.bindings.begin(); it != info.bindings.end(); ++it) {
            nlohmann::json vermap_j = nlohmann::json::object();
            for (auto vit = it->second.begin(); vit != it->second.end(); ++vit) {
                vermap_j[vit->first] = vit->second;
            }
            binds[it->first] = vermap_j;
        }
        j["bindings"] = binds;
    }

    return j;
}

VInfo vinfo_from_json(const nlohmann::json& j) {
    VInfo info;
    if (j.contains("type") && j["type"].is_string())
        info.type = j["type"].get<std::string>();
    if (j.contains("filename") && j["filename"].is_string())
        info.filename = j["filename"].get<std::string>();
    if (j.contains("versions") && j["versions"].is_object()) {
        auto& vers = j["versions"];
        for (auto it = vers.begin(); it != vers.end(); ++it) {
            info.versions[it.key()] = vdata_from_json(it.value());
        }
    }
    if (j.contains("bindings") && j["bindings"].is_object()) {
        auto& binds = j["bindings"];
        for (auto it = binds.begin(); it != binds.end(); ++it) {
            if (it.value().is_object()) {
                std::map<std::string, std::string> vermap;
                for (auto vit = it.value().begin(); vit != it.value().end(); ++vit) {
                    if (vit.value().is_string())
                        vermap[vit.key()] = vit.value().get<std::string>();
                }
                info.bindings[it.key()] = std::move(vermap);
            }
        }
    }
    return info;
}

nlohmann::json versions_to_json(const VersionDB& db) {
    nlohmann::json j = nlohmann::json::object();
    for (auto it = db.begin(); it != db.end(); ++it) {
        j[it->first] = vinfo_to_json(it->second);
    }
    return j;
}

VersionDB versions_from_json(const nlohmann::json& j) {
    VersionDB db;
    if (!j.is_object()) return db;
    for (auto it = j.begin(); it != j.end(); ++it) {
        db[it.key()] = vinfo_from_json(it.value());
    }
    return db;
}

// Resolve a project-style workspace value (string | platform-conditional
// object) to a single version string. Lifted out of workspace_from_json
// so subos_workspace_from_json can reuse the same fallback semantics
// when it falls through to the form-(3) platform branch. Returns
// std::nullopt when the value has no matching platform key and no
// `default` key — caller decides whether to skip or error.
inline std::optional<std::string>
resolve_platform_workspace_value_(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (!value.is_object()) {
        return std::nullopt;
    }
    if (!platform::OS_NAME.empty()) {
        auto platformIt = value.find(std::string(platform::OS_NAME));
        if (platformIt != value.end() && platformIt->is_string()) {
            return platformIt->get<std::string>();
        }
    }
    if (auto defaultIt = value.find("default");
        defaultIt != value.end() && defaultIt->is_string()) {
        return defaultIt->get<std::string>();
    }
    return std::nullopt;
}

Workspace workspace_from_json(const nlohmann::json& j) {
    Workspace ws;
    if (!j.is_object()) return ws;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (auto resolved = resolve_platform_workspace_value_(it.value())) {
            ws[it.key()] = *resolved;
        }
    }
    return ws;
}

nlohmann::json workspace_to_json(const Workspace& ws) {
    nlohmann::json j = nlohmann::json::object();
    for (auto it = ws.begin(); it != ws.end(); ++it) {
        j[it->first] = it->second;
    }
    return j;
}

// Subos-flavored workspace parser. Accepts three value shapes per target:
//
//   1. string                      (legacy, pre-0.4.19) — active version,
//                                  no installed[] info available
//   2. {active, installed[]}       (current, 0.4.19+)   — both fields
//   3. {linux|windows|macosx|...}  (project-style)      — resolved to a
//                                  single string the same way project files
//                                  resolve it; subos files normally don't
//                                  have this shape but we accept it as a
//                                  graceful fallback so users who ever
//                                  hand-edit a subos file aren't surprised
//
// Disambiguation between (2) and (3) is by reserved-key detection: the
// presence of an "active" or "installed" key marks form (2). This is the
// "Plan 1" disambiguation strategy from the C2 design discussion — `active`
// and `installed` cannot meaningfully be platform names, so the two object
// shapes can never collide.
SubosWorkspace subos_workspace_from_json(const nlohmann::json& j) {
    SubosWorkspace sws;
    if (!j.is_object()) return sws;

    for (auto it = j.begin(); it != j.end(); ++it) {
        const auto& key = it.key();
        const auto& value = it.value();

        if (value.is_string()) {
            // Legacy form: bare version string. installed[] left empty —
            // callers that maintain installed[] (install/remove flow) will
            // populate it on next write, lazily migrating the file.
            sws.active[key] = value.get<std::string>();
            continue;
        }

        if (!value.is_object()) continue;

        // Form (2): C2 object form
        bool hasActive = value.contains("active");
        bool hasInstalled = value.contains("installed");
        if (hasActive || hasInstalled) {
            if (hasActive && value.at("active").is_string()) {
                sws.active[key] = value.at("active").get<std::string>();
            }
            if (hasInstalled && value.at("installed").is_array()) {
                std::vector<std::string> installed;
                for (auto& v : value.at("installed")) {
                    if (v.is_string()) installed.push_back(v.get<std::string>());
                }
                if (!installed.empty()) sws.installed[key] = std::move(installed);
            }
            continue;
        }

        // Form (3): platform-conditional fallback. Reuses the project-style
        // resolver — subos files don't normally carry this shape, but
        // honoring it here means a hand-edited subos file with platform
        // branches behaves the same way the project manifest would.
        if (auto resolved = resolve_platform_workspace_value_(value)) {
            sws.active[key] = *resolved;
        }
    }

    return sws;
}

// Subos workspace serializer. Always emits the C2 object form with
// `active` and `installed` keys. The `installed` set is normalized to
// always include the active version (write-time invariant: an active
// version is implicitly installed). `installed` is omitted from output
// when empty, keeping legacy entries — those that were read in form (1)
// and never had an install/remove since — visually compact.
//
// Stable sort on installed[] so json diffs remain reviewable across runs.
nlohmann::json subos_workspace_to_json(const SubosWorkspace& sws) {
    nlohmann::json j = nlohmann::json::object();

    // Union of all target keys present in either map
    std::set<std::string> keys;
    for (auto& [k, _] : sws.active) keys.insert(k);
    for (auto& [k, _] : sws.installed) keys.insert(k);

    for (const auto& key : keys) {
        std::string active;
        if (auto it = sws.active.find(key); it != sws.active.end()) {
            active = it->second;
        }

        std::vector<std::string> installed;
        if (auto it = sws.installed.find(key); it != sws.installed.end()) {
            installed = it->second;
        }
        if (!active.empty() &&
            std::find(installed.begin(), installed.end(), active) == installed.end()) {
            installed.push_back(active);
        }
        std::sort(installed.begin(), installed.end());

        nlohmann::json entry = nlohmann::json::object();
        if (!active.empty()) entry["active"] = active;
        if (!installed.empty()) {
            entry["installed"] = nlohmann::json::array();
            for (auto& v : installed) entry["installed"].push_back(v);
        }
        j[key] = std::move(entry);
    }

    return j;
}

} // namespace xlings::xvm
