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
}

std::expected<NameReservation, std::string> reserve_name(const fs::path& home, std::string_view name) {
    const auto dir = names_dir_(home);
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto file = dir / std::string(name);
    const auto pid = xlings::platform::get_pid();
    const auto since = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    for (int attempt = 0; attempt < 2; ++attempt) {
        // Written whole beside it, then moved into place only where nothing is.
        const auto staging = dir / std::format(".{}.{}", std::string(name), pid);
        xlings::platform::write_file_atomic(staging.string(), std::format("{} {}\n", pid, since));
        auto moved = xlings::platform::rename_no_replace(staging, file);
        fs::remove(staging, ec);
        if (moved) return NameReservation(file);
        // Held: by a process that is no longer, taken over; else said.
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

}  // namespace luban::service
