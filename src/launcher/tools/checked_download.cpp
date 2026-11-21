#include "std_include.hpp"
#include "checked_download.hpp"

#include <utils/cryptography.hpp>
#include <utils/http.hpp>

namespace checked_download
{
    std::string download_file(const std::string& url, const std::filesystem::path& file, const std::uint64_t size,
                              const progress_callback& progress, const cancel_check& cancelled, std::string& sha256)
    {
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return "Could not write the download into the launcher's data folder.";
        }

        utils::cryptography::sha256::stream hasher;
        std::uint64_t written = 0;
        bool too_big = false;
        const auto result = utils::http::get_data_stream(url, {}, {}, {}, [&](const char* data, const size_t length)
        {
            if (cancelled && cancelled())
            {
                return false;
            }
            if (size && length > size - written)
            {
                too_big = true;
                return false;
            }
            written += length;
            stream.write(data, static_cast<std::streamsize>(length));
            hasher.update(data, length);
            if (progress)
            {
                progress(written, size);
            }
            return static_cast<bool>(stream);
        }, {}, 900, 0);
        stream.close();

        if (cancelled && cancelled())
        {
            return "cancelled";
        }
        if (!stream && !too_big)
        {
            return "Could not finish writing the download.";
        }
        if (too_big)
        {
            return "The download is larger than the release says; it was stopped.";
        }
        if (!result || result->code != CURLE_OK)
        {
            return std::format("The download failed ({}).", result ? curl_easy_strerror(result->code) : "no connection");
        }
        if (result->response_code != 200)
        {
            return std::format("The download failed (HTTP {}).", result->response_code);
        }
        sha256 = hasher.finish_hex();
        if (size && written != size)
        {
            return std::format("The download is incomplete ({} of {} bytes).", written, size);
        }
        return {};
    }
}
