export module xlings.testkit.index_fixture;
import std;
export namespace xlings::testkit::index_fixture {
// One owned loopback mirror per test process, shared by its isolated homes.
std::string config(std::string_view mirror);
std::string url();
}
