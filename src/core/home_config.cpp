module xlings.core.home_config;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.core.log;
import xlings.core.xvm.lock;

namespace xlings {

std::filesystem::path home_config_path(const std::filesystem::path& home) {
    return home / ".xlings.json";
}

namespace {

// The one key a partial capture never materializes.
constexpr std::string_view kVersionsKey = "versions";

// Materialize the top-level keys of an object while lexing only as far as
// the caller needs.
//
// On a real home `versions` is ~90% of the config's bytes (2.4 MB of 3.65,
// 3970 entries) and no shim dispatch ever reads it. AbortAtVersions stops
// the parse when that key appears; SkipVersions lexes the whole file but
// builds nothing under it -- for the CLI, which needs `xim` (sorting after
// `versions`) but not `versions` itself once the DB has its own file.
//
// What makes the abort safe is decided by the CALLER (home_config_capture):
// it is only requested for bytes proven to be xlings's own sorted dump. A
// stream that is sorted SO FAR proves nothing about what follows `versions`
// -- a hand edit appended after it is the common shape -- so the checks
// below (sorted so far, `activeSubos` seen) are defense in depth, not the
// proof. A stream failing them degrades to SkipVersions, which is slower
// and misses nothing.
class SelectiveCaptureSax {
public:
    SelectiveCaptureSax(nlohmann::json& out, bool abortAtVersions)
        : out_(out), abortAtVersions_(abortAtVersions) {}

    bool ok() const { return rootWasObject_ && !failed_; }
    bool truncated() const { return truncated_; }

    bool null() { return scalar_(nlohmann::json(nullptr)); }
    bool boolean(bool v) { return scalar_(nlohmann::json(v)); }
    bool number_integer(nlohmann::json::number_integer_t v) {
        return scalar_(nlohmann::json(v));
    }
    bool number_unsigned(nlohmann::json::number_unsigned_t v) {
        return scalar_(nlohmann::json(v));
    }
    bool number_float(double v, const std::string&) {
        return scalar_(nlohmann::json(v));
    }
    bool string(std::string& v) { return scalar_(nlohmann::json(std::move(v))); }
    bool binary(nlohmann::json::binary_t& v) {
        return scalar_(nlohmann::json(std::move(v)));
    }

    bool start_object(std::size_t) { return start_container_(true); }
    bool start_array(std::size_t) { return start_container_(false); }

    bool key(std::string& k) {
        if (skipping_) return true;
        if (stack_.empty()) {
            if (depth_ == 1) {
                // The abort is DOUBLY guarded. Sortedness alone is not
                // proof: a hand-built file can put `versions` FIRST, and
                // aborting there would miss every key that follows. So the
                // abort additionally requires that `activeSubos` -- the one
                // behavior-critical consumer key, and the alphabetically
                // first -- has already been seen. A file that lacks it
                // entirely never aborts and pays a full lex instead; those
                // are the small, fresh homes, where a lex is nothing.
                if (static_cast<std::string_view>(k) == kVersionsKey) {
                    if (abortAtVersions_ && sortedSoFar_ && sawActiveSubos_) {
                        truncated_ = true;
                        return false;
                    }
                    skipping_ = true;
                    return true;
                }
                if (static_cast<std::string_view>(k) == "activeSubos") {
                    sawActiveSubos_ = true;
                }
                // A key going backwards means the stream is not in the
                // writer's (sorted) order, so "everything before `versions`
                // is captured" no longer holds once we abort. Degrade.
                if (prevTopKey_ && k < *prevTopKey_) sortedSoFar_ = false;
                prevTopKey_ = k;
                topKey_ = std::move(k);
                return true;
            }
            return true;
        }
        nestedKey_ = std::move(k);
        return true;
    }

    bool end_object() { return end_container_(); }
    bool end_array() { return end_container_(); }

    bool parse_error(std::size_t, const std::string&, const nlohmann::json::exception&) {
        failed_ = true;
        return false;
    }

private:
    bool scalar_(nlohmann::json&& v) {
        if (skipping_) return true;
        if (stack_.empty()) {
            if (depth_ == 1) out_[topKey_] = std::move(v);
            return true;
        }
        attach_to_parent_(std::move(v));
        return true;
    }

