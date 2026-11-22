#include "test.hpp"

#include <versions/records.hpp>
#include <Windows.h>

#include <chrono>
#include <fstream>
#include <stdexcept>

namespace
{
    using namespace versions;
    constexpr char slot[] = "0.1.3";

    struct metadata_fixture
    {
        fs::path root = fs::temp_directory_path() / ("show23-metadata-" + request_id());
        fs::path folder = root / slot;
        fs::path payload = folder / "ui/main.html";

        metadata_fixture()
        {
            if (!fs::create_directories(payload.parent_path())) throw std::runtime_error("Could not create metadata fixture.");
            write(folder / launcher_exe, "exe");
            write(payload, "abc");
            write(folder / "version.txt", encode(inventory{slot, "stable", 1,
                {{launcher_exe, sha256("exe"), 3}, {"ui/main.html", sha256("abc"), 3}}, {}}));
        }

        ~metadata_fixture()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }

        std::string fingerprint() const
        {
            const auto manifest = parse_inventory(read_record(folder / "version.txt"));
            if (!manifest) throw std::runtime_error("Fixture inventory was invalid.");
            return manifest->fingerprint;
        }

        bool ntfs() const
        {
            wchar_t name[MAX_PATH]{};
            return GetVolumeInformationW(root.root_path().c_str(), nullptr, 0, nullptr, nullptr,
                nullptr, name, MAX_PATH) && std::wstring_view(name) == L"NTFS";
        }

        static void write(const fs::path& path, std::string_view bytes)
        {
            std::ofstream stream(path, std::ios::binary);
            if (!stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size())))
                throw std::runtime_error("Could not write metadata fixture file.");
        }
    };
}

TEST(version_confirmed_unchanged_files_use_cached_metadata)
{
    metadata_fixture f;
    const auto hash = f.fingerprint();
    CHECK(capture_confirmation(f.root, slot, hash));
    CHECK(read_confirmation(f.root, slot) == hash);
    const auto check = verify_confirmed(f.root, slot);
    CHECK(check && check.fingerprint == hash && check.used_cached_metadata == f.ntfs());
}

TEST(version_confirmation_hashes_every_inventory_file_before_writing)
{
    metadata_fixture f;
    const auto hash = f.fingerprint();
    f.write(f.payload, "bad");
    CHECK(!capture_confirmation(f.root, slot, hash));
    CHECK(read_confirmation(f.root, slot).empty());
}

TEST(version_confirmed_same_size_replacement_with_restored_write_time_is_rehashed)
{
    metadata_fixture f;
    CHECK(capture_confirmation(f.root, slot, f.fingerprint()));
    const auto time = fs::last_write_time(f.payload);
    const auto replacement = f.root / "replacement.html";
    f.write(replacement, "bad");
    fs::last_write_time(replacement, time);
    fs::remove(f.payload);
    fs::rename(replacement, f.payload);
    CHECK(fs::last_write_time(f.payload) == time);
    const auto check = verify_confirmed(f.root, slot);
    CHECK(!check && check.bad_file == "ui/main.html" && !check.retryable && !check.needs_confirmation);
}

TEST(version_confirmed_metadata_change_with_intact_bytes_refreshes_confirmation)
{
    metadata_fixture f;
    CHECK(capture_confirmation(f.root, slot, f.fingerprint()));
    fs::last_write_time(f.payload, fs::last_write_time(f.payload) - std::chrono::hours(1));
    const auto refreshed = verify_confirmed(f.root, slot);
    CHECK(refreshed && !refreshed.used_cached_metadata);
    const auto next = verify_confirmed(f.root, slot);
    CHECK(next && next.used_cached_metadata == f.ntfs());
}

TEST(version_confirmed_existing_record_requires_full_recheck)
{
    metadata_fixture f;
    CHECK(write_confirmation(f.root, slot, f.fingerprint()));
    f.write(f.payload, "bad");
    const auto check = verify_confirmed(f.root, slot);
    CHECK(!check && check.bad_file == "ui/main.html" && !check.retryable);
    f.write(f.payload, "abc");
    const auto refreshed = verify_confirmed(f.root, slot);
    CHECK(refreshed && !refreshed.used_cached_metadata);
    const auto next = verify_confirmed(f.root, slot);
    CHECK(next && next.used_cached_metadata == f.ntfs());
}

