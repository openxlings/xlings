// What `$XLINGS_HOME/bin/xlings` actually is -- one reader.
//
// WHY THIS FILE EXISTS
//
// The entry binary is not a copy that drifts by accident. It is written on
// purpose, by `xlings use xlings <v>` and by the install-time equivalent
// (xvm/commands.cppm's self-replace, installer.cppm's twin): switching the
// `xlings` package physically replaces the bootstrap file, because main.cpp
// short-circuits the multicall names and would otherwise keep running the old
// code while the workspace claimed otherwise.
//
// Every shim in the home reaches that one file -- `subos/<s>/bin/<tool>` is a
// link to it -- so its version decides how EVERY tool in the home is
// dispatched. Measured on a real home: the entry was replaced with a June
// build, which predated `${XLINGS_DYNAMIC_SUBOS_DIR}`, so gcc's alias reached
// a shell as `--sysroot=` and the whole toolchain quietly stopped being
// self-contained. The first visible symptom was `cannot find crt1.o`, three
// layers away from anything mentioning xlings.
//
// What was missing was not the writer. It was that nobody ever compared the
// entry against what the home believed was active -- so a divergence created
// in one command could persist indefinitely with every channel reporting
// health.
//
// WHY IT RUNS THE BINARY INSTEAD OF READING A RECORD
//
// The home records a version in `.xlings.json` and the versions database
// records an active binding. Both are records, and this check exists precisely
// because a record and the file it describes can disagree. Checking a record
// against another record cannot see that. Asking the file costs one process
// (~60ms, measured) and is the only answer that cannot be stale.
module xlings.core.entry_binary;

import std;
import xlings.core.log;
import xlings.core.version_order;
import xlings.platform;
import xlings.core.xvm.shim_identity;
import xlings.core.xvm.lock;
import xlings.libs.json;
import xlings.libs.sha256;

namespace xlings::entry_binary {

fs::path path_of(const fs::path& homeDir) {
    const auto name = std::string("xlings") + std::string(platform::exe_suffix);
    auto p = homeDir / "bin" / name;
    if (!fs::exists(p)) p = homeDir / name;
    return p;
}

std::string version_of(const fs::path& entry) {
    std::error_code ec;
    if (!fs::exists(entry, ec) || ec) return {};
    auto [rc, out] = platform::run_command_capture(
        platform::shell_quote(entry.string()) + " --version 2>&1");
    if (rc != 0) return {};
    // `xlings <version>` -- take the last whitespace-delimited token of the
    // first non-empty line. Tolerant on purpose: a future banner change must
    // degrade to "no observation", not to a wrong observation.
    std::string_view text { out };
    auto nl = text.find('\n');
    auto line = text.substr(0, nl == std::string_view::npos ? text.size() : nl);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.remove_suffix(1);
    }
    auto sp = line.rfind(' ');
    if (sp == std::string_view::npos) return {};
    auto token = std::string(line.substr(sp + 1));
    // A version starts with a digit. Anything else -- an error message, a
    // usage line -- is not an observation of a version.
    if (token.empty() || !std::isdigit(static_cast<unsigned char>(token[0])))
        return {};
    return token;
}

