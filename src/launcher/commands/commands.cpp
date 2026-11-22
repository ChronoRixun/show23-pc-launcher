#include "std_include.hpp"
#include "versioned_start.hpp"
#include "commands.hpp"
#include "mod_commands.hpp"
#include "support_commands.hpp"
#include "ui_commands.hpp"
#include "cef/cef_ui.hpp"
#include "show23/setup_run.hpp"
#include "show23/play_session.hpp"
#include "show23/card_import.hpp"
#include "show23/server_paths.hpp"
#include "show23/player_name.hpp"
#include "show23/shortcuts.hpp"
#include "show23/unpacker.hpp"
#include "updater/launcher_update.hpp"
#include "updater/part_store.hpp"
#include "updater/release_source.hpp"

#include <utils/flags.hpp>
#include <utils/properties.hpp>
#include <utils/property_keys.hpp>
#include <utils/string.hpp>
#include <shellapi.h>
#include <chrono>
#include <thread>

namespace commands
{
    namespace
    {
        std::string string(const rapidjson::Value& value, const char* key)
        {
            if (!value.IsObject()) return {};
            const auto field = value.FindMember(key);
            if (field == value.MemberEnd() || !field->value.IsString()) return {};
            return {field->value.GetString(), field->value.GetStringLength()};
        }

        show23::resolution saved_resolution()
        {
            const auto id = utils::properties::load(property_keys::RESOLUTION).value_or("");
            const auto* choice = show23::find_resolution(id);
            return choice ? *choice : show23::default_resolution;
        }

        rapidjson::Value json(const std::string& text, rapidjson::Document::AllocatorType& allocator)
        {
            return rapidjson::Value(text.data(), static_cast<rapidjson::SizeType>(text.size()), allocator);
        }

        void result(rapidjson::Document& response, const std::string& message)
        {
            response.SetObject();
            response.AddMember("message", json(message, response.GetAllocator()), response.GetAllocator());
        }

        struct task
        {
            std::mutex mutex;
            bool busy{};
            std::string message;
            std::vector<show23::finding> findings;
        };
        task verify;
        std::jthread verify_worker;
        bool setup_busy{};
        std::atomic_bool setup_canceled{};
        show23::setup_report setup_result;
        std::jthread setup_worker;
        std::atomic<show23::play_phase> play_status{show23::play_phase::idle};
        std::atomic_bool play_stop{};
        show23::play_result last_play;
        const char* player_name_warning{};
        std::jthread play_worker;

        bool runtime_worker_busy{};
        std::optional<show23::fs::path> runtime_target;
        parts::runtime_check runtime_status;
        bool repair_busy{}, repair_success{};
        std::string repair_error;
        std::jthread runtime_worker;

        std::atomic_bool importing_cards{};
        std::atomic_bool cancel_cards{};
        show23::card_progress card_progress;
        show23::card_import_result card_result{};
        std::optional<show23::fs::path> card_target;
        std::jthread card_worker;

        const char* card_import_block(const std::optional<show23::fs::path>& install)
        {
            if (utils::flags::has_flag("offline") || utils::properties::load(property_keys::OFFLINE_MODE) == "true")
                return "offline";
            if (play_active()) return "playing";
            if (setup_busy || verify.busy || runtime_worker_busy) return "busy";
            if (!install || show23::inspect_game(*install).state != show23::record_state::ready) return "setup";
            if (runtime_target != install || !runtime_status.ready) return "runtime";
            return nullptr;
        }

