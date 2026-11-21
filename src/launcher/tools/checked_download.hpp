#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace checked_download
{
    using progress_callback = std::function<void(std::uint64_t done, std::uint64_t total)>;
    using cancel_check = std::function<bool()>;

    // Streams at most size bytes to file and returns the lowercase SHA-256 to compare with
    // a trusted checksum. Returns an error string on HTTP, I/O, size or cancellation failure.
    std::string download_file(const std::string& url, const std::filesystem::path& file, std::uint64_t size,
                              const progress_callback& progress, const cancel_check& cancelled, std::string& sha256);
}
