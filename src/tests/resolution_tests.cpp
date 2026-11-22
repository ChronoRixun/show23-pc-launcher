#include "std_include.hpp"
#include "test.hpp"
#include "show23/resolution.hpp"

#include <Windows.h>
#include <fstream>
#include <iterator>

namespace
{
    using namespace show23;
    using enum resolution_result;

    struct fixture
    {
        fs::path root = fs::absolute("build") / ("resolution-fixture-" +
            std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        fixture() { fs::create_directories(root / "user"); }
        ~fixture()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }
        fs::path file() const { return root / "user/config.json"; }
        void write(std::string_view bytes) const { std::ofstream(file(), std::ios::binary) << bytes; }
        std::string read() const
        {
            std::ifstream in(file(), std::ios::binary);
            return {std::istreambuf_iterator<char>(in), {}};
        }
    };

    void check_dimensions(const std::string& text, unsigned width, unsigned height)
    {
        rapidjson::Document doc;
        doc.Parse(text.c_str());
        CHECK(!doc.HasParseError() && doc.HasMember("GPU"));
        if (doc.HasParseError() || !doc.HasMember("GPU")) return;
        const auto& gpu = doc["GPU"];
        for (const auto* key : {"internal_screen_width", "window_width"})
            CHECK(gpu.HasMember(key) && gpu[key].IsUint() && gpu[key].GetUint() == width);
        for (const auto* key : {"internal_screen_height", "window_height"})
            CHECK(gpu.HasMember(key) && gpu[key].IsUint() && gpu[key].GetUint() == height);
    }
}

TEST(resolution_updates_existing_config_without_changing_other_bytes)
{
    fixture f;
    const std::string prefix = "\xef\xbb\xbf{\r\n    \"General\": { \"neo_mode\": false },\r\n    \"GPU\": {\r\n"
        "        \"other\" : [true, {\"window_width\":900, \"note\":\"\\u00a9 }\"}],\r\n";
    const auto suffix = "\r\n    },\r\n    \"Audio\": {\"custom\":2.00e1}\r\n}";
    const auto values = [](const char* width, const char* height)
    {
        return std::string("        \"internal_screen_width\" : ") + width +
            ",\r\n        \"internal_screen_height\" : " + height + ",\r\n        \"window_width\" : " +
            width + ",\r\n        \"window_height\" : " + height;
    };
    f.write(prefix + values("1280", "720") + suffix);
    CHECK(write_resolution(f.root, default_resolution) == updated);
    CHECK(f.read() == prefix + values("1920", "1080") + suffix);
    const auto time = fs::last_write_time(f.file());
    CHECK(write_resolution(f.root, default_resolution) == updated);
    CHECK(fs::last_write_time(f.file()) == time);
}

TEST(resolution_creates_missing_config_with_only_gpu_values)
{
    fixture f;
    CHECK(!fs::exists(f.file()));
    CHECK(write_resolution(f.root, default_resolution) == updated);
    CHECK(f.read().starts_with("{\n  \"GPU\": {\n    "));
    check_dimensions(f.read(), 1920, 1080);
}

TEST(resolution_fills_partial_gpu_values_using_existing_formatting)
{
    fixture f;
    f.write("{\r\n\t\"GPU\": {\r\n\t\t\"window_width\": 8,\r\n\t\t\"other\": [1, 2]\r\n\t}\r\n}\r\n");
    CHECK(write_resolution(f.root, *find_resolution("720p")) == updated);
    CHECK(f.read() == "{\r\n\t\"GPU\": {\r\n\t\t\"window_width\": 1280,\r\n\t\t\"other\": [1, 2],\r\n"
        "\t\t\"internal_screen_width\": 1280,\r\n\t\t\"internal_screen_height\": 720,\r\n\t\t\"window_height\": 720\r\n\t}\r\n}\r\n");
    f.write("{\"Other\":[]}");
    CHECK(write_resolution(f.root, default_resolution) == updated);
    CHECK(f.read().starts_with("{\"Other\":[],\"GPU\":{"));
    check_dimensions(f.read(), 1920, 1080);
}

TEST(resolution_leaves_corrupt_or_ambiguous_configs_untouched)
{
    fixture f;
    for (const auto* bytes : {"", "[]", "{\"GPU\": null}", "{\"GPU\": {", "{\"GPU\":{},\"GPU\":{}}",
        "{\"GPU\":{\"window_width\":2,\"window_\\u0077idth\":3}}", "{\"GPU\":{\"window_width\":[]}}"})
    {
        f.write(bytes);
        CHECK(write_resolution(f.root, default_resolution) == malformed);
        CHECK(f.read() == bytes);
    }
    CHECK(!find_resolution("1440p"));
}
