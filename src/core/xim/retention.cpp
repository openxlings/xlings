module xlings.core.xim.retention;

import std;
import xlings.store;
import xlings.subos.rootfs;
import xlings.core.config;
import xlings.core.log;
import xlings.core.xim.payload;
import xlings.core.xim.installer;
import xlings.core.xim.libxpkg.types.type;

namespace xlings::xim::retention {

namespace rf = xlings::subos::rootfs;

namespace {

fs::path ledger() { return store::ledger_path(Config::paths().dataDir); }

}  // namespace

store::Pins generation_pins(const fs::path& payload) {
    std::vector<store::RootSet> roots;
    std::vector<std::string> unreadable;
    const auto subosRoot = Config::paths().homeDir / "subos";
    std::error_code ec;
    for (auto it = fs::directory_iterator(subosRoot, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const auto& dir = it->path();
        std::error_code sec;
        if (fs::is_symlink(it->symlink_status(sec)) || !it->is_directory(sec)) continue;  // `current`
        if (!fs::exists(dir / std::string(rf::kGenerations), sec)) continue;
        const auto name = dir.filename().string();
        for (const int k : rf::generations(dir)) {
            auto targets = rf::linked_targets(dir, k);
            if (!targets) {
                unreadable.push_back(std::format("generation {} of '{}'", k, name));
                continue;
            }
            roots.push_back({name, k, std::move(*targets)});
        }
    }
    if (ec && ec != std::errc::no_such_file_or_directory)
        unreadable.push_back(subosRoot.string() + " (" + ec.message() + ")");
    return store::pins(payload, roots, unreadable);
}

std::string describe(const store::Pins& pins) {
    std::string out;
    for (const auto& h : pins.holders)
        out += std::format("{}generation {} of '{}'", out.empty() ? "" : ", ", h.generation, h.subos);
    for (const auto& u : pins.unreadable)
        out += std::format("{}{} (unreadable)", out.empty() ? "" : ", ", u);
    return out;
}

std::expected<void, std::string> retain(const fs::path& payload, std::string target,
                                        std::string version) {
    return store::retain(ledger(), {payload, std::move(target), std::move(version), {}});
}

std::vector<fs::path> release() {
    std::vector<fs::path> released;
    auto entries = store::read_ledger(ledger());
    if (!entries) {
        log::warn("[xim] retained payloads are kept: {}", entries.error());
        return released;
    }
    for (const auto& entry : *entries) {
        std::error_code ec;
        if (!fs::exists(entry.payload, ec)) {
            if (!ec) (void)store::forget(ledger(), entry.payload);
            continue;
        }
        // A workspace uses it again, or one cannot be read: not ours to delete.
        if (!entry.target.empty() &&
            detail_::is_version_referenced_anywhere_(PackageScope::Global, entry.target, entry.version))
            continue;
        if (generation_pins(entry.payload).held()) continue;
        sweep_payload_trash(payload_trash_root(entry.payload));
        if (remove_payload_dir(entry.payload, entry.version) != RemoveOutcome::Removed) {
            log::warn("[xim] {} is no longer held by any generation but could not be removed yet",
                      entry.payload.string());
            continue;
        }
        if (auto forgotten = store::forget(ledger(), entry.payload); !forgotten)
            log::warn("[xim] {}", forgotten.error());
        log::info("[xim] released {}@{}: the last generation that linked into it was pruned",
                  entry.target, entry.version);
        released.push_back(entry.payload);
    }
    return released;
}

}  // namespace xlings::xim::retention
