// Archive sources by content, bundles, and what no source has
// (design mcpp 2026-11-21 offline host inputs §3.1, §6 X1 X2).
//
// The fixture index holds three packages whose payload is a plain file the
// install hook copies into place: alpha (depends on beta), beta, and gamma,
// whose recipe states no sha256. Their URLs name a loopback server: the one
// test that downloads starts it, every other test runs offline
// (XLINGS_NET_UNREACHABLE) against a port nothing listens on.
#include <gtest/gtest.h>
import xlings.testkit;
import xlings.testkit.fixture;
#include "xlings/xtest.hpp"

import std;
import xlings.libs.json;
import xlings.libs.sha256;
import xlings.core.config;
import xlings.core.xim.archives;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;
namespace ar = xlings::xim::archives;

namespace {

constexpr std::string_view kOffline = "offline (test)";

std::string sha_(std::string_view bytes) { return xlings::sha256::hex(bytes); }

// Bytes of a payload: distinct per name, and past the size an error stub has.
std::string bytes_(std::string_view name, std::string_view salt) {
    std::string out;
    while (out.size() < 4096) out += std::format("{}{} payload {};", salt, name, out.size());
    return out;
}

struct Fixture {
    fs::path primary;   // the default index `xim`: the relocation tool only
    fs::path index;     // `fixture`
    fs::path served;    // what the loopback server serves
    std::map<std::string, std::string> files;   // served name -> bytes
};

// `name`'s recipe. Linux and macOS take `<name>-1.0.0.bin`, Windows
// `<name>-1.0.0-windows.bin`; `stated` says whether the recipe states their
// sha256.
void write_recipe_(Fixture& f, std::string_view base, std::string_view name, std::string_view deps, bool stated,
                   std::string_view salt) {
    const auto host = std::format("{}-1.0.0.bin", name);
    const auto windows = std::format("{}-1.0.0-windows.bin", name);
    f.files[host] = bytes_(host, salt);
    f.files[windows] = bytes_(windows, salt);
    const auto entry = [&](const std::string& file) {
        return stated ? std::format("{{ url = '{}/{}', sha256 = '{}' }}", base, file, sha_(f.files[file]))
                      : std::format("{{ url = '{}/{}' }}", base, file);
    };
    tk::write_file(f.index / "pkgs" / name.substr(0, 1) / std::format("{}.lua", name), std::format(R"LUA(
package = {{
    spec = '1', name = '{0}', type = 'package', archs = {{'x86_64', 'aarch64'}},
    xpm = {{
        linux   = {{ deps = {{ {1} }}, ['1.0.0'] = {2} }},
        macosx  = {{ deps = {{ {1} }}, ['1.0.0'] = {2} }},
        windows = {{ deps = {{ {1} }}, ['1.0.0'] = {3} }},
    }},
}}
import('xim.libxpkg.pkginfo')
function install()
    os.mkdir(pkginfo.install_dir())
    os.cp(pkginfo.install_file(), path.join(pkginfo.install_dir(), 'payload.bin'))
    return true
end
function config() return true end
)LUA", name, deps, entry(host), entry(windows)));
}

// `salt` makes another index of the same packages, versions and URLs whose
// bytes (and so sha256) differ.
Fixture make_fixture_(const fs::path& root, std::string_view base, std::string_view salt = "") {
    Fixture f{root / "primary", root / "index", root / "served", {}};
    tk::write_file(f.primary / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    tk::write_file(f.index / "xim-indexrepos.lua", "xim_indexrepos = {}\n");
    // What an install on a Linux host without patchelf takes first, payload
    // free here: the closure is then the same whatever the host has.
    tk::write_file(f.primary / "pkgs" / "p" / "patchelf.lua", R"LUA(
package = {
    spec = '1', name = 'patchelf', type = 'package', archs = {'x86_64', 'aarch64'},
    xpm = { linux = { ['0.18.0'] = {} }, macosx = { ['0.18.0'] = {} }, windows = { ['0.18.0'] = {} } },
}
import('xim.libxpkg.pkginfo')
function install()
    os.mkdir(pkginfo.install_dir())
    io.writefile(path.join(pkginfo.install_dir(), 'marker'), 'x')
    return true
end
function config() return true end
)LUA");
    write_recipe_(f, base, "alpha", "'fixture:beta@1.0.0'", true, salt);
    write_recipe_(f, base, "beta", "", true, salt);
    write_recipe_(f, base, "gamma", "", false, salt);
    for (const auto& [name, content] : f.files) tk::write_file(f.served / name, content);
    return f;
}

nlohmann::json config_(const Fixture& f) {
    return {{"mirror", "GLOBAL"},
            {"xim", {{"index-repo", f.primary.generic_string()}}},
            {"index_repos", nlohmann::json::array({
                {{"name", std::string(xlings::Config::DEFAULT_INDEX_REPO_NAME)},
                 {"url", f.primary.generic_string()}, {"source", "git"}},
                {{"name", "fixture"}, {"url", f.index.generic_string()}, {"source", "git"}}})}};
}

// `config` replaces the isolated home's own (its loopback index) when given.
void init_home_(const tk::Home& home, const std::optional<nlohmann::json>& config) {
    if (config) tk::write_file(home.dir() / ".xlings.json", config->dump());
    fs::create_directories(home.dir() / "bin");
    fs::copy_file(tk::xlings_binary(), home.dir() / "bin" / (tk::is_windows ? "xlings.exe" : "xlings"),
                  fs::copy_options::overwrite_existing);
    const auto init = home.xlings({"self", "init"});
    ASSERT_EQ(init.exit_code, 0) << init.transcript();
}

std::string payload_(const tk::Home& home, std::string_view name) {
    const auto file = home.dir() / "data" / "xpkgs" / std::format("fixture-x-{}", name) / "1.0.0" / "payload.bin";
    std::error_code ec;
    return fs::is_regular_file(file, ec) ? tk::read_file(file) : std::string("<absent>");
}

std::string host_file_(std::string_view name) {
    return std::format(tk::is_windows ? "{}-1.0.0-windows.bin" : "{}-1.0.0.bin", name);
}

std::size_t count_(std::string_view text, std::string_view what) {
    std::size_t n = 0;
    for (auto at = text.find(what); at != std::string_view::npos; at = text.find(what, at + what.size())) ++n;
    return n;
}

// A network namespace this user may make: the strongest "no network" a test
// can give a process. Not every host lets an unprivileged user make one.
bool netns_available_() {
    if constexpr (!tk::is_linux) return false;
    tk::RunOptions probe;
    probe.argv = {"unshare", "--map-current-user", "-n", "true"};
    probe.env = {{"PATH", "/usr/local/bin:/usr/bin:/bin"}};
    probe.timeout = std::chrono::seconds(20);
    return tk::run(probe).exit_code == 0;
}

}  // namespace

// X1: by content, under any name, at any depth.
XTEST(ArchiveSources, ADirectoryAnswersByContentUnderAnyName, .area = "xim",
      .covers = {"ARCHIVE-DIR-BY-CONTENT", "ARCHIVE-DIR-VERIFIED"}) {
    auto home = tk::Home::isolated("archives-by-content");
    const auto dir = home.root() / "archives";
    tk::write_file(dir / "a" / "b" / "c" / "whatever name.dat", "the bytes");
    tk::write_file(dir / "other.bin", "other bytes");
    const auto memo = home.root() / "memo.json";

    const auto hit = ar::find_in(dir, sha_("the bytes"), memo);
    ASSERT_TRUE(hit);
    EXPECT_TRUE(fs::equivalent(*hit, dir / "a" / "b" / "c" / "whatever name.dat"));
    // A digest nothing has is not answered by a file that merely exists.
    EXPECT_FALSE(ar::find_in(dir, sha_("bytes nobody wrote"), memo));
    // Upper-case hex (as a recipe may write it) is the same identity.
    std::string upper = sha_("other bytes");
    for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    EXPECT_TRUE(ar::find_in(dir, upper, memo));
}

// X1: the memo follows a file whose size or mtime changed, and forgets one
// that is gone.
XTEST(ArchiveSources, TheDirectoryIndexFollowsAChangedFile, .area = "xim",
      .covers = {"ARCHIVE-DIR-INDEX"}) {
    auto home = tk::Home::isolated("archives-index");
    const auto dir = home.root() / "archives";
    const auto file = dir / "payload";
    const auto memo = home.root() / "memo.json";
    tk::write_file(file, "first content");
    ASSERT_TRUE(ar::find_in(dir, sha_("first content"), memo));
    ASSERT_TRUE(fs::is_regular_file(memo));
    EXPECT_NE(tk::read_file(memo).find(sha_("first content")), std::string::npos);

    tk::write_file(file, "second, longer content");
    fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(5));
    EXPECT_FALSE(ar::find_in(dir, sha_("first content"), memo));
    EXPECT_TRUE(ar::find_in(dir, sha_("second, longer content"), memo));
    const auto recorded = tk::read_file(memo);
    EXPECT_EQ(recorded.find(sha_("first content")), std::string::npos) << recorded;
    EXPECT_NE(recorded.find(sha_("second, longer content")), std::string::npos) << recorded;

    fs::remove(file);
    EXPECT_FALSE(ar::find_in(dir, sha_("second, longer content"), memo));
    EXPECT_EQ(tk::read_file(memo).find(sha_("second, longer content")), std::string::npos);
}

// The manifest: a reader ignores what it does not know, and a writer keeps
// it -- another program's tables (mcpp's [[git]]) and the keys a later format
// 1 adds to the tables it knows.
XTEST(ArchiveSources, ABundleManifestKeepsWhatItDoesNotKnow, .area = "xim",
      .covers = {"ARCHIVE-BUNDLE-MANIFEST"}) {
    const std::string text = R"(# written by hand
[bundle]
format = 1
created_by = "xlings 1"
host = 'linux-x86_64'
future_key = [
  "[[not a header]]",
]

[[archive]]
package = "fixture:alpha"
version = "1.0.0"
host = "linux-x86_64"
sha256 = "ab"
file = "archives/ab-alpha \"quoted\".bin"
size = 3

[[git]]
url = "https://example.invalid/dep.git"
note = """
[[index]]
name = "not an index"
"""
paths = [
  "[[index]]",
]

[[index]]   # a header with a comment
name = "fixture"
revision = "artifact:1"
path = "index/fixture"
)";
    auto parsed = ar::parse_bundle(text);
    ASSERT_TRUE(parsed) << parsed.error();
    EXPECT_EQ(parsed->host, "linux-x86_64");   // a literal string
    ASSERT_EQ(parsed->archives.size(), 1u);
    EXPECT_EQ(parsed->archives[0].file, "archives/ab-alpha \"quoted\".bin");
    ASSERT_EQ(parsed->indexes.size(), 1u) << "a multi-line string or a commented header misread";
    EXPECT_EQ(parsed->indexes[0].name, "fixture");
    EXPECT_EQ(parsed->indexes[0].revision, "artifact:1");
    EXPECT_NE(parsed->otherTables.find("[[git]]"), std::string::npos);
    EXPECT_NE(parsed->otherTables.find("name = \"not an index\""), std::string::npos);

