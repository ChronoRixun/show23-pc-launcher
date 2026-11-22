#include "std_include.hpp"
#include "test.hpp"
#include "check_inputs_cli.hpp"
#include "package_setup_tests.hpp"
#include "play_session_tests.hpp"
#include "versions_tests.hpp"
#include <versions/records.hpp>
#include "show23/card_import.hpp"
#include "updater/part_store.hpp"
#include <utils/flags.hpp>
#include <utils/properties.hpp>
#include <utils/property_keys.hpp>
#include <utils/string.hpp>
#include "version.hpp"

#include <atomic>
#include <Windows.h>

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

namespace
{
    std::atomic_bool import_canceled{};
    BOOL WINAPI stop_import(DWORD event)
    {
        if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
        import_canceled = true;
        return TRUE;
    }

    int card_import_cli(std::span<wchar_t*> args)
    {
        if (args.size() != 3 || std::wstring_view(args[1]) != L"--install" || !*args[2])
        {
            std::fputs("Usage: show23-pc-tests.exe --import-cards --install <install>\n", stderr);
            return 1;
        }

        if (utils::flags::has_flag("offline") || utils::properties::load(property_keys::OFFLINE_MODE) == "true")
        {
            std::fputs("Turn off Offline mode to import special cards.\n", stderr);
            return 1;
        }
        try
        {
            const show23::fs::path install = args[2];
            const auto runtime = parts::read_runtime(install);
            std::string message;
            const auto version = release_channel::launcher_build(GIT_DESCRIBE, GIT_TAG, VERSION_PRODUCT).version;
            if (!parts::verify_card_import_runtime(runtime, version, message))
            {
                std::fprintf(stderr, "%s\n", message.c_str());
                return 1;
            }
            if (!SetConsoleCtrlHandler(stop_import, TRUE))
            {
                std::fputs("Could not register the cancel handler.\n", stderr);
                return 1;
            }
            const auto result = show23::import_cards(install, parts::component_folder(runtime, parts::id::server),
                [] { return import_canceled.load(); }, [](const show23::card_progress& progress)
                {
                    if (progress.done) std::printf("import: done %u cards\n", progress.cards);
                    else std::printf("import: page %u/%u\n", progress.page, progress.total);
                    std::fflush(stdout);
                });
            SetConsoleCtrlHandler(stop_import, FALSE);
            const auto log = utils::string::path_to_utf8(result.log);
            if (!log.empty()) std::fprintf(stderr, "Import log: %s\n", log.c_str());
            const std::string_view outcome = result.status;
            if (outcome == "complete") return 0;
            if (outcome == "network")
            {
                std::fputs("Network failure. Try again; finished pages are kept.\n", stderr);
                return 2;
            }
            std::fprintf(stderr, "Import stopped: %s\n", result.status);
        }
        catch (const std::exception& ex) { std::fprintf(stderr, "Import failed: %s\n", ex.what()); }
        return 1;
    }
}

int wmain(const int argc, wchar_t** argv)
{
    const auto args = std::span(argv, static_cast<std::size_t>(argc));
    if (argc == 3 && std::wstring_view(argv[1]) == L"--verify-version-folder")
    {
        const auto result = versions::verify(argv[2], versions::check_mode::full);
        if (!result) std::fprintf(stderr, "Version verification failed: %s\n", result.bad_file.c_str());
        else std::printf("Verified version: %s\n", result.fingerprint.c_str());
        return result ? 0 : 1;
    }
    if (const auto fake = fake_bootstrap_child(args)) return *fake;
    if (const auto fake = fake_play_child(args)) return *fake;
    if (const auto fake = fake_unpacker(args)) return *fake;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--import-cards") return card_import_cli(args.subspan(1));
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
