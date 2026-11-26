#pragma once

#include "setup.hpp"

namespace show23
{
    bool profile_settings_available(const fs::path& install);
    bool read_unlock_all_cards(const fs::path& install);
    bool write_unlock_all_cards(const fs::path& install, bool enabled);
}
