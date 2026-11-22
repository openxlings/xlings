module xlings.store;

import std;
import xlings.libs.json;
import xlings.platform;

namespace xlings::store {

namespace {

std::string utc_now() {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

bool within(const fs::path& path, const fs::path& root) {
    auto p = path.lexically_normal();
    auto r = root.lexically_normal();
    auto pi = p.begin();
    for (auto ri = r.begin(); ri != r.end(); ++ri, ++pi) {
        if (ri->empty()) continue;   // a trailing separator
        if (pi == p.end() || *pi != *ri) return false;
    }
    return true;
}

std::expected<void, std::string> write_ledger(const fs::path& ledger,
                                              const std::vector<Retained>& entries) {
    nlohmann::json doc{{"schema", 1}, {"retained", nlohmann::json::array()}};
    for (const auto& e : entries)
        doc["retained"].push_back({{"payload", e.payload.generic_string()}, {"target", e.target},
                                   {"version", e.version}, {"since", e.since}});
    try {
        std::error_code ec;
        fs::create_directories(ledger.parent_path(), ec);
        platform::write_file_atomic(ledger.string(), doc.dump(2) + "\n");
    } catch (const std::exception& e) {
        return std::unexpected(ledger.string() + ": " + e.what());
    }
    return {};
}

}  // namespace

std::optional<fs::path> payload_root(const fs::path& p) {
    fs::path root;
    int after = -1;
    for (const auto& part : p) {
        root /= part;
        if (after >= 0 && ++after == 2) return root;
        if (after < 0 && part == "xpkgs") after = 0;
    }
    return std::nullopt;
}

Pins pins(const fs::path& payload, std::span<const RootSet> roots,
          std::span<const std::string> unreadable) {
    Pins out;
    out.unreadable.assign(unreadable.begin(), unreadable.end());
    for (const auto& root : roots) {
        for (const auto& target : root.targets) {
            if (!within(target, payload)) continue;
            out.holders.push_back({root.subos, root.generation});
            break;
        }
    }
    return out;
}

std::string payload_key(const fs::path& payloadRoot) {
    return payloadRoot.parent_path().filename().string() + "/" + payloadRoot.filename().string();
}

LinkWalk walk_links(const fs::path& dir) {
    LinkWalk out;
    std::error_code ec;
    const auto top = fs::symlink_status(dir, ec);
    if (ec || !fs::exists(top)) {
        if (ec && ec != std::errc::no_such_file_or_directory)
            out.unreadable.push_back(dir.string() + " (" + ec.message() + ")");
        return out;
    }
    if (auto target = platform::dir_link_target(dir)) {
        out.links.push_back({dir, std::move(*target)});
        return out;
    }
    if (!fs::is_directory(top)) return out;
    // An explicit stack instead of recursive_directory_iterator: a directory
    // that cannot be opened is named and skipped, and the walk goes on with
    // its siblings -- the recursive iterator's state after such an error is
    // the library's.
    std::vector<std::pair<fs::path, int>> pending{{dir, 0}};
    while (!pending.empty()) {
        auto [current, depth] = std::move(pending.back());
        pending.pop_back();
        std::error_code dec;
        for (auto it = fs::directory_iterator(current, dec); !dec && it != fs::directory_iterator();
             it.increment(dec)) {
            const auto& path = it->path();
            if (auto target = platform::dir_link_target(path)) {
                out.links.push_back({path, std::move(*target)});
                continue;
            }
            std::error_code tec;
            if (depth + 1 < 10 && it->is_directory(tec)) pending.emplace_back(path, depth + 1);
        }
        if (dec) out.unreadable.push_back(current.string() + " (" + dec.message() + ")");
    }
    return out;
}

fs::path roots_dir(const fs::path& data_dir) { return data_dir / "roots"; }

std::expected<RootScan, std::string> read_roots(const fs::path& data_dir) {
    RootScan scan;
    const auto dir = roots_dir(data_dir);
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(dir, ec))) {
        if (ec && ec != std::errc::no_such_file_or_directory)
            return std::unexpected(dir.string() + ": " + ec.message());
        return scan;
    }
    // One level of owners, one level of roots: a flat walk of both.
    std::vector<std::pair<std::string, fs::path>> entries;
    for (auto owners = fs::directory_iterator(dir, ec); !ec && owners != fs::directory_iterator();
         owners.increment(ec)) {
        std::error_code tec;
        if (!owners->is_directory(tec) || fs::is_symlink(owners->symlink_status(tec))) {
            scan.ignored.push_back(owners->path());
            continue;
        }
        const auto owner = owners->path().filename().string();
        std::error_code rec;
        for (auto it = fs::directory_iterator(owners->path(), rec); !rec && it != fs::directory_iterator();
             it.increment(rec)) {
            entries.emplace_back(owner, it->path());
        }
        if (rec) return std::unexpected(owners->path().string() + ": " + rec.message());
    }
    if (ec) return std::unexpected(dir.string() + ": " + ec.message());

    for (auto& [owner, at] : entries) {
        auto target = platform::dir_link_target(at);
        if (!target) {
            scan.ignored.push_back(at);
            continue;
        }
        std::error_code sec;
        const bool present = fs::exists(at, sec);
        if (sec && sec != std::errc::no_such_file_or_directory)
            return std::unexpected(at.string() + ": " + sec.message());
        if (!present) {
            scan.dangling.push_back(at);
            continue;
        }
        // The payload directory itself, not a file inside it.
        auto named = target->lexically_normal();
        if (!named.has_filename()) named = named.parent_path();
        auto payload = payload_root(named);
        if (!payload || *payload != named) {
            scan.ignored.push_back(at);
            continue;
        }
        scan.live.push_back({owner, at, std::move(*payload)});
    }
    return scan;
}

fs::path ledger_path(const fs::path& data_dir) { return data_dir / "retained.json"; }

std::expected<std::vector<Retained>, std::string> read_ledger(const fs::path& ledger) {
    std::error_code ec;
    if (!fs::exists(ledger, ec)) {
        if (ec) return std::unexpected(ledger.string() + ": " + ec.message());
        return std::vector<Retained>{};
    }
    auto read = platform::read_file(ledger);
    if (!read) return std::unexpected(ledger.string() + ": cannot be read: " + read.error().message());
    const std::string& text = *read;
    auto doc = nlohmann::json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("retained") || !doc["retained"].is_array())
        return std::unexpected(ledger.string() + ": not a retained-payload ledger");
    std::vector<Retained> out;
    for (const auto& e : doc["retained"]) {
        if (!e.is_object() || !e.contains("payload") || !e["payload"].is_string())
            return std::unexpected(ledger.string() + ": an entry has no payload");
        out.push_back({fs::path(e["payload"].get<std::string>()), e.value("target", ""),
                       e.value("version", ""), e.value("since", "")});
    }
    return out;
}

std::expected<void, std::string> retain(const fs::path& ledger, Retained entry) {
    auto entries = read_ledger(ledger);
    if (!entries) return std::unexpected(entries.error());
    if (entry.since.empty()) entry.since = utc_now();
    std::erase_if(*entries, [&](const Retained& e) { return e.payload == entry.payload; });
    entries->push_back(std::move(entry));
    return write_ledger(ledger, *entries);
}

std::expected<void, std::string> forget(const fs::path& ledger, const fs::path& payload) {
    auto entries = read_ledger(ledger);
    if (!entries) return std::unexpected(entries.error());
    const auto before = entries->size();
    std::erase_if(*entries, [&](const Retained& e) { return e.payload == payload; });
    if (entries->size() == before) return {};
    return write_ledger(ledger, *entries);
}

}  // namespace xlings::store
