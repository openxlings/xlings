module xlings.core.abi;

import std;
import xlings.libs.json;
import xlings.platform;
import xlings.observe;
import xlings.core.version_order;

namespace xlings::abi {

namespace {

// `<ns>-x-<name>` store directories of the runtime packages.
std::optional<std::string> runtime_package_of_(const std::string& storeName) {
    for (const auto* p : {"glibc", "musl", "gcc-runtime"})
        if (storeName.ends_with(std::string("-x-") + p)) return std::string(p);
    return std::nullopt;
}

std::string arch_of_loader_(std::string_view name) {
    // ld-linux-x86-64.so.2 / ld-linux-aarch64.so.1 / ld-musl-x86_64.so.1
    if (name.find("x86-64") != std::string_view::npos || name.find("x86_64") != std::string_view::npos) return "x86_64";
    if (name.find("aarch64") != std::string_view::npos) return "aarch64";
    return {};
}

}  // namespace

bool is_cxx_runtime(std::string_view package) { return package == "gcc-runtime"; }
bool is_runtime(std::string_view package) {
    return package == "glibc" || package == "musl" || is_cxx_runtime(package);
}

std::optional<Provider> provider_at(std::string_view package, std::string_view version, const fs::path& payload) {
    if (!is_runtime(package)) return std::nullopt;
    Provider p{std::string(package), std::string(version), payload, {}, {}};
    std::error_code ec;
    for (const auto* sub : {"lib64", "lib"}) {
        const auto dir = payload / sub;
        if (!fs::is_directory(dir, ec)) continue;
        if (is_cxx_runtime(package)) {
            if (fs::exists(dir / "libstdc++.so.6", ec) || fs::exists(dir / "libgcc_s.so.1", ec)) {
                p.libdir = dir;
                return p;
            }
            continue;
        }
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const auto name = it->path().filename().string();
            if ((name.starts_with("ld-linux") || name.starts_with("ld-musl")) && name.find(".so") != std::string::npos) {
                p.loader = it->path();
                p.libdir = dir;
                return p;
            }
        }
    }
    return std::nullopt;
}

std::optional<Family> family_of(const Provider& libc) {
    if (libc.loader.empty()) return std::nullopt;
    const auto name = libc.loader.filename().string();
    const auto arch = arch_of_loader_(name);
    if (arch.empty()) return std::nullopt;
    return Family{std::format("linux-{}-{}", arch, libc.package), libc, std::nullopt};
}

fs::path dir(const fs::path& logicalHome, std::string_view tag) { return logicalHome / "abi" / tag; }
fs::path loader_path(const fs::path& logicalHome, std::string_view tag, const fs::path& loaderName) {
    return dir(logicalHome, tag) / loaderName.filename();
}

std::map<std::string, std::string> pins(const fs::path& home) {
    std::map<std::string, std::string> out;
    std::ifstream in(home / ".xlings.json");
    if (!in) return out;
    const auto doc = nlohmann::json::parse(in, nullptr, false);
    if (!doc.is_object()) return out;
    if (auto r = doc.find("runtime"); r != doc.end() && r->is_object())
        for (auto it = r->begin(); it != r->end(); ++it)
            if (it.value().is_string() && it.value().get<std::string>() != "latest") out[it.key()] = it.value().get<std::string>();
    return out;
}

std::vector<Family> select(const fs::path& home, const std::map<std::string, std::string>& pinned) {
    // Every installed runtime payload, by package.
    std::map<std::string, std::vector<Provider>> found;
    std::error_code ec;
    for (fs::directory_iterator s(home / "data" / "xpkgs", ec), end; !ec && s != end; s.increment(ec)) {
        const auto package = runtime_package_of_(s->path().filename().string());
        if (!package) continue;
        std::error_code vec;
        for (fs::directory_iterator v(s->path(), vec), vend; !vec && v != vend; v.increment(vec)) {
            const auto version = v->path().filename().string();
            if (version.starts_with('.') || !v->is_directory(vec)) continue;
            if (fs::exists(v->path() / ".xlings-failed", vec)) continue;
            if (auto p = provider_at(*package, version, v->path())) found[*package].push_back(std::move(*p));
        }
    }
    const auto choose = [&](const std::string& package) -> std::optional<Provider> {
        auto it = found.find(package);
        if (it == found.end() || it->second.empty()) return std::nullopt;
        auto& all = it->second;
        if (auto pin = pinned.find(package); pin != pinned.end())
            for (const auto& p : all)
                if (p.version == pin->second) return p;
        return *std::ranges::max_element(all, [](const Provider& a, const Provider& b) {
            return version_order::compare(a.version, b.version) == std::strong_ordering::less;
        });
    };
    std::vector<Family> out;
    for (const auto* libc : {"glibc", "musl"}) {
        auto chosen = choose(libc);
        if (!chosen) continue;
        auto family = family_of(*chosen);
        if (!family) continue;
        if (std::string_view(libc) == "glibc") family->cxx = choose("gcc-runtime");
        out.push_back(std::move(*family));
    }
    return out;
}