    const auto rendered = ar::render_bundle(*parsed);
    EXPECT_NE(rendered.find("future_key = [\n  \"[[not a header]]\",\n]"), std::string::npos) << rendered;
    EXPECT_NE(rendered.find("size = 3"), std::string::npos) << rendered;
    auto again = ar::parse_bundle(rendered);
    ASSERT_TRUE(again) << again.error();
    EXPECT_EQ(again->archives[0].file, parsed->archives[0].file);
    EXPECT_EQ(again->archives[0].extra, parsed->archives[0].extra);
    EXPECT_EQ(again->extra, parsed->extra);
    EXPECT_EQ(again->indexes.size(), 1u);
    EXPECT_EQ(again->otherTables, parsed->otherTables);

    EXPECT_FALSE(ar::parse_bundle("[[archive]]\npackage = \"x\"\n"));   // no [bundle] format

    // A format this client does not know is refused, with the way forward.
    auto home = tk::Home::isolated("archives-format");
    tk::write_file(home.root() / "newer" / "bundle.toml", "[bundle]\nformat = 2\n");
    const auto newer = ar::read_bundle(home.root() / "newer");
    ASSERT_FALSE(newer);
    EXPECT_NE(newer.error().find("newer than this xlings reads"), std::string::npos) << newer.error();
    EXPECT_NE(newer.error().find("xlings self update"), std::string::npos) << newer.error();
}

