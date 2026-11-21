#include "std_include.hpp"
#include "commands.hpp"
#include "mod_commands.hpp"
#include "support_commands.hpp"
#include "ui_commands.hpp"
#include "cef/cef_ui.hpp"
#include "show23/setup_run.hpp"
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
        bool setup_busy{};
        show23::setup_report setup_result;

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
                state = outcome.error.starts_with("No release") ? "unpublished" : "error";
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
            ui.add_command("get-settings", [](const auto&, rapidjson::Document& response)
            {
                response.SetObject();
                auto& a = response.GetAllocator();
                response.AddMember("offline", utils::flags::has_flag("offline"), a);
                response.AddMember("offlineSaved", utils::properties::load(property_keys::OFFLINE_MODE) == "true", a);
                response.AddMember("portable", utils::properties::is_portable(), a);
                response.AddMember("language", json(utils::properties::load(property_keys::LANGUAGE).value_or("en"), a), a);
                response.AddMember("channel", rapidjson::StringRef(release_channel::channel_name(release_source::current_channel())), a);
            });

            ui.add_command("save-setting", [](const rapidjson::Value& request, rapidjson::Document& response)
            {
                const auto key = string(request, "key");
                const auto value = string(request, "value");
                if (key == "language" && (value == "en" || value == "fr" || value == "es" || value == "ru"))
                    utils::properties::store(property_keys::LANGUAGE, value);
                else if (key == "offline" && (value == "true" || value == "false"))
                {
                    utils::properties::store(property_keys::OFFLINE_MODE, value);
                }
                else if (key == "channel" && (value == "stable" || value == "beta"))
                {
                    {
                        std::lock_guard lock(updates.mutex);
                        ++updates.generation;
                        updates.releases.fill({});
                        updates.checked = {};
                    }
                    release_source::set_channel(release_channel::parse_channel(value));
                    launcher_update::channel_changed();
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
                launcher_update::revoke_downgrade();
                response.SetBool(true);
            });
            ui.add_command("restart-launcher-update", [](const auto&, rapidjson::Document& response)
            {
                std::string error;
                const auto success = launcher_update::restart(error);
                response.SetObject();
                auto& a = response.GetAllocator();
                response.AddMember("success", success, a);
                response.AddMember("ok", success, a);
                response.AddMember("error", json(error, a), a);
            });
            ui.add_command("open-launcher-release", [](const auto&, rapidjson::Document& response)
            {
                if (utils::flags::has_flag("offline"))
                {
                    response.SetBool(false);
                    return;
                }
                const auto opened = ShellExecuteW(nullptr, L"open", L"https://github.com/ChronoRixun/show23-pc-launcher/releases/latest",
                                                  nullptr, nullptr, SW_SHOWNORMAL);
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
                    if (index) write_part_release(row, a, updates.releases[index - 1], version, updates.busy);
                    rows.PushBack(row, a);
                }
                response.AddMember("parts", rows, a);
            });
        }

        void register_game(cef::cef_ui& ui)
        {
            ui.add_command("get-show23", [](const auto&, rapidjson::Document& response)
            {
                const auto runtime = parts::installed();
                const auto install = show23::install_folder();
                response.SetObject();
                auto& a = response.GetAllocator();
                const auto game = install ? show23::inspect_game(*install) : show23::game_presence{};
                const auto set_up = game.state == show23::record_state::ready;
                const auto missing = game.state == show23::record_state::invalid;
                const auto updated = set_up && game.record->update.has_value();
                const auto version = set_up ? (updated ? game.record->update->version : game.record->base.version) : std::string{};
                response.AddMember("ready", set_up && parts::runtime_ready(runtime), a);
                response.AddMember("setUp", set_up, a);
                response.AddMember("gameMissing", missing, a);
                response.AddMember("sourcePath", json(utils::string::path_to_utf8(game.source_path), a), a);
                response.AddMember("version", json(version, a), a);
                response.AddMember("updated", updated, a);
                response.AddMember("folder", json(install ? utils::string::path_to_utf8(*install) : std::string{}, a), a);
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
                    utils::string::utf8_to_path(string(value, "install"))
                };
                std::lock_guard lock(verify.mutex);
                if (verify.busy || setup_busy)
                {
                    write_setup(response, setup_failure("check.busy"));
                    return;
                }
                setup_result = {};
                setup_result.stages.front().state = show23::stage_state::running;
                setup_busy = true;
                verify.message.clear();
                verify.findings.clear();
                try
                {
                    std::thread([request]
                    {
                        show23::setup_report outcome;
                        try
                        {
                            outcome = show23::run_setup(request, show23::default_setup_options(), [](const auto& latest)
                            {
                                std::lock_guard update(verify.mutex);
                                setup_result = latest;
                            });
                        }
                        catch (...) { outcome = setup_failure("check.internal"); }

                        std::lock_guard completed(verify.mutex);
                        setup_result = std::move(outcome);
                        setup_busy = false;
                    }).detach();
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

            ui.add_command("play-show23", [](const auto&, rapidjson::Document& response)
            {
                const auto runtime = parts::installed();
                if (!parts::ready(runtime))
                {
                    result(response, "Set up the game first.");
                    return;
                }
                const auto install = *show23::install_folder();
                const auto server = show23::start_local_server(install, parts::component_folder(runtime, parts::id::server));
                if (server.state != show23::status::complete)
                {
                    result(response, server.message);
                    return;
                }
                const auto emulator = show23::start_emulator(install, parts::component_folder(runtime, parts::id::emulator));
                result(response, emulator.message);
            });

            ui.add_command("verify-show23", [](const auto&, rapidjson::Document& response)
            {
                std::lock_guard lock(verify.mutex);
                response.SetBool(false);
                if (verify.busy || setup_busy) return;
                verify.busy = true;
                verify.message.clear();
                verify.findings.clear();
                try
                {
                    std::thread([]
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
                        verify.message = std::move(outcome);
                        verify.findings = std::move(findings);
                        verify.busy = false;
                    }).detach();
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

    void register_all_commands(cef::cef_ui& ui)
    {
        static command_context context;
        ui_commands::register_commands(ui, context);
        mod_commands::register_commands(ui, context);
        support_commands::register_commands(ui, context);
        register_settings(ui);
        register_updates(ui);
        register_game(ui);
        ui.add_command("confirm-launcher-started", [](const auto&, auto&) { launcher_update::confirm_started(); });
    }
}
