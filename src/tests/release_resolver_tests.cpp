#include "std_include.hpp"
#include "test.hpp"

#include "updater/release_resolver.hpp"

#include <map>

TEST(release_assets_for_the_native_runtime_are_ineligible_for_old_updaters)
{
    // The two asset names are a compatibility boundary for the sealed inventory.
    const auto native = R"([{"tag_name":"v2.0.0","draft":false,"prerelease":false,"assets":[
        {"name":"show23-pc-2.0.0-win64-native-portable.zip","size":10,"browser_download_url":"https://github.com/test/repo/releases/download/v2.0.0/show23-pc-2.0.0-win64-native-portable.zip"},
        {"name":"launcher.json","size":10,"browser_download_url":"https://github.com/test/repo/releases/download/v2.0.0/launcher.json"},
        {"name":"SHA256SUMS.txt","size":10,"browser_download_url":"https://github.com/test/repo/releases/download/v2.0.0/SHA256SUMS.txt"}]}])";
    using release_channel::channel;
    const release_resolver::http_answer reply{true, {}, 200, native};
    CHECK(!release_resolver::select_from_feed(reply, channel::stable,
        {"show23-pc-{version}-win64-portable.zip", "launcher.json", "SHA256SUMS.txt"}).chosen);
    CHECK(release_resolver::select_from_feed(reply, channel::stable,
        {"show23-pc-{version}-win64-native-portable.zip", "launcher.json", "SHA256SUMS.txt"}).chosen.has_value());
}

// Release discovery and channel-specific fallback against an in-memory GitHub feed.
namespace
{
    using release_channel::channel;
    using release_resolver::http_answer;

    constexpr auto test_repo = "example-owner/example-project";
    const std::vector<std::string> test_files{"show23-pc.json"};

    std::string release_json(const std::string& tag, const bool prerelease, const std::vector<std::string>& assets = {"show23-pc.json"})
    {
        std::string files;
        for (const auto& name : assets)
        {
            files += std::format(R"({}{{"name":"{}","size":10,"browser_download_url":"https://github.com/{}/releases/download/{}/{}"}})",
                                 files.empty() ? "" : ",", name, test_repo, tag, name);
        }
        return std::format(R"({{"tag_name":"{}","draft":false,"prerelease":{},"html_url":"https://github.com/{}/releases/tag/{}","assets":[{}]}})",
                           tag, prerelease ? "true" : "false", test_repo, tag, files);
    }

    http_answer ok(const std::string& body)
    {
        return {true, {}, 200, body};
    }

    // GitHub and the launcher's properties, in memory.
    struct fake
    {
        std::map<std::string, http_answer> answers; // by URL
        std::map<std::string, std::string> properties;
        std::vector<std::string> asked;
        std::chrono::steady_clock::time_point clock{};

        release_resolver::io io()
        {
            return {
                [this](const std::string& url)
                {
                    this->asked.push_back(url);
                    const auto it = this->answers.find(url);
                    return it == this->answers.end() ? http_answer{false, "no connection", 0, {}} : it->second;
                },
                [this](const std::string& key) -> std::optional<std::string>
                {
                    const auto it = this->properties.find(key);
                    return it == this->properties.end() ? std::nullopt : std::optional(it->second);
                },
                [this](const std::string& key, const std::string& value)
                {
                    this->properties[key] = value;
                },
                [this]
                {
                    return this->clock;
                },
                []
                {
                    return std::string("https://api.github.com");
                },
            };
        }

        void latest(const http_answer& answer)
        {
            this->answers[std::string("https://api.github.com/repos/") + test_repo + "/releases/latest"] = answer;
        }

        void list(const http_answer& answer)
        {
            this->answers[std::string("https://api.github.com/repos/") + test_repo + "/releases?per_page=30"] = answer;
        }

        void later()
        {
            this->clock += std::chrono::minutes(11); // past the answers' reuse
        }
    };


}

// ---- discovery and its fallback ----

