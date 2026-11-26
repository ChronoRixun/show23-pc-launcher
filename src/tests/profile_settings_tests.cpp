#include "std_include.hpp"
#include "show23/profile_settings.hpp"
#include "test.hpp"

#include <Windows.h>
#include <fstream>

TEST(profile_unlock_all_cards_is_off_by_default_and_writes_only_the_setting)
{
    using namespace show23;
    const auto install = fs::absolute("build") / ("profile-settings-fixture-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    const auto profile = install / "data/profiles/default/server";
    const auto setting = profile / "unlock-all-cards.json";
    const auto state = profile / "state.json";
    const auto read = [](const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), {});
    };

    CHECK(!profile_settings_available(install));
    CHECK(!write_unlock_all_cards(install, true));
    fs::create_directories(profile);
    std::ofstream(state, std::ios::binary) << "{\"profile\":{\"stubs\":23}}";
    const auto before = read(state);
    CHECK(profile_settings_available(install));
    CHECK(!read_unlock_all_cards(install));
    CHECK(write_unlock_all_cards(install, true));
    CHECK(read_unlock_all_cards(install));
    CHECK(read(setting) == "{\"unlock_all_cards\":true}\n");
    CHECK(write_unlock_all_cards(install, false));
    CHECK(!read_unlock_all_cards(install));
    CHECK(read(setting) == "{\"unlock_all_cards\":false}\n");
    std::ofstream(setting, std::ios::binary) << "{\"unlock_all_cards\":\"true\"}";
    CHECK(!read_unlock_all_cards(install));
    CHECK(read(state) == before);
    std::error_code ignored;
    fs::remove_all(install, ignored);
}
