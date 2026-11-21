module xlings.core.xim.archives;

import std;
import xlings.core.xim.libxpkg.types.type;
import xlings.core.config;
import xlings.core.diag;
import xlings.core.log;
import xlings.libs.json;
import xlings.libs.sha256;
import xlings.platform;

namespace xlings::xim::archives {

namespace fs = std::filesystem;

namespace {

struct MemoEntry {
    std::uintmax_t size {};
    std::int64_t mtime {};
    std::string sha256;
};

struct Memo {
    fs::path dir;
    bool loaded { false };
    bool dirty { false };
    std::map<std::string, MemoEntry> files;   // key: path relative to dir, '/'-separated
};

struct Seen {
    std::string rel;
    fs::path path;
    std::uintmax_t size {};
    std::int64_t mtime {};
};

// One lock for the sources: download_all asks from several threads, and a
// memo is read, extended and written as one step.
std::mutex& lock_() {
    static std::mutex m;
    return m;
}

std::vector<fs::path>& prepended_() {
    static std::vector<fs::path> dirs;
    return dirs;
}

std::map<fs::path, Memo>& memos_() {
    static std::map<fs::path, Memo> memos;
    return memos;
}

std::string lower_(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

fs::path absolute_(const fs::path& p) {
    std::error_code ec;
    auto abs = fs::absolute(p, ec);
    return (ec ? p : abs).lexically_normal();
}

Memo& memo_(const fs::path& memoFile, const fs::path& dir) {
    auto& memo = memos_()[memoFile];
    if (memo.loaded) return memo;
    memo.loaded = true;
    memo.dir = dir;
    std::error_code ec;
    if (!fs::is_regular_file(memoFile, ec)) return memo;
    try {
        auto doc = nlohmann::json::parse(platform::read_file_to_string(memoFile.string()), nullptr, false);
        if (!doc.is_object() || doc.value("format", 0) != 1 || !doc.contains("files") || !doc["files"].is_object())
            return memo;
        for (auto it = doc["files"].begin(); it != doc["files"].end(); ++it) {
            const auto& v = it.value();
            if (!v.is_object() || !v.contains("sha256") || !v["sha256"].is_string()) continue;
            memo.files[it.key()] = MemoEntry{
                .size = v.value("size", std::uintmax_t{0}),
                .mtime = v.value("mtime", std::int64_t{0}),
                .sha256 = v["sha256"].get<std::string>(),
            };
        }
    } catch (...) {
        // A memo is a cache: one that cannot be read costs a hash, not a failure.
        log::debug("[archives] memo {} unreadable; rebuilding it", memoFile.string());
        memo.files.clear();
    }
    return memo;
}

void save_(const fs::path& memoFile, Memo& memo) {
    if (!memo.dirty) return;
    memo.dirty = false;
    nlohmann::json files = nlohmann::json::object();
    for (const auto& [rel, e] : memo.files)
        files[rel] = {{"size", e.size}, {"mtime", e.mtime}, {"sha256", e.sha256}};
    nlohmann::json doc = {{"format", 1}, {"dir", memo.dir.generic_string()}, {"files", std::move(files)}};
    try {
        std::error_code ec;
        fs::create_directories(memoFile.parent_path(), ec);
        platform::write_file_atomic(memoFile.string(), doc.dump(1));
    } catch (const std::exception& e) {
        // A home that cannot be written (a view, a read-only mount) keeps the
        // memo for this process only.
        log::debug("[archives] memo {} not saved: {}", memoFile.string(), e.what());
    }
}

// Every regular file under `dir`, sorted, without a `.git` tree (an index
// snapshot's history is not an archive).
std::vector<Seen> scan_(const fs::path& dir) {
    std::vector<Seen> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code fec;
        if (it->path().filename() == ".git") {
            if (it->is_directory(fec)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(fec)) continue;
        const auto size = it->file_size(fec);
        if (fec) continue;
        const auto mtime = it->last_write_time(fec);
        if (fec) continue;
        out.push_back({it->path().lexically_relative(dir).generic_string(), it->path(), size,
                       static_cast<std::int64_t>(mtime.time_since_epoch().count())});
    }
    if (ec) log::debug("[archives] walking {} stopped: {}", dir.string(), ec.message());
    std::ranges::sort(out, {}, &Seen::rel);
    return out;
}

std::optional<fs::path> find_in_locked_(const fs::path& dir, std::string_view sha256,
                                        const fs::path& memoFile) {
    const auto want = lower_(sha256);
    if (want.empty()) return std::nullopt;
    auto& memo = memo_(memoFile, dir);
    const auto files = scan_(dir);

    // What is gone is forgotten, so the memo does not grow with every file
    // that ever passed through the directory.
    std::set<std::string> present;
    for (const auto& f : files) present.insert(f.rel);
    std::erase_if(memo.files, [&](const auto& kv) {
        const bool gone = !present.contains(kv.first);
        if (gone) memo.dirty = true;
        return gone;
    });

    const auto known = [&](const Seen& f) -> const MemoEntry* {
        auto it = memo.files.find(f.rel);
        if (it == memo.files.end() || it->second.size != f.size || it->second.mtime != f.mtime) return nullptr;
        return &it->second;
    };
    const auto hash = [&](const Seen& f) {
        auto digest = sha256::hex_file(f.path);
        if (!digest) return std::string{};
        memo.files[f.rel] = MemoEntry{.size = f.size, .mtime = f.mtime, .sha256 = *digest};
        memo.dirty = true;
        return *digest;
    };

    std::optional<fs::path> hit;
    // 1. What is already known: no file is read.
    for (const auto& f : files) {
        if (const auto* e = known(f); e && e->sha256 == want) { hit = f.path; break; }
    }
    // 2. A file named by its content (a bundle's `<sha256>-<name>`): read first.
    for (const auto& f : files) {
        if (hit) break;
        if (!known(f) && f.path.filename().string().starts_with(want) && hash(f) == want) hit = f.path;
    }
    // 3. The rest, until one matches; the others wait for a later question.
    for (const auto& f : files) {
        if (hit) break;
        if (!known(f) && hash(f) == want) hit = f.path;
    }
    save_(memoFile, memo);
    return hit;
}

std::string toml_string_(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
                    out += std::format("\\u{:04X}", static_cast<unsigned>(static_cast<unsigned char>(c)));
                else
                    out += c;
        }
    }
    return out + "\"";
}

void append_utf8_(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) { out += static_cast<char>(cp); return; }
    if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    }
    out += static_cast<char>(0x80 | (cp & 0x3F));
}

