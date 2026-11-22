#include "std_include.hpp"
#include "test.hpp"
#include "show23/input_paths.hpp"
#include "updater/part_store.hpp"
#include <Windows.h>

#include <chrono>
#include <fstream>
#include <iterator>
#include <rapidjson/writer.h>
#include <utils/finally.hpp>

namespace
{
    namespace fs = std::filesystem;
    constexpr auto digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    void write(const fs::path& path, std::string_view data)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        if (!file.write(data.data(), static_cast<std::streamsize>(data.size())))
            throw std::runtime_error("Could not write runtime fixture");
    }

    std::string read(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("Could not read runtime fixture");
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    struct fixture
    {
        fs::path root, bundle, install;
        fixture()
        {
            static unsigned serial{};
            root = fs::current_path() / "build" / ("bundle fixture " + std::to_string(GetCurrentProcessId()) +
                " " + std::to_string(GetTickCount64()) + " " + std::to_string(++serial));
            bundle = root / "source";
            install = root / "install";
            write(bundle / "manifests/runtime.txt", "0.1.0\n");
            part("emulator", "emulator", {"shadPS4.exe"});
            part("server", "server", {"python/python.exe", "olserver.py", "roster_decode.py", "card_import.py",
                "tables/ol-messages.csv", "replies/fixture.xml"});
            part("profile", "tools/profile", {"defaults.txt"});
            write(install / "data/profiles/default/user/save", "preserve");
        }
        ~fixture()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }
        void part(std::string_view name, std::string_view directory, std::initializer_list<const char*> files)
        {
            for (auto file : files) write(bundle / "runtime/0.1.0" / directory / file, "abc");
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> out(buf);
            out.StartObject();
            out.Key("schema"); out.Int(1);
            out.Key("part"); out.String(name.data(), static_cast<rapidjson::SizeType>(name.size()));
            out.Key("version"); out.String("0.1.0");
            out.Key("channel"); out.String("stable");
            out.Key("min_launcher"); out.String("0.1.0");
            out.Key("files"); out.StartArray();
            for (auto file : files)
            {
                out.StartObject();
                out.Key("path"); out.String(file);
                out.Key("size"); out.Int(3);
                out.Key("sha256"); out.String(digest);
                out.EndObject();
            }
            out.EndArray();
            out.Key("compatibility"); out.StartObject();
            for (auto other : {"launcher", "emulator", "server", "profile"})
                if (name != other) { out.Key(other); out.String("*"); }
            out.EndObject();
            out.EndObject();
            write(bundle / "manifests/0.1.0" / (std::string(name) + ".json"), buf.GetString());
        }
    };
}

TEST(bundled_runtime_stages_and_verifies_files_without_touching_user_data)
{
    fixture f;
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    std::string error;
    CHECK(parts::verify_runtime(parts::read_runtime(f.install), "0.1.0", error));
    const auto save = f.install / "data/profiles/default/user/save";
    const auto exe = f.install / "runtime/0.1.0/emulator/shadPS4.exe";
    const auto marker = f.install / "manifests/runtime.txt";
    const auto before = fs::last_write_time(exe);
    const auto before_marker = fs::last_write_time(marker);
    fs::remove_all(f.bundle);
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(fs::last_write_time(exe) == before && fs::last_write_time(marker) == before_marker);
    std::string content;
    std::ifstream(save) >> content;
    CHECK(content == "preserve");
}