// X1: XLINGS_ARCHIVES (several entries) before `archives` (one relative to
// the home), by content; a file under the URL's own name whose bytes differ
// is not taken.
XTEST(ArchiveSources, DirectoriesAnswerInOrderAndOnlyByContent, .area = "xim",
      .covers = {"ARCHIVE-DIR-BY-CONTENT", "ARCHIVE-DIR-VERIFIED", "ARCHIVE-DIR-ORDER", "ARCHIVE-CONFIG"},
      .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("archives-order");
    auto f = make_fixture_(home.root() / "fixture", "http://127.0.0.1:9");
    auto config = config_(f);
    config["archives"] = nlohmann::json::array({"second"});   // relative to the home
    init_home_(home, config);
    const auto absent = home.root() / "absent";
    const auto first = home.root() / "first";       // XLINGS_ARCHIVES, after an absent entry
    const auto second = home.dir() / "second";      // .xlings.json archives
    tk::write_file(first / "x" / "copy one", f.files[host_file_("alpha")]);
    tk::write_file(first / host_file_("beta"), "not beta's bytes, under beta's own file name");
    tk::write_file(second / "y" / "copy two", f.files[host_file_("alpha")]);
    tk::write_file(second / "deep" / "er" / "unrelated.name", f.files[host_file_("beta")]);

    const char sep = tk::is_windows ? ';' : ':';
    const auto install = home.xlings({"install", "fixture:alpha@1.0.0", "-y", "-v"},
                                     {{"XLINGS_ARCHIVES", absent.string() + sep + first.string()},
                                      {"XLINGS_NET_UNREACHABLE", std::string(kOffline)}});
    ASSERT_EQ(install.exit_code, 0) << install.transcript();
    EXPECT_EQ(payload_(home, "alpha"), f.files[host_file_("alpha")]);
    EXPECT_EQ(payload_(home, "beta"), f.files[host_file_("beta")]) << install.transcript();
    // XLINGS_ARCHIVES before `archives`: alpha came from the first directory,
    // beta from the relative `archives` entry.
    EXPECT_NE(install.transcript().find((first / "x" / "copy one").string()), std::string::npos)
        << install.transcript();
    EXPECT_EQ(install.transcript().find((second / "y" / "copy two").string()), std::string::npos)
        << install.transcript();
    EXPECT_NE(install.transcript().find((second / "deep" / "er" / "unrelated.name").string()), std::string::npos)
        << install.transcript();
    EXPECT_EQ(count_(install.transcript(), absent.string() + " is not a directory here"), 1u) << install.transcript();
}