        void register_cards(cef::cef_ui& ui)
        {
            ui.add_command("get-card-import", [](const auto&, rapidjson::Document& response)
            {
                const auto install = show23::install_folder();
                std::lock_guard lock(verify.mutex);
                const auto latest = install ? show23::read_card_status(*install) : std::nullopt;
                response.SetObject();
                auto& a = response.GetAllocator();
                response.AddMember("busy", importing_cards.load(), a);
                response.AddMember("canceling", importing_cards.load() && cancel_cards.load(), a);
                if (const auto blocked = card_import_block(install))
                    response.AddMember("blocked", rapidjson::StringRef(blocked), a);
                if (latest)
                {
                    response.AddMember("cards", latest->cards, a);
                    response.AddMember("date", json(latest->date, a), a);
                }
                if (card_target != install) return;
                response.AddMember("page", card_progress.page, a);
                response.AddMember("total", card_progress.total, a);
                if (card_result.status) response.AddMember("result", rapidjson::StringRef(card_result.status), a);
                if (!card_result.log.empty()) response.AddMember("log", json(utils::string::path_to_utf8(card_result.log), a), a);
            });

            ui.add_command("start-card-import", [](const auto&, rapidjson::Document& response)
            {
                std::lock_guard lock(verify.mutex);
                const auto install = show23::install_folder();
                if (importing_cards)
                {
                    response.SetString(rapidjson::StringRef("busy"));
                    return;
                }
                if (const auto blocked = card_import_block(install))
                {
                    response.SetString(rapidjson::StringRef(blocked));
                    return;
                }
                cancel_cards = false;
                importing_cards = true;
                card_target = install;
                card_progress = {};
                card_result = {};
                try
                {
                    card_worker = std::jthread([install = *install]
                    {
                        show23::card_import_result outcome{"runtime"};
                        try
                        {
                            const auto runtime = parts::read_runtime(install);
                            std::string message;
                            if (parts::verify_card_import_runtime(runtime, launcher_update::running_version(), message))
                            {
                                outcome = show23::import_cards(install, parts::component_folder(runtime, parts::id::server),
                                    [] { return cancel_cards.load(); }, [](const auto& next)
                                    {
                                        std::lock_guard lock(verify.mutex);
                                        card_progress = next;
                                    });
                            }
                        }
                        catch (...) {}
                        std::lock_guard lock(verify.mutex);
                        card_result = std::move(outcome);
                        importing_cards = false;
                    });
                    response.SetString(rapidjson::StringRef("started"));
                }
                catch (...)
                {
                    importing_cards = false;
                    response.SetString(rapidjson::StringRef("launch"));
                }
            });

            ui.add_command("cancel-card-import", [](const auto&, rapidjson::Document& response)
            {
                std::lock_guard lock(verify.mutex);
                if (importing_cards) cancel_cards = true;
                response.SetBool(importing_cards.load());
            });
        }

        std::filesystem::path bundle_root()
        {
            const auto& layout = versioned_start::layout();
            return layout ? layout.release : utils::properties::get_appdata_path();
        }

        bool prepare_runtime(const std::filesystem::path& install)
        {
            if (!versioned_start::unlocked()) return false;
            return parts::stage_bundled_runtime(bundle_root(), install, launcher_update::running_version());
        }

        void verify_runtime_for(const show23::fs::path& install)
        {
            if (runtime_worker_busy || runtime_target == install) return;
            runtime_target = install;
            runtime_status = {};
            runtime_worker_busy = true;
            try
            {
                runtime_worker = std::jthread([install]
                {
                    parts::runtime_check checked;
                    try { checked = parts::inspect_bundled_runtime(bundle_root(), install, launcher_update::running_version()); }
                    catch (...) { checked.message = "Could not verify the runtime files."; }
                    std::lock_guard lock(verify.mutex);
                    runtime_status = std::move(checked);
                    runtime_worker_busy = false;
                });
            }
            catch (...) { runtime_worker_busy = false; }
        }

        rapidjson::Value write_findings(const std::vector<show23::finding>& rows, rapidjson::Document::AllocatorType& allocator)
        {
            rapidjson::Value findings(rapidjson::kArrayType);
            for (const auto& finding : rows)
            {
                rapidjson::Value row(rapidjson::kObjectType);
                row.AddMember("id", json(finding.id, allocator), allocator);
                row.AddMember("state", rapidjson::StringRef(show23::state_name(finding.state)), allocator);
                rapidjson::Value values(rapidjson::kObjectType);
                for (const auto& [key, value] : finding.values)
                {
                    if (const auto number = std::get_if<std::uint64_t>(&value))
                        values.AddMember(json(key, allocator), rapidjson::Value(*number), allocator);
                    else
                        values.AddMember(json(key, allocator), json(std::get<std::string>(value), allocator), allocator);
                }
                row.AddMember("values", values, allocator);
                findings.PushBack(row, allocator);
            }
            return findings;
        }

