#include "std_include.hpp"
#include "show23/resolution.hpp"
#include "test.hpp"

TEST(resolution_presets_match_the_engine_window_dimensions)
{
    using namespace show23;
    const auto* hd = find_resolution("720p");
    const auto* full_hd = find_resolution("1080p");
    CHECK(hd && hd->width == 1280 && hd->height == 720);
    CHECK(full_hd && full_hd->width == 1920 && full_hd->height == 1080);
    CHECK(default_resolution.id == "1080p");
    CHECK(!find_resolution("1440p"));
}