bool replace_with(const fs::path& payloadBinary, const fs::path& entry,
                  std::string_view coordinate, std::string_view toVersion) {
    // Read BEFORE the swap: afterwards the old version is unrecoverable, and
    // "we changed something, we cannot say from what" is not a report.
    // Identical bytes: nothing to replace, and replacing anyway is not free.
    // On Windows it gives the entry a new file object, which detaches every
    // hard-link shim in the home from it (#615) -- `self update` used to do
    // that twice per run, once in `install --use` and again in
    // `use xlings latest`, for the same content.
    if (xvm::same_bytes(payloadBinary, entry)) {
        log::debug("entry binary already is {} ({})", coordinate,
                   payloadBinary.string());
        return true;
    }
    const auto before = version_of(entry);
    if (!platform::atomic_replace_executable(payloadBinary, entry)) {
        log::warn("could not replace the entry binary {} <- {}",
                  entry.string(), payloadBinary.string());
        return false;
    }
    // `toVersion` arrives as the WORKSPACE KEY, which since 2026.9.2.1 may be
    // spelled `xim:2026.9.2.1`. The comparator wants a bare version -- given a
    // key whose first character is not a digit it ranks the key below every
    // bare version, so an upgrade was reported as DOWNGRADED (#579). The
    // binary's own report (`before`) is always bare; make the other side match.
    const auto bare = [](std::string_view v) {
        const auto colon = v.find(':');
        return std::string(colon == std::string_view::npos ? v
                                                            : v.substr(colon + 1));
    }(toVersion);
    if (before.empty() || before == bare) {
        log::debug("entry binary -> {} ({})", bare, coordinate);
        return true;
    }
    if (version_order::compare(bare, before) < 0) {
        log::warn("entry binary DOWNGRADED {} -> {} ({})",
                  before, bare, coordinate);
        log::warn("  every shim in this home dispatches through it; an older "
                  "client may not understand records a newer index wrote");
        return true;
    }
    log::info("entry binary {} -> {} ({})", before, bare, coordinate);
    return true;
}

namespace {
using Json = nlohmann::json;
std::expected<bool, std::string> present_(const fs::path& path) {
    std::error_code ec;
    const auto status = fs::symlink_status(path, ec);
    if (status.type() == fs::file_type::not_found && (!ec || ec == std::errc::no_such_file_or_directory)) return false;
    if (ec) return std::unexpected(path.string() + ": cannot inspect managed entry: " + ec.message());
    return true;
}
std::expected<std::string, std::string> digest_(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(path, ec)) || ec)
        return std::unexpected(path.string() + ": managed entry must be a readable regular file");
    const auto digest = sha256::hex_file(path);
    if (!digest) return std::unexpected(path.string() + ": cannot hash managed entry");
    return *digest;
}
std::expected<Json, std::string> read_record_(const fs::path& path) {
    if (auto digest = digest_(path); !digest) return std::unexpected(digest.error());
    std::ifstream in(path, std::ios::binary);
    auto doc = Json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return std::unexpected(path.string() + ": invalid managed entry record");
    return doc;
}
std::expected<void, std::string> publish_record_(const fs::path& path, const Json& doc,
                                               const std::optional<Json>& previous) {
    auto exists = present_(path);
    if (!exists) return std::unexpected(exists.error());
    if (*exists != previous.has_value()) return std::unexpected(path.string() + ": managed entry record ownership changed");
    if (previous) {
        auto current = read_record_(path);
        if (!current || *current != *previous) return std::unexpected(path.string() + ": managed entry record changed");
    }
    try { platform::write_file_atomic(path.string(), doc.dump(2)); }
    catch (const std::exception& error) { return std::unexpected(error.what()); }
    return {};
}
bool hash_(const Json& value) {
    if (!value.is_string()) return false;
    const auto text = value.get<std::string>();
    return text.size() == 64 && std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
}

