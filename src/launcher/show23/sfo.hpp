#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace show23
{
    inline constexpr std::size_t sfo_limit = 64 * 1024;

    struct sfo_metadata
    {
        std::string title;
        std::string content_id;
        std::string category;
        std::string version;
    };

    // Malformed data returns nullopt. An absent key is left empty so its check can report it.
    std::optional<sfo_metadata> read_sfo(std::string_view bytes);
}
