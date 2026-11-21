#include "std_include.hpp"
#include "test.hpp"
#include "input_fixtures.hpp"
#include "show23/input_checks.hpp"

#include <Windows.h>
#include <atomic>
#include <fstream>

namespace
{
    using show23::finding_state;
    using show23::source_kind;
    namespace fs = std::filesystem;
    constexpr std::uint64_t allowance = 2ull * 1024 * 1024 * 1024;
    // SHA-256 of the three synthetic bytes used as the executable.
    constexpr std::string_view abc_hash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    struct fixture
    {
        fs::path root;
        show23::input_check_options options;

        fixture()
        {
            static std::atomic_uint32_t sequence{};
            const auto directory = fs::current_path() / L"build";
            fs::create_directories(directory);
            root = directory / (L"check-inputs-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence));
            if (!fs::create_directory(root)) throw std::runtime_error("Could not create an isolated fixture folder");
            options.base = {3, abc_hash};
            options.update = {3, abc_hash};
            options.free_space = [](const fs::path&) { return 8 * allowance; };
        }

        ~fixture()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }

        fs::path file(const fs::path& name, std::string_view data)
        {
            const auto path = root / name;
            fs::create_directories(path.parent_path());
            std::ofstream stream(path, std::ios::binary);
            if (!stream.write(data.data(), static_cast<std::streamsize>(data.size())) || !stream.flush())
                throw std::runtime_error("Could not write a fixture");
            return path;
        }

        fs::path folder(const fs::path& name = L"base", std::string metadata = input_fixtures::sfo(), std::string_view executable = "abc")
        {
            file(name / L"sce_sys" / L"param.sfo", metadata);
            file(name / L"eboot.bin", executable);
            return root / name;
        }

        show23::checked_inputs check(const fs::path& base, std::optional<fs::path> update = std::nullopt,
                                    source_kind type = source_kind::automatic)
        {
            return show23::check_inputs({type, base, std::move(update), root / L"destination"}, options);
        }
    };

    const show23::finding* find(const show23::checked_inputs& check, std::string_view id)
    {
        const auto row = std::find_if(check.findings.begin(), check.findings.end(), [id](const auto& entry) { return entry.id == id; });
        return row == check.findings.end() ? nullptr : &*row;
    }

    bool state(const show23::checked_inputs& check, std::string_view id, finding_state expected)
    {
        const auto row = find(check, id);
        return row && row->state == expected;
    }

    bool reason(const show23::checked_inputs& check, std::string_view id, std::string_view expected)
    {
        const auto row = find(check, id);
        if (!row) return false;
        const auto field = row->values.find("reason");
        return field != row->values.end() && std::get<std::string>(field->second) == expected;
    }
}

TEST(check_inputs_accepts_a_package_and_defers_its_executable)
{
    fixture f;
    const auto pkg = f.file(L"base.pkg", input_fixtures::package());
    const auto report = f.check(pkg, std::nullopt, source_kind::package);
    CHECK(report.passed());
    CHECK(report.base_version == "1.00" && !report.has_update);
    CHECK(report.needed_bytes == allowance + 24);
    CHECK(report.available_bytes == 8 * allowance);
    CHECK(state(report, "base.complete", finding_state::passed));
    CHECK(state(report, "base.executable", finding_state::later));
    CHECK(state(report, "install.writable", finding_state::passed));
    CHECK(!fs::exists(f.root / L"destination"));
    CHECK(std::distance(fs::directory_iterator(f.root), fs::directory_iterator{}) == 1);
}

TEST(check_inputs_rejects_the_wrong_title_content_category_or_version)
{
    fixture f;
    const auto path = f.file(L"inputs.pkg", input_fixtures::package(input_fixtures::sfo(false, "CUSA00000")));
    CHECK(state(f.check(path), "base.title", finding_state::failed));
    f.file(L"inputs.pkg", input_fixtures::package(input_fixtures::sfo(true)));
    CHECK(state(f.check(path), "base.category", finding_state::failed));
    f.file(L"inputs.pkg", input_fixtures::package(input_fixtures::sfo(false, "CUSA35521", "01.19")));
    CHECK(state(f.check(path), "base.version", finding_state::failed));
    auto bytes = input_fixtures::package();
    bytes[0x40] = 'X';
    f.file(L"inputs.pkg", bytes);
    CHECK(state(f.check(path), "base.content", finding_state::failed));
    input_fixtures::put(bytes, 0x74, 0x26, 4, true);
    f.file(L"inputs.pkg", bytes);
    CHECK(state(f.check(path), "base.type", finding_state::failed));
}

TEST(check_inputs_requires_metadata_keys_and_reports_truncation)
{
    fixture f;
    auto bytes = input_fixtures::sfo();
    bytes[84] = 'X';
    const auto missing = f.file(L"missing.pkg", input_fixtures::package(bytes));
    CHECK(state(f.check(missing), "base.title", finding_state::failed));

    bytes = input_fixtures::package();
    bytes.pop_back();
    const auto incomplete = f.file(L"incomplete.pkg", bytes);
    CHECK(reason(f.check(incomplete), "base.complete", "truncated"));
}

