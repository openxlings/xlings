module xlings.core.xvm.types;

import std;

namespace xlings::xvm {

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

std::string effective_kind_of(const VersionDB& db,
                              const std::string& target,
                              const std::string& version) {
    auto it = db.find(target);
    if (it == db.end()) return {};
    auto vit = it->second.versions.find(version);
    if (vit == it->second.versions.end()) return it->second.type;
    return effective_kind(it->second, vit->second);
}

bool kind_can_strand(std::string_view kind) { return kind != "group"; }

bool has_program_kind(const VersionDB& db, const std::string& target) {
    auto it = db.find(target);
    if (it == db.end()) return false;
    if (it->second.versions.empty()) return it->second.type == "program";
    for (const auto& [version, data] : it->second.versions) {
        if (effective_kind(it->second, data) == "program") return true;
    }
    return false;
}

}

 // namespace xlings::xvm

#if !defined(_MSC_VER)

#endif
