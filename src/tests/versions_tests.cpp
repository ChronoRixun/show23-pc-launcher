#include "test.hpp"
#include "versions_tests.hpp"
#include "../bootstrap/process.hpp"
#include "../launcher/versioned_start.hpp"

#include <versions/decision.hpp>
#include <Windows.h>

#include <cwchar>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace
{
    using namespace versions;

    struct fixture
    {
        fs::path root;
        fixture()
        {
            const auto inside = fs::temp_directory_path() / ("show23-versions-" + request_id());
            if (!fs::create_directory(inside)) throw std::runtime_error("Could not create test directory.");
            root = inside;
        }
        ~fixture()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }
        void write(const fs::path& path, std::string_view data)
        {
            fs::create_directories((root / path).parent_path());
            std::ofstream out(root / path, std::ios::binary);
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
            if (!out) throw std::runtime_error("Could not write test fixture.");
        }

        fs::path complete(const std::string& name)
        {
            inventory record{std::string(folder_version(name)), name.find('-') == name.npos ? "stable" : "beta", 1,
                {{launcher_exe, sha256("exe"), 3}, {"ui/main.html", sha256("abc"), 3}}, {}};
            write(fs::path(name) / launcher_exe, "exe");
            write(fs::path(name) / "ui/main.html", "abc");
            write(fs::path(name) / "version.txt", encode(record));
            return root / name;
        }
    };

    verification good(std::string_view bytes = "version")
    {
        return {sha256(bytes), {}};
    }

    disk_view populated()
    {
        disk_view view;
        view.state.current = "0.1.3";
        view.state.previous = "0.1.2";
        view.confirmed_folders = {view.state.current, view.state.previous};
        view.folders = {
            {"0.1.2", good("previous")}, {"0.1.3", good("current")},
            {"0.1.4-beta.2", good("older beta")}, {"0.1.4", good("stable")},
            {"0.1.4~2", good("stable copy")}, {"0.1.4.partial", good("partial")}
        };
        return view;
    }

    std::string seal(std::string text)
    {
        return text + "end " + sha256(text) + '\n';
    }
}

