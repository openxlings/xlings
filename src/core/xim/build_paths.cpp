module xlings.core.xim.build_paths;

import std;
import xlings.libs.elfread;

namespace xlings::xim::build_paths {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kExamples = 3;

// The operating system's directories: what a host-anchored payload names by
// design, and nothing a build machine leaves behind.
constexpr std::string_view kSystem[] = {
    "/usr", "/lib", "/lib32", "/lib64", "/libx32", "/etc", "/bin", "/sbin", "/opt", "/var",
    "/run", "/proc", "/sys", "/dev", "/System", "/Library", "/Applications", "/nonexistent",
};

bool under(std::string_view path, std::string_view root) {
    if (root.empty() || !path.starts_with(root)) return false;
    return path.size() == root.size() || path[root.size()] == '/' || root.back() == '/';
}

bool foreign_path(std::string_view path, const std::vector<std::string>& allowed) {
    if (path.size() < 2 || path.front() != '/' || path.find('$') != std::string_view::npos) return false;
    for (const auto root : kSystem)
        if (under(path, root)) return false;
    for (const auto& root : allowed)
        if (under(path, root)) return false;
    return true;
}

// Absolute paths in a pkg-config or libtool file's text: what follows `=`,
// `-L`, `-I`, `-R`, or a quote or space, up to the next space, quote or colon.
std::vector<std::string> paths_in_text(std::string_view text) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '/') continue;
        const char before = i == 0 ? '\n' : text[i - 1];
        const bool starts = before == '=' || before == ' ' || before == '\'' || before == '"' || before == '\n'
            || before == '\t' || before == ':'
            || (i >= 2 && text[i - 2] == '-' && (before == 'L' || before == 'I' || before == 'R'));
        if (!starts) continue;
        std::size_t end = i;
        while (end < text.size() && text[end] != ' ' && text[end] != '\'' && text[end] != '"'
               && text[end] != '\n' && text[end] != '\t' && text[end] != ':' && text[end] != '\r')
            ++end;
        out.emplace_back(text.substr(i, end - i));
        i = end;
    }
    return out;
}

}  // namespace

Report scan(const fs::path& payload, const std::vector<fs::path>& allowedRoots) {
    Report report;
    std::vector<std::string> allowed;
    for (const auto& root : allowedRoots)
        if (!root.empty()) allowed.push_back(fs::path(root).lexically_normal().generic_string());
    allowed.push_back(payload.lexically_normal().generic_string());
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(payload, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code fec;
        if (it->is_symlink(fec) || !it->is_regular_file(fec)) continue;
        const auto& file = it->path();
        const auto ext = file.extension().string();
        std::vector<std::string> named;
        if (ext == ".pc" || ext == ".la") {
            std::ifstream in(file, std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            named = paths_in_text(text.str());
        } else if (const auto info = elfread::read(file)) {
            named = info->searchPaths;
        } else {
            continue;
        }
        std::optional<std::string> first;
        for (const auto& path : named)
            if (foreign_path(path, allowed)) { first = path; break; }
        if (!first) continue;
        ++report.files;
        if (report.examples.size() < kExamples)
            report.examples.push_back({ .file = file.string(), .path = *first });
    }
    return report;
}

std::string describe(const Report& report, const fs::path& payload) {
    if (report.files == 0) return {};
    const auto rel = [&](const std::string& file) {
        const auto r = fs::path(file).lexically_relative(payload);
        return r.empty() ? file : r.generic_string();
    };
    std::string out = std::format("{} file(s) name a path of the machine the payload was built on: {} names {}",
                                  report.files, rel(report.examples.front().file), report.examples.front().path);
    for (std::size_t i = 1; i < report.examples.size(); ++i)
        out += std::format("; {} names {}", rel(report.examples[i].file), report.examples[i].path);
    if (report.files > report.examples.size())
        out += std::format("; and {} more", report.files - report.examples.size());
    return out + " (rule P: a published payload names no build machine path; the index's payload is what to fix)";
}

}  // namespace xlings::xim::build_paths