        void write_setup(rapidjson::Document& response, const show23::setup_report& report, const bool busy = false)
        {
            response.SetObject();
            auto& a = response.GetAllocator();
            response.AddMember("busy", busy, a);
            response.AddMember("canCancel", busy && report.can_cancel, a);
            response.AddMember("canceled", report.canceled, a);
            if (report.extraction)
            {
                rapidjson::Value progress(rapidjson::kObjectType);
                progress.AddMember("elapsed", report.extraction->elapsed, a);
                progress.AddMember("written", report.extraction->written, a);
                progress.AddMember("expected", report.extraction->expected, a);
                response.AddMember("unpackProgress", progress, a);
            }
            rapidjson::Value stages(rapidjson::kObjectType);
            for (std::size_t i = 0; i < report.stages.size(); ++i)
            {
                const auto& stage = report.stages[i];
                rapidjson::Value item(rapidjson::kObjectType);
                item.AddMember("state", rapidjson::StringRef(show23::stage_state_name(stage.state)), a);
                item.AddMember("findings", write_findings(stage.findings, a), a);
                const auto name = show23::stage_name(static_cast<show23::setup_stage>(i));
                stages.AddMember(rapidjson::Value(rapidjson::StringRef(name)), item, a);
            }
            response.AddMember("stages", stages, a);
        }

        show23::setup_report setup_failure(const char* id)
        {
            show23::setup_report report;
            report.stages.front() = {show23::stage_state::failed, {{id, show23::finding_state::failed, {}}}};
            return report;
        }
        struct update_state
        {
            std::mutex mutex;
            bool busy{};
            unsigned generation{};
            std::array<release_channel::outcome, 3> releases;
            std::chrono::system_clock::time_point checked;
        };
        update_state updates;

        void write_part_release(rapidjson::Value& row, rapidjson::Document::AllocatorType& allocator,
                                const release_channel::outcome& outcome, const std::string& installed, const bool busy)
        {
            const char* state = busy ? "checking" : "idle";
            if (outcome.chosen)
            {
                const auto& version = outcome.chosen->version;
                state = outcome.from_cache ? "cached" : release_channel::compare(version, installed) > 0 || installed.empty() ?
                    "available" : "up-to-date";
                row.AddMember("availableVersion", json(version, allocator), allocator);
            }
            else if (!outcome.error.empty())
            {
                state = outcome.unpublished ? "unpublished" : "error";
            }
            row.AddMember("state", rapidjson::StringRef(state), allocator);
            row.AddMember("status", json(outcome.error, allocator), allocator);
        }

        void check_parts()
        {
            if (utils::flags::has_flag("offline")) return;
            const auto channel = release_source::current_channel();
            unsigned generation;
            {
                std::lock_guard lock(updates.mutex);
                if (updates.busy) return;
                updates.busy = true;
                generation = updates.generation;
                updates.releases.fill({});
            }
            try
            {
                std::thread([channel, generation]
                {
                    std::array<release_channel::outcome, 3> releases;
                    try
                    {
                        const auto emulator = release_source::resolve(release_source::emulator_repo, channel,
                                                                     {"emulator.json", "SHA256SUMS.txt"}, true);
                        const auto shared = release_source::resolve_shared_parts(channel);
                        releases = {emulator, shared[0], shared[1]};
                    }
                    catch (const std::exception&)
                    {
                        for (auto& release : releases) release.error = "Could not check for updates.";
                    }

                    std::lock_guard lock(updates.mutex);
                    if (generation == updates.generation)
                    {
                        updates.releases = std::move(releases);
                        updates.checked = std::chrono::system_clock::now();
                    }
                    updates.busy = false;
                }).detach();
            }
            catch (...)
            {
                std::lock_guard lock(updates.mutex);
                updates.busy = false;
                for (auto& release : updates.releases) release.error = "Could not start the update check.";
            }
        }

