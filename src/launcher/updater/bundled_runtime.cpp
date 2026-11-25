#include "std_include.hpp"
#include "part_store.hpp"
#include "show23/card_import.hpp"
#include "show23/input_paths.hpp"

#include <TlHelp32.h>
#include <atomic>
#include <unordered_set>
#include <utility>
#include <utils/cryptography.hpp>
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
            return play_layout_error(runtime).empty() &&
                listed(id::server, "roster_decode.py") && listed(id::server, "manual_mode.py") &&
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
                return manifest && fs::path(manifest->version) == release &&
                    release_channel::compare(launcher_version, manifest->min_launcher) >= 0;
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
            const auto state = inspect(install / "data/profiles/default/server/state.json", true);
            if (state.present || state.error != path_error::none) return {};
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

        fs::path history_folder(const fs::path& install)
        {
            return install / "manifests/runtime-history";
        }

        std::string previous_runtime(const fs::path& install, const std::string& active)
        {
            auto previous = manifest_bytes(history_folder(install) / (active + ".txt")).value_or("");
            if (!previous.empty() && previous.back() == '\n') previous.pop_back();
            return is_version(previous) && release_channel::compare(previous, active) < 0 ? previous : std::string{};
        }

        bool newer_bundle(const installed_runtime& active, const installed_runtime& source)
        {
            return active.error.empty() && !active.root.empty() && !source.root.empty() &&
                release_channel::compare(source.root.filename().string(), active.root.filename().string()) > 0;
        }

        std::string publish_inactive_slot(const fs::path& bundle, const fs::path& install, const installed_runtime& source,
                                          const std::string& release, std::string_view launcher_version)
        {
            const slot_paths target(install, release), expected(bundle, release);
            const auto manifests = directory_at(target.manifests);
            const auto files = directory_at(target.runtime);
            if (files == directory_state::invalid || manifests == directory_state::invalid ||
                (files == directory_state::present && manifests != directory_state::present) ||
                (manifests == directory_state::present && !exact_manifests(target.manifests, expected.manifests)))
                return "An inactive runtime slot conflicts with the bundled update.";

            if (files == directory_state::missing)
            {
                const auto runtime = ensure_directory(install / "runtime");
                if (!usable_directory(runtime)) return "Could not open the runtime folder.";
                const auto stage = create_stage(runtime.path);
                if (stage.empty()) return "Could not create a runtime staging folder.";
                const auto clean = utils::finally([&] { remove_stage(stage); });
                const slot_paths candidate(stage, release);
                if (!atomically_write(inspect(stage), ".fresh-stage", fresh_record(release)) ||
                    !stage_copy(bundle, source, stage, release, launcher_version))
                    return "Could not copy and verify the bundled runtime.";
                fixture_cut(bundle, "runtime-update-after-copy");
                if (manifests == directory_state::missing && !move(candidate.manifests, target.manifests))
                    return "Could not publish the new runtime manifests.";
                fixture_cut(bundle, "runtime-update-after-manifests");
                if (!move(candidate.runtime, target.runtime)) return "Could not publish the new runtime files.";
                fixture_cut(bundle, "runtime-update-after-slot");
            }

            const auto installed = read_runtime(install, release);
            std::string verification;
            if (!exact_manifests(target.manifests, expected.manifests) || !exact_runtime_files(installed) ||
                !verify_play_runtime(installed, launcher_version, verification))
                return verification.empty() ? "The staged runtime did not match the bundle." : verification;
            return {};
        }

        bool sharing_error(const DWORD error)
        {
            return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_USER_MAPPED_FILE;
        }

        bool previous_repair_establishes_version(const fs::path& install, const installed_runtime& source,
                                                 const std::string& release, std::string_view launcher_version)
        {
            // Older Repair could move the active manifests away before publishing their replacements.
            // Require both the original version records and a verified staged runtime to recover that state.
            for (unsigned number = 1; number != 0; ++number)
            {
                const auto transaction = install / "runtime" / (".repair-" + release + "-" + std::to_string(number));
                if (directory_at(transaction) != directory_state::present) break;
                const auto staged = transaction / "new" / "runtime" / release;
                if (directory_at(staged) != directory_state::present ||
                    !versions_match(read_runtime(transaction / "old", release), source)) continue;
                const installed_runtime candidate{staged, source.manifests, {}};
                std::string verification;
                if (exact_runtime_files(candidate) && verify_runtime(candidate, launcher_version, verification)) return true;
            }
            return false;
        }

        std::string repair_error(const fs::path& install, const installed_runtime& source, const installed_runtime& active,
                                 const std::string& release, std::string_view launcher_version)
        {
            if (active.root.filename() != fs::path(release))
                return "The active runtime release could not be established as " + release + ".";
            const slot_paths live(install, release);
            if (directory_at(live.runtime) == directory_state::invalid || directory_at(live.manifests) == directory_state::invalid)
                return "The active runtime paths are invalid.";
            if (versions_match(active, source)) return {};

            constexpr auto unknown = "The active part versions could not be established as the bundled versions.";
            for (const auto part : runtime_parts)
            {
                const auto& actual = active.manifests[index(part)];
                if (actual)
                {
                    if (actual->version != source.manifests[index(part)]->version) return unknown;
                    continue;
                }
                const auto item = inspect(live.manifests / (std::string(name(part)) + ".json"), true, true);
                if (item.error == path_error::access && sharing_error(GetLastError())) return "in-use";
                if (item.error != path_error::none || item.present) return unknown;
            }
            return previous_repair_establishes_version(install, source, release, launcher_version) ? std::string{} : unknown;
        }

        bool same_manifest(const manifest& lhs, const manifest& rhs)
        {
            return lhs.part == rhs.part && lhs.version == rhs.version && lhs.channel == rhs.channel &&
                lhs.min_launcher == rhs.min_launcher && lhs.compatibility == rhs.compatibility &&
                std::ranges::is_permutation(lhs.files, rhs.files, [](const auto& left, const auto& right)
                {
                    return left.path == right.path && left.size == right.size && left.sha256 == right.sha256;
                });
        }

        std::string repair_file(const fs::path& bundle, const fs::path& source, const fs::path& destination,
                                std::uint64_t size, const std::string& digest, unsigned& replacement)
        {
            auto target = inspect(destination, true, true);
            const auto open_error = GetLastError();
            const auto filename = destination.filename();
            if (target.error != path_error::none)
                return target.error == path_error::access && sharing_error(open_error) ? "in-use" :
                    "The repair target could not be opened: " + filename.string();
            if (target.directory) return "The repair target is a directory: " + filename.string();
            if (target.present && target.length == size)
            {
                const auto actual = sha256(target);
                if (!actual) return sharing_error(GetLastError()) ? "in-use" :
                    "The repair target could not be read: " + filename.string();
                if (*actual == digest) return {};
            }
            if (target.links > 1) return "The repair target has multiple hard links: " + filename.string();

            const auto input = inspect(source, false, true);
            const auto parent = ensure_directory(destination.parent_path());
            if (!input.present || input.directory || input.error != path_error::none || input.length != size ||
                !usable_directory(parent)) return "The replacement file could not be prepared: " + filename.string();
            const auto sequence = std::to_string(++replacement);
            fixture_cut(bundle, "runtime-repair-before-file-" + sequence);

            const auto temporary = parent.path / (".show23-repair-" + std::to_string(GetCurrentProcessId()) + "-" +
                std::to_string(GetTickCount64()) + "-" + sequence);
            const auto temporary_path = extended(temporary);
            location output;
            output.pins.emplace_back(CreateFileW(temporary_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!output.pins.back()) return "A temporary repair file could not be created: " + filename.string();
            auto cleanup = utils::finally([&]
            {
                output.pins.clear();
                DeleteFileW(temporary_path.c_str());
            });
            output.present = true;
            const auto file = output.pins.back().get();
            std::array<char, 64 * 1024> bytes{};
            while (output.length < size)
            {
                const auto length = static_cast<DWORD>((std::min<std::uint64_t>)(bytes.size(), size - output.length));
                DWORD written = 0;
                if (!read_at(input.pins.back().get(), output.length, std::span(bytes).first(length)) ||
                    !WriteFile(file, bytes.data(), length, &written, nullptr) || written != length)
                    return "The replacement file could not be copied: " + filename.string();
                output.length += written;
            }
            if (!FlushFileBuffers(file) || sha256(output) != digest)
                return "The replacement file could not be verified: " + filename.string();

            if (!target.present)
            {
                const auto current = inspect(parent.path / filename, true, true);
                if (current.error == path_error::access && sharing_error(GetLastError())) return "in-use";
                if (current.error != path_error::none || current.present)
                    return "The repair target changed during the copy: " + filename.string();
            }
            // Keep the parent folders pinned and release our file handles before asking Windows to replace the target.
            // The rename can fail if an external process opened the target or loaded it as an image.
            output.pins.clear();
            if (target.present) target.pins.pop_back();
            if (!MoveFileExW(temporary_path.c_str(), extended(parent.path / filename).c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                const auto error = GetLastError();
                if (sharing_error(error) || (target.present && error == ERROR_ACCESS_DENIED)) return "in-use";
                return "The verified replacement file could not be published: " + filename.string();
            }
            cleanup.cancel();
            fixture_cut(bundle, "runtime-repair-after-file-" + sequence);
            return {};
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
        if (verify_play_runtime(active, launcher_version, verification))
        {
            runtime_check status{true, false, false, {}};
            const auto source = read_runtime(bundle);
            if (newer_bundle(active, source) && separate_roots(bundle, install) && frozen_source(source, launcher_version) &&
                server_state_error(install, source.manifests[index(id::server)]->version).empty())
                status.update_version = source.root.filename().string();
            const auto previous = previous_runtime(install, active.root.filename().string());
            if (!previous.empty())
            {
                const auto retained = read_runtime(install, previous);
                if (complete_layout(retained))
                {
                    status.rollback_blocked = !server_state_error(install, retained.manifests[index(id::server)]->version).empty();
                    if (!status.rollback_blocked && frozen_source(retained, launcher_version) && exact_runtime_files(retained))
                        status.rollback_version = previous;
                }
            }
            return status;
        }
        const auto source = read_runtime(bundle);
        if (!separate_roots(bundle, install) || !frozen_source(source, launcher_version)) return {false, false, false, verification};
        const auto server_version = source.manifests[index(id::server)]->version;
        if (const auto error = server_state_error(install, server_version); !error.empty()) return {false, false, false, error};
        const auto release = source.root.filename().string();
        if (marker_missing(install))
        {
            if (inspect_fresh(bundle, install, release, launcher_version).state != fresh_state::invalid)
                return {false, true, false, {}};
            return {false, false, false, "The bundled runtime needs a fresh install folder."};
        }
        const auto error = repair_error(install, source, active, release, launcher_version);
        return {false, false, error.empty() || error == "in-use", verification};
    }

    bool stage_bundled_runtime(const fs::path& bundle, const fs::path& install, std::string_view launcher_version)
    {
        std::string verification;
        if (verify_play_runtime(read_runtime(install), launcher_version, verification)) return true;
        if (!marker_missing(install) || !separate_roots(bundle, install)) return false;
        const auto source = read_runtime(bundle);
        if (!frozen_source(source, launcher_version)) return false;
        const auto server_version = source.manifests[index(id::server)]->version;
        if (!server_state_error(install, server_version).empty()) return false;
        const auto release = source.root.filename().string();
        if (inspect_fresh(bundle, install, release, launcher_version).state == fresh_state::invalid) return false;

        const runtime_lease lease(install, true);
        if (verify_play_runtime(read_runtime(install), launcher_version, verification)) return true;
        if (!lease) return false;
        if (!server_state_error(install, server_version).empty()) return false;
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
                !remember_server_state_version(install, server_version) ||
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

    std::string change_runtime(const fs::path& bundle, const fs::path& install, std::string_view launcher_version,
                               runtime_change action, std::string_view expected)
    {
        // An activation never stages into its source or claims a folder without an active slot.
        if (!separate_roots(bundle, install) || read_runtime(install).root.empty())
            return "Choose an existing game install.";
        const runtime_lease lease(install, true);
        if (!lease) return lease.busy() ? "in-use" : "Could not lock the game runtime.";
        const show23::activity_lock activity;
        if (!activity) return "in-use";
        const auto active = read_runtime(install);
        if (!complete_layout(active)) return "The active runtime could not be identified.";
        if (runtime_process_running(active.root)) return "in-use";

        const auto current = active.root.filename().string();
        const auto updating = action == runtime_change::update;
        const auto previous = updating ? std::string{} : previous_runtime(install, current);
        if (!updating && previous.empty()) return "There is no previous runtime to restore.";
        const auto target = updating ? read_runtime(bundle) : read_runtime(install, previous);
        if (updating && !newer_bundle(active, target)) return "No newer bundled runtime is available.";
        const auto selection = target.root.filename().string();
        if (!expected.empty() && selection != expected) return "The offered runtime changed. Check again.";
        if (!frozen_source(target, launcher_version)) return "The selected runtime failed verification or is incompatible.";
        if (const auto error = server_state_error(install, target.manifests[index(id::server)]->version); !error.empty())
            return error;
        if (runtime_process_running(install / "runtime" / selection)) return "in-use";

        if (updating)
        {
            if (const auto error = publish_inactive_slot(bundle, install, target, selection, launcher_version); !error.empty())
                return error;
            const auto history = ensure_directory(history_folder(install));
            if (!atomically_write(history, selection + ".txt", current + '\n'))
                return "Could not record the previous runtime.";
            fixture_cut(bundle, "runtime-update-after-history");
        }
        else if (!exact_runtime_files(target)) return "The previous runtime contains unverified files.";

        const auto manifests = inspect(install / "manifests");
        if (!atomically_write(manifests, "runtime.txt", selection + '\n')) return "Could not select the new runtime.";
        if (updating)
        {
            fixture_cut(bundle, "runtime-update-after-selection");
            clean_fresh_stages(install / "runtime", selection);
        }
        return {};
    }

    std::string repair_bundled_runtime(const fs::path& bundle, const fs::path& install, std::string_view launcher_version)
    {
        if (!separate_roots(bundle, install)) return "The runtime install and bundle paths are invalid.";
        if (runtime_in_use(install)) return "in-use";
        const auto source = read_runtime(bundle);
        if (!frozen_source(source, launcher_version)) return "The bundled runtime failed verification.";
        const auto server_version = source.manifests[index(id::server)]->version;
        if (const auto error = server_state_error(install, server_version); !error.empty()) return error;
        const auto release = source.root.filename().string();
        // Validate eligibility without creating a lock file on an unrecognized install.
        if (const auto error = repair_error(install, source, read_runtime(install), release, launcher_version); !error.empty())
            return error;
        const runtime_lease lease(install, true);
        if (!lease) return lease.busy() ? "in-use" : "The runtime repair lease could not be opened.";
        if (const auto error = server_state_error(install, server_version); !error.empty()) return error;
        const auto active = read_runtime(install);
        std::string verification;
        if (active.root.filename() == fs::path(release) && versions_match(active, source) &&
            verify_play_runtime(active, launcher_version, verification)) return {};
        if (const auto error = repair_error(install, source, active, release, launcher_version); !error.empty()) return error;
        if (runtime_process_running(active.root)) return "in-use";

        unsigned replacement = 0;
        for (const auto part : runtime_parts)
        {
            const auto& expected = *source.manifests[index(part)];
            const auto from = component_folder(source, part);
            const auto to = component_folder(active, part);
            for (const auto& file : expected.files)
                if (const auto error = repair_file(bundle, from / file.path, to / file.path, file.size, file.sha256, replacement);
                    !error.empty()) return error;
        }

        const slot_paths live(install, release);
        const slot_paths expected(bundle, release);
        for (const auto part : runtime_parts)
        {
            const auto& known = *source.manifests[index(part)];
            const auto& installed = active.manifests[index(part)];
            if (installed && same_manifest(*installed, known)) continue;
            const auto filename = std::string(name(part)) + ".json";
            const auto from = expected.manifests / filename;
            const auto bytes = manifest_bytes(from);
            std::string parse_error;
            const auto parsed = bytes ? parse(*bytes, parse_error) : std::nullopt;
            if (!parsed || !same_manifest(*parsed, known)) return "A bundled manifest changed during repair.";
            utils::cryptography::sha256::stream manifest_hash;
            manifest_hash.update(bytes->data(), bytes->size());
            const auto digest = manifest_hash.finish_hex();
            if (const auto error = repair_file(bundle, from, live.manifests / filename, bytes->size(), digest, replacement);
                !error.empty()) return error;
        }
        return verify_play_runtime(read_runtime(install), launcher_version, verification) ? std::string{} : verification;
    }
}
