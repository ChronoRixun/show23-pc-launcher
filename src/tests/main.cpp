#include "std_include.hpp"
#include "test.hpp"
#include "check_inputs_cli.hpp"
#include "package_setup_tests.hpp"
#include "play_session_tests.hpp"

#include <cstdio>

namespace tests
{
    namespace
    {
        int failures = 0;
    }

    std::vector<test_case>& registry()
    {
        static std::vector<test_case> cases;
        return cases;
    }

    void check(const bool ok, const char* expression, const char* file, const int line)
    {
        if (!ok)
        {
            ++failures;
            std::printf("    FAIL  %s (%s:%d)\n", expression, file, line);
        }
    }
}

int wmain(const int argc, wchar_t** argv)
{
    const auto args = std::span(argv, static_cast<std::size_t>(argc));
    if (const auto fake = fake_play_child(args)) return *fake;
    if (const auto fake = fake_unpacker(args)) return *fake;
    if (argc > 1) return run_check_inputs_cli(args.subspan(1));
    auto failed_tests = 0;
    for (const auto& test : tests::registry())
    {
        const auto before = tests::failures;
        test.body();
        const auto ok = tests::failures == before;
        failed_tests += !ok;
        std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", test.name);
    }
    std::printf("%zu tests, %d failed\n", tests::registry().size(), failed_tests);
    return failed_tests ? 1 : 0;
}