        void register_settings(cef::cef_ui& ui)
        {
            ui.add_command("use-this-copy", [](const auto&, rapidjson::Document& response)
            {
                show23::shortcut_result updated;
                try { updated = show23::update_shortcuts(true); }
                catch (...) {}
                response.SetObject();
                auto& a = response.GetAllocator();
                response.AddMember("desktop", updated.desktop, a);
                response.AddMember("startMenu", updated.start_menu, a);
            });

            ui.add_command("suggest-player-name", [](const auto&, rapidjson::Document& response)
            {
                response.CopyFrom(json(show23::windows_player_name(), response.GetAllocator()), response.GetAllocator());
            });

            ui.add_command("get-settings", [](const auto&, rapidjson::Document& response)
            {
                response.SetObject();
                auto& a = response.GetAllocator();
                const auto name = show23::normalize_player_name(utils::properties::load(property_keys::PLAYER_NAME).value_or(""));
                response.AddMember("playerName", json(name.value_or(""), a), a);
                response.AddMember("cardsOffered", utils::properties::load(property_keys::CARDS_OFFERED) == "true", a);
                response.AddMember("resolution", json(std::string(saved_resolution().id), a), a);
                response.AddMember("fullscreen", utils::properties::load(property_keys::FULLSCREEN) == "true", a);
                response.AddMember("offline", utils::flags::has_flag("offline"), a);
                response.AddMember("offlineSaved", utils::properties::load(property_keys::OFFLINE_MODE) == "true", a);
                response.AddMember("portable", utils::properties::is_portable(), a);
                response.AddMember("unpacker", json(utils::properties::load(property_keys::UNPACKER).value_or(""), a), a);
                const auto bundled = show23::find_unpacker({}, utils::properties::get_appdata_path() / "unpacker");
                response.AddMember("bundledUnpacker", json(bundled.error ? std::string{} :
                    utils::string::path_to_utf8(bundled.file.path), a), a);
                response.AddMember("language", json(utils::properties::load(property_keys::LANGUAGE).value_or("en"), a), a);
                response.AddMember("channel", rapidjson::StringRef(release_channel::channel_name(release_source::current_channel())), a);
            });

            ui.add_command("save-setting", [](const rapidjson::Value& request, rapidjson::Document& response)
            {
                const auto key = string(request, "key");
                const auto value = string(request, "value");
                if (key == "language" && (value == "en" || value == "fr" || value == "es" || value == "ru"))
                    utils::properties::store(property_keys::LANGUAGE, value);
                else if (key == "playerName")
                {
                    const auto name = show23::normalize_player_name(value);
                    if (name) utils::properties::store(property_keys::PLAYER_NAME, *name);
                    response.SetBool(name && utils::properties::load(property_keys::PLAYER_NAME) == name);
                    return;
                }
                else if (key == "cardsOffered" && value == "true")
                    utils::properties::store(property_keys::CARDS_OFFERED, value);
                else if (key == "resolution" && show23::find_resolution(value))
                {
                    utils::properties::store(property_keys::RESOLUTION, value);
                    response.SetBool(utils::properties::load(property_keys::RESOLUTION) == value);
                    return;
                }
                else if (key == "fullscreen" && (value == "true" || value == "false"))
                    utils::properties::store(property_keys::FULLSCREEN, value);
                else if (key == "offline" && (value == "true" || value == "false"))
                {
                    utils::properties::store(property_keys::OFFLINE_MODE, value);
                }
                else if (key == "unpacker" &&
                         (value.empty() || show23::is_local_path(utils::string::utf8_to_path(value))))
                    utils::properties::store(property_keys::UNPACKER, value);
                else if (key == "channel" && (value == "stable" || value == "beta"))
                {
                    if (!launcher_update::channel_changed(release_channel::parse_channel(value)))
                    {
                        response.SetBool(false);
                        return;
                    }
                    {
                        std::lock_guard lock(updates.mutex);
                        ++updates.generation;
                        updates.releases.fill({});
                        updates.checked = {};
                    }
                }
                else
                {
                    response.SetBool(false);
                    return;
                }
                response.SetBool(true);
            });
        }

