// XTEST: a gtest TEST with metadata (design §24.3).
//
//   #include <gtest/gtest.h>
//   import xlings.testkit;          // before the header: it uses the module,
//   #include "xlings/xtest.hpp"     // and mcpp's scanner reads imports from
//   import std;                     // the source file, not from headers
//
//   XTEST(SubosExec, ExitCodeIsTheCommands,
//         .area = "subos", .covers = {"EXIT-CMD"}, .requires_ = {"sandbox"}) {
//       auto home = xlings::testkit::Home::isolated("exec");
//       ...
//   }
//
// The metadata is registered at static-init time, so `XTEST_META_OUT` lists
// every test in the binary whether or not a filter ran it. Before the body
// runs, the declared capabilities are checked: missing on a developer machine
// is a skip that says why; missing on a lane that declared it is a failure.
//
// This is a header and not part of the module because a macro cannot be
// exported from a module, and the listener has to be registered by the test
// binary itself.
#pragma once

// No standard headers here: the test source has imported xlings.testkit by
// now, and GCC rejects a std header textually included after an import that
// reaches std. Everything used below comes in with gtest.h.
#include <gtest/gtest.h>

namespace xlings::testkit::detail {

inline std::string current_test_name() {
    auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    if (!info) return "unknown";
    return std::string(info->test_suite_name()) + "." + info->name();
}

inline bool current_test_failed() { return ::testing::Test::HasFailure(); }

// One result line per test into $XTEST_RESULTS_OUT, with the metadata joined.
class ResultListener : public ::testing::EmptyTestEventListener {
    void OnTestEnd(const ::testing::TestInfo& info) override {
        const auto* r = info.result();
        std::string status = r->Skipped() ? "skip" : (r->Failed() ? "fail" : "pass");
        std::string message;
        for (int i = 0; i < r->total_part_count(); ++i) {
            const auto& part = r->GetTestPartResult(i);
            if (part.message() && *part.message()) {
                message = part.message();
                break;
            }
        }
        xlings::testkit::record_result(
            std::string(info.test_suite_name()) + "." + info.name(), status,
            static_cast<long long>(r->elapsed_time()), message);
    }
};

inline const bool installed = [] {
    xlings::testkit::set_probes(&current_test_failed, &current_test_name);
    ::testing::UnitTest::GetInstance()->listeners().Append(new ResultListener);
    return true;
}();

}  // namespace xlings::testkit::detail

#define XTEST_CAT_(a, b) a##b
#define XTEST_CAT(a, b) XTEST_CAT_(a, b)

#define XTEST(suite, name, ...)                                                    \
    static const ::xlings::testkit::Meta XTEST_CAT(xtest_meta_##suite##_, name) {   \
        __VA_ARGS__ };                                                              \
    [[maybe_unused]] static const bool XTEST_CAT(xtest_reg_##suite##_, name) =      \
        ::xlings::testkit::register_meta(#suite "." #name,                          \
                                         XTEST_CAT(xtest_meta_##suite##_, name));   \
    static void XTEST_CAT(xtest_body_##suite##_, name)();                           \
    TEST(suite, name) {                                                             \
        if (auto verdict = ::xlings::testkit::check_requirements(                   \
                XTEST_CAT(xtest_meta_##suite##_, name))) {                          \
            if (verdict->fail) FAIL() << verdict->reason;                           \
            GTEST_SKIP() << verdict->reason;                                        \
        }                                                                           \
        XTEST_CAT(xtest_body_##suite##_, name)();                                   \
    }                                                                               \
    static void XTEST_CAT(xtest_body_##suite##_, name)()
