// **Each case named as it starts, when a run asks** (`ENG_TEST_PROGRESS`).
//
// ctest prints a test only when it ends, and doctest a case only when it
// fails. A test executable that never ends -- one case waiting on a thread, a
// socket or a child process -- left a log that named the executable and none
// of its several hundred cases. With the variable set, every case's name goes
// to the error stream before it runs, and how long it took after, flushed at
// once: what the log shows last is the case that hung.
//
// Compiled into every module's test executable (`engine_add_module_tests`),
// beside the shared headers here; it registers itself and nothing calls it.
#include <cstdio>
#include <cstdlib>
#include <doctest/doctest.h>

namespace {

[[nodiscard]] bool progressAsked()
{
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    const bool set = _dupenv_s(&value, &size, "ENG_TEST_PROGRESS") == 0 && value != nullptr;
    std::free(value);
    return set;
#else
    return std::getenv("ENG_TEST_PROGRESS") != nullptr;
#endif
}

struct Progress final : doctest::IReporter
{
    explicit Progress(const doctest::ContextOptions& options) : on(progressAsked()) { (void)options; }

    void report_query(const doctest::QueryData&) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats&) override {}
    void test_case_start(const doctest::TestCaseData& data) override
    {
        if (!on)
            return;
        std::fprintf(stderr, "[case] %s\n", data.m_name);
        std::fflush(stderr);
    }
    void test_case_reenter(const doctest::TestCaseData&) override {}
    void test_case_end(const doctest::CurrentTestCaseStats& stats) override
    {
        if (!on)
            return;
        std::fprintf(stderr, "[case] ended in %.2f s\n", stats.seconds);
        std::fflush(stderr);
    }
    void test_case_exception(const doctest::TestCaseException&) override {}
    void subcase_start(const doctest::SubcaseSignature&) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData&) override {}
    void log_message(const doctest::MessageData&) override {}
    void test_case_skipped(const doctest::TestCaseData&) override {}

    bool on = false;
};

} // namespace

REGISTER_LISTENER("progress", 1, Progress);
