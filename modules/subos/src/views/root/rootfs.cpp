module xlings.subos.rootfs;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.observe;
import xlings.store;

namespace xlings::subos::rootfs {

namespace {

// libfoo.so, libfoo.so.1.2 -- and the loader, ld-linux-x86-64.so.2.
bool is_shared_object(std::string_view name) {
    return (name.starts_with("lib") || name.starts_with("ld-"))
        && (name.ends_with(".so") || name.find(".so.") != std::string_view::npos);
}

bool is_executable_entry(const fs::directory_entry& e) {
    std::error_code ec;
    // A link counts as what it names (busybox's applets are links to it).
    if (!e.is_regular_file(ec)) return false;
    const auto perms = e.status(ec).permissions();
    return !ec && (perms & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec))
                      != fs::perms::none;
}

std::vector<fs::directory_entry> sorted_entries(const fs::path& dir) {
    std::vector<fs::directory_entry> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        out.push_back(*it);
    std::ranges::sort(out, {}, [](const fs::directory_entry& e) { return e.path().filename(); });
    return out;
}

std::string utc_now() {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                       std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
}

fs::path generation_dir(const fs::path& subos, int k) {
    return subos / std::string(kGenerations) / std::to_string(k);
}

nlohmann::json conflicts_json(const std::vector<Conflict>& cs) {
    auto a = nlohmann::json::array();
    for (auto& c : cs) a.push_back({{"path", c.rel}, {"kept", c.kept}, {"dropped", c.dropped}});
    return a;
}

// The links a generation holds, as "rel\ttarget" lines in order: what makes
// two plans the same generation.
std::string fingerprint(const Plan& p) {
    std::string s;
    for (auto& l : p.links) s += l.rel + "\t" + l.target.generic_string() + "\n";
    return s;
}

std::optional<std::string> read_checked_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return std::nullopt;
    const auto size = static_cast<std::streamoff>(input.tellg());
    if (size < 0 || size > std::numeric_limits<std::streamsize>::max())
        return std::nullopt;
    std::string text(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size())) ||
        input.peek() != std::char_traits<char>::eof() || input.bad())
        return std::nullopt;
    return text;
}

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

bool safe_relative_link(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.lexically_normal() != path) return false;
    for (const auto& part : path)
        if (part == ".." || part == ".") return false;
    return *path.begin() == "usr" && std::distance(path.begin(), path.end()) > 1;
}

// ── durability (ROOT-GEN-DURABLE) ────────────────────────────────────
//
// A generation is published by two renames, and a rename is durable only
// once the directory holding it is flushed -- the entries it publishes, and
// the files they name, before it. `XLINGS_TRACE=durability` prints the
// order, which is what the test of it reads.

void trace_sync(std::string_view what, const fs::path& path) {
    if (observe::trace_enabled("durability")) observe::trace("durability", std::format("{} {}", what, path.string()));
}

void sync_file(const fs::path& path) {
    trace_sync("file", path);
    (void)platform::sync_file(path);
}

void sync_dir(const fs::path& path) {
    trace_sync("dir", path);
    platform::sync_directory(path);
}

class OwnedStage {
private:
    fs::path path_;

public:
    explicit OwnedStage(fs::path path) : path_{std::move(path)} {}
    OwnedStage(OwnedStage&& other) noexcept : path_{std::exchange(other.path_, {})} {}
    OwnedStage(const OwnedStage&) = delete;
    ~OwnedStage() {
        if (path_.empty()) return;
        std::error_code ec;
        fs::remove_all(path_, ec); // subos-remove-all-ok: exclusively created staging directory, never a preexisting path
    }
    const fs::path& path() const { return path_; }

    static std::expected<OwnedStage, std::string> create(const fs::path& parent) {
        std::error_code ec;
        const auto status = fs::symlink_status(parent, ec);
        if (fs::is_symlink(status)) return std::unexpected("staging parent is a symlink: " + parent.string());
        fs::create_directories(parent, ec);
        if (ec) return std::unexpected("cannot create staging parent: " + ec.message());
        static std::atomic<unsigned long long> serial { 0 };
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto path = parent / std::format(".xlings-stage-{}-{}",
                std::chrono::steady_clock::now().time_since_epoch().count(), serial++);
            if (fs::create_directory(path, ec)) return OwnedStage{path};
            if (ec && ec != std::errc::file_exists)
                return std::unexpected("cannot create staging directory: " + ec.message());
        }
        return std::unexpected("cannot reserve staging directory");
    }
};

