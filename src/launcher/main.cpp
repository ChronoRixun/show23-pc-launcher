#include "std_include.hpp"
#include "cef/cef_ui.hpp"
#include "commands/commands.hpp"
#include "show23/shortcuts.hpp"
#include "updater/launcher_update.hpp"
#include "versioned_start.hpp"
#include "show23/child_process.hpp"
#include "show23/server_paths.hpp"
#include "updater/part_store.hpp"
#include "updater/release_source.hpp"
#include "show23/game_record.hpp"
#include <versions/records.hpp>
#include <versions/update_fixture.hpp>
#include <utils/finally.hpp>
#ifdef SHOW23_UPDATE_FIXTURE
#include <utils/http.hpp>
#endif
#include <versions/instance.hpp>

#include <utils/flags.hpp>
#include <utils/named_mutex.hpp>
#include <utils/properties.hpp>
#include <utils/property_keys.hpp>
#include <utils/io.hpp>
#include <utils/nt.hpp>

#include <crtdbg.h>
#include <shellapi.h>
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
        return singleton_mutex().try_lock(3s);
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
        launcher_update::stop();
    }

    int manual_play()
    {
        AttachConsole(ATTACH_PARENT_PROCESS);
        const auto error = [](std::string_view message)
        {
            const auto text = std::string(message) + "\r\n";
            DWORD written{};
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
            return 1;
        };
        const auto& layout = versioned_start::layout();
        if (!layout) return error("Extract the entire launcher ZIP before using manual Play.");
        int count{};
        const auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) return 1;
        const auto release = utils::finally([&] { LocalFree(args); });
        if (count < 3 || !*args[2]) return error("Choose an install folder for manual Play.");
        const auto install = std::filesystem::absolute(args[2]).lexically_normal();
        const parts::runtime_lease lease(install, false);
        if (!lease) return error("The game runtime is in use or its lock cannot be opened.");

        const auto runtime = parts::read_runtime(layout.release);
        std::string details;
        if (!parts::verify_runtime(runtime, launcher_update::running_version(), details)) return error(details);
        const auto server = show23::server_paths(parts::component_folder(runtime, parts::id::server));
        const auto emulator = parts::component_folder(runtime, parts::id::emulator) / "shadPS4.exe";
        std::vector<std::wstring> command{L"-B", server.script.native(), L"play", L"--install", install.native()};
        for (int i = 3; i < count; ++i) command.emplace_back(args[i]);
        command.insert(command.end(), {L"--emulator", emulator.native()});

        const auto duplicate = [](DWORD stream)
        {
            HANDLE result{};
            DuplicateHandle(GetCurrentProcess(), GetStdHandle(stream), GetCurrentProcess(), &result,
                0, TRUE, DUPLICATE_SAME_ACCESS);
            return show23::input_paths::handle(result);
        };
        const auto input = duplicate(STD_INPUT_HANDLE), output = duplicate(STD_OUTPUT_HANDLE);
        show23::child_process child;
        bool hidden = false;
#ifdef SHOW23_UPDATE_FIXTURE
        hidden = !utils::http::fixture_origin().empty() &&
            versions::plain_path(layout.versions.parent_path() / "fixture-mode");
#endif
        if (!input || !output || !child.start(server.python, server.script.parent_path(), command, input.get(), output.get(), hidden))
            return error("Could not start manual Play.");
        while (child.running()) Sleep(100);
        const auto code = child.exit_code().value_or(1);
        // Stop and wait for any remaining descendants before the lease is released.
        return child.stop() ? static_cast<int>(code) : 1;
    }

