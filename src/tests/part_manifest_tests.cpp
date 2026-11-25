#include "std_include.hpp"
#include "test.hpp"
#include "updater/part_manifest.hpp"

namespace
{
    std::string fixture(const std::string& part = "server", const std::string& version = "1.2.3",
                        const std::string& channel = "stable", const std::string& minimum = "0.1.0",
                        const std::string& files = {}, const std::string& rules = {})
    {
        const auto example = std::format(R"({{"path":"bin/server.exe","size":3,"sha256":"{}"}})", std::string(64, 'a'));
        const auto members = rules.empty() ? R"("launcher":"*","emulator":">=2.0.0 <3.0.0","profile":"=1.0.0")" : rules;
        return std::format(R"({{"schema":1,"part":"{}","version":"{}","channel":"{}","min_launcher":"{}","files":[{}],"compatibility":{{{}}}}})",
                           part, version, channel, minimum, files.empty() ? example : files, members);
    }
}

TEST(manifest_accepts_strict_schema_and_requirements)
{
    std::string error;
    const auto parsed = parts::parse(fixture(), error);
    CHECK(parsed && error.empty());
    CHECK(parsed && parsed->part == parts::id::server && parsed->version == "1.2.3");
    CHECK(parsed && parsed->min_launcher == "0.1.0" && parsed->files.size() == 1 && parsed->files.front().size == 3);
    CHECK(parsed && parsed->compatibility[parts::index(parts::id::emulator)] == ">=2.0.0 <3.0.0");
    CHECK(parts::parse(fixture("server", "1.2.3-beta.2", "beta"), error));
}

TEST(manifest_rejects_wrong_shapes_versions_and_channels)
{
    std::string error;
    const auto json = fixture();
    for (const auto& invalid : {"null"s, "[]"s, json + "junk", json + '\0',
                                 R"({"unknown":false,)"s + json.substr(1),
                                 fixture("invalid"), fixture("server", "1.02.3"), fixture("server", "1.2"),
                                 fixture("server", "1.2.3+build"), fixture("server", "1.2.3-beta.2"),
                                 fixture("server", "1.2.3", "beta"), fixture("server", "1.2.3", "stable", "later")})
        CHECK(!parts::parse(invalid, error) && !error.empty());
}

TEST(manifest_validates_file_paths_and_checksums)
{
    std::string error;
    const auto file = [](std::string_view path, std::string_view size, std::string_view hash)
    {
        return std::format(R"({{"path":"{}","size":{},"sha256":"{}"}})", path, size, hash);
    };
    const std::string hash(64, 'a');
    CHECK(parts::parse(fixture("emulator", "1.2.3", "stable", "0.1.0", file("libc++.dll", "1", hash),
        R"("launcher":"*","server":"*","profile":"*")"), error));
    for (const auto path : {"", "../x", "one/../x", "/x", "one//x", "one/x/", "one/./x", "one/NUL.txt", "x.", "q:y", "q\\\\x"})
        CHECK(!parts::parse(fixture("server", "1.2.3", "stable", "0.1.0", file(path, "1", hash)), error));
    for (const auto size : {"-1", "1.0", "\"1\"", "18446744073709551616"})
        CHECK(!parts::parse(fixture("server", "1.2.3", "stable", "0.1.0", file("x", size, hash)), error));
    for (const auto& invalid : {std::string(63, 'a'), std::string(64, 'A'), std::string(64, 'g')})
        CHECK(!parts::parse(fixture("server", "1.2.3", "stable", "0.1.0", file("x", "1", invalid)), error));
    for (const auto path : {"FILE", "file/x"})
    {
        const auto files = file("file", "1", hash) + "," + file(path, "1", hash);
        CHECK(!parts::parse(fixture("server", "1.2.3", "stable", "0.1.0", files), error));
    }
}

TEST(manifest_requires_one_valid_rule_per_other_part)
{
    std::string error;
    for (const auto rules : {
        R"("launcher":"*","launcher":"*","profile":"*")",
        R"("launcher":"*","emulator":"*","server":"*")",
        R"("launcher":"*","emulator":"*")",
        R"("launcher":"*","emulator":">=2","profile":"*")",
        R"("launcher":"*","emulator":">=2.0.0 || =1.0.0","profile":"*")",
        R"("launcher":"*","emulator":" ","profile":"*")"})
        CHECK(!parts::parse(fixture("server", "1.2.3", "stable", "0.1.0", {}, rules), error));
}

TEST(manifest_install_gate_checks_reciprocal_dependencies_channel_and_consent)
{
    using parts::id;
    using release_channel::channel;
    std::string error;
    parts::install_set installed;
    installed[parts::index(id::server)] = parts::parse(fixture(), error);
    installed[parts::index(id::emulator)] = parts::parse(fixture("emulator", "2.0.0", "stable", "0.1.0", {},
        R"("launcher":">=0.2.0","server":"*","profile":"*")"), error);
    installed[parts::index(id::profile)] = parts::parse(fixture("profile", "1.0.0", "stable", "0.1.0", {},
        R"("launcher":"*","server":"*","emulator":"*")"), error);
    CHECK(parts::compatibility_error(installed, "0.2.0").empty());
    const auto bad = parts::parse(fixture("emulator", "3.0.0", "stable", "0.1.0", {},
        R"("launcher":"*","server":"*","profile":"*")"), error);
    CHECK(bad && !parts::install_error(installed, *bad, "0.2.0", channel::stable, false).empty());

    const auto launcher = parts::parse(fixture("launcher", "0.1.9", "stable", "0.1.0", {},
        R"("server":"*","profile":"*","emulator":"*")"), error);
    CHECK(launcher && !parts::install_error(installed, *launcher, "0.2.0-beta.1", channel::stable, true).empty());
    CHECK(launcher && !parts::install_error({}, *launcher, "0.2.0-beta.1", channel::stable, false).empty());
    CHECK(launcher && parts::install_error({}, *launcher, "0.2.0-beta.1", channel::stable, true).empty());

    const auto newer = parts::parse(fixture("launcher", "0.4.0-beta.1", "beta", "0.3.0", {},
        R"("server":"*","profile":"*","emulator":"*")"), error);
    CHECK(newer && !parts::install_error(installed, *newer, "0.2.0", channel::beta, false).empty());
    CHECK(newer && !parts::install_error(installed, *newer, "0.3.0", channel::stable, false).empty());
    CHECK(newer && parts::install_error(installed, *newer, "0.3.0", channel::beta, false).empty());
}
