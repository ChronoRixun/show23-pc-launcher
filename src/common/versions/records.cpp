#include "records.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <map>
#include <set>

namespace versions
{
    namespace
    {
        bool digits(std::string_view s)
        {
            return !s.empty() && s.find_first_not_of("0123456789") == s.npos;
        }

        template<class T> bool number(std::string_view s, T& out)
        {
            if (!digits(s) || (s.size() > 1 && s.front() == '0')) return false;
            const auto result = std::from_chars(s.data(), s.data() + s.size(), out);
            return result.ec == std::errc{} && result.ptr == s.data() + s.size();
        }

        bool hex(std::string_view s, std::size_t length)
        {
            return s.size() == length && s.find_first_not_of("0123456789abcdef") == s.npos;
        }

        std::vector<std::string_view> split(std::string_view text, char separator)
        {
            std::vector<std::string_view> result;
            for (;;)
            {
                const auto end = text.find(separator);
                result.push_back(text.substr(0, end));
                if (end == text.npos) return result;
                text.remove_prefix(end + 1);
            }
        }

        struct record_text
        {
            std::vector<std::string_view> lines;
            std::string_view fingerprint;
        };

        std::optional<record_text> parse_text(std::string_view bytes, std::string_view header, std::size_t limit)
        {
            if (bytes.empty() || bytes.size() > limit || bytes.find('\0') != bytes.npos) return {};
            if (bytes.ends_with('\n')) bytes.remove_suffix(1);
            const auto last = bytes.rfind('\n');
            if (last == bytes.npos) return {};
            auto tail = bytes.substr(last + 1);
            if (tail.ends_with('\r')) tail.remove_suffix(1);
            if (!tail.starts_with("end ") || !hex(tail.substr(4), 64)) return {};
            const auto fingerprint = tail.substr(4);
            if (sha256(bytes.substr(0, last + 1)) != fingerprint) return {};
            auto lines = split(bytes.substr(0, last), '\n');
            for (auto& line : lines)
                if (line.ends_with('\r')) line.remove_suffix(1);
            if (lines.front() != header) return {};
            return record_text{std::move(lines), fingerprint};
        }

        std::optional<std::string_view> field(std::string_view line, std::string_view key)
        {
            if (!line.starts_with(key) || line.size() <= key.size() + 1 || line[key.size()] != ' ') return {};
            return line.substr(key.size() + 1);
        }

        std::string seal(std::string content)
        {
            return content + "end " + sha256(content) + '\n';
        }

        std::string lowercase(std::string_view s)
        {
            std::string value(s);
            for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            return value;
        }

        std::string display(std::string_view value)
        {
            return value.empty() ? "-" : std::string(value);
        }
    }

    std::string_view folder_version(const std::string_view name)
    {
        return name.substr(0, name.find('~'));
    }

    bool valid_version(const std::string_view text)
    {
        if (text.empty() || text.size() > 48) return false;
        const auto dash = text.find('-');
        const auto core = split(text.substr(0, dash), '.');
        if (core.size() != 3) return false;
        for (const auto part : core)
        {
            unsigned value{};
            if (part.size() > 9 || !number(part, value)) return false;
        }
        if (dash == text.npos) return true;
        for (const auto part : split(text.substr(dash + 1), '.'))
            if (part.empty() || part.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyz-") != part.npos ||
                (digits(part) && part.size() > 1 && part.front() == '0')) return false;
        return true;
    }

    bool valid_folder(const std::string_view name)
    {
        if (!valid_version(folder_version(name))) return false;
        const auto suffix = name.find('~');
        unsigned copy{};
        return suffix == name.npos || (number(name.substr(suffix + 1), copy) && copy >= 2);
    }