// The configuration of `archives`.
XTEST(ArchiveSources, ConfigAddsAndRemovesArchiveDirectories, .area = "xim",
      .covers = {"ARCHIVE-CONFIG"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("archives-config");
    const auto dir = home.root() / "archives";
    fs::create_directories(dir);
    auto added = home.xlings({"config", "--add-archives", dir.string()});
    ASSERT_EQ(added.exit_code, 0) << added.transcript();
    const auto config = nlohmann::json::parse(tk::read_file(home.dir() / ".xlings.json"));
    ASSERT_TRUE(config.contains("archives")) << config.dump();
    ASSERT_EQ(config["archives"].size(), 1u);
    EXPECT_TRUE(fs::equivalent(fs::path(config["archives"][0].get<std::string>()), dir));
    const auto shown = home.xlings({"config"});
    EXPECT_NE(shown.transcript().find(dir.string()), std::string::npos) << shown.transcript();
    EXPECT_NE(home.xlings({"config", "--add-archives", (home.root() / "absent").string()}).exit_code, 0);

    auto removed = home.xlings({"config", "--rm-archives", dir.string()});
    ASSERT_EQ(removed.exit_code, 0) << removed.transcript();
    EXPECT_FALSE(nlohmann::json::parse(tk::read_file(home.dir() / ".xlings.json")).contains("archives"));
    EXPECT_NE(home.xlings({"config", "--rm-archives", dir.string()}).exit_code, 0);
}

// The download cache before `install --from` before XLINGS_ARCHIVES.
XTEST(ArchiveSources, TheCacheThenFromThenTheEnvironment, .area = "xim",
      .covers = {"ARCHIVE-DIR-ORDER"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("archives-cache-first");
    auto f = make_fixture_(home.root() / "fixture", "http://127.0.0.1:9");
    init_home_(home, config_(f));
    const auto from = home.root() / "from";
    const auto env = home.root() / "env";
    tk::write_file(home.dir() / "data" / "runtimedir" / host_file_("beta"), f.files[host_file_("beta")]);
    tk::write_file(from / "beta copy", f.files[host_file_("beta")]);
    tk::write_file(from / "alpha copy", f.files[host_file_("alpha")]);
    tk::write_file(env / "alpha copy", f.files[host_file_("alpha")]);
    const auto install = home.xlings({"install", "--from", from.string(), "fixture:alpha@1.0.0", "-y", "-v"},
                                     {{"XLINGS_ARCHIVES", env.string()},
                                      {"XLINGS_NET_UNREACHABLE", std::string(kOffline)}});
    const auto out = install.transcript();
    ASSERT_EQ(install.exit_code, 0) << out;
    EXPECT_EQ(payload_(home, "beta"), f.files[host_file_("beta")]);
    EXPECT_EQ(out.find("[archives] fixture:beta@1.0.0 from"), std::string::npos) << "the cache answers first\n" << out;
    EXPECT_NE(out.find((from / "alpha copy").string()), std::string::npos) << out;
    EXPECT_EQ(out.find((env / "alpha copy").string()), std::string::npos) << out;
}

// A memo that claims a digest its file no longer has (same size and mtime,
// other bytes) is caught when the file is taken, and the file is not used.
XTEST(ArchiveSources, AStaleMemoEntryIsNotTrusted, .area = "xim",
      .covers = {"ARCHIVE-DIR-VERIFIED", "ARCHIVE-DIR-INDEX"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("archives-stale-memo");
    auto f = make_fixture_(home.root() / "fixture", "http://127.0.0.1:9");
    init_home_(home, config_(f));
    const auto dir = home.root() / "archives";
    const auto file = dir / "beta";
    const auto& good = f.files[host_file_("beta")];
    tk::write_file(file, good);
    const auto memo = ar::memo_file_for(dir, home.dir() / "data");
    ASSERT_TRUE(ar::find_in(dir, sha_(good), memo));   // the memo now names the good bytes
    const auto mtime = fs::last_write_time(file);
    std::string forged = good;
    forged[0] = static_cast<char>(forged[0] ^ 1);
    tk::write_file(file, forged);
    fs::last_write_time(file, mtime);

    const auto install = home.xlings({"install", "fixture:beta@1.0.0", "-y"},
                                     {{"XLINGS_ARCHIVES", dir.string()},
                                      {"XLINGS_NET_UNREACHABLE", std::string(kOffline)}});
    EXPECT_NE(install.exit_code, 0) << install.transcript();
    EXPECT_EQ(payload_(home, "beta"), "<absent>");
    EXPECT_NE(install.transcript().find("1 archive is in no source"), std::string::npos) << install.transcript();
    EXPECT_EQ(tk::read_file(memo).find(sha_(good)), std::string::npos) << "the stale entry was kept";
}

// A symbolic link in an archive directory is taken as the file it names: the
// cache holds the bytes, not a link that dangles once it is beside them.
XTEST(ArchiveSources, ALinkIsTakenAsTheFileItNames, .area = "xim",
      .covers = {"ARCHIVE-DIR-BY-CONTENT"}, .requires_ = {"xlings-bin", "posix"}) {
    auto home = tk::Home::isolated("archives-link");
    auto f = make_fixture_(home.root() / "fixture", "http://127.0.0.1:9");
    init_home_(home, config_(f));
    const auto dir = home.root() / "archives";
    tk::write_file(dir / "real" / "beta.bytes", f.files[host_file_("beta")]);
    fs::create_symlink(fs::path("real") / "beta.bytes", dir / "beta-link");
    const auto install = home.xlings({"install", "fixture:beta@1.0.0", "-y"},
                                     {{"XLINGS_ARCHIVES", dir.string()},
                                      {"XLINGS_NET_UNREACHABLE", std::string(kOffline)}});
    ASSERT_EQ(install.exit_code, 0) << install.transcript();
    const auto cached = home.dir() / "data" / "runtimedir" / host_file_("beta");
    EXPECT_FALSE(fs::is_symlink(fs::symlink_status(cached))) << cached.string();
    EXPECT_EQ(tk::read_file(cached), f.files[host_file_("beta")]);
}

// X2: `download --to --with-index` on a machine with a network, then
// `install --from` where there is none -- in a network namespace where this
// host lets a user make one, else with XLINGS_NET_UNREACHABLE (either way the
// server is gone by then). gamma states no sha256: the bundle's manifest
// names its bytes.
XTEST(ArchiveSources, ABundleInstallsWhereThereIsNoNetwork, .area = "xim", .cost = tk::Cost::Medium,
      .covers = {"ARCHIVE-BUNDLE-ROUNDTRIP", "ARCHIVE-BUNDLE-MANIFEST", "ARCHIVE-BUNDLE-NO-SHA", "ARCHIVE-BUNDLE-SNAPSHOTS"},
      .requires_ = {"xlings-bin"}) {
    auto online = tk::Home::isolated("archives-bundle-online");
    const auto root = online.root() / "fixture";
    fs::create_directories(root / "served");
    auto server = tk::fixture::Server::start(root / "served");
    ASSERT_TRUE(server) << server.error();
    auto f = make_fixture_(root, server->url());
    init_home_(online, config_(f));

    const auto bundle = online.root() / "bundle";
    tk::write_file(bundle / "bundle.toml",
                   "[bundle]\nformat = 1\ncreated_by = \"mcpp\"\nhost = \"\"\n\n[[git]]\nurl = \"https://example.invalid/d.git\"\n");
    const auto download = online.xlings({"download", "fixture:alpha@1.0.0", "fixture:gamma@1.0.0",
                                         "--to", bundle.string(), "--with-index"});
    ASSERT_EQ(download.exit_code, 0) << download.transcript();
    server->stop();

    auto manifest = ar::read_bundle(bundle);
    ASSERT_TRUE(manifest && *manifest) << (manifest ? std::string("no manifest") : manifest.error());
    const auto& b = **manifest;
    EXPECT_EQ(b.format, 1);
    EXPECT_TRUE(b.createdBy.starts_with("xlings ")) << b.createdBy;
    EXPECT_NE(b.otherTables.find("[[git]]"), std::string::npos) << "another program's table was dropped";
    std::set<std::string> packages;
    for (const auto& a : b.archives) {
        packages.insert(a.package);
        EXPECT_EQ(a.host, b.host);
        EXPECT_TRUE(a.file.starts_with("archives/" + a.sha256 + "-")) << a.file;
        ASSERT_TRUE(fs::is_regular_file(bundle / a.file)) << a.file;
        EXPECT_EQ(sha_(tk::read_file(bundle / a.file)), a.sha256);
    }
    EXPECT_EQ(packages, (std::set<std::string>{"fixture:alpha", "fixture:beta", "fixture:gamma"}));
    std::set<std::string> indexes;
    for (const auto& i : b.indexes) {
        indexes.insert(i.name);
        EXPECT_TRUE(fs::is_directory(bundle / i.path / "pkgs")) << i.path;
        EXPECT_FALSE(fs::exists(bundle / i.path / ".git")) << i.path;
    }
    EXPECT_TRUE(indexes.contains("fixture")) << download.transcript();

    // Another machine, whose own index has the same packages at the same
    // versions with other bytes: the bundle's snapshots resolve, not it.
    auto offline = tk::Home::isolated("archives-bundle-offline");
    const auto conflicting = make_fixture_(offline.root() / "fixture", "http://127.0.0.1:9", "other ");
    init_home_(offline, config_(conflicting));
    const std::vector<std::string> args{"install", "--from", bundle.string(),
                                        "fixture:alpha@1.0.0", "fixture:gamma@1.0.0", "-y"};
    tk::RunResult install;
    if (netns_available_()) {
        tk::RunOptions run;
        run.argv = {"unshare", "--map-current-user", "-n", tk::xlings_binary().string()};
        run.argv.insert(run.argv.end(), args.begin(), args.end());
        run.env = offline.env();
        run.timeout = std::chrono::seconds(180);
        install = tk::run(run);
        std::println("install --from ran in a network namespace (unshare -n)");
    } else {
        install = offline.xlings(args, {{"XLINGS_NET_UNREACHABLE", std::string(kOffline)}});
        std::println("install --from ran with XLINGS_NET_UNREACHABLE (no network namespace here)");
    }
    ASSERT_EQ(install.exit_code, 0) << install.transcript();
    EXPECT_EQ(payload_(offline, "alpha"), f.files[host_file_("alpha")]);
    EXPECT_EQ(payload_(offline, "beta"), f.files[host_file_("beta")]);
    EXPECT_EQ(payload_(offline, "gamma"), f.files[host_file_("gamma")]);
}

// The archives of another host's closure.
XTEST(ArchiveSources, ADownloadForAnotherHostTakesThatHostsArchives, .area = "xim",
      .covers = {"ARCHIVE-BUNDLE-HOST"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("archives-host");
    const auto root = home.root() / "fixture";
    fs::create_directories(root / "served");
    auto server = tk::fixture::Server::start(root / "served");
    ASSERT_TRUE(server) << server.error();
    auto f = make_fixture_(root, server->url());
    init_home_(home, config_(f));
    const auto bundle = home.root() / "bundle";
    const auto download = home.xlings({"download", "fixture:alpha@1.0.0", "--to", bundle.string(),
                                       "--host", "windows-x86_64"});
    ASSERT_EQ(download.exit_code, 0) << download.transcript();
    auto manifest = ar::read_bundle(bundle);
    ASSERT_TRUE(manifest && *manifest);
    EXPECT_EQ((*manifest)->host, "windows-x86_64");
    std::map<std::string, std::string> sha;
    for (const auto& a : (*manifest)->archives) {
        EXPECT_EQ(a.host, "windows-x86_64");
        sha[a.package] = a.sha256;
    }
    EXPECT_EQ(sha, (std::map<std::string, std::string>{
                       {"fixture:alpha", sha_(f.files["alpha-1.0.0-windows.bin"])},
                       {"fixture:beta", sha_(f.files["beta-1.0.0-windows.bin"])}}));
    const auto plan9 = home.xlings({"download", "fixture:alpha@1.0.0", "--to", bundle.string(), "--host", "plan9"});
    EXPECT_NE(plan9.exit_code, 0);
    EXPECT_NE(plan9.transcript().find("'plan9' is not a host: <os>-<arch>"), std::string::npos) << plan9.transcript();
}

// A manifest that cannot be read stops `download --to` before anything is
// written into its directory.
XTEST(ArchiveSources, AnUnreadableManifestStopsADownload, .area = "xim",
      .covers = {"ARCHIVE-BUNDLE-MANIFEST"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("archives-unreadable");
    auto f = make_fixture_(home.root() / "fixture", "http://127.0.0.1:9");
    init_home_(home, config_(f));
    const auto bundle = home.root() / "bundle";
    tk::write_file(bundle / "bundle.toml", "this is not a manifest\n");
    const auto download = home.xlings({"download", "fixture:alpha@1.0.0", "--to", bundle.string()});
    EXPECT_NE(download.exit_code, 0) << download.transcript();
    EXPECT_NE(download.transcript().find("bundle.toml"), std::string::npos) << download.transcript();
    EXPECT_FALSE(fs::exists(bundle / "archives"));
    EXPECT_EQ(tk::read_file(bundle / "bundle.toml"), "this is not a manifest\n");
}

// X2: what no source has is said once, all of it, with the way out.
XTEST(ArchiveSources, ArchivesNoSourceHasAreListedTogether, .area = "xim",
      .covers = {"ARCHIVE-UNAVAILABLE"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("archives-missing");
    auto f = make_fixture_(home.root() / "fixture", "http://127.0.0.1:9");
    init_home_(home, config_(f));
    const auto empty = home.root() / "empty";
    fs::create_directories(empty);
    const auto install = home.xlings({"install", "--from", empty.string(), "fixture:alpha@1.0.0", "-y"},
                                     {{"XLINGS_NET_UNREACHABLE", std::string(kOffline)}});
    const auto out = install.transcript();
    EXPECT_NE(install.exit_code, 0) << out;
    EXPECT_EQ(count_(out, "archives are in no source"), 1u) << out;
    EXPECT_NE(out.find("2 archives are in no source, and the network is not used"), std::string::npos) << out;
    for (const auto* name : {"alpha", "beta"}) {
        EXPECT_NE(out.find(sha_(f.files[host_file_(name)])), std::string::npos) << name << "\n" << out;
        EXPECT_NE(out.find(host_file_(name)), std::string::npos) << name << "\n" << out;
        EXPECT_NE(out.find(std::format("fixture:{}@1.0.0", name)), std::string::npos) << out;
    }
    EXPECT_NE(out.find("xlings download fixture:beta@1.0.0 fixture:alpha@1.0.0 --to"), std::string::npos) << out;
    EXPECT_NE(out.find(empty.string()), std::string::npos) << "the directories asked are named\n" << out;
}

// A download the network was asked for and failed is reported once, by its
// package, with what failed -- not again as an archive no source had.
XTEST(ArchiveSources, ANetworkFailureIsReportedOnceWithItsReason, .area = "xim",
      .covers = {"ARCHIVE-UNAVAILABLE"}, .requires_ = {"xlings-bin"}) {
    auto home = tk::Home::isolated("archives-404");
    const auto root = home.root() / "fixture";
    fs::create_directories(root / "nothing");
    auto server = tk::fixture::Server::start(root / "nothing");   // serves no file at all
    ASSERT_TRUE(server) << server.error();
    auto f = make_fixture_(root, server->url());
    init_home_(home, config_(f));
    const auto install = home.xlings({"install", "fixture:beta@1.0.0", "-y"});
    const auto out = install.transcript();
    EXPECT_NE(install.exit_code, 0) << out;
    EXPECT_NE(out.find("404"), std::string::npos) << out;
    EXPECT_EQ(count_(out, "download failed"), 1u) << out;
    EXPECT_EQ(out.find("in no source"), std::string::npos) << out;
}
