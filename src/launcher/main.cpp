#include "std_include.hpp"
#include "cef/cef_ui.hpp"
#include "commands/commands.hpp"
#include "show23/shortcuts.hpp"
#include "updater/launcher_update.hpp"
#include "versioned_start.hpp"
#include "show23/child_process.hpp"
#include "show23/card_import.hpp"
#include "show23/play_session.hpp"
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

    enum class manual_action { setup, play, import_cards, update_runtime };

    int run_manual(manual_action action)
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
        if (!layout) return error("Extract the entire launcher ZIP before using the manual commands.");
        int count{};
        const auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args) return 1;
        const auto release = utils::finally([&] { LocalFree(args); });
        if (count < 3 || !*args[2]) return error("Choose an install folder.");
        const auto install = std::filesystem::absolute(args[2]).lexically_normal();
        if (!show23::is_local_path(install)) return error("Choose a local install folder.");
        const auto version = launcher_update::running_version();

        const auto duplicate = [](DWORD stream)
        {
            HANDLE result{};
            DuplicateHandle(GetCurrentProcess(), GetStdHandle(stream), GetCurrentProcess(), &result,
                0, TRUE, DUPLICATE_SAME_ACCESS);
            return show23::input_paths::handle(result);
        };
        const auto input = duplicate(STD_INPUT_HANDLE), output = duplicate(STD_OUTPUT_HANDLE);
        bool hidden = false;
#ifdef SHOW23_UPDATE_FIXTURE
        hidden = !utils::http::fixture_origin().empty() &&
            versions::plain_path(layout.versions.parent_path() / "fixture-mode");
