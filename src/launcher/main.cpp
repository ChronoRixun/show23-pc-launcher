#include "std_include.hpp"
#include "cef/cef_ui.hpp"
#include "commands/commands.hpp"
#include "show23/shortcuts.hpp"
#include "updater/launcher_update.hpp"
#include "versioned_start.hpp"
#include <versions/instance.hpp>

#include <utils/flags.hpp>
#include <utils/named_mutex.hpp>
#include <utils/properties.hpp>
#include <utils/property_keys.hpp>
#include <utils/io.hpp>
#include <utils/nt.hpp>

#include <crtdbg.h>
#include <thread>

namespace
{
    void set_working_directory()
    {
        const auto appdata = utils::properties::get_appdata_path();

        std::filesystem::create_directories(appdata / "data" / "logs");

        std::filesystem::current_path(appdata);
    }

    void enable_dpi_awareness()
    {
        const utils::nt::library user32{"user32.dll"};

        const auto set_dpi_awareness_context = user32
            ? user32.get_proc<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>("SetProcessDpiAwarenessContext")
            : nullptr;

        // Minimum: Windows 10, version 1703
        if (set_dpi_awareness_context)
        {
            set_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
        }

        const utils::nt::library shcore{"shcore.dll"};

        const auto set_dpi_awareness = shcore
            ? shcore.get_proc<HRESULT(WINAPI*)(PROCESS_DPI_AWARENESS)>("SetProcessDpiAwareness")
            : nullptr;

        // Minimum: Windows 8.1
        if (set_dpi_awareness)
        {
            set_dpi_awareness(PROCESS_PER_MONITOR_DPI_AWARE);
            return;
        }

        // Call vista function if nothing else was not resolved
        SetProcessDPIAware();
    }

    bool remove_data_root(const std::filesystem::path& root)
    {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        return !std::filesystem::exists(root, ec);
    }

    // Finishes a Settings data move: the new root carries the move marker, the old root gets deleted.
    void remove_previous_data_root()
    {
        const auto current = utils::properties::get_appdata_path();
        const auto move_marker = utils::properties::get_move_marker(current);

        std::error_code ec;
        if (!std::filesystem::exists(move_marker, ec)) return;

        const auto previous = utils::properties::is_portable() ? utils::properties::get_local_root() : utils::properties::get_portable_root();
        const bool can_delete = !utils::io::is_inside_folder(previous, current) &&
                                !utils::io::is_inside_folder(current, previous);

        std::thread([previous, move_marker, can_delete]
        {
            // The old instance's CEF subprocesses hold files open for a few seconds after it exits.
            auto done = !can_delete;
            for (auto attempt = 0; !done && attempt < 15; ++attempt)
            {
                done = remove_data_root(previous);
                if (!done) std::this_thread::sleep_for(1s);
            }

            if (done)
            {
                utils::io::remove_file(move_marker);
            }
        }).detach();
    }

    utils::named_mutex& singleton_mutex()
    {
        static utils::named_mutex mutex{versioned_start::layout() ? versions::launcher_mutex_name : versions::bootstrap_mutex_name};
        return mutex;
    }

    bool try_become_singleton()
    {
        // Started by the self-update's restart: the previous process lets go of the lock only as it
        // exits, which can take longer than a few seconds on a busy PC.
        return singleton_mutex().try_lock(launcher_update::is_restart() ? 30s : 3s);
    }

    bool is_subprocess()
    {
        return utils::flags::has_flag("-show23-subprocess");
    }

    void run_watchdog()
    {
        std::thread([]()
        {
            const auto parent = utils::nt::get_parent_pid();
            if (utils::nt::wait_for_process(parent))
            {
                std::this_thread::sleep_for(3s);
                utils::nt::terminate();
            }
        }).detach();
    }

    std::filesystem::path browser_path(const std::filesystem::path& data)
    {
        const auto& layout = versioned_start::layout();
        return (layout ? layout.release : data / "data") / "cef" / CONFIG_NAME;
    }

