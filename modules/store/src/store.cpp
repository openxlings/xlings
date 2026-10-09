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

fs::path ledger_path(const fs::path& data_dir) { return data_dir / "retained.json"; }

std::expected<std::vector<Retained>, std::string> read_ledger(const fs::path& ledger) {
    std::error_code ec;
    if (!fs::exists(ledger, ec)) {
        if (ec) return std::unexpected(ledger.string() + ": " + ec.message());
        return std::vector<Retained>{};
    }
    std::ifstream in(ledger, std::ios::binary);
    if (!in) return std::unexpected(ledger.string() + ": cannot be read");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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
