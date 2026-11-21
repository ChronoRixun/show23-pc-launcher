#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace input_fixtures
{
    inline constexpr std::string_view content = "UP9000-CUSA35521_00-MLBTHESHOW23SHIP";

    inline void put(std::string& bytes, std::size_t at, std::uint64_t value, std::size_t width, bool big = false)
    {
        for (std::size_t n = 0; n < width; ++n)
        {
            const auto shift = (big ? width - n - 1 : n) * 8;
            bytes.at(at + n) = static_cast<char>((value >> shift) & 0xff);
        }
    }

    inline std::string sfo(bool update = false, std::string title = "CUSA35521",
                           std::string version = {}, std::string id = std::string(content))
    {
        const std::vector<std::pair<std::string, std::string>> fields{
            {"TITLE_ID", std::move(title)}, {"CATEGORY", update ? "gp" : "gd"},
            {"APP_VER", version.empty() ? (update ? "01.21" : "01.00") : std::move(version)}, {"CONTENT_ID", std::move(id)}
        };
        std::string table(20 + fields.size() * 16, '\0');
        std::string keys;
        std::string values;
        table.replace(0, 4, "\0PSF", 4);
        put(table, 4, 0x101, 4);
        put(table, 8, table.size(), 4);
        put(table, 16, fields.size(), 4);
        for (std::size_t i = 0; i < fields.size(); ++i)
        {
            const auto& [key, value] = fields[i];
            const auto index = 20 + i * 16;
            put(table, index, keys.size(), 2);
            put(table, index + 2, 0x0204, 2);
            put(table, index + 4, value.size() + 1, 4);
            put(table, index + 8, value.size() + 1, 4);
            put(table, index + 12, values.size(), 4);
            keys += key;
            keys.push_back('\0');
            values += value;
            values.push_back('\0');
        }
        put(table, 12, table.size() + keys.size(), 4);
        return table + keys + values;
    }

    inline std::string package(std::string metadata = sfo())
    {
        constexpr std::size_t table = 0x440;
        constexpr std::size_t body = 0x480;
        constexpr std::size_t image_size = 17;
        const auto image = body + metadata.size();
        std::string bytes(image + image_size, '\0');
        put(bytes, 0, 0x7f434e54, 4, true);
        put(bytes, 0x10, 1, 4, true);
        put(bytes, 0x18, table, 4, true);
        put(bytes, 0x20, body, 8, true);
        put(bytes, 0x28, metadata.size(), 8, true);
        bytes.replace(0x40, content.size(), content);
        put(bytes, 0x74, 0x1a, 4, true);
        put(bytes, 0x410, image, 8, true);
        put(bytes, 0x418, image_size, 8, true);
        put(bytes, table, 0x1000, 4, true);
        put(bytes, table + 16, body, 4, true);
        put(bytes, table + 20, metadata.size(), 4, true);
        bytes.replace(body, metadata.size(), metadata);
        return bytes;
    }
}
