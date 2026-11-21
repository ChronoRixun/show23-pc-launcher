#pragma once

#include "setup.hpp"

namespace show23
{
    struct server_files
    {
        fs::path python;
        fs::path script;
    };

    inline server_files server_paths(const fs::path& component)
    {
        return {component / "python/python.exe", component / "olserver.py"};
    }
}
