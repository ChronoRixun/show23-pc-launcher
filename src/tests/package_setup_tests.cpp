#include "std_include.hpp"
#include "test.hpp"
#include "package_setup_tests.hpp"
#include "input_fixtures.hpp"
#include "show23/setup_run.hpp"
#include "show23/unpacker.hpp"

#include <shellapi.h>
#include <cstring>
#include <fstream>
#include <winioctl.h>

namespace
{
    using namespace show23;
    constexpr auto digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    constexpr auto allowance = 2ull * 1024 * 1024 * 1024;
    constexpr auto unpack = static_cast<std::size_t>(setup_stage::unpack);

    void write(const fs::path& path, const std::string_view contents)
    {
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        if (!stream.write(contents.data(), static_cast<std::streamsize>(contents.size())))
            throw std::runtime_error("Could not write fixture");
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
        const input_paths::handle handle(CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        DWORD used{};
        return handle && DeviceIoControl(handle.get(), FSCTL_SET_REPARSE_POINT, buffer.data(),
            static_cast<DWORD>(buffer.size()), nullptr, 0, &used, nullptr);
    }

    struct fixture
    {
        fs::path root;
        setup_request request;
        setup_options options;

        fixture()
        {
            static unsigned sequence = 0;
            root = fs::current_path() / "build" / ("package fixture " + std::to_string(GetCurrentProcessId()) +
                " " + std::to_string(GetTickCount64()) + " " + std::to_string(++sequence));
            fs::create_directories(root);
            request.kind = source_kind::automatic;
            request.install = root / "install here";
            request.unpacker = root / "orbis-unpkg.exe";
            wchar_t path[MAX_PATH]{};
            if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) throw std::runtime_error("Could not locate test executable");
            fs::copy_file(path, request.unpacker);
            fs::copy_file(path, root / "PS4-PKG-TOOL.EXE");
            options.remember_unpacker = [](const auto&) { return true; };
            request.unpacker_consent = find_unpacker(request.unpacker, {}).consent;
            options.inputs.base = options.inputs.update = {3, digest};
            options.inputs.free_space = [](const auto&) { return 8 * allowance; };
            options.base.directories.fill(4);
            options.base.modules.fill({3, digest});
            options.update = options.base;
            package("ok");
        }

        ~fixture()
        {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }

        void package(const char* behavior)
        {
            request.base = root / (std::string("base ") + behavior + ".pkg");
            write(request.base, input_fixtures::package());
        }

        fs::path game() const { return game_directory(request.install); }

        bool no_stage() const
        {
            if (!fs::exists(game())) return true;
            return std::ranges::none_of(fs::directory_iterator(game()), [](const auto& item)
            {
                return item.path().filename().string().starts_with("staging-");
            });
        }
    };

    bool has(const setup_report& report, const std::string_view id)
    {
        for (const auto& stage : report.stages)
            for (const auto& finding : stage.findings)
                if (finding.id == id) return true;
        return false;
    }
}

