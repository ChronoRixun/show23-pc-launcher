#include "std_include.hpp"
#include "resolution.hpp"
#include "input_paths.hpp"

#include <array>
#include <rapidjson/reader.h>

namespace show23
{
    namespace
    {
        constexpr std::string_view whitespace = " \t\r\n";
        constexpr std::array<std::string_view, 4> keys{
            "internal_screen_width", "internal_screen_height", "window_width", "window_height"};

        struct span
        {
            std::size_t begin{}, end{};
        };

        struct object_style
        {
            std::size_t open{}, close{};
            std::string_view colon;
        };

        // Use the JSON reader to locate the relevant tokens. Everything outside those
        // spans, including formatting and unknown values, stays in the original buffer.
        class config_spans : public rapidjson::BaseReaderHandler<rapidjson::UTF8<>, config_spans>
        {
        public:
            explicit config_spans(std::string_view text) : stream(text.data(), text.size()), text_(text) {}

            bool parse()
            {
                rapidjson::Reader reader;
                return reader.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseNumbersAsStringsFlag>(stream, *this);
            }

            bool StartObject()
            {
                if (!depth_) root.open = stream.Tell() - 1;
                else if (depth_ == 1 && gpu_key_)
                {
                    if (gpu.open) return false;
                    gpu.open = stream.Tell() - 1;
                    in_gpu_ = true;
                }
                else if (dimension_) return false;
                return ++depth_ <= 128;
            }
            bool EndObject(rapidjson::SizeType)
            {
                if (depth_ == 1) root.close = stream.Tell() - 1;
                if (depth_ == 2 && in_gpu_)
                {
                    gpu.close = stream.Tell() - 1;
                    in_gpu_ = false;
                    gpu_key_ = false;
                }
                --depth_;
                return true;
            }
            bool StartArray() { return other_value() && ++depth_ <= 128; }
            bool EndArray(rapidjson::SizeType) { --depth_; return true; }
            bool String(const char*, rapidjson::SizeType, bool) { return other_value(); }
            bool Bool(bool) { return other_value(); }
            bool Null() { return other_value(); }

            bool Key(const char* key, rapidjson::SizeType length, bool)
            {
                const std::string_view name(key, length);
                if (depth_ == 1)
                {
                    gpu_key_ = name == "GPU";
                    remember_colon(root);
                }
                else if (depth_ == 2 && in_gpu_)
                {
                    remember_colon(gpu);
                    const auto found = std::ranges::find(keys, name);
                    if (found != keys.end())
                    {
                        dimension_ = static_cast<std::size_t>(found - keys.begin());
                        if (values[*dimension_]) return false;
                    }
                }
                return true;
            }

            bool RawNumber(const char*, rapidjson::SizeType length, bool)
            {
                if (!dimension_) return other_value();
                const auto end = stream.Tell();
                if (end < length) return false;
                values[*dimension_] = span{end - length, end};
                dimension_.reset();
                return true;
            }

            rapidjson::MemoryStream stream;
            object_style root, gpu;
            std::array<std::optional<span>, keys.size()> values;

        private:
            bool other_value() const
            {
                return depth_ && !dimension_ && !(depth_ == 1 && gpu_key_);
            }

            void remember_colon(object_style& object)
            {
                if (!object.colon.empty()) return;
                const auto start = stream.Tell();
                const auto colon = text_.find_first_not_of(whitespace, start);
                if (colon == text_.npos || text_[colon] != ':') return;
                const auto value = text_.find_first_not_of(whitespace, colon + 1);
                if (value != text_.npos) object.colon = text_.substr(start, value - start);
            }

            std::string_view text_;
            unsigned depth_{};
            bool in_gpu_{}, gpu_key_{};
            std::optional<std::size_t> dimension_;
        };

        std::string member(std::string_view key, std::string_view colon, std::string_view value)
        {
            return '"' + std::string(key) + '"' + std::string(colon) + std::string(value);
        }

        struct edit
        {
            span range;
            std::string replacement;
        };

