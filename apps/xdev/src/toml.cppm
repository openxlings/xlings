export module xlings.xdev.toml;

import std;

// The TOML this repository's test manifests use, and no more: `[table]`
// headers, `key = "string"`, `key = true|false|<integer>`, and arrays of
// strings that may span lines. Comments start with `#` outside a string.
//
// A full TOML library would be a dependency for three files that a person
// edits by hand; an unsupported construct is reported with its line number
// rather than silently read as something else.
export namespace xlings::xdev::toml {

struct Value {
    std::variant<std::string, bool, long long, std::vector<std::string>> v;

    const std::string* str() const { return std::get_if<std::string>(&v); }
    const std::vector<std::string>* list() const { return std::get_if<std::vector<std::string>>(&v); }
    const bool* boolean() const { return std::get_if<bool>(&v); }
};

using Table = std::map<std::string, Value, std::less<>>;

struct Document {
    std::vector<std::string> order;              // table names, in file order
    std::map<std::string, Table, std::less<>> tables;
};

std::expected<Document, std::string> parse(std::string_view text);
std::expected<Document, std::string> parse_file(const std::filesystem::path& path);

}  // namespace xlings::xdev::toml

namespace xlings::xdev::toml {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Strip a trailing comment that is not inside a string.
std::string_view strip_comment(std::string_view s) {
    bool in_str = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && in_str) { ++i; continue; }
        if (s[i] == '"') in_str = !in_str;
        else if (s[i] == '#' && !in_str) return s.substr(0, i);
    }
    return s;
}

std::expected<std::string, std::string> read_string(std::string_view& s) {
    if (s.empty() || s.front() != '"') return std::unexpected("expected a string");
    std::string out;
    std::size_t i = 1;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') break;
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            switch (n) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            default: out.push_back('\\'); out.push_back(n);
            }
            continue;
        }
        out.push_back(c);
    }
    if (i >= s.size()) return std::unexpected("unterminated string");
    s.remove_prefix(i + 1);
    return out;
}

}  // namespace

std::expected<Document, std::string> parse(std::string_view text) {
    Document doc;
    std::string current;
    doc.order.push_back("");
    doc.tables[""] = {};

    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start <= text.size();) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }

    for (std::size_t ln = 0; ln < lines.size(); ++ln) {
        auto where = [&](std::string_view what) {
            return std::format("line {}: {}", ln + 1, what);
        };
        auto line = trim(strip_comment(lines[ln]));
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']' || line.starts_with("[[")) return std::unexpected(where("unsupported table header"));
            current = std::string(trim(line.substr(1, line.size() - 2)));
            if (current.size() >= 2 && current.front() == '"' && current.back() == '"')
                current = current.substr(1, current.size() - 2);
            if (!doc.tables.contains(current)) doc.order.push_back(current);
            doc.tables[current];
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string_view::npos) return std::unexpected(where("expected key = value"));
        auto key = std::string(trim(line.substr(0, eq)));
        auto rest = trim(line.substr(eq + 1));
        if (key.size() >= 2 && key.front() == '"' && key.back() == '"') key = key.substr(1, key.size() - 2);

        Value value;
        if (!rest.empty() && rest.front() == '"') {
            auto s = read_string(rest);
            if (!s) return std::unexpected(where(s.error()));
            value.v = std::move(*s);
        } else if (rest == "true" || rest == "false") {
            value.v = (rest == "true");
        } else if (!rest.empty() && rest.front() == '[') {
            // Join continuation lines until the closing bracket.
            std::string joined(rest);
            while (joined.find(']') == std::string::npos || [&] {
                       // a ']' inside a string does not close the array
                       bool in_str = false;
                       for (std::size_t i = 0; i < joined.size(); ++i) {
                           if (joined[i] == '\\' && in_str) { ++i; continue; }
                           if (joined[i] == '"') in_str = !in_str;
                           else if (joined[i] == ']' && !in_str) return false;
                       }
                       return true;
                   }()) {
                if (++ln >= lines.size()) return std::unexpected(where("unterminated array"));
                joined += ' ';
                joined += trim(strip_comment(lines[ln]));
            }
            std::string_view a(joined);
            a.remove_prefix(1);
            std::vector<std::string> items;
            while (true) {
                a = trim(a);
                if (a.empty()) return std::unexpected(where("unterminated array"));
                if (a.front() == ']') break;
                auto s = read_string(a);
                if (!s) return std::unexpected(where("arrays hold strings only"));
                items.push_back(std::move(*s));
                a = trim(a);
                if (!a.empty() && a.front() == ',') a.remove_prefix(1);
            }
            value.v = std::move(items);
        } else {
            long long n = 0;
            auto [p, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), n);
            if (ec != std::errc{} || p != rest.data() + rest.size())
                return std::unexpected(where("unsupported value: " + std::string(rest)));
            value.v = n;
        }
        doc.tables[current][key] = std::move(value);
    }
    return doc;
}

std::expected<Document, std::string> parse_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::unexpected("cannot read " + path.string());
    std::string text{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    auto doc = parse(text);
    if (!doc) return std::unexpected(path.string() + ": " + doc.error());
    return doc;
}

}  // namespace xlings::xdev::toml