    int compare(const std::string_view left, const std::string_view right)
    {
        const auto a = folder_version(left), b = folder_version(right);
        if (a == b) return 0;
        const auto a_dash = a.find('-'), b_dash = b.find('-');
        const auto a_core = split(a.substr(0, a_dash), '.'), b_core = split(b.substr(0, b_dash), '.');
        const auto cmp_num = [](std::string_view first, std::string_view second)
        {
            if (first.size() != second.size()) return first.size() < second.size() ? -1 : 1;
            return first == second ? 0 : first < second ? -1 : 1;
        };
        for (std::size_t i = 0; i < std::min(a_core.size(), b_core.size()); ++i)
            if (const auto order = cmp_num(a_core[i], b_core[i])) return order;
        if (a_core.size() != b_core.size()) return a_core.size() < b_core.size() ? -1 : 1;
        if (a_dash == a.npos || b_dash == b.npos) return a_dash == b_dash ? 0 : a_dash == a.npos ? 1 : -1;
        const auto a_pre = split(a.substr(a_dash + 1), '.'), b_pre = split(b.substr(b_dash + 1), '.');
        for (std::size_t i = 0; i < std::min(a_pre.size(), b_pre.size()); ++i)
        {
            const auto x = a_pre[i], y = b_pre[i];
            if (x == y) continue;
            if (digits(x) && digits(y)) return cmp_num(x, y);
            if (digits(x) != digits(y)) return digits(x) ? -1 : 1;
            return x < y ? -1 : 1;
        }
        return a_pre.size() == b_pre.size() ? 0 : a_pre.size() < b_pre.size() ? -1 : 1;
    }

    bool safe_relative(const std::string_view path)
    {
        if (path.empty() || path.size() > 240) return false;
        for (const auto component : split(path, '/'))
        {
            if (component.empty() || component == "." || component == ".." ||
                component.front() == ' ' || component.back() == ' ' || component.back() == '.' ||
                component.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_+- .") != component.npos)
                return false;
            auto base = lowercase(component.substr(0, component.find('.')));
            while (!base.empty() && base.back() == ' ') base.pop_back();
            if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
                (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && digits(base.substr(3))))
                return false;
        }
        return true;
    }

    std::optional<inventory> parse_inventory(const std::string_view bytes)
    {
        const auto input = parse_text(bytes, "show23-pc version 1", 2 * 1024 * 1024);
        if (!input || input->lines.size() < 5 || input->lines.size() > 8196) return {};
        const auto version = field(input->lines[1], "version");
        const auto channel = field(input->lines[2], "channel");
        const auto boot = field(input->lines[3], "bootstrap");
        inventory value;
        if (!version || !valid_version(*version) || !channel ||
            *channel != (version->find('-') == version->npos ? "stable" : "beta") ||
            !boot || !number(*boot, value.bootstrap) || !value.bootstrap) return {};
        value.version = *version;
        value.channel = *channel;
        value.fingerprint = input->fingerprint;
        std::set<std::string> paths;
        for (std::size_t i = 4; i < input->lines.size(); ++i)
        {
            const auto row = field(input->lines[i], "file");
            if (!row || row->size() < 68 || (*row)[64] != ' ' || !hex(row->substr(0, 64), 64)) return {};
            const auto separator = row->find(' ', 65);
            if (separator == row->npos) return {};
            const auto path = row->substr(separator + 1);
            const auto key = lowercase(path);
            file entry;
            if (!safe_relative(path) || !number(row->substr(65, separator - 65), entry.size) ||
                key == "version.txt" || key == ".writing" || !paths.insert(key).second) return {};
            entry.path = path;
            entry.sha256 = row->substr(0, 64);
            value.files.push_back(std::move(entry));
        }
        if (!paths.contains(launcher_exe)) return {};
        for (const auto& path : paths)
            for (auto at = path.find('/'); at != path.npos; at = path.find('/', at + 1))
                if (paths.contains(path.substr(0, at))) return {};
        return value;
    }

