module xlings.core.xvm.shim_view;

import std;

import xlings.libs.json;
import xlings.core.log;
import xlings.platform;
import xlings.core.xvm.types;
import xlings.core.xvm.db;

namespace xlings::xvm {

namespace {

constexpr std::string_view kFormatKey = "format";
constexpr std::string_view kProgramKey = "program";
constexpr std::string_view kFingerprintKey = "fingerprint";
constexpr std::string_view kVinfoKey = "vinfo";
constexpr int kFormat = 1;

// <subos>/.shim-view/<ctxTag>[-]<program>.json
std::filesystem::path shim_view_path(const std::filesystem::path& subosDir,
                                     std::string_view ctxTag,
                                     const std::string& program) {
    namespace fs = std::filesystem;
    // A program name is a command name -- a single path component. Anything
    // carrying a separator is not a name this cache will ever have resolved,
    // and writing it would be a path traversal waiting for a hostile index.
    if (program.empty() || program.find('/') != std::string::npos
        || program.find('\\') != std::string::npos
        || program == "." || program == "..") {
        return {};
    }
    auto name = std::string(ctxTag);
    if (!name.empty()) name += '-';
    name += program;
    name += ".json";
    return subosDir / ".shim-view" / name;
}

std::string stat_to_json_value(const ShimViewStat& s) {
    return std::format("{}:{}", s.size, s.mtime);
}

std::optional<ShimViewStat> stat_from_json_value(const nlohmann::json& j) {
    if (!j.is_string()) return std::nullopt;
    auto text = j.get<std::string>();
    auto colon = text.find(':');
    if (colon == std::string::npos) return std::nullopt;
    ShimViewStat s;
    auto sizeEnd = std::from_chars(text.data(), text.data() + colon, s.size);
    if (sizeEnd.ec != std::errc{}) return std::nullopt;
    auto mtimeEnd = std::from_chars(text.data() + colon + 1,
                                    text.data() + text.size(), s.mtime);
    if (mtimeEnd.ec != std::errc{}) return std::nullopt;
    return s;
}

}  // namespace

std::optional<ShimViewStat> shim_view_stat(const std::filesystem::path& p) {
    std::error_code sec, tec;
    const auto size = std::filesystem::file_size(p, sec);
    const auto mtime = std::filesystem::last_write_time(p, tec);
    if (sec || tec) return std::nullopt;
    return ShimViewStat{size, mtime.time_since_epoch().count()};
}

std::optional<ShimView>
load_shim_view(const std::filesystem::path& subosDir,
               std::string_view ctxTag,
               const std::string& program,
               const std::map<std::string, ShimViewStat>& fingerprint) {
    namespace fs = std::filesystem;
    auto path = shim_view_path(subosDir, ctxTag, program);
    if (path.empty()) return std::nullopt;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return std::nullopt;

    nlohmann::json j;
    try {
        auto content = platform::read_file_to_string(path.string());
        j = nlohmann::json::parse(content, nullptr, false);
    } catch (...) {
        return std::nullopt;
    }
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    if (auto f = j.find(kFormatKey); f == j.end() || !f->is_number_integer()
        || f->get<int>() != kFormat) {
        return std::nullopt;
    }
    if (auto p = j.find(kProgramKey);
        p == j.end() || !p->is_string() || p->get<std::string>() != program) {
        return std::nullopt;
    }

    // The stored fingerprint must name exactly the inputs the caller just
    // stat'ed, with identical identities. Fewer entries = the view was
    // stored from a different context; more = same problem; one mismatched
    // stat = an input changed since. All are misses.
    auto fp = j.find(kFingerprintKey);
    if (fp == j.end() || !fp->is_object() || fp->size() != fingerprint.size()) {
        return std::nullopt;
    }
    for (auto it = fp->begin(); it != fp->end(); ++it) {
        auto expected = fingerprint.find(it.key());
        if (expected == fingerprint.end()) return std::nullopt;
        auto stored = stat_from_json_value(it.value());
        if (!stored || !(*stored == expected->second)) return std::nullopt;
    }

    auto sws = subos_workspace_from_json(j.contains("workspace")
                                             ? j["workspace"]
                                             : nlohmann::json::object());
    ShimView view;
    view.workspace = std::move(sws.active);
    view.installed = std::move(sws.installed);

    auto vi = j.find(kVinfoKey);
    if (vi == j.end() || !vi->is_object()) return std::nullopt;
    if (auto sliceIt = vi->find(program);
        sliceIt != vi->end() && sliceIt->is_object()) {
        view.slice[program] = vinfo_from_json(*sliceIt);
    } else {
        // No entry for the program: a legitimate shape (the name is not in
        // this scope's database -- the tri-state diagnostic path). The view
        // still carries workspace/installed, which is what that path reads.
        // It is ONLY accepted when the caller's own fingerprint says the
        // inputs have not moved, so "absent here" means "absent in the DB".
    }
    return view;
}

void store_shim_view(const std::filesystem::path& subosDir,
                     std::string_view ctxTag,
                     const std::string& program,
                     const std::map<std::string, ShimViewStat>& fingerprint,
                     const ShimView& view) {
    namespace fs = std::filesystem;
    auto path = shim_view_path(subosDir, ctxTag, program);
    if (path.empty()) return;

    nlohmann::json j = nlohmann::json::object();
    j[kFormatKey] = kFormat;
    j[kProgramKey] = program;
    nlohmann::json fp = nlohmann::json::object();
    for (const auto& [file, stat] : fingerprint) {
        fp[file] = stat_to_json_value(stat);
    }
    j[kFingerprintKey] = std::move(fp);

    // The per-target shape subos_workspace_from_json parses -- one entry
    // per target, each carrying `active` and `installed[]`. ONLY the
    // dispatching program's entry is stored: the subos workspace map runs
    // to thousands of targets on a real home (~350 KB), and dispatch reads
    // exactly one of them. The file is keyed by program; a map of one is
    // what its single consumer (get_active_version + the installed[] view
    // in the not-installed diagnostic) can see.
    xlings::xvm::SubosWorkspace sws;
    if (auto it = view.workspace.find(program); it != view.workspace.end()) {
        sws.active[program] = it->second;
    }
    if (auto it = view.installed.find(program); it != view.installed.end()) {
        sws.installed[program] = it->second;
    }
    j["workspace"] = subos_workspace_to_json(std::move(sws));

    nlohmann::json vinfo = nlohmann::json::object();
    if (auto it = view.slice.find(program); it != view.slice.end()) {
        vinfo[program] = vinfo_to_json(it->second);
    }
    j[kVinfoKey] = std::move(vinfo);

    try {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) return;
        platform::write_file_atomic(path.string(), j.dump(2));
    } catch (const std::exception& e) {
        // An accelerator that could not write costs one full resolution on
        // the next dispatch. Nothing else.
        log::debug("shim view: could not store {}: {}", path.string(), e.what());
    }
}

}  // namespace xlings::xvm
