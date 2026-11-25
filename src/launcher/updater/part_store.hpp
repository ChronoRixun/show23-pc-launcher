#pragma once

#include "part_manifest.hpp"

#include <filesystem>

namespace parts
{
    struct installed_runtime
    {
        std::filesystem::path root;
        install_set manifests;
        std::string error;
    };

    // Windows Play and manual entry points hold a shared lease for the game lifetime.
    // Staging and Repair acquire an exclusive lease before publishing any files.
    class runtime_lease
    {
    public:
        runtime_lease(const std::filesystem::path& install, bool exclusive);
        ~runtime_lease();
        runtime_lease(const runtime_lease&) = delete;
        runtime_lease& operator=(const runtime_lease&) = delete;
        explicit operator bool() const noexcept { return handle_ != nullptr; }
        bool busy() const noexcept { return busy_; }

    private:
        void* handle_{};
        bool busy_{};
    };

    struct runtime_check
    {
        bool ready{};
        bool fresh{}; // Setup or Play can stage, or finish an interrupted fresh stage.
        bool repairable{};
        std::string message;
        std::string update_version;
        std::string rollback_version;
        bool rollback_blocked{};
    };

    enum class runtime_change { update, rollback };

    // Uses only the current runtime and its manifests. Invalid metadata fails closed.
    installed_runtime installed();
    std::filesystem::path component_folder(const installed_runtime& runtime, id part);
    bool ordinary_path(const std::filesystem::path& path, bool allow_missing = false);
    installed_runtime read_runtime(const std::filesystem::path& install);
    installed_runtime read_runtime(const std::filesystem::path& install, const std::string& release);
    bool runtime_in_use(const std::filesystem::path& install);
    runtime_check inspect_bundled_runtime(const std::filesystem::path& bundle, const std::filesystem::path& install,
                                          std::string_view launcher_version);
    bool stage_bundled_runtime(const std::filesystem::path& bundle, const std::filesystem::path& install,
                               std::string_view launcher_version);
    // Empty on success. A version supplied by the caller binds the confirmation to its offer.
    std::string change_runtime(const std::filesystem::path& bundle, const std::filesystem::path& install,
                               std::string_view launcher_version, runtime_change action, std::string_view expected = {});
    // Empty on success, "in-use" if leased, an active-slot image is running, or a replacement is blocked by a lock.
    std::string repair_bundled_runtime(const std::filesystem::path& bundle, const std::filesystem::path& install,
                                       std::string_view launcher_version);
    bool has_bundled_part(const installed_runtime& bundle, id part);
    bool verify_card_import_runtime(const installed_runtime& runtime, std::string_view launcher_version, std::string& message);
    bool verify_runtime(const installed_runtime& runtime, std::string_view launcher_version, std::string& message);
    std::string play_layout_error(const installed_runtime& runtime);
    bool verify_play_runtime(const installed_runtime& runtime, std::string_view launcher_version, std::string& message);
    std::string server_state_error(const std::filesystem::path& install, std::string_view server_version);
    // Caller holds the exclusive lease, or a shared lease and the Play activity lock.
    bool remember_server_state_version(const std::filesystem::path& install, std::string_view server_version);

    bool ready(const installed_runtime& runtime);
    bool runtime_ready(const installed_runtime& runtime);
    std::string verify_files(const installed_runtime& runtime);
}