std::optional<int> fake_unpacker(const std::span<wchar_t*> args)
{
    const auto exe = fs::path(args.front()).filename().wstring();
    if (args.size() == 2 && std::wstring_view(args[1]) == L"--fake-child")
    {
        Sleep(INFINITE);
        return 9;
    }
    const bool orbis = _wcsicmp(exe.c_str(), L"orbis-unpkg.exe") == 0;
    if (!orbis && _wcsicmp(exe.c_str(), L"ps4-pkg-tool.exe") != 0) return std::nullopt;
    // orbis-unpkg: extract <package> -o <output>; ps4-pkg-tool: <package> <output>
    if (orbis ? args.size() != 5 || std::wstring_view(args[1]) != L"extract" || std::wstring_view(args[3]) != L"-o"
              : args.size() != 3) return 8;
    const auto package = fs::path(orbis ? args[2] : args[1]);
    const auto out = fs::path(args.back());
    if (!fs::equivalent(out, fs::current_path()) || !fs::is_regular_file(package)) return 8;
    const auto kind = package.stem().string();
    if (kind.ends_with("exit")) return 23;
    if (kind.ends_with("hang"))
    {
        const fs::path image(args.front());
        auto command = quote_process_argument(image.native()) + L" --fake-child";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, out.c_str(), &startup, &child)) return 9;
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        write(out / "child-id", std::to_string(child.dwProcessId));
        Sleep(INFINITE);
    }
    const bool update = kind.starts_with("update");
    const auto output = kind.ends_with("nested") ? out / "CUSA35521" : out;
    write(output / "eboot.bin", "abc");
    write(output / "sce_sys" / "param.sfo", input_fixtures::sfo(update));
    if (kind.ends_with("incomplete")) return 0;
    write(output / (update ? "PATCHLIST.BIN" : "FILELIST.BIN"), "list");
    if (!update) fs::create_directory(output / "BOOT");
    for (const auto* name : directory_names) write(output / name / "asset.bin", "1234");
    for (const auto* name : module_names) write(output / "sce_module" / name, "abc");
    if (kind.ends_with("outside")) write(out.parent_path() / "escaped", "outside");
    if (kind.ends_with("linked") && !junction(output / "linked", package.parent_path())) return 9;
    if (kind.ends_with("log"))
    {
        const std::string line(4096, 'x');
        for (unsigned n = 0; n < 600; ++n) std::fwrite(line.data(), 1, line.size(), stderr);
    }
    std::puts("Extracted fixture entries");
    return 0;
}

TEST(unpacker_quotes_arguments_with_spaces_backslashes_and_literal_quotes)
{
    const std::wstring value = L"one two\\\"three\\";
    auto line = L"fake.exe " + quote_process_argument(value);
    int argc{};
    auto argv = CommandLineToArgvW(line.c_str(), &argc);
    CHECK(argv && argc == 2 && std::wstring_view(argv[1]) == value);
    if (argv) LocalFree(argv);
}

TEST(package_setup_dispatches_orbis_and_caps_the_combined_log)
{
    fixture f;
    f.package("log");
    f.request.update = f.root / "update ok.pkg";
    write(*f.request.update, input_fixtures::package(input_fixtures::sfo(true)));
    CHECK(run_setup(f.request, f.options).finished());
    CHECK(f.no_stage());
    CHECK(fs::exists(f.game() / "base" / "eboot.bin"));
    CHECK(fs::exists(f.game() / "update" / "PATCHLIST.BIN"));
    const auto game = inspect_game(f.request.install);
    CHECK(game.state == record_state::ready && game.record && game.record->installed && game.record->update);
    bool capped = false;
    for (const auto& log : fs::directory_iterator(f.request.install / "data" / "logs"))
    {
        CHECK(fs::file_size(log.path()) <= 2 * 1024 * 1024);
        capped |= fs::file_size(log.path()) == 2 * 1024 * 1024;
    }
    CHECK(capped);
}

TEST(package_setup_dispatches_pkg_tool_case_insensitively_and_locates_a_child_folder)
{
    fixture f;
    f.package("nested");
    f.request.unpacker = f.root / "PS4-PKG-TOOL.EXE";
    f.request.unpacker_consent = find_unpacker(f.request.unpacker, {}).consent;
    CHECK(run_setup(f.request, f.options).finished());
    CHECK(fs::exists(f.game() / "base" / "eboot.bin"));
    CHECK(f.no_stage());
}

TEST(package_setup_rejects_unavailable_and_unsupported_programs)
{
    fixture f;
    f.request.unpacker = f.root / "unsupported.exe";
    CHECK(has(run_setup(f.request, f.options), "unpack.unsupported"));
    f.request.unpacker.clear();
    CHECK(has(run_setup(f.request, f.options), "unpack.noProgram"));
    CHECK(!fs::exists(f.request.install));
    f.options.bundled_unpackers = f.root;
    CHECK(run_setup(f.request, f.options).finished());
}

