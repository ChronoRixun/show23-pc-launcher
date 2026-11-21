#pragma once

#include "input_checks.hpp"

#include <array>
#include <string_view>

namespace show23
{
    inline constexpr std::array directory_names{"PS4", "CROSS", "FLUX", "FRONTEND"};
    inline constexpr std::array module_names{
        "libc.prx", "libSceFace.prx", "libSceFios2.prx", "libSceHand.prx", "libSceS3DConversion.prx"};

    struct folder_reference
    {
        std::array<std::uint64_t, directory_names.size()> directories;
        std::array<executable_reference, module_names.size()> modules;
    };

    struct game_folder
    {
        fs::path path;
        std::string version;
        std::uint64_t executable_size{};
        std::string executable_sha256;
        std::array<std::uint64_t, directory_names.size()> directories{};
    };

    struct game_record
    {
        game_folder base;
        std::optional<game_folder> update;
        std::string created_utc;
    };

    const folder_reference& reference_folder(bool update);
    bool findings_pass(const std::vector<finding>& findings);
    game_folder verify_folder(const fs::path& root, bool update, const executable_reference& executable,
                              const folder_reference& reference, std::vector<finding>& findings);
    std::vector<finding> verify_recorded_game(const game_record& record);
}
