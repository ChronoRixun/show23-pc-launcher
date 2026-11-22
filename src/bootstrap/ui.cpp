#include "ui.hpp"
#include "process.hpp"

#include <commctrl.h>
#include <shellapi.h>


#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace bootstrap
{
    void log(const versions::fs::path& root, const std::string_view line)
    {
        const auto file = root / "bootstrap.log";
        if (!versions::plain_path(file, true)) return;
        SYSTEMTIME now{};
        GetSystemTime(&now);
        const auto pad = [](unsigned n) { return (n < 10 ? "0" : "") + std::to_string(n); };
        const auto text = std::to_string(now.wYear) + "-" + pad(now.wMonth) + "-" + pad(now.wDay) + " " +
            pad(now.wHour) + ":" + pad(now.wMinute) + ":" + pad(now.wSecond) + "Z " + std::string(line) + "\r\n";
        std::error_code error;
        if (const auto size = versions::fs::file_size(file, error); !error && size + text.size() > 256 * 1024)
        {
            const auto rotated = root / "bootstrap.log.old";
            if (!versions::plain_path(rotated, true) ||
                !MoveFileExW(file.c_str(), rotated.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
        }
        const handle stream(CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
            nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!stream) return;
        DWORD count{};
        WriteFile(stream.get(), text.data(), static_cast<DWORD>(text.size()), &count, nullptr);
    }

    answer dialog(const versions::fs::path& root, std::wstring title, std::wstring detail, std::string_view newest,
        bool can_wait, std::string_view previous, bool can_retry, bool release_page)
    {
        detail += L"\n\nYour games, saves and settings are not affected.\nSee bootstrap.log in the versions folder.";
        log(root, narrow(title + L" " + detail));
        constexpr int wait = 100, prior = 101, retry = 102, folder = 103, release = 104;
        const auto previous_label = L"Start " + widen(previous);
        std::vector<TASKDIALOG_BUTTON> buttons;
        if (can_wait) buttons.push_back({wait, L"Keep waiting"});
        if (!previous.empty()) buttons.push_back({prior, previous_label.c_str()});
        if (can_retry) buttons.push_back({retry, L"Try again"});
        buttons.push_back({folder, L"Open folder"});
        if (release_page) buttons.push_back({release, L"Release page"});
        TASKDIALOGCONFIG config{};
        config.cbSize = sizeof(config);
        config.hInstance = GetModuleHandleW(nullptr);
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        config.pszWindowTitle = L"show23-pc";
        config.pszMainInstruction = title.c_str();
        config.pszContent = detail.c_str();
        config.pButtons = buttons.data();
        config.cButtons = static_cast<UINT>(buttons.size());
        config.nDefaultButton = can_wait ? wait : IDCLOSE;
        int selected{};
        if (FAILED(TaskDialogIndirect(&config, &selected, nullptr, nullptr)))
        {
            MessageBoxW(nullptr, detail.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
            return answer::close;
        }
        if (selected == folder)
            ShellExecuteW(nullptr, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (selected == release)
        {
            auto url = std::wstring(L"https://github.com/ChronoRixun/show23-pc-launcher/releases");
            if (versions::valid_version(newest)) url += L"/tag/v" + widen(newest);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (selected == wait) return answer::wait;
        if (selected == prior) return answer::previous;
        if (selected == retry) return answer::retry;
        return answer::close;
    }

    namespace
    {
        struct arrival_check
        {
            const versions::fs::path& folder;
            versions::verification result{{}, "version.txt"};
        };

        DWORD WINAPI verify_arrival(void* context)
        {
            auto& task = *static_cast<arrival_check*>(context);
            try { task.result = versions::verify(task.folder, versions::check_mode::full); }
            catch (...) {}
            return 0;
        }

        HRESULT CALLBACK arrival_progress(HWND window, UINT event, WPARAM, LPARAM, LONG_PTR context)
        {
            const auto worker = reinterpret_cast<HANDLE>(context);
            if (event == TDN_CREATED) SendMessageW(window, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 40);
            if (event == TDN_TIMER && WaitForSingleObject(worker, 0) == WAIT_OBJECT_0)
                PostMessageW(window, TDM_CLICK_BUTTON, IDCLOSE, 0);
            if (event == TDN_BUTTON_CLICKED && WaitForSingleObject(worker, 0) == WAIT_TIMEOUT) return S_FALSE;
            return S_OK;
        }
    }

    versions::verification check_arrival(const versions::fs::path& folder, bool quiet)
    {
        if (quiet) return versions::verify(folder, versions::check_mode::full);
        arrival_check task{folder};
        const handle worker(CreateThread(nullptr, 0, verify_arrival, &task, 0, nullptr));
        if (!worker) return versions::verify(folder, versions::check_mode::full);
        if (WaitForSingleObject(worker.get(), 1000) == WAIT_TIMEOUT)
        {
            TASKDIALOGCONFIG config{};
            config.cbSize = sizeof(config);
            config.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER;
            config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
            config.pszWindowTitle = L"show23-pc";
            config.pszMainInstruction = L"Checking files…";
            config.lpCallbackData = reinterpret_cast<LONG_PTR>(worker.get());
            config.pfCallback = arrival_progress;
            TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
        }
        WaitForSingleObject(worker.get(), INFINITE);
        return task.result;
    }

    bool print_folder(std::string_view folder)
    {
        auto output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!output || output == INVALID_HANDLE_VALUE)
        {
            if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
            output = GetStdHandle(STD_OUTPUT_HANDLE);
        }
        const auto line = std::string(folder) + "\r\n";
        DWORD count{};
        return WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &count, nullptr) && count == line.size();
    }
}
