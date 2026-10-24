// openhal.display: a display as a device on a bus (SPEC.md §6, openhal 0.3)
// -- what one is (its kind, its mode), the role a desktop gives it, and the
// names its endpoints and properties are offered with. What draws on it is
// opensal.wire; what keeps or shows the picture is the implementation's (a
// window, openhal-emu's virtual display).
export module openhal.display;

import std;

export namespace openhal::display {

inline constexpr std::string_view kInterface = "openhal.display@0.3";
inline constexpr std::string_view kControlInterface = "openhal.display-control@0.3";
inline constexpr std::string_view kInputInterface = "openhal.input@0.3";

enum class Kind : std::uint8_t { physical, window, virtual_, remote };

constexpr std::string_view name_of(Kind k) {
    switch (k) {
    case Kind::physical: return "physical";
    case Kind::window: return "window";
    case Kind::virtual_: return "virtual";
    case Kind::remote: return "remote";
    }
    return "virtual";
}
std::optional<Kind> kind_of(std::string_view name);

// Device pixels, and device pixels a logical pixel.
struct Mode {
    std::uint32_t width { 1920 }, height { 1080 };
    double scale { 1.0 };
    bool operator==(const Mode&) const = default;
};
// "1920x1080@1.25"; "1920x1080" is scale 1. Sides 64 to 16384, scale 0.5 to 4.
std::string format_mode(const Mode& m);
std::optional<Mode> parse_mode(std::string_view text);

struct Role {
    enum class Kind : std::uint8_t { primary, mirror, extend } kind { Kind::primary };
    std::uint32_t of { 0 };               // mirror, extend: the display it is set against
    std::string side;                     // extend: left, right, above, below
    bool operator==(const Role&) const = default;
};
// "primary", "mirror 1", "extend 1 right".
std::string format_role(const Role& r);
std::optional<Role> parse_role(std::string_view text);

// Where a display and its control are on a bus, under a root.
inline std::string device_path(std::string_view root, std::uint32_t n) { return std::format("{}/device/display/{}", root, n); }
inline std::string control_path(std::string_view device) { return std::string(device) + "/control"; }

}  // namespace openhal::display
