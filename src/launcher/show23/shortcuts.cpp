#include "std_include.hpp"
#include "shortcuts.hpp"
#include "versioned_start.hpp"

#include <utils/com.hpp>
#include <utils/finally.hpp>
#include <utils/properties.hpp>
#include <utils/property_keys.hpp>

namespace show23
{
    namespace fs = std::filesystem;

    bool should_write_shortcut(const fs::path& shortcut, bool explicit_choice,
        const std::function<fs::path(const fs::path&)>& read_target)
    {
        const auto absent = [](const fs::file_status& state, const std::error_code& error)
        {
            return state.type() == fs::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory);
        };

        std::error_code error;
        const auto link = fs::symlink_status(shortcut, error);
        if (absent(link, error)) return true;
        if (error || !fs::is_regular_file(link)) return false;
        if (explicit_choice) return true;

        const auto target = read_target(shortcut);
        if (!target.is_absolute()) return false;
        const auto state = fs::status(target, error);
        return absent(state, error);
    }

    shortcut_result update_shortcuts(bool explicit_choice)
    {
        if (utils::nt::is_wine_environment()) return {};
        if (!explicit_choice && utils::properties::load(property_keys::AUTO_SHORTCUTS) == "false") return {};

        // Startup and Settings call this on different threads. Shell link interfaces
        // need COM on the calling thread, including when that thread already has an apartment.
        const auto apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) return {};
        const auto uninitialize = utils::finally([apartment]
        {
            if (SUCCEEDED(apartment)) CoUninitialize();
        });

        const auto launcher = versioned_start::layout() ? versioned_start::layout().bootstrap : utils::nt::library{}.get_path();
        if (launcher.empty()) return {};
        const auto update = [&](const fs::path& folder, const char* created_key)
        {
            try
            {
                if (folder.empty()) return false;
                const auto link = folder / L"show23-pc launcher.lnk";
                if (!should_write_shortcut(link, explicit_choice, utils::com::read_shortcut_target)) return false;

                std::error_code error;
                fs::create_directories(folder, error);
                if (error || !utils::com::create_shortcut(launcher, link, "Launch show23-pc launcher"))
                    return false;

                try { utils::properties::store(created_key, "true"); }
                catch (...) {}
                return true;
            }
            catch (...) { return false; }
        };

        const auto desktop = utils::com::get_desktop_path();
        const auto programs = utils::com::get_start_menu_programs_path();
        return {update(desktop, property_keys::SHORTCUT_CREATED),
                !programs.empty() && update(programs / L"show23-pc launcher", property_keys::START_MENU_SHORTCUT_CREATED)};
    }
}
