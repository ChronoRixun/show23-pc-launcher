#include "process.hpp"
#include "ui.hpp"
#include <versions/bootstrap_update_fixture.hpp>
#include <versions/decision.hpp>
#include <versions/instance.hpp>
#include <versions/update_fixture.hpp>
#include <utils/named_mutex.hpp>

#include <algorithm>
#include <chrono>

namespace bootstrap
{
    using namespace versions;
    using namespace std::chrono_literals;

    namespace
    {
        constexpr DWORD confirmation_timeout = 120000;
        constexpr DWORD restart = 75;
        struct session
        {
            fs::path executable, root;
            arguments args;
            disk_view view;
            bool retry_used{}, fixture_run{};
            std::string newest;
            unsigned notices{};
            bool empty_on_load{};
            std::optional<pointer> pending_fallback;
            FILETIME prior_log_time{};

            bool newly_extracted(const fs::path& folder) const
            {
                WIN32_FILE_ATTRIBUTE_DATA data{};
                // ZIP entries retain old modification timestamps. A fresh slot
                // has a new directory creation time.
                return GetFileAttributesExW(folder.c_str(), GetFileExInfoStandard, &data) &&
                    CompareFileTime(&data.ftCreationTime, &prior_log_time) > 0;
            }

            void record_notice(std::string_view id)
            {
                const auto item = std::ranges::find(notice_ids, id);
                if (item != std::end(notice_ids)) notices |= 1u << (item - std::begin(notice_ids));
            }

            void publish_notices()
            {
                if (!notices) return;
                const auto file = root / notice_file;
                const auto existing = parse_notices(read_record(file, 256));
                if (!write_record(file, encode_notices(existing | notices)))
                    log(root, "could not write the Home notice");
                notices = 0;
            }

            void check(const std::string& folder, check_mode mode = check_mode::quick)
            {
                if (folder.empty() || (mode == check_mode::quick && view.folders.contains(folder))) return;
                const auto version = folder_version(folder);
                if (newest.empty() || compare(version, newest) > 0) newest = version;
                view.folders[folder] = verify(root / folder, mode);
            }

            void check_confirmed(const std::string& folder, bool is_current)
            {
                if (folder.empty()) return;
                view.folders[folder] = verify_confirmed(root, folder);
                const auto& existing = view.folders.at(folder);
                if (existing) view.confirmed_folders.insert(folder);
                else if (is_current && existing.needs_confirmation)
                    check(folder, check_mode::full); // B3 may turn the replaced current into a trial.
            }

            bool save_pointer(const decision& selected) const
            {
#ifdef SHOW23_UPDATE_FIXTURE
                if (fixture_run)
                {
                    std::string_view label;
                    switch (view.where)
                    {
                    case step::validate_names: label = "B3"; break;
                    case step::process_request: label = "B4"; break;
                    case step::arrival: label = view.arrival_result ? "B5-arrival" : "B5-damaged"; break;
                    case step::choose: label = selected.next == action::launch_trial ? "B6-count" : "B6-failed"; break;
                    case step::confirmed: label = "B8-confirm"; break;
                    default: break;
                    }
                    return fixture::commit_pointer(root, view.state, *selected.save, label);
                }
#endif
                return commit(root, view.state, *selected.save);
            }

            decision apply(decision selected)
            {
                if (selected.save)
                {
                    if (!save_pointer(selected))
                    {
                        const auto error = GetLastError();
                        log(root, "couldn't record the update's start or state: " + std::to_string(error));
                        if (view.where == step::choose && selected.next == action::launch_trial)
                            record_notice("counter-write");
                        const auto fallback = failed_commit(view);
                        if (!fallback.folder.empty() && fallback.folder != view.state.current)
                        {
                            log(root, "using verified previous for this start");
                            record_notice("fallback-write");
                            if (selected.save->current == fallback.folder) pending_fallback = *selected.save;
                        }
                        if (fallback.next == action::dialog_write && !args.prepare) write_dialog(error);
                        return fallback;
                    }
                    view.state = *selected.save;
                    if (empty_on_load)
                    {
                        log(root, "version record created");
                        empty_on_load = false;
                    }
                    if (selected.remove_request) fixture::kill_at(root.parent_path(), "B4-request-commit");
                }
                if (selected.invalid_request)
                {
                    log(root, "invalid-request");
                    record_notice("invalid-request");
                }
                if (selected.remove_request && plain_path(root / "request.txt"))
                    DeleteFileW((root / "request.txt").c_str());
                return selected;
            }

