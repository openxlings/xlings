// xlings.core.mirror.forms — three URL-rewriting strategies that mirrors
// can declare via `form` in the registry JSON. Each strategy is a pure
// function so it's trivially unit-testable.

module xlings.core.mirror.forms;

import std;
import xlings.core.mirror.types;

namespace xlings::mirror {

namespace {

// "https://github.com/" prefix used by the host-replace strategy.
constexpr std::string_view kGithubHost = "https://github.com/";
constexpr std::string_view kRawHost    = "https://raw.githubusercontent.com/";
constexpr std::string_view kCodeloadHost = "https://codeload.github.com/";

// Replace the leading host portion of `url` (matched by `prefix`, which
// must end with '/') with `replacement` (also ending with '/'). Returns
// nullopt if `url` doesn't start with `prefix`.
std::optional<std::string> replace_host_prefix(std::string_view url,
                                                std::string_view prefix,
                                                std::string_view replacement) {
    if (!url.starts_with(prefix)) return std::nullopt;
    std::string out;
    out.reserve(replacement.size() + url.size() - prefix.size());
    out.append(replacement);
    out.append(url.substr(prefix.size()));
    return out;
}

// jsDelivr's gh/ scheme expects: gh/<owner>/<repo>@<ref>/<path>
// Source URL shape we accept: https://raw.githubusercontent.com/<owner>/<repo>/<ref>/<path>
// Returns nullopt if path doesn't have all 4 segments.
std::optional<std::string> rewrite_jsdelivr_(std::string_view url,
                                              std::string_view mirror_host) {
    if (!url.starts_with(kRawHost)) return std::nullopt;
    auto rest = url.substr(kRawHost.size());

    // Split on '/' for the first three segments (owner, repo, ref).
    std::array<std::string_view, 3> parts{};
    std::size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        auto slash = rest.find('/', start);
        if (slash == std::string_view::npos) return std::nullopt;
        parts[i] = rest.substr(start, slash - start);
        start = slash + 1;
    }
    if (parts[0].empty() || parts[1].empty() || parts[2].empty())
        return std::nullopt;
    auto path = rest.substr(start);
    if (path.empty()) return std::nullopt;

    return std::format("https://{}/gh/{}/{}@{}/{}",
                       mirror_host, parts[0], parts[1], parts[2], path);
}

// Host-replace handles github.com directly and rewrites raw/codeload
// flavors into the equivalent path under the mirror host (kkgithub-style).
std::optional<std::string> rewrite_host_replace_(std::string_view url,
                                                  std::string_view mirror_host,
                                                  ResourceType type) {
    auto base = std::format("https://{}/", mirror_host);

    // github.com → mirror_host (1:1 path mapping)
    if (auto out = replace_host_prefix(url, kGithubHost, base)) return out;

    // raw.githubusercontent.com/x/y/<ref>/path → mirror/x/y/raw/<ref>/path
    if (url.starts_with(kRawHost)) {
        auto rest = url.substr(kRawHost.size());
        // Need at least owner/repo/ref/path
        std::size_t slash1 = rest.find('/');
        if (slash1 == std::string_view::npos) return std::nullopt;
        std::size_t slash2 = rest.find('/', slash1 + 1);
        if (slash2 == std::string_view::npos) return std::nullopt;
        // Insert "raw/" between owner/repo and the rest:
        //   owner/repo/<ref>/path → owner/repo/raw/<ref>/path
        return std::format("https://{}/{}/raw/{}",
                           mirror_host,
                           rest.substr(0, slash2),  // owner/repo
                           rest.substr(slash2 + 1));  // ref/path
    }

    // codeload.github.com/x/y/<type>/<ref> → mirror/x/y/<type>/<ref>
    // (Most host-replace mirrors don't proxy codeload; mark as unsupported
    // by returning nullopt so the registry filter drops it instead of
    // returning a bad URL.)
    if (url.starts_with(kCodeloadHost) && type == ResourceType::Archive) {
        return std::nullopt;
    }

    return std::nullopt;
}

// Prefix is the simplest: prepend the mirror as a proxy.
std::optional<std::string> rewrite_prefix_(std::string_view url,
                                            std::string_view mirror_host) {
    // Only proxy URLs we actually want mirrored. Refuse to wrap
    // non-GitHub-family URLs — caller should not have asked.
    if (!url.starts_with(kGithubHost) &&
        !url.starts_with(kRawHost) &&
        !url.starts_with(kCodeloadHost)) {
        return std::nullopt;
    }
    return std::format("https://{}/{}", mirror_host, url);
}

}

std::optional<std::string> rewrite(std::string_view url,
                                          const Mirror& mirror,
                                          ResourceType type) {
    switch (mirror.form) {
        case Form::Prefix:      return rewrite_prefix_(url, mirror.host);
        case Form::HostReplace: return rewrite_host_replace_(url, mirror.host, type);
        case Form::JsDelivr:    return rewrite_jsdelivr_(url, mirror.host);
    }
    return std::nullopt;
}

}