TEST(version_records_validate_checksum_and_schemas)
{
    const pointer value{"0.1.3", "0.1.2", "0.1.4~2", 1, "0123456789abcdef", {{"0.1.4", sha256("part"), "damaged:ui/A B"}}};
    const auto bytes = encode(value);
    CHECK(parse_pointer(bytes) == value);
    CHECK(!parse_pointer(""));
    CHECK(!parse_pointer(bytes.substr(0, bytes.size() - 4)));
    CHECK(!parse_pointer(bytes + '\n'));
    CHECK(!parse_pointer(std::string(9000, 'x')));
    auto altered = bytes;
    altered[35] = 'x';
    CHECK(!parse_pointer(altered));
    const auto body = bytes.substr(0, bytes.rfind("end "));
    for (const auto* line : {"current 0.1.3\n", "unknown 3\n", "failed 0.1.4 33 damaged\n"})
        CHECK(!parse_pointer(seal(body + line)));
    auto invalid = value;
    invalid.trial = "../0.1.3";
    CHECK(!parse_pointer(encode(invalid)));
    invalid.trial.clear();
    CHECK(!parse_pointer(encode(invalid)));
    invalid.trial = invalid.current;
    CHECK(!parse_pointer(encode(invalid)));

    const request incoming{"fedcba9876543210", "0.1.2~2", "switch-to-stable"};
    CHECK(parse_request(encode(incoming)) == incoming);
    CHECK(!parse_request(encode(request{incoming.id, incoming.trial, "automatic"})));
    CHECK(!parse_request(encode(request{"bad", incoming.trial, "retry"})));
    CHECK(!parse_request(encode(request{incoming.id, "0.1.4.partial", "retry"})));
    CHECK(request_id().size() == 16);
    CHECK(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(version_inventory_and_paths_are_unambiguous)
{
    const auto hash = sha256("abc");
    inventory value{"0.1.3-beta.1", "beta", 1, {{launcher_exe, hash, 3}}, {}};
    const auto parsed = parse_inventory(encode(value));
    CHECK(parsed && parsed->version == value.version && parsed->files == value.files);
    const auto body = encode(value);
    CHECK(!parse_inventory(body.substr(0, body.size() - 4)));
    for (const auto* path : {"/absolute", "../escape", "D:/escape", "dir/../escape", "dir/./x", "NUL.txt", "a/b.",
        "a//b", "a\\b", "a/file ", "a/COM1", "CON .txt"})
    {
        CHECK(!safe_relative(path));
        value.files.resize(1);
        value.files.push_back({path, hash, 3});
        CHECK(!parse_inventory(encode(value)));
    }
    value.files = {{launcher_exe, hash, 3}, {"UI/x", hash, 3}, {"ui/X", hash, 3}};
    CHECK(!parse_inventory(encode(value)));
    value.files.back().path = "UI";
    CHECK(!parse_inventory(encode(value)));
    value.files.back().path = "version.txt";
    CHECK(!parse_inventory(encode(value)));
    CHECK(valid_folder("0.1.3-beta.2~2"));
    CHECK(!valid_folder("0.1.3~01"));
    CHECK(!valid_folder("01.1.3"));
    CHECK(!valid_folder("0.1.3.partial"));
    CHECK(!valid_folder("0.1.3-beta.01"));
    CHECK(!valid_folder("0.1.3+metadata"));
    CHECK(compare("0.1.3", "0.1.3-beta.3") > 0);
    CHECK(compare("0.1.3-beta.10", "0.1.3-beta.3") > 0);
    CHECK(compare("0.1.3-beta.3", "0.1.3-beta.3~2") == 0);
}

TEST(version_quick_full_and_partial_checks_are_distinct)
{
    fixture f;
    auto folder = f.complete("0.1.3");
    CHECK(verify(folder, check_mode::quick));
    CHECK(verify(folder, check_mode::full));
    f.write("0.1.3/ui/main.html", "abd");
    CHECK(verify(folder, check_mode::quick));
    CHECK(verify(folder, check_mode::full).bad_file == "ui/main.html");
    f.write("0.1.3/ui/main.html", "longer");
    CHECK(verify(folder, check_mode::quick).bad_file == "ui/main.html");
    f.write("0.1.3/ui/main.html", "abc");
    f.write("0.1.3/new.txt", "unexpected");
    CHECK(verify(folder, check_mode::full).bad_file == "new.txt");
    fs::remove(folder / "new.txt");
    f.write("0.1.3/.writing", "");
    CHECK(!verify(folder, check_mode::full));
    const auto partial = f.root / "0.1.3.partial";
    fs::rename(folder, partial);
    CHECK(verify(partial, check_mode::partial));
    CHECK(!verify(partial, check_mode::quick));
    fs::remove(partial / launcher_exe);
    CHECK(verify(partial, check_mode::partial).bad_file == launcher_exe);
}

TEST(version_pointer_recovers_valid_backup_without_replacing_it)
{
    fixture f;
    const pointer old{"0.1.2"}, current{"0.1.3", "0.1.2"};
    CHECK(commit(f.root, old, current));
    auto loaded = load_pointer(f.root);
    CHECK(loaded.source == pointer_source::primary && loaded.value == current);
    CHECK(!loaded.repair_backup && !loaded.repair_primary);
    CHECK(repair_pointer(f.root, loaded));
    CHECK(read_record(f.root / "pointer.bak") == encode(old));

    for (const std::string bytes : {std::string{}, std::string("garbage"), encode(current).substr(0, 77)})
    {
        f.write("pointer.txt", bytes);
        loaded = load_pointer(f.root);
        CHECK(loaded.source == pointer_source::backup && loaded.value == old && loaded.repair_primary);
        CHECK(repair_pointer(f.root, loaded));
        CHECK(read_record(f.root / "pointer.bak") == encode(old));
        CHECK(read_record(f.root / "pointer.txt") == encode(old));
    }
    f.write("pointer.bak", "corrupt");
    loaded = load_pointer(f.root);
    CHECK(loaded.source == pointer_source::primary && loaded.repair_backup);
    CHECK(repair_pointer(f.root, loaded));
    f.write("pointer.txt", "");
    f.write("pointer.bak", "");
    CHECK(load_pointer(f.root).source == pointer_source::empty);
}

TEST(version_commit_does_not_back_up_corrupt_state_or_replace_on_write_failure)
{
    fixture f;
    const pointer old{"0.1.2"}, current{"0.1.3", "0.1.2"};
    CHECK(commit(f.root, old, current));
    const pointer invalid{"../escape"};
    CHECK(!commit(f.root, invalid, current));
    CHECK(read_record(f.root / "pointer.bak") == encode(old));
    fs::create_directory(f.root / "pointer.txt.tmp");
    CHECK(!commit(f.root, current, pointer{"0.1.4", "0.1.3"}));
    CHECK(read_record(f.root / "pointer.txt") == encode(current));
    CHECK(read_record(f.root / "pointer.bak") == encode(current));
    CHECK(fs::is_directory(f.root / "pointer.txt.tmp"));
}

TEST(version_decisions_cover_recovery_request_arrival_and_launch_paths)
{
    auto view = populated();
    view.where = step::validate_names;
    CHECK(decide(view).next == action::next && !decide(view).save);
    view.folders["0.1.3"].bad_file = launcher_exe;
    auto result = decide(view);
    CHECK(result.save && result.save->current == "0.1.2" && result.save->previous.empty());
    CHECK(result.save && result.save->failed.front().reason == std::string("damaged:") + launcher_exe);
    view.state.trial = "0.1.2";
    view.state.starts = 1;
    result = decide(view);
    CHECK(result.save && result.save->current == "0.1.2" && result.save->trial.empty() && !result.save->starts);
    CHECK(result.save && parse_pointer(encode(*result.save)));
    view.folders["0.1.2"].bad_file = "version.txt";
    view.state.trial = "0.1.5";
    view.state.starts = 1;
    result = decide(view);
    CHECK(result.save && result.save->current.empty() && result.save->trial.empty() && result.save->starts == 0);

    view = populated();
    view.where = step::process_request;
    CHECK(!decide(view).remove_request);
    view.request_present = true;
    CHECK(decide(view).invalid_request);
    view.incoming = request{"1234567890abcdef", "0.1.2", "switch-to-stable"};
    result = decide(view);
    CHECK(result.save && result.save->trial == "0.1.2" && result.save->last_request == view.incoming->id);
    CHECK(result.remove_request && !result.invalid_request);
    view.state = *result.save;
    CHECK(decide(view).remove_request && !decide(view).save);
    view.incoming = request{"0000000000000001", "0.1.4-beta.2", "switch-to-stable"};
    CHECK(decide(view).invalid_request);
    view.incoming->trial = "0.1.5";
    CHECK(decide(view).invalid_request);

    view = populated();
    view.where = step::arrival;
    CHECK(decide(view).next == action::scan && decide(view).folder == "0.1.4~2");
    view.checked_arrival = "0.1.4~2";
    view.arrival_result = view.folders.at(view.checked_arrival);
    result = decide(view);
    CHECK(result.save && result.save->trial == view.checked_arrival && result.save->current == "0.1.3");
    view.state = *result.save;
    CHECK(!decide(view).save && decide(view).next == action::next);
    view.state.trial.clear();
    view.arrival_result.bad_file = "ui/main.html";
    view.state = *decide(view).save;
    CHECK(decide(view).folder == "0.1.4");
    view.folders["0.1.4~2"] = good("changed seal");
    view.checked_arrival.clear();
    CHECK(decide(view).folder == "0.1.4~2");
    view.state.current = "0.1.5";
    CHECK(decide(view).next == action::next);

    view = populated();
    view.where = step::choose;
    CHECK(decide(view).next == action::launch_current);
    view.state.trial = "0.1.4";
    result = decide(view);
    CHECK(result.next == action::launch_trial && result.save && result.save->starts == 1);
    CHECK(failed_commit(view).next == action::launch_current);
    view.state = *result.save;
    view.state.current.clear();
    CHECK(failed_commit(view).folder == view.state.previous);
    view.state.previous.clear();
    CHECK(failed_commit(view).next == action::dialog_write);
    view.state.starts = trial_starts;
    for (const std::string_view reason : {"could-not-start:2", "exit:0", "exit:1", "unconfirmed-timeout", ""})
    {
        view.last_reason = reason;
        const auto exhausted = decide(view);
        CHECK(exhausted.save && exhausted.save->trial.empty() && exhausted.save->starts == 0);
        CHECK(exhausted.save && exhausted.save->failed.front().reason == (reason.empty() ? "interrupted-start" : reason));
    }
    view.state.trial.clear();
    view.state.starts = 0;
    CHECK(decide(view).next == action::dialog_none);
    view.state.current = "0.1.3";
    view.launches = launch_limit;
    CHECK(decide(view).next == action::dialog_current);

    view = populated();
    view.state.trial = "0.1.4";
    view.state.starts = 1;
    view.where = step::confirmed;
    result = decide(view);
    CHECK(result.next == action::wait && result.save && result.save->current == "0.1.4" && result.save->previous == "0.1.3");
    CHECK(result.save && result.save->trial.empty() && !result.save->starts);
    CHECK(failed_commit(view).next == action::wait);
    view.where = step::choose;
    view.prepare = true;
    CHECK(decide(view).next == action::prepared && decide(view).folder == "0.1.3");
    view.state.current.clear();
    CHECK(decide(view).next == action::prepared && decide(view).folder == "0.1.4");
    view.state.starts = trial_starts;
    CHECK(decide(view).next == action::dialog_none);
    view.where = step::retry;
    view.state.failed = {{"0.1.8", sha256("bad"), "damaged"}, {"0.1.4", sha256("good"), "exit:1"}};
    CHECK(decide(view).folder == "0.1.4");
}


namespace
{
    fs::path test_program()
    {
        std::wstring path(32768, L'\0');
        const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size || size >= path.size()) throw std::runtime_error("Could not locate test program.");
        path.resize(size);
        return path;
    }

    std::pair<DWORD, std::string> prepare(const fs::path& top)
    {
        const auto exe = top / "show23-pc.exe";
        HANDLE reader{}, writer{};
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        if (!CreatePipe(&reader, &writer, &security, 0)) throw std::runtime_error("Could not create test pipe.");
        bootstrap::handle input(reader), output(writer);
        if (!SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("Could not configure pipe.");
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = startup.hStdError = writer;
        PROCESS_INFORMATION info{};
        auto cmd = bootstrap::quote(exe.native()) + L" --prepare";
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
            nullptr, top.c_str(), &startup, &info))
            throw std::runtime_error("Could not start disposable bootstrap.");
        bootstrap::handle process(info.hProcess), thread(info.hThread);
        output.reset(nullptr);
        if (WaitForSingleObject(process.get(), 15000) != WAIT_OBJECT_0)
        {
            TerminateProcess(process.get(), 1);
            WaitForSingleObject(process.get(), 5000);
            throw std::runtime_error("Disposable bootstrap did not exit.");
        }
        DWORD code{}, count{};
        GetExitCodeProcess(process.get(), &code);
        std::string text(128, '\0');
        ReadFile(input.get(), text.data(), static_cast<DWORD>(text.size()), &count, nullptr);
        text.resize(count);
        return {code, text};
    }
}