    std::optional<pointer> parse_pointer(const std::string_view bytes)
    {
        const auto input = parse_text(bytes, "show23-pc pointer 1", 8192);
        if (!input || input->lines.size() < 6 || input->lines.size() > 14) return {};
        pointer value;
        const auto name = [&](std::size_t index, std::string_view key, std::string& out)
        {
            const auto raw = field(input->lines[index], key);
            if (!raw || (*raw != "-" && !valid_folder(*raw))) return false;
            if (*raw != "-") out = *raw;
            return true;
        };
        const auto starts = field(input->lines[4], "starts");
        const auto id = field(input->lines[5], "last-request");
        if (!name(1, "current", value.current) || !name(2, "previous", value.previous) || !name(3, "trial", value.trial) ||
            !starts || !number(*starts, value.starts) || (value.trial.empty() && value.starts) ||
            (!value.previous.empty() && value.previous == value.current) ||
            (!value.trial.empty() && value.trial == value.current) ||
            !id || (*id != "-" && !hex(*id, 16))) return {};
        if (*id != "-") value.last_request = *id;
        std::set<std::string> failed;
        for (std::size_t i = 6; i < input->lines.size(); ++i)
        {
            const auto row = field(input->lines[i], "failed");
            if (!row) return {};
            const auto first = row->find(' '), second = first == row->npos ? row->npos : row->find(' ', first + 1);
            if (second == row->npos) return {};
            const auto folder = row->substr(0, first), hash = row->substr(first + 1, second - first - 1),
                reason = row->substr(second + 1);
            if (!valid_folder(folder) || !hex(hash, 64) || reason.empty() || reason.size() > 256 ||
                std::ranges::any_of(reason, [](unsigned char c) { return c < 32 || c > 126; }) ||
                !failed.insert(std::string(folder)).second) return {};
            value.failed.push_back({std::string(folder), std::string(hash), std::string(reason)});
        }
        return value;
    }

    std::optional<request> parse_request(const std::string_view bytes)
    {
        const auto input = parse_text(bytes, "show23-pc request 1", 8192);
        if (!input || input->lines.size() != 4) return {};
        const auto id = field(input->lines[1], "id");
        const auto trial = field(input->lines[2], "trial");
        const auto reason = field(input->lines[3], "reason");
        if (!id || !hex(*id, 16) || !trial || !valid_folder(*trial) ||
            !reason || (*reason != "switch-to-stable" && *reason != "retry")) return {};
        return request{std::string(*id), std::string(*trial), std::string(*reason)};
    }

    std::string encode(const inventory& value)
    {
        auto body = "show23-pc version 1\nversion " + value.version + "\nchannel " + value.channel +
            "\nbootstrap " + std::to_string(value.bootstrap) + '\n';
        for (const auto& entry : value.files)
            body += "file " + entry.sha256 + ' ' + std::to_string(entry.size) + ' ' + entry.path + '\n';
        return seal(std::move(body));
    }

    std::string encode(const pointer& value)
    {
        auto body = "show23-pc pointer 1\ncurrent " + display(value.current) +
            "\nprevious " + display(value.previous) + "\ntrial " + display(value.trial) +
            "\nstarts " + std::to_string(value.starts) + "\nlast-request " + display(value.last_request) + '\n';
        for (const auto& entry : value.failed)
            body += "failed " + entry.folder + ' ' + entry.fingerprint + ' ' + entry.reason + '\n';
        return seal(std::move(body));
    }

    std::string encode(const request& value)
    {
        return seal("show23-pc request 1\nid " + value.id + "\ntrial " + value.trial + "\nreason " + value.reason + '\n');
    }

    unsigned parse_notices(std::string_view text)
    {
        constexpr std::string_view header = "show23-pc notice 1\n";
        if (text.size() > 256 || !text.starts_with(header) || !text.ends_with('\n')) return 0;
        text.remove_prefix(header.size());
        unsigned result{};
        while (!text.empty())
        {
            const auto end = text.find('\n');
            const auto id = std::ranges::find(notice_ids, text.substr(0, end));
            if (id == std::end(notice_ids)) return 0;
            const auto bit = 1u << (id - std::begin(notice_ids));
            if (result & bit) return 0;
            result |= bit;
            text.remove_prefix(end + 1);
        }
        return result;
    }

    std::string encode_notices(unsigned mask)
    {
        std::string text = "show23-pc notice 1\n";
        for (std::size_t i = 0; i < std::size(notice_ids); ++i)
            if (mask & (1u << i))
            {
                text += notice_ids[i];
                text += '\n';
            }
        return text;
    }

    std::optional<update_channel> read_channel(const fs::path& root)
    {
        const auto value = read_record(root / "channel.txt", 64);
        if (value == "show23-pc channel 1\nstable\n") return update_channel::stable;
        if (value == "show23-pc channel 1\nbeta\n") return update_channel::beta;
        return {};
    }

    bool write_channel(const fs::path& root, update_channel value)
    {
        return read_channel(root) == value ||
            write_record(root / "channel.txt", value == update_channel::stable ?
                "show23-pc channel 1\nstable\n" : "show23-pc channel 1\nbeta\n");
    }

