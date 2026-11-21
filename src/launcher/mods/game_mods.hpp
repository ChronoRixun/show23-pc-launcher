#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Content packs live under <install>/data/mods. Each pack has a pack.json file and
// load-order.txt uses + or - before a pack's folder name to record its enabled state.

namespace game_mods
{
    struct mod_info
    {
        std::string name; // folder name, UTF-8
        bool enabled{};
        std::uint64_t files{};
        std::uint64_t size{};

        // Optional display fields in pack.json: {"name", "version", "author", "description"}.
        std::string title;
        std::string version;
        std::string author;
        std::string description;
    };

    std::filesystem::path mods_folder(const std::filesystem::path& game_dir);

    // The game's mods in load order. Folders the load order doesn't list yet (e.g. copied in by
    // hand) are appended as enabled; entries whose folder is gone are dropped.
    std::vector<mod_info> list(const std::filesystem::path& game_dir);

    bool set_enabled(const std::filesystem::path& game_dir, const std::string& name, bool enabled, std::string& error);

    // `names` is the complete new order, lowest priority first.
    bool set_order(const std::filesystem::path& game_dir, const std::vector<std::string>& names, std::string& error);

    // Imports one pack from a folder or ZIP with pack.json at the root or inside one wrapper.
    std::optional<std::string> import(const std::filesystem::path& game_dir, const std::filesystem::path& source, bool is_zip, std::string& error);

    // Deletes the mod's folder and its load-order entry.
    bool uninstall(const std::filesystem::path& game_dir, const std::string& name, std::string& error);
}
