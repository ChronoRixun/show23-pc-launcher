#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <rapidjson/document.h>

namespace cef { class cef_ui; }

namespace commands
{
    struct game_entry
    {
        std::string get_game_key() const;
        std::optional<std::filesystem::path> get_install_path() const;
    };

    struct command_context
    {
        std::optional<game_entry> get_game_config_from_request(const rapidjson::Value& value) const;
    };

    bool play_active();
    bool card_import_active();
    void stop_workers();
    void register_all_commands(cef::cef_ui& cef_ui);
}