TEST(bundled_runtime_leaves_damage_for_explicit_same_version_repair)
{
    fixture f;
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    const auto exe = f.install / "runtime/0.1.0/emulator/shadPS4.exe";
    const auto marker = f.install / "manifests/runtime.txt";
    const auto old_manifest = f.install / "manifests/0.1.0/emulator.json";
    const auto healthy = f.install / "runtime/0.1.0/server/olserver.py";
    const auto extra = f.install / "runtime/0.1.0/emulator/player-notes.txt";
    const auto marker_time = fs::last_write_time(marker);
    const auto manifest = read(old_manifest) + "\n";
    write(old_manifest, manifest);
    const auto manifest_time = fs::last_write_time(old_manifest);
    fs::last_write_time(healthy, fs::last_write_time(healthy) - std::chrono::hours(48));
    const auto healthy_time = fs::last_write_time(healthy);
    write(extra, "keep in slot");
    write(exe, "bad");
    const auto damaged = parts::inspect_bundled_runtime(f.bundle, f.install, "0.1.0");
    CHECK(!damaged.ready && !damaged.fresh && damaged.repairable);
    CHECK(damaged.message.find("emulator/shadPS4.exe") != std::string::npos);
    CHECK(!parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(read(exe) == "bad");
    CHECK(parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0").empty());
    CHECK(parts::inspect_bundled_runtime(f.bundle, f.install, "0.1.0").ready);
    CHECK(read(exe) == "abc");
    CHECK(fs::last_write_time(marker) == marker_time);
    CHECK(read(old_manifest) == manifest && fs::last_write_time(old_manifest) == manifest_time);
    CHECK(fs::last_write_time(healthy) == healthy_time);
    CHECK(read(extra) == "keep in slot");
    CHECK(read(f.install / "data/profiles/default/user/save") == "preserve");

    write(exe, "more damage");
    CHECK(parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0").empty());
    CHECK(read(exe) == "abc");
    CHECK(fs::last_write_time(healthy) == healthy_time);
    CHECK(read(extra) == "keep in slot");
}

TEST(bundled_runtime_keeps_completed_repairs_when_a_later_file_is_locked)
{
    for (const auto* locked_file : {"emulator/support.dll", "server/olserver.py"})
    {
        fixture f;
        f.part("emulator", "emulator", {"shadPS4.exe", "support.dll"});
        CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
        const auto slot = f.install / "runtime/0.1.0";
        const auto exe = slot / "emulator/shadPS4.exe";
        const auto target = slot / locked_file;
        const auto missing = slot / "tools/profile/defaults.txt";
        write(exe, "bad");
        write(target, "bad");
        CHECK(fs::remove(missing));

        const show23::input_paths::handle directory(CreateFileW(slot.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        CHECK(directory);
        {
            const show23::input_paths::handle lock(CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            CHECK(lock);
            CHECK(!parts::runtime_in_use(f.install));
            CHECK(parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0") == "in-use");
            CHECK(read(exe) == "abc");
            CHECK(read(target) == "bad");
            CHECK(!fs::exists(missing));
        }
        fs::last_write_time(exe, fs::last_write_time(exe) - std::chrono::hours(48));
        const auto repaired_time = fs::last_write_time(exe);
        CHECK(parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0").empty());
        CHECK(parts::inspect_bundled_runtime(f.bundle, f.install, "0.1.0").ready);
        CHECK(fs::last_write_time(exe) == repaired_time);
        CHECK(read(target) == "abc" && read(missing) == "abc");
    }
}

TEST(bundled_runtime_refuses_staging_and_repair_from_a_corrupt_bundle)
{
    fixture f;
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    const auto exe = f.install / "runtime/0.1.0/emulator/shadPS4.exe";
    write(exe, "bad");
    write(f.bundle / "runtime/0.1.0/server/python/python.exe", "bad");
    const auto other = f.root / "other";
    CHECK(!parts::stage_bundled_runtime(f.bundle, other, "0.1.0"));
    CHECK(!fs::exists(other / "manifests/runtime.txt"));
    CHECK(!parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0").empty());
    CHECK(read(exe) == "bad");
}

TEST(bundled_runtime_completes_a_markerless_slot_only_with_exact_manifests_and_files)
{
    fixture f;
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    const auto marker = f.install / "manifests/runtime.txt";
    const auto manifest = f.install / "manifests/0.1.0/emulator.json";
    const auto exe = f.install / "runtime/0.1.0/emulator/shadPS4.exe";
    CHECK(fs::remove(marker));
    const auto original = read(manifest);
    write(manifest, original + "\n");
    CHECK(!parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(!fs::exists(marker));
    write(manifest, original);
    write(exe, "bad");
    CHECK(!parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(!fs::exists(marker));
    CHECK(read(exe) == "bad");
    write(exe, "abc");
    const auto extra = f.install / "runtime/0.1.0/emulator/unlisted.txt";
    write(extra, "unknown");
    CHECK(!parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(!fs::exists(marker));
    CHECK(read(extra) == "unknown");
    CHECK(fs::remove(extra));
    const auto extra_manifest = f.install / "manifests/0.1.0/unlisted.json";
    write(extra_manifest, "{}");
    CHECK(!parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(read(extra_manifest) == "{}");
    CHECK(fs::remove(extra_manifest));
    const auto before = fs::last_write_time(exe);
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(read(marker) == "0.1.0\n");
    CHECK(fs::last_write_time(exe) == before);
}

TEST(bundled_runtime_recovers_between_slot_and_manifest_renames)
{
    fixture f;
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    const auto marker = f.install / "manifests/runtime.txt";
    const auto manifests = f.install / "manifests/0.1.0";
    const auto private_stage = f.install / "runtime/.bundled-123-456-7";
    const auto partial_stage = f.install / "runtime/.bundled-123-456-8";
    const auto unrelated = f.install / "runtime/.bundled-123-456-9";
    const auto backup = f.install / "runtime/.repair-0.1.0-1";
    const auto exe = f.install / "runtime/0.1.0/emulator/shadPS4.exe";
    CHECK(fs::remove(marker));
    fs::create_directories(private_stage / "manifests");
    fs::rename(manifests, private_stage / "manifests/0.1.0");
    write(private_stage / ".fresh-stage", "show23-fresh-runtime-v1\n0.1.0\n");
    write(partial_stage / ".fresh-stage", "show23-fresh-runtime-v1\n0.1.0\n");
    write(partial_stage / "runtime/0.1.0/emulator/shadPS4.exe", "partial copy");
    write(unrelated / "file", "keep");
    write(backup / "old/file", "keep");

    const auto candidate = private_stage / "manifests/0.1.0/emulator.json";
    const auto expected = read(candidate);
    write(candidate, expected + "\n");
    CHECK(!parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(!fs::exists(marker) && !fs::exists(manifests));
    write(candidate, expected);
    write(exe, "bad");
    CHECK(!parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(!fs::exists(marker) && !fs::exists(manifests));
    write(exe, "abc");
    const auto before = fs::last_write_time(exe);
    CHECK(parts::inspect_bundled_runtime(f.bundle, f.install, "0.1.0").fresh);
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(read(marker) == "0.1.0\n");
    CHECK(read(manifests / "emulator.json") == expected);
    CHECK(fs::last_write_time(exe) == before);
    CHECK(!fs::exists(private_stage) && !fs::exists(partial_stage));
    CHECK(read(unrelated / "file") == "keep");
    CHECK(read(backup / "old/file") == "keep");
}

TEST(bundled_runtime_preserves_unknown_ahead_and_unreadable_install_states)
{
    fixture f;
    const auto unknown = f.install / "runtime/9.1.0/server/preserve";
    const auto marker = f.install / "manifests/runtime.txt";
    write(unknown, "existing");
    CHECK(!parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(!fs::exists(marker));
    CHECK(read(unknown) == "existing");
    write(marker, "9.1.0\n");
    CHECK(!parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(!parts::inspect_bundled_runtime(f.bundle, f.install, "0.1.0").repairable);
    CHECK(!parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0").empty());
    CHECK(read(marker) == "9.1.0\n");
    write(marker, "?\n");
    CHECK(!parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(!parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0").empty());
    CHECK(read(marker) == "?\n");
}

TEST(bundled_runtime_repair_requires_known_exact_part_versions)
{
    fixture f;
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    const auto manifest = f.install / "manifests/0.1.0/emulator.json";
    const auto exe = f.install / "runtime/0.1.0/emulator/shadPS4.exe";
    auto ahead = read(manifest);
    const auto version = ahead.find("\"version\":\"0.1.0\"");
    CHECK(version != std::string::npos);
    ahead.replace(version, std::string_view("\"version\":\"0.1.0\"").size(), "\"version\":\"9.1.0\"");
    write(manifest, ahead);
    CHECK(parts::inspect_bundled_runtime(f.bundle, f.install, "0.1.0").ready);
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(!parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0").empty());
    write(exe, "bad");
    CHECK(!parts::inspect_bundled_runtime(f.bundle, f.install, "0.1.0").repairable);
    CHECK(!parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0").empty());
    CHECK(read(manifest) == ahead);
    CHECK(read(exe) == "bad");
    write(manifest, "unreadable");
    CHECK(!parts::inspect_bundled_runtime(f.bundle, f.install, "0.1.0").repairable);
    CHECK(!parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0").empty());
    CHECK(read(manifest) == "unreadable");
    CHECK(read(exe) == "bad");
}

TEST(bundled_runtime_only_resumes_an_interrupted_repair_on_request)
{
    for (unsigned interruption = 0; interruption <= 3; ++interruption)
    {
        fixture f;
        CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
        const auto slot = f.install / "runtime/0.1.0";
        const auto manifests = f.install / "manifests/0.1.0";
        write(slot / "emulator/shadPS4.exe", "bad");
        const auto transaction = f.install / "runtime/.repair-0.1.0-1";
        fs::create_directories(transaction / "new");
        fs::copy(f.bundle / "runtime", transaction / "new/runtime", fs::copy_options::recursive);
        fs::copy(f.bundle / "manifests", transaction / "new/manifests", fs::copy_options::recursive);
        fs::create_directories(transaction / "old/runtime");
        fs::create_directories(transaction / "old/manifests");
        if (interruption >= 1) fs::rename(slot, transaction / "old/runtime/0.1.0");
        if (interruption >= 2) fs::rename(manifests, transaction / "old/manifests/0.1.0");
        if (interruption >= 3) fs::rename(transaction / "new/manifests/0.1.0", manifests);
        CHECK(!parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
        CHECK(fs::exists(slot) == (interruption == 0));
        CHECK(fs::exists(manifests) == (interruption != 2));
        CHECK(parts::inspect_bundled_runtime(f.bundle, f.install, "0.1.0").repairable);
        if (interruption == 2)
        {
            // A later in-place Repair may have been interrupted while restoring individual manifests.
            fs::create_directories(manifests);
            fs::copy_file(f.bundle / "manifests/0.1.0/emulator.json", manifests / "emulator.json");
        }
        CHECK(parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0").empty());
        CHECK(parts::inspect_bundled_runtime(f.bundle, f.install, "0.1.0").ready);
        const auto original = transaction / "old/runtime/0.1.0/emulator/shadPS4.exe";
        CHECK(fs::exists(original) == (interruption >= 1));
        if (interruption >= 1) CHECK(read(original) == "bad");
    }
}

TEST(bundled_runtime_leases_allow_shared_play_and_exclude_repair)
{
    fixture f;
    const auto manifests = f.install / "manifests";
    CHECK(!parts::runtime_in_use(f.install));
    CHECK(!fs::exists(manifests));
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    const auto exe = f.install / "runtime/0.1.0/emulator/shadPS4.exe";
    write(exe, "bad");
    {
        const parts::runtime_lease play(f.install, false);
        const parts::runtime_lease manual(f.install, false);
        CHECK(play && manual);
        CHECK(parts::runtime_in_use(f.install));
        CHECK(parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0") == "in-use");
        CHECK(read(exe) == "bad");
    }
    CHECK(!parts::runtime_in_use(f.install));
    {
        const parts::runtime_lease repair(f.install, true);
        CHECK(static_cast<bool>(repair));
        const parts::runtime_lease play(f.install, false);
        CHECK(!play);
    }
    CHECK(parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0").empty());
}

TEST(bundled_runtime_excludes_unleased_images_from_new_and_interrupted_repair)
{
    std::wstring test_image(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, test_image.data(), static_cast<DWORD>(test_image.size()));
    CHECK(length && length < test_image.size());
    if (!length || length >= test_image.size()) return;
    test_image.resize(length);

    for (const bool pending : {false, true})
    {
        fixture f;
        CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
        const auto slot = f.install / "runtime/0.1.0";
        const auto damaged = slot / "emulator/shadPS4.exe";
        const auto image = slot / "emulator/fixture-child.exe";
        const auto transaction = f.install / "runtime/.repair-0.1.0-1";
        const auto backup = transaction / "old/runtime/0.1.0";
        fs::copy_file(test_image, image);
        write(damaged, "bad");

        if (pending)
        {
            fs::create_directories(transaction / "new");
            fs::copy(f.bundle / "runtime", transaction / "new/runtime", fs::copy_options::recursive);
            fs::copy(f.bundle / "manifests", transaction / "new/manifests", fs::copy_options::recursive);
            fs::create_directories(transaction / "old/runtime");
            fs::create_directories(transaction / "old/manifests");
        }

        {
            auto command = L"\"" + image.native() + L"\" --fake-child";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION child{};
            const auto started = CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, f.root.c_str(), &startup, &child);
            CHECK(started);
            if (!started) continue;
            CloseHandle(child.hThread);
            const auto cleanup = utils::finally([&]
            {
                TerminateProcess(child.hProcess, 0);
                CHECK(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0);
                CloseHandle(child.hProcess);
            });
            CHECK(WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT);
            {
                const parts::runtime_lease exclusive(f.install, true);
                CHECK(exclusive);
            }
            CHECK(parts::inspect_bundled_runtime(f.bundle, f.install, "0.1.0").repairable);
            CHECK(parts::runtime_in_use(f.install));
            CHECK(parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0") == "in-use");
            CHECK(fs::exists(damaged) && read(damaged) == "bad");
            CHECK(fs::exists(transaction) == pending);
            CHECK(!fs::exists(backup));
            CHECK(WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT);
        }
        CHECK(!parts::runtime_in_use(f.install));
        const show23::input_paths::handle directory(CreateFileW(slot.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        CHECK(directory);
        CHECK(parts::repair_bundled_runtime(f.bundle, f.install, "0.1.0").empty());
        CHECK(parts::inspect_bundled_runtime(f.bundle, f.install, "0.1.0").ready);
        CHECK(!fs::exists(backup));
        CHECK(fs::exists(image));
    }
}

TEST(bundled_runtime_requires_the_card_importer_in_the_manifest)
{
    fixture f;
    f.part("server", "server", {"python/python.exe", "olserver.py", "roster_decode.py",
        "tables/ol-messages.csv", "replies/fixture.xml"});
    CHECK(!parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(!fs::exists(f.install / "manifests/runtime.txt"));
}

TEST(bundled_part_badge_requires_manifest_and_regular_listed_files)
{
    fixture f;
    const auto server = parts::id::server;
    CHECK(parts::has_bundled_part(parts::read_runtime(f.bundle), server));

    const auto script = f.bundle / "runtime/0.1.0/server/olserver.py";
    CHECK(fs::remove(script));
    CHECK(!parts::has_bundled_part(parts::read_runtime(f.bundle), server));

    CHECK(fs::create_directory(script));
    CHECK(!parts::has_bundled_part(parts::read_runtime(f.bundle), server));
    CHECK(fs::remove(script));
    write(script, "abc");

    write(f.bundle / "manifests/0.1.0/profile.json", "malformed");
    const auto invalid = parts::read_runtime(f.bundle);
    CHECK(!parts::has_bundled_part(invalid, parts::id::profile));
    CHECK(!parts::has_bundled_part(invalid, server));
}