std::expected<bool, std::string> refresh_mirror(const fs::path& selected, const ManagedMirror& mirror) {
    try {
    std::error_code ec;
    const auto owner = fs::canonical(mirror.ownerHome, ec);
    if (ec || owner != mirror.ownerHome.lexically_normal()) return std::unexpected("managed entry owner must be canonical");
    const auto home = fs::canonical(mirror.privateHome, ec);
    if (ec || home != mirror.privateHome.lexically_normal() || home != owner / "domains/xlings/private")
        return std::unexpected("managed entry private home escapes its owner");
    for (const auto& dir : {owner / "domains", owner / "domains/xlings", home, home / "bin"}) {
        if (!fs::is_directory(fs::symlink_status(dir, ec)) || ec)
            return std::unexpected(dir.string() + ": managed entry parent is redirected or unreadable");
    }
    const auto marker = home / ".xlings-domain.json";
    auto domain = read_record_(marker);
    if (!domain || !domain->contains("owner_home") || (*domain)["owner_home"] != owner.generic_string() ||
        !domain->contains("physical_home") || (*domain)["physical_home"] != home.generic_string() ||
        !domain->contains("private_home") || (*domain)["private_home"] != true)
        return std::unexpected(marker.string() + ": missing or contradictory managed domain authority");
    const auto markerHash = digest_(marker);
    if (!markerHash) return std::unexpected(markerHash.error());
    const auto source = fs::canonical(selected, ec);
    if (ec) return std::unexpected(selected.string() + ": cannot resolve selected dispatcher");
    const auto sourceHash = digest_(source);
    if (!sourceHash) return std::unexpected(sourceHash.error());
    xvm::ShimClassifier classifier{fs::path{}};
    const auto identity = classifier.classify(source);
    if (identity.state != xvm::ShimState::Stale || !identity.handoffCapable)
        return std::unexpected(source.string() + ": selected dispatcher is not a marked xlings build");
    auto lock = xvm::acquire_state_lock(home);
    if (!lock) return std::unexpected(lock.error());
    const auto entry = home / "bin/xlings";
    const auto proof = home / ".xlings-domain-entry.json";
    auto entryExists = present_(entry), proofExists = present_(proof);
    if (!entryExists || !proofExists) return std::unexpected(!entryExists ? entryExists.error() : proofExists.error());
    std::optional<std::string> actual;
    if (*entryExists) {
        auto digest = digest_(entry);
        if (!digest) return std::unexpected(digest.error());
        actual = *digest;
    }
    std::optional<Json> record;
    bool recovered = false;
    if (*proofExists) {
        auto doc = read_record_(proof);
        if (!doc) return std::unexpected(doc.error());
        record = std::move(*doc);
        if ((*record).value("schema", 0) == 1) {
            if (!actual || !record->contains("source") || !(*record)["source"].is_string())
                return std::unexpected(proof.string() + ": legacy entry ownership cannot be observed");
            auto originalHash = digest_(fs::path((*record)["source"].get<std::string>()));
            if (!originalHash || *originalHash != *actual)
                return std::unexpected(entry.string() + ": legacy dispatcher differs from its recorded source; preserved");
        } else if ((*record).value("schema", 0) == 2) {
            if (record->value("owner_home", "") != owner.generic_string() ||
                record->value("physical_home", "") != home.generic_string() ||
                record->value("marker_sha256", "") != *markerHash ||
                record->value("role", "") != "managed-mirror" || !record->contains("sha256"))
                return std::unexpected(proof.string() + ": managed entry ownership changed; preserved");
            const bool pending = record->value("state", "") == "pending";
            const auto& oldHash = (*record)["sha256"];
            if ((!oldHash.is_null() && !hash_(oldHash)) || (!pending && !hash_(oldHash)) ||
                (!pending && record->value("state", "") != "ready") ||
                (pending && (!record->contains("pending_sha256") || !hash_((*record)["pending_sha256"]))))
                return std::unexpected(proof.string() + ": invalid managed entry journal");
            const bool oldMatches = actual ? oldHash == *actual : oldHash.is_null();
            const bool newMatches = pending && actual && (*record)["pending_sha256"] == *actual;
            if (!oldMatches && !newMatches)
                return std::unexpected(entry.string() + ": dispatcher was changed outside its managed writer; preserved");
            recovered = pending;
        } else return std::unexpected(proof.string() + ": unknown managed entry schema; preserved");
    } else if (actual) return std::unexpected(entry.string() + ": dispatcher ownership is unknown; preserved");

    Json ready = record.value_or(Json::object());
    ready["schema"] = 2;
    ready["owner_home"] = owner.generic_string();
    ready["physical_home"] = home.generic_string();
    ready["marker_sha256"] = *markerHash;
    ready["role"] = "managed-mirror";
    ready["source"] = source.generic_string();
    ready["state"] = "ready";
    ready["sha256"] = actual ? Json(*actual) : Json{};
    ready.erase("pending_sha256");
    ready.erase("pending_staging");
    // The existing executable publisher uses this fixed sibling. A pre-existing
    // unknown file cannot be pre-cleaned merely because we own the entry.
    const auto staging = fs::path(entry.string() + ".xlings.new");
    auto stageExists = present_(staging);
    if (!stageExists) return std::unexpected(stageExists.error());
    if (*stageExists) {
        const auto stageHash = digest_(staging);
        if (!record || record->value("state", "") != "pending" ||
            record->value("pending_staging", "") != staging.generic_string() || !stageHash ||
            !record->contains("pending_sha256") || (*record)["pending_sha256"] != *stageHash)
            return std::unexpected(staging.string() + ": unknown entry staging file preserved");
        if (!fs::remove(staging, ec) || ec)
            return std::unexpected(staging.string() + ": cannot retire proved interrupted entry staging");
    }
    if (actual && *actual == *sourceHash) {
        if (!record || *record != ready) {
            auto saved = publish_record_(proof, ready, record);
            if (!saved) return std::unexpected(saved.error());
        }
        return recovered;
    }
    struct Snapshot {
        fs::path directory;
        ~Snapshot() {
            if (directory.empty()) return;
            std::error_code ignored;
            fs::remove(directory / "source", ignored);
            fs::remove(directory, ignored);
        }
    } snapshot;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto candidate = home / (".xlings-domain-source-" + std::to_string(std::random_device{}()));
        if (fs::create_directory(candidate, ec)) { snapshot.directory = std::move(candidate); break; }
        if (ec && ec != std::errc::file_exists) return std::unexpected("cannot reserve dispatcher source snapshot: " + ec.message());
    }
    if (snapshot.directory.empty()) return std::unexpected("cannot reserve dispatcher source snapshot");
    fs::permissions(snapshot.directory, fs::perms::owner_all, fs::perm_options::replace, ec);
    if (ec) return std::unexpected("cannot protect dispatcher source snapshot: " + ec.message());
    const auto pinned = snapshot.directory / "source";
    if (!fs::copy_file(source, pinned, fs::copy_options::none, ec) || ec)
        return std::unexpected("cannot capture selected dispatcher: " + ec.message());
    const auto pinnedHash = digest_(pinned);
    if (!pinnedHash || *pinnedHash != *sourceHash)
        return std::unexpected("selected dispatcher changed during capture; old entry preserved");
    const auto pinnedIdentity = classifier.classify(pinned);
    if (pinnedIdentity.state != xvm::ShimState::Stale || !pinnedIdentity.handoffCapable)
        return std::unexpected("captured dispatcher is not a marked xlings build; old entry preserved");
    Json pending = ready;
    pending["state"] = "pending";
    pending["pending_sha256"] = *sourceHash;
    pending["pending_staging"] = staging.generic_string();
    if (auto saved = publish_record_(proof, pending, record); !saved) return std::unexpected(saved.error());
    if (!replace_with(pinned, entry, "managed domain dispatcher", version_of(pinned)))
        return std::unexpected(entry.string() + ": managed dispatcher publication failed; journal retained");
    auto published = digest_(entry);
    if (!published || *published != *sourceHash)
        return std::unexpected(entry.string() + ": managed dispatcher changed during publication; journal retained");
    ready["sha256"] = *sourceHash;
    if (auto saved = publish_record_(proof, ready, pending); !saved) return std::unexpected(saved.error());
    return true;
    } catch (const std::exception& error) {
        return std::unexpected(std::string("managed dispatcher refresh refused: ") + error.what());
    }
}

}