std::optional<int> fake_bootstrap_child(const std::span<wchar_t*> args)
{
    if (std::wstring_view(args.back()) != L"--bootstrap-fixture") return {};
    if (args.size() != 13) return 3;
    if (std::wstring_view(args[1]) != L"--bootstrap" || std::wstring_view(args[2]) != L"C:\\fake bootstrap.exe" ||
        std::wstring_view(args[3]) != L"--bootstrap-version" || std::wstring_view(args[4]) != L"1" ||
        std::wstring_view(args[5]) != L"--folder" || std::wstring_view(args[6]) != L"0.1.3~2" ||
        std::wstring_view(args[7]) != L"--trial" || std::wstring_view(args[8]) != L"--confirm-event" ||
        std::wstring_view(args[10]) != L"arg with \\\" and trailing\\" ||
        fs::current_path() != test_program().parent_path()) return 3;
    const auto excluded = reinterpret_cast<HANDLE>(std::wcstoull(args[11], nullptr, 10));
    if (SetEvent(excluded)) return 4;
    const auto inherited = reinterpret_cast<HANDLE>(std::wcstoull(args[9], nullptr, 10));
    return SetEvent(inherited) ? 0 : 5;
}

TEST(bootstrap_prepare_repairs_and_consumes_requests_without_launching)
{
    fixture top;
    const auto boot = test_program().parent_path() / "show23-pc-bootstrap.exe";
    fs::copy_file(boot, top.root / "show23-pc.exe");
    const auto layout = top.root / "show23-pc/versions";

    fixture versions;
    versions.complete("0.1.3");
    versions.complete("0.1.4");
    versions.write("0.1.4/ui/main.html", "abd");
    fs::create_directories(layout.parent_path());
    fs::copy(versions.root, layout, fs::copy_options::recursive);
    auto [code, output] = prepare(top.root);
    CHECK(code == 0 && output == "0.1.3\r\n");
    const auto first = load_pointer(layout).value;
    CHECK(first.current.empty() && first.trial == "0.1.3" && first.starts == 0);
    CHECK(first.failed.size() == 1 && first.failed.front().folder == "0.1.4");
    CHECK(read_record(layout / "bootstrap.log").find("rebuilt") == std::string::npos);
    CHECK(!fs::exists(layout / notice_file));

    const pointer prior{"0.1.3"};
    CHECK(write_confirmation(layout, prior.current, verify(layout / prior.current, check_mode::startup).fingerprint));
    const request damaged{"1234567890abcdef", "0.1.4", "retry"};
    CHECK(write_record(layout / "pointer.bak", encode(prior)));
    top.write("show23-pc/versions/pointer.txt", "torn");
    top.write("show23-pc/versions/pointer.txt.tmp", "left over");
    CHECK(write_record(layout / "request.txt", encode(damaged)));
    std::tie(code, output) = prepare(top.root);
    CHECK(code == 0 && output == "0.1.3\r\n");
    const auto rejected = load_pointer(layout).value;
    CHECK(rejected.current == prior.current && rejected.trial.empty() && rejected.last_request.empty());
    CHECK(!fs::exists(layout / "request.txt"));

    fs::copy(versions.complete("0.1.5"), layout / "0.1.5", fs::copy_options::recursive);
    const request incoming{"1234567890abcdee", "0.1.5", "retry"};
    CHECK(write_record(layout / "request.txt", encode(incoming)));
    std::tie(code, output) = prepare(top.root);
    CHECK(code == 0 && output == "0.1.3\r\n");
    const auto consumed = load_pointer(layout).value;
    CHECK(consumed.current == "0.1.3" && consumed.trial == incoming.trial && consumed.starts == 0);
    CHECK(consumed.last_request == incoming.id && !fs::exists(layout / "request.txt"));
    CHECK(!fs::exists(layout / "pointer.txt.tmp"));

    auto counted = consumed;
    counted.starts = 1;
    CHECK(commit(layout, consumed, counted));
    CHECK(write_record(layout / "request.txt", encode(incoming)));
    std::tie(code, output) = prepare(top.root);
    CHECK(code == 0 && output == "0.1.3\r\n");
    CHECK(load_pointer(layout).value == counted && !fs::exists(layout / "request.txt"));
}