    bool start_container_(bool isObject) {
        if (depth_ == 0 && isObject) rootWasObject_ = true;
        ++depth_;
        if (skipping_) return true;
        if (stack_.empty()) {
            if (depth_ == 2) {
                auto& slot = out_[topKey_] = isObject ? nlohmann::json::object()
                                                      : nlohmann::json::array();
                stack_.push_back(&slot);
            }
            return true;
        }
        nlohmann::json child = isObject ? nlohmann::json::object()
                                        : nlohmann::json::array();
        attach_to_parent_(std::move(child));
        // attach_to_parent_ placed the child under the parent; hand the
        // parser a pointer into it.
        auto& parent = *stack_.back();
        stack_.push_back(parent.is_array() ? &parent.back() : &parent[nestedKey_]);
        return true;
    }

    bool end_container_() {
        --depth_;
        if (skipping_) {
            if (depth_ == 1) skipping_ = false;
            return true;
        }
        if (depth_ >= 1 && !stack_.empty()) stack_.pop_back();
        return true;
    }

    void attach_to_parent_(nlohmann::json&& v) {
        auto& parent = *stack_.back();
        if (parent.is_array()) {
            parent.push_back(std::move(v));
        } else {
            parent[nestedKey_] = std::move(v);
        }
    }

    nlohmann::json& out_;
    bool abortAtVersions_;
    bool rootWasObject_ { false };
    bool failed_ { false };
    bool truncated_ { false };
    bool skipping_ { false };
    bool sortedSoFar_ { true };
    bool sawActiveSubos_ { false };
    int depth_ { 0 };
    std::string topKey_;
    std::optional<std::string> prevTopKey_;
    std::string nestedKey_;
    std::vector<nlohmann::json*> stack_;
};

// The capture memo: one parse of the home config per mode per process.
struct CaptureMemoEntry {
    std::uintmax_t size {};
    std::filesystem::file_time_type mtime {};
    std::shared_ptr<const HomeConfigCapture> capture;
};

std::mutex& capture_memo_mutex() {
    static std::mutex mu;
    return mu;
}

std::unordered_map<std::string, CaptureMemoEntry>& capture_memo() {
    static std::unordered_map<std::string, CaptureMemoEntry> memo;
    return memo;
}

std::string capture_memo_key(const std::filesystem::path& configPath,
                             HomeCaptureMode mode) {
    return configPath.string() + '\x1f'
         + (mode == HomeCaptureMode::AbortAtVersions ? 'A' : 'B');
}

std::shared_ptr<HomeConfigCapture> capture_from_content(
        const std::string& content, bool abortAtVersions) {
    auto capture = std::make_shared<HomeConfigCapture>();
    try {
        SelectiveCaptureSax sax(capture->json, abortAtVersions);
        nlohmann::json::sax_parse(content, &sax);
        capture->ok = sax.ok();
        capture->truncated = sax.truncated();
    } catch (...) {
        // Same contract as read_home_config: a config that cannot be
        // parsed is "no config", never an error thrown into the caller --
        // the constructor calls this on the ambient home, where a throw
        // would abort a command that only wanted a display path.
        capture->ok = false;
        capture->truncated = false;
    }
    return capture;
}

// Reads `format` and `stamp` from the head of the versions DB file and stops
// at its `versions` key. save_versions dumps the wrapper sorted, so both
// sit in the first few dozen bytes; the 2.4 MB map behind them is never
// read, let alone parsed.
class DbStampSax {
public:
    std::optional<std::string> stamp;
    std::optional<std::int64_t> format;

    bool null() { return true; }
    bool boolean(bool) { return true; }
    bool number_integer(nlohmann::json::number_integer_t v) {
        if (depth_ == 1 && key_ == "format") format = v;
        return true;
    }
    bool number_unsigned(nlohmann::json::number_unsigned_t v) {
        if (depth_ == 1 && key_ == "format")
            format = static_cast<std::int64_t>(v);
        return true;
    }
    bool number_float(double, const std::string&) { return true; }
    bool string(std::string& v) {
        if (depth_ == 1 && key_ == "stamp") stamp = std::move(v);
        return true;
    }
    bool binary(nlohmann::json::binary_t&) { return true; }
    bool start_object(std::size_t) { ++depth_; return true; }
    bool start_array(std::size_t) { ++depth_; return true; }
    bool end_object() { --depth_; return true; }
    bool end_array() { --depth_; return true; }
    bool key(std::string& k) {
        if (depth_ == 1) {
            if (k == "versions") return false;  // everything needed is behind us
            key_ = std::move(k);
        }
        return true;
    }
    bool parse_error(std::size_t, const std::string&,
                     const nlohmann::json::exception&) {
        return false;
    }

private:
    int depth_ { 0 };
    std::string key_;
};

std::optional<std::string> read_versions_db_stamp_prefix(
        const std::filesystem::path& dbPath) {
    constexpr std::size_t kPrefix = 4096;
    std::ifstream in(dbPath, std::ios::binary);
    if (!in) return std::nullopt;
    std::string head(kPrefix, '\0');
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(in.gcount()));
    try {
        DbStampSax sax;
        nlohmann::json::sax_parse(head, &sax);
        if (sax.format && *sax.format == 1 && sax.stamp) return sax.stamp;
    } catch (...) {
    }
    return std::nullopt;
}

}  // namespace

