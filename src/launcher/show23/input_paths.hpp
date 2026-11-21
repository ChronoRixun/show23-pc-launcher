#pragma once

#include "setup.hpp"

#include <Windows.h>

#include <cstdint>
#include <span>
#include <vector>

namespace show23::input_paths
{
    enum class path_error { none, missing, reparse, access, not_directory };

    class handle
    {
    public:
        handle() = default;
        explicit handle(HANDLE value);
        ~handle();
        handle(handle&& other) noexcept;
        handle& operator=(handle&& other) noexcept;

        HANDLE get() const { return value_; }
        explicit operator bool() const { return value_ != INVALID_HANDLE_VALUE; }

    private:
        HANDLE value_{INVALID_HANDLE_VALUE};
    };

    struct location
    {
        // Each handle denies rename/delete of a checked component while its descendants are used.
        std::vector<handle> pins;
        fs::path path;
        fs::path existing;
        path_error error{};
        bool present{};
        bool directory{};
        std::uint64_t length{};
    };

    bool fixed_drive(const fs::path& path);
    location inspect(const fs::path& path, bool allow_missing = false, bool read_file = false);
    bool contains(const fs::path& directory, const fs::path& path);
    bool read_at(HANDLE file, std::uint64_t offset, std::span<char> out);
    std::optional<std::uint64_t> folder_size(const fs::path& folder);
    bool probe(const fs::path& existing, bool creating_folder);
    std::optional<std::uint64_t> free_space(const fs::path& existing);
}
