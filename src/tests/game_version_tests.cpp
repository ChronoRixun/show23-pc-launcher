#include "std_include.hpp"
#include "test.hpp"
#include "show23/game_version.hpp"

namespace
{
    std::string metadata(std::string_view value)
    {
        // One string key in a minimal little-endian parameter file.
        std::string bytes(44, '\0');
        bytes.replace(0, 4, "\0PSF", 4);
        bytes[8] = 36;
        bytes[12] = 44;
        bytes[16] = 1;
        bytes[22] = 4;
        bytes[23] = 2;
        bytes[24] = static_cast<char>(value.size() + 1);
        bytes[28] = 16;
        bytes.replace(36, 7, "APP_VER");
        bytes.append(value);
        bytes.push_back('\0');
        return bytes;
    }
}

TEST(game_version_preserves_the_minor_digits_and_strips_major_padding)
{
    CHECK(show23::parse_app_version(metadata("01.07")) == "1.07");
    CHECK(show23::parse_app_version(metadata("02.23")) == "2.23");
    CHECK(show23::parse_app_version(metadata("1.00")) == "1.00");
}

TEST(game_version_rejects_invalid_metadata)
{
    auto bytes = metadata("01.21");
    bytes[8] = 4;
    CHECK(show23::parse_app_version(bytes).empty());
    bytes = metadata("01.21");
    bytes[32] = 90;
    CHECK(show23::parse_app_version(bytes).empty());
    bytes = metadata("01.21");
    bytes.pop_back();
    CHECK(show23::parse_app_version(bytes).empty());
    CHECK(show23::parse_app_version(metadata("01.xy")).empty());
    CHECK(show23::parse_app_version(metadata("01.2")).empty());
    CHECK(show23::parse_app_version("unknown").empty());
}

TEST(game_version_only_reads_app_ver)
{
    auto bytes = metadata("01.21");
    bytes.replace(36, 7, "VERSION");
    CHECK(show23::parse_app_version(bytes).empty());
}