            void write_dialog(DWORD error)
            {
                wchar_t message[512]{};
                const auto detail = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    nullptr, error, 0, message, static_cast<DWORD>(std::size(message)), nullptr)
                    ? std::wstring(message) : L"Windows error " + std::to_wstring(error);
                dialog(root, L"show23-pc can't write the version record",
                    root.native() + L"\n" + detail +
                    L"\nMove the launcher folder somewhere you can write to, outside Program Files.",
                    newest, false, {}, false, false);
            }

            bool had_version_record() const
            {
                for (const auto* name : {"pointer.txt", "pointer.bak"})
                {
                    std::error_code error;
                    if (fs::exists(fs::symlink_status(root / name, error))) return true;
                }
                // If both files vanished, the bootstrap's own history can still distinguish
                // a previously created record from an untouched download.
                for (const auto* name : {"bootstrap.log", "bootstrap.log.old"})
                {
                    const auto history = read_record(root / name, 256 * 1024);
                    if (history.find("Z version record created\r\n") != history.npos ||
                        history.find("Z launch ") != history.npos) return true;
                }
                return false;
            }

            decision load()
            {
                const auto loaded = load_pointer(root);
                view.state = loaded.value;
                view.folders.clear();
                view.confirmed_folders.clear();
                view.published_folders.clear();
                view.explicit_request = false;
                pending_fallback.reset();
                view.recovery_history.clear();
                newest.clear();
                view.last_reason.clear();
                view.rejected_arrivals.clear();
                view.prepare = args.prepare;
                empty_on_load = loaded.source == pointer_source::empty;
                if (empty_on_load && had_version_record())
                {
                    log(root, "rebuilt");
                    record_notice("rebuilt");
                }
                for (const auto* name : {"pointer.txt.tmp", "pointer.bak.tmp", "request.txt.tmp", "notice.txt.tmp"})
                    if (plain_path(root / name)) DeleteFileW((root / name).c_str());
                if (!repair_pointer(root, loaded)) log(root, "could not repair the version record");

                check_confirmed(view.state.current, true);
                check_confirmed(view.state.previous, false);
                check(view.state.trial, check_mode::full);
                view.where = step::validate_names;
                if (auto selected = apply(decide(view)); selected.next != action::next) return selected;
                if (view.state.current.empty()) view.recovery_history = confirmation_history(root);
                const auto channel_choice = read_channel(root);
                const auto last_running = !view.state.current.empty() ? view.state.current :
                    (view.recovery_history.empty() ? std::string{} : view.recovery_history.front().folder);
                view.stable_only = channel_choice ? *channel_choice == update_channel::stable :
                    !last_running.empty() && folder_version(last_running).find('-') == std::string_view::npos;

                const auto request_path = root / "request.txt";
                const auto bytes = read_record(request_path);
                std::error_code error;
                view.request_present = fs::exists(fs::symlink_status(request_path, error));
                view.incoming = parse_request(bytes);
                if (view.incoming) check(view.incoming->trial, check_mode::full);
                view.where = step::process_request;
                auto selected = decide(view);
                view.explicit_request = selected.save && selected.remove_request;
                return apply(std::move(selected));
            }

            decision arrival()
            {
                if (view.explicit_request && !view.state.trial.empty()) return {action::next};
                view.published_folders.clear();
                std::error_code error;
                for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
                {
                    const auto filename = narrow(it->path().filename().native());
                    std::error_code attributes_error;
                    if (!valid_folder(filename) || !plain_path(it->path()) || !it->is_directory(attributes_error)) continue;
                    check(filename);
                    const auto& verified = view.folders.at(filename);
                    if (verified && read_record(root / (filename + ".arrival.txt"), 65) == verified.fingerprint + "\n")
                        view.published_folders.insert(filename);
                    if (!view.state.current.empty() && filename != view.state.previous &&
                        compare(filename, view.state.current) < 0 && view.folders[filename] && newly_extracted(it->path()))
                    {
                        log(root, filename + ": ignored older release");
                        record_notice("ignored-older");
                    }
                }
                view.where = step::arrival;
                view.checked_arrival.clear();
                for (auto selected = decide(view); selected.next == action::scan; selected = decide(view))
                {
                    view.checked_arrival = selected.folder;
                    view.arrival_result = check_arrival(root / selected.folder, args.prepare);
                    if (view.arrival_result)
                    {
                        view.last_reason.clear();
                        view.folders[view.checked_arrival] = view.arrival_result;
                        auto checked = decide(view);
                        if (checked.next == action::scan) continue;
                        return apply(std::move(checked));
                    }
                    log(root, selected.folder + (view.arrival_result.retryable ? ": temporarily unreadable:" : ": damaged:") +
                        view.arrival_result.bad_file);
                    selected = apply(decide(view));
                    if (selected.next != action::next) return selected;
                    view.rejected_arrivals.insert(view.checked_arrival);
                }
                return {action::next};
            }