    fs::path detail::confirmation_path(const fs::path& root, std::string_view folder)
    {
        return root / "confirmed" / (std::string(folder) + ".txt");
    }

    std::optional<detail::confirmation> detail::parse_confirmation(std::string_view data, std::string_view folder)
    {
        const bool legacy = data.starts_with("show23-pc confirmed 1");
        const auto input = legacy ? parse_text(data, "show23-pc confirmed 1", 512) :
            parse_text(data, "show23-pc confirmed 2", confirmation_limit);
        if (!input || (legacy ? input->lines.size() != 3 : input->lines.size() < 4 || input->lines.size() > 8196) ||
            field(input->lines[1], "folder") != folder) return {};
        const auto fingerprint = field(input->lines[2], "inventory");
        if (!fingerprint || !hex(*fingerprint, 64)) return {};
        confirmation value{std::string(*fingerprint), {}};
        if (legacy) return value;
        const auto time = field(input->lines[3], "confirmed-at");
        if (!time || !number(*time, value.confirmed_at) || !value.confirmed_at) return {};
        value.files.reserve(input->lines.size() - 4);
        for (std::size_t i = 4; i < input->lines.size(); ++i)
        {
            const auto row = field(input->lines[i], "file");
            if (!row) return {};
            auto path = *row;
            file_stamp stamp;
            for (const auto member : std::array{&stamp.size, &stamp.last_write, &stamp.changed, &stamp.volume, &stamp.index})
            {
                const auto space = path.find(' ');
                if (space == path.npos || !number(path.substr(0, space), *member)) return {};
                path.remove_prefix(space + 1);
            }
            if (!stamp.index || !safe_relative(path)) return {};
            stamp.path = path;
            value.files.push_back(std::move(stamp));
        }
        return value;
    }

    bool detail::persist_confirmation(const fs::path& root, std::string_view folder, const confirmation& value)
    {
        if (!valid_folder(folder) || !hex(value.fingerprint, 64)) return false;
        const auto record = confirmation_path(root, folder);
        const auto base = record.parent_path();
        if (!plain_path(base, true)) return false;
        std::error_code error;
        fs::create_directory(base, error);
        if (error || !fs::is_directory(base, error) || error) return false;
        auto stale = record;
        stale += ".tmp";
        if (plain_path(stale) && fs::is_regular_file(stale, error) && !error) fs::remove(stale, error);
        if (error) return false;
        std::string data = value.confirmed_at ? "show23-pc confirmed 2\nfolder " : "show23-pc confirmed 1\nfolder ";
        data += std::string(folder) + "\ninventory " + value.fingerprint + '\n';
        if (value.confirmed_at) data += "confirmed-at " + std::to_string(value.confirmed_at) + '\n';
        for (const auto& entry : value.files)
            data += "file " + std::to_string(entry.size) + ' ' + std::to_string(entry.last_write) + ' ' +
                std::to_string(entry.changed) + ' ' + std::to_string(entry.volume) + ' ' + std::to_string(entry.index) +
                ' ' + entry.path + '\n';
        const auto checksum = sha256(data);
        if (checksum.empty()) return false;
        data += "end " + checksum + '\n';
        return data.size() <= confirmation_limit && write_record(record, data);
    }

    std::string read_confirmation(const fs::path& root, std::string_view folder)
    {
        if (!valid_folder(folder)) return {};
        const auto record = detail::parse_confirmation(
            read_record(detail::confirmation_path(root, folder), detail::confirmation_limit), folder);
        return record ? record->fingerprint : std::string{};
    }

    bool write_confirmation(const fs::path& root, std::string_view folder, std::string_view fingerprint)
    {
        if (!valid_folder(folder) || !hex(fingerprint, 64)) return false;
        return read_confirmation(root, folder) == fingerprint ||
            detail::persist_confirmation(root, folder, {std::string(fingerprint), {}});
    }

    loaded_pointer select_pointer(const std::optional<pointer>& primary, const std::optional<pointer>& backup)
    {
        if (primary) return {*primary, pointer_source::primary, false, !backup};
        if (backup) return {*backup, pointer_source::backup, true, false};
        return {};
    }
}
