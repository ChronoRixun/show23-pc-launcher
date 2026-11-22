#include "versioned_start.hpp"
#include "show23/child_process.hpp"

#include <versions/records.hpp>
#include <utils/finally.hpp>
#include <utils/flags.hpp>
#include <utils/properties.hpp>
#include <utils/string.hpp>

#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>

namespace versioned_start
{
    namespace
    {
        paths resolved;
        HANDLE confirm_event{};
        std::mutex gate_mutex;
        gate access = gate::open;
        std::chrono::steady_clock::time_point deadline{};
        std::atomic_bool restart_requested{};
    }

    paths locate(const std::filesystem::path& executable)
    {
        const auto slot = executable.parent_path();
        const auto versions_root = slot.parent_path();
        const auto root = versions_root.parent_path();
        const auto folder = utils::string::path_to_utf8(slot.filename());
        if (executable.filename() != L"show23-pc-launcher.exe" || versions_root.filename() != L"versions" ||
            root.filename() != L"show23-pc" || !versions::valid_folder(folder) || !root.has_parent_path()) return {};
        return {slot, versions_root, root.parent_path() / "show23-pc.exe", folder};
    }

    void initialize(const std::filesystem::path& executable)
    {
        resolved = locate(executable);
        if (resolved) utils::properties::set_portable_root(resolved.versions.parent_path());
    }

    const paths& layout() { return resolved; }

    bool accept_parent()
    {
        const auto supplied = utils::flags::get_flag_value("-bootstrap");
        const auto schema = utils::flags::get_flag_value("-bootstrap-version");
        const auto folder = utils::flags::get_flag_value("-folder");
        const auto event = utils::flags::get_flag_value("-confirm-event").value_or("");
        std::uintptr_t value{};
        const auto result = std::from_chars(event.data(), event.data() + event.size(), value);
        if (!resolved || !supplied || utils::string::utf8_to_path(*supplied) != resolved.bootstrap ||
            schema != std::to_string(versions::bootstrap_version) || folder != resolved.folder ||
            result.ec != std::errc{} || result.ptr != event.data() + event.size() || !value) return false;
        const auto handle = reinterpret_cast<HANDLE>(value);
        DWORD flags{};
        if (!GetHandleInformation(handle, &flags) || !SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) return false;

        confirm_event = handle;
        if (utils::flags::has_flag("-trial")) access = gate::waiting;
        return true;
    }

    bool hand_over(bool online)
    {
        if (!resolved || !versions::plain_path(resolved.bootstrap)) return false;
        int count{};
        const auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) return false;
        const auto release = utils::finally([&] { LocalFree(args); });
        auto command = show23::quote_process_argument(resolved.bootstrap.native());
        if (online) command += L" -update-restart";
        for (int i = 1; i < count; ++i)
        {
            const std::wstring_view arg(args[i]);
            if (arg == L"--bootstrap" || arg == L"--bootstrap-version" || arg == L"--folder" || arg == L"--confirm-event")
            {
                ++i;
                continue;
            }
            if (arg == L"--trial" || (online && _wcsicmp(args[i], L"-offline") == 0)) continue;
            command += L" " + show23::quote_process_argument(arg);
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(resolved.bootstrap.c_str(), command.data(), nullptr, nullptr, FALSE,
            0, nullptr, resolved.bootstrap.parent_path().c_str(), &startup, &process)) return false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    bool confirm()
    {
        std::lock_guard lock(gate_mutex);
        if (!confirm_event) return false;
        const auto signaled = SetEvent(confirm_event);
        CloseHandle(std::exchange(confirm_event, nullptr));
        if (access == gate::waiting)
        {
            if (signaled) deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            else access = gate::deferred;
        }
        return signaled != FALSE;
    }

    gate status()
    {
        std::lock_guard lock(gate_mutex);
        if (access != gate::waiting || deadline == std::chrono::steady_clock::time_point{}) return access;
        if (std::chrono::steady_clock::now() >= deadline) access = gate::deferred;
        else
        {
            const auto pointer = versions::load_pointer(resolved.versions);
            if (pointer.source == versions::pointer_source::primary && pointer.value.current == resolved.folder)
                access = gate::open;
        }
        return access;
    }

    bool unlocked() { return status() == gate::open; }

    bool command_allowed(std::string_view command)
    {
        if (unlocked()) return true;
        // Keep the window and read-only Home information usable during the trial.
        constexpr std::string_view allowed[] = {
            "show", "close", "minimize", "is-maximized", "toggle-maximize",
            "confirm-launcher-started", "get-startup", "get-settings", "get-show23",
            "get-parts", "get-launcher-update", "get-play", "get-card-import", "get-verify",
            "get-setup-inputs", "get-mod-import", "get-support-diagnostics", "suggest-player-name"
        };
        return std::ranges::find(allowed, command) != std::end(allowed);
    }

    std::string take_notices()
    {
        if (!resolved) return {};
        const auto path = resolved.versions / versions::notice_file;
        const auto mask = versions::parse_notices(versions::read_record(path, 256));
        if (!versions::plain_path(path) || !DeleteFileW(path.c_str())) return {};
        std::string messages;
        for (std::size_t i = 0; i < std::size(versions::notice_ids); ++i)
            if (mask & (1u << i))
            {
                messages += versions::notice_ids[i];
                messages += '\n';
            }
        return messages;
    }

    bool request_restart()
    {
        if (!resolved || !unlocked()) return false;
        restart_requested = true;
        return true;
    }

    bool restarting() { return restart_requested.load(); }

    void set_taskbar_target(void* window)
    {
        if (!resolved || !window) return;
        IPropertyStore* store{};
        if (FAILED(SHGetPropertyStoreForWindow(static_cast<HWND>(window), IID_PPV_ARGS(&store)))) return;
        const auto release = utils::finally([&] { store->Release(); });
        const auto set_string = [&](const PROPERTYKEY& key, const std::wstring& text)
        {
            PROPVARIANT property{};
            property.vt = VT_LPWSTR;
            property.pwszVal = const_cast<wchar_t*>(text.c_str());
            store->SetValue(key, property);
        };
        set_string(PKEY_AppUserModel_ID, L"show23-pc.launcher");
        set_string(PKEY_AppUserModel_RelaunchCommand, show23::quote_process_argument(resolved.bootstrap.native()));
        set_string(PKEY_AppUserModel_RelaunchIconResource, resolved.bootstrap.native() + L",0");
        set_string(PKEY_AppUserModel_RelaunchDisplayNameResource, L"@" + resolved.bootstrap.native() + L",-102");
        store->Commit();
    }
}
