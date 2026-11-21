#pragma once

#include "commands.hpp"

namespace commands::ui_commands
{
    // Registers: close, minimize, show, browse-folder, open-folder and settings mode changes
    void register_commands(cef::cef_ui& cef_ui, command_context& ctx);
}