std::shared_ptr<const HomeConfigCapture>
home_config_capture(const std::filesystem::path& configPath,
                    HomeCaptureMode mode) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(configPath, ec) || ec) return nullptr;

    const auto key = capture_memo_key(configPath, mode);
    const auto stat_now = [](const fs::path& p)
        -> std::optional<std::pair<std::uintmax_t, std::filesystem::file_time_type>> {
        std::error_code sec, tec;
        const auto size = fs::file_size(p, sec);
        const auto mtime = fs::last_write_time(p, tec);
        if (sec || tec) return std::nullopt;
        return std::pair{size, mtime};
    };

    if (const auto st = stat_now(configPath)) {
        std::scoped_lock lock(capture_memo_mutex());
        auto it = capture_memo().find(key);
        if (it != capture_memo().end() && it->second.size == st->first
            && it->second.mtime == st->second) {
            return it->second.capture;
        }
    }

    // Read, then re-stat the SAME content's file: if the file changed under
    // us the capture is still what a single read would have produced (the
    // behavior of every pre-existing reader), but it must not be memoized --
    // a later caller validating against the NEW stat would receive a capture
    // of the OLD bytes.
    const auto statBefore = stat_now(configPath);
    auto content = platform::read_file_to_string(configPath.string());
    const auto after = stat_now(configPath);

    // Stopping at `versions` is safe only for bytes xlings itself dumped
    // (sorted, so nothing that sorts before `versions` can follow it). A
    // hand edit or a script may append `"mirror": "CN"` after it, and an
    // early stop would silently drop that key. The proof is the versions DB
    // stamp: only xlings's writers stamp it, only with the stat of a config
    // they just dumped, and only while the DB mirrors it. Without a matching
    // stamp, lex the whole file (SkipVersions) -- slower, misses nothing.
    // The proof is about these bytes, so it holds for the memo entry as
    // long as the config's stat does.
    bool abortAtVersions = false;
    if (mode == HomeCaptureMode::AbortAtVersions && statBefore && after
        && statBefore->first == after->first
        && statBefore->second == after->second) {
        const auto stamp = read_versions_db_stamp_prefix(
            versions_db_path(configPath.parent_path()));
        abortAtVersions =
            stamp && *stamp == versions_db_stamp(after->first, after->second);
    }

    auto capture = capture_from_content(content, abortAtVersions);

    if (const auto before = stat_now(configPath);
        before && after && before->first == after->first
        && before->second == after->second) {
        capture->size = after->first;
        capture->mtime = after->second;
        std::scoped_lock lock(capture_memo_mutex());
        capture_memo()[key] = {after->first, after->second, capture};
    }
    return capture;
}

std::filesystem::path versions_db_path(const std::filesystem::path& home) {
    return home / "data" / "versions.json";
}

std::string versions_db_stamp(const std::uintmax_t size,
                              const std::filesystem::file_time_type mtime) {
    // file_clock's rep is __int128 on libc++; ticks fit int64 for every
    // file that exists, and the value is only compared against another one.
    return std::format("{}:{}",
                       static_cast<std::int64_t>(size),
                       static_cast<std::int64_t>(
                           mtime.time_since_epoch().count()));
}

