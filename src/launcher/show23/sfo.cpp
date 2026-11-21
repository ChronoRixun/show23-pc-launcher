#include "std_include.hpp"
#include "sfo.hpp"
#include "input_bytes.hpp"

#include <algorithm>
#include <array>

namespace show23
{
    std::optional<sfo_metadata> read_sfo(const std::string_view bytes)
    {
        constexpr std::size_t header_size = 20;
        constexpr std::size_t entry_size = 16;
        constexpr auto field_limit = 256;
        if (bytes.size() < header_size || bytes.size() > sfo_limit ||
            !bytes.starts_with(std::string_view("\0PSF", 4))) return std::nullopt;

        const auto key_offset = input_bytes::integer(bytes, 8, 4);
        const auto data_offset = input_bytes::integer(bytes, 12, 4);
        const auto entries = input_bytes::integer(bytes, 16, 4);
        if (entries > 4096 || key_offset < header_size + entries * entry_size ||
            data_offset < key_offset || data_offset > bytes.size()) return std::nullopt;

        const auto keys = bytes.substr(static_cast<std::size_t>(key_offset), static_cast<std::size_t>(data_offset - key_offset));
        const auto data = bytes.substr(static_cast<std::size_t>(data_offset));
        sfo_metadata result;
        const std::array<std::pair<std::string_view, std::string*>, 4> wanted{{
            {"TITLE_ID", &result.title}, {"CONTENT_ID", &result.content_id},
            {"CATEGORY", &result.category}, {"APP_VER", &result.version}
        }};
        std::array<bool, wanted.size()> seen{};
        for (std::size_t i = 0; i < entries; ++i)
        {
            const auto entry = header_size + i * entry_size;
            const auto key = static_cast<std::size_t>(input_bytes::integer(bytes, entry, 2));
            const auto end = keys.find('\0', key);
            const auto length = input_bytes::integer(bytes, entry + 4, 4);
            const auto capacity = input_bytes::integer(bytes, entry + 8, 4);
            const auto offset = input_bytes::integer(bytes, entry + 12, 4);
            const auto format = input_bytes::integer(bytes, entry + 2, 2);
            if (key >= keys.size() || end == keys.npos || key == end || length > capacity ||
                !input_bytes::range(offset, length, data.size()) ||
                (format != 0x0204 && format != 0x0004 && format != 0x0404) ||
                (format == 0x0404 && length != 4)) return std::nullopt;

            const auto name = keys.substr(key, end - key);
            const auto field = std::find_if(wanted.begin(), wanted.end(), [name](const auto& item) { return item.first == name; });
            if (field == wanted.end()) continue;

            const auto index = static_cast<std::size_t>(field - wanted.begin());
            if (seen[index] || format == 0x0404 || length == 0 || length > field_limit) return std::nullopt;
            seen[index] = true;
            auto value = data.substr(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
            if (format == 0x0204)
            {
                const auto terminator = value.find('\0');
                if (terminator == value.npos) return std::nullopt;
                value = value.substr(0, terminator);
            }
            if (!std::ranges::all_of(value, [](const unsigned char c) { return c >= 0x20 && c <= 0x7e; }))
                return std::nullopt;
            *field->second = value;
        }
        return result;
    }
}