        void register_updates(cef::cef_ui& ui)
        {
            const auto launcher_status = [](rapidjson::Document& response)
            {
                rapidjson::Value status;
                launcher_update::write_status(status, response.GetAllocator());
                response.CopyFrom(status, response.GetAllocator());
            };
            ui.add_command("get-launcher-update", [launcher_status](const auto&, rapidjson::Document& response)
            {
                launcher_status(response);
            });
            ui.add_command("check-launcher-update", [launcher_status](const auto&, rapidjson::Document& response)
            {
                launcher_update::check();
                launcher_status(response);
            });
            ui.add_command("start-launcher-update", [](const auto&, rapidjson::Document& response)
            {
                response.SetBool(launcher_update::start_download());
            });
            ui.add_command("request-stable", [](const rapidjson::Value& value, rapidjson::Document& response)
            {
                const auto version = string(value, "version");
                response.SetBool(!version.empty() && launcher_update::start_download(version));
            });
            ui.add_command("keep-beta", [](const auto&, rapidjson::Document& response)
            {
                response.SetBool(launcher_update::revoke_downgrade());
            });
            ui.add_command("retry-launcher-version", [](const auto& value, rapidjson::Document& response)
            {
                response.SetBool(launcher_update::retry(string(value, "folder")));
            });
            ui.add_command("restart-launcher-update", [&ui](const auto&, rapidjson::Document& response)
            {
                std::string error;
                const auto success = restart_launcher(error);
                response.SetObject();
                auto& a = response.GetAllocator();
                response.AddMember("success", success, a);
                response.AddMember("ok", success, a);
                response.AddMember("error", json(error, a), a);
                if (success) ui.close_browser();
            });
            ui.add_command("open-launcher-release", [](const auto&, rapidjson::Document& response)
            {
                if (utils::flags::has_flag("offline"))
                {
                    response.SetBool(false);
                    return;
                }
                const auto url = utils::string::convert(launcher_update::release_page());
                const auto opened = ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                response.SetBool(reinterpret_cast<INT_PTR>(opened) > 32);
            });
            ui.add_command("check-parts", [](const auto&, rapidjson::Document& response)
            {
                launcher_update::check();
                check_parts();
                response.SetBool(!utils::flags::has_flag("offline"));
            });
            ui.add_command("get-parts", [](const auto&, rapidjson::Document& response)
            {
                const auto runtime = parts::installed();
                const auto bundle = parts::read_runtime(bundle_root());
                std::lock_guard lock(updates.mutex);
                response.SetObject();
                auto& a = response.GetAllocator();
                response.AddMember("busy", updates.busy, a);
                const auto checked = std::chrono::duration_cast<std::chrono::milliseconds>(updates.checked.time_since_epoch()).count();
                response.AddMember("checkedAt", static_cast<std::int64_t>(checked), a);
                rapidjson::Value rows(rapidjson::kArrayType);
                for (const auto part : parts::all)
                {
                    const auto index = parts::index(part);
                    const auto& installed = runtime.manifests[index];
                    const auto version = part == parts::id::launcher ? launcher_update::running_version() :
                        (installed ? installed->version : std::string{});
                    rapidjson::Value row(rapidjson::kObjectType);
                    row.AddMember("part", rapidjson::StringRef(parts::name(part)), a);
                    row.AddMember("version", json(version, a), a);
                    row.AddMember("prerelease", release_channel::is_prerelease(version), a);
                    if (index)
                    {
                        auto outcome = updates.releases[index - 1];
                        if (outcome.unpublished && (!runtime.error.empty() || !bundle.error.empty()))
                        {
                            outcome.unpublished = false;
                            outcome.error = runtime.error.empty() ? bundle.error : runtime.error;
                        }
                        row.AddMember("bundled", outcome.unpublished && parts::has_bundled_part(bundle, part), a);
                        write_part_release(row, a, outcome, version, updates.busy);
                    }
                    rows.PushBack(row, a);
                }
                response.AddMember("parts", rows, a);
            });
        }