// A TOML basic or literal string at the start of `v`; nullopt for any other
// value (a reader ignores what it does not need).
std::optional<std::string> toml_value_string_(std::string_view v) {
    if (v.empty()) return std::nullopt;
    if (v.front() == '\'') {
        const auto end = v.find('\'', 1);
        if (end == std::string_view::npos) return std::nullopt;
        return std::string(v.substr(1, end - 1));
    }
    if (v.front() != '"') return std::nullopt;
    std::string out;
    for (std::size_t i = 1; i < v.size(); ++i) {
        const char c = v[i];
        if (c == '"') return out;
        if (c != '\\') { out += c; continue; }
        if (++i >= v.size()) return std::nullopt;
        switch (v[i]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case 'u':
            case 'U': {
                const std::size_t n = v[i] == 'u' ? 4 : 8;
                if (i + n >= v.size()) return std::nullopt;
                std::uint32_t cp = 0;
                const auto digits = v.substr(i + 1, n);
                auto [ptr, err] = std::from_chars(digits.data(), digits.data() + digits.size(), cp, 16);
                if (err != std::errc{} || ptr != digits.data() + digits.size()) return std::nullopt;
                append_utf8_(out, cp);
                i += n;
                break;
            }
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}

// How many '[' minus ']' a line opens, outside strings and comments: a
// multi-line array's rows are values, not table headers.
int bracket_delta_(std::string_view line) {
    int depth = 0;
    char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote) {
            if (c == '\\' && quote == '"') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '#') break;
        if (c == '"' || c == '\'') quote = c;
        else if (c == '[') ++depth;
        else if (c == ']') --depth;
    }
    return depth;
}

std::string_view trim_(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string header_name_(std::string_view line) {
    std::string out;
    for (const char c : line)
        if (c != ' ' && c != '\t') out += c;
    return out;
}

}  // namespace

std::vector<fs::path> directories() {
    std::vector<fs::path> out;
    const auto add = [&](const fs::path& p) {
        if (p.empty()) return;
        auto abs = absolute_(p);
        if (std::ranges::find(out, abs) == out.end()) out.push_back(std::move(abs));
    };
    {
        std::lock_guard guard(lock_());
        for (const auto& p : prepended_()) add(p);
    }
    if (const char* env = std::getenv("XLINGS_ARCHIVES"); env && *env) {
        std::string_view rest(env);
        while (!rest.empty()) {
            const auto sep = rest.find(platform::PATH_SEPARATOR);
            add(fs::path(std::string(rest.substr(0, sep))));
            if (sep == std::string_view::npos) break;
            rest.remove_prefix(sep + 1);
        }
    }
    for (const auto& p : Config::archives()) add(p);
    return out;
}