TEST(version_confirmed_read_only_check_preserves_confirmation_bytes)
{
    metadata_fixture f;
    CHECK(write_confirmation(f.root, slot, f.fingerprint()));
    const auto path = f.root / "confirmed/0.1.3.txt";
    const auto before = read_record(path);
    const auto checked = verify_confirmed(f.root, slot, false);
    CHECK(checked && !checked.used_cached_metadata);
    CHECK(read_record(path) == before);
}

TEST(version_last_confirmation_uses_capture_time_after_metadata_refresh)
{
    metadata_fixture f;
    const auto prior = f.fingerprint();
    CHECK(write_confirmation(f.root, slot, prior));
    const auto record = f.root / "confirmed/0.1.3.txt";
    fs::last_write_time(record, fs::last_write_time(record) - std::chrono::hours(1));
    const auto second = f.root / "0.1.4";
    fs::create_directory(second);
    f.write(second / launcher_exe, "exe");
    f.write(second / "version.txt", encode(inventory{"0.1.4", "stable", 1, {{launcher_exe, sha256("exe"), 3}}, {}}));
    const auto second_check = verify(second, check_mode::full);
    CHECK(second_check && capture_confirmation(f.root, "0.1.4", second_check.fingerprint));
    CHECK(verify_confirmed(f.root, slot));
    CHECK(last_confirmed(f.root) == "0.1.4");
    fs::remove_all(second);
    CHECK(last_confirmed(f.root) == slot);
}

TEST(version_confirmed_inventory_must_match_confirmation)
{
    metadata_fixture f;
    const auto prior = f.fingerprint();
    const auto absent = verify_confirmed(f.root, slot);
    CHECK(!absent && absent.needs_confirmation && absent.fingerprint == prior);
    CHECK(capture_confirmation(f.root, slot, prior));
    f.write(f.payload, "new");
    f.write(f.folder / "version.txt", encode(inventory{slot, "stable", 1,
        {{launcher_exe, sha256("exe"), 3}, {"ui/main.html", sha256("new"), 3}}, {}}));
    const auto changed = verify_confirmed(f.root, slot);
    CHECK(!changed && changed.needs_confirmation && !changed.retryable && changed.fingerprint == f.fingerprint());
    CHECK(!capture_confirmation(f.root, slot, prior));
    CHECK(read_confirmation(f.root, slot) == prior);
}

TEST(version_confirmation_unreadable_payload_is_retryable)
{
    metadata_fixture f;
    CHECK(capture_confirmation(f.root, slot, f.fingerprint()));
    const auto lock = CreateFileW(f.payload.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (lock == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not lock metadata fixture file.");
    const auto confirmed = verify_confirmed(f.root, slot);
    const auto full = verify(f.folder, check_mode::full);
    CloseHandle(lock);
    CHECK(!confirmed && confirmed.retryable && confirmed.bad_file == "ui/main.html");
    CHECK(!full && full.retryable && full.bad_file == "ui/main.html");
    CHECK(verify_confirmed(f.root, slot));
    fs::remove(f.payload);
    const auto missing = verify_confirmed(f.root, slot);
    CHECK(!missing && !missing.retryable && missing.bad_file == "ui/main.html");
}

TEST(version_confirmation_unreadable_ledger_is_retryable)
{
    metadata_fixture f;
    const auto hash = f.fingerprint();
    CHECK(capture_confirmation(f.root, slot, hash));
    const auto path = f.root / "confirmed/0.1.3.txt";
    const auto lock = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (lock == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not lock metadata fixture confirmation.");
    const auto checked = verify_confirmed(f.root, slot);
    CloseHandle(lock);
    CHECK(!checked && checked.retryable && !checked.needs_confirmation && checked.fingerprint == hash);
    CHECK(verify_confirmed(f.root, slot));
}

TEST(version_confirmed_rejects_files_added_outside_the_inventory)
{
    metadata_fixture f;
    CHECK(capture_confirmation(f.root, slot, f.fingerprint()));
    f.write(f.folder / "unexpected.dll", "extra");
    const auto checked = verify_confirmed(f.root, slot);
    CHECK(!checked && !checked.retryable && checked.bad_file == "unexpected.dll");
}
