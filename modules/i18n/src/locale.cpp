module xlings.i18n;

import std;
import xlings.platform;

namespace xlings::i18n {

std::string normalize(std::string_view tag) {
    if (const auto at = tag.find_first_of(".@"); at != std::string_view::npos) tag = tag.substr(0, at);
    std::string out;
    for (const char c : tag) out += c == '_' ? '-' : c;
    if (out == "auto") return {};
    if (out == "C" || out == "POSIX") return "en";
    // language lower case, region upper case: zh-cn -> zh-CN.
    const auto dash = out.find('-');
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = static_cast<char>(i < dash ? std::tolower(static_cast<unsigned char>(out[i]))
                                             : std::toupper(static_cast<unsigned char>(out[i])));
    return out;
}

std::vector<std::string> fallback_chain(std::string_view tag) {
    std::vector<std::string> chain;
    for (std::string t = normalize(tag); !t.empty();) {
        chain.push_back(t);
        const auto dash = t.rfind('-');
        if (dash == std::string::npos) break;
        t.resize(dash);
    }
    if (std::ranges::find(chain, std::string("en")) == chain.end()) chain.push_back("en");
    return chain;
}

namespace {

std::string& setting_() { static std::string v; return v; }
std::string& override_env_() { static std::string v; return v; }
std::string& resolved_() { static std::string v; return v; }

}  // namespace

void set_override_env(std::string variable) {
    override_env_() = std::move(variable);
    resolved_().clear();
}

void set_language(std::string lang) {
    // "auto" and "" both mean follow the system, spelled explicitly because
    // `xlings config --lang auto` is how a user goes BACK to following it, and
    // an unrecognised value must not silently pin.
    setting_() = normalize(lang);
    resolved_().clear();
}

const std::string& language() {
    if (!resolved_().empty()) return resolved_();
    // What is asked, most binding first: the override variable (tests, a
    // person's one-off), the setting, the system.
    std::string asked;
    if (!override_env_().empty())
        if (const char* v = std::getenv(override_env_().c_str()); v && *v) asked = normalize(v);
    if (asked.empty()) asked = setting_();
    if (asked.empty()) asked = normalize(platform::get_system_language());
    // The first of its chain some catalogue speaks. English is the base, so
    // it is a complete answer rather than a degraded one.
    for (const auto& t : fallback_chain(asked))
        if (speaks(t)) { resolved_() = t; return resolved_(); }
    resolved_() = "en";
    return resolved_();
}

void forget_resolved_language() { resolved_().clear(); }

}  // namespace xlings::i18n