namespace {

// The DB file's wrapper: format, the freshness stamp of the home config it
// was written beside, and the versions map itself. The stamp is what makes
// the dual-write window safe: a client that does not know the DB file
// exists (every client ≤2026.9.30.1) updates the home config's `versions`
// field and leaves both the stamp and the file behind -- and the reader,
// seeing the config's current stat no longer matches, falls back to the
// config. "Which copy is fresher" is decided by data, not by write order.
constexpr std::string_view kDbFormatKey = "format";
constexpr std::string_view kDbStampKey = "stamp";
constexpr std::string_view kDbVersionsKey = "versions";
constexpr int kDbFormat = 1;

// The memo: one versions read per process, keyed by both files' identities.
// home_knows_program's existence check and Config's version load hit the
// same entry, so an unmigrated home pays one parse per process, not two.
struct VersionsMemoEntry {
    std::uintmax_t configSize {};
    std::filesystem::file_time_type configMtime {};
    std::uintmax_t dbSize {};
    std::filesystem::file_time_type dbMtime {};
    bool dbExists { false };
    std::shared_ptr<const nlohmann::json> versions;
};

std::mutex& versions_memo_mutex() {
    static std::mutex mu;
    return mu;
}

std::unordered_map<std::string, VersionsMemoEntry>& versions_memo() {
    static std::unordered_map<std::string, VersionsMemoEntry> memo;
    return memo;
}

}  // namespace

std::optional<nlohmann::json> load_versions_json(const std::filesystem::path& home) {
    namespace fs = std::filesystem;
    const auto dbPath = versions_db_path(home);
    const auto configPath = home_config_path(home);
    std::error_code ec;

    const auto configStat = [&]() -> std::optional<
        std::pair<std::uintmax_t, std::filesystem::file_time_type>> {
        if (!fs::is_regular_file(configPath, ec) || ec) return std::nullopt;
        std::error_code sec, tec;
        const auto size = fs::file_size(configPath, sec);
        const auto mtime = fs::last_write_time(configPath, tec);
        if (sec || tec) return std::nullopt;
        return std::pair{size, mtime};
    }();
    const bool dbExists = fs::is_regular_file(dbPath, ec) && !ec;
    const auto dbStat = [&]() -> std::optional<
        std::pair<std::uintmax_t, std::filesystem::file_time_type>> {
        if (!dbExists) return std::nullopt;
        std::error_code sec, tec;
        const auto size = fs::file_size(dbPath, sec);
        const auto mtime = fs::last_write_time(dbPath, tec);
        if (sec || tec) return std::nullopt;
        return std::pair{size, mtime};
    }();

    if (configStat && dbStat) {
        std::scoped_lock lock(versions_memo_mutex());
        auto it = versions_memo().find(home.string());
        if (it != versions_memo().end()
            && it->second.configSize == configStat->first
            && it->second.configMtime == configStat->second
            && it->second.dbExists && it->second.dbSize == dbStat->first
            && it->second.dbMtime == dbStat->second) {
            return std::optional<nlohmann::json>{*it->second.versions};
        }
    }

    std::optional<nlohmann::json> result;
    std::optional<std::pair<std::uintmax_t, std::filesystem::file_time_type>>
        stampedConfig;

    if (dbExists) {
        try {
            auto content = platform::read_file_to_string(dbPath.string());
            auto json = nlohmann::json::parse(content, nullptr, false);
            if (!json.is_discarded() && json.is_object()
                && json.value(kDbFormatKey, 0) == kDbFormat
                && json.contains(kDbStampKey) && json[kDbStampKey].is_string()
                && json.contains(kDbVersionsKey)
                && json[kDbVersionsKey].is_object()) {
                // Trust the file only while the home config it was written
                // beside still has the stat the stamp recorded. A client
                // that never heard of this file (or a hand edit) changed
                // the config since, and the config is the fresher copy.
                if (configStat) {
                    const auto expected = json[kDbStampKey].get<std::string>();
                    if (expected == versions_db_stamp(configStat->first,
                                                    configStat->second)) {
                        result = json[kDbVersionsKey];
                        stampedConfig = configStat;
                    }
                }
            } else {
                // debug, not warn: a shim runs this once per tool invocation,
                // and a warning here would land in every wrapped tool's
                // stderr. `self doctor` is where a broken DB is reported.
                log::debug("{} is malformed; falling back to the versions in {}",
                          dbPath.string(), configPath.string());
            }
        } catch (const std::exception& e) {
            log::debug("could not read {} ({}); falling back to the versions in {}",
                      dbPath.string(), e.what(), configPath.string());
        }
    }
    if (!result) {
        // The home config: three cases, not two. A config that is ABSENT is
        // an observed empty database (a fresh home's legitimate shape). A
        // config that exists but cannot be parsed is UNOBSERVED -- nullopt
        // -- because the consumers that refuse on nullopt must keep
        // refusing: profile.cpp's payload collector treats "could not read"
        // as "references unknown", and treating it as empty would list live
        // payloads for removal.
        if (!fs::is_regular_file(configPath, ec) || ec) {
            result = nlohmann::json::object();
        } else {
            try {
                auto content = platform::read_file_to_string(configPath.string());
                auto doc = nlohmann::json::parse(content, nullptr, false);
                if (doc.is_discarded() || !doc.is_object()) return std::nullopt;
                if (auto it = doc.find(kVersionsKey);
                    it != doc.end() && it->is_object()) {
                    result = *it;
                } else {
                    result = nlohmann::json::object();
                }
            } catch (...) {
                return std::nullopt;
            }
        }
    }

    if (result && configStat && dbStat) {
        std::scoped_lock lock(versions_memo_mutex());
        versions_memo()[home.string()] = VersionsMemoEntry{
            configStat->first, configStat->second, dbStat->first,
            dbStat->second,    true,
            std::make_shared<const nlohmann::json>(*result)};
    }
    // The memo path returns a COPY from the shared json; this path hands
    // out the parsed value directly. Callers only read.
    return result;
}