TEST(stable_falls_back_to_its_own_last_choice)
{
    fake github;
    release_resolver::resolver resolver{github.io()};
    github.latest(ok(release_json("v1.7.1", false)));
    auto found = resolver.resolve(test_repo, channel::stable, test_files);
    CHECK(found.chosen && found.chosen->version == "1.7.1" && !found.from_cache && found.error.empty());

    // GitHub's rate limit, then its other limit, then no connection: the same choice, saying why.
    for (const auto& failure : {http_answer{true, {}, 403, "{\"message\":\"API rate limit exceeded\"}"}, http_answer{true, {}, 429, {}},
                                http_answer{false, "Could not resolve host", 0, {}}})
    {
        github.later();
        github.latest(failure);
        found = resolver.resolve(test_repo, channel::stable, test_files);
        CHECK(found.chosen && found.chosen->version == "1.7.1" && found.from_cache);
        CHECK(!found.error.empty());
    }
    CHECK(found.error.find("Could not reach GitHub") != std::string::npos);

    // A restart (a new resolver, the same properties) still has it.
    release_resolver::resolver restarted{github.io()};
    found = restarted.resolve(test_repo, channel::stable, test_files);
    CHECK(found.chosen && found.chosen->version == "1.7.1" && found.from_cache);
}

TEST(beta_falls_back_to_its_own_last_choice_never_stables)
{
    fake github;
    release_resolver::resolver resolver{github.io()};
    github.latest(ok(release_json("v1.7.1", false)));
    CHECK(resolver.resolve(test_repo, channel::stable, test_files).chosen);

    // First use of Beta, GitHub limited: no beta known - and Stable's choice is not used for it.
    github.list({true, {}, 403, {}});
    auto found = resolver.resolve(test_repo, channel::beta, test_files);
    CHECK(!found.chosen && !found.from_cache && found.error.find("HTTP 403") != std::string::npos);

    // A later retry that works; then GitHub fails again: that beta, from the last check.
    github.later();
    github.list(ok("[" + release_json("v1.8.0-beta.2", true) + "," + release_json("v1.7.1", false) + "]"));
    found = resolver.resolve(test_repo, channel::beta, test_files);
    CHECK(found.chosen && found.chosen->version == "1.8.0-beta.2" && !found.from_cache);
    github.later();
    github.list({false, "no connection", 0, {}});
    found = resolver.resolve(test_repo, channel::beta, test_files);
    CHECK(found.chosen && found.chosen->version == "1.8.0-beta.2" && found.from_cache);

    // And Stable, failing too, keeps Stable's (not the beta).
    github.later();
    github.latest({true, {}, 500, {}});
    found = resolver.resolve(test_repo, channel::stable, test_files);
    CHECK(found.chosen && found.chosen->version == "1.7.1" && found.from_cache && found.error == "GitHub answered HTTP 500.");
}

TEST(a_bad_answer_is_a_failure_with_its_reason)
{
    fake github;
    release_resolver::resolver resolver{github.io()};
    github.latest(ok("not json"));
    auto found = resolver.resolve(test_repo, channel::stable, test_files);
    CHECK(!found.chosen && !found.error.empty());

    // The latest release still uploading its files is not a choice (and not cached).
    github.later();
    github.latest(ok(release_json("v1.7.2", false, {"dinput.dll"})));
    found = resolver.resolve(test_repo, channel::stable, test_files);
    CHECK(!found.chosen && found.error.find("does not have all its files") != std::string::npos);
    CHECK(github.properties.empty());

    github.later();
    github.list(ok("{\"message\":\"Not Found\"}"));
    found = resolver.resolve(test_repo, channel::beta, test_files);
    CHECK(!found.chosen && !found.error.empty());
}

