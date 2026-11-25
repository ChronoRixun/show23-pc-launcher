#include "std_include.hpp"
#include "release_source.hpp"
#include "versioned_start.hpp"
#include <versions/records.hpp>
#include <versions/update_fixture.hpp>
#include "release_resolver.hpp"

#include <utils/http.hpp>
#include <utils/flags.hpp>
#include <utils/logger.hpp>
#include <utils/properties.hpp>
#include <utils/property_keys.hpp>

namespace release_source
{
    namespace
    {
        using release_channel::channel;
        bool write_bootstrap_channel(const std::filesystem::path& root, channel value)
        {
            const auto temp = root / "channel.txt.tmp";
            std::error_code error;
            if (versions::plain_path(temp) && std::filesystem::is_regular_file(temp, error) && !error)
                std::filesystem::remove(temp, error);
            return !error && versions::write_channel(root, value == channel::stable ?
                versions::update_channel::stable : versions::update_channel::beta);
        }

        std::string api_base()
        {
#ifdef SHOW23_UPDATE_FIXTURE
            if (const auto local = utils::http::fixture_origin(); !local.empty()) return local;
#endif
            return "https://api.github.com";
        }

        release_resolver::http_answer get(const std::string& url)
        {
            const utils::http::headers headers{
                {"Accept", "application/vnd.github+json"},
                {"User-Agent", "show23-pc-launcher"},
                {"X-GitHub-Api-Version", "2022-11-28"},
            };
            const auto result = utils::http::get_data(url, {}, headers, {}, 15, 0);
            release_resolver::http_answer answer{};
            if (!result || result->code != CURLE_OK)
            {
                answer.reach_error = result ? curl_easy_strerror(result->code) : "no connection";
                return answer;
            }
            answer.reached = true;
            answer.status = result->response_code;
            answer.body = result->buffer;
            return answer;
        }

        release_resolver::resolver& shared()
        {
            static release_resolver::resolver resolver{{
                get,
                [](const std::string& key)
                {
                    return utils::properties::load(key);
                },
                [](const std::string& key, const std::string& value)
                {
                    utils::properties::store(key, value);
                },
                []
                {
                    return std::chrono::steady_clock::now();
                },
                api_base,
            }};
            return resolver;
        }
    }

    channel current_channel()
    {
        const auto& layout = versioned_start::layout();
        if (!layout)
            return release_channel::parse_channel(utils::properties::load(property_keys::UPDATE_CHANNEL).value_or(""));
        const auto choice = versions::read_channel(layout.versions);
        if (choice) return *choice == versions::update_channel::beta ? channel::beta : channel::stable;
        return release_channel::is_prerelease(versions::folder_version(layout.folder)) ? channel::beta : channel::stable;
    }

    void set_channel(const channel value)
    {
        const auto previous = current_channel();
        const auto& layout = versioned_start::layout();
        if (layout)
        {
            if (!write_bootstrap_channel(layout.versions, value)) return;
            versions::fixture::kill_at(layout.versions.parent_path(), "channel-written");
        }
        else
        {
            utils::properties::store(property_keys::UPDATE_CHANNEL, release_channel::channel_name(value));
        }
        if (previous != value) shared().invalidate();
    }

    release_channel::outcome resolve(const std::string& repo, const channel value, const std::vector<std::string>& required, const bool force)
    {
        auto outcome = shared().resolve(repo, value, required, force, utils::flags::has_flag("offline"));
        if (!outcome.chosen || outcome.from_cache)
        {
            utils::logger::write("release channel: {} ({}): {}{}", repo, release_channel::channel_name(value), outcome.error,
                                 outcome.from_cache ? " - using the last release found, " + outcome.chosen->version : std::string{});
        }
        return outcome;
    }

    std::optional<release_channel::outcome> last(const std::string& repo, const channel value)
    {
        return shared().last(repo, value);
    }

    std::array<release_channel::outcome, 2> resolve_shared_parts(const channel selected)
    {
        std::array<release_channel::outcome, 2> outcomes;
        if (utils::flags::has_flag("offline")) return outcomes;
        const auto reply = get(api_base() + "/repos/" + parts_repo + "/releases?per_page=30");
        outcomes[0] = release_resolver::select_from_feed(reply, selected, {"server.json", "SHA256SUMS.txt"}, "server-");
        outcomes[1] = release_resolver::select_from_feed(reply, selected, {"profile.json", "SHA256SUMS.txt"}, "profile-");
        return outcomes;
    }

    bool allowed_download_url(const std::string_view url)
    {
        const auto path = "/" + std::string(launcher_repo) + "/releases/download/";
#ifdef SHOW23_UPDATE_FIXTURE
        if (const auto local = utils::http::fixture_origin(); !local.empty())
            return url.starts_with(local + path);
#endif
        return url.starts_with("https://github.com" + path);
    }

    std::string file_url(const std::string& repo, const release_channel::release& chosen, const std::string& file)
    {
#ifdef SHOW23_UPDATE_FIXTURE
        if (const auto local = utils::http::fixture_origin(); !local.empty())
            return local + "/" + repo + "/releases/download/" + chosen.tag + "/" + file;
#endif
        return release_channel::download_url(repo, chosen.tag, file);
    }
}