// Metadata alone is not ownership: every entry must be a recorded projection
// link, its parent directory, or one of the two metadata files we wrote.
bool owned_generation(const fs::path& subos, int generation,
                      std::string* checkedRecords = nullptr) {
    const auto dir = generation_dir(subos, generation);
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(subos / kGenerations, ec)) || ec ||
        !fs::is_directory(fs::symlink_status(dir, ec)) || ec)
        return false;
    for (const auto name : {"generation.json", "links.tsv"})
        if (!fs::is_regular_file(fs::symlink_status(dir / name, ec)) || ec)
            return false;
    const auto manifest = read_checked_text(dir / "generation.json");
    auto contents = read_checked_text(dir / "links.tsv");
    if (!manifest || !contents)
        return false;
    const auto metadata = nlohmann::json::parse(*manifest, nullptr, false);
    if (!metadata.is_object() || !metadata.contains("number") ||
        !metadata["number"].is_number_integer() || metadata["number"] != generation)
        return false;
    std::unordered_map<std::string, fs::path> links;
    std::unordered_set<std::string> directories{"usr"};
    std::istringstream records(*contents);
    for (std::string line; std::getline(records, line);) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos)
            return false;
        const fs::path rel = line.substr(0, tab);
        if (!safe_relative_link(rel))
            return false;
        const auto key = rel.generic_string();
        if (!links.emplace(key, line.substr(tab + 1)).second)
            return false;
        for (auto slash = key.rfind('/'); slash != std::string::npos;
             slash = key.rfind('/', slash - 1))
            directories.insert(key.substr(0, slash));
    }
    const auto prefix = dir.generic_string() + "/";
    std::size_t seen{0};
    bool sawUsr = false;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto path = it->path().generic_string();
        if (!path.starts_with(prefix))
            return false;
        const auto rel = path.substr(prefix.size());
        if (const auto link = links.find(rel); link != links.end()) {
            // read_symlink proves both the entry type and its exact target.
            // Do not follow it even if its directory-entry type changed.
            it.disable_recursion_pending();
            if (fs::read_symlink(it->path(), ec) != link->second || ec)
                return false;
            ++seen;
            continue;
        }
        const auto status = it->symlink_status(ec);
        if (ec)
            return false;
        if (fs::is_directory(status)) {
            if (!directories.contains(rel))
                return false;
            if (rel == "usr")
                sawUsr = true;
        } else if (!fs::is_regular_file(status) ||
                   (rel != "generation.json" && rel != "links.tsv")) {
            return false;
        }
    }
    if (ec || !sawUsr || seen != links.size())
        return false;
    if (checkedRecords)
        *checkedRecords = std::move(*contents);
    return true;
}

// ── the inventory (ROOT-SWITCH-SCALE) ────────────────────────────────
//
// What `owned_generation` proves by reading every link, recorded once when
// the generation is placed: the change stamp (inode + ctime) of each of its
// directories and of its two record files, and the payloads it links into.
// Adding, removing or replacing an entry changes its directory's ctime, and
// ctime cannot be set back, so equal stamps mean "as placed". It lives beside
// the generation (root.gen/.<k>.inventory), never inside: the tree is
// immutable once placed. Missing or unequal falls back to the full walk.

fs::path inventory_path(const fs::path& subos, int generation) {
    return subos / std::string(kGenerations) / std::format(".{}.inventory", generation);
}

std::string stamp_line(std::string_view kind, std::string_view rel, const platform::ChangeStamp& s) {
    return std::format("{}\t{}\t{}\t{}\t{}\n", kind, rel, s.inode, s.seconds, s.nanoseconds);
}

std::expected<void, std::string> write_inventory(const fs::path& subos, int generation,
                                                 const std::set<fs::path>& directories,
                                                 const std::set<fs::path>& payloads) {
    const auto dir = generation_dir(subos, generation);
    std::string text;
    for (const auto& rel : directories) {
        auto stamp = platform::change_stamp(rel.empty() ? dir : dir / rel);
        if (!stamp) return std::unexpected("cannot stamp " + (dir / rel).string());
        text += stamp_line("dir", rel.generic_string(), *stamp);
    }
    for (const auto* name : {"generation.json", "links.tsv"}) {
        auto stamp = platform::change_stamp(dir / name);
        if (!stamp) return std::unexpected("cannot stamp " + (dir / name).string());
        text += stamp_line("file", name, *stamp);
    }
    for (const auto& payload : payloads) text += std::format("payload\t{}\n", payload.generic_string());
    try {
        platform::write_file_atomic(inventory_path(subos, generation).string(), text);
    } catch (const std::exception& e) {
        return std::unexpected(std::string("cannot write the generation inventory: ") + e.what());
    }
    return {};
}

enum class Inventory { Matches, Differs, Missing };

