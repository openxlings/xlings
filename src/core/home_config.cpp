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
// The abort is guarded by a sortedness check, and that guard is what makes
// it safe rather than merely fast: `versions` sorts near the end of the key
// order nlohmann's std::map-backed dump always produces, so the moment a
// SORTED stream reaches `versions`, every key the consumer needs has either
// been seen or genuinely does not exist. A hand-edited file whose order is
// not sorted (a key went backwards) never aborts -- it degrades to
// SkipVersions, which is slower and misses nothing.
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
    auto content = platform::read_file_to_string(configPath.string());
    const auto after = stat_now(configPath);

    auto capture = capture_from_content(content, mode == HomeCaptureMode::AbortAtVersions);

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

std::optional<nlohmann::json> load_versions_json(const std::filesystem::path& home) {
    namespace fs = std::filesystem;
    const auto dbPath = versions_db_path(home);
    std::error_code ec;
    if (fs::is_regular_file(dbPath, ec) && !ec) {
        try {
            auto content = platform::read_file_to_string(dbPath.string());
            auto json = nlohmann::json::parse(content, nullptr, false);
            if (!json.is_discarded() && json.is_object()) return json;
            log::warn("{} is malformed; falling back to the versions in {}",
                      dbPath.string(), home_config_path(home).string());
        } catch (const std::exception& e) {
            log::warn("could not read {} ({}); falling back to the versions in {}",
                      dbPath.string(), e.what(),
                      home_config_path(home).string());
        }
    }
    // The home config: three cases, not two. A config that is ABSENT is an
    // observed empty database (a fresh home's legitimate shape). A config
    // that exists but cannot be parsed is UNOBSERVED -- nullopt -- because
    // the consumers that refuse on nullopt must keep refusing: profile.cpp's
    // payload collector treats "could not read" as "references unknown", and
    // treating it as empty would list live payloads for removal.
    auto configPath = home_config_path(home);
    if (!fs::is_regular_file(configPath, ec) || ec) {
        return nlohmann::json::object();
    }
    try {
        auto content = platform::read_file_to_string(configPath.string());
        auto doc = nlohmann::json::parse(content, nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) return std::nullopt;
        if (auto it = doc.find(kVersionsKey); it != doc.end() && it->is_object()) {
            return *it;
        }
        return nlohmann::json::object();
    } catch (...) {
        return std::nullopt;
    }
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
    return true;
}

}
