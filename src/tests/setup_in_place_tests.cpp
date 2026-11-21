#include "std_include.hpp"
#include "test.hpp"
#include "input_fixtures.hpp"
#include "show23/setup_run.hpp"
#include "show23/input_paths.hpp"

#include <atomic>
#include <fstream>
#include <winioctl.h>

namespace
{
    using namespace show23;
    constexpr auto allowance = 2ull * 1024 * 1024 * 1024;
    constexpr std::string_view abc_hash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    constexpr std::array base_entries{"eboot.bin", "sce_sys/param.sfo", "FILELIST.BIN", "BOOT", "CROSS", "FLUX", "FRONTEND", "PS4", "sce_module",
        "sce_module/libc.prx", "sce_module/libSceFace.prx", "sce_module/libSceFios2.prx", "sce_module/libSceHand.prx", "sce_module/libSceS3DConversion.prx"};
    constexpr auto index(setup_stage stage) { return static_cast<std::size_t>(stage); }

    void write(const fs::path& path, const std::string_view data)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        if (!file.write(data.data(), static_cast<std::streamsize>(data.size()))) throw std::runtime_error("Fixture write failed");
    }

    std::string read(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    struct fixture
    {
        fs::path root;
        setup_request request;
        setup_options options;

        fixture()
        {
            static std::atomic_uint serial{};
            const auto builds = fs::current_path() / "build";
            fs::create_directories(builds);
            root = builds / (L"setup-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial));
            if (!fs::create_directory(root)) throw std::runtime_error("Fixture directory already exists");
            request = {source_kind::automatic, root / "base", std::nullopt, root / "install"};
            options.inputs.base = options.inputs.update = {3, abc_hash};
            options.inputs.free_space = [](const auto&) { return allowance; };
            options.base.directories.fill(4);
            options.base.modules.fill({3, abc_hash});
            options.update = options.base;
            game(request.base, false);
        }

        ~fixture()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        static void game(const fs::path& path, bool update)
        {
            write(path / "eboot.bin", "abc");
            write(path / "sce_sys" / "param.sfo", input_fixtures::sfo(update));
            write(path / (update ? "PATCHLIST.BIN" : "FILELIST.BIN"), "list");
            if (!update) fs::create_directory(path / "BOOT");
            for (const auto* name : directory_names) write(path / name / "asset.bin", "1234");
            for (const auto* name : module_names) write(path / "sce_module" / name, "abc");
        }

        fs::path marker() const { return request.install / "manifests" / "game-ready.txt"; }
        fs::path record() const { return request.install / "data" / "games" / title_id / "game.json"; }
        setup_report run(const setup_progress& progress = {}) const { return run_setup(request, options, progress); }
    };

    bool has(const std::vector<finding>& findings, const std::string_view id, finding_state state,
             const std::string_view key = {}, const std::string_view value = {})
    {
        return std::ranges::any_of(findings, [&](const auto& item)
        {
            if (item.id != id || item.state != state) return false;
            if (key.empty()) return true;
            const auto field = item.values.find(std::string(key));
            if (field == item.values.end()) return false;
            const auto text = std::get_if<std::string>(&field->second);
            return text && *text == value;
        });
    }

    struct snapshot_entry
    {
        std::string bytes;
        bool directory;
        std::uint64_t size;
        std::uint64_t created;
        std::uint64_t written;
        bool operator==(const snapshot_entry&) const = default;
    };

    auto snapshot(const fs::path& path)
    {
        std::map<fs::path, snapshot_entry> result;
        const auto collect = [&](const fs::path& item, const bool directory)
        {
            const auto bytes = directory ? std::string{} : read(item);
            const auto pin = input_paths::inspect(item);
            FILETIME created{}, written{};
            // Windows can update last-access time on reads from the game and from this snapshot.
            CHECK(GetFileTime(pin.pins.back().get(), &created, nullptr, &written));
            const auto value = [](FILETIME stamp)
            {
                return (static_cast<std::uint64_t>(stamp.dwHighDateTime) << 32) | stamp.dwLowDateTime;
            };
            result.emplace(item.lexically_relative(path), snapshot_entry{bytes, directory, directory ? 0 : pin.length,
                value(created), value(written)});
        };
        collect(path, true);
        for (const auto& item : fs::recursive_directory_iterator(path)) collect(item.path(), item.is_directory());
        return result;
    }

    bool junction(const fs::path& link, const fs::path& target)
    {
        fs::create_directory(link);
        const auto substitute = L"\\??\\" + target.native();
        std::vector<wchar_t> paths(substitute.begin(), substitute.end());
        paths.push_back(L'\0');
        paths.insert(paths.end(), target.native().begin(), target.native().end());
        paths.push_back(L'\0');
        const auto bytes = [](std::size_t count) { return static_cast<WORD>(count * sizeof(wchar_t)); };
        struct header { DWORD tag; WORD length, reserved, substitute_at, substitute_len, print_at, print_len; };
        const header data{IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(8 + bytes(paths.size())), 0, 0,
            bytes(substitute.size()), bytes(substitute.size() + 1), bytes(target.native().size())};
        std::vector<char> buffer(sizeof(data) + bytes(paths.size()));
        std::memcpy(buffer.data(), &data, sizeof(data));
        std::memcpy(buffer.data() + sizeof(data), paths.data(), bytes(paths.size()));
        const input_paths::handle dest(CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        DWORD used{};
        return dest && DeviceIoControl(dest.get(), FSCTL_SET_REPARSE_POINT, buffer.data(),
            static_cast<DWORD>(buffer.size()), nullptr, 0, &used, nullptr);
    }
}

TEST(setup_in_place_publishes_record_before_marker_and_can_be_repeated_without_touching_sources)
{
    fixture f;
    f.request.update = f.root / "update";
    fixture::game(*f.request.update, true);
    const auto base = snapshot(f.request.base);
    const auto update = snapshot(*f.request.update);
    unsigned remembered = 0;
    f.options.remember = [&](const fs::path& where)
    {
        ++remembered;
        CHECK(input_paths::contains(where, f.request.install));
        CHECK(fs::is_regular_file(f.marker()) && fs::is_regular_file(f.record()));
        return true;
    };
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto result = f.run([&](const setup_report& progress)
        {
            if (progress.stages[index(setup_stage::finish)].state == stage_state::running)
            {
                CHECK(fs::is_regular_file(f.record()));
                CHECK(!fs::exists(f.marker()));
            }
        });
        CHECK(result.finished());
        CHECK(result.stages[index(setup_stage::unpack)].state == stage_state::not_needed);
        CHECK(has(result.stages[index(setup_stage::unpack)].findings, "unpack.inPlace", finding_state::passed));
        CHECK(inspect_game(f.request.install).state == record_state::ready);
    }
    fs::path source;
    const auto record = decode_game(read(f.record()), source);
    CHECK(record && record->base.version == "1.00" && record->update && record->update->version == "1.21");
    CHECK(record && record->base.executable_sha256 == abc_hash && record->base.executable_size == 3);
    CHECK(record && record->base.directories == f.options.base.directories && record->created_utc.ends_with('Z'));
    CHECK(record && input_paths::contains(record->base.path, f.request.base));
    CHECK(record && record->update && input_paths::contains(record->update->path, *f.request.update));
    CHECK(remembered == 2);
    CHECK(fs::is_directory(f.request.install / "data/profiles/default/user"));
    CHECK(fs::is_directory(f.request.install / "data/profiles/default/server"));
    CHECK(fs::is_directory(f.request.install / "data/logs"));
    CHECK(snapshot(f.request.base) == base);
    CHECK(snapshot(*f.request.update) == update);
}

TEST(setup_package_inputs_stop_at_unpack_without_creating_an_install)
{
    fixture f;
    f.options.inputs.free_space = [](const auto&) { return 4 * allowance; };
    const auto base = f.root / "base.pkg";
    const auto update = f.root / "update.pkg";
    write(base, input_fixtures::package());
    write(update, input_fixtures::package(input_fixtures::sfo(true)));
    f.options.remember = [](const auto&) { CHECK(false); return false; };
    for (const bool patch : {false, true})
    {
        auto request = f.request;
        if (patch) request.update = update;
        else request.base = base;
        const auto report = run_setup(request, f.options);
        const auto& unpack = report.stages[index(setup_stage::unpack)];
        CHECK(unpack.state == stage_state::stopped);
        CHECK(has(unpack.findings, "unpack.package", finding_state::later, "part", patch ? "update" : "base"));
        CHECK(!fs::exists(request.install));
    }
}

TEST(verify_in_place_names_each_missing_required_entry)
{
    fixture f;
    for (const auto* relative : base_entries)
    {
        const auto source = f.request.base / relative;
        fs::rename(source, f.root / "away");
        std::vector<finding> findings;
        verify_folder(f.request.base, false, f.options.inputs.base, f.options.base, findings);
        CHECK(has(findings, "verify.entry", finding_state::failed, "name", relative));
        fs::rename(f.root / "away", source);
    }
    const auto update = f.root / "update";
    fixture::game(update, true);
    for (const auto* relative : base_entries)
    {
        if (std::string_view(relative) == "BOOT") continue;
        if (std::string_view(relative) == "FILELIST.BIN") relative = "PATCHLIST.BIN";
        const auto source = update / relative;
        fs::rename(source, f.root / "away");
        std::vector<finding> findings;
        verify_folder(update, true, f.options.inputs.update, f.options.update, findings);
        CHECK(has(findings, "verify.entry", finding_state::failed, "name", relative));
        fs::rename(f.root / "away", source);
    }
}

TEST(verify_in_place_distinguishes_size_matches_shortfalls_and_extra_files_and_notices_module_sha)
{
    fixture f;
    write(f.request.base / "sce_module/libc.prx", "def");
    write(f.request.base / "PS4/asset.bin", "123");
    write(f.request.base / "FLUX/asset.bin", "123456");
    const auto report = f.run();
    const auto& findings = report.stages[index(setup_stage::verify)].findings;
    CHECK(has(findings, "verify.moduleDigest", finding_state::notice, "name", "libc.prx"));
    CHECK(has(findings, "verify.directory", finding_state::failed, "name", "PS4"));
    CHECK(has(findings, "verify.directory", finding_state::notice, "name", "FLUX"));
    CHECK(has(findings, "verify.directory", finding_state::passed, "name", "CROSS"));
    CHECK(!fs::exists(f.marker()));
    write(f.request.base / "PS4/asset.bin", "1234");
    CHECK(f.run().finished());
    write(f.request.base / "sce_module/libSceFace.prx", "x");
    const auto invalid = f.run();
    CHECK(has(invalid.stages[index(setup_stage::verify)].findings, "verify.moduleSize", finding_state::failed));
    CHECK(!fs::exists(f.marker()));
}

TEST(verify_runs_the_executable_hash_again_and_failed_publication_leaves_no_marker)
{
    fixture f;
    const auto report = f.run([&](const setup_report& progress)
    {
        if (progress.stages[index(setup_stage::verify)].state == stage_state::running)
            write(f.request.base / "eboot.bin", "xyz");
    });
    CHECK(has(report.stages[index(setup_stage::verify)].findings, "verify.executable", finding_state::failed));
    CHECK(!fs::exists(f.marker()));
    write(f.request.base / "eboot.bin", "abc");
    f.options.remember = [](const auto&) { return false; };
    CHECK(!f.run().finished());
    CHECK(fs::exists(f.record()) && !fs::exists(f.marker()));
    f.options.remember = {};
    CHECK(f.run().finished());
}

TEST(verify_refuses_junctions_and_setup_does_not_write_through_install_junctions)
{
    fixture f;
    const auto outside = f.root / "unrelated";
    write(outside / "extra", "data");
    const auto link = f.request.base / "PS4" / "linked";
    CHECK(junction(link, outside));
    std::vector<finding> findings;
    verify_folder(f.request.base, false, f.options.inputs.base, f.options.base, findings);
    CHECK(has(findings, "verify.tree", finding_state::failed));
    CHECK(!has(findings, "verify.directory", finding_state::notice, "name", "PS4"));
    CHECK(fs::remove(link));
    fs::create_directory(f.request.install);
    CHECK(junction(f.request.install / "data", f.request.base));
    CHECK(!f.run().finished());
    CHECK(!fs::exists(f.marker()));
    CHECK(!fs::exists(f.request.base / "games"));
    CHECK(fs::remove(f.request.install / "data"));
    CHECK(f.run().finished());
}

TEST(game_record_readiness_checks_saved_executable_size_without_hashing_and_reports_missing_paths)
{
    fixture f;
    CHECK(f.run().finished());
    write(f.request.base / "eboot.bin", "xyz");
    CHECK(inspect_game(f.request.install).state == record_state::ready);
    write(f.request.base / "eboot.bin", "too long");
    CHECK(inspect_game(f.request.install).state == record_state::invalid);
    fs::rename(f.request.base, f.root / "removed");
    const auto lost = inspect_game(f.request.install);
    CHECK(lost.state == record_state::invalid && input_paths::contains(lost.source_path, f.request.base));
    auto json = read(f.record());
    json.replace(json.find("in_place"), 8, "invalid");
    write(f.record(), json);
    const auto invalid = inspect_game(f.request.install);
    CHECK(invalid.state == record_state::invalid && !invalid.record && input_paths::contains(invalid.source_path, f.request.base));
}

TEST(check_inputs_budgets_two_gib_total_for_an_in_place_base_and_update)
{
    fixture f;
    f.request.update = f.root / "update";
    fixture::game(*f.request.update, true);
    const auto accepted = check_inputs(f.request, f.options.inputs);
    CHECK(accepted.passed() && accepted.needed_bytes == allowance);
    CHECK(has(accepted.findings, "install.space", finding_state::passed, "kind", "in_place"));
    f.options.inputs.free_space = [](const auto&) { return allowance - 1; };
    CHECK(!check_inputs(f.request, f.options.inputs).passed());
}