// `missingPayload` names the first payload the generation links into that
// is gone: the generation is intact and still cannot be used.
Inventory check_inventory(const fs::path& subos, int generation, fs::path* missingPayload = nullptr,
                          bool payloads = true) {
    const auto text = read_checked_text(inventory_path(subos, generation));
    if (!text) return Inventory::Missing;
    const auto dir = generation_dir(subos, generation);
    auto number = [](std::string_view v, auto& out) {
        auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
        return ec == std::errc{} && ptr == v.data() + v.size();
    };
    bool sawDir = false;
    std::string_view rest(*text);
    while (!rest.empty()) {
        const auto eol = rest.find('\n');
        const auto line = rest.substr(0, eol);
        rest = eol == std::string_view::npos ? std::string_view{} : rest.substr(eol + 1);
        if (line.empty()) continue;
        std::array<std::string_view, 5> f{};
        std::size_t n = 0;
        for (std::size_t at = 0; n < f.size();) {
            const auto tab = line.find('\t', at);
            f[n++] = line.substr(at, tab == std::string_view::npos ? std::string_view::npos : tab - at);
            if (tab == std::string_view::npos) break;
            at = tab + 1;
        }
        if (n == 2 && f[0] == "payload") {
            // One lstat: a payload root is a directory, never a link.
            if (payloads && !platform::change_stamp(fs::path(f[1]))) {
                if (missingPayload) *missingPayload = fs::path(f[1]);
                return Inventory::Differs;
            }
            continue;
        }
        if (n != 5 || (f[0] != "dir" && f[0] != "file")) return Inventory::Differs;
        platform::ChangeStamp want;
        if (!number(f[2], want.inode) || !number(f[3], want.seconds) || !number(f[4], want.nanoseconds))
            return Inventory::Differs;
        const auto have = platform::change_stamp(f[1].empty() ? dir : dir / fs::path(f[1]));
        if (!have || *have != want) return Inventory::Differs;
        sawDir |= f[0] == "dir" && f[1].empty();
    }
    return sawDir ? Inventory::Matches : Inventory::Differs;
}

// Every payload generation `generation` links into still exists.
std::optional<fs::path> missing_payload(const fs::path& subos, int generation) {
    const auto text = read_checked_text(generation_dir(subos, generation) / "links.tsv");
    if (!text) return std::nullopt;
    std::set<fs::path> seen;
    std::istringstream records(*text);
    for (std::string line; std::getline(records, line);) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const auto root = store::payload_root(line.substr(tab + 1));
        if (!root || !seen.insert(*root).second) continue;
        std::error_code ec;
        if (!fs::exists(*root, ec)) return *root;
    }
    return std::nullopt;
}

}  // namespace

// ── plan ─────────────────────────────────────────────────────────────

Plan plan(const Inputs& in) {
    Plan p;
    std::map<std::string, std::size_t> claimed;   // rel -> index in p.links
    auto claim = [&](std::string rel, fs::path target, std::string from) {
        if (auto it = claimed.find(rel); it != claimed.end()) {
            auto& kept = p.links[it->second];
            // The same file reached another way (lib64 -> lib, a sysroot link
            // into the payload) is not a conflict.
            std::error_code ec;
            if (kept.target != target && !fs::equivalent(kept.target, target, ec))
                p.conflicts.push_back({rel, kept.from, from});
            return;
        }
        claimed.emplace(rel, p.links.size());
        p.links.push_back({std::move(rel), std::move(target), std::move(from)});
    };

    // Fixed shape of a merged /usr; claimed first so nothing takes the names.
    claim("usr/lib64", "lib", "layout");
    claim("usr/sbin", "bin", "layout");

    for (auto& prog : in.programs) claim("usr/bin/" + prog.name, prog.target, "program");
    for (auto& lib : in.libraries) claim("usr/lib/" + lib.name, lib.target, "library");

    for (auto& payload : in.payloads) {
        const auto who = payload.generic_string();
        for (auto sub : {"bin", "sbin"}) {
            for (auto& e : sorted_entries(payload / sub))
                if (is_executable_entry(e))
                    claim("usr/bin/" + e.path().filename().string(), e.path(), who);
        }
        // A kernel package's modules (and its vmlinuz, the systemd layout):
        // usr/lib/modules/<version>.
        for (auto& e : sorted_entries(payload / "lib" / "modules"))
            claim("usr/lib/modules/" + e.path().filename().string(), e.path(), who);
        for (auto sub : {"lib", "lib64"}) {
            for (auto& e : sorted_entries(payload / sub)) {
                const auto name = e.path().filename().string();
                std::error_code ec;
                if (is_shared_object(name) && !e.is_directory(ec))
                    claim("usr/lib/" + name, e.path(), who);
            }
        }
    }

    if (!in.sysroot_lib.empty()) {
        for (auto& e : sorted_entries(in.sysroot_lib))
            claim("usr/lib/" + e.path().filename().string(), e.path(), "sysroot");
    }
    for (auto& e : sorted_entries(in.sysroot_share))
        claim("usr/share/" + e.path().filename().string(), e.path(), "sysroot");
    if (!in.sysroot_usr.empty()) {
        for (auto& e : sorted_entries(in.sysroot_usr)) {
            const auto name = e.path().filename().string();
            std::error_code ec;
            if (name == "lib" && e.is_directory(ec)) {
                for (auto& l : sorted_entries(e.path()))
                    claim("usr/lib/" + l.path().filename().string(), l.path(), "sysroot");
            } else if (name == "share" && e.is_directory(ec)) {
                for (auto& shared : sorted_entries(e.path()))
                    claim("usr/share/" + shared.path().filename().string(), shared.path(), "sysroot");
            } else if (name != "bin" && name != "sbin" && name != "lib64") {
                claim("usr/" + name, e.path(), "sysroot");
            }
        }
    }
    return p;
}

