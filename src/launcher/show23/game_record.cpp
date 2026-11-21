#include "std_include.hpp"
#include "game_record.hpp"
#include "input_paths.hpp"

#include <utils/string.hpp>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>

namespace show23
{
    namespace
    {
        using rapidjson::Value;
        using writer = rapidjson::Writer<rapidjson::StringBuffer>;

        void text(writer& out, const std::string& value)
        {
            out.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
        }

        void write_folder(writer& out, const game_folder& folder)
        {
            out.StartObject();
            out.Key("path"); text(out, utils::string::path_to_utf8(folder.path));
            out.Key("version"); text(out, folder.version);
            out.Key("executable"); out.StartObject();
            out.Key("sha256"); text(out, folder.executable_sha256);
            out.Key("size"); out.Uint64(folder.executable_size);
            out.EndObject();
            out.Key("directory_bytes"); out.StartObject();
            for (std::size_t i = 0; i < directory_names.size(); ++i)
            {
                out.Key(directory_names[i]); out.Uint64(folder.directories[i]);
            }
            out.EndObject();
            out.EndObject();
        }

        const Value& field(const Value& object, const char* name)
        {
            static const Value absent;
            if (!object.IsObject()) return absent;
            const auto found = object.FindMember(name);
            if (found == object.MemberEnd()) return absent;
            for (auto it = std::next(found); it != object.MemberEnd(); ++it)
                if (std::string_view(it->name.GetString(), it->name.GetStringLength()) == name) return absent;
            return found->value;
        }

        std::string_view string(const Value& value)
        {
            return value.IsString() ? std::string_view(value.GetString(), value.GetStringLength()) : std::string_view{};
        }

        fs::path read_path(const Value& value)
        {
            const auto raw = string(value);
            if (raw.empty() || raw.find('\0') != raw.npos) return {};
            const auto path = utils::string::utf8_to_path(std::string(raw));
            return is_local_path(path) ? path : fs::path{};
        }

        std::optional<game_folder> read_folder(const Value& value, const bool update)
        {
            if (!value.IsObject() || value.MemberCount() != 4) return std::nullopt;
            const auto path = read_path(field(value, "path"));
            const auto version = string(field(value, "version"));
            if (path.empty() || version != (update ? "1.21" : "1.00")) return std::nullopt;
            const auto& executable = field(value, "executable");
            if (!executable.IsObject() || executable.MemberCount() != 2) return std::nullopt;
            const auto digest = string(field(executable, "sha256"));
            const auto& size = field(executable, "size");
            if (!size.IsUint64() || !size.GetUint64() || digest.size() != 64 ||
                digest.find_first_not_of("0123456789abcdef") != digest.npos) return std::nullopt;
            const auto& totals = field(value, "directory_bytes");
            if (!totals.IsObject() || totals.MemberCount() != directory_names.size()) return std::nullopt;
            game_folder folder{path, std::string(version), size.GetUint64(), std::string(digest)};
            for (std::size_t i = 0; i < directory_names.size(); ++i)
            {
                const auto& total = field(totals, directory_names[i]);
                if (!total.IsUint64()) return std::nullopt;
                folder.directories[i] = total.GetUint64();
            }
            return folder;
        }

        bool utc(const std::string_view value)
        {
            if (value.size() != 20) return false;
            constexpr std::string_view format = "0000-00-00T00:00:00Z";
            for (std::size_t i = 0; i < format.size(); ++i)
            {
                if (format[i] == '0' ? value[i] < '0' || value[i] > '9' : value[i] != format[i]) return false;
            }
            return true;
        }

        std::optional<std::string> read_small(const fs::path& path, const std::size_t limit)
        {
            const auto file = input_paths::inspect(path, false, true);
            if (file.error != input_paths::path_error::none || file.directory || file.length > limit) return std::nullopt;
            std::string bytes(static_cast<std::size_t>(file.length), '\0');
            if (!input_paths::read_at(file.pins.back().get(), 0, bytes)) return std::nullopt;
            return bytes;
        }

        bool executable_present(const game_folder& game)
        {
            const auto executable = input_paths::inspect(game.path / "eboot.bin");
            return executable.present && executable.error == input_paths::path_error::none &&
                !executable.directory && executable.length == game.executable_size;
        }
    }

    std::string encode_game(const game_record& record)
    {
        rapidjson::StringBuffer buffer;
        writer out(buffer);
        out.StartObject();
        out.Key("schema"); out.Int(1);
        out.Key("title_id"); out.String(title_id);
        out.Key("mode"); out.String(record.installed ? "installed" : "in_place");
        out.Key("base"); write_folder(out, record.base);
        if (record.update) { out.Key("update"); write_folder(out, *record.update); }
        out.Key("created_utc"); text(out, record.created_utc);
        out.EndObject();
        return std::string(buffer.GetString(), buffer.GetSize()) + '\n';
    }

    std::optional<game_record> decode_game(const std::string_view bytes, fs::path& source_path)
    {
        rapidjson::Document doc;
        if (bytes.find('\0') != bytes.npos || doc.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(bytes.data(), bytes.size()).HasParseError() ||
            !doc.IsObject()) return std::nullopt;
        source_path = read_path(field(field(doc, "base"), "path"));
        const auto& schema = field(doc, "schema");
        const bool updated = doc.HasMember("update");
        const auto mode = string(field(doc, "mode"));
        const auto created = string(field(doc, "created_utc"));
        if (doc.MemberCount() != (updated ? 6u : 5u) || !schema.IsInt() || schema.GetInt() != 1 ||
            (mode != "in_place" && mode != "installed") || string(field(doc, "title_id")) != title_id || !utc(created))
            return std::nullopt;
        auto base = read_folder(field(doc, "base"), false);
        if (!base) return std::nullopt;
        auto update = updated ? read_folder(field(doc, "update"), true) : std::nullopt;
        if (updated && !update) return std::nullopt;
        return game_record{std::move(*base), std::move(update), std::string(created), mode == "installed"};
    }

    fs::path game_directory(const fs::path& install)
    {
        return install / "data" / "games" / title_id;
    }

    bool installed_paths_match(const game_record& record, const fs::path& install)
    {
        const auto root = game_directory(install);
        const auto same = [](const fs::path& a, const fs::path& b)
        {
            return input_paths::contains(a, b) && input_paths::contains(b, a);
        };
        return record.installed && same(record.base.path, root / "base") &&
            (!record.update || same(record.update->path, root / "update"));
    }

    game_presence inspect_game(const fs::path& install)
    {
        game_presence result;
        const auto marker_path = install / "manifests" / "game-ready.txt";
        if (input_paths::inspect(marker_path).error == input_paths::path_error::missing) return result;
        result.state = record_state::invalid;
        const auto bytes = read_small(install / "data" / "games" / title_id / "game.json", 256 * 1024);
        if (bytes) result.record = decode_game(*bytes, result.source_path);
        const auto marker = read_small(marker_path, 32);
        if (!marker || *marker != std::string(title_id) + '\n')
        {
            result.record.reset();
            return result;
        }
        if (result.record && result.record->installed && !installed_paths_match(*result.record, install))
            result.record.reset();
        if (!result.record || !executable_present(result.record->base)) return result;
        if (result.record->update && !executable_present(*result.record->update))
        {
            result.source_path = result.record->update->path;
            return result;
        }
        result.state = record_state::ready;
        return result;
    }
}