void prepend_directory(const fs::path& dir) {
    std::lock_guard guard(lock_());
    auto& dirs = prepended_();
    const auto abs = absolute_(dir);
    std::erase(dirs, abs);
    dirs.insert(dirs.begin(), abs);
}

std::string offline_reason() {
    const char* down = std::getenv("XLINGS_NET_UNREACHABLE");
    return down && *down ? std::string(down) : std::string{};
}

fs::path memo_file_for(const fs::path& dir) {
    const auto key = sha256::hex(absolute_(dir).generic_string()).substr(0, 16);
    return Config::paths().dataDir / "archive-index" / (key + ".json");
}

std::optional<fs::path> find_in(const fs::path& dir, std::string_view sha256, const fs::path& memoFile) {
    std::lock_guard guard(lock_());
    return find_in_locked_(dir, sha256, memoFile);
}

std::optional<Hit> find(std::string_view sha256) {
    static std::set<fs::path> warned;
    for (const auto& dir : directories()) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            std::lock_guard guard(lock_());
            if (warned.insert(dir).second)
                log::warn("archive directory {} is not a directory here; it is skipped", dir.string());
            continue;
        }
        std::lock_guard guard(lock_());
        if (auto file = find_in_locked_(dir, sha256, memo_file_for(dir)))
            return Hit{.file = std::move(*file), .directory = dir};
    }
    return std::nullopt;
}

void forget(const fs::path& file) {
    std::lock_guard guard(lock_());
    for (auto& [memoFile, memo] : memos_()) {
        const auto rel = file.lexically_normal().lexically_relative(memo.dir).generic_string();
        if (rel.empty() || rel.starts_with("..")) continue;
        if (memo.files.erase(rel) > 0) {
            memo.dirty = true;
            save_(memoFile, memo);
        }
    }
}

std::optional<std::string> recorded_sha256(std::string_view package, std::string_view version,
                                           std::string_view host) {
    for (const auto& dir : directories()) {
        auto bundle = read_bundle(dir);
        if (!bundle) {
            log::warn("{}", bundle.error());
            continue;
        }
        if (!*bundle) continue;
        for (const auto& a : (*bundle)->archives) {
            if (a.package == package && a.version == version && a.host == host && !a.sha256.empty())
                return lower_(a.sha256);
        }
    }
    return std::nullopt;
}

std::expected<Bundle, std::string> parse_bundle(std::string_view text) {
    Bundle bundle;
    bundle.format = 0;
    enum class Table { None, Bundle, Archive, Index, Other } table = Table::None;
    int depth = 0;   // open brackets of a multi-line value
    std::size_t lineNo = 0;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const auto raw = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        ++lineNo;
        const auto line = trim_(raw);
        if (depth == 0 && line.starts_with('[')) {
            const auto name = header_name_(line);
            if (name == "[bundle]") table = Table::Bundle;
            else if (name == "[[archive]]") { table = Table::Archive; bundle.archives.emplace_back(); }
            else if (name == "[[index]]") { table = Table::Index; bundle.indexes.emplace_back(); }
            else table = Table::Other;
            if (table == Table::Other) bundle.otherTables += std::string(raw) + "\n";
            continue;
        }
        if (table == Table::Other) {
            bundle.otherTables += std::string(raw) + "\n";
            depth = std::max(0, depth + bracket_delta_(line));
            continue;
        }
        if (depth > 0) {
            depth = std::max(0, depth + bracket_delta_(line));
            continue;
        }
        if (line.empty() || line.starts_with('#')) continue;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos)
            return std::unexpected(std::format("line {}: expected `key = value`", lineNo));
        auto key = trim_(line.substr(0, eq));
        if (key.size() >= 2 && (key.front() == '"' || key.front() == '\'')) key = key.substr(1, key.size() - 2);
        const auto value = trim_(line.substr(eq + 1));
        depth = std::max(0, bracket_delta_(value));
        const auto str = toml_value_string_(value);
        if (table == Table::Bundle) {
            if (key == "format") {
                int v = 0;
                auto [ptr, err] = std::from_chars(value.data(), value.data() + value.size(), v);
                if (err != std::errc{}) return std::unexpected(std::format("line {}: format is not an integer", lineNo));
                bundle.format = v;
            } else if (key == "created_by" && str) bundle.createdBy = *str;
            else if (key == "host" && str) bundle.host = *str;
        } else if (table == Table::Archive && str) {
            auto& a = bundle.archives.back();
            if (key == "package") a.package = *str;
            else if (key == "version") a.version = *str;
            else if (key == "host") a.host = *str;
            else if (key == "sha256") a.sha256 = *str;
            else if (key == "file") a.file = *str;
        } else if (table == Table::Index && str) {
            auto& i = bundle.indexes.back();
            if (key == "name") i.name = *str;
            else if (key == "revision") i.revision = *str;
            else if (key == "path") i.path = *str;
        }
    }
    if (bundle.format == 0) return std::unexpected("no [bundle] format");
    return bundle;
}