TEST(bootstrap_counts_createprocess_failure_and_passes_only_the_confirm_handle)
{
    fixture f;
    const auto invalid = f.complete("0.1.3") / launcher_exe;
    disk_view view;
    view.state.trial = "0.1.3";
    view.where = step::choose;
    view.folders[view.state.trial] = verify(invalid.parent_path(), check_mode::full);
    while (view.state.starts < trial_starts)
    {
        const auto selected = decide(view);
        if (!selected.save) { CHECK(false); return; }
        CHECK(selected.next == action::launch_trial && selected.save->starts == view.state.starts + 1);
        CHECK(commit(f.root, view.state, *selected.save));
        view.state = load_pointer(f.root).value;
        bootstrap::child child;
        const auto error = child.launch(invalid, L"C:\\fake bootstrap.exe", selected.folder, true, {});
        CHECK(error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND);
        if (error == ERROR_SUCCESS)
        {
            TerminateJobObject(child.job.get(), ERROR_PROCESS_ABORTED);
            WaitForSingleObject(child.process.get(), 5000);
            return;
        }
        view.last_reason = "could-not-start:" + std::to_string(error);
    }
    const auto rejected = decide(view);
    CHECK(rejected.save && rejected.save->trial.empty() && rejected.save->failed.front().reason == view.last_reason);

    const auto child_file = f.root / "fixture child.exe";
    fs::copy_file(test_program(), child_file);
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    const bootstrap::handle excluded(CreateEventW(&inheritable, TRUE, FALSE, nullptr));
    CHECK(excluded);
    bootstrap::child child;
    const auto error = child.launch(child_file, L"C:\\fake bootstrap.exe", "0.1.3~2", true,
        {L"arg with \\\" and trailing\\", std::to_wstring(reinterpret_cast<ULONG_PTR>(excluded.get())), L"--bootstrap-fixture"});
    CHECK(error == ERROR_SUCCESS);
    if (error != ERROR_SUCCESS) return;
    const auto status = WaitForSingleObject(child.process.get(), 10000);
    CHECK(status == WAIT_OBJECT_0);
    if (status != WAIT_OBJECT_0)
    {
        TerminateJobObject(child.job.get(), ERROR_TIMEOUT);
        WaitForSingleObject(child.process.get(), 5000);
        return;
    }
    DWORD code{};
    CHECK(GetExitCodeProcess(child.process.get(), &code) && code == 0);
    CHECK(WaitForSingleObject(child.confirm.get(), 0) == WAIT_OBJECT_0);
}

