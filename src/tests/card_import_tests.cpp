#include "std_include.hpp"
#include "test.hpp"
#include "show23/card_import.hpp"

#include <Windows.h>
#include <fstream>

using namespace show23;

TEST(card_import_accepts_only_well_formed_progress_lines)
{
    card_progress progress;
    CHECK(parse_card_progress("import: page 17/165\r", progress));
    CHECK(progress.page == 17 && progress.total == 165);
    for (const auto line : {"import: page 0/165", "import: page 166/165", "import: page 17/0",
        "import: page 999999999999/1", "import: page 19/165 extra", "import: done -1 cards", "unrelated"})
        CHECK(!parse_card_progress(line, progress));
    CHECK(progress.page == 17 && progress.total == 165);
    CHECK(parse_card_progress("import: done 4101 cards", progress));
    CHECK(progress.cards == 4101);
}

TEST(card_import_status_prefers_newest_complete_valid_manifest)
{
    const auto root = fs::absolute("build") / ("card-status-fixture-" + std::to_string(GetCurrentProcessId()) +
        "-" + std::to_string(GetTickCount64()));
    const auto cache = root / "data/import-cache";
    const auto write = [&](const char* day, const char* payload)
    {
        const auto folder = cache / day;
        fs::create_directories(folder);
        std::ofstream(folder / "manifest.json") << payload;
    };
    CHECK(!read_card_status(root));
    write("2026-11-21", R"({"complete":true,"cards":4101,"date":"2026-11-21","pages":165})");
    write("2026-11-22", R"({"complete":false,"cards":5000,"date":"2026-11-22"})");
    write("2026-11-23", R"({"complete":true,"cards":-3,"date":"2026-11-23"})");
    write("2026-11-24", R"({"complete":true,"cards":12,"date":"2026-11-25"})");
    write("2026-11-31", R"({"complete":true,"cards":12,"date":"2026-11-31"})");
    fs::create_directories(cache / "2026-12-01");
    auto status = read_card_status(root);
    CHECK(status && status->cards == 4101 && status->date == "2026-11-21");
    write("2026-12-01", R"({"complete":true,"cards":5012,"date":"2026-12-01"})");
    status = read_card_status(root);
    CHECK(status && status->cards == 5012 && status->date == "2026-12-01");
    std::error_code ignored;
    fs::remove_all(root, ignored);
}