        bool patch_config(std::string& bytes, resolution choice)
        {
            if (bytes.find('\0') != bytes.npos) return false;
            const std::size_t bom = bytes.starts_with("\xef\xbb\xbf") ? 3 : 0;
            const std::string_view text(bytes.data() + bom, bytes.size() - bom);
            config_spans config(text);
            if (!config.parse()) return false;

            const auto lf = text.find('\n');
            const std::string_view newline = lf != text.npos && lf && text[lf - 1] == '\r' ? "\r\n" : "\n";
            const auto indent_to = [&](std::size_t position) -> std::string_view
            {
                if (!position) return {};
                const auto line = text.rfind('\n', position - 1);
                const auto start = line == text.npos ? 0 : line + 1;
                const auto first = text.find_first_not_of(" \t", start);
                if (first == text.npos || first > position) return {};
                return text.substr(start, first - start);
            };
            const auto gap = [&](object_style object)
            {
                const auto first = text.find_first_not_of(whitespace, object.open + 1);
                return text.substr(object.open + 1, first - object.open - 1);
            };
            const auto root_gap = gap(config.root);
            const auto line_indent = root_gap.rfind('\n');
            auto step = line_indent == root_gap.npos ? std::string_view{} : root_gap.substr(line_indent + 1);
            const auto root_indent = indent_to(config.root.open);
            if (step.starts_with(root_indent)) step.remove_prefix(root_indent.size());
            if (step.empty()) step = "  ";

            std::vector<edit> edits;
            const auto append_members = [&](object_style object, const std::vector<std::string>& additions)
            {
                if (additions.empty()) return;
                const auto last = text.find_last_not_of(whitespace, object.close - 1);
                const bool empty = last == object.open;
                const auto leading = gap(object);
                const bool pretty = leading.find('\n') != leading.npos || (empty && lf != text.npos);
                const auto indent = empty ? std::string(indent_to(object.open)) + std::string(step) :
                    std::string(leading.substr(leading.rfind('\n') + 1));
                const auto separator = pretty ? std::string(newline) + indent : std::string(leading);
                std::string addition;
                for (const auto& entry : additions)
                {
                    if (!empty || !addition.empty()) addition += ',';
                    if (!empty || pretty || !addition.empty()) addition += separator;
                    addition += entry;
                }
                if (empty && pretty && leading.find('\n') == leading.npos)
                    addition += std::string(newline) + std::string(indent_to(object.open));
                edits.push_back({{last + 1, last + 1}, std::move(addition)});
            };

            const auto colon = !config.gpu.colon.empty() ? config.gpu.colon :
                !config.root.colon.empty() ? config.root.colon : (lf == text.npos ? ":" : ": ");
            std::vector<std::string> additions;
            for (std::size_t i = 0; i < keys.size(); ++i)
            {
                auto value = std::to_string(i % 2 == 0 ? choice.width : choice.height);
                if (config.values[i]) edits.push_back({*config.values[i], std::move(value)});
                else additions.push_back(member(keys[i], colon, value));
            }

            if (config.gpu.open) append_members(config.gpu, additions);
            else
            {
                const bool pretty = root_gap.find('\n') != root_gap.npos ||
                    (text.find_first_not_of(whitespace, config.root.open + 1) == config.root.close && lf != text.npos);
                const auto parent = std::string(root_indent) + std::string(step);
                std::string object = "{";
                for (std::size_t i = 0; i < additions.size(); ++i)
                {
                    if (i) object += ',';
                    if (pretty) object += std::string(newline) + parent + std::string(step);
                    object += additions[i];
                }
                if (pretty) object += std::string(newline) + parent;
                object += '}';
                append_members(config.root, {member("GPU", colon, object)});
            }

            std::ranges::sort(edits, std::greater{}, [](const edit& item) { return item.range.begin; });
            for (const auto& [range, replacement] : edits)
                bytes.replace(bom + range.begin, range.end - range.begin, replacement);
            return true;
        }
    }

    const resolution* find_resolution(std::string_view id)
    {
        const auto found = std::ranges::find(resolutions, id, &resolution::id);
        return found == std::end(resolutions) ? nullptr : &*found;
    }

    resolution_result write_resolution(const fs::path& profile, resolution choice)
    {
        using namespace input_paths;
        using enum resolution_result;
        const auto directory = ensure_directory(profile / "user");
        if (!directory.directory || directory.error != path_error::none) return unavailable;
        const fs::path name = "config.json";
        std::string bytes = "{\n}\n";
        {
            const auto file = inspect(directory.path / name, true, true);
            if (file.error != path_error::none || file.directory || (file.present && file.links != 1)) return unavailable;
            if (file.present)
            {
                if (!file.length || file.length > 1024 * 1024) return malformed;
                bytes.resize(static_cast<std::size_t>(file.length));
                if (!read_at(file.pins.back().get(), 0, bytes)) return unavailable;
                const auto before = bytes;
                if (!patch_config(bytes, choice)) return malformed;
                if (bytes == before) return updated;
            }
            else if (!patch_config(bytes, choice)) return unavailable;
        }
        return atomically_write(directory, name, bytes) ? updated : unavailable;
    }
}