    int run_subprocess(const utils::nt::library& process, const std::filesystem::path& path)
    {
        const cef::cef_ui cef_ui{process, path, browser_path(path)};
        return cef_ui.run_process();
    }

    void show_window(const utils::nt::library& process, const std::filesystem::path& path)
    {
        cef::cef_ui cef_ui{process, path, browser_path(path)};
        commands::register_all_commands(cef_ui);
        const auto& layout = versioned_start::layout();
        cef_ui.create(layout ? layout.release / "ui" : path / "data" / "launcher-ui", "main.html");
        versioned_start::set_taskbar_target(cef_ui.get_window());

        cef::cef_ui::work();
        commands::stop_workers();
    }


}

int CALLBACK WinMain(const HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    // Harden DLL search before anything else runs: System32 + AddDllDirectory entries only.
    // Prevents planting via stray DLLs in the launcher's own directory.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);

#ifdef _DEBUG
    // Test runs (tools\run-test-debug.bat passes -no-assert-dialogs): a failed assertion must never
    // leave a modal dialog on the desktop that stops a launcher thread until someone clicks it. It
    // goes to the debugger output instead and the process ends (exit code 3), which the test sees.
    if (utils::flags::has_flag("no-assert-dialogs"))
    {
        _set_error_mode(_OUT_TO_STDERR);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
#endif

    try
    {
        const utils::nt::library lib{instance};
        const auto executable = lib.get_path();
        versioned_start::initialize(executable);
        const auto versioned = static_cast<bool>(versioned_start::layout());
        if (!versioned && _wcsicmp(executable.filename().c_str(), L"show23-pc-launcher.exe") == 0)
            throw std::runtime_error("Extract the entire release ZIP, then start show23-pc.exe from its top folder.");
        if (is_subprocess())
        {
            set_working_directory();
            run_watchdog();
            return run_subprocess(lib, utils::properties::get_appdata_path());
        }

        if (versioned && !utils::flags::has_flag("-bootstrap"))
        {
            if (versioned_start::hand_over()) return 0;
            throw std::runtime_error("Could not open show23-pc.exe from the extracted launcher folder.");
        }
        if (versioned && !versioned_start::accept_parent()) return 1;

        if (!try_become_singleton())
        {
            if (!versions::activate_running_window())
                MessageBoxW(nullptr, L"show23-pc is already open.", L"show23-pc", MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        set_working_directory();
        const auto path = utils::properties::get_appdata_path();
        enable_dpi_awareness();

#if defined(DEBUG)
        AllocConsole();
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        printf("Debug console enabled\n");
#endif

        // A downloaded launcher update (portable installs) goes in now, before CEF loads its files.
        if (!versioned) switch (launcher_update::apply_pending())
        {
        case launcher_update::result::relaunch:
            singleton_mutex().unlock(); // the new launcher takes it
            if (!launcher_update::relaunch())
            {
                MessageBoxW(nullptr, L"show23-pc launcher could not start itself again after an update step. Start it again.",
                            L"show23-pc launcher", MB_ICONERROR | MB_OK);
                return 1;
            }
            return 0;
        case launcher_update::result::stop:
            return 1;
        case launcher_update::result::none:
            break;
        }
        if (!versioned)
        {
            launcher_update::cleanup();
            remove_previous_data_root();
        }

        // Persistent equivalent of -offline, settable from the Settings page.
        if (utils::properties::load(property_keys::OFFLINE_MODE) == "true") utils::flags::add_flag("offline");

        try { if (versioned_start::unlocked()) show23::update_shortcuts(false); }
        catch (...) { printf("Error creating shortcuts\n"); }

        show_window(lib, path);

        return versioned_start::restarting() ? 75 : 0;
    }
    catch (std::exception& e)
    {
        MessageBoxA(nullptr, e.what(), "ERROR", MB_ICONERROR);
    }
    catch (...)
    {
        MessageBoxA(nullptr, "An unknown error occurred", "ERROR", MB_ICONERROR);
    }

    return 1;
}
