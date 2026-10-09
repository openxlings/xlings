#include <gtest/gtest.h>
import xlings.testkit;
#include "xlings/xtest.hpp"
import std;
import xlings.subos.caps;

namespace tk = xlings::testkit;
namespace fs = std::filesystem;

XTEST(SubosCaps, ProbeRequestsUserNamespaceAndPreservesBackendFailure,
      .area = "subos", .requires_ = {"posix"}) {
    auto home = tk::Home::isolated("caps-probe");
    const auto backend = home.root() / "backend with spaces";
    tk::write_file(backend, R"SH(#!/bin/sh
if [ "$#" != 6 ] || [ "$1" != --unshare-user ] || [ "$2" != --ro-bind ] ||
   [ "$3" != / ] || [ "$4" != / ] || [ "$5" != -- ] || [ "$6" != /bin/true ]; then
    printf 'probe did not request its own user namespace\n' >&2
    exit 23
fi
)SH");
    fs::permissions(backend, fs::perms::owner_exec, fs::perm_options::add);
    xlings::subos::caps::Backend candidate{.name = "bwrap", .bin = backend, .source = "fixture"};
    xlings::subos::caps::probe_bwrap(candidate);
    ASSERT_TRUE(candidate.usable) << candidate.probe_output;
    EXPECT_TRUE(candidate.probe_output.empty());

    tk::write_file(backend, "#!/bin/sh\nprintf 'namespace denied by fixture\\n' >&2\nexit 1\n");
    xlings::subos::caps::probe_bwrap(candidate);
    EXPECT_FALSE(candidate.usable);
    EXPECT_NE(candidate.probe_output.find("namespace denied by fixture"), std::string::npos);
}
