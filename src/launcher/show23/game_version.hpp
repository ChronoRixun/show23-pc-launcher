#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace show23
{
    // Reads APP_VER from the installed parameter metadata. An empty result means unknown.
    inline std::string parse_app_version(const std::string_view bytes)
    {
        constexpr std::size_t header_size = 20;
        constexpr std::size_t entry_size = 16;
        if (bytes.size() < header_size || !bytes.starts_with(std::string_view("\0PSF", 4))) return {};

        const auto little = [bytes](std::size_t offset, std::size_t width)
        {
            std::uint32_t value = 0;
            for (std::size_t i = 0; i < width; ++i)
                value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
            return value;
        };

        const std::size_t key_offset = little(8, 4);
        const std::size_t data_offset = little(12, 4);
        const std::size_t entries = little(16, 4);
        if (entries > (bytes.size() - header_size) / entry_size ||
            key_offset < header_size + entries * entry_size ||
            data_offset < key_offset || data_offset > bytes.size()) return {};

        const auto keys = bytes.substr(key_offset, data_offset - key_offset);
        const auto data = bytes.substr(data_offset);
        for (std::size_t i = 0; i < entries; ++i)
        {
            const auto entry = header_size + i * entry_size;
            const std::size_t key = little(entry, 2);
            if (key >= keys.size()) return {};
            const auto end = keys.find('\0', key);
            if (end == keys.npos) return {};
            if (keys.substr(key, end - key) != "APP_VER") continue;

            const std::size_t size = little(entry + 4, 4);
            const std::size_t max_size = little(entry + 8, 4);
            const std::size_t start = little(entry + 12, 4);
            if (little(entry + 2, 2) != 0x0204 || size < 2 || size > max_size ||
                start > data.size() || size > data.size() - start) return {};

            auto value = data.substr(start, size);
            const auto terminator = value.find('\0');
            if (terminator == value.npos) return {};
            value = value.substr(0, terminator);
            const auto dot = value.find('.');
            if (dot == value.npos || dot == 0 || dot > 3 || value.size() - dot != 3) return {};
            const auto decimal = [](char c) { return c >= '0' && c <= '9'; };
            if (!std::ranges::all_of(value.substr(0, dot), decimal) ||
                !std::ranges::all_of(value.substr(dot + 1), decimal)) return {};
            while (value.size() > 4 && value.front() == '0') value.remove_prefix(1);
            return std::string(value);
        }
        return {};
    }
}