std::string render_bundle(const Bundle& bundle) {
    std::string out;
    out += "[bundle]\n";
    out += std::format("format = {}\n", bundle.format);
    out += "created_by = " + toml_string_(bundle.createdBy) + "\n";
    out += "host = " + toml_string_(bundle.host) + "\n";
    for (const auto& a : bundle.archives) {
        out += "\n[[archive]]\n";
        out += "package = " + toml_string_(a.package) + "\n";
        out += "version = " + toml_string_(a.version) + "\n";
        out += "host = " + toml_string_(a.host) + "\n";
        out += "sha256 = " + toml_string_(a.sha256) + "\n";
        out += "file = " + toml_string_(a.file) + "\n";
    }
    for (const auto& i : bundle.indexes) {
        out += "\n[[index]]\n";
        out += "name = " + toml_string_(i.name) + "\n";
        out += "revision = " + toml_string_(i.revision) + "\n";
        out += "path = " + toml_string_(i.path) + "\n";
    }
    if (!bundle.otherTables.empty()) {
        out += "\n";
        out += bundle.otherTables;
    }
    return out;
}

std::expected<std::optional<Bundle>, std::string> read_bundle(const fs::path& dir) {
    const auto file = dir / kBundleManifest;
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return std::optional<Bundle>{};
    std::string text;
    try {
        text = platform::read_file_to_string(file.string());
    } catch (const std::exception& e) {
        return std::unexpected(std::format("{} could not be read: {}", file.string(), e.what()));
    }
    auto bundle = parse_bundle(text);
    if (!bundle) return std::unexpected(std::format("{}: {}", file.string(), bundle.error()));
    if (bundle->format > kBundleFormat)
        return std::unexpected(std::format("{} is format {}, newer than this xlings reads ({}): xlings self update",
                                           file.string(), bundle->format, kBundleFormat));
    return std::optional<Bundle>(std::move(*bundle));
}

std::expected<void, std::string> write_bundle(const fs::path& dir, const Bundle& bundle) {
    try {
        platform::write_file_atomic((dir / kBundleManifest).string(), render_bundle(bundle));
    } catch (const std::exception& e) {
        return std::unexpected(std::format("{} could not be written: {}", (dir / kBundleManifest).string(), e.what()));
    }
    return {};
}

void report_unavailable(std::span<const DownloadTask> tasks, std::span<const DownloadResult> results) {
    std::vector<const DownloadTask*> missing;
    for (const auto& r : results) {
        if (!r.unavailable) continue;
        const auto it = std::ranges::find(tasks, r.name, &DownloadTask::name);
        if (it != tasks.end()) missing.push_back(&*it);
    }
    if (missing.empty()) return;

    std::vector<diag::Fact> facts;
    std::string keys;
    for (const auto* t : missing) {
        const auto key = t->package.empty() ? t->name : t->package + "@" + t->version;
        keys += (keys.empty() ? "" : " ") + key;
        facts.push_back({key, std::format("sha256 {}  {}", t->sha256.empty() ? "(none stated)" : lower_(t->sha256), t->url)});
    }
    const auto reason = offline_reason();
    const auto dirs = directories();
    facts.push_back({"archive directories", dirs.empty() ? std::string("(none)") : [&] {
        std::string joined;
        for (const auto& d : dirs) joined += (joined.empty() ? "" : ", ") + d.string();
        return joined;
    }()});
    diag::emit({
        .code = "xim.archives_unavailable",
        .summary = std::format("{} archive{} in no source, and the network {}", missing.size(),
                               missing.size() == 1 ? " is" : "s are",
                               reason.empty() ? "did not answer" : "is not used (" + reason + ")"),
        .facts = std::move(facts),
        .actions = {
            {"write them on a machine with a network", std::format("xlings download {} --to ./xlings-bundle", keys)},
            {"then install from that directory here", std::format("xlings install --from ./xlings-bundle {}", keys)},
            {"or name a directory that holds them", "xlings config --add-archives ./xlings-bundle"},
        },
    });
}

}  // namespace xlings::xim::archives
