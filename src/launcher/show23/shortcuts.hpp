#pragma once

#include <filesystem>
#include <functional>

namespace show23
{
    bool should_write_shortcut(const std::filesystem::path& shortcut, bool explicit_choice,
        const std::function<std::filesystem::path(const std::filesystem::path&)>& read_target);

    struct shortcut_result
    {
        bool desktop{};
        bool start_menu{};
    };

    shortcut_result update_shortcuts(bool explicit_choice);
}