// ── generations ──────────────────────────────────────────────────────

std::vector<int> generations(const fs::path& subos) {
    std::vector<int> out;
    for (auto& e : sorted_entries(subos / std::string(kGenerations))) {
        const auto name = e.path().filename().string();
        int k = 0;
        auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), k);
        if (ec == std::errc{} && ptr == name.data() + name.size() && k > 0) out.push_back(k);
    }
    std::ranges::sort(out);
    return out;
}

std::optional<int> current(const fs::path& subos) {
    std::error_code ec;
    const auto target = platform::read_symlink(subos / std::string(kPointer), ec);
    if (ec) return std::nullopt;
    const auto name = target.filename().string();
    int k = 0;
    auto [ptr, err] = std::from_chars(name.data(), name.data() + name.size(), k);
    if (err != std::errc{} || ptr != name.data() + name.size() || k <= 0
        || target != fs::path(kGenerations) / std::to_string(k)) return std::nullopt;
    return k;
}

fs::path usr_of(const fs::path& subos) { return subos / std::string(kPointer) / "usr"; }

std::optional<GenerationInfo> info(const fs::path& subos, int generation) {
    auto j = nlohmann::json::parse(read_text(generation_dir(subos, generation) / "generation.json"),
                                   nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    try {
        GenerationInfo g;
        g.number = generation;
        g.created = j.value("created", "");
        g.reason = j.value("reason", "");
        g.links = j.value("links", std::size_t{0});
        const auto conflicts = j.value("conflicts", nlohmann::json::array());
        if (!conflicts.is_array()) return std::nullopt;
        for (const auto& c : conflicts)
            g.conflicts.push_back({c.value("path", ""), c.value("kept", ""), c.value("dropped", "")});
        return g;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

std::expected<void, std::string> switch_to(const fs::path& subos, int generation, Flush flush, Verify verify) {
    const bool payloads = verify == Verify::TreeAndPayloads;
    std::error_code ec;
    fs::path gone;
    switch (check_inventory(subos, generation, &gone, payloads)) {
    case Inventory::Matches: break;
    case Inventory::Differs:
        if (!gone.empty())
            return std::unexpected(std::format("generation {} links into {}, which is gone; reinstall it "
                                               "or choose another generation", generation, gone.string()));
        [[fallthrough]];
    case Inventory::Missing:
        if (!owned_generation(subos, generation))
            return std::unexpected(std::format("generation {} is absent or is not an intact projection", generation));
        if (auto missing = payloads ? missing_payload(subos, generation) : std::nullopt)
            return std::unexpected(std::format("generation {} links into {}, which is gone; reinstall it "
                                               "or choose another generation", generation, missing->string()));
    }
    const auto pointer = subos / std::string(kPointer);
    const auto status = fs::symlink_status(pointer, ec);
    if (ec && ec != std::errc::no_such_file_or_directory)
        return std::unexpected("cannot inspect root pointer: " + ec.message());
    if (fs::exists(status) && !fs::is_symlink(status))
        return std::unexpected("root pointer is not a symlink; it is left alone");
    auto scratch = OwnedStage::create(subos);
    if (!scratch) return std::unexpected(scratch.error());
    const auto staged = scratch->path() / "pointer";
    fs::create_directory_symlink(fs::path(kGenerations) / std::to_string(generation), staged, ec);
    if (ec) return std::unexpected("cannot stage root pointer: " + ec.message());
    fs::rename(staged, pointer, ec);
    if (ec) return std::unexpected("cannot move root pointer: " + ec.message());
    if (flush == Flush::Durable) sync_dir(subos);
    return {};
}

std::expected<int, std::string> commit(const fs::path& subos, const Plan& p,
                                       std::string_view reason) {
    std::set<fs::path> names;
    for (const auto& link : p.links) {
        const fs::path rel = link.rel;
        if (!safe_relative_link(rel) || !names.insert(rel).second)
            return std::unexpected("invalid projection path: " + link.rel);
        if (link.rel.find_first_of("\t\n\r") != std::string::npos
            || link.target.generic_string().find_first_of("\t\n\r") != std::string::npos)
            return std::unexpected("projection path cannot contain record separators");
    }
    for (const auto& rel : names)
        for (auto parent = rel.parent_path(); !parent.empty(); parent = parent.parent_path())
            if (names.contains(parent)) return std::unexpected("projection link has a link as parent: " + rel.string());
    const auto gens = generations(subos);
    const auto now = current(subos);
    const auto print = fingerprint(p);
    if (now) {
        std::string checkedRecords;
        if (check_inventory(subos, *now) == Inventory::Matches) {
            if (auto text = read_checked_text(generation_dir(subos, *now) / "links.tsv")) checkedRecords = *text;
        } else if (!owned_generation(subos, *now, &checkedRecords)) {
            checkedRecords.clear();
        }
        if (!checkedRecords.empty() && checkedRecords == print) return *now;
    }
    if (!gens.empty() && gens.back() == std::numeric_limits<int>::max())
        return std::unexpected("generation numbers are exhausted");
    const int next = gens.empty() ? 1 : gens.back() + 1;
    const auto dir = generation_dir(subos, next);
    auto scratch = OwnedStage::create(subos / std::string(kGenerations));
    if (!scratch) return std::unexpected(scratch.error());
    const auto staging = scratch->path() / "generation";
    std::error_code ec;
    fs::create_directories(staging / "usr", ec);
    if (ec) return std::unexpected("cannot create generation: " + ec.message());
    for (const auto& link : p.links) {
        const auto at = staging / link.rel;
        fs::create_directories(at.parent_path(), ec);
        if (ec) return std::unexpected("cannot create projection directory: " + ec.message());
        fs::create_symlink(link.target, at, ec);
        if (ec) return std::unexpected(std::format("cannot link {}: {}", link.rel, ec.message()));
    }
    {
        std::ofstream records(staging / "links.tsv", std::ios::binary);
        records << print;
        records.close();
        nlohmann::json metadata{{"number", next}, {"created", utc_now()}, {"reason", std::string(reason)},
                               {"links", p.links.size()}, {"conflicts", conflicts_json(p.conflicts)}};
        std::ofstream manifest(staging / "generation.json");
        manifest << metadata.dump(2) << "\n";
        manifest.close();
        if (!records || !manifest) return std::unexpected("cannot write generation metadata");
    }
    // Durable before it is published: the records, then every directory the
    // links were created in, deepest first.
    std::set<fs::path> directories{fs::path{}, fs::path{"usr"}};
    std::set<fs::path> payloads;
    for (const auto& link : p.links) {
        for (auto parent = fs::path(link.rel).parent_path(); !parent.empty(); parent = parent.parent_path())
            directories.insert(parent);
        if (auto root = store::payload_root(link.target)) payloads.insert(*root);
    }
    sync_file(staging / "links.tsv");
    sync_file(staging / "generation.json");
    std::vector<fs::path> deepest(directories.begin(), directories.end());
    std::ranges::sort(deepest, std::greater<>{}, [](const fs::path& d) {
        return std::ranges::distance(d.begin(), d.end());
    });
    for (const auto& d : deepest) sync_dir(d.empty() ? staging : staging / d);
    if (auto placed = platform::rename_no_replace(staging, dir); !placed)
        return std::unexpected(std::format("cannot place generation {}: {}", next, placed.error()));
    sync_dir(subos / std::string(kGenerations));
    if (auto inventory = write_inventory(subos, next, directories, payloads); !inventory)
        trace_sync("inventory-skipped", inventory_path(subos, next));   // the full check still works
    if (auto sw = switch_to(subos, next, Flush::Durable, Verify::Tree); !sw) return std::unexpected(sw.error());
    return next;
}

std::expected<std::vector<fs::path>, std::string> linked_targets(const fs::path& subos, int generation) {
    const auto file = generation_dir(subos, generation) / "links.tsv";
    const auto text = read_checked_text(file);
    if (!text) return std::unexpected(file.string() + ": cannot be read");
    std::vector<fs::path> out;
    std::istringstream records(*text);
    for (std::string line; std::getline(records, line);) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) return std::unexpected(file.string() + ": malformed record");
        out.emplace_back(line.substr(tab + 1));
    }
    return out;
}

std::expected<void, std::string> validate_generation(const fs::path& subos, int generation) {
    if (generation <= 0 || !owned_generation(subos, generation))
        return std::unexpected(generation_dir(subos, generation).string()
            + ": projection is incomplete, modified, or not owned by xlings");
    return {};
}

std::vector<int> prune(const fs::path& subos, std::size_t keep, std::span<const int> pinned) {
    std::vector<int> removed;
    auto gens = generations(subos);
    const auto now = current(subos);
    std::error_code pointerError;
    const auto pointer = fs::symlink_status(subos / std::string(kPointer), pointerError);
    if (!now && (fs::exists(pointer) || (pointerError && pointerError != std::errc::no_such_file_or_directory)))
        return removed;
    std::size_t kept = 0;
    for (auto it = gens.rbegin(); it != gens.rend(); ++it) {
        const int k = *it;
        if (now && k == *now) continue;
        if (std::ranges::find(pinned, k) != pinned.end()) continue;
        if (kept < keep) { ++kept; continue; }
        if (!owned_generation(subos, k)) continue;
        std::error_code ec;
        fs::remove_all(generation_dir(subos, k), ec); // subos-remove-all-ok: intact manifest and complete inventory prove derived generation ownership
        if (!ec) {
            fs::remove(inventory_path(subos, k), ec);
            removed.push_back(k);
        }
    }
    std::ranges::sort(removed);
    return removed;
}

// ── a root tree ──────────────────────────────────────────────────────

std::expected<void, std::string> lay_out(const fs::path& root, const fs::path& usr_target,
                                         const fs::path& home) {
    std::error_code ec;
    auto machine = platform::machine_etc::Directory::open(root, true);
    if (!machine) return std::unexpected(machine.error());

    // /usr: replaced only when it names something else (a SubOS moved).
    const auto usr = root / "usr";
    if (fs::is_symlink(usr, ec)) {
        if (fs::read_symlink(usr, ec) != usr_target) {
            auto scratch = OwnedStage::create(root);
            if (!scratch) return std::unexpected(scratch.error());
            const auto staged = scratch->path() / "usr";
            fs::create_directory_symlink(usr_target, staged, ec);
            if (!ec) fs::rename(staged, usr, ec);
            if (ec) return std::unexpected(std::format("cannot repoint {}: {}", usr.string(), ec.message()));
        }
    } else if (!fs::exists(usr, ec)) {
        fs::create_directory_symlink(usr_target, usr, ec);
        if (ec) return std::unexpected(std::format("cannot link {}: {}", usr.string(), ec.message()));
    } else {
        return std::unexpected(std::format("{} is a directory, not a projection; it is left alone",
                                           usr.string()));
    }

    for (auto [name, target] : {std::pair{"bin", "usr/bin"}, std::pair{"sbin", "usr/bin"},
                                std::pair{"lib", "usr/lib"}, std::pair{"lib64", "usr/lib64"}}) {
        const auto at = root / name;
        const auto status = fs::symlink_status(at, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            return std::unexpected("cannot inspect root entry: " + ec.message());
        if (!fs::exists(status)) {
            fs::create_directory_symlink(target, at, ec);
            if (ec) return std::unexpected("cannot create merged-usr link: " + ec.message());
        }
    }
    for (auto d : {"etc", "var/tmp", "var/log", "var/lib", "var/cache", "home", "srv", "tmp", "proc",
                   "sys", "dev", "run", "mnt", "opt", "root"}) {
        std::optional<unsigned> mode;
        if (std::string_view(d) == "root") mode = 0700;
        else if (std::string_view(d) == "tmp" || std::string_view(d) == "var/tmp") mode = 01777;
        auto directory = machine->ensure_directory(d, mode);
        if (!directory) return std::unexpected(directory.error());
        if (!*directory) return std::unexpected("root directory is blocked by a user file: " + (root / d).string());
    }
    if (!home.empty()) {
        auto directory = machine->ensure_directory(home.relative_path());
        if (!directory) return std::unexpected(directory.error());
        if (!*directory) return std::unexpected("system home mount point is blocked by a user file: " + home.string());
    }
    return {};
}

namespace {

using MachineDirectory = platform::machine_etc::Directory;

std::expected<std::optional<fs::path>, std::string> factory_directory(const fs::path& source) {
    std::error_code ec;
    const auto status = fs::symlink_status(source, ec);
    if (ec == std::errc::no_such_file_or_directory || (!ec && !fs::exists(status)))
        return std::optional<fs::path>{};
    if (ec) return std::unexpected("cannot inspect factory " + source.string() + ": " + ec.message());
    const auto canonical = fs::canonical(source, ec);
    if (ec) return std::unexpected("cannot resolve factory " + source.string() + ": " + ec.message());
    if (!fs::is_directory(canonical, ec) || ec)
        return std::unexpected("factory is not a readable directory: " + source.string());
    return std::optional<fs::path>{canonical};
}

// Projection leaves can themselves be links into payloads. Read a passwd or
// group factory through its canonical regular file; destination authority is
// always the anchored machine directory, never the source's canonical path.
std::expected<std::string, std::string> factory_text(const fs::path& file) {
    std::error_code ec;
    const auto canonical = fs::canonical(file, ec);
    if (ec) return std::unexpected("cannot resolve factory file " + file.string() + ": " + ec.message());
    auto source = MachineDirectory::open(canonical.parent_path());
    if (!source) return std::unexpected(source.error());
    auto text = source->read_regular(canonical.filename());
    if (!text) return std::unexpected(text.error());
    if (!*text) return std::unexpected("factory file disappeared: " + file.string());
    return std::move(**text);
}

std::expected<std::vector<std::string>, std::string>
fill_factory(MachineDirectory& destination, const fs::path& prefix, const fs::path& factory) {
    std::vector<std::string> added;
    auto source = factory_directory(factory);
    if (!source) return std::unexpected(source.error());
    if (!*source) return added;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(**source, ec), end; !ec && it != end; it.increment(ec)) {
        // relative() resolves source symlinks and can manufacture '../...'.
        // The iterator's spelling, relative to its base, owns the destination.
        const auto rel = it->path().lexically_relative(**source);
        const auto at = prefix / rel;
        if (rel.empty() || rel.is_absolute() || std::ranges::any_of(rel, [](const fs::path& c) { return c == ".."; }))
            return std::unexpected("factory entry escapes its directory: " + it->path().string());
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_directory(status)) {
            auto directory = destination.ensure_directory(at);
            if (!directory) return std::unexpected(directory.error());
            if (!*directory) it.disable_recursion_pending();
            continue;
        }
        if (!fs::is_regular_file(status) && !fs::is_symlink(status))
            return std::unexpected("unsupported factory entry: " + it->path().string());
        std::expected<bool, std::string> created { false };
        if (at == "passwd" || at == "group") {
            auto existing = destination.read_regular(at, true);
            if (!existing) return std::unexpected(existing.error());
            if (*existing) continue;
            auto text = factory_text(it->path());
            if (!text) return std::unexpected(text.error());
            created = destination.write_missing(at, *text);
        } else {
            // Keep the projection's original logical spelling across prefixes.
            created = destination.link_missing(at, factory / rel);
        }
        if (!created) return std::unexpected(created.error());
        if (*created) added.push_back(at.generic_string());
    }
    if (ec) return std::unexpected("cannot enumerate factory " + factory.string() + ": " + ec.message());
    return added;
}

} // namespace

