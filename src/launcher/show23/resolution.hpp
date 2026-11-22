#pragma once

#include "setup.hpp"

#include <string_view>

namespace show23
{
    struct resolution
    {
        std::string_view id;
        unsigned width;
        unsigned height;
    };

    inline constexpr resolution resolutions[]{{"720p", 1280, 720}, {"1080p", 1920, 1080}};
    inline constexpr auto default_resolution = resolutions[1];

    const resolution* find_resolution(std::string_view id);
    enum class resolution_result { updated, malformed, unavailable };
    resolution_result write_resolution(const fs::path& profile, resolution choice);
}
