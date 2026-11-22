#include "std_include.hpp"
#include "release_resolver.hpp"

namespace release_resolver
{
    namespace
    {
        using release_channel::channel;

        // Reuse a successful answer during repeated checks.
        constexpr auto reuse_for = std::chrono::minutes(10);

        std::string answer_key(const std::string& repo, const channel value)
        {
            return repo + "|" + release_channel::channel_name(value);
        }
    }

    std::string cache_key(const std::string& repo, const channel value)
    {
        return std::string("release-cache-") + release_channel::channel_name(value) + "-" + repo;
    }

    std::string describe_failure(const http_answer& answer)
    {
        if (!answer.reached)
        {
            return "Could not reach GitHub (" + (answer.reach_error.empty() ? std::string("no connection") : answer.reach_error) + ").";
        }
        if (answer.status == 403 || answer.status == 429)
        {
            return std::format("GitHub's limit on update checks was reached (HTTP {}); it resets within an hour.", answer.status);
        }
        if (answer.status == 404)
        {
            return "No release has been published yet.";
        }
        if (answer.status != 200)
        {
            return std::format("GitHub answered HTTP {}.", answer.status);
        }
        return {};
    }

    resolver::resolver(io io)
        : io_(std::move(io))
    {
    }

    release_channel::outcome select_from_feed(const http_answer& answer, const channel value,
        const std::vector<std::string>& required, std::string_view tag_prefix, bool strict)
    {
        release_channel::outcome result;
        result.error = describe_failure(answer);
        if (!result.error.empty())
        {
            result.unpublished = answer.reached && answer.status == 404;
            return result;
        }
        const auto releases = release_channel::parse_releases_json(answer.body, result.error, tag_prefix);
        if (!result.error.empty()) return result;
        result.chosen = release_channel::pick(value, releases, required);
        if (result.chosen) return result;

        if (strict)
        {
            // Keep the existing selection when it finds a usable release. Before
            // treating an empty result as unpublished, check that the feed is sound.
            release_channel::parse_releases_json(answer.body, result.error, tag_prefix, true);
            if (!result.error.empty()) return result;
        }
        result.unpublished = !release_channel::pick(value, releases, {});
        result.error = result.unpublished ? "No release has been published yet." :
            "A release has been published, but its required files are missing.";
        return result;
    }

    release_channel::outcome resolver::fetch(const std::string& repo, const channel value,
        const std::vector<std::string>& required, bool strict) const
    {
        const auto base = this->io_.api_base() + "/repos/" + repo + "/releases";
        const auto reply = this->io_.get(value == channel::stable ? base + "/latest" : base + "?per_page=30");
        if (value == channel::beta)
        {
            auto result = select_from_feed(reply, value, required, {}, strict);
            if (!strict && (reply.reached && reply.status == 200) && !result.chosen &&
                (result.unpublished || result.error == "A release has been published, but its required files are missing."))
                result.error = "No release has been published yet with all its files.";
            return result;
        }

        release_channel::outcome result;
        result.error = describe_failure(reply);
        if (!result.error.empty())
        {
            result.unpublished = reply.reached && reply.status == 404;
            return result;
        }
        auto latest = release_channel::parse_release_json(reply.body, result.error);
        if (!latest) return result;
        if (latest->draft || latest->prerelease || release_channel::is_prerelease(latest->version) ||
            !release_channel::has_files(*latest, required))
        {
            result.error = "The latest release (" + latest->version + ") does not have all its files yet.";
            return result;
        }
        result.chosen = std::move(latest);
        return result;
    }

    release_channel::outcome resolver::resolve(const std::string& repo, const channel value, const std::vector<std::string>& required,
                                               const bool force, const bool cache_only, const bool strict)
    {
        const auto id = answer_key(repo, value);
        std::uint64_t generation;
        {
            std::lock_guard lock(this->mutex_);
            generation = this->generation_;
            if (const auto found = this->answers_.find(id); !force && !cache_only && found != this->answers_.end() && found->second.outcome.chosen &&
                                                            !found->second.outcome.from_cache && this->io_.now() - found->second.at < reuse_for)
            {
                return found->second.outcome;
            }
        }

        auto fetched = cache_only ? release_channel::outcome{} : this->fetch(repo, value, required, strict);
        if (cache_only) fetched.error = "Offline mode: using the last release checked for this channel.";
        std::optional<release_channel::release> cached;
        if (const auto stored = !fetched.chosen ? this->io_.load(cache_key(repo, value)) : std::nullopt)
        {
            // Only a choice that still has the files asked for (a cache from an older launcher may not).
            cached = release_channel::release_from_json(*stored);
            if (cached && !release_channel::has_files(*cached, required))
            {
                cached.reset();
            }
        }
        auto outcome = release_channel::settle(std::move(fetched.chosen), fetched.error, cached);
        outcome.unpublished = !outcome.chosen && fetched.unpublished;

        std::lock_guard lock(this->mutex_);
        if (generation == this->generation_)
        {
            // An old worker must not overwrite the selected channel's persisted fallback either.
            if (outcome.chosen && !outcome.from_cache) this->io_.store(cache_key(repo, value), release_channel::to_json(*outcome.chosen));
            this->answers_[id] = {outcome, this->io_.now()};
        }
        return outcome;
    }

    void resolver::invalidate()
    {
        std::lock_guard lock(this->mutex_);
        ++this->generation_;
        this->answers_.clear();
    }

    std::optional<release_channel::outcome> resolver::last(const std::string& repo, const channel value)
    {
        std::lock_guard lock(this->mutex_);
        const auto found = this->answers_.find(answer_key(repo, value));
        return found == this->answers_.end() ? std::nullopt : std::optional(found->second.outcome);
    }
}