TEST(version_notices_accept_only_the_one_shot_format)
{
    CHECK(parse_notices(encode_notices(15)) == 15);
    CHECK(parse_notices(encode_notices(9)) == 9);
    CHECK(!parse_notices(""));
    CHECK(!parse_notices("show23-pc notice 1\nrebuilt"));
    CHECK(!parse_notices("show23-pc notice 1\nrebuilt\nunknown\n"));
    CHECK(!parse_notices("show23-pc notice 1\nrebuilt\nrebuilt\n"));
}

TEST(versioned_layout_resolves_stable_roots_from_a_version_slot)
{
    const fs::path top = L"Q:\\Fixture Folder";
    const auto versions_root = top / L"show23-pc\\versions";
    const auto slot = versions_root / "0.1.3-beta.2~3";
    const auto layout = versioned_start::locate(slot / launcher_exe);
    CHECK(layout && layout.release == slot && layout.versions == versions_root);
    CHECK(layout.bootstrap == top / "show23-pc.exe" && layout.folder == "0.1.3-beta.2~3");
    CHECK(!versioned_start::locate(top / "show23-pc.exe"));
    CHECK(!versioned_start::locate(versions_root / "0.1.3.partial" / launcher_exe));
}

TEST(version_record_readers_allow_an_atomic_replacement_handle)
{
    fixture f;
    const auto path = f.root / "pointer.txt";
    pointer record{"0.1.3"};
    CHECK(write_record(path, encode(record)));
    bootstrap::handle rename_handle(CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!rename_handle) { CHECK(false); return; }
    CHECK(parse_pointer(read_record(path)) == record);
}

