#include "std_include.hpp"
#include "show23/player_name.hpp"
#include "test.hpp"

using namespace show23;

TEST(player_name_validation_trims_and_enforces_ascii_and_length)
{
    CHECK(normalize_player_name("  Batter 7-A_ ") == "Batter 7-A_");
    CHECK(normalize_player_name("1234567890123456").has_value());
    for (const auto* invalid : {"", "AB", "12345678901234567", "A\tBC", "Batter!", "\xc3\x89lodie"})
        CHECK(!normalize_player_name(invalid));
    CHECK(suggest_player_name(L"  _Bat!ter  ") == "_Batter");
    CHECK(suggest_player_name(L"  !  ") == "Player");
    CHECK(suggest_player_name(L"12345678901234567") == "1234567890123456");
}