std::expected<std::vector<std::string>, std::string> fill_etc(const fs::path& etc, const fs::path& factory) {
    auto destination = MachineDirectory::open(etc, true);
    if (!destination) return std::unexpected(destination.error());
    return fill_factory(*destination, {}, factory);
}

std::expected<std::vector<std::string>, std::string> fill_machine_etc(const fs::path& etc, const fs::path& subos) {
    constexpr std::array<std::string_view, 4> kViewEtc{"passwd", "group", "hosts", "nsswitch.conf"};
    auto destination = MachineDirectory::open(etc, true);
    if (!destination) return std::unexpected(destination.error());
    auto added = fill_factory(*destination, {}, subos / std::string(kPointer) / "usr" / "share" / "factory" / "etc");
    if (!added) return std::unexpected(added.error());
    auto source = factory_directory(subos / "etc");
    if (!source) return std::unexpected(source.error());
    if (!*source) return added;
    std::error_code ec;
    for (fs::directory_iterator it(**source, ec), end; !ec && it != end; it.increment(ec)) {
        const auto n = it->path().filename().string();
        if (std::ranges::find(kViewEtc, n) != kViewEtc.end()) continue;
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_directory(status)) {
            auto directory = destination->ensure_directory(n);
            if (!directory) return std::unexpected(directory.error());
            if (!*directory) continue;
            auto files = fill_factory(*destination, n, subos / "etc" / n);
            if (!files) return std::unexpected(files.error());
            added->insert(added->end(), files->begin(), files->end());
        } else if (fs::is_regular_file(status) || fs::is_symlink(status)) {
            auto created = destination->link_missing(n, subos / "etc" / n);
            if (!created) return std::unexpected(created.error());
            if (*created) added->push_back(n);
        } else return std::unexpected("unsupported SubOS /etc entry: " + it->path().string());
    }
    if (ec) return std::unexpected("cannot enumerate SubOS /etc: " + ec.message());
    return added;
}