nlohmann::json read_home_config(const std::filesystem::path& home) {
    const auto path = home_config_path(home);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) return nlohmann::json::object();
    try {
        auto content = platform::read_file_to_string(path.string());
        auto parsed = nlohmann::json::parse(content, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            return nlohmann::json::object();
        }
        return parsed;
    } catch (...) {
        return nlohmann::json::object();
    }
}

void restamp_versions_db_if_equal(const std::filesystem::path& home,
                                  const nlohmann::json& written) {
    namespace fs = std::filesystem;
    const auto dbPath = versions_db_path(home);
    std::error_code ec;
    if (!fs::is_regular_file(dbPath, ec) || ec) return;  // nothing to carry
    try {
        // The config must carry a versions map for the DB to mirror; one
        // without it is not something a stamp may vouch for.
        auto field = written.find(kVersionsKey);
        if (field == written.end() || !field->is_object()) return;

        const auto cfgPath = home_config_path(home);
        std::error_code sec, tec;
        const auto size = fs::file_size(cfgPath, sec);
        const auto mtime = fs::last_write_time(cfgPath, tec);
        if (sec || tec) return;

        auto wrapper = nlohmann::json::parse(
            platform::read_file_to_string(dbPath.string()), nullptr, false);
        if (wrapper.is_discarded() || !wrapper.is_object()
            || wrapper.value(kDbFormatKey, 0) != kDbFormat
            || !wrapper.contains(kDbStampKey)
            || !wrapper[kDbStampKey].is_string()) {
            return;
        }
        auto dbVersions = wrapper.find(kDbVersionsKey);
        if (dbVersions == wrapper.end() || !dbVersions->is_object()) return;

        // The proof. A DB that went stale before this write (an older
        // client edited `versions` since the last save_versions) stays
        // stale: its stamp keeps mismatching and readers keep falling back.
        if (*dbVersions != *field) {
            log::debug("{} differs from the versions in {}; leaving its stamp "
                       "stale", dbPath.string(), cfgPath.string());
            return;
        }

        const auto newStamp = versions_db_stamp(size, mtime);
        if (wrapper[kDbStampKey].get<std::string>() == newStamp) return;
        wrapper[kDbStampKey] = newStamp;
        platform::write_file_atomic(dbPath.string(), wrapper.dump(2));
    } catch (...) {
        // Stamp stays stale; readers fall back to the config field.
    }
}

std::expected<bool, std::string> update_home_config(const std::filesystem::path& home, const std::function<bool(nlohmann::json&)>& mutate, std::chrono::milliseconds timeout) {
    auto lock = xvm::acquire_state_lock(home, timeout);
    if (!lock) return std::unexpected(lock.error());

    auto json = read_home_config(home);
    if (!mutate(json)) return false;

    try {
        platform::write_string_to_file(
            home_config_path(home).string(), json.dump(2));
    } catch (const std::exception& e) {
        return std::unexpected(std::format(
            "failed to write {}: {}",
            home_config_path(home).string(), e.what()));
    }

    // The config write just invalidated the versions DB file's stamp; carry
    // it forward only if the DB still mirrors what was written.
    restamp_versions_db_if_equal(home, json);
    return true;
}

}
