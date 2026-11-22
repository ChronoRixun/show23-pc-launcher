#include "test.hpp"
#include "../bootstrap/process.hpp"

#include <cwchar>
#include <optional>
#include <span>

namespace
{
    constexpr std::wstring_view confirmed_marker = L"--bootstrap-job-confirmed-fixture";
    constexpr std::wstring_view unconfirmed_marker = L"--bootstrap-job-unconfirmed-fixture";
    constexpr int finished_code = 42;

    struct trial_fixture
    {
        const std::wstring name = L"Local\\show23-pc-job-test-" + bootstrap::widen(versions::request_id());
        bootstrap::handle ready{CreateEventW(nullptr, TRUE, FALSE, (name + L"-ready").c_str())};
        bootstrap::handle finish{CreateEventW(nullptr, TRUE, FALSE, (name + L"-finish").c_str())};
        bootstrap::child app;

        bool launch(std::wstring_view marker)
        {
            if (!ready || !finish) return false;
            std::wstring path(32768, L'\0');
            const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (!size || size >= path.size()) return false;
            path.resize(size);
            return app.launch(path, path, "0.1.3", true, {name, std::wstring(marker)}) == ERROR_SUCCESS &&
                WaitForSingleObject(ready.get(), 5000) == WAIT_OBJECT_0;
        }

        ~trial_fixture()
        {
            if (app.process && WaitForSingleObject(app.process.get(), 0) == WAIT_TIMEOUT)
            {
                TerminateProcess(app.process.get(), ERROR_PROCESS_ABORTED);
                WaitForSingleObject(app.process.get(), 5000);
            }
        }
    };
}

std::optional<int> fake_bootstrap_job_child(const std::span<wchar_t*> args)
{
    const std::wstring_view marker(args.back());
    if (marker != confirmed_marker && marker != unconfirmed_marker) return {};
    if (args.size() != 12 || std::wstring_view(args[8]) != L"--confirm-event") return 3;

    const std::wstring name(args[10]);
    const bootstrap::handle ready{OpenEventW(EVENT_MODIFY_STATE, FALSE, (name + L"-ready").c_str())};
    const bootstrap::handle finish{OpenEventW(SYNCHRONIZE, FALSE, (name + L"-finish").c_str())};
    if (!ready || !finish) return 3;
    if (marker == confirmed_marker)
    {
        const auto confirm = reinterpret_cast<HANDLE>(std::wcstoull(args[9], nullptr, 10));
        if (!SetEvent(confirm)) return 4;
    }
    if (!SetEvent(ready.get())) return 3;
    return WaitForSingleObject(finish.get(), 30000) == WAIT_OBJECT_0 ? finished_code : 5;
}

TEST(bootstrap_closing_job_terminates_an_unconfirmed_trial)
{
    trial_fixture trial;
    const auto launched = trial.launch(unconfirmed_marker);
    CHECK(launched);
    if (!launched) return;

    CHECK(WaitForSingleObject(trial.app.confirm.get(), 0) == WAIT_TIMEOUT);
    CHECK(!trial.app.release_trial_kill_on_close());
    trial.app.job.reset(nullptr);
    CHECK(WaitForSingleObject(trial.app.process.get(), 5000) == WAIT_OBJECT_0);
}

TEST(bootstrap_a_confirmed_trial_survives_after_releasing_the_job_limit)
{
    trial_fixture trial;
    const auto launched = trial.launch(confirmed_marker);
    CHECK(launched);
    if (!launched) return;

    CHECK(WaitForSingleObject(trial.app.confirm.get(), 0) == WAIT_OBJECT_0);
    CHECK(trial.app.release_trial_kill_on_close());
    trial.app.job.reset(nullptr);
    CHECK(SetEvent(trial.finish.get()));
    CHECK(WaitForSingleObject(trial.app.process.get(), 5000) == WAIT_OBJECT_0);
    DWORD code{};
    CHECK(GetExitCodeProcess(trial.app.process.get(), &code) && code == static_cast<DWORD>(finished_code));
}