namespace {

// "u name uid \"gecos\" home shell" -> fields, quotes honoured.
std::vector<std::string> sysusers_fields(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    bool quoted = false, any = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (any || !cur.empty()) { f.push_back(cur); cur.clear(); any = false; }
            continue;
        }
        cur.push_back(c);
    }
    if (any || !cur.empty()) f.push_back(cur);
    return f;
}

}  // namespace

std::expected<std::vector<std::string>, std::string> apply_sysusers(const fs::path& etc, const fs::path& usr) {
    auto destination = MachineDirectory::open(etc, true);
    if (!destination) return std::unexpected(destination.error());
    // A factory DB is copied exclusively before this step. Unknown links and
    // shared inodes must never be materialized or appended through.
    for (const auto name : {"passwd", "group"}) {
        auto observed = destination->read_regular(name, true);
        if (!observed) return std::unexpected(observed.error());
    }
    std::vector<std::pair<std::string, std::string>> users;
    std::vector<std::pair<std::string, std::string>> groups;
    auto add_group = [&](const std::string& name, const std::string& gid) {
        groups.emplace_back(name, std::format("{}:x:{}:\n", name, gid));
    };
    auto add_user = [&](const std::string& name, const std::string& uid, const std::string& gecos,
                        const std::string& home, const std::string& shell) {
        add_group(name, uid);
        users.emplace_back(name, std::format("{}:x:{}:{}:{}:{}:{}\n", name, uid, uid, gecos, home, shell));
    };
    add_user("root", "0", "root", "/root", "/bin/sh");
    auto confDirectory = factory_directory(usr / "lib" / "sysusers.d");
    if (!confDirectory) return std::unexpected(confDirectory.error());
    if (*confDirectory) {
        std::vector<fs::path> confs;
        std::error_code ec;
        for (fs::directory_iterator it(**confDirectory, ec), end; !ec && it != end; it.increment(ec))
            if (it->path().extension() == ".conf") confs.push_back(it->path());
        if (ec) return std::unexpected("cannot enumerate sysusers.d: " + ec.message());
        std::ranges::sort(confs);
        for (const auto& conf : confs) {
            auto text = factory_text(conf);
            if (!text) return std::unexpected(text.error());
            std::istringstream in{*text};
            for (std::string line; std::getline(in, line);) {
                auto f = sysusers_fields(line);
                if (f.size() < 3 || f[0].starts_with('#')) continue;
                if (f[0] == "g") add_group(f[1], f[2]);
                else if (f[0] == "u")
                    add_user(f[1], f[2], f.size() > 3 && f[3] != "-" ? f[3] : f[1],
                             f.size() > 4 && f[4] != "-" ? f[4] : "/",
                             f.size() > 5 && f[5] != "-" ? f[5] : "/bin/sh");
            }
        }
    }
    auto groupNames = destination->append_named("group", groups);
    if (!groupNames) return std::unexpected(groupNames.error());
    auto userNames = destination->append_named("passwd", users);
    if (!userNames) return std::unexpected(userNames.error());
    std::vector<std::string> added;
    for (const auto& name : *groupNames) added.push_back("group " + name);
    for (const auto& name : *userNames) added.push_back("user " + name);
    return added;
}

