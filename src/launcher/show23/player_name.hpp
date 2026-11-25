#pragma once

#include "setup.hpp"

#include <string_view>

namespace show23
{
    std::optional<std::string> normalize_player_name(std::string_view name);
    std::string suggest_player_name(std::wstring_view account);
    std::string windows_player_name();
}
