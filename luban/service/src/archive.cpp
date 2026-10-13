module luban.service.archive;

import std;
import xlings.libs.json;

namespace luban::service::archive {

namespace {

constexpr std::size_t kBlock = 512;

void octal_(char* field, std::size_t width, std::uint64_t value) {
    // width-1 digits and a NUL, zero padded.
    auto text = std::format("{:0{}o}", value, width - 1);
    if (text.size() > width - 1) text = text.substr(text.size() - (width - 1));
    std::memcpy(field, text.data(), text.size());
    field[width - 1] = '\0';
}

std::uint64_t parse_octal_(const char* field, std::size_t width) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width && field[i]; ++i)
        if (field[i] >= '0' && field[i] <= '7') v = v * 8 + static_cast<std::uint64_t>(field[i] - '0');
    return v;
}

std::string pax_record_(std::string_view key, std::string_view value) {
    // "<len> <key>=<value>\n", where len counts itself.
    const auto body = std::format(" {}={}\n", key, value);
    auto len = body.size() + 1;
    while (std::to_string(len).size() + body.size() != len) ++len;
    return std::to_string(len) + body;
}

void header_(std::ostream& out, std::string_view name, char type, std::uint64_t size, unsigned mode,
             std::string_view link = {}) {
    std::array<char, kBlock> h{};
    std::memcpy(h.data(), name.data(), std::min<std::size_t>(name.size(), 100));
    octal_(h.data() + 100, 8, mode);
    octal_(h.data() + 108, 8, 0);
    octal_(h.data() + 116, 8, 0);
    octal_(h.data() + 124, 12, size);
    octal_(h.data() + 136, 12, 0);
    std::memset(h.data() + 148, ' ', 8);
    h[156] = type;
    std::memcpy(h.data() + 157, link.data(), std::min<std::size_t>(link.size(), 100));
    std::memcpy(h.data() + 257, "ustar", 6);
    std::memcpy(h.data() + 263, "00", 2);
    unsigned sum = 0;
    for (char c : h) sum += static_cast<unsigned char>(c);
    octal_(h.data() + 148, 7, sum);
    h[155] = ' ';
    out.write(h.data(), kBlock);
}

void pad_(std::ostream& out, std::uint64_t size) {
    static const std::array<char, kBlock> zero{};
    if (const auto rest = size % kBlock) out.write(zero.data(), static_cast<std::streamsize>(kBlock - rest));
}

void put_(std::ostream& out, const std::string& name, char type, std::string_view data, unsigned mode,
          std::string_view link = {}) {
    if (name.size() > 100 || link.size() > 100) {
        std::string pax;
        if (name.size() > 100) pax += pax_record_("path", name);
        if (link.size() > 100) pax += pax_record_("linkpath", link);
        header_(out, "././@PaxHeader", 'x', pax.size(), 0644);
        out.write(pax.data(), static_cast<std::streamsize>(pax.size()));
        pad_(out, pax.size());
    }
    header_(out, name, type, data.size(), mode, link);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    pad_(out, data.size());
}

std::string field_(const char* p, std::size_t width) {
    std::size_t n = 0;
    while (n < width && p[n]) ++n;
    return std::string(p, n);
}

bool safe_(std::string_view rel) {
    if (rel.empty() || rel.front() == '/') return false;
    for (std::string_view rest = rel; !rest.empty();) {
        const auto slash = rest.find('/');
        if (rest.substr(0, slash) == "..") return false;
        if (slash == std::string_view::npos) break;
        rest.remove_prefix(slash + 1);
    }
    return true;
}

}  // namespace

std::span<const std::string_view> state_dirs() {
    static constexpr std::array<std::string_view, 6> value{"etc", "root", "home", "var", "opt", "srv"};
    return value;
}

std::expected<std::size_t, std::string> write(const fs::path& file, const nlohmann::json& manifest, const fs::path& root,
                                              const fs::path& home) {
    const auto home_prefix = home.empty() ? std::string{} : (home.lexically_normal() / "").generic_string();
    std::error_code ec;
    if (fs::exists(file, ec)) return std::unexpected(file.string() + " exists; name a new file");
    const auto staged = fs::path(file.string() + ".partial");
    std::ofstream out(staged, std::ios::binary | std::ios::trunc);
    if (!out) return std::unexpected("cannot write " + staged.string());
    const auto m = manifest.dump(2) + "\n";
    put_(out, "manifest.json", '0', m, 0644);
    std::size_t count = 0;
    for (const auto dir : state_dirs()) {
        const auto base = root / dir;
        if (!fs::is_directory(base, ec) || fs::is_symlink(base, ec)) continue;
        put_(out, "state/" + std::string(dir) + "/", '5', {}, 0755);
        for (fs::recursive_directory_iterator it(base, fs::directory_options::none, ec), end; !ec && it != end;
             it.increment(ec)) {
            // Lexically: fs::relative resolves a link first, naming the
            // entry by where it points (outside the root, for a store link).
            const auto rel = "state/" + it->path().lexically_relative(root).generic_string();
            const auto st = it->symlink_status(ec);
            const unsigned mode = static_cast<unsigned>(st.permissions()) & 07777;
            if (fs::is_symlink(st)) {
                const auto target = fs::read_symlink(it->path(), ec).generic_string();
                it.disable_recursion_pending();
                if (!home_prefix.empty() && target.starts_with(home_prefix)) continue;
                put_(out, rel, '2', {}, 0777, target);
            } else if (fs::is_directory(st)) {
                put_(out, rel + "/", '5', {}, mode);
            } else if (fs::is_regular_file(st)) {
                std::ifstream in(it->path(), std::ios::binary);
                std::string data((std::istreambuf_iterator<char>(in)), {});
                put_(out, rel, '0', data, mode);
            } else {
                continue;   // sockets, fifos, devices are not state
            }
            ++count;
        }
        if (ec) return std::unexpected(std::format("cannot read {}: {}", base.string(), ec.message()));
    }
    static const std::array<char, kBlock * 2> end{};
    out.write(end.data(), end.size());
    out.close();
    if (!out) return std::unexpected("writing " + staged.string() + " failed (disk full?)");
    fs::rename(staged, file, ec);
    if (ec) return std::unexpected("cannot place " + file.string() + ": " + ec.message());
    return count;
}