TEST(bootstrap_rebuilt_notice_requires_prior_record_history)
{
    fixture top;
    const auto boot = test_program().parent_path() / "show23-pc-bootstrap.exe";
    fs::copy_file(boot, top.root / "show23-pc.exe");
    const auto layout = top.root / "show23-pc/versions";
    fixture payload;
    payload.complete("0.1.3");
    fs::create_directories(layout.parent_path());
    fs::copy(payload.root, layout, fs::copy_options::recursive);

    const auto first = prepare(top.root);
    CHECK(first.first == 0 && first.second == "0.1.3\r\n");
    CHECK(!fs::exists(layout / notice_file));

    fs::remove(layout / "pointer.txt");
    fs::remove(layout / "pointer.bak");
    const auto recovered = prepare(top.root);
    CHECK(recovered == first);
    CHECK(parse_notices(read_record(layout / notice_file)) == 1);
    CHECK(load_pointer(layout).value.trial == "0.1.3");
}

TEST(version_confirmation_and_channel_records_validate_independently)
{
    fixture f;
    const auto fingerprint = sha256("inventory");
    CHECK(read_confirmation(f.root, "0.1.3").empty());
    CHECK(write_confirmation(f.root, "0.1.3", fingerprint));
    CHECK(read_confirmation(f.root, "0.1.3") == fingerprint);
    CHECK(write_confirmation(f.root, "0.1.3", fingerprint));
    CHECK(!write_confirmation(f.root, "../elsewhere", fingerprint));
    CHECK(!write_confirmation(f.root, "0.1.3", "bad digest"));
    f.write("confirmed/0.1.3.txt", seal("show23-pc confirmed 1\nfolder 0.1.4\ninventory " + fingerprint + "\n"));
    CHECK(read_confirmation(f.root, "0.1.3").empty());
    f.write("confirmed/0.1.3.txt", "torn");
    CHECK(read_confirmation(f.root, "0.1.3").empty());
    CHECK(!read_channel(f.root));
    CHECK(write_channel(f.root, update_channel::stable));
    CHECK(read_channel(f.root) == update_channel::stable);
    f.write("channel.txt", "stabl");
    CHECK(!read_channel(f.root));
    CHECK(write_channel(f.root, update_channel::beta));
    CHECK(read_channel(f.root) == update_channel::beta);
}

