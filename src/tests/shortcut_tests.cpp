#include "std_include.hpp"
#include "test.hpp"
#include "show23/shortcuts.hpp"

#include <Windows.h>
#include <fstream>

TEST(shortcut_startup_creates_only_absent_or_broken_links)
{
    namespace fs = std::filesystem;
    const auto root = fs::absolute("build") / ("shortcut-fixture-" + std::to_string(GetCurrentProcessId()) +
        "-" + std::to_string(GetTickCount64()));
    fs::create_directories(root);
    const auto link = root / "launcher.lnk";
    const auto here = root / "current.exe";
    const auto other = root / "other.exe";
    auto target = here;
    unsigned reads{};
    const auto read_target = [&](const fs::path& path)
    {
        CHECK(path == link);
        ++reads;
        return target;
    };
    const auto needs_update = [&](bool explicit_choice = false)
    {
        return show23::should_write_shortcut(link, explicit_choice, read_target);
    };

    CHECK(needs_update());
    CHECK(reads == 0);
    std::ofstream(here).put('1');
    std::ofstream(other).put('2');
    std::ofstream(link).put('3');
    CHECK(!needs_update());
    target = other;
    CHECK(!needs_update());
    CHECK(needs_update(true));
    CHECK(reads == 2);
    fs::remove(other);
    CHECK(needs_update());
    target.clear();
    CHECK(!needs_update());
    target = "unresolved.exe";
    CHECK(!needs_update());
    fs::remove(link);
    fs::create_directory(link);
    CHECK(!needs_update());
    CHECK(!needs_update(true));

    std::error_code ignored;
    fs::remove_all(root, ignored);
}
