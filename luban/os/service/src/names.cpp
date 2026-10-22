// A name while it is made (design 2026-10-20 WuXian §11, H1): held from the
// check to the end, so two makings -- two luban commands, the window's quick
// start and a command -- never take one name; and the record an unfinished
// making left, said as what it is (service.cppm).
module luban.service;

import std;
import xlings.platform;
import xlings.subos.home_view;

namespace luban::service {

namespace {

fs::path names_dir_(const fs::path& home) { return home / "run" / "luban" / "names"; }

// The names this process holds now, by their reservation files: the Luban
// OS app makes several systems in one process, so the process id alone does
// not say a name is free (design 2026-10-21 §7.3, J7).
std::mutex held_lock_;
std::set<fs::path>& held_() {
    static std::set<fs::path> held;
    return held;
}

// The process holding a reservation (0: unreadable).
int holder_(const fs::path& file) {
    std::ifstream in(file);
    int pid = 0;
    in >> pid;
    return pid;
}

}  // namespace

NameReservation::~NameReservation() {
    if (file_.empty()) return;
    std::error_code ec;
    fs::remove(file_, ec);
    std::scoped_lock guard(held_lock_);
    held_().erase(file_);
}

std::expected<NameReservation, std::string> reserve_name(const fs::path& home, std::string_view name) {
    const auto dir = names_dir_(home);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto file = dir / std::string(name);
    const auto pid = xlings::platform::get_pid();
    const auto since = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::scoped_lock guard(held_lock_);
    // Held by a making in this process (another of the window's tasks).
    if (held_().contains(file)) return std::unexpected(std::format("'{}' is being made", name));
    for (int attempt = 0; attempt < 2; ++attempt) {
        // Written whole beside it, then moved into place only where nothing is.
        const auto staging = dir / std::format(".{}.{}", std::string(name), pid);
        xlings::platform::write_file_atomic(staging.string(), std::format("{} {}\n", pid, since));
        auto moved = xlings::platform::rename_no_replace(staging, file);
        fs::remove(staging, ec);
        if (moved) {
            held_().insert(file);
            return NameReservation(file);
        }
        // Held: by another process that still runs, said; one that is no
        // longer (or this process's id, recorded before, held by none of its
        // makings), taken over.
        const int other = holder_(file);
        if (other > 0 && other != static_cast<int>(pid) && xlings::platform::is_process_alive(other))
            return std::unexpected(std::format("'{}' is being made (process {})", name, other));
        fs::remove(file, ec);
    }
    return std::unexpected(std::format("'{}' could not be held for the making ({})", name, file.string()));
}

bool name_reserved(const fs::path& home, std::string_view name) {
    const auto file = names_dir_(home) / std::string(name);
    std::error_code ec;
    if (!fs::exists(file, ec)) return false;
    const int pid = holder_(file);
    return pid > 0 && xlings::platform::is_process_alive(pid);
}

bool name_left_over(const fs::path& home, std::string_view name) {
    const xlings::subos::HomeView view{home};
    std::error_code ec;
    return !name.empty() && !fs::is_directory(view.instance(name), ec) && fs::exists(view.config_dir(name), ec)
           && !name_reserved(home, name);
}

std::vector<Unfinished> unfinished(const fs::path& home) {
    std::vector<Unfinished> out;
    for (const auto& dir : {home / "subos", home / "domains" / "xlings" / "private" / "subos"}) {
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const auto file = it->path().filename().string();
            constexpr std::string_view prefix = ".unfinished-";
            if (!file.starts_with(prefix) || !it->is_directory(ec)) continue;
            auto name = file.substr(prefix.size());
            if (const auto dash = name.rfind('-'); dash != std::string::npos) name.resize(dash);
            out.push_back({name, it->path()});
        }
    }
    std::ranges::sort(out, {}, &Unfinished::name);
    return out;
}

std::size_t remove_unfinished(const fs::path& home) {
    std::size_t removed = 0;
    for (const auto& u : unfinished(home)) {
        std::error_code ec;
        // A making's half, set aside and never entered: asked before.
        fs::permissions(u.path, fs::perms::owner_all, fs::perm_options::add, ec);
        for (fs::recursive_directory_iterator it(u.path, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
            if (it->is_directory(ec) && !it->is_symlink(ec)) fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        ec.clear();
        if (fs::remove_all(u.path, ec) > 0 && !ec) ++removed;
    }
    return removed;
}

}  // namespace luban::service