TEST(an_answer_is_reused_for_a_while_unless_forced)
{
    fake github;
    release_resolver::resolver resolver{github.io()};
    github.latest(ok(release_json("v1.7.1", false)));
    resolver.resolve(test_repo, channel::stable, test_files);
    resolver.resolve(test_repo, channel::stable, test_files);
    CHECK(github.asked.size() == 1);
    resolver.resolve(test_repo, channel::stable, test_files, true);
    CHECK(github.asked.size() == 2);
    github.later();
    resolver.resolve(test_repo, channel::stable, test_files);
    CHECK(github.asked.size() == 3);
}

TEST(files_come_from_the_chosen_tag_even_when_latest_moves)
{
    fake github;
    github.latest(ok(release_json("v1.7.1", false)));
    release_resolver::resolver resolver{github.io()};
    const auto chosen = resolver.resolve(test_repo, channel::stable, test_files).chosen;
    CHECK(chosen.has_value());
    github.latest(ok(release_json("v1.8.0", false)));
    CHECK(release_channel::download_url(test_repo, chosen->tag, test_files.front()) ==
          "https://github.com/example-owner/example-project/releases/download/v1.7.1/show23-pc.json");
}

TEST(channel_switch_invalidates_reused_answers_for_every_repository_and_retains_only_channel_local_fallback)
{
    fake github;
    release_resolver::resolver resolver{github.io()};
    for (const auto* repo : {test_repo, "example-owner/example-tools", "ChronoRixun/show23-pc", "example-owner/example-extra"})
    {
        const auto base = std::string("https://api.github.com/repos/") + repo + "/releases";
        github.answers[base + "/latest"] = ok(release_json("v0.4.0", false));
        github.answers[base + "?per_page=30"] = ok("[" + release_json("v0.6.0-beta.1", true) + "]");
        CHECK(resolver.resolve(repo, channel::stable, test_files).chosen->version == "0.4.0");
        CHECK(resolver.resolve(repo, channel::beta, test_files).chosen->version == "0.6.0-beta.1");
    }
    resolver.invalidate(); // settings switch: discard the ten-minute in-memory reuse
    CHECK(!resolver.last(test_repo, channel::stable));
    CHECK(!resolver.last(test_repo, channel::beta));
    github.asked.clear();
    for (const auto* repo : {test_repo, "example-owner/example-tools", "ChronoRixun/show23-pc", "example-owner/example-extra"})
    {
        const auto base = std::string("https://api.github.com/repos/") + repo + "/releases";
        github.answers[base + "?per_page=30"] = {false, "offline", 0, {}};
        const auto beta = resolver.resolve(repo, channel::beta, test_files);
        CHECK(beta.from_cache && beta.chosen && beta.chosen->version == "0.6.0-beta.1");
        github.answers[base + "/latest"] = {false, "offline", 0, {}};
        const auto stable = resolver.resolve(repo, channel::stable, test_files);
        CHECK(stable.from_cache && stable.chosen && stable.chosen->version == "0.4.0");
    }
    CHECK(github.asked.size() == 8); // every repository/channel really rechecked
}

TEST(channel_switch_during_a_feed_fetch_does_not_repopulate_the_reuse_cache)
{
    fake github;
    auto io = github.io();
    release_resolver::resolver* running = nullptr;
    io.get = [&](const std::string&)
    {
        running->invalidate(); // deliberately switch while a response is in flight
        return ok(release_json("v0.4.0", false));
    };
    release_resolver::resolver resolver{std::move(io)};
    running = &resolver;
    CHECK(resolver.resolve(test_repo, channel::stable, test_files).chosen.has_value());
    CHECK(!resolver.last(test_repo, channel::stable));
}

TEST(offline_restart_resolves_only_the_persisted_selected_channel_without_any_http)
{
    fake github;
    release_resolver::resolver before{github.io()};
    github.latest(ok(release_json("v0.4.0", false)));
    github.list(ok("[" + release_json("v0.6.0-beta.1", true) + "]"));
    before.resolve(test_repo, channel::stable, test_files);
    before.resolve(test_repo, channel::beta, test_files);
    github.asked.clear();
    release_resolver::resolver restarted{github.io()};
    const auto beta = restarted.resolve(test_repo, channel::beta, test_files, false, true);
    CHECK(beta.chosen && beta.chosen->version == "0.6.0-beta.1" && beta.from_cache);
    const auto stable = restarted.resolve(test_repo, channel::stable, test_files, false, true);
    CHECK(stable.chosen && stable.chosen->version == "0.4.0" && stable.from_cache);
    CHECK(github.asked.empty());
    CHECK(!restarted.resolve("no-selected-channel-record", channel::beta, test_files, false, true).chosen);
    CHECK(github.asked.empty());
}

