// openhal: devices (SPEC.md).
//
// The question is "which X": a machine has several GPUs, displays, audio
// endpoints, input devices, and a program wants to know which, with what each
// can do. Several implementations live in one program (one per platform, a
// fake for tests, a remote one), so the contract is a concept, not a set of C
// functions.
//
// Version 0.1 has one interface: enumeration. A source lists the devices of
// one kind; it answers "these", "none" (an empty list) or "could not read"
// (an error with its reason) -- never an empty list for a failure.
export module openhal;

import std;

export namespace openhal {

enum class Kind : std::uint8_t {
    Gpu,
    Display,
    AudioOutput,
    AudioInput,
    Input,
};

inline constexpr std::array<Kind, 5> kKinds{Kind::Gpu, Kind::Display, Kind::AudioOutput, Kind::AudioInput, Kind::Input};

constexpr std::string_view name_of(Kind k) {
    switch (k) {
    case Kind::Gpu: return "gpu";
    case Kind::Display: return "display";
    case Kind::AudioOutput: return "audio-output";
    case Kind::AudioInput: return "audio-input";
    case Kind::Input: return "input";
    }
    return "unknown";
}

struct Device {
    Kind kind { Kind::Gpu };
    // Stable for as long as the device stays where it is: the same device
    // listed twice has the same id. Unique within its kind.
    std::string id;
    std::string name;                               // for a person: "NVIDIA GeForce RTX 4090", "DELL U2723QE"
    std::string vendor;                             // "nvidia", "amd", "intel", "apple", "virtio", ... or empty
    std::map<std::string, std::string> traits;      // what it can do: "resolution" = "3840x2160", "builtin" = "1"
};

// What could not be read, and why -- never an empty list in its place.
struct Unreadable {
    std::string reason;
};

using Listing = std::expected<std::vector<Device>, Unreadable>;

// A device source: lists the devices of one kind, and names itself.
template <class S>
concept Source = requires(const S& s, Kind k) {
    { s.list(k) } -> std::same_as<Listing>;
    { s.name() } -> std::convertible_to<std::string_view>;
};

}  // namespace openhal
