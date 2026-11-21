#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace show23
{
    namespace fs = std::filesystem;
    inline constexpr auto game_id = "show23";
    inline constexpr auto title_id = "CUSA35521";

    enum class source_kind { package, folder };
    enum class status { complete, rejected, not_implemented };
    struct step_result { status state; const char* message; };

    struct setup_request
    {
        source_kind kind;
        fs::path base;
        std::optional<fs::path> update;
        fs::path install;
    };

    // check_inputs will validate title/version, available space, and source and destination paths.
    // Only on success may the caller supply the checked inputs to unpack_into_staging.
    struct checked_inputs { setup_request request; std::string game_version; };

    // Unpack will write base and optional update into these paths in sibling staging.
    struct staged_game { fs::path base; std::optional<fs::path> update; };

    std::optional<fs::path> install_folder();
    bool is_local_path(const fs::path& path);
    step_result begin_setup(const setup_request& request);

    step_result check_inputs(const setup_request& request, checked_inputs& checked);
    step_result unpack_into_staging(const checked_inputs& checked, const fs::path& staging, staged_game& game);
    step_result verify_game(const staged_game& game, const fs::path& profile_folder);
    step_result generate_settings(const checked_inputs& checked, const staged_game& game, const fs::path& staging);
    // Adopt only verified staging. The install's manifest is published last as the ready marker.
    step_result adopt_install(const staged_game& game, const fs::path& install);
    // Future return value includes the server process and its ready loopback endpoint.
    step_result start_local_server(const fs::path& install, const fs::path& server_folder);
    // Future return value includes an emulator process handle for coordinated shutdown.
    step_result start_emulator(const fs::path& install, const fs::path& emulator_folder);
}
