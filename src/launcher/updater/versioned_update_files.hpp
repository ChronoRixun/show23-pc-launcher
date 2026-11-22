#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace versioned_update_files
{
    bool has_space(std::uint64_t available, std::uint64_t archive, std::uint64_t expanded);
    std::optional<std::string> verified_slot(const std::filesystem::path& root, std::string_view version);
    void cleanup(const std::filesystem::path& root);
    void remove_transient(const std::filesystem::path& folder);

    class partial_slot
    {
    public:
        partial_slot(const std::filesystem::path& root, const std::string& version);
        ~partial_slot();
        partial_slot(const partial_slot&) = delete;
        partial_slot& operator=(const partial_slot&) = delete;

        std::string extract(const std::filesystem::path& zip, const std::function<bool()>& cancelled);

        // Publish only after checking that the selected channel and consent are still current.
        std::string publish(const std::filesystem::path& manifest, const std::optional<std::string>& reuse,
                            std::string& folder);

    private:
        std::filesystem::path root_, folder_;
        std::string version_;
        void* marker_{};
        bool owns_folder_{}, verified_{};
    };
}