TEST(invalidated_feed_cannot_overwrite_a_newer_persisted_selection)
{
    fake github;
    auto io = github.io();
    release_resolver::resolver* running = nullptr;
    bool first = true;
    io.get = [&](const std::string&)
    {
        if (first)
        {
            first = false;
            running->invalidate(); // Stable -> Beta -> Stable while the old Stable request is alive
            CHECK(running->resolve(test_repo, channel::stable, test_files).chosen->version == "0.5.0");
            return ok(release_json("v0.4.0", false)); // original response arrives last
        }
        return ok(release_json("v0.5.0", false));
    };
    release_resolver::resolver resolver{std::move(io)};
    running = &resolver;
    resolver.resolve(test_repo, channel::stable, test_files);
    const auto stored = release_channel::release_from_json(github.properties[release_resolver::cache_key(test_repo, channel::stable)]);
    CHECK(stored && stored->version == "0.5.0");
    release_resolver::resolver offline_restart{github.io()};
    CHECK(offline_restart.resolve(test_repo, channel::stable, test_files, false, true).chosen->version == "0.5.0");
}

TEST(a_private_or_unpublished_repository_stops_after_one_request)
{
    fake github;
    github.latest({true, {}, 404, R"({"message":"Not Found"})"});
    release_resolver::resolver resolver{github.io()};
    const auto found = resolver.resolve(test_repo, channel::stable, test_files);
    CHECK(!found.chosen && found.error == "No release has been published yet.");
    CHECK(github.asked.size() == 1);
}


TEST(part_release_feed_distinguishes_missing_publications_and_errors)
{
    using release_resolver::select_from_feed;
    const std::vector<std::string> required{"server.json", "SHA256SUMS.txt"};
    const auto choose = [&](const http_answer& answer)
    {
        return select_from_feed(answer, channel::stable, required, "server-");
    };
    CHECK(choose(ok("[]")).unpublished);
    CHECK(choose({true, {}, 404, {}}).unpublished);
    CHECK(choose(ok("[" + release_json("profile-v1.0.0", false) + "]")).unpublished);
    CHECK(choose(ok("[" + release_json("server-v1.0.0-beta.1", true) + "]")).unpublished);
    CHECK(!choose({false, "no connection", 0, {}}).unpublished);
    CHECK(!choose(ok("{broken")).unpublished);
    CHECK(!choose(ok("[{}]")).unpublished);
    CHECK(!choose(ok("[" + release_json("server-invalid", false) + "]")).unpublished);

    const auto incomplete = choose(ok("[" + release_json("server-v1.0.0", false, {"server.json"}) + "]"));
    CHECK(!incomplete.chosen && !incomplete.unpublished && !incomplete.error.empty());
    const auto published = choose(ok("[" + release_json("server-v1.0.0", false, required) + "]"));
    CHECK(published.chosen && !published.unpublished && published.error.empty());
}

TEST(emulator_beta_feed_reports_unpublished_only_without_a_channel_release)
{
    fake github;
    release_resolver::resolver resolver{github.io()};
    github.list(ok("[]"));
    CHECK(resolver.resolve(test_repo, channel::beta, test_files).unpublished);
    github.list(ok("[" + release_json("v1.0.1-beta.1", true, {}) + "]"));
    const auto incomplete = resolver.resolve(test_repo, channel::beta, test_files, true, false, true);
    CHECK(!incomplete.chosen && !incomplete.unpublished && !incomplete.error.empty());
}
