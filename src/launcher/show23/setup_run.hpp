#pragma once

#include "game_record.hpp"

namespace show23
{
    enum class setup_stage { check, unpack, verify, settings, finish };
    enum class stage_state { waiting, running, done, not_needed, stopped, failed };
    const char* stage_name(setup_stage stage);
    const char* stage_state_name(stage_state state);

    struct stage_report
    {
        stage_state state{};
        std::vector<finding> findings;
    };

    struct unpack_progress
    {
        std::uint64_t elapsed{}, written{}, expected{};
    };

    struct setup_report
    {
        std::array<stage_report, 5> stages;
        std::optional<unpack_progress> extraction;
        bool can_cancel{};
        bool canceled{};
        bool finished() const { return stages.back().state == stage_state::done; }
    };

    struct setup_options
    {
        input_check_options inputs;
        folder_reference base{reference_folder(false)};
        folder_reference update{reference_folder(true)};
        std::function<bool(const fs::path&)> remember;
        fs::path bundled_unpackers;
        std::string trusted_unpacker;
        std::function<bool(const std::string&)> remember_unpacker;
        std::function<bool()> cancel;
        bool require_preview{};
    };

    using setup_progress = std::function<void(const setup_report&)>;
    setup_options default_setup_options();
    bool remove_game_copy(const fs::path& install);
    setup_report run_setup(const setup_request& request, const setup_options& options, const setup_progress& progress = {});
}