TEST(package_setup_requires_consent_for_exact_path_and_digest)
{
    fixture f;
    f.request.unpacker_consent.clear();
    const auto first = run_setup(f.request, f.options);
    CHECK(has(first, "unpack.consent") && !fs::exists(f.request.install));
    const auto& values = first.stages[unpack].findings.back().values;
    f.request.unpacker_consent = std::get<std::string>(values.at("consent"));
    f.options.remember_unpacker = [&](const auto& consent)
    {
        f.options.trusted_unpacker = consent;
        return true;
    };
    CHECK(run_setup(f.request, f.options).finished());
    f.request.existing_copy = "replace";
    std::ofstream(f.request.unpacker, std::ios::binary | std::ios::app) << "x";
    CHECK(has(run_setup(f.request, f.options), "unpack.consent"));
    CHECK(inspect_game(f.request.install).state == record_state::ready);
}

TEST(package_setup_rejects_bad_exit_incomplete_output_escape_and_links)
{
    const std::array cases{
        std::pair{"exit", "unpack.exit"}, std::pair{"incomplete", "verify.entry"},
        std::pair{"outside", "unpack.outside"}, std::pair{"linked", "unpack.result"}};
    for (const auto& [behavior, expected] : cases)
    {
        fixture f;
        f.package(behavior);
        const auto report = run_setup(f.request, f.options);
        CHECK(has(report, expected) && !report.finished());
        CHECK(f.no_stage() && fs::exists(f.request.base));
        CHECK(inspect_game(f.request.install).state == record_state::absent);
    }
}

TEST(package_setup_cancel_kills_child_and_cleans_staging)
{
    fixture f;
    f.package("hang");
    input_paths::handle child;
    const auto start = GetTickCount64();
    f.options.cancel = [&]
    {
        if (!fs::exists(f.game())) return false;
        for (const auto& entry : fs::directory_iterator(f.game()))
        {
            std::ifstream in(entry.path() / "base" / "child-id");
            DWORD pid{};
            if (in >> pid)
            {
                child = input_paths::handle(OpenProcess(SYNCHRONIZE, FALSE, pid));
                return true;
            }
        }
        return GetTickCount64() - start > 10'000;
    };
    const auto report = run_setup(f.request, f.options);
    CHECK(report.canceled && has(report, "unpack.canceled"));
    CHECK(child && WaitForSingleObject(child.get(), 0) == WAIT_OBJECT_0);
    CHECK(f.no_stage());
}

TEST(package_setup_keeps_or_replaces_and_rolls_back_failed_publication)
{
    fixture f;
    CHECK(run_setup(f.request, f.options).finished());
    const auto old = f.game() / "base" / "old";
    write(old, "previous");
    CHECK(has(run_setup(f.request, f.options), "unpack.existing"));
    f.request.existing_copy = "keep";
    CHECK(run_setup(f.request, f.options).finished() && fs::exists(old));
    f.request.existing_copy = "replace";
    f.package("incomplete");
    CHECK(!run_setup(f.request, f.options).finished() && fs::exists(old));
    f.package("ok");
    f.options.remember = [](const auto&) { return false; };
    CHECK(!run_setup(f.request, f.options).finished() && fs::exists(old));
    CHECK(inspect_game(f.request.install).state == record_state::ready && f.no_stage());
    f.options.remember = {};
    CHECK(run_setup(f.request, f.options).finished() && !fs::exists(old));
    write(f.request.install / "data" / "profiles" / "save", "save");
    CHECK(remove_game_copy(f.request.install));
    CHECK(!fs::exists(f.game() / "base") && !fs::exists(f.game() / "game.json"));
    CHECK(fs::exists(f.request.install / "data" / "profiles" / "save") && fs::exists(f.request.base) && f.no_stage());
    CHECK(!remove_game_copy(f.request.install));
}

TEST(package_setup_rejects_a_folder_update_for_a_package_base)
{
    fixture f;
    f.request.update = f.root / "expanded update";
    write(*f.request.update / "eboot.bin", "abc");
    write(*f.request.update / "sce_sys" / "param.sfo", input_fixtures::sfo(true));
    CHECK(has(run_setup(f.request, f.options), "unpack.mixed"));
    CHECK(!fs::exists(f.request.install));
}


TEST(package_setup_will_not_replace_an_unrecorded_game_directory)
{
    fixture f;
    const auto unrelated = f.game() / "base" / "unrelated";
    write(unrelated, "existing");
    CHECK(has(run_setup(f.request, f.options), "unpack.adopt"));
    CHECK(fs::exists(unrelated) && f.no_stage());
}
