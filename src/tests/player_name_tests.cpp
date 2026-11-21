#include "std_include.hpp"
#include "show23/player_name.hpp"
#include "test.hpp"

#include <Windows.h>
#include <fstream>
#include <iterator>

namespace
{
    using namespace show23;
    using enum player_name_result;

    struct fixture
    {
        fs::path root;
        fixture()
        {
            static unsigned serial{};
            root = fs::absolute("build") / ("player-name-fixture-" + std::to_string(GetCurrentProcessId()) +
                "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++serial));
            fs::create_directories(root / "user");
        }

        ~fixture()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }

        fs::path file() const { return root / "user" / "users.json"; }

        void write(std::string_view bytes) const
        {
            std::ofstream stream(file(), std::ios::binary);
            if (!stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size())))
                throw std::runtime_error("Could not write user fixture");
        }

        std::string contents() const
        {
            std::ifstream stream(file(), std::ios::binary);
            return {std::istreambuf_iterator<char>(stream), {}};
        }
    };
}

TEST(player_name_validation_trims_and_enforces_ascii_and_length)
{
    CHECK(normalize_player_name("  Batter 7-A_ ") == "Batter 7-A_");
    CHECK(normalize_player_name("1234567890123456").has_value());
    for (const auto* invalid : {"", "AB", "12345678901234567", "A\tBC", "Batter!", "\xc3\x89lodie"})
        CHECK(!normalize_player_name(invalid));
    CHECK(suggest_player_name(L"  _Bat!ter  ") == "_Batter");
    CHECK(suggest_player_name(L"  !  ") == "Player");
    CHECK(suggest_player_name(L"12345678901234567") == "1234567890123456");
}

TEST(player_name_replaces_only_the_correct_original_json_token)
{
    fixture f;
    const auto original = R"({"Users":{"commit_hash":"abc","user":[
  {"user_id":1001,"user_name":"Same\" Name","player_index":2,"custom":3.00e+3},
  {"user_id":1000,"player_index":1,"account":{"user_name":"Same\" Name"},"user_n\u0061me":"Same\" Name","enabled":false}
]},"outer":{"value":"\u0041"}})";
    f.write(original);
    CHECK(write_player_name(f.root, "New Name") == updated);
    std::string expected(original);
    expected.replace(expected.rfind(R"(Same\" Name)"), std::string_view(R"(Same\" Name)").size(), "New Name");
    CHECK(f.contents() == expected);
    const auto time = fs::last_write_time(f.file());
    CHECK(write_player_name(f.root, "  New Name  ") == updated);
    CHECK(fs::last_write_time(f.file()) == time);
}

TEST(player_name_creates_four_default_profile_entries)
{
    fixture f;
    CHECK(!fs::exists(f.file()));
    CHECK(write_player_name(f.root, "Test Batter") == updated);
    const auto bytes = f.contents();
    CHECK(bytes.starts_with("{\n  \"Users\""));
    rapidjson::Document doc;
    doc.Parse(bytes.c_str());
    CHECK(!doc.HasParseError() && doc.HasMember("Users"));
    if (doc.HasParseError() || !doc.HasMember("Users")) return;
    const auto& users = doc["Users"];
    CHECK(std::string_view(users["commit_hash"].GetString()).empty());
    const auto& entries = users["user"];
    CHECK(entries.IsArray() && entries.Size() == 4);
    if (!entries.IsArray() || entries.Size() != 4) return;
    for (rapidjson::SizeType index = 0; index < 4; ++index)
    {
        const auto& entry = entries[index];
        CHECK(entry["user_id"].GetUint() == 1000 + index);
        CHECK(entry["user_color"].GetUint() == index + 1);
        CHECK(entry["player_index"].GetUint() == index + 1);
        const auto name = index ? "Player " + std::to_string(index + 1) : "Test Batter";
        CHECK(entry["user_name"].GetString() == name);
        CHECK(entry["np_age"].GetInt() == 30);
        CHECK(std::string_view(entry["np_country"].GetString()) == "us");
        CHECK(std::string_view(entry["np_language"].GetString()) == "en");
        CHECK(std::string_view(entry["np_date_of_birth"].GetString()) == "1994-01-01");
        CHECK(!entry["shadnet_enabled"].GetBool());
        for (auto field : {"shadnet_npid", "shadnet_password", "shadnet_token", "shadnet_email"})
            CHECK(entry[field].IsString() && entry[field].GetStringLength() == 0);
    }
}

TEST(player_name_leaves_malformed_and_ambiguous_profiles_untouched)
{
    fixture f;
    for (const auto* bytes : {
        "",
        R"({"Users":)",
        R"({"Users":{"user":[{"user_id":1001,"player_index":2,"user_name":"Player 2"}]}})",
        R"({"Users":{"user":[{"user_id":1000,"player_index":1,"user_name":"Player 1","user_name":"Ambiguous"}]}})",
        R"({"Users":{"user":[{"user_id":1000,"player_index":2,"user_name":"Unexpected slot"}]}})"})
    {
        f.write(bytes);
        CHECK(write_player_name(f.root, "Test Batter") == malformed);
        CHECK(f.contents() == bytes);
    }
}
