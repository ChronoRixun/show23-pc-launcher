#include "test.hpp"
#include "updater/versioned_update_files.hpp"

#include <versions/records.hpp>
#include <Windows.h>
#include <miniz.h>

#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{
    namespace fs = std::filesystem;
    namespace update = versioned_update_files;

    struct update_fixture
    {
        fs::path root = fs::temp_directory_path() / ("show23-update-" + versions::request_id());

        update_fixture()
        {
            if (!fs::create_directory(root)) throw std::runtime_error("Could not create update fixture.");
        }

        ~update_fixture()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }

        void write(std::string_view name, std::string_view data)
        {
            const auto path = root / name;
            fs::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary);
            file << data;
            if (!file) throw std::runtime_error("Could not write update fixture.");
        }

        void complete(std::string_view name)
        {
            const auto folder = std::string(name) + '/';
            const versions::inventory inventory{"0.1.4", "stable", 1,
                {{versions::launcher_exe, versions::sha256("exe"), 3}}, {}};
            write(folder + versions::launcher_exe, "exe");
            write(folder + "version.txt", versions::encode(inventory));
        }

        fs::path zip(std::initializer_list<std::pair<std::string, std::string>> entries)
        {
            const auto path = root / ".download-12/archive.zip";
            fs::create_directories(path.parent_path());
            FILE* file{};
            if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file)
                throw std::runtime_error("Could not create fixture ZIP.");
            mz_zip_archive archive{};
            bool good = mz_zip_writer_init_cfile(&archive, file, 0);
            if (good)
            {
                for (const auto& [name, bytes] : entries)
                    good = good && mz_zip_writer_add_mem(&archive, name.c_str(), bytes.data(), bytes.size(), 0);
                good = good && mz_zip_writer_finalize_archive(&archive);
                mz_zip_writer_end(&archive);
            }
            fclose(file);
            if (!good) throw std::runtime_error("Could not write fixture ZIP.");
            return path;
        }
    };
}

TEST(versioned_updates_include_archive_expansion_and_round_up_disk_margin)
{
    CHECK(!update::has_space(110, 60, 41));
    CHECK(!update::has_space(111, 60, 41));
    CHECK(update::has_space(112, 60, 41));
    CHECK(!update::has_space(std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(), 1));
}

TEST(versioned_updates_reuse_only_fully_verified_slots)
{
    update_fixture fixture;
    fixture.complete("0.1.4");
    fixture.complete("0.1.4~2");
    CHECK(update::verified_slot(fixture.root, "0.1.4") == "0.1.4");
    fixture.write("0.1.4/show23-pc-launcher.exe", "bad");
    CHECK(update::verified_slot(fixture.root, "0.1.4") == "0.1.4~2");
    fixture.write("0.1.4~2/extra.txt", "unlisted");
    CHECK(!update::verified_slot(fixture.root, "0.1.4"));
}

TEST(versioned_updates_clean_only_idle_recognized_work_directories)
{
    update_fixture fixture;
    fixture.complete("0.1.4");
    fixture.write("0.1.5.partial/.writing", "");
    fixture.write("0.1.6.partial/.writing", "");
    fixture.write(".download-12/archive.zip", "partial bytes");
    fixture.write(".download-personal/keep.txt", "keep");
    fixture.write("mine.partial/keep.txt", "keep");
    const auto marker = fixture.root / "0.1.6.partial/.writing";
    const auto handle = CreateFileW(marker.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(handle != INVALID_HANDLE_VALUE);
    update::cleanup(fixture.root);
    CHECK(fs::exists(marker));
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    CHECK(!fs::exists(fixture.root / "0.1.5.partial"));
    CHECK(!fs::exists(fixture.root / ".download-12"));
    CHECK(fs::exists(fixture.root / "0.1.4/version.txt"));
    CHECK(fs::exists(fixture.root / ".download-personal/keep.txt"));
    CHECK(fs::exists(fixture.root / "mine.partial/keep.txt"));
    update::cleanup(fixture.root);
    CHECK(!fs::exists(marker));
}

TEST(versioned_updates_publish_only_the_verified_subtree_beside_a_damaged_slot)
{
    update_fixture fixture;
    fixture.write("0.1.4/show23-pc-launcher.exe", "old");
    fixture.write("0.1.4~/sentinel", "outside");
    fixture.write("manual/player.txt", "keep");
    fixture.write(".download-12/launcher.json", "receipt");
    const versions::inventory inventory{"0.1.4", "stable", 1,
        {{versions::launcher_exe, versions::sha256("exe"), 3}}, {}};
    const std::string prefix = "show23-pc-0.1.4-win64-portable/";
    const auto slot = prefix + "show23-pc/versions/0.1.4/";
    const auto zip = fixture.zip({{prefix + "manual/player.txt", "new"}, {prefix + "show23-pc.exe", "new"},
        {slot + versions::launcher_exe, "exe"}, {slot + "version.txt", versions::encode(inventory)}});

    update::partial_slot partial(fixture.root, "0.1.4");
    CHECK(partial.extract(zip, [] { return false; }).empty());
    CHECK(fs::exists(fixture.root / "0.1.4.partial/.writing"));
    update::remove_transient(fixture.root / "0.1.4.partial");
    CHECK(fs::exists(fixture.root / "0.1.4.partial/.writing"));
    CHECK(!fs::exists(fixture.root / "0.1.4.partial/manual/player.txt"));
    CHECK(!fs::exists(fixture.root / "0.1.4.partial/show23-pc.exe"));

    std::string folder;
    CHECK(partial.publish(fixture.root / ".download-12/launcher.json", {}, folder).empty());
    CHECK(folder == "0.1.4~2");
    CHECK(versions::verify(fixture.root / "0.1.4~2", versions::check_mode::full));
    CHECK(versions::read_record(fixture.root / "0.1.4~2.launcher.json") == "receipt");
    CHECK(versions::read_record(fixture.root / "0.1.4/show23-pc-launcher.exe") == "old");
    CHECK(versions::read_record(fixture.root / "manual/player.txt") == "keep");
    CHECK(!fs::exists(fixture.root / "0.1.4.partial"));
}

TEST(versioned_updates_reject_an_unsafe_file_in_the_selected_subtree)
{
    update_fixture fixture;
    const std::string slot = "show23-pc-0.1.4-win64-portable/show23-pc/versions/0.1.4/";
    const auto zip = fixture.zip({{slot + "../user.txt", "injected"}});
    update::partial_slot partial(fixture.root, "0.1.4");
    CHECK(!partial.extract(zip, [] { return false; }).empty());
    CHECK(!fs::exists(fixture.root / "user.txt"));
    CHECK(!fs::exists(fixture.root / "0.1.4.partial"));
}
