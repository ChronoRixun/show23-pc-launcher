#pragma once

#include "release_channel.hpp"

#include <array>

#include <string>
#include <vector>

// Asks GitHub which release of a repository the update channel takes (release_resolver.hpp: Stable
// releases/latest, Beta the releases list). Downloads are pinned to the selected tag.
// Unauthenticated GitHub API calls are limited (60 an hour per address), so an answer is reused for a
// few minutes, and each channel's last valid choice per repository is kept in the launcher's properties
// for when GitHub can't be asked: then that release is used and the outcome says why (from_cache,
// error). Never called with -offline.
namespace release_source
{
    constexpr auto launcher_repo = "ChronoRixun/show23-pc-launcher";
    constexpr auto parts_repo = "ChronoRixun/show23-pc";
    constexpr auto emulator_repo = "ChronoRixun/show23-pc-emulator";

    // The setting (launcher-update-channel): Stable unless the player chose Beta.
    release_channel::channel current_channel();
    void set_channel(release_channel::channel value);

    // With -offline this reads only that repository/channel's persisted metadata, without HTTP.
    // The release of `repo` for `channel` with every file in `required` (a release still uploading them
    // isn't one yet). `force` asks GitHub even when a recent answer is at hand (Check for updates).
    release_channel::outcome resolve(const std::string& repo, release_channel::channel channel, const std::vector<std::string>& required,
                                     bool force = false);

    // The last outcome for `repo` and `channel` this run, without asking GitHub (nullopt: none yet).
    std::optional<release_channel::outcome> last(const std::string& repo, release_channel::channel channel);

    // One feed is shared by server and profile. Filter on distinct tag prefixes.
    std::array<release_channel::outcome, 2> resolve_shared_parts(release_channel::channel channel);

    // Where a file of `chosen` is: by its tag (never releases/latest).
    bool allowed_download_url(std::string_view url);
    std::string file_url(const std::string& repo, const release_channel::release& chosen, const std::string& file);
}
