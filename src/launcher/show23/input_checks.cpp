#include "std_include.hpp"
#include "input_checks.hpp"
#include "input_paths.hpp"
#include "package_header.hpp"
#include "sfo.hpp"

#include <utils/properties.hpp>

#include <algorithm>
#include <limits>

namespace show23
{
    namespace
    {
        using enum finding_state;
        using input_paths::path_error;
        constexpr std::string_view supported_content = "UP9000-CUSA35521_00-MLBTHESHOW23SHIP";
        constexpr std::uint64_t staging_allowance = 2ull * 1024 * 1024 * 1024;
        constexpr auto max_bytes = (std::numeric_limits<std::uint64_t>::max)();

        void add(checked_inputs& report, std::string id, const finding_state state, finding_values values = {})
        {
            report.findings.push_back({std::move(id), state, std::move(values)});
        }

        const char* path_reason(const path_error error)
        {
            switch (error)
            {
            case path_error::missing: return "missing";
            case path_error::reparse: return "link";
            case path_error::not_directory: return "directory";
            default: return "unreadable";
            }
        }

        std::optional<std::uint64_t> expanded(const std::uint64_t image_size)
        {
            // ceil(image_size * 7 / 5), with room left for the staging allowance.
            const auto whole = image_size / 5;
            const auto remainder = (image_size % 5 * 7 + 4) / 5;
            if (whole > (max_bytes - staging_allowance - remainder) / 7) return std::nullopt;
            return whole * 7 + remainder + staging_allowance;
        }

        struct source
        {
            fs::path path;
            bool directory{};
            std::optional<sfo_metadata> metadata;
            std::optional<std::uint64_t> needed;
        };

        std::optional<sfo_metadata> folder_metadata(checked_inputs& report, const std::string& prefix, const fs::path& folder)
        {
            const auto entry = input_paths::inspect(folder / L"sce_sys" / L"param.sfo", false, true);
            if (entry.error != path_error::none)
            {
                add(report, prefix + ".metadata", failed, {{"reason", std::string(path_reason(entry.error))}});
                return std::nullopt;
            }
            if (entry.directory || entry.length == 0 || entry.length > sfo_limit)
            {
                add(report, prefix + ".metadata", failed, {{"reason", std::string("invalid")}});
                return std::nullopt;
            }
            std::string bytes(static_cast<std::size_t>(entry.length), '\0');
            auto parsed = input_paths::read_at(entry.pins.back().get(), 0, bytes) ? read_sfo(bytes) : std::nullopt;
            add(report, prefix + ".metadata", parsed ? passed : failed, {{"reason", std::string(parsed ? "" : "invalid")}});
            return parsed;
        }

        void package_findings(checked_inputs& report, const std::string& prefix, const input_paths::location& input, source& found)
        {
            const auto package = read_package(input.length, [&input](const std::uint64_t at, std::span<char> out)
            {
                return input_paths::read_at(input.pins.back().get(), at, out);
            });
            using enum package_error;
            switch (package.error)
            {
            case none:
                add(report, prefix + ".complete", passed);
                break;
            case truncated:
                add(report, prefix + ".complete", failed, {{"reason", std::string("truncated")}});
                return;
            case magic:
                add(report, prefix + ".complete", failed, {{"reason", std::string("magic")}});
                return;
            case metadata_missing:
            case metadata_encrypted:
            case metadata_size:
                add(report, prefix + ".metadata", failed, {{"reason", std::string(
                    package.error == metadata_missing ? "missing" : package.error == metadata_encrypted ? "encrypted" : "invalid")}});
                return;
            default:
                add(report, prefix + ".complete", failed, {{"reason", std::string(package.error == read ? "unreadable" : "layout")}});
                return;
            }

            add(report, prefix + ".type", package.header.content_type == 0x1a ? passed : failed);
            found.metadata = read_sfo(package.metadata);
            add(report, prefix + ".metadata", found.metadata ? passed : failed, {{"reason", std::string(found.metadata ? "" : "invalid")}});
            const auto header_id_ok = package.header.content_id == supported_content;
            const auto sfo_id_ok = !found.metadata || found.metadata->content_id.empty() || found.metadata->content_id == package.header.content_id;
            add(report, prefix + ".content", header_id_ok && sfo_id_ok ? passed : failed);
            if (found.metadata) found.metadata->content_id = package.header.content_id;
            found.needed = expanded(package.header.image_size);
        }