            decision choose()
            {
                view.where = step::choose;
                return apply(decide(view));
            }

            bool retry()
            {
                view.where = step::retry;
                const auto choice = decide(view);
                std::wstring detail = L"No complete launcher version could open.";
                for (const auto& failure : view.state.failed)
                    detail += L"\n" + widen(failure.folder + ": " + failure.reason);
                if (dialog(root, L"show23-pc could not open", std::move(detail), newest,
                    false, {}, !retry_used && view.launches < launch_limit &&
                    !choice.folder.empty()) != answer::retry) return false;
                retry_used = true;
                const auto id = request_id();
                if (!id.empty() && write_record(root / "request.txt", encode(request{id, choice.folder, "retry"}))) return true;
                write_dialog(id.empty() ? ERROR_GEN_FAILURE : GetLastError());
                return false;
            }

            DWORD launch(const decision& selected)
            {
                auto folder = selected.folder;
                auto trial = selected.next == action::launch_trial;
                bool use_previous = false;
                for (;;)
                {
                    ++view.launches;
                    log(root, "launch " + folder + (trial ? " (trial)" : ""));
                    child app;
                    const auto error = app.launch(root / folder / launcher_exe, executable, folder, trial, args.flags);
                    DWORD code{};
                    bool timeout = false, confirmed = false;
                    std::string reason;
                    if (error) reason = "could-not-start:" + std::to_string(error);
                    else
                    {
#ifdef SHOW23_UPDATE_FIXTURE
                        if (trial && fixture_run && fixture::setting("SHOW23_FIXTURE_KILL_AT") == "B7-trial-running")
                        {
                            // The app writes its trace only after taking the launcher mutex.
                            const auto marker = " " + std::to_string(GetProcessId(app.process.get())) + "\n";
                            const auto trace = root.parent_path() / "fixture-launches.txt";
                            const auto deadline = GetTickCount64() + 4000;
                            while (read_record(trace, 65536).find(marker) == std::string::npos)
                            {
                                if (GetTickCount64() >= deadline || WaitForSingleObject(app.process.get(), 10) != WAIT_TIMEOUT)
                                {
                                    log(root, "fixture trial did not reach the launcher mutex");
                                    return 114;
                                }
                            }
                            fixture::kill_at(root.parent_path(), "B7-trial-running");
                        }
#endif
                        const HANDLE waiting[]{app.confirm.get(), app.process.get()};
                        const auto timeout_ms = fixture_run ? 2000 : confirmation_timeout;
                        const auto event = WaitForMultipleObjects(2, waiting, FALSE, timeout_ms);
                        timeout = event == WAIT_TIMEOUT;
                        confirmed = event == WAIT_OBJECT_0;
                        if (event == WAIT_FAILED) reason = "wait-failed:" + std::to_string(GetLastError());
                        if (trial && (timeout || event == WAIT_FAILED))
                        {
                            if (timeout) reason = "unconfirmed-timeout";
                            if (!TerminateJobObject(app.job.get(), timeout ? ERROR_TIMEOUT : ERROR_PROCESS_ABORTED) ||
                                WaitForSingleObject(app.process.get(), 5000) != WAIT_OBJECT_0)
                            {
                                log(root, folder + ": " + reason + "; could not stop the trial");
                                dialog(root, L"show23-pc could not stop the trial",
                                    L"Close the trial before reopening the launcher.", newest);
                                return 1;
                            }
                        }
                        if (!confirmed && !timeout && reason.empty())
                        {
                            GetExitCodeProcess(app.process.get(), &code);
                            if (!trial && !code) return 0;
                            reason = "exit:" + std::to_string(code);
                        }
                    }
                    if (!confirmed && trial)
                    {
                        view.last_reason = reason;
                        log(root, folder + ": " + reason);
                        return restart;
                    }
                    if (!confirmed)
                    {
                        const auto previous = use_previous ? std::string{} : previous_for_dialog(view, folder);
                        const auto choice = dialog(root, L"show23-pc " + widen(folder_version(folder)) + L" did not open",
                            timeout ? (fixture_run ? L"No window after 2 seconds." : L"No window after 120 seconds.") : widen(reason),
                            newest, timeout, previous);
                        if (choice == answer::wait)
                        {
                            const HANDLE waiting[]{app.confirm.get(), app.process.get()};
                            const auto event = WaitForMultipleObjects(2, waiting, FALSE, INFINITE);
                            confirmed = event == WAIT_OBJECT_0;
                            if (!confirmed)
                            {
                                GetExitCodeProcess(app.process.get(), &code);
                                if (code) dialog(root, L"show23-pc did not open", L"Exit code " + std::to_wstring(code), newest);
                                return code;
                            }
                        }
                        else if (choice == answer::previous)
                        {
                            // Never start an alternate while a timed-out current still owns the app mutex.
                            if (timeout && WaitForSingleObject(app.process.get(), 0) != WAIT_OBJECT_0)
                            {
                                dialog(root, L"The current launcher is still running",
                                    L"Close it before starting the previous version.", newest);
                                return 1;
                            }
                            use_previous = true;
                            folder = previous;
                            continue;
                        }
                        else return 1;
                    }

                    if (trial)
                    {
                        view.where = step::confirmed;
                        const auto promote = decide(view);
                        const auto before = view.folders.find(folder);
                        const bool can_capture = promote.save && before != view.folders.end();
                        const bool captured = can_capture && capture_confirmation(root, folder, before->second.fingerprint);
                        if (can_capture && !captured && GetLastError() == ERROR_READ_FAULT && view.state.starts)
                        {
                            auto retry = view.state;
                            --retry.starts;
                            if (commit(root, view.state, retry)) view.state = std::move(retry);
                            log(root, "confirmation temporarily unreadable for " + folder);
                        }
                        bool recorded{};
                        for (unsigned attempt = 0; captured && attempt < 3; ++attempt)
                        {
                            if (save_pointer(promote))
                            {
                                view.state = *promote.save;
                                view.confirmed_folders.insert(folder);
                                view.last_reason.clear();
                                recorded = true;
                                if (!app.release_trial_kill_on_close())
                                    log(root, "could not release trial supervision for " + folder);
                                break;
                            }
                            if (attempt != 2) Sleep(500);
                        }
                        log(root, recorded ? "confirmed " + folder : "could not record confirmation for " + folder);
                    }
                    const auto fallback_deadline = GetTickCount64() + 3000;
                    while (pending_fallback && GetTickCount64() < fallback_deadline &&
                           WaitForSingleObject(app.process.get(), 250) == WAIT_TIMEOUT)
                    {
                        const decision retry{action::next, *pending_fallback};
                        if (!save_pointer(retry)) continue;
                        view.state = *pending_fallback;
                        pending_fallback.reset();
                        log(root, "saved the previously blocked fallback record");
                    }
                    if (pending_fallback) log(root, "will retry the fallback record on the next start");
                    WaitForSingleObject(app.process.get(), INFINITE);
                    if (!GetExitCodeProcess(app.process.get(), &code)) return 1;
                    return code;
                }
            }