std::optional<std::string> host_of(const fs::path& root, const fs::path& home) {
    std::error_code ec;
    const auto target = fs::read_symlink(root / "usr", ec);
    if (ec) return std::nullopt;
    // <home>/subos/<name>/root/usr
    const auto rel = target.lexically_relative(home / "subos");
    auto it = rel.begin();
    if (rel.empty() || it == rel.end() || it->string() == "..") return std::nullopt;
    const auto name = it->string();
    if (++it == rel.end() || it->string() != kPointer) return std::nullopt;
    if (++it == rel.end() || it->string() != "usr" || ++it != rel.end()) return std::nullopt;
    return name;
}

std::optional<int> running_projection(const fs::path& root, const fs::path& scope) {
    std::error_code ec;
    const auto target = platform::read_symlink(root / "usr", ec);
    if (ec || !target.is_absolute() || target.filename() != "usr") return std::nullopt;
    const auto parent = target.parent_path();
    fs::path owner;
    if (parent.filename() == kPointer) owner = parent.parent_path();
    else if (parent.parent_path().filename() == kGenerations) {
        const auto text = parent.filename().string();
        int generation = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), generation);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || generation <= 0)
            return std::nullopt;
        owner = parent.parent_path().parent_path();
    } else return std::nullopt;
    if (owner != scope && (!fs::equivalent(owner, scope, ec) || ec)) return std::nullopt;
    for (const int generation : generations(scope)) {
        ec.clear();
        if (fs::equivalent(root / "usr", generation_dir(scope, generation) / "usr", ec) && !ec)
            return generation;
    }
    return std::nullopt;
}

}  // namespace xlings::subos::rootfs
