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
            // Closing the bootstrap after a confirmed start must not kill the game.
        }
        SIZE_T bytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) return GetLastError();
        std::vector<unsigned char> storage(bytes);
        auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) return GetLastError();
        struct cleanup
        {
            PPROC_THREAD_ATTRIBUTE_LIST list;
            ~cleanup() { DeleteProcThreadAttributeList(list); }
        } release{attributes};
        auto inherited = confirm.get();
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            &inherited, sizeof(inherited), nullptr, nullptr)) return GetLastError();
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
                EXTENDED_STARTUPINFO_PRESENT | (trial ? CREATE_SUSPENDED : 0), nullptr,
                exe.parent_path().c_str(), &startup.StartupInfo, &info))
            {
                process.reset(info.hProcess);
                const handle thread(info.hThread);
                if (trial && (!AssignProcessToJobObject(job.get(), process.get()) || ResumeThread(thread.get()) == DWORD(-1)))
                {
                    error = GetLastError();
                    TerminateProcess(process.get(), error);
                    WaitForSingleObject(process.get(), INFINITE);
                    process.reset(nullptr);
                    return error;
                }
                return ERROR_SUCCESS;
            }
            error = GetLastError();
            if (attempt < 2) Sleep(500);
        }
        return error;
    }
}
