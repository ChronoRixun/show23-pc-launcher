#pragma once

#include "sfo.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace show23
{
    // Reads APP_VER from the installed parameter metadata. An empty result means unknown.
    inline std::string parse_app_version(const std::string_view bytes)
    {
        const auto metadata = read_sfo(bytes);
        if (!metadata) return {};
        auto value = std::string_view(metadata->version);
        const auto dot = value.find('.');
        if (dot == value.npos || dot == 0 || dot > 3 || value.size() - dot != 3) return {};
        const auto decimal = [](char c) { return c >= '0' && c <= '9'; };
        if (!std::ranges::all_of(value.substr(0, dot), decimal) ||
            !std::ranges::all_of(value.substr(dot + 1), decimal)) return {};
        while (value.size() > 4 && value.front() == '0') value.remove_prefix(1);
        return std::string(value);
    }
}
