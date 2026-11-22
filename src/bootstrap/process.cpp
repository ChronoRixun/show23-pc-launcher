#include "process.hpp"

#include <shellapi.h>

namespace bootstrap
{
    std::wstring widen(std::string_view text)
    {
        if (text.empty()) return {};
        const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (!size) return {};
        std::wstring result(size, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
        return result;
    }

    std::string narrow(std::wstring_view text)
    {
        if (text.empty()) return {};
        const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
            nullptr, 0, nullptr, nullptr);
        if (!size) return {};
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
            result.data(), size, nullptr, nullptr);
        return result;
    }

    std::wstring quote(std::wstring_view arg)
    {
        std::wstring result = L"\"";
        std::size_t slashes{};
        for (const auto c : arg)
        {
            if (c == L'\\') { ++slashes; continue; }
            result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            result += c;
        }
        result.append(slashes * 2, L'\\');
        return result + L'"';
    }

    arguments parse_arguments()
    {
        arguments result;
        int count{};
        const auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!argv) return result;
        for (int i = 1; i < count; ++i)
        {
            const std::wstring_view arg(argv[i]);
            if (arg == L"--prepare") result.prepare = true;
            else if (arg == L"-update-restart") result.update_restart = true;
            else if (arg == L"--bootstrap" || arg == L"--bootstrap-version" || arg == L"--folder" || arg == L"--confirm-event")
                ++i;
            else if (arg != L"--trial") result.flags.emplace_back(arg);
        }
        LocalFree(argv);
        return result;
    }

    DWORD child::launch(const versions::fs::path& exe, const versions::fs::path& bootstrap,
        const std::string& folder, bool trial, const std::vector<std::wstring>& flags)
    {
        if (!versions::plain_path(exe)) return ERROR_FILE_NOT_FOUND;
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        confirm.reset(CreateEventW(&security, TRUE, FALSE, nullptr));
        if (!confirm) return GetLastError();
        if (trial)
        {
            job.reset(CreateJobObjectW(nullptr, nullptr));
            if (!job) return GetLastError();
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
                return GetLastError();
        }
        const DWORD attribute_count = trial ? 2 : 1;
        SIZE_T bytes{};
        InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &bytes);
        if (!bytes) return GetLastError();
        std::vector<unsigned char> storage(bytes);
        auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, attribute_count, 0, &bytes)) return GetLastError();
        auto inherited = confirm.get();
        auto trial_job = job.get();
        struct cleanup
        {
            PPROC_THREAD_ATTRIBUTE_LIST list;
            ~cleanup() { DeleteProcThreadAttributeList(list); }
        } release{attributes};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            &inherited, sizeof(inherited), nullptr, nullptr)) return GetLastError();
        // Apply the job limit before the trial starts running.
        if (trial && !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
            &trial_job, sizeof(trial_job), nullptr, nullptr)) return GetLastError();
        const auto command = quote(exe.native()) + L" --bootstrap " + quote(bootstrap.native()) +
            L" --bootstrap-version " + std::to_wstring(versions::bootstrap_version) +
            L" --folder " + quote(widen(folder)) + (trial ? L" --trial" : L"") +
            L" --confirm-event " + std::to_wstring(reinterpret_cast<ULONG_PTR>(inherited));
        std::wstring cmd = command;
        for (const auto& flag : flags) cmd += L" " + quote(flag);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION info{};
        DWORD error{};
        for (unsigned attempt = 0; attempt < 3; ++attempt)
        {
            auto buffer = cmd;
            if (CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT, nullptr, exe.parent_path().c_str(), &startup.StartupInfo, &info))
            {
                process.reset(info.hProcess);
                const handle thread(info.hThread);
                return ERROR_SUCCESS;
            }
            error = GetLastError();
            if (attempt < 2) Sleep(500);
        }
        return error;
    }

    bool child::release_trial_kill_on_close()
    {
        if (!job || !confirm)
        {
            SetLastError(ERROR_INVALID_HANDLE);
            return false;
        }
        const auto status = WaitForSingleObject(confirm.get(), 0);
        if (status != WAIT_OBJECT_0)
        {
            if (status == WAIT_TIMEOUT) SetLastError(ERROR_NOT_READY);
            return false;
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        if (!QueryInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr))
            return false;
        limits.BasicLimitInformation.LimitFlags &= ~JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        return SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE;
    }
}
