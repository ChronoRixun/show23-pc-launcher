#include "std_include.hpp"
#include "part_store.hpp"
#include "show23/input_paths.hpp"

#include <TlHelp32.h>
#include <atomic>
#include <unordered_set>
#include <utility>
#include <utils/finally.hpp>

#ifdef SHOW23_UPDATE_FIXTURE
#include <versions/update_fixture.hpp>
#endif

namespace parts
{
    namespace
    {
        namespace fs = std::filesystem;
        using namespace show23::input_paths;
        constexpr std::array runtime_parts{id::emulator, id::server, id::profile};
        constexpr std::uint64_t manifest_limit = 256 * 1024;

        enum class directory_state { missing, present, invalid };
        enum class fresh_state { empty, complete, invalid };

        bool usable_directory(const location& directory)
        {
            return directory.error == path_error::none && directory.directory;
        }

        directory_state directory_at(const fs::path& path)
        {
            const auto item = inspect(path, true);
            if (item.error != path_error::none || (item.present && !item.directory)) return directory_state::invalid;
            return item.present ? directory_state::present : directory_state::missing;
        }

        HANDLE open_lease(const fs::path& install, bool exclusive, bool create, bool& busy)
        {
            const auto parent = create ? ensure_directory(install / "manifests") : inspect(install / "manifests", true);
            if (!usable_directory(parent)) return nullptr;
            const auto file = CreateFileW(extended(parent.path / "runtime.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                exclusive ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                busy = GetLastError() == ERROR_SHARING_VIOLATION;
                return nullptr;
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if (GetFileInformationByHandle(file, &info) &&
                !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                info.nNumberOfLinks == 1) return file;
            CloseHandle(file);
            return nullptr;
        }

        bool runtime_process_running(const fs::path& slot)
        {
            if (slot.empty()) return false;
            const auto active = inspect(slot);
            if (!usable_directory(active)) return false;

            const handle processes(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
            if (!processes) return true;
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (!Process32FirstW(processes.get(), &entry)) return true;

            std::wstring image(32768, L'\0');
            do
            {
                // Windows can restrict queries to unrelated protected or other-user processes.
                const handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
                if (!process) continue;
                auto length = static_cast<DWORD>(image.size());
                if (!QueryFullProcessImageNameW(process.get(), 0, image.data(), &length)) continue;
                std::wstring_view path(image.data(), length);
                if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
                const fs::path executable(path);
                if (contains(active.path, executable) || contains(slot, executable)) return true;
            } while (Process32NextW(processes.get(), &entry));
            return GetLastError() != ERROR_NO_MORE_FILES;
        }

        bool move(const fs::path& from, const fs::path& to)
        {
            return MoveFileExW(extended(from).c_str(), extended(to).c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
        }

        void fixture_cut(const fs::path& bundle, std::string_view label)
        {
#ifdef SHOW23_UPDATE_FIXTURE
            versions::fixture::kill_at(bundle.parent_path().parent_path(), label);
#else
            (void)bundle;
            (void)label;
#endif
        }

        bool complete_layout(const installed_runtime& runtime)
        {
            const auto listed = [&](id part, std::string_view path)
            {
                const auto& item = runtime.manifests[index(part)];
                return item && std::ranges::any_of(item->files, [&](const auto& file) { return file.path == path; });
            };
            return listed(id::emulator, "shadPS4.exe") && listed(id::server, "python/python.exe") &&
                listed(id::server, "olserver.py") && listed(id::server, "roster_decode.py") &&
                listed(id::server, "card_import.py") && listed(id::server, "tables/ol-messages.csv") &&
                std::ranges::any_of(runtime.manifests[index(id::server)]->files, [](const auto& file)
                {
                    return file.path.starts_with("replies/");
                });
        }

        bool copy_file_checked(const fs::path& source, const fs::path& target)
        {
            const auto input = inspect(source, false, true);
            if (!input.present || input.error != path_error::none || input.directory) return false;
            const auto parent = ensure_directory(target.parent_path());
            if (!parent.directory || parent.error != path_error::none) return false;
            return CopyFileW(extended(input.path).c_str(), extended(parent.path / target.filename()).c_str(), TRUE) != FALSE;
        }

        bool separate_roots(const fs::path& bundle, const fs::path& install)
        {
            const auto source = inspect(bundle);
            const auto target = inspect(install, true);
            return usable_directory(source) && target.error == path_error::none && (!target.present || target.directory) &&
                !contains(source.path, target.path) && !contains(target.path, source.path);
        }

        bool versions_match(const installed_runtime& installed, const installed_runtime& source)
        {
            return installed.error.empty() && std::ranges::all_of(runtime_parts, [&](const auto part)
            {
                const auto& actual = installed.manifests[index(part)];
                const auto& expected = source.manifests[index(part)];
                return actual && expected && actual->version == expected->version;
            });
        }

        bool frozen_source(const installed_runtime& source, std::string_view launcher_version)
        {
            if (!complete_layout(source)) return false;
            const auto release = source.root.filename();
            if (!std::ranges::all_of(runtime_parts, [&](const auto part)
            {
                const auto& manifest = source.manifests[index(part)];
                return manifest && fs::path(manifest->version) == release;
            })) return false;
            std::string verification;
            return verify_runtime(source, launcher_version, verification);
        }

        std::optional<std::string> manifest_bytes(const fs::path& path)
        {
            const auto input = inspect(path, false, true);
            if (!input.present || input.directory || input.error != path_error::none ||
                !input.length || input.length > manifest_limit) return std::nullopt;
            std::string bytes(static_cast<std::size_t>(input.length), '\0');
            if (!read_at(input.pins.back().get(), 0, bytes)) return std::nullopt;
            return bytes;
        }

        bool marker_missing(const fs::path& install)
        {
            const auto marker = inspect(install / "manifests" / "runtime.txt", true);
            return marker.error == path_error::none && !marker.present;
        }

        std::string fresh_record(const std::string& release)
        {
            return "show23-fresh-runtime-v1\n" + release + '\n';
        }

        bool fresh_stage_name(std::wstring_view name)
        {
            if (!name.starts_with(L".bundled-")) return false;
            name.remove_prefix(std::wstring_view(L".bundled-").size());
            for (unsigned field = 0; field != 3; ++field)
            {
                const auto hyphen = name.find(L'-');
                const auto number = name.substr(0, hyphen);
                if (number.empty() || number.find_first_not_of(L"0123456789") != number.npos) return false;
                if (field == 2) return hyphen == name.npos;
                if (hyphen == name.npos) return false;
                name.remove_prefix(hyphen + 1);
            }
            return false;
        }

        std::vector<fs::path> recorded_fresh_stages(const fs::path& runtime, const std::string& release)
        {
            const auto root = inspect(runtime);
            if (!usable_directory(root)) return {};
            const auto record = fresh_record(release);
            std::vector<fs::path> stages;
            std::error_code error;
            fs::directory_iterator entry(extended(root.path), error), end;
            for (; !error && entry != end; entry.increment(error))
            {
                const auto filename = entry->path().filename();
                if (!fresh_stage_name(filename.native())) continue;
                const auto stage = root.path / filename;
                if (directory_at(stage) == directory_state::present && manifest_bytes(stage / ".fresh-stage") == record)
                    stages.push_back(stage);
            }
            return error ? std::vector<fs::path>{} : stages;
        }

        bool exact_inventory(const fs::path& root, std::unordered_set<fs::path> listed)
        {
            const auto pin = inspect(root);
            if (!usable_directory(pin)) return false;
            const fs::path absolute = extended(pin.path);
            std::error_code error;
            fs::recursive_directory_iterator entry(absolute, error), end;
            for (; !error && entry != end; entry.increment(error))
            {
                const auto attributes = GetFileAttributesW(entry->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
                if (attributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (listed.erase(entry->path().lexically_relative(absolute).make_preferred()) != 1) return false;
            }
            return !error && listed.empty();
        }

        bool exact_manifests(const fs::path& actual, const fs::path& expected)
        {
            std::unordered_set<fs::path> listed;
            for (const auto part : runtime_parts)
            {
                const auto file = std::string(name(part)) + ".json";
                const auto bytes = manifest_bytes(expected / file);
                if (!bytes || bytes != manifest_bytes(actual / file)) return false;
                listed.emplace(file);
            }
            return exact_inventory(actual, std::move(listed));
        }

        bool exact_runtime_files(const installed_runtime& runtime)
        {
            std::unordered_set<fs::path> listed;
            for (const auto part : runtime_parts)
            {
                const auto& manifest = runtime.manifests[index(part)];
                if (!manifest) return false;
                const auto folder = component_folder(runtime, part).lexically_relative(runtime.root);
                for (const auto& file : manifest->files) listed.insert((folder / file.path).make_preferred());
            }
            return exact_inventory(runtime.root, std::move(listed));
        }

        fresh_state existing_slots(const fs::path& install, const fs::path& release)
        {
            const auto runtime = inspect(install / "runtime", true);
            if (runtime.error != path_error::none) return fresh_state::invalid;
            if (!runtime.present) return fresh_state::empty;
            if (!runtime.directory) return fresh_state::invalid;
            WIN32_FIND_DATAW entry{};
            const auto search = FindFirstFileW(extended(runtime.path / L"*").c_str(), &entry);
            if (search == INVALID_HANDLE_VALUE)
                return GetLastError() == ERROR_FILE_NOT_FOUND ? fresh_state::empty : fresh_state::invalid;
            const auto close = utils::finally([&] { FindClose(search); });
            auto result = fresh_state::empty;
            do
            {
                const std::wstring_view name(entry.cFileName);
                if (name.starts_with(L'.') || !(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (result != fresh_state::empty || fs::path(name) != release ||
                    (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return fresh_state::invalid;
                result = fresh_state::complete;
            } while (FindNextFileW(search, &entry));
            return GetLastError() == ERROR_NO_MORE_FILES ? result : fresh_state::invalid;
        }

        struct fresh_plan
        {
            fresh_state state{fresh_state::invalid};
            fs::path staged_manifests;
        };

        fresh_plan inspect_fresh(const fs::path& bundle, const fs::path& install, const std::string& release,
                                 std::string_view launcher_version)
        {
            if (!marker_missing(install)) return {};
            const auto slots = existing_slots(install, release);
            if (slots == fresh_state::invalid) return {};
            const auto target = install / "manifests" / release;
            const auto manifests = directory_at(target);
            const auto expected = bundle / "manifests" / release;
            if (manifests == directory_state::invalid ||
                (manifests == directory_state::present && !exact_manifests(target, expected))) return {};
            if (slots == fresh_state::empty)
                return directory_at(install / "runtime" / release) == directory_state::missing ? fresh_plan{slots, {}} : fresh_plan{};

            // The plain runtime is published first. If interrupted before the manifest rename,
            // recognize its own recorded staging directory and verify the already-published files.
            fs::path owner = install;
            fs::path staged_manifests;
            if (manifests == directory_state::missing)
            {
                for (const auto& stage : recorded_fresh_stages(install / "runtime", release))
                {
                    const auto candidate = stage / "manifests" / release;
                    if (directory_at(stage / "runtime" / release) != directory_state::missing ||
                        !exact_manifests(candidate, expected)) continue;
                    if (!staged_manifests.empty()) return {};
                    owner = stage;
                    staged_manifests = candidate;
                }
                if (staged_manifests.empty()) return {};
            }

            auto existing = read_runtime(owner, release);
            existing.root = install / "runtime" / release;
            std::string verification;
            return exact_runtime_files(existing) && verify_runtime(existing, launcher_version, verification)
                ? fresh_plan{slots, staged_manifests} : fresh_plan{};
        }

        fs::path create_stage(const fs::path& parent)
        {
            static std::atomic_uint serial{};
            const auto stage = parent / (".bundled-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(GetTickCount64()) + "-" + std::to_string(++serial));
            return CreateDirectoryW(extended(stage).c_str(), nullptr) ? stage : fs::path{};
        }

        void remove_stage(const fs::path& stage)
        {
            std::error_code ignored;
            fs::remove_all(extended(stage), ignored);
        }

        bool private_stage_tree(const fs::path& stage)
        {
            const auto root = inspect(stage);
            if (!usable_directory(root)) return false;
            const fs::path absolute = extended(root.path);
            std::error_code error;
            fs::recursive_directory_iterator entry(absolute, error), end;
            for (; !error && entry != end; entry.increment(error))
            {
                const auto child = inspect(root.path / entry->path().lexically_relative(absolute));
                if (child.error != path_error::none || !child.present || (!child.directory && child.links != 1)) return false;
            }
            return !error;
        }

        void clean_fresh_stages(const fs::path& runtime, const std::string& release)
        {
            for (const auto& stage : recorded_fresh_stages(runtime, release))
                if (private_stage_tree(stage)) remove_stage(stage);
        }

        bool stage_copy(const fs::path& bundle, const installed_runtime& source, const fs::path& stage,
                        const std::string& release, std::string_view launcher_version)
        {
            const installed_runtime staged{stage / "runtime" / release, source.manifests, {}};
            for (const auto part : runtime_parts)
            {
                for (const auto& file : source.manifests[index(part)]->files)
                    if (!copy_file_checked(component_folder(source, part) / file.path,
                                           component_folder(staged, part) / file.path)) return false;
                const auto file = std::string(name(part)) + ".json";
                if (!copy_file_checked(bundle / "manifests" / release / file, stage / "manifests" / release / file)) return false;
            }
            std::string verification;
            return exact_manifests(stage / "manifests" / release, bundle / "manifests" / release) &&
                verify_runtime(read_runtime(stage, release), launcher_version, verification);
        }

        struct slot_paths
        {
            fs::path runtime, manifests;
            slot_paths(const fs::path& root, const std::string& release)
                : runtime(root / "runtime" / release), manifests(root / "manifests" / release) {}
        };

        struct repair_plan
        {
            fs::path transaction;
            std::string error;
            bool pending{};
            bool save_runtime{};
            bool save_manifests{};
            bool publish_manifests{};
        };

        repair_plan plan_repair(const fs::path& bundle, const fs::path& install, const installed_runtime& source,
                                const installed_runtime& active, const std::string& release, std::string_view launcher_version)
        {
            if (marker_missing(install) || active.root.filename() != fs::path(release))
                return {{}, "The active runtime release could not be established as " + release + "."};

            const slot_paths live(install, release);
            const auto live_runtime = directory_at(live.runtime);
            const auto live_manifests = directory_at(live.manifests);
            if (live_runtime == directory_state::invalid || live_manifests == directory_state::invalid)
                return {{}, "The active runtime paths are invalid."};

            for (unsigned number = 1; number != 0; ++number)
            {
                const auto transaction = install / "runtime" / (".repair-" + release + "-" + std::to_string(number));
                const auto state = directory_at(transaction);
                if (state == directory_state::missing)
                {
                    if (!versions_match(active, source))
                        return {{}, "The active part versions could not be established as the bundled versions."};
                    return {transaction, {}, false, live_runtime == directory_state::present, true, true};
                }
                if (state == directory_state::invalid) break;

                // The replacement runtime is always published last. Completed transactions retain the originals.
                const slot_paths staged(transaction / "new", release);
                const slot_paths old(transaction / "old", release);
                const auto stage_runtime = directory_at(staged.runtime);
                const auto stage_manifests = directory_at(staged.manifests);
                const auto old_runtime = directory_at(old.runtime);
                const auto old_manifests = directory_at(old.manifests);
                if (stage_runtime == directory_state::invalid || stage_manifests == directory_state::invalid ||
                    old_runtime == directory_state::invalid || old_manifests == directory_state::invalid) break;
                if (stage_runtime == directory_state::missing)
                {
                    if (stage_manifests == directory_state::missing && old_manifests == directory_state::present) continue;
                    break;
                }

                const bool publish_manifests = stage_manifests == directory_state::present;
                const bool save_manifests = old_manifests == directory_state::missing;
                const bool save_runtime = live_runtime == directory_state::present;
                if ((save_runtime && (old_runtime == directory_state::present || !save_manifests)) ||
                    (save_manifests && (!publish_manifests || live_manifests != directory_state::present)) ||
                    (!save_manifests && (publish_manifests == (live_manifests == directory_state::present)))) break;
                const auto original = save_manifests ? active : read_runtime(transaction / "old", release);
                if (!versions_match(original, source))
                    return {{}, "The original runtime part versions could not be established as the bundled versions."};

                const auto manifest_owner = publish_manifests ? transaction / "new" : install;
                auto candidate = read_runtime(manifest_owner, release);
                candidate.root = staged.runtime;
                std::string verification;
                if (!exact_manifests(manifest_owner / "manifests" / release, bundle / "manifests" / release) ||
                    !exact_runtime_files(candidate) ||
                    !verify_runtime(candidate, launcher_version, verification))
                    return {{}, "The interrupted repair's replacement does not match the verified bundle."};
                return {transaction, {}, true, save_runtime, save_manifests, publish_manifests};
            }
            return {{}, "The previous repair's folders could not be verified."};
        }
    }

    runtime_lease::runtime_lease(const fs::path& install, const bool exclusive)
    {
        handle_ = open_lease(install, exclusive, true, busy_);
    }

    runtime_lease::~runtime_lease()
    {
        if (handle_) CloseHandle(handle_);
    }

    bool runtime_in_use(const fs::path& install)
    {
        bool busy = false;
        const auto handle = open_lease(install, true, false, busy);
        if (handle) CloseHandle(handle);
        return busy || runtime_process_running(read_runtime(install).root);
    }

    runtime_check inspect_bundled_runtime(const fs::path& bundle, const fs::path& install, std::string_view launcher_version)
    {
        const auto active = read_runtime(install);
        std::string verification;
        if (verify_runtime(active, launcher_version, verification)) return {true, false, false, {}};
        const auto source = read_runtime(bundle);
        if (!separate_roots(bundle, install) || !frozen_source(source, launcher_version)) return {false, false, false, verification};
        const auto release = source.root.filename().string();
        if (marker_missing(install))
        {
            if (inspect_fresh(bundle, install, release, launcher_version).state != fresh_state::invalid)
                return {false, true, false, {}};
            return {false, false, false, "The runtime marker is missing and the existing folders could not be verified as a fresh stage."};
        }
        const auto repair = plan_repair(bundle, install, source, active, release, launcher_version);
        return {false, false, repair.error.empty(), verification};
    }

    bool stage_bundled_runtime(const fs::path& bundle, const fs::path& install, std::string_view launcher_version)
    {
        std::string verification;
        if (verify_runtime(read_runtime(install), launcher_version, verification)) return true;
        if (!marker_missing(install) || !separate_roots(bundle, install)) return false;
        const auto source = read_runtime(bundle);
        if (!frozen_source(source, launcher_version)) return false;
        const auto release = source.root.filename().string();
        if (inspect_fresh(bundle, install, release, launcher_version).state == fresh_state::invalid) return false;

        const runtime_lease lease(install, true);
        if (verify_runtime(read_runtime(install), launcher_version, verification)) return true;
        if (!lease) return false;
        const auto fresh = inspect_fresh(bundle, install, release, launcher_version);
        if (fresh.state == fresh_state::invalid) return false;
        const auto manifests = inspect(install / "manifests");
        const auto runtime = ensure_directory(install / "runtime");
        if (!usable_directory(runtime) || !usable_directory(manifests)) return false;
        const slot_paths target(install, release);
        const auto publish_manifests = [&](const fs::path& candidate)
        {
            fixture_cut(bundle, "runtime-fresh-before-manifests");
            if (!move(candidate, target.manifests)) return false;
            fixture_cut(bundle, "runtime-fresh-after-manifests");
            return true;
        };
        const auto publish_marker = [&]
        {
            if (!marker_missing(install) ||
                !exact_manifests(target.manifests, bundle / "manifests" / release) ||
                !atomically_write(manifests, "runtime.txt", release + '\n')) return false;
            clean_fresh_stages(runtime.path, release);
            return true;
        };
        if (fresh.state == fresh_state::complete)
        {
            if (!fresh.staged_manifests.empty() && !publish_manifests(fresh.staged_manifests)) return false;
            return publish_marker();
        }

        const auto stage = create_stage(runtime.path);
        if (stage.empty()) return false;
        auto clean = utils::finally([&] { remove_stage(stage); });
        const slot_paths candidate(stage, release);
        if (!atomically_write(inspect(stage), ".fresh-stage", fresh_record(release)) ||
            !stage_copy(bundle, source, stage, release, launcher_version))
            return false;
        fixture_cut(bundle, "runtime-fresh-before-slot");
        if (!move(candidate.runtime, target.runtime)) return false;
        clean.cancel(); // On interruption the recorded stage supplies the exact manifests.
        fixture_cut(bundle, "runtime-fresh-after-slot");
        if (directory_at(target.manifests) == directory_state::missing && !publish_manifests(candidate.manifests))
            return false;
        return publish_marker();
    }

    std::string repair_bundled_runtime(const fs::path& bundle, const fs::path& install, std::string_view launcher_version)
    {
        if (!separate_roots(bundle, install)) return "The runtime install and bundle paths are invalid.";
        if (runtime_in_use(install)) return "in-use";
        const auto source = read_runtime(bundle);
        if (!frozen_source(source, launcher_version)) return "The bundled runtime failed verification.";
        const auto release = source.root.filename().string();
        // Validate eligibility without creating a lock file on an unrecognized install.
        const auto before = plan_repair(bundle, install, source, read_runtime(install), release, launcher_version);
        if (!before.error.empty()) return before.error;
        const runtime_lease lease(install, true);
        if (!lease) return lease.busy() ? "in-use" : "The runtime repair lease could not be opened.";
        const auto active = read_runtime(install);
        std::string verification;
        if (active.root.filename() == fs::path(release) && versions_match(active, source) &&
            verify_runtime(active, launcher_version, verification)) return {};
        const auto plan = plan_repair(bundle, install, source, active, release, launcher_version);
        if (!plan.error.empty()) return plan.error;

        const auto runtime = ensure_directory(install / "runtime");
        const auto manifests = inspect(install / "manifests");
        if (!usable_directory(runtime) || !usable_directory(manifests)) return "The runtime folders could not be opened for repair.";
        if (!plan.pending)
        {
            const auto stage = create_stage(runtime.path);
            if (stage.empty()) return "The replacement runtime folder could not be created.";
            auto clean = utils::finally([&] { remove_stage(stage); });
            const slot_paths old(stage / "old", release);
            if (!stage_copy(bundle, source, stage / "new", release, launcher_version) ||
                !usable_directory(ensure_directory(old.runtime.parent_path())) ||
                !usable_directory(ensure_directory(old.manifests.parent_path())))
                return "The replacement runtime could not be copied and verified.";
            if (!move(stage, plan.transaction)) return "The verified replacement could not be prepared for publishing.";
            clean.cancel();
        }

        const slot_paths live(install, release);
        const slot_paths old(plan.transaction / "old", release);
        const slot_paths staged(plan.transaction / "new", release);
        // Each rename is resumable, and no existing slot is removed.
        if (plan.save_runtime)
        {
            const auto deadline = GetTickCount64() + 1500;
            for (;;)
            {
                // A direct launch can start while Repair waits for a closed image to be released.
                if (runtime_process_running(live.runtime)) return "in-use";
                if (move(live.runtime, old.runtime)) break;
                const auto error = GetLastError();
                if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) ||
                    GetTickCount64() >= deadline)
                    return "The original runtime could not be moved to its backup.";
                Sleep(25);
            }
            fixture_cut(bundle, "runtime-repair-after-save-slot");
        }
        if (plan.save_manifests)
        {
            if (!move(live.manifests, old.manifests)) return "The original manifests could not be moved to their backup.";
            fixture_cut(bundle, "runtime-repair-after-save-manifests");
        }
        if (plan.publish_manifests)
        {
            if (!move(staged.manifests, live.manifests)) return "The replacement manifests could not be published.";
            fixture_cut(bundle, "runtime-repair-after-publish-manifests");
        }
        if (!move(staged.runtime, live.runtime)) return "The replacement runtime could not be published.";
        fixture_cut(bundle, "runtime-repair-after-publish-slot");
        return verify_runtime(read_runtime(install), launcher_version, verification) ? std::string{} : verification;
    }
}