#ifdef SHOW23_UPDATE_FIXTURE
    int run_update_fixture(const std::filesystem::path& data)
    {
        using versions::fixture::setting;
        const auto& layout = versioned_start::layout();
        const auto target = versions::read_record(data / "fixture-mode", 64);
        if (!layout || !versions::valid_version(target) || utils::http::fixture_origin().empty() ||
            !versions::fixture::enabled(data)) return 2;
        {
            std::ofstream trace(data / "fixture-launches.txt", std::ios::binary | std::ios::app);
            trace << layout.folder << (utils::flags::has_flag("-trial") ? " trial " : " current ")
                  << GetCurrentProcessId() << '\n';
        }

        const auto current = launcher_update::running_version();
        if (setting("SHOW23_FIXTURE_BAD_VERSION") == current)
        {
            const auto mode = setting("SHOW23_FIXTURE_BAD_MODE");
            if (mode == "crash") TerminateProcess(GetCurrentProcess(), 112);
            if (mode == "hang") { Sleep(5000); return 113; }
        }
        if (!versioned_start::confirm()) return 2;
        while (versioned_start::status() == versioned_start::gate::waiting) Sleep(25);
        if (!versioned_start::unlocked()) return 3;
        release_source::sync_bootstrap_channel();
        launcher_update::cleanup();

        utils::io::write_file(data / ("fixture-confirmed-" + current + ".txt"), layout.folder);
        if (setting("SHOW23_FIXTURE_SHORTCUTS") == "1") show23::update_shortcuts(false);
        const auto action = setting("SHOW23_FIXTURE_ACTION");
        if (action == "confirm") return 0;
        const auto install = std::filesystem::path(setting("SHOW23_FIXTURE_INSTALL"));
        if (action == "stage" || action == "inspect-runtime" || action == "repair")
        {
            const auto relative = install.lexically_relative(data.parent_path());
            if (relative.empty() || relative == "." || *relative.begin() == ".." ||
                !versions::plain_path(install, true)) return 6;
            const auto& bundle = layout.release;
            bool ok = true;
            std::string error;
            if (action == "stage") ok = parts::stage_bundled_runtime(bundle, install, current);
            if (action == "repair")
            {
                error = parts::repair_bundled_runtime(bundle, install, current);
                ok = error.empty();
            }
            const auto status = parts::inspect_bundled_runtime(bundle, install, current);
            rapidjson::Document doc(rapidjson::kObjectType);
            auto& allocator = doc.GetAllocator();
            doc.AddMember("ready", status.ready, allocator);
            doc.AddMember("fresh", status.fresh, allocator);
            doc.AddMember("repairable", status.repairable, allocator);
            doc.AddMember("inUse", parts::runtime_in_use(install), allocator);
            doc.AddMember("ok", ok, allocator);
            const auto& message = error.empty() ? status.message : error;
            doc.AddMember("message", rapidjson::Value(message.c_str(), allocator), allocator);
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            utils::io::write_file(data / "fixture-runtime-result.json", buffer.GetString());
            return 0;
        }
        if (current == target) return 0;
        if (action == "retry")
        {
            if (!launcher_update::retry(target)) return 5;
            bool ready = false;
            for (unsigned i = 0; i < 200; ++i)
            {
                rapidjson::Document state;
                launcher_update::write_status(state, state.GetAllocator());
                const std::string_view phase(state["state"].GetString());
                if (phase == "ready") { ready = true; break; }
                if (phase != "downloading") break;
                Sleep(25);
            }
            launcher_update::stop();
            return ready ? 0 : 5;
        }
        if (action == "switch" && !launcher_update::channel_changed(release_channel::channel::stable)) return 6;
        std::optional<parts::runtime_lease> lease;
        if (action == "lease-update")
        {
            const auto selected = show23::install_folder();
            if (!selected || *selected != install) return 7;
            lease.emplace(install, false);
            if (!*lease) return 7;
        }
        launcher_update::check();
        bool requested{};
        auto code = 4;
        for (unsigned i = 0; i < 800; ++i)
        {
            rapidjson::Document state;
            launcher_update::write_status(state, state.GetAllocator());
            const std::string_view phase(state["state"].GetString());
            if (!requested && (phase == "available" || (action == "switch" && phase == "up-to-date")))
                requested = launcher_update::start_download(action == "switch" ? target : std::string{});
            if (phase == "ready")
            {
                std::string error;
                if (lease)
                {
                    if (commands::restart_launcher(error) || error != "busy") { code = 7; break; }
                    utils::io::write_file(data / "fixture-restart-blocked.txt", error);
                    lease.reset();
                }
                if (commands::restart_launcher(error)) code = 75;
                else utils::io::write_file(data / "fixture-error.txt", error);
                break;
            }
            if (phase == "failed" || phase == "check-failed")
            {
                const auto& detail = state["error"];
                utils::io::write_file(data / "fixture-error.txt", detail.GetString());
                break;
            }
            Sleep(50);
        }
        launcher_update::stop();
        return code;
    }
#endif
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
        if (utils::flags::has_flag("-manual-play")) return manual_play();
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

        if (!versioned) remove_previous_data_root();

        // Persistent equivalent of -offline, settable from the Settings page.
        if (utils::properties::load(property_keys::OFFLINE_MODE) == "true") utils::flags::add_flag("offline");

#ifdef SHOW23_UPDATE_FIXTURE
        if (utils::flags::has_flag("-update-fixture")) return run_update_fixture(path);
#endif
        try { if (versioned_start::unlocked()) show23::update_shortcuts(false); }
        catch (...) { printf("Error creating shortcuts\n"); }

        show_window(lib, path);

        return versioned_start::restarting() ? 75 : 0;
    }
    catch (std::exception& e)
    {
#ifdef SHOW23_UPDATE_FIXTURE
        const auto& layout = versioned_start::layout();
        if (layout && versions::fixture::enabled(layout.versions.parent_path()))
        {
            utils::io::write_file(layout.versions.parent_path() / "fixture-error.txt", e.what());
            return 1;
        }
#endif
        MessageBoxA(nullptr, e.what(), "ERROR", MB_ICONERROR);
    }
    catch (...)
    {
        MessageBoxA(nullptr, "An unknown error occurred", "ERROR", MB_ICONERROR);
    }

    return 1;
}
