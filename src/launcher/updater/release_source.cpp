#include "std_include.hpp"
#include "release_source.hpp"
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

        std::string api_base()
        {
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
        return release_channel::parse_channel(utils::properties::load(property_keys::UPDATE_CHANNEL).value_or(""));
    }

    void set_channel(const channel value)
    {
        if (current_channel() != value) shared().invalidate();
        utils::properties::store(property_keys::UPDATE_CHANNEL, release_channel::channel_name(value));
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
        const auto failure = release_resolver::describe_failure(reply);
        constexpr std::array prefixes{"server-", "profile-"};
        constexpr std::array manifests{"server.json", "profile.json"};
        for (std::size_t i = 0; i < outcomes.size(); ++i)
        {
            auto& outcome = outcomes[i];
            outcome.error = failure;
            if (!failure.empty()) continue;
            const auto releases = release_channel::parse_releases_json(reply.body, outcome.error, prefixes[i]);
            outcome.chosen = release_channel::pick(selected, releases, {manifests[i], "SHA256SUMS.txt"});
            if (!outcome.chosen && outcome.error.empty()) outcome.error = "No release has been published yet.";
        }
        return outcomes;
    }

    std::string file_url(const std::string& repo, const release_channel::release& chosen, const std::string& file)
    {
        return release_channel::download_url(repo, chosen.tag, file);
    }
}
