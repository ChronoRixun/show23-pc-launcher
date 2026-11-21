#include "std_include.hpp"
#include "test.hpp"
#include "updater/part_store.hpp"
#include <Windows.h>

#include <fstream>
#include <rapidjson/writer.h>

namespace
{
    namespace fs = std::filesystem;
    constexpr auto digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    void write(const fs::path& path, std::string_view data)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        if (!file.write(data.data(), static_cast<std::streamsize>(data.size())))
            throw std::runtime_error("Could not write runtime fixture");
    }

    struct fixture
    {
        fs::path root, bundle, install;
        fixture()
        {
            static unsigned serial{};
            root = fs::current_path() / "build" / ("bundle fixture " + std::to_string(GetCurrentProcessId()) +
                " " + std::to_string(GetTickCount64()) + " " + std::to_string(++serial));
            bundle = root / "source";
            install = root / "install";
            write(bundle / "manifests/runtime.txt", "0.1.0\n");
            part("emulator", "emulator", {"shadPS4.exe"});
            part("server", "server", {"python/python.exe", "olserver.py", "roster_decode.py", "tables/ol-messages.csv", "replies/fixture.xml"});
            part("profile", "tools/profile", {"defaults.txt"});
            write(install / "data/profiles/default/user/save", "preserve");
        }
        ~fixture()
        {
            std::error_code error;
            fs::remove_all(root, error);
        }
        void part(std::string_view name, std::string_view directory, std::initializer_list<const char*> files)
        {
            for (auto file : files) write(bundle / "runtime/0.1.0" / directory / file, "abc");
            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> out(buf);
            out.StartObject();
            out.Key("schema"); out.Int(1);
            out.Key("part"); out.String(name.data(), static_cast<rapidjson::SizeType>(name.size()));
            out.Key("version"); out.String("0.1.0");
            out.Key("channel"); out.String("stable");
            out.Key("min_launcher"); out.String("0.1.0");
            out.Key("files"); out.StartArray();
            for (auto file : files)
            {
                out.StartObject();
                out.Key("path"); out.String(file);
                out.Key("size"); out.Int(3);
                out.Key("sha256"); out.String(digest);
                out.EndObject();
            }
            out.EndArray();
            out.Key("compatibility"); out.StartObject();
            for (auto other : {"launcher", "emulator", "server", "profile"})
                if (name != other) { out.Key(other); out.String("*"); }
            out.EndObject();
            out.EndObject();
            write(bundle / "manifests/0.1.0" / (std::string(name) + ".json"), buf.GetString());
        }
    };
}

TEST(bundled_runtime_stages_and_verifies_files_without_touching_user_data)
{
    fixture f;
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    std::string error;
    CHECK(parts::verify_runtime(parts::read_runtime(f.install), "0.1.0", error));
    const auto save = f.install / "data/profiles/default/user/save";
    const auto exe = f.install / "runtime/0.1.0/emulator/shadPS4.exe";
    const auto marker = f.install / "manifests/runtime.txt";
    const auto before = fs::last_write_time(exe);
    const auto before_marker = fs::last_write_time(marker);
    fs::remove_all(f.bundle);
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(fs::last_write_time(exe) == before && fs::last_write_time(marker) == before_marker);
    std::string content;
    std::ifstream(save) >> content;
    CHECK(content == "preserve");
}

TEST(bundled_runtime_repairs_bad_hash_but_refuses_a_corrupt_bundle)
{
    fixture f;
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    write(f.install / "runtime/0.1.0/emulator/shadPS4.exe", "bad");
    std::string error;
    CHECK(!parts::verify_runtime(parts::read_runtime(f.install), "0.1.0", error));
    CHECK(parts::stage_bundled_runtime(f.bundle, f.install, "0.1.0"));
    CHECK(parts::verify_runtime(parts::read_runtime(f.install), "0.1.0", error));
    write(f.bundle / "runtime/0.1.0/server/python/python.exe", "bad");
    const auto other = f.root / "other";
    CHECK(!parts::stage_bundled_runtime(f.bundle, other, "0.1.0"));
    CHECK(!fs::exists(other / "manifests/runtime.txt"));
}