std::vector<std::pair<fs::path, fs::path>> links(const std::vector<Family>& families) {
    std::vector<std::pair<fs::path, fs::path>> out;
    for (const auto& f : families) {
        std::set<std::string> taken;
        const auto add_dir = [&](const fs::path& libdir) {
            std::error_code ec;
            std::vector<fs::path> entries;
            for (fs::directory_iterator it(libdir, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(it->path());
            std::ranges::sort(entries);
            for (const auto& e : entries) {
                const auto name = e.filename().string();
                if (name.find(".so") == std::string::npos) continue;   // shared objects and the loader
                if (!taken.insert(name).second) continue;              // the libc's own wins
                out.emplace_back(fs::path(f.tag) / name, e);
            }
        };
        add_dir(f.libc.libdir);
        if (f.cxx) add_dir(f.cxx->libdir);
    }
    return out;
}

std::expected<Switched, std::string> refresh(const fs::path& home, std::string_view reason) {
    Switched result;
    if constexpr (!platform::is_linux) return result;
    const auto families = select(home, pins(home));
    const auto wanted = links(families);
    // What the current generation holds, to tell whether anything changes.
    const auto current = home / "abi";
    std::error_code ec;
    std::set<std::pair<std::string, std::string>> have, want;
    for (const auto& [rel, target] : wanted) want.emplace(rel.generic_string(), target.generic_string());
    if (fs::is_directory(current, ec))
        for (fs::recursive_directory_iterator it(current, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_symlink(ec))
                have.emplace(it->path().lexically_relative(current).generic_string(),
                             fs::read_symlink(it->path(), ec).generic_string());
    // An unchanged runtime writes nothing -- unless there is no link yet:
    // <home>/abi always exists once asked for, if only as an empty
    // generation, so a root has somewhere to see its own runtime.
    if (have == want && fs::exists(fs::symlink_status(current, ec))) return result;

    // Before / after, per family, for the record.
    nlohmann::json previous = nlohmann::json::object();
    if (std::ifstream in(home / "abi.json"); in) {
        auto doc = nlohmann::json::parse(in, nullptr, false);
        if (doc.is_object() && doc.contains("families")) previous = doc["families"];
    }
    nlohmann::json now = nlohmann::json::object();
    for (const auto& f : families) {
        now[f.tag] = {{"libc", f.libc.package + "@" + f.libc.version}, {"payload", f.libc.payload.generic_string()}};
        if (f.cxx) now[f.tag]["cxx"] = f.cxx->package + "@" + f.cxx->version;
        const auto before = previous.contains(f.tag) ? previous[f.tag].value("libc", std::string("none")) : std::string("none");
        if (before != now[f.tag]["libc"].get<std::string>())
            result.what.push_back(std::format("{} {} -> {}", f.tag, before, now[f.tag]["libc"].get<std::string>()));
    }

    const auto gens = home / "abi.gen";
    fs::create_directories(gens, ec);
    if (ec) return std::unexpected(std::format("cannot write {}: {}", gens.string(), ec.message()));
    int next = 1;
    for (fs::directory_iterator it(gens, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (!name.empty() && std::ranges::all_of(name, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            next = std::max(next, std::stoi(name) + 1);
    }
    const auto gen = gens / std::to_string(next);
    fs::remove_all(gen, ec);  // a half-written one from an interrupted run
    for (const auto& [rel, target] : wanted) {
        const auto at = gen / rel;
        fs::create_directories(at.parent_path(), ec);
        fs::create_symlink(target, at, ec);
        if (ec) return std::unexpected(std::format("cannot link {}: {}", at.string(), ec.message()));
    }
    fs::create_directories(gen, ec);
    // One rename: a process resolves the old runtime or the new, never neither.
    const auto staged = home / ".abi.new";
    fs::remove(staged, ec);
    fs::create_symlink(fs::path("abi.gen") / std::to_string(next), staged, ec);
    if (ec) return std::unexpected(std::format("cannot stage {}: {}", staged.string(), ec.message()));
    if (fs::is_directory(fs::symlink_status(current, ec)) && !fs::is_symlink(fs::symlink_status(current, ec)))
        return std::unexpected(std::format("{} is a directory, not the runtime link; nothing switched", current.string()));
    fs::rename(staged, current, ec);
    if (ec) return std::unexpected(std::format("cannot switch {}: {}", current.string(), ec.message()));
    result.changed = true;

    platform::write_file_atomic((home / "abi.json").string(),
        nlohmann::json{{"generation", next}, {"families", now}}.dump(2) + "\n");
    observe::append_json(home / "logs" / "abi.ndjson",
        {{"ts", observe::utc_now()}, {"generation", next}, {"reason", std::string(reason)},
         {"before", previous}, {"after", now}});
    // The three newest stay, for a rollback by pin.
    std::vector<int> all;
    for (fs::directory_iterator it(gens, ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (!name.empty() && std::ranges::all_of(name, [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            all.push_back(std::stoi(name));
    }
    std::ranges::sort(all, std::greater<>{});
    for (std::size_t i = 3; i < all.size(); ++i) fs::remove_all(gens / std::to_string(all[i]), ec);
    return result;
}

}  // namespace xlings::abi