        void register_game(cef::cef_ui& ui)
        {
            ui.add_command("get-show23", [](const auto&, rapidjson::Document& response)
            {
                const auto install = show23::install_folder();
                response.SetObject();
                auto& a = response.GetAllocator();
                const auto game = install ? show23::inspect_game(*install) : show23::game_presence{};
                const auto set_up = game.state == show23::record_state::ready;
                const auto missing = game.state == show23::record_state::invalid;
                const auto updated = set_up && game.record->update.has_value();
                const auto version = set_up ? (updated ? game.record->update->version : game.record->base.version) : std::string{};
                {
                    std::lock_guard lock(verify.mutex);
                    if (set_up && versioned_start::unlocked() && !setup_busy && !verify.busy && !play_active() && !importing_cards)
                        verify_runtime_for(*install);
                    const auto current = set_up && runtime_target == install;
                    const auto in_use = set_up && (play_active() || importing_cards || parts::runtime_in_use(*install));
                    response.AddMember("checkingRuntime", set_up && runtime_worker_busy && !repair_busy, a);
                    response.AddMember("ready", current && runtime_status.ready, a);
                    response.AddMember("runtimeFresh", current && runtime_status.fresh, a);
                    response.AddMember("runtimeRepairable", current && runtime_status.repairable, a);
                    response.AddMember("runtimeInUse", in_use, a);
                    if (current && !runtime_status.ready && !runtime_status.fresh)
                        response.AddMember("runtimeError", json(runtime_status.message, a), a);
                }
                response.AddMember("setUp", set_up, a);
                response.AddMember("gameMissing", missing, a);
                response.AddMember("installedCopy", game.record && game.record->installed, a);
                response.AddMember("sourcePath", json(utils::string::path_to_utf8(game.source_path), a), a);
                response.AddMember("version", json(version, a), a);
                response.AddMember("updated", updated, a);
                response.AddMember("folder", json(install ? utils::string::path_to_utf8(*install) : std::string{}, a), a);
            });

            ui.add_command("repair-runtime", [](const auto&, rapidjson::Document& response)
            {
                std::lock_guard lock(verify.mutex);
                const auto install = show23::install_folder();
                response.SetBool(false);
                if (!install || runtime_target != install || !runtime_status.repairable || play_active() ||
                    importing_cards || setup_busy || verify.busy || runtime_worker_busy) return;
                // The repair worker takes the exclusive lease before it reads or changes the slot.
                repair_busy = runtime_worker_busy = true;
                repair_success = false;
                repair_error.clear();
                try
                {
                    runtime_worker = std::jthread([install = *install]
                    {
                        std::string error;
                        parts::runtime_check checked;
                        try
                        {
                            const auto version = launcher_update::running_version();
                            const auto bundle = bundle_root();
                            error = parts::repair_bundled_runtime(bundle, install, version);
                            checked = parts::inspect_bundled_runtime(bundle, install, version);
                        }
                        catch (...) { error = "Could not repair the runtime."; }
                        std::lock_guard completed(verify.mutex);
                        runtime_status = std::move(checked);
                        repair_success = error.empty();
                        repair_error = std::move(error);
                        repair_busy = runtime_worker_busy = false;
                    });
                    response.SetBool(true);
                }
                catch (...) { repair_busy = runtime_worker_busy = false; }
            });
            ui.add_command("get-runtime-repair", [](const auto&, rapidjson::Document& response)
            {
                std::lock_guard lock(verify.mutex);
                response.SetObject();
                auto& a = response.GetAllocator();
                response.AddMember("busy", repair_busy, a);
                response.AddMember("success", repair_success, a);
                response.AddMember("error", json(repair_error, a), a);
            });

            ui.add_command("setup-show23", [](const rapidjson::Value& value, rapidjson::Document& response)
            {
                const auto kind = string(value, "kind");
                if (kind != "package" && kind != "folder")
                {
                    write_setup(response, setup_failure("check.selection"));
                    return;
                }
                const auto update = string(value, "update");
                const show23::setup_request request{
                    kind == "package" ? show23::source_kind::package : show23::source_kind::folder,
                    utils::string::utf8_to_path(string(value, "base")),
                    update.empty() ? std::nullopt : std::optional(utils::string::utf8_to_path(update)),
                    utils::string::utf8_to_path(string(value, "install")),
                    utils::string::utf8_to_path(utils::properties::load(property_keys::UNPACKER).value_or("")),
                    string(value, "unpackerConsent"), string(value, "existingCopy"),
                    string(value, "displayedUnpacker")
                };
                std::lock_guard lock(verify.mutex);
                if (verify.busy || setup_busy || runtime_worker_busy || play_active() || importing_cards)
                {
                    write_setup(response, setup_failure("check.busy"));
                    return;
                }
                setup_result = {};
                setup_result.stages.front().state = show23::stage_state::running;
                setup_busy = true;
                setup_canceled = false;
                verify.message.clear();
                verify.findings.clear();
                try
                {
                    setup_worker = std::jthread([request]
                    {
                        show23::setup_report outcome;
                        try
                        {
                            auto options = show23::default_setup_options();
                            options.cancel = [] { return setup_canceled.load(); };
                            options.require_preview = true;
                            outcome = show23::run_setup(request, options, [](const auto& latest)
                            {
                                std::lock_guard update(verify.mutex);
                                setup_result = latest;
                            });
                        }
                        catch (...) { outcome = setup_failure("check.internal"); }

                        parts::runtime_check checked;
                        if (outcome.finished())
                        {
                            try
                            {
                                const auto install = show23::install_folder();
                                if (install)
                                {
                                    prepare_runtime(*install);
                                    checked = parts::inspect_bundled_runtime(bundle_root(), *install, launcher_update::running_version());
                                }
                            }
                            catch (...) { checked.message = "Could not verify the runtime files."; }
                        }
                        std::lock_guard completed(verify.mutex);
                        if (outcome.finished())
                        {
                            runtime_target = show23::install_folder();
                            runtime_status = std::move(checked);
                        }
                        setup_result = std::move(outcome);
                        setup_busy = false;
                    });
                    response.SetObject();
                    response.AddMember("busy", true, response.GetAllocator());
                }
                catch (const std::exception&)
                {
                    setup_busy = false;
                    write_setup(response, setup_failure("check.internal"));
                }
            });

            ui.add_command("get-setup-inputs", [](const auto&, rapidjson::Document& response)
            {
                std::lock_guard lock(verify.mutex);
                write_setup(response, setup_result, setup_busy);
            });

            ui.add_command("cancel-setup", [](const auto&, rapidjson::Document& response)
            {
                std::lock_guard lock(verify.mutex);
                const bool accepted = setup_busy && setup_result.can_cancel;
                if (accepted) setup_canceled = true;
                response.SetBool(accepted);
            });

            ui.add_command("remove-game-copy", [](const auto&, rapidjson::Document& response)
            {
                std::lock_guard lock(verify.mutex);
                const auto install = show23::install_folder();
                response.SetBool(!setup_busy && !verify.busy && !runtime_worker_busy && !play_active() && !importing_cards && install && show23::remove_game_copy(*install));
            });

            ui.add_command("play-show23", [](const auto& value, rapidjson::Document& response)
            {
                using enum show23::play_phase;
                std::lock_guard lock(verify.mutex);
                if (setup_busy || verify.busy || runtime_worker_busy || play_active() || importing_cards)
                {
                    response.SetBool(false);
                    return;
                }
                play_stop = false;
                last_play = {};
                player_name_warning = nullptr;
                const auto name = show23::normalize_player_name(string(value, "playerName")).value_or(
                    show23::normalize_player_name(utils::properties::load(property_keys::PLAYER_NAME).value_or("")).value_or(""));
                play_status = checking;
                try
                {
                    play_worker = std::jthread([name]
                    {
                        show23::play_result outcome{"paths"};
                        bool verified{};
                        try
                        {
                            const auto install = show23::install_folder();
                            const auto game = install ? show23::inspect_game(*install) : show23::game_presence{};
                            if (install && game.state == show23::record_state::ready)
                            {
                                verified = prepare_runtime(*install);
                                outcome = {"runtime"};
                                parts::runtime_lease lease(*install, false);
                                if (!lease) outcome = {"runtimeInUse"};
                                else if (verified)
                                {
                                    const auto runtime = parts::read_runtime(*install);
                                    std::string details;
                                    verified = parts::verify_runtime(runtime, launcher_update::running_version(), details);
                                    if (!verified) throw std::runtime_error(details);
                                    const auto server_folder = parts::component_folder(runtime, parts::id::server);
                                    const auto server = show23::server_paths(server_folder);
                                    const auto emulator_folder = parts::component_folder(runtime, parts::id::emulator);
                                    const show23::play_request request{*install, game.record->base.path,
                                        server.python, server.script,
                                        emulator_folder / "shadPS4.exe", utils::properties::load(property_keys::FULLSCREEN) == "true", name,
                                        saved_resolution()};
                                    show23::play_options options;
                                    options.stop = [] { return play_stop.load(); };
                                    options.phase = [](auto phase)
                                    {
                                        if (!play_stop.load()) play_status = phase;
                                    };
                                    options.warning = [](const char* warning)
                                    {
                                        std::lock_guard lock(verify.mutex);
                                        player_name_warning = warning;
                                    };
                                    outcome = show23::run_play(request, options);
                                }
                            }
                        }
                        catch (...) {}
                        std::lock_guard completed(verify.mutex);
                        if (verified) { runtime_status = {}; runtime_status.ready = true; }
                        else runtime_target.reset(); // Reinspect after a failed start or incomplete stage.
                        last_play = play_stop.load() ? show23::play_result{"stopped"} : std::move(outcome);
                        play_status = idle;
                    });
                    response.SetBool(true);
                }
                catch (...)
                {
                    play_status = idle;
                    last_play = {"paths"};
                    response.SetBool(false);
                }
            });

            ui.add_command("get-play", [](const auto&, rapidjson::Document& response)
            {
                std::lock_guard lock(verify.mutex);
                response.SetObject();
                auto& a = response.GetAllocator();
                response.AddMember("phase", rapidjson::StringRef(show23::phase_name(play_status.load())), a);
                if (player_name_warning) response.AddMember("nameWarning", rapidjson::StringRef(player_name_warning), a);
                if (last_play.message) response.AddMember("result", rapidjson::StringRef(last_play.message), a);
                response.AddMember("code", last_play.number, a);
                response.AddMember("log", json(last_play.log, a), a);
            });

            ui.add_command("stop-play", [](const auto&, rapidjson::Document& response)
            {
                std::lock_guard lock(verify.mutex);
                const auto active = play_active();
                if (active)
                {
                    play_stop = true;
                    play_status = show23::play_phase::stopping;
                }
                response.SetBool(active);
            });

            ui.add_command("verify-show23", [](const auto&, rapidjson::Document& response)
            {
                std::lock_guard lock(verify.mutex);
                response.SetBool(false);
                if (verify.busy || setup_busy || runtime_worker_busy || play_active() || importing_cards) return;
                verify.busy = true;
                verify.message.clear();
                verify.findings.clear();
                try
                {
                    verify_worker = std::jthread([]
                    {
                        std::vector<show23::finding> findings;
                        try
                        {
                            const auto install = show23::install_folder();
                            const auto game = install ? show23::inspect_game(*install) : show23::game_presence{};
                            if (game.record) findings = show23::verify_recorded_game(*game.record);
                            else if (game.state == show23::record_state::invalid)
                                findings.push_back({"verify.record", show23::finding_state::failed, {}});
                        }
                        catch (...) { findings.push_back({"verify.record", show23::finding_state::failed, {}}); }
                        std::string outcome;
                        try { outcome = parts::verify_files(parts::installed()); }
                        catch (...) { outcome = "Could not verify the runtime files."; }
                        std::lock_guard finish(verify.mutex);
                        runtime_target.reset();
                        verify.message = std::move(outcome);
                        verify.findings = std::move(findings);
                        verify.busy = false;
                    });
                    response.SetBool(true);
                }
                catch (const std::exception&)
                {
                    verify.busy = false;
                    verify.message = "Could not start verification.";
                }
            });

            ui.add_command("get-verify", [](const auto&, rapidjson::Document& response)
            {
                std::lock_guard lock(verify.mutex);
                result(response, verify.message);
                response.AddMember("busy", verify.busy, response.GetAllocator());
                response.AddMember("findings", write_findings(verify.findings, response.GetAllocator()), response.GetAllocator());
            });
        }
    }

