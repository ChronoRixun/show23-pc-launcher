#include "std_include.hpp"
#include "setup_run.hpp"
#include "input_paths.hpp"

#include <utils/named_mutex.hpp>
#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>

namespace show23
{
    namespace
    {
        using namespace input_paths;

        location ensure_directory(const fs::path& path)
        {
            auto directory = inspect(path, true);
            if (directory.error != path_error::none || directory.present) return directory;
            const auto relative = directory.path.lexically_relative(directory.existing);
            auto current = directory.existing;
            for (const auto& component : relative)
            {
                current /= component;
                if (!CreateDirectoryW(extended(current).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                    return {};
                auto created = inspect(current);
                if (created.error != path_error::none || !created.directory) return {};
                directory = std::move(created);
            }
            return directory;
        }

        bool atomically_write(const location& parent, const fs::path& name, const std::string& text)
        {
            if (!parent.directory || parent.error != path_error::none) return false;
            const auto path = parent.path / name;
            {
                const auto existing = inspect(path, true);
                if (existing.error != path_error::none || existing.directory || existing.links > 1) return false;
            }
            static std::atomic_uint serial{};
            const auto temp = parent.path / (L".show23-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                      std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial));
            handle file(CreateFileW(extended(temp).c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_DELETE, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!file) return false;
            DWORD written = 0;
            const auto length = static_cast<DWORD>(text.size());
            const bool flushed = WriteFile(file.get(), text.data(), length, &written, nullptr) && written == length &&
                FlushFileBuffers(file.get());
            if (flushed && [&]
            {
                const auto destination = extended(path);
                const auto size = static_cast<DWORD>(offsetof(FILE_RENAME_INFO, FileName) + (destination.size() + 1) * sizeof(wchar_t));
                auto storage = std::make_unique<char[]>(size);
                auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.get());
                rename->ReplaceIfExists = TRUE;
                rename->RootDirectory = nullptr;
                rename->FileNameLength = static_cast<DWORD>(destination.size() * sizeof(wchar_t));
                std::memcpy(rename->FileName, destination.data(), rename->FileNameLength);
                return SetFileInformationByHandle(file.get(), FileRenameInfo, rename, size) != FALSE;
            }()) return true;
            FILE_DISPOSITION_INFO remove{TRUE};
            SetFileInformationByHandle(file.get(), FileDispositionInfo, &remove, sizeof(remove));
            return false;
        }

        bool clear_marker(const fs::path& path)
        {
            const auto parent = inspect(path.parent_path(), true);
            if (parent.error != path_error::none) return false;
            if (!parent.present) return true;
            if (!parent.directory) return false;
            {
                const auto existing = inspect(parent.path / path.filename(), true);
                if (existing.error != path_error::none || existing.directory || existing.links > 1) return false;
                if (!existing.present) return true;
            }
            const auto marker = extended(parent.path / path.filename());
            if (DeleteFileW(marker.c_str())) return true;
            return GetLastError() == ERROR_FILE_NOT_FOUND;
        }

        std::string now_utc()
        {
            SYSTEMTIME utc{};
            GetSystemTime(&utc);
            return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", utc.wYear, utc.wMonth, utc.wDay,
                               utc.wHour, utc.wMinute, utc.wSecond);
        }

        const char* publish_record(const fs::path& install, const game_record& game)
        {
            for (const auto* relative : {"data/profiles/default/user", "data/profiles/default/server", "data/logs", "manifests"})
            {
                const auto folder = ensure_directory(install / relative);
                if (folder.error != path_error::none || !folder.directory) return "settings.tree";
            }
            const auto folder = ensure_directory(install / "data" / "games" / title_id);
            return atomically_write(folder, "game.json", encode_game(game)) ? nullptr : "settings.record";
        }
    }

    const char* stage_name(const setup_stage stage)
    {
        return std::array{"check", "unpack", "verify", "settings", "finish"}[static_cast<std::size_t>(stage)];
    }

    const char* stage_state_name(const stage_state state)
    {
        return std::array{"waiting", "running", "done", "not_needed", "stopped", "failed"}[static_cast<std::size_t>(state)];
    }

    setup_options default_setup_options()
    {
        setup_options options;
        options.inputs = default_input_check_options();
        options.remember = remember_install;
        return options;
    }

    setup_report run_setup(const setup_request& request, const setup_options& options, const setup_progress& progress)
    {
        using enum finding_state;
        using enum setup_stage;
        setup_report report;
        auto current = check;
        const auto show = [&] { if (progress) progress(report); };
        const auto row = [&]() -> stage_report& { return report.stages[static_cast<std::size_t>(current)]; };
        const auto start = [&](const setup_stage next)
        {
            current = next;
            row().state = stage_state::running;
            show();
        };
        const auto add = [&](const char* id, const finding_state state, finding_values values = {})
        {
            row().findings.push_back({id, state, std::move(values)});
        };
        const auto fail = [&](const char* id)
        {
            add(id, failed);
            row().state = stage_state::failed;
            return report;
        };
        try
        {
            start(check);
            static const utils::named_mutex active{"show23-pc-game-setup"};
            std::unique_lock lock(active, std::try_to_lock);
            if (!lock.owns_lock()) return fail("check.busy");
            auto checked = check_inputs(request, options.inputs);
            row().state = checked.passed() ? stage_state::done : stage_state::failed;
            row().findings = std::move(checked.findings);
            if (row().state != stage_state::done) return report;

            start(unpack);
            if (checked.request.kind == source_kind::package || checked.update_is_package)
            {
                add("unpack.package", later, {{"part", std::string(checked.request.kind == source_kind::package ? "base" : "update")}});
                row().state = stage_state::stopped;
                return report;
            }
            add("unpack.inPlace", passed);
            row().state = stage_state::not_needed;

            start(verify);
            const auto base = inspect(checked.request.base);
            const auto update = checked.request.update ? inspect(*checked.request.update) : location{};
            const auto install = inspect(request.install, true);
            if (install.error != path_error::none || (install.present && !install.directory) ||
                base.error != path_error::none || !base.directory ||
                (checked.has_update && (update.error != path_error::none || !update.directory))) return fail("verify.paths");
            for (const auto* source : {&base, &update})
            {
                if (!source->directory) continue;
                if (contains(source->path, install.path) || contains(install.path, source->path)) return fail("verify.paths");
                for (const auto& settings_path : options.inputs.protected_destinations)
                {
                    const auto settings_root = inspect(settings_path, true);
                    if (settings_root.error != path_error::none || contains(source->path, settings_root.path))
                        return fail("verify.paths");
                }
            }
            const auto marker = install.path / "manifests" / "game-ready.txt";
            if (!clear_marker(marker)) return fail("finish.marker");

            game_record game;
            game.base = verify_folder(base.path, false, options.inputs.base, options.base, row().findings);
            if (checked.has_update) game.update = verify_folder(update.path, true, options.inputs.update, options.update, row().findings);
            row().state = findings_pass(row().findings) ? stage_state::done : stage_state::failed;
            if (row().state != stage_state::done) return report;

            start(settings);
            game.created_utc = now_utc();
            if (const auto error = publish_record(install.path, game)) return fail(error);
            add("settings.written", passed);
            row().state = stage_state::done;

            start(finish);
            {
                const auto manifests = inspect(marker.parent_path());
                if (!atomically_write(manifests, marker.filename(), std::string(title_id) + '\n')) return fail("finish.marker");
                try
                {
                    if (options.remember && !options.remember(install.path))
                        throw std::runtime_error("Could not save install folder");
                }
                catch (...)
                {
                    if (!clear_marker(marker)) return fail("finish.marker");
                    return fail("finish.remember");
                }
            }
            add("finish.ready", passed);
            row().state = stage_state::done;
            return report;
        }
        catch (...)
        {
            return fail(current == check ? "check.internal" : current == verify ? "verify.paths" :
                current == settings ? "settings.record" : current == finish ? "finish.marker" : "check.internal");
        }
    }
}