#endif
        const auto launch = [&](const parts::installed_runtime& runtime, const std::vector<std::wstring>& arguments)
        {
            const auto server = show23::server_paths(parts::component_folder(runtime, parts::id::server));
            show23::child_process child;
            if (!input || !output || !child.start(server.python, server.script.parent_path(), arguments,
                                                   input.get(), output.get(), hidden))
                return error("Could not start the manual command.");
            while (child.running()) Sleep(100);
            const auto code = child.exit_code().value_or(1);
            return child.stop() ? static_cast<int>(code) : 1;
        };
        const auto manual_script = [](const parts::installed_runtime& runtime)
        {
            const auto& files = runtime.manifests[parts::index(parts::id::server)]->files;
            return std::ranges::any_of(files, [](const auto& file) { return file.path == "manual_mode.py"; }) ?
                parts::component_folder(runtime, parts::id::server) / "manual_mode.py" : std::filesystem::path{};
        };
        if (action == manual_action::setup)
        {
            if (count < 4 || count > 5) return error("Choose an unpacked base folder and an optional update folder.");
            const auto bundle = parts::read_runtime(layout.release);
            std::string details;
            if (!parts::verify_play_runtime(bundle, version, details)) return error(details);
            const auto script = manual_script(bundle);
            if (script.empty()) return error("The bundle is missing its manual setup script.");
            std::vector<std::wstring> command{L"-B", L"-X", L"utf8", L"-u", script.native(),
                                              L"setup", install.native(), L"--base", args[3]};
            if (count == 5) command.insert(command.end(), {L"--update", args[4]});
            {
                const parts::runtime_lease lease(install, true);
                if (!lease) return error("The game runtime is in use or its lock cannot be opened.");
                const auto status = parts::inspect_bundled_runtime(layout.release, install, version);
                if (!status.fresh) return error("Choose a fresh install folder for manual setup.");
                if (const auto code = launch(bundle, command); code) return code;
            }
            if (!parts::stage_bundled_runtime(layout.release, install, version))
                return error("The game folders were recorded, but the bundled runtime could not be installed.");
            return 0;
        }

        const auto game_record = show23::input_paths::inspect(install / "data/games/CUSA35521/game.json");
        if (!game_record.present || game_record.directory || game_record.error != show23::input_paths::path_error::none)
            return error("Run setup for this install folder first.");
        if (action == manual_action::update_runtime)
        {
            const auto rollback = count == 4 && std::wstring_view(args[3]) == L"--rollback";
            if (count != 3 && !rollback) return error("Give an install folder and optional --rollback.");
            const auto result = parts::change_runtime(layout.release, install, version,
                rollback ? parts::runtime_change::rollback : parts::runtime_change::update);
            if (result == "in-use") return error("Stop Play and close card import before changing the runtime.");
            return result.empty() ? 0 : error(result);
        }
        if (!parts::stage_bundled_runtime(layout.release, install, version))
            return error(parts::inspect_bundled_runtime(layout.release, install, version).message);
        const parts::runtime_lease lease(install, false);
        if (!lease) return error("The game runtime is in use or its lock cannot be opened.");
        const auto runtime = parts::read_runtime(install);
        std::string details;
        if (!parts::verify_play_runtime(runtime, version, details)) return error(details);
        const show23::activity_lock activity;
        if (!activity) return error("Close the active game session or card import before continuing.");
        if (action == manual_action::import_cards)
        {
            if (count != 3) return error("Give one install folder for card import.");
            if (!show23::local_server_ports_available()) return error("The local server is already running or its ports are unavailable.");
            const auto server = show23::server_paths(parts::component_folder(runtime, parts::id::server));
            return launch(runtime, {L"-B", L"-X", L"utf8", L"-u", server.script.native(), L"import-cards",
                                    L"--install", install.native(), L"--pictures"});
        }
        const auto script = manual_script(runtime);
        if (script.empty()) return error("The active runtime is missing its manual play script.");
        if (!parts::remember_server_state_version(install, runtime.manifests[parts::index(parts::id::server)]->version))
            return error("Could not record the local server's progress version.");
        std::vector<std::wstring> command{L"-B", L"-X", L"utf8", L"-u", script.native(), L"play", install.native()};
        for (int i = 3; i < count; ++i) command.emplace_back(args[i]);
        return launch(runtime, command);
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
        launcher_update::cleanup();

        utils::io::write_file(data / "fixture-channel.txt",
            release_channel::channel_name(release_source::current_channel()));
        utils::io::write_file(data / ("fixture-confirmed-" + current + ".txt"), layout.folder);
        if (setting("SHOW23_FIXTURE_SHORTCUTS") == "1") show23::update_shortcuts(false);
        const auto action = setting("SHOW23_FIXTURE_ACTION");
        if (action == "confirm") return 0;
        const auto install = std::filesystem::path(setting("SHOW23_FIXTURE_INSTALL"));
        if (action == "stage" || action == "inspect-runtime" || action == "repair" ||
            action == "activate-runtime" || action == "verify-runtime")
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
            if (action == "activate-runtime")
            {
                error = parts::change_runtime(bundle, install, current, parts::runtime_change::update);
                ok = error.empty();
            }
            if (action == "verify-runtime")
            {
                ok = parts::verify_play_runtime(parts::read_runtime(install), current, error);
                const auto game = show23::inspect_game(install);
                if (game.state != show23::record_state::ready) ok = false;
                else
                {
                    const auto findings = show23::verify_recorded_game(*game.record);
                    ok = ok && !findings.empty() && std::ranges::none_of(findings, [](const auto& finding)
                    {
                        return finding.state == show23::finding_state::failed;
                    });
                }
            }
            const auto status = parts::inspect_bundled_runtime(bundle, install, current);
            rapidjson::Document doc(rapidjson::kObjectType);
            auto& allocator = doc.GetAllocator();
            doc.AddMember("ready", status.ready, allocator);
            doc.AddMember("fresh", status.fresh, allocator);
            doc.AddMember("repairable", status.repairable, allocator);
            doc.AddMember("inUse", parts::runtime_in_use(install), allocator);
            doc.AddMember("ok", ok, allocator);
            doc.AddMember("update", rapidjson::Value(status.update_version.c_str(), allocator), allocator);
            doc.AddMember("rollback", rapidjson::Value(status.rollback_version.c_str(), allocator), allocator);
            const auto& message = error.empty() ? status.message : error;
            doc.AddMember("message", rapidjson::Value(message.c_str(), allocator), allocator);
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            doc.Accept(writer);
            utils::io::write_file(data / "fixture-runtime-result.json", buffer.GetString());
            return 0;
        }
        const auto fixture_channel = setting("SHOW23_FIXTURE_CHANNEL");
        if (!fixture_channel.empty() &&
            !launcher_update::channel_changed(release_channel::parse_channel(fixture_channel))) return 6;
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
                if (setting("SHOW23_FIXTURE_STOP_WHEN_DEFERRED") == "1" && state["requestDeferred"].GetBool())
                {
                    rapidjson::StringBuffer buffer;
                    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                    state.Accept(writer);
                    utils::io::write_file(data / "fixture-deferred-request.json", buffer.GetString());
                    code = 0;
                    break;
                }
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
        if (utils::flags::has_flag("-manual-play")) return run_manual(manual_action::play);
        if (utils::flags::has_flag("-manual-setup")) return run_manual(manual_action::setup);
        if (utils::flags::has_flag("-manual-import")) return run_manual(manual_action::import_cards);
        if (utils::flags::has_flag("-manual-update")) return run_manual(manual_action::update_runtime);
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