TEST(version_startup_hashes_same_size_launcher_and_cef_damage)
{
    fixture f;
    const auto slot = f.complete("0.1.3");
    auto manifest = *parse_inventory(read_record(slot / "version.txt"));
    manifest.files.push_back({"cef/release/libcef.dll", sha256("cef"), 3});
    f.write("0.1.3/cef/release/libcef.dll", "cef");
    f.write("0.1.3/version.txt", encode(manifest));
    CHECK(verify(slot, check_mode::startup));
    f.write("0.1.3/cef/release/libcef.dll", "bad");
    CHECK(verify(slot, check_mode::quick));
    CHECK(verify(slot, check_mode::startup).bad_file == "cef/release/libcef.dll");
    f.write("0.1.3/cef/release/libcef.dll", "cef");
    f.write(fs::path("0.1.3") / launcher_exe, "bad");
    CHECK(verify(slot, check_mode::quick));
    CHECK(verify(slot, check_mode::startup).bad_file == launcher_exe);
}

TEST(version_decisions_require_confirmation_for_fallback_and_replaced_current)
{
    auto view = populated();
    view.where = step::validate_names;
    view.confirmed_folders.erase(view.state.current);
    auto decision = decide(view);
    CHECK(decision.save && decision.save->current == "0.1.2" && decision.save->trial == "0.1.3");
    CHECK(failed_commit(view).next == action::launch_current && failed_commit(view).folder == "0.1.2");
    view.confirmed_folders.clear();
    decision = decide(view);
    CHECK(decision.save && decision.save->current.empty() && decision.save->trial == "0.1.3");
    CHECK(failed_commit(view).next == action::dialog_write);
}

TEST(version_stable_choice_filters_beta_and_budget_preserves_confirmed_start)
{
    auto view = populated();
    view.folders.erase("0.1.4");
    view.folders.erase("0.1.4~2");
    view.where = step::arrival;
    CHECK(decide(view).folder == "0.1.4-beta.2");
    view.stable_only = true;
    CHECK(decide(view).next == action::next);
    view.where = step::process_request;
    view.request_present = true;
    view.incoming = request{"0000000000000001", "0.1.4-beta.2", "retry"};
    CHECK(decide(view).invalid_request);
    view.where = step::validate_names;
    view.state.trial = view.incoming->trial;
    CHECK(decide(view).save && decide(view).save->trial.empty());
    view.stable_only = false;
    view.where = step::choose;
    view.launches = launch_limit - 1;
    CHECK(decide(view).next == action::launch_current && !decide(view).save);
    view.state.current.clear();
    CHECK(decide(view).next == action::launch_trial);
}
