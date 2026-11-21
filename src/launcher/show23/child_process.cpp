#include "std_include.hpp"
#include "child_process.hpp"

#include <array>
#include <cstddef>
#include <utility>

namespace show23
{
    namespace
    {
        using input_paths::handle;
        bool stop_job(HANDLE job, HANDLE primary)
        {
            // Job accounting can reach zero before process objects become signaled
            // and their file handles have closed. Capture wait handles before terminating.
            std::vector<ULONG_PTR> storage(32);
            std::vector<handle> members;
            for (;;)
            {
                auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
                if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list,
                    static_cast<DWORD>(storage.size() * sizeof(ULONG_PTR)), nullptr))
                {
                    for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i)
                    {
                        handle member(OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(list->ProcessIdList[i])));
                        if (member) members.push_back(std::move(member));
                    }
                    break;
                }
                if (GetLastError() != ERROR_MORE_DATA || storage.size() >= 65536) break;
                storage.resize(storage.size() * 2);
            }

            if (!TerminateJobObject(job, 1) || WaitForSingleObject(primary, INFINITE) != WAIT_OBJECT_0) return false;
            for (const auto& member : members)
                if (WaitForSingleObject(member.get(), 5000) != WAIT_OBJECT_0) return false;
            for (;;)
            {
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION counts{};
                if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &counts, sizeof(counts), nullptr))
                    return false;
                if (!counts.ActiveProcesses) return true;
                Sleep(10);
            }
        }

        struct process_attributes
        {
            std::vector<std::byte> storage;
            LPPROC_THREAD_ATTRIBUTE_LIST list{};
            ~process_attributes() { if (list) DeleteProcThreadAttributeList(list); }

            bool init(std::array<HANDLE, 2>& inherited)
            {
                SIZE_T size{};
                InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
                storage.resize(size);
                auto* ptr = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
                if (!InitializeProcThreadAttributeList(ptr, 1, 0, &size)) return false;
                list = ptr;
                return UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                                 sizeof(inherited), nullptr, nullptr) != FALSE;
            }
        };
    }

    std::wstring quote_process_argument(const std::wstring_view argument)
    {
        std::wstring quoted(1, L'"');
        std::size_t slashes{};
        for (const auto ch : argument)
        {
            if (ch == L'\\') { ++slashes; continue; }
            quoted.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            quoted += ch;
        }
        quoted.append(slashes * 2, L'\\');
        quoted += L'"';
        return quoted;
    }

    bool child_process::start(const fs::path& executable, const fs::path& directory,
        const std::span<const std::wstring> arguments, HANDLE input, HANDLE output, bool console_hidden)
    {
        if (process_ || job_) return false;
        handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return false;
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output;
        std::array inherited_handles{output, input};
        process_attributes attributes;
        if (!attributes.init(inherited_handles)) return false;
        startup.lpAttributeList = attributes.list;
        auto command = quote_process_argument(executable.native());
        for (const auto& argument : arguments)
        {
            command += L' ';
            command += quote_process_argument(argument);
        }
        PROCESS_INFORMATION process{};
        const DWORD flags = CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | (console_hidden ? CREATE_NO_WINDOW : 0);
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, flags, nullptr,
            directory.c_str(), &startup.StartupInfo, &process)) return false;
        handle child(process.hProcess), thread(process.hThread);
        if (!AssignProcessToJobObject(job.get(), child.get()) || ResumeThread(thread.get()) == DWORD(-1))
        {
            TerminateProcess(child.get(), 1);
            WaitForSingleObject(child.get(), INFINITE);
            return false;
        }
        process_ = std::move(child);
        job_ = std::move(job);
        return true;
    }

    bool child_process::running() const
    {
        return process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT;
    }

    DWORD child_process::id() const
    {
        return GetProcessId(process_.get());
    }

    std::optional<DWORD> child_process::exit_code() const
    {
        DWORD code{};
        if (!process_ || running() || !GetExitCodeProcess(process_.get(), &code)) return std::nullopt;
        return code;
    }

    bool child_process::stop() noexcept
    {
        if (!job_) return true;
        bool stopped = false;
        try { stopped = stop_job(job_.get(), process_.get()); }
        catch (...) {}
        job_ = {};
        process_ = {};
        return stopped;
    }
}