    std::string game_entry::get_game_key() const { return show23::game_id; }
    std::optional<std::filesystem::path> game_entry::get_install_path() const
    {
        const auto path = show23::install_folder();
        if (!path || !parts::ordinary_path(*path / "data" / "mods", true)) return std::nullopt;
        return path;
    }

    std::optional<game_entry> command_context::get_game_config_from_request(const rapidjson::Value& value) const
    {
        if (string(value, "game") == show23::game_id) return game_entry{};
        return std::nullopt;
    }

    bool play_active()
    {
        return play_status.load() != show23::play_phase::idle;
    }

    bool card_import_active() { return importing_cards.load(); }

    bool restart_launcher(std::string& error)
    {
        const auto install = show23::install_folder();
        std::lock_guard lock(verify.mutex);
        if (play_active() || card_import_active() || setup_busy || verify.busy ||
            runtime_worker_busy || (install && parts::runtime_in_use(*install)))
        {
            error = "busy";
            return false;
        }
        return launcher_update::restart(error);
    }

    void stop_workers()
    {
        setup_canceled = true;
        play_stop = true;
        cancel_cards = true;
        if (card_worker.joinable()) card_worker.join();
        if (play_worker.joinable()) play_worker.join();
        if (setup_worker.joinable()) setup_worker.join();
        if (runtime_worker.joinable()) runtime_worker.join();
        if (verify_worker.joinable()) verify_worker.join();
    }