            int run()
            {
                WIN32_FILE_ATTRIBUTE_DATA previous_log{};
                const auto log_file = root / "bootstrap.log";
                if (plain_path(log_file) && GetFileAttributesExW(log_file.c_str(), GetFileExInfoStandard, &previous_log))
                    prior_log_time = previous_log.ftLastWriteTime;
                unsigned restarts{};
                for (;;)
                {
                    auto selected = load();
                    if (selected.next == action::dialog_write) return 1;
                    for (;;)
                    {
                        if (selected.next == action::next) selected = arrival();
                        if (selected.next == action::next) selected = choose();
                        if (selected.next == action::next) continue;
                        if (selected.next == action::dialog_write) return 1;
                        publish_notices();
                        if (selected.next == action::prepared)
                        {
                            if (print_folder(selected.folder)) return 0;
                            return 1;
                        }
                        if (selected.next == action::dialog_none)
                        {
                            if (!args.prepare && retry()) break;
                            if (args.prepare) log(root, "prepare: no runnable version was found");
                            return 1;
                        }
                        if (selected.next == action::dialog_current)
                        {
                            dialog(root, L"show23-pc did not open", L"The startup attempt limit was reached.", newest);
                            return 1;
                        }
                        const auto counted = view.state.trial;
                        const auto code = launch(selected);
                        if (code != restart) return static_cast<int>(code);
                        if (!counted.empty() && view.state.trial == counted && !view.last_reason.empty())
                        {
                            selected = choose();
                            continue;
                        }
                        if (++restarts <= 3 && view.launches < launch_limit) break;
                        dialog(root, L"show23-pc could not restart", L"The startup attempt limit was reached.", newest);
                        return 1;
                    }
                }
            }
        };