        void check_executable(checked_inputs& report, const std::string& prefix, const fs::path& folder,
                              const executable_reference& expected)
        {
            const auto binary = input_paths::inspect(folder / L"eboot.bin", false, true);
            if (binary.error != path_error::none || binary.directory)
            {
                add(report, prefix + ".executable", failed, {{"reason", std::string(binary.directory ? "missing" : path_reason(binary.error))}});
                return;
            }
            if (binary.length != expected.size)
            {
                add(report, prefix + ".executable", failed, {{"reason", std::string("digest")}});
                return;
            }
            const auto digest = input_paths::sha256(binary);
            const auto known = digest && *digest == expected.sha256;
            add(report, prefix + ".executable", known ? passed : failed,
                {{"reason", std::string(known ? "" : digest ? "digest" : "unreadable")}});
        }

        source check_source(checked_inputs& report, const fs::path& path, const source_kind kind,
                            const bool is_update, const executable_reference& expected)
        {
            const std::string prefix = is_update ? "update" : "base";
            source found;
            const auto input = input_paths::inspect(path, false, true);
            if (input.error != path_error::none)
            {
                add(report, prefix + ".kind", failed, {{"reason", std::string(path_reason(input.error))}});
                return found;
            }
            found.path = input.path;
            found.directory = input.directory;
            if ((kind == source_kind::folder && !input.directory) || (kind == source_kind::package && input.directory))
            {
                add(report, prefix + ".kind", failed, {{"reason", std::string(kind == source_kind::folder ? "expectedFolder" : "expectedFile")}});
                return found;
            }
            add(report, prefix + ".kind", passed, {{"kind", std::string(input.directory ? "folder" : "package")}});
            if (input.directory)
            {
                found.metadata = folder_metadata(report, prefix, input.path);
                const auto size = input_paths::folder_size(input.path);
                if (size) found.needed = staging_allowance;
                add(report, prefix + ".complete", found.needed ? passed : failed,
                    {{"kind", std::string("folder")}, {"reason", std::string(found.needed ? "" : "folder")}});
                if (found.metadata)
                    add(report, prefix + ".content", found.metadata->content_id == supported_content ? passed : failed);
            }
            else
            {
                package_findings(report, prefix, input, found);
            }
            if (!found.metadata) return found;

            const auto& metadata = *found.metadata;
            const bool title = metadata.title == title_id;
            const bool category = metadata.category == (is_update ? "gp" : "gd");
            const bool version = metadata.version == (is_update ? "01.21" : "01.00");
            add(report, prefix + ".title", title ? passed : failed);
            add(report, prefix + ".category", category ? passed : failed);
            const auto display = version ? metadata.version.substr(1) : std::string(is_update ? "1.21" : "1.00");
            add(report, prefix + ".version", version ? passed : failed, {{"version", display}});
            if (version)
            {
                auto& destination = is_update ? report.update_version : report.base_version;
                destination = display;
            }
            if (title && category && version)
            {
                if (input.directory) check_executable(report, prefix, input.path, expected);
                else add(report, prefix + ".executable", later);
            }
            return found;
        }

