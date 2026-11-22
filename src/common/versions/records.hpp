#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace versions
{
    namespace fs = std::filesystem;
    inline constexpr unsigned bootstrap_version = 1;
    inline constexpr unsigned trial_starts = 2;
    inline constexpr unsigned launch_limit = 4;
    inline constexpr char launcher_exe[] = "show23-pc-launcher.exe";

    struct file
    {
        std::string path, sha256;
        std::uint64_t size{};
        bool operator==(const file&) const = default;
    };

    struct inventory
    {
        std::string version, channel;
        unsigned bootstrap{};
        std::vector<file> files;
        std::string fingerprint;
        bool operator==(const inventory&) const = default;
    };

    struct failure
    {
        std::string folder, fingerprint, reason;
        bool operator==(const failure&) const = default;
    };

    struct pointer
    {
        std::string current, previous, trial;
        unsigned starts{};
        std::string last_request;
        std::vector<failure> failed;
        bool operator==(const pointer&) const = default;
    };

    struct request
    {
        std::string id, trial, reason;
        bool operator==(const request&) const = default;
    };

    bool valid_version(std::string_view text);
    bool valid_folder(std::string_view name);
    std::string_view folder_version(std::string_view name);
    int compare(std::string_view left, std::string_view right);
    bool safe_relative(std::string_view path);

    std::optional<inventory> parse_inventory(std::string_view data);
    std::optional<pointer> parse_pointer(std::string_view data);
    std::optional<request> parse_request(std::string_view data);
    std::string encode(const inventory& value);
    std::string encode(const pointer& value);
    std::string encode(const request& value);

    // These reads and writes are bounded, local and reject linked path components.
    std::string sha256(std::string_view data);
    std::string read_record(const fs::path& file, std::size_t limit = 8192);
    bool write_record(const fs::path& file, std::string_view bytes);
    std::string request_id();

    inline constexpr char notice_file[] = "notice.txt";
    inline constexpr std::string_view notice_ids[] = {"rebuilt", "invalid-request", "ignored-older", "counter-write"};
    unsigned parse_notices(std::string_view text);
    std::string encode_notices(unsigned mask);
    bool plain_path(const fs::path& path, bool allow_missing_leaf = false);

    enum class check_mode { quick, full, partial };
    struct verification
    {
        std::string fingerprint, bad_file;
        explicit operator bool() const { return bad_file.empty(); }
    };
    verification verify(const fs::path& folder, check_mode mode);

    enum class pointer_source { primary, backup, empty };
    struct loaded_pointer
    {
        pointer value;
        pointer_source source{pointer_source::empty};
        bool repair_primary{}, repair_backup{};
    };

    loaded_pointer select_pointer(const std::optional<pointer>& primary, const std::optional<pointer>& backup);
    loaded_pointer load_pointer(const fs::path& root);
    bool repair_pointer(const fs::path& root, const loaded_pointer& loaded);
    bool commit(const fs::path& root, const pointer& old, const pointer& next);
}
