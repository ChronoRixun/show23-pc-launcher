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

        static std::string inventory_for(std::string_view launcher)
        {
            return versions::encode(versions::inventory{"0.1.4", "stable", 1,
                {{versions::launcher_exe, versions::sha256(launcher), launcher.size()}}, {}});
        }

        void complete(std::string_view name, std::string_view launcher = "exe")
        {
            const auto folder = std::string(name) + '/';
            write(folder + versions::launcher_exe, launcher);
            write(folder + "version.txt", inventory_for(launcher));
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

        fs::path launcher_zip()
        {
            const std::string slot = "show23-pc-0.1.4-win64-native-portable/show23-pc/versions/0.1.4/";
            return zip({{slot + versions::launcher_exe, "exe"}, {slot + "version.txt", inventory_for("exe")}});
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
    const std::string dll = "runtime/0.1.4/emulator/libc++.dll";
    const versions::inventory inventory{"0.1.4", "stable", 1,
        {{versions::launcher_exe, versions::sha256("exe"), 3}, {dll, versions::sha256("dll"), 3}}, {}};
    const std::string prefix = "show23-pc-0.1.4-win64-native-portable/";
    const auto slot = prefix + "show23-pc/versions/0.1.4/";
    const auto zip = fixture.zip({{prefix + "manual/player.txt", "new"}, {prefix + "show23-pc.exe", "new"},
        {slot + versions::launcher_exe, "exe"}, {slot + dll, "dll"}, {slot + "version.txt", versions::encode(inventory)}});

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
    CHECK(versions::read_record(fixture.root / "0.1.4~2" / dll) == "dll");
    CHECK(versions::read_record(fixture.root / "0.1.4~2.launcher.json") == "receipt");
    CHECK(versions::read_record(fixture.root / "0.1.4/show23-pc-launcher.exe") == "old");
    CHECK(versions::read_record(fixture.root / "manual/player.txt") == "keep");
    CHECK(!fs::exists(fixture.root / "0.1.4.partial"));
}

TEST(versioned_updates_reject_an_unsafe_file_in_the_selected_subtree)
{
    update_fixture fixture;
    const std::string slot = "show23-pc-0.1.4-win64-native-portable/show23-pc/versions/0.1.4/";
    const auto zip = fixture.zip({{slot + "../user.txt", "injected"}});
    update::partial_slot partial(fixture.root, "0.1.4");
    CHECK(!partial.extract(zip, [] { return false; }).empty());
    CHECK(!fs::exists(fixture.root / "user.txt"));
    CHECK(!fs::exists(fixture.root / "0.1.4.partial"));
}

TEST(versioned_updates_preserve_different_valid_same_tag_bytes_and_their_receipt)
{
    update_fixture fixture;
    fixture.complete("0.1.4", "old");
    fixture.write("0.1.4.launcher.json", "original receipt");
    fixture.write(".download-12/launcher.json", "new receipt");
    CHECK(versions::verify(fixture.root / "0.1.4", versions::check_mode::full));

    update::partial_slot partial(fixture.root, "0.1.4");
    CHECK(partial.extract(fixture.launcher_zip(), [] { return false; }).empty());
    std::string folder;
    versions::pointer arrival;
    arrival.current = "0.1.3";
    arrival.trial = "0.1.4";
    arrival.starts = 1;
    CHECK(partial.publish(fixture.root / ".download-12/launcher.json", &arrival, folder).empty());
    CHECK(folder == "0.1.4~2");
    const auto published = versions::verify(fixture.root / "0.1.4~2", versions::check_mode::full);
    CHECK(published);
    CHECK(versions::read_record(fixture.root / "0.1.4~2.arrival.txt") == published.fingerprint + '\n');
    CHECK(versions::read_record(fixture.root / "0.1.4~2/show23-pc-launcher.exe") == "exe");
    CHECK(versions::read_record(fixture.root / "0.1.4~2.launcher.json") == "new receipt");
    CHECK(versions::read_record(fixture.root / "0.1.4/show23-pc-launcher.exe") == "old");
    CHECK(versions::read_record(fixture.root / "0.1.4.launcher.json") == "original receipt");
}

TEST(versioned_updates_reuse_a_verified_same_tag_slot_matching_the_download)
{
    update_fixture fixture;
    fixture.complete("0.1.4", "old");
    fixture.complete("0.1.4~2");
    fixture.write(".download-12/launcher.json", "new receipt");

    std::string folder;
    {
        update::partial_slot partial(fixture.root, "0.1.4");
        CHECK(partial.extract(fixture.launcher_zip(), [] { return false; }).empty());
        versions::pointer arrival;
        arrival.current = "0.1.3";
        CHECK(partial.publish(fixture.root / ".download-12/launcher.json", &arrival, folder).empty());
    }
    CHECK(folder == "0.1.4~2");
    CHECK(versions::read_record(fixture.root / "0.1.4~2.launcher.json") == "new receipt");
    const auto reused = versions::verify(fixture.root / "0.1.4~2", versions::check_mode::full);
    CHECK(reused);
    CHECK(versions::read_record(fixture.root / "0.1.4~2.arrival.txt") == reused.fingerprint + '\n');
    CHECK(!fs::exists(fixture.root / "0.1.4.launcher.json"));
    CHECK(!fs::exists(fixture.root / "0.1.4~3"));
    CHECK(!fs::exists(fixture.root / "0.1.4.partial"));
}

TEST(versioned_updates_publish_above_an_eligible_conflicting_suffix)
{
    update_fixture fixture;
    fixture.complete("0.1.4");
    fixture.complete("0.1.4~4", "old");
    fixture.write("0.1.4~4.launcher.json", "original receipt");
    fixture.write(".download-12/launcher.json", "new receipt");

    update::partial_slot partial(fixture.root, "0.1.4");
    CHECK(partial.extract(fixture.launcher_zip(), [] { return false; }).empty());
    versions::pointer arrival;
    arrival.current = "0.1.3";
    std::string folder;
    CHECK(partial.publish(fixture.root / ".download-12/launcher.json", &arrival, folder).empty());
    CHECK(folder == "0.1.4~5");
    CHECK(versions::verify(fixture.root / "0.1.4~5", versions::check_mode::full));
    CHECK(versions::read_record(fixture.root / "0.1.4~5.launcher.json") == "new receipt");
    CHECK(versions::read_record(fixture.root / "0.1.4~4.launcher.json") == "original receipt");
    CHECK(!fs::exists(fixture.root / "0.1.4.launcher.json"));
}

TEST(versioned_updates_do_not_reuse_a_previous_slot_for_implicit_arrival)
{
    update_fixture fixture;
    fixture.complete("0.1.4");
    fixture.write(".download-12/launcher.json", "new receipt");

    update::partial_slot partial(fixture.root, "0.1.4");
    CHECK(partial.extract(fixture.launcher_zip(), [] { return false; }).empty());
    versions::pointer arrival;
    arrival.current = "0.1.3";
    arrival.previous = "0.1.4";
    std::string folder;
    CHECK(partial.publish(fixture.root / ".download-12/launcher.json", &arrival, folder).empty());
    CHECK(folder == "0.1.4~2");
    CHECK(versions::read_record(fixture.root / "0.1.4~2.launcher.json") == "new receipt");
    CHECK(!fs::exists(fixture.root / "0.1.4.launcher.json"));
}

TEST(versioned_updates_preserve_the_retry_gate_for_identical_failed_bytes)
{
    update_fixture fixture;
    fixture.complete("0.1.4");
    fixture.write("0.1.4.launcher.json", "original receipt");
    fixture.write(".download-12/launcher.json", "new receipt");

    update::partial_slot partial(fixture.root, "0.1.4");
    CHECK(partial.extract(fixture.launcher_zip(), [] { return false; }).empty());
    versions::pointer arrival;
    arrival.current = "0.1.3";
    const auto failed = versions::verify(fixture.root / "0.1.4", versions::check_mode::full);
    arrival.failed.push_back({"0.1.4", failed.fingerprint, "failed to start"});
    std::string folder;
    CHECK(!partial.publish(fixture.root / ".download-12/launcher.json", &arrival, folder).empty());
    CHECK(folder.empty());
    CHECK(!fs::exists(fixture.root / "0.1.4~2"));
    CHECK(versions::read_record(fixture.root / "0.1.4.launcher.json") == "original receipt");
}

TEST(versioned_update_requests_wait_for_the_primary_to_select_the_running_fallback)
{
    update_fixture fixture;
    const versions::request request{"a1389f39d3b84ece", "0.1.3", "switch-to-stable"};
    const auto request_file = fixture.root / "request.txt";
    versions::pointer stale;
    stale.current = "0.1.4-beta.2";
    stale.previous = "0.1.4-beta.1";
    fixture.write("pointer.txt", versions::encode(stale));
    fixture.write("pointer.bak", versions::encode(stale));
    CHECK(update::try_request(fixture.root, stale.previous, request) == update::request_write::pending);
    CHECK(!fs::exists(request_file));

    versions::pointer repaired;
    repaired.current = stale.previous;
    fixture.write("pointer.bak", versions::encode(repaired));
    const auto primary = fixture.root / "pointer.txt";
    const auto lock = CreateFileW(primary.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    const auto pending = update::try_request(fixture.root, repaired.current, request);
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    CHECK(pending == update::request_write::pending);
    CHECK(!fs::exists(request_file));

    fixture.write("pointer.txt", versions::encode(repaired));
    CHECK(update::try_request(fixture.root, repaired.current, request) == update::request_write::saved);
    CHECK(versions::parse_request(versions::read_record(request_file)) == request);
}

TEST(versioned_update_requests_retry_when_windows_prevents_replacing_the_request)
{
    update_fixture fixture;
    versions::pointer state;
    state.current = "0.1.4-beta.1";
    fixture.write("pointer.txt", versions::encode(state));
    const versions::request old{"9f914a2f32cb60dd", "0.1.2", "switch-to-stable"};
    const versions::request request{"a1389f39d3b84ece", "0.1.3", "switch-to-stable"};
    fixture.write("request.txt", versions::encode(old));
    const auto request_file = fixture.root / "request.txt";
    const auto lock = CreateFileW(request_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    const auto pending = update::try_request(fixture.root, state.current, request);
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    CHECK(pending == update::request_write::pending);
    CHECK(versions::parse_request(versions::read_record(request_file)) == old);
    CHECK(update::try_request(fixture.root, state.current, request) == update::request_write::saved);
    CHECK(versions::parse_request(versions::read_record(request_file)) == request);
}

TEST(versioned_update_retry_requires_a_failure_for_the_verified_inventory)
{
    update_fixture fixture;
    versions::pointer state;
    state.current = "0.1.4-beta.1";
    const versions::request request{"a1389f39d3b84ece", "0.1.4-beta.2", "retry"};
    state.failed.push_back({request.trial, std::string(64, '1'), "failed to start"});
    fixture.write("pointer.txt", versions::encode(state));
    CHECK(update::try_request(fixture.root, state.current, request, std::string(64, '2')) ==
        update::request_write::rejected);
    CHECK(!fs::exists(fixture.root / "request.txt"));
    CHECK(update::try_request(fixture.root, state.current, request, state.failed.front().fingerprint) ==
        update::request_write::saved);
}

TEST(versioned_update_deadline_defers_a_valid_choice_without_writing_the_pointer)
{
    update_fixture fixture;
    versions::pointer stale;
    stale.current = "0.1.4-beta.2";
    stale.previous = "0.1.4-beta.1";
    const auto fingerprint = std::string(64, '1');
    const versions::request retry{"0123456789abcdef", "0.1.4-beta.3", "retry"};
    stale.failed.push_back({retry.trial, fingerprint, "failed to start"});
    const auto pointer = versions::encode(stale);
    fixture.write("pointer.txt", pointer);
    fixture.write("pointer.bak", pointer);

    const versions::request choice{"a1389f39d3b84ece", "0.1.3", "switch-to-stable"};
    const auto primary = fixture.root / "pointer.txt";
    const auto held = CreateFileW(primary.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    if (held == INVALID_HANDLE_VALUE) return;
    const auto file = fixture.root / "request.txt";
    CHECK(update::try_request(fixture.root, stale.previous, choice) == update::request_write::pending);
    CHECK(!fs::exists(file));
    CHECK(update::try_request(fixture.root, stale.previous, choice, {}, true) == update::request_write::deferred);
    CHECK(versions::parse_request(versions::read_record(file)) == choice);
    CHECK(versions::read_record(primary) == pointer);
    CHECK(versions::read_record(fixture.root / "pointer.bak") == pointer);

    const auto held_request = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(held_request != INVALID_HANDLE_VALUE);
    if (held_request != INVALID_HANDLE_VALUE)
    {
        auto later = choice;
        later.id = "9f914a2f32cb60dd";
        CHECK(update::try_request(fixture.root, stale.previous, later, {}, true) == update::request_write::pending);
        CHECK(versions::parse_request(versions::read_record(file)) == choice);
        CloseHandle(held_request);
    }
    CHECK(update::try_request(fixture.root, stale.previous, retry, std::string(64, '2'), true) ==
        update::request_write::rejected);
    CHECK(versions::parse_request(versions::read_record(file)) == choice);
    CHECK(update::try_request(fixture.root, stale.previous, retry, fingerprint, true) ==
        update::request_write::deferred);
    CHECK(versions::parse_request(versions::read_record(file)) == retry);
    CHECK(versions::read_record(primary) == pointer);
    CloseHandle(held);
}
