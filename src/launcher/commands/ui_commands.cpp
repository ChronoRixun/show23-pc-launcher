#include "std_include.hpp"
#include "ui_commands.hpp"
#include "cef/cef_ui.hpp"

#include "updater/part_store.hpp"
#include <utils/com.hpp>
#include <utils/flags.hpp>
#include <utils/logger.hpp>
#include <utils/nt.hpp>
#include <utils/properties.hpp>
#include <utils/property_keys.hpp>
#include <utils/string.hpp>



#include <utils/io.hpp>

#include <cctype>
#include <cwchar>

namespace commands::ui_commands
{
    void register_commands(cef::cef_ui& cef_ui, command_context&)
    {
        cef_ui.add_command("browse-folder", [&cef_ui](const auto&, rapidjson::Document& response)
        {
            response.SetNull();

            try
            {
                std::string folder;
                if (utils::com::select_folder(folder))
                {
                    response.SetString(folder, response.GetAllocator());
                }
            }
            catch (const std::exception& e)
            {
                printf("browse-folder failed: %s\n", e.what());
                if (utils::nt::is_wine_environment())
                {
                    cef_ui.show_message_box("Browse Failed", "Folder browser is not available under Wine. Please paste the game path manually.");
                }
            }
        });

        cef_ui.add_command("open-folder", [](const rapidjson::Value& value, auto&)
        {
            if (value.IsObject() && value.HasMember("path") && value["path"].IsString())
            {
                const auto wide_path = utils::string::convert(value["path"].GetString());
                if (!parts::ordinary_path(wide_path)) return;
                const auto result = ShellExecuteW(nullptr, L"explore", wide_path.data(), nullptr, nullptr, SW_SHOWNORMAL);

                // Under Wine, "explore" may fail — fall back to "open" which Wine can route to xdg-open
                if (reinterpret_cast<INT_PTR>(result) <= 32 && utils::nt::is_wine_environment())
                {
                    ShellExecuteW(nullptr, L"open", wide_path.data(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
        });

        cef_ui.add_command("open-logs", [](const auto&, auto&)
        {
            auto wide_path = (utils::properties::get_appdata_path() / "data" / "logs").wstring();
            // With a trailing separator: Wine's ShellExecute tries "<name>.exe" first, and the portable data
            // folder "show23-pc" sits beside show23-pc.exe (it started a second launcher).
            if (!wide_path.empty() && wide_path.back() != L'\\') wide_path.push_back(L'\\');
            const auto result = ShellExecuteW(nullptr, L"explore", wide_path.data(), nullptr, nullptr, SW_SHOWNORMAL);

            if (reinterpret_cast<INT_PTR>(result) <= 32 && utils::nt::is_wine_environment())
            {
                ShellExecuteW(nullptr, L"open", wide_path.data(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        });

        cef_ui.add_command("close", [&cef_ui](const auto& request, auto&)
        {
            if (commands::play_active() && (!request.IsObject() || !request.HasMember("confirmed") ||
                !request["confirmed"].IsTrue()))
            {
                cef_ui.execute_javascript("window.requestLauncherClose?.()");
                return;
            }
            // Mark closing first so the teardown resize doesn't overwrite the saved placement.
            if (auto* const window = cef_ui.get_window())
            {
                SetPropA(window, "show23_closing", reinterpret_cast<HANDLE>(1));
            }
            cef_ui.close_browser();
        });

        cef_ui.add_command("minimize", [&cef_ui](const auto&, auto&)
        {
            ShowWindow(cef_ui.get_window(), SW_MINIMIZE);
        });

        cef_ui.add_command("toggle-maximize", [&cef_ui](const auto&, auto&)
        {
            auto* const window = cef_ui.get_window();
            ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
        });

        cef_ui.add_command("is-maximized", [&cef_ui](const auto&, rapidjson::Document& response)
        {
            response.SetBool(IsZoomed(cef_ui.get_window()) != FALSE);
        });

        cef_ui.add_command("show", [&cef_ui](const auto&, auto&)
        {
            auto* const window = cef_ui.get_window();

            // Preserve the window's current state. SW_SHOWDEFAULT would un-maximize a
            // window that was restored maximized on launch, and clobber the saved flag.
            if (IsIconic(window))
            {
                ShowWindow(window, SW_RESTORE);
            }
            else if (IsZoomed(window))
            {
                ShowWindow(window, SW_SHOWMAXIMIZED);
            }
            else
            {
                ShowWindow(window, SW_SHOWNORMAL);
            }

            SetForegroundWindow(window);

            PostMessageA(window, WM_DELAYEDDPICHANGE, 0, 0);

        });

        // Copies the live data folder to the other root, skipping self-rebuilding caches, then
        // flips the marker and restarts. Property values ride along, so "desired" travels with the data.
        cef_ui.add_command("switch-portable", [](const rapidjson::Value& request, rapidjson::Document& response)
        {
            response.SetObject();
            auto& allocator = response.GetAllocator();
            const auto set_error = [&](const char* code)
            {
                response.AddMember("success", false, allocator);
                response.AddMember("error", rapidjson::StringRef(code), allocator);
            };

            if (commands::play_active() || commands::card_import_active()) { set_error("busy"); return; }
            const bool to_portable = request.IsObject() && request.HasMember("portable") &&
                                     request["portable"].IsBool() && request["portable"].GetBool();
            if (to_portable == utils::properties::is_portable())
            {
                set_error("no-change");
                return;
            }

            const auto src = utils::properties::get_appdata_path();
            const auto dst = to_portable ? utils::properties::get_portable_root() : utils::properties::get_local_root();
            const auto portable_marker = utils::properties::get_portable_marker();
            const auto move_marker = utils::properties::get_move_marker(dst);

            static const std::vector<std::filesystem::path> skipped = {
                std::filesystem::path("user") / "cef-data",
                std::filesystem::path("updates"), // the self-update's downloads (portable installs only)
            };

            try
            {
                std::error_code ec;
                std::filesystem::create_directories(dst, ec);
                if (ec)
                {
                    set_error("not-writable");
                    return;
                }

                for (auto it = std::filesystem::recursive_directory_iterator(src); it != std::filesystem::recursive_directory_iterator(); ++it)
                {
                    const auto rel = std::filesystem::relative(it->path(), src);
                    const bool skip = rel.filename() == portable_marker.filename() || rel.filename() == move_marker.filename() ||
                                      rel.extension() == ".log" ||
                                      std::find(skipped.begin(), skipped.end(), rel) != skipped.end();
                    if (skip)
                    {
                        if (it->is_directory()) it.disable_recursion_pending();
                        continue;
                    }

                    const auto target = dst / rel;
                    if (it->is_directory())
                    {
                        std::filesystem::create_directories(target);
                    }
                    else
                    {
                        std::filesystem::create_directories(target.parent_path());
                        std::filesystem::copy_file(it->path(), target, std::filesystem::copy_options::overwrite_existing);
                    }
                }

                if (to_portable)
                {
                    if (!utils::io::write_file(portable_marker, "")) throw std::runtime_error("marker");
                }
                else
                {
                    utils::io::remove_file(portable_marker);
                    if (std::filesystem::exists(portable_marker, ec)) throw std::runtime_error("marker");
                }

                // Only once the switch is certain, so a failed switch never schedules deleting live data.
                utils::io::write_file(move_marker, "");
            }
            catch (const std::exception& e)
            {
                printf("switch-portable failed: %s\n", e.what());
                set_error("copy-failed");
                return;
            }

            response.AddMember("success", true, allocator);
            // Preserve command-line offline mode when changing the data location.
            utils::nt::relaunch_self(utils::flags::has_flag("offline") ? "-offline" : "");
            utils::nt::terminate();
        });

        // Fresh process with no flags; the stored offline toggle decides the new mode.
        cef_ui.add_command("relaunch", [](const auto&, rapidjson::Document& response)
        {
            if (commands::play_active() || commands::card_import_active()) { response.SetBool(false); return; }
            response.SetBool(true);
            utils::nt::relaunch_self("");
            utils::nt::terminate();
        });

        cef_ui.add_command("relaunch-online", [](const auto&, rapidjson::Document& response)
        {
            if (commands::play_active() || commands::card_import_active()) { response.SetBool(false); return; }
            response.SetBool(true);
            utils::properties::store(property_keys::OFFLINE_MODE, "false");
            utils::nt::relaunch_self(""); // empty args drops -offline
            utils::nt::terminate();
        });

    }
}