TEST(check_inputs_streams_known_folder_executables_and_estimates_directory_bytes)
{
    fixture f;
    const auto base = f.folder();
    f.file(L"base/data/extra.bin", "12345");
    const auto accepted = f.check(base, std::nullopt, source_kind::folder);
    CHECK(accepted.passed());
    CHECK(state(accepted, "base.executable", finding_state::passed));
    CHECK(accepted.needed_bytes == allowance + input_fixtures::sfo().size() + 3 + 5);

    f.file(L"base/eboot.bin", "def");
    const auto rejected = f.check(base);
    CHECK(!rejected.passed());
    CHECK(reason(rejected, "base.executable", "digest"));
}

TEST(check_inputs_rejects_folders_without_the_required_files)
{
    fixture f;
    const auto base = f.folder();
    fs::remove(base / L"eboot.bin");
    CHECK(reason(f.check(base), "base.executable", "missing"));
    fs::remove(base / L"sce_sys" / L"param.sfo");
    CHECK(reason(f.check(base), "base.metadata", "missing"));
    CHECK(state(f.check(base, std::nullopt, source_kind::package), "base.kind", finding_state::failed));
    CHECK(state(f.check(f.root / L"not-there"), "base.kind", finding_state::failed));
}

TEST(check_inputs_accepts_both_update_input_types_and_explains_offline_support)
{
    fixture f;
    const auto base = f.file(L"base.pkg", input_fixtures::package());
    const auto pkg = f.file(L"update.pkg", input_fixtures::package(input_fixtures::sfo(true)));
    const auto pkg_check = f.check(base, pkg);
    CHECK(pkg_check.passed() && pkg_check.has_update && pkg_check.update_version == "1.21");
    CHECK(pkg_check.needed_bytes == 2 * allowance + 48);
    CHECK(state(pkg_check, "update.matches", finding_state::passed));
    CHECK(state(pkg_check, "update.executable", finding_state::later));
    CHECK(state(pkg_check, "update.offline", finding_state::notice));

    const auto folder = f.folder(L"update", input_fixtures::sfo(true));
    const auto folder_check = f.check(base, folder);
    CHECK(folder_check.passed());
    CHECK(state(folder_check, "update.executable", finding_state::passed));
    CHECK(state(folder_check, "update.offline", finding_state::notice));
}

TEST(check_inputs_rejects_a_base_as_update_and_reports_update_identity_mismatch)
{
    fixture f;
    const auto base = f.folder();
    const auto wrong_role = f.folder(L"wrong-role");
    CHECK(state(f.check(base, wrong_role), "update.category", finding_state::failed));

    const auto wrong_title = f.folder(L"wrong-title", input_fixtures::sfo(true, "CUSA00000"));
    const auto wrong_content = f.folder(L"wrong-id", input_fixtures::sfo(true, "CUSA35521", "01.21", "SYNTHETIC-ID"));
    for (const auto& candidate : {wrong_title, wrong_content})
    {
        const auto result = f.check(base, candidate);
        CHECK(state(result, "update.matches", finding_state::failed));
        CHECK(find(result, "update.offline") == nullptr);
    }
}

TEST(check_inputs_refuses_destination_overlap_in_both_directions)
{
    fixture f;
    const auto base = f.folder();
    const auto inspect = [&](const fs::path& dest)
    {
        return show23::check_inputs({source_kind::folder, base, std::nullopt, dest}, f.options);
    };
    CHECK(state(inspect(base / L"child"), "install.overlap", finding_state::failed));
    CHECK(state(inspect(f.root), "install.overlap", finding_state::failed));
    CHECK(state(inspect(f.root / L"BASE" / L"sibling"), "install.overlap", finding_state::failed));
    CHECK(inspect(f.root / L"base-other").passed());
}

TEST(check_inputs_refuses_non_local_non_folders_and_unwritable_destinations)
{
    fixture f;
    const auto base = f.folder();
    const auto bad = f.file(L"file.txt", "x");
    CHECK(state(show23::check_inputs({source_kind::folder, base, std::nullopt, L"relative"}, f.options),
                "install.local", finding_state::failed));
    CHECK(state(show23::check_inputs({source_kind::folder, base, std::nullopt, bad}, f.options),
                "install.writable", finding_state::failed));
    f.options.writable = [](const fs::path&, bool creating) { CHECK(creating); return false; };
    CHECK(reason(f.check(base), "install.writable", "unwritable"));
}

TEST(check_inputs_checks_space_against_the_estimate_with_an_injected_answer)
{
    fixture f;
    const auto base = f.folder();
    f.options.free_space = [](const fs::path&) { return allowance; };
    const auto too_little = f.check(base);
    CHECK(!too_little.passed() && too_little.needed_bytes > too_little.available_bytes);
    CHECK(state(too_little, "install.space", finding_state::failed));
    f.options.free_space = [](const fs::path&) -> std::optional<std::uint64_t> { return std::nullopt; };
    CHECK(reason(f.check(base), "install.space", "unreadable"));
}

TEST(check_inputs_probes_existing_destinations_without_leaving_a_file)
{
    fixture f;
    const auto base = f.folder();
    const auto destination = f.root / L"destination";
    fs::create_directory(destination);
    CHECK(f.check(base).passed());
    CHECK(fs::is_empty(destination));
}