std::expected<nlohmann::json, std::string> read(const fs::path& file, const std::function<void(const Entry&)>& entry) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::unexpected("cannot read " + file.string());
    std::optional<nlohmann::json> manifest;
    std::map<std::string, std::string> pax;
    std::array<char, kBlock> h{};
    while (in.read(h.data(), kBlock)) {
        if (std::all_of(h.begin(), h.end(), [](char c) { return c == 0; })) break;
        if (std::string_view(h.data() + 257, 5) != "ustar") return std::unexpected(file.string() + " is not a Luban archive");
        const auto size = parse_octal_(h.data() + 124, 12);
        std::string data(size, '\0');
        if (size && !in.read(data.data(), static_cast<std::streamsize>(size)))
            return std::unexpected(file.string() + " is cut short");
        if (const auto rest = size % kBlock) in.ignore(static_cast<std::streamsize>(kBlock - rest));
        const char type = h[156];
        if (type == 'x') {
            for (std::string_view r = data; !r.empty();) {
                const auto sp = r.find(' ');
                if (sp == std::string_view::npos) break;
                std::size_t len = 0;
                std::from_chars(r.data(), r.data() + sp, len);
                if (len == 0 || len > r.size()) break;
                const auto rec = r.substr(sp + 1, len - sp - 2);
                const auto eq = rec.find('=');
                if (eq != std::string_view::npos) pax[std::string(rec.substr(0, eq))] = std::string(rec.substr(eq + 1));
                r.remove_prefix(len);
            }
            continue;
        }
        std::string name = pax.contains("path") ? pax["path"] : field_(h.data(), 100);
        std::string link = pax.contains("linkpath") ? pax["linkpath"] : field_(h.data() + 157, 100);
        pax.clear();
        if (name == "manifest.json") {
            auto j = nlohmann::json::parse(data, nullptr, false);
            if (!j.is_object() || j.value("format", "") != kFormat)
                return std::unexpected(file.string() + " is not a Luban environment archive");
            if (j.value("version", 0) > kVersion)
                return std::unexpected(std::format("{} was written by a newer luban (format {}): luban self update",
                                                   file.string(), j.value("version", 0)));
            manifest = j;
            continue;
        }
        if (!name.starts_with("state/")) continue;
        auto rel = name.substr(6);
        while (!rel.empty() && rel.back() == '/') rel.pop_back();
        if (rel.empty()) continue;
        if (!safe_(rel)) return std::unexpected(std::format("{}: '{}' leaves the environment", file.string(), name));
        Entry e;
        e.path = rel;
        e.mode = static_cast<unsigned>(parse_octal_(h.data() + 100, 8));
        if (type == '5') e.type = Entry::Type::Dir;
        else if (type == '2') { e.type = Entry::Type::Symlink; e.target = link; }
        else if (type == '0' || type == '\0') { e.type = Entry::Type::File; e.content = std::move(data); }
        else continue;
        if (entry) entry(e);
    }
    if (!manifest) return std::unexpected(file.string() + " has no manifest");
    return *manifest;
}

std::expected<std::size_t, std::string> extract_state(const fs::path& file, const fs::path& root) {
    std::size_t count = 0;
    std::string failure;
    auto m = read(file, [&](const Entry& e) {
        if (!failure.empty()) return;
        std::error_code ec;
        const auto at = root / fs::path(e.path);
        fs::create_directories(at.parent_path(), ec);
        if (e.type == Entry::Type::Dir) {
            if (fs::is_symlink(at, ec)) fs::remove(at, ec);
            fs::create_directories(at, ec);
        } else {
            if (fs::is_directory(at, ec) && !fs::is_symlink(at, ec)) fs::remove_all(at, ec);
            else fs::remove(at, ec);
            ec.clear();
            if (e.type == Entry::Type::Symlink) fs::create_symlink(e.target, at, ec);
            else {
                std::ofstream out(at, std::ios::binary | std::ios::trunc);
                out << e.content;
                out.close();
                if (!out) ec = std::make_error_code(std::errc::io_error);
                fs::permissions(at, static_cast<fs::perms>(e.mode & 07777), ec);
            }
        }
        if (ec) failure = std::format("{}: {}", at.string(), ec.message());
        else ++count;
    });
    if (!m) return std::unexpected(m.error());
    if (!failure.empty()) return std::unexpected(failure);
    return count;
}

}  // namespace luban::service::archive