        void check_destination(checked_inputs& report, const source& base, const source& update, const input_check_options& options)
        {
            const auto& path = report.request.install;
            if (!input_paths::fixed_drive(path) || path == path.root_path())
            {
                add(report, "install.local", failed);
                return;
            }
            add(report, "install.local", passed);
            const auto destination = input_paths::inspect(path, true);
            if (destination.error != path_error::none || (destination.present && !destination.directory))
            {
                const auto reason = destination.present && !destination.directory ? "directory" : path_reason(destination.error);
                add(report, "install.writable", failed, {{"reason", std::string(reason)}});
                return;
            }
            for (const auto& protected_path : options.protected_destinations)
            {
                const auto canonical = input_paths::inspect(protected_path, true);
                const auto& protected_root = canonical.error == path_error::none ? canonical.path : protected_path;
                if (input_paths::contains(protected_root, destination.path))
                {
                    add(report, "install.overlap", failed, {{"reason", std::string("settings")}});
                    return;
                }
            }
            const auto overlaps = [&destination](const source& src)
            {
                return !src.path.empty() && (input_paths::contains(destination.path, src.path) ||
                    (src.directory && input_paths::contains(src.path, destination.path)));
            };
            if (overlaps(base) || overlaps(update))
            {
                add(report, "install.overlap", failed);
                return;
            }
            if (base.path.empty() || (report.has_update && update.path.empty()))
            {
                add(report, "install.writable", later);
                return;
            }
            add(report, "install.overlap", passed);

            const auto write = options.writable ? options.writable(destination.existing, !destination.present) :
                input_paths::probe(destination.existing, !destination.present);
            add(report, "install.writable", write ? passed : failed, {{"reason", std::string(write ? "" : "unwritable")}});

            report.available_bytes = options.free_space ? options.free_space(destination.existing) :
                input_paths::free_space(destination.existing);
            if (!report.needed_bytes)
            {
                if (base.metadata && (!report.has_update || update.metadata))
                    add(report, "install.space", failed, {{"reason", std::string("size")}});
                return;
            }
            finding_values values{{"needed", *report.needed_bytes},
                {"drive", destination.path.root_name().string()},
                {"kind", std::string(base.directory ? "in_place" : "package")}};
            if (report.available_bytes) values.emplace("available", *report.available_bytes);
            else values.emplace("reason", std::string("unreadable"));
            const auto sufficient = report.available_bytes && *report.available_bytes >= *report.needed_bytes;
            add(report, "install.space", sufficient ? passed : failed, std::move(values));
        }
    }

    const char* state_name(const finding_state value)
    {
        switch (value)
        {
        case passed: return "passed";
        case notice: return "notice";
        case later: return "later";
        default: return "failed";
        }
    }

    bool checked_inputs::passed() const
    {
        return !findings.empty() &&
            std::ranges::none_of(findings, [](const auto& item) { return item.state == finding_state::failed; });
    }

    input_check_options default_input_check_options()
    {
        input_check_options options;
        options.protected_destinations = {utils::properties::get_local_root(), utils::properties::get_portable_root()};
        return options;
    }

    checked_inputs check_inputs(const setup_request& request, const input_check_options& options)
    {
        checked_inputs answer;
        answer.request = request;
        answer.has_update = request.update.has_value();
        const auto base = check_source(answer, request.base, request.kind, false, options.base);
        source update;
        if (request.update)
        {
            const auto start = answer.findings.size();
            update = check_source(answer, *request.update, source_kind::automatic, true, options.update);
            if (base.metadata && update.metadata)
            {
                const auto same = base.metadata->title == update.metadata->title &&
                    base.metadata->content_id == update.metadata->content_id;
                add(answer, "update.matches", same ? passed : failed);
                if (std::none_of(answer.findings.begin() + static_cast<std::ptrdiff_t>(start), answer.findings.end(),
                    [](const auto& item) { return item.state == failed; }))
                    add(answer, "update.offline", notice);
            }
        }
        const auto extra = answer.has_update ? update.needed : std::optional<std::uint64_t>(0);
        if (base.needed && extra)
        {
            if (base.directory) answer.needed_bytes = staging_allowance;
            else if (*extra <= max_bytes - *base.needed) answer.needed_bytes = *base.needed + *extra;
        }
        answer.request.kind = base.path.empty() ? request.kind : base.directory ? source_kind::folder : source_kind::package;
        if (!base.path.empty()) answer.request.base = base.path;
        if (!update.path.empty()) answer.request.update = update.path;
        answer.update_is_package = answer.has_update && !update.directory;
        check_destination(answer, base, update, options);
        return answer;
    }
}
