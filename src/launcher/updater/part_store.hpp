#pragma once

#include "part_manifest.hpp"

#include <filesystem>

namespace parts
{
    struct installed_runtime
    {
        std::filesystem::path root;
        install_set manifests;
        std::string error;
    };

    // Uses only the current runtime and its manifests. Invalid metadata fails closed.
    installed_runtime installed();
    std::filesystem::path component_folder(const installed_runtime& runtime, id part);
    bool ordinary_path(const std::filesystem::path& path, bool allow_missing = false);
    bool ready(const installed_runtime& runtime);
    std::string verify_files(const installed_runtime& runtime);
}