    void register_all_commands(cef::cef_ui& ui)
    {
        static command_context context;
        ui_commands::register_commands(ui, context);
        mod_commands::register_commands(ui, context);
        support_commands::register_commands(ui, context);
        register_settings(ui);
        register_updates(ui);
        register_game(ui);
        register_cards(ui);
        ui.add_command("confirm-launcher-started", [&ui](const auto&, auto&)
        {
            if (versioned_start::layout() && IsWindowVisible(ui.get_window())) versioned_start::confirm();
        });
        ui.add_command("get-startup", [](const auto&, rapidjson::Document& response)
        {
            const auto startup = versioned_start::status();
            response.SetObject();
            auto& a = response.GetAllocator();
            response.AddMember("waiting", startup == versioned_start::gate::waiting, a);
            response.AddMember("deferred", startup == versioned_start::gate::deferred, a);
            if (startup != versioned_start::gate::waiting) response.AddMember("notices", json(versioned_start::take_notices(), a), a);
            if (startup == versioned_start::gate::open)
            {
                static const auto started = []
                {
                    launcher_update::cleanup();
                    if (utils::flags::has_flag("-trial"))
                        try { show23::update_shortcuts(false); } catch (...) {}
                    return true;
                }();
                (void)started;
            }
        });
    }
}
