module xlings.testkit.index_fixture;
import std;
import xlings.testkit.fixture;
import xlings.libs.json;
import xlings.libs.sha256;

namespace xlings::testkit::index_fixture {
namespace {
namespace fs = std::filesystem;
void write(const fs::path& file, std::string_view data) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) throw std::runtime_error("cannot write HTTP fixture " + file.string());
}
// A ustar archive of regular files: {name, mode, contents}.
struct Entry { std::string_view name; std::string_view mode; std::string_view contents; };
std::string tar(std::initializer_list<Entry> entries) {
    std::string out;
    for (const auto& e : entries) {
        std::string header(512, '\0');
        header.replace(0, e.name.size(), e.name);
        header.replace(100, 7, e.mode);
        header.replace(108, 7, "0000000");
        header.replace(116, 7, "0000000");
        header.replace(124, 11, std::format("{:011o}", e.contents.size()));
        header.replace(136, 11, "00000000000");
        header.replace(148, 8, "        ");
        header[156] = '0';
        header.replace(257, 5, "ustar");
        header.replace(263, 2, "00");
        unsigned sum = 0;
        for (unsigned char byte : header) sum += byte;
        header.replace(148, 6, std::format("{:06o}", sum));
        header[154] = '\0';
        header[155] = ' ';
        std::string body(e.contents);
        body.resize((body.size() + 511) / 512 * 512, '\0');
        out += header + body;
    }
    out.resize(out.size() + 1024, '\0');
    return out;
}
std::string payload() { return tar({{"fixture.txt", "0000644", "xlings HTTP fixture\n"}}); }
// A program a SubOS can put on PATH and a root can put in /usr/bin.
std::string tool_payload() {
    // Two top-level entries, so no extractor reads the archive as one
    // wrapping directory to strip.
    return tar({{"bin/fixture-tool", "0000755", "#!/bin/sh\necho fixture-tool\n"},
                {"README", "0000644", "fixture-tool\n"}});
}
struct Store {
    fs::path root;
    std::optional<fixture::Server> server;
    Store() {
        for (int attempt = 0; attempt != 64; ++attempt) {
            const auto candidate = fs::temp_directory_path() / std::format("xtest-http-{}-{}",
                std::chrono::steady_clock::now().time_since_epoch().count(), attempt);
            std::error_code ec;
            if (fs::create_directory(candidate, ec)) { root = candidate; break; }
            if (ec && ec != std::errc::file_exists) throw std::runtime_error(ec.message());
        }
        if (root.empty()) throw std::runtime_error("cannot reserve HTTP fixture directory");
        try {
            const auto data = payload();
            write(root / "fixture-data-1.0.0.tar", data);
            write(root / "index/xim-indexrepos.lua", "xim_indexrepos = {}\n");
            fs::create_directories(root / "index/pkgs/f");
            auto listening = fixture::Server::start(root);
            if (!listening) throw std::runtime_error(listening.error());
            server.emplace(std::move(*listening));
            const auto version = std::format("{{url='{}',sha256='{}'}}",
                server->url() + "/fixture-data-1.0.0.tar", sha256::hex(data));
            std::string recipe = "package = {spec='1',name='fixture-data',type='package',"
                "archs={'x86_64','aarch64'},xpm={";
            for (const auto* platform : {"linux", "macosx", "windows"})
                recipe += std::string(platform) + "={['1.0.0']=" + version + "},";
            recipe += "}}\n";
            write(root / "index/pkgs/f/fixture-data.lua", recipe);
            const auto tool = tool_payload();
            write(root / "fixture-tool-1.0.0.tar", tool);
            const auto toolVersion = std::format("{{url='{}',sha256='{}'}}",
                server->url() + "/fixture-tool-1.0.0.tar", sha256::hex(tool));
            std::string toolRecipe = "package = {spec='1',name='fixture-tool',type='package',"
                "archs={'x86_64','aarch64'},xpm={";
            for (const auto* platform : {"linux", "macosx", "windows"})
                toolRecipe += std::string(platform) + "={['1.0.0']=" + toolVersion + "},";
            toolRecipe += "}}\n"
                "import('xim.libxpkg.pkginfo')\nimport('xim.libxpkg.xvm')\n"
                "function config()\n"
                "  xvm.add('fixture-tool', { bindir = path.join(pkginfo.install_dir(), 'bin') })\n"
                "  return true\nend\n";
            write(root / "index/pkgs/f/fixture-tool.lua", toolRecipe);
        } catch (...) {
            server.reset();
            std::error_code ec;
            fs::remove_all(root, ec);
            throw;
        }
    }
    ~Store() {
        server.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};
Store& store() { static Store value; return value; }
}
std::string config(std::string_view mirror) {
    const auto& fixture = store();
    const auto index = (fixture.root / "index").generic_string();
    return nlohmann::json{
        {"mirror", mirror}, {"xim", {{"index-repo", index}}},
        {"index_repos", nlohmann::json::array({{{"name", "xim"}, {"url", index}, {"source", "git"}}})},
        {"XLINGS_RES", {{"GLOBAL", {fixture.server->url()}}, {"CN", {fixture.server->url()}}}}
    }.dump(2);
}
std::string url() { return store().server->url(); }
}
