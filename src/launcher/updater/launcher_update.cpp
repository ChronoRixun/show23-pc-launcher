#include "std_include.hpp"
#include <versions/update_fixture.hpp>
#include "launcher_update.hpp"
#include "channel_refresh.hpp"
#include "part_manifest.hpp"
#include "part_store.hpp"
#include "release_source.hpp"
#include "tools/checked_download.hpp"
#include "versioned_start.hpp"
#include "versioned_update_files.hpp"

#include <utils/finally.hpp>
#include <utils/flags.hpp>
#include <utils/http.hpp>
#include <utils/logger.hpp>
#include <utils/string.hpp>
#include <versions/records.hpp>
#include <version.hpp>

#include <condition_variable>
#include <limits>
#include <thread>
#include <utility>

namespace launcher_update
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace files = versioned_update_files;
        using release_channel::channel;
        constexpr auto releases = "https://github.com/ChronoRixun/show23-pc-launcher/releases";
        constexpr auto first_versioned = "0.1.3-beta.1";
        constexpr auto sums_name = "SHA256SUMS.txt";
        constexpr auto manifest_name = "launcher.json";
        constexpr auto manifest_limit = 256 * 1024;

        struct release
        {
            std::string version, tag, zip, zip_url, sums_url, manifest_url;
            std::uint64_t size{}, manifest_size{};
            bool prerelease{};
        };

        struct state_t
        {
            std::string state = "idle";
            std::optional<release> latest;
            channel_refresh::ticket selection;
            channel latest_channel = channel::stable;
            bool from_cache{}, downgrade{}, retrying{}, request_deferred{};
            std::uint64_t done{}, total{};
            std::string cache_error, error, install_error, ready_folder, request_id;
            std::chrono::system_clock::time_point checked{};
        };

        std::mutex mutex_;
        state_t state_;
        channel_refresh::gate refresh_;
        std::condition_variable work_ready_;
        std::function<void()> pending_;
        std::thread worker_;
        std::atomic_bool stopping_{};

        void run_worker()
        {
            for (;;)
            {
                std::function<void()> work;
                {
                    std::unique_lock lock(mutex_);
                    work_ready_.wait(lock, [] { return stopping_ || pending_; });
                    if (stopping_) return;
                    work = std::exchange(pending_, {});
                }
                work();
            }
        }

        void queue_locked(std::function<void()> work)
        {
            if (!worker_.joinable()) worker_ = std::thread(run_worker);
            pending_ = std::move(work);
            work_ready_.notify_one();
        }

        bool can_install()
        {
            const auto& layout = versioned_start::layout();
            const auto current = installed_version();
            const auto marker = layout.versions.parent_path() / "portable.marker";
            std::error_code error;
            return layout && versioned_start::unlocked() && !current.empty() &&
                versions::folder_version(layout.folder) == current && versions::plain_path(layout.versions) &&
                versions::plain_path(marker) && fs::is_regular_file(marker, error);
        }

        std::string page_for(std::string_view tag)
        {
            return tag.empty() ? releases : std::string(releases) + "/tag/" + std::string(tag);
        }

        std::string page_locked()
        {
            if (state_.latest) return page_for(state_.latest->tag);
            const auto version = running_version();
            return versions::valid_version(version) ? page_for("v" + version) : releases;
        }

        std::string zip_name(const std::string& version)
        {
            return "show23-pc-" + version + files::archive_suffix + ".zip";
        }

        std::optional<release> fetch_release(channel selected, std::string& error, bool& from_cache)
        {
            const auto repo = release_source::launcher_repo;
            const auto outcome = release_source::resolve(repo, selected, {zip_name("{version}"), sums_name, manifest_name}, true);
            from_cache = outcome.from_cache;
            error = outcome.error;
            if (!outcome.chosen) return {};

            const auto& found = *outcome.chosen;
            if (!versions::valid_version(found.version) || release_channel::tag_version(found.tag) != found.version)
            {
                error = "The selected release does not have a supported version tag.";
                return {};
            }
            release latest{};
            latest.version = found.version;
            latest.tag = found.tag;
            latest.prerelease = found.prerelease;
            latest.zip = zip_name(latest.version);
            for (const auto& asset : found.assets)
            {
                if (!release_source::allowed_download_url(asset.url)) continue;
                if (asset.name != latest.zip && asset.name != sums_name && asset.name != manifest_name) continue;
                const auto url = release_source::file_url(repo, found, asset.name);
                if (!release_source::allowed_download_url(url)) continue;
                if (asset.name == latest.zip && asset.size)
                {
                    latest.zip_url = url;
                    latest.size = asset.size;
                }
                else if (asset.name == sums_name)
                    latest.sums_url = url;
                else if (asset.name == manifest_name && asset.size && asset.size <= manifest_limit)
                {
                    latest.manifest_url = url;
                    latest.manifest_size = asset.size;
                }
            }
            if (latest.zip_url.empty() || latest.sums_url.empty() || latest.manifest_url.empty())
            {
                error = "Release " + latest.version + " is missing the portable ZIP, manifest or SHA256SUMS.txt.";
                return {};
            }
            return latest;
        }

        // Accept the standard "<hash>  <name>" and "<hash> *<name>" checksum lines.
        std::string expected_sha256(const std::string& sums, const std::string& name)
        {
            std::istringstream lines(sums);
            std::string line;
            while (std::getline(lines, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() < 66 || line[64] != ' ') continue;
                const auto hash = utils::string::to_lower(line.substr(0, 64));
                auto file = line.substr(64);
                file.erase(0, file.find_first_not_of(' '));
                if (file.starts_with('*')) file.erase(0, 1);
                if (file == name && hash.find_first_not_of("0123456789abcdef") == hash.npos) return hash;
            }
            return {};
        }

        std::optional<std::string> fetch_small(const std::string& url, std::uint64_t limit)
        {
            std::string body;
            const auto result = utils::http::get_data_stream(url, {}, {}, {},
                [&](const char* bytes, std::size_t size)
                {
                    if (size > limit - body.size()) return false;
                    body.append(bytes, size);
                    return true;
                }, [] { return stopping_.load(); }, 20, 0);
            if (!result || result->code != CURLE_OK || result->response_code != 200) return {};
            return body;
        }

        bool legacy_stable()
        {
            return state_.latest && state_.latest_channel == channel::stable &&
                versions::compare(state_.latest->version, first_versioned) < 0;
        }

        bool busy_locked()
        {
            return state_.state == "checking" || state_.state == "downloading";
        }

        bool channel_change_allowed_locked()
        {
            return !stopping_ && !(state_.state == "downloading" && !state_.downgrade && !state_.retrying) &&
                !(state_.state == "ready" && state_.request_id.empty());
        }

        bool selected_locked(channel_refresh::ticket selection, bool downgrade)
        {
            return !stopping_ && refresh_.current(selection) && release_source::current_channel() == selection.channel &&
                (!downgrade || state_.downgrade);
        }

        std::string offered_state_locked()
        {
            const auto current = installed_version();
            if (current.empty() || !state_.latest) return "idle";
            return versions::compare(state_.latest->version, current) > 0 ? "available" : "up-to-date";
        }

        std::string wait_for_request_locked(std::unique_lock<std::mutex>& lock, const std::string& folder,
            std::string reason, const auto& still_selected, std::string_view retry_fingerprint = {})
        {
            constexpr auto unsaved = "Could not save the version choice. The current version is still active.";
            const auto& layout = versioned_start::layout();
            const auto id = versions::request_id();
            if (id.empty() || !versions::valid_folder(folder)) return unsaved;
            const versions::request request{id, folder, std::move(reason)};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            bool waiting{};
            while (!stopping_ && still_selected())
            {
                const auto now = std::chrono::steady_clock::now();
                const auto defer = now >= deadline;
                const auto result = files::try_request(layout.versions, layout.folder, request, retry_fingerprint, defer);
                if (result == files::request_write::saved || result == files::request_write::deferred)
                {
                    state_.request_id = id;
                    state_.request_deferred = result == files::request_write::deferred;
                    return {};
                }
                if (result == files::request_write::rejected || defer) return unsaved;
                if (!waiting)
                {
                    utils::logger::write("launcher update: waiting to record the update request for {}", folder);
                    waiting = true;
                }
                // Release the updater lock while bootstrap tries to save the fallback pointer.
                work_ready_.wait_until(lock, (std::min)(deadline, now + std::chrono::milliseconds(250)));
            }
            return "The launcher update was cancelled.";
        }

        void ready_locked(std::string folder)
        {
            state_.ready_folder = std::move(folder);
            state_.state = "ready";
        }

        bool revoke_locked()
        {
            const auto& layout = versioned_start::layout();
            if (layout && versioned_start::unlocked())
            {
                const auto path = layout.versions / "request.txt";
                const auto request = versions::parse_request(versions::read_record(path));
                const auto pointer = versions::load_pointer(layout.versions);
                if (request && request->reason == "switch-to-stable" && request->id != pointer.value.last_request)
                {
                    if (pointer.source == versions::pointer_source::empty || !DeleteFileW(path.c_str()))
                    {
                        state_.install_error = "Could not cancel the pending switch to Stable.";
                        return false;
                    }
                }
            }
            if (state_.downgrade && state_.state == "ready")
            {
                state_.request_id.clear();
                state_.ready_folder.clear();
                state_.state = offered_state_locked();
            }
            state_.downgrade = false;
            return true;
        }

        std::uint64_t installed_size()
        {
            const auto& layout = versioned_start::layout();
            const auto inventory = versions::parse_inventory(versions::read_record(layout.release / "version.txt", 2 * 1024 * 1024));
            if (!inventory) return std::numeric_limits<std::uint64_t>::max();
            std::uint64_t total{};
            for (const auto& file : inventory->files)
            {
                if (file.size > std::numeric_limits<std::uint64_t>::max() - total)
                    return std::numeric_limits<std::uint64_t>::max();
                total += file.size;
            }
            return total;
        }

        fs::path make_download(const fs::path& root)
        {
            auto sequence = GetTickCount64();
            for (unsigned tries = 0; tries < 32; ++tries)
            {
                const auto folder = root / (".download-" + std::to_string(sequence + tries));
                if (!versions::plain_path(folder, true)) continue;
                std::error_code error;
                if (fs::create_directory(folder, error)) return folder;
                if (error && error != std::errc::file_exists) break;
            }
            return {};
        }

        std::string stage(const release& latest, channel_refresh::ticket selection, bool downgrade)
        {
            const auto& root = versioned_start::layout().versions;
            const auto still_selected = [&] { return selected_locked(selection, downgrade); };
            const auto cancelled = [&]
            {
                if (stopping_) return true;
                std::lock_guard lock(mutex_);
                return !refresh_.current(selection) || (downgrade && !state_.downgrade);
            };
            const auto runtime = parts::installed();
            if (!runtime.error.empty()) return runtime.error;
            const auto current = installed_version();
            if (const auto reuse = downgrade ? files::verified_slot(root, latest.version) : std::nullopt)
            {
                std::string parse_error;
                const auto local = parts::parse(versions::read_record(root / *reuse / "manifests" / manifest_name, manifest_limit), parse_error);
                if (local && local->part == parts::id::launcher && local->version == latest.version &&
                    local->channel == channel::stable)
                {
                    const auto admission = parts::install_error(runtime.manifests, *local, current, selection.channel, true);
                    if (!admission.empty()) return admission;
                    std::unique_lock lock(mutex_);
                    if (!still_selected()) return "The launcher update was cancelled.";
                    if (auto error = wait_for_request_locked(lock, *reuse, "switch-to-stable", still_selected); !error.empty())
                        return error;
                    ready_locked(*reuse);
                    return {};
                }
            }

            if (cancelled()) return "The launcher update was cancelled.";
            const auto receipt = fetch_small(latest.manifest_url, latest.manifest_size);
            if (!receipt || receipt->size() != latest.manifest_size) return "Could not download the release manifest.";
            std::string parse_error;
            const auto manifest = parts::parse(*receipt, parse_error);
            if (!manifest) return parse_error;
            if (manifest->part != parts::id::launcher || manifest->version != latest.version ||
                (manifest->channel == channel::beta) != latest.prerelease ||
                release_channel::is_prerelease(manifest->version) != latest.prerelease ||
                manifest->files.size() != 1 || manifest->files.front().path != latest.zip ||
                manifest->files.front().size != latest.size)
                return "The launcher manifest does not match the selected release.";
            const auto admission = parts::install_error(runtime.manifests, *manifest, current, selection.channel, downgrade);
            if (!admission.empty()) return admission;

            // Schema 1 has no expansion field. Use the current package as an estimate, then check
            // the exact expansion from the verified ZIP before creating or writing a partial.
            std::error_code disk_error;
            const auto available = fs::space(root, disk_error).available;
            if (disk_error || !files::has_space(available, latest.size, installed_size()))
                return "There is not enough disk space for the launcher ZIP, its extracted files and 10% extra space.";
            if (cancelled()) return "The launcher update was cancelled.";
            const auto sums = fetch_small(latest.sums_url, manifest_limit);
            if (!sums) return "Could not download the release's SHA256SUMS.txt.";
            const auto sha256 = expected_sha256(*sums, latest.zip);
            if (sha256.empty() || manifest->files.front().sha256 != sha256)
                return "The release's SHA256SUMS.txt does not match the manifest for " + latest.zip + ".";

            const auto download = make_download(root);
            if (download.empty()) return "Could not create a launcher update download folder.";
            const auto discard_download = [&]
            {
                try { files::remove_transient(download); }
                catch (const std::exception&) {} // Startup can retry cleanup.
            };
            auto clean = utils::finally(discard_download);
            const auto zip = download / latest.zip;
            std::string actual;
            const auto download_error = checked_download::download_file(latest.zip_url, zip, latest.size,
                [&](std::uint64_t done, std::uint64_t total)
                {
                    if (done) versions::fixture::kill_at(root.parent_path(), "U2-download");
                    std::lock_guard lock(mutex_);
                    state_.done = done;
                    state_.total = total;
                }, cancelled, actual);
            if (!download_error.empty()) return download_error;
            if (actual != sha256) return "The launcher ZIP does not match the release's SHA-256.";

            versions::fixture::kill_at(root.parent_path(), "U2-downloaded");
            std::string folder;
            std::unique_lock finish(mutex_, std::defer_lock);
            {
                files::partial_slot partial(root, latest.version);
                if (const auto error = partial.extract(zip, cancelled); !error.empty()) return error;
                const auto metadata = download / manifest_name;
                if (!versions::write_record(metadata, *receipt)) return "Could not stage the verified release manifest.";

                finish.lock();
                if (!still_selected()) return "The launcher update was cancelled.";
                const auto pointer = versions::load_pointer(root);
                if (const auto error = partial.publish(metadata, downgrade ? nullptr : &pointer.value, folder); !error.empty())
                    return error;
            }
            versions::fixture::kill_at(root.parent_path(), "U6-before-cleanup");
            discard_download();
            clean.cancel();
            if (downgrade)
            {
                if (auto error = wait_for_request_locked(finish, folder, "switch-to-stable", still_selected); !error.empty())
                    return error;
            }
            ready_locked(folder);
            finish.unlock();
            utils::logger::write("launcher update: verified {} (SHA-256 {})", folder, sha256);
            return {};
        }

        rapidjson::Value make_string(std::string_view value, rapidjson::Document::AllocatorType& allocator)
        {
            rapidjson::Value output;
            output.SetString(value.empty() ? "" : value.data(), static_cast<rapidjson::SizeType>(value.size()), allocator);
            return output;
        }
    }

    void cleanup()
    {
        if (!can_install()) return;
        std::lock_guard lock(mutex_);
        if (busy_locked() || stopping_) return;
        try { files::cleanup(versioned_start::layout().versions); }
        catch (const std::exception& error) { utils::logger::write("launcher update cleanup: {}", error.what()); }
    }

    void stop()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            pending_ = {};
        }
        work_ready_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    void check()
    {
        if (utils::flags::has_flag("offline") || !versioned_start::unlocked()) return;
        std::lock_guard lock(mutex_);
        if (stopping_ || busy_locked() || state_.state == "ready") return;
        const auto selection = refresh_.begin(release_source::current_channel());
        if (!selection) return;
        state_.state = "checking";
        queue_locked([work = *selection]
        {
            std::string error;
            bool from_cache{};
            std::optional<release> latest;
            try { latest = fetch_release(work.channel, error, from_cache); }
            catch (const std::exception&) { error = "Could not check for a launcher release."; }
            bool again;
            {
                std::lock_guard lock(mutex_);
                again = refresh_.finish(work, latest.has_value());
                state_.state = "idle";
                if (!again)
                {
                    state_.latest = std::move(latest);
                    state_.selection = work;
                    state_.latest_channel = work.channel;
                    state_.checked = std::chrono::system_clock::now();
                    state_.from_cache = from_cache && state_.latest.has_value();
                    state_.cache_error = state_.from_cache ? error : std::string{};
                    state_.error = state_.latest ? std::string{} : std::move(error);
                    state_.state = state_.latest ? offered_state_locked() : "check-failed";
                }
            }
            if (again) check();
        });
    }

    bool start_download(const std::string& downgrade_to)
    {
        if (utils::flags::has_flag("offline") || !can_install()) return false;
        std::lock_guard lock(mutex_);
        if (stopping_ || busy_locked() || state_.state == "ready" || !state_.latest || legacy_stable() ||
            !refresh_.ready(release_source::current_channel())) return false;
        const auto current = installed_version();
        const auto newer = versions::compare(state_.latest->version, current) > 0;
        const auto back = !downgrade_to.empty() && state_.latest_channel == channel::stable &&
            state_.latest->version == downgrade_to && release_channel::is_ahead_of(current, state_.latest->version);
        if (downgrade_to.empty() ? !newer : !back) return false;

        state_.downgrade = back;
        state_.state = "downloading";
        state_.done = 0;
        state_.total = state_.latest->size;
        state_.error.clear();
        state_.install_error.clear();
        state_.request_deferred = false;
        queue_locked([latest = *state_.latest, selection = state_.selection, back]
        {
            std::string error;
            try { error = stage(latest, selection, back); }
            catch (const std::exception&) { error = "Could not stage the launcher update."; }
            bool again{};
            {
                std::lock_guard state_lock(mutex_);
                if (state_.state != "downloading") return;
                if (!refresh_.current(selection))
                {
                    again = true;
                    state_.state = "idle";
                }
                else if (back && !state_.downgrade)
                    state_.state = offered_state_locked();
                else if (back)
                {
                    state_.install_error = "Switching back to " + latest.version + " failed: " + error;
                    state_.state = offered_state_locked();
                }
                else
                {
                    state_.state = "failed";
                    state_.error = error;
                }
                state_.downgrade = false;
            }
            if (!error.empty()) utils::logger::write("launcher update {}: {}", latest.version, error);
            if (again) check();
        });
        return true;
    }

    bool retry(const std::string& folder)
    {
        if (!can_install() || !versions::valid_folder(folder)) return false;
        std::lock_guard lock(mutex_);
        if (stopping_ || busy_locked() || state_.state == "ready") return false;
        const auto& layout = versioned_start::layout();
        const auto previous = versions::load_pointer(layout.versions);
        const auto recorded = previous.source == versions::pointer_source::primary && previous.value.current == layout.folder;
        if (folder == layout.folder || (recorded && !std::ranges::any_of(previous.value.failed,
            [&](const auto& failure) { return failure.folder == folder; }))) return false;

        state_.state = "downloading";
        state_.retrying = true;
        state_.done = state_.total = 0;
        state_.error.clear();
        state_.install_error.clear();
        state_.request_deferred = false;
        queue_locked([root = layout.versions, folder]
        {
            versions::verification valid;
            try { valid = versions::verify(root / folder, versions::check_mode::full); }
            catch (const std::exception&) { valid.bad_file = "version.txt"; }
            std::unique_lock state_lock(mutex_);
            auto error = valid ? wait_for_request_locked(state_lock, folder, "retry",
                [] { return true; }, valid.fingerprint) :
                "That launcher version failed verification (" + valid.bad_file + ").";
            state_.retrying = false;
            if (error.empty())
                ready_locked(folder);
            else
            {
                state_.state = offered_state_locked();
                state_.install_error = std::move(error);
            }
        });
        return true;
    }

    bool restart(std::string& error)
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || state_.state != "ready" || !can_install())
        {
            error = "not-ready";
            return false;
        }
        const auto& root = versioned_start::layout().versions;
        const auto request = versions::parse_request(versions::read_record(root / "request.txt"));
        if (!versions::verify(root / state_.ready_folder, versions::check_mode::quick) ||
            (!state_.request_id.empty() && (!request || request->id != state_.request_id ||
                request->trial != state_.ready_folder)) || !versioned_start::request_restart())
        {
            error = "not-ready";
            return false;
        }
        return true;
    }

    void write_status(rapidjson::Value& out, rapidjson::Document::AllocatorType& allocator)
    {
        const auto current = installed_version();
        const auto installable = can_install();
        const auto& layout = versioned_start::layout();
        const auto pointer = layout ? versions::load_pointer(layout.versions).value : versions::pointer{};
        const auto failed = std::ranges::find_if(pointer.failed,
            [&](const auto& failure) { return failure.folder != layout.folder; });
        const auto updated = layout && versioned_start::unlocked() && utils::flags::has_flag("-trial") &&
            pointer.current == layout.folder && !pointer.previous.empty();

        std::lock_guard lock(mutex_);
        out.SetObject();
        const auto add = [&](const char* key, std::string_view value)
            { out.AddMember(rapidjson::StringRef(key), make_string(value, allocator), allocator); };
        out.AddMember("enabled", !current.empty(), allocator);
        out.AddMember("canInstall", installable, allocator);
        add("current", current);
        add("state", state_.state);
        add("latest", state_.latest ? state_.latest->version : std::string{});
        add("page", page_locked());
        out.AddMember("size", state_.latest ? state_.latest->size : std::uint64_t{0}, allocator);
        out.AddMember("done", state_.done, allocator);
        out.AddMember("total", state_.total, allocator);
        add("error", state_.error);
        add("installError", state_.install_error);
        add("readyVersion", versions::folder_version(state_.ready_folder));
        out.AddMember("requestDeferred", state_.state == "ready" && state_.request_deferred, allocator);
        add("updatedTo", updated ? current : std::string{});
        const auto checked = std::chrono::duration_cast<std::chrono::milliseconds>(state_.checked.time_since_epoch()).count();
        out.AddMember("checkedAt", static_cast<std::int64_t>(checked), allocator);
        add("channel", release_channel::channel_name(release_source::current_channel()));
        add("latestChannel", release_channel::channel_name(state_.latest_channel));
        out.AddMember("prerelease", release_channel::is_prerelease(current), allocator);
        out.AddMember("aheadOfStable", state_.latest && state_.latest_channel == channel::stable &&
            release_channel::is_ahead_of(current, state_.latest->version), allocator);
        out.AddMember("stableNeedsExtraction", legacy_stable(), allocator);
        out.AddMember("channelChangeAllowed", channel_change_allowed_locked(), allocator);
        out.AddMember("fromCache", state_.from_cache, allocator);
        add("cacheError", state_.cache_error);
        out.AddMember("downgrade", state_.downgrade, allocator);
        add("failedFolder", failed != pointer.failed.end() ? failed->folder : std::string{});
        add("failedVersion", failed != pointer.failed.end() ? versions::folder_version(failed->folder) : std::string_view{});
        add("failedReason", failed != pointer.failed.end() ? failed->reason : std::string{});
    }

    std::string release_page()
    {
        std::lock_guard lock(mutex_);
        return page_locked();
    }

    std::string installed_version()
    {
#ifdef _DEBUG
        return {};
#else
        const auto build = release_channel::launcher_build(GIT_DESCRIBE, GIT_TAG, VERSION_PRODUCT);
        return build.release && versions::valid_version(build.version) ? build.version : std::string{};
#endif
    }

    std::string running_version()
    {
        return release_channel::launcher_build(GIT_DESCRIBE, GIT_TAG, VERSION_PRODUCT).version;
    }

    bool revoke_downgrade()
    {
        std::lock_guard lock(mutex_);
        return !stopping_ && revoke_locked();
    }

    bool channel_changed(channel selected)
    {
        if (!versioned_start::unlocked()) return false;
        {
            std::lock_guard lock(mutex_);
            if (release_source::current_channel() == selected) return true;
            if (!channel_change_allowed_locked() || !revoke_locked()) return false;
            release_source::set_channel(selected);
            if (release_source::current_channel() != selected) return false;
            refresh_.invalidate();
            state_.latest.reset();
            state_.from_cache = false;
            state_.cache_error.clear();
            state_.checked = {};
            state_.error.clear();
            if (!busy_locked() && state_.state != "ready") state_.state = "idle";
        }
        check();
        return true;
    }
}
