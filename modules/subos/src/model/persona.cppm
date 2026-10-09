// The persona of an instance with a neutral identity (Luban design §C4).
//
// Long-lived privacy is a stable, neutral identity that is not yours, not a
// new one each session -- one that changes on every entry is itself the
// anomaly a service flags. So an instance gets a host name (12 hex digits,
// the shape every container has) and a machine-id once, and keeps them; a
// fork or an export is made without this file and gets its own.
//
// It also remembers the time zone the proxy's exit resolved to, and for which
// proxy, so `tz = proxy` asks the network only when the proxy changes or the
// answer is a day old.
export module xlings.subos.persona;

import std;
import xlings.subos.home_view;

export namespace xlings::subos::persona {

struct Persona {
    std::string hostname;        // 12 lowercase hex digits
    std::string machine_id;      // 32 lowercase hex digits
    std::string tz_proxy;        // the proxy the zone below was resolved through
    std::string tz_zone;         // e.g. "Asia/Tokyo"; empty: never resolved
    long long tz_at { 0 };       // seconds since the epoch
};

// The instance's persona, made (and written) the first time it is asked for.
// An unreadable file is an error, never a reason to make a new identity.
std::expected<Persona, std::string> read_or_make(const HomeView& home, std::string_view name);

std::expected<void, std::string> write(const HomeView& home, std::string_view name, const Persona& p);

// "utc" -> "UTC"; an IANA name (Area/City, letters digits _ - + /) as given;
// anything else -> empty (not a zone).
std::string normalize_zone(std::string_view tz);

}  // namespace xlings::subos::persona
