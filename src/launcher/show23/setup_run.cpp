#include "std_include.hpp"
#include "setup_run.hpp"
#include "input_paths.hpp"
#include "unpacker.hpp"
#include <utils/finally.hpp>
#include <utils/properties.hpp>
#include <utils/property_keys.hpp>
#include <utils/string.hpp>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

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

        const utils::named_mutex& setup_lock()
        {
            static const utils::named_mutex lock{"show23-pc-game-setup"};
            return lock;
        }

        fs::path make_staging(const fs::path& install)
        {
            const auto root = ensure_directory(game_directory(install));
            if (!root.directory || root.error != path_error::none) return {};
            std::array<unsigned char, 16> bytes{};
            if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
                return {};
            std::string name = "staging-";
            for (const auto byte : bytes) name += std::format("{:02x}", static_cast<unsigned>(byte));
            const auto stage = root.path / name;
            return CreateDirectoryW(extended(stage).c_str(), nullptr) ? stage : fs::path{};
        }

        bool rename_item(const fs::path& from, const fs::path& to) noexcept
        {
            try { return MoveFileExW(extended(from).c_str(), extended(to).c_str(), MOVEFILE_WRITE_THROUGH) != FALSE; }
            catch (...) { return false; }
        }

        bool delete_tree(const fs::path& path, unsigned depth = 0) noexcept
        {
            try
            {
                if (depth > 256) return false;
                const auto full = extended(path);
                const auto attributes = GetFileAttributesW(full.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    const auto error = GetLastError();
                    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
                }
                if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    if (DeleteFileW(full.c_str())) return true;
                    if (!(attributes & FILE_ATTRIBUTE_READONLY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
                    {
                        const auto leaf = inspect(path);
                        if (leaf.error != path_error::none || leaf.links != 1 ||
                            !SetFileAttributesW(full.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY)) return false;
                    }
                    return DeleteFileW(full.c_str()) != FALSE;
                }
                if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                {
                    const auto directory = inspect(path);
                    if (directory.error != path_error::none || !directory.directory) return false;
                    for (const auto& entry : fs::directory_iterator(full))
                        if (!delete_tree(path / entry.path().filename(), depth + 1)) return false;
                }
                return RemoveDirectoryW(full.c_str()) != FALSE;
            }
            catch (...) { return false; }
        }

        // Back up only known launcher-owned destinations. Keep the backup if a rollback
        // cannot complete (for example, another process locks a destination).
        struct publication
        {
            fs::path staging;
            std::vector<std::pair<fs::path, fs::path>> moved;
            bool committed{};
            bool blocked{};

            bool move(const fs::path& from, const fs::path& to)
            {
                moved.emplace_back(from, to);
                if (rename_item(from, to)) return true;
                moved.pop_back();
                return false;
            }

            bool save(const fs::path& path, const char* name, bool directory)
            {
                {
                    const auto item = inspect(path, true);
                    if (item.error != path_error::none) return false;
                    if (!item.present) return true;
                    if (item.directory != directory || (!directory && item.links != 1)) return false;
                }
                return move(path, staging / name);
            }

            bool rollback() noexcept
            {
                if (committed) return true;
                while (!moved.empty())
                {
                    const auto& [from, to] = moved.back();
                    if (!rename_item(to, from)) { blocked = true; return false; }
                    moved.pop_back();
                }
                return true;
            }
        };

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
        options.bundled_unpackers = utils::properties::get_appdata_path() / "unpacker";
        options.trusted_unpacker = utils::properties::load(property_keys::UNPACKER_CONSENT).value_or("");
        options.remember_unpacker = [](const auto& consent)
        {
            utils::properties::store(property_keys::UNPACKER_CONSENT, consent);
            return utils::properties::load(property_keys::UNPACKER_CONSENT) == consent;
        };
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
            report.can_cancel = false;
            add(id, failed);
            row().state = stage_state::failed;
            return report;
        };
        try
        {
            start(check);
            std::unique_lock lock(setup_lock(), std::try_to_lock);
            if (!lock.owns_lock()) return fail("check.busy");
            auto checked = check_inputs(request, options.inputs);
            row().state = checked.passed() ? stage_state::done : stage_state::failed;
            row().findings = std::move(checked.findings);
            if (row().state != stage_state::done) return report;

            start(unpack);
            const bool packages = checked.request.kind == source_kind::package;
            const auto stop = [&](const char* id, finding_values values = {})
            {
                report.can_cancel = false;
                add(id, later, std::move(values));
                row().state = stage_state::stopped;
                return report;
            };
            if (checked.has_update && packages != checked.update_is_package)
                return stop("unpack.mixed");
            if (packages)
            {
                const auto install = inspect(request.install, true);
                if (install.error != path_error::none || (install.present && !install.directory)) return fail("unpack.staging");
                for (const auto* input : {&checked.request.base, checked.has_update ? &*checked.request.update : nullptr})
                    if (input && contains(install.path, *input)) return fail("unpack.staging");
                const auto root = game_directory(install.path);
                const auto previous = inspect_game(install.path);
                const auto previous_owned = previous.record && installed_paths_match(*previous.record, install.path);
                for (const auto* name : {"base", "update"})
                {
                    const auto destination = inspect(root / name, true);
                    if (destination.error != path_error::none) return fail("unpack.adopt");
                    if (destination.present && (!previous_owned ||
                        (std::string_view(name) == "update" && !previous.record->update)))
                        return fail("unpack.adopt");
                }
                if (previous.state == record_state::ready && previous.record->installed &&
                    previous.record->base.version == checked.base_version &&
                    previous.record->update.has_value() == checked.has_update &&
                    (!checked.has_update || previous.record->update->version == checked.update_version))
                {
                    if (request.existing_copy.empty()) return stop("unpack.existing");
                    if (request.existing_copy == "keep")
                    {
                        add("unpack.kept", passed);
                        row().state = stage_state::not_needed;
                        start(verify);
                        verify_folder(previous.record->base.path, false, options.inputs.base, options.base, row().findings);
                        if (previous.record->update)
                            verify_folder(previous.record->update->path, true, options.inputs.update, options.update, row().findings);
                        row().state = findings_pass(row().findings) ? stage_state::done : stage_state::failed;
                        if (row().state != stage_state::done) return report;
                        start(finish);
                        if (options.remember && !options.remember(install.path)) return fail("finish.remember");
                        add("finish.ready", passed);
                        row().state = stage_state::done;
                        return report;
                    }
                }

                auto program = find_unpacker(request.unpacker, options.bundled_unpackers);
                if (program.error)
                    return program.error == std::string_view("unpack.noProgram") ? stop(program.error) : fail(program.error);
                const auto program_path = utils::string::path_to_utf8(program.file.path);
                if (!program.bundled && program.consent != options.trusted_unpacker)
                {
                    if (request.unpacker_consent != program.consent)
                        return stop("unpack.consent", {{"path", program_path}, {"consent", program.consent}});
                    if (!options.remember_unpacker || !options.remember_unpacker(program.consent))
                        return fail("unpack.consentSave");
                }

                if (options.require_preview && request.displayed_unpacker != program_path &&
                    (program.bundled || request.unpacker_consent != program.consent))
                    return stop("unpack.preview", {{"path", program_path}});
                if (options.cancel && options.cancel())
                {
                    report.canceled = true;
                    return stop("unpack.canceled");
                }
                report.can_cancel = true;
                add("unpack.program", notice, {{"path", program_path}});
                show();
                const auto staging = make_staging(install.path);
                if (staging.empty()) return fail("unpack.staging");
                publication transaction{staging};
                auto cleanup = utils::finally([&]
                {
                    if (transaction.blocked || !transaction.rollback()) return;
                    for (unsigned attempt = 0; !delete_tree(staging) && attempt < 10; ++attempt) Sleep(50);
                });
                const auto game_root = inspect(root);
                if (!game_root.directory || game_root.error != path_error::none) return fail("unpack.staging");
                const auto logs = ensure_directory(install.path / "data" / "logs");
                if (!logs.directory || logs.error != path_error::none) return fail("unpack.staging");
                fs::path base, update;
                std::uint64_t last_written{}, last_elapsed{};
                const auto estimate = [](std::uint64_t image) { return image + image / 100 * 27 + image % 100 * 27 / 100; };
                report.extraction = unpack_progress{0, 0, estimate(checked.base_image_bytes) + estimate(checked.update_image_bytes)};
                show();
                for (const bool patch : {false, true})
                {
                    if (patch && !checked.has_update) break;
                    const auto role = patch ? "update" : "base";
                    const auto target = ensure_directory(staging / role);
                    if (!target.directory || target.error != path_error::none) return fail("unpack.staging");
                    auto timestamp = now_utc();
                    std::erase(timestamp, ':');
                    const auto log = logs.path / ("unpack-" + timestamp + "-" +
                        staging.filename().string().substr(8) + "-" + role + ".log");
                    const auto extracted = extract_package(program, patch ? *checked.request.update : checked.request.base,
                        target.path, log, options.cancel, [&](auto elapsed, auto written)
                        {
                            report.extraction->elapsed = last_elapsed + elapsed;
                            report.extraction->written = last_written + written;
                            show();
                        });
                    if (extracted.error)
                    {
                        if (extracted.error == std::string_view("unpack.canceled"))
                        {
                            report.canceled = true;
                            return stop(extracted.error);
                        }
                        add(extracted.error, failed, {{"code", std::uint64_t(extracted.exit_code)}});
                        report.can_cancel = false;
                        row().state = stage_state::failed;
                        return report;
                    }
                    (patch ? update : base) = extracted.folder;
                    last_elapsed = report.extraction->elapsed;
                    last_written = report.extraction->written;
                }
                row().state = stage_state::done;
                start(verify);
                game_record game;
                game.installed = true;
                game.base = verify_folder(base, false, options.inputs.base, options.base, row().findings);
                if (checked.has_update) game.update = verify_folder(update, true, options.inputs.update, options.update, row().findings);
                if (options.cancel && options.cancel())
                {
                    report.canceled = true;
                    return stop("unpack.canceled");
                }
                row().state = findings_pass(row().findings) ? stage_state::done : stage_state::failed;
                report.can_cancel = false;
                show();
                if (options.cancel && options.cancel())
                {
                    report.canceled = true;
                    return stop("unpack.canceled");
                }
                if (row().state != stage_state::done) return report;

                start(settings);
                const auto marker = install.path / "manifests" / "game-ready.txt";
                const auto record = root / "game.json";
                const auto manifests = ensure_directory(marker.parent_path());
                if (manifests.error != path_error::none || !manifests.directory) return fail("settings.tree");
                const auto abort_publish = [&](const char* error)
                {
                    return fail(transaction.rollback() ? error : "unpack.rollback");
                };
                if (!transaction.save(marker, "previous-ready", false) ||
                    !transaction.save(record, "previous-record", false) ||
                    !transaction.save(root / "base", "previous-base", true) ||
                    !transaction.save(root / "update", "previous-update", true) ||
                    !transaction.move(base, root / "base") ||
                    (game.update && !transaction.move(update, root / "update")))
                    return abort_publish("unpack.adopt");
                game.base.path = root / "base";
                if (game.update) game.update->path = root / "update";
                game.created_utc = now_utc();

                bool record_written{}, marker_written{};
                auto undo_metadata = utils::finally([&]
                {
                    if (transaction.committed) return;
                    if (marker_written) clear_marker(marker);
                    if (record_written) clear_marker(record);
                });
                if (const auto error = publish_record(install.path, game)) return abort_publish(error);
                record_written = true;
                add("settings.written", passed);
                row().state = stage_state::done;
                start(finish);
                if (!atomically_write(manifests, marker.filename(), std::string(title_id) + '\n'))
                    return fail("finish.marker");
                marker_written = true;
                if (options.remember && !options.remember(install.path)) return fail("finish.remember");
                add("finish.ready", passed);
                row().state = stage_state::done;
                transaction.committed = true;
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
            return fail(current == check ? "check.internal" : current == unpack ? "unpack.staging" : current == verify ? "verify.paths" :
                current == settings ? "settings.record" : current == finish ? "finish.marker" : "check.internal");
        }
    }

    bool remove_game_copy(const fs::path& install)
    {
        std::unique_lock lock(setup_lock(), std::try_to_lock);
        if (!lock.owns_lock()) return false;
        const auto root = inspect(game_directory(install));
        if (!root.directory || root.error != path_error::none) return false;
        const auto current = inspect_game(install);
        if (!current.record || !installed_paths_match(*current.record, install)) return false;

        const auto staging = make_staging(install);
        if (staging.empty()) return false;
        publication transaction{staging};
        const bool moved = transaction.save(install / "manifests" / "game-ready.txt", "ready", false) &&
            transaction.save(root.path / "game.json", "record", false) &&
            transaction.save(root.path / "base", "base", true) &&
            (!current.record->update || transaction.save(root.path / "update", "update", true));
        if (!moved)
        {
            if (transaction.rollback()) delete_tree(staging);
            return false;
        }
        transaction.committed = true;
        return delete_tree(staging);
    }

}