        int select_manual_folder(const fs::path& root)
        {
            if (!plain_path(root)) return 1;
            const auto loaded = load_pointer(root);
            const auto& state = loaded.value;
            for (const auto* name : {&state.current, &state.previous})
                if (!name->empty() && verify_confirmed(root, *name, false))
                    return print_folder(*name) ? 0 : 1;

            // A recorded confirmed slot that could not be verified must not be
            // bypassed by running its unverified replacement through manual mode.
            if (!state.current.empty() || !state.previous.empty()) return 1;
            if (!state.trial.empty())
                return verify(root / state.trial, check_mode::full) && print_folder(state.trial) ? 0 : 1;
            const auto history = confirmation_history(root);
            for (const auto& entry : history)
                if (verify_confirmed(root, entry.folder, false)) return print_folder(entry.folder) ? 0 : 1;
            if (!history.empty()) return 1;

            std::string newest;
            std::error_code error;
            for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
            {
                const auto name = narrow(it->path().filename().native());
                std::error_code status;
                if (valid_folder(name) && plain_path(it->path()) && it->is_directory(status) &&
                    (newest.empty() || compare_slots(newest, name) < 0)) newest = name;
            }
            return !error && !newest.empty() && verify(root / newest, check_mode::full) &&
                print_folder(newest) ? 0 : 1;
        }
    }

    int run()
    {
        std::wstring path(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return 1;
        path.resize(length);
        session app;
        app.executable = path;
        app.root = app.executable.parent_path() / "show23-pc/versions";
        app.args = parse_arguments();
        if (app.args.manual) return select_manual_folder(app.root);
        app.fixture_run = fixture::enabled(app.root.parent_path());

        const auto already_open = [&]
        {
            if (app.args.prepare || activate_running_window()) return 0;
            if (app.fixture_run) log(app.root, "show23-pc is already open");
            else MessageBoxW(nullptr, L"show23-pc is already open.", L"show23-pc", MB_OK | MB_ICONINFORMATION);
            return 0;
        };

        utils::named_mutex bootstrap_lock(bootstrap_mutex_name);
        if (!bootstrap_lock.try_lock(app.args.update_restart ? 30s : 3s)) return already_open();
        utils::named_mutex child_lock(launcher_mutex_name);
        // A killed bootstrap closes its trial job; let the OS finish releasing the app mutex.
        if (!child_lock.try_lock(3s)) return already_open();
        child_lock.unlock();

        if (!plain_path(app.root))
        {
            if (!app.args.prepare)
                dialog(app.root, L"show23-pc could not open", L"No readable versions folder was found. Re-extract the launcher ZIP.", {});
            return 1;
        }
        try { return app.run(); }
        catch (...)
        {
            if (!app.fixture_run) throw;
            log(app.root, "show23-pc could not read its version records");
            return 1;
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    try { return bootstrap::run(); }
    catch (...)
    {
        const auto args = bootstrap::parse_arguments();
        if (args.prepare || args.manual) return 1;
        MessageBoxW(nullptr, L"show23-pc could not read its version records.\nYour games, saves and settings are not affected.",
            L"show23-pc", MB_OK | MB_ICONERROR);
        return 1;
    }
}
