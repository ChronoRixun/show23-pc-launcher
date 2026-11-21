#pragma once

#include "setup_verification.hpp"

namespace show23
{
    enum class record_state { absent, invalid, ready };
    struct game_presence
    {
        record_state state{record_state::absent};
        fs::path source_path;
        std::optional<game_record> record;
    };

    fs::path game_directory(const fs::path& install);
    bool installed_paths_match(const game_record& record, const fs::path& install);
    game_presence inspect_game(const fs::path& install);
    std::string encode_game(const game_record& record);
    std::optional<game_record> decode_game(std::string_view bytes, fs::path& source_path);
}
